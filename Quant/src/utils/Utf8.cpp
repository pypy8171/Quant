#include "utils/Utf8.h"

namespace utf8
{
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
