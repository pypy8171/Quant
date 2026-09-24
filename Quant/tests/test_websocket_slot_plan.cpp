// WebSocket 구독 칸 배정(core/WebSocketSlotPlan.h) 단위 테스트. 우선순위 매기기(보유 → 선점 → 점수 → 그 밖)와,
//  빈 칸 채우기·보유 보호·유지 시간·순위 차 문턱·한 번 교체 상한·칸 수가 다른 종목의 교체를 고정한다.
//  관련 결정: D-132.
// 빌드: cmake --build <directory> --target test_websocket_slot_plan
#include "core/WebSocketSlotPlan.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                   \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                  \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n"; \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

websocket_slot::Entry entry(int32_t priority, bool on_socket, long long held_sec = 600, int channels = 1)
{
    websocket_slot::Entry result;
    result.channels  = channels;
    result.priority  = priority;
    result.on_socket = on_socket;
    result.held_sec  = held_sec;
    return result;
}

bool contains(const std::vector<size_t>& list, size_t value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

websocket_slot::Rules rules(int capacity)
{
    websocket_slot::Rules result;
    result.capacity = capacity;
    return result;
}

int test_priority_of()
{
    CHECK(websocket_slot::priority_of(true, true, 3) == websocket_slot::kHeld);
    CHECK(websocket_slot::priority_of(false, true, 3) == websocket_slot::kReserved);
    CHECK(websocket_slot::priority_of(false, false, 3) == websocket_slot::kScoreBase + 3);
    CHECK(websocket_slot::priority_of(false, false, -1) == websocket_slot::kUnranked);
    CHECK(websocket_slot::kReserved < websocket_slot::kScoreBase);
    return 0;
}

int test_fill_free_slots()
{
    // 빈 칸 둘에 후보 셋 — 앞선 순위 둘만 받는다.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kScoreBase + 5, false), entry(websocket_slot::kHeld, false),
                                              entry(websocket_slot::kScoreBase + 1, false)};
    const auto result = websocket_slot::plan(entries, rules(2));
    CHECK(result.release.empty());
    CHECK(result.take.size() == 2 && contains(result.take, 1) && contains(result.take, 2));
    return 0;
}

int test_held_takes_slot_from_low_score()
{
    // 칸이 찬 상태에서 보유 종목이 칸 밖 — 가장 뒤진 점수 종목이 내준다.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kScoreBase + 2, true), entry(websocket_slot::kScoreBase + 80, true),
                                              entry(websocket_slot::kHeld, false)};
    const auto result = websocket_slot::plan(entries, rules(2));
    CHECK(result.release.size() == 1 && result.release[0] == 1);
    CHECK(result.take.size() == 1 && result.take[0] == 2);
    return 0;
}

int test_protected_never_released()
{
    // 칸을 쥔 것이 보유·선점뿐이면 다른 보유 종목이 와도 내주지 않는다.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kHeld, true), entry(websocket_slot::kReserved, true),
                                              entry(websocket_slot::kHeld, false)};
    const auto result = websocket_slot::plan(entries, rules(2));
    CHECK(result.release.empty() && result.take.empty());
    return 0;
}

int test_min_hold()
{
    // 받은 지 얼마 안 된 종목은 내주지 않는다 — 뒤진 순위라도.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kScoreBase + 90, true, 10), entry(websocket_slot::kHeld, false)};
    const auto result = websocket_slot::plan(entries, rules(1));
    CHECK(result.release.empty() && result.take.empty());

    // 유지 시간이 지나면 내준다.
    const std::vector<websocket_slot::Entry> later{entry(websocket_slot::kScoreBase + 90, true, 60), entry(websocket_slot::kHeld, false)};
    const auto after = websocket_slot::plan(later, rules(1));
    CHECK(after.release.size() == 1 && after.take.size() == 1);
    return 0;
}

int test_rank_gap()
{
    // 순위 차가 문턱(10)보다 작으면 바꾸지 않는다 — 경계 종목끼리 칸을 주고받지 않게.
    const std::vector<websocket_slot::Entry> near{entry(websocket_slot::kScoreBase + 15, true), entry(websocket_slot::kScoreBase + 6, false)};
    CHECK(websocket_slot::plan(near, rules(1)).take.empty());

    const std::vector<websocket_slot::Entry> far{entry(websocket_slot::kScoreBase + 16, true), entry(websocket_slot::kScoreBase + 6, false)};
    const auto result = websocket_slot::plan(far, rules(1));
    CHECK(result.release.size() == 1 && result.take.size() == 1);

    // 점수 순위가 없는 종목끼리는 바꾸지 않는다.
    const std::vector<websocket_slot::Entry> unranked{entry(websocket_slot::kUnranked, true), entry(websocket_slot::kUnranked, false)};
    CHECK(websocket_slot::plan(unranked, rules(1)).take.empty());
    return 0;
}

int test_max_swaps()
{
    // 교체 후보가 다섯이어도 한 번에 넷(max_swaps)까지만 내준다.
    std::vector<websocket_slot::Entry> entries;

    for (int index = 0; index < 5; ++index)
    {
        entries.push_back(entry(websocket_slot::kScoreBase + 50 + index, true));
    }

    for (int index = 0; index < 5; ++index)
    {
        entries.push_back(entry(websocket_slot::kScoreBase + index, false));
    }

    const auto result = websocket_slot::plan(entries, rules(5));
    CHECK(result.release.size() == 4 && result.take.size() == 4);
    // 가장 뒤진 넷(54·53·52·51)이 나가고 가장 앞선 넷(0~3)이 들어온다.
    CHECK(contains(result.release, 4) && contains(result.release, 1) && !contains(result.release, 0));
    CHECK(contains(result.take, 5) && contains(result.take, 8) && !contains(result.take, 9));
    return 0;
}

int test_channels()
{
    // 호가+체결(2칸) 종목이 들어오려면 1칸 종목 둘이 나가야 한다. 하나만 비울 수 있으면 아무것도 안 내준다.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kScoreBase + 60, true), entry(websocket_slot::kScoreBase + 70, true),
                                              entry(websocket_slot::kHeld, false, 0, 2)};
    const auto result = websocket_slot::plan(entries, rules(2));
    CHECK(result.release.size() == 2 && result.take.size() == 1 && result.take[0] == 2);

    const std::vector<websocket_slot::Entry> partial{entry(websocket_slot::kReserved, true), entry(websocket_slot::kScoreBase + 70, true),
                                              entry(websocket_slot::kHeld, false, 0, 2)};
    const auto none = websocket_slot::plan(partial, rules(2));
    CHECK(none.release.empty() && none.take.empty());

    // 빈 칸 하나 + 내준 칸 하나로 2칸 종목이 들어간다.
    const std::vector<websocket_slot::Entry> mixed{entry(websocket_slot::kScoreBase + 70, true), entry(websocket_slot::kHeld, false, 0, 2)};
    const auto combined = websocket_slot::plan(mixed, rules(2));
    CHECK(combined.release.size() == 1 && combined.take.size() == 1);
    return 0;
}

int test_released_not_reused()
{
    // 한 종목이 내준 칸을 두 후보가 함께 셈하지 않는다 — 두 번째 후보는 다른 종목을 내보내야 한다.
    const std::vector<websocket_slot::Entry> entries{entry(websocket_slot::kScoreBase + 80, true), entry(websocket_slot::kScoreBase + 90, true),
                                              entry(websocket_slot::kHeld, false), entry(websocket_slot::kHeld, false)};
    const auto result = websocket_slot::plan(entries, rules(2));
    CHECK(result.release.size() == 2 && result.take.size() == 2);
    return 0;
}

int test_managed_and_channels()
{
    // 칸 배정은 국내 현물만 받는다. 칸 수는 체결만이면 1, 호가까지면 2.
    WatchSpec stock;
    stock.ticker = "005930";
    CHECK(websocket_slot::is_managed(stock) && websocket_slot::channels_of(stock) == 2);
    stock.trade_only = true;
    CHECK(websocket_slot::channels_of(stock) == 1);

    WatchSpec future = stock;
    future.is_future = true;
    CHECK(!websocket_slot::is_managed(future));

    WatchSpec overseas = stock;
    overseas.market = Market::US;
    CHECK(!websocket_slot::is_managed(overseas));

    // 같은 구독인가는 체결만 여부를 보지 않는다.
    WatchSpec both = stock;
    both.trade_only = false;
    CHECK(same_watch(stock, both) && !same_watch(stock, future) && !same_watch(stock, overseas));
    return 0;
}
} // namespace

int main()
{
    if (test_priority_of() || test_fill_free_slots() || test_held_takes_slot_from_low_score() ||
        test_protected_never_released() || test_min_hold() || test_rank_gap() || test_max_swaps() || test_channels() ||
        test_released_not_reused() || test_managed_and_channels())
    {
        return 1;
    }

    std::cout << "test_websocket_slot_plan: " << g_checks << " checks passed\n";
    return 0;
}
