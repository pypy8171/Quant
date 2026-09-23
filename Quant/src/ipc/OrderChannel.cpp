#include "ipc/OrderChannel.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace ipc
{
namespace
{

// 고정 칸에 글자를 옮긴다. 칸을 넘으면 자르되 **글자 경계에서** 자른다 — UTF-8 한글은 한 글자가 세 바이트라
//  바이트로 끊으면 반쪽 글자가 남고, 그 글을 그대로 싣는 원장 CSV·로그가 깨진다. 항상 0으로 끝낸다.
//  잘렸으면 참을 준다.
bool copy_text(char* destination, size_t capacity, std::string_view text) noexcept
{
    size_t      length = std::min(text.size(), capacity - 1);
    const bool  cut    = length < text.size();

    if (cut)
    {
        // 자를 자리가 글자 가운데(10xxxxxx)면 그 글자가 시작하는 자리까지 물러선다.
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
    const void* end = std::memchr(field, '\0', capacity);
    const size_t length = (end != nullptr) ? static_cast<size_t>(static_cast<const char*>(end) - field) : capacity;
    return std::string_view(field, length);
}

} // namespace

bool is_plausible(const OrderRequest& request, const RequestLimits& limits) noexcept
{
    if (request.sequence == 0 || request.sent_at_ns <= 0)
    {
        return false;
    }

    // 종목·전략 번호는 표 안의 자리를 가리킨다. 표 밖이면 그 값으로 배열을 짚는 순간 끝이다.
    //  아직 표에 없는 종목은 번호가 kNone 이고 코드 글자로 온다 — 받는 쪽이 그 글자로 표에 올린다.
    if (request.symbol_id > limits.symbol_count)
    {
        return false;
    }

    if (request.symbol_id == symbol::kNone && request.ticker.length == 0)
    {
        return false;
    }

    if (request.ticker.length > symbol::Ticker::kMax)
    {
        return false;
    }

    if (request.strategy_index == strategy_table::kNone || request.strategy_index > limits.strategy_count)
    {
        return false;
    }

    if (request.action > static_cast<uint8_t>(OrderAction::REPLACE))
    {
        return false;
    }

    if (request.quantity < 0 || request.quantity > limits.quantity_max)
    {
        return false;
    }

    // 신규 주문만 수량과 방향을 요구한다. 취소는 수량이 0이고 방향이 NONE 이어도 맞는 주문이다 —
    //  대상은 원주문 번호로 찾고, 수량·방향은 참고 값이다. [why D-114]
    if (request.action == static_cast<uint8_t>(OrderAction::NEW))
    {
        if (request.quantity <= 0)
        {
            return false;
        }

        if (request.side != OrderSide::BUY && request.side != OrderSide::SELL)
        {
            return false;
        }
    }
    else if (request.original_client_order_number == 0 && request.original_client_order_id[0] == '\0')
    {
        // 취소·정정인데 대상이 없다 — 이 줄로는 무엇을 거둘지 정할 수 없다.
        return false;
    }
    else if (request.side > OrderSide::NONE)
    {
        return false;
    }

    // 시장가는 가격이 0이다. 음수·NaN·무한대는 비교가 전부 거짓이라 뒤집어 본다.
    if (!(request.price >= 0.0) || request.price > limits.price_max)
    {
        return false;
    }

    if (!(request.reference_price >= 0.0) || request.reference_price > limits.price_max)
    {
        return false;
    }

    if (request.order_type > static_cast<uint8_t>(OrderType::LIMIT))
    {
        return false;
    }

    if (request.market > static_cast<uint8_t>(Market::US))
    {
        return false;
    }

    // 글자 칸은 모두 칸 안에서 끝나야 한다 — 하나라도 안 끝나면 읽다가 칸을 넘는다.
    if (!is_terminated(request.exchange, kExchangeMax) || !is_terminated(request.account_id, kAccountIdMax) ||
        !is_terminated(request.client_order_id, kClientOrderIdMax) ||
        !is_terminated(request.original_client_order_id, kClientOrderIdMax) ||
        !is_terminated(request.reason, kSignalReasonMax))
    {
        return false;
    }

    return true;
}

bool is_plausible(const OrderResponse& response) noexcept
{
    if (response.sequence == 0)
    {
        return false;
    }

    if (response.result < static_cast<uint8_t>(OrderResult::kAccepted) ||
        response.result > static_cast<uint8_t>(OrderResult::kInvalid))
    {
        return false;
    }

    // 사유 칸이 칸 안에서 끝나야 한다 — 끝나지 않으면 읽는 쪽이 칸을 넘어 읽는다.
    if (std::memchr(response.reason, '\0', kOrderReasonMax) == nullptr)
    {
        return false;
    }

    return true;
}

OrderRequest to_request(const OrderSignal& signal, bool* truncated) noexcept
{
    OrderRequest request;
    request.sequence                     = signal.sequence;
    request.sent_at_ns                   = signal.signal_at_ns;
    request.tick_at_ns                   = signal.tick_at_ns;
    request.timestamp_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(signal.timestamp.time_since_epoch()).count();
    request.client_order_number          = signal.client_order_number;
    request.original_client_order_number = signal.original_client_order_number;
    request.price                        = signal.price;
    request.reference_price              = signal.reference_price;
    request.symbol_id                    = signal.symbol_id;
    request.strategy_index               = signal.strategy_index;
    request.quantity                     = signal.quantity;
    request.side                         = static_cast<uint8_t>(signal.side);
    request.order_type                   = static_cast<uint8_t>(signal.type);
    request.action                       = static_cast<uint8_t>(signal.action);
    request.market                       = static_cast<uint8_t>(signal.market);
    request.ticker                       = signal.ticker;

    bool cut = copy_text(request.exchange, kExchangeMax, signal.exchange);
    cut      = copy_text(request.account_id, kAccountIdMax, signal.account_id) || cut;
    cut      = copy_text(request.client_order_id, kClientOrderIdMax, signal.client_order_id) || cut;
    cut      = copy_text(request.original_client_order_id, kClientOrderIdMax, signal.original_client_order_id) || cut;
    cut      = copy_text(request.reason, kSignalReasonMax, signal.reason) || cut;

    if (truncated != nullptr)
    {
        *truncated = cut;
    }

    return request;
}

OrderSignal to_signal(const OrderRequest& request, std::string_view strategy_id)
{
    OrderSignal signal;
    signal.ticker          = request.ticker.string();
    signal.symbol_id       = request.symbol_id;
    signal.side            = static_cast<OrderSide::Value>(request.side);
    signal.type            = static_cast<OrderType>(request.order_type);
    signal.quantity        = request.quantity;
    signal.price           = request.price;
    signal.reference_price = request.reference_price;
    signal.strategy_id     = std::string(strategy_id);
    signal.strategy_index  = request.strategy_index;
    signal.market          = static_cast<Market>(request.market);
    signal.exchange        = std::string(text_of(request.exchange, kExchangeMax));
    // system_clock 의 눈금은 나노초가 아니다(MSVC는 100나노초) — 눈금을 맞춰 넣는다. 눈금보다 작은 자리는 버려진다.
    signal.timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(request.timestamp_ns)));
    signal.account_id                   = std::string(text_of(request.account_id, kAccountIdMax));
    signal.action                       = static_cast<OrderAction>(request.action);
    signal.client_order_id              = std::string(text_of(request.client_order_id, kClientOrderIdMax));
    signal.original_client_order_id     = std::string(text_of(request.original_client_order_id, kClientOrderIdMax));
    signal.client_order_number          = request.client_order_number;
    signal.original_client_order_number = request.original_client_order_number;
    signal.reason                       = std::string(text_of(request.reason, kSignalReasonMax));
    signal.tick_at_ns                   = request.tick_at_ns;
    signal.signal_at_ns                 = request.sent_at_ns;
    signal.sequence                     = request.sequence;
    return signal;
}

uint64_t to_order_number(std::string_view kis_order_no) noexcept
{
    uint64_t number = 0;

    for (const char character : kis_order_no)
    {
        if (character < '0' || character > '9')
        {
            return 0;
        }

        number = number * 10 + static_cast<uint64_t>(character - '0');
    }

    return number;
}

OrderResponse make_response(uint64_t sequence, OrderResult result, uint64_t kis_order_number,
                            std::string_view reason, int64_t handled_at_ns) noexcept
{
    OrderResponse response;
    response.sequence         = sequence;
    response.kis_order_number = kis_order_number;
    response.handled_at_ns    = handled_at_ns;
    response.result           = static_cast<uint8_t>(result);

    // 칸을 넘치면 자르고 항상 0으로 끝낸다 — 받는 쪽이 길이 없이 읽는다.
    (void)copy_text(response.reason, kOrderReasonMax, reason);
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────

PendingRequests::PendingRequests(size_t capacity) : capacity_(std::max<size_t>(capacity, 1))
{
    entries_.reserve(capacity_);
}

void PendingRequests::note_sent(uint64_t sequence, int64_t now_ns)
{
    if (sequence == 0)
    {
        return; // 순번 없는 요청은 답을 맞출 수 없다
    }

    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [sequence](const Entry& entry) { return entry.sequence == sequence; });

    if (found != entries_.end())
    {
        found->sent_at_ns = now_ns; // 재전송 — 기다린 시간을 다시 센다
        return;
    }

    if (entries_.size() >= capacity_)
    {
        // 상한을 넘겼다. 가장 오래된 것을 버린다 — 주문 쪽이 답을 못 주는 동안 여기가 자라면
        //  전략 스레드가 메모리로 벌을 받는다. 버린 수는 밖에서 본다.
        entries_.erase(entries_.begin());
        ++evicted_;
    }

    entries_.push_back(Entry{sequence, now_ns});
}

bool PendingRequests::note_response(uint64_t sequence)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [sequence](const Entry& entry) { return entry.sequence == sequence; });

    if (found == entries_.end())
    {
        return false;
    }

    entries_.erase(found);
    return true;
}

std::optional<uint64_t> PendingRequests::oldest_overdue(int64_t now_ns, int64_t timeout_ns) const
{
    const Entry* oldest = nullptr;

    for (const auto& entry : entries_)
    {
        if (now_ns - entry.sent_at_ns < timeout_ns)
        {
            continue;
        }

        if (oldest == nullptr || entry.sent_at_ns < oldest->sent_at_ns)
        {
            oldest = &entry;
        }
    }

    if (oldest == nullptr)
    {
        return std::nullopt;
    }

    return oldest->sequence;
}

// ─────────────────────────────────────────────────────────────────────────────

DuplicateFilter::DuplicateFilter(size_t window) : seen_(std::max<size_t>(window, 1), 0)
{
}

bool DuplicateFilter::accept(uint64_t sequence)
{
    if (sequence == 0)
    {
        return false;
    }

    // 흔한 길: 전략이 순번을 단조 증가로 찍으므로 새 요청은 거의 언제나 지금까지 본 것보다 크다.
    if (sequence > highest_seen_)
    {
        highest_seen_    = sequence;
        seen_[write_index_] = sequence;
        write_index_     = (write_index_ + 1) % seen_.size();
        return true;
    }

    // 뒤처진 순번이면 창 안에 있었는지 본다.
    if (std::find(seen_.begin(), seen_.end(), sequence) != seen_.end())
    {
        ++duplicates_;
        return false;
    }

    // 창보다 오래된 순번이다. 생산자가 하나라 이만큼 밀린 요청은 정상 경로에서 나오지 않는다 —
    //  판정할 근거가 없으니 거른다. 같은 순번을 두 번 내는 것보다 한 번을 놓치는 쪽이 낫다. [inv]
    ++duplicates_;
    return false;
}

} // namespace ipc
