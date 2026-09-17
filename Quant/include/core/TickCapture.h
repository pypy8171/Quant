// 실시간 틱·호가의 append-only 이진 캡처와 그 리더. 수신 스레드는 고정 크기 레코드를 MPSC 큐에 넣기만 하고
//  파일 쓰기는 기록 스레드가 한다. 리플레이 백테스트의 입력이 이 파일이다. [why D-071]
//  스레드: on_trade/on_book은 WS 수신 스레드 여럿(FeedMux 레인마다 하나)이 부른다 — 생산자가 여럿이라 큐는 MpscQueue
//  (원칙 5). 기록 스레드 하나가 pop. TickReader는 단일 스레드용.
#pragma once

#include "core/MarketSession.h"
#include "core/MpscQueue.h"
#include "core/Types.h"
#include "core/WakeGate.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>

namespace feed
{

// ── 파일 형식 v1 ─────────────────────────────────────────────────────────────
//  머리 16바이트: "QTCAP\0" + ver(uint8=1) + pad(1) + 시작 utc_ms(int64, LE).
//  레코드: uint16 length(본문 바이트) + uint8 kind(1=체결 2=호가) + uint8 ver(1) + 본문. 본문은 아래 POD를 그대로 쓴다
//  (LE, x64 정렬 그대로). 꼬리가 잘려 있으면 리더가 그 앞까지만 돌려준다.
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kKindTrade     = 1;
constexpr uint8_t kKindBook      = 2;
constexpr size_t  kTickerMax     = 12; // KIS 현물 6자리·선물 8자리·미국 티커. 넘치면 잘린다.
constexpr size_t  kTimeMax       = 8;  // HHMMSS

struct Common
{
    int64_t  received_ns = 0; // 수신 스레드 steady_clock ns (TradeData.received_ns·OrderBook.received_ns).
    int64_t  wall_us = 0; // system_clock us — 리플레이의 timestamp 복원용
    char     ticker[kTickerMax] = {};
    char     time[kTimeMax]     = {};
    uint32_t symbol_id       = 0; // 캡처 시점 id. 기동마다 달라지므로 리플레이는 ticker로 다시 등록한다.
    uint8_t  market    = 0; // Market enum 값
    uint8_t  direction = 0; // 1=매수 5=매도 0=없음
    uint8_t  pad[6]    = {}; // 8바이트 정렬 채움
};

struct TradeBody
{
    Common  common;
    double  price       = 0.0;
    int64_t quantity    = 0;
    double  strength    = 0.0;
    int64_t accumulated_volume = 0;
};

struct BookBody
{
    Common         common;
    OrderBookLevel asks[5];
    OrderBookLevel bids[5];
};

static_assert(std::is_trivially_copyable_v<TradeBody> && std::is_trivially_copyable_v<BookBody>);
static_assert(sizeof(Common) == 48 && sizeof(TradeBody) == 80 && sizeof(BookBody) == 208,
              "파일 형식 v1의 본문 크기 — 바뀌면 kFormatVersion을 올린다");

// 큐 원소. 호가 크기라 체결도 200바이트를 차지하지만 큐는 메모리라 상관없고, 파일에는 kind에 맞는 길이만 쓴다.
struct Record
{
    uint8_t kind = 0;
    union
    {
        TradeBody trade;
        BookBody  book;
    };

    Record() : trade{}
    {
    }
};

inline int64_t wall_us_of(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

inline void put_str(char* dst, size_t capture, std::string_view text)
{
    const size_t count = text.size() < capture - 1 ? text.size() : capture - 1;
    std::memcpy(dst, text.data(), count);
    dst[count] = '\0';
}

// 파일 형식은 그대로 "HHMMSS" 문자다(정수화 전 캡처와 호환). 여섯 자리를 손으로 찍는다 — 수신 스레드라 할당이 없다.
inline void put_hhmmss(char* dst, int32_t hhmmss)
{
    for (int index = 5; index >= 0; --index)
    {
        dst[index] = static_cast<char>('0' + hhmmss % 10);
        hhmmss /= 10;
    }

    dst[6] = '\0';
}

inline void fill_common(Common& common, std::string_view ticker, int32_t hhmmss, uint32_t symbol_id, Market market,
                        int direction, int64_t received_ns, std::chrono::system_clock::time_point timestamp)
{
    common.received_ns = received_ns;
    common.wall_us = wall_us_of(timestamp);
    put_str(common.ticker, kTickerMax, ticker);
    put_hhmmss(common.time, hhmmss);
    common.symbol_id       = symbol_id;
    common.market    = static_cast<uint8_t>(market);
    common.direction = static_cast<uint8_t>(direction);
}

inline TradeBody to_body(const TradeData& trade)
{
    TradeBody trade_body;
    fill_common(trade_body.common, trade.ticker, trade.hhmmss, trade.symbol_id, trade.market, trade.direction, trade.received_ns, trade.timestamp);
    trade_body.price       = trade.price;
    trade_body.quantity    = trade.quantity;
    trade_body.strength    = trade.strength;
    trade_body.accumulated_volume = trade.accumulated_volume;
    return trade_body;
}

inline BookBody to_body(const OrderBook& order_book)
{
    BookBody book_body;
    fill_common(book_body.common, order_book.ticker, order_book.hhmmss, order_book.symbol_id, Market::KR, 0, order_book.received_ns, order_book.timestamp);
    std::memcpy(book_body.asks, order_book.asks, sizeof(book_body.asks));
    std::memcpy(book_body.bids, order_book.bids, sizeof(book_body.bids));
    return book_body;
}

inline TradeData to_trade(const TradeBody& trade_body)
{
    TradeData trade;
    trade.ticker      = trade_body.common.ticker;
    trade.hhmmss      = krx::parse_hhmmss(trade_body.common.time);
    trade.symbol_id         = trade_body.common.symbol_id;
    trade.market      = static_cast<Market>(trade_body.common.market);
    trade.direction   = trade_body.common.direction;
    trade.received_ns     = trade_body.common.received_ns;
    trade.timestamp   = std::chrono::system_clock::time_point(std::chrono::microseconds(trade_body.common.wall_us));
    trade.price       = trade_body.price;
    trade.quantity    = trade_body.quantity;
    trade.strength    = trade_body.strength;
    trade.accumulated_volume = trade_body.accumulated_volume;
    return trade;
}

inline OrderBook to_book(const BookBody& book_body)
{
    OrderBook order_book;
    order_book.ticker    = book_body.common.ticker;
    order_book.hhmmss    = krx::parse_hhmmss(book_body.common.time);
    order_book.symbol_id       = book_body.common.symbol_id;
    order_book.received_ns   = book_body.common.received_ns;
    order_book.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(book_body.common.wall_us));
    std::memcpy(order_book.asks, book_body.asks, sizeof(order_book.asks));
    std::memcpy(order_book.bids, book_body.bids, sizeof(order_book.bids));
    return order_book;
}

// ── 기록기 ───────────────────────────────────────────────────────────────────
class TickCapture
{
public:
    // 파일을 열지 못하면 ok()가 false고 on_*는 아무것도 하지 않는다 — 캡처 실패가 매매를 막지 않는다.
    explicit TickCapture(std::filesystem::path file, size_t queue_capacity = 1u << 16)
        : path_(std::move(file)), q_(queue_capacity)
    {
        std::error_code error_code;
        std::filesystem::create_directories(path_.parent_path(), error_code);
        fp_ = std::fopen(path_.string().c_str(), "ab");

        if (fp_ == nullptr)
        {
            return;
        }

        // 새 파일에만 머리를 쓴다 — 같은 파일에 이어 쓰면(재기동) 레코드가 붙는다.
        if (std::filesystem::file_size(path_, error_code) == 0 && !error_code)
        {
            std::array<char, 16> head{};
            std::memcpy(head.data(), "QTCAP", 6);
            head[6]                = static_cast<char>(kFormatVersion);
            const int64_t start_ms = wall_us_of(std::chrono::system_clock::now()) / 1000;
            std::memcpy(head.data() + 8, &start_ms, sizeof(start_ms));
            std::fwrite(head.data(), 1, head.size(), fp_);
        }

        running_.store(true, std::memory_order_release);
        writer_ = std::thread([this] { writer_loop(); });
    }

    ~TickCapture()
    {
        if (fp_ == nullptr)
        {
            return;
        }

        running_.store(false, std::memory_order_release);
        wake_.notify();
        writer_.join();
        std::fclose(fp_);
    }

    TickCapture(const TickCapture&)            = delete;
    TickCapture& operator=(const TickCapture&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return fp_ != nullptr;
    }

    // 생산자(수신 스레드). 큐가 차면 버리고 센다 — 캡처 때문에 틱 경로가 서지 않는다.
    void on_trade(const TradeData& trade) noexcept
    {
        if (fp_ == nullptr)
        {
            return;
        }

        Record record;
        record.kind  = kKindTrade;
        record.trade = to_body(trade);
        enqueue(std::move(record));
    }

    void on_book(const OrderBook& order_book) noexcept
    {
        if (fp_ == nullptr)
        {
            return;
        }

        Record record;
        record.kind = kKindBook;
        record.book = to_body(order_book);
        enqueue(std::move(record));
    }

    // 큐가 빌 때까지 기다리고 파일을 flush한다. 종료·테스트용 — hot path에서 부르지 않는다.
    void flush()
    {
        if (fp_ == nullptr)
        {
            return;
        }

        while (written_.load(std::memory_order_acquire) + dropped_.load(std::memory_order_relaxed) <
               offered_.load(std::memory_order_relaxed))
        {
            wake_.notify();
            std::this_thread::yield();
        }

        flush_req_.store(true, std::memory_order_release);
        wake_.notify();

        while (flush_req_.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }

    [[nodiscard]] uint64_t written() const noexcept
    {
        return written_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t dropped() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    void enqueue(Record&& record) noexcept
    {
        offered_.fetch_add(1, std::memory_order_relaxed);

        if (!q_.push(std::move(record)))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        wake_.notify();
    }

    void write_one(const Record& record)
    {
        const uint16_t length = record.kind == kKindTrade ? static_cast<uint16_t>(sizeof(TradeBody))
                                                  : static_cast<uint16_t>(sizeof(BookBody));
        const uint8_t  hdr[4] = {static_cast<uint8_t>(length & 0xFF), static_cast<uint8_t>(length >> 8), record.kind,
                                 kFormatVersion};
        std::fwrite(hdr, 1, sizeof(hdr), fp_);
        std::fwrite(record.kind == kKindTrade ? static_cast<const void*>(&record.trade) : static_cast<const void*>(&record.book), 1,
                    length, fp_);
        written_.fetch_add(1, std::memory_order_release);
    }

    void writer_loop()
    {
        using namespace std::chrono_literals;

        while (true)
        {
            bool did = false;

            while (auto option = q_.pop())
            {
                write_one(*option);
                did = true;
            }

            if (flush_req_.load(std::memory_order_acquire))
            {
                std::fflush(fp_);
                flush_req_.store(false, std::memory_order_release);
            }

            if (!running_.load(std::memory_order_acquire) && !did)
            {
                std::fflush(fp_);
                return;
            }

            if (!did)
            {
                // 유휴면 stdio 버퍼를 비우고 잔다. 1초 상한은 종료 지연의 상한이지 깨우는 수단이 아니다.
                std::fflush(fp_);
                wake_.wait_for(1s,
                               [this]
                               {
                                   return q_.empty() && running_.load(std::memory_order_acquire) &&
                                          !flush_req_.load(std::memory_order_acquire);
                               });
            }
        }
    }

    std::filesystem::path  path_;
    MpscQueue<Record>      q_;
    std::FILE*             fp_ = nullptr;
    std::thread            writer_;
    sync::WakeGate         wake_;
    std::atomic<bool>      running_{false};
    std::atomic<bool>      flush_req_{false};
    std::atomic<uint64_t>  offered_{0};
    std::atomic<uint64_t>  written_{0};
    std::atomic<uint64_t>  dropped_{0};
};

// ── 리더 ─────────────────────────────────────────────────────────────────────
class TickReader
{
public:
    explicit TickReader(const std::filesystem::path& file)
    {
        fp_ = std::fopen(file.string().c_str(), "rb");

        if (fp_ == nullptr)
        {
            return;
        }

        std::array<char, 16> head{};

        if (std::fread(head.data(), 1, head.size(), fp_) != head.size() || std::memcmp(head.data(), "QTCAP", 6) != 0 ||
            head[6] != static_cast<char>(kFormatVersion))
        {
            std::fclose(fp_);
            fp_ = nullptr;
            return;
        }

        std::memcpy(&start_utc_ms_, head.data() + 8, sizeof(start_utc_ms_));
    }

    ~TickReader()
    {
        if (fp_ != nullptr)
        {
            std::fclose(fp_);
        }
    }

    TickReader(const TickReader&)            = delete;
    TickReader& operator=(const TickReader&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return fp_ != nullptr;
    }

    [[nodiscard]] int64_t start_utc_ms() const noexcept
    {
        return start_utc_ms_;
    }

    // 다음 레코드. 끝이거나 꼬리가 잘렸거나 모르는 kind면 false — 그 뒤로는 계속 false.
    bool next(Record& out)
    {
        if (fp_ == nullptr)
        {
            return false;
        }

        uint8_t hdr[4];

        if (std::fread(hdr, 1, sizeof(hdr), fp_) != sizeof(hdr))
        {
            return stop();
        }

        const uint16_t length  = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
        const uint8_t  kind = hdr[2];
        void*          dst  = nullptr;

        if (kind == kKindTrade && length == sizeof(TradeBody))
        {
            dst = &out.trade;
        }
        else if (kind == kKindBook && length == sizeof(BookBody))
        {
            dst = &out.book;
        }
        else
        {
            return stop();
        }

        if (std::fread(dst, 1, length, fp_) != length)
        {
            return stop();
        }

        out.kind = kind;
        return true;
    }

private:
    bool stop()
    {
        std::fclose(fp_);
        fp_ = nullptr;
        return false;
    }

    std::FILE* fp_           = nullptr;
    int64_t    start_utc_ms_ = 0;
};

} // namespace feed
