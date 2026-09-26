#pragma once
// REST 현재가 폴러 — WS 대신(폴링 모드·WS 폴백) 유니버스를 훑거나, WS 구독 상한에 밀린 종목을 재구독·REST로
//  대신 흘리거나, 틱이 끊긴 보유 종목의 현재가를 보충한다.
//  스레드: poll_*는 이 객체의 조회 스레드(start로 띄움)가 부르고, top_up은 Engine의 data_thread가 부른다. add_overflow·
//  remove_overflow는 제어 스레드(구독 요청 반영)도 부른다 — 그래서 넘침 목록만 overflow_mutex_로 지킨다. 1회 로그 집합은
//  조회 스레드 소유라 락이 없다. 브로커 호출·틱 배출·재구독은 std::function으로 받아 KIS 없이 시험한다.
//  [why D-062] [why D-138]
#include "core/Types.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace poller
{
// 같은 구독인가 — 종목·시장이 같으면 채널이 같다. 넘침 목록의 중복 판정에 쓴다.
inline bool same_specification(const WatchSpec& specification_a, const WatchSpec& specification_b)
{
    return specification_a.ticker == specification_b.ticker && specification_a.market == specification_b.market;
}

// REST 현재가 한 건을 WS 체결 틱과 같은 모양으로. quantity·direction·strength는 REST에 없어 0이다.
TradeData make_tick(const std::string& ticker, double price, int32_t hhmmss,
                           std::chrono::system_clock::time_point timestamp);

// 이 틱이 make_tick이 만든 REST 대체 틱인가. WS 체결 틱은 체결량이 항상 1주 이상이고 REST 현재가에는
//  체결량·누적량이 없다 — DeviationScaleStrategy가 이 판정으로 REST 틱을 거르고 REST 봉으로 되돌아간다
//  (봉 집계기는 거르지 않는다). [why D-069]
inline bool is_rest_tick(const TradeData& trade)
{
    return trade.quantity == 0 && trade.accumulated_volume == 0;
}

// 틱이 끊긴 종목 고르기 — last_seen이 비었거나(틱 없음) cutoff보다 오래됐으면 고른다. 구독 여부는 따지지
//  않는다: 유니버스 밖 보유(구독 없음)와 WS 상한에 밀린 종목(구독 실패)을 한 조건으로 다 잡기 위해서다.
using LastSeenFn = std::function<std::optional<std::chrono::steady_clock::time_point>(const std::string&)>;

std::vector<std::string> select_stale(const std::vector<std::string>& held, const LastSeenFn& last_seen,
                                             std::chrono::steady_clock::time_point cutoff);
} // namespace poller

class DataPoller
{
public:
    using QuoteFn       = std::function<double(const std::string& ticker)>; // 현재가(원). 실패·파싱 불가 = 0
    using TickSink      = std::function<void(TradeData)>;                   // 큐 push(값으로 넘겨 sink가 옮긴다). 가득 찼을 때 기다림은 호출자 몫
    using ResubscribeFn = std::function<bool(const WatchSpec&)>;            // WS 재구독 시도. true = 슬롯 확보
    using KeepGoingFn   = std::function<bool()>;                            // running_ — 종료 중이면 루프를 끊는다

    // 조회 스레드가 한 바퀴마다 묻는 것들. 어느 것이든 비어 있으면 그 일을 하지 않는다.
    struct LoopSources
    {
        std::function<bool()>                   rest_mode;      // true = WS가 없거나 죽어 감시 종목 전체를 REST로 받는다
        std::function<std::vector<WatchSpec>()> universe;       // 감시 종목 사본(rest_mode일 때만 묻는다)
        std::function<std::vector<WatchSpec>()> from_websocket; // 소켓이 상한에 밀어낸 종목. 가져가면 소켓 쪽은 비워진다
        std::function<void(int)>                on_ticks;       // 한 바퀴에 흘린 틱 수(data_count_ 가산용)
    };

    DataPoller(QuoteFn quote, TickSink sink);
    ~DataPoller();
    DataPoller(const DataPoller&)            = delete;
    DataPoller& operator=(const DataPoller&) = delete;

    // 조회 스레드를 띄운다. 한 바퀴(넘침 종목 전체, REST 폴백이면 감시 종목 전체)를 round_period 안에 끝내는 것이
    //  목표다. 종목이 많아 한 바퀴가 그보다 길면 쉬지 않고 다음 바퀴로 간다 — 그때 주기는 종목 수 × 호출 간격이다.
    //  예전에는 data_thread의 30초 사이클 안에서 돌아, 넘친 종목의 시세가 30초에 한 점이었다. [why D-138]
    void start(LoopSources sources, std::chrono::milliseconds round_period);
    // 정지 요청만 한다(엔진 request_shutdown에서). 회수는 join이다.
    void request_stop();
    // 조회 스레드 회수. sink가 넣는 행렬보다 먼저 멈춰야 하므로 엔진 stop()이 샤드 join 전에 부른다.
    void join();

    void set_keep_going(KeepGoingFn keep_going)
    {
        keep_going_ = std::move(keep_going);
    }

    // 종목 간 호출 간격. 실전 앱키 한도는 초당 20건이고 주문·잔고·스캔도 같은 한도를 쓴다(실계좌는 같은 키).
    //  100ms면 조회 스레드가 초당 10건까지만 쓰고 나머지를 남긴다 — 넘친 종목 10개까지 1초 주기다. [why D-138]
    void set_universe_call_interval(std::chrono::milliseconds milliseconds)
    {
        universe_call_interval_ = milliseconds;
    }

    // 보유 보충은 모의 도메인(초당 한도가 낮다)에서도 돌아 300ms.
    void set_top_up_call_interval(std::chrono::milliseconds milliseconds)
    {
        top_up_call_interval_ = milliseconds;
    }

    // 폴링 모드: KR 현물 spec마다 현재가를 받아 틱으로 흘린다. 반환 = 흘린 틱 수(data_count_ 가산용).
    int poll_universe(const std::vector<WatchSpec>& specifications, std::time_t now_utc);

    // WS 상한에 밀린 종목 등록. 이미 있으면 false. 반환 뒤 overflow_count()로 로그 문구를 만든다.
    bool add_overflow(const WatchSpec& specification);
    // 넘침 목록에서 뺀다(칸을 받았거나 더 볼 필요가 없어졌다). 없었으면 false. [why D-132]
    bool remove_overflow(const WatchSpec& specification);
    size_t overflow_count() const;

    // 넘침 처리 한 사이클: from_websocket(최초 연결·재연결에서 밀린 것)를 합치고, 종목마다 재구독을 먼저 시도해 되면
    //  목록에서 빼고, 안 되면 REST 현재가를 틱으로 흘린다. 재구독·REST 호출은 락 밖에서 한다. 반환 = 흘린 틱 수.
    int poll_overflow(const std::vector<WatchSpec>& from_websocket, const ResubscribeFn& resub, std::time_t now_utc);

    // 틱이 끊긴 보유 종목의 현재가 보충. 전략이 볼 일은 없어 틱은 안 흘리고 on_price로만 준다(운영단말 현재가·
    //  수동주문 reference_price). 반환 = 조회한 종목 수.
    int top_up(const std::vector<std::string>& tickers, const std::function<void(const std::string&, double)>& on_price);

private:
    bool keep_going() const
    {
        return !keep_going_ || keep_going_();
    }

    void loop(std::stop_token stop_token, const LoopSources& sources, std::chrono::milliseconds round_period);

    QuoteFn                   quote_;
    TickSink                  sink_;
    KeepGoingFn               keep_going_;
    std::chrono::milliseconds universe_call_interval_{100};
    std::chrono::milliseconds top_up_call_interval_{300};
    // [lock-order] overflow_mutex_ 안에서는 다른 락을 잡지 않고 네트워크 호출도 하지 않는다.
    mutable std::mutex        overflow_mutex_;
    std::vector<WatchSpec>    overflow_;    // WS 상한에 밀려 REST로 대신 흘리는 종목. overflow_mutex_ 아래서만
    // 종목당 첫 성공·첫 실패만 남긴다 — 대체 경로가 실제로 틱을 흘리는지 로그로 확인할 수 있어야 한다.
    //  문자열인 이유: 소스 계층은 종목 테이블 앞이라 WatchSpec.ticker(문자열)만 있다. REST 왕복당 한 번.
    std::unordered_set<std::string> rest_seen_;
    std::unordered_set<std::string> rest_failed_;
    std::jthread                    loop_thread_; // 마지막 멤버 — 소멸 때 가장 먼저 멈추고 회수된다
};
