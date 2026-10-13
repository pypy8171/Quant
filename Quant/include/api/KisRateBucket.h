// api/KisRateBucket.h — KIS REST 호출 토큰 버킷의 크기 계산. 헤더에 두는 이유는 constexpr 순수 함수라서다(D-118).
//  전송 계층(KisTransport.cpp)과 단위 테스트가 같은 계산을 본다.
#pragma once

namespace kis_rate
{

struct BucketPlan
{
    double published_limit; // KIS가 공표한 초당 한도
    double capacity;        // 모아둘 수 있는 최대 토큰(한 번에 몰아칠 수 있는 양)
    double refill;          // 초당 채워 넣는 토큰
    double quote_need;      // 시세·기타 호출이 나가려면 버킷에 있어야 하는 토큰
    double trade_need;      // 주문·잔고 호출이 나가려면 버킷에 있어야 하는 토큰
};

// 공표 한도. 실전 초당 20건, 모의 초당 2건.
// 근거: 공식 샘플에는 한도 숫자가 없다(2026-09-27 MCP). 실전 20건은 docs/DECISIONS.md D-138, 모의 2건은 D-074와
//  _private/_intraday_issues/2026-09-09.md의 EGW00201(초당 거래건수 초과) 기록.
constexpr double bucket_published_limit(bool is_paper)
{
    return is_paper ? 2.0 : 20.0;
}

// 한 번에 몰아칠 수 있는 양. 최소 1건은 있어야 첫 호출이 기다리지 않고 나간다.
constexpr double bucket_burst(bool is_paper)
{
    return is_paper ? 1.0 : 5.0;
}

// [inv] capacity + refill <= published_limit — 버킷은 모아둔 capacity를 한꺼번에 내보낸 뒤 이어지는
//  1초 동안 refill만큼을 더 내보내므로, 한 초에 나가는 최대치가 그 합이다. 예전에는 capacity를 refill과
//  같게 두어 모의가 한 초에 4건(2+2)이 되었고 공표 한도 2건을 넘겼다 — 2026-09-23 09:03 EGW00201
//  "초당 거래건수를 초과하였습니다" 실측. 실전도 30(15+15) vs 20으로 같은 결함인데 여유분에 가려져 있었다.
//  공표 한도를 몰아치기 몫과 리필 몫으로 나눠 갖는 것으로 바꾼다.
constexpr double bucket_refill(bool is_paper)
{
    return bucket_published_limit(is_paper) - bucket_burst(is_paper);
}

// 주문·잔고 경로에는 예약분을 남긴다. 시세 조회가 버킷을 다 비운 순간 청산 주문이 그 뒤에 줄서면
//  몇 백 ms가 늦는데, 그 지연은 조회 지연과 값이 다르다. 버킷이 작아 예약분을 뗄 자리가 없으면(모의)
//  예약 없이 간다 — [inv] quote_need <= capacity 라야 시세 호출이 영영 못 나가는 일이 없다.
constexpr double bucket_trade_need(bool)
{
    return 1.0;
}

constexpr double bucket_quote_need(bool is_paper)
{
    return bucket_burst(is_paper) >= 2.0 ? 2.0 : 1.0;
}

// 이 프로세스가 쓸 초당 몫. config kis·quote_kis의 rest_calls_per_second 칸이다. 0 이하이거나 공표 한도 이상이면
//  공표 한도를 그대로 쓴다(칸이 없을 때의 옛 동작). 2건 아래로는 내리지 않는다 — 몫이 1이면 몰아치기 1건과
//  리필 0건이 되어 두 번째 호출이 영영 못 나간다.
// [why D-138] 같은 앱키를 두 프로세스(모의 엔진의 시세 키 = 실계좌 엔진의 주문 키)가 쓰면 각자 20건을 채워
//  합쳐서 한도를 넘는다. 10-13 실계좌 로그의 "조회 (초당 한도)"가 그 결과다. 몫은 프로세스끼리 나눠 갖는 값이다.
constexpr double bucket_share_limit(bool is_paper, double calls_per_second)
{
    const double published = bucket_published_limit(is_paper);

    if (calls_per_second <= 0.0 || calls_per_second >= published)
    {
        return published;
    }

    return calls_per_second < 2.0 ? 2.0 : calls_per_second;
}

// 몫을 받으면 몰아치기도 몫에 비례해 줄인다. 그러지 않으면 몫 6에 몰아치기 5가 남아 한 순간에 5건이 나가고,
//  다른 프로세스와 겹친 그 순간 합이 공표 한도를 넘는다. 1건 아래로는 내리지 않는다(첫 호출이 기다리지 않게).
constexpr BucketPlan bucket_plan(bool is_paper, double calls_per_second = 0.0)
{
    const double limit       = bucket_share_limit(is_paper, calls_per_second);
    const double scaled      = bucket_burst(is_paper) * limit / bucket_published_limit(is_paper);
    const double burst       = scaled < 1.0 ? 1.0 : scaled;
    const double quote_need  = burst >= 2.0 ? 2.0 : 1.0;
    return BucketPlan{ limit, burst, limit - burst, quote_need, bucket_trade_need(is_paper) };
}

// 한도 초과 응답 뒤 되보내기까지 기다리는 초. note_rate_limited가 버킷을 0으로 비우므로 요구 토큰이 다시
//  찰 때까지다 — 실전 시세 2/15≈0.13초·주문 1/15≈0.07초, 모의 1/1=1초. 예전에는 1.1초를 고정으로 쉬었다(W-3).
constexpr double wait_after_rate_limited(const BucketPlan& plan, bool priority)
{
    return (priority ? plan.trade_need : plan.quote_need) / plan.refill;
}

constexpr double wait_after_rate_limited(bool is_paper, bool priority)
{
    return wait_after_rate_limited(bucket_plan(is_paper), priority);
}

} // namespace kis_rate
