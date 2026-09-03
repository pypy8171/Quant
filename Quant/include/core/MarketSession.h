#pragma once
#include <string>

// KRX 정규장 세션 경계 (HHMM 정수) — 여러 전략이 같은 09:00~15:30 창을 각자 복사해
// 쓰던 것을 한 곳으로 모은다. 파장(15:30) 이후 시간외/동시호가는 제외.
namespace krx
{
constexpr int kSessionOpenHHMM  = 900;   // 09:00 정규장 시작
constexpr int kSessionCloseHHMM = 1530;  // 15:30 정규장 종료

// "HHMMSS"(또는 "HHMM") 앞 4자리를 HHMM 정수로. 형식 불량이면 0.
inline int parse_hhmm(const std::string& t)
{
    if (t.size() < 4)
        return 0;
    try
    {
        return std::stoi(t.substr(0, 2)) * 100 + std::stoi(t.substr(2, 2));
    }
    catch (...)
    {
        return 0;
    }
}

// 정규장 세션 안(09:00 이상 15:30 미만)인지.
inline bool in_session(int hhmm)
{
    return hhmm >= kSessionOpenHHMM && hhmm < kSessionCloseHHMM;
}
inline bool in_session_str(const std::string& t)
{
    return in_session(parse_hhmm(t));
}
} // namespace krx
