// REST 현재가 폴러 구현 — 호출 간격·1회 로그·넘침 목록. 판정·틱 생성은 core/DataPoller.h의 순수 함수. [why D-062]
#include "core/DataPoller.h"

#include "core/KstTime.h"
#include "utils/Logger.h"

#include <thread>

DataPoller::DataPoller(QuoteFn quote, TickSink sink) : quote_(std::move(quote)), sink_(std::move(sink)) {}

int DataPoller::poll_universe(const std::vector<WatchSpec>& specs, std::time_t now_utc)
{
    const std::string hhmmss = kst::hhmmss(now_utc);
    int               n      = 0;

    for (const auto& spec : specs)
    {
        if (spec.market != Market::KR)
        {
            continue;
        }

        if (!keep_going())
        {
            break;
        }

        if (universe_pacing_.count() > 0)
        {
            std::this_thread::sleep_for(universe_pacing_);
        }

        const double px = quote_(spec.ticker);

        if (px <= 0.0)
        {
            continue;
        }

        sink_(poller::make_tick(spec.ticker, px, hhmmss, std::chrono::system_clock::now()));
        ++n;
    }

    return n;
}

bool DataPoller::add_overflow(const WatchSpec& spec)
{
    for (const auto& w : overflow_)
    {
        if (poller::same_spec(w, spec))
        {
            return false;
        }
    }

    overflow_.push_back(spec);
    return true;
}

int DataPoller::poll_overflow(const std::vector<WatchSpec>& from_ws, const ResubscribeFn& resub, std::time_t now_utc)
{
    // 최초 연결·재연결에서 상한에 밀린 종목도 여기로 합친다 — 재스캔 등록분만 챙기면 기동 시 뒤쪽에 선
    //  종목(청산 관리 시드)이 틱을 영영 못 받는다.
    for (const auto& spec : from_ws)
    {
        if (add_overflow(spec))
        {
            LOG_WARN("[Engine] WS 구독 상한 — " + spec.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                     std::to_string(overflow_.size()) + "종목)");
        }
    }

    if (overflow_.empty())
    {
        return 0;
    }

    const std::string      hhmmss  = kst::hhmmss(now_utc);
    const auto             pending = overflow_; // 재구독 성공이 목록을 줄이므로 복사본을 돈다
    int                    n       = 0;

    for (const auto& spec : pending)
    {
        if (resub && resub(spec))
        {
            for (auto it = overflow_.begin(); it != overflow_.end(); ++it)
            {
                if (poller::same_spec(*it, spec))
                {
                    overflow_.erase(it);
                    break;
                }
            }

            LOG_INFO("[Engine] WS 슬롯 확보 — " + spec.ticker + " 구독 복귀");
            continue;
        }

        if (spec.market != Market::KR)
        {
            continue;
        }

        if (!keep_going())
        {
            break;
        }

        if (universe_pacing_.count() > 0)
        {
            std::this_thread::sleep_for(universe_pacing_);
        }

        const double px = quote_(spec.ticker);

        if (px <= 0.0)
        {
            if (rest_failed_.insert(spec.ticker).second)
            {
                LOG_WARN("[Engine] REST 대체 시세 실패 " + spec.ticker + " — 현재가 0(응답 없음/파싱 실패)");
            }

            continue;
        }

        if (rest_seen_.insert(spec.ticker).second)
        {
            LOG_INFO("[Engine] REST 대체 시세 첫 수신 " + spec.ticker + " px=" + std::to_string(px));
        }

        sink_(poller::make_tick(spec.ticker, px, hhmmss, std::chrono::system_clock::now()));
        ++n;
    }

    return n;
}

int DataPoller::top_up(const std::vector<std::string>& tickers,
                       const std::function<void(const std::string&, double)>& on_px)
{
    int n = 0;

    for (const auto& ticker : tickers)
    {
        if (!keep_going())
        {
            break;
        }

        if (top_up_pacing_.count() > 0)
        {
            std::this_thread::sleep_for(top_up_pacing_);
        }

        on_px(ticker, quote_(ticker));
        ++n;
    }

    return n;
}
