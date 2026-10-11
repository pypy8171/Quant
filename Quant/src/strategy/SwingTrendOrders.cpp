// strategy/SwingTrendOrders.cpp — 스윙 추세 슬리브의 주문: 시가 확인 뒤 지정가 매수, 15:19 미체결 취소, 손절선 올리기, 종가 동시호가 손절 매도.
//  스레드: 샤드 스레드(on_clock 안). prefetch 결과는 snapshot_mutex_를 쥐고 읽는다. 관련 결정: D-160.
#include "strategy/SwingTrendStrategy.h"
#include "strategy/SurgeHoldPlan.h"
#include "utils/Logger.h"
#include <algorithm>
#include <ctime>
#include <format>
#include <string>
#include <utility>
#include <vector>

OrderSignal SwingTrendStrategy::make_order(const std::string& ticker, OrderSide side, OrderType type, int quantity, double price,
                                           const std::string& reason) const
{
    OrderSignal signal     = StrategyBase::make_signal(ticker, symbol_of(ticker), side, type, quantity);
    signal.price           = type == OrderType::LIMIT ? price : 0.0;
    signal.reference_price = price; // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
    signal.account_id      = parameters_.account;
    signal.reason          = reason;
    // 계획 주문은 몇 초 늦어도 판단이 낡지 않는다 — 큐 나이 제한에서 뺀다(D-155와 같은 이유).
    signal.exempt_from_age_limit = true;
    return signal;
}

// 상태 파일·계획이 오래됐는지 본다. 오늘 새 매수를 낼 수 있으면 true.
bool SwingTrendStrategy::plan_usable()
{
    if (state_broken_)
    {
        if (!broken_logged_)
        {
            broken_logged_ = true;
            LOG_ERROR("[" + id_ + "] 상태 파일을 못 읽어 매수 없음 — " + parameters_.state_file + " 을 고친 뒤 재기동");
        }

        return false;
    }

    if (swing_trend::plan_stale(surge::date_number(plan_->as_of), surge::date_number(day_), parameters_.stale_days))
    {
        if (!plan_stale_logged_)
        {
            plan_stale_logged_ = true;
            LOG_WARN(std::format("[{}] 계획 오래됨 as_of={} — {}일을 넘어 새 매수 없음, 보유 감시만", id_, plan_->as_of, parameters_.stale_days));
        }

        return false;
    }

    return true;
}

void SwingTrendStrategy::run_buys(std::vector<OrderSignal>& out)
{
    const int hhmm = kst_hhmm(clock_());

    if (!plan_ || hhmm < parameters_.buy_start_hhmm || hhmm >= parameters_.buy_cancel_hhmm || buys_stopped_today_ || !plan_usable())
    {
        return;
    }

    // 국면 목록에서 빠졌거나 계좌 신규매수가 막혔으면(수동 정지 등 — 국면 중단은 ignores_regime_halt로 빠진다) 판단을 미룬다(종목을 판단 끝으로 표시하지 않는다 — 풀리면 그때 산다).
    if (!is_active() || entry_halted())
    {
        if (!inactive_logged_)
        {
            inactive_logged_ = true;
            LOG_INFO(std::format("[{}] 매수 보류 — {}", id_, !is_active() ? "국면 꺼짐" : "계좌 신규매수 차단"));
        }

        return;
    }

    std::vector<std::pair<const swing_trend::PlanEntry*, double>> ready;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);

        for (const auto& entry : plan_->entries)
        {
            const auto open = snapshot_.opens.find(entry.ticker);

            if (attempted_today_.count(entry.ticker) == 0 && open != snapshot_.opens.end() && open->second > 0.0)
            {
                ready.emplace_back(&entry, open->second);
            }
        }
    }

    for (const auto& [entry, open_price] : ready)
    {
        if (!buy_entry(*entry, open_price, out))
        {
            return;
        }
    }
}

// 후보 하나를 판단하고, 통과하면 상태 파일에 먼저 적은 뒤 지정가 매수를 낸다. 상태 파일 쓰기에 실패해 오늘 매수를 멈췄으면 false.
bool SwingTrendStrategy::buy_entry(const swing_trend::PlanEntry& entry, double open_price, std::vector<OrderSignal>& out)
{
    attempted_today_.insert(entry.ticker);
    dirty_ = true;

    const auto ledger   = ledger_sellable(parameters_.account, entry.ticker);
    const int  quantity = swing_trend::buy_quantity(parameters_.notional_krw, entry.entry_price);
    const bool ours     = std::any_of(positions_.begin(), positions_.end(), [&entry](const Position& position)
    {
        return position.ticker == entry.ticker;
    });
    std::string skip;

    if (std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), entry.ticker) !=
        parameters_.excluded_tickers.end())
    {
        skip = "다른 슬리브(바스켓·급등) 소유";
    }
    else if (ours)
    {
        skip = "이 전략이 이미 보유";
    }
    else if (confirmed_position(parameters_.account, entry.ticker) > 0)
    {
        skip = "계좌가 이미 보유(다른 전략)";
    }
    else if (ledger && ledger->reserved != 0)
    {
        skip = "다른 미체결 주문 있음";
    }
    else if (!swing_trend::should_buy(open_price, entry.entry_price))
    {
        skip = std::format("시가 {:.0f} ≤ 매수가 {} — 오늘은 사지 않음", open_price, entry.entry_price);
    }
    else if (quantity <= 0)
    {
        skip = std::format("수량 0(금액 {} / 매수가 {})", parameters_.notional_krw, entry.entry_price);
    }

    if (!skip.empty())
    {
        LOG_INFO(std::format("[{}] 매수 건너뜀 {} {} — {}", id_, entry.ticker, entry.name, skip));
        return true;
    }

    const std::string reason = std::format("SWING 되돌림 지정가 매수 시가 {:.0f} 매수가 {} 손절선 {} 신호일 {}", open_price, entry.entry_price,
                                           entry.cancel_below, entry.signal_date);

    if (parameters_.dry_run)
    {
        LOG_INFO(std::format("[{}] [dry-run] 매수 주문 {} {} {}주 — {}", id_, entry.ticker, entry.name, quantity, reason));
        return true;
    }

    // 상태 파일에 먼저 적고 낸다 — 내고 죽으면 재기동이 같은 매수를 다시 내지 않는다.
    Position position;
    position.ticker       = entry.ticker;
    position.entry_date   = day_;
    position.entry_price  = entry.entry_price;
    position.initial_stop = entry.cancel_below;
    position.stop         = entry.cancel_below;
    position.ordered      = quantity;
    position.status       = Status::Pending;
    position.order_number = next_client_order_number();
    positions_.push_back(std::move(position));

    if (!save_state())
    {
        // 적지 못한 매수는 내지 않는다 — 재기동이 이 주문을 모르게 된다. 오늘은 더 사지 않는다.
        positions_.pop_back();
        buys_stopped_today_ = true;
        LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패로 " + entry.ticker + " 매수를 내지 않고 오늘 매수를 멈춘다");
        return false;
    }

    OrderSignal signal = make_order(entry.ticker, OrderSide::BUY, OrderType::LIMIT, quantity, static_cast<double>(entry.entry_price), reason);
    signal.client_order_number = positions_.back().order_number;
    out.push_back(std::move(signal));
    LOG_INFO(std::format("[{}] 매수 주문 {} {} {}주 지정가 {} 시가 {:.0f} 손절선 {}", id_, entry.ticker, entry.name, quantity, entry.entry_price,
                         open_price, entry.cancel_below));
    return true;
}

// 15:19부터 안 잡힌 매수 잔량은 취소 주문을 낸다. 취소 확인 신호는 전략에 오지 않으므로, 장부의 미체결 선점이
//  0이 되는 것을 보고 track_fills가 Pending을 지운다.
void SwingTrendStrategy::run_unfilled_cancels(std::vector<OrderSignal>& out)
{
    if (parameters_.dry_run || kst_hhmm(clock_()) < parameters_.buy_cancel_hhmm)
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

        OrderSignal signal = make_order(position.ticker, OrderSide::BUY, OrderType::LIMIT, 0, 0.0, "SWING 매수 미체결 취소");
        signal.action                       = OrderAction::CANCEL;
        signal.original_client_order_number = position.order_number;
        out.push_back(std::move(signal));
        position.cancel_sent = true;
        dirty_               = true;
        LOG_INFO(std::format("[{}] 매수 미체결 취소 {} — 선점 {}주가 남아 취소 주문을 낸다", id_, position.ticker, ledger->reserved));
    }
}

// 그날 일봉을 받았으면 보유마다 손절선을 한 번 다시 본다. 손절선은 올리기만 한다.
void SwingTrendStrategy::update_stops()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    for (auto& position : positions_)
    {
        if (position.status == Status::Pending || position.stop_checked_day == day_)
        {
            continue;
        }

        const auto bars = snapshot_.bars.find(position.ticker);

        if (bars == snapshot_.bars.end() || bars->second.day != day_)
        {
            continue;
        }

        position.stop_checked_day = day_;
        const size_t  entry_index = swing_trend::entry_bar_index(bars->second.session_dates, surge::date_number(position.entry_date));
        const int64_t raised      = swing_trend::raised_stop(bars->second.lows, entry_index, parameters_.pivot_window,
                                                             std::max(position.stop, position.initial_stop));

        if (raised > position.stop)
        {
            LOG_INFO(std::format("[{}] 손절선 올림 {} {} → {} (확정 저점)", id_, position.ticker, position.stop, raised));
            position.stop = raised;
            dirty_        = true;
        }
    }
}

// 매도를 낸다. 매도가능 수량 전부를 시장가로(종가 동시호가). 팔 수량이 없으면 false.
bool SwingTrendStrategy::emit_sell(Position& position, const std::string& reason, std::vector<OrderSignal>& out)
{
    const auto sellable = ledger_sellable(parameters_.account, position.ticker);
    const int  quantity = sellable ? sellable->sellable : position.quantity;

    if (quantity <= 0)
    {
        return false;
    }

    position.status       = Status::Exiting;
    position.exit_reason  = reason;
    position.last_exit_at = clock_();
    ++position.exit_attempts;
    dirty_ = true;

    if (parameters_.dry_run)
    {
        LOG_INFO(std::format("[{}] [dry-run] 손절 매도 {} {}주 — {}", id_, position.ticker, quantity, reason));
        return true;
    }

    out.push_back(make_order(position.ticker, OrderSide::SELL, OrderType::MARKET, quantity, static_cast<double>(position.stop), reason));
    LOG_INFO(std::format("[{}] 손절 매도 {} {}주 — {} (시도 {})", id_, position.ticker, quantity, reason, position.exit_attempts));
    return true;
}

// 15:20–15:29 창의 첫 판단에서 현재가가 손절선 아래면 판다. 매도가 남았으면 간격·횟수 안에서 다시 낸다.
void SwingTrendStrategy::run_sells(std::vector<OrderSignal>& out)
{
    const std::time_t now  = clock_();
    const int         hhmm = kst_hhmm(now);

    if (hhmm < parameters_.sell_start_hhmm || hhmm > parameters_.sell_end_hhmm)
    {
        return;
    }

    std::vector<std::pair<Position*, double>> judged;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);

        for (auto& position : positions_)
        {
            if (position.status != Status::Open || position.sell_checked_day == day_)
            {
                continue;
            }

            const auto price = snapshot_.prices.find(position.ticker);

            if (price != snapshot_.prices.end())
            {
                judged.emplace_back(&position, price->second);
            }
            else if (hhmm == parameters_.sell_end_hhmm && price_missing_logged_.insert(position.ticker).second)
            {
                LOG_WARN(std::format("[{}] 손절 판단 못 함 {} — 매도 창 끝까지 현재가를 못 받았다(손절선 {})", id_, position.ticker, position.stop));
            }
        }
    }

    for (const auto& [position, price] : judged)
    {
        position->sell_checked_day = day_;

        if (!swing_trend::should_sell(hhmm, price, position->stop, parameters_.sell_start_hhmm, parameters_.sell_end_hhmm))
        {
            LOG_INFO(std::format("[{}] 손절 판단 {} 현재가 {:.0f} 손절선 {} → 보유", id_, position->ticker, price, position->stop));
            continue;
        }

        emit_sell(*position, std::format("손절 현재가 {:.0f} < 손절선 {}", price, position->stop), out);
    }

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
            const std::string reason = position.exit_reason;
            emit_sell(position, reason, out);
        }
    }
}
