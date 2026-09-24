#pragma once
#include "core/StrategyTable.h"
#include "core/SymbolTable.h"
#include "core/Types.h"
#include "risk/EntryPriority.h"
#include "risk/LedgerKeys.h"
#include "risk/PositionLedger.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderGate — 주문 전 위험 검증 게이트
//
//  주문 스레드의 OrderRouter::new_route(Quant/src/ipc/OrderRouter.cpp)가 check()를 부르고, 라우터가
//  ledger()의 on_intent/on_accepted/on_reject/on_cancel/on_fill_confirmed로 장부를 갱신한다. 장부(보유·선점·
//  평단·손익·저널)는 Quant/include/risk/PositionLedger.h이고, 이 클래스는 한도·교체 판정과 유량·중복 제한을 든다.
//  검사 항목과 그 실행 순서의 정본은 `OrderGate.cpp::check` 하나다 — 목록을 여기에 복사하지 않는다.
//
// [lock-order] check()는 원장 Reader(PositionLedger의 positions_mutex_)를 쥔 채 EntryPriority의 displace_mutex_·
//   priority_mutex_를 잡는다(교체 후보·우선순위 판정이 보유 스냅샷과 같은 시점이어야 해서). 그러므로 순서는
//   positions → {displace, priority}이고, displace·priority를 쥔 채 positions를 잡는 경로는 두지 않는다
//   (plan_displacement는 비중첩). 원장 안의 순서(positions → journal, publish → positions)는
//   Quant/include/risk/PositionLedger.h. pnl·rate·dedup은 독립 스코프에서만 획득한다.
// ─────────────────────────────────────────────────────────────────────────────
namespace ipc
{
class LedgerSnapshot; // 장부 사본. 구현(.cpp)에서만 include한다 — 배선은 OrderGate → ipc 한 방향 [why D-114]
}

// 게이트가 주문을 막은 까닭. 판정(OrderGate::evaluate)은 이 코드와 숫자만 돌려주고, 문장은
//  OrderGate::describe가 원장 잠금 밖에서 만든다. [why CODE_REVIEW W-8]
enum class GateReject : uint8_t
{
    None,                // 통과
    KillSwitch,
    SideNone,
    EntryHalt,
    OutsideSession,      // amount = 지금 KST 분
    BadQuantity,         // amount = 주문 수량
    OrderQuantityLimit,  // amount = 주문 수량
    OrderNotionalLimit,  // amount = 주문 명목(원), market_reference
    TickerQuantityLimit, // amount = 주문 수량, base = 이미 가진 수량(보유 + 선점)
    TickerNotionalLimit, // amount = 가진 수량까지 더한 명목(원)
    ConcurrentLimit,     // amount = 열린 종목 수, base = 그중 실보유, symbol = 막힌 종목
    DisplaceCooling,
    SlotReserved,        // symbol = 비운 슬롯을 예약받은 종목
    PriorityBar,         // amount = 열린 종목 수, base = 유효 랭크, ceiling = 모집단, rank·ratio·bar
    GrossExposure,       // amount = 이 주문 뒤 총노출(원), ceiling = 상한(원), money = 자본(원)
    DailyLoss,           // money = 당일 손익(원)
    PnlStale,
    Duplicate,
    RatePerSecond,
    RatePerMinute,
};

// 게이트 판정 한 건. 칸의 뜻은 코드마다 다르다(GateReject 옆 주석). 한도 값은 싣지 않는다 —
//  describe가 게이트 설정에서 읽는다.
struct GateVerdict
{
    GateReject       code               = GateReject::None;
    bool             market_reference   = false; // 시장가라 참조가로 명목을 쟀다
    bool             sell_over_notional = false; // 통과했지만 1주문 명목을 넘은 청산 — check()가 경고 한 줄을 남긴다
    int32_t          rank               = 0;
    int64_t          amount             = 0;
    int64_t          base               = 0;
    int64_t          ceiling            = 0;
    double           ratio              = 0.0;
    double           bar                = 0.0;
    double           money              = 0.0;
    symbol::SymbolId symbol             = symbol::kNone;

    [[nodiscard]] bool passed() const noexcept { return code == GateReject::None; }
};

class OrderGate
{
public:
    struct Config
    {
        int max_quantity_per_ticker  = 100;          // 종목당 최대 보유 수량(BUY 누적) — fat-finger 백스톱
        // ── 명목 사이징 백스톱 — 전략이 자본%로 사이징할 때의 상한/집중 제어(0=미적용) ──
        double max_notional_per_ticker  = 0.0;  // 종목당 최대 보유 명목(원). 지정가=price, 시장가=ref_price로 평가. 0=수량 한도만
        int    max_concurrent_positions = 0;    // 동시 보유 종목 상한(새 종목 여는 BUY NEW에만). 0=미적용
        // ── 점수 우선순위 바 — 슬롯이 찰수록 요구 랭크가 올라간다. false면 선착순(기존 동작) ──
        //  [formula] eff_rank/pool ≤ 1 − (open/max_concurrent_positions) × session_remaining_ratio().
        //   eff_rank = below_by_symbol − taken_ahead + 1(나보다 위인데 아직 안 잡힌 수 + 1),
        //   pool = max(total, max_concurrent_positions). session_remaining_ratio()는 장 마감까지 남은 시간
        //   비율(09:00=1.0 → 15:00=0.0)이라 오후로 갈수록 바가 내려간다. 빈 책이면 우변이 1.0이라
        //   전부 통과하고, 마지막 한 칸은 최상위만 가져간다. [why D-018]
        bool   entry_priority_enabled = false;
        // ── 교체 진입(displacement) — 슬롯이 꽉 찼는데 더 높은 점수가 오면 최약체를 비운다 ──
        //  교체는 공짜가 아니다. 왕복 비용 0.23%(수수료 0.03% + 세금 0.20%)에 피교체 종목의
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
        double max_gross_exposure_percent   = 0.0;  // 예: 0.95 = 자본의 95%. equity_ 미주입(0)이면 자동 비활성
        double daily_loss_limit = -300'000.0;   // 일일 최대 손실 (-30만원)
        int max_orders_per_min  = 20;           // 분당 최대 주문 (KIS 권장)
        int max_orders_per_sec  = 5;            // 초당 최대 주문 (KIS 안전 한도)
        double deduplicate_window_sec = 1.0;          // 중복 신호 제거 윈도우(초)
        // ── 1주문 fat-finger 백스톱 (C-3) — NEW BUY/SELL 공통. 시장가 대량주문 슬리피지 방어.
        //    보유 전량 매도 등 정상 주문은 통과할 만큼 넉넉하게, 비정상 대량만 차단.
        int max_quantity_per_order        = 10'000;         // 1주문 최대 수량
        double max_notional_per_order = 50'000'000.0;  // 1주문 최대 명목(원). price>0일 때만 검사
        // ── 매매 세션 창(KST, 자정부터의 분) — 이 밖의 NEW 주문은 막는다. KRX+NXT 통합 피드는 08:00~20:00
        //    틱을 주지만 매매는 정규장 09:00~15:30뿐이라, 전략이 그 밖에서 낸 신호를 여기서 잡는다.
        //    CANCEL/REPLACE는 통과(미체결 정리). 둘 다 0이면 검사 없음 — 테스트·리플레이 기본. [why D-096]
        //    두 번째 창은 KRX 애프터마켓(16:00~20:00, 2026-09-14 개장) — 정규장 창과 합집합으로 본다.
        //    15:30~16:00(장후 종가 거래)은 두 창 사이라 막힌다. 0/0이면 애프터마켓 없음. [why D-097]
        int session_open_min  = 0;
        int session_close_min = 0;
        int after_open_min    = 0;
        int after_close_min   = 0;
    };

    OrderGate() : config_()
    {
    }

    explicit OrderGate(Config config) : config_(config)
    {
    }

    // 뮤텍스·원자값·유량 창을 안고 있고 Engine이 한 개를 소유한다 — 복사 대상이 아니다.
    OrderGate(const OrderGate&)            = delete;
    OrderGate& operator=(const OrderGate&) = delete;

    // 위험 한도 주입 — 반드시 order_thread 시작 전에만 호출(config_는 check()에서 락 없이 읽힘).
    void set_config(const Config& config) { config_ = config; }
    const Config& config() const { return config_; }

    // 원장 — 보유·선점·평단·당일 손익과 원장 저널(Quant/include/risk/PositionLedger.h). 체결 반영·시드·조회는 여기로 간다.
    [[nodiscard]] PositionLedger& ledger() noexcept { return ledger_; }
    [[nodiscard]] const PositionLedger& ledger() const noexcept { return ledger_; }

    // 원장 쪽 중첩 형 — 바깥 코드가 OrderGate:: 이름으로 쓰던 것을 그대로 받는다.
    using OpenIntent   = PositionLedger::OpenIntent;
    using OrderRef     = PositionLedger::OrderRef;
    using FillResult   = PositionLedger::FillResult;
    using SellableView = PositionLedger::SellableView;
    using HeldPos      = PositionLedger::HeldPos;

    // ── 주문 검증 (true = 통과, false = 거부) ──────────────────────────────
    // evaluate로 판정하고, 막혔으면 describe로 사유 문장을 채운다.
    bool check(const OrderSignal& signal, std::string& reject_reason);
    // 판정만 한다 — 문자열을 만들지 않는다. 통과하면 유량 창·중복 창에 이 신호를 적는다.
    [[nodiscard]] GateVerdict evaluate(const OrderSignal& signal);
    // 판정을 로그·운영단말에 나가는 문장으로 옮긴다. 원장 잠금을 잡지 않는다.
    [[nodiscard]] std::string describe(const GateVerdict& verdict) const;

    // ── 한도 클램프 (BUY NEW 전용) ─────────────────────────────────────────
    // 한도를 넘는 수량을 거부하는 대신 한도 안으로 줄여 돌려준다. 분할 매수 전략은 매 틱
    // 같은 분할 단계를 다시 내므로, 넘친다고 버리면 그 종목은 영원히 발주되지 않고 초당 주문
    // 예산만 태운다. 줄여서라도 나가는 편이 의도(부분 진입)에 가깝다.
    // 검사 대상은 수량·명목·포지션·총노출 한도뿐이다. 킬스위치·entry_halt·손실컷 같은
    // "발주 자체를 막는" 게이트는 여기서 손대지 않는다 — 그건 check()가 그대로 거부한다.
    // 반환 0 = 여유 없음(발주 불가). 정정·취소는 원본 수량을 그대로 돌려준다.
    // SELL NEW는 매도가능수량(보유 - 미체결매도)으로 깎는다. 자기 익절 지정가가 자기
    // 청산을 막아 KIS가 40240000으로 주문을 통째로 거부하면 한 주도 못 빠져나온다.
    // 원장이 그 종목을 0으로 알고 있으면 손대지 않는다(과소 인식 방어).
    int clamp_buy_quantity(const OrderSignal& signal);

    // ── Kill switch ─────────────────────────────────────────────────────────
    void set_kill_switch(bool on);

    bool is_killed() const
    {
        return kill_switch_.load();
    }

    // ── Entry halt (신규 진입 정지) ────────────────────────────────────────────
    // kill_switch_(전방향 하드스톱)와 분리된 "BUY-only 정지" 플래그. 지수 급락·일일손실 등
    // 국면 리스크로 신규 진입만 막고 보유분 청산(SELL)은 통과시킨다 — 급락장에서 청산이 미완료로 남지 않게(C-2).
    void set_entry_halt(bool on);

    // 운영단말(HALT_REQ)이 켜는 수동 정지 — entry_halt_(국면 자동, RegimeFileJudge가 갱신)와
    //  분리된 플래그다. 같은 변수를 같이 쓰면 RegimeFileJudge의 자동 해제가 사람이 켠 정지를
    //  모른 채 지워버린다 — is_entry_halted()에서만 OR로 합친다. [why D-091]
    //  매수·매도 따로다(D-095). 매도 정지는 전략이 내는 SELL NEW만 막고(SignalDispatcher::from_strategy),
    //  운영단말 수동 매도와 국면 강제청산(force_liquidate)은 그대로 나간다.
    void set_manual_halt(OrderSide side, bool on);

    // 전략 스레드가 죽어 주문 쪽이 마무리에 들어갔을 때 켜는 진입 정지. 앞의 둘과 따로 두는 이유는 D-091과 같다 —
    //  한 원천의 자동 해제가 다른 원천이 켠 정지를 모른 채 지운다. 박동이 돌아오면 주문 스레드가 끈다. [why D-114]
    void set_strategy_down_halt(bool on);

    bool is_manual_buy_halted() const
    {
        return manual_buy_halt_.load();
    }

    bool is_manual_sell_halted() const
    {
        return manual_sell_halt_.load();
    }

    bool is_entry_halted() const
    {
        return entry_halt_.load() || manual_buy_halt_.load() || strategy_down_halt_.load();
    }

    // 매수 명목 비율(0~1). 국면 점수를 스위치가 아니라 비율로 옮긴 값 — 전략이 분할 단계 명목에 곱한다.
    //  게이트 자체는 이 값으로 주문을 막지 않는다(0이면 entry_halt가 같이 켜진다). [why D-083]
    void set_entry_scale(double entry_scale);

    double entry_scale() const
    {
        return entry_scale_.load();
    }

    // 유니버스 스캔이 낸 종합 점수 랭크(1=최고)를 주입한다. 재스캔이 매번 덮어쓴다.
    //  total은 랭크의 모집단 크기(등록 종목 수). 비어 있으면 우선순위 바는 동작하지 않는다.
    //  z는 같은 점수의 표준화값 — 랭크는 "몇 번째"만 알려주고 "얼마나 더 좋은지"는 못 알려준다.
    //  교체는 격차가 잡음보다 큰지를 봐야 하므로 z가 따로 필요하다.
    //  종목은 id로 받는다(intern_symbol·symbol_id_of). 표는 id 배열 세 개(랭크·"나보다 위" 수·z)로 굳혀
    //  check()가 "나보다 위인데 아직 안 산 종목 수"를 원장 순회(보유·선점 ≤ 슬롯 수) 안에서 정수 조회로 센다 —
    //  문자열 맵 전체를 돌며 항목마다 해시하던 것(300종목 4.2µs)을 없앤다. 표는 통째로 바꿔 끼우고(shared_ptr)
    //  읽는 쪽은 포인터만 복사하므로 plan_displacement가 맵을 복사해 락 밖으로 들고 나오던 일도 없다. [why D-112]
    using PriorityEntry = EntryPriority::Entry;
    void set_entry_priority(const std::vector<PriorityEntry>& entries, int total);

    // ── 교체 진입 ────────────────────────────────────────────────────────────
    //  슬롯이 꽉 찬 상태에서 new_ticker가 들어오려 할 때, 비워 줄 최약체를 고른다.
    //  고르기만 하고 주문은 내지 않는다 — 교체 매도는 주문 스레드의 DisplacementDesk가 낸다. 여기서는 계획만 만든다.
    struct DisplacePlan
    {
        bool             ok = false;
        std::string      account;      // 비울 종목의 계좌
        std::string      ticker;       // 비울 종목(발주·로그용 문자열)
        symbol::SymbolId symbol = symbol::kNone; // 비울 종목 id(쿨다운 기록용)
        int         quantity = 0;      // 매도할 수량(미체결 매도 제외)
        double      average_price = 0.0;
        double      victim_z = 0.0;
        double      new_z = 0.0;
        std::string reason;       // 로그·원장에 남길 사유
    };
    //  new_symbol은 신호의 symbol_id(원장 테이블 번호). 모르는 종목(kNone)은 점수가 없어 거절된다.
    DisplacePlan plan_displacement(const std::string& account, symbol::SymbolId new_symbol) const;
    // 교체를 실제로 발주했을 때 호출 — 쿨다운·횟수·슬롯 예약을 기록한다. beneficiary는 자리를 받을 종목 id.
    void note_displacement(const DisplacePlan& plan, symbol::SymbolId beneficiary);
    // 동시 보유 슬롯이 꽉 찼는가(신규 종목을 열 자리가 없는가).
    bool slots_full() const;
    // 신규 종목을 열 여력이 없는가 — 자리(슬롯)와 예산(총노출) 중 하나만 막혀도 없다.
    bool capacity_full() const;

    // position/reserved/슬롯가득참을 한 번의 잠금으로 함께 읽는다. [why D-086]
    // [inv] 총노출 상한(capacity_full()의 나머지 절반)은 포함하지 않는다 — 호출부가 따로 더한다.
    struct EntrySnapshot
    {
        int  position   = 0;
        int  reserved   = 0;
        bool slots_full = false;
    };
    EntrySnapshot entry_snapshot(const std::string& account, const std::string& ticker) const;

    // ── PnL stale guard (B2) — 잔고 대조 정체 시 신규 매수 정지 ──────────────
    // rest_price_feed 모드는 daily_pnl_을 잔고 대조(총평가금 델타)로만 갱신한다. 잔고조회가
    // 연속 실패(12002 타임아웃 등)해 서킷브레이커가 잔고 대조를 스킵하는 동안 daily_pnl_은 낡은
    // 값이라, 그 창에서 손실이 나도 §4 손실컷이 트립하지 못한다. Engine이 실패 스트릭이 임계를
    // 넘으면 이 플래그를 세워 BUY NEW만 보수적으로 차단(SELL 청산·취소는 통과 — entry_halt와 동일
    // 의미론). 잔고조회 복구 시 자동 해제. 손실컷을 대체하지 않고 "믿을 수 없는 창"만 보수 처리.
    void set_pnl_stale(bool on);

    bool is_pnl_stale() const
    {
        return pnl_stale_.load();
    }

    // ── 자정 리셋 ────────────────────────────────────────────────────────────
    //  장 시작 때 호출. Both 역할은 데이터 스레드가 직접 부르고, 역할이 분리되면 제어 요청을 거쳐 주문 스레드가 부른다.
    void reset_daily();

    // ── 장부 사본 발행 (D-114 단계 2.5) ──────────────────────────────────────
    //  전략 쪽이 읽을 사본을 한 판 낸다. 게이트가 든 전역값(국면 플래그·한도)을 넘기고, 종목별 값과 열린 슬롯·
    //  여력은 PositionLedger::publish가 positions_mutex_ 한 번으로 함께 담는다 — 따로 담으면 판 안에서 서로
    //  안 맞는다. [why D-086] 장부를 바꾼 스레드가 부른다 — 접수(주문 스레드)와 체결(체결 스레드) 둘이라
    //  발행끼리는 원장의 발행 잠금으로 줄을 세운다. 읽는 쪽은 그 잠금을 안 잡는다.
    void publish_ledger(ipc::LedgerSnapshot& snapshot) const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 원장 파티션 키 — (계좌 번호, 종목 id). 만드는 규칙과 [why]는 Quant/include/risk/LedgerKeys.h.
    static constexpr uint32_t kUnknownAccount = LedgerKeys::kUnknownAccount;
    using PosKey = PositionLedger::PosKey;

    Config config_;
    std::atomic<bool> kill_switch_{false};
    std::atomic<bool> entry_halt_{false};  // 신규 진입(BUY NEW)만 정지, SELL 청산은 통과 — 국면 리스크용
    std::atomic<bool> manual_buy_halt_{false};  // 운영단말 HALT_REQ(BUY)가 켜는 수동 진입 정지 — entry_halt_와 별도 원천 [why D-091]
    std::atomic<bool> strategy_down_halt_{false};  // 전략 사망 마무리가 켜는 진입 정지 — 위 둘과 별도 원천 [why D-114]
    std::atomic<bool> manual_sell_halt_{false}; // 운영단말 HALT_REQ(SELL)가 켜는 전략 매도 정지 [why D-095]
    std::atomic<double> entry_scale_{1.0}; // 매수 명목 비율(0~1). 국면 점수의 비례판 [why D-083]
    std::atomic<bool> pnl_stale_{false};   // 잔고 대조 정체 → daily_pnl 미갱신, BUY NEW 보수 정지(B2)

    // 진입 우선순위 표와 교체 기록. 표는 불변 스냅샷(포인터 복사), 교체 기록은 자체 락 — 둘 다 잎 잠금이다. [why D-112]
    using PriorityTable = EntryPriority::Table;
    EntryPriority entry_priority_;

    // 원장 — 자체 락(positions·journal·pnl·publish)을 든다. 판정은 ledger_.read()로 positions_mutex_를 쥔 채 본다.
    PositionLedger ledger_;

    mutable std::mutex rate_mutex_;
    std::deque<TimePoint> order_times_min_; // 최근 1분 내 주문 시각 (분당 제한)
    std::deque<TimePoint> order_times_sec_; // 최근 1초 내 주문 시각 (초당 제한)

    // 중복 신호 키 — (계좌 번호, 전략 번호, 종목 id, 방향, 지정가). 문자열 "계좌:전략:종목:방향[:가격]"을 신호마다
    //  이어 붙여 해시하던 것(105ns + 해시)을 정수 다섯 개로 바꿨다. 시장가는 price 0. [why D-112]
    struct SignalKey
    {
        uint32_t                   account  = kUnknownAccount;
        strategy_table::StrategyId strategy = strategy_table::kNone;
        symbol::SymbolId           symbol   = symbol::kNone;
        int32_t                    side     = 0;
        int64_t                    price    = 0;

        bool operator==(const SignalKey&) const = default;
    };

    struct SignalKeyHash
    {
        size_t operator()(const SignalKey& key) const noexcept;
    };

    mutable std::mutex deduplicate_mutex_;
    std::unordered_map<SignalKey, TimePoint, SignalKeyHash> last_signal_; // 신호 키 → 마지막 신호 시각
};
