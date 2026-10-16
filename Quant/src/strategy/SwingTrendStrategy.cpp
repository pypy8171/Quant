// strategy/SwingTrendStrategy.cpp — 스윙 추세 슬리브 본체: 생성·소유 종목 게시·하루 넘김·초 단위 순서(on_clock)·REST 미리 받기(시가·대기 종목 현재가·일봉).
//  계획·상태 파일과 체결 반영은 SwingTrendState.cpp, 매수·취소·손절 매도는 SwingTrendOrders.cpp에 있다.
//  스레드: prefetch_once만 prefetch::Pool 스레드, 나머지는 샤드 스레드. 관련 결정: D-160.
#include "strategy/SwingTrendStrategy.h"
#include "api/KisClient.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr size_t kMaxOpenFetchPerPass = 5; // 한 주기(3초)에 받는 시가 조회 수 — 09:00에 후보가 몰려도 초당 한도를 다 쓰지 않게

bool is_weekday(std::time_t now)
{
    const std::chrono::weekday weekday{std::chrono::sys_days{kst::date(now)}};
    return weekday != std::chrono::Saturday && weekday != std::chrono::Sunday;
}
} // namespace

SwingTrendStrategy::SwingTrendStrategy(Params parameters, OwnedSink owned_sink)
    : parameters_(std::move(parameters)), owned_sink_(std::move(owned_sink)), clock_([]
    {
        return std::time(nullptr);
    }),
      id_("SWING_" + parameters_.label)
{
}

SwingTrendStrategy::~SwingTrendStrategy()
{
    stop_prefetch();
}

const char* SwingTrendStrategy::status_text(Status status)
{
    switch (status)
    {
    case Status::Pending:
        return "PENDING";
    case Status::Open:
        return "OPEN";
    case Status::Exiting:
        return "EXITING";
    }

    return "?";
}

std::string SwingTrendStrategy::describe() const
{
    return std::format("SwingTrend({} 종목당 {}원 저점 창 {}일 발주선 +{}% 취소선 +{}% {}) 계획 {}", parameters_.label, parameters_.notional_krw,
                       parameters_.pivot_window, parameters_.buy_trigger_percent, parameters_.buy_disarm_percent,
                       parameters_.dry_run ? "dry-run" : "실주문", parameters_.plan_file);
}

std::string SwingTrendStrategy::kst_date() const
{
    return kst::date_yyyymmdd(clock_());
}

std::vector<std::string> SwingTrendStrategy::held_tickers() const
{
    std::vector<std::string> tickers;

    for (const auto& position : positions_)
    {
        tickers.push_back(position.ticker);
    }

    return tickers;
}

std::vector<std::string> SwingTrendStrategy::candidate_tickers() const
{
    std::vector<std::string> tickers;

    if (!plan_)
    {
        return tickers;
    }

    for (const auto& entry : plan_->entries)
    {
        const bool excluded = std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), entry.ticker) !=
                              parameters_.excluded_tickers.end();

        if (!excluded)
        {
            tickers.push_back(entry.ticker);
        }
    }

    return tickers;
}

// 시가·현재가·일봉은 REST로 받는다 — 체결 구독을 하지 않아 웹소켓 칸을 쓰지 않는다.
std::vector<WatchSpec> SwingTrendStrategy::get_watch_specifications() const
{
    return {};
}

// 틱을 쓰지 않는다(on_trade_batch가 비었다) — 빈 목록이어도 전 종목 틱을 받지 않는다.
bool SwingTrendStrategy::wants_all_ticks_when_unlisted() const
{
    return false;
}

void SwingTrendStrategy::on_trade_batch(const TradeData& /*trade*/, std::vector<OrderSignal>& /*out*/)
{
}

bool SwingTrendStrategy::wants_clock() const
{
    return true;
}

// 시장 구간과 무관하게 성과가 났다(지수가 60일선 −5% 아래 81건 건당 +20.3%, 위 +5% 1,019건 +9.3%) — 국면 중단을 받지 않는다. [why D-160]
bool SwingTrendStrategy::ignores_regime_halt() const
{
    return true;
}

void SwingTrendStrategy::publish_owned()
{
    // dry_run은 주문을 내지 않으니 다른 슬리브의 슬롯 계산을 건드리지 않는다.
    if (parameters_.dry_run)
    {
        return;
    }

    owned_scratch_ = held_tickers();

    // 오늘 아직 판단하지 않은 후보와 매수 대기 종목도 넣는다 — 체결되는 순간부터 슬롯 계산 밖이어야 장중 전략의 강제청산 대상이 되지 않는다.
    //  계좌가 이미 든 후보는 넣지 않는다(사지 않고, 넣으면 다른 슬리브 보유가 슬롯 밖으로 빠진다).
    for (const auto& ticker : candidate_tickers())
    {
        const bool undecided = attempted_today_.count(ticker) == 0 || armed_today_.count(ticker) > 0;

        if (undecided && confirmed_position(parameters_.account, ticker) == 0)
        {
            owned_scratch_.push_back(ticker);
        }
    }

    std::sort(owned_scratch_.begin(), owned_scratch_.end());
    owned_scratch_.erase(std::unique(owned_scratch_.begin(), owned_scratch_.end()), owned_scratch_.end());

    if (owned_scratch_ == owned_published_)
    {
        return;
    }

    // 못 보냈으면 지난 목록을 그대로 두어 다음 on_clock에 다시 보낸다.
    if (owned_sink_ && !owned_sink_(owned_scratch_))
    {
        return;
    }

    owned_published_.swap(owned_scratch_);
}

void SwingTrendStrategy::on_start()
{
    load_plan();
    roll_day();
    publish_requests();
    publish_owned();
    LOG_INFO("[" + id_ + "] 시작 " + describe() + " 보유 " + std::to_string(positions_.size()) + "종목");

    // set_kis()·set_prefetch_pool()은 on_start 직전에 불린다. 재등록 경로 대비로 이전 등록을 먼저 뗀다.
    stop_prefetch();

    if (prefetch_pool_)
    {
        prefetch_task_ = prefetch_pool_->add([this]
        {
            prefetch_once();
        });
    }
}

void SwingTrendStrategy::on_stop()
{
    stop_prefetch();
    save_state();
}

void SwingTrendStrategy::stop_prefetch()
{
    if (prefetch_pool_ && prefetch_task_ != 0)
    {
        prefetch_pool_->remove(prefetch_task_);
    }

    prefetch_task_ = 0;
}

void SwingTrendStrategy::roll_day()
{
    const std::string today = kst_date();

    if (today == day_ && day_started_)
    {
        return;
    }

    // 상태 파일의 day가 오늘이면(같은 날 재기동) 오늘 판단한 종목과 매도 중 표시를 그대로 둔다.
    const bool new_day  = today != day_;
    day_                = today;
    day_started_        = true;
    buys_stopped_today_ = false;
    plan_stale_logged_  = false;
    inactive_logged_    = false;
    price_missing_logged_.clear();

    if (new_day)
    {
        attempted_today_.clear();
        armed_today_.clear();

        // 어제 다 못 판 매도 — 주문은 밤사이 사라지므로 Open으로 되돌려 오늘 창에서 다시 판단한다.
        for (auto& position : positions_)
        {
            if (position.status == Status::Exiting)
            {
                position.status        = Status::Open;
                position.exit_attempts = 0;
            }
        }
    }

    const size_t candidates = candidate_tickers().size();
    LOG_INFO(std::format("[{}] 하루 시작 {} 계획 as_of={} 후보 {} 보유 {}", id_, day_, plan_ ? plan_->as_of : std::string("없음"),
                         candidates, positions_.size()));
    dirty_ = true;
}

// prefetch 스레드에 이번에 받을 것을 알린다. 날이 바뀌었으면 어제 시가·현재가를 비운다.
void SwingTrendStrategy::publish_requests()
{
    std::vector<std::string> want_open;
    std::vector<std::string> want_bars;
    std::vector<std::string> want_price;
    std::vector<std::string> want_quote;
    const int                hhmm = kst_hhmm(clock_());

    if (plan_ && hhmm < parameters_.buy_cancel_hhmm)
    {
        for (const auto& ticker : candidate_tickers())
        {
            if (attempted_today_.count(ticker) == 0)
            {
                want_open.push_back(ticker);
            }
        }
    }

    // 매수 창 안에서는 대기 종목(근접 발주)과 체결 0주 오늘 매수(멀어짐 취소)의 현재가를 받는다.
    if (hhmm >= parameters_.buy_start_hhmm && hhmm < parameters_.buy_cancel_hhmm)
    {
        want_quote.assign(armed_today_.begin(), armed_today_.end());

        for (const auto& position : positions_)
        {
            if (position.status == Status::Pending && position.entry_date == day_ && position.order_number != 0 && !position.cancel_sent &&
                position.quantity == 0)
            {
                want_quote.push_back(position.ticker);
            }
        }
    }

    for (const auto& position : positions_)
    {
        if (position.status == Status::Pending)
        {
            continue;
        }

        want_bars.push_back(position.ticker);

        if (position.status == Status::Open && hhmm >= parameters_.sell_start_hhmm && hhmm <= parameters_.sell_end_hhmm)
        {
            want_price.push_back(position.ticker);
        }
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    if (snapshot_.day != day_)
    {
        snapshot_.day = day_;
        snapshot_.opens.clear();
        snapshot_.prices.clear();
        snapshot_.quotes.clear();
    }

    snapshot_.want_open.swap(want_open);
    snapshot_.want_bars.swap(want_bars);
    snapshot_.want_price.swap(want_price);
    snapshot_.want_quote.swap(want_quote);
}

void SwingTrendStrategy::prefetch_once()
{
    const std::time_t now = clock_();

    if (!is_weekday(now) || (!kis_ && !quote_source_.open && !quote_source_.prices))
    {
        return;
    }

    const int         hhmm  = kst_hhmm(now);
    const std::string today = kst::date_yyyymmdd(now);
    std::vector<std::string> want_open;
    std::vector<std::string> want_bars;
    std::vector<std::string> want_price;
    std::vector<std::string> want_quote;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);

        if (snapshot_.day != today)
        {
            return; // 샤드 스레드가 아직 오늘 요청을 올리지 않았다
        }

        if (hhmm >= parameters_.buy_start_hhmm)
        {
            for (const auto& ticker : snapshot_.want_open)
            {
                const auto found = snapshot_.opens.find(ticker);

                if ((found == snapshot_.opens.end() || found->second <= 0.0) && want_open.size() < kMaxOpenFetchPerPass)
                {
                    want_open.push_back(ticker);
                }
            }
        }

        // 한 패스에 묶음 조회 1회(최대 kMultiPriceMax종목). 넘치면 다음 패스가 이어서 돈다.
        const size_t quote_count = snapshot_.want_quote.size();

        for (size_t offset = 0; offset < quote_count && offset < KisClient::kMultiPriceMax; ++offset)
        {
            want_quote.push_back(snapshot_.want_quote[(quote_cursor_ + offset) % quote_count]);
        }

        quote_cursor_ = quote_count == 0 ? 0 : (quote_cursor_ + want_quote.size()) % quote_count;

        for (const auto& ticker : snapshot_.want_bars)
        {
            const auto found = snapshot_.bars.find(ticker);

            if (found == snapshot_.bars.end() || found->second.day != today)
            {
                want_bars.push_back(ticker);
            }
        }

        for (const auto& ticker : snapshot_.want_price)
        {
            if (snapshot_.prices.count(ticker) == 0)
            {
                want_price.push_back(ticker);
            }
        }
    }

    // REST는 잠금 밖에서 부른다 — 한 조회가 샤드 스레드의 on_clock을 막지 않게.
    for (const auto& ticker : want_open)
    {
        double open = 0.0;

        if (quote_source_.open)
        {
            open = quote_source_.open(ticker);
        }
        else if (kis_)
        {
            open = kis_->get_fundamentals(ticker).open;
        }

        if (open > 0.0)
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.opens[ticker] = open;
        }
    }

    fetch_quotes(want_quote);

    if (!kis_)
    {
        return; // 시험 출처만 있다 — 일봉·매도 창 현재가는 KisClient로만 받는다
    }

    for (const auto& ticker : want_bars)
    {
        auto bars = kis_->get_daily_ohlcv(ticker, parameters_.daily_lookback);

        if (bars.empty())
        {
            continue; // 다음 주기에 다시 받는다
        }

        std::sort(bars.begin(), bars.end(), [](const MarketData& left, const MarketData& right)
        {
            return left.session_date < right.session_date;
        });

        DailyLows daily;
        daily.day = today;

        for (const auto& bar : bars)
        {
            daily.session_dates.push_back(bar.session_date);
            daily.lows.push_back(static_cast<int64_t>(bar.low));
        }

        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.bars[ticker] = std::move(daily);
    }

    for (const auto& ticker : want_price)
    {
        const double price = kis_->get_current_price(ticker);

        if (price > 0.0)
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.prices[ticker] = price;
        }
    }
}

// 대기·미체결 종목 현재가를 묶음 조회 한 번으로 받는다(prefetch 스레드). 실패하면 이번 패스는 버린다 — 낡은 값은 fresh_quote가 거른다.
void SwingTrendStrategy::fetch_quotes(const std::vector<std::string>& wanted)
{
    if (wanted.empty())
    {
        return;
    }

    std::optional<std::vector<double>> prices;

    if (quote_source_.prices)
    {
        prices = quote_source_.prices(wanted);
    }
    else if (kis_)
    {
        auto result = kis_->get_current_prices(wanted);

        if (result)
        {
            prices = std::move(*result);
        }
    }

    if (!prices || prices->size() != wanted.size())
    {
        return;
    }

    const std::time_t           fetched_at = clock_();
    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    for (size_t index = 0; index < wanted.size(); ++index)
    {
        if ((*prices)[index] > 0.0)
        {
            snapshot_.quotes[wanted[index]] = Quote{(*prices)[index], fetched_at};
        }
    }
}

void SwingTrendStrategy::on_clock(std::vector<OrderSignal>& out)
{
    roll_day();
    const auto steady_now = std::chrono::steady_clock::now();

    if (steady_now - last_reload_ >= std::chrono::seconds(parameters_.reload_sec))
    {
        last_reload_ = steady_now;
        load_plan();
    }

    track_fills();
    update_stops();
    run_buys(out);
    run_far_cancels(out);
    run_unfilled_cancels(out);
    release_armed();
    run_sells(out);
    publish_requests();
    publish_owned();

    if (dirty_)
    {
        save_state();
    }
}

void SwingTrendStrategy::set_clock(Clock clock)
{
    clock_ = std::move(clock);
}

void SwingTrendStrategy::set_quote_source(QuoteSource source)
{
    quote_source_ = std::move(source);
}
