#include "strategy/SupplyDemandPullbackStrategy.h"

const std::string& SupplyDemandPullbackStrategy::id() const
{
    static const std::string kId = "SUPPLY_DEMAND_PULLBACK";

    return kId;
}

std::string SupplyDemandPullbackStrategy::describe() const
{
    return id() + " | uni=" + std::to_string(parameters_.universe_size) +
           " | dual>=" + std::to_string(parameters_.min_dual_days) +
           " | band=" + std::to_string(static_cast<int>(parameters_.pullback_band * 100)) + "%" +
           " | qty=" + std::to_string(parameters_.quantity) +
           " | mode=" + (parameters_.mode == EntryMode::DAILY ? "DAILY" : "INTRADAY");
}

std::vector<WatchSpec> SupplyDemandPullbackStrategy::get_watch_specifications() const
{
    std::vector<WatchSpec> specifications;

    if (parameters_.mode != EntryMode::INTRADAY)
    {
        return specifications;
    }

    for (const auto& ticker : candidates_)
    {
        specifications.push_back({ticker, Market::KR, "", /*trade_only=*/true});
    }

    return specifications;
}

void SupplyDemandPullbackStrategy::on_start()
{
    if (!kis_)
    {
        LOG_ERROR("[SDP] KisClient 없음");
        return;
    }

    // past_hhmm은 틱마다 불려 잘못된 값을 조용히 "아직 아님"으로 보므로, 여기서 한 번 소리 내어 잡는다.
    if (!valid_hhmm(parameters_.market_close_exit_hhmm))
    {
        LOG_WARN("[SDP] market_close_exit_hhmm 형식 오류 '" + parameters_.market_close_exit_hhmm +
                 "' — 마감 청산이 걸리지 않는다 (HHMM 네 자리 숫자여야 한다)");
    }

    candidates_.clear();
    closes_.clear();
    reference_ma5_.clear();
    held_.clear();

    const std::string today = today_yyyymmdd();

    // 1. 유니버스: 시총 상위 N
    auto ranked = kis_->fetch_kr_ranking(parameters_.universe_size, parameters_.market_div);
    LOG_INFO("[SDP] 유니버스: " + std::to_string(ranked.size()) + "종목");

    // 2. 종목별 수급 시계열 조회 → 쌍끌이 점수
    for (const auto& stock : ranked)
    {
        const std::string& ticker = stock.ticker;
        auto flows = kis_->get_investor_flow(ticker, parameters_.market_div);
        std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestIntervalMs));

        if (flows.empty())
        {
            continue;
        }

        Score score = calculate_score(ticker, flows, today);
        bool dual_ok = score.dual_days >= parameters_.min_dual_days;
        bool consumed_ok = (parameters_.min_consec_days == 0) || (score.consec_days >= parameters_.min_consec_days);
        bool cumulative_ok = score.cumulative_foreign > parameters_.net_buy_threshold &&
                             score.cumulative_institution > parameters_.net_buy_threshold;

        if (dual_ok && consumed_ok && cumulative_ok)
        {
            candidates_.push_back(ticker);
            LOG_INFO("[SDP] 후보: " + ticker + " " + stock.name + " | 쌍끌이=" + std::to_string(score.dual_days) +
                     "일" + " | 연속=" + std::to_string(score.consec_days) + "일" +
                     " | 외인누적=" + std::to_string(score.cumulative_foreign) +
                     " | 기관누적=" + std::to_string(score.cumulative_institution));
        }
    }

    LOG_INFO("[SDP] 수급 필터 통과: " + std::to_string(candidates_.size()) + "종목");

    // candidates_ 확정 후 O(1) 조회용 set 동기화 (필수 — 누락 시 is_candidate가 항상 false)
    rebuild_set();

    // 3-A. 일봉 모드: 최근 일봉으로 moving_average 초기화
    if (parameters_.mode == EntryMode::DAILY)
    {
        for (const auto& ticker : candidates_)
        {
            auto bars = kis_->get_daily_ohlcv(ticker, parameters_.moving_average_period + 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestIntervalMs));
            auto& closes = closes_[symbol_of(ticker)];

            for (auto iterator = bars.rbegin(); iterator != bars.rend(); ++iterator)
            {
                closes.push_back(iterator->close);
            }

            trim(closes);
        }
    }

    // 3-B. INTRADAY 모드: 전일 확정 일봉으로 reference_ma5_ 고정
    else
    {
        for (const auto& ticker : candidates_)
        {
            auto bars = kis_->get_daily_ohlcv(ticker, parameters_.moving_average_period + 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestIntervalMs));

            if (static_cast<int>(bars.size()) < parameters_.moving_average_period)
            {
                continue;
            }

            // bars[0]=최신(당일 미완성 가능) → bars[1..moving_average_period] 사용
            double sum = 0.0;
            // 현재는 두 가지(당일봉 유무)를 구분하지 않고 항상 1부터 쓴다(보수적).
            // 삼항의 두 분기 값이 같아 실질 무조건 1 — 당일봉 처리 분기 지점만 남겨둔 자리(보류 목록).
            int start = (bars[0].volume == 0) ? 1 : 1;

            if (static_cast<int>(bars.size()) <= start + parameters_.moving_average_period - 1)
            {
                continue;
            }

            for (int moving_average_period_index = start;
                 moving_average_period_index < start + parameters_.moving_average_period; ++moving_average_period_index)
            {
                sum += bars[moving_average_period_index].close;
            }

            reference_ma5_[symbol_of(ticker)] = sum / parameters_.moving_average_period;
        }
    }
}

std::optional<OrderSignal> SupplyDemandPullbackStrategy::on_data(const MarketData& market_data)
{
    if (parameters_.mode != EntryMode::DAILY)
    {
        return std::nullopt;
    }

    const symbol::SymbolId id =
        market_data.symbol_id != symbol::kNone ? market_data.symbol_id : symbol_of(market_data.ticker);

    if (!is_candidate(id))
    {
        return std::nullopt;
    }

    auto& closes = closes_[id];
    double previous_close = closes.empty() ? market_data.close : closes.back();
    closes.push_back(market_data.close);
    trim(closes);

    if (static_cast<int>(closes.size()) < parameters_.moving_average_period)
    {
        return std::nullopt;
    }

    double moving_average = simple_moving_average(closes, parameters_.moving_average_period);
    double price = market_data.close;

    // 보유 중이면 손절만 체크
    if (held_.count(id))
    {
        if (parameters_.stop_below_moving_average > 0.0 &&
            price < moving_average * (1.0 - parameters_.stop_below_moving_average))
        {
            held_.erase(id);
            return make_signal(id, market_data.ticker, OrderSide::SELL, price);
        }

        return std::nullopt;
    }

    // 눌림목 진입 조건
    bool previous_above = !parameters_.require_previous_above || (previous_close >= moving_average);
    bool in_band = price >= moving_average * (1.0 - parameters_.pullback_band) &&
                   price <= moving_average * (1.0 + parameters_.pullback_band);
    bool supported = price >= moving_average;

    if (is_active() && previous_above && in_band && supported) // 진입 — 국면 게이트
    {
        held_.insert(id);
        return make_signal(id, market_data.ticker, OrderSide::BUY, price);
    }

    return std::nullopt;
}

std::optional<OrderSignal> SupplyDemandPullbackStrategy::on_trade(const TradeData& trade)
{
    if (parameters_.mode != EntryMode::INTRADAY)
    {
        return std::nullopt;
    }

    if (trade.market != Market::KR)
    {
        return std::nullopt;
    }

    const symbol::SymbolId id = trade.symbol_id != symbol::kNone ? trade.symbol_id : symbol_of(trade.ticker);

    if (!is_candidate(id))
    {
        return std::nullopt;
    }

    auto reference = reference_ma5_.find(id);

    if (reference == reference_ma5_.end())
    {
        return std::nullopt;
    }

    double moving_average = reference->second;
    double price = trade.price;

    // 청산 시각 도달
    if (past_hhmm(parameters_.market_close_exit_hhmm))
    {
        if (held_.count(id))
        {
            held_.erase(id);
            return make_signal(id, trade.ticker, OrderSide::SELL, price);
        }

        return std::nullopt;
    }

    // 보유 중 손절
    if (held_.count(id))
    {
        if (parameters_.stop_below_moving_average > 0.0 &&
            price < moving_average * (1.0 - parameters_.stop_below_moving_average))
        {
            held_.erase(id);
            return make_signal(id, trade.ticker, OrderSide::SELL, price);
        }

        return std::nullopt;
    }

    // 눌림목 진입
    bool in_band = price >= moving_average * (1.0 - parameters_.pullback_band) &&
                   price <= moving_average * (1.0 + parameters_.pullback_band);
    bool supported = price >= moving_average;

    if (is_active() && in_band && supported) // 진입 — 국면 게이트
    {
        held_.insert(id);
        return make_signal(id, trade.ticker, OrderSide::BUY, price);
    }

    return std::nullopt;
}

void SupplyDemandPullbackStrategy::on_stop()
{
    LOG_INFO("[SDP] 종료: 진입=" + std::to_string(held_.size()) + "종목 보유");
}

SupplyDemandPullbackStrategy::Score SupplyDemandPullbackStrategy::calculate_score(
    const std::string& /*ticker*/, const std::vector<InvestorFlow>& flows, const std::string& today) const
{
    Score score;
    int used = 0;
    bool consec_broken = false;

    for (const auto& flow : flows)
    {
        if (flow.date == today)
        {
            continue; // look-ahead 방지: 당일 제외
        }

        if (used >= parameters_.lookback_days)
        {
            break;
        }

        ++used;

        bool dual = (flow.foreign_net > 0) && (flow.institution_net > 0);
        score.cumulative_foreign += flow.foreign_net;
        score.cumulative_institution += flow.institution_net;

        if (dual)
        {
            score.dual_days++;
        }

        if (!consec_broken)
        {
            if (dual)
            {
                score.consec_days++;
            }
            else
            {
                consec_broken = true;
            }
        }
    }

    return score;
}

void SupplyDemandPullbackStrategy::rebuild_set()
{
    cand_set_.clear();

    for (const auto& ticker : candidates_)
    {
        cand_set_.insert(symbol_of(ticker));
    }
}

double SupplyDemandPullbackStrategy::simple_moving_average(const std::deque<double>& closes, int count)
{
    if (static_cast<int>(closes.size()) < count)
    {
        return 0.0;
    }

    double sum = 0.0;

    for (int close_index = static_cast<int>(closes.size()) - count; close_index < static_cast<int>(closes.size());
         ++close_index)
    {
        sum += closes[close_index];
    }

    return sum / count;
}

void SupplyDemandPullbackStrategy::trim(std::deque<double>& closes) const
{
    while (static_cast<int>(closes.size()) > parameters_.moving_average_period + 2)
    {
        closes.pop_front();
    }
}

OrderSignal SupplyDemandPullbackStrategy::make_signal(symbol::SymbolId sid, std::string_view ticker, OrderSide side,
                                                      double price) const
{
    OrderSignal signal;
    signal.ticker = ticker;
    signal.symbol_id = sid;
    signal.side = side;
    signal.type = OrderType::MARKET;
    signal.quantity = parameters_.quantity;
    signal.price = price;
    signal.market = Market::KR;
    signal.strategy_id = id();
    signal.timestamp = std::chrono::system_clock::now();
    return signal;
}

bool SupplyDemandPullbackStrategy::valid_hhmm(const std::string& hhmm)
{
    if (hhmm.size() != 4)
    {
        return false;
    }

    int hours = 0;
    int minutes = 0;
    auto hours_result = std::from_chars(hhmm.data(), hhmm.data() + 2, hours);
    auto minutes_result = std::from_chars(hhmm.data() + 2, hhmm.data() + 4, minutes);
    return hours_result.ec == std::errc() && minutes_result.ec == std::errc() && hours_result.ptr == hhmm.data() + 2 &&
           minutes_result.ptr == hhmm.data() + 4 && hours < 24 && minutes < 60;
}

bool SupplyDemandPullbackStrategy::past_hhmm(const std::string& hhmm)
{
    if (hhmm.size() < 4)
    {
        return false;
    }

    const auto time_of_day = kst::time_of_day(std::time(nullptr));
    const int now = static_cast<int>(time_of_day.hours().count() * 100 + time_of_day.minutes().count());
    // substr 없이 자리 그대로 읽는다 — 틱마다 불리는 자리라 임시 문자열을 만들지 않는다.
    int hours = 0;
    int minutes = 0;
    auto hours_result = std::from_chars(hhmm.data(), hhmm.data() + 2, hours);
    auto minutes_result = std::from_chars(hhmm.data() + 2, hhmm.data() + 4, minutes);

    if (hours_result.ec != std::errc() || minutes_result.ec != std::errc())
    {
        return false; // 숫자가 아닌 설정값은 "아직 아님"으로 본다
    }

    const int target = hours * 100 + minutes;
    return now >= target;
}
