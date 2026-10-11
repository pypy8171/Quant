// DbManager 연결 쪽 — libpq 연결 열기·닫기·COPY, 멈춘 호출을 끊는 취소 수단, 워커가 나눠 보는 연결 상태표,
//  윈도우에서 WSL 주소로 바로 붙기. 적재 루프는 DbWorkers.cpp, 공개 API는 DbManager.cpp. [why D-148] [why D-144]
#include "ipc/DbWorkerState.h"
#include "utils/Logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#ifdef ERROR
#undef ERROR
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace db
{
namespace
{

constexpr int kMaxBackoffMs      = 30000;
constexpr int kStatementTimeoutMs = 30000; // COPY 묶음 하나의 서버 쪽 한도

} // namespace

namespace detail
{

void InterruptSlot::set(PGconn* handle)
{
    std::shared_ptr<PGcancel> cancel(PQgetCancel(handle), [](PGcancel* pointer)
    {
        PQfreeCancel(pointer);
    });
    std::lock_guard<std::mutex> lock(mutex_);
    socket_ = PQsocket(handle);
    cancel_ = std::move(cancel);
}

void InterruptSlot::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    socket_ = -1;
    cancel_.reset();
}

void InterruptSlot::interrupt()
{
    std::shared_ptr<PGcancel> cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel = cancel_;

        if (socket_ >= 0)
        {
#ifdef _WIN32
            ::shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
#else
            ::shutdown(socket_, SHUT_RDWR);
#endif
        }
    }

    if (cancel == nullptr)
    {
        return;
    }

    // PQcancel은 서버에 새로 붙어 취소를 보낸다 — 서버가 사라졌으면 접속에서 오래 걸리니 stop()을 붙잡지 않게 떼어 보낸다.
    //  취소 수단은 shared_ptr로 넘겨, 그사이 적재 스레드가 연결을 닫아도 살아 있다.
    std::thread(
        [cancel]
        {
            char error[256];
            PQcancel(cancel.get(), error, sizeof(error));
        })
        .detach();
}

std::string environment_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

Connection::Connection(InterruptSlot& slot) : slot_(slot)
{
}

Connection::~Connection()
{
    close();
}

bool Connection::open(const DbConfig& config, const std::string& password, const std::string& label)
{
    close();
    const std::string host = resolve_host(config.host);
    const std::string port = std::to_string(config.port);
    const std::string connect_timeout = std::to_string(kConnectTimeoutSeconds);
    const std::string options = "-c statement_timeout=" + std::to_string(kStatementTimeoutMs);
    // DB가 멈춰도 워커가 영영 붙들리지 않게 하는 시간 제한들.
    //  keepalives: 답을 기다리는 중에 상대가 사라지면 약 25초 안에 끊긴 것으로 안다(윈도우는 count를 무시한다).
    //  tcp_user_timeout: 보낸 것이 30초 동안 확인되지 않으면 끊는다(리눅스만, 윈도우는 OS 재전송 한도가 대신한다).
    //  statement_timeout: 서버가 잠금 대기 등으로 COPY를 한도 넘게 끌면 서버가 취소한다.
    const char* const keywords[] = {"host", "port", "dbname", "user", "password", "connect_timeout",
                                    "application_name", "keepalives", "keepalives_idle", "keepalives_interval",
                                    "keepalives_count", "tcp_user_timeout", "options", nullptr};
    const char* const values[] = {host.c_str(), port.c_str(), config.dbname.c_str(), config.user.c_str(),
                                  password.c_str(), connect_timeout.c_str(), "quant_db_manager", "1", "10", "5",
                                  "3", "30000", options.c_str(), nullptr};
    handle_ = PQconnectdbParams(keywords, values, 0);

    if (usable())
    {
        slot_.set(handle_);
        generation_ += 1;
        LOG_INFO("[DbManager] " + label + " 연결 — " + host + ":" + port);
        return true;
    }

    LOG_WARN("[DbManager] " + label + " 연결 실패 " + host + ":" + port + " — " +
             (handle_ != nullptr ? PQerrorMessage(handle_) : "메모리 부족"));
    close();
    return false;
}

void Connection::close()
{
    if (handle_ != nullptr)
    {
        slot_.clear();
        PQfinish(handle_);
        handle_ = nullptr;
    }
}

bool Connection::execute(const char* statement, std::string& error)
{
    PGresult*            result = PQexec(handle_, statement);
    const ExecStatusType status = PQresultStatus(result);
    PQclear(result);

    if (status == PGRES_COMMAND_OK)
    {
        return true;
    }

    error = PQerrorMessage(handle_);
    return false;
}

Connection::Outcome Connection::copy(const char* statement, const std::string& text, std::string& error)
{
    PGresult* start = PQexec(handle_, statement);
    const ExecStatusType start_status = PQresultStatus(start);
    PQclear(start);

    if (start_status != PGRES_COPY_IN)
    {
        error = PQerrorMessage(handle_);
        drain_results();
        return usable() ? Outcome::kRejected : Outcome::kNotSent;
    }

    if (PQputCopyData(handle_, text.data(), static_cast<int>(text.size())) != 1 ||
        PQputCopyEnd(handle_, nullptr) != 1)
    {
        error = PQerrorMessage(handle_);
        return Outcome::kNotSent;
    }

    PGresult* finish = PQgetResult(handle_);

    if (finish == nullptr)
    {
        error = PQerrorMessage(handle_);
        return Outcome::kAmbiguous;
    }

    const ExecStatusType finish_status = PQresultStatus(finish);
    PQclear(finish);

    if (finish_status == PGRES_COMMAND_OK)
    {
        drain_results();
        return Outcome::kWritten;
    }

    error = PQerrorMessage(handle_);
    drain_results();
    // 서버가 오류를 돌려줬다면 트랜잭션은 롤백됐다. 끊겨서 생긴 오류는 커밋 여부를 모른다.
    return usable() ? Outcome::kRejected : Outcome::kAmbiguous;
}

void Connection::drain_results()
{
    while (PGresult* rest = PQgetResult(handle_))
    {
        PQclear(rest);
    }
}

bool acquire_connection(DbManager::State& state, Connection& connection, Worker& worker)
{
    const auto is_running = [&state]
    {
        return state.running.load(std::memory_order_acquire);
    };

    while (!connection.usable())
    {
        if (state.abandon.load(std::memory_order_acquire))
        {
            return false;
        }

        const bool    may_wait = is_running();
        const int64_t retry_at = state.retry_at_ms.load(std::memory_order_acquire);
        const int64_t now = now_ms();

        if (now < retry_at)
        {
            if (!may_wait)
            {
                return false;
            }

            worker.wake.wait_for(std::chrono::milliseconds(retry_at - now), is_running);
            continue;
        }

        // 실패 기록이 있으면 붙어 보는 차례를 하나만 준다. 기록이 없으면(정상) 워커마다 각자 붙는다.
        const bool recovering = retry_at != 0;
        bool       expected = false;

        if (recovering && !state.probing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            if (!may_wait)
            {
                return false;
            }

            worker.wake.wait_for(std::chrono::milliseconds(100), is_running);
            continue;
        }

        const bool opened = connection.open(state.config, state.password, worker.label);

        if (opened)
        {
            state.backoff_ms.store(1000, std::memory_order_relaxed);
            state.retry_at_ms.store(0, std::memory_order_release);
        }
        else
        {
            const int backoff = state.backoff_ms.load(std::memory_order_relaxed);
            state.retry_at_ms.store(now_ms() + backoff, std::memory_order_release);
            state.backoff_ms.store(backoff * 2 > kMaxBackoffMs ? kMaxBackoffMs : backoff * 2,
                                   std::memory_order_relaxed);
        }

        if (recovering)
        {
            state.probing.store(false, std::memory_order_release);
        }

        if (!opened && !may_wait)
        {
            return false;
        }
    }

    return true;
}

} // namespace detail

std::string resolve_host(const std::string& host)
{
#ifdef _WIN32
    if ((host != "localhost" && host != "127.0.0.1" && host != "::1") ||
        detail::environment_or("TSDB_WSL_DIRECT", "1") == "0")
    {
        return host;
    }

    const std::string distribution = detail::environment_or("TSDB_WSL_DISTRO", "Ubuntu-24.04");
    const std::string command = "wsl -d " + distribution + " -- ip -4 -o addr show eth0 2>NUL";
    std::FILE* pipe = _popen(command.c_str(), "r");

    if (pipe == nullptr)
    {
        return host;
    }

    std::string output;
    char buffer[256];

    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output.append(buffer);
    }

    _pclose(pipe);
    // "2: eth0    inet 172.x.x.x/20 brd ..." — inet 다음 칸의 / 앞까지.
    const size_t inet = output.find("inet ");

    if (inet == std::string::npos)
    {
        return host;
    }

    const size_t begin = inet + 5;
    const size_t end = output.find('/', begin);

    if (end == std::string::npos || end == begin)
    {
        return host;
    }

    return output.substr(begin, end - begin);
#else
    return host;
#endif
}

} // namespace db
