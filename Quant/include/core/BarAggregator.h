#pragma once
// WS 체결 틱을 종목별 N분봉으로 모은다. 버킷은 REST 집계(KisRestDecode.h aggregate_minutes)와 같은 시계 정렬이고,
//  스냅샷 배치도 같다([0]=최신, bar_index 0=최신). 주인은 전략(전략 스레드에서만 부른다) — 락이 없다.
//  Engine·KIS·Logger에 의존하지 않는다. [why D-068]
#include "core/Types.h"

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bars
{
// 봉 하나의 자리. day는 KST 거래일의 단조 키(연도*400+연중일), bucket은 그 날의 몇 번째 봉인가.
struct BarSlot
{
    int64_t day    = 0;
    int     bucket = -1;

    bool valid() const { return bucket >= 0; }
    bool operator==(const BarSlot& o) const { return day == o.day && bucket == o.bucket; }
    bool operator!=(const BarSlot& o) const { return !(*this == o); }
    bool operator<(const BarSlot& o) const { return day != o.day ? day < o.day : bucket < o.bucket; }
};

// 틱 → 봉 자리. 분은 hhmmss(거래소 체결 시각)에서, 날짜는 recv_utc의 KST 거래일에서 온다. hhmmss가 여섯 자리가
//  아니면(REST 대체 틱) 수신 시각의 KST 분을 쓴다. 장 밖(open_hhmm 전·close_hhmm 뒤)이면 valid()가 거짓.
BarSlot slot_of(const std::string& hhmmss, std::time_t recv_utc, int interval_min, int open_hhmm, int close_hhmm);

// 봉 시작 시각(UTC). MarketData.timestamp에 넣는다 — REST 봉의 timestamp가 봉의 마지막 1분 시각이라 뜻이 조금
//  다르지만, 전략은 timestamp를 판단에 쓰지 않는다.
std::chrono::system_clock::time_point slot_start(const BarSlot& s, std::time_t recv_utc, int interval_min);

class BarAggregator
{
public:
    struct Config
    {
        int interval_min    = 3;    // 봉 길이(분)
        int keep            = 64;   // 종목당 닫힌 봉 이력 상한(진행 중 봉 제외)
        int session_open    = 900;  // 이 HHMM 앞의 틱은 버린다
        int session_close   = 1530; // 이 HHMM 뒤의 틱은 버린다(15:30:xx 마감 동시호가는 든다)
    };

    using BarSink = std::function<void(const MarketData&)>; // 닫힌 봉 한 개 — 다음 버킷 첫 틱이 닫는다

    explicit BarAggregator(Config cfg);
    BarAggregator(const BarAggregator&)            = delete; // 종목별 이력을 든다 — 전략 스레드에 하나
    BarAggregator& operator=(const BarAggregator&) = delete;

    void set_sink(BarSink f) { sink_ = std::move(f); }

    // 틱 한 개. 장 밖·가격 0·자리를 못 정하면 버리고 false. 앞 봉을 닫았으면 sink가 그 안에서 불린다.
    bool on_tick(const TradeData& td);

    // REST 봉([0]=최신)을 한 번 넣는다. 닫힌 자리는 REST가 이기고, 진행 중 자리는 합치고(시가 REST·고저 max/min·
    //  종가 로컬·거래량 큰 쪽), 로컬에 없는 자리는 채운다. 돌아오는 값은 새로 들어간 봉 수.
    int seed(const std::string& ticker, const std::vector<MarketData>& rest_bars);

    // [0]=진행 중 봉(있으면), 그 뒤 닫힌 봉 최신→과거. max_count를 넘기지 않는다(0이면 전부).
    std::vector<MarketData> snapshot(const std::string& ticker, int max_count = 0) const;

    // 닫힌 봉 수(진행 중 제외). 워밍업 판정용.
    int closed_count(const std::string& ticker) const;

    // 진행 중 봉의 자리. 없으면 valid()가 거짓.
    BarSlot current_slot(const std::string& ticker) const;

    void clear(const std::string& ticker);
    const Config& config() const { return cfg_; }

private:
    struct Live
    {
        BarSlot    slot;
        MarketData md;
        int64_t    acml_base = -1; // 버킷 시작 시점의 당일 누적 거래량(첫 틱의 acml − qty). 모르면 −1 → qty 합산
        bool       active    = false;
    };

    struct Series
    {
        std::deque<MarketData> closed; // [0]=최신
        std::deque<BarSlot>    slots;  // closed와 같은 순서
        Live                   live;
    };

    void close_live(Series& s);
    void trim(Series& s);

    Config                                  cfg_;
    BarSink                                 sink_;
    std::unordered_map<std::string, Series> series_;
};
} // namespace bars
