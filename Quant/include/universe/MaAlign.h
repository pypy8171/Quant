#pragma once

// 일봉 이동평균 정배열 판정 한 곳. 전략(DeviationScaleStrategy)과 스캐너(UniverseScanner)가
//  같은 식을 각자 들고 있어 한쪽만 고치면 스캐너는 등록하는데 전략은 활성하지 않는 슬롯이 생겼다.
//  스레드 소유권 없음(순수 함수). [why D-005] 일봉 캐시가 전일치인 이유는 그쪽에 있다.

namespace quant
{
namespace ma
{

// 최신 봉이 앞(index 0)인 일봉에서 뽑은 전일까지의 이동평균.
struct Smas
{
    double s5  = 0.0;
    double s10 = 0.0;
    double s20 = 0.0;
    double s60 = 0.0;
};

// [formula] 당일 SMA_n = (전일까지 SMA_n x n - 창에서 밀려나는 봉 종가 + 오늘 현재가) / n.
//  일봉 조회가 include_today=false라 전일치에서 멈춘다. 그대로 쓰면 정배열이 하루 종일
//  고정돼 장중에 이평이 깨져도 판정이 따라가지 않는다. 오늘 봉을 여기서 접어 넣는다.
//  drop_n = 각 창에서 밀려나는 봉의 종가(newest-first 배열의 d[n-1].close).
//  px<=0 또는 60봉 미달(drop60<=0)이면 접지 않고 전일 값을 그대로 돌려준다.
inline Smas fold_today(const Smas& prev,
                       double drop5, double drop10, double drop20, double drop60,
                       double px)
{
    if (px <= 0.0 || drop60 <= 0.0)
    {
        return prev;
    }

    Smas out;
    out.s5  = (prev.s5  *  5 - drop5  + px) /  5.0;
    out.s10 = (prev.s10 * 10 - drop10 + px) / 10.0;
    out.s20 = (prev.s20 * 20 - drop20 + px) / 20.0;
    out.s60 = (prev.s60 * 60 - drop60 + px) / 60.0;
    return out;
}

// [inv] 정배열 = s5>s10>s20>s60. 마지막 조건에만 허용오차 tol을 준다 — SMA20>SMA60은
//  "3개월 추세 위"라 긴 하락 뒤 회복 국면에서는 주도주도 여기서 먼저 떨어진다.
//  tol=0이면 엄격 판정, 1.0 이상이면 마지막 조건이 사라진다.
inline bool aligned(const Smas& s, double tol)
{
    const double s60_bar = s.s60 * (1.0 - tol);
    return s.s5 > s.s10 && s.s10 > s.s20 && s.s20 > s60_bar;
}

}  // namespace ma
}  // namespace quant
