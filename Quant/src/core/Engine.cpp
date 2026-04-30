#include "core/Engine.h"
#include "utils/Logger.h"
#include <chrono>
#include <thread>
#include <ctime>
#include <sstream>
#include <iomanip>

using namespace std::chrono_literals;

Engine::Engine(KisConfig kis_cfg,
               std::vector<std::string> tickers,
               int fetch_interval_sec)
    : kis_cfg_(std::move(kis_cfg))
    , tickers_(std::move(tickers))
    , fetch_interval_sec_(fetch_interval_sec)
{}

Engine::~Engine() {
    stop();
}

void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy) {
    LOG_INFO("[Engine] 전략 등록: " + strategy->describe());
    strategies_.push_back(std::move(strategy));
}

void Engine::start() {
    if (running_.load()) return;

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");
    LOG_INFO("[Engine] 종목: " + [&]{
        std::string s;
        for (auto& t : tickers_) s += t + " ";
        return s;
    }());
    LOG_INFO("[Engine] 전략 수: " + std::to_string(strategies_.size()));
    LOG_INFO("[Engine] 수집 주기: " + std::to_string(fetch_interval_sec_) + "초");

    // KIS 인증
    kis_ = std::make_unique<KisClient>(kis_cfg_);
    if (!kis_->authenticate()) {
        LOG_ERROR("[Engine] KIS 인증 실패 — 엔진 시작 중단");
        return;
    }

    // 전략 초기화
    for (auto& s : strategies_) s->on_start();

    running_.store(true);

    // 스레드 시작
    data_thread_     = std::thread(&Engine::data_thread_fn,     this);
    strategy_thread_ = std::thread(&Engine::strategy_thread_fn, this);
    order_thread_    = std::thread(&Engine::order_thread_fn,    this);

    LOG_INFO("[Engine] 모든 스레드 시작 완료");
}

void Engine::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (data_thread_.joinable())     data_thread_.join();
    if (strategy_thread_.joinable()) strategy_thread_.join();
    if (order_thread_.joinable())    order_thread_.join();

    for (auto& s : strategies_) s->on_stop();

    print_stats();
    LOG_INFO("[Engine] ── 퀀트 엔진 종료 ──────────────────────────────");
}

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn() {
    LOG_INFO("[DataThread] 시작");

    while (running_.load()) {
        if (!is_market_open()) {
            LOG_INFO("[DataThread] 장 외 시간 — 대기 중...");
            std::this_thread::sleep_for(60s);
            continue;
        }

        for (const auto& ticker : tickers_) {
            auto bars = kis_->get_daily_ohlcv(ticker, 1);
            if (bars.empty()) continue;

            auto& md = bars[0];
            md.bar_index = static_cast<int>(data_count_.load());

            // RingBuffer에 push (lock-free)
            while (!market_queue_.push(md) && running_.load()) {
                // 버퍼 가득 참 → 잠시 대기
                std::this_thread::sleep_for(1ms);
            }

            ++data_count_;
            LOG_DEBUG("[DataThread] " + ticker + " 시세 수집: "
                      + std::to_string(md.close));
        }

        std::this_thread::sleep_for(
            std::chrono::seconds(fetch_interval_sec_));
    }

    LOG_INFO("[DataThread] 종료");
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
void Engine::strategy_thread_fn() {
    LOG_INFO("[StrategyThread] 시작");

    while (running_.load()) {
        auto md_opt = market_queue_.pop();
        if (!md_opt) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const auto& md = *md_opt;

        for (auto& strategy : strategies_) {
            auto signal = strategy->on_data(md);
            if (!signal) continue;

            ++signal_count_;
            LOG_INFO("[Strategy] 신호 발생: ["
                     + signal->strategy_id + "] "
                     + signal->ticker + " "
                     + (signal->side == OrderSide::BUY ? "BUY" : "SELL")
                     + " " + std::to_string(signal->quantity) + "주");

            // 주문 큐에 push
            while (!order_queue_.push(*signal) && running_.load()) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    LOG_INFO("[StrategyThread] 종료");
}

// ─── 주문 실행 스레드 ─────────────────────────────────────────────────────
void Engine::order_thread_fn() {
    LOG_INFO("[OrderThread] 시작");

    while (running_.load()) {
        auto sig_opt = order_queue_.pop();
        if (!sig_opt) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        bool ok = kis_->send_order(*sig_opt);
        if (ok) ++order_count_;
    }

    LOG_INFO("[OrderThread] 종료");
}

// ─── 장 시간 체크 (KST 09:00 ~ 15:30) ────────────────────────────────────
bool Engine::is_market_open() const {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    struct tm* lt = std::localtime(&t);

    int h = lt->tm_hour;
    int m = lt->tm_min;
    int wday = lt->tm_wday;  // 0=일, 6=토

    // 주말 제외
    if (wday == 0 || wday == 6) return false;

    int total_min = h * 60 + m;
    return (total_min >= 9 * 60) && (total_min < 15 * 60 + 30);
}

// ─── 통계 출력 ────────────────────────────────────────────────────────────
void Engine::print_stats() const {
    LOG_INFO("[Engine] ── 통계 ─────────────────────────────────────────");
    LOG_INFO("[Engine] 수집된 시세: " + std::to_string(data_count_.load()));
    LOG_INFO("[Engine] 발생 신호:   " + std::to_string(signal_count_.load()));
    LOG_INFO("[Engine] 실행 주문:   " + std::to_string(order_count_.load()));
}
