// 실시간 틱·호가의 append-only 이진 캡처와 그 리더. 수신 스레드는 고정 크기 레코드를 SPSC 큐에 넣기만 하고
//  파일 쓰기는 기록 스레드가 한다. 리플레이 백테스트의 입력이 이 파일이다. [why D-071]
//  스레드: on_trade/on_book은 생산자 하나(WS 수신 스레드)만 부른다. TickReader는 단일 스레드용.
#pragma once

#include "core/RingBuffer.h"
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
//  레코드: uint16 len(본문 바이트) + uint8 kind(1=체결 2=호가) + uint8 ver(1) + 본문. 본문은 아래 POD를 그대로 쓴다
//  (LE, x64 정렬 그대로). 꼬리가 잘려 있으면 리더가 그 앞까지만 돌려준다.
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kKindTrade     = 1;
constexpr uint8_t kKindBook      = 2;
constexpr size_t  kTickerMax     = 12; // KIS 현물 6자리·선물 8자리·미국 티커. 넘치면 잘린다.
constexpr size_t  kTimeMax       = 8;  // HHMMSS

struct Common
{
    int64_t  recv_ns = 0; // 수신 스레드 steady_clock ns (TradeData.recv_ns). 호가는 캡처 시각.
    int64_t  wall_us = 0; // system_clock us — 리플레이의 timestamp 복원용
    char     ticker[kTickerMax] = {};
    char     time[kTimeMax]     = {};
    uint32_t sym       = 0; // 캡처 시점 id. 기동마다 달라지므로 리플레이는 ticker로 다시 등록한다.
    uint8_t  market    = 0; // Market enum 값
    uint8_t  direction = 0; // 1=매수 5=매도 0=없음
    uint8_t  pad[6]    = {}; // 8바이트 정렬 채움
};

struct TradeBody
{
    Common  c;
    double  price       = 0.0;
    int64_t quantity    = 0;
    double  strength    = 0.0;
    int64_t acml_volume = 0;
};

struct BookBody
{
    Common         c;
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

inline void put_str(char* dst, size_t cap, const std::string& s)
{
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

inline void fill_common(Common& c, const std::string& ticker, const std::string& time, uint32_t sym, Market market,
                        int direction, int64_t recv_ns, std::chrono::system_clock::time_point ts)
{
    c.recv_ns = recv_ns;
    c.wall_us = wall_us_of(ts);
    put_str(c.ticker, kTickerMax, ticker);
    put_str(c.time, kTimeMax, time);
    c.sym       = sym;
    c.market    = static_cast<uint8_t>(market);
    c.direction = static_cast<uint8_t>(direction);
}

inline TradeBody to_body(const TradeData& td)
{
    TradeBody b;
    fill_common(b.c, td.ticker, td.time, td.sym, td.market, td.direction, td.recv_ns, td.timestamp);
    b.price       = td.price;
    b.quantity    = td.quantity;
    b.strength    = td.strength;
    b.acml_volume = td.acml_volume;
    return b;
}

inline BookBody to_body(const OrderBook& ob, int64_t recv_ns)
{
    BookBody b;
    fill_common(b.c, ob.ticker, ob.time, ob.sym, Market::KR, 0, recv_ns, ob.timestamp);
    std::memcpy(b.asks, ob.asks, sizeof(b.asks));
    std::memcpy(b.bids, ob.bids, sizeof(b.bids));
    return b;
}

inline TradeData to_trade(const TradeBody& b)
{
    TradeData td;
    td.ticker      = b.c.ticker;
    td.time        = b.c.time;
    td.sym         = b.c.sym;
    td.market      = static_cast<Market>(b.c.market);
    td.direction   = b.c.direction;
    td.recv_ns     = b.c.recv_ns;
    td.timestamp   = std::chrono::system_clock::time_point(std::chrono::microseconds(b.c.wall_us));
    td.price       = b.price;
    td.quantity    = b.quantity;
    td.strength    = b.strength;
    td.acml_volume = b.acml_volume;
    return td;
}

inline OrderBook to_book(const BookBody& b)
{
    OrderBook ob;
    ob.ticker    = b.c.ticker;
    ob.time      = b.c.time;
    ob.sym       = b.c.sym;
    ob.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(b.c.wall_us));
    std::memcpy(ob.asks, b.asks, sizeof(ob.asks));
    std::memcpy(ob.bids, b.bids, sizeof(ob.bids));
    return ob;
}

// ── 기록기 ───────────────────────────────────────────────────────────────────
class TickCapture
{
public:
    // 파일을 열지 못하면 ok()가 false고 on_*는 아무것도 하지 않는다 — 캡처 실패가 매매를 막지 않는다.
    explicit TickCapture(std::filesystem::path file, size_t queue_capacity = 1u << 16)
        : path_(std::move(file)), q_(queue_capacity)
    {
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        fp_ = std::fopen(path_.string().c_str(), "ab");

        if (fp_ == nullptr)
        {
            return;
        }

        // 새 파일에만 머리를 쓴다 — 같은 파일에 이어 쓰면(재기동) 레코드가 붙는다.
        if (std::filesystem::file_size(path_, ec) == 0 && !ec)
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
    void on_trade(const TradeData& td) noexcept
    {
        if (fp_ == nullptr)
        {
            return;
        }

        Record r;
        r.kind  = kKindTrade;
        r.trade = to_body(td);
        enqueue(std::move(r));
    }

    void on_book(const OrderBook& ob, int64_t recv_ns) noexcept
    {
        if (fp_ == nullptr)
        {
            return;
        }

        Record r;
        r.kind = kKindBook;
        r.book = to_body(ob, recv_ns);
        enqueue(std::move(r));
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
    void enqueue(Record&& r) noexcept
    {
        offered_.fetch_add(1, std::memory_order_relaxed);

        if (!q_.push(std::move(r)))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        wake_.notify();
    }

    void write_one(const Record& r)
    {
        const uint16_t len = r.kind == kKindTrade ? static_cast<uint16_t>(sizeof(TradeBody))
                                                  : static_cast<uint16_t>(sizeof(BookBody));
        const uint8_t  hdr[4] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8), r.kind,
                                 kFormatVersion};
        std::fwrite(hdr, 1, sizeof(hdr), fp_);
        std::fwrite(r.kind == kKindTrade ? static_cast<const void*>(&r.trade) : static_cast<const void*>(&r.book), 1,
                    len, fp_);
        written_.fetch_add(1, std::memory_order_release);
    }

    void writer_loop()
    {
        using namespace std::chrono_literals;

        while (true)
        {
            bool did = false;

            while (auto opt = q_.pop())
            {
                write_one(*opt);
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
    RingBuffer<Record>     q_;
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

        const uint16_t len  = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
        const uint8_t  kind = hdr[2];
        void*          dst  = nullptr;

        if (kind == kKindTrade && len == sizeof(TradeBody))
        {
            dst = &out.trade;
        }
        else if (kind == kKindBook && len == sizeof(BookBody))
        {
            dst = &out.book;
        }
        else
        {
            return stop();
        }

        if (std::fread(dst, 1, len, fp_) != len)
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
