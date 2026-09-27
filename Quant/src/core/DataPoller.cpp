// REST 현재가 폴러 구현 — 조회 스레드·호출 간격·1회 로그·넘침 목록. 판정·틱 생성은 poller 네임스페이스의 순수 함수(선언은 Quant/include/core/DataPoller.h, 정의는 이 파일 아래쪽). [why D-062]
#include "core/DataPoller.h"

#include "core/KstTime.h"
#include "core/WakeGate.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <algorithm>
#include <thread>

DataPoller::DataPoller(QuoteFn quote, TickSink sink) : quote_(std::move(quote)), sink_(std::move(sink)) {}

DataPoller::~DataPoller()
{
    request_stop();
    join();
}

void DataPoller::set_batch_quote(BatchQuoteFn batch_quote, size_t batch_size)
{
    batch_quote_ = std::move(batch_quote);
    batch_size_  = std::max<size_t>(batch_size, 1);
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
    auto summary_start = std::chrono::steady_clock::now();

    while (!stop_token.stop_requested() && keep_going())
    {
        const auto      round_start   = std::chrono::steady_clock::now();
        const long long symbols_start = round_statistics_.symbols;
        int             ticks         = 0;

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
        record_round(symbols_start, std::chrono::duration_cast<std::chrono::milliseconds>(spent), summary_start);

        if (spent < round_period && !wake::sleep_unless_stopped(stop_token, round_period - spent))
        {
            break;
        }
    }
}

int DataPoller::poll_universe(const std::vector<WatchSpec>& specifications, std::time_t now_utc)
{
    const int32_t            hhmmss = kst::hhmmss_int(now_utc);
    int                      count  = 0;
    std::vector<std::string> tickers;
    tickers.reserve(specifications.size());

    for (const auto& specification : specifications)
    {
        if (specification.market == Market::KR)
        {
            tickers.push_back(specification.ticker);
        }
    }

    const std::vector<double> prices = fetch_prices(tickers);

    for (size_t index = 0; index < prices.size(); ++index)
    {
        if (prices[index] <= 0.0)
        {
            continue;
        }

        sink_(poller::make_tick(tickers[index], prices[index], hhmmss, std::chrono::system_clock::now()));
        ++count;
    }

    return count;
}

void DataPoller::wait_call_interval() const
{
    if (universe_call_interval_.count() > 0)
    {
        std::this_thread::sleep_for(universe_call_interval_);
    }
}

std::vector<double> DataPoller::fetch_prices(const std::vector<std::string>& tickers)
{
    std::vector<double> prices;
    prices.reserve(tickers.size());
    round_statistics_.symbols += static_cast<long long>(tickers.size());

    for (size_t begin = 0; begin < tickers.size();)
    {
        const size_t end = batch_quote_ ? std::min(begin + batch_size_, tickers.size()) : begin + 1;

        if (!keep_going())
        {
            break;
        }

        if (batch_quote_)
        {
            const std::vector<std::string> chunk(tickers.begin() + static_cast<std::ptrdiff_t>(begin),
                                                 tickers.begin() + static_cast<std::ptrdiff_t>(end));
            wait_call_interval();
            ++round_statistics_.calls;
            const auto chunk_prices = batch_quote_(chunk);

            if (chunk_prices && chunk_prices->size() == chunk.size())
            {
                prices.insert(prices.end(), chunk_prices->begin(), chunk_prices->end());
                begin = end;
                continue;
            }

            ++round_statistics_.batch_failures;

            if (!batch_failure_logged_)
            {
                batch_failure_logged_ = true;
                LOG_WARN("[Engine] REST 묶음 시세 실패 — 이 묶음은 한 종목씩 조회로 받는다 - first_ticker(" + chunk.front() +
                         ") count(" + std::to_string(chunk.size()) + ")");
            }
        }

        // 묶음이 없거나 실패한 묶음은 한 종목씩 받는다.
        for (size_t index = begin; index < end; ++index)
        {
            if (!keep_going())
            {
                return prices;
            }

            wait_call_interval();
            ++round_statistics_.calls;
            prices.push_back(quote_(tickers[index]));
        }

        begin = end;
    }

    return prices;
}

void DataPoller::record_round(long long symbols_start, std::chrono::milliseconds spent,
                              std::chrono::steady_clock::time_point& summary_start)
{
    if (round_statistics_.symbols > symbols_start)
    {
        ++round_statistics_.busy_rounds;
        round_statistics_.elapsed_ms_sum += spent.count();
        round_statistics_.elapsed_ms_max = std::max<long long>(round_statistics_.elapsed_ms_max, spent.count());
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - summary_start < std::chrono::minutes(1))
    {
        return;
    }

    // 1초 바퀴마다 한 줄이면 하루 수만 줄이라 1분에 한 줄로 묶는다. 조회한 종목이 없던 1분은 남기지 않는다.
    //  scripts/check_runtime_health.py "REST 조회 한 바퀴" 행이 이 줄을 읽는다. [why D-150]
    if (round_statistics_.busy_rounds > 0)
    {
        LOG_INFO("[Engine] REST 조회 1분 요약 - rounds(" + std::to_string(round_statistics_.busy_rounds) + ") symbols(" +
                 std::to_string(round_statistics_.symbols / round_statistics_.busy_rounds) + ") calls(" +
                 std::to_string(round_statistics_.calls / round_statistics_.busy_rounds) + ") avg_ms(" +
                 std::to_string(round_statistics_.elapsed_ms_sum / round_statistics_.busy_rounds) + ") max_ms(" +
                 std::to_string(round_statistics_.elapsed_ms_max) + ") batch_fail(" +
                 std::to_string(round_statistics_.batch_failures) + ")");
    }

    round_statistics_  = RoundStats{};
    summary_start = now;
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

    const int32_t            hhmmss = kst::hhmmss_int(now_utc);
    int                      count  = 0;
    std::vector<std::string> tickers;
    tickers.reserve(pending.size());

    // 재구독을 종목마다 먼저 시도하고, 안 된 KR 종목만 모아 REST로 받는다.
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

        if (specification.market == Market::KR)
        {
            tickers.push_back(specification.ticker);
        }
    }

    const std::vector<double> prices = fetch_prices(tickers);

    for (size_t index = 0; index < prices.size(); ++index)
    {
        const std::string& ticker = tickers[index];
        const double       price  = prices[index];

        if (price <= 0.0)
        {
            if (rest_failed_.insert(ticker).second)
            {
                LOG_WARN("[Engine] REST 대체 시세 실패 " + ticker + " — 현재가 0(응답 없음/파싱 실패)");
            }

            continue;
        }

        if (rest_seen_.insert(ticker).second)
        {
            LOG_INFO("[Engine] REST 대체 시세 첫 수신 " + ticker + " px=" + std::to_string(price));
        }

        sink_(poller::make_tick(ticker, price, hhmmss, std::chrono::system_clock::now()));
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
