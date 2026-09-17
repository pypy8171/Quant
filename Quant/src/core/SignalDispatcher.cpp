// 신호 디스패처 구현 — strategy_thread 전용. 큐에 넣는 자리는 emit 하나다. [why D-063]
#include "core/SignalDispatcher.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"

namespace dispatch
{
std::vector<OrderSignal> force_liq_orders(const std::vector<OrderGate::HeldPos>& held, const ReservedFn& reserved)
{
    std::vector<OrderSignal> out;

    for (const auto& holding : held)
    {
        const int resv         = reserved(holding.account, holding.ticker);
        const int sell_pending = (resv < 0) ? -resv : 0; // 이미 낸 미체결 매도
        const int sellable     = holding.quantity - sell_pending;

        if (sellable <= 0)
        {
            continue;
        }

        OrderSignal signal;
        signal.ticker      = holding.ticker;
        signal.account_id  = holding.account;
        signal.side        = OrderSide::SELL;
        signal.type        = OrderType::MARKET;
        signal.quantity    = sellable;
        signal.price       = 0.0;
        signal.ref_price   = holding.average_price;
        signal.strategy_id = "FORCE_LIQ";
        signal.reason      = "강제청산(force_liquidate) 보유=" + std::to_string(holding.quantity) +
                        " 미체결매도=" + std::to_string(sell_pending);
        out.push_back(std::move(signal));
    }

    return out;
}

std::vector<OrderSignal> trim_orders(const std::vector<OrderGate::HeldPos>& held, double cap_notional,
                                     const ReservedFn& reserved)
{
    std::vector<OrderSignal> out;

    if (cap_notional <= 0.0)
    {
        return out;
    }

    for (const auto& holding : held)
    {
        if (holding.quantity <= 0 || holding.average_price <= 0.0)
        {
            continue;
        }

        const int cap_qty = static_cast<int>(cap_notional / holding.average_price);
        int       excess  = holding.quantity - cap_qty;

        if (excess <= 0)
        {
            continue;
        }

        const int resv = reserved(holding.account, holding.ticker);

        if (resv < 0)
        {
            excess -= -resv;
        }

        if (excess <= 0)
        {
            continue;
        }

        OrderSignal signal;
        signal.ticker      = holding.ticker;
        signal.account_id  = holding.account;
        signal.side        = OrderSide::SELL;
        signal.type        = OrderType::MARKET;
        signal.quantity    = excess;
        signal.price       = 0.0;
        signal.ref_price   = holding.average_price;
        signal.strategy_id = "LIMIT_TRIM";
        signal.reason      = "종목당 명목 한도 초과분 정리 보유=" + std::to_string(holding.quantity) +
                        " 한도수량=" + std::to_string(cap_qty) +
                        " 평단=" + std::to_string(static_cast<long long>(holding.average_price));
        out.push_back(std::move(signal));
    }

    return out;
}

std::string describe(const OrderSignal& signal, const std::string& label)
{
    const char* act = (signal.action == OrderAction::CANCEL)  ? "취소 "
                    : (signal.action == OrderAction::REPLACE) ? "정정 "
                                                           : "";
    const std::string tgt = signal.orig_client_oid.empty() ? std::string() : " 대상=" + signal.orig_client_oid;
    return "[" + signal.strategy_id + "] " + label + " " + act + (signal.side == OrderSide::BUY ? "BUY" : "SELL") + " " +
           std::to_string(signal.quantity) + tgt + (signal.reason.empty() ? "" : " | 근거: " + signal.reason);
}
} // namespace dispatch

SignalDispatcher::SignalDispatcher(OrderGate& gate, Sink sink, Clock::time_point now)
    : gate_(gate), sink_(std::move(sink)), last_liq_(now), trim_at_(now + std::chrono::seconds(20))
{
}

void SignalDispatcher::emit(const OrderSignal& in)
{
    OrderSignal signal = in;
    signal.sequence         = ++seq_;
    signal.signal_at_ns = trace::now_ns();
    LOG_INFO("[Strategy] 신호: " + dispatch::describe(signal, label(signal.ticker)));
    sink_(signal);
}

void SignalDispatcher::from_strategy(bool active, const std::string& strategy_id, const OrderSignal& signal)
{
    // 국면·유니버스 게이트 적용 지점(active = 국면 축 AND 유니버스 축, D-077). set_active로 표시만 해 두고 여기서 보지 않으면
    //  국면-전략 자동선택이 아무것도 막지 않는다(2026-09-08 확인). 비활성 전략이 보유분을 못 팔면 보호가 사라진다.
    if (!active && signal.action == OrderAction::NEW && signal.side == OrderSide::BUY)
    {
        return;
    }

    // 청산 관리가 맡은 티커는 스캔 슬리브가 새로 사지도, 팔지도 않는다. 소유자를 하나로 두지 않으면 청산 관리가
    //  턴 물량을 스캔 전략이 되사는 회전이 나고, 매도가 둘에서 나가면 같은 보유분에 두 장의 매도가 걸린다
    //  (sellable_qty 클램프가 있어도 순서에 따라 한쪽이 0을 받아 분할 주문을 3초마다 되감는다). 취소·정정은
    //  통과한다 — 이미 낸 주문을 거두는 길까지 막으면 미체결이 미연결 주문이 된다.
    if (signal.action == OrderAction::NEW && guardian_ && guardian_(signal.ticker) && !strategy_id.starts_with("ITB_"))
    {
        if (guard_logged_.insert(signal.ticker).second)
        {
            LOG_INFO("[Engine] 청산 관리 보유종목 신규 " + std::string(signal.side == OrderSide::BUY ? "매수" : "매도") +
                     " 차단 " + label(signal.ticker) + " (요청 " + strategy_id + ") — 매매 소유권은 청산 관리에 있다");
        }

        return;
    }

    submit(signal);
}

void SignalDispatcher::submit(const OrderSignal& signal)
{
    // 교체 진입 — 슬롯이 꽉 찬 상태에서 더 높은 점수의 신규 종목이 오면 최약체를 먼저 비운다. 비우고 끝내는
    //  이유: 매도 체결은 비동기라 같은 틱에 매수를 붙이면 노출이 이중 계상된다. 게이트가 빈 자리를 이 종목에게
    //  예약해 두고, 매수 신호는 여기서 들고 있다가 자리가 나면 낸다. 흘리기만 하면 안 되는 이유: 전략은 자기
    //  예약이 살아 있다고 낙관하므로(계획 시그니처 가드) 게이트 거부를 모르고 다시 내지 않는다. 그러면 예약된
    //  슬롯이 displace_slot_hold_sec 동안 비어 있다가 만료되고, 그동안 다른 종목까지 "예약분" 거부를 받는다
    //  (09-11 10:05 322000: 교체 매도 뒤 매수는 40>=40 거부, 5분간 아무도 못 삼).
    const auto& gcfg    = gate_.config();
    const bool  buy_new = signal.side == OrderSide::BUY && signal.action == OrderAction::NEW;

    if (!held_ticker_.empty() && signal.ticker == held_ticker_)
    {
        if (signal.action == OrderAction::CANCEL)
        {
            held_.clear(); // 전략이 분할 매수를 다시 깐다 — 새 rung이 뒤따른다
        }
        else if (buy_new && Clock::now() < held_until_ && gate_.capacity_full())
        {
            held_.push_back(signal); // 아직 자리가 안 났다 — 같은 분할 매수의 다음 rung
            return;
        }
    }

    // entry_snapshot()으로 position/reserved/slots_full을 한 번의 잠금에서 함께 읽는다 — 따로 세 번
    //  잠그면(구 코드) 그 사이 주문 스레드가 positions_/reserved_를 바꿔 낡은 조합을 볼 수 있다.
    //  [why D-086]
    const auto snapshot = buy_new ? gate_.entry_snapshot(signal.account_id, signal.ticker) : OrderGate::EntrySnapshot{};

    if (gcfg.displace_enabled && buy_new && snapshot.position == 0 && snapshot.reserved == 0 &&
        (snapshot.slots_full || gate_.capacity_full()))
    {
        const auto plan = gate_.plan_displacement(signal.account_id, signal.ticker);

        if (plan.ok)
        {
            OrderSignal ev;
            ev.ticker      = plan.ticker;
            ev.account_id  = plan.account;
            ev.side        = OrderSide::SELL;
            ev.type        = OrderType::MARKET;
            ev.quantity    = plan.quantity;
            ev.price       = 0.0;
            ev.ref_price   = plan.average_price; // 시장가 명목 백스톱이 우회되지 않게 평단을 stamp
            ev.strategy_id = "DISPLACE";
            ev.reason      = plan.reason;
            LOG_INFO("[Displace] " + label(plan.ticker) + " 전량 매도 " + std::to_string(plan.quantity) + "주 — " +
                     plan.reason);
            emit(ev);
            gate_.note_displacement(plan, signal.ticker);
            held_.clear();
            held_ticker_ = signal.ticker;
            held_until_  = Clock::now() +
                          std::chrono::seconds(gcfg.displace_slot_hold_sec > 0 ? gcfg.displace_slot_hold_sec : 120);
            held_.push_back(signal); // 매도 체결로 자리가 나면 flush_held가 낸다
            return;
        }
    }

    emit(signal);
}

void SignalDispatcher::flush_held(Clock::time_point now)
{
    if (held_ticker_.empty())
    {
        return;
    }

    // 예약 시한이 지나면 버린다 — 그 뒤엔 게이트 예약도 풀려 있어 전략의 다음 재구성이 보통 경로로 들어온다.
    if (now >= held_until_)
    {
        if (!held_.empty())
        {
            LOG_WARN("[Displace] 보류 매수 만료 " + label(held_ticker_) + " " + std::to_string(held_.size()) +
                     "건 — 자리가 안 나 버린다");
        }

        held_.clear();
        held_ticker_.clear();
        return;
    }

    if (held_.empty() || gate_.capacity_full())
    {
        return;
    }

    LOG_INFO("[Displace] 자리가 나 보류 매수 " + std::to_string(held_.size()) + "건 발주 " + label(held_ticker_));

    for (const auto& held_signal : held_)
    {
        emit(held_signal);
    }

    held_.clear();
}

void SignalDispatcher::force_liquidate(Clock::time_point now)
{
    // entry_halt가 함께 켜져 SELL만 통과한다. 대량은 fat-finger·초당 한도에 일부 막힐 수 있으나 다음 주기에
    //  잔량이 다시 나가 미완료로 남지 않는다(G3-2).
    if (now - last_liq_ < liq_interval_)
    {
        return;
    }

    last_liq_ = now;

    for (const auto& liquidation_signal : dispatch::force_liq_orders(gate_.snapshot_positions(),
                                                    [this](const std::string& argument, const std::string& ticker)
                                                    { return gate_.reserved(argument, ticker); }))
    {
        submit(liquidation_signal);
    }
}

void SignalDispatcher::trim_excess_once(Clock::time_point now)
{
    // 재기동으로 미체결 분할 매수 기억이 사라지면 게이트가 보유를 0으로 보고 같은 층을 다시 깔아 한도를 넘겨
    //  체결시킨다(09-08 047050: 한도 800만인데 명목 2,080만). 넘긴 채로 두면 다음 재기동에서 또 얹히므로, 시드가
    //  끝난 뒤 초과분을 시장가로 덜어낸다. 전량 청산이 아니라 한도까지만 줄인다.
    if (trim_done_ || now < trim_at_)
    {
        return;
    }

    trim_done_ = true;

    for (const auto& trim_signal : dispatch::trim_orders(gate_.snapshot_positions(), gate_.config().max_notional_per_ticker,
                                               [this](const std::string& argument, const std::string& ticker)
                                               { return gate_.reserved(argument, ticker); }))
    {
        LOG_WARN("[Engine] 한도 초과분 정리 " + label(trim_signal.ticker) + " 매도 " + std::to_string(trim_signal.quantity) + "주 — " +
                 trim_signal.reason);
        submit(trim_signal);
    }
}
