#pragma once
#include "core/Types.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

// 주문의 미체결 잔량. 음수가 되면 0이다.
inline int outstanding_of(const ManagedOrder& managed_order)
{
    return std::max(0, managed_order.signal.quantity - managed_order.confirmed_quantity);
}

// 아직 살아 있는 주문인가 — 접수됐고(ACCEPTED, 부분체결도 여기 든다) 미체결 잔량이 남았다.
inline bool is_live(const ManagedOrder& managed_order)
{
    return managed_order.status == OrderStatus::ACCEPTED && managed_order.confirmed_quantity < managed_order.signal.quantity;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderHistory  —  주문 라우터의 이번 세션 주문 이력과 번호 색인
//
//  이력은 deque라 앞을 잘라내면 위치가 밀린다. 항목마다 이력 순번(맨 앞이 base_)을 매기고
//  색인은 순번을 든다 — 잘라내도 순번은 그대로라 색인 값이 안 죽는다. 같은 키가 다시 오면 최신 항목이 이긴다.
//  체결통보·취소·정정이 이력을 훑는 대신 색인에서 한 번 찾는다. [why D-112]
//
//  [inv] 자체 락이 없다. 모든 멤버 함수는 라우터 history_mutex_를 쥔 채 부른다. 돌려준 포인터·참조는
//   그 락을 쥐고 있는 동안만 유효하다 — 락을 풀면 축출(evict_to)로 항목이 사라질 수 있다.
//  [lock-order] 라우터 in_flight_mutex_ → history_mutex_ → carry_mutex_ / 기록기 io_mutex_. 이 클래스는 다른 락을 잡지 않는다.
// ─────────────────────────────────────────────────────────────────────────────
class OrderHistory
{
public:
    using Container = std::deque<ManagedOrder>;

    // 뒤에 넣고 두 색인(ODNO 정수·주문 번호)에 등록한다.
    void push(ManagedOrder managed_order);
    // 앞을 빼고 그 항목의 색인을 지운다. 같은 키를 더 새 항목이 차지했으면 그 색인은 남긴다.
    void pop_front();
    // max_history를 넘는 동안 앞에서부터 뺀다. 맨 앞이 ACCEPTED(체결 대기 중)이면 멈춘다 —
    //  ODNO 매핑이 끊기면 체결통보를 놓친다.
    void evict_to(int max_history);

    // 이력 순번으로 항목을 찾는다. 이미 빠졌거나 아직 없으면 nullptr.
    ManagedOrder* at(uint64_t history_sequence);
    // ODNO 정수로 찾는다. 0이면 nullptr.
    ManagedOrder* find_by_order_number(uint64_t kis_order_number);
    // 주문 번호로 찾는다. 0이면 nullptr.
    ManagedOrder* find_by_client_number(uint64_t client_order_number);
    // 주문 번호로 아직 살아있는(ACCEPTED, 미체결 잔량>0) 주문을 찾는다. 부분체결도 status는 ACCEPTED다.
    //  FILLED/CANCELLED/REJECTED는 취소·정정 대상이 아니라 nullptr.
    ManagedOrder* find_live(uint64_t client_order_number);
    // 최근 count건의 사본. 호출자는 락 밖에서 읽는다.
    std::vector<ManagedOrder> recent(int count) const;

    // 훑기 — 선점 정리·대조·되찾기처럼 색인으로 못 찾는 조건을 볼 때 쓴다.
    Container::iterator begin() noexcept
    {
        return orders_.begin();
    }

    Container::iterator end() noexcept
    {
        return orders_.end();
    }

    Container::const_iterator begin() const noexcept
    {
        return orders_.begin();
    }

    Container::const_iterator end() const noexcept
    {
        return orders_.end();
    }

    bool empty() const noexcept
    {
        return orders_.empty();
    }

private:
    Container orders_;
    uint64_t  base_ = 0; // orders_.front()의 이력 순번
    // ODNO 정수 → 이력 순번, 주문 번호 → 이력 순번.
    std::unordered_map<uint64_t, uint64_t> slot_by_order_number_;
    std::unordered_map<uint64_t, uint64_t> slot_by_client_number_;
};
