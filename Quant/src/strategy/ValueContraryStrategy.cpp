#include "strategy/ValueContraryStrategy.h"

namespace
{
// 미국 정규장(서머타임 기준) KST 22:30~05:00. 표준시 전환은 반영하지 않는다.
constexpr int kUsSessionOpenHhmm  = 2230;
constexpr int kUsSessionCloseHhmm = 500;
constexpr int kHhmmNextDay        = 2400; // 자정 뒤 hhmm에 더해 전날 밤 창 뒤로 놓는다
} // namespace

std::string ValueContraryStrategy::describe() const
{
    return id() + " | PBR<=" + std::to_string(pbr_max_) + " | qty=" + std::to_string(quantity_) +
           " | market_close=" + std::to_string(market_close_exit_hhmm_);
}

void ValueContraryStrategy::on_start()
{
    candidates_.clear();
    buy_sent_.clear();
    sell_sent_.clear();

    if (!kis_)
    {
        LOG_WARN("[ValueContrary] KisClient 없음");
        return;
    }

    LOG_INFO("[ValueContrary] " + id() + " 스크리닝 시작");

    // 1. Universe 조회
    std::vector<std::string> universe;

    if (market_ == Market::KR)
    {
        // KOSPI(J) + KOSDAQ(W) 합산
        auto kospi = kis_->fetch_universe_by_pbr(pbr_max_ > 0 ? pbr_max_ : 999.0, "J");
        auto kosdaq = kis_->fetch_universe_by_pbr(pbr_max_ > 0 ? pbr_max_ : 999.0, "W");
        universe = std::move(kospi);
        universe.insert(universe.end(), std::make_move_iterator(kosdaq.begin()), std::make_move_iterator(kosdaq.end()));
    }
    else
    {
        universe = kis_->fetch_us_universe_by_pbr(pbr_max_, exchange_);
    }

    LOG_INFO("[ValueContrary] Universe 크기: " + std::to_string(universe.size()));

    // 2. 3일 연속 하락 필터 — universe는 이 반복 뒤 버려지므로 후보 티커를 옮긴다
    for (auto& ticker : universe)
    {
        std::vector<MarketData> bars;

        if (market_ == Market::KR)
        {
            bars = kis_->get_daily_ohlcv(ticker, 5);
        }
        else
        {
            bars = kis_->get_us_daily_ohlcv(ticker, 5, exchange_);
        }

        // KIS 초당 거래건수 제한 — 매 호출 후 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(kValueContraryRestIntervalMs));

        // 오늘 미완성 bar 제거 (pre-market or 장 개시 전: volume=0)
        while (!bars.empty() && bars[0].volume == 0)
        {
            bars.erase(bars.begin());
        }

        if (static_cast<int>(bars.size()) < 4)
        {
            continue;
        }

        // bars[0]=최근 완성 거래일, bars[1~3]=그 이전 3일
        bool declining =
            bars[0].close < bars[1].close && bars[1].close < bars[2].close && bars[2].close < bars[3].close;

        if (!declining)
        {
            continue;
        }

        LOG_INFO("[ValueContrary] 후보: " + ticker + "  종가 " + std::to_string(bars[0].close) + " < " +
                 std::to_string(bars[1].close) + " < " + std::to_string(bars[2].close));
        candidates_.insert(std::move(ticker));
    }

    LOG_INFO("[ValueContrary] 후보 확정: " + std::to_string(candidates_.size()) + "종목");

    // 틱 경로는 id로만 본다 — 후보 문자열은 구독 스펙·로그용으로 남긴다.
    pending_.clear();

    for (const auto& ticker : candidates_)
    {
        pending_.insert(symbol_of(ticker));
    }
}

std::vector<WatchSpec> ValueContraryStrategy::get_watch_specifications() const
{
    std::vector<WatchSpec> specifications;

    for (const auto& ticker : candidates_)
    {
        WatchSpec specification;
        specification.ticker = ticker;
        specification.market = market_;
        specification.exchange = exchange_;
        specifications.push_back(std::move(specification));
    }

    return specifications;
}

std::optional<OrderSignal> ValueContraryStrategy::on_order_book(const OrderBook& order_book)
{
    if (market_ != Market::KR)
    {
        return std::nullopt;
    }

    double price = order_book.asks[0].price > 0 ? order_book.asks[0].price : order_book.bids[0].price;
    return check_entry_exit(order_book.symbol_id, order_book.ticker, order_book.hhmmss, price);
}

std::optional<OrderSignal> ValueContraryStrategy::on_trade(const TradeData& trade)
{
    if (trade.market != market_)
    {
        return std::nullopt;
    }

    return check_entry_exit(trade.symbol_id, trade.ticker, trade.hhmmss, trade.price);
}

void ValueContraryStrategy::on_stop()
{
    LOG_INFO("[ValueContrary] " + id() + " 매수=" + std::to_string(buy_sent_.size()) +
             " 청산=" + std::to_string(sell_sent_.size()));
}

std::optional<OrderSignal> ValueContraryStrategy::check_entry_exit(symbol::SymbolId symbol_id, std::string_view ticker,
                                                                   int32_t hhmmss, double reference_price)
{
    int hhmm = hhmmss / 100;

    if (!is_in_session(hhmm))
    {
        return std::nullopt;
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
        signal.market = market_;
        signal.exchange = exchange_;
        signal.strategy_id = id();
        signal.timestamp = std::chrono::system_clock::now();

        LOG_INFO("[ValueContrary] BUY: " + std::string(ticker) + " @" + krx::hhmmss_string(hhmmss));
        return signal;
    }

    // 청산: 매수 보냈고, 청산 안 했고, 청산 시각 도달
    if (buy_sent_.count(symbol_id) && !sell_sent_.count(symbol_id) &&
        session_order(hhmm) >= session_order(market_close_exit_hhmm_))
    {
        sell_sent_.insert(symbol_id);

        OrderSignal signal;
        signal.ticker = ticker;
        signal.symbol_id = symbol_id;
        signal.side = OrderSide::SELL;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity_;
        signal.reference_price = reference_price; // 시장가 명목 백스톱 기준가(현재가/체결가)
        signal.market = market_;
        signal.exchange = exchange_;
        signal.strategy_id = id();
        signal.timestamp = std::chrono::system_clock::now();

        LOG_INFO("[ValueContrary] SELL(장 마감): " + std::string(ticker) + " @" + krx::hhmmss_string(hhmmss));
        return signal;
    }

    return std::nullopt;
}

bool ValueContraryStrategy::is_in_session(int hhmm) const
{
    if (market_ == Market::KR)
    {
        return krx::in_session(hhmm); // 09:00~15:30 정규장(core/MarketSession.h)
    }

    // 미국 정규장(서머타임 기준) KST 22:30~05:00. 표준시 전환은 반영하지 않는다
    return hhmm >= kUsSessionOpenHhmm || hhmm < kUsSessionCloseHhmm;
}

int ValueContraryStrategy::session_order(int hhmm) const
{
    if (market_ == Market::US && hhmm < kUsSessionOpenHhmm)
    {
        return hhmm + kHhmmNextDay;
    }

    return hhmm;
}
