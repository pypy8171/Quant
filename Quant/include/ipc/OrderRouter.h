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
    void        record(const ManagedOrder& mo);

    // ── MM-1: 주문 생명주기 라우팅 ────────────────────────────────────────
    ManagedOrder new_route(const OrderSignal& sig);     // 기존 신규 주문 경로
    ManagedOrder cancel_route(const OrderSignal& sig);  // action=CANCEL
    ManagedOrder replace_route(const OrderSignal& sig); // action=REPLACE(정정)
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
