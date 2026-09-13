#pragma once
// REST 현재가 폴러 — WS 대신(폴링 모드·WS 폴백) 유니버스를 훑거나, WS 구독 상한에 밀린 종목을 재구독·REST로
//  대신 흘리거나, 틱이 끊긴 보유 종목의 현재가를 보충한다. Engine의 data_thread만 부른다 — 넘침 목록·1회 로그
//  집합은 그 스레드 소유라 락이 없다. 브로커 호출·틱 배출·재구독은 std::function으로 받아 KIS 없이 시험한다.
//  [why D-062]
#include "core/Types.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace poller
{
// 같은 구독인가 — 종목·시장·선물 여부가 같으면 채널이 같다. 넘침 목록의 중복 판정에 쓴다.
inline bool same_spec(const WatchSpec& a, const WatchSpec& b)
{
    return a.ticker == b.ticker && a.market == b.market && a.is_future == b.is_future;
}

// REST 현재가 한 건을 WS 체결 틱과 같은 모양으로. quantity·direction·strength는 REST에 없어 0이다.
inline TradeData make_tick(const std::string& ticker, double px, const std::string& hhmmss,
                           std::chrono::system_clock::time_point ts)
{
    TradeData td;
    td.ticker    = ticker;
    td.time      = hhmmss;
    td.price     = px;
    td.quantity  = 0;
    td.direction = 0;
    td.market    = Market::KR;
    td.timestamp = ts;
    return td;
}

// 이 틱이 make_tick이 만든 REST 대체 틱인가. WS 체결 틱은 체결량이 항상 1주 이상이고 REST 현재가에는
//  체결량·누적량이 없다 — 봉 집계기가 REST 틱을 거르고 REST 봉으로 되돌아가는 판정에 쓴다. [why D-069]
inline bool is_rest_tick(const TradeData& td)
{
    return td.quantity == 0 && td.acml_volume == 0;
}

// 틱이 끊긴 종목 고르기 — last_seen이 비었거나(틱 없음) cutoff보다 오래됐으면 고른다. 구독 여부는 따지지
//  않는다: 유니버스 밖 보유(구독 없음)와 WS 상한에 밀린 종목(구독 실패)을 한 조건으로 다 잡기 위해서다.
using LastSeenFn = std::function<std::optional<std::chrono::steady_clock::time_point>(const std::string&)>;

inline std::vector<std::string> select_stale(const std::vector<std::string>& held, const LastSeenFn& last_seen,
                                             std::chrono::steady_clock::time_point cutoff)
{
    std::vector<std::string> out;

    for (const auto& t : held)
    {
        const auto at = last_seen(t);

        if (!at || *at < cutoff)
        {
            out.push_back(t);
        }
    }

    return out;
}
} // namespace poller

class DataPoller
{
public:
    using QuoteFn       = std::function<double(const std::string& ticker)>; // 현재가(원). 실패·파싱 불가 = 0
    using TickSink      = std::function<void(const TradeData&)>;            // 큐 push. 가득 찼을 때 기다림은 호출자 몫
    using ResubscribeFn = std::function<bool(const WatchSpec&)>;            // WS 재구독 시도. true = 슬롯 확보
    using KeepGoingFn   = std::function<bool()>;                            // running_ — 종료 중이면 루프를 끊는다

    DataPoller(QuoteFn quote, TickSink sink);

    void set_keep_going(KeepGoingFn f) { keep_going_ = std::move(f); }
    // 종목 간 호출 간격. 실전 도메인 시세는 초당 한도(~20/s)가 있어 무간격으로 몰아치면 뒷종목이 HTTP 500으로
    //  떨어진다 — 150ms면 한도 밑에 깔려 전 종목이 매 사이클 틱을 받는다(종목 수×150ms가 사이클 안에 들게).
    void set_universe_pacing(std::chrono::milliseconds ms) { universe_pacing_ = ms; }
    // 보유 보충은 모의 도메인(초당 한도가 낮다)에서도 돌아 300ms.
    void set_top_up_pacing(std::chrono::milliseconds ms) { top_up_pacing_ = ms; }

    // 폴링 모드: KR 현물 spec마다 현재가를 받아 틱으로 흘린다. 반환 = 흘린 틱 수(data_count_ 가산용).
    int poll_universe(const std::vector<WatchSpec>& specs, std::time_t now_utc);

    // WS 상한에 밀린 종목 등록. 이미 있으면 false. 반환 뒤 overflow_count()로 로그 문구를 만든다.
    bool add_overflow(const WatchSpec& spec);
    size_t overflow_count() const { return overflow_.size(); }

    // 넘침 처리 한 사이클: from_ws(최초 연결·재연결에서 밀린 것)를 합치고, 종목마다 재구독을 먼저 시도해 되면
    //  목록에서 빼고, 안 되면 REST 현재가를 틱으로 흘린다. 반환 = 흘린 틱 수.
    int poll_overflow(const std::vector<WatchSpec>& from_ws, const ResubscribeFn& resub, std::time_t now_utc);

    // 틱이 끊긴 보유 종목의 현재가 보충. 전략이 볼 일은 없어 틱은 안 흘리고 on_px로만 준다(운영단말 현재가·
    //  수동주문 ref_price). 반환 = 조회한 종목 수.
    int top_up(const std::vector<std::string>& tickers, const std::function<void(const std::string&, double)>& on_px);

private:
    bool keep_going() const { return !keep_going_ || keep_going_(); }

    QuoteFn                   quote_;
    TickSink                  sink_;
    KeepGoingFn               keep_going_;
    std::chrono::milliseconds universe_pacing_{150};
    std::chrono::milliseconds top_up_pacing_{300};
    std::vector<WatchSpec>    overflow_;    // WS 상한에 밀려 REST로 대신 흘리는 종목. data_thread 전용
    // 종목당 첫 성공·첫 실패만 남긴다 — 대체 경로가 실제로 틱을 흘리는지 로그로 확인할 수 있어야 한다.
    std::unordered_set<std::string> rest_seen_;
    std::unordered_set<std::string> rest_failed_;
};
