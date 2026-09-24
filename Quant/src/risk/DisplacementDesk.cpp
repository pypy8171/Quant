#include "risk/DisplacementDesk.h"

#include "utils/Logger.h"

namespace risk
{
namespace
{
// 교체 보류 시한의 기본값 — config의 displace_slot_hold_sec이 0 이하일 때만 쓴다.
//  게이트의 슬롯 예약(note_displacement)도 같은 값으로 잡으므로 둘을 같이 고친다. [inv]
constexpr int kDefaultHoldSeconds = 120;
} // namespace

DisplacementDesk::DisplacementDesk(OrderGate& gate) : gate_(gate), displace_index_(gate.ledger().strategy_index_of("DISPLACE"))
{
}

void DisplacementDesk::set_label(LabelFn label)
{
    label_ = std::move(label);
}

std::string DisplacementDesk::label(const std::string& ticker) const
{
    return label_ ? label_(ticker) : ticker;
}

std::size_t DisplacementDesk::held_count() const noexcept
{
    return held_.size();
}

symbol::SymbolId DisplacementDesk::held_symbol() const noexcept
{
    return held_symbol_;
}

DisplacementDesk::Verdict DisplacementDesk::consider(OrderSignal& signal, Clock::time_point now)
{
    // 교체 진입 — 슬롯이 꽉 찬 상태에서 더 높은 점수의 신규 종목이 오면 최약체를 먼저 비운다. 비우고 끝내는
    //  이유: 매도 체결은 비동기라 같은 틱에 매수를 붙이면 노출이 이중 계상된다. 게이트가 빈 자리를 이 종목에게
    //  예약해 두고, 매수 신호는 여기서 들고 있다가 자리가 나면 낸다. 흘리기만 하면 안 되는 이유: 전략은 자기
    //  예약이 살아 있다고 낙관하므로(계획 시그니처 가드) 게이트 거부를 모르고 다시 내지 않는다. 그러면 예약된
    //  슬롯이 displace_slot_hold_sec 동안 비어 있다가 만료되고, 그동안 다른 종목까지 "예약분" 거부를 받는다
    //  (09-11 10:05 322000: 교체 매도 뒤 매수는 40>=40 거부, 5분간 아무도 못 삼).
    const bool buy_new = signal.side == OrderSide::BUY && signal.action == OrderAction::NEW;

    if (held_symbol_ != symbol::kNone && signal.symbol_id == held_symbol_)
    {
        if (signal.action == OrderAction::CANCEL)
        {
            held_.clear(); // 전략이 분할 매수를 다시 깐다 — 새 분할 단계가 뒤따른다
        }
        else if (buy_new && now < held_until_ && gate_.capacity_full())
        {
            held_.push_back(std::move(signal)); // 아직 자리가 안 났다 — 같은 분할 매수의 다음 분할 단계
            return Verdict::kHold;
        }
    }

    if (!buy_new || !gate_.config().displace_enabled)
    {
        return Verdict::kPass;
    }

    // 보유·선점·여력을 주문 쪽 장부에서 바로 읽는다 — entry_snapshot은 positions_mutex_ 하나로 읽으므로
    //  쓰는 스레드가 여럿이어도 한 시점의 값이다. 자리(슬롯)만 보던 것을 여력으로 보는 것은 예전과 같다: 여력은 "자리 또는 예산"이다.
    const OrderGate::EntrySnapshot entry = gate_.entry_snapshot(signal.account_id, signal.ticker);

    if (entry.position != 0 || entry.reserved != 0 || !gate_.capacity_full())
    {
        return Verdict::kPass;
    }

    const OrderGate::DisplacePlan plan = gate_.plan_displacement(signal.account_id, signal.symbol_id);

    if (!plan.ok)
    {
        return Verdict::kPass;
    }

    OrderSignal sell;
    sell.ticker          = plan.ticker;
    sell.symbol_id       = plan.symbol;
    sell.account_id      = plan.account;
    sell.side            = OrderSide::SELL;
    sell.type            = OrderType::MARKET;
    sell.quantity        = plan.quantity;
    sell.price           = 0.0;
    sell.reference_price = plan.average_price; // 시장가 명목 백스톱이 우회되지 않게 평단을 stamp
    sell.strategy_id     = "DISPLACE";
    sell.strategy_index  = displace_index_;
    sell.reason          = plan.reason;
    LOG_INFO("[Displace] " + label(plan.ticker) + " 전량 매도 " + std::to_string(plan.quantity) + "주 — " + plan.reason);
    gate_.note_displacement(plan, signal.symbol_id);
    held_.clear();
    held_symbol_  = signal.symbol_id;
    drain_logged_ = false;
    const int hold_seconds = gate_.config().displace_slot_hold_sec;
    held_until_ = now + std::chrono::seconds(hold_seconds > 0 ? hold_seconds : kDefaultHoldSeconds);
    held_.push_back(std::move(signal)); // 매도 체결로 자리가 나면 take_ready가 낸다
    signal = std::move(sell);
    return Verdict::kSellFirst;
}

void DisplacementDesk::expire(Clock::time_point now, std::vector<OrderSignal>& expired)
{
    if (held_symbol_ == symbol::kNone || now < held_until_)
    {
        return;
    }

    // 시한이 지나면 버린다 — 그 뒤엔 게이트 예약도 풀려 있어 전략의 다음 재구성이 보통 경로로 들어온다.
    if (!held_.empty())
    {
        LOG_WARN("[Displace] 보류 매수 만료 " + label(gate_.ledger().symbols().name(held_symbol_).string()) + " " +
                 std::to_string(held_.size()) + "건 — 자리가 안 나 버린다");

        for (OrderSignal& held_signal : held_)
        {
            expired.push_back(std::move(held_signal));
        }

        held_.clear();
    }

    held_symbol_  = symbol::kNone;
    drain_logged_ = false;
}

bool DisplacementDesk::take_ready(OrderSignal& out)
{
    if (held_.empty() || gate_.capacity_full())
    {
        return false;
    }

    if (!drain_logged_)
    {
        drain_logged_ = true;
        LOG_INFO("[Displace] 자리가 나 보류 매수 " + std::to_string(held_.size()) + "건 발주 " +
                 label(gate_.ledger().symbols().name(held_symbol_).string()));
    }

    out = std::move(held_.front());
    held_.pop_front();
    return true;
}

} // namespace risk
