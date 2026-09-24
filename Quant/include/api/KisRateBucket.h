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

constexpr BucketPlan bucket_plan(bool is_paper)
{
    return BucketPlan{ bucket_published_limit(is_paper), bucket_burst(is_paper), bucket_refill(is_paper),
                       bucket_quote_need(is_paper), bucket_trade_need(is_paper) };
}

// 한도 초과 응답 뒤 되보내기까지 기다리는 초. note_rate_limited가 버킷을 0으로 비우므로 요구 토큰이 다시
//  찰 때까지다 — 실전 시세 2/15≈0.13초·주문 1/15≈0.07초, 모의 1/1=1초. 예전에는 1.1초를 고정으로 쉬었다(W-3).
constexpr double wait_after_rate_limited(bool is_paper, bool priority)
{
    return (priority ? bucket_trade_need(is_paper) : bucket_quote_need(is_paper)) / bucket_refill(is_paper);
}

} // namespace kis_rate
