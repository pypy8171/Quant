#include "ipc/DbManager.h"
#include "core/MpscQueue.h"
#include "core/WakeGate.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

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

#include <libpq-fe.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace db
{
namespace
{

constexpr const char* kCopyStatement = "COPY ticks(ts,ticker,price,volume,direction,market) FROM STDIN";
constexpr int         kMaxBackoffMs  = 30000;
constexpr int         kConnectTimeoutSeconds = 5;
constexpr int         kStatementTimeoutMs = 30000; // COPY 묶음 하나의 서버 쪽 한도

// 워커가 지금 쓰는 연결의 취소 수단. stop()이 기다리다 못 하면 interrupt()로 막힌 libpq 호출을 오류로 돌려보낸다.
//  서버가 잡고 있는 경우(잠금 대기 등)는 PQcancel이 풀고 — 윈도우에서는 소켓 shutdown만으로 select가 깨지지 않았다
//  (2026-09-26 test_db_writer) — 상대가 사라진 경우는 리눅스에서 shutdown이 푼다. 윈도우에서 상대가 사라졌으면
//  keepalive가 끊을 때까지 못 푸니 stop()이 스레드를 떼어 둔다.
//  close() 쪽은 clear() 뒤에 PQfinish 하므로, interrupt()가 닫힌 소켓 번호를 건드리는 일은 없다.
class InterruptSlot
{
public:
    void set(PGconn* handle)
    {
        std::shared_ptr<PGcancel> cancel(PQgetCancel(handle), [](PGcancel* pointer) { PQfreeCancel(pointer); });
        std::lock_guard<std::mutex> lock(mutex_);
        socket_ = PQsocket(handle);
        cancel_ = std::move(cancel);
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socket_ = -1;
        cancel_.reset();
    }

    void interrupt()
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

private:
    std::mutex                mutex_;
    int                       socket_ = -1;
    std::shared_ptr<PGcancel> cancel_;
};

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string environment_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

void append_digits(std::string& out, int64_t value, int width)
{
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    const auto length = static_cast<int>(result.ptr - buffer);

    for (int pad = length; pad < width; ++pad)
    {
        out.push_back('0');
    }

    out.append(buffer, result.ptr);
}

template <typename Number>
void append_number(std::string& out, Number value)
{
    char buffer[40];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

// 연결 하나. 워커 하나만 쓴다.
class Connection
{
public:
    explicit Connection(InterruptSlot& slot) : slot_(slot) {}

    ~Connection()
    {
        close();
    }

    [[nodiscard]] bool usable() const
    {
        return handle_ != nullptr && PQstatus(handle_) == CONNECTION_OK;
    }

    [[nodiscard]] PGconn* handle() const
    {
        return handle_;
    }

    bool open(const DbConfig& config, const std::string& password, const std::string& label)
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
            LOG_INFO("[DbManager] " + label + " 연결 — " + host + ":" + port);
            return true;
        }

        LOG_WARN("[DbManager] " + label + " 연결 실패 " + host + ":" + port + " — " +
                 (handle_ != nullptr ? PQerrorMessage(handle_) : "메모리 부족"));
        close();
        return false;
    }

    void close()
    {
        if (handle_ != nullptr)
        {
            slot_.clear();
            PQfinish(handle_);
            handle_ = nullptr;
        }
    }

    // 결과 셋. 이 셋만 보고 부르는 쪽이 다시 넣을지를 정한다.
    enum class Outcome
    {
        kWritten,   // 들어갔다
        kNotSent,   // 끝 신호 전에 끊겼다 — 서버가 COPY를 버렸다. 다시 붙어 한 번 더 넣어도 된다
        kRejected,  // 연결은 살아 있고 서버가 거절했다 — 다시 넣어도 같다
        kAmbiguous, // 끝 신호는 갔는데 답 전에 끊겼다 — 들어갔는지 모른다
    };

    Outcome copy(const std::string& text, std::string& error)
    {
        PGresult* start = PQexec(handle_, kCopyStatement);
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

private:
    void drain_results()
    {
        while (PGresult* rest = PQgetResult(handle_))
        {
            PQclear(rest);
        }
    }

    InterruptSlot& slot_;
    PGconn*        handle_ = nullptr;
};

struct Row
{
    TradeData trade;
    int64_t   enqueue_ms = 0;
};

// 적재 워커 하나의 큐·스레드·깨우기·취소 수단.
struct TickWorker
{
    explicit TickWorker(size_t capacity) : queue(capacity), finished(done.get_future()) {}

    MpscQueue<Row>     queue;
    std::string        label;
    wake::WakeGate     wake;
    InterruptSlot      interrupt;
    std::promise<void> done; // 워커 스레드가 끝나면 채운다 — stop()이 시간 한도를 두고 기다리는 데 쓴다
    std::future<void>  finished;
    std::thread        thread;
};

} // namespace

struct DbManager::State
{
    DbConfig                                  config;
    std::string                               password;
    std::vector<std::unique_ptr<TickWorker>> tick_workers;
    std::atomic<bool>                        running{false};
    std::atomic<bool>                        abandon{false}; // stop()이 기다리기를 그만뒀다 — 남은 행은 넣지 않고 센다
    // 연결 상태표 — 모든 워커가 나눠 본다. 접속이 한 번 실패하면 retry_at_ms까지 아무도 다시 붙지 않고,
    //  그 뒤에도 probing을 잡은 워커 하나만 붙어 본다. 나머지는 그동안 기다린다(행은 큐에 쌓이다 넘치면 버린다).
    std::atomic<int64_t>  retry_at_ms{0}; // 0이면 실패 기록 없음 — 워커마다 각자 붙는다
    std::atomic<int>      backoff_ms{1000};
    std::atomic<bool>     probing{false};
    std::atomic<uint64_t> ticks_offered{0};
    std::atomic<uint64_t> ticks_written{0};
    std::atomic<uint64_t> ticks_dropped{0};
    std::atomic<uint64_t> ticks_failed{0};
    std::atomic<uint64_t> ticks_ambiguous{0};
};

namespace
{
void tick_loop(DbManager::State& state, TickWorker& worker, unsigned index);
} // namespace

void append_copy_text(std::string& out, std::string_view text)
{
    for (const char character : text)
    {
        switch (character)
        {
        case '\\':
            out.append("\\\\");
            break;

        case '\t':
            out.append("\\t");
            break;

        case '\n':
            out.append("\\n");
            break;

        case '\r':
            out.append("\\r");
            break;

        default:
            out.push_back(character);
        }
    }
}

void append_iso_utc(std::string& out, int64_t epoch_ms)
{
    using namespace std::chrono;
    const sys_time<milliseconds> time_point{milliseconds(epoch_ms)};
    const sys_days               day = floor<days>(time_point);
    const year_month_day         date{day};
    const hh_mm_ss<milliseconds> clock{time_point - day};

    append_digits(out, static_cast<int>(date.year()), 4);
    out.push_back('-');
    append_digits(out, static_cast<unsigned>(date.month()), 2);
    out.push_back('-');
    append_digits(out, static_cast<unsigned>(date.day()), 2);
    out.push_back('T');
    append_digits(out, clock.hours().count(), 2);
    out.push_back(':');
    append_digits(out, clock.minutes().count(), 2);
    out.push_back(':');
    append_digits(out, clock.seconds().count(), 2);
    out.push_back('.');
    append_digits(out, clock.subseconds().count(), 3);
    out.append("+00:00");
}

void append_trade_row(std::string& out, const TradeData& trade, int64_t enqueue_ms)
{
    append_iso_utc(out, enqueue_ms);
    out.push_back('\t');
    append_copy_text(out, trade.ticker.view());
    out.push_back('\t');
    append_number(out, trade.price);
    out.push_back('\t');
    append_number(out, trade.quantity);
    out.push_back('\t');
    append_number(out, trade.direction);
    out.push_back('\t');
    out.append(trade.market == Market::US ? "US" : "KR");
    out.push_back('\n');
}

std::string resolve_host(const std::string& host)
{
#ifdef _WIN32
    if ((host != "localhost" && host != "127.0.0.1" && host != "::1") ||
        environment_or("TSDB_WSL_DIRECT", "1") == "0")
    {
        return host;
    }

    const std::string distribution = environment_or("TSDB_WSL_DISTRO", "Ubuntu-24.04");
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

DbManager::DbManager(DbConfig config) : state_(std::make_shared<State>())
{
    State& state = *state_;
    state.config = std::move(config);
    state.password = environment_or("TSDB_PASSWORD", "");

    if (state.password.empty())
    {
        LOG_ERROR("[DbManager] TSDB_PASSWORD 환경변수가 없다 — 체결을 DB에 넣지 않는다");
        return;
    }

    const unsigned tick_count = state.config.tick_workers;
    state.running.store(true, std::memory_order_release);

    for (unsigned index = 0; index < tick_count; ++index)
    {
        auto worker = std::make_unique<TickWorker>(state.config.tick_queue_capacity);
        worker->label = "적재 워커 " + std::to_string(index);
        state.tick_workers.push_back(std::move(worker));
    }

    // 스레드가 상태를 나눠 가진다 — stop()이 떼어 둔 스레드가 늦게 돌아와도 상태는 살아 있다.
    for (unsigned index = 0; index < tick_count; ++index)
    {
        TickWorker& worker = *state.tick_workers[index];
        worker.thread = std::thread(
            [shared = state_, &worker, index]
            {
                tick_loop(*shared, worker, index);
                worker.done.set_value();
            });
    }

    LOG_INFO("[DbManager] 시작 — 적재 워커 " + std::to_string(tick_count) + "개, 묶음 " + std::to_string(state.config.batch_rows) + "행, " +
             std::to_string(state.config.flush_ms) + "ms");
}

DbManager::~DbManager()
{
    stop();
}

bool DbManager::ok() const noexcept
{
    return !state_->tick_workers.empty();
}

void DbManager::on_trade(const TradeData& trade) noexcept
{
    State& state = *state_;

    if (state.tick_workers.empty())
    {
        return;
    }

    state.ticks_offered.fetch_add(1, std::memory_order_relaxed);
    TickWorker& worker = *state.tick_workers[trade.symbol_id % state.tick_workers.size()];

    if (!worker.queue.push(Row{trade, now_ms()}))
    {
        state.ticks_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    worker.wake.notify();
}

void DbManager::flush_ticks()
{
    const State& state = *state_;

    while (state.ticks_written.load(std::memory_order_acquire) +
               state.ticks_dropped.load(std::memory_order_relaxed) +
               state.ticks_failed.load(std::memory_order_relaxed) +
               state.ticks_ambiguous.load(std::memory_order_relaxed) <
           state.ticks_offered.load(std::memory_order_relaxed))
    {
        for (const auto& worker : state.tick_workers)
        {
            worker->wake.notify();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

DbStatistics DbManager::statistics() const noexcept
{
    const State& state = *state_;
    DbStatistics statistics;
    statistics.ticks_offered = state.ticks_offered.load(std::memory_order_relaxed);
    statistics.ticks_written = state.ticks_written.load(std::memory_order_relaxed);
    statistics.ticks_dropped = state.ticks_dropped.load(std::memory_order_relaxed);
    statistics.ticks_failed = state.ticks_failed.load(std::memory_order_relaxed);
    statistics.ticks_ambiguous = state.ticks_ambiguous.load(std::memory_order_relaxed);
    return statistics;
}

void DbManager::stop()
{
    using namespace std::chrono;
    State& state = *state_;
    state.running.store(false, std::memory_order_release);
    const auto& workers = state.tick_workers;

    for (const auto& worker : workers)
    {
        worker->wake.notify();
    }

    // 1) 남은 행을 넣을 시간을 준다.
    const auto grace_deadline = steady_clock::now() + milliseconds(state.config.stop_grace_ms);
    bool       late = false;

    for (const auto& worker : workers)
    {
        if (worker->thread.joinable() && worker->finished.wait_until(grace_deadline) != std::future_status::ready)
        {
            late = true;
        }
    }

    // 2) 한도를 넘겼다 — DB가 멈췄다고 보고 취소를 보내고 소켓을 끊는다. 막혀 있던 libpq 호출이 오류로 돌아오고,
    //  워커는 남은 행을 센 뒤 끝난다. 접속 중(PQconnectdbParams)은 끊을 소켓이 없어
    //  연결 제한만큼 더 걸린다.
    if (late)
    {
        LOG_WARN("[DbManager] 종료 대기 " + std::to_string(state.config.stop_grace_ms) +
                 "ms를 넘김 — DB 연결을 끊고 남은 일은 못 한 것으로 센다");
        state.abandon.store(true, std::memory_order_release);

        for (const auto& worker : workers)
        {
            worker->interrupt.interrupt();
            worker->wake.notify();
        }
    }

    // 3) 그래도 안 돌아오는 스레드는 떼어 둔다 — 엔진 종료를 DB에 묶지 않는다.
    const auto last_deadline = steady_clock::now() + seconds(kConnectTimeoutSeconds + 1);
    bool       reaped = false;

    for (const auto& worker : workers)
    {
        if (!worker->thread.joinable())
        {
            continue;
        }

        reaped = true;

        if (worker->finished.wait_until(last_deadline) == std::future_status::ready)
        {
            worker->thread.join();
            continue;
        }

        worker->thread.detach();
        LOG_ERROR("[DbManager] " + worker->label + "가 DB 호출에서 돌아오지 않아 떼어 두고 끝낸다");
    }

    // 소멸자도 stop()을 부른다 — 스레드를 거둔 첫 호출만 결산을 남긴다.
    if (reaped)
    {
        const DbStatistics totals = statistics();
        LOG_INFO("[DbManager] 종료 — 받음 " + std::to_string(totals.ticks_offered) + ", 넣음 " +
                 std::to_string(totals.ticks_written) + ", 큐 넘쳐 버림 " + std::to_string(totals.ticks_dropped) +
                 ", 거절 " + std::to_string(totals.ticks_failed) + ", 모호 " + std::to_string(totals.ticks_ambiguous));
    }
}

namespace
{

// 연결을 확보한다. 상태표에 실패가 적혀 있으면 retry_at_ms 전에는 붙지 않고, 그 뒤에도 한 워커만 붙어 본다.
//  멈추라는 말이 올 때까지 기다린다. 멈추는 중에는 기다리지 않고, 붙어 볼 차례면 한 번만 붙어 본다.
bool acquire_connection(DbManager::State& state, Connection& connection, TickWorker& worker)
{
    const auto is_running = [&state] { return state.running.load(std::memory_order_acquire); };

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

// 넣지 못하고 끝낼 때 — 손에 든 묶음과 큐에 남은 행을 거절로 센다.
void give_up(DbManager::State& state, TickWorker& worker, uint64_t rows, const char* reason)
{
    uint64_t remaining = rows;

    while (worker.queue.pop())
    {
        remaining += 1;
    }

    state.ticks_failed.fetch_add(remaining, std::memory_order_release);
    LOG_ERROR("[DbManager] " + worker.label + " " + reason + ", " + std::to_string(remaining) + "행 못 넣음");
}

void tick_loop(DbManager::State& state, TickWorker& worker, unsigned index)
{
    thread_name::set_current("DbTick" + std::to_string(index));

    using namespace std::chrono;
    const DbConfig& config = state.config;
    Connection      connection(worker.interrupt);
    std::string     text;
    text.reserve(config.batch_rows * 64);
    const auto      is_running = [&state] { return state.running.load(std::memory_order_acquire); };
    const auto      is_idle = [&worker, &is_running] { return worker.queue.empty() && is_running(); };

    while (true)
    {
        // 1) 묶음 모으기 — batch_rows 가 차거나, 첫 행부터 flush_ms 가 지나거나, 멈추라는 말이 오면 끝.
        size_t rows = 0;
        auto   deadline = steady_clock::now();

        while (rows < config.batch_rows)
        {
            if (auto row = worker.queue.pop())
            {
                if (rows == 0)
                {
                    deadline = steady_clock::now() + milliseconds(config.flush_ms);
                }

                append_trade_row(text, row->trade, row->enqueue_ms);
                rows += 1;
                continue;
            }

            if (!is_running())
            {
                break;
            }

            if (rows == 0)
            {
                worker.wake.wait_for(1s, is_idle);
                continue;
            }

            const auto now = steady_clock::now();

            if (now >= deadline)
            {
                break;
            }

            worker.wake.wait_for(deadline - now, is_idle);
        }

        if (rows == 0)
        {
            if (!is_running() && worker.queue.empty())
            {
                return;
            }

            continue;
        }

        if (state.abandon.load(std::memory_order_acquire))
        {
            give_up(state, worker, rows, "종료 대기 한도를 넘김");
            return;
        }

        // 2) 넣기. 끝 신호 전에 끊긴 것만 다시 붙어 한 번 더 넣는다.
        //  다시 붙는 동안 쌓이는 것은 큐 용량까지만 두고, 넘치면 on_trade 가 버리고 센다.
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!acquire_connection(state, connection, worker))
            {
                // 멈추는 중에 DB가 없다 — 묶음마다 접속 시간을 기다리지 않고 남은 것을 한 번에 세고 끝낸다.
                give_up(state, worker, rows, "종료 중 DB 연결 없음");
                return;
            }

            std::string error;
            const auto  outcome = connection.copy(text, error);

            if (outcome == Connection::Outcome::kWritten)
            {
                state.ticks_written.fetch_add(rows, std::memory_order_release);
                break;
            }

            if (outcome == Connection::Outcome::kNotSent && attempt == 0 &&
                !state.abandon.load(std::memory_order_acquire))
            {
                LOG_WARN("[DbManager] " + worker.label + " 연결 끊김, 다시 넣는다 — " + error);
                connection.close();
                continue;
            }

            if (outcome == Connection::Outcome::kAmbiguous)
            {
                state.ticks_ambiguous.fetch_add(rows, std::memory_order_release);
                LOG_ERROR("[DbManager] " + worker.label + " 답 전에 끊김, " + std::to_string(rows) +
                          "행 들어갔는지 모름 — " + error);
                connection.close();
                break;
            }

            state.ticks_failed.fetch_add(rows, std::memory_order_release);
            LOG_ERROR("[DbManager] " + worker.label + " COPY 실패 " + std::to_string(rows) + "행 — " + error);

            if (!connection.usable())
            {
                connection.close();
            }

            break;
        }

        text.clear();
    }
}

} // namespace

} // namespace db
