#pragma once
#include "core/Types.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderGate  —  주문 전 위험 검증 게이트
//
//  Engine::order_thread_fn이 send_order 직전에 check()를 통과한 신호만 실행.
//  OrderRouter가 on_accept()·add_realized_pnl()로 내부 상태를 업데이트한다.
//
//  체크 항목:
//   1. Kill switch   — 강제 중단 플래그(전방향 차단: BUY·SELL 모두)
//   1b. Entry halt   — 신규 진입 정지(BUY NEW만 차단, SELL 청산은 통과). 지수 급락 킬스위치용.
//   2. NONE side     — 신호 없음, 즉시 거부
//   3. 포지션 수량   — 종목당 최대 보유 수량
//   4. 일일 손실     — 일일 최대 손실 초과 시 신규 매수 거부
//   4b. PnL stale    — 잔고 리컨사일 정체로 daily_pnl 미갱신 시 신규 매수 보수적 정지(B2)
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
        int max_qty_per_ticker  = 100;          // 종목당 최대 보유 수량(BUY 누적) — fat-finger 백스톱
        // ── 명목 사이징 백스톱 — 전략이 자본%로 사이징할 때의 상한/집중 제어(0=미적용) ──
        double max_notional_per_ticker  = 0.0;  // 종목당 최대 보유 명목(원). limit가로 평가. 0=수량 한도만
        int    max_concurrent_positions = 0;    // 동시 보유 종목 상한(새 종목 여는 BUY NEW에만). 0=미적용
        double daily_loss_limit = -300'000.0;   // 일일 최대 손실 (-30만원)
        int max_orders_per_min  = 20;           // 분당 최대 주문 (KIS 권장)
        int max_orders_per_sec  = 5;            // 초당 최대 주문 (KIS 안전 한도)
        double dedup_window_sec = 1.0;          // 중복 신호 제거 윈도우(초)
        // ── 1주문 fat-finger 백스톱 (C-3) — NEW BUY/SELL 공통. 시장가 대량주문 슬리피지 방어.
        //    보유 전량 매도 등 정상 주문은 통과할 만큼 넉넉하게, 비정상 대량만 차단.
        int max_qty_per_order        = 10'000;         // 1주문 최대 수량
        double max_notional_per_order = 50'000'000.0;  // 1주문 최대 명목(원). price>0일 때만 검사
    };

    OrderGate() : cfg_()
    {
    }
    explicit OrderGate(Config cfg) : cfg_(cfg)
    {
    }

    // 위험 한도 주입 — 반드시 order_thread 시작 전에만 호출(cfg_는 check()에서 락 없이 읽힘).
    void set_config(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    // ── 주문 검증 (true = 통과, false = 거부) ──────────────────────────────
    bool check(const OrderSignal& sig, std::string& reject_reason);

    // ── 상태 업데이트 ───────────────────────────────────────────────────────
    // KIS 접수(ODNO 수신) 시 reserved_에 선점만 기록(실체결 원장 positions_는 불변).
    // check()는 positions_ + reserved_ 합산으로 한도를 보므로 미체결 주문이 과잉 주문을 차단한다.
    // 체결(on_fill_confirmed) 시 reserved_가 해제되고 positions_/avg_price가 갱신된다.
    // 원장은 (account_id:ticker)로 파티셔닝 — 계좌별 독립. 아래 4-arg 오버로드는 account="" 하위호환.
    void on_accept(const std::string& account, const std::string& ticker,
                   OrderSide side, int qty, double price);
    void on_accept(const std::string& ticker, OrderSide side, int qty, double price)
    {
        on_accept(std::string(), ticker, side, qty, price);
    }
    void add_realized_pnl(double pnl);  // SELL 체결 시 실현 손익 추가 (테스트에서도 사용)
    // C-1: rest_price_feed 모드는 체결콜백이 없어 daily_pnl_이 0 고정 → BUY-only 손실컷(§4) 死.
    //  Engine이 잔고 재조회로 당일 기준선 대비 평가금 델타를 계산해 이 값으로 직접 덮어쓴다.
    //  (add_realized_pnl은 누적, 이건 절대치 세팅 — 리컨사일 전용)
    void set_daily_pnl(double pnl)
    {
        std::lock_guard<std::mutex> lk(pnl_mtx_);
        daily_pnl_ = pnl;
    }

    // ── 원장 부트스트랩 (G5) — 기동 시 실계좌 보유분을 원장에 시드 ─────────────
    // 체결이 아니므로 reserved_/daily_pnl_은 불변, positions_/avg_prices_만 설정.
    // on_fill_confirmed 재사용 금지(수수료·실현손익 오적립) → 전용 API.
    // 계좌키는 신호가 쓰는 account_id와 반드시 동일해야 조회된다(단일계좌는 account="").
    void seed_position(const std::string& account, const std::string& ticker, int qty, double avg_price);
    void seed_position(const std::string& ticker, int qty, double avg_price)
    {
        seed_position(std::string(), ticker, qty, avg_price);
    }

    // ── 미체결 취소/정정 축소 시 선점 해제 (C5, MM-1) ─────────────────────
    // qty = 취소된 미체결 잔량(>0). reserved_만 감소 — positions_/avg_price는 불변(취소는 체결 아님).
    // 방향은 on_fill_confirmed의 선점 해제와 동일: BUY 선점(+)은 -qty, SELL 선점(-)은 +qty.
    // 호출 규약: 반드시 KIS 취소 성공(rt_cd=="0") 이후에만 호출 — 실패 시 호출하면 이중해제.
    void on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int qty);
    void on_cancel(const std::string& ticker, OrderSide side, int qty)
    {
        on_cancel(std::string(), ticker, side, qty);
    }

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
    FillResult on_fill_confirmed(const std::string& account, const std::string& ticker,
                                 OrderSide side, int qty, double price);
    FillResult on_fill_confirmed(const std::string& ticker, OrderSide side,
                                 int qty, double price)
    {
        return on_fill_confirmed(std::string(), ticker, side, qty, price);
    }

    // ── Kill switch ─────────────────────────────────────────────────────────
    void set_kill_switch(bool on)
    {
        kill_switch_.store(on);
    }
    bool is_killed() const
    {
        return kill_switch_.load();
    }

    // ── Entry halt (신규 진입 정지) ────────────────────────────────────────────
    // kill_switch_와 분리된 "BUY-only 정지" 플래그. 지수 급락·일일손실 등 국면 리스크로
    // 신규 진입만 막되 보유분 청산(SELL)은 반드시 통과시켜야 하는 상황에 쓴다.
    // kill_switch_(전방향 하드스톱)와 달리 SELL은 게이트를 통과 → 급락장 청산 좌초 방지(C-2).
    void set_entry_halt(bool on)
    {
        entry_halt_.store(on);
    }
    bool is_entry_halted() const
    {
        return entry_halt_.load();
    }

    // ── PnL stale guard (B2) — 잔고 리컨사일 정체 시 신규 매수 정지 ──────────────
    // rest_price_feed 모드는 daily_pnl_을 잔고 리컨사일(총평가금 델타)로만 갱신한다. 잔고조회가
    // 연속 실패(12002 타임아웃 등)해 서킷브레이커가 리컨사일을 스킵하는 동안 daily_pnl_은 낡은
    // 값이라, 그 창에서 손실이 나도 §4 손실컷이 트립하지 못한다. Engine이 실패 스트릭이 임계를
    // 넘으면 이 플래그를 세워 BUY NEW만 보수적으로 차단(SELL 청산·취소는 통과 — entry_halt와 동일
    // 의미론). 잔고조회 복구 시 자동 해제. 손실컷을 대체하지 않고 "믿을 수 없는 창"만 보수 처리.
    void set_pnl_stale(bool on)
    {
        pnl_stale_.store(on);
    }
    bool is_pnl_stale() const
    {
        return pnl_stale_.load();
    }

    // ── 자정 리셋 (Engine 데이터 스레드가 장 시작 시 호출) ──────────────────
    void reset_daily();

    // ── 선점(reserved_) 전면 초기화 — REST 리컨사일 전용 ────────────────────
    // 체결피드(H0STCNI0)가 없는 rest_price_feed 모드는 on_fill_confirmed가 호출되지 않아
    // reserved_(미체결 선점)가 영구 누적된다(H-1 드리프트) → check()가 positions_+reserved_로
    // 한도를 봐 정상 신호까지 과잉 차단. 잔고 리컨사일은 서버 확정 스냅샷이므로, 재동기 시점에
    // reserved_를 통째로 비우고 실보유(positions_)만 신뢰한다. 잔고조회 성공 사이클에만 호출.
    void reset_reserved();

    // ── 조회 ─────────────────────────────────────────────────────────────────
    // 계좌 지정 버전(주 경로) + account="" 하위호환(단일 계좌).
    int    position(const std::string& account, const std::string& ticker) const;
    int    reserved(const std::string& account, const std::string& ticker) const;
    double avg_price(const std::string& account, const std::string& ticker) const;
    int    position(const std::string& ticker) const { return position(std::string(), ticker); }
    int    reserved(const std::string& ticker) const { return reserved(std::string(), ticker); }
    double avg_price(const std::string& ticker) const { return avg_price(std::string(), ticker); }
    double daily_pnl() const;

    // ── 보유 포지션 스냅샷 (G3 강제청산) — net>0 실보유분만 락 하 복사 반환 ──────
    //  합성키(make_key = "<len>:<account><ticker>")를 역파싱해 (account,ticker)를 복원한다.
    //  data_thread가 아닌 strategy_thread(order_queue_ 단일 생산자)가 force_liquidate 시
    //  이 목록으로 전량 시장가 매도를 발주한다.
    struct HeldPos { std::string account; std::string ticker; int qty; double avg_price; };
    std::vector<HeldPos> snapshot_positions() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 원장 파티션 키 — 계좌별 독립. account_id는 외부(법인/DMA) 입력이 될 수 있어
    // 단순 구분자('account:ticker')는 "A"+"B:C"와 "A:B"+"C"가 충돌한다(US 영문 티커·
    // 외부 계좌ID에 ':' 가능). account 길이를 접두해 모호성을 제거한다(주입 안전).
    static std::string make_key(const std::string& account, const std::string& ticker)
    {
        return std::to_string(account.size()) + ":" + account + ticker;
    }

    Config cfg_;
    std::atomic<bool> kill_switch_{false};
    std::atomic<bool> entry_halt_{false};  // 신규 진입(BUY NEW)만 정지, SELL 청산은 통과 — 국면 리스크용
    std::atomic<bool> pnl_stale_{false};   // 잔고 리컨사일 정체 → daily_pnl 미갱신, BUY NEW 보수 정지(B2)

    mutable std::mutex positions_mtx_;
    std::unordered_map<std::string, int>    reserved_;   // account:ticker → 미체결 선점 수량 (BUY +, SELL -). 재주문 차단용
    std::unordered_map<std::string, int>    positions_;  // account:ticker → 실체결 순보유 수량 (양수=롱)
    std::unordered_map<std::string, double> avg_prices_; // account:ticker → 매수 평균단가 (실체결 기준)

    mutable std::mutex pnl_mtx_;
    double daily_pnl_{0.0};

    mutable std::mutex rate_mtx_;
    std::deque<TimePoint> order_times_min_; // 최근 1분 내 주문 시각 (분당 제한)
    std::deque<TimePoint> order_times_sec_; // 최근 1초 내 주문 시각 (초당 제한)

    mutable std::mutex dedup_mtx_;
    std::unordered_map<std::string, TimePoint> last_signal_; // "strategy:ticker" → 마지막 신호 시각
};
