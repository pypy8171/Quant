// KIS REST 초당 한도 버킷(api/KisRateBucket.h) 단위 테스트. 헤더 전용이라 링크할 라이브러리가 없다.
//  고정하는 것은 셋이다 — 한 초에 나가는 최대치가 공표 한도를 넘지 않는지, 첫 호출이 기다리지 않는지,
//  시세 호출이 요구하는 토큰이 버킷 크기 안에 드는지. 셋째가 빠지면 모의계좌 시세 조회가 영영 못 나간다.
// 빌드: cmake --build <directory> --target test_kis_rate_bucket
#include "api/KisRateBucket.h"

#include <cstdlib>
#include <iostream>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

int check_plan(bool is_paper)
{
    const kis_rate::BucketPlan plan = kis_rate::bucket_plan(is_paper);

    // 한 초에 나가는 최대치는 모아둔 capacity를 한꺼번에 내보낸 뒤 그 1초 동안 차는 refill을 더한 값이다.
    //  2026-09-23 09:03 EGW00201은 이 합이 모의에서 4건(2+2)이라 공표 한도 2건을 넘겨서 났다.
    CHECK(plan.capacity + plan.refill <= plan.published_limit);

    // 첫 호출은 기다리지 않는다 — 버킷이 가득 찬 상태로 출발하므로 1건은 들어 있어야 한다.
    CHECK(plan.capacity >= 1.0);
    CHECK(plan.refill > 0.0);

    // 시세·주문 호출이 요구하는 토큰이 버킷 크기를 넘으면 그 호출은 영원히 못 나간다.
    CHECK(plan.quote_need <= plan.capacity);
    CHECK(plan.trade_need <= plan.capacity);

    // 주문·잔고는 시세보다 먼저 나가야 하므로 요구 토큰이 더 크면 안 된다.
    CHECK(plan.trade_need <= plan.quote_need);

    return 0;
}
} // namespace

int main()
{
    if (check_plan(true) != 0)
    {
        return 1;
    }

    if (check_plan(false) != 0)
    {
        return 1;
    }

    // 공표 한도 자체가 뒤바뀌지 않았는지 — 실전 20건, 모의 2건.
    CHECK(kis_rate::bucket_plan(true).published_limit == 2.0);
    CHECK(kis_rate::bucket_plan(false).published_limit == 20.0);

    // 실전에서는 주문 예약분이 실제로 서 있어야 한다(시세는 1건을 더 요구해 그만큼 남긴다).
    CHECK(kis_rate::bucket_plan(false).quote_need > kis_rate::bucket_plan(false).trade_need);

    std::cout << "test_kis_rate_bucket OK (" << g_checks << " checks)\n";
    return 0;
}
