// 기동 전 설정 — Engine의 세터 몸통. 헤더에는 선언만 둔다(D-118). 전부 start() 전에 main·EngineConfigure가 부른다.
//  Engine.h·Engine.cpp에서 옮긴 것이다. 관련 결정: D-071·D-114·D-118.

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "core/ReconcilePlan.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include "utils/Utf8.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

void Engine::set_rest_price_feed(bool rest_price_feed)
{
    feed_.rest_price_feed = rest_price_feed;
}

void Engine::set_capture_directory(const std::string& directory)
{
    feed_.capture_directory = directory;
}

void Engine::set_capture_tickers(const std::vector<std::string>& tickers)
{
    feed_.capture_tickers = tickers;
}

void Engine::set_websocket_pin_tickers(const std::vector<std::string>& tickers)
{
    websocket_pin_tickers_ = tickers;
}

void Engine::set_database(const db::DbConfig& config)
{
    database_config_ = config;
}

void Engine::set_ledger_journal(const std::string& directory, bool fsync)
{
    ledger_journal_directory_ = directory;
    ledger_journal_fsync_     = fsync;
}

void Engine::set_strategy_shards(uint32_t strategy_shards)
{
    pipeline_.strategy_shards = strategy_shards == 0 ? 1u : strategy_shards;
}

void Engine::add_feed_config(const KisConfig& kis_config)
{
    feed_.extra_feed_cfgs.push_back(kis_config);
}

void Engine::set_replay(const std::string& file, double speed, double cash)
{
    feed_.replay_file  = file;
    feed_.replay_speed = speed;
    feed_.replay_cash  = cash;
}

void Engine::set_feed_source(std::unique_ptr<feed::IFeedSource> source, double cash)
{
    feed_.feed_override = std::move(source);
    feed_.replay_cash   = cash;
}

void Engine::set_broker_offline(bool offline, double cash)
{
    feed_.broker_offline = offline;
    feed_.replay_cash    = cash;
}

void Engine::set_bootstrap_ledger(bool bootstrap_ledger)
{
    bootstrap_ledger_ = bootstrap_ledger;
}

void Engine::set_quote_kis_config(const KisConfig& quote_kis_config)
{
    feed_.quote_kis_config = quote_kis_config;
    feed_.has_quote_kis = true;
}

void Engine::set_regime_halt_expire_min(int regime_halt_expire_min)
{
    regime_file_judge_.set_halt_expire_min(regime_halt_expire_min);
}

void Engine::set_order_interval(int min_interval_ms, int max_retries)
{
    order_min_interval_ms_ = min_interval_ms;
    order_max_retries_ = max_retries;
}

void Engine::set_order_transport_threads(int thread_count)
{
    order_transport_threads_ = thread_count;
}

void Engine::set_risk_config(const OrderGate::Config& risk_config)
{
    order_gate_.set_config(risk_config);
}

void Engine::set_regime_strategies(std::map<Regime, std::vector<std::string>> regime_strategies)
{
    strategy_.regime_map = std::move(regime_strategies);
    strategy_.has_regime_map = !strategy_.regime_map.empty();
}

void Engine::set_zmq_ports(ZmqPorts ports)
{
    zmq_ports_ = ports;
}

void Engine::set_zmq_enabled(bool enabled)
{
    zmq_enabled_ = enabled;
}

void Engine::set_instance(std::string name)
{
    instance_ = std::move(name);
}

void Engine::set_ops_control(const std::string& bind_address, int port, const std::string& token)
{
    ops_.bind_address = bind_address;
    ops_.port      = port;
    ops_.token     = token;
}

void Engine::mark_exit_managed_ticker(const std::string& ticker)
{
    mark_exit_managed(symbols_.table.intern(ticker));
}

void Engine::set_paper_acknowledgement_delay(feed::PaperExecutor::AcknowledgementDelay delay)
{
    if (feed_.paper)
    {
        feed_.paper->set_acknowledgement_delay(std::move(delay));
    }
}

void Engine::set_role(ProcessRole role)
{
    role_ = role;
}

// 제어 요청 한 줄을 전략 스레드가 옮겨 줄 통로에 싣는다. 순번과 보낸 시각은 여기서 찍는다. 통로가 가득이면
//  거짓을 돌려주고 버린 수(control_dropped)만 센다 — 표를 보내는 쪽이 그 판을 접는다. [why D-114]
// 스캔 스레드가 슬리브마다 부른다(20초 간격). 파일은 임시 이름으로 쓰고 바꿔치기해
//  대시보드가 반쯤 쓰인 JSON을 읽지 않게 한다. 쓰기 실패는 매매와 무관하므로 경고만 남긴다.
void Engine::set_entry_priority(const std::vector<OrderGate::PriorityEntry>& entries, int total)
{
    nlohmann::json scores = nlohmann::json::object();

    for (const auto& entry : entries)
    {
        scores[symbols_.table.name(entry.symbol).string()] = {{"rank", entry.rank}, {"z", entry.z_score}};
    }

    // 표를 게이트에 바로 걸지 않고 주문 스레드로 보낸다 — 표를 고치는 것은 단일 시퀀서다(원칙 4).
    //  부르는 쪽이 스캔 스레드라 프로세스가 갈리면 이 자리가 통째로 막힌다. [why D-114]
    {
        ipc::ControlRequest open;
        open.kind = ipc::ControlKind::kEntryPriorityBegin;

        if (control_plane_.send(open))
        {
            uint32_t sent = 0;
            bool     full = false;

            for (const OrderGate::PriorityEntry& entry : entries)
            {
                ipc::ControlRequest row;
                row.kind      = ipc::ControlKind::kEntryPriorityEntry;
                row.batch     = open.sequence;
                row.symbol_id = entry.symbol;
                row.rank      = entry.rank;
                row.score_z   = entry.z_score;

                if (!control_plane_.send(row))
                {
                    full = true;
                    break;
                }

                ++sent;
            }

            if (full)
            {
                LOG_WARN("[Engine] 진입 우선순위 표를 보내다 통로가 가득 찼다 — 이번 판을 접는다(보낸 " +
                         std::to_string(sent) + "줄)");
            }
            else
            {
                ipc::ControlRequest close;
                close.kind      = ipc::ControlKind::kEntryPriorityCommit;
                close.batch     = open.sequence;
                close.rank      = total;
                close.row_count = sent;

                if (!control_plane_.send(close))
                {
                    LOG_WARN("[Engine] 진입 우선순위 표 마무리를 못 보냈다 — 이번 판은 걸리지 않는다");
                }
            }
        }
        else
        {
            LOG_WARN("[Engine] 진입 우선순위 " + std::to_string(entries.size()) +
                     "종목 — 제어 통로가 가득 차 이번 판을 접는다");
        }
    }

    static std::mutex file_mutex; // 두 슬리브가 겹쳐 불러도 파일은 한 번에 하나만 쓴다
    std::lock_guard<std::mutex> lock(file_mutex);
    const auto path = Logger::instance().base_directory() / "entry_scores.json";
    const auto temporary  = Logger::instance().base_directory() / "entry_scores.json.tmp";
    std::error_code error_code;
    {
        std::ofstream output_file(temporary, std::ios::trunc);

        if (!output_file.is_open())
        {
            LOG_WARN("[Engine] entry_scores.json 쓰기 실패: " + temporary.string());
            return;
        }

        const auto now = std::chrono::system_clock::now();
        output_file << nlohmann::json{{"ts", std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()},
                             {"total", total},
                             {"unscored_z", order_gate_.config().displace_unscored_z},
                             {"scores", std::move(scores)}}
                  .dump();
    }

    std::filesystem::rename(temporary, path, error_code);

    if (error_code)
    {
        LOG_WARN("[Engine] entry_scores.json 교체 실패: " + error_code.message());
    }
}

void Engine::set_session_end(int close_min, int grace_sec)
{
    session_end::Config config;
    config.close_min = close_min;
    config.grace_sec = grace_sec;
    session_end_ = session_end::Judge(config);
}

void Engine::set_universe_rescan(std::function<std::vector<symbol::SymbolId>(KisClient&)> universe_fn,
                                 std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)> factory,
                                 int interval_sec, size_t max_registered, int drop_after_sec, int block_after_sec,
                                 int return_confirm, bool follow_board)
{
    // 슬리브마다 한 번씩 부른다 — 덮어쓰지 않고 쌓는다. 예전에는 단일 슬롯이라
    //  두 번째 호출이 첫 번째를 조용히 지웠다(먼저 건 재스캔이 사라짐).
    UniverseRescan::Job rescan_job;
    rescan_job.universe_fn = std::move(universe_fn);
    rescan_job.factory = std::move(factory);
    rescan_job.interval_sec = interval_sec;
    rescan_job.max_registered = max_registered;
    rescan_job.drop_after_sec = drop_after_sec;
    rescan_job.block_after_sec = block_after_sec;
    rescan_job.return_confirm = return_confirm;
    rescan_job.follow_board = follow_board;
    universe_rescan_.add_job(std::move(rescan_job));
}

void Engine::set_zmq_control(const std::string& bind_address, const std::string& token)
{
    if (!bind_address.empty())
    {
        zmq_bind_address_ = bind_address;
    }

    zmq_control_token_ = token;
}

void Engine::set_protective_orders(const std::string& mode, int interval_ms, int retry_ms)
{
    protective_book_.set_mode(risk::protective_mode_from_string(mode));
    protective_interval_ = std::chrono::milliseconds(interval_ms > 0 ? interval_ms : kProtectiveIntervalMsDefault);
    protective_book_.set_retry_interval(std::chrono::milliseconds(retry_ms > 0 ? retry_ms : kProtectiveRetryMsDefault));
}

void Engine::mark_exit_managed(symbol::SymbolId symbol)
{
    if (symbol != symbol::kNone && symbol < symbols_.exit_managed.size())
    {
        symbols_.exit_managed[symbol] = true;
    }
}
