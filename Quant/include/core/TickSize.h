#pragma once
#include "core/Types.h"
#include <cmath>

// KRX 호가단위 (2023 통합) — 코스피/코스닥 일반주식 동일 구간.
// 여러 전략(MarketMaking·DeviationScale 등)이 같은 표를 복사해 쓰던 것을 한 곳으로 모은다.
namespace krx
{
// 호가단위 구간 — 이 값 "미만"이면 이 호가단위. 마지막 구간 위는 kTopTickSize.
struct TickBand
{
    double below;
    double tick;
};

// [formula] KRX 호가가격단위(2023-01-25 개편, 유가증권·코스닥 공통) — 2천 원 미만 1원, 5천 원 미만 5원, 2만 원 미만 10원,
//  5만 원 미만 50원, 20만 원 미만 100원, 50만 원 미만 500원, 그 이상 1,000원. 출처: 유가증권시장 업무규정 시행세칙 제30조.
inline constexpr TickBand kTickBands[] = {
    {2'000, 1}, {5'000, 5}, {20'000, 10}, {50'000, 50}, {200'000, 100}, {500'000, 500},
};
inline constexpr double kTopTickSize = 1'000;

constexpr double tick_size(double price)
{
    for (const TickBand& band : kTickBands)
    {
        if (price < band.below)
        {
            return band.tick;
        }
    }

    return kTopTickSize;
}

// 호가단위 격자로 절사. BUY=내림, SELL=올림(스프레드 보존).
double round_to_tick(double price, OrderSide side);
} // namespace krx
