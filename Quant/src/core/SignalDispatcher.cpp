// 신호 디스패처 구현 — strategy_thread 전용. 큐에 넣는 자리는 emit 하나다. [why D-063]
#include "core/SignalDispatcher.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <algorithm>
#include <utility>

namespace
{
} // namespace

namespace dispatch
{
std::vector<OrderSignal> force_liquidation_orders(const std::vector<OrderGate::HeldPos>& held, const SellPendingFn& sell_pending_of,
                                                  strategy_table::StrategyId strategy)
{
    std::vector<OrderSignal> out;

    for (const auto& holding : held)
    {
        const int sell_pending = sell_pending_of(holding.account, holding.symbol); // 이미 낸 미체결 매도
        const int sellable     = holding.quantity - sell_pending;

        if (sellable <= 0)
        {
            continue;
        }

        OrderSignal signal;
        signal.ticker      = holding.ticker;
        signal.symbol_id   = holding.symbol;
        signal.account_id  = holding.account;
        signal.side        = OrderSide::SELL;
        signal.type        = OrderType::MARKET;
        signal.quantity    = sellable;
        signal.price       = 0.0;
        signal.reference_price   = holding.average_price;
        signal.strategy_id    = "FORCE_LIQ";
        signal.strategy_index = strategy;
        signal.reason      = "강제청산(force_liquidate) 보유=" + std::to_string(holding.quantity) +
                        " 미체결매도=" + std::to_string(sell_pending);
        out.push_back(std::move(signal));
    }

    return out;
}

std::vector<OrderSignal> trim_orders(const std::vector<OrderGate::HeldPos>& held, double cap_notional,
                                     const SellPendingFn& sell_pending_of, strategy_table::StrategyId strategy)
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

        const int cap_quantity = static_cast<int>(cap_notional / holding.average_price);
        int       excess  = holding.quantity - cap_quantity;

        if (excess <= 0)
        {
            continue;
        }

        excess -= sell_pending_of(holding.account, holding.symbol);

        if (excess <= 0)
        {
            continue;
        }

        OrderSignal signal;
        signal.ticker      = holding.ticker;
        signal.symbol_id   = holding.symbol;
        signal.account_id  = holding.account;
        signal.side        = OrderSide::SELL;
        signal.type        = OrderType::MARKET;
        signal.quantity    = excess;
        signal.price       = 0.0;
        signal.reference_price   = holding.average_price;
        signal.strategy_id    = "LIMIT_TRIM";
        signal.strategy_index = strategy;
        signal.reason      = "종목당 명목 한도 초과분 정리 보유=" + std::to_string(holding.quantity) +
                        " 한도수량=" + std::to_string(cap_quantity) +
                        " 평단=" + std::to_string(static_cast<long long>(holding.average_price));
        out.push_back(std::move(signal));
    }

    return out;
}

std::string describe(const OrderSignal& signal, const std::string& label)
{
    const char* action_text = (signal.action == OrderAction::CANCEL)  ? "취소 "
                    : (signal.action == OrderAction::REPLACE) ? "정정 "
                                                           : "";
    const std::string tgt = signal.original_client_order_id.empty() ? std::string() : " 대상=" + signal.original_client_order_id;
    return "[" + signal.strategy_id + "] " + label + " " + action_text + (signal.side == OrderSide::BUY ? "BUY" : "SELL") + " " +
           std::to_string(signal.quantity) + tgt + (signal.reason.empty() ? "" : " | 근거: " + signal.reason);
}
} // namespace dispatch

SignalDispatcher::SignalDispatcher(OrderGate& gate, const ipc::LedgerSnapshot& ledger, Sink sink, Clock::time_point now,
                                   SystemIds system_ids)
    : gate_(gate), ledger_(ledger), sink_(std::move(sink)), force_liquidation_index_(system_ids.force_liquidation),
      limit_trim_index_(system_ids.limit_trim),
      guard_logged_(gate.ledger().symbols().capacity(), false), sell_halt_logged_(gate.ledger().symbols().capacity(), false),
      last_liquidation_(now), trim_at_(now + std::chrono::seconds(20))
{
}

symbol::SymbolId SignalDispatcher::symbol_of(const OrderSignal& signal) const
{
    // 찾기만 한다 — 종목 표에 줄을 더하는 것은 주문 쪽이다. 처음 보는 종목은 여기서 kNone이고,
    //  주문 스레드가 받는 자리에서 번호를 준다. 그 사이 종목당 한 번 로그(mark_once)가 한 줄 덜 나가는 것이
    //  유일한 차이다 — 기동·시드·피드를 한 번이라도 지난 종목은 이미 번호가 있다. [why D-114]
    return signal.symbol_id != symbol::kNone ? signal.symbol_id : gate_.ledger().symbol_id_of(signal.ticker);
}

bool SignalDispatcher::mark_once(std::vector<bool>& flags, symbol::SymbolId symbol)
{
    if (symbol >= flags.size())
    {
        return false; // 테이블 밖 번호(kNone 포함)는 로그를 세지 않는다
    }

    if (flags[symbol])
    {
        return false;
    }

    flags[symbol] = true;
    return true;
}

void SignalDispatcher::emit(OrderSignal signal)
{
    signal.sequence         = ++sequence_;
    signal.signal_at_ns = trace::now_ns();
    LOG_INFO("[Strategy] 신호: " + dispatch::describe(signal, label(signal.ticker)));
    sink_(signal);
}

void SignalDispatcher::from_strategy(bool active, bool exit_manager, const OrderSignal& signal)
{
    // 국면·유니버스 게이트 적용 지점(active = 국면 축 AND 유니버스 축, D-077). set_active로 표시만 해 두고 여기서 보지 않으면
    //  국면-전략 자동선택이 아무것도 막지 않는다(2026-09-08 확인). 비활성 전략이 보유분을 못 팔면 보호가 사라진다.
    if (!active && signal.action == OrderAction::NEW && signal.side == OrderSide::BUY)
    {
        return;
    }

    // 운영단말 수동 매도 정지(D-095) — 전략이 내는 SELL NEW만 여기서 거른다. 손절·트레일·마감 청산도 전략 신호라
    //  같이 멈춘다는 뜻이다. 수동 주문(MANUAL)은 이 함수를 지나지 않고, 국면 강제청산은 force_liquidate()가 따로 낸다.
    //  정지 여부는 사본에서 한 번만 읽는다 — 막는 판단과 로그를 비우는 판단이 다른 판의 값이면
    //  "막아 놓고 곧바로 로그를 비우는" 어긋남이 난다. [why D-114]
    const bool sell_halted = ledger_.globals().manual_sell_halted != 0;

    if (signal.action == OrderAction::NEW && signal.side == OrderSide::SELL && sell_halted)
    {
        if (mark_once(sell_halt_logged_, symbol_of(signal)))
        {
            sell_halt_logged_any_ = true;
            LOG_WARN("[Engine] 수동 매도 정지 — 전략 매도 차단 " + label(signal.ticker) + " (요청 " + signal.strategy_id + ")");
        }

        return;
    }

    if (sell_halt_logged_any_ && !sell_halted)
    {
        std::fill(sell_halt_logged_.begin(), sell_halt_logged_.end(), false);
        sell_halt_logged_any_ = false;
    }

    // 청산 관리가 맡은 티커는 스캔 슬리브가 새로 사지도, 팔지도 않는다. 소유자를 하나로 두지 않으면 청산 관리가
    //  턴 물량을 스캔 전략이 되사는 회전이 나고, 매도가 둘에서 나가면 같은 보유분에 두 장의 매도가 걸린다
    //  (sellable_quantity 클램프가 있어도 순서에 따라 한쪽이 0을 받아 분할 주문을 3초마다 되감는다). 취소·정정은
    //  통과한다 — 이미 낸 주문을 거두는 길까지 막으면 미체결이 미연결 주문이 된다.
    if (signal.action == OrderAction::NEW && !exit_manager && exit_managed_check_ && exit_managed_check_(symbol_of(signal)))
    {
        if (mark_once(guard_logged_, symbol_of(signal)))
        {
            LOG_INFO("[Engine] 청산 관리 보유종목 신규 " + std::string(signal.side == OrderSide::BUY ? "매수" : "매도") +
                     " 차단 " + label(signal.ticker) + " (요청 " + signal.strategy_id + ") — 매매 소유권은 청산 관리에 있다");
        }

        return;
    }

    submit(signal);
}

void SignalDispatcher::submit(OrderSignal signal)
{
    // 여기서 한 번 찍어 두면 게이트·교체 창구·원장이 전부 이 번호로 간다.
    //  교체 진입(최약체 매도 뒤 매수 보류)은 주문 쪽 risk::DisplacementDesk가 한다 — 최약체를 고르는 읽기와
    //  자리를 예약하는 쓰기가 한 덩어리라 전략 쪽에 두면 둘이 같은 종목을 두 번 판다. [why D-114]
    signal.symbol_id = symbol_of(signal);
    emit(std::move(signal));
}

// 강제청산·초과 정리가 보는 보유분 — 바스켓 슬리브 소유 종목은 뺀다. 국면 강제청산은 스캔 슬리브의 당일 포지션을
//  거두는 장치이고, 바스켓은 파일이 DROP을 적을 때만 판다(전량 청산은 파일의 liquidate_all 뿐). [why D-109]
std::vector<OrderGate::HeldPos> SignalDispatcher::scan_sleeve_positions() const
{
    // 보유 전체를 사본 한 판에서 모아 온다 — 줄마다 따로 읽으면 앞 종목과 뒤 종목이 다른 판의 것이 되어
    //  "이미 판 종목이 아직 보유로 잡히는" 조합을 본다. 종목 이름과 계좌는 사본이 같이 들고 온다.
    ipc::collect_all_rows(ledger_, snapshot_ids_, snapshot_rows_);

    const ipc::LedgerGlobals globals = ledger_.globals();
    std::vector<OrderGate::HeldPos> held;
    held.reserve(snapshot_rows_.size());

    for (size_t index = 0; index < snapshot_rows_.size(); ++index)
    {
        const ipc::LedgerRow& row = snapshot_rows_[index];

        // 롱 보유분만 청산 대상이다(사본에는 미체결 선점만 있는 줄도 실린다). 바스켓 슬리브 소유 종목도 뺀다.
        if (row.position <= 0 || row.slot_exempt != 0)
        {
            continue;
        }

        OrderGate::HeldPos held_position;
        held_position.account       = globals.account;
        held_position.symbol        = snapshot_ids_[index];
        held_position.ticker        = gate_.ledger().symbols().name(held_position.symbol).string();
        held_position.quantity      = row.position;
        held_position.average_price = row.average_price;
        held_position.slot_exempt   = false;
        held.push_back(std::move(held_position));
    }

    return held;
}

void SignalDispatcher::force_liquidate(Clock::time_point now)
{
    // entry_halt가 함께 켜져 SELL만 통과한다. 대량은 fat-finger·초당 한도에 일부 막힐 수 있으나 다음 주기에
    //  잔량이 다시 나가 미완료로 남지 않는다(G3-2).
    if (now - last_liquidation_ < liquidation_interval_)
    {
        return;
    }

    last_liquidation_ = now;

    for (auto& liquidation_signal : dispatch::force_liquidation_orders(
             scan_sleeve_positions(),
             [this](const std::string&, symbol::SymbolId symbol)
             {
                 return ledger_.row(symbol).reserved_sell;
             },
             force_liquidation_index_))
    {
        submit(std::move(liquidation_signal));
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

    for (auto& trim_signal : dispatch::trim_orders(
             scan_sleeve_positions(), ledger_.globals().max_notional_per_ticker,
             [this](const std::string&, symbol::SymbolId symbol)
             {
                 return ledger_.row(symbol).reserved_sell;
             },
             limit_trim_index_))
    {
        LOG_WARN("[Engine] 한도 초과분 정리 " + label(trim_signal.ticker) + " 매도 " + std::to_string(trim_signal.quantity) + "주 — " +
                 trim_signal.reason);
        submit(std::move(trim_signal));
    }
}
