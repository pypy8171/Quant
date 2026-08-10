#pragma once
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "api/IOrderExecutor.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OrderRouter  —  FEP 역할의 주문 라우팅 레이어
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

    // ── 일별 리셋 (장 시작) — 멱등키(seen_fills_) 정리 ───────────────────
    void reset_daily();

    // ── 통계 조회 ─────────────────────────────────────────────────────────
    struct Stats
    {
        uint64_t total    = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
    };
    Stats stats() const;

    // ── 최근 N건 이력 조회 ────────────────────────────────────────────────
    std::vector<ManagedOrder> recent(int n = 20) const;

private:
    std::string next_id();
    // 직전 KIS 주문/취소/정정 오류코드를 " [코드]" 꼬리표로 만든다(EGW00201 재시도 판별용). 없으면 "".
    std::string kis_err_suffix() const;
    void        record(const ManagedOrder& mo);
    // 거래 원장 CSV 적재 — 주문/체결을 logs/trades_YYYYMMDD.csv 에 한 줄씩 영속화.
    //   event 빈 문자열이면 mo.status 문자열을 event로 사용(접수/거부/취소). 체결은 "FILL".
    //   호출자(record·on_fill)가 hist_mtx_ 보유 상태라 파일 쓰기가 직렬화된다.
    void        write_trade_row(const std::string& event, const ManagedOrder& mo,
                                int fill_qty, double fill_price);

    // ── MM-1: 주문 생명주기 라우팅 ────────────────────────────────────────
    ManagedOrder new_route(const OrderSignal& sig);     // 기존 신규 주문 경로
    ManagedOrder cancel_route(const OrderSignal& sig);  // action=CANCEL
    ManagedOrder replace_route(const OrderSignal& sig); // action=REPLACE(정정)
    // SELL이 40240000(주문가능분 없음)으로 막히면: 그 종목의 미체결 예약매도를 조회·취소하고
    //  시장가 매도를 1회 재시도한다(장중 자가 청산 정리). 성공 시 odno 채워진 OrderAck,
    //  해당 예약 없음/취소 실패 시 빈 ack. 이전 세션·수동 예약이 보유수량을 묶은 경우를 해소.
    OrderAck reconcile_blocked_sell(const OrderSignal& sig);
    // client_oid로 아직 살아있는(ACCEPTED, 미체결 잔량>0) 주문을 history_에서 찾는다.
    // 호출자는 반드시 hist_mtx_를 보유해야 한다. 반환 포인터는 lock 보유 동안만 유효.
    ManagedOrder* find_live_by_oid(const std::string& client_oid);

    OrderGate&       gate_;
    IOrderExecutor&  kis_;
    OrderRouterConfig cfg_;
#ifdef HAS_ZMQ
    ZmqBridge*       zmq_ = nullptr;
#endif

    mutable std::mutex       hist_mtx_;
    std::deque<ManagedOrder> history_;
    std::unordered_set<std::string> seen_fills_; // 멱등 처리: 처리한 체결통보 키 (hist_mtx_로 보호)
    // MM-1: client_oid → order_id 존재 힌트 (hist_mtx_로 보호). 실제 ManagedOrder는
    //   history_ 스캔으로 해석한다(deque 요소는 pop_front로 소멸 가능 → 안정 핸들 아님).
    std::unordered_map<std::string, std::string> oid_index_;

    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> accepted_count_{0};
    std::atomic<uint64_t> rejected_count_{0};
};
