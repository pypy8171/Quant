#pragma once
#include <string>

// UTF-8 문자열 터미널 표시폭 계산 및 패딩 유틸
// CJK 문자는 2칸, ASCII는 1칸으로 계산
//
// 선두 바이트 값으로 문자 길이를 판별한다(UTF-8 규격):
//   <0x80  = ASCII 1바이트(폭 1)
//   <0xE0  = 2바이트 시퀀스(폭 2 — 라틴 확장·한글 자모 등)
//   <0xF0  = 3바이트 시퀀스(폭 2 — 한글 완성형·CJK 등)
//   그 외  = 4바이트 시퀀스(폭 2 — 이모지·보조평면)

namespace utf8 {

inline int display_width(const std::string& s)
{
    int w = 0;
    size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = s[i];
        if      (c < 0x80) { i += 1; w += 1; }
        else if (c < 0xE0) { i += 2; w += 2; }
        else if (c < 0xF0) { i += 3; w += 2; } // CJK
        else               { i += 4; w += 2; }
    }
    return w;
}

inline std::string pad_right(const std::string& s, int target)
{
    int w = display_width(s);
    return (w >= target) ? s : s + std::string(target - w, ' ');
}

inline std::string trunc(const std::string& s, int max_w)
{
    int w = 0;
    size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = s[i];
        int cw, cb;
        if      (c < 0x80) { cw = 1; cb = 1; }
        else if (c < 0xE0) { cw = 2; cb = 2; }
        else if (c < 0xF0) { cw = 2; cb = 3; }
        else               { cw = 2; cb = 4; }
        if (w + cw > max_w) break;
        w += cw;
        i += cb;
    }
    return s.substr(0, i);
}

} // namespace utf8
