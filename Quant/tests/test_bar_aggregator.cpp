// N분봉 집계기(core/BarAggregator.h) 단위 테스트. 버킷 정렬(REST aggregate_minutes와 같은 식)·자정 단조·빈 구간
//  건너뜀·장 밖 틱 폐기·누적 거래량 차와 quantity 합산 뒷걸음·시드 병합(닫힌 봉 REST 우선, 진행 봉 합침, 빈 자리
//  채움)·keep 상한·bar_index 0=최신·닫힘 콜백, 그리고 1분 기저를 N분으로 접는 resample이 직접 집계와 같음을
//  고정한다. KIS·Engine·Logger 없이 링크한다.
//  관련 결정: D-068·D-072.
// 빌드: cmake --build <directory> --target test_bar_aggregator
#include "core/BarAggregator.h"
#include "core/KstTime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 2026-09-14 09:00:00 KST = 00:00:00 UTC. 틱 수신 시각은 거래소 시각과 같은 초로 둔다.
constexpr std::time_t kDay0900Utc = 1789344000;

std::time_t utc_of(const std::string& hhmmss)
{
    const int hour = std::stoi(hhmmss.substr(0, 2));
    const int minute = std::stoi(hhmmss.substr(2, 2));
    const int second = std::stoi(hhmmss.substr(4, 2));
    return kDay0900Utc + (hour - 9) * 3600 + minute * 60 + second;
}

// 시험용 종목 id — 집계기는 id로만 찾고 문자열은 닫힌 봉에 싣는다.
constexpr symbol::SymbolId kSamsung = 1, kHynix = 2, kNaver = 3, kUnknown = 99;

symbol::SymbolId symbol_id_of(const char* ticker)
{
    return std::string_view(ticker) == "005930" ? kSamsung : std::string_view(ticker) == "000660" ? kHynix : kNaver;
}

TradeData tick(const std::string& hhmmss, double price, int64_t quantity, int64_t accumulated_volume, const char* ticker = "005930")
{
    TradeData trade;
    trade.ticker      = ticker;
    trade.symbol_id         = symbol_id_of(ticker);
    trade.hhmmss      = std::stoi(hhmmss);
    trade.price       = price;
    trade.quantity    = quantity;
    trade.accumulated_volume = accumulated_volume;
    trade.market      = Market::KR;
    trade.timestamp   = std::chrono::system_clock::from_time_t(utc_of(hhmmss));
    return trade;
}

// REST 봉 흉내 — timestamp는 버킷의 마지막 1분 시각(aggregate_minutes와 같다).
MarketData rest_bar(const std::string& last_min_hhmmss, double open, double high, double low, double close, int64_t value)
{
    MarketData market_data;
    market_data.ticker    = "005930";
    market_data.symbol_id       = kSamsung;
    market_data.market    = Market::KR;
    market_data.open      = open;
    market_data.high      = high;
    market_data.low       = low;
    market_data.close     = close;
    market_data.volume    = value;
    market_data.timestamp = std::chrono::system_clock::from_time_t(utc_of(last_min_hhmmss));
    return market_data;
}
} // namespace

int main()
{
    using bars::BarAggregator;
    using bars::BarSlot;

    // ── slot_of: 시계 정렬, 장 밖, 시각 0(모름) ────────────────────────────────
    {
        const BarSlot slot_a = bars::slot_of(90000, utc_of("090000"), 3, 900, 1530);
        const BarSlot slot_b = bars::slot_of(90259, utc_of("090259"), 3, 900, 1530);
        const BarSlot slot_three = bars::slot_of(90300, utc_of("090300"), 3, 900, 1530);
        CHECK(slot_a.valid() && slot_a == slot_b && slot_b != slot_three && slot_a < slot_three);
        CHECK(slot_a.bucket == (9 * 60) / 3 && slot_three.bucket == slot_a.bucket + 1);
        CHECK(!bars::slot_of(85959, utc_of("085959"), 3, 900, 1530).valid());   // 동시호가 전
        CHECK(bars::slot_of(153000, utc_of("153000"), 3, 900, 1530).valid());    // 마감 동시호가 체결
        CHECK(!bars::slot_of(153100, utc_of("153100"), 3, 900, 1530).valid());   // 장 뒤
        CHECK(!bars::slot_of(154000, utc_of("154000"), 3, 900, 1530).valid());   // 시간외
        CHECK(!bars::slot_of(990000, utc_of("090000"), 3, 900, 1530).valid());   // 깨진 시각
        CHECK(!bars::slot_of(90000, utc_of("090000"), 0, 900, 1530).valid());   // 간격 0
        // 시각이 0(모름)이면 수신 시각의 KST 분을 쓴다 — REST 대체 틱.
        const BarSlot resolved_slot = bars::slot_of(0, utc_of("100130"), 3, 900, 1530);
        CHECK(resolved_slot.valid() && resolved_slot.bucket == (10 * 60 + 1) / 3);
        // 자정 단조: 다음날 09:00 자리는 오늘 15:30 자리보다 크다.
        const BarSlot today_last = bars::slot_of(153000, utc_of("153000"), 3, 900, 1530);
        const BarSlot tomorrow   = bars::slot_of(90000, utc_of("090000") + 86400, 3, 900, 1530);
        CHECK(today_last < tomorrow && tomorrow.day == today_last.day + 1);
        // 봉 시작 시각은 버킷의 첫 분.
        const auto stop_token = std::chrono::system_clock::to_time_t(bars::slot_start(slot_b, utc_of("090259"), 3));
        CHECK(stop_token == utc_of("090000"));
    }

    // ── 한 봉 안 OHLC·누적 거래량 차, 다음 버킷 첫 틱이 앞 봉을 닫음, bar_index 0=최신 ──
    {
        BarAggregator::Config config;
        BarAggregator aggregator(config);
        std::vector<MarketData> closed;
        aggregator.set_sink([&](const MarketData& market_data) { closed.push_back(market_data); });

        CHECK(aggregator.on_tick(tick("090000", 100.0, 10, 10)));
        CHECK(aggregator.on_tick(tick("090030", 103.0, 5, 15)));
        CHECK(aggregator.on_tick(tick("090159", 98.0, 7, 22)));
        CHECK(aggregator.on_tick(tick("090259", 101.0, 3, 25)));
        CHECK(closed.empty() && aggregator.closed_count(kSamsung) == 0);

        auto snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 1);
        CHECK(snapshot[0].open == 100.0 && snapshot[0].high == 103.0 && snapshot[0].low == 98.0 && snapshot[0].close == 101.0);
        CHECK(snapshot[0].volume == 25);
        CHECK(snapshot[0].bar_index == 0 && snapshot[0].ticker == "005930");

        // 09:03:00 첫 틱이 09:00 봉을 닫는다. 사이에 틱을 흘렸어도(accumulated_volume 25→40) 거래량은 누적차라 맞다.
        CHECK(aggregator.on_tick(tick("090300", 102.0, 4, 44)));
        CHECK(closed.size() == 1 && closed[0].close == 101.0 && closed[0].volume == 25);
        CHECK(aggregator.closed_count(kSamsung) == 1);
        CHECK(aggregator.on_tick(tick("090400", 104.0, 6, 50)));
        snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 2 && snapshot[0].bar_index == 0 && snapshot[1].bar_index == 1);
        CHECK(snapshot[0].open == 102.0 && snapshot[0].close == 104.0 && snapshot[0].volume == 10); // 44−40 + 6
        CHECK(snapshot[1].close == 101.0);

        // 늦게 온 과거 틱은 버린다 — 닫힌 봉을 고치지 않는다.
        CHECK(!aggregator.on_tick(tick("090250", 90.0, 1, 51)));
        CHECK(aggregator.snapshot(kSamsung)[1].close == 101.0);

        // 빈 구간(09:06~09:08 틱 없음) 뒤 09:09 틱 — 봉을 만들지 않고 건너뛴다.
        CHECK(aggregator.on_tick(tick("090900", 105.0, 1, 60)));
        CHECK(aggregator.closed_count(kSamsung) == 2);
        const BarSlot current = aggregator.current_slot(kSamsung);
        CHECK(current.bucket == (9 * 60 + 9) / 3);
        snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 3 && snapshot[1].open == 102.0); // [1]=09:03 봉, 09:06 봉은 없다

        // max_count
        CHECK(aggregator.snapshot(kSamsung, 2).size() == 2);
        CHECK(aggregator.snapshot(kUnknown).empty());
    }

    // ── 장 밖·가격 0·다른 종목은 섞이지 않음, 종목별 독립 ────────────────────────
    {
        BarAggregator aggregator(BarAggregator::Config{});
        CHECK(!aggregator.on_tick(tick("085000", 100.0, 1, 1)));
        CHECK(!aggregator.on_tick(tick("200100", 100.0, 1, 1))); // 애프터마켓 마감 20:00 뒤 [why D-097]
        CHECK(!aggregator.on_tick(tick("090000", 0.0, 1, 1)));
        CHECK(aggregator.on_tick(tick("090000", 100.0, 1, 1)));
        CHECK(aggregator.on_tick(tick("090000", 50.0, 1, 1, "000660")));
        CHECK(aggregator.snapshot(kSamsung)[0].close == 100.0 && aggregator.snapshot(kHynix)[0].close == 50.0);
        aggregator.clear(kSamsung);
        CHECK(aggregator.snapshot(kSamsung).empty() && aggregator.snapshot(kHynix).size() == 1);
    }

    // ── accumulated_volume 없는 틱(REST 대체·선물)은 quantity 합산으로 뒷걸음 ────────────────────────
    {
        BarAggregator aggregator(BarAggregator::Config{});
        CHECK(aggregator.on_tick(tick("100000", 10.0, 3, 0)));
        CHECK(aggregator.on_tick(tick("100100", 11.0, 4, 0)));
        CHECK(aggregator.snapshot(kSamsung)[0].volume == 7);
        // 중간에 acml이 붙어도 기준값이 없으면 계속 합산한다.
        CHECK(aggregator.on_tick(tick("100200", 12.0, 5, 999)));
        CHECK(aggregator.snapshot(kSamsung)[0].volume == 12);
    }

    // ── 시드: 빈 상태에 REST 봉 넣기, 닫힌 봉은 REST가 이김, 진행 봉은 합침 ──────────
    {
        BarAggregator aggregator(BarAggregator::Config{});
        std::vector<MarketData> closed;
        aggregator.set_sink([&](const MarketData& market_data) { closed.push_back(market_data); });

        // 로컬은 09:06 봉을 09:07부터만 봤다(구독이 늦었다).
        CHECK(aggregator.on_tick(tick("090700", 200.0, 10, 500)));
        CHECK(aggregator.on_tick(tick("090800", 205.0, 10, 510)));
        CHECK(aggregator.snapshot(kSamsung)[0].open == 200.0 && aggregator.snapshot(kSamsung)[0].volume == 20);

        // REST가 09:00·09:03(닫힘)·09:06(진행 중, 시가 198·거래량 480) 세 봉을 돌려줬다.
        std::vector<MarketData> rest = {
            rest_bar("090800", 198.0, 206.0, 197.0, 204.0, 480), // [0]=최신(진행 중)
            rest_bar("090500", 190.0, 199.0, 189.0, 198.0, 300),
            rest_bar("090200", 185.0, 191.0, 184.0, 190.0, 250),
        };
        CHECK(aggregator.seed(kSamsung, rest) == 2);
        CHECK(aggregator.closed_count(kSamsung) == 2);
        auto snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 3);
        CHECK(snapshot[0].open == 198.0 && snapshot[0].high == 206.0 && snapshot[0].low == 197.0);
        CHECK(snapshot[0].close == 205.0);   // 종가는 로컬(더 늦다)
        CHECK(snapshot[0].volume == 480);    // 거래량은 큰 쪽
        CHECK(snapshot[1].close == 198.0 && snapshot[2].close == 190.0);
        CHECK(snapshot[1].ticker == "005930" && snapshot[1].bar_index == 1 && snapshot[2].bar_index == 2);

        // 합친 뒤 틱이 더 오면 거래량은 합친 값 위에 쌓인다(누적차 기준 이동).
        CHECK(aggregator.on_tick(tick("090830", 207.0, 5, 515)));
        CHECK(aggregator.snapshot(kSamsung)[0].volume == 485 && aggregator.snapshot(kSamsung)[0].high == 207.0);

        // 다시 시드하면 닫힌 봉은 REST 값으로 덮인다(로컬 09:03 봉이 틀렸다고 치자). 새로 든 봉은 0.
        std::vector<MarketData> again = {rest_bar("090500", 191.0, 199.5, 189.0, 199.0, 310)};
        CHECK(aggregator.seed(kSamsung, again) == 0);
        CHECK(aggregator.snapshot(kSamsung)[1].close == 199.0 && aggregator.snapshot(kSamsung)[1].volume == 310);

        // 시드가 진행 봉보다 새 자리를 갖고 있으면 진행 봉을 닫고 그 뒤에 붙인다. 그 자리의 틱은 REST 봉 위에 이어 쓴다.
        std::vector<MarketData> ahead = {rest_bar("091100", 210.0, 212.0, 208.0, 211.0, 100)};
        CHECK(aggregator.seed(kSamsung, ahead) == 1);
        CHECK(closed.size() == 1 && closed[0].close == 207.0);
        CHECK(aggregator.closed_count(kSamsung) == 4 && !aggregator.current_slot(kSamsung).valid());
        CHECK(aggregator.on_tick(tick("091130", 213.0, 2, 700)));
        snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 4 && snapshot[0].open == 210.0 && snapshot[0].high == 213.0 && snapshot[0].close == 213.0);
        CHECK(snapshot[0].volume == 102);
        CHECK(aggregator.on_tick(tick("091140", 214.0, 3, 703)));
        CHECK(aggregator.snapshot(kSamsung)[0].volume == 105);
        // 시드가 닫아 둔 자리보다 오래된 틱은 버린다.
        CHECK(!aggregator.on_tick(tick("090600", 1.0, 1, 704)));

        // timestamp 없는 REST 봉·빈 벡터·빈 종목은 무시.
        MarketData nots;
        nots.close = 1.0;
        CHECK(aggregator.seed(kSamsung, {nots}) == 0);
        CHECK(aggregator.seed(kSamsung, {}) == 0);
        CHECK(aggregator.seed(symbol::kNone, rest) == 0);
    }

    // ── keep 상한: 오래된 봉부터 버림, 슬롯 순서 유지 ────────────────────────────
    {
        BarAggregator::Config config;
        config.keep = 3;
        BarAggregator aggregator(config);
        int n_closed = 0;
        aggregator.set_sink([&](const MarketData&) { ++n_closed; });

        for (int index = 0; index < 6; ++index)
        {
            char buffer[7];
            std::snprintf(buffer, sizeof(buffer), "%02d%02d00", 9, index * 3);
            CHECK(aggregator.on_tick(tick(buffer, 100.0 + index, 1, index + 1)));
        }

        CHECK(n_closed == 5 && aggregator.closed_count(kSamsung) == 3);
        auto snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot.size() == 4);
        CHECK(snapshot[0].close == 105.0 && snapshot[3].close == 102.0); // [0]=진행 중(09:15), 09:00·09:03은 버려짐
        // 시드로 더 넣어도 상한을 넘기지 않는다.
        std::vector<MarketData> old = {rest_bar("090200", 1, 1, 1, 1, 1)};
        aggregator.seed(kSamsung, old);
        CHECK(aggregator.closed_count(kSamsung) == 3 && aggregator.snapshot(kSamsung)[3].close == 102.0);
    }

    // ── 자정을 넘긴 두 날은 같은 버킷 번호라도 다른 봉 ──────────────────────────
    {
        BarAggregator aggregator(BarAggregator::Config{});
        TradeData trade_a = tick("090000", 10.0, 1, 1);
        TradeData trade_b = tick("090000", 20.0, 1, 1);
        trade_b.timestamp += std::chrono::hours(24);
        CHECK(aggregator.on_tick(trade_a));
        CHECK(aggregator.on_tick(trade_b));
        CHECK(aggregator.closed_count(kSamsung) == 1);
        auto snapshot = aggregator.snapshot(kSamsung);
        CHECK(snapshot[0].close == 20.0 && snapshot[1].close == 10.0);
    }

    // ── resample: 1분 기저 → 3분. 틱을 바로 3분에 넣은 집계기와 봉이 같아야 한다 (D-072) ──
    {
        BarAggregator::Config c1;
        c1.interval_min = 1;
        BarAggregator::Config c3;
        c3.interval_min = 3;
        BarAggregator one(c1), three(c3);
        // 09:00~09:07 사이 매 분 두 틱. 09:04는 비워 두어 빈 분이 버킷 안에 있어도 접힌다.
        const char* mins[] = {"0900", "0901", "0902", "0903", "0905", "0906", "0907"};
        int         accumulated_volume   = 0;

        for (int index = 0; index < 7; ++index)
        {
            const std::string message  = mins[index];
            const double      price = 100.0 + index;
            accumulated_volume += 5;
            CHECK(one.on_tick(tick(message + "10", price + 0.5, 5, accumulated_volume)));
            CHECK(three.on_tick(tick(message + "10", price + 0.5, 5, accumulated_volume)));
            accumulated_volume += 7;
            CHECK(one.on_tick(tick(message + "40", price, 7, accumulated_volume)));
            CHECK(three.on_tick(tick(message + "40", price, 7, accumulated_volume)));
        }

        const auto snapshot_one = one.snapshot(kSamsung);
        CHECK(snapshot_one.size() == 7); // 진행 중 09:07 + 닫힌 6
        const auto result_c = bars::resample(snapshot_one, 3);
        const auto snapshot_three = three.snapshot(kSamsung);
        CHECK(result_c.size() == 3 && snapshot_three.size() == 3); // 09:06(진행 중)·09:03·09:00

        for (size_t index = 0; index < 3; ++index)
        {
            CHECK(result_c[index].open == snapshot_three[index].open && result_c[index].high == snapshot_three[index].high && result_c[index].low == snapshot_three[index].low &&
                  result_c[index].close == snapshot_three[index].close && result_c[index].volume == snapshot_three[index].volume);
            CHECK(result_c[index].bar_index == static_cast<int>(index));
        }

        CHECK(result_c[2].open == 100.5 && result_c[2].close == 102.0 && result_c[2].high == 102.5 && result_c[2].low == 100.0);
        CHECK(result_c[1].volume == 24); // 09:03·09:05 두 분(09:04 없음) × 12
        // timestamp는 버킷 마지막 분(aggregate_minutes와 같다).
        CHECK(std::chrono::system_clock::to_time_t(result_c[1].timestamp) == utc_of("090500"));
        // max_count는 최신부터 자른다. interval 1은 복사·bar_index 재부여.
        CHECK(bars::resample(snapshot_one, 3, 2).size() == 2 && bars::resample(snapshot_one, 3, 2)[1].close == result_c[1].close);
        const auto same = bars::resample(snapshot_one, 1, 4);
        CHECK(same.size() == 4 && same[3].close == snapshot_one[3].close && same[3].bar_index == 3);
        CHECK(bars::resample({}, 3).empty());
        // REST 1분봉(진짜 UTC timestamp)을 접어도 같은 자리에 떨어진다.
        std::vector<MarketData> rest = {rest_bar("090100", 1, 2, 1, 2, 1), rest_bar("090000", 1, 1, 1, 1, 1),
                                        rest_bar("085900", 9, 9, 9, 9, 1)};
        const auto run_result = bars::resample(rest, 3);
        CHECK(run_result.size() == 2 && run_result[0].open == 1 && run_result[0].close == 2 && run_result[0].volume == 2 && run_result[1].open == 9);
    }

    // ── 시계 확정(close_stale): 다음 틱 없이도 분이 지나면 닫힌다 ──────────────────────────────── [D-074]
    {
        bars::BarAggregator aggregator(bars::BarAggregator::Config{1, 64, 900, 1530});
        std::vector<MarketData> closed;
        aggregator.set_sink([&](const MarketData& market_data) { closed.push_back(market_data); });
        CHECK(aggregator.on_tick(tick("152959", 100.0, 1, 1)));
        CHECK(aggregator.on_tick(tick("153000", 101.0, 2, 3))); // 마감 동시호가 체결 → 15:29 닫힘, 15:30 진행
        CHECK(closed.size() == 1 && aggregator.current_slot(kSamsung).bucket == 15 * 60 + 30);
        CHECK(aggregator.close_stale(kSamsung, utc_of("153030")) == 0); // 같은 분 — 아직
        CHECK(aggregator.close_stale(kUnknown, utc_of("153100")) == 0);
        CHECK(aggregator.close_stale(kSamsung, utc_of("153100")) == 1); // 15:31 시계가 15:30 봉을 닫는다(장 필터 무관)
        CHECK(closed.size() == 2 && closed[1].close == 101.0 && closed[1].volume == 2);
        CHECK(!aggregator.current_slot(kSamsung).valid() && aggregator.closed_count(kSamsung) == 2);
        CHECK(aggregator.close_stale(utc_of("153200")) == 0); // 진행 봉이 없으면 0
        // 전체 꼴: 두 종목의 진행 봉을 한 번에. 아직 안 지난 종목은 남는다.
        CHECK(aggregator.on_tick(tick("100000", 50.0, 1, 1, "000660")));
        CHECK(aggregator.on_tick(tick("100100", 60.0, 1, 1, "035420"))); // 005930은 15:30이 닫혀 있어 과거 틱을 안 받는다
        CHECK(aggregator.close_stale(utc_of("100100")) == 1); // 000660(10:00)만 — 035420은 10:01 진행 중
        CHECK(aggregator.close_stale(utc_of("100200")) == 1);
        CHECK(closed.size() == 4 && closed[2].ticker == "000660" && closed[3].ticker == "035420");
        // 시계로 닫은 분에 늦은 틱이 오면 그 봉을 다시 열어 이어 붙인다(시드가 닫아 둔 자리와 같은 규칙) —
        //  틱 하나를 버리는 것보다 고저·거래량이 맞는 쪽이 낫고, 다음 시계·틱이 다시 닫는다. sink는 두 번 온다.
        CHECK(aggregator.on_tick(tick("100159", 61.0, 1, 2, "035420")));
        CHECK(aggregator.current_slot(kNaver).bucket == 10 * 60 + 1 && aggregator.closed_count(kNaver) == 0);
        CHECK(aggregator.close_stale(kNaver, utc_of("100200")) == 1);
        CHECK(closed.size() == 5 && closed[4].close == 61.0 && closed[4].high == 61.0 && closed[4].volume == 2);
    }

    // 측정(원칙 7). 한 종목 틱 100만 건을 같은 분에 넣는 on_tick의 틱당 시간 — DevScale이 WS 틱마다 스로틀 앞에서 부른다.
    {
        constexpr int kTicks = 1'000'000;
        BarAggregator aggregator(BarAggregator::Config{});
        TradeData     trade = tick("100000", 100.0, 1, 1);

        const auto start_time = std::chrono::steady_clock::now();

        for (int tick_index = 0; tick_index < kTicks; ++tick_index)
        {
            trade.price       = 100.0 + (tick_index & 7);
            trade.accumulated_volume = tick_index + 1;
            (void)aggregator.on_tick(trade);
        }

        const auto end_time = std::chrono::steady_clock::now();
        CHECK(aggregator.current_slot(kSamsung).valid());
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / kTicks;
        std::cout << "  틱당 on_tick " << nanoseconds << "ns (같은 분, 종목 하나)\n";
    }

    std::cout << "test_bar_aggregator: " << g_checks << " checks passed\n";
    return 0;
}
