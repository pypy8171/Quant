#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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

inline std::string truncate(const std::string& text, int max_width)
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

        if (width + char_width > max_width)
        {
            break;
        }

        width += char_width;
        index += char_bytes;
    }

    return text.substr(0, index);
}

// 바이트열이 제대로 된 UTF-8인지 본다 — 이어지는 바이트 개수·과다 인코딩·서러게이트·U+10FFFF 초과를 거른다.
inline bool is_valid_utf8(std::string_view text)
{
    size_t index = 0;

    while (index < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        size_t              extra = 0;
        uint32_t            code_point = 0;

        if (lead < 0x80)
        {
            index += 1;
            continue;
        }
        else if (lead >= 0xC2 && lead <= 0xDF)
        {
            extra = 1;
            code_point = lead & 0x1Fu;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            extra = 2;
            code_point = lead & 0x0Fu;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            extra = 3;
            code_point = lead & 0x07u;
        }
        else
        {
            return false; // 0x80~0xC1 — 이어지는 바이트가 선두에 왔거나 과다 인코딩
        }

        if (index + extra >= text.size())
        {
            return false;
        }

        for (size_t step = 1; step <= extra; ++step)
        {
            const unsigned char following = static_cast<unsigned char>(text[index + step]);

            if ((following & 0xC0u) != 0x80u)
            {
                return false;
            }

            code_point = (code_point << 6) | (following & 0x3Fu);
        }

        const bool overlong  = (extra == 2 && code_point < 0x800u) || (extra == 3 && code_point < 0x10000u);
        const bool surrogate = code_point >= 0xD800u && code_point <= 0xDFFFu;

        if (overlong || surrogate || code_point > 0x10FFFFu)
        {
            return false;
        }

        index += extra + 1;
    }

    return true;
}

// 설정 json의 문자열은 UTF-8이다(nlohmann). 그 바이트를 그대로 std::filesystem::path에 넣으면 Windows는
//  프로세스 ANSI 코드페이지(CP949)로 읽어 변환에 실패한다 — 사용자 폴더 이름에 한글이 든 리플레이 경로에서 기동 중
//  예외로 죽었다(09-22 실측, "No mapping for the Unicode character exists in the target multi-byte code page").
//  char8_t 생성자는 입력을 UTF-8로 규정하므로 코드페이지를 타지 않는다.
//  다만 같은 자리에 path::string()이 만든 코드페이지 바이트가 들어오기도 한다(test_engine이 temp 경로를 그렇게 넘긴다).
//  그걸 UTF-8로 읽으면 이번엔 반대 방향으로 예외가 나 기동이 죽으므로, UTF-8이 아닐 때는 예전대로 코드페이지로 읽는다.
//  경로를 좁은 문자열로 주고받는 한 이 분기는 남는다 — 타입을 std::filesystem::path로 올리는 것이 정리다. [why D-071]
inline std::filesystem::path path_from_utf8(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    if (!is_valid_utf8(text))
    {
        return std::filesystem::path(std::string(text));
    }

    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

} // namespace utf8
