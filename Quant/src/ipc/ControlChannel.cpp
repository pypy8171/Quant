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
} // namespace

void set_account(ControlRequest& request, std::string_view account) noexcept
{
    const size_t length = std::min(account.size(), kControlAccountMax - 1);
    std::memcpy(request.account, account.data(), length);
    request.account[length] = '\0';
}

std::string_view account_of(const ControlRequest& request) noexcept
{
    // 채우는 쪽이 끝에 0을 넣지만, 통로 저쪽에서 온 칸은 믿지 않고 칸 안에서 끝을 찾는다. [inv]
    const char* end = static_cast<const char*>(std::memchr(request.account, '\0', kControlAccountMax));
    const size_t length = end != nullptr ? static_cast<size_t>(end - request.account) : kControlAccountMax;
    return std::string_view(request.account, length);
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
