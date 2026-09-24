#include "ipc/ControlChannel.h"

#include <algorithm>
#include <cstring>

namespace ipc
{
namespace
{
// 표 하나에 흔히 실리는 줄 수 — 미리 잡아 두는 크기일 뿐이라 넘으면 vector 가 늘린다.
//  상한(kControlTableMax)과는 다른 값이다.
constexpr size_t kTypicalTableRows = 256;

// 고정 칸에 글자를 담는다. 칸을 넘으면 자르고 끝에 0을 넣는다.
void copy_into_field(char* field, size_t field_size, std::string_view text) noexcept
{
    const size_t length = std::min(text.size(), field_size - 1);
    std::memcpy(field, text.data(), length);
    field[length] = '\0';
}

// 고정 칸에 담긴 글자. 채우는 쪽이 끝에 0을 넣지만, 통로 저쪽에서 온 칸은 믿지 않고 칸 안에서 끝을
//  찾는다 — 0이 없으면 칸 끝이 끝이다. [inv] 돌려주는 조각은 칸이 사는 동안만 유효하다.
std::string_view view_of_field(const char* field, size_t field_size) noexcept
{
    const char*  end    = static_cast<const char*>(std::memchr(field, '\0', field_size));
    const size_t length = end != nullptr ? static_cast<size_t>(end - field) : field_size;
    return std::string_view(field, length);
}
} // namespace

bool routes_to_feed(ControlKind kind) noexcept
{
    // 소켓을 쥔 쪽에 닿아야 하는 낱말만 고른다. 나머지(종목 등록·전략 등록·진입 정지·배수·킬스위치·손 정지·
    //  하루 초기화·보호 주문·슬롯 면제·진입 우선순위)는 원장과 주문 게이트를 쥔 주문 프로세스 몫이다.
    return kind == ControlKind::kWatchSubscribe || kind == ControlKind::kWatchUnsubscribe;
}

void set_account(ControlRequest& request, std::string_view account) noexcept
{
    copy_into_field(request.account, kControlAccountMax, account);
}

std::string_view account_of(const ControlRequest& request) noexcept
{
    return view_of_field(request.account, kControlAccountMax);
}

void set_exchange(ControlRequest& request, std::string_view exchange) noexcept
{
    copy_into_field(request.exchange, kControlExchangeMax, exchange);
}

std::string_view exchange_of(const ControlRequest& request) noexcept
{
    return view_of_field(request.exchange, kControlExchangeMax);
}

ControlTableBuilder::ControlTableBuilder(size_t capacity) : capacity_(capacity)
{
    rows_.reserve(std::min<size_t>(capacity_, kTypicalTableRows)); // 흔한 표 크기만 미리 잡는다
}

void ControlTableBuilder::begin(uint64_t batch)
{
    if (open_ && !rows_.empty())
    {
        discarded_ += rows_.size(); // 앞 표가 commit 을 못 받았다 — 그 줄은 걸지 않는다
    }

    rows_.clear();
    open_batch_ = batch;
    open_       = true;
    spoiled_    = false;
}

void ControlTableBuilder::add(const ControlRequest& request)
{
    if (!open_ || request.batch != open_batch_ || rows_.size() >= capacity_)
    {
        ++discarded_;

        if (open_ && request.batch == open_batch_)
        {
            spoiled_ = true; // 내 표의 줄을 잃었다 — 반쪽 표는 걸지 않는다
        }

        return;
    }

    rows_.push_back(request);
}

bool ControlTableBuilder::commit(uint64_t batch, size_t expected_rows)
{
    if (!open_ || batch != open_batch_)
    {
        ++discarded_;
        return false;
    }

    open_ = false;

    if (spoiled_ || rows_.size() != expected_rows)
    {
        discarded_ += rows_.size();
        rows_.clear();
        return false;
    }

    return true;
}

} // namespace ipc
