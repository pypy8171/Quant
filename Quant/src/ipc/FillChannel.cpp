// FillChannel.h 구현 — 체결통보 한 건을 고정 칸 레코드로 옮기고, 그 레코드를 큐 하나로 나른다.
#include "ipc/FillChannel.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ipc
{
namespace
{

// 고정 칸에 글자를 옮긴다. 칸을 넘으면 자르되 **글자 경계에서** 자른다 — 체결통보 칸은 다 아스키지만,
//  거래소 구분처럼 나중에 글자가 섞일 수 있는 칸이 있어 OrderChannel 과 같은 규칙을 쓴다. 항상 0으로
//  끝낸다. 잘렸으면 참을 준다.
bool copy_text(char* destination, size_t capacity, std::string_view text) noexcept
{
    size_t     length = std::min(text.size(), capacity - 1);
    const bool cut    = length < text.size();

    if (cut)
    {
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
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
bool is_terminated(const char* field, size_t capacity) noexcept
{
    return std::memchr(field, '\0', capacity) != nullptr;
}

// 칸을 글자로 읽는다. [inv] 0으로 끝나는 것을 확인한 뒤에만 부른다(is_plausible).
std::string_view text_of(const char* field, size_t capacity) noexcept
{
    const void*  end    = std::memchr(field, '\0', capacity);
    const size_t length = (end != nullptr) ? static_cast<size_t>(static_cast<const char*>(end) - field) : capacity;
    return std::string_view(field, length);
}

// 글자 칸이 모두 0으로 끝나는가.
bool texts_terminated(const FillNotice& notice) noexcept
{
    return is_terminated(notice.kis_order_no, kFillOrderNumberMax) &&
           is_terminated(notice.original_order_no, kFillOrderNumberMax) &&
           is_terminated(notice.ticker, kFillTickerMax) && is_terminated(notice.fill_time, kFillTimeMax) &&
           is_terminated(notice.exchange, kFillExchangeMax);
}

} // namespace

bool is_plausible(const FillNotice& notice, const FillLimits& limits) noexcept
{
    if (notice.sequence == 0 || notice.sent_at_ns <= 0)
    {
        return false;
    }

    // 방향은 배열 첨자로 쓰이지는 않지만, 표 밖 값이 들어오면 아래 갈래가 전부 "매수"로 읽힌다.
    if (notice.side > static_cast<uint8_t>(OrderSide::NONE))
    {
        return false;
    }

    // 재연결 표지는 수량·가격·종목이 빈 레코드다. 종류 칸과 글자 칸 끝만 보고 받는다. [why D-149]
    if (notice.kind == static_cast<uint8_t>(FillKind::SessionResumed))
    {
        return texts_terminated(notice);
    }

    if (notice.kind != static_cast<uint8_t>(FillKind::Fill))
    {
        return false;
    }

    // 체결 수량 0은 체결이 아니다. 음수·상한 초과는 예약 수량을 엉뚱하게 푼다.
    if (notice.filled_quantity <= 0 || notice.filled_quantity > limits.quantity_max)
    {
        return false;
    }

    // 주문수량은 짧은 전문에서 0으로 온다 — "모른다"는 뜻이라 0은 받는다. 음수만 막는다.
    if (notice.order_quantity < 0 || notice.order_quantity > limits.quantity_max)
    {
        return false;
    }

    if (!(notice.filled_price > 0.0) || notice.filled_price > limits.price_max)
    {
        return false;
    }

    // 종목 코드가 비면 이 체결을 어디에 붙일지 알 수 없다.
    if (notice.ticker[0] == '\0')
    {
        return false;
    }

    return texts_terminated(notice);
}

FillNotice to_notice(const FillNotification& fill, uint64_t sequence, int64_t sent_at_ns, bool* truncated) noexcept
{
    FillNotice notice;
    notice.sequence        = sequence;
    notice.sent_at_ns      = sent_at_ns;
    notice.timestamp_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(fill.timestamp.time_since_epoch()).count();
    notice.filled_price    = fill.filled_price;
    notice.filled_quantity = fill.filled_quantity;
    notice.order_quantity  = fill.order_quantity;
    notice.session_generation = fill.session_generation;
    notice.kind            = static_cast<uint8_t>(fill.kind);
    notice.side            = static_cast<uint8_t>(static_cast<OrderSide::Value>(fill.side));

    bool cut = copy_text(notice.kis_order_no, kFillOrderNumberMax, fill.kis_order_no);
    cut      = copy_text(notice.original_order_no, kFillOrderNumberMax, fill.original_order_no) || cut;
    cut      = copy_text(notice.ticker, kFillTickerMax, fill.ticker) || cut;
    cut      = copy_text(notice.fill_time, kFillTimeMax, fill.fill_time) || cut;
    cut      = copy_text(notice.exchange, kFillExchangeMax, fill.exchange) || cut;

    if (truncated != nullptr)
    {
        *truncated = cut;
    }

    return notice;
}

FillNotification to_fill(const FillNotice& notice)
{
    FillNotification fill;
    fill.kis_order_no      = std::string(text_of(notice.kis_order_no, kFillOrderNumberMax));
    fill.original_order_no = std::string(text_of(notice.original_order_no, kFillOrderNumberMax));
    fill.ticker            = std::string(text_of(notice.ticker, kFillTickerMax));
    fill.side              = static_cast<OrderSide::Value>(notice.side);
    fill.filled_quantity   = notice.filled_quantity;
    fill.filled_price      = notice.filled_price;
    fill.fill_time         = std::string(text_of(notice.fill_time, kFillTimeMax));
    fill.session_generation = notice.session_generation;
    fill.kind              = static_cast<FillKind>(notice.kind);
    fill.order_quantity    = notice.order_quantity;
    fill.exchange          = std::string(text_of(notice.exchange, kFillExchangeMax));
    fill.timestamp         = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(notice.timestamp_ns)));
    return fill;
}

size_t FillChannel::bytes_for(size_t capacity)
{
    return SharedSpscRing<FillNotice>::bytes_for(capacity);
}

bool FillChannel::create(std::byte* base, size_t bytes, size_t capacity)
{
    last_error_.clear();

    if (!ring_.create(base, bytes, capacity))
    {
        last_error_ = "체결 통로: " + std::string(ring_.last_error());
        return false;
    }

    return true;
}

bool FillChannel::attach(std::byte* base, size_t bytes, RingEndpoint endpoint, size_t capacity)
{
    last_error_.clear();

    if (!ring_.attach(base, bytes, capacity, endpoint))
    {
        last_error_ = "체결 통로: " + std::string(ring_.last_error());
        return false;
    }

    return true;
}

void FillChannel::unbind() noexcept
{
    ring_.unbind();
}

bool FillChannel::push(const FillNotice& notice) noexcept
{
    if (!ring_.push(notice))
    {
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool FillChannel::pop(const FillLimits& limits, FillNotice& out) noexcept
{
    FillNotice notice;

    while (ring_.pop(notice))
    {
        if (is_plausible(notice, limits))
        {
            out = notice;
            return true;
        }

        discarded_.fetch_add(1, std::memory_order_relaxed);
    }

    return false;
}

uint64_t FillChannel::sent() const
{
    return ring_.sent();
}

uint64_t FillChannel::received() const
{
    return ring_.received();
}

uint64_t FillChannel::stamp_out_of_turn() const
{
    return ring_.stamp_out_of_turn();
}

size_t FillChannel::pending() const
{
    return ring_.pending();
}

size_t FillChannel::readable() const
{
    return ring_.readable();
}

} // namespace ipc
