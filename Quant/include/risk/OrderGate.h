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
//  OrderRouter가 on_accept()·add_realized_pnl()로 내부 상태를 업데이트한다.
//
//  체크 항목:
//   1. Kill switch   — 강제 중단 플래그
//   2. NONE side     — 신호 없음, 즉시 거부
//   3. 포지션 수량   — 종목당 최대 보유 수량
//   4. 일일 손실     — 일일 최대 손실 초과 시 신규 매수 거부
//   5. Rate limit    — 분당 최대 주문수 초과 방지
//   6. 중복 신호     — 동일 strategy+ticker 1초 이내 중복 거부
//
//  뮤텍스 획득 규칙:
//   각 뮤텍스는 항상 독립 스코프에서만 획득 — 중첩 락 없음.
//   중첩이 필요할 경우 반드시 선언 순서(positions→pnl→rate→dedup)를 따를 것.
// ─────────────────────────────────────────────────────────────────────────────
class OrderGate
{
public:
    struct Config
    {
        int max_qty_per_ticker  = 100;          // 종목당 최대 보유 수량
        double daily_loss_limit = -300'000.0;   // 일일 최대 손실 (-30만원)
        int max_orders_per_min  = 20;           // 분당 최대 주문 (KIS 권장)
        int max_orders_per_sec  = 5;            // 초당 최대 주문 (KIS 안전 한도)
        double dedup_window_sec = 1.0;          // 중복 신호 제거 윈도우(초)
    };

    OrderGate() : cfg_()
    {
    }
    explicit OrderGate(Config cfg) : cfg_(cfg)
    {
    }

    // ── 주문 검증 (true = 통과, false = 거부) ──────────────────────────────
    bool check(const OrderSignal& sig, std::string& reject_reason);

    // ── 상태 업데이트 ───────────────────────────────────────────────────────
    // KIS 접수(ODNO 수신) 시 보수적으로 포지션을 선점. 실제 체결(FILLED)을
    // 확인하기 전까지 접수 수량을 포지션으로 간주해 과잉 주문을 차단한다.
    void on_accept(const std::string& ticker, OrderSide side, int qty, double price);
    void add_realized_pnl(double pnl);  // SELL 체결 시 실현 손익 추가 (테스트에서도 사용)

    // ── 체결 확인 시 원장 갱신 ─────────────────────────────────────────────
    // H0STCNI0 체결통보 수신 후 호출. avg_price 재계산 + 실현손익 적립.
    struct FillResult
    {
        double avg_price    = 0.0; // 갱신된 매수 평균단가
        int    net_qty      = 0;   // 체결 후 순 보유수량
        double commission   = 0.0; // 수수료 (0.015%)
        double tax          = 0.0; // 거래세 (매도 0.18%)
        double realized_pnl = 0.0; // 이번 체결 실현손익 (SELL만 양수)
    };
    FillResult on_fill_confirmed(const std::string& ticker, OrderSide side,
                                 int qty, double price);

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
    int    position(const std::string& ticker) const;
    double avg_price(const std::string& ticker) const;
    double daily_pnl() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Config cfg_;
    std::atomic<bool> kill_switch_{false};

    mutable std::mutex positions_mtx_;
    std::unordered_map<std::string, int>    positions_;  // ticker → net qty (양수=롱)
    std::unordered_map<std::string, double> avg_prices_; // ticker → 매수 평균단가

    mutable std::mutex pnl_mtx_;
    double daily_pnl_{0.0};

    mutable std::mutex rate_mtx_;
    std::deque<TimePoint> order_times_min_; // 최근 1분 내 주문 시각 (분당 제한)
    std::deque<TimePoint> order_times_sec_; // 최근 1초 내 주문 시각 (초당 제한)

    mutable std::mutex dedup_mtx_;
    std::unordered_map<std::string, TimePoint> last_signal_; // "strategy:ticker" → 마지막 신호 시각
};
