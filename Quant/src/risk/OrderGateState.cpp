// 주문 게이트 — 거부 사유 문장(describe), 하루 리셋, 국면·정지 스위치, 중복 신호 키 해시. [why CODE_REVIEW W-8]
#include "risk/OrderGate.h"

#include "risk/GateReasons.h"
#include "utils/Logger.h"
#include <format>

std::string OrderGate::describe(const GateVerdict& verdict) const
{
    switch (verdict.code)
    {
    case GateReject::None:
        return {};

    case GateReject::KillSwitch:
        return "KILL_SWITCH 활성";

    case GateReject::SideNone:
        return "OrderSide::NONE — 유효하지 않은 주문 방향";

    case GateReject::EntryHalt:
        return "ENTRY_HALT 활성 — 신규 진입 정지(청산은 허용)";

    case GateReject::OutsideSession:
    {
        const int64_t now_min = verdict.amount;
        std::string   text    = std::format("{} ({:02}:{:02}, 허용 {:02}:{:02}~{:02}:{:02}", gate_reason::kOutsideSession,
                                            now_min / 60, now_min % 60,
                                            config_.session_open_min / 60, config_.session_open_min % 60,
                                            config_.session_close_min / 60, config_.session_close_min % 60);

        if (config_.after_close_min > config_.after_open_min)
        {
            text += std::format(" 및 {:02}:{:02}~{:02}:{:02}", config_.after_open_min / 60, config_.after_open_min % 60,
                                config_.after_close_min / 60, config_.after_close_min % 60);
        }

        text += ") — 매매 창에만 주문한다";
        return text;
    }

    case GateReject::BadQuantity:
        return std::format("잘못된 주문 수량 ({})", verdict.amount);

    case GateReject::OrderQuantityLimit:
        return std::format("1주문 수량 한도 초과 ({} > {})", verdict.amount, config_.max_quantity_per_order);

    case GateReject::OrderNotionalLimit:
        return std::format("1주문 명목 한도 초과 ({} > {}{}", verdict.amount, static_cast<long long>(config_.max_notional_per_order),
                           verdict.market_reference ? ", 시장가 참조평가)" : ")");

    case GateReject::TickerQuantityLimit:
        return std::format("포지션 한도 초과 ({}+{} > {})", verdict.base, verdict.amount, config_.max_quantity_per_ticker);

    case GateReject::TickerNotionalLimit:
        return std::format("종목당 명목 한도 초과 ({} > {})", verdict.amount, static_cast<long long>(config_.max_notional_per_ticker));

    case GateReject::ConcurrentLimit:
    {
        std::string text = std::format("동시 보유 종목 한도 초과 ({} >= {}, 실보유 {} 선점만 {})", verdict.amount,
                                       config_.max_concurrent_positions, verdict.base, verdict.amount - verdict.base);

        // 교체 진입이 켜져 있으면 여기 오는 BUY는 교체 판정에서 떨어진 것이다. 그 사유를
        //  같이 적지 않으면 한도 문구만 남아 "교체가 안 도는 것"으로 읽힌다(09-11 11:21).
        if (!entry_priority_.append_decline(verdict.symbol, text))
        {
            text += " — 신규 종목 진입 정지";
        }

        return text;
    }

    case GateReject::DisplaceCooling:
        return "교체 쿨다운 중 — 방금 슬롯을 내준 종목의 재진입 금지";

    case GateReject::SlotReserved:
        return std::format("교체로 비운 슬롯 예약분 ({}) — 다른 종목 진입 보류", ledger_.symbols().name(verdict.symbol).view());

    case GateReject::PriorityBar:
        return std::format("점수 우선순위 미달 (랭크 {} 유효 {}/{} = {:.2f} > 기준 {:.2f}, 슬롯 {}/{}) — 더 높은 점수 종목을 위해 보류",
                           verdict.rank, verdict.base, verdict.ceiling, verdict.ratio, verdict.bar, verdict.amount,
                           config_.max_concurrent_positions);

    case GateReject::GrossExposure:
        return std::format("총노출 한도 초과 ({} > {} = 자본 {}×{:g}) — 신규 매수 정지(청산 허용)", verdict.amount, verdict.ceiling,
                           static_cast<long long>(verdict.money), config_.max_gross_exposure_percent);

    case GateReject::DailyLoss:
        return std::format("일일 손실 한도 초과 (현재 {:.0f}원 / 한도 {:.0f}원)", verdict.money, config_.daily_loss_limit);

    case GateReject::PnlStale:
        return "PNL_STALE — 잔고 대조 정체(daily_pnl 미갱신), 신규 진입 보수적 정지";

    case GateReject::Duplicate:
        return std::format("중복 신호 (윈도우 {}초)", config_.deduplicate_window_sec);

    case GateReject::RatePerSecond:
        return gate_reason::rate_limit(false, config_.max_orders_per_sec);

    case GateReject::RatePerMinute:
        return gate_reason::rate_limit(true, config_.max_orders_per_min);
    }

    return "알 수 없는 게이트 거부";
}

// ─── 일별 리셋 (거래일 첫 회차) ─────────────────────────────────────────────────
bool OrderGate::reset_daily(uint32_t trading_date_yyyymmdd)
{
    // 하루 한 번이다. 데이터 스레드는 기동 직후 첫 회차마다 부르므로 장중 재기동 때도
    //  불린다. 그때 다시 비우면 리플레이·미체결 대조로 되살린 선점과 당일 손익이 0이 된다.
    //  그날 리셋을 했는지는 저널의 RESET_DAY가 안다. [why A-4]
    if (ledger_.last_daily_reset_date() == trading_date_yyyymmdd)
    {
        LOG_INFO(std::format("[OrderGate] 하루 리셋 건너뜀 - 거래일({}) 리셋은 이미 했다", trading_date_yyyymmdd));
        return false;
    }

    ledger_.set_daily_pnl(0.0); // 저널에도 DAILY_PNL 0 — 리셋 전 체결이 리플레이로 되살아나지 않게

    {
        std::lock_guard<std::mutex> lock(rate_mutex_);
        order_times_min_.clear();
        order_times_sec_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        last_signal_.clear();
    }

    entry_priority_.reset_daily();

    ledger_.expire_reservations(trading_date_yyyymmdd); // 미체결 선점 일일 만료 — 사유는 PositionLedger::expire_reservations

    // average_prices_ / positions_ 는 영속 장부 — 장 시작에 초기화하지 않는다
    LOG_INFO(std::format("[OrderGate] 하루 리셋 - 거래일({}) 선점·당일 손익·주문 한도를 새로 열었다", trading_date_yyyymmdd));
    return true;
}

void OrderGate::set_kill_switch(bool on)
{
    kill_switch_.store(on);
}

void OrderGate::set_entry_halt(bool on)
{
    entry_halt_.store(on);
}

void OrderGate::set_strategy_down_halt(bool on)
{
    strategy_down_halt_.store(on);
}

void OrderGate::set_feed_down_halt(bool on)
{
    feed_down_halt_.store(on);
}

void OrderGate::set_manual_halt(OrderSide side, bool on)
{
    (side == OrderSide::SELL ? manual_sell_halt_ : manual_buy_halt_).store(on);
}

void OrderGate::set_entry_scale(double entry_scale)
{
    entry_scale_.store(entry_scale);
}

void OrderGate::set_pnl_stale(bool on)
{
    pnl_stale_.store(on);
}

size_t OrderGate::SignalKeyHash::operator()(const SignalKey& key) const noexcept
{
    uint64_t mixed = (static_cast<uint64_t>(key.account) << 32) | key.symbol;
    mixed =
        (mixed ^ (static_cast<uint64_t>(key.strategy) << 8) ^ static_cast<uint64_t>(key.side)) * 0x9e3779b97f4a7c15ull;
    mixed ^= static_cast<uint64_t>(key.price) * 0xbf58476d1ce4e5b9ull;
    return static_cast<size_t>(mixed ^ (mixed >> 31));
}
