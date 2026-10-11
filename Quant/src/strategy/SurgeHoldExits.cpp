// strategy/SurgeHoldExits.cpp — 급등 보유 슬리브의 청산: 손절·익절 판정(on_trade_batch)·창 안 매도·종가 만기·재시도.
//  관련 결정: D-157.
#include "strategy/SurgeHoldStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <ctime>
#include <format>
#include <string>
#include <utility>
#include <vector>

// 매도를 낸다. 저장은 on_clock이 한다(체결 틱 경로에서 파일을 쓰지 않는다). 팔 수량이 없으면 false.
bool SurgeHoldStrategy::emit_sell(Position& position, const std::string& reason, std::vector<OrderSignal>& out)
{
    const auto sellable = ledger_sellable(parameters_.account, position.ticker);
    const int  quantity = std::min(position.quantity, sellable ? sellable->sellable : position.quantity);

    if (quantity <= 0)
    {
        return false;
    }

    position.status       = Status::Exiting;
    position.exit_reason  = reason;
    position.exit_due.clear();
    position.last_exit_at = clock_();
    ++position.exit_attempts;
    dirty_ = true;

    if (parameters_.dry_run)
    {
        LOG_INFO(std::format("[{}] [dry-run] 매도 {} {}주 — {}", id_, position.ticker, quantity, reason));
        return true;
    }

    out.push_back(make_signal(position.ticker, OrderSide::SELL, quantity, position.entry_price, reason));
    LOG_INFO(std::format("[{}] 매도 {} {}주 — {} (시도 {})", id_, position.ticker, quantity, reason, position.exit_attempts));
    return true;
}

// 창 밖에서 닿은 손절·익절, 갭 하락 체결, 어제 못 판 매도를 창이 열리면 낸다.
void SurgeHoldStrategy::run_due_exits(std::vector<OrderSignal>& out)
{
    for (auto& position : positions_)
    {
        if (position.status == Status::Open && !position.exit_due.empty())
        {
            const std::string reason = position.exit_due;
            emit_sell(position, reason, out);
        }
    }
}

void SurgeHoldStrategy::run_close_exits(std::vector<OrderSignal>& out)
{
    const int hhmm = kst_hhmm(clock_());

    if (hhmm < parameters_.close_start_hhmm || hhmm > parameters_.close_end_hhmm)
    {
        return;
    }

    const std::time_t now = clock_();

    // 처음이면 내고, 이미 냈는데 남았으면 간격·횟수 안에서 다시 낸다.
    const auto close_sell = [this, now, &out](Position& position, const std::string& reason)
    {
        if (position.entry_date == day_)
        {
            return;
        }

        if (position.status == Status::Open)
        {
            emit_sell(position, reason, out);
            return;
        }

        if (position.status == Status::Exiting && position.exit_attempts < parameters_.max_exit_attempts &&
            now - position.last_exit_at >= parameters_.exit_retry_sec)
        {
            emit_sell(position, position.exit_reason, out);
        }
    };

    const int  today      = surge::date_number(day_);
    const bool plan_today = plan_ && surge::plan_fresh(surge::date_number(plan_->as_of), today);

    if (plan_ && surge::date_number(plan_->as_of) < today)
    {
        for (const auto& row : plan_->rows)
        {
            if (row.action != surge::Action::ExitClose)
            {
                continue;
            }

            for (auto& position : positions_)
            {
                if (position.ticker == row.ticker)
                {
                    close_sell(position, std::format("만기 {}거래일 종가 동시호가", row.held_days));
                }
            }
        }
    }

    // 계획이 없는 날의 백스톱 — 17:00 계획 생성이 며칠 멈춰도 보유가 끝없이 남지 않게 평일 수로 판다.
    if (!plan_today)
    {
        for (auto& position : positions_)
        {
            const int weekdays = surge::weekdays_between(surge::date_number(position.entry_date), today);

            if (weekdays >= parameters_.backstop_weekdays)
            {
                close_sell(position, std::format("만기 백스톱 평일 {}일(계획 없음) 종가 동시호가", weekdays));
            }
        }
    }
}

void SurgeHoldStrategy::run_exit_retries(std::vector<OrderSignal>& out)
{
    const std::time_t now = clock_();

    for (auto& position : positions_)
    {
        if (position.status != Status::Exiting || position.exit_attempts >= parameters_.max_exit_attempts ||
            now - position.last_exit_at < parameters_.exit_retry_sec)
        {
            continue;
        }

        const auto sellable = ledger_sellable(parameters_.account, position.ticker);

        if (sellable && sellable->sellable > 0)
        {
            emit_sell(position, position.exit_reason, out);
        }
    }
}

void SurgeHoldStrategy::on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out)
{
    const auto found = index_of_symbol_.find(trade.symbol_id);

    if (found == index_of_symbol_.end() || found->second >= positions_.size())
    {
        return;
    }

    Position& position = positions_[found->second];

    if (position.status != Status::Open || position.entry_price <= 0.0)
    {
        return;
    }

    const surge::Exit exit = surge::exit_check(trade.price, position.stop, position.take);

    if (exit == surge::Exit::None)
    {
        return;
    }

    // 창 밖이고 이미 다음 창으로 넘긴 판정이면 사유를 만들지 않고 돌아간다. 장 마감 뒤 NXT 체결과 장 전 체결이
    //  올 때마다 문자열을 만들었다가 버리던 자리다.
    const bool window_open = in_exit_window();

    if (!window_open && !position.exit_due.empty())
    {
        return;
    }

    std::string reason = exit == surge::Exit::Stop ? std::format("손절 체결가 {:.0f} ≤ {:.0f}", trade.price, position.stop)
                                                   : std::format("익절 체결가 {:.0f} ≥ {:.0f}", trade.price, position.take);

    if (window_open)
    {
        emit_sell(position, reason, out);
        return;
    }

    // 장 마감 뒤 NXT 체결이나 장 시작 전 — 주문은 내지 않고 다음 창으로 넘긴다(exit_due가 비어 있을 때만 여기 온다).
    LOG_INFO(std::format("[{}] {} {} — 매도 창 밖이라 다음 창에서 판다", id_, position.ticker, reason));
    position.exit_due = std::move(reason);
    dirty_            = true;
}
