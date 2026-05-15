#include "core/Engine.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>

using namespace std::chrono_literals;

Engine::Engine(KisConfig kis_cfg, int fetch_interval_sec)
    : kis_cfg_(std::move(kis_cfg)), fetch_interval_sec_(fetch_interval_sec)
{
}

Engine::~Engine()
{
    stop();
}

void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)
{
    LOG_INFO("[Engine] 전략 등록: " + strategy->describe());
    strategies_.push_back(std::move(strategy));
}

void Engine::start()
{
    if (running_.load())
        return;

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");

#ifdef HAS_ZMQ
    zmq_bridge_ = std::make_unique<ZmqBridge>();
    zmq_bridge_->set_command_handler(
        [this](const std::string& cmd) -> std::string
        {
            if (cmd == "KILL")
            {
                LOG_WARN("[ZMQ] KILL 명령 수신 — 신규 주문 차단 + 엔진 종료");
                order_gate_.set_kill_switch(true);
                running_.store(false);
                return "OK";
            }
            if (cmd == "STATUS")
            {
                return "{\"running\":true"
                       ",\"data\":" +
                       std::to_string(data_count_.load()) + ",\"signal\":" + std::to_string(signal_count_.load()) +
                       ",\"order\":" + std::to_string(order_count_.load()) + "}";
            }
            return "UNKNOWN";
        });
    zmq_bridge_->start();
#endif

    kis_ = std::make_unique<KisClient>(kis_cfg_);
    if (!kis_->authenticate())
    {
        LOG_ERROR("[Engine] KIS 인증 실패");
        return;
    }

    // 전략 초기화 (kis_ 주입 → on_start 내부에서 Universe 조회)
    for (auto& s : strategies_)
    {
        s->set_kis(kis_.get());
        s->on_start();
    }

    // 전략별 구독 스펙 수집 (중복 제거)
    watch_specs_.clear();
    {
        std::unordered_set<std::string> seen;
        for (auto& s : strategies_)
        {
            for (auto& spec : s->get_watch_specs())
            {
                std::string key = (spec.market == Market::US ? "US:" : "KR:") + spec.exchange + ":" + spec.ticker;
                if (seen.insert(key).second)
                    watch_specs_.push_back(spec);
            }
        }
    }
    LOG_INFO("[Engine] WS 구독 종목: " + std::to_string(watch_specs_.size()) + "개");

    running_.store(true);

    // WebSocket — 동적 구독 스펙으로 연결
    if (!watch_specs_.empty())
    {
        ws_ = std::make_unique<KisWebSocket>(kis_cfg_);
        ws_->set_callbacks([this](const OrderBook& ob) { ob_queue_.push(ob); },
                           [this](const TradeData& td)
                           {
                               td_queue_.push(td);
#ifdef HAS_ZMQ
                               if (zmq_bridge_)
                                   zmq_bridge_->publish_trade(td);
#endif
                           });
        if (!ws_->connect(watch_specs_))
            LOG_WARN("[Engine] WebSocket 연결 실패 — 호가/체결 이벤트 없이 동작");
    }

    data_thread_ = std::thread(&Engine::data_thread_fn, this);
    strategy_thread_ = std::thread(&Engine::strategy_thread_fn, this);
    order_thread_ = std::thread(&Engine::order_thread_fn, this);
    control_thread_ = std::thread(&Engine::control_thread_fn, this);

    LOG_INFO("[Engine] 모든 스레드 시작 완료");
}

void Engine::stop()
{
    if (!running_.load())
        return;
    running_.store(false);

    if (ws_ && ws_->is_connected())
        ws_->disconnect();

    if (data_thread_.joinable())
        data_thread_.join();
    if (strategy_thread_.joinable())
        strategy_thread_.join();
    if (order_thread_.joinable())
        order_thread_.join();
    if (control_thread_.joinable())
        control_thread_.join();

#ifdef HAS_ZMQ
    if (zmq_bridge_)
        zmq_bridge_->stop();
#endif

    for (auto& s : strategies_)
        s->on_stop();
    print_stats();
    LOG_INFO("[Engine] ── 퀀트 엔진 종료 ──────────────────────────────");
}

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn()
{
    LOG_INFO("[DataThread] 시작");
    bool was_market_open = false;
    while (running_.load())
    {
        bool market_now = is_any_market_open();

        // 장 시작 감지 → 일별 카운터 리셋
        if (market_now && !was_market_open)
        {
            order_gate_.reset_daily();
            LOG_INFO("[DataThread] 장 시작 — OrderGate 일별 카운터 리셋");
        }
        was_market_open = market_now;

        if (!market_now)
        {
            std::this_thread::sleep_for(60s);
            continue;
        }

        for (const auto& spec : watch_specs_)
        {
            std::vector<MarketData> bars;
            if (spec.market == Market::KR)
                bars = kis_->get_daily_ohlcv(spec.ticker, 1);
            else
                bars = kis_->get_us_daily_ohlcv(spec.ticker, 1, spec.exchange);

            if (bars.empty())
                continue;
            auto& md = bars[0];
            md.bar_index = static_cast<int>(data_count_.load());
            while (!market_queue_.push(md) && running_.load())
                std::this_thread::sleep_for(1ms);
            ++data_count_;
        }
        std::this_thread::sleep_for(std::chrono::seconds(fetch_interval_sec_));
#ifdef HAS_ZMQ
        if (zmq_bridge_)
            zmq_bridge_->publish_health(data_count_.load(), signal_count_.load(), order_count_.load());
#endif
    }
    LOG_INFO("[DataThread] 종료");
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
// ob_queue_(호가) → td_queue_(체결) → market_queue_(일봉) 순 우선처리
// 아이들 시 100µs 슬립 → 저지연 유지
void Engine::strategy_thread_fn()
{
    LOG_INFO("[StrategyThread] 시작");

    auto push_signal = [&](const OrderSignal& sig)
    {
        ++signal_count_;
        LOG_INFO("[Strategy] 신호: [" + sig.strategy_id + "] " + sig.ticker + " " +
                 (sig.side == OrderSide::BUY ? "BUY" : "SELL") + " " + std::to_string(sig.quantity));
#ifdef HAS_ZMQ
        if (zmq_bridge_)
            zmq_bridge_->publish_signal(sig);
#endif
        while (!order_queue_.push(sig) && running_.load())
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    };

    while (running_.load())
    {
        bool did_work = false;

        // 호가 (국내 — 고주파)
        while (auto opt = ob_queue_.pop())
        {
            for (auto& s : strategies_)
            {
                auto sig = s->on_order_book(*opt);
                if (sig && sig->side != OrderSide::NONE)
                    push_signal(*sig);
            }
            did_work = true;
        }

        // 체결 (미국 + 국내)
        while (auto opt = td_queue_.pop())
        {
            for (auto& s : strategies_)
            {
                auto sig = s->on_trade(*opt);
                if (sig && sig->side != OrderSide::NONE)
                    push_signal(*sig);
            }
            did_work = true;
        }

        // 일봉
        if (auto opt = market_queue_.pop())
        {
            for (auto& s : strategies_)
            {
                auto sig = s->on_data(*opt);
                if (sig && sig->side != OrderSide::NONE)
                    push_signal(*sig);
            }
            did_work = true;
        }

        if (!did_work)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    LOG_INFO("[StrategyThread] 종료");
}

// ─── 주문 실행 스레드 ─────────────────────────────────────────────────────
void Engine::order_thread_fn()
{
    LOG_INFO("[OrderThread] 시작");
    while (running_.load())
    {
        auto opt = order_queue_.pop();
        if (!opt)
        {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        std::string reject_reason;
        if (!order_gate_.check(*opt, reject_reason))
        {
            LOG_WARN("[OrderGate] 주문 거부 [" + opt->strategy_id + "] " + opt->ticker + " → " + reject_reason);
            continue;
        }

        bool ok = (opt->market == Market::US) ? kis_->send_us_order(*opt) : kis_->send_order(*opt);
        if (ok)
        {
            ++order_count_;
            order_gate_.on_fill(opt->ticker, opt->side, opt->quantity, opt->price);
        }
#ifdef HAS_ZMQ
        if (zmq_bridge_)
            zmq_bridge_->publish_order(*opt, ok);
#endif
    }
    LOG_INFO("[OrderThread] 종료");
}

// ─── 장 시간 체크 ─────────────────────────────────────────────────────────
bool Engine::is_kr_market_open() const
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt
    {
    };
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    if (lt.tm_wday == 0 || lt.tm_wday == 6)
        return false;
    int m = lt.tm_hour * 60 + lt.tm_min;
    return m >= 540 && m < 930; // 09:00~15:30 KST
}

// 미국 정규장: ET 09:30~16:00 = KST 22:30~05:00 (다음날)
bool Engine::is_us_market_open() const
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt
    {
    };
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    if (lt.tm_wday == 0 || lt.tm_wday == 6)
        return false;
    int m = lt.tm_hour * 60 + lt.tm_min;
    // KST 22:30~익일 05:00 → 1350~1500 (당일), 0~300 (익일)
    return (m >= 1350) || (m < 300);
}

bool Engine::is_any_market_open() const
{
    return is_kr_market_open() || is_us_market_open();
}

void Engine::print_stats() const
{
    LOG_INFO("[Engine] 수집: " + std::to_string(data_count_.load()) +
             "  신호: " + std::to_string(signal_count_.load()) + "  주문: " + std::to_string(order_count_.load()));
}

// ─── ZMQ 제어 스레드 ──────────────────────────────────────────────────────
// ZmqBridge 자체 스레드가 REP 소켓을 처리하므로 이 스레드는
// running_ 감시만 담당 (ZMQ 없을 때도 컴파일 가능하도록 유지)
void Engine::control_thread_fn()
{
    using namespace std::chrono_literals;
    while (running_.load())
    {
        std::this_thread::sleep_for(500ms);
    }
}
