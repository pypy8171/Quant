#include "core/TickCapture.h"

namespace feed
{
std::FILE* open_capture_file(const std::filesystem::path& file, const char* mode)
{
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfopen(file.c_str(), wide_mode.c_str());
#else
    return std::fopen(file.c_str(), mode);
#endif
}

uint16_t body_bytes_of(uint8_t kind)
{
    switch (kind)
    {
    case kKindTrade:
        return static_cast<uint16_t>(sizeof(TradeBody));

    case kKindBook:
        return static_cast<uint16_t>(sizeof(BookBody));

    case kKindBar:
        return static_cast<uint16_t>(sizeof(BarBody));

    case kKindUniverse:
        return static_cast<uint16_t>(sizeof(UniverseBody));

    default:
        return 0;
    }
}

void put_string(char* destination, size_t capture, std::string_view text)
{
    const size_t count = text.size() < capture - 1 ? text.size() : capture - 1;
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

void put_hhmmss(char* destination, int32_t hhmmss)
{
    for (int index = 5; index >= 0; --index)
    {
        destination[index] = static_cast<char>('0' + hhmmss % 10);
        hhmmss /= 10;
    }

    destination[6] = '\0';
}

void fill_common(Common& common, std::string_view ticker, int32_t hhmmss, uint32_t symbol_id, Market market,
                 int direction, int64_t received_ns, std::chrono::system_clock::time_point timestamp)
{
    common.received_ns = received_ns;
    common.wall_us = wall_us_of(timestamp);
    put_string(common.ticker, kTickerMax, ticker);
    put_hhmmss(common.time, hhmmss);
    common.symbol_id = symbol_id;
    common.market = static_cast<uint8_t>(market);
    common.direction = static_cast<uint8_t>(direction);
}

TradeBody to_body(const TradeData& trade)
{
    TradeBody trade_body;
    fill_common(trade_body.common, trade.ticker, trade.hhmmss, trade.symbol_id, trade.market, trade.direction,
                trade.received_ns, trade.timestamp);
    trade_body.price = trade.price;
    trade_body.quantity = trade.quantity;
    trade_body.strength = trade.strength;
    trade_body.accumulated_volume = trade.accumulated_volume;
    return trade_body;
}

BookBody to_body(const OrderBook& order_book)
{
    BookBody book_body;
    fill_common(book_body.common, order_book.ticker, order_book.hhmmss, order_book.symbol_id, Market::KR, 0,
                order_book.received_ns, order_book.timestamp);
    std::memcpy(book_body.asks, order_book.asks, sizeof(book_body.asks));
    std::memcpy(book_body.bids, order_book.bids, sizeof(book_body.bids));
    return book_body;
}

TradeData to_trade(const TradeBody& trade_body)
{
    TradeData trade;
    trade.ticker = trade_body.common.ticker;
    trade.hhmmss = krx::parse_hhmmss(trade_body.common.time);
    trade.symbol_id = trade_body.common.symbol_id;
    trade.market = static_cast<Market>(trade_body.common.market);
    trade.direction = trade_body.common.direction;
    trade.received_ns = trade_body.common.received_ns;
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(trade_body.common.wall_us));
    trade.price = trade_body.price;
    trade.quantity = trade_body.quantity;
    trade.strength = trade_body.strength;
    trade.accumulated_volume = trade_body.accumulated_volume;
    return trade;
}

OrderBook to_book(const BookBody& book_body)
{
    OrderBook order_book;
    order_book.ticker = book_body.common.ticker;
    order_book.hhmmss = krx::parse_hhmmss(book_body.common.time);
    order_book.symbol_id = book_body.common.symbol_id;
    order_book.received_ns = book_body.common.received_ns;
    order_book.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(book_body.common.wall_us));
    std::memcpy(order_book.asks, book_body.asks, sizeof(order_book.asks));
    std::memcpy(order_book.bids, book_body.bids, sizeof(order_book.bids));
    return order_book;
}

TickCapture::TickCapture(std::filesystem::path file, size_t queue_capacity)
    : path_(std::move(file)), queue_(queue_capacity)
{
    std::error_code error_code;
    std::filesystem::create_directories(path_.parent_path(), error_code);
    file_ = open_capture_file(path_, "ab");

    if (file_ == nullptr)
    {
        return;
    }

    // 새 파일에만 머리를 쓴다 — 같은 파일에 이어 쓰면(재기동) 레코드가 붙는다.
    if (std::filesystem::file_size(path_, error_code) == 0 && !error_code)
    {
        std::array<char, 16> head{};
        std::memcpy(head.data(), "QTCAP", 6);
        head[6] = static_cast<char>(kFormatVersion);
        const int64_t start_ms = wall_us_of(std::chrono::system_clock::now()) / 1000;
        std::memcpy(head.data() + 8, &start_ms, sizeof(start_ms));
        std::fwrite(head.data(), 1, head.size(), file_);
    }

    running_.store(true, std::memory_order_release);
    writer_ = std::thread([this] { writer_loop(); });
}

TickCapture::~TickCapture()
{
    if (file_ == nullptr)
    {
        return;
    }

    running_.store(false, std::memory_order_release);
    wake_.notify();
    writer_.join();
    std::fclose(file_);
}

void TickCapture::on_trade(const TradeData& trade) noexcept
{
    if (file_ == nullptr)
    {
        return;
    }

    Record record;
    record.kind = kKindTrade;
    record.trade = to_body(trade);
    enqueue(std::move(record));
}

void TickCapture::on_book(const OrderBook& order_book) noexcept
{
    if (file_ == nullptr)
    {
        return;
    }

    Record record;
    record.kind = kKindBook;
    record.book = to_body(order_book);
    enqueue(std::move(record));
}

void TickCapture::on_bar(const MarketData& bar, int32_t interval_sec) noexcept
{
    if (file_ == nullptr)
    {
        return;
    }

    Record record;
    record.kind = kKindBar;
    record.bar = BarBody{};
    record.bar.common.wall_us = wall_us_of(bar.timestamp);
    record.bar.common.symbol_id = bar.symbol_id;
    record.bar.common.market = static_cast<uint8_t>(bar.market);
    put_string(record.bar.common.ticker, kTickerMax, bar.ticker.string());
    record.bar.bar_index = bar.bar_index;
    record.bar.interval_sec = interval_sec;
    record.bar.open = bar.open;
    record.bar.high = bar.high;
    record.bar.low = bar.low;
    record.bar.close = bar.close;
    record.bar.volume = bar.volume;
    enqueue(std::move(record));
}

void TickCapture::on_universe(std::string_view ticker, uint8_t market, uint32_t symbol_id, bool trade_only) noexcept
{
    if (file_ == nullptr)
    {
        return;
    }

    Record record;
    record.kind = kKindUniverse;
    record.universe = UniverseBody{};
    record.universe.common.wall_us = wall_us_of(std::chrono::system_clock::now());
    record.universe.common.market = market;
    record.universe.common.symbol_id = symbol_id;
    put_string(record.universe.common.ticker, kTickerMax, ticker);
    record.universe.trade_only = trade_only ? 1 : 0;
    enqueue(std::move(record));
}

void TickCapture::flush()
{
    if (file_ == nullptr)
    {
        return;
    }

    while (written_.load(std::memory_order_acquire) + dropped_.load(std::memory_order_relaxed) <
           offered_.load(std::memory_order_relaxed))
    {
        wake_.notify();
        std::this_thread::yield();
    }

    flush_request_.store(true, std::memory_order_release);
    wake_.notify();

    while (flush_request_.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

void TickCapture::enqueue(Record&& record) noexcept
{
    offered_.fetch_add(1, std::memory_order_relaxed);

    if (!queue_.push(std::move(record)))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    wake_.notify();
}

void TickCapture::write_one(const Record& record)
{
    const uint16_t length = body_bytes_of(record.kind);

    if (length == 0)
    {
        return; // 쓰는 쪽이 모르는 종류를 만들었다는 뜻 — 파일에 길이 없는 레코드를 남기지 않는다.
    }

    const uint8_t header[4] = {static_cast<uint8_t>(length & 0xFF), static_cast<uint8_t>(length >> 8), record.kind,
                               kFormatVersion};
    std::fwrite(header, 1, sizeof(header), file_);
    // union 멤버들은 같은 자리에서 시작한다. 길이는 위에서 종류로 정했다.
    std::fwrite(static_cast<const void*>(&record.trade), 1, length, file_);
    written_.fetch_add(1, std::memory_order_release);
}

void TickCapture::writer_loop()
{
    using namespace std::chrono_literals;

    while (true)
    {
        bool drained = false;

        while (auto option = queue_.pop())
        {
            write_one(*option);
            drained = true;
        }

        if (flush_request_.load(std::memory_order_acquire))
        {
            std::fflush(file_);
            flush_request_.store(false, std::memory_order_release);
        }

        if (!running_.load(std::memory_order_acquire) && !drained)
        {
            std::fflush(file_);
            return;
        }

        if (!drained)
        {
            // 유휴면 stdio 버퍼를 비우고 잔다. 1초 상한은 종료 지연의 상한이지 깨우는 수단이 아니다.
            std::fflush(file_);
            wake_.wait_for(1s,
                           [this]
                           {
                               return queue_.empty() && running_.load(std::memory_order_acquire) &&
                                      !flush_request_.load(std::memory_order_acquire);
                           });
        }
    }
}

TickReader::TickReader(const std::filesystem::path& file)
{
    file_ = open_capture_file(file, "rb");

    if (file_ == nullptr)
    {
        return;
    }

    std::array<char, 16> head{};

    if (std::fread(head.data(), 1, head.size(), file_) != head.size() || std::memcmp(head.data(), "QTCAP", 6) != 0 ||
        static_cast<uint8_t>(head[6]) < kFormatVersionOldest || static_cast<uint8_t>(head[6]) > kFormatVersion)
    {
        std::fclose(file_);
        file_ = nullptr;
        return;
    }

    std::memcpy(&start_utc_ms_, head.data() + 8, sizeof(start_utc_ms_));
}

TickReader::~TickReader()
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
    }
}

bool TickReader::next(Record& out)
{
    while (true)
    {
        if (file_ == nullptr)
        {
            return false;
        }

        uint8_t header[4];

        if (std::fread(header, 1, sizeof(header), file_) != sizeof(header))
        {
            return stop();
        }

        const uint16_t length = static_cast<uint16_t>(header[0] | (header[1] << 8));
        const uint8_t kind = header[2];
        const uint16_t expected = body_bytes_of(kind);

        if (expected == 0)
        {
            // 모르는 종류. 길이가 터무니없으면 파일이 깨진 것으로 본다.
            if (length > kMaxRecordBytes || std::fseek(file_, length, SEEK_CUR) != 0)
            {
                return stop();
            }

            skipped_ += 1;
            continue;
        }

        if (length != expected)
        {
            return stop(); // 아는 종류인데 길이가 다르다 — 형식이 깨졌다.
        }

        if (std::fread(static_cast<void*>(&out.trade), 1, length, file_) != length)
        {
            return stop();
        }

        out.kind = kind;
        return true;
    }
}

bool TickReader::stop()
{
    std::fclose(file_);
    file_ = nullptr;
    return false;
}

} // namespace feed
