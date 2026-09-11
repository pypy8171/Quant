#pragma once
// 게이트 거부 사유 문자열의 계약 — 만드는 쪽(OrderGate)과 읽는 쪽(OrderPacer의 재시도 분류)이 같은 정의를 쓴다.
//  사유는 로그·운영단말에 그대로 나가는 문장이라 문자열로 남기되, 부분문자열 검색의 양끝을 여기 한 곳에 둔다. [why D-067]
#include <string>

namespace gate_reason
{
// 유량 한도 거부의 머리. 분류기는 rfind(kRateLimit, 0) == 0으로 게이트 거부를 알아본다.
inline constexpr char kRateLimit[] = "Rate limit 초과";
// 분당 한도 표시 — 창이 비기까지 최대 60초라 재시도 지연을 길게 잡는 신호.
inline constexpr char kPerMinute[] = "분당";
inline constexpr char kPerSecond[] = "초당";

// "Rate limit 초과 (초당 N건)" / "Rate limit 초과 (분당 N건)"
inline std::string rate_limit(bool per_minute, int limit)
{
    return std::string(kRateLimit) + " (" + (per_minute ? kPerMinute : kPerSecond) + " " + std::to_string(limit) +
           "건)";
}

inline bool is_rate_limit(const std::string& reason)
{
    return reason.rfind(kRateLimit, 0) == 0;
}

inline bool is_per_minute(const std::string& reason)
{
    return reason.find(kPerMinute) != std::string::npos;
}
} // namespace gate_reason
