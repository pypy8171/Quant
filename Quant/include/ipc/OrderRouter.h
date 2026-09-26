#pragma once
#include "core/Types.h"
#include "ipc/FillKey.h"
#include "core/ReconcilePlan.h"
#include "risk/OrderGate.h"
#include "api/IOrderExecutor.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <stop_token>
#include <thread>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderRouter  —  주문 전처리·중계(FEP, Front-End Processor) 역할의 주문 라우팅 레이어
//
//  흐름(신규 주문, submit → new_route. 취소·정정은 cancel_route·replace_route가 따로 맡는다):
//    OrderSignal
//       │
//       ▼
//    한도 클램프·이력 가드 — 취소 빗나감 뒤 매수 보류, 같은 시장가 매도 중복 생략
//       │
//       ▼
//    OrderGate::check()   — 검사 항목과 순서의 정본은 Quant/src/risk/OrderGate.cpp의 check
//       │ PASS
//       ▼
//    take_intent()        — 원장에 INTENT를 먼저 적고 선점을 잡는다. 못 적으면 보내지 않는다 [why D-113]
//       │
//       ▼
//    IOrderExecutor::submit_order_acknowledgement() — KIS 전송 → ODNO·조직번호 수신(실패면 error_code)
//       │
//       ├─ 성공 → ACCEPTED, 원장 ACCEPT, ZMQ publish_order(ok=true)
//       └─ 실패 → REJECTED, 원장 REJECT, ZMQ publish_order(ok=false). 전송 타임아웃이면 되묻기 스레드를 띄운다
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
                OrderRouterConfig config = OrderRouterConfig())
        : gate_(gate), kis_(kis), config_(config), zmq_(zmq), unlinked_strategy_index_(gate.ledger().strategy_index_of("UNLINKED")) {}
#else
    OrderRouter(OrderGate& gate, IOrderExecutor& kis,
                OrderRouterConfig config = OrderRouterConfig())
        : gate_(gate), kis_(kis), config_(config), unlinked_strategy_index_(gate.ledger().strategy_index_of("UNLINKED")) {}
#endif

    // ── 주문 제출 — 검증 → KIS 전송 → 상태 기록 ─────────────────────────
    [[nodiscard]] ManagedOrder submit(const OrderSignal& signal);

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
    //  Engine이 브로커 잔고와 원장을 비교한 결과를 원장 CSV에 `RECONCILE` 행으로 남긴다.
    //  order_quantity/order_price=원장 수량·평단, fill_quantity/fill_price=브로커 수량·평단, status=action,
    //  reason에 그 종목의 살아있는 주문 수(live_orders)를 적는다 — 미체결이 있으면 불일치가
    //  체결 지연일 수 있어 사람이 어느 단계인지 가를 근거가 된다. 덮어쓰기·정리(action이 KEEP이
    //  아닌 것)는 LOG_WARN도 낸다. 원장 자체는 바꾸지 않는다(그건 OrderGate 몫).
    using ReconcileNote = reconcile::Row;   // 필드는 core/ReconcilePlan.h. Engine의 plan() 결과를 그대로 받는다
    void record_reconcile(const ReconcileNote& note);

    // ── 재기동 대조 — 원장 저널이 남긴 미결 주문 (D-113) ──────────────────────
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

    // 살아있는 주문이 없는데 원장에 남은 선점을 푼다. 살아 있다고 치는 것은 이력의 접수·미체결 주문과,
    //  INTENT를 적고 KIS 답을 기다리는 주문(in_flight_symbols_)이다 — 선점은 전송 전 INTENT 때 생기고
    //  이력에는 답이 온 뒤에야 들어간다. 통보를 한 번 놓치면 선점이 슬롯을 물고 하루를 가서 정리한다.
    //  데이터 스레드가 잔고 대조와 같은 사이클에 부른다. 반환값은 푼 종목 수. [why D-113]
    int sweep_stale_reservations();

    // ── 일별 리셋 (장 시작) — 체결 목격 기록(fill_sightings_) 정리 ─────────────
    void reset_daily();

    // 재연결 뒤 재전송으로 보고 원장에 넣지 않은 체결통보 누적 수.
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

    // 줄 세워 둔 원장 CSV·사유 줄을 부르는 스레드에서 전부 써 버린다. 쓸 것이 없으면 아무것도 안 한다.
    //  평소에는 전담 스레드가 알아서 비우므로 부를 일이 없다 — 방금 낸 주문의 행을 곧바로 파일에서
    //  읽어 확인해야 하는 시험이 쓴다.
    void flush_file_writes();

    // 되묻기 스레드·기동 취소 스레드·두 쓰기 스레드를 세우고 기다린 뒤, 남은 미결주문 스냅샷·원장 줄을 마저 쓴다.
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
    //  호출자는 history_mutex_를 보유해야 한다. 파일 쓰기는 쓰기 스레드가 write_open_orders_file로 락 밖에서 한다.
    std::string snapshot_open_orders_locked() const;
    // 부속 파일 덮어쓰기(io_mutex_). sequence가 이미 쓴 것보다 오래됐으면 건너뛴다 —
    //  락 밖에서 쓰므로 스냅샷 순서와 쓰기 순서가 뒤집힐 수 있다. 실패는 매매를 막지 않는다.
    void        write_open_orders_file(const std::string& body, uint64_t sequence);
    // 스냅샷을 새로 떠서 대기함에 넘긴다(history_mutex_를 잠깐 잡고, 쓰기는 쓰기 스레드가).
    //  이전 세션 줄(carry_rows_)이 빠질 때마다 부른다 — 기동 취소 스레드와 주문 스레드의 청산차단 해소.
    void        rewrite_open_orders();
    // 부속 파일 쓰기를 전담 스레드에 넘기고 곧바로 돌아온다. 대기함은 한 칸이고 최신이 이긴다 —
    //  중간 스냅샷을 읽는 쪽이 없어서다(다음 기동이 보는 것은 마지막 하나뿐). 주문 스레드가
    //  여기서 디스크를 기다리면 시퀀서 전체가 같이 선다. [why D-123]
    void        queue_open_orders_file(std::string body, uint64_t sequence);
    // 대기 중인 스냅샷을 부르는 스레드에서 끝까지 쓴다. 쓸 것이 없으면 아무것도 안 한다.
    void        flush_open_orders_file();
    // 쓰기 스레드 본문 — 대기함에 뭔가 들어올 때까지 자고, 깨면 비운다. 멈춤 요청 뒤 한 번 더 비운다.
    void        open_orders_writer_loop(std::stop_token stop_token);

    // ── 덧붙이기 큐(원장 CSV·사유) ────────────────────────────────────────
    //  미결주문 파일과 달리 이 둘은 중간 것도 다 남아야 한다. 그래서 한 칸짜리 대기함이 아니라
    //  줄을 세우는 큐이고, 꺼낸 순서와 쓴 순서가 같아야 한다. [why D-124]
    struct PendingLine
    {
        enum class Sink
        {
            TRADE,   // logs/trades_YYYYMMDD.csv
            REASON   // logs/order_reasons_YYYYMMDD.txt
        };

        Sink        sink = Sink::TRADE;
        std::string date;   // 어느 날짜 파일로 갈 줄인가 — 자정을 넘겨도 줄이 제 파일로 간다
        std::string text;   // TRADE는 시각 열까지 붙은 완성된 행(줄바꿈 없음), REASON은 줄바꿈까지 포함한 한 줄
    };

    // 줄 하나를 큐에 놓고 쓰기 스레드를 깨운다. 큐가 한도(kAppendOutboxLimit)를 넘으면 부른 쪽이
    //  직접 비운다 — 디스크가 못 따라가는 동안 큐만 자라는 것을 막는 대신 그때는 예전처럼 기다린다.
    void        queue_append_line(PendingLine line);
    // 큐를 끝까지 비운다. io_mutex_를 먼저 잡고 그 안에서 꺼낸다 — 두 스레드가 같이 비워도
    //  꺼낸 순서와 쓴 순서가 어긋나지 않는다.
    void        flush_append_outbox();
    // 꺼낸 묶음을 파일에 쓴다. 호출자는 io_mutex_를 보유해야 한다. flush는 묶음당 한 번이다.
    void        write_pending_lines_locked(const std::deque<PendingLine>& batch);
    // order_reason_file_을 그 날짜 파일로 (재)연다. 호출자는 io_mutex_를 보유해야 한다.
    void        open_order_reason_file_locked(const std::string& date);
    // 쓰기 스레드 본문 — 큐에 줄이 들어올 때까지 자고, 깨면 비운다. 멈춤 요청 뒤 한 번 더 비운다.
    void        append_writer_loop(std::stop_token stop_token);
    // 거래 원장 CSV 적재 — 주문/체결을 logs/trades_YYYYMMDD.csv 에 한 줄씩 영속화.
    //   event가 빈 문자열이면 managed_order.status를 event로 사용(접수/거부/취소). 체결은 "FILL".
    //   줄을 만들어 덧붙이기 큐에 넣을 뿐 파일은 쓰기 스레드가 쓴다(history_mutex_ 밖에서 호출). [why D-124]
    //   realized_pnl은 매도 체결의 실현손익(수수료·세금 차감 후). 그 외 행은 빈 칸으로 남긴다.
    //   strategy_realized_pnl은 같은 매도 체결의 strategy_id 기준 실현손익(D-089, 열 맨 끝 추가분).
    void        write_trade_row(const std::string& event, const ManagedOrder& managed_order,
                                int fill_quantity, double fill_price,
                                double realized_pnl = 0.0,
                                double strategy_realized_pnl = 0.0);
    // 원장 CSV에 덧붙일 한 줄을 줄 세우는 큐에 놓고 곧바로 돌아온다(디스크는 전담 스레드가 기다린다).
    //  시각 열은 여기서 박는다 — 쓰기 스레드가 언제 쓰든 행의 시각은 주문 스레드가 지나간 그 순간이다.
    //  write_trade_row·record_reconcile이 줄을 만들어 여기로 보낸다. [why D-094] [why D-124]
    void        append_trade_line(const std::string& line);
    // trade_file_을 그 날짜 파일로 (재)연다 — 없으면 헤더를 쓰고, 옛 헤더면 열을 맞춰 한 번
    //  재작성한다. 호출자는 io_mutex_를 보유해야 한다.
    void        open_trade_file_locked(const std::string& date);
    // 원장 CSV 시각 열 — 날짜 파일명(YYYYMMDD)과 행 시각("YYYY-MM-DD HH:MM:SS")을 같이 만든다. KST 고정.
    static void trade_row_timestamp(std::string& date, std::string& stamp);

    // ── MM-1: 주문 생명주기 라우팅 ────────────────────────────────────────
    ManagedOrder new_route(const OrderSignal& signal);     // 기존 신규 주문 경로
    [[nodiscard]] ManagedOrder cancel_route(const OrderSignal& signal);  // action=CANCEL
    [[nodiscard]] ManagedOrder replace_route(const OrderSignal& signal); // action=REPLACE(정정)
    // SELL이 40240000(주문가능분 없음)으로 막히면: 그 종목의 미체결 예약매도를 조회·취소하고
    //  시장가 매도를 1회 재시도한다(장중 자가 청산 정리). 성공 시 kis_order_no 채운 OrderAck,
    //  예약 없음/취소 실패 시 빈 acknowledgement. 이전 세션·수동 예약이 보유수량을 묶은 경우를 해소.
    //  취소한 예약이 이번 세션 주문이면 history_를 CANCELLED로 닫고 게이트 선점을 푼다(C-2).
    //  reference/intent_taken: 재매도도 원장에 INTENT를 적은 뒤에만 나간다. 이미 적었으면(본 경로가 먼저 보낸 뒤
    //  40240000으로 돌아온 경우) 다시 적지 않는다. [why D-113]
    [[nodiscard]] OrderAck reconcile_blocked_sell(const OrderSignal& signal, const OrderGate::OrderRef& reference,
                                                  bool& intent_taken);

    // KIS 전송 직전 원장 기록 — 선점을 잡고 INTENT를 적는다. 거짓이면 파일에 안 적혀 주문을 보내지 않는다.
    [[nodiscard]] bool take_intent(const OrderSignal& signal, const OrderGate::OrderRef& reference);
    // 주문 번호로 아직 살아있는(ACCEPTED, 미체결 잔량>0) 주문을 찾는다. 색인 한 번 — 이력을 훑지 않는다.
    // 호출자는 반드시 history_mutex_를 보유해야 한다. 반환 포인터는 lock 보유 동안만 유효.
    ManagedOrder* find_live_by_client_number(uint64_t client_order_number);

    // ── 이력 색인 (history_mutex_ 아래) ─────────────────────────────────────
    //  history_는 deque라 앞을 잘라내면 위치가 밀린다. 항목마다 이력 순번(맨 앞이 history_base_)을 매기고
    //  색인은 순번을 든다 — 잘라내도 순번은 그대로라 색인 값이 안 죽는다. 같은 키가 다시 오면 최신 항목이 이긴다.
    void          push_history_locked(ManagedOrder managed_order);   // 뒤에 넣고 두 색인에 등록
    void          pop_history_front_locked();                        // 앞을 빼고 그 항목의 색인을 지운다
    ManagedOrder* history_at_locked(uint64_t history_sequence);
    ManagedOrder* find_by_order_number_locked(uint64_t kis_order_number);   // ODNO 정수
    // 연결된 주문에 체결 incoming_quantity주를 넣고 원장·원장 CSV·미결 파일·발행까지 한다. 잔량을 넘으면 잔량으로
    //  자른다. note가 비지 않으면 원장 CSV의 사유 칸에 적는다. [inv] lock은 history_mutex_를 쥔 채로 받고, 여기서 푼다.
    void apply_linked_fill(std::unique_lock<std::mutex>& lock, ManagedOrder& managed_order,
                           const FillNotification& fill_notification, int incoming_quantity, std::string_view note);
    // 조회로 되찾은 몫에 드는 늦은 통보면 그 수량을 몫에서 깎고 돌려준다(원장에 다시 넣지 않을 수량).
    //  [inv] history_mutex_를 쥐고 부른다.
    [[nodiscard]] static int consume_recovered_credit_locked(ManagedOrder& managed_order,
                                                             const FillNotification& fill_notification);
    void fill_recovery_loop(std::stop_token stop_token);
    ManagedOrder* find_by_client_number_locked(uint64_t client_order_number);
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
    // 접수된 주문 한 건을 덧붙이기 큐에 넣는다. 파일은 쓰기 스레드가 상주 핸들 order_reason_file_로
    //  쓰고, 날짜가 바뀌면 다시 연다. record()가 락 밖에서 부른다. [why D-094] [why D-124]
    void append_order_reason(const ManagedOrder& managed_order);
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
    //  [lock-order] in_flight_mutex_ → history_mutex_ → 원장 positions_mutex_(정리 한 곳). 거는 쪽은
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

    std::mutex                    in_flight_mutex_;
    std::vector<symbol::SymbolId> in_flight_symbols_; // 같은 종목이 두 번 들 수 있다(신규 안의 청산 재매도)

    mutable std::mutex       history_mutex_;
    std::deque<ManagedOrder> history_;
    uint64_t                 history_base_ = 0; // history_.front()의 이력 순번
    // ODNO 정수 → 이력 순번, 주문 번호 → 이력 순번 (history_mutex_로 보호). 체결통보·취소·정정이 이력을
    //  훑는 대신 여기서 한 번 찾는다. [why D-112]
    std::unordered_map<uint64_t, uint64_t> slot_by_order_number_;
    std::unordered_map<uint64_t, uint64_t> slot_by_client_number_;

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
    // 재연결 뒤 재전송으로 보고 원장에 넣지 않은 통보 수. 수량은 주기 잔고 대조가 맞춘다.
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
        int confirmed_quantity = 0; // 지금까지 원장에 반영한 수량
    };

    // 주문 단위 키(거래일+ODNO, 체결 건별 칸은 0) → 그 주문의 누적 상태 (history_mutex_로 보호).
    //  ODNO는 영업일마다 재사용되므로 거래일을 키에 같이 담는다. 일별 리셋으로 비운다.
    std::unordered_map<FillKey, UnlinkedOrder, FillKeyHash> unlinked_orders_;
    // ODNO 정수 → 이전 세션이 남긴 주문 사유 (history_mutex_로 보호). 파일에서 한 번 읽고,
    //  되살린 주문은 지운다(같은 ODNO를 두 번 되살리지 않게).
    std::unordered_map<uint64_t, OrderReason> order_reasons_;
    bool order_reasons_loaded_ = false;
    // 취소가 "취소 대상 없음"으로 되돌아온 종목 → 그 시각 (history_mutex_로 보호, 종목 id 인덱스, 0=없음).
    //  전략은 취소 결과를 보지 못한 채 재구성 주문을 이어 내므로, 원주문이 이미 체결돼
    //  있었으면 대체 주문이 그대로 중복 매수가 된다(09-09 033790·108490 5건).
    //  다음 재구성 주기까지 그 종목의 신규 주문을 짧게 막는다.
    std::vector<std::chrono::steady_clock::time_point> cancel_miss_;
    // 부속 파일 스냅샷 번호. history_mutex_ 아래에서 올리고, io_mutex_ 아래에서 "마지막으로 쓴 번호"와 비교한다.
    uint64_t open_orders_sequence_         = 0;
    // 이전 세션에서 넘어온 미체결 줄(kis_order_no|orgno|ticker|side|remaining). 취소 스레드가 한 건씩 정리한다.
    //  스냅샷이 history_만 보면 취소를 못 마친 줄(한도 거부·종료 중단·크래시)이 이번 세션 첫 기록에서
    //  파일에서 사라지고 다음 재기동은 그 주문을 모른다. 정리될 때까지 스냅샷에 같이 실린다.
    //  [lock-order] history_mutex_ → carry_mutex_. 기동 취소 스레드는 carry_mutex_를 단독으로만 잡는다.
    std::vector<std::array<std::string, 5>> carry_rows_;
    mutable std::mutex                      carry_mutex_;
    std::mutex io_mutex_;                       // 원장 CSV·부속 파일 쓰기 직렬화
    uint64_t open_orders_written_sequence_ = 0;    // io_mutex_ 보호
    // 부속 파일 쓰기 대기함 — 한 칸짜리, 최신이 이긴다. sequence 0은 "대기 중인 것 없음"(스냅샷 번호는 1부터).
    //  [lock-order] history_mutex_·open_orders_outbox_mutex_·io_mutex_ 셋은 겹쳐 잡지 않는다 — 스냅샷은
    //   history_mutex_ 안에서 뜨고 대기함에는 그 락을 푼 뒤 넣으며, 쓰기 스레드는 대기함 락을 푼 뒤 io_mutex_를 잡는다.
    std::mutex                  open_orders_outbox_mutex_;
    std::condition_variable_any open_orders_outbox_signal_;
    std::string                 open_orders_pending_body_;
    uint64_t                    open_orders_pending_sequence_ = 0;
    // 상주 파일 핸들(io_mutex_ 보호) — 주문마다 열고 닫는 대신 날짜가 바뀔 때만 다시 연다.
    //  LatencyTrace.h의 opened_ 패턴과 같다. [why D-094]
    std::ofstream trade_file_;
    std::string   trade_file_date_;
    std::ofstream order_reason_file_;
    std::string   order_reason_file_date_;

    // 원장 CSV·사유 덧붙이기 큐 — 줄을 세운다(미결주문 파일과 달리 중간 것도 다 남아야 한다).
    //  [lock-order] io_mutex_ → append_outbox_mutex_. 넣는 쪽은 append_outbox_mutex_만 잡는다.
    //   on_fill은 세션 첫 체결 때 history_mutex_를 쥔 채 큐를 비운다(history_mutex_ → io_mutex_ → append_outbox_mutex_).
    std::mutex                  append_outbox_mutex_;
    std::condition_variable_any append_outbox_signal_;
    std::deque<PendingLine>     append_outbox_;
    // 큐가 이만큼 밀리면 넣은 쪽이 직접 비운다 — 디스크가 못 따라갈 때 메모리만 늘지 않게.
    //  2,700종목 부하가 초당 300건대를 접수하므로 한도를 넘는 것은 디스크가 몇 분 멈춘 상황뿐이다.
    static constexpr size_t     kAppendOutboxLimit = 50'000;

    // 유령주문 취소 스레드. 종료가 몇 분씩 걸리지 않도록 매 건 전에 stop_token을 본다.
    std::jthread       stale_threshold_;

    // 부속 파일 쓰기 스레드. 멤버 기본값으로 바로 뜬다.
    //  [inv] 이 줄은 대기함 멤버(open_orders_outbox_*)·io_mutex_보다 반드시 뒤에 있어야 한다 —
    //   멤버는 선언 순서대로 지어지고, 스레드는 지어지는 즉시 그 셋을 만진다.
    std::jthread       open_orders_writer_{[this](std::stop_token stop_token)
    {
        open_orders_writer_loop(stop_token);
    }};

    // 원장 CSV·사유 쓰기 스레드. 멤버 기본값으로 바로 뜬다.
    //  [inv] 이 줄은 큐 멤버(append_outbox_*)·파일 핸들·io_mutex_보다 반드시 뒤에 있어야 한다 —
    //   멤버는 선언 순서대로 지어지고, 스레드는 지어지는 즉시 그것들을 만진다.
    std::jthread       append_writer_{[this](std::stop_token stop_token)
    {
        append_writer_loop(stop_token);
    }};

    // 전송 타임아웃 뒤 되묻기 스레드. 주문 스레드를 막지 않도록 한 번에 한 건만 돌리고,
    //  돌고 있으면 새 요청은 버린다(다음 타임아웃이나 다음 기동이 다시 잡는다).
    std::jthread       transport_reconcile_;
    std::atomic<bool>  reconcile_busy_{false};

    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> accepted_count_{0};
    std::atomic<uint64_t> rejected_count_{0};
    std::atomic<uint64_t> kis_calls_{0};      // [inv] kis_ 주문 호출 7곳(신규 2·취소 4·정정 1)과 되묻기 미체결조회 1곳, 호출 직전에만 올린다

    // 놓친 체결 되찾기 요청. on_session_resumed가 올리고 복구 스레드가 따라잡는다(fill_recovery_mutex_로 보호).
    std::mutex                  fill_recovery_mutex_;
    std::condition_variable_any fill_recovery_wake_;
    uint64_t                    fill_recovery_requests_ = 0;
    // 복구 스레드. 위 멤버를 쓰므로 그 뒤에 선언한다 — 소멸자가 맨 먼저 세운다.
    std::jthread fill_recovery_{[this](std::stop_token stop_token)
    {
        fill_recovery_loop(stop_token);
    }};
};
