// 유안타 시세 입력부 — 32비트 연결 프로그램(PYQuant/tools/yuanta_bridge.py serve)이 ZMQ PUB로 내보낸 체결을 받아
//  엔진의 시세 창구(feed::IFeedSource)로 올린다. 유안타 DLL은 32비트라 64비트 엔진 안에서 부를 수 없어 프로세스를
//  가른다. 엔진이 보기에는 그냥 또 하나의 피드라 샤드·전략·OrderGate·장부가 평소대로 돈다.
//
//        유안타 서버 ─DLL─▶ yuanta_bridge.py ─ZMQ PUB 5700─▶ 수신 스레드(ZMQ SUB) ─▶ TradeData ─▶ 엔진 파이프라인
//
//  연결 프로그램은 국내 주식 전 종목을 구독해 다 보낸다. 여기서 구독 목록에 있는 종목만 골라 올린다 — 칸 개념이
//  없으므로(free_slots -1) Engine은 칸 배정을 하지 않는다.
//
// 스레드: 수신 스레드 하나가 소켓 하나를 맡는다(원칙 1). 구독 목록은 제어 스레드가 바꾸고 수신 스레드가 읽으므로
//  집합을 새로 만들어 포인터를 갈아 끼우고, 수신 스레드는 전문 한 통마다 그 포인터를 한 번 잡는다(CODE_CONVENTIONS 4.4). [why D-163] (원칙 1·3은 D-071)
#pragma once
#include "core/IFeedSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace yuanta
{

// 'YTRD' 를 리틀엔디언 정수로 굳힌 값. 엉뚱한 소켓에서 온 바이트를 체결로 읽는 것을 여기서 막는다.
inline constexpr uint32_t kTradeWireMagic = 0x44525459;

// 전문 모양이 바뀌면 올린다. 판번호가 다르면 받는 쪽이 통째로 버린다.
inline constexpr uint16_t kTradeWireVersion = 1;

inline constexpr size_t kTradeWireHeaderBytes = 16;
inline constexpr size_t kTradeWireRecordBytes = 48;

// 종목코드 칸 크기. 국내 주식은 여섯 글자이고 남는 칸은 0으로 채운다.
inline constexpr size_t kTradeWireCodeBytes = 8;

#pragma pack(push, 1)

// 전문 앞머리. ZMQ 메시지가 경계를 주므로 길이 필드 없이 크기에서 건수를 되짚는다(exchange/OrderWire.h와 같은 약속).
//  같은 기계 안에서만 오가므로 리틀엔디언 그대로 싣는다.
// 파이썬 쪽: struct.pack("<IHHQ", magic, version, record_count, sent_unix_ns)
struct TradeWireHeader
{
    uint32_t magic        = kTradeWireMagic;
    uint16_t version      = kTradeWireVersion;
    uint16_t record_count = 0;
    uint64_t sent_unix_ns = 0; // 연결 프로그램이 보낸 시각. 받는 쪽이 전달 지연을 잰다
};

// 체결 한 건. [wire] 출처: 유안타 qry/auto/AUTO11.qry(국내 주식 현재가 실시간) OutBlock1, 확인 2026-10-15.
// 파이썬 쪽: struct.pack("<8siiqqqd", code, hhmmss, direction, price_krw, quantity, accumulated_volume, strength)
struct TradeWireRecord
{
    char    code[kTradeWireCodeBytes] = {}; // jongcode
    int32_t hhmmss             = 0;         // time2를 HHMMSS 정수로 바꾼 값. 0이면 모름
    int32_t direction          = 0;         // 1=매수, 5=매도, 0=모름 — TradeData.direction과 같은 값
    int64_t price_krw          = 0;         // chejuka(체결가), 원
    int64_t quantity           = 0;         // nowvol(직전체결량)의 절댓값, 주
    int64_t accumulated_volume = 0;         // volume(누적거래량), 주
    double  strength           = 0.0;       // chegyulrate(체결강도), %
};

#pragma pack(pop)

static_assert(sizeof(TradeWireHeader) == kTradeWireHeaderBytes);
static_assert(sizeof(TradeWireRecord) == kTradeWireRecordBytes);
static_assert(std::is_trivially_copyable_v<TradeWireRecord>);

// 전문 한 통을 읽어 체결마다 on_record를 부른다. 앞머리·크기·건수가 안 맞으면 하나도 부르지 않고 false.
//  정렬을 장담할 수 없는 버퍼라 건마다 memcpy로 꺼낸다.
template <typename OnRecord>
bool decode_trade_wire(const void* data, size_t size, OnRecord&& on_record)
{
    if (data == nullptr || size < kTradeWireHeaderBytes)
    {
        return false;
    }

    TradeWireHeader header;
    std::memcpy(&header, data, kTradeWireHeaderBytes);

    if (header.magic != kTradeWireMagic || header.version != kTradeWireVersion)
    {
        return false;
    }

    const size_t body_bytes = size - kTradeWireHeaderBytes;

    if (body_bytes % kTradeWireRecordBytes != 0 || body_bytes / kTradeWireRecordBytes != header.record_count)
    {
        return false;
    }

    const auto* body = static_cast<const unsigned char*>(data) + kTradeWireHeaderBytes;

    for (size_t index = 0; index < header.record_count; ++index)
    {
        TradeWireRecord record;
        std::memcpy(&record, body + index * kTradeWireRecordBytes, kTradeWireRecordBytes);
        on_record(header, record);
    }

    return true;
}

class YuantaFeed final : public feed::IFeedSource
{
public:
    struct Options
    {
        std::string endpoint = "tcp://127.0.0.1:5700"; // 연결 프로그램의 PUB 주소. 같은 기계 안에서만 쓴다
        // 소켓을 열고 수신 스레드를 띄울지. 시험은 false로 두고 ingest()로 바이트를 직접 먹인다.
        bool        start_receive_thread = true;
    };

    struct Statistics
    {
        uint64_t batches   = 0; // 받은 전문 통 수
        uint64_t records   = 0; // 읽은 체결 건수
        uint64_t published = 0; // 구독 목록에 있어 엔진에 올린 건수
        uint64_t malformed = 0; // 앞머리·크기가 안 맞아 버린 통 수
    };

    explicit YuantaFeed(Options options);
    ~YuantaFeed() override;

    YuantaFeed(const YuantaFeed&)            = delete;
    YuantaFeed& operator=(const YuantaFeed&) = delete;

    // ── feed::IFeedSource ───────────────────────────────────────────────────
    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override;
    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override;

    // 체결통보는 유안타 주문을 붙이는 단계에서 맡는다. 지금은 주문이 모의 체결기로 가므로 맡지 않는다.
    [[nodiscard]] bool owns_fill_notice() const override
    {
        return false;
    }

    bool connect(const std::vector<WatchSpec>& specifications) override;
    void disconnect() override;

    bool                   subscribe_incremental(const WatchSpec& specification) override;
    bool                   unsubscribe_incremental(const WatchSpec& specification) override;
    [[nodiscard]] bool     has_specification(const WatchSpec& specification) const override;
    std::vector<WatchSpec> take_overflow_specifications() override;

    [[nodiscard]] bool is_connected() const override;

    // threshold_sec 동안 전문이 한 통도 안 왔으면 true. 연결 프로그램이 죽었거나 유안타 접속이 끊긴 것이다.
    [[nodiscard]] bool is_stale(int threshold_sec) const override;

    // ── 시험 전용 ───────────────────────────────────────────────────────────
    // 소켓을 거치지 않고 바이트를 바로 먹인다. [inv] 수신 스레드가 안 돌고 있을 때만 부른다.
    void ingest(const void* data, size_t size);

    [[nodiscard]] Statistics statistics() const;

private:
    // 구독 종목 집합. 키는 종목코드 8바이트 칸(뒤는 0)을 정수 하나로 읽은 값이다 — 체결마다 문자열을 만들지 않으려고
    //  그렇게 한다(원칙 6). 만드는 법은 YuantaFeed.cpp의 pack_code 하나다.
    using TickerSet = std::unordered_set<uint64_t>;

    // 전문 한 통을 처리한다. 수신 스레드에서만 부른다.
    void process_message(const void* data, size_t size);

    // 구독 목록을 집합으로 굳혀 수신 스레드가 읽는 자리에 갈아 끼운다. specifications_mutex_를 쥔 채 부른다.
    void publish_ticker_set();

    // 수신 스레드 본체. 소켓을 열고 메시지를 받아 process_message로 넘긴다.
    void receive_loop();

    Options options_;

    mutable std::mutex     specifications_mutex_; // specifications_·ticker_set_을 지킨다
    std::vector<WatchSpec> specifications_;
    // [inv] 수신 스레드는 전문 한 통마다 락 안에서 포인터만 복사해 쥐고 락 밖에서 읽는다 — 제어 스레드가 갈아
    //  끼워도 쥔 쪽 집합은 산다. 집합 자체는 만든 뒤 고치지 않는다.
    std::shared_ptr<const TickerSet> ticker_set_;

    LaneTradeCb on_lane_trade_;

    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<int64_t>  last_message_ns_{0}; // steady_clock 나노초. 0이면 아직 한 통도 안 옴
    std::atomic<uint64_t> batches_{0};
    std::atomic<uint64_t> records_{0};
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> malformed_{0};
};

} // namespace yuanta
