// DbManager 공개 API — 기동·종료와 큐에 넣기(on_trade·on_signal·on_health), 셈 조회. 연결은 DbConnection.cpp,
//  적재 루프는 DbWorkers.cpp, COPY 글은 DbCopyFormat.cpp. [why D-148]
#include "ipc/DbManager.h"
#include "ipc/DbWorkerState.h"
#include "utils/Logger.h"
#include "utils/ThreadGuard.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace db
{

// on_trade(hot path)가 부르므로 정의를 이 파일에 둔다 — 같은 번역 단위라 컴파일러가 펼칠 수 있다.
int64_t detail::now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

using detail::environment_or;
using detail::event_loop;
using detail::EventWorker;
using detail::HealthRow;
using detail::kConnectTimeoutSeconds;
using detail::now_ms;
using detail::Row;
using detail::tick_loop;
using detail::TickWorker;
using detail::Worker;

DbManager::DbManager(DbConfig config) : state_(std::make_shared<State>())
{
    State& state = *state_;
    state.config = std::move(config);
    state.password = environment_or("TSDB_PASSWORD", "");

    if (state.password.empty())
    {
        LOG_ERROR("[DbManager] TSDB_PASSWORD 환경변수가 없다 — 체결·신호·헬스를 DB에 넣지 않는다");
        return;
    }

    const unsigned tick_count = state.config.tick_workers;

    for (unsigned index = 0; index < tick_count; ++index)
    {
        auto worker = std::make_unique<TickWorker>(state.config.tick_queue_capacity);
        worker->label = "적재 워커 " + std::to_string(index);
        state.workers.push_back(worker.get());
        state.tick_workers.push_back(std::move(worker));
    }

    // 계좌가 없으면 신호·헬스는 넣지 않는다 — 어느 엔진의 행인지 가를 수 없어서다(옛 적재기도 계좌 없는
    //  메시지를 버렸다). 부하 하네스처럼 계좌 없이 뜨는 엔진이 운영 표에 섞이지 않는다. [why D-154]
    if (state.config.account.empty())
    {
        LOG_WARN("[DbManager] 계좌가 비어 신호·헬스를 DB에 넣지 않는다");
    }
    else
    {
        state.event_worker = std::make_unique<EventWorker>(state.config.signal_queue_capacity,
                                                           state.config.health_queue_capacity);
        state.event_worker->label = "신호·헬스 워커";
        state.workers.push_back(state.event_worker.get());
    }

    if (state.workers.empty())
    {
        LOG_WARN("[DbManager] 띄울 적재 워커가 없다 — DB에 넣지 않는다");
        return;
    }

    state.running.store(true, std::memory_order_release);

    // 스레드가 상태를 나눠 가진다 — stop()이 떼어 둔 스레드가 늦게 돌아와도 상태는 살아 있다.
    //  적재 루프가 던지면 그 워커만 끝낸다(로그를 남기고). 다시 돌리지 않는 이유: 연결·묶음 문자열이 어떤 상태인지
    //  모르고, DB 적재는 매매 경로가 아니라 멈춰도 주문에는 영향이 없다(큐가 차면 on_trade가 버리며 센다).
    //  done은 예외가 나도 채운다 — 안 채우면 stop()이 종료 대기 한도까지 선다.
    for (unsigned index = 0; index < tick_count; ++index)
    {
        TickWorker& worker = *state.tick_workers[index];
        worker.thread = std::thread(
            [shared = state_, &worker, index]
            {
                thread_guard::run_and_log("DbManager 체결 적재",
                                          [&shared, &worker, index]
                                          {
                                              tick_loop(*shared, worker, index);
                                          });
                worker.done.set_value();
            });
    }

    if (state.event_worker != nullptr)
    {
        EventWorker& worker = *state.event_worker;
        worker.thread = std::thread(
            [shared = state_, &worker]
            {
                thread_guard::run_and_log("DbManager 신호·헬스 적재",
                                          [&shared, &worker]
                                          {
                                              event_loop(*shared, worker);
                                          });
                worker.done.set_value();
            });
    }

    LOG_INFO("[DbManager] 시작 — 적재 워커 " + std::to_string(tick_count) + "개, 묶음 " + std::to_string(state.config.batch_rows) + "행, " +
             std::to_string(state.config.flush_ms) + "ms, 신호·헬스 워커 " +
             std::to_string(state.event_worker != nullptr ? 1 : 0) + "개(역할 " + state.config.role + ")");
}

DbManager::~DbManager()
{
    stop();
}

bool DbManager::ok() const noexcept
{
    return !state_->workers.empty();
}

void DbManager::on_trade(const TradeData& trade) noexcept
{
    State& state = *state_;

    if (state.tick_workers.empty())
    {
        return;
    }

    state.ticks.offered.fetch_add(1, std::memory_order_relaxed);
    TickWorker& worker = *state.tick_workers[trade.symbol_id % state.tick_workers.size()];

    if (!worker.queue.push(Row{trade, now_ms()}))
    {
        state.ticks.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    worker.wake.notify();
}

void DbManager::on_signal(const OrderSignal& signal, std::string_view regime) noexcept
{
    State& state = *state_;

    if (state.event_worker == nullptr)
    {
        return;
    }

    state.signals.offered.fetch_add(1, std::memory_order_relaxed);

    if (!state.event_worker->signals.push(make_signal_row(signal, regime, now_ms())))
    {
        state.signals.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    state.event_worker->wake.notify();
}

void DbManager::on_health(const HealthSnapshot& snapshot) noexcept
{
    State& state = *state_;

    if (state.event_worker == nullptr)
    {
        return;
    }

    state.health.offered.fetch_add(1, std::memory_order_relaxed);

    if (!state.event_worker->health.push(HealthRow{now_ms(), snapshot}))
    {
        state.health.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    state.event_worker->wake.notify();
}

void DbManager::flush_ticks()
{
    const State& state = *state_;

    while (!state.ticks.settled())
    {
        for (const auto& worker : state.tick_workers)
        {
            worker->wake.notify();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DbManager::flush_events()
{
    const State& state = *state_;

    if (state.event_worker == nullptr)
    {
        return;
    }

    while (!state.signals.settled() || !state.health.settled())
    {
        state.event_worker->wake.notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

DbStatistics DbManager::statistics() const noexcept
{
    const State& state = *state_;
    DbStatistics statistics;
    statistics.ticks_offered = state.ticks.offered.load(std::memory_order_relaxed);
    statistics.ticks_written = state.ticks.written.load(std::memory_order_relaxed);
    statistics.ticks_dropped = state.ticks.dropped.load(std::memory_order_relaxed);
    statistics.ticks_failed = state.ticks.failed.load(std::memory_order_relaxed);
    statistics.ticks_ambiguous = state.ticks.ambiguous.load(std::memory_order_relaxed);
    statistics.signals_offered = state.signals.offered.load(std::memory_order_relaxed);
    statistics.signals_written = state.signals.written.load(std::memory_order_relaxed);
    statistics.signals_dropped = state.signals.dropped.load(std::memory_order_relaxed);
    statistics.signals_failed = state.signals.failed.load(std::memory_order_relaxed) +
                                state.signals.ambiguous.load(std::memory_order_relaxed);
    statistics.health_offered = state.health.offered.load(std::memory_order_relaxed);
    statistics.health_written = state.health.written.load(std::memory_order_relaxed);
    statistics.health_dropped = state.health.dropped.load(std::memory_order_relaxed);
    statistics.health_failed = state.health.failed.load(std::memory_order_relaxed) +
                               state.health.ambiguous.load(std::memory_order_relaxed);
    return statistics;
}

void DbManager::stop()
{
    using namespace std::chrono;
    State& state = *state_;
    state.running.store(false, std::memory_order_release);
    const auto& workers = state.workers;

    for (Worker* worker : workers)
    {
        worker->wake.notify();
    }

    // 1) 남은 행을 넣을 시간을 준다.
    const auto grace_deadline = steady_clock::now() + milliseconds(state.config.stop_grace_ms);
    bool       late = false;

    for (Worker* worker : workers)
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

        for (Worker* worker : workers)
        {
            worker->interrupt.interrupt();
            worker->wake.notify();
        }
    }

    // 3) 그래도 안 돌아오는 스레드는 떼어 둔다 — 엔진 종료를 DB에 묶지 않는다.
    const auto last_deadline = steady_clock::now() + seconds(kConnectTimeoutSeconds + 1);
    bool       reaped = false;

    for (Worker* worker : workers)
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
    //  첫 줄의 모양은 scripts/check_runtime_health.py DB_WRITER_END_RE가 읽는다 — 바꾸면 거기도 고친다. [wire]
    if (reaped)
    {
        const DbStatistics totals = statistics();
        LOG_INFO("[DbManager] 종료 — 받음 " + std::to_string(totals.ticks_offered) + ", 넣음 " +
                 std::to_string(totals.ticks_written) + ", 큐 넘쳐 버림 " + std::to_string(totals.ticks_dropped) +
                 ", 거절 " + std::to_string(totals.ticks_failed) + ", 모호 " + std::to_string(totals.ticks_ambiguous));

        if (state.event_worker != nullptr)
        {
            LOG_INFO("[DbManager] 종료(신호·헬스) — 신호 받음 " + std::to_string(totals.signals_offered) + ", 넣음 " +
                     std::to_string(totals.signals_written) + ", 버림 " + std::to_string(totals.signals_dropped) +
                     ", 못 넣음 " + std::to_string(totals.signals_failed) + " / 헬스 받음 " +
                     std::to_string(totals.health_offered) + ", 넣음 " + std::to_string(totals.health_written) +
                     ", 버림 " + std::to_string(totals.health_dropped) + ", 못 넣음 " +
                     std::to_string(totals.health_failed));
        }
    }
}

} // namespace db
