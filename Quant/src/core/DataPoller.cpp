// REST 현재가 폴러 구현 — 호출 간격·1회 로그·넘침 목록. 판정·틱 생성은 poller 네임스페이스의 순수 함수(선언은 Quant/include/core/DataPoller.h, 정의는 이 파일 아래쪽). [why D-062]
#include "core/DataPoller.h"

#include "core/KstTime.h"
#include "utils/Logger.h"

#include <thread>

DataPoller::DataPoller(QuoteFn quote, TickSink sink) : quote_(std::move(quote)), sink_(std::move(sink)) {}

int DataPoller::poll_universe(const std::vector<WatchSpec>& specifications, std::time_t now_utc)
{
    const int32_t hhmmss = kst::hhmmss_int(now_utc);
    int           count      = 0;

    for (const auto& specification : specifications)
    {
        if (specification.market != Market::KR)
        {
            continue;
        }

        if (!keep_going())
        {
            break;
        }

        if (universe_call_interval_.count() > 0)
        {
            std::this_thread::sleep_for(universe_call_interval_);
        }

        const double price = quote_(specification.ticker);

        if (price <= 0.0)
        {
            continue;
        }

        sink_(poller::make_tick(specification.ticker, price, hhmmss, std::chrono::system_clock::now()));
        ++count;
    }

    return count;
}

bool DataPoller::add_overflow(const WatchSpec& specification)
{
    const std::lock_guard lock(overflow_mutex_);

    for (const auto& overflow_entry : overflow_)
    {
        if (poller::same_specification(overflow_entry, specification))
        {
            return false;
        }
    }

    overflow_.push_back(specification);
    return true;
}

size_t DataPoller::overflow_count() const
{
    const std::lock_guard lock(overflow_mutex_);
    return overflow_.size();
}

int DataPoller::poll_overflow(const std::vector<WatchSpec>& from_websocket, const ResubscribeFn& resub, std::time_t now_utc)
{
    // 최초 연결·재연결에서 상한에 밀린 종목도 여기로 합친다 — 재스캔 등록분만 챙기면 기동 시 뒤쪽에 선
    //  종목(청산 관리 시드)이 틱을 영영 못 받는다.
    for (const auto& specification : from_websocket)
    {
        if (add_overflow(specification))
        {
            LOG_WARN("[Engine] WS 구독 상한 — " + specification.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                     std::to_string(overflow_count()) + "종목)");
        }
    }

    std::vector<WatchSpec> pending;
    {
        // 재구독 성공이 목록을 줄이고 제어 스레드가 목록을 늘리므로, 락 안에서 뜬 사본을 락 밖에서 돈다
        const std::lock_guard lock(overflow_mutex_);
        pending = overflow_;
    }

    if (pending.empty())
    {
        return 0;
    }

    const int32_t          hhmmss  = kst::hhmmss_int(now_utc);
    int                    count       = 0;

    for (const auto& specification : pending)
    {
        if (resub && resub(specification))
        {
            const std::lock_guard lock(overflow_mutex_);

            for (auto iterator = overflow_.begin(); iterator != overflow_.end(); ++iterator)
            {
                if (poller::same_specification(*iterator, specification))
                {
                    overflow_.erase(iterator);
                    break;
                }
            }

            LOG_INFO("[Engine] WS 슬롯 확보 — " + specification.ticker + " 구독 복귀");
            continue;
        }

        if (specification.market != Market::KR)
        {
            continue;
        }

        if (!keep_going())
        {
            break;
        }

        if (universe_call_interval_.count() > 0)
        {
            std::this_thread::sleep_for(universe_call_interval_);
        }

        const double price = quote_(specification.ticker);

        if (price <= 0.0)
        {
            if (rest_failed_.insert(specification.ticker).second)
            {
                LOG_WARN("[Engine] REST 대체 시세 실패 " + specification.ticker + " — 현재가 0(응답 없음/파싱 실패)");
            }

            continue;
        }

        if (rest_seen_.insert(specification.ticker).second)
        {
            LOG_INFO("[Engine] REST 대체 시세 첫 수신 " + specification.ticker + " px=" + std::to_string(price));
        }

        sink_(poller::make_tick(specification.ticker, price, hhmmss, std::chrono::system_clock::now()));
        ++count;
    }

    return count;
}

int DataPoller::top_up(const std::vector<std::string>& tickers,
                       const std::function<void(const std::string&, double)>& on_price)
{
    int count = 0;

    for (const auto& ticker : tickers)
    {
        if (!keep_going())
        {
            break;
        }

        if (top_up_call_interval_.count() > 0)
        {
            std::this_thread::sleep_for(top_up_call_interval_);
        }

        on_price(ticker, quote_(ticker));
        ++count;
    }

    return count;
}

namespace poller
{
TradeData make_tick(const std::string& ticker, double price, int32_t hhmmss,
                    std::chrono::system_clock::time_point timestamp)
{
    TradeData trade;
    trade.ticker = ticker;
    trade.hhmmss = hhmmss;
    trade.price = price;
    trade.quantity = 0;
    trade.direction = 0;
    trade.market = Market::KR;
    trade.timestamp = timestamp;
    return trade;
}

std::vector<std::string> select_stale(const std::vector<std::string>& held, const LastSeenFn& last_seen,
                                      std::chrono::steady_clock::time_point cutoff)
{
    std::vector<std::string> out;

    for (const auto& held_ticker : held)
    {
        const auto at = last_seen(held_ticker);

        if (!at || *at < cutoff)
        {
            out.push_back(held_ticker);
        }
    }

    return out;
}

} // namespace poller
