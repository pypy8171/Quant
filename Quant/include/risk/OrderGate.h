#pragma once
#include "core/Types.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderGate — 주문 전 위험 검증 게이트
//
//  Engine::order_thread_fn이 send_order 직전에 check()를 부르고, 통과한 신호만 실행한다.
//  OrderRouter가 on_accept()·add_realized_pnl()로 내부 상태를 갱신한다.
//  검사 항목과 그 실행 순서의 정본은 `OrderGate.cpp::check` 하나다 — 목록을 여기에 복사하지 않는다.
//
// [lock-order] check()는 positions_mtx_ 안에서 displace_mtx_·prio_mtx_를 잡는다(교체 후보·우선순위
//   판정이 보유 스냅샷과 같은 시점이어야 해서). 그러므로 순서는 positions → {displace, prio}이고,
//   displace·prio를 쥔 채 positions를 잡는 경로는 두지 않는다(plan_displacement는 비중첩).
//   pnl·rate·dedup은 독립 스코프에서만 획득한다.
// ─────────────────────────────────────────────────────────────────────────────
class OrderGate
{
public:
    struct Config
    {
        int max_qty_per_ticker  = 100;          // 종목당 최대 보유 수량(BUY 누적) — fat-finger 백스톱
        // ── 명목 사이징 백스톱 — 전략이 자본%로 사이징할 때의 상한/집중 제어(0=미적용) ──
        double max_notional_per_ticker  = 0.0;  // 종목당 최대 보유 명목(원). 지정가=price, 시장가=ref_price로 평가. 0=수량 한도만
        int    max_concurrent_positions = 0;    // 동시 보유 종목 상한(새 종목 여는 BUY NEW에만). 0=미적용
        // ── 점수 우선순위 바 — 슬롯이 찰수록 요구 랭크가 올라간다. false면 선착순(기존 동작) ──
        //  [formula] rank/total ≤ 1 − (open/slots) × decay(t). decay(t)는 장 마감까지 남은 시간
        //   비율(09:00=1.0 → 15:00=0.0)이라 오후로 갈수록 바가 내려간다. 빈 책이면 우변이 1.0이라
        //   전부 통과하고, 마지막 한 칸은 최상위만 가져간다. [why D-018]
        bool   entry_priority_enabled = false;
        // ── 교체 진입(displacement) — 슬롯이 꽉 찼는데 더 높은 점수가 오면 최약체를 비운다 ──
        //  교체는 공짜가 아니다. 왕복 비용 0.195%(수수료 0.03% + 세금 0.18% 근사)에 피교체 종목의
        //  분할 매수가 리셋되므로, 아래 넷으로 회전을 묶는다. [why D-019]
        bool   displace_enabled       = false;
        double displace_min_z_gap     = 0.5;  // σ, 신규가 최약체보다 이만큼 높아야 교체
        int    displace_min_hold_sec  = 900;  // 초, 방금 산 종목은 안 뺀다
        int    displace_cooldown_sec  = 1800; // 초, 밀려난 종목의 재진입 금지 시간
        int    displace_max_per_day   = 5;    // 하루 교체 횟수 상한
        int    displace_slot_hold_sec = 300;  // 초, 비운 슬롯의 예약 유지 시간. 3분봉 한 개 + 체결 지연
        // 오늘 어느 슬리브의 스캔에도 안 잡힌 보유분에 매길 z. 스캔에서 빠졌다는 건 오늘 필터를
        //  통과하지 못했다는 뜻이라, 통과한 어떤 종목보다 아래로 둔다(z 클립 하한 -2.0보다 낮게).
        //  이 값이 없으면 그런 보유분은 "점수 미상"이라 교체 후보에서 제외되고, 전일 이월분이
        //  슬롯을 영구히 점유한다. 0으로 두면 기존 동작(제외)으로 돌아간다.
        double displace_unscored_z    = 0.0;
        // ── 포트폴리오 총노출 상한 — 모든 종목 보유·예약 명목 합이 자본의 이 비율을 넘으면 신규 매수 차단(0=미적용).
        //    종목당 상한(15%)×동시보유(10)=150% 같은 과노출을 총합 단에서 막는다(청산은 통과).
        double max_gross_exposure_pct   = 0.0;  // 예: 0.95 = 자본의 95%. equity_ 미주입(0)이면 자동 비활성
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

    // 뮤텍스·원자값·유량 창을 안고 있고 Engine이 한 개를 소유한다 — 복사 대상이 아니다.
    OrderGate(const OrderGate&)            = delete;
    OrderGate& operator=(const OrderGate&) = delete;

    // 위험 한도 주입 — 반드시 order_thread 시작 전에만 호출(cfg_는 check()에서 락 없이 읽힘).
    void set_config(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    // ── 주문 검증 (true = 통과, false = 거부) ──────────────────────────────
    bool check(const OrderSignal& sig, std::string& reject_reason);

    // ── 한도 클램프 (BUY NEW 전용) ─────────────────────────────────────────
    // 한도를 넘는 수량을 거부하는 대신 한도 안으로 줄여 돌려준다. 분할 매수 전략은 매 틱
    // 같은 rung을 다시 내므로, 넘친다고 버리면 그 종목은 영원히 발주되지 않고 초당 주문
    // 예산만 태운다. 줄여서라도 나가는 편이 의도(부분 진입)에 가깝다.
    // 검사 대상은 수량·명목·포지션·총노출 한도뿐이다. 킬스위치·entry_halt·손실컷 같은
    // "발주 자체를 막는" 게이트는 여기서 손대지 않는다 — 그건 check()가 그대로 거부한다.
    // 반환 0 = 여유 없음(발주 불가). 정정·취소는 원본 수량을 그대로 돌려준다.
    // SELL NEW는 매도가능수량(보유 - 미체결매도)으로 깎는다. 자기 익절 지정가가 자기
    // 청산을 막아 KIS가 40240000으로 주문을 통째로 거부하면 한 주도 못 빠져나온다.
    // 원장이 그 종목을 0으로 알고 있으면 손대지 않는다(과소 인식 방어).
    int clamp_buy_qty(const OrderSignal& sig);

    // ── 상태 업데이트 ───────────────────────────────────────────────────────
    // KIS 접수(주문번호 ODNO 수신) 시 reserved_에 선점만 기록(실체결 원장 positions_는 불변).
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
    // C-1: rest_price_feed 모드는 체결 콜백이 없어 daily_pnl_이 0에 고정되고, 그러면 §4의
    //  BUY 전용 손실컷이 동작하지 못한다.
    //  Engine이 잔고 재조회로 당일 기준선 대비 평가금 델타를 계산해 이 값으로 직접 덮어쓴다.
    //  (add_realized_pnl은 누적, 이건 절대치 세팅 — 잔고 대조 전용)
    void set_daily_pnl(double pnl)
    {
        std::lock_guard<std::mutex> lk(pnl_mtx_);
        daily_pnl_ = pnl;
    }

    // ── 총노출 게이트용 자본 주입 (§3d) ──────────────────────────────────────
    // 잔고 대조 스레드가 총평가금(tot_evlu_amt) 갱신 시 호출. check()가 락 없이 읽도록 atomic.
    // 0이면 §3d 게이트 비활성(자본 미상 시 폴백 안전 — 종목당·동시보유 백스톱이 커버).
    void set_equity(double equity) { equity_.store(equity, std::memory_order_relaxed); }
    double equity() const { return equity_.load(std::memory_order_relaxed); }

    // 주문가능현금(원). 잔고 대조가 output2에서 읽어 넣는다. 0=미주입(클램프 비활성).
    //  총평가금(equity_)과 다르다 — 평가금이 1억이어도 미체결 지정가와 미결제 매수가
    //  현금을 묶으면 살 수 없다. 이 값이 없으면 게이트가 그걸 모른 채 계속 발주하고
    //  KIS가 40250000으로 전량 거부한다(2026-09-08 59건).
    void set_available_cash(double v) { available_cash_.store(v, std::memory_order_relaxed); }
    double available_cash() const { return available_cash_.load(std::memory_order_relaxed); }

    // ── 원장 부트스트랩 (G5) — 기동 시 실계좌 보유분을 원장에 시드 ─────────────
    // 체결이 아니므로 reserved_/daily_pnl_은 불변, positions_/avg_prices_만 설정.
    // on_fill_confirmed 재사용 금지(수수료·실현손익 오적립) → 전용 API.
    // 계좌키는 신호가 쓰는 account_id와 반드시 동일해야 조회된다(단일계좌는 account="").
    // sellable < 0 이면 "모름"으로 보고 보유수량을 그대로 쓴다.
    void seed_position(const std::string& account, const std::string& ticker, int qty, double avg_price,
                       int sellable);
    void seed_position(const std::string& account, const std::string& ticker, int qty, double avg_price)
    {
        seed_position(account, ticker, qty, avg_price, -1);
    }

    void seed_position(const std::string& ticker, int qty, double avg_price)
    {
        seed_position(std::string(), ticker, qty, avg_price, -1);
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
        // SELL인데 원장이 평단을 모를 때 true. 그 경우 realized_pnl은 0으로 두고 daily_pnl에도
        //  더하지 않는다 — (price-0)*qty가 이익으로 잡히면 일일 손실컷이 무력화된다(C-1).
        bool   basis_unknown = false;
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
    // kill_switch_(전방향 하드스톱)와 분리된 "BUY-only 정지" 플래그. 지수 급락·일일손실 등
    // 국면 리스크로 신규 진입만 막고 보유분 청산(SELL)은 통과시킨다 — 급락장에서 청산이 미완료로 남지 않게(C-2).
    void set_entry_halt(bool on)
    {
        entry_halt_.store(on);
    }

    bool is_entry_halted() const
    {
        return entry_halt_.load();
    }

    // 유니버스 스캔이 낸 종합 점수 랭크(1=최고)를 주입한다. 재스캔이 매번 덮어쓴다.
    //  total은 랭크의 모집단 크기(등록 종목 수). 비어 있으면 우선순위 바는 동작하지 않는다.
    //  z는 같은 점수의 표준화값 — 랭크는 "몇 번째"만 알려주고 "얼마나 더 좋은지"는 못 알려준다.
    //  교체는 격차가 잡음보다 큰지를 봐야 하므로 z가 따로 필요하다.
    void set_entry_priority(std::unordered_map<std::string, int> rank,
                            std::unordered_map<std::string, double> z, int total)
    {
        std::lock_guard<std::mutex> lk(prio_mtx_);
        entry_rank_  = std::move(rank);
        entry_z_     = std::move(z);
        entry_total_ = total;
    }

    // ── 교체 진입 ────────────────────────────────────────────────────────────
    //  슬롯이 꽉 찬 상태에서 new_ticker가 들어오려 할 때, 비워 줄 최약체를 고른다.
    //  고르기만 하고 주문은 내지 않는다 — 발주는 order_queue_ 단일 생산자인 전략 스레드 몫이다.
    struct DisplacePlan
    {
        bool        ok = false;
        std::string account;      // 비울 종목의 계좌
        std::string ticker;       // 비울 종목
        int         qty = 0;      // 매도할 수량(미체결 매도 제외)
        double      avg_price = 0.0;
        double      victim_z = 0.0;
        double      new_z = 0.0;
        std::string reason;       // 로그·원장에 남길 사유
    };
    DisplacePlan plan_displacement(const std::string& account, const std::string& new_ticker) const;
    // 교체를 실제로 발주했을 때 호출 — 쿨다운·횟수·슬롯 예약을 기록한다.
    void note_displacement(const DisplacePlan& plan, const std::string& beneficiary);
    // 동시 보유 슬롯이 꽉 찼는가(신규 종목을 열 자리가 없는가).
    bool slots_full() const;
    // 신규 종목을 열 여력이 없는가 — 자리(슬롯)와 예산(총노출) 중 하나만 막혀도 없다.
    bool capacity_full() const;

    // ── PnL stale guard (B2) — 잔고 대조 정체 시 신규 매수 정지 ──────────────
    // rest_price_feed 모드는 daily_pnl_을 잔고 대조(총평가금 델타)로만 갱신한다. 잔고조회가
    // 연속 실패(12002 타임아웃 등)해 서킷브레이커가 잔고 대조를 스킵하는 동안 daily_pnl_은 낡은
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

    // ── 선점(reserved_) 전면 초기화 — REST 잔고 대조 전용 ────────────────────
    // 체결피드(H0STCNI0)가 없는 rest_price_feed 모드는 on_fill_confirmed가 호출되지 않아
    // reserved_(미체결 선점)가 영구 누적된다(H-1 드리프트) → check()가 positions_+reserved_로
    // 한도를 봐 정상 신호까지 과잉 차단. 잔고 대조는 서버 확정 스냅샷이므로, 재동기 시점에
    // reserved_를 통째로 비우고 실보유(positions_)만 신뢰한다. 잔고조회 성공 사이클에만 호출.
    void reset_reserved();

    // ── 유령 슬롯 정리 ───────────────────────────────────────────────────────
    // 원장에 남았는데 실제로는 없는 종목이 슬롯을 물면 "보유 20인데 한도 25 초과"가 난다
    //  (09-09 관측). 정본은 둘로 갈린다 — 실보유는 브로커 잔고, 선점은 라우터 미체결 이력.
    //  live_tickers = 그 정본이 살아 있다고 답한 종목. 여기 없는 항목만 걷어낸다.
    //  min_age_sec 안에 열린 포지션은 잔고 스냅샷이 방금 체결을 아직 못 봤을 수 있어 남긴다.
    //  반환값은 걷어낸 종목 코드(호출부가 로그로 남긴다).
    std::vector<std::string> prune_positions(const std::vector<std::string>& live_tickers,
                                             int min_age_sec);

    // 선점은 접수 때만 생기므로 라우터 이력이 정본이다. 살아있는 주문이 없는 선점을 푼다.
    std::vector<std::string> prune_reservations(const std::vector<std::string>& live_tickers);

    // 미체결 매도를 브로커에서 취소한 뒤 매도가능수량을 되돌린다. 취소는 KIS에서 수량을 푸는데
    //  게이트의 sellable_은 잔고 시드값(ord_psbl_qty) 그대로 남아, 미체결이 없는데도 자기 청산이
    //  막힌다(09-09 000215: 13:45 취소 후 16분간 "매도가능수량 0"으로 교체 진입 4회 무산).
    //  보유수량을 넘지 않게 자른다. 원장이 모르는 종목이면 아무 것도 하지 않는다.
    void restore_sellable(const std::string& account, const std::string& ticker, int qty);

    // 매도가능수량이 0으로 잘린 이유를 로그에 남길 조각. 원장 보유·잔고 주문가능(시드/대조값)·
    //  이 세션 미체결 매도(선점). 원장이 모르는 종목이면 held=0이고 나머지도 0이다.
    struct SellableView
    {
        int held     = 0;
        int psbl_cap = 0;
        int pending  = 0;
    };

    SellableView sellable_view(const std::string& account, const std::string& ticker) const;

    // 잔고 대조마다 KIS ord_psbl_qty로 매도가능수량을 다시 맞춘다. 기동 시드는 유령주문 취소가
    //  끝나기 전에 읽혀 0으로 박힐 수 있고(09-11 10:18 12종목), 체결통보 모드는 잔고 재동기를
    //  건너뛰어 그 0이 하루 종일 남아 익절·청산이 "매도가능수량 0"으로 막혔다(000720).
    //  KIS 값은 이 세션의 미체결 매도까지 뺀 수라 되더해 둔다 — clamp가 그만큼 다시 빼기 때문이다.
    void refresh_sellable(const std::string& account, const std::string& ticker, int ord_psbl_qty);
    // 체결통보(WS) 모드 잔고 대조에서 원장 수량 > 잔고 수량인 종목을 놓친 매도 체결로 보고 맞춘다.
    //  조건: 차이가 미체결 매도(reserved_<0) 이내이고, 같은 잔고 수량이 두 번 연속 관측될 때만
    //  (잔고 왕복이 통보보다 빠른 순간의 경합 회피). 맞춘 수량을 돌려주고 아니면 0.
    //  09-11 11:00 재연결 사이에 248170 매도 52주 통보가 빠져 18분간 유령 52주가 슬롯을 물었다.
    int absorb_missed_sell(const std::string& account, const std::string& ticker, int balance_qty);

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
    //  data_thread가 아닌 strategy_thread(order_queue_ 단일 생산자)가 force_liquidate 시
    //  이 목록으로 전량 시장가 매도를 발주한다.
    struct HeldPos { std::string account; std::string ticker; int qty; double avg_price; };
    std::vector<HeldPos> snapshot_positions() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 원장 파티션 키 — 계좌별 독립. 두 필드를 따로 들어 "A"+"B:C"와 "A:B"+"C"가 섞이지 않고(W-1),
    //  키에서 (account,ticker)를 되찾는 파싱이 없다. 문자열 하나로 합치던 때는 역파싱이 정리·교체·
    //  스냅샷 세 곳에 복제돼 있었고 키 하나 만들 때마다 힙 할당이 났다. [why D-057]
    struct PosKey
    {
        std::string account;
        std::string ticker;

        bool operator==(const PosKey& o) const
        {
            return account == o.account && ticker == o.ticker;
        }
    };

    struct PosKeyHash
    {
        size_t operator()(const PosKey& k) const
        {
            // boost::hash_combine 모양. 두 필드를 xor만 하면 (a,b)와 (b,a)가 같은 버킷에 간다.
            const size_t h1 = std::hash<std::string>{}(k.account);
            const size_t h2 = std::hash<std::string>{}(k.ticker);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
        }
    };

    template <class V>
    using PosMap = std::unordered_map<PosKey, V, PosKeyHash>;

    static PosKey make_key(const std::string& account, const std::string& ticker)
    {
        return PosKey{account, ticker};
    }

    // 선점 해제의 유일한 경로 — 취소 통보(on_cancel)와 체결 통보(on_fill_confirmed)가 함께 쓴다.
    //  없는 선점은 손대지 않고, 과잉 해제는 0에서 멈춘다. 규칙이 두 곳에 갈라져 있으면 한쪽만
    //  고쳐지므로 여기 하나만 둔다. 호출 전에 positions_mtx_를 잡아야 한다(내부에서 잡지 않음).
    void release_reservation(const PosKey& key, int delta);

    Config cfg_;
    std::atomic<bool> kill_switch_{false};
    std::atomic<bool> entry_halt_{false};  // 신규 진입(BUY NEW)만 정지, SELL 청산은 통과 — 국면 리스크용
    std::atomic<bool> pnl_stale_{false};   // 잔고 대조 정체 → daily_pnl 미갱신, BUY NEW 보수 정지(B2)
    std::atomic<double> available_cash_{0.0}; // 주문가능현금 스냅샷. 잔고 대조가 갱신, clamp_buy_qty가 락 없이 읽음
    std::atomic<double> equity_{0.0};      // 총평가금 스냅샷(§3d 총노출 게이트 분모). 잔고 대조가 갱신, check()가 락 없이 읽음

    mutable std::mutex prio_mtx_;
    std::unordered_map<std::string, int> entry_rank_; // ticker → 종합점수 랭크(1=최고)
    int entry_total_ = 0;                             // 랭크 모집단 크기(등록 종목 수)
    std::unordered_map<std::string, double> entry_z_; // ticker → 종합점수 z (교체 격차 판정용)

    mutable std::mutex displace_mtx_;
    std::unordered_map<std::string, TimePoint> displace_cooldown_; // 밀려난 종목 → 재진입 허용 시각
    std::string slot_reserved_for_;      // 비운 슬롯을 쓸 종목(다른 종목이 가로채지 못하게)
    TimePoint   slot_reserved_until_{};  // 예약 만료 시각
    mutable std::unordered_map<std::string, std::string> displace_decline_; // 신규 종목 → 직전 교체 거절 사유(거부 문구용)
    int         displace_count_ = 0;     // 당일 교체 횟수(reset_daily에서 0으로)

    mutable std::mutex positions_mtx_;
    PosMap<int>    reserved_;    // (account,ticker) → 미체결 선점 수량 (BUY +, SELL -). 재주문 차단용
    PosMap<double> reserved_px_; // (account,ticker) → 미체결 선점가(§3d 총노출 계산용). reserved_와 동일 생명주기로 정리
    PosMap<int>    positions_;   // (account,ticker) → 실체결 순보유 수량 (양수=롱)
    PosMap<double> avg_prices_;
    // account:ticker -> 매도가능수량. 보유수량과 다르다: 기동 전 세션이 남긴 미체결 매도,
    //  미결제분 때문에 KIS가 실제로 받아주는 매도 수량은 보유보다 적을 수 있다. 이걸 모르면
    //  전량 청산이 40240000(주문가능분 없음)으로 통째 거부돼 한 주도 못 빠져나온다.
    //  기동 시드에서 잔고의 ord_psbl_qty로 채우고, 이후 체결로 증감시킨다.
    PosMap<int>       sellable_;         // (account,ticker) → 매도가능수량(주)
    PosMap<int>       missed_sell_seen_; // (account,ticker) → 직전 대조에서 본 잔고 수량(2회 연속 확인용)
    PosMap<TimePoint> opened_at_;        // (account,ticker) → 포지션이 0에서 열린 시각(교체 최소 보유 판정)

    mutable std::mutex pnl_mtx_;
    double daily_pnl_{0.0};

    mutable std::mutex rate_mtx_;
    std::deque<TimePoint> order_times_min_; // 최근 1분 내 주문 시각 (분당 제한)
    std::deque<TimePoint> order_times_sec_; // 최근 1초 내 주문 시각 (초당 제한)

    mutable std::mutex dedup_mtx_;
    std::unordered_map<std::string, TimePoint> last_signal_; // "strategy:ticker" → 마지막 신호 시각
};
