#include "ipc/OrderChannel.h"

#include <algorithm>
#include <cstring>

namespace ipc
{

OrderRequest to_request(const OrderSignal& signal) noexcept
{
    OrderRequest request;
    request.sequence       = signal.sequence;
    request.sent_at_ns     = signal.signal_at_ns;
    request.symbol_id      = signal.symbol_id;
    request.strategy_index = signal.strategy_index;
    request.quantity       = signal.quantity;
    request.price          = signal.price;
    request.side           = static_cast<uint8_t>(signal.side);
    request.order_type     = static_cast<uint8_t>(signal.type);
    request.action         = static_cast<uint8_t>(signal.action);
    return request;
}

uint64_t to_order_number(std::string_view kis_order_no) noexcept
{
    uint64_t number = 0;

    for (const char character : kis_order_no)
    {
        if (character < '0' || character > '9')
        {
            return 0;
        }

        number = number * 10 + static_cast<uint64_t>(character - '0');
    }

    return number;
}

OrderResponse make_response(uint64_t sequence, OrderResult result, uint64_t kis_order_number,
                            std::string_view reason, int64_t handled_at_ns) noexcept
{
    OrderResponse response;
    response.sequence         = sequence;
    response.kis_order_number = kis_order_number;
    response.handled_at_ns    = handled_at_ns;
    response.result           = static_cast<uint8_t>(result);

    // 칸을 넘치면 자르고 항상 0으로 끝낸다 — 받는 쪽이 길이 없이 읽는다.
    const size_t length = std::min(reason.size(), kOrderReasonMax - 1);

    if (length > 0)
    {
        std::memcpy(response.reason, reason.data(), length);
    }

    response.reason[length] = '\0';
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────

PendingRequests::PendingRequests(size_t capacity) : capacity_(std::max<size_t>(capacity, 1))
{
    entries_.reserve(capacity_);
}

void PendingRequests::note_sent(uint64_t sequence, int64_t now_ns)
{
    if (sequence == 0)
    {
        return; // 순번 없는 요청은 답을 맞출 수 없다
    }

    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [sequence](const Entry& entry) { return entry.sequence == sequence; });

    if (found != entries_.end())
    {
        found->sent_at_ns = now_ns; // 재전송 — 기다린 시간을 다시 센다
        return;
    }

    if (entries_.size() >= capacity_)
    {
        // 상한을 넘겼다. 가장 오래된 것을 버린다 — 주문 쪽이 답을 못 주는 동안 여기가 자라면
        //  전략 스레드가 메모리로 벌을 받는다. 버린 수는 밖에서 본다.
        entries_.erase(entries_.begin());
        ++evicted_;
    }

    entries_.push_back(Entry{sequence, now_ns});
}

bool PendingRequests::note_response(uint64_t sequence)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [sequence](const Entry& entry) { return entry.sequence == sequence; });

    if (found == entries_.end())
    {
        return false;
    }

    entries_.erase(found);
    return true;
}

std::optional<uint64_t> PendingRequests::oldest_overdue(int64_t now_ns, int64_t timeout_ns) const
{
    const Entry* oldest = nullptr;

    for (const auto& entry : entries_)
    {
        if (now_ns - entry.sent_at_ns < timeout_ns)
        {
            continue;
        }

        if (oldest == nullptr || entry.sent_at_ns < oldest->sent_at_ns)
        {
            oldest = &entry;
        }
    }

    if (oldest == nullptr)
    {
        return std::nullopt;
    }

    return oldest->sequence;
}

// ─────────────────────────────────────────────────────────────────────────────

DuplicateFilter::DuplicateFilter(size_t window) : seen_(std::max<size_t>(window, 1), 0)
{
}

bool DuplicateFilter::accept(uint64_t sequence)
{
    if (sequence == 0)
    {
        return false;
    }

    // 흔한 길: 전략이 순번을 단조 증가로 찍으므로 새 요청은 거의 언제나 지금까지 본 것보다 크다.
    if (sequence > highest_seen_)
    {
        highest_seen_    = sequence;
        seen_[write_index_] = sequence;
        write_index_     = (write_index_ + 1) % seen_.size();
        return true;
    }

    // 뒤처진 순번이면 창 안에 있었는지 본다.
    if (std::find(seen_.begin(), seen_.end(), sequence) != seen_.end())
    {
        ++duplicates_;
        return false;
    }

    // 창보다 오래된 순번이다. 생산자가 하나라 이만큼 밀린 요청은 정상 경로에서 나오지 않는다 —
    //  판정할 근거가 없으니 거른다. 같은 순번을 두 번 내는 것보다 한 번을 놓치는 쪽이 낫다. [inv]
    ++duplicates_;
    return false;
}

} // namespace ipc
