#pragma once
#include "core/Types.h"
#include "ipc/FillKey.h"
#include "ipc/OrderHistory.h"
#include "ipc/OrderJournal.h"
#include "core/ReconcilePlan.h"
#include "risk/OrderGate.h"
#include "api/IOrderExecutor.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderRouter  —  주문 전처리·중계(FEP, Front-End Processor) 역할의 주문 라우팅 레이어
//
//  흐름(신규 주문, submit → new_route. 취소·정정은 modify_route가 따로 맡는다):
//    OrderSignal
//       │
//       ▼
//    한도 클램프·이력 가드 — 취소 빗나감 뒤 매수 보류, 같은 시장가 매도 중복 생략
//       │
//       ▼
//    OrderGate::check()   — 검사 항목과 순서의 정본은 Quant/src/risk/OrderGate.cpp의 check
//       │ PASS
//       ▼
//    take_intent()        — 장부에 INTENT를 먼저 적고 선점을 잡는다. 못 적으면 보내지 않는다 [why D-113]
//       │
//       ▼
//    IOrderExecutor::submit_order_acknowledgement() — KIS 전송 → ODNO·조직번호 수신(실패면 error_code)
//       │
//       ├─ 성공 → ACCEPTED, 장부 ACCEPT, ZMQ publish_order(ok=true)
//       └─ 실패 → REJECTED, 장부 REJECT, ZMQ publish_order(ok=false). 전송 타임아웃이면 되묻기 스레드를 띄운다
// ─────────────────────────────────────────────────────────────────────────────

struct OrderRouterConfig
{
    int max_history = 500; // 보관할 최대 주문 이력 건수
};

class OrderRouter
{
public:
#ifdef HAS_ZMQ
    OrderRouter(OrderGate& gate, IOrderExecutor& kis,
                ZmqBridge* zmq = nullptr,
                OrderRouterConfig config = OrderRouterConfig());
#else
    OrderRouter(OrderGate& gate, IOrderExecutor& kis,
                OrderRouterConfig config = OrderRouterConfig());
#endif

    // ── 주문 제출 — 검증 → KIS 전송 → 상태 기록 ─────────────────────────
    [[nodiscard]] ManagedOrder submit(const OrderSignal& signal);

    // ── 신규 주문을 세 토막으로 — 전송만 다른 스레드에 맡길 때 쓴다 ─────────
    //  submit의 신규 경로는 open_new → send_new → close_new를 한 스레드에서 차례로 부른 것과 같다. 주문 스레드는
    //  open_new·close_new를, 전송 스레드는 send_new만 부른다 — 게이트·장부·이력은 주문 스레드 하나만 만진다. [why D-151]
    struct NewOrderSend;
    // 전송 직전까지 연다(클램프·가드·게이트·INTENT). 보낼 것이 없으면(로컬 거부, 예약매도 취소 뒤 재발주로 이미 접수)
    //  끝난 주문을 돌려준다. [inv] 주문 스레드 전용.
    [[nodiscard]] std::variant<ManagedOrder, NewOrderSend> open_new(const OrderSignal& signal);
    // KIS로 보내고 응답을 send에 담는다. kis_ 밖의 라우터 상태는 만지지 않으므로 어느 스레드에서 불러도 된다.
    //  예외는 밖으로 내지 않고 send에 적는다.
    void send_new(NewOrderSend& send) const noexcept;
    // 응답으로 접수·거부를 확정하고 장부·이력에 적는다. [inv] 주문 스레드 전용.
    [[nodiscard]] ManagedOrder close_new(NewOrderSend&& send);

    // ── 취소·정정도 같은 세 토막 — 원주문 사본 → KIS 취소/정정 → 원주문 닫기 ──────────
    //  submit의 취소·정정 경로는 open_modify → send_modify → close_modify를 차례로 부른 것과 같다. 스레드 약속은
    //  신규와 같다. [why D-151]
    struct ModifyOrderSend;
    // 원주문을 값으로 뜨고, 정정이면 새 수량의 INTENT까지 적는다. 대상이 없거나 못 적었으면 끝난 주문을 돌려준다.
    //  [inv] 주문 스레드 전용.
    [[nodiscard]] std::variant<ManagedOrder, ModifyOrderSend> open_modify(const OrderSignal& signal);
    // KIS 취소·정정을 보내고 응답을 send에 담는다. 예외는 밖으로 내지 않고 send에 적는다.
    void send_modify(ModifyOrderSend& send) const noexcept;
    // 응답으로 접수·거부를 확정하고 원주문을 닫는다. [inv] 주문 스레드 전용.
    [[nodiscard]] ManagedOrder close_modify(ModifyOrderSend&& send);

    // ── 체결통보 수신 — ODNO로 이력 조회 후 FILLED 상태 갱신 ────────────
    void on_fill(const FillNotification& fill_notification);

    // ── 체결통보 구독 재개 — 끊긴 사이 놓친 체결 되찾기 ──────────────────────
    //  소켓이 끊긴 사이의 체결통보는 다시 온다는 보장이 없다. 구독이 새로 붙었다는 표지를 받으면 복구 스레드가
    //  잠시 기다린 뒤(KIS가 재전송하면 그것이 먼저 들어오게) 일별주문체결조회로 주문별 누적을 받아, 이 프로세스가
    //  낸 주문의 누적과 견준 차이를 체결로 넣는다. 단가는 누적 금액 차이 ÷ 수량이다. 부르는 스레드는 바로 돌아온다. [why D-149]
    void on_session_resumed(uint32_t session_generation);
    // 위 조회·반영을 부르는 스레드에서 한 번 한다. 되찾은 주문 수를 돌려준다. 복구 스레드와 시험이 부른다.
    int recover_missed_fills();

    // ── 잔고 대조 기록 (C-2) ────────────────────────────────────────────────
    //  Engine이 브로커 잔고와 장부를 비교한 결과를 장부 CSV에 `RECONCILE` 행으로 남긴다.
    //  order_quantity/order_price=장부 수량·평단, fill_quantity/fill_price=브로커 수량·평단, status=action,
    //  reason에 그 종목의 살아있는 주문 수(live_orders)를 적는다 — 미체결이 있으면 불일치가
    //  체결 지연일 수 있어 사람이 어느 단계인지 가를 근거가 된다. 덮어쓰기·정리(action이 KEEP이
    //  아닌 것)는 LOG_WARN도 낸다. 장부 자체는 바꾸지 않는다(그건 OrderGate 몫).
    using ReconcileNote = reconcile::Row;   // 필드는 core/ReconcilePlan.h. Engine의 plan() 결과를 그대로 받는다
    void record_reconcile(const ReconcileNote& note);

    // ── 재기동 대조 — 장부 저널이 남긴 미결 주문 (D-113) ──────────────────────
    //  저널 리플레이가 "INTENT는 적혔는데 닫히지 않은" 주문을 준다. KIS 미체결조회와 맞춰
    //  ① 아직 호가창에 살아 있는 것은 history_에 ACCEPTED로 되살린다 — 늦은 체결통보가 ODNO로
    //     매칭돼 전략까지 이어진다(안 되살리면 미매핑 체결로 떨어져 귀속이 "UNLINKED"가 된다).
    //  ② KIS가 모르는 것은 선점을 푼다 — 체결됐다면 잔고 대조가 이미 보유를 맞췄고, 안 나갔다면
    //     선점만 남아 그 종목을 하루 종일 막는다.
    //  ③ 주문번호 없이 남은 것(전송 뒤 접수 응답 전에 죽음)은 미체결에서 종목·방향·수량·가격이 맞는 주문과
    //     짝지어 되살린다 — 무조건 풀면 살아 있는 주문을 잊고 같은 수량을 또 낸다.
    //  모의투자는 ACCEPT를 본 주문을 살아 있는 것으로 보고, 미체결조회는 ③이 있을 때만 쓴다.
    //  [inv] 스레드 시작 전, 잔고 시드 뒤에 한 번.
    struct AdoptResult
    {
        int restored = 0; // history_에 되살린 미체결 주문
        int released = 0; // 선점만 푼 주문(KIS가 모르는 것)
    };
    AdoptResult adopt_open_intents(const std::vector<OrderGate::OpenIntent>& intents);

    // 내부 주문번호를 highest 다음부터 매긴다. 재기동하면 번호가 1부터 다시 시작해 같은 날 저널 안에서 다른 주문과
    //  겹치므로, 리플레이가 본 가장 큰 번호를 넘긴다. 이미 그보다 크면 그대로 둔다. [inv] 스레드 시작 전에 부른다.
    void continue_order_numbers(uint64_t highest) noexcept;

    // 내부 주문번호 문자열("ORD-000123") → 저널에 적는 정수(123). 형식이 다르면 0.
    [[nodiscard]] static uint64_t order_number_of(std::string_view order_id) noexcept;

    // 살아있는 주문이 없는데 장부에 남은 선점을 푼다. 살아 있다고 치는 것은 이력의 접수·미체결 주문과,
    //  INTENT를 적고 KIS 답을 기다리는 주문(in_flight_symbols_)이다 — 선점은 전송 전 INTENT 때 생기고
    //  이력에는 답이 온 뒤에야 들어간다. 통보를 한 번 놓치면 선점이 슬롯을 물고 하루를 가서 정리한다.
    //  데이터 스레드가 잔고 대조와 같은 사이클에 부른다. 반환값은 푼 종목 수. [why D-113]
    int sweep_stale_reservations();

    // ── 일별 리셋 (거래일 첫 회차) — 체결 목격 기록(fill_sightings_) 정리 ─────────────
    void reset_daily();

    // 재연결 뒤 재전송으로 보고 장부에 넣지 않은 체결통보 누적 수.
    [[nodiscard]] uint64_t replayed_fills() const noexcept
    {
        return replayed_fills_.load(std::memory_order_relaxed);
    }

    // ── 통계 조회 ─────────────────────────────────────────────────────────
    struct Stats
    {
        uint64_t total    = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
    };
    Stats statistics() const;

    // KIS 주문 API(신규·취소·정정)와 전송 타임아웃 되묻기의 미체결조회를 부른 누적 횟수. 주문 스레드가
    //  submit 전후 값을 비교해 발주 간격(order_min_interval_ms)을 실제 호출 뒤에만 건다 — 게이트·ENTRY_HALT의
    //  로컬 거부는 KIS에 안 나가는데도 같은 간격을 먹어 재기동 직후 거부 62건이 31초를 삼켰다(09-10 12:58). [why D-035]
    //  기동 취소 스레드·되묻기 스레드도 이 값을 올리므로, 그 사이에 걸린 로컬 거부도 간격을 먹을 수 있다.
    uint64_t kis_calls() const
    {
        return kis_calls_.load(std::memory_order_relaxed);
    }

    // ── 최근 N건 이력 조회 ────────────────────────────────────────────────
    std::vector<ManagedOrder> recent(int count = 20) const;

    // ── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ─────────────────
    //  재기동하면 history_가 비어 이전 세션 주문의 ODNO를 잊는다. 그래서 접수 때마다
    //  살아있는 주문을 부속 파일(open_orders.txt)에 적어 두고 여기서 읽어 취소한다. 파일에 없는
    //  미체결(ODNO를 못 받은 주문)은 브로커 미체결조회로 채운다 — 모의도 VTTC0081R로 답한다.
    //  방치하면 오전 분할 매수 지정가가 하루 종일 걸려 있으면서 (1) 주문가능현금을
    //  묶고(40250000 도배) (2) 청산 관리가 청산한 직후 되사서 원치 않는 재진입을 만든다
    //  (2026-09-08 047050: 13:07 청산 → 오전 ODNO 22814가 13:11 체결).
    //  네트워크 왕복이 건당 3~5초라 85건이면 5분이다. 기동을 그만큼 막으면 장중
    //  재기동이 사실상 불가능해지므로 취소는 별도 스레드로 돌린다. 부속 파일은 비우지
    //  않는다 — 읽은 줄을 carry_rows_에 들고 스냅샷마다 이번 세션 줄과 합쳐 쓰고, 취소가
    //  접수되거나 이미 끝난 것으로 확인된 줄만 뺀다. 한도 거부·전송 실패로 남긴 줄은 다음 재기동에 넘어간다. [why D-035]
    //  KisClient는 토큰·레이트리밋을 뮤텍스로 직렬화해 스레드 공유를 전제로 한다.
    void cancel_stale_orders_async();
    // 전송이 타임아웃 난 주문을 브로커에 되물어 맞춘다. 응답을 못 받았을 뿐 접수됐을 수 있고,
    //  그렇게 남은 주문은 엔진 장부 밖이라 보유분을 묶은 채 아무도 못 지운다. [why D-101]
    void reconcile_unknown_order_async(std::string ticker);

    // 줄 세워 둔 장부 CSV·사유 줄을 부르는 스레드에서 전부 써 버린다. 쓸 것이 없으면 아무것도 안 한다.
    //  평소에는 전담 스레드가 알아서 비우므로 부를 일이 없다 — 방금 낸 주문의 행을 곧바로 파일에서
    //  읽어 확인해야 하는 시험이 쓴다.
    void flush_file_writes();

    // 되묻기 스레드·기동 취소 스레드·두 쓰기 스레드를 세우고 기다린 뒤, 남은 미결주문 스냅샷·장부 줄을 마저 쓴다.
    ~OrderRouter();
    // 스레드·뮤텍스를 소유한다 — 복사는 원본과 사본이 같은 자원을 두 번 닫는 길이라 막는다.
    OrderRouter(const OrderRouter&)            = delete;
    OrderRouter& operator=(const OrderRouter&) = delete;

private:
    std::string next_id();
    // 직전 KIS 주문/취소/정정 오류코드를 " [코드]" 꼬리표로 만든다(EGW00201 재시도 판별용). 없으면 "".
    static std::string kis_error_suffix(const OrderAck& acknowledgement);
    int64_t     record(const ManagedOrder& managed_order,
                       int64_t* open_orders_us = nullptr); // 쓴 시간(us) 반환 — 구간 계측용, 버려도 된다
    // 살아있는(ACCEPTED·미체결 잔량>0) 주문 목록을 부속 파일 본문 문자열로 만든다.
    //  호출자는 history_mutex_를 보유해야 한다. 파일 쓰기는 기록기(journal_) 쓰기 스레드가 락 밖에서 한다.
    std::string snapshot_open_orders_locked() const;
    // 스냅샷을 새로 떠서 대기함에 넘긴다(history_mutex_를 잠깐 잡고, 쓰기는 쓰기 스레드가).
    //  이전 세션 줄(carry_rows_)이 빠질 때마다 부른다 — 기동 취소 스레드와 주문 스레드의 청산차단 해소.
    void        rewrite_open_orders();

    // ── MM-1: 주문 생명주기 라우팅 ────────────────────────────────────────
    ManagedOrder new_route(const OrderSignal& signal);     // 기존 신규 주문 경로
    [[nodiscard]] ManagedOrder modify_route(const OrderSignal& signal);  // action=CANCEL·REPLACE(정정)
    // SELL이 40240000(주문가능분 없음)으로 막히면: 그 종목의 미체결 예약매도를 조회·취소하고
    //  시장가 매도를 1회 재시도한다(장중 자가 청산 정리). 성공 시 kis_order_no 채운 OrderAck,
    //  예약 없음/취소 실패 시 빈 acknowledgement. 이전 세션·수동 예약이 보유수량을 묶은 경우를 해소.
    //  취소한 예약이 이번 세션 주문이면 history_를 CANCELLED로 닫고 게이트 선점을 푼다(C-2).
    //  reference/intent_taken: 재매도도 장부에 INTENT를 적은 뒤에만 나간다. 이미 적었으면(본 경로가 먼저 보낸 뒤
    //  40240000으로 돌아온 경우) 다시 적지 않는다. [why D-113]
    [[nodiscard]] OrderAck reconcile_blocked_sell(const OrderSignal& signal, const OrderGate::OrderRef& reference,
                                                  bool& intent_taken);

    // KIS 전송 직전 장부 기록 — 선점을 잡고 INTENT를 적는다. 거짓이면 파일에 안 적혀 주문을 보내지 않는다.
    [[nodiscard]] bool take_intent(const OrderSignal& signal, const OrderGate::OrderRef& reference);

    // ── 신규 주문 단계 (new_route) ──────────────────────────────────────────
    //  신규 주문 한 건이 단계 함수들을 지나며 들고 다니는 상태. 순서는 클램프 → 이력 가드 → 게이트 → 전송 → 마무리다.
    //  각 단계가 거짓을 돌려주면 그 자리에서 거부로 닫고 이력까지 적은 것이다 — new_route는 그대로 돌려주기만 한다.
    struct NewRoute
    {
        OrderSignal         signal;          // 들어온 신호의 사본 — 클램프가 수량을 고친다
        ManagedOrder        managed_order;
        OrderGate::OrderRef order_reference; // 장부 레코드 이름표 — 내부 주문번호와 주문 유형. ODNO는 접수 뒤에 붙는다
        OrderAck            acknowledgement;
        std::string         reject_reason;
        int                 allowed          = 0;     // 게이트가 허락한 수량(clamp_buy_quantity)
        bool                sell_no_quantity = false; // 매도가능이 모자라 예약매도 취소부터 해 봐야 하는가
        bool                freed            = false; // 예약매도 취소 뒤 재발주가 이미 접수됐는가
        bool                intent_taken     = false; // 장부에 INTENT를 적었는가
        // 구간 계측. 게이트까지 두 구간(이력 가드·게이트)은 stamp_gate_stages가 찍는다. [why D-117] [why D-126]
        int64_t entered_ns           = 0; // new_route에 들어온 시각
        int64_t history_guard_ns     = 0; // 이력 잠금·중복 가드에 쓴 시간 합
        int64_t history_lock_wait_ns = 0; // 그중 잠금을 기다린 몫
        // 접수 왕복지연과 그 안의 초당 한도 버킷 대기. count()의 타입 그대로 — MSVC는 long long이라 long이면 잘린다(C4244)
        std::chrono::milliseconds::rep rtt_ms         = 0;
        std::chrono::milliseconds::rep bucket_wait_ms = 0;
        // 왕복지연을 재기 시작한 시각 — INTENT 전이다. 전송 스레드로 넘기면 넘겨받기까지의 대기도 여기 들어간다.
        std::chrono::steady_clock::time_point send_started{};
        bool        transport_failed = false; // 전송이 예외로 끝났다 — 접수 여부를 모른다
        std::string transport_error;          // 그 예외 문구

        // 게이트까지의 두 구간을 한 번에 찍는다 — 이력 가드 몫을 게이트에서 빼 둘이 겹치지 않게 한다. [why D-117]
        void stamp_gate_stages();
    };
    // 한도 클램프 — 한도를 넘치면 거부 대신 한도 안으로 줄인다. 매도가능이 모자라면 sell_no_quantity를 세운다.
    void clamp_new_order(NewRoute& route);
    // 직전 취소가 "대상 없음"이던 종목의 매수를 잠깐 막는다. 막았으면 거부로 닫고 참.
    [[nodiscard]] bool hold_after_cancel_miss(NewRoute& route);
    // 같은 종목·전략의 시장가 매도가 아직 살아 있으면 다시 보내지 않는다. 생략했으면 거부로 닫고 참.
    [[nodiscard]] bool skip_duplicate_market_sell(NewRoute& route);
    // 예약매도 취소 시도와 OrderGate::check. 거부면 거부로 닫고 거짓.
    [[nodiscard]] bool pass_gate(NewRoute& route);
    // 장부 INTENT를 적고 보낼 채비를 한다. 못 적었으면 거부로 닫고 거짓.
    [[nodiscard]] bool prepare_transmit(NewRoute& route);
    // 전송이 예외로 끝난 주문을 거부로 닫는다 — 선점을 풀고 이력에 적는다.
    void close_transport_failure(NewRoute& route);
    // 전송 뒤 마무리 — 청산차단 재시도, 접수·거부 확정, 발행, 이력 저장.
    void finalize_new_order(NewRoute& route);

    // ── 발주 경로 공통 조각 ──────────────────────────────────────────────
    // 새 주문 항목을 PENDING으로 만들고 총 주문 수를 올린다. now는 부른 쪽이 경로에 들어온 시각이다.
    ManagedOrder make_pending_order(const OrderSignal& signal, std::chrono::system_clock::time_point now);
    // 거부로 표시하고 거부 수를 올린다. 로그·장부·이력 기록은 부른 쪽이 한다.
    void mark_rejected(ManagedOrder& managed_order, std::string reason);
    // 전송 전에 끝난 주문을 이력에 적는다. 이 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라
    //  따로 세지 않는다. 그래서 history_store_us도 같은 값이다. [why D-126]
    void record_before_transport(ManagedOrder& managed_order);
    // 주문 결과를 ZMQ로 발행한다. ZMQ 없이 빌드하면 아무것도 안 한다.
    void publish_order_result(const OrderSignal& signal, bool accepted);
    // 체결을 ZMQ로 발행한다. ZMQ 없이 빌드하면 아무것도 안 한다.
    void publish_fill_result(const FillNotification& fill_notification, const std::string& strategy_id,
                             const PositionLedger::FillResult& result);
    // 체결 한 건을 장부 CSV에 적는다. 평단을 모르는 매도면 경고를 먼저 남긴다. 락 밖에서 부른다.
    void emit_fill(const ManagedOrder& fill_order, const FillNotification& fill_notification, int quantity,
                   const PositionLedger::FillResult& result);
    // KIS 취소(남은 수량 전부)를 보낸다. 예외는 부른 쪽이 잡는다. [inv] 이 경로의 kis_calls_는 여기서만 올린다(취소 3곳).
    [[nodiscard]] OrderAck send_cancel(const std::string& ticker, const std::string& kis_order_no,
                                       const std::string& krx_forwarding_org_no, int quantity);
    // KIS 미체결을 조회해 open_orders에 담는다. 실패·예외면 경고를 남기고 거짓이며 open_orders는 건드리지 않는다.
    //  두 문구는 부른 자리의 로그 머리말이다(실패 문구 뒤에 오류 설명, 예외 문구 뒤에 예외 내용이 붙는다).
    bool fetch_open_orders(std::vector<OpenOrder>& open_orders, std::string_view failure_message,
                           std::string_view exception_message);
    // 이전 세션 줄(carry_rows_)에서 그 ODNO 줄을 뺀다. 뺐으면 참. carry_mutex_를 안에서 잡는다.
    bool erase_carry_row(const std::string& kis_order_no);
    // 부속 파일의 수량 칸을 읽는다. 숫자가 아니면 빈 값.
    static std::optional<int> parse_quantity(const std::string& text);

    // 취소·정정할 원주문의 값 사본 — 락 밖에서 KIS를 부르는 동안 history_ 원소가 축출될 수 있어 값으로 뜬다.
    struct OriginalOrder
    {
        std::string ticker;
        std::string kis_order_no;
        std::string krx_forwarding_org_no;
        std::string account;
        OrderSide   side        = OrderSide::NONE;
        int         outstanding = 0; // 뜬 시점의 미체결 잔량
    };
    // 살아 있는 원주문을 찾아 값으로 뜬다. 없으면 거짓. [inv] history_mutex_를 쥐고 부른다.
    bool snapshot_live_original_locked(uint64_t client_order_number, OriginalOrder& original);
    // 취소·정정이 접수된 뒤 원주문을 CANCELLED로 닫고 그 시점 잔량만큼 선점을 푼다. 잔량은 지금 confirmed_quantity로
    //  다시 센다 — 전송 사이 체결 스레드가 올렸을 수 있어서다. 원주문이 이미 빠졌으면 release_if_gone만큼 푼다.
    //  [inv] history_mutex_를 쥐고 부른다. [lock-order] history_mutex_ → 장부 positions_mutex_(on_fill과 같다).
    void close_live_original_locked(const OrderSignal& signal, const OriginalOrder& original, int release_if_gone);
    // 취소·정정 한 건이 세 토막을 지나며 들고 다니는 상태.
    struct ModifyRoute
    {
        OrderSignal         signal;
        ManagedOrder        managed_order;
        OriginalOrder       original;
        int                 new_quantity = 0;    // 정정 수량. 취소는 쓰지 않는다
        OrderGate::OrderRef order_reference;      // 정정의 장부 레코드 이름표
        OrderAck            acknowledgement;
        bool                transport_failed = false; // 전송이 예외로 끝났다
        std::string         transport_error;          // 그 예외 문구
    };
    // close_modify의 두 갈래 — 응답으로 거부·접수를 확정하고 원주문을 닫은 뒤 이력에 적는다.
    void close_cancel(ModifyRoute& route);
    void close_replace(ModifyRoute& route);
    // 원주문이 없을 때 그 이유를 가른다. 체결 흔적이 있으면 참을 돌려준다. [inv] history_mutex_를 쥐고 부른다. [why D-035]
    bool classify_missing_original_locked(uint64_t client_order_number, const char*& gone_why);
    // 취소할 원주문이 없을 때 CANCELLED로 닫는다. 체결 흔적이 있으면 그 종목 매수를 잠깐 잠근다. [why D-035]
    void close_cancel_without_target(ManagedOrder& managed_order, const OrderSignal& signal,
                                     bool original_may_have_filled, const char* gone_why);

    // ── 청산차단 자가정리 단계 (reconcile_blocked_sell) ─────────────────────
    // 모의투자 — 이번 세션 이력과 이전 세션 줄에서 그 종목의 예약매도를 모은다.
    void collect_session_sells(const OrderSignal& signal, std::vector<OpenOrder>& opens);
    // 예약매도 한 건을 취소하고, 이번 세션 주문이면 이력·선점을, 이전 세션 줄이면 부속 파일을 정리한다. 취소됐으면 참.
    bool cancel_blocking_sell(const OrderSignal& signal, const OpenOrder& open);

    // 재기동 때 되살리는 주문 항목을 만든다. signal의 종목 id·전략 번호는 여기서 채운다(파일의 문자열이라 복원 때 한 번).
    ManagedOrder make_restored_order(std::string order_id, std::string kis_order_no, OrderSignal signal,
                                     std::chrono::system_clock::time_point at);

    // ── 재기동 대조 단계 (adopt_open_intents) ────────────────────────────────
    // 살아 있는 INTENT 하나를 이력에 ACCEPTED로 되살리고 주문 번호를 그 위로 올린다.
    void restore_intent(const OrderGate::OpenIntent& intent, uint64_t kis_order_number, const OpenOrder* open);

    // ── 기동 취소 단계 (cancel_stale_orders_async) ─────────────────────────
    //  부속 파일 한 줄 = odno|orgno|ticker|side|remaining.
    using CarryRow = std::array<std::string, 5>;
    // 부속 파일을 읽어 줄로 나눈다. 칸이 모자란 줄·ODNO가 빈 줄은 버린다.
    static std::vector<CarryRow> read_open_orders_file(const std::filesystem::path& path);
    // 부속 파일이 모르는 브로커 미체결을 줄로 채운다(ODNO를 못 받은 주문). [why D-101]
    void add_broker_open_orders(std::vector<CarryRow>& rows);
    // 기동 취소 스레드 본문 — 줄마다 취소하고, 끝난 줄은 부속 파일에서 뺀다.
    void cancel_stale_rows(std::stop_token stop_token, const std::vector<CarryRow>& rows);

    // ── 체결통보 단계 (on_fill) ────────────────────────────────────────────
    // 이전 세션 주문이면 주문 사유 기록에서 이력에 되살린다. [inv] history_mutex_를 쥐고 부른다.
    void restore_from_order_reason_locked(const FillNotification& fill_notification, uint64_t order_number);
    // ODNO 색인, 없으면 원주문번호로 연결된 주문을 찾는다. [inv] history_mutex_를 쥐고 부른다.
    ManagedOrder* find_linked_order_locked(const FillNotification& fill_notification, uint64_t order_number);
    // 재전송 거르기 뒤의 단계. 연결 주문이 없는데 접수 답을 기다리는 신규 주문이 있으면 early_fills_에 붙든다.
    //  [inv] lock은 history_mutex_를 쥔 채로 받고, 여기서 풀 수 있다.
    void route_fill(std::unique_lock<std::mutex>& lock, const FillNotification& fill_notification,
                    const fill_key::FillKey& fill_key, uint64_t order_number);
    // close_new 끝 — 보내는 중 수를 하나 내리고 붙든 체결을 다시 판정한다. history_mutex_를 쥐지 않고 부른다.
    void finish_sending_new();
    // 이 프로세스가 낸 주문이 아닌 체결을 장부에 넣는다. [inv] lock은 history_mutex_를 쥔 채로 받고, 여기서 푼다.
    void apply_unlinked_fill(std::unique_lock<std::mutex>& lock, const FillNotification& fill_notification,
                             const fill_key::FillKey& fill_key, uint64_t order_number);
    // 연결된 주문에 체결 incoming_quantity주를 넣고 장부·장부 CSV·미결 파일·발행까지 한다. 잔량을 넘으면 잔량으로
    //  자른다. note가 비지 않으면 장부 CSV의 사유 칸에 적는다. [inv] lock은 history_mutex_를 쥔 채로 받고, 여기서 푼다.
    void apply_linked_fill(std::unique_lock<std::mutex>& lock, ManagedOrder& managed_order,
                           const FillNotification& fill_notification, int incoming_quantity, std::string_view note);
    // 조회로 되찾은 몫에 드는 늦은 통보면 그 수량을 몫에서 깎고 돌려준다(장부에 다시 넣지 않을 수량).
    //  [inv] history_mutex_를 쥐고 부른다.
    [[nodiscard]] static int consume_recovered_credit_locked(ManagedOrder& managed_order,
                                                             const FillNotification& fill_notification);
    void fill_recovery_loop(std::stop_token stop_token);
    // 신호의 종목 id — 배선이 빠진 경로(테스트·수동)만 문자열로 한 번 채운다.
    symbol::SymbolId symbol_of(const OrderSignal& signal);

    // ── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
    //  history_는 메모리에만 있어 재기동하면 이전 세션 주문의 ODNO를 잊는다. 그 주문이
    //  나중에 체결되면 전략도 사유도 모르는 미매핑 체결로 들어가고, 주문수량을 모르니
    //  잔량 클램프도 걸 수 없다. 접수 시점에 한 줄씩 파일로 남겨 재기동 뒤에도 같은
    //  정보를 복원한다. 파일은 거래일별 append 전용이다 — open_orders.txt는 스냅샷마다
    //  통째로 덮어쓰고 살아 있는 주문만 담으므로 거기에 얹으면 안 된다.
    struct OrderReason
    {
        std::string ticker;
        std::string strategy_id;
        std::string reason;
        OrderSide   side      = OrderSide::BUY;
        int         quantity  = 0;
        double      price     = 0.0;
        double      reference_price = 0.0;
    };
    // 오늘자 기록 파일을 읽어 order_reasons_를 채운다. 첫 체결통보 때 1회.
    //  호출자는 history_mutex_를 보유해야 한다.
    void load_order_reasons_locked();


    OrderGate&       gate_;
    IOrderExecutor&  kis_;
    OrderRouterConfig config_;
#ifdef HAS_ZMQ
    ZmqBridge*       zmq_ = nullptr;
#endif
    // 미매핑 체결("UNLINKED")의 전략 번호 — 생성자에서 한 번 받는다. [why D-112]
    const strategy_table::StrategyId unlinked_strategy_index_;

    // INTENT를 적기 직전부터 이력에 적힐 때까지 걸어 두는 종목 표시. 선점 정리가 이 목록도 살아 있는
    //  선점으로 친다 — 없으면 KIS 답을 기다리는 사이(수백 ms~2초)에 도는 정리가 방금 잡은 선점을 푼다.
    //  [lock-order] in_flight_mutex_ → history_mutex_ → 장부 positions_mutex_(정리 한 곳). 거는 쪽은
    //  in_flight_mutex_만 잠깐 잡는다. [why D-113]
    class InFlightMark
    {
    public:
        InFlightMark(OrderRouter& router, symbol::SymbolId symbol_id);
        ~InFlightMark();
        InFlightMark(const InFlightMark&)            = delete;
        InFlightMark& operator=(const InFlightMark&) = delete;

    private:
        OrderRouter&     router_;
        symbol::SymbolId symbol_id_;
    };

public:
    // open_new가 연 신규 주문 하나 — 전송 스레드로 옮겨 다닌다. 종목 표시(in_flight)는 close_new가 이력에 적은
    //  뒤에 풀린다. [inv] 이 값이 살아 있는 동안 라우터도 살아 있어야 한다(표시가 라우터를 가리킨다).
    struct NewOrderSend
    {
        NewRoute                      route;
        std::unique_ptr<InFlightMark> in_flight;
    };

    // open_modify가 연 취소·정정 하나. 정정만 종목 표시를 건다(새 수량의 INTENT를 적었으므로).
    //  [inv] NewOrderSend와 같다 — 이 값이 살아 있는 동안 라우터도 살아 있어야 한다.
    struct ModifyOrderSend
    {
        ModifyRoute                   route;
        std::unique_ptr<InFlightMark> in_flight;
    };

private:

    std::mutex                    in_flight_mutex_;
    std::vector<symbol::SymbolId> in_flight_symbols_; // 같은 종목이 두 번 들 수 있다(신규 안의 청산 재매도)

    mutable std::mutex history_mutex_;
    // 이번 세션 주문 이력과 ODNO·주문 번호 색인. [inv] history_mutex_를 쥐고만 만진다(OrderHistory에 자체 락 없음).
    //  반환 포인터는 그 락을 쥔 동안만 유효하다. [why D-112]
    OrderHistory       history_;

    using FillKey     = fill_key::FillKey;     // 정의는 ipc/FillKey.h
    using FillKeyHash = fill_key::FillKeyHash;
    // 체결통보 키 하나를 어느 실시간 세션에서 몇 번 봤는가. 같은 세션 안의 같은 키는 분할체결이고,
    //  새 세션에서 앞 세션까지 받은 횟수 이하로 다시 오면 재연결 뒤 재전송이다(on_fill 주석).
    struct FillSighting
    {
        uint32_t session_generation      = 0; // 마지막으로 이 키를 본 세션
        int      accepted_before_session = 0; // 그 세션이 시작되기 전까지 실체결로 받은 횟수
        int      seen_in_session         = 0; // 그 세션에서 본 횟수
        int      accepted                = 0; // 실체결로 받은 총 횟수
    };
    // 체결통보 키 → 목격 기록 (history_mutex_로 보호).
    std::unordered_map<FillKey, FillSighting, FillKeyHash> fill_sightings_;
    // 재연결 뒤 재전송으로 보고 장부에 넣지 않은 통보 수. 수량은 주기 잔고 대조가 맞춘다.
    std::atomic<uint64_t> replayed_fills_{0};
    // 이 통보가 재연결 뒤 재전송인지 판정하고 목격 기록을 올린다. [inv] history_mutex_를 쥐고 부른다.
    [[nodiscard]] bool is_replayed_fill_locked(const FillKey& fill_key, uint32_t session_generation);
    // 미매핑(미연결) 체결로 이미 반영한 키 (history_mutex_로 보호). 전문이 주문수량(ODER_QTY)을 주지 않아
    //  잔량 상한을 못 잡는 통보만 여기로 막는다 — 주문수량을 받은 통보는 아래 unlinked_orders_가 맡는다.
    std::unordered_set<FillKey, FillKeyHash> unlinked_fill_keys_;

    // 미연결 주문 하나의 누적 상태. 연결된 주문의 (signal.quantity, confirmed_quantity) 짝과 같은 역할이다.
    struct UnlinkedOrder
    {
        int order_quantity     = 0; // 전문 [16]ODER_QTY. 0이면 "전문이 안 줘서 모른다"
        int confirmed_quantity = 0; // 지금까지 장부에 반영한 수량
    };

    // 주문 단위 키(거래일+ODNO, 체결 건별 칸은 0) → 그 주문의 누적 상태 (history_mutex_로 보호).
    //  ODNO는 영업일마다 재사용되므로 거래일을 키에 같이 담는다. 일별 리셋으로 비운다.
    std::unordered_map<FillKey, UnlinkedOrder, FillKeyHash> unlinked_orders_;

    // 접수 답을 닫기 전에 온 체결 하나. 전송 스레드가 답을 받고 주문 스레드가 close_new로 ODNO를 적기까지의
    //  틈에 체결통보가 먼저 올 수 있다. [why D-151]
    struct EarlyFill
    {
        FillNotification notification;
        FillKey          key;
        uint64_t         order_number = 0;
    };

    // 붙든 체결 (history_mutex_로 보호). close_new가 비운다.
    std::vector<EarlyFill> early_fills_;
    // open_new·open_modify(정정)가 넘기고 close_new·close_modify가 닫기 전인 주문 수 (history_mutex_로 보호).
    //  둘 다 새 ODNO를 받는다. 0보다 크면 연결 안 된 체결을 붙든다.
    int sending_new_orders_ = 0;
    // ODNO 정수 → 이전 세션이 남긴 주문 사유 (history_mutex_로 보호). 파일에서 한 번 읽고,
    //  되살린 주문은 지운다(같은 ODNO를 두 번 되살리지 않게).
    std::unordered_map<uint64_t, OrderReason> order_reasons_;
    bool order_reasons_loaded_ = false;
    // 취소가 "취소 대상 없음"으로 되돌아온 종목 → 그 시각 (history_mutex_로 보호, 종목 id 인덱스, 0=없음).
    //  전략은 취소 결과를 보지 못한 채 재구성 주문을 이어 내므로, 원주문이 이미 체결돼
    //  있었으면 대체 주문이 그대로 중복 매수가 된다(09-09 033790·108490 5건).
    //  다음 재구성 주기까지 그 종목의 신규 주문을 짧게 막는다.
    std::vector<std::chrono::steady_clock::time_point> cancel_miss_;
    // 부속 파일 스냅샷 번호. history_mutex_ 아래에서 올리고, 기록기가 io_mutex_ 아래에서 "마지막으로 쓴 번호"와 비교한다.
    uint64_t open_orders_sequence_         = 0;
    // 이전 세션에서 넘어온 미체결 줄(kis_order_no|orgno|ticker|side|remaining). 취소 스레드가 한 건씩 정리한다.
    //  스냅샷이 history_만 보면 취소를 못 마친 줄(한도 거부·종료 중단·크래시)이 이번 세션 첫 기록에서
    //  파일에서 사라지고 다음 재기동은 그 주문을 모른다. 정리될 때까지 스냅샷에 같이 실린다.
    //  [lock-order] history_mutex_ → carry_mutex_. 기동 취소 스레드는 carry_mutex_를 단독으로만 잡는다.
    std::vector<CarryRow> carry_rows_;
    mutable std::mutex                      carry_mutex_;
    // 부속 파일(미결주문·장부 CSV·주문 사유) 쓰기. 쓰기 스레드 둘을 들고 있다.
    //  [inv] 라우터 스레드(stale_threshold_·transport_reconcile_·fill_recovery_)보다 앞에 선언한다 — 그 스레드들이 여기로
    //   줄을 넘기므로 기록기가 나중에 소멸해야 한다. 소멸자는 그 스레드들을 먼저 세운 뒤 journal_.stop()을 부른다.
    //  [lock-order] history_mutex_ → 기록기 io_mutex_ → append_outbox_mutex_ (정본은 Quant/include/ipc/OrderJournal.h).
    OrderJournal journal_;

    // 유령주문 취소 스레드. 종료가 몇 분씩 걸리지 않도록 매 건 전에 stop_token을 본다.
    std::jthread       stale_threshold_;

    // 전송 타임아웃 뒤 되묻기 스레드. 주문 스레드를 막지 않도록 한 번에 한 건만 돌리고,
    //  돌고 있으면 새 요청은 버린다(다음 타임아웃이나 다음 기동이 다시 잡는다).
    std::jthread       transport_reconcile_;
    std::atomic<bool>  reconcile_busy_{false};

    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> accepted_count_{0};
    std::atomic<uint64_t> rejected_count_{0};
    std::atomic<uint64_t> kis_calls_{0};      // [inv] kis_ 주문 호출(신규 2곳·취소는 send_cancel과 open_modify·정정은 open_modify)과 되묻기 미체결조회 1곳, 호출 직전에만 올린다

    // 놓친 체결 되찾기 요청. on_session_resumed가 올리고 복구 스레드가 따라잡는다(fill_recovery_mutex_로 보호).
    std::mutex                  fill_recovery_mutex_;
    std::condition_variable_any fill_recovery_wake_;
    uint64_t                    fill_recovery_requests_ = 0;
    // 복구 스레드. 위 멤버를 쓰므로 그 뒤에 선언한다 — 생성자가 마지막으로 띄우고 소멸자가 맨 먼저 세운다.
    std::jthread fill_recovery_;
};
