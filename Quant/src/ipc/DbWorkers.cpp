// DbManager 적재 워커 — 체결 워커(tick_loop)와 신호·헬스 워커(event_loop)가 큐에서 묶음을 모아 COPY로 넣는다.
//  연결은 DbConnection.cpp, 공개 API는 DbManager.cpp. [why D-148] [why D-154]
#include "ipc/DbWorkerState.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <chrono>
#include <optional>
#include <string>

namespace db
{
namespace detail
{
namespace
{

constexpr const char* kTickCopyStatement   = "COPY ticks(ts,ticker,price,volume,direction,market) FROM STDIN";
constexpr const char* kSignalCopyStatement = "COPY signals(ts,strategy,ticker,side,qty,price,market,regime,account) FROM STDIN";

template <typename Item>
uint64_t drain(MpscQueue<Item>& queue)
{
    uint64_t count = 0;

    while (queue.pop())
    {
        count += 1;
    }

    return count;
}

// 넣지 못하고 끝낼 때 — 손에 든 묶음과 큐에 남은 행을 못 넣은 것으로 센다.
void give_up(const Worker& worker, Counters& counters, const char* table, uint64_t rows, const char* reason)
{
    if (rows == 0)
    {
        return;
    }

    counters.failed.fetch_add(rows, std::memory_order_release);
    LOG_ERROR("[DbManager] " + worker.label + " " + reason + ", " + table + " " + std::to_string(rows) + "행 못 넣음");
}

// 묶음 하나를 넣는다. 끝 신호 전에 끊긴 것만 다시 붙어 한 번 더 넣는다 — 다시 붙는 동안 쌓이는 것은 큐 용량까지만
//  두고, 넘치면 on_* 가 버리고 센다. false면 멈추는 중에 DB가 없다 — 부르는 쪽이 이 묶음과 남은 것을 세고 끝낸다.
bool write_batch(DbManager::State& state, Connection& connection, Worker& worker, const char* statement,
                 const std::string& text, uint64_t rows, Counters& counters)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!acquire_connection(state, connection, worker))
        {
            return false;
        }

        std::string error;
        const auto  outcome = connection.copy(statement, text, error);

        if (outcome == Connection::Outcome::kWritten)
        {
            counters.written.fetch_add(rows, std::memory_order_release);
            return true;
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
            counters.ambiguous.fetch_add(rows, std::memory_order_release);
            LOG_ERROR("[DbManager] " + worker.label + " 답 전에 끊김, " + std::to_string(rows) +
                      "행 들어갔는지 모름 — " + error);
            connection.close();
            return true;
        }

        counters.failed.fetch_add(rows, std::memory_order_release);
        LOG_ERROR("[DbManager] " + worker.label + " COPY 실패 " + std::to_string(rows) + "행 — " + error);

        if (!connection.usable())
        {
            connection.close();
        }

        return true;
    }

    return true;
}

// 체결 묶음 하나를 text에 모은다 — batch_rows 가 차거나, 첫 행부터 flush_ms 가 지나거나, 멈추라는 말이 오면 끝.
//  모은 행 수를 돌려준다.
size_t collect_tick_batch(const DbManager::State& state, TickWorker& worker, std::string& text)
{
    using namespace std::chrono;
    const DbConfig& config = state.config;
    const auto      is_running = [&state]
    {
        return state.running.load(std::memory_order_acquire);
    };
    const auto      is_idle = [&worker, &is_running]
    {
        return worker.queue.empty() && is_running();
    };
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

    return rows;
}

// 기존 DB의 health 표에도 수치 열이 있게 한다. schema.sql은 DB를 새로 만들 때만 돈다 — 옛 파이썬 적재기의
//  ensure_health_metric_columns와 같은 일을 문장 하나로 한다. 실패해도 COPY는 해 본다(열이 이미 있으면 들어간다).
void ensure_health_columns(Connection& connection, const EventWorker& worker, const HealthSnapshot& snapshot)
{
    std::string statement = "ALTER TABLE health";

    for (const std::string& column : health_metric_columns(snapshot))
    {
        statement.append(" ADD COLUMN IF NOT EXISTS ").append(column).append(" BIGINT,");
    }

    // 역할·계좌는 글자라 위(BIGINT)에 못 낀다. 역할 기본값 'order'는 이 열이 없던 시절 한 프로세스로 뜨던 날의
    //  행이 기존 쿼리에 그대로 걸리게 하려고, 계좌에 기본값이 없는 것은 옛 행의 계좌를 지어내지 않으려고다. [why D-129]
    statement.append(" ADD COLUMN IF NOT EXISTS role TEXT NOT NULL DEFAULT 'order',");
    statement.append(" ADD COLUMN IF NOT EXISTS account TEXT");
    std::string error;

    if (!connection.execute(statement.c_str(), error))
    {
        LOG_WARN("[DbManager] " + worker.label + " health 열 보장 실패 — " + error);
    }
}

std::string health_copy_statement(const HealthSnapshot& snapshot)
{
    std::string statement = "COPY health(ts,role,account,data_cnt,signal_cnt,order_cnt";

    for (const std::string& column : health_metric_columns(snapshot))
    {
        statement.push_back(',');
        statement.append(column);
    }

    statement.append(") FROM STDIN");
    return statement;
}

// 신호·헬스 워커가 루프 내내 들고 다니는 것 — 연결, 표마다의 묶음 글, health 열 보장 기록.
struct EventSession
{
    explicit EventSession(InterruptSlot& slot) : connection(slot)
    {
    }

    Connection  connection;
    std::string signal_text;
    std::string health_text;
    std::string health_statement;
    uint64_t    health_columns_generation = 0; // 이 연결에서 health 열을 보장했으면 그 연결의 번호
};

// 신호·헬스 묶음 하나의 행 수와 첫 헬스 행(열 목록을 정하는 데 쓴다).
struct EventBatch
{
    size_t                        signal_rows = 0;
    size_t                        health_rows = 0;
    std::optional<HealthSnapshot> first_health;
};

// 넣지 못하고 끝낼 때 — 두 표의 손에 든 묶음과 큐에 남은 행을 못 넣은 것으로 센다.
void give_up_events(DbManager::State& state, EventWorker& worker, uint64_t signal_rows, uint64_t health_rows,
                    const char* reason)
{
    give_up(worker, state.signals, "signals", signal_rows + drain(worker.signals), reason);
    give_up(worker, state.health, "health", health_rows + drain(worker.health), reason);
}

// 신호·헬스 묶음 하나를 모은다 — 체결 워커와 같은 규칙이다.
EventBatch collect_event_batch(const DbManager::State& state, EventWorker& worker, EventSession& session)
{
    using namespace std::chrono;
    const DbConfig& config = state.config;
    const auto      is_running = [&state]
    {
        return state.running.load(std::memory_order_acquire);
    };
    const auto      is_idle = [&worker, &is_running]
    {
        return worker.signals.empty() && worker.health.empty() && is_running();
    };
    EventBatch batch;
    auto       deadline = steady_clock::now();

    while (batch.signal_rows + batch.health_rows < config.batch_rows)
    {
        const bool first = batch.signal_rows + batch.health_rows == 0;
        bool       took = false;

        if (auto row = worker.signals.pop())
        {
            append_signal_row(session.signal_text, *row, config.account);
            batch.signal_rows += 1;
            took = true;
        }

        if (auto row = worker.health.pop())
        {
            if (batch.health_rows == 0)
            {
                batch.first_health = row->snapshot;
            }

            append_health_row(session.health_text, row->snapshot, row->ts_ms, config.role, config.account);
            batch.health_rows += 1;
            took = true;
        }

        if (took)
        {
            if (first)
            {
                deadline = steady_clock::now() + milliseconds(config.flush_ms);
            }

            continue;
        }

        if (!is_running())
        {
            break;
        }

        if (batch.signal_rows + batch.health_rows == 0)
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

    return batch;
}

// 모은 묶음을 넣는다. 신호 먼저 — 헬스는 30초 뒤 다음 행이 같은 누적 값을 다시 싣지만 신호는 한 번뿐이다.
//  false면 멈추는 중에 DB가 없어 남은 것을 이미 세었다 — 부르는 쪽은 끝낸다.
bool write_event_batch(DbManager::State& state, EventWorker& worker, EventSession& session, const EventBatch& batch)
{
    Connection& connection = session.connection;

    if (batch.signal_rows > 0)
    {
        if (!write_batch(state, connection, worker, kSignalCopyStatement, session.signal_text, batch.signal_rows,
                         state.signals))
        {
            give_up_events(state, worker, batch.signal_rows, batch.health_rows, "종료 중 DB 연결 없음");
            return false;
        }
    }

    if (batch.health_rows > 0)
    {
        // 열 목록은 구간 이름으로 만들고, 이름은 엔진이 기동 때 정한 정적 목록이라 묶음 안에서 같다.
        session.health_statement = health_copy_statement(*batch.first_health);

        if (acquire_connection(state, connection, worker) &&
            connection.generation() != session.health_columns_generation)
        {
            ensure_health_columns(connection, worker, *batch.first_health);
            session.health_columns_generation = connection.generation();
        }

        if (!write_batch(state, connection, worker, session.health_statement.c_str(), session.health_text,
                         batch.health_rows, state.health))
        {
            give_up_events(state, worker, 0, batch.health_rows, "종료 중 DB 연결 없음");
            return false;
        }
    }

    return true;
}

} // namespace

TickWorker::TickWorker(size_t capacity) : queue(capacity)
{
}

EventWorker::EventWorker(size_t signal_capacity, size_t health_capacity)
    : signals(signal_capacity), health(health_capacity)
{
}

void tick_loop(DbManager::State& state, TickWorker& worker, unsigned index)
{
    thread_name::set_current("DbTick" + std::to_string(index));

    const DbConfig& config = state.config;
    Connection      connection(worker.interrupt);
    std::string     text;
    text.reserve(config.batch_rows * 64);

    // 첫 행을 기다리지 않고 미리 붙는다. 붙는 데 9초가 걸리는데(2026-09-26 실측, WSL 안 서버까지
    //  21:44:44 기동 -> 21:44:53 연결) 그 시간이 묶음을 모은 뒤에 흐르면, 장 시작처럼 첫 순간에
    //  몰리는 구간이 통째로 큐 용량 밖으로 밀려난다. 같은 부하를 파이썬 적재기로 받으면 251만 행이
    //  들어오는데 엔진으로 받으니 135만 행만 들어왔고, 그 차이가 이 9초였다. 워커는 어차피 첫 행이
    //  올 때까지 할 일이 없으니 기다리는 자리를 앞으로 옮기는 것뿐이다.
    //  못 붙으면 그대로 둔다 — 아래 루프가 묶음마다 같은 함수로 다시 붙어 본다.
    acquire_connection(state, connection, worker);

    while (true)
    {
        // 1) 묶음 모으기.
        const size_t rows = collect_tick_batch(state, worker, text);

        if (rows == 0)
        {
            if (!state.running.load(std::memory_order_acquire) && worker.queue.empty())
            {
                return;
            }

            continue;
        }

        if (state.abandon.load(std::memory_order_acquire))
        {
            give_up(worker, state.ticks, "ticks", rows + drain(worker.queue), "종료 대기 한도를 넘김");
            return;
        }

        // 2) 넣기.
        if (!write_batch(state, connection, worker, kTickCopyStatement, text, rows, state.ticks))
        {
            // 멈추는 중에 DB가 없다 — 묶음마다 접속 시간을 기다리지 않고 남은 것을 한 번에 세고 끝낸다.
            give_up(worker, state.ticks, "ticks", rows + drain(worker.queue), "종료 중 DB 연결 없음");
            return;
        }

        text.clear();
    }
}

// 신호·헬스 워커. 두 큐를 한 묶음 주기로 모아 표마다 COPY 한 번씩 넣는다. 신호는 하루 수천 건, 헬스는 30초에
//  한 건이라 연결 하나로 넉넉하다. [why D-154]
void event_loop(DbManager::State& state, EventWorker& worker)
{
    thread_name::set_current("DbEvent");

    EventSession session(worker.interrupt);
    acquire_connection(state, session.connection, worker);

    while (true)
    {
        // 1) 묶음 모으기.
        const EventBatch batch = collect_event_batch(state, worker, session);

        if (batch.signal_rows + batch.health_rows == 0)
        {
            if (!state.running.load(std::memory_order_acquire) && worker.signals.empty() && worker.health.empty())
            {
                return;
            }

            continue;
        }

        if (state.abandon.load(std::memory_order_acquire))
        {
            give_up_events(state, worker, batch.signal_rows, batch.health_rows, "종료 대기 한도를 넘김");
            return;
        }

        // 2) 넣기.
        if (!write_event_batch(state, worker, session, batch))
        {
            return;
        }

        session.signal_text.clear();
        session.health_text.clear();
    }
}

} // namespace detail
} // namespace db
