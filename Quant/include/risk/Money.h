#pragma once
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Money — 원화 금액의 단위와 반올림 규칙. 장부·게이트가 금액을 셈하는 곳은 모두 이 함수를 거친다.
//
//  금액은 원 단위 정수(Won)로 든다. double로 들면 체결이 쌓일수록 소수 오차가 붙어, 같은 장부를 다시 적용해도
//  당일 손익이 0.01원 단위로 달라지고 한도 비교(>=, <=)가 경계에서 갈렸다. 비율(총노출 %, 수수료율)과 지표만 double로 둔다.
//  반올림은 0.5에서 0에서 먼 쪽으로 간다(31.5 → 32, -31.5 → -32). 규칙을 여기 한 곳에만 두는 것은, 곳마다 자르기와
//  반올림이 섞이면 재기동 리플레이와 실시간 경로가 1원씩 어긋나기 때문이다.
// ─────────────────────────────────────────────────────────────────────────────
namespace money
{
using Won = int64_t;

// double을 Won으로 옮길 때 넘지 않는 크기. int64 끝(약 9.22e18)에 붙으면 변환 결과가 정해져 있지 않다.
inline constexpr double kWonLimit = 9.0e18;

// 실수 금액을 원으로 반올림한다. NaN·무한대는 0, 범위를 넘으면 끝값으로 자른다.
inline Won round_won(double value) noexcept
{
    if (!std::isfinite(value))
    {
        return 0;
    }

    if (value >= kWonLimit || value <= -kWonLimit)
    {
        return value > 0.0 ? static_cast<Won>(kWonLimit) : -static_cast<Won>(kWonLimit);
    }

    return static_cast<Won>(std::llround(value));
}

// 한도 비교용 내림·올림. "금액 > 실수 한도"는 "금액 > floor(한도)"와 같고, "금액 >= 실수 한도"는
//  "금액 >= ceil(한도)"와 같다 — 비율로 만든 한도를 정수 비교로 바꿀 때 판정이 달라지지 않게 쓴다.
inline Won floor_won(double value) noexcept
{
    return round_won(std::floor(value));
}

inline Won ceil_won(double value) noexcept
{
    return round_won(std::ceil(value));
}

// 정수 나눗셈 반올림(0.5는 0에서 먼 쪽). [inv] denominator > 0.
constexpr Won divide_round(Won numerator, Won denominator) noexcept
{
    const Won half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator : -((-numerator + half) / denominator);
}

// 금액 × (분자/분모) 반올림 — 수수료율·세율처럼 소수 비율을 정수 둘로 적어 쓴다(0.015% = 15/100000).
//  amount × 분자가 int64 안이어야 한다. 분자는 두 자리(15·20)라 명목이 1경(1e16) 원이어도 끝(약 9.22e18)에 못 미친다.
constexpr Won apply_rate(Won amount, Won rate_numerator, Won rate_denominator) noexcept
{
    return divide_round(amount * rate_numerator, rate_denominator);
}

// 단가 × 수량 = 명목(원). 게이트의 1주문·총노출 명목과 장부의 체결 명목이 이 한 곳을 지난다.
//  넘침 검사는 두지 않는다 — 수량은 int32라 끝이 약 21억 주이고, 그때도 단가 42억 9천만 원까지는 int64 끝 안이다. 국내 주가는
//  1주 수백만 원이 끝이라 여유가 수백 배 남는다. 경계는 Quant/tests/test_position_ledger.cpp가 컴파일 시점에 본다.
constexpr Won notional(Won price, int64_t quantity) noexcept
{
    return price * quantity;
}
} // namespace money
