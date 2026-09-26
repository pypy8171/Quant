// WebSocket 구독 칸 배정의 순수 부분 구현 — 선언·규칙은 Quant/include/core/WebSocketSlotPlan.h. [why D-132]
#include "core/WebSocketSlotPlan.h"

#include <algorithm>
#include <numeric>

namespace websocket_slot
{
bool is_managed(const WatchSpec& specification)
{
    return specification.market == Market::KR;
}

int channels_of(const WatchSpec& specification)
{
    return specification.trade_only ? 1 : 2;
}

int32_t priority_of(bool held, bool reserved, int32_t scan_rank)
{
    if (held)
    {
        return kHeld;
    }

    if (reserved)
    {
        return kReserved;
    }

    if (scan_rank >= 0)
    {
        return kScoreBase + scan_rank;
    }

    return kUnranked;
}

Plan plan(const std::vector<Entry>& entries, const Rules& rules)
{
    Plan result;
    int  used = 0;
    std::vector<size_t> candidates;
    std::vector<size_t> victims;

    for (size_t index = 0; index < entries.size(); ++index)
    {
        if (entries[index].on_socket)
        {
            used += entries[index].channels;
            victims.push_back(index);
        }
        else
        {
            candidates.push_back(index);
        }
    }

    // 받을 쪽은 앞선 순위부터, 내줄 쪽은 뒤진 순위부터 본다. 같은 순위는 들어온 순서를 지킨다.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&entries](size_t left, size_t right)
                     {
                         return entries[left].priority < entries[right].priority;
                     });
    std::stable_sort(victims.begin(), victims.end(),
                     [&entries](size_t left, size_t right)
                     {
                         return entries[left].priority > entries[right].priority;
                     });

    std::vector<bool> released(entries.size(), false);
    int               swaps = 0;

    for (size_t candidate : candidates)
    {
        const Entry& incoming = entries[candidate];

        if (used + incoming.channels <= rules.capacity)
        {
            result.take.push_back(candidate);
            used += incoming.channels;
            continue;
        }

        // 칸이 모자라다 — 문턱을 넘는 종목만 뒤진 순위부터 골라, 들어올 종목이 들어갈 만큼 모이면 바꾼다.
        std::vector<size_t> chosen;
        int                 freed = 0;

        for (size_t victim : victims)
        {
            if (used - freed + incoming.channels <= rules.capacity)
            {
                break;
            }

            if (released[victim] || swaps + static_cast<int>(chosen.size()) >= rules.max_swaps)
            {
                continue;
            }

            const Entry& outgoing = entries[victim];

            // 내줄 종목이 들어올 종목보다 순위 차만큼 뒤져야 한다 — 뒤진 순으로 도니 여기서 막히면 뒤도 막힌다.
            //  두 값 모두 0 이상이라 뺄셈이 넘치지 않는다.
            if (outgoing.priority - incoming.priority < rules.rank_gap)
            {
                break;
            }

            if (outgoing.priority <= kProtected || outgoing.held_sec < rules.min_hold_sec)
            {
                continue;
            }

            chosen.push_back(victim);
            freed += outgoing.channels;
        }

        if (used - freed + incoming.channels > rules.capacity)
        {
            continue; // 모자라면 아무것도 내주지 않는다 — 반만 비우면 칸만 잃는다
        }

        for (size_t victim : chosen)
        {
            released[victim] = true;
            result.release.push_back(victim);
        }

        swaps += static_cast<int>(chosen.size());
        used  += incoming.channels - freed;
        result.take.push_back(candidate);
    }

    return result;
}

} // namespace websocket_slot
