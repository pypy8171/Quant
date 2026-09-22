// 실시간 틱·호가의 append-only 이진 캡처와 그 리더. 수신 스레드는 고정 크기 레코드를 MPSC 큐에 넣기만 하고
//  파일 쓰기는 기록 스레드가 한다. 리플레이 백테스트의 입력이 이 파일이다. [why D-071]
//  스레드: on_trade/on_book은 WS 수신 스레드 여럿(FeedMux 수신 스레드마다 하나)이 부른다 — 생산자가 여럿이라 큐는 MpscQueue
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

// path::string()은 와이드 경로를 프로세스 코드페이지로 되돌리는데, 사용자 폴더 이름에 한글이 들어 있으면
//  코드페이지에 매핑이 없어 예외를 던진다 — 리플레이가 기동 중에 죽었다(09-22 실측).
//  Windows에서는 와이드 경로 그대로 _wfopen에 넘긴다. [why D-071]
std::FILE* open_capture_file(const std::filesystem::path& file, const char* mode);

// ── 파일 형식 v1·v2 ─────────────────────────────────────────────────────────────
//  머리 16바이트: "QTCAP\0" + version(uint8=kFormatVersion, 지금 2) + pad(1) + 시작 utc_ms(int64, LE).
//  레코드: uint16 length(본문 바이트) + uint8 kind(1=체결 2=호가 3=봉 4=유니버스) + uint8 version + 본문. 본문은 아래 POD를 그대로 쓴다
//  (LE, x64 정렬 그대로). 꼬리가 잘려 있으면 리더가 그 앞까지만 돌려준다.
// v2에서 봉·유니버스 레코드가 늘었다. 리더는 v1도 그대로 읽는다 — 09-21까지 받아 둔 파일이 있다.
//  레코드 종류가 늘어도 옛 리더가 안 깨지도록, 모르는 종류는 멈추지 않고 길이만큼 건너뛴다. [why D-071]
constexpr uint8_t kFormatVersion       = 2;
constexpr uint8_t kFormatVersionOldest = 1; // 리더가 받아 주는 가장 낮은 버전
constexpr uint8_t kKindTrade           = 1;
constexpr uint8_t kKindBook            = 2;
constexpr uint8_t kKindBar             = 3; // 파이프라인에 들어간 봉(일봉 폴링·기동용 과거 봉)
constexpr uint8_t kKindUniverse        = 4; // 그날 무엇을 보기로 했는지
constexpr int32_t kDailyBarSeconds     = 86400; // BarBody.interval_sec의 일봉 값
constexpr uint16_t kMaxRecordBytes     = 4096; // 이보다 긴 레코드는 깨진 것으로 본다(모르는 종류를 건너뛸 때의 안전선)
constexpr size_t  kTickerMax     = 12; // KIS 현물 6자리·선물 8자리·미국 티커. 넘치면 잘린다.
constexpr size_t  kTimeMax       = 8;  // HHMMSS

struct Common
{
    int64_t  received_ns = 0; // 수신 스레드 steady_clock nanoseconds (TradeData.received_ns·OrderBook.received_ns).
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

// 봉(MarketData). 체결·호가와 달리 HTTP로 받아 파이프라인에 넣는데, 그 값이 리플레이에 안 남아
//  일봉을 쓰는 전략은 같은 날을 다시 돌려도 같은 자리에서 시작하지 못한다. 들어가는 자리에서 같이 적는다.
struct BarBody
{
    Common  common;
    int32_t bar_index    = 0; // MarketData.bar_index — 파이프라인에 들어간 순번
    int32_t interval_sec = 0; // 60=1분봉, 86400=일봉
    double  open         = 0.0;
    double  high         = 0.0;
    double  low          = 0.0;
    double  close        = 0.0;
    int64_t volume       = 0;
};

// 그날 무엇을 보기로 했는지. 종목 수와 구독 내용은 날마다 달라서, 이것 없이는 호가가 왜 비어 있는지
//  같은 질문을 파일만 보고 답할 수 없다(09-21 캡처가 그랬다).
struct UniverseBody
{
    Common  common;
    uint8_t trade_only = 0; // 1이면 체결만 구독 — 이 종목의 호가는 파일에 없다
    uint8_t pad[7]     = {};
};

static_assert(std::is_trivially_copyable_v<TradeBody> && std::is_trivially_copyable_v<BookBody>
              && std::is_trivially_copyable_v<BarBody> && std::is_trivially_copyable_v<UniverseBody>);
static_assert(sizeof(Common) == 48 && sizeof(TradeBody) == 80 && sizeof(BookBody) == 208
              && sizeof(BarBody) == 96 && sizeof(UniverseBody) == 56,
              "파일 형식의 본문 크기 — 바뀌면 kFormatVersion을 올린다");

// 종류마다 파일에 쓰는 본문 길이. 0이면 이 리더가 모르는 종류라는 뜻이고, 그때는 길이만큼 건너뛴다.
uint16_t body_bytes_of(uint8_t kind);

// 큐 원소. 호가 크기라 체결도 200바이트를 차지하지만 큐는 메모리라 상관없고, 파일에는 kind에 맞는 길이만 쓴다.
struct Record
{
    uint8_t kind = 0;
    union
    {
        TradeBody    trade;
        BookBody     book;
        BarBody      bar;
        UniverseBody universe;
    };

    Record() : trade{}
    {
    }
};

inline int64_t wall_us_of(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

void put_string(char* destination, size_t capture, std::string_view text);

// 파일 형식은 그대로 "HHMMSS" 문자다(정수화 전 캡처와 호환). 여섯 자리를 손으로 찍는다 — 수신 스레드라 할당이 없다.
void put_hhmmss(char* destination, int32_t hhmmss);

void fill_common(Common& common, std::string_view ticker, int32_t hhmmss, uint32_t symbol_id, Market market,
                        int direction, int64_t received_ns, std::chrono::system_clock::time_point timestamp);

TradeBody to_body(const TradeData& trade);

BookBody to_body(const OrderBook& order_book);

TradeData to_trade(const TradeBody& trade_body);

OrderBook to_book(const BookBody& book_body);

// ── 기록기 ───────────────────────────────────────────────────────────────────
class TickCapture
{
public:
    // 파일을 열지 못하면 ok()가 false고 on_*는 아무것도 하지 않는다 — 캡처 실패가 매매를 막지 않는다.
    explicit TickCapture(std::filesystem::path file, size_t queue_capacity = 1u << 16);

    ~TickCapture();

    TickCapture(const TickCapture&)            = delete;
    TickCapture& operator=(const TickCapture&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return file_ != nullptr;
    }

    // 생산자(수신 스레드). 큐가 차면 버리고 센다 — 캡처 때문에 틱 경로가 서지 않는다.
    void on_trade(const TradeData& trade) noexcept;

    void on_book(const OrderBook& order_book) noexcept;

    // 봉 하나. 일봉 폴링은 사이클마다 종목 수만큼이라 hot path가 아니다.
    void on_bar(const MarketData& bar, int32_t interval_sec) noexcept;

    // 그날 구독하기로 한 종목 하나. 구독을 거는 자리에서 한 번씩 부른다.
    void on_universe(std::string_view ticker, uint8_t market, uint32_t symbol_id, bool trade_only) noexcept;

    // 큐가 빌 때까지 기다리고 파일을 flush한다. 종료·테스트용 — hot path에서 부르지 않는다.
    void flush();

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
    void enqueue(Record&& record) noexcept;

    void write_one(const Record& record);

    void writer_loop();

    std::filesystem::path  path_;
    MpscQueue<Record>      queue_;
    std::FILE*             file_ = nullptr;
    std::thread            writer_;
    wake::WakeGate         wake_;
    std::atomic<bool>      running_{false};
    std::atomic<bool>      flush_request_{false};
    std::atomic<uint64_t>  offered_{0};
    std::atomic<uint64_t>  written_{0};
    std::atomic<uint64_t>  dropped_{0};
};

// ── 리더 ─────────────────────────────────────────────────────────────────────
class TickReader
{
public:
    explicit TickReader(const std::filesystem::path& file);

    ~TickReader();

    TickReader(const TickReader&)            = delete;
    TickReader& operator=(const TickReader&) = delete;

    [[nodiscard]] bool ok() const noexcept
    {
        return file_ != nullptr;
    }

    [[nodiscard]] int64_t start_utc_ms() const noexcept
    {
        return start_utc_ms_;
    }

    // 다음 레코드. 끝이거나 꼬리가 잘렸으면 false — 그 뒤로는 계속 false.
    //  모르는 종류는 길이만큼 건너뛰고 다음으로 간다 — 나중에 늘어난 종류가 옛 리더를 세우지 않게. [why D-071]
    bool next(Record& out);

    // 모르는 종류라 건너뛴 레코드 수. 옛 리더로 새 파일을 읽었을 때 얼마를 못 봤는지 알려면 필요하다.
    [[nodiscard]] uint64_t skipped() const noexcept
    {
        return skipped_;
    }

private:
    bool stop();

    std::FILE* file_           = nullptr;
    int64_t    start_utc_ms_ = 0;
    uint64_t   skipped_        = 0;
};

} // namespace feed
