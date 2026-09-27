// 주문 라우터의 이력과 번호 색인. 선언부 주석은 Quant/include/ipc/OrderHistory.h.
//  [inv] 모든 함수는 라우터 history_mutex_를 쥔 채 불린다.
#include "ipc/OrderHistory.h"

#include <algorithm>

void OrderHistory::push(ManagedOrder managed_order)
{
    const uint64_t history_sequence = base_ + orders_.size();

    if (managed_order.kis_order_number != 0)
    {
        slot_by_order_number_[managed_order.kis_order_number] = history_sequence;
    }

    if (managed_order.signal.client_order_number != 0)
    {
        slot_by_client_number_[managed_order.signal.client_order_number] = history_sequence;
    }

    orders_.push_back(std::move(managed_order));
}

void OrderHistory::pop_front()
{
    const ManagedOrder& front = orders_.front();
    // 같은 키를 더 새 항목이 차지했으면 그 색인은 남긴다.
    const auto erase_if_mine = [this](std::unordered_map<uint64_t, uint64_t>& index, uint64_t key)
    {
        const auto iterator = index.find(key);

        if (iterator != index.end() && iterator->second == base_)
        {
            index.erase(iterator);
        }
    };

    if (front.kis_order_number != 0)
    {
        erase_if_mine(slot_by_order_number_, front.kis_order_number);
    }

    if (front.signal.client_order_number != 0)
    {
        erase_if_mine(slot_by_client_number_, front.signal.client_order_number);
    }

    orders_.pop_front();
    ++base_;
}

void OrderHistory::evict_to(int max_history)
{
    while (static_cast<int>(orders_.size()) > max_history)
    {
        // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
        if (orders_.front().status == OrderStatus::ACCEPTED)
        {
            break;
        }

        pop_front();
    }
}

ManagedOrder* OrderHistory::at(uint64_t history_sequence)
{
    if (history_sequence < base_ || history_sequence - base_ >= orders_.size())
    {
        return nullptr;
    }

    return &orders_[history_sequence - base_];
}

ManagedOrder* OrderHistory::find_by_order_number(uint64_t kis_order_number)
{
    if (kis_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_order_number_.find(kis_order_number);
    return iterator == slot_by_order_number_.end() ? nullptr : at(iterator->second);
}

ManagedOrder* OrderHistory::find_by_client_number(uint64_t client_order_number)
{
    if (client_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_client_number_.find(client_order_number);
    return iterator == slot_by_client_number_.end() ? nullptr : at(iterator->second);
}

ManagedOrder* OrderHistory::find_live(uint64_t client_order_number)
{
    ManagedOrder* managed_order = find_by_client_number(client_order_number);

    if (managed_order && is_live(*managed_order))
    {
        return managed_order;
    }

    return nullptr;
}

std::vector<ManagedOrder> OrderHistory::recent(int count) const
{
    const int start = std::max(0, static_cast<int>(orders_.size()) - count);
    return {orders_.begin() + start, orders_.end()};
}
