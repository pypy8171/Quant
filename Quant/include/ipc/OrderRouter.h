#pragma once
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "api/IOrderExecutor.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include <array>
#include <atomic>
#include <deque>
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
//    KisClient::submit_order() — KIS API 전송 → ODNO 수신
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
                OrderRouterConfig cfg = OrderRouterConfig())
        : gate_(gate), kis_(kis), cfg_(cfg), zmq_(zmq) {}
#else
    OrderRouter(OrderGate& gate, IOrderExecutor& kis,
                OrderRouterConfig cfg = OrderRouterConfig())
        : gate_(gate), kis_(kis), cfg_(cfg) {}
#endif

    // ── 주문 제출 — 검증 → KIS 전송 → 상태 기록 ─────────────────────────
    ManagedOrder submit(const OrderSignal& sig);

    // ── 체결통보 수신 — ODNO로 이력 조회 후 FILLED 상태 갱신 ────────────
    void on_fill(const FillNotification& fn);

    // ── 잔고 대조 기록 (C-2) ────────────────────────────────────────────────
    //  Engine이 브로커 잔고와 원장을 비교한 결과를 원장 CSV에 `RECONCILE` 행으로 남긴다.
    //  order_qty/order_price=원장 수량·평단, fill_qty/fill_price=브로커 수량·평단, status=action,
    //  reason에 그 종목의 살아있는 주문 수(live_orders)를 적는다 — 미체결이 있으면 불일치가
    //  체결 지연일 수 있어 사람이 어느 단계인지 가를 근거가 된다. 덮어쓰기·정리(action이 KEEP이
    //  아닌 것)는 LOG_WARN도 낸다. 원장 자체는 바꾸지 않는다(그건 OrderGate 몫).
    struct ReconcileNote
    {
        std::string ticker;
        int         ledger_qty = 0;
        int         broker_qty = 0;
        double      ledger_avg = 0.0;
        double      broker_avg = 0.0;
        std::string action;      // "OVERWRITE" | "PRUNE" | "KEEP"
        std::string note;        // 자유 문구(대조 모드 등). 콤마는 공백으로 바뀐다.
    };
    void record_reconcile(const ReconcileNote& n);

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
    Stats stats() const;

    // KIS 주문 API(신규·취소·정정)를 실제로 부른 누적 횟수. 주문 스레드가 submit 전후 값을 비교해
    //  발주 간격(order_min_interval_ms)을 실제 호출 뒤에만 건다 — 게이트·ENTRY_HALT의 로컬 거부는
    //  KIS에 안 나가는데도 같은 간격을 먹어 재기동 직후 거부 62건이 31초를 삼켰다(09-10 12:58).
    uint64_t kis_calls() const
    {
        return kis_calls_.load(std::memory_order_relaxed);
    }

    // ── 최근 N건 이력 조회 ────────────────────────────────────────────────
    std::vector<ManagedOrder> recent(int n = 20) const;

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

    // 취소 스레드를 세우고 기다린다(소멸자에서 호출). 중복 호출은 무해하다.
    ~OrderRouter();

private:
    std::string next_id();
    // 직전 KIS 주문/취소/정정 오류코드를 " [코드]" 꼬리표로 만든다(EGW00201 재시도 판별용). 없으면 "".
    std::string kis_err_suffix() const;
    void        record(const ManagedOrder& mo);
    // 살아있는(ACCEPTED·미체결 잔량>0) 주문 목록을 부속 파일 본문 문자열로 만든다.
    //  호출자는 hist_mtx_를 보유해야 한다. 파일 쓰기는 write_open_orders_file이 락 밖에서 한다.
    std::string snapshot_open_orders_locked() const;
    // 부속 파일 덮어쓰기(io_mtx_). seq가 이미 쓴 것보다 오래됐으면 건너뛴다 —
    //  락 밖에서 쓰므로 스냅샷 순서와 쓰기 순서가 뒤집힐 수 있다. 실패는 매매를 막지 않는다.
    void        write_open_orders_file(const std::string& body, uint64_t seq);
    // 스냅샷을 새로 떠서 부속 파일을 다시 쓴다(hist_mtx_를 잠깐 잡고, 쓰기는 밖에서).
    //  이전 세션 줄(carry_rows_)이 정리될 때마다 취소 스레드가 부른다.
    void        rewrite_open_orders();
    // 거래 원장 CSV 적재 — 주문/체결을 logs/trades_YYYYMMDD.csv 에 한 줄씩 영속화.
    //   event가 빈 문자열이면 mo.status를 event로 사용(접수/거부/취소). 체결은 "FILL".
    //   파일 쓰기는 io_mtx_로 직렬화한다(hist_mtx_ 밖에서 호출 — 디스크가 원장 락을 잡지 않게).
    //   realized_pnl은 매도 체결의 실현손익(수수료·세금 차감 후). 그 외 행은 빈 칸으로 남긴다.
    void        write_trade_row(const std::string& event, const ManagedOrder& mo,
                                int fill_qty, double fill_price,
                                double realized_pnl = 0.0);
    // 원장 CSV에 한 줄을 덧붙인다(io_mtx_). 파일이 없으면 헤더를 쓰고, 옛 헤더면 열을 맞춰
    //  한 번 재작성한다. write_trade_row·record_reconcile이 줄을 만들어 여기로 보낸다.
    void        append_trade_line(const std::string& line);
    // 원장 CSV 시각 열 — 날짜 파일명(YYYYMMDD)과 행 시각("YYYY-MM-DD HH:MM:SS")을 같이 만든다.
    static void trade_row_timestamp(char (&dbuf)[9], char (&tbuf)[20]);

    // ── MM-1: 주문 생명주기 라우팅 ────────────────────────────────────────
    ManagedOrder new_route(const OrderSignal& sig);     // 기존 신규 주문 경로
    ManagedOrder cancel_route(const OrderSignal& sig);  // action=CANCEL
    ManagedOrder replace_route(const OrderSignal& sig); // action=REPLACE(정정)
    // SELL이 40240000(주문가능분 없음)으로 막히면: 그 종목의 미체결 예약매도를 조회·취소하고
    //  시장가 매도를 1회 재시도한다(장중 자가 청산 정리). 성공 시 odno 채운 OrderAck,
    //  예약 없음/취소 실패 시 빈 ack. 이전 세션·수동 예약이 보유수량을 묶은 경우를 해소.
    //  취소한 예약이 이번 세션 주문이면 history_를 CANCELLED로 닫고 게이트 선점을 푼다(C-2).
    OrderAck reconcile_blocked_sell(const OrderSignal& sig);
    // client_oid로 아직 살아있는(ACCEPTED, 미체결 잔량>0) 주문을 history_에서 찾는다.
    // 호출자는 반드시 hist_mtx_를 보유해야 한다. 반환 포인터는 lock 보유 동안만 유효.
    ManagedOrder* find_live_by_oid(const std::string& client_oid);

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
        double      ref_price = 0.0;
    };
    // 접수된 주문 한 건을 기록 파일에 덧붙인다(io_mtx_). record()가 락 밖에서 부른다.
    void append_order_reason(const ManagedOrder& mo);
    // 오늘자 기록 파일을 읽어 order_reasons_를 채운다. 첫 체결통보 때 1회.
    //  호출자는 hist_mtx_를 보유해야 한다.
    void load_order_reasons_locked();


    OrderGate&       gate_;
    IOrderExecutor&  kis_;
    OrderRouterConfig cfg_;
#ifdef HAS_ZMQ
    ZmqBridge*       zmq_ = nullptr;
#endif

    mutable std::mutex       hist_mtx_;
    std::deque<ManagedOrder> history_;
    // 체결통보 키 → 그 키로 들어온 통보 횟수 (hist_mtx_로 보호).
    //  같은 초·같은 수량·단가의 분할체결은 키가 겹치므로 집합이 아니라 횟수로 센다.
    //  자세한 배경은 on_fill() 주석 참고.
    std::unordered_map<std::string, int> seen_fills_;
    // 미매핑(ORPHAN) 체결로 이미 반영한 키 (hist_mtx_로 보호). 미연결 주문은 주문수량을 모르니
    //  잔량 클램프가 없어 같은 통보의 재전송을 이 키로만 막는다.
    std::unordered_set<std::string> orphan_fill_keys_;
    // ODNO → 이전 세션이 남긴 주문 사유 (hist_mtx_로 보호). 파일에서 한 번 읽고,
    //  되살린 주문은 지운다(같은 ODNO를 두 번 되살리지 않게).
    std::unordered_map<std::string, OrderReason> order_reasons_;
    bool order_reasons_loaded_ = false;
    // MM-1: client_oid → order_id 존재 힌트 (hist_mtx_로 보호). 실제 ManagedOrder는
    //   history_ 스캔으로 해석(deque 요소는 pop_front로 소멸 가능 → 안정 핸들 아님).
    std::unordered_map<std::string, std::string> oid_index_;
    // 취소가 "취소 대상 없음"으로 되돌아온 종목 → 그 시각 (hist_mtx_로 보호).
    //  전략은 취소 결과를 보지 못한 채 재구성 주문을 이어 내므로, 원주문이 이미 체결돼
    //  있었으면 대체 주문이 그대로 중복 매수가 된다(09-09 033790·108490 5건).
    //  다음 재구성 주기까지 그 종목의 신규 주문을 짧게 막는다.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> cancel_miss_;
    // 부속 파일 스냅샷 번호. hist_mtx_ 아래에서 올리고, io_mtx_ 아래에서 "마지막으로 쓴 번호"와 비교한다.
    uint64_t open_orders_seq_         = 0;
    // 이전 세션에서 넘어온 미체결 줄(odno|orgno|ticker|side|remaining). 취소 스레드가 한 건씩 정리한다.
    //  스냅샷이 history_만 보면 취소를 못 마친 줄(한도 거부·종료 중단·크래시)이 이번 세션 첫 기록에서
    //  파일에서 사라지고 다음 재기동은 그 주문을 모른다. 정리될 때까지 스냅샷에 같이 실린다.
    //  [lock-order] hist_mtx_ → carry_mtx_. 취소 스레드는 carry_mtx_를 단독으로만 잡는다.
    std::vector<std::array<std::string, 5>> carry_rows_;
    mutable std::mutex                      carry_mtx_;
    std::mutex io_mtx_;                       // 원장 CSV·부속 파일 쓰기 직렬화
    uint64_t open_orders_written_seq_ = 0;    // io_mtx_ 보호

    // 유령주문 취소 스레드. 종료가 몇 분씩 걸리지 않도록 매 건 전에 정지 플래그를 본다.
    std::thread        stale_thr_;
    std::atomic<bool>  stale_stop_{false};

    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> accepted_count_{0};
    std::atomic<uint64_t> rejected_count_{0};
    std::atomic<uint64_t> kis_calls_{0};      // [inv] 6곳의 kis_ 주문 호출 직전에만 올린다
};
