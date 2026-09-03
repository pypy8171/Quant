#pragma once
#include "core/Types.h"
#include <cmath>

// KRX 호가단위 (2023 통합) — 코스피/코스닥 일반주식 동일 구간.
// 여러 전략(MarketMaking·DeviationScale 등)이 같은 표를 복사해 쓰던 것을 한 곳으로 모은다.
namespace krx
{
inline double tick_size(double price)
{
    if (price < 2000)   return 1;
    if (price < 5000)   return 5;
    if (price < 20000)  return 10;
    if (price < 50000)  return 50;
    if (price < 200000) return 100;
    if (price < 500000) return 500;
    return 1000;
}

// 호가단위 격자로 절사. BUY=내림, SELL=올림(스프레드 보존).
inline double round_to_tick(double p, OrderSide side)
{
    const double t = tick_size(p);
    if (t <= 0.0)
        return p;
    return (side == OrderSide::BUY) ? std::floor(p / t) * t : std::ceil(p / t) * t;
}
} // namespace krx
