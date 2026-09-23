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
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderRouter  —  주문 전처리·중계(FEP, Front-End Processor) 역할의 주문 라우팅 레이어
//
//  흐름:
//    OrderSignal
//       │
//       ▼
//    OrderGate::check()   — Kill switch / Rate / 포지션 / 중복 검증
//       │ PASS
//       ▼
//    KisClient::submit_order_acknowledgement() — KIS API 전송 → ODNO·조직번호 수신(실패면 error_code)
//       │
//       ├─ 성공 → ACCEPTED,  ZMQ publish_order(ok=true)
//       └─ 실패 → REJECTED,  ZMQ publish_order(ok=false)
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
        : gate_(gate), kis_(kis), config_(config), zmq_(zmq), unlinked_strategy_index_(gate.strategy_index_of("UNLINKED")) {}
#else
    OrderRouter(OrderGate& gate, IOrderExecutor& kis,
                OrderRouterConfig config = OrderRouterConfig())
        : gate_(gate), kis_(kis), config_(config), unlinked_strategy_index_(gate.strategy_index_of("UNLINKED")) {}
#endif

    // ── 주문 제출 — 검증 → KIS 전송 → 상태 기록 ─────────────────────────
    [[nodiscard]] ManagedOrder submit(const OrderSignal& signal);

    // ── 체결통보 수신 — ODNO로 이력 조회 후 FILLED 상태 갱신 ────────────
    void on_fill(const FillNotification& fill_notification);

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
    //  모의투자는 미체결조회 TR이 없어(§reconcile_blocked_sell) 되살릴 근거가 없다 — 접수된 것만
    //   되살리고 나머지는 푼다. [inv] 스레드 시작 전, 잔고 시드 뒤에 한 번.
    struct AdoptResult
    {
        int restored = 0; // history_에 되살린 미체결 주문
        int released = 0; // 선점만 푼 주문(KIS가 모르는 것)
    };
    AdoptResult adopt_open_intents(const std::vector<OrderGate::OpenIntent>& intents);

    // 살아있는 주문이 없는데 게이트에 남은 선점을 푼다. 선점은 접수 때만 생기므로
    //  라우터 이력이 정본이다. 모의투자는 미체결조회(inquire-psbl-rvsecncl)를 지원하지 않아
    //  브로커에 물어볼 수가 없고, 통보를 한 번 놓치면 선점이 슬롯을 물고 하루를 간다.
    //  주기 호출(잔고 대조와 같은 사이클) 전제. 반환값은 푼 종목 수.
    int sweep_stale_reservations();

    // ── 일별 리셋 (장 시작) — 중복방지 키(seen_fills_) 정리 ───────────────────
    void reset_daily();

    // ── 통계 조회 ─────────────────────────────────────────────────────────
    struct Stats
    {
        uint64_t total    = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
    };
    Stats statistics() const;

    // KIS 주문 API(신규·취소·정정)를 실제로 부른 누적 횟수. 주문 스레드가 submit 전후 값을 비교해
    //  발주 간격(order_min_interval_ms)을 실제 호출 뒤에만 건다 — 게이트·ENTRY_HALT의 로컬 거부는
    //  KIS에 안 나가는데도 같은 간격을 먹어 재기동 직후 거부 62건이 31초를 삼켰다(09-10 12:58).
    uint64_t kis_calls() const
    {
        return kis_calls_.load(std::memory_order_relaxed);
    }

    // ── 최근 N건 이력 조회 ────────────────────────────────────────────────
    std::vector<ManagedOrder> recent(int count = 20) const;

    // ── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ─────────────────
    //  재기동하면 history_가 비어 이전 세션 주문의 ODNO를 잊는다. 모의투자는
    //  정정취소가능조회 TR이 없어 브로커에 미체결을 물어볼 수도 없다. 그래서
    //  접수 때마다 살아있는 주문을 부속 파일에 적어 두고 여기서 읽어 취소한다.
    //  방치하면 오전 분할 매수 지정가가 하루 종일 걸려 있으면서 (1) 주문가능현금을
    //  묶고(40250000 도배) (2) 청산 관리가 청산한 직후 되사서 원치 않는 재진입을 만든다
    //  (2026-09-08 047050: 13:07 청산 → 오전 ODNO 22814가 13:11 체결).
    //  네트워크 왕복이 건당 3~5초라 85건이면 5분이다. 기동을 그만큼 막으면 장중
    //  재기동이 사실상 불가능해지므로 취소는 별도 스레드로 돌린다. 부속 파일을
    //  비우는 것만 동기로 끝낸다 — 스레드가 나중에 비우면 그 사이 현재 세션이 적어 둔
    //  미체결 기록까지 같이 지워진다(그러면 다음 재기동이 오늘 주문을 잊는다).
    //  KisClient는 토큰·레이트리밋을 뮤텍스로 직렬화해 스레드 공유를 전제로 한다.
    void cancel_stale_orders_async();
    // 전송이 타임아웃 난 주문을 브로커에 되물어 맞춘다. 응답을 못 받았을 뿐 접수됐을 수 있고,
    //  그렇게 남은 주문은 엔진 장부 밖이라 보유분을 묶은 채 아무도 못 지운다. [why D-101]
    void reconcile_unknown_order_async(std::string ticker);

    // 줄 세워 둔 원장 CSV·사유 줄을 부르는 스레드에서 전부 써 버린다. 쓸 것이 없으면 아무것도 안 한다.
    //  평소에는 전담 스레드가 알아서 비우므로 부를 일이 없다 — 방금 낸 주문의 행을 곧바로 파일에서
    //  읽어 확인해야 하는 시험이 쓴다.
    void flush_file_writes();

    // 취소 스레드를 세우고 기다린다(소멸자에서 호출). 중복 호출은 무해하다.
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
    //  호출자는 hist_mtx_를 보유해야 한다. 파일 쓰기는 write_open_orders_file이 락 밖에서 한다.
    std::string snapshot_open_orders_locked() const;
    // 부속 파일 덮어쓰기(io_mutex_). sequence가 이미 쓴 것보다 오래됐으면 건너뛴다 —
    //  락 밖에서 쓰므로 스냅샷 순서와 쓰기 순서가 뒤집힐 수 있다. 실패는 매매를 막지 않는다.
    void        write_open_orders_file(const std::string& body, uint64_t sequence);
    // 스냅샷을 새로 떠서 부속 파일을 다시 쓴다(hist_mtx_를 잠깐 잡고, 쓰기는 밖에서).
    //  이전 세션 줄(carry_rows_)이 정리될 때마다 취소 스레드가 부른다.
    void        rewrite_open_orders();
    // 부속 파일 쓰기를 전담 스레드에 넘기고 곧바로 돌아온다. 대기함은 한 칸이고 최신이 이긴다 —
    //  중간 스냅샷을 읽는 쪽이 없어서다(다음 기동이 보는 것은 마지막 하나뿐). 주문 스레드가
    //  여기서 디스크를 기다리면 시퀀서 전체가 같이 선다. [why D-071]
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
    //   파일 쓰기는 io_mtx_로 직렬화한다(history_mutex_ 밖에서 호출 — 디스크가 원장 락을 잡지 않게).
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
    ManagedOrder* find_by_client_number_locked(uint64_t client_order_number);
    // 신호의 종목 id — 배선이 빠진 경로(테스트·수동)만 문자열로 한 번 채운다.
    symbol::SymbolId symbol_of(const OrderSignal& signal);

    // ── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
    //  history_는 메모리에만 있어 재기동하면 이전 세션 주문의 ODNO를 잊는다. 그 주문이
    //  나중에 체결되면 전략도 사유도 모르는 미매핑 체결로 들어가고, 주문수량을 모르니
    //  잔량 클램프도 걸 수 없다. 접수 시점에 한 줄씩 파일로 남겨 재기동 뒤에도 같은
    //  정보를 복원한다. 파일은 거래일별 append 전용이다 — open_orders.txt는 기동 때
    //  통째로 지워지므로 거기에 얹으면 안 된다.
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
    // 접수된 주문 한 건을 기록 파일에 덧붙인다(io_mutex_). 상주 핸들 order_reason_file_이
    //  오늘 날짜와 맞지 않으면 다시 연다. record()가 락 밖에서 부른다. [why D-094]
    void append_order_reason(const ManagedOrder& managed_order);
    // 오늘자 기록 파일을 읽어 order_reasons_를 채운다. 첫 체결통보 때 1회.
    //  호출자는 hist_mtx_를 보유해야 한다.
    void load_order_reasons_locked();


    OrderGate&       gate_;
    IOrderExecutor&  kis_;
    OrderRouterConfig config_;
#ifdef HAS_ZMQ
    ZmqBridge*       zmq_ = nullptr;
#endif
    // 미매핑 체결("UNLINKED")의 전략 번호 — 생성자에서 한 번 받는다. [why D-112]
    const strategy_table::StrategyId unlinked_strategy_index_;

    mutable std::mutex       history_mutex_;
    std::deque<ManagedOrder> history_;
    uint64_t                 history_base_ = 0; // history_.front()의 이력 순번
    // ODNO 정수 → 이력 순번, 주문 번호 → 이력 순번 (history_mutex_로 보호). 체결통보·취소·정정이 이력을
    //  훑는 대신 여기서 한 번 찾는다. [why D-112]
    std::unordered_map<uint64_t, uint64_t> slot_by_order_number_;
    std::unordered_map<uint64_t, uint64_t> slot_by_client_number_;

    using FillKey     = fill_key::FillKey;     // 정의는 ipc/FillKey.h
    using FillKeyHash = fill_key::FillKeyHash;
    // 체결통보 키 → 그 키로 들어온 통보 횟수 (history_mutex_로 보호).
    //  같은 초·같은 수량·단가의 분할체결은 키가 겹치므로 집합이 아니라 횟수로 센다.
    //  자세한 배경은 on_fill() 주석 참고.
    std::unordered_map<FillKey, int, FillKeyHash> seen_fills_;
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
    //  [lock-order] history_mutex_ → carry_mutex_. 취소 스레드는 carry_mtx_를 단독으로만 잡는다.
    std::vector<std::array<std::string, 5>> carry_rows_;
    mutable std::mutex                      carry_mutex_;
    std::mutex io_mutex_;                       // 원장 CSV·부속 파일 쓰기 직렬화
    uint64_t open_orders_written_sequence_ = 0;    // io_mutex_ 보호
    // 부속 파일 쓰기 대기함 — 한 칸짜리, 최신이 이긴다. sequence 0은 "대기 중인 것 없음"(스냅샷 번호는 1부터).
    //  [lock-order] history_mutex_ → open_orders_outbox_mutex_ → io_mutex_. 쓰기 스레드는 뒤 둘만 잡는다.
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
    std::jthread       open_orders_writer_{[this](std::stop_token stop_token) { open_orders_writer_loop(stop_token); }};

    // 원장 CSV·사유 쓰기 스레드. 멤버 기본값으로 바로 뜬다.
    //  [inv] 이 줄은 큐 멤버(append_outbox_*)·파일 핸들·io_mutex_보다 반드시 뒤에 있어야 한다 —
    //   멤버는 선언 순서대로 지어지고, 스레드는 지어지는 즉시 그것들을 만진다.
    std::jthread       append_writer_{[this](std::stop_token stop_token) { append_writer_loop(stop_token); }};

    // 전송 타임아웃 뒤 되묻기 스레드. 주문 스레드를 막지 않도록 한 번에 한 건만 돌리고,
    //  돌고 있으면 새 요청은 버린다(다음 타임아웃이나 다음 기동이 다시 잡는다).
    std::jthread       transport_reconcile_;
    std::atomic<bool>  reconcile_busy_{false};

    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> accepted_count_{0};
    std::atomic<uint64_t> rejected_count_{0};
    std::atomic<uint64_t> kis_calls_{0};      // [inv] 6곳의 kis_ 주문 호출 직전에만 올린다
};
