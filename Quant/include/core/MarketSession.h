#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// KRX 정규장 세션 경계 (HHMM 정수) — 여러 전략이 같은 09:00~15:30 창을 각자 복사해
// 쓰던 것을 한 곳으로 모은다. 파장(15:30) 이후 시간외/동시호가는 제외.
namespace krx
{
constexpr int kSessionOpenHHMM  = 900;   // 09:00 정규장 시작
constexpr int kSessionCloseHHMM = 1530;  // 15:30 정규장 종료

// "HHMMSS"(또는 "HHMM") 앞 4자리를 HHMM 정수로. 형식 불량이면 0.
// 앞 4자리가 전부 숫자여야 한다 — stoi는 "1a"를 1로 받아 "1a30"이 130(장 밖이지만 유효값)으로 새고,
// "+930"은 930으로 장중 판정된다.
inline int parse_hhmm(std::string_view ticker)
{
    if (ticker.size() < 4)
    {
        return 0;
    }

    for (int index = 0; index < 4; ++index)
    {
        if (ticker[index] < '0' || ticker[index] > '9')
        {
            return 0;
        }
    }

    return (ticker[0] - '0') * 1000 + (ticker[1] - '0') * 100 + (ticker[2] - '0') * 10 + (ticker[3] - '0');
}

// 정규장 세션 안(09:00 이상 15:30 미만)인지.
inline bool in_session(int hhmm)
{
    return hhmm >= kSessionOpenHHMM && hhmm < kSessionCloseHHMM;
}

inline bool in_session_string(std::string_view ticker)
{
    return in_session(parse_hhmm(ticker));
}

// "HHMMSS" 여섯 자리를 정수로("093001" → 93001). 여섯 자리 숫자가 아니면 0 — 틱은 디코더가 한 번만 부르고
//  소비자는 정수만 본다(원칙 6). 뒤에 더 붙은 글자(밀리초 등)는 무시한다. [why D-071]
inline int32_t parse_hhmmss(std::string_view ticker)
{
    if (ticker.size() < 6)
    {
        return 0;
    }

    int32_t value = 0;

    for (int index = 0; index < 6; ++index)
    {
        if (ticker[index] < '0' || ticker[index] > '9')
        {
            return 0;
        }

        value = value * 10 + (ticker[index] - '0');
    }

    return value;
}

// 정수 HHMMSS를 여섯 자리 문자열로(93001 → "093001"). 화면·CSV·캡처 파일용 — hot path에서 부르지 않는다.
inline std::string hhmmss_string(int32_t hhmmss)
{
    char buffer[7];

    for (int index = 5; index >= 0; --index)
    {
        buffer[index] = static_cast<char>('0' + hhmmss % 10);
        hhmmss /= 10;
    }

    buffer[6] = '\0';
    return std::string(buffer);
}
} // namespace krx
