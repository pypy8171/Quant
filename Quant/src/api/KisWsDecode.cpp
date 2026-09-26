#include "api/KisWsDecode.h"

namespace kis_websocket
{
void split_fields(std::string_view text, char delim, std::vector<std::string_view>& out)
{
    out.clear();
    size_t start = 0;

    for (size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == delim)
        {
            out.emplace_back(text.data() + start, index - start);
            start = index + 1;
        }
    }

    out.emplace_back(text.data() + start, text.size() - start);
}

Records split_records(Fields fields, int count, size_t min_fields) noexcept
{
    Records out;

    if (count <= 1 || fields.empty())
    {
        return out;
    }

    const size_t total = static_cast<size_t>(count);

    if (fields.size() % total != 0)
    {
        return out;
    }

    const size_t width = fields.size() / total;

    if (width < min_fields || width == 0)
    {
        return out;
    }

    out.all = fields;
    out.width = width;
    out.count = total;
    return out;
}

} // namespace kis_websocket

namespace kis_websocket
{
namespace detail
{
bool to_double(std::string_view text, double& out) noexcept
{
    const char* begin = number_begin(text);
    const char* other_end = text.data() + text.size();

    if (begin == other_end)
    {
        return false;
    }

#if defined(__cpp_lib_to_chars)
    double item = 0.0;
    auto parse_result = std::from_chars(begin, other_end, item);

    if (parse_result.ec != std::errc() || parse_result.ptr != other_end)
    {
        return false;
    }

    out = item;
    return true;
#else
    // strtod는 앞 공백을 건너뛰고 로케일을 보며 널 종료를 요구한다 — KIS 전문은 ASCII 숫자만 오므로
    //  스택 버퍼에 옮겨 끝 포인터만 확인한다. 32자를 넘는 숫자 필드는 없다.
    char buffer[32];
    const size_t length = static_cast<size_t>(other_end - begin);

    if (length >= sizeof(buffer))
    {
        return false;
    }

    std::memcpy(buffer, begin, length);
    buffer[length] = '\0';
    char* end = nullptr;
    double value = std::strtod(buffer, &end);

    if (end != buffer + length)
    {
        return false;
    }

    out = value;
    return true;
#endif
}

bool fill_levels(Fields fields, size_t ask_price, size_t ask_quantity, size_t bid_price, size_t bid_quantity,
                 OrderBook& order_book)
{
    bool ok = true;

    for (size_t index = 0; index < 5; ++index)
    {
        ok &= to_double(fields[ask_price + index], order_book.asks[index].price);
        ok &= to_i64(fields[ask_quantity + index], order_book.asks[index].quantity);
        ok &= to_double(fields[bid_price + index], order_book.bids[index].price);
        ok &= to_i64(fields[bid_quantity + index], order_book.bids[index].quantity);
    }

    return ok;
}

} // namespace detail
} // namespace kis_websocket

namespace kis_websocket
{
Decode decode_orderbook(Fields fields, OrderBook& order_book)
{
    if (fields.size() < kMinFieldsOrderbook)
    {
        return Decode::kShort;
    }

    order_book.ticker = fields[0];
    order_book.hhmmss = krx::parse_hhmmss(fields[1]);
    order_book.timestamp = std::chrono::system_clock::now();
    return detail::fill_levels(fields, 3, 23, 13, 33, order_book) ? Decode::kOk : Decode::kBadNumber;
}

Decode decode_kr_trade(Fields fields, TradeData& trade)
{
    if (fields.size() < kMinFieldsKrTrade)
    {
        return Decode::kShort;
    }

    trade.ticker = fields[0];
    trade.hhmmss = krx::parse_hhmmss(fields[1]);
    trade.market = Market::KR;
    trade.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_double(fields[2], trade.price);
    ok &= detail::to_i64(fields[12], trade.quantity);
    ok &= detail::to_int(fields[21], trade.direction);

    if (!detail::to_i64(fields[13], trade.accumulated_volume))
    {
        trade.accumulated_volume = 0;
    }

    if (!detail::to_double(fields[18], trade.strength))
    {
        trade.strength = 0.0;
    }

    return ok ? Decode::kOk : Decode::kBadNumber;
}

Decode decode_us_trade(Fields fields, TradeData& trade)
{
    if (fields.size() < kMinFieldsUsTrade)
    {
        return Decode::kShort;
    }

    trade.ticker = fields[0];
    trade.hhmmss = krx::parse_hhmmss(fields[1]);
    trade.market = Market::US;
    trade.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_double(fields[2], trade.price);
    ok &= detail::to_i64(fields[8], trade.quantity);
    trade.direction = 0;

    if (fields.size() > 20)
    {
        ok &= detail::to_int(fields[20], trade.direction);
    }

    return ok ? Decode::kOk : Decode::kBadNumber;
}

Decode decode_fill(Fields fields, FillNotification& fill_notification)
{
    if (fields.size() < kMinFieldsFill)
    {
        return Decode::kShort;
    }

    if (fields[13] != "2")
    {
        return Decode::kSkip;
    }

    if (fields[4] == "02")
    {
        fill_notification.side = OrderSide::BUY;
    }
    else if (fields[4] == "01")
    {
        fill_notification.side = OrderSide::SELL;
    }
    else
    {
        return Decode::kBadSide;
    }

    fill_notification.kis_order_no      = fields[2];
    fill_notification.original_order_no = fields[3];
    fill_notification.ticker            = fields[8];
    fill_notification.fill_time         = fields[11];
    fill_notification.timestamp         = std::chrono::system_clock::now();

    // 보조 필드는 있으면 채우고 없으면 "모른다"로 둔다. 호출자가 같은 구조체를 돌려 쓰므로
    //  앞 레코드의 값이 남지 않게 먼저 비운다.
    fill_notification.order_quantity = 0;
    fill_notification.exchange.clear();

    if (fields.size() > kFillFieldOrderQuantity)
    {
        (void)detail::to_int(fields[kFillFieldOrderQuantity], fill_notification.order_quantity);
    }

    if (fields.size() > kFillFieldExchange)
    {
        fill_notification.exchange = fields[kFillFieldExchange];
    }

    bool ok = detail::to_int(fields[9], fill_notification.filled_quantity);
    ok &= detail::to_double(fields[10], fill_notification.filled_price);
    return ok ? Decode::kOk : Decode::kBadNumber;
}

} // namespace kis_websocket
