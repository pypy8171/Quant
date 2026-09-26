// REST 현재가 폴러 구현 — 조회 스레드·호출 간격·1회 로그·넘침 목록. 판정·틱 생성은 poller 네임스페이스의 순수 함수(선언은 Quant/include/core/DataPoller.h, 정의는 이 파일 아래쪽). [why D-062]
#include "core/DataPoller.h"

#include "core/KstTime.h"
#include "core/WakeGate.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <thread>

DataPoller::DataPoller(QuoteFn quote, TickSink sink) : quote_(std::move(quote)), sink_(std::move(sink)) {}

DataPoller::~DataPoller()
{
    request_stop();
    join();
}

void DataPoller::start(LoopSources sources, std::chrono::milliseconds round_period)
{
    if (loop_thread_.joinable())
    {
        return;
    }

    // sources는 스레드 안으로 옮긴다 — 이 뒤로 다른 스레드가 만지지 않는다.
    loop_thread_ = std::jthread([this, sources = std::move(sources), round_period](std::stop_token stop_token)
                                {
                                    loop(stop_token, sources, round_period);
                                });
}

void DataPoller::request_stop()
{
    loop_thread_.request_stop();
}

void DataPoller::join()
{
    if (loop_thread_.joinable())
    {
        loop_thread_.join();
    }
}

void DataPoller::loop(std::stop_token stop_token, const LoopSources& sources, std::chrono::milliseconds round_period)
{
    thread_name::set_current("RestPoller");

    LOG_INFO("[Engine] REST 조회 스레드 시작 — 목표 주기 " + std::to_string(round_period.count()) + "ms");

    while (!stop_token.stop_requested() && keep_going())
    {
        const auto round_start = std::chrono::steady_clock::now();
        int        ticks       = 0;

        try
        {
            const bool rest_mode = sources.rest_mode && sources.rest_mode();

            if (rest_mode && sources.universe)
            {
                ticks = poll_universe(sources.universe(), std::time(nullptr));
            }
            else if (!rest_mode && sources.from_websocket)
            {
                ticks = poll_overflow(sources.from_websocket(), {}, std::time(nullptr));
            }
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[Engine] REST 조회 스레드 예외 - what(" + std::string(exception.what()) + ")");
        }

        if (ticks > 0 && sources.on_ticks)
        {
            sources.on_ticks(ticks);
        }

        // 한 바퀴가 목표보다 짧으면 남은 만큼 잔다. 길었으면 바로 다음 바퀴 — 호출 간격과 앱키 한도가 속도를 잡는다.
        const auto spent = std::chrono::steady_clock::now() - round_start;

        if (spent < round_period && !wake::sleep_unless_stopped(stop_token, round_period - spent))
        {
            break;
        }
    }
}

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

bool DataPoller::remove_overflow(const WatchSpec& specification)
{
    const std::lock_guard lock(overflow_mutex_);

    for (auto iterator = overflow_.begin(); iterator != overflow_.end(); ++iterator)
    {
        if (poller::same_specification(*iterator, specification))
        {
            overflow_.erase(iterator);
            return true;
        }
    }

    return false;
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
