// strategy/SurgeHoldStrategy.cpp — 급등 뒤 되돌림 보유 슬리브 본체: 생성·후보와 감시 목록·소유 종목 게시·하루 넘김·분 단위 순서(on_clock).
//  계획·상태 파일은 SurgeHoldState.cpp, 매수는 SurgeHoldBuys.cpp, 청산은 SurgeHoldExits.cpp에 있다. 관련 결정: D-157.
#include "strategy/SurgeHoldStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

SurgeHoldStrategy::SurgeHoldStrategy(Params parameters, OwnedSink owned_sink)
    : parameters_(std::move(parameters)), owned_sink_(std::move(owned_sink)), clock_([]
    {
        return std::time(nullptr);
    }),
      id_("SURGE_" + parameters_.label)
{
}

const char* SurgeHoldStrategy::status_text(Status status)
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

std::string SurgeHoldStrategy::describe() const
{
    return std::format("SurgeHold({} 종목당 {:.0f}원 상한 {}종목 하루 {}건 {}) 계획 {}", parameters_.label, parameters_.amount_krw,
                       parameters_.max_positions, parameters_.max_daily_buys, parameters_.dry_run ? "dry-run" : "실주문",
                       parameters_.plan_file);
}

std::string SurgeHoldStrategy::kst_date() const
{
    return kst::date_yyyymmdd(clock_());
}

// 손절·익절 주문을 내는 창 — 09:00–15:19. 장 마감 뒤 NXT 체결이나 장 시작 전에 닿은 판정은 여기서 걸러 다음 창으로 미룬다.
bool SurgeHoldStrategy::in_exit_window() const
{
    const int hhmm = kst_hhmm(clock_());
    return hhmm >= parameters_.exit_start_hhmm && hhmm < parameters_.close_start_hhmm;
}

std::vector<std::string> SurgeHoldStrategy::held_tickers() const
{
    std::vector<std::string> tickers;

    for (const auto& position : positions_)
    {
        tickers.push_back(position.ticker);
    }

    return tickers;
}

std::vector<std::string> SurgeHoldStrategy::buy_candidates() const
{
    std::vector<std::string> tickers;

    if (!plan_)
    {
        return tickers;
    }

    for (const auto& row : plan_->rows)
    {
        const bool excluded = std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) !=
                              parameters_.excluded_tickers.end();

        if (row.action == surge::Action::BuyOpen && !excluded)
        {
            tickers.push_back(row.ticker);
        }
    }

    return tickers;
}

std::vector<WatchSpec> SurgeHoldStrategy::get_watch_specifications() const
{
    // 손절·익절은 체결 가격으로 보므로 보유와 오늘 매수 후보를 체결만 구독한다(호가 칸을 아낀다).
    std::set<std::string> tickers;

    for (const auto& ticker : held_tickers())
    {
        tickers.insert(ticker);
    }

    for (const auto& ticker : buy_candidates())
    {
        tickers.insert(ticker);
    }

    std::vector<WatchSpec> specifications;

    for (const auto& ticker : tickers)
    {
        WatchSpec specification;
        specification.ticker     = ticker;
        specification.market     = Market::KR;
        specification.trade_only = true;
        specifications.push_back(std::move(specification));
    }

    return specifications;
}

void SurgeHoldStrategy::rebuild_index()
{
    index_of_symbol_.clear();

    for (size_t index = 0; index < positions_.size(); ++index)
    {
        const symbol::SymbolId symbol = symbol_of(positions_[index].ticker);

        if (symbol != symbol::kNone)
        {
            index_of_symbol_[symbol] = index;
        }
    }
}

void SurgeHoldStrategy::publish_owned()
{
    // dry_run은 주문을 내지 않으니 다른 슬리브의 슬롯 계산을 건드리지 않는다.
    if (parameters_.dry_run)
    {
        return;
    }

    owned_scratch_.clear();

    for (const auto& position : positions_)
    {
        owned_scratch_.push_back(position.ticker);
    }

    // 오늘 매수할 후보도 넣는다 — 체결되는 순간부터 슬롯 계산 밖이어야 장중 전략의 강제청산 대상이 되지 않는다.
    //  다른 슬리브가 이미 든 후보는 넣지 않는다(겹침이라 사지 않고, 넣으면 그 슬리브 보유가 슬롯 밖으로 빠진다).
    if (plan_ && surge::plan_fresh(surge::date_number(plan_->as_of), surge::date_number(kst_date())))
    {
        for (const auto& row : plan_->rows)
        {
            const bool candidate = row.action == surge::Action::BuyOpen &&
                                   std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) ==
                                       parameters_.excluded_tickers.end();

            if (!candidate || attempted_today_.count(row.ticker) != 0)
            {
                continue;
            }

            const bool ours = std::any_of(positions_.begin(), positions_.end(), [&row](const Position& position)
            {
                return position.ticker == row.ticker;
            });

            if (!ours && confirmed_position(parameters_.account, row.ticker) > 0)
            {
                continue;
            }

            owned_scratch_.push_back(row.ticker);
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

void SurgeHoldStrategy::on_start()
{
    load_plan();
    roll_day();
    rebuild_index();
    publish_owned();
    LOG_INFO("[" + id_ + "] 시작 " + describe() + " 보유 " + std::to_string(positions_.size()) + "종목");
}

void SurgeHoldStrategy::on_stop()
{
    save_state();
}

void SurgeHoldStrategy::roll_day()
{
    const std::string today = kst_date();

    if (today == day_)
    {
        return;
    }

    day_                   = today;
    bought_today_          = 0;
    attempted_today_.clear();
    dry_sent_.clear();
    buys_stopped_today_    = false;
    plan_stale_logged_     = false;
    plan_done_logged_      = false;
    regime_unknown_logged_ = false;

    // 어제 다 못 판 매도 — 주문은 밤사이 사라지므로 시도 횟수를 0으로 되돌리고 오늘 창에서 다시 판다.
    for (auto& position : positions_)
    {
        if (position.status != Status::Exiting)
        {
            continue;
        }

        const auto sellable = ledger_sellable(parameters_.account, position.ticker);

        if (sellable && sellable->sellable > 0)
        {
            position.status        = Status::Open;
            position.exit_attempts = 0;
            position.exit_due      = position.exit_reason;
            LOG_INFO(std::format("[{}] 어제 남은 매도 {} {}주 — 오늘 창에서 다시 판다({})", id_, position.ticker, sellable->sellable,
                                 position.exit_reason));
        }
    }

    save_state();
}

bool SurgeHoldStrategy::wants_clock() const
{
    return true;
}

OrderSignal SurgeHoldStrategy::make_signal(const std::string& ticker, OrderSide side, int quantity, double reference_price,
                                           const std::string& reason) const
{
    OrderSignal signal = StrategyBase::make_signal(ticker, symbol_of(ticker), side, OrderType::MARKET, quantity);
    signal.price           = 0.0;
    signal.reference_price = reference_price; // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
    signal.account_id      = parameters_.account;
    signal.reason          = reason;
    // 계획 주문은 몇 초 늦어도 판단이 낡지 않는다 — 큐 나이 제한에서 뺀다(D-155와 같은 이유).
    signal.exempt_from_age_limit = true;
    return signal;
}

void SurgeHoldStrategy::on_clock(std::vector<OrderSignal>& out)
{
    roll_day();
    const auto steady_now = std::chrono::steady_clock::now();

    if (steady_now - last_reload_ >= std::chrono::seconds(parameters_.reload_sec))
    {
        last_reload_ = steady_now;
        load_plan();
    }

    track_fills();
    run_buys(out);
    run_unfilled_cancels(out);

    if (in_exit_window())
    {
        run_due_exits(out);
        run_exit_retries(out);
    }

    run_close_exits(out);
    publish_owned();

    if (dirty_)
    {
        save_state();
    }
}

void SurgeHoldStrategy::set_clock(Clock clock)
{
    clock_ = std::move(clock);
}

void SurgeHoldStrategy::set_regime_check(RegimeCheck check)
{
    regime_check_ = std::move(check);
}
