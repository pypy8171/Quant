// DbManager 적재 워커가 나눠 쓰는 내부 타입 — 연결(libpq)과 취소 수단, 워커와 큐, 표마다의 행 수 셈, DbManager::State.
//  libpq 헤더를 끌어오므로 HAS_PQ일 때만 빌드되는 Quant/src/ipc/Db*.cpp만 포함한다(바깥은 ipc/DbManager.h만 본다). [why D-148]
#pragma once

#include "ipc/DbManager.h"
#include "core/MpscQueue.h"
#include "core/WakeGate.h"

#include <libpq-fe.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace db
{
namespace detail
{

constexpr int kConnectTimeoutSeconds = 5; // 접속 한 번의 한도(초). stop()의 마지막 기다림도 이 값으로 정한다

// 워커가 지금 쓰는 연결의 취소 수단. stop()이 기다리다 못 하면 interrupt()로 막힌 libpq 호출을 오류로 돌려보낸다.
//  서버가 잡고 있는 경우(잠금 대기 등)는 PQcancel이 풀고 — 윈도우에서는 소켓 shutdown만으로 select가 깨지지 않았다
//  (2026-09-26 test_db_writer) — 상대가 사라진 경우는 리눅스에서 shutdown이 푼다. 윈도우에서 상대가 사라졌으면
//  keepalive가 끊을 때까지 못 푸니 stop()이 스레드를 떼어 둔다.
//  close() 쪽은 clear() 뒤에 PQfinish 하므로, interrupt()가 닫힌 소켓 번호를 건드리는 일은 없다.
class InterruptSlot
{
public:
    void set(PGconn* handle);
    void clear();
    void interrupt();

private:
    std::mutex                mutex_;
    int                       socket_ = -1;
    std::shared_ptr<PGcancel> cancel_;
};

// 지금 시각(epoch 밀리초). 큐에 넣는 시각과 재접속 시각에 쓴다.
int64_t now_ms();

// 환경변수 값. 없거나 비었으면 fallback.
std::string environment_or(const char* name, const std::string& fallback);

// 연결 하나. 워커 하나만 쓴다.
class Connection
{
public:
    explicit Connection(InterruptSlot& slot);
    ~Connection();
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool usable() const
    {
        return handle_ != nullptr && PQstatus(handle_) == CONNECTION_OK;
    }

    [[nodiscard]] PGconn* handle() const
    {
        return handle_;
    }

    // 연결을 새로 열 때마다 1씩 는다. 연결마다 한 번 할 일(health 열 보장)을 다시 할지 가르는 데 쓴다.
    [[nodiscard]] uint64_t generation() const
    {
        return generation_;
    }

    bool open(const DbConfig& config, const std::string& password, const std::string& label);
    void close();

    // 결과 셋. 이 셋만 보고 부르는 쪽이 다시 넣을지를 정한다.
    enum class Outcome
    {
        kWritten,   // 들어갔다
        kNotSent,   // 끝 신호 전에 끊겼다 — 서버가 COPY를 버렸다. 다시 붙어 한 번 더 넣어도 된다
        kRejected,  // 연결은 살아 있고 서버가 거절했다 — 다시 넣어도 같다
        kAmbiguous, // 끝 신호는 갔는데 답 전에 끊겼다 — 들어갔는지 모른다
    };

    // 결과를 돌려주지 않는 문장 하나(ALTER 등). 실패하면 오류 글을 채우고 false.
    bool execute(const char* statement, std::string& error);
    Outcome copy(const char* statement, const std::string& text, std::string& error);

private:
    void drain_results();

    InterruptSlot& slot_;
    PGconn*        handle_ = nullptr;
    uint64_t       generation_ = 0;
};

struct Row
{
    TradeData trade;
    int64_t   enqueue_ms = 0;
};

struct HealthRow
{
    int64_t        ts_ms = 0;
    HealthSnapshot snapshot;
};

// 적재 워커 하나의 스레드·깨우기·취소 수단. 큐는 아래 두 갈래가 따로 든다.
struct Worker
{
    std::string        label;
    wake::WakeGate     wake;
    InterruptSlot      interrupt;
    std::promise<void> done; // 워커 스레드가 끝나면 채운다 — stop()이 시간 한도를 두고 기다리는 데 쓴다
    std::future<void>  finished = done.get_future(); // [inv] done 뒤에 선언한다(먼저 만들어진 done에서 뜬다)
    std::thread        thread;
};

// 체결 적재 워커. 수신 스레드 여럿이 넣는다.
struct TickWorker : Worker
{
    explicit TickWorker(size_t capacity);

    MpscQueue<Row> queue;
};

// 신호·헬스 적재 워커. 신호는 전략 스레드, 헬스는 데이터 스레드가 넣는다 — 역할을 갈라 띄운 날과 한 프로세스로
//  띄운 날의 생산자가 달라 MPSC로 둔다(원칙 5). [why D-154]
struct EventWorker : Worker
{
    EventWorker(size_t signal_capacity, size_t health_capacity);

    MpscQueue<SignalRow> signals;
    MpscQueue<HealthRow> health;
};

// 표 하나의 행 수 셈. 넣음 + 버림 + 못 넣음 + 모호가 받음에 닿으면 큐가 빈 것이다.
struct Counters
{
    std::atomic<uint64_t> offered{0};
    std::atomic<uint64_t> written{0};
    std::atomic<uint64_t> dropped{0};   // 큐가 차서 버린 행
    std::atomic<uint64_t> failed{0};    // 서버가 거절했거나 끝낼 때 DB가 없어 못 넣은 행
    std::atomic<uint64_t> ambiguous{0}; // 끝 신호 뒤 답 전에 끊긴 행

    [[nodiscard]] bool settled() const
    {
        return written.load(std::memory_order_acquire) + dropped.load(std::memory_order_relaxed) +
                   failed.load(std::memory_order_relaxed) + ambiguous.load(std::memory_order_relaxed) >=
               offered.load(std::memory_order_relaxed);
    }
};

} // namespace detail

struct DbManager::State
{
    DbConfig                                         config;
    std::string                                      password;
    std::vector<std::unique_ptr<detail::TickWorker>> tick_workers;
    std::unique_ptr<detail::EventWorker>             event_worker; // 계좌가 없으면 비어 있다
    std::vector<detail::Worker*>                     workers;      // 위 둘을 한 줄로 — stop()이 차례로 거둔다
    std::atomic<bool>                                running{false};
    std::atomic<bool>                                abandon{false}; // stop()이 기다리기를 그만뒀다 — 남은 행은 넣지 않고 센다
    // 연결 상태표 — 모든 워커가 나눠 본다. 접속이 한 번 실패하면 retry_at_ms까지 아무도 다시 붙지 않고,
    //  그 뒤에도 probing을 잡은 워커 하나만 붙어 본다. 나머지는 그동안 기다린다(행은 큐에 쌓이다 넘치면 버린다).
    std::atomic<int64_t> retry_at_ms{0}; // 0이면 실패 기록 없음 — 워커마다 각자 붙는다
    std::atomic<int>     backoff_ms{1000};
    std::atomic<bool>    probing{false};
    detail::Counters     ticks;
    detail::Counters     signals;
    detail::Counters     health;
};

namespace detail
{

// 연결을 확보한다. 상태표에 실패가 적혀 있으면 retry_at_ms 전에는 붙지 않고, 그 뒤에도 한 워커만 붙어 본다.
//  멈추라는 말이 올 때까지 기다린다. 멈추는 중에는 기다리지 않고, 붙어 볼 차례면 한 번만 붙어 본다.
//  정의는 Quant/src/ipc/DbConnection.cpp.
bool acquire_connection(DbManager::State& state, Connection& connection, Worker& worker);

// 적재 워커 스레드 본체. 정의는 Quant/src/ipc/DbWorkers.cpp.
void tick_loop(DbManager::State& state, TickWorker& worker, unsigned index);
void event_loop(DbManager::State& state, EventWorker& worker);

} // namespace detail
} // namespace db
