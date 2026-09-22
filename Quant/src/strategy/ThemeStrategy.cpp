#include "strategy/ThemeStrategy.h"

const std::string& ThemeStrategy::id() const
{
    static const std::string kId = "THEME_KR";

    return kId;
}

std::string ThemeStrategy::describe() const
{
    return "THEME_KR | top_sectors=" + std::to_string(top_n_sectors_) +
           " | vol_surge=" + std::to_string(static_cast<int>(volume_surge_mult_)) + "x" +
           " | inst=" + (institution_filter_ ? "Y" : "N") + " | qty=" + std::to_string(quantity_) +
           " | market_close=" + std::to_string(market_close_exit_hhmm_);
}

std::vector<WatchSpec> ThemeStrategy::get_watch_specifications() const
{
    std::vector<WatchSpec> specifications;

    for (const auto& ticker : candidates_)
    {
        specifications.push_back({ticker, Market::KR, ""});
    }

    return specifications;
}

void ThemeStrategy::on_start()
{
    candidates_.clear();
    buy_sent_.clear();
    sell_sent_.clear();

    if (!kis_)
    {
        LOG_WARN("[ThemeStrategy] KisClient 없음");
        return;
    }

    // 스캔 대상 업종 결정 — 기본 표(KOSPI_SECTORS)는 복사하지 않고 참조로 본다.
    std::vector<std::pair<std::string, std::string>> configured_sectors;

    for (const auto& code : sector_codes_)
    {
        configured_sectors.push_back({code, code});
    }

    const std::vector<std::pair<std::string, std::string>>& sectors =
        sector_codes_.empty() ? KOSPI_SECTORS : configured_sectors;

    // ── Step 1: 업종 5일 모멘텀 계산 ─────────────────────────────────
    LOG_INFO("[ThemeStrategy] Step1 — 업종 모멘텀 계산 (" + std::to_string(sectors.size()) + "개 업종)");

    std::vector<std::pair<double, std::string>> momentum_rank; // <수익률, 코드>

    for (const auto& [code, name] : sectors)
    {
        auto bars = kis_->get_index_daily_ohlcv(code, 6);
        std::this_thread::sleep_for(std::chrono::milliseconds(kThemeIndexIntervalMs));

        if (bars.size() < 2 || bars[0].close <= 0 || bars.back().close <= 0)
        {
            LOG_WARN("[ThemeStrategy] " + name + "(" + code + ") 일봉 부족");
            continue;
        }

        // 최신이 bars[0], 과거가 bars[N-1]
        double ret = (bars[0].close - bars.back().close) / bars.back().close * 100.0;
        LOG_INFO("[ThemeStrategy] " + name + "(" + code + ") 5일 수익률: " + std::to_string(ret).substr(0, 6) + "%");
        momentum_rank.push_back({ret, code});
    }

    // 수익률 내림차순 정렬 → 상위 N개 선택
    std::ranges::sort(momentum_rank, std::ranges::greater{}, [](const auto& element) { return element.first; });

    int count = std::min(top_n_sectors_, static_cast<int>(momentum_rank.size()));
    LOG_INFO("[ThemeStrategy] Step1 완료 — 상위 " + std::to_string(count) + "개 업종 선택:");

    for (int index = 0; index < count; ++index)
    {
        LOG_INFO("  [" + std::to_string(index + 1) + "] " + momentum_rank[index].second + " (" +
                 std::to_string(momentum_rank[index].first).substr(0, 6) + "%)");
    }

    // ── Step 2: 업종 내 종목 스캔 + 거래량 급증 필터 ─────────────────
    LOG_INFO("[ThemeStrategy] Step2 — 거래량 급증 종목 스캔");

    std::vector<std::string> surge_candidates;

    for (int index = 0; index < count; ++index)
    {
        const std::string& sector_code = momentum_rank[index].second;
        auto ranked = kis_->fetch_sector_ranking(sector_code, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestIntervalMs));

        for (auto& stock : ranked) // ranked는 이 반복 뒤 버려지므로 티커를 옮긴다
        {
            if (surge_candidates.size() >= kThemeMaxSurgeCandidates)
            {
                break; // 안전 상한
            }

            auto bars = kis_->get_daily_ohlcv(stock.ticker, 21);
            std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestIntervalMs));

            if (bars.size() < 5)
            {
                continue;
            }

            // 20일 평균 거래량
            double volume_sum = 0;
            int volume_count = std::min(20, static_cast<int>(bars.size()) - 1);

            for (int volume_index = 1; volume_index <= volume_count; ++volume_index)
            {
                volume_sum += static_cast<double>(bars[volume_index].volume);
            }

            double average_volume = volume_sum / volume_count;

            if (average_volume <= 0)
            {
                continue;
            }

            double surge = static_cast<double>(bars[0].volume) / average_volume;

            if (surge >= volume_surge_mult_)
            {
                LOG_INFO("[ThemeStrategy] 거래량 급증: " + stock.ticker + " " + stock.name +
                         " (배수: " + std::to_string(surge).substr(0, 4) + "x)");
                surge_candidates.push_back(std::move(stock.ticker));
            }
        }
    }

    LOG_INFO("[ThemeStrategy] Step2 완료 — 거래량 급증 " + std::to_string(surge_candidates.size()) + "종목");

    // ── Step 3: 외국인+기관 동시 순매수 필터 ─────────────────────────
    if (!institution_filter_)
    {
        // 필터 미적용 시 surge_candidates 바로 사용
        for (auto& ticker : surge_candidates)
        {
            candidates_.insert(std::move(ticker));
        }
    }
    else
    {
        LOG_INFO("[ThemeStrategy] Step3 — 수급 필터 (외국인+기관 동시 순매수)");

        for (auto& ticker : surge_candidates)
        {
            auto trend = kis_->get_investor_trend(ticker);
            std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestIntervalMs));

            if (trend.foreign_net > 0 && trend.institution_net > 0)
            {
                LOG_INFO("[ThemeStrategy] 수급 통과: " + ticker + " 외국인=" + std::to_string(trend.foreign_net) +
                         " 기관=" + std::to_string(trend.institution_net));
                candidates_.insert(std::move(ticker));
            }
        }
    }

    LOG_INFO("[ThemeStrategy] 최종 후보: " + std::to_string(candidates_.size()) + "종목");

    // 틱 경로는 id로만 본다 — 후보 문자열은 구독 스펙·로그용으로 남긴다.
    pending_.clear();

    for (const auto& ticker : candidates_)
    {
        pending_.insert(symbol_of(ticker));
    }

    for (const auto& ticker : candidates_)
    {
        LOG_INFO("  → " + ticker);
    }
}

std::optional<OrderSignal> ThemeStrategy::on_order_book(const OrderBook& order_book)
{
    double price = order_book.asks[0].price > 0 ? order_book.asks[0].price : order_book.bids[0].price;
    return check_entry_exit(order_book.symbol_id, order_book.ticker, order_book.hhmmss, price);
}

std::optional<OrderSignal> ThemeStrategy::on_trade(const TradeData& trade)
{
    if (trade.market != Market::KR)
    {
        return std::nullopt;
    }

    return check_entry_exit(trade.symbol_id, trade.ticker, trade.hhmmss, trade.price);
}

void ThemeStrategy::on_stop()
{
    LOG_INFO("[ThemeStrategy] 종료: 매수=" + std::to_string(buy_sent_.size()) +
             " 청산=" + std::to_string(sell_sent_.size()));
}

std::optional<OrderSignal> ThemeStrategy::check_entry_exit(symbol::SymbolId symbol_id, std::string_view ticker,
                                                           int32_t hhmmss, double reference_price)
{
    int hhmm = hhmmss / 100;

    if (!krx::in_session(hhmm))
    {
        return std::nullopt; // 09:00~15:30 정규장만(core/MarketSession.h)
    }

    if (symbol_id == symbol::kNone)
    {
        symbol_id = symbol_of(ticker); // id 없이 온 틱(옛 경로) — 찍힌 게 정상이라 여기는 드물다
    }

    // 진입: 후보이고 아직 매수 안 했으면 (국면 게이트 적용)
    if (is_active() && pending_.count(symbol_id) && !buy_sent_.count(symbol_id))
    {
        buy_sent_.insert(symbol_id);
        pending_.erase(symbol_id);

        if (auto found = candidates_.find(ticker); found != candidates_.end())
        {
            candidates_.erase(found);
        }

        OrderSignal signal;
        signal.ticker = ticker;
        signal.symbol_id = symbol_id;
        signal.side = OrderSide::BUY;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity_;
        signal.reference_price = reference_price; // 시장가 명목 백스톱 기준가(현재가/체결가)
        signal.market = Market::KR;
        signal.strategy_id = id();
        signal.timestamp = std::chrono::system_clock::now();

        LOG_INFO("[ThemeStrategy] BUY: " + std::string(ticker) + " @" + krx::hhmmss_string(hhmmss));
        return signal;
    }

    // 청산: 매수했고, 아직 청산 안 했고, 청산 시각 도달
    if (buy_sent_.count(symbol_id) && !sell_sent_.count(symbol_id) && hhmm >= market_close_exit_hhmm_)
    {
        sell_sent_.insert(symbol_id);

        OrderSignal signal;
        signal.ticker = ticker;
        signal.symbol_id = symbol_id;
        signal.side = OrderSide::SELL;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity_;
        signal.reference_price = reference_price; // 시장가 명목 백스톱 기준가(현재가/체결가)
        signal.market = Market::KR;
        signal.strategy_id = id();
        signal.timestamp = std::chrono::system_clock::now();

        LOG_INFO("[ThemeStrategy] SELL(장 마감): " + std::string(ticker) + " @" + krx::hhmmss_string(hhmmss));
        return signal;
    }

    return std::nullopt;
}
