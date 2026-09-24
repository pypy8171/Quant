#pragma once
#include "core/StrategyTable.h"
#include "core/SymbolTable.h"
#include "core/Types.h"
#include "risk/LedgerJournal.h"
#include "risk/LedgerKeys.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PositionLedger — 주문 원장(보유·선점·평단·매도가능·당일 손익)과 원장 저널
//
//  계좌·종목별 보유와 미체결 선점, 평단, 매도가능수량, 당일 손익, 전략별 서브원장을 든다. 원장을 바꾸는 사건은
//  바꾸기 전에 저널에 적고, 재기동은 저널을 다시 적용해 원장을 되살린다. OrderGate에서 떼어 냈다 — 한도·교체
//  판정과 장부가 한 클래스에 있으면 장부 규칙을 고칠 때 판정 코드를 같이 읽어야 했다. 판정은 OrderGate에 남고,
//  판정이 원장을 볼 때는 Reader로 positions_mutex_를 쥔 채 여러 맵을 한 시점으로 읽는다.
//
// 쓰는 스레드는 셋이다 — 주문 스레드(선점·접수·거부·취소), 체결 스레드(체결), 데이터 스레드(시드·잔고 대조).
//   positions_mutex_가 보유·선점 맵과 전략 서브원장, 계좌 이름표(keys_)를 함께 지킨다.
// [lock-order] positions_mutex_ → LedgerJournal::stage_mutex_(잎, 메모리만). 디스크 쓰기(journal_flush)는 positions_mutex_ 밖. ledger_publish_mutex_ → positions_mutex_.
//   OrderGate::check()는 Reader를 쥔 채 EntryPriority의 displace_mutex_·priority_mutex_를 잡는다
//   (positions → {displace, priority}). pnl_mutex_는 독립 스코프에서만 잡는다.
// ─────────────────────────────────────────────────────────────────────────────
namespace ipc
{
class LedgerSnapshot; // 장부 사본. 구현(.cpp)에서만 include한다 — 배선은 원장 → ipc 한 방향 [why D-114]
struct LedgerGlobals;
}

class PositionLedger
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 원장 파티션 키 — (계좌 번호, 종목 id). 만드는 규칙과 [why]는 Quant/include/risk/LedgerKeys.h.
    static constexpr uint32_t kUnknownAccount = LedgerKeys::kUnknownAccount;
    using PosKey = LedgerKeys::Key;
    template <class V>
    using PosMap = LedgerKeys::Map<V>;

    PositionLedger()                                 = default;
    PositionLedger(const PositionLedger&)            = delete;
    PositionLedger& operator=(const PositionLedger&) = delete;

    // ── 판정용 읽기 창 ────────────────────────────────────────────────────────
    // 살아 있는 동안 positions_mutex_를 쥐고 원장 맵을 읽게 한다. OrderGate의 판정(check·clamp_buy_quantity·
    //  capacity_full·plan_displacement·entry_snapshot)이 보유·선점·평단을 한 시점으로 봐야 해서 둔다.
    //  [inv] 쥔 채 positions_mutex_를 잡는 이 원장의 함수(position·open_slot_count 등)를 부르면 같은 락을 다시 잡아
    //  멈춘다 — 맵과 keys()만 읽는다. 복사·이동이 안 되는 형이라 read()가 그 자리에 만들어 준다(C++17 복사 생략).
    class Reader
    {
    public:
        explicit Reader(const PositionLedger& ledger) : lock_(ledger.positions_mutex_), ledger_(ledger)
        {
        }

        const PosMap<int>& positions() const noexcept { return ledger_.positions_; }
        const PosMap<int>& reserved() const noexcept { return ledger_.reserved_; }
        const PosMap<double>& reserved_price() const noexcept { return ledger_.reserved_price_; }
        const PosMap<double>& average_prices() const noexcept { return ledger_.average_prices_; }
        const PosMap<int>& sellable() const noexcept { return ledger_.sellable_; }
        const PosMap<TimePoint>& opened_at() const noexcept { return ledger_.opened_at_; }
        const std::unordered_set<symbol::SymbolId>& slot_exempt() const noexcept { return ledger_.slot_exempt_; }
        const LedgerKeys& keys() const noexcept { return ledger_.keys_; }

    private:
        std::lock_guard<std::mutex> lock_;
        const PositionLedger&       ledger_;
    };

    [[nodiscard]] Reader read() const
    {
        return Reader(*this);
    }

    // 종목 id 테이블 주입 — Engine이 자기 SymbolTable을 넘겨 신호의 symbol_id와 원장 키가 같은 번호를 쓴다.
    //  nullptr이면 자체 테이블(단독 테스트·벤치). [inv] 원장에 첫 키가 생기기 전에 부른다 — 뒤에 바꾸면
    //  이미 든 키의 번호가 다른 테이블의 것이 된다.
    void set_symbol_table(symbol::SymbolTable* table) noexcept
    {
        keys_.set_symbol_table(table);
    }

    // 원장이 아는 종목 id(모르면 kNone). 찾기만 하고 새 번호는 주지 않는다 — 신호에 id가 비었을 때
    //  디스패처·라우터가 부른다(Quant/src/core/SignalDispatcher.cpp symbol_of).
    [[nodiscard]] symbol::SymbolId symbol_id_of(std::string_view ticker) const
    {
        return keys_.symbols().lookup(ticker);
    }

    // 종목을 테이블에 등록하고 id를 돌려준다 — 우선순위 표·테스트가 원장보다 먼저 종목을 알 때 쓴다.
    [[nodiscard]] symbol::SymbolId intern_symbol(std::string_view ticker)
    {
        return keys_.symbols().intern(ticker);
    }

    // 원장이 쓰는 종목 테이블(읽기) — id를 로그용 문자열로 되돌릴 때만 쓴다.
    [[nodiscard]] const symbol::SymbolTable& symbols() const noexcept
    {
        return keys_.symbols();
    }

    // 원장 저널 — 원장을 바꾸는 모든 사건(시드·주문 의도·접수·거부·체결·취소·대조·현금)을 바꾸기 전에 파일에 적고,
    //  재기동은 오늘 파일을 처음부터 다시 적용해 원장을 되살린다. directory/ledger_<date>.bin을 열고 그 자리에서
    //  리플레이한다. 거짓이면 파일을 못 연 것 — Engine은 기동을 거부한다(원장 없이 주문을 내지 않는다). [why D-113]
    //  [inv] Engine이 첫 신호 전, set_symbol_table 직후에 한 번만 부른다. 실계좌 복수 프로세스가 같은 경로를
    //  공유하면 안 된다(파일 하나 = 원장 하나).
    [[nodiscard]] bool set_journal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync);

    [[nodiscard]] bool journal_open() const noexcept
    {
        return journal_ && journal_->ok();
    }

    // 기동 리플레이 결과 — 재기동 대조가 로그·판정 행에 쓴다(적용 레코드 수, 꼬리 잘림 여부).
    [[nodiscard]] const ledger_journal::ReplayResult& journal_replay() const noexcept
    {
        return replay_result_;
    }

    // 지금 쓰고 있는 저널 파일 경로(저널이 없으면 빈 경로). 테스트·점검 도구가 파일을 직접 읽을 때 쓴다.
    [[nodiscard]] std::filesystem::path journal_path() const
    {
        return journal_ ? journal_->path() : std::filesystem::path();
    }

    // append 실패 누계. 0이 아니면 파일이 원장보다 뒤처진 것이다 — 되돌릴 수 없는 사건(체결·대조)은 원장에는
    //  반영하고 실패만 센다. 건강 판정(check_runtime_health.py)이 이 값을 읽는다.
    [[nodiscard]] uint64_t journal_failures() const noexcept
    {
        return journal_failures_.load(std::memory_order_relaxed);
    }

    // 리플레이가 남긴 미결 주문 — INTENT는 적혔는데 체결·취소·거부로 닫히지 않은 것들. 엔진이 죽은 순간
    //  거래소 호가창에 살아 있었을 주문이다. 재기동 대조(Engine::resolve_open_intents)가 KIS 미체결조회와
    //  맞춰, 살아 있으면 라우터 이력에 되살리고(늦은 체결통보가 전략까지 이어진다) 없으면 선점을 푼다. [why D-113]
    struct OpenIntent
    {
        uint64_t    order_id         = 0;
        uint64_t    kis_order_number = 0; // ACCEPT를 못 본 주문은 0 — 접수됐는지조차 모른다는 뜻
        std::string account;
        std::string ticker;
        std::string strategy_name;
        OrderSide   side      = OrderSide::BUY;
        OrderType   type      = OrderType::MARKET;
        int         remaining = 0;   // 아직 안 닫힌 수량
        double      price     = 0.0;
        bool        accepted  = false;
    };

    // 주문번호 오름차순 — 적힌 순서대로 본다. 리플레이 뒤에만 채워져 있다.
    [[nodiscard]] std::vector<OpenIntent> open_intents() const;

    // 주문 하나를 저널 레코드에 잇는 이름표 — 주문 사건(INTENT·ACCEPT·REJECT·FILL·CANCEL)이 같이 받는다.
    struct OrderRef
    {
        uint64_t  order_id         = 0; // OrderSignal.client_order_number
        uint64_t  kis_order_number = 0; // KIS ODNO 정수(접수 전 0)
        OrderType type             = OrderType::MARKET;
    };

    // 전략 번호 테이블 — 원장 서브원장·중복 신호 키가 쓰는 번호. 엔진이 전략을 등록할 때 받는다. 고정 이름은
    //  FORCE_LIQ·LIMIT_TRIM은 Engine이 받아 디스패처에 넘기고, UNLINKED는 라우터, DISPLACE는 DisplacementDesk가
    //  생성자에서 받는다. 신호마다 부르지 않는다. [why D-112]
    [[nodiscard]] strategy_table::StrategyId strategy_index_of(std::string_view strategy_id)
    {
        return strategies_.intern(strategy_id);
    }

    [[nodiscard]] const strategy_table::StrategyTable& strategy_table() const noexcept
    {
        return strategies_;
    }

    // 전략 이름표 알맹이를 남이 놓은 것으로 바꾼다 — 갈라 띄운 두 프로세스가 공유 쪽지 위 한 표를 같이 본다.
    //  [inv] 스레드가 뜨기 전, 원장에 첫 서브원장 키가 생기기 전에 부른다 — 뒤에 바꾸면 이미 든 키의
    //  번호가 다른 표의 것이 된다. [why D-114]
    void adopt_strategy_table(const strategy_table::TableSlots&                          slots,
                              std::function<strategy_table::StrategyId(std::string_view)> register_hook);

    // ── 상태 업데이트 ───────────────────────────────────────────────────────
    // 주문 생명주기는 증권사 원장 순서를 따른다: KIS 전송 직전 on_intent(선점 + INTENT 기록) → 응답에 따라
    //  on_accepted(ACCEPT) 또는 on_reject(선점 해제 + REJECT) → 체결통보 on_fill_confirmed(FILL) / 취소 on_cancel(CANCEL).
    //  check()는 positions_ + reserved_ 합산으로 한도를 보므로 미체결 주문이 과잉 주문을 차단한다.
    //  원장은 (account_id:ticker)로 파티셔닝 — 계좌별 독립. [why D-113]
    // KIS 전송 직전 — 선점(reserved_)을 잡고 INTENT를 적는다. 거짓이면 저널에 적히지 않아 선점도 되돌린 것이다 —
    //  라우터는 그 주문을 보내지 않고 거부한다(적히지 않은 주문은 나가지 않는다).
    [[nodiscard]] bool on_intent(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                                 double price, const OrderRef& reference, strategy_table::StrategyId strategy = strategy_table::kNone);
    // KIS 접수(주문번호 확보). 원장 상태는 그대로, INTENT를 ACCEPT로 확정한다.
    void on_accepted(const std::string& account, const std::string& ticker, OrderSide side, int quantity, const OrderRef& reference);
    // KIS 거부·전송 예외 — INTENT가 잡은 선점을 풀고 REJECT를 적는다. reason은 KIS 메시지·예외 문구.
    void on_reject(const std::string& account, const std::string& ticker, OrderSide side, int quantity, const OrderRef& reference,
                   std::string_view reason);
    // 접수 뒤 선점 — 라우터가 on_intent로 옮겨 가기 전 이름. 저널에는 INTENT(order_id 0)로 남는다. 테스트 전용으로 남긴다.
    void on_accept(const std::string& account, const std::string& ticker, OrderSide side, int quantity, double price);

    void on_accept(const std::string& ticker, OrderSide side, int quantity, double price);

    void add_realized_pnl(double pnl);  // SELL 체결 시 실현 손익 추가 (테스트에서도 사용)
    // C-1: rest_price_feed 모드는 체결 콜백이 없어 daily_pnl_이 0에 고정되고, 그러면 §4의
    //  BUY 전용 손실컷이 동작하지 못한다.
    //  Engine이 잔고 재조회로 당일 기준선 대비 평가금 델타를 계산해 이 값으로 직접 덮어쓴다.
    //  (add_realized_pnl은 누적, 이건 절대치 세팅 — 잔고 대조 전용)
    void set_daily_pnl(double pnl);

    // ── 총노출 게이트용 자본 주입 (§3d) ──────────────────────────────────────
    // 잔고 대조 스레드가 총평가금(tot_evlu_amt) 갱신 시 호출. check()가 락 없이 읽도록 atomic.
    // 0이면 §3d 게이트 비활성(자본 미상 시 폴백 안전 — 종목당·동시보유 백스톱이 커버).
    void set_equity(double equity);
    double equity() const { return equity_.load(std::memory_order_relaxed); }

    // 주문가능현금(원). 잔고 대조가 output2에서 읽어 넣는다. 0=미주입(클램프 비활성).
    //  총평가금(equity_)과 다르다 — 평가금이 1억이어도 미체결 지정가와 미결제 매수가
    //  현금을 묶으면 살 수 없다. 이 값이 없으면 게이트가 그걸 모른 채 계속 발주하고
    //  KIS가 40250000으로 전량 거부한다(2026-09-08 59건).
    void set_available_cash(double available_cash);
    double available_cash() const { return available_cash_.load(std::memory_order_relaxed); }

    // ── 원장 부트스트랩 (G5) — 기동 시 실계좌 보유분을 원장에 시드 ─────────────
    // 체결이 아니므로 reserved_/daily_pnl_은 불변. 기동 때와 데이터 스레드의 재동기(LedgerReconciler) 때 부른다.
    // positions_·average_prices_·sellable_·opened_at_을 설정한다.
    // on_fill_confirmed 재사용 금지(수수료·실현손익 오적립) → 전용 API.
    // 계좌키는 신호가 쓰는 account_id와 반드시 동일해야 조회된다(단일계좌는 account="").
    // sellable < 0 이면 "모름"으로 보고 보유수량을 그대로 쓴다.
    void seed_position(const std::string& account, const std::string& ticker, int quantity, double average_price,
                       int sellable);
    void seed_position(const std::string& account, const std::string& ticker, int quantity, double average_price);

    void seed_position(const std::string& ticker, int quantity, double average_price);

    // ── 미체결 취소/정정 축소 시 선점 해제 (C5, MM-1) ─────────────────────
    // quantity = 취소된 미체결 잔량(>0). reserved_만 감소 — positions_/average_price는 불변(취소는 체결 아님).
    // 방향은 on_fill_confirmed의 선점 해제와 동일: BUY 선점(+)은 -quantity, SELL 선점(-)은 +quantity.
    // 호출 규약: 반드시 KIS 취소 성공(rt_cd=="0") 이후에만 호출 — 실패 시 호출하면 이중해제.
    // reference 기본값을 `= OrderRef{}`로 두지 않는다 — 중첩 구조체의 멤버 기본값은 바깥 클래스가 끝나야 읽히는데
    //  GCC는 기본 인자에서 그것을 요구해 컴파일을 거부한다(리눅스 빌드 09-22). 인자 없는 겹정의 본문은 그 뒤에
    //  읽히므로 거기서 만든다. 아래 on_fill_confirmed도 같다.
    void on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                   const OrderRef& reference);
    void on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity);

    void on_cancel(const std::string& ticker, OrderSide side, int quantity);

    // ── 체결 확인 시 원장 갱신 ─────────────────────────────────────────────
    // H0STCNI0 체결통보 수신 후 호출. average_price 재계산 + 실현손익 적립.
    struct FillResult
    {
        double average_price    = 0.0; // 갱신된 매수 평균단가
        int    net_quantity      = 0;   // 체결 후 순 보유수량
        double commission   = 0.0; // 수수료 (0.015%)
        double tax          = 0.0; // 거래세 (매도 0.20%)
        double realized_pnl = 0.0; // 이번 체결 실현손익 (SELL만 양수)
        // SELL인데 원장이 평단을 모를 때 true. 그 경우 realized_pnl은 0으로 두고 daily_pnl에도
        //  더하지 않는다 — (price-0)*quantity가 이익으로 잡히면 일일 손실컷이 무력화된다(C-1).
        bool   basis_unknown = false;
        // strategy_id별 서브원장(전략별 손익 귀속, D-089) — 위 필드들과 계산은 독립이고
        //  daily_pnl_·kill switch 판정에는 안 들어간다. 참고용 집계만.
        double strategy_realized_pnl  = 0.0; // 이번 체결의 strategy_id 기준 실현손익 (SELL만)
        bool   strategy_basis_unknown = false; // strategy_id가 비었거나 그 전략의 평단을 모를 때 true
    };
    // strategy: OrderSignal.strategy_index(strategy_index_of로 받은 번호). kNone이면 서브원장 갱신을 건너뛴다.
    //  reference: 이 체결이 속한 주문(저널 FILL 레코드용). 라우터가 ODNO를 못 이은 체결은 비워 둔다.
    FillResult on_fill_confirmed(const std::string& account, const std::string& ticker,
                                 OrderSide side, int quantity, double price,
                                 strategy_table::StrategyId strategy, const OrderRef& reference);
    FillResult on_fill_confirmed(const std::string& account, const std::string& ticker,
                                 OrderSide side, int quantity, double price,
                                 strategy_table::StrategyId strategy = strategy_table::kNone)
    {
        return on_fill_confirmed(account, ticker, side, quantity, price, strategy, OrderRef{});
    }

    FillResult on_fill_confirmed(const std::string& ticker, OrderSide side,
                                 int quantity, double price)
    {
        return on_fill_confirmed(std::string(), ticker, side, quantity, price, strategy_table::kNone);
    }

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
    //  반환값은 걷어낸 종목 id(호출부가 로그·대조 행에 쓴다).
    std::vector<symbol::SymbolId> prune_positions(const std::vector<std::string>& live_tickers, int min_age_sec);

    // 선점은 접수 때만 생기므로 라우터 이력이 정본이다. 살아있는 주문이 없는 선점을 푼다.
    //  live_symbols는 종목 id 인덱스 비트(라우터가 이력의 symbol_id로 만든다). 문자열 판은 브로커 잔고처럼
    //  입력이 문자열인 곳용.
    std::vector<std::string> prune_reservations(const std::vector<bool>& live_symbols);
    std::vector<std::string> prune_reservations(const std::vector<std::string>& live_tickers);

    // 미체결 매도를 브로커에서 취소한 뒤 매도가능수량을 되돌린다. 취소는 KIS에서 수량을 푸는데
    //  게이트의 sellable_은 잔고 시드값(ord_psbl_qty) 그대로 남아, 미체결이 없는데도 자기 청산이
    //  막힌다(09-09 000215: 13:45 취소 후 16분간 "매도가능수량 0"으로 교체 진입 4회 무산).
    //  보유수량을 넘지 않게 자른다. 원장이 모르는 종목이면 아무 것도 하지 않는다.
    void restore_sellable(const std::string& account, const std::string& ticker, int quantity);

    // 매도가능수량이 0으로 잘린 이유를 로그에 남길 조각. 원장 보유·잔고 주문가능(시드/대조값)·
    //  이 세션 미체결 매도(선점). 원장이 모르는 종목이면 held=0이고 나머지도 0이다.
    struct SellableView
    {
        int held     = 0;
        int possible_quantity_cap = 0;
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
    int absorb_missed_sell(const std::string& account, const std::string& ticker, int balance_quantity);

    // ── 조회 ─────────────────────────────────────────────────────────────────
    // 계좌 지정 버전(주 경로) + account="" 하위호환(단일 계좌).
    int    position(const std::string& account, const std::string& ticker) const;
    // 정수 id 버전 — 문자열 해시가 없다. [why D-105] 전략은 LedgerSnapshot 사본을 읽는다(D-114).
    //  이 조회 함수들은 주문 스레드(속도 제한 판정)와 risk 내부에서만 쓴다.
    int    position(const std::string& account, symbol::SymbolId symbol) const;
    int    reserved(const std::string& account, const std::string& ticker) const;
    int    reserved(const std::string& account, symbol::SymbolId symbol) const;
    double average_price(const std::string& account, const std::string& ticker) const;
    int    position(const std::string& ticker) const { return position(std::string(), ticker); }
    int    reserved(const std::string& ticker) const { return reserved(std::string(), ticker); }
    double average_price(const std::string& ticker) const { return average_price(std::string(), ticker); }
    double daily_pnl() const;

    // ── 보유 포지션 스냅샷 (G3 강제청산) — net>0 실보유분만 락 하 복사 반환 ──────
    //  보호 주문 평가, LedgerReconciler 재동기, Engine::held_positions가 부른다.
    //  강제 청산은 LedgerSnapshot을 읽고 이 함수를 쓰지 않는다.
    // symbol은 원장 키의 종목 id — 강제청산·한도 정리가 미체결 잔량을 물을 때 문자열 대신 이 번호로 묻는다.
    struct HeldPos
    {
        std::string      account;
        std::string      ticker;
        int              quantity;
        double           average_price;
        symbol::SymbolId symbol      = symbol::kNone;
        bool             slot_exempt = false; // 슬롯 계산 밖 종목(바스켓 슬리브 소유). 강제청산·초과 정리는 이 종목을 건너뛴다 [why D-109]
    };
    std::vector<HeldPos> snapshot_positions() const;

    // 사본에 못 실은 줄 수 — 한 프로세스는 한 계좌만 다루는데(엔진 안 다계좌 금지) 다른 계좌 줄이
    //  원장에 섞여 들어온 경우다. 0이 아니면 판정 행이 잡는다.
    [[nodiscard]] uint64_t ledger_foreign_account_rows() const
    {
        return ledger_foreign_account_rows_.load(std::memory_order_relaxed);
    }

    // ── 슬롯 계산 밖 종목(바스켓 슬리브 소유) ─────────────────────────────────
    //  동시 보유 상한(3c)·교체 후보·강제청산·초과 정리는 전부 "원장 전체 = 스캔 슬리브 것"으로 세는데, 목표 비중표로
    //  60종목을 드는 바스켓이 같은 원장에 들어오면 상한 20에 걸려 매수가 거부되고 교체가 바스켓을 먼저 판다.
    //  바스켓 로더·전략이 자기 종목을 여기 넣으면 그 종목은 슬롯을 먹지도, 교체·청산 대상이 되지도 않는다.
    //  종목당 명목·수량 한도·현금·총노출·일일 손실은 그대로 적용된다(바스켓도 계좌 위험을 진다). [why D-109]
    void set_slot_exempt(const std::vector<std::string>& tickers);
    // 번호로 받는 갈래 — 전략 쪽 제어 요청이 티커가 아니라 id를 실어 온다(레코드에 문자열을 안 싣는다). [why D-114]
    void set_slot_exempt_by_id(const std::vector<symbol::SymbolId>& symbols);
    bool is_slot_exempt(symbol::SymbolId symbol) const;
    std::vector<symbol::SymbolId> slot_exempt_symbols() const; // 오름차순 id

    // 열린 슬롯 수 — 보유 수량 > 0인 종목 + 보유 없이 매수 선점만 있는 종목. positions_mutex_를 잡는다.
    size_t open_slot_count() const;

    // check() 3절의 원장 키 — 처음 보는 계좌·종목은 등록한다. positions_mutex_를 잠깐 잡는다.
    [[nodiscard]] PosKey register_signal(const OrderSignal& signal);

    // 장 시작(OrderGate::reset_daily) — 미체결 선점을 비운다. 보유·평단은 영속 원장이라 두고, 저널에도 적지 않는다.
    void expire_reservations();

    // 장부 사본 한 판(D-114 단계 2.5). 종목별 값과 원장에서 셈하는 전역값(열린 슬롯·여력)을 positions_mutex_
    //  한 번으로 담는다. fill_globals는 게이트가 든 전역값을 채우는 자리로, 발행 잠금을 쥔 뒤·원장 잠금 전에
    //  한 번 불린다. 두 한도는 여력 판정(capacity_full과 같은 셈)에 쓴다.
    //  [lock-order] ledger_publish_mutex_ → positions_mutex_. 발행 잠금을 잡는 곳은 여기뿐이라 고리가 없다.
    void publish(ipc::LedgerSnapshot& snapshot, const std::function<void(ipc::LedgerGlobals&)>& fill_globals,
                 int max_concurrent_positions, double max_gross_exposure_percent) const;

private:
    // 선점 해제의 유일한 경로 — 취소 통보(on_cancel)와 체결 통보(on_fill_confirmed)가 함께 쓴다.
    //  없는 선점은 손대지 않고, 과잉 해제는 0에서 멈춘다. 규칙이 두 곳에 갈라져 있으면 한쪽만
    //  고쳐지므로 여기 하나만 둔다. 호출 전에 positions_mutex_를 잡아야 한다(내부에서 잡지 않음).
    void release_reservation(const PosKey& key, int delta);

    // on_intent의 reserved_/reserved_price_ 갱신 본체 — 저널 리플레이(apply_record)도 이걸 그대로 써서
    //  기동 시 복구된 상태가 실시간 경로와 같은 규칙을 거친다. [inv] positions_mutex_를 잡고 부른다.
    void apply_reservation_delta(std::string_view account, std::string_view ticker, int delta, double price);

    // ── 저널 ────────────────────────────────────────────────────────────────
    // 레코드 하나를 저널 버퍼에 쌓고(계좌·종목을 채워서) seq를 돌려준다. 디스크는 건드리지 않는다 — 원장 잠금 안에서
    //  불러 순서를 잡고, 쓰기는 잠금을 푼 뒤 journal_flush()가 한다. 리플레이 중이거나 저널이 없으면 0. [why CODE_REVIEW W-2]
    uint64_t journal_append(ledger_journal::Record& record, std::string_view account, std::string_view ticker);
    // 쌓인 레코드를 디스크에 쓴다. 실패는 세고 로그 한 줄. [inv] positions_mutex_를 쥔 채 부르지 않는다.
    void journal_flush();
    // seq가 디스크에 남았는지 — 먼저 쓰고 묻는다. 리플레이 중이거나 저널이 없으면 참(적을 것이 없다).
    //  INTENT만 이걸로 확인하고, 못 남겼으면 선점을 되돌린다.
    bool journal_written(uint64_t sequence);
    // 함수 끝에서 journal_flush()를 부른다. positions_mutex_ 잠금보다 먼저 선언하면 잠금이 풀린 뒤에 쓴다
    //  (지역 변수는 선언의 역순으로 사라진다).
    struct JournalFlushAfter
    {
        PositionLedger& ledger;

        ~JournalFlushAfter();
    };
    // 잔고 대조가 맞춘 종목의 지금 상태(보유·평단·매도가능·선점) 한 줄 — 리플레이는 이 값을 그대로 놓는다.
    //  [inv] positions_mutex_를 잡고 부른다.
    void journal_adjust(const PosKey& key, std::string_view reason);
    // 리플레이 — 레코드 한 줄을 실시간 경로와 같은 함수로 원장에 적용한다. journal_append는 replaying_로 막힌다.
    void apply_record(const ledger_journal::Record& record);
    void apply_adjust_locked(const PosKey& key, const ledger_journal::Record& record);
    // 리플레이 중에만 미결 주문을 센다 — 실시간 경로는 라우터 history_가 같은 것을 안다(hot path에 더 얹지 않는다).
    void track_open_intent(const ledger_journal::Record& record);

    std::atomic<double> available_cash_{0.0}; // 주문가능현금 스냅샷. 잔고 대조가 갱신, clamp_buy_quantity가 락 없이 읽음
    std::atomic<double> equity_{0.0};      // 총평가금 스냅샷(§3d 총노출 게이트 분모). 잔고 대조가 갱신, check()가 락 없이 읽음

    // 원장 키 표 — 종목 테이블(자체 락)과 계좌 이름. 계좌 이름 쪽은 positions_mutex_를 잡고 쓴다.
    LedgerKeys keys_;

    mutable std::mutex positions_mutex_;
    PosMap<int>    reserved_;    // (account,ticker) → 미체결 선점 수량 (BUY +, SELL -). 재주문 차단용
    PosMap<double> reserved_price_; // (account,ticker) → 미체결 선점가(§3d 총노출 계산용). reserved_와 동일 생명주기로 정리
    // 원장 저널 — set_journal() 이전엔 nullptr(저널 없이 동작, 테스트·벤치 기본). replaying_은 set_journal 안에서만
    //  참(스레드 시작 전)이라 락 없이 읽는다. [why D-113]
    std::unique_ptr<ledger_journal::LedgerJournal> journal_;
    ledger_journal::ReplayResult replay_result_;
    bool                         replaying_ = false;
    std::atomic<uint64_t>        journal_failures_{0};
    // 리플레이가 남긴 미결 주문. set_journal 안에서만 쓰이고(스레드 시작 전) 그 뒤로는 읽기만 한다.
    std::unordered_map<uint64_t, OpenIntent> open_intents_;
    PosMap<int>    positions_;   // (account,ticker) → 실체결 순보유 수량 (양수=롱)
    PosMap<double> average_prices_;
    // account:ticker -> 매도가능수량. 보유수량과 다르다: 기동 전 세션이 남긴 미체결 매도,
    //  미결제분 때문에 KIS가 실제로 받아주는 매도 수량은 보유보다 적을 수 있다. 이걸 모르면
    //  전량 청산이 40240000(주문가능분 없음)으로 통째 거부돼 한 주도 못 빠져나온다.
    //  기동 시드에서 잔고의 ord_psbl_qty로 채우고, 이후 체결로 증감시킨다.
    PosMap<int>       sellable_;         // (account,ticker) → 매도가능수량(주)
    PosMap<int>       missed_sell_seen_; // (account,ticker) → 직전 대조에서 본 잔고 수량(2회 연속 확인용)
    PosMap<TimePoint> opened_at_;        // (account,ticker) → 포지션이 0에서 열린 시각(교체 최소 보유 판정)
    std::unordered_set<symbol::SymbolId> slot_exempt_; // 슬롯 계산 밖 종목(바스켓 소유). positions_mutex_ 보호 [why D-109]
    // 발행끼리 줄 세우는 잠금 — 사본의 판 번호는 한 번에 한 스레드만 뒤집어야 한다.
    mutable std::mutex ledger_publish_mutex_;
    // 사본에 못 실은 다른 계좌 줄의 누적 수. publish_ledger만 쓴다.
    mutable std::atomic<uint64_t> ledger_foreign_account_rows_{0};

    // 전략별 서브원장(D-089, 손익 귀속 전용) — positions_/average_prices_와 같은 락(positions_mutex_)으로 보호.
    //  키는 (전략 번호, 종목 id). 전략 이름 문자열 하나가 키이던 때는 한 전략이 여러 종목을 사면 평단이 섞였다
    //  (DEVSCALE이 A·B를 같이 들면 A 매도의 손익이 B 매수가에 물렸다). 계좌 축은 종목 원장이 든다. [why D-112]
    struct StrategyKey
    {
        strategy_table::StrategyId strategy = strategy_table::kNone;
        symbol::SymbolId           symbol   = symbol::kNone;

        bool operator==(const StrategyKey&) const = default;
    };

    struct StrategyKeyHash
    {
        size_t operator()(const StrategyKey& key) const noexcept;
    };

    strategy_table::StrategyTable                            strategies_; // 전략 이름 → 번호. 자체 소유(엔진·디스패처·라우터가 이 표를 쓴다)
    std::unordered_map<StrategyKey, int, StrategyKeyHash>    strategy_positions_;
    std::unordered_map<StrategyKey, double, StrategyKeyHash> strategy_average_prices_;

    mutable std::mutex pnl_mutex_;
    double daily_pnl_{0.0};
};
