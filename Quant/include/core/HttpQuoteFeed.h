// core/HttpQuoteFeed.h — 전 종목 시세를 주기마다 HTTP로 통째로 받아 체결(TradeData)로 흘리는 피드.
//  왜 있나: KIS WebSocket은 세션당 구독 40개(kMaxWsSubs, 체결·호가 각각 센다)라 전 종목 시세를 실시간으로 받을 길이 없다. 이 피드는 전 종목을
//  한 주기에 한 바퀴 받아 "2,500+종목을 주기마다 받아 전략까지 돌린다"를 실제로 재려는 것이다. 체결 하나하나가
//  아니라 주기 사이의 누적 거래량 증분이라 틱이 아니다 — 주기 동안의 체결을 한 건으로 뭉친 값이다.
//
//  [inv] 종목 하나는 수신 스레드(lane) 하나만 내보낸다 — 종목 안 순서 보장(원칙 1·2). 종목의 lane은
//        목록에서의 자리로 정해지고(index % lane_count) 도는 동안 바뀌지 않는다.
//
//  ⚠ 주기·묶음 크기를 상수로 고정하지 않고 설정값으로 둔 이유 — 받는 쪽 사정에 맞춰 낮추거나 올리면서 재기 위해서다.
//    라이브 트레이더의 시세판 스레드(Quant/src/universe/MarketBoard.cpp)가 같은 곳을 같은 IP로 부른다 — 여기서
//    여기가 실패하면 실매매 시세도 같이 먼다. 두 쪽 주기를 같이 보고 정한다.
#pragma once

#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace feed
{

class HttpQuoteFeed
{
public:
    // 큐 push. 어느 수신 스레드가 낸 것인지 같이 준다 — 받는 쪽이 종목·스레드 짝을 그대로 이어야 순서가 지켜진다.
    using TickSink = std::function<void(size_t lane, TradeData)>;

    struct Config
    {
        std::vector<std::string>  codes;                  // 받을 종목 코드(여섯 자리). 순서가 lane 배정을 정한다
        size_t                    lane_count = 4;         // 수신 스레드 수
        std::chrono::milliseconds sweep_period{1000};     // 한 바퀴 주기
        size_t                    max_codes_per_call = 900; // 한 번에 묶어 보낼 종목 수(900까지는 전부 돌아오는 것을 쟀다)
    };

    // 한 바퀴를 돌 때마다 쌓는 값. 재는 쪽이 읽는다(스레드 여럿이 더한다).
    struct Counters
    {
        std::atomic<uint64_t> sweeps{0};              // 돈 바퀴 수(레인별 합)
        std::atomic<uint64_t> calls{0};               // 보낸 요청 수
        std::atomic<uint64_t> call_failures{0};       // 빈 응답·파싱 실패
        std::atomic<uint64_t> bytes_received{0};      // 받은 본문 바이트
        std::atomic<uint64_t> quotes_received{0};     // 응답에 들어 있던 종목 수
        std::atomic<uint64_t> ticks_emitted{0};       // 값이 바뀌어 실제로 흘린 건수
        std::atomic<uint64_t> sweep_overruns{0};      // 한 바퀴가 주기를 넘긴 횟수
        std::atomic<uint64_t> sweep_micros_total{0};  // 바퀴 소요 합(µs) — 평균은 total/sweeps
        std::atomic<uint64_t> sweep_micros_max{0};    // 가장 오래 걸린 바퀴(µs)
    };

    HttpQuoteFeed(Config config, TickSink sink);
    ~HttpQuoteFeed();

    HttpQuoteFeed(const HttpQuoteFeed&)            = delete;
    HttpQuoteFeed& operator=(const HttpQuoteFeed&) = delete;

    void start();
    void stop(); // 되돌아올 때 수신 스레드는 전부 멈춰 있다

    const Counters& counters() const
    {
        return counters_;
    }

private:
    void run_lane(size_t lane);

    Config            config_;
    TickSink          sink_;
    Counters          counters_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> lanes_;
};

// 응답 본문에서 종목 하나를 읽어 낸 값. 파서는 이것만 만들고 TradeData 조립은 부르는 쪽이 한다.
struct ParsedQuote
{
    std::string_view code;                 // [inv] 본문 문자열이 살아 있는 동안만 유효하다
    double           price              = 0.0;
    int64_t          accumulated_volume = 0;
    int32_t          hhmmss             = 0;
};

// 본문을 훑어 종목별 값을 뽑는다. nlohmann으로 통째로 파싱하지 않는 이유는 한 바퀴가 수 MB라서다 — 필요한
//  네 필드만 찾아 읽는다. 테스트가 부를 수 있게 공개한다.
std::vector<ParsedQuote> parse_quotes(std::string_view body);

} // namespace feed
