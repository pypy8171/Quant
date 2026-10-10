#pragma once
// 주문 방향 라벨 — 로그·부속 파일에 적는 "BUY"/"SELL"/"NONE" 글자를 한 곳에서 정한다. 라우터 파일마다 삼항으로
//  따로 쓰던 것을 모았다. 상태가 없어 어느 스레드가 불러도 된다. constexpr이라 헤더에 둔다.
//  자리는 OrderSide 옆(Quant/include/core/Types.h)이 맞다 — 그 파일을 고치는 단계에서 옮긴다. [why D-071]
#include <string_view>

#include "core/Types.h" // OrderSide

// NONE은 "NONE"으로 적는다. 라우터의 옛 삼항은 NONE을 "SELL"로 적었지만, 이 함수를 쓰는 자리는 게이트를 지난
//  주문·장부 의도·체결통보(방향을 모르면 해석 단계에서 버린다)라 NONE이 오지 않는다.
constexpr std::string_view side_label(OrderSide side) noexcept
{
    if (side == OrderSide::BUY)
    {
        return "BUY";
    }

    if (side == OrderSide::SELL)
    {
        return "SELL";
    }

    return "NONE";
}
