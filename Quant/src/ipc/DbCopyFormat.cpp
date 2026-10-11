// DbManager COPY 글 만들기 — 체결·신호·헬스 한 행을 COPY 텍스트 형식으로 붙이는 공개 함수와 그 숫자 조각.
//  libpq를 쓰지 않는다. Quant/tests/test_db_manager.cpp가 이 함수들을 바로 부른다. [why D-148]
#include "ipc/DbManager.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>

namespace db
{
namespace
{

void append_digits(std::string& out, int64_t value, int width)
{
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    const auto length = static_cast<int>(result.ptr - buffer);

    for (int pad = length; pad < width; ++pad)
    {
        out.push_back('0');
    }

    out.append(buffer, result.ptr);
}

template <typename Number>
void append_number(std::string& out, Number value)
{
    char buffer[40];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

// 값이 있으면 숫자, 없으면 COPY의 NULL(\N).
void append_optional(std::string& out, bool present, uint64_t value)
{
    if (present)
    {
        append_number(out, value);
        return;
    }

    out.append("\\N");
}

// health의 고정 수치 열. 순서는 append_health_row가 값을 쓰는 순서와 같다. [wire] PYQuant/db/client.py _HEALTH_METRIC_COLUMNS
constexpr std::array<std::string_view, 27> kHealthFixedColumns = {
    "drop_cnt",
    "drop_socket_full",
    "drop_socket_error",
    "drop_send_queue_full",
    "drop_trade_ring_full",
    "queue_shard_high_water",
    "queue_shard_capacity",
    "queue_shard_out_size",
    "queue_shard_out_capacity",
    "queue_order_high_water",
    "queue_order_capacity",
    "queue_fill_high_water",
    "queue_fill_capacity",
    "dropped_shard",
    "dropped_order",
    "stale_order",
    "dropped_fill",
    "latency_samples",
    "tick_to_signal_p50_us",
    "tick_to_signal_p99_us",
    "signal_to_pop_p50_us",
    "signal_to_pop_p99_us",
    "pop_to_done_p50_us",
    "pop_to_done_p99_us",
    "total_p50_us",
    "total_p99_us",
    "latency_interval_samples",
};

} // namespace

void append_copy_text(std::string& out, std::string_view text)
{
    for (const char character : text)
    {
        switch (character)
        {
        case '\\':
            out.append("\\\\");
            break;

        case '\t':
            out.append("\\t");
            break;

        case '\n':
            out.append("\\n");
            break;

        case '\r':
            out.append("\\r");
            break;

        default:
            out.push_back(character);
        }
    }
}

void append_iso_utc(std::string& out, int64_t epoch_ms)
{
    using namespace std::chrono;
    const sys_time<milliseconds> time_point{milliseconds(epoch_ms)};
    const sys_days               day = floor<days>(time_point);
    const year_month_day         date{day};
    const hh_mm_ss<milliseconds> clock{time_point - day};

    append_digits(out, static_cast<int>(date.year()), 4);
    out.push_back('-');
    append_digits(out, static_cast<unsigned>(date.month()), 2);
    out.push_back('-');
    append_digits(out, static_cast<unsigned>(date.day()), 2);
    out.push_back('T');
    append_digits(out, clock.hours().count(), 2);
    out.push_back(':');
    append_digits(out, clock.minutes().count(), 2);
    out.push_back(':');
    append_digits(out, clock.seconds().count(), 2);
    out.push_back('.');
    append_digits(out, clock.subseconds().count(), 3);
    out.append("+00:00");
}

void append_trade_row(std::string& out, const TradeData& trade, int64_t enqueue_ms)
{
    append_iso_utc(out, enqueue_ms);
    out.push_back('\t');
    append_copy_text(out, trade.ticker.view());
    out.push_back('\t');
    append_number(out, trade.price);
    out.push_back('\t');
    append_number(out, trade.quantity);
    out.push_back('\t');
    append_number(out, trade.direction);
    out.push_back('\t');
    out.append(trade.market == Market::US ? "US" : "KR");
    out.push_back('\n');
}

SignalRow make_signal_row(const OrderSignal& signal, std::string_view regime, int64_t ts_ms)
{
    SignalRow row;
    row.ts_ms = ts_ms;
    row.ticker = signal.ticker;
    const size_t length = std::min(signal.strategy_id.size(), SignalRow::kStrategyMax);
    std::copy_n(signal.strategy_id.data(), length, row.strategy);
    row.strategy_length = static_cast<uint8_t>(length);
    row.side = signal.side;
    row.quantity = signal.quantity;
    row.price = signal.price;
    row.market = signal.market;
    row.regime = regime;
    return row;
}

void append_signal_row(std::string& out, const SignalRow& row, std::string_view account)
{
    append_iso_utc(out, row.ts_ms);
    out.push_back('\t');
    append_copy_text(out, std::string_view(row.strategy, row.strategy_length));
    out.push_back('\t');
    append_copy_text(out, row.ticker.view());
    out.push_back('\t');
    out.append(row.side == OrderSide::BUY ? "BUY" : (row.side == OrderSide::SELL ? "SELL" : "NONE"));
    out.push_back('\t');
    append_number(out, row.quantity);
    out.push_back('\t');
    append_number(out, row.price);
    out.push_back('\t');
    out.append(row.market == Market::US ? "US" : "KR");
    out.push_back('\t');
    append_copy_text(out, row.regime);
    out.push_back('\t');
    append_copy_text(out, account);
    out.push_back('\n');
}

std::vector<std::string> health_metric_columns(const HealthSnapshot& snapshot)
{
    std::vector<std::string> columns(kHealthFixedColumns.begin(), kHealthFixedColumns.end());

    for (const auto& segment : snapshot.interval_segments)
    {
        if (segment.name.empty())
        {
            continue;
        }

        columns.emplace_back(segment.name).append("_p50_interval_us");
        columns.emplace_back(segment.name).append("_p99_interval_us");
    }

    return columns;
}

void append_health_row(std::string& out, const HealthSnapshot& snapshot, int64_t ts_ms, std::string_view role,
                       std::string_view account)
{
    const auto field = [&out](auto value)
    {
        out.push_back('\t');
        append_number(out, value);
    };
    const auto optional_field = [&out, &snapshot](uint64_t value)
    {
        out.push_back('\t');
        append_optional(out, snapshot.has_publish_drops, value);
    };

    append_iso_utc(out, ts_ms);
    out.push_back('\t');
    append_copy_text(out, role);
    out.push_back('\t');
    append_copy_text(out, account);
    field(snapshot.data_count);
    field(snapshot.signal_count);
    field(snapshot.order_count);
    // 순서는 kHealthFixedColumns와 같다.
    optional_field(snapshot.publish_dropped);
    optional_field(snapshot.drop_socket_full);
    optional_field(snapshot.drop_socket_error);
    optional_field(snapshot.drop_send_queue_full);
    optional_field(snapshot.drop_trade_ring_full);
    field(snapshot.shard_high_water);
    field(snapshot.shard_capacity);
    field(snapshot.shard_out_size);
    field(snapshot.shard_out_capacity);
    field(snapshot.order_queue_high_water);
    field(snapshot.order_queue_capacity);
    field(snapshot.fill_queue_high_water);
    field(snapshot.fill_queue_capacity);
    field(snapshot.shard_dropped);
    field(snapshot.order_dropped);
    field(snapshot.order_stale);
    field(snapshot.fill_dropped);
    field(snapshot.latency_samples);
    field(snapshot.tick_to_signal_p50_us);
    field(snapshot.tick_to_signal_p99_us);
    field(snapshot.signal_to_pop_p50_us);
    field(snapshot.signal_to_pop_p99_us);
    field(snapshot.pop_to_done_p50_us);
    field(snapshot.pop_to_done_p99_us);
    field(snapshot.total_p50_us);
    field(snapshot.total_p99_us);
    field(snapshot.interval_samples);

    for (const auto& segment : snapshot.interval_segments)
    {
        if (segment.name.empty())
        {
            continue;
        }

        field(segment.p50_us);
        field(segment.p99_us);
    }

    out.push_back('\n');
}

} // namespace db
