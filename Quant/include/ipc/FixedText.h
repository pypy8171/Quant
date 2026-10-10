#pragma once
// 공유 메모리 레코드의 고정 글자 칸 도우미 — 칸에 글자를 옮기고(글자 경계에서 자르기), 칸이 0으로 끝나는지 보고,
//  칸을 글자로 읽는다. 주문 통로(OrderChannel.cpp)와 체결 통로(FillChannel.cpp)가 같은 규칙을 쓰도록 한 곳에 둔다.
//  상태가 없어 어느 스레드가 불러도 된다. 칸 크기는 배열 타입에서 읽어, 칸과 다른 크기 상수를 넘기는 실수를 막는다.
//  템플릿이라 헤더에 몸통을 둔다 — 옮기기 전처럼 부르는 자리에 인라인된다. [why D-114]
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace ipc
{

// UTF-8 이어지는 바이트(10xxxxxx)를 가리는 마스크와 값.
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8ContinuationBits = 0x80;

// 고정 칸에 글자를 옮긴다. 칸을 넘으면 자르되 **글자 경계에서** 자른다 — UTF-8 한글은 한 글자가 세 바이트라
//  바이트로 끊으면 반쪽 글자가 남고, 그 글을 그대로 싣는 장부 CSV·로그가 깨진다. 항상 0으로 끝낸다.
//  잘렸으면 참을 준다.
template <size_t Capacity>
bool copy_text(char (&destination)[Capacity], std::string_view text) noexcept
{
    static_assert(Capacity > 0, "글자 칸은 0으로 끝낼 자리가 있어야 한다");
    size_t     length = std::min(text.size(), Capacity - 1);
    const bool cut    = length < text.size();

    if (cut)
    {
        // 자를 자리가 글자 가운데(10xxxxxx)면 그 글자가 시작하는 자리까지 물러선다.
        while (length > 0 &&
               (static_cast<unsigned char>(text[length]) & kUtf8ContinuationMask) == kUtf8ContinuationBits)
        {
            --length;
        }
    }

    if (length > 0)
    {
        std::memcpy(destination, text.data(), length);
    }

    destination[length] = '\0';
    return cut;
}

// 칸 안에서 0으로 끝나는가. 끝나지 않으면 읽는 쪽이 칸을 넘어 읽는다.
template <size_t Capacity>
bool is_terminated(const char (&field)[Capacity]) noexcept
{
    return std::memchr(field, '\0', Capacity) != nullptr;
}

// 칸을 글자로 읽는다. [inv] 0으로 끝나는 것을 확인한 뒤에만 부른다(is_plausible).
template <size_t Capacity>
std::string_view text_of(const char (&field)[Capacity]) noexcept
{
    const void*  end    = std::memchr(field, '\0', Capacity);
    const size_t length = (end != nullptr) ? static_cast<size_t>(static_cast<const char*>(end) - field) : Capacity;
    return std::string_view(field, length);
}

} // namespace ipc
