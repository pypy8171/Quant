// strategy/SurgeHoldBuys.cpp — 급등 보유 슬리브의 장 시작 동시호가 매수와 09:30 미체결 취소.
//  관련 결정: D-157.
#include "strategy/SurgeHoldStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

std::optional<bool> SurgeHoldStrategy::regime_allows_buy() const
{
    // 엔진의 국면 선택은 09:00 첫 장중 사이클에 적용된다 — 08:50의 is_active()는 어제 값이라 파일을 직접 본다.
    if (regime_check_)
    {
        return regime_check_();
    }

    return is_active();
}

bool SurgeHoldStrategy::bought_same_signal(const std::string& ticker, const std::string& surge_date) const
{
    if (surge_date.empty())
    {
        return false;
    }

    for (const auto& position : positions_)
    {
        if (position.ticker == ticker && position.surge_date == surge_date)
        {
            return true;
        }
    }

    for (const auto& record : closed_)
    {
        const auto record_ticker = record.find("ticker");
        const auto record_surge_date     = record.find("d0");

        if (record_ticker != record.end() && record_surge_date != record.end() && record_ticker->is_string() && record_surge_date->is_string() &&
            record_ticker->get_ref<const std::string&>() == ticker && record_surge_date->get_ref<const std::string&>() == surge_date)
        {
            return true;
        }
    }

    return false;
}

void SurgeHoldStrategy::run_buys(std::vector<OrderSignal>& out)
{
    const std::optional<int> as_of = buy_plan_as_of();

    if (!as_of)
    {
        return;
    }

    const std::optional<bool> allowed = regime_allows_buy();

    if (!allowed)
    {
        if (!regime_unknown_logged_)
        {
            regime_unknown_logged_ = true;
            LOG_WARN("[" + id_ + "] 국면 파일을 못 읽음(없음·오래됨·판정 보류) — 매수 보류, 매수 창 안에서 다시 본다");
        }

        return;
    }

    bool changed = false;

    if (*as_of != last_executed_as_of_)
    {
        last_executed_as_of_ = *as_of;
        executed_on_         = day_;
        changed              = true;
    }

    // 행은 거래대금 비중 큰 순으로 정렬돼 있다(parse_plan) — 상한에 걸리면 앞의 것을 산다.
    for (const auto& row : plan_->rows)
    {
        if (row.action != surge::Action::BuyOpen || attempted_today_.count(row.ticker) != 0)
        {
            continue;
        }

        changed = true;

        if (!buy_row(row, *allowed, out))
        {
            return;
        }
    }

    if (changed)
    {
        save_state();
    }
}

// 매수 창·상태 파일·계획 신선도·한 번만 집행·남은 매수 행을 차례로 본다. 이번 분에 살 계획이 있으면 그 as_of.
std::optional<int> SurgeHoldStrategy::buy_plan_as_of()
{
    const int hhmm = kst_hhmm(clock_());

    if (!plan_ || hhmm < parameters_.buy_start_hhmm || hhmm > parameters_.buy_end_hhmm || buys_stopped_today_)
    {
        return std::nullopt;
    }

    if (state_broken_)
    {
        if (!broken_logged_)
        {
            broken_logged_ = true;
            LOG_ERROR("[" + id_ + "] 상태 파일을 못 읽어 매수 없음 — " + parameters_.state_file + " 을 고친 뒤 재기동");
        }

        return std::nullopt;
    }

    const int as_of = surge::date_number(plan_->as_of);

    if (!surge::plan_fresh(as_of, surge::date_number(day_)))
    {
        if (!plan_stale_logged_)
        {
            plan_stale_logged_ = true;
            LOG_WARN("[" + id_ + "] 계획 as_of=" + plan_->as_of + " 가 오늘 쓸 것이 아님 — 매수 없음");
        }

        return std::nullopt;
    }

    // 계획 하나는 한 번만 집행한다 — 17:00 계획 생성이 실패한 날 어제 계획으로 또 사지 않는다.
    if (as_of < last_executed_as_of_ || (as_of == last_executed_as_of_ && executed_on_ != day_))
    {
        if (!plan_done_logged_)
        {
            plan_done_logged_ = true;
            LOG_WARN(std::format("[{}] 계획 as_of={} 는 {} 에 이미 집행함 — 새 계획이 없어 매수 없음", id_, plan_->as_of, executed_on_));
        }

        return std::nullopt;
    }

    const bool pending_rows = std::any_of(plan_->rows.begin(), plan_->rows.end(), [this](const surge::PlanRow& row)
    {
        return row.action == surge::Action::BuyOpen && attempted_today_.count(row.ticker) == 0;
    });

    if (!pending_rows)
    {
        return std::nullopt;
    }

    return as_of;
}

// 매수 행 하나를 판정하고, 통과하면 상태 파일에 먼저 적은 뒤 동시호가 매수를 낸다. 상태 파일 쓰기에 실패해
//  오늘 매수를 멈췄으면 false.
bool SurgeHoldStrategy::buy_row(const surge::PlanRow& row, bool allowed, std::vector<OrderSignal>& out)
{
    int open_positions = 0;

    for (const auto& position : positions_)
    {
        open_positions += position.status != Status::Exiting ? 1 : 0;
    }

    const auto        ledger = ledger_sellable(parameters_.account, row.ticker);
    surge::EntryCheck check;
    check.active           = allowed;
    check.entry_halted     = entry_halted();
    check.excluded         = std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) !=
                             parameters_.excluded_tickers.end();
    check.same_signal      = bought_same_signal(row.ticker, row.surge_date);
    check.account_position = confirmed_position(parameters_.account, row.ticker);
    check.account_reserved = ledger ? ledger->reserved : 0;
    check.open_positions   = open_positions;
    check.max_positions    = parameters_.max_positions;
    check.bought_today     = bought_today_;
    check.max_daily_buys   = parameters_.max_daily_buys;
    check.quantity         = surge::buy_quantity(parameters_.amount_krw, row.reference_close);
    const surge::Skip skip = surge::entry_skip(check);
    attempted_today_.insert(row.ticker);

    if (skip != surge::Skip::None)
    {
        LOG_INFO(std::format("[{}] 매수 건너뜀 {} — {} (신호일 {} 급등일 {})", id_, row.ticker, surge::skip_text(skip), row.signal_date,
                             row.surge_date));
        return true;
    }

    const std::string reason = std::format("SURGE 시가 매수 신호일 {} 기준 저가 {:.0f} 손절 {:.0f}% 익절 {:.0f}%", row.signal_date,
                                           row.stop_basis_low, row.stop_percent, row.take_percent);

    if (parameters_.dry_run)
    {
        if (dry_sent_.insert(row.ticker).second)
        {
            LOG_INFO(std::format("[{}] [dry-run] 동시호가 매수 {} {}주 기준가 {:.0f} — {}", id_, row.ticker, check.quantity,
                                 row.reference_close, reason));
        }

        return true;
    }

    // 상태 파일에 먼저 적고 낸다 — 내고 죽으면 재기동이 같은 매수를 다시 내지 않는다.
    Position position;
    position.ticker         = row.ticker;
    position.surge_date             = row.surge_date;
    position.entry_date     = day_;
    position.ordered        = check.quantity;
    position.stop_basis_low = row.stop_basis_low;
    position.stop_percent   = row.stop_percent;
    position.take_percent   = row.take_percent;
    position.stop           = surge::stop_price(row.stop_basis_low, row.stop_percent);
    position.take           = surge::take_price(row.reference_close, row.take_percent);
    position.status         = Status::Pending;
    position.order_number   = next_client_order_number();
    positions_.push_back(std::move(position));
    ++bought_today_;

    if (!save_state())
    {
        // 적지 못한 매수는 내지 않는다 — 재기동이 이 주문을 모르게 된다. 오늘은 더 사지 않는다.
        positions_.pop_back();
        --bought_today_;
        buys_stopped_today_ = true;
        LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패로 " + row.ticker + " 매수를 내지 않고 오늘 매수를 멈춘다");
        rebuild_index();
        return false;
    }

    rebuild_index();
    OrderSignal signal         = make_signal(row.ticker, OrderSide::BUY, check.quantity, row.reference_close, reason);
    signal.opening_auction     = true;
    signal.client_order_number = positions_.back().order_number;
    out.push_back(std::move(signal));
    LOG_INFO(std::format("[{}] 동시호가 매수 {} {}주 기준가 {:.0f} — {}", id_, row.ticker, check.quantity, row.reference_close, reason));

    return true;
}

// 09:30까지 안 잡힌 매수 잔량은 취소 주문을 낸다. 취소 확인 신호는 전략에 오지 않으므로, 장부의 미체결 선점이
//  0이 되는 것을 보고 track_fills가 PENDING을 지운다.
void SurgeHoldStrategy::run_unfilled_cancels(std::vector<OrderSignal>& out)
{
    if (parameters_.dry_run || kst_hhmm(clock_()) < parameters_.fill_wait_hhmm)
    {
        return;
    }

    for (auto& position : positions_)
    {
        if (position.entry_date != day_ || position.order_number == 0 || position.cancel_sent || position.status == Status::Exiting)
        {
            continue;
        }

        const auto ledger = ledger_sellable(parameters_.account, position.ticker);

        if (!ledger || ledger->reserved <= 0)
        {
            continue;
        }

        OrderSignal signal                  = make_signal(position.ticker, OrderSide::BUY, 0, 0.0, "SURGE 매수 미체결 취소");
        signal.action                       = OrderAction::CANCEL;
        signal.original_client_order_number = position.order_number;
        out.push_back(std::move(signal));
        position.cancel_sent = true;
        dirty_               = true;
        LOG_WARN(std::format("[{}] 매수 미체결 {} — {} 까지 선점 {}주가 남아 취소 주문을 낸다", id_, position.ticker,
                             parameters_.fill_wait_hhmm, ledger->reserved));
    }
}
