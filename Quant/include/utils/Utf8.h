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

inline int display_width(const std::string& text)
{
    int width = 0;
    size_t index = 0;

    while (index < text.size())
    {
        unsigned char byte = text[index];

        if      (byte < 0x80) { index += 1; width += 1; }
        else if (byte < 0xE0) { index += 2; width += 2; }
        else if (byte < 0xF0) { index += 3; width += 2; } // CJK
        else               { index += 4; width += 2; }
    }

    return width;
}

inline std::string pad_right(const std::string& text, int target)
{
    int width = display_width(text);
    return (width >= target) ? text : text + std::string(target - width, ' ');
}

inline std::string trunc(const std::string& text, int max_w)
{
    int width = 0;
    size_t index = 0;

    while (index < text.size())
    {
        unsigned char byte = text[index];
        int char_width, char_bytes;

        if      (byte < 0x80) { char_width = 1; char_bytes = 1; }
        else if (byte < 0xE0) { char_width = 2; char_bytes = 2; }
        else if (byte < 0xF0) { char_width = 2; char_bytes = 3; }
        else               { char_width = 2; char_bytes = 4; }

        if (width + char_width > max_w)
        {
            break;
        }

        width += char_width;
        index += char_bytes;
    }

    return text.substr(0, index);
}

} // namespace utf8
