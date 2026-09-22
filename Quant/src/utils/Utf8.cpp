#include "utils/Utf8.h"

namespace utf8
{
int display_width(const std::string& text)
{
    int width = 0;
    size_t index = 0;

    while (index < text.size())
    {
        unsigned char byte = text[index];

        if (byte < 0x80)
        {
            index += 1;
            width += 1;
        }
        else if (byte < 0xE0)
        {
            index += 2;
            width += 2;
        }
        else if (byte < 0xF0)
        {
            index += 3;
            width += 2;
        } // CJK
        else
        {
            index += 4;
            width += 2;
        }
    }

    return width;
}

std::string pad_right(const std::string& text, int target)
{
    int width = display_width(text);
    return (width >= target) ? text : text + std::string(target - width, ' ');
}

std::string truncate(const std::string& text, int max_width)
{
    int width = 0;
    size_t index = 0;

    while (index < text.size())
    {
        unsigned char byte = text[index];
        int char_width, char_bytes;

        if (byte < 0x80)
        {
            char_width = 1;
            char_bytes = 1;
        }
        else if (byte < 0xE0)
        {
            char_width = 2;
            char_bytes = 2;
        }
        else if (byte < 0xF0)
        {
            char_width = 2;
            char_bytes = 3;
        }
        else
        {
            char_width = 2;
            char_bytes = 4;
        }

        if (width + char_width > max_width)
        {
            break;
        }

        width += char_width;
        index += char_bytes;
    }

    return text.substr(0, index);
}

bool is_valid_utf8(std::string_view text)
{
    size_t index = 0;

    while (index < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        size_t extra = 0;
        uint32_t code_point = 0;

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

        const bool overlong = (extra == 2 && code_point < 0x800u) || (extra == 3 && code_point < 0x10000u);
        const bool surrogate = code_point >= 0xD800u && code_point <= 0xDFFFu;

        if (overlong || surrogate || code_point > 0x10FFFFu)
        {
            return false;
        }

        index += extra + 1;
    }

    return true;
}

std::filesystem::path path_from_utf8(std::string_view text)
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
