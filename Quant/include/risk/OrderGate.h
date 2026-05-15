#pragma once
#include "core/Types.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// OrderGate  —  주문 전 위험 검증 게이트
//
//  Engine::order_thread_fn이 send_order 직전에 check()를 통과한 신호만 실행.
//  Engine이 on_fill()·add_realized_pnl()로 내부 상태를 업데이트한다.
//
//  체크 항목:
//   1. Kill switch   — 강제 중단 플래그
//   2. 포지션 수량   — 종목당 최대 보유 수량
//   3. 일일 손실     — 일일 최대 손실 초과 시 신규 매수 거부
//   4. Rate limit    — 분당 최대 주문수 초과 방지
//   5. 중복 신호     — 동일 strategy+ticker 1초 이내 중복 거부
// ─────────────────────────────────────────────────────────────────────────────
class OrderGate
{
public:
    struct Config
    {
        int max_qty_per_ticker = 100;         // 종목당 최대 보유 수량
        double daily_loss_limit = -300'000.0; // 일일 최대 손실 (-30만원)
        int max_orders_per_min = 20;          // 분당 최대 주문
        double dedup_window_sec = 1.0;        // 중복 신호 제거 윈도우(초)
    };

    OrderGate() : cfg_()
    {
    }
    explicit OrderGate(Config cfg) : cfg_(cfg)
    {
    }

    // ── 주문 검증 (true = 통과, false = 거부) ──────────────────────────────
    bool check(const OrderSignal& sig, std::string& reject_reason);

    // ── 상태 업데이트 (Engine이 체결 후 호출) ────────────────────────────────
    void on_fill(const std::string& ticker, OrderSide side, int qty, double price);
    void add_realized_pnl(double pnl); // SELL 체결 시 실현 손익 추가

    // ── Kill switch ─────────────────────────────────────────────────────────
    void set_kill_switch(bool on)
    {
        kill_switch_.store(on);
    }
    bool is_killed() const
    {
        return kill_switch_.load();
    }

    // ── 자정 리셋 (Engine 데이터 스레드가 장 시작 시 호출) ──────────────────
    void reset_daily();

    // ── 조회 ─────────────────────────────────────────────────────────────────
    int position(const std::string& ticker) const;
    double daily_pnl() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Config cfg_;
    std::atomic<bool> kill_switch_{false};

    mutable std::mutex positions_mtx_;
    std::unordered_map<std::string, int> positions_; // ticker → net qty (양수=롱, 음수=숏)

    mutable std::mutex pnl_mtx_;
    double daily_pnl_{0.0};

    mutable std::mutex rate_mtx_;
    std::deque<TimePoint> order_times_; // 최근 1분 내 주문 시각

    mutable std::mutex dedup_mtx_;
    std::unordered_map<std::string, TimePoint> last_signal_; // "strategy:ticker" → 마지막 신호 시각
};
