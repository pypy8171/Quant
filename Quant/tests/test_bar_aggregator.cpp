// N분봉 집계기(core/BarAggregator.h) 단위 테스트. 버킷 정렬(REST aggregate_minutes와 같은 식)·자정 단조·빈 구간
//  건너뜀·장 밖 틱 폐기·누적 거래량 차와 qty 합산 뒷걸음·시드 병합(닫힌 봉 REST 우선, 진행 봉 합침, 빈 자리
//  채움)·keep 상한·bar_index 0=최신·닫힘 콜백, 그리고 1분 기저를 N분으로 접는 resample이 직접 집계와 같음을
//  고정한다. KIS·Engine·Logger 없이 링크한다.
//  관련 결정: D-068·D-072.
// 빌드: cmake --build <dir> --target test_bar_aggregator
#include "core/BarAggregator.h"
#include "core/KstTime.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 2026-09-14 09:00:00 KST = 00:00:00 UTC. 틱 수신 시각은 거래소 시각과 같은 초로 둔다.
constexpr std::time_t kDay0900Utc = 1789344000;

std::time_t utc_of(const std::string& hhmmss)
{
    const int hh = std::stoi(hhmmss.substr(0, 2));
    const int mm = std::stoi(hhmmss.substr(2, 2));
    const int ss = std::stoi(hhmmss.substr(4, 2));
    return kDay0900Utc + (hh - 9) * 3600 + mm * 60 + ss;
}

TradeData tick(const std::string& hhmmss, double px, int64_t qty, int64_t acml, const char* ticker = "005930")
{
    TradeData td;
    td.ticker      = ticker;
    td.time        = hhmmss;
    td.price       = px;
    td.quantity    = qty;
    td.acml_volume = acml;
    td.market      = Market::KR;
    td.timestamp   = std::chrono::system_clock::from_time_t(utc_of(hhmmss));
    return td;
}

// REST 봉 흉내 — timestamp는 버킷의 마지막 1분 시각(aggregate_minutes와 같다).
MarketData rest_bar(const std::string& last_min_hhmmss, double o, double h, double l, double c, int64_t v)
{
    MarketData md;
    md.ticker    = "005930";
    md.market    = Market::KR;
    md.open      = o;
    md.high      = h;
    md.low       = l;
    md.close     = c;
    md.volume    = v;
    md.timestamp = std::chrono::system_clock::from_time_t(utc_of(last_min_hhmmss));
    return md;
}
} // namespace

int main()
{
    using bars::BarAggregator;
    using bars::BarSlot;

    // ── slot_of: 시계 정렬, 장 밖, 여섯 자리 아님 ────────────────────────────────
    {
        const BarSlot a = bars::slot_of("090000", utc_of("090000"), 3, 900, 1530);
        const BarSlot b = bars::slot_of("090259", utc_of("090259"), 3, 900, 1530);
        const BarSlot c = bars::slot_of("090300", utc_of("090300"), 3, 900, 1530);
        CHECK(a.valid() && a == b && b != c && a < c);
        CHECK(a.bucket == (9 * 60) / 3 && c.bucket == a.bucket + 1);
        CHECK(!bars::slot_of("085959", utc_of("085959"), 3, 900, 1530).valid());   // 동시호가 전
        CHECK(bars::slot_of("153000", utc_of("153000"), 3, 900, 1530).valid());    // 마감 동시호가 체결
        CHECK(!bars::slot_of("153100", utc_of("153100"), 3, 900, 1530).valid());   // 장 뒤
        CHECK(!bars::slot_of("154000", utc_of("154000"), 3, 900, 1530).valid());   // 시간외
        CHECK(!bars::slot_of("990000", utc_of("090000"), 3, 900, 1530).valid());   // 깨진 시각
        CHECK(!bars::slot_of("090000", utc_of("090000"), 0, 900, 1530).valid());   // 간격 0
        // 여섯 자리가 아니면 수신 시각의 KST 분을 쓴다 — REST 대체 틱.
        const BarSlot r = bars::slot_of("", utc_of("100130"), 3, 900, 1530);
        CHECK(r.valid() && r.bucket == (10 * 60 + 1) / 3);
        // 자정 단조: 다음날 09:00 자리는 오늘 15:30 자리보다 크다.
        const BarSlot today_last = bars::slot_of("153000", utc_of("153000"), 3, 900, 1530);
        const BarSlot tomorrow   = bars::slot_of("090000", utc_of("090000") + 86400, 3, 900, 1530);
        CHECK(today_last < tomorrow && tomorrow.day == today_last.day + 1);
        // 봉 시작 시각은 버킷의 첫 분.
        const auto st = std::chrono::system_clock::to_time_t(bars::slot_start(b, utc_of("090259"), 3));
        CHECK(st == utc_of("090000"));
    }

    // ── 한 봉 안 OHLC·누적 거래량 차, 다음 버킷 첫 틱이 앞 봉을 닫음, bar_index 0=최신 ──
    {
        BarAggregator::Config cfg;
        BarAggregator agg(cfg);
        std::vector<MarketData> closed;
        agg.set_sink([&](const MarketData& md) { closed.push_back(md); });

        CHECK(agg.on_tick(tick("090000", 100.0, 10, 10)));
        CHECK(agg.on_tick(tick("090030", 103.0, 5, 15)));
        CHECK(agg.on_tick(tick("090159", 98.0, 7, 22)));
        CHECK(agg.on_tick(tick("090259", 101.0, 3, 25)));
        CHECK(closed.empty() && agg.closed_count("005930") == 0);

        auto snap = agg.snapshot("005930");
        CHECK(snap.size() == 1);
        CHECK(snap[0].open == 100.0 && snap[0].high == 103.0 && snap[0].low == 98.0 && snap[0].close == 101.0);
        CHECK(snap[0].volume == 25);
        CHECK(snap[0].bar_index == 0 && snap[0].ticker == "005930");

        // 09:03:00 첫 틱이 09:00 봉을 닫는다. 사이에 틱을 흘렸어도(acml 25→40) 거래량은 누적차라 맞다.
        CHECK(agg.on_tick(tick("090300", 102.0, 4, 44)));
        CHECK(closed.size() == 1 && closed[0].close == 101.0 && closed[0].volume == 25);
        CHECK(agg.closed_count("005930") == 1);
        CHECK(agg.on_tick(tick("090400", 104.0, 6, 50)));
        snap = agg.snapshot("005930");
        CHECK(snap.size() == 2 && snap[0].bar_index == 0 && snap[1].bar_index == 1);
        CHECK(snap[0].open == 102.0 && snap[0].close == 104.0 && snap[0].volume == 10); // 44−40 + 6
        CHECK(snap[1].close == 101.0);

        // 늦게 온 과거 틱은 버린다 — 닫힌 봉을 고치지 않는다.
        CHECK(!agg.on_tick(tick("090250", 90.0, 1, 51)));
        CHECK(agg.snapshot("005930")[1].close == 101.0);

        // 빈 구간(09:06~09:08 틱 없음) 뒤 09:09 틱 — 봉을 만들지 않고 건너뛴다.
        CHECK(agg.on_tick(tick("090900", 105.0, 1, 60)));
        CHECK(agg.closed_count("005930") == 2);
        const BarSlot cur = agg.current_slot("005930");
        CHECK(cur.bucket == (9 * 60 + 9) / 3);
        snap = agg.snapshot("005930");
        CHECK(snap.size() == 3 && snap[1].open == 102.0); // [1]=09:03 봉, 09:06 봉은 없다

        // max_count
        CHECK(agg.snapshot("005930", 2).size() == 2);
        CHECK(agg.snapshot("없는종목").empty());
    }

    // ── 장 밖·가격 0·다른 종목은 섞이지 않음, 종목별 독립 ────────────────────────
    {
        BarAggregator agg(BarAggregator::Config{});
        CHECK(!agg.on_tick(tick("085000", 100.0, 1, 1)));
        CHECK(!agg.on_tick(tick("154000", 100.0, 1, 1)));
        CHECK(!agg.on_tick(tick("090000", 0.0, 1, 1)));
        CHECK(agg.on_tick(tick("090000", 100.0, 1, 1)));
        CHECK(agg.on_tick(tick("090000", 50.0, 1, 1, "000660")));
        CHECK(agg.snapshot("005930")[0].close == 100.0 && agg.snapshot("000660")[0].close == 50.0);
        agg.clear("005930");
        CHECK(agg.snapshot("005930").empty() && agg.snapshot("000660").size() == 1);
    }

    // ── acml 없는 틱(REST 대체·선물)은 qty 합산으로 뒷걸음 ────────────────────────
    {
        BarAggregator agg(BarAggregator::Config{});
        CHECK(agg.on_tick(tick("100000", 10.0, 3, 0)));
        CHECK(agg.on_tick(tick("100100", 11.0, 4, 0)));
        CHECK(agg.snapshot("005930")[0].volume == 7);
        // 중간에 acml이 붙어도 기준값이 없으면 계속 합산한다.
        CHECK(agg.on_tick(tick("100200", 12.0, 5, 999)));
        CHECK(agg.snapshot("005930")[0].volume == 12);
    }

    // ── 시드: 빈 상태에 REST 봉 넣기, 닫힌 봉은 REST가 이김, 진행 봉은 합침 ──────────
    {
        BarAggregator agg(BarAggregator::Config{});
        std::vector<MarketData> closed;
        agg.set_sink([&](const MarketData& md) { closed.push_back(md); });

        // 로컬은 09:06 봉을 09:07부터만 봤다(구독이 늦었다).
        CHECK(agg.on_tick(tick("090700", 200.0, 10, 500)));
        CHECK(agg.on_tick(tick("090800", 205.0, 10, 510)));
        CHECK(agg.snapshot("005930")[0].open == 200.0 && agg.snapshot("005930")[0].volume == 20);

        // REST가 09:00·09:03(닫힘)·09:06(진행 중, 시가 198·거래량 480) 세 봉을 돌려줬다.
        std::vector<MarketData> rest = {
            rest_bar("090800", 198.0, 206.0, 197.0, 204.0, 480), // [0]=최신(진행 중)
            rest_bar("090500", 190.0, 199.0, 189.0, 198.0, 300),
            rest_bar("090200", 185.0, 191.0, 184.0, 190.0, 250),
        };
        CHECK(agg.seed("005930", rest) == 2);
        CHECK(agg.closed_count("005930") == 2);
        auto snap = agg.snapshot("005930");
        CHECK(snap.size() == 3);
        CHECK(snap[0].open == 198.0 && snap[0].high == 206.0 && snap[0].low == 197.0);
        CHECK(snap[0].close == 205.0);   // 종가는 로컬(더 늦다)
        CHECK(snap[0].volume == 480);    // 거래량은 큰 쪽
        CHECK(snap[1].close == 198.0 && snap[2].close == 190.0);
        CHECK(snap[1].ticker == "005930" && snap[1].bar_index == 1 && snap[2].bar_index == 2);

        // 합친 뒤 틱이 더 오면 거래량은 합친 값 위에 쌓인다(누적차 기준 이동).
        CHECK(agg.on_tick(tick("090830", 207.0, 5, 515)));
        CHECK(agg.snapshot("005930")[0].volume == 485 && agg.snapshot("005930")[0].high == 207.0);

        // 다시 시드하면 닫힌 봉은 REST 값으로 덮인다(로컬 09:03 봉이 틀렸다고 치자). 새로 든 봉은 0.
        std::vector<MarketData> again = {rest_bar("090500", 191.0, 199.5, 189.0, 199.0, 310)};
        CHECK(agg.seed("005930", again) == 0);
        CHECK(agg.snapshot("005930")[1].close == 199.0 && agg.snapshot("005930")[1].volume == 310);

        // 시드가 진행 봉보다 새 자리를 갖고 있으면 진행 봉을 닫고 그 뒤에 붙인다. 그 자리의 틱은 REST 봉 위에 이어 쓴다.
        std::vector<MarketData> ahead = {rest_bar("091100", 210.0, 212.0, 208.0, 211.0, 100)};
        CHECK(agg.seed("005930", ahead) == 1);
        CHECK(closed.size() == 1 && closed[0].close == 207.0);
        CHECK(agg.closed_count("005930") == 4 && !agg.current_slot("005930").valid());
        CHECK(agg.on_tick(tick("091130", 213.0, 2, 700)));
        snap = agg.snapshot("005930");
        CHECK(snap.size() == 4 && snap[0].open == 210.0 && snap[0].high == 213.0 && snap[0].close == 213.0);
        CHECK(snap[0].volume == 102);
        CHECK(agg.on_tick(tick("091140", 214.0, 3, 703)));
        CHECK(agg.snapshot("005930")[0].volume == 105);
        // 시드가 닫아 둔 자리보다 오래된 틱은 버린다.
        CHECK(!agg.on_tick(tick("090600", 1.0, 1, 704)));

        // timestamp 없는 REST 봉·빈 벡터·빈 종목은 무시.
        MarketData nots;
        nots.close = 1.0;
        CHECK(agg.seed("005930", {nots}) == 0);
        CHECK(agg.seed("005930", {}) == 0);
        CHECK(agg.seed("", rest) == 0);
    }

    // ── keep 상한: 오래된 봉부터 버림, 슬롯 순서 유지 ────────────────────────────
    {
        BarAggregator::Config cfg;
        cfg.keep = 3;
        BarAggregator agg(cfg);
        int n_closed = 0;
        agg.set_sink([&](const MarketData&) { ++n_closed; });

        for (int i = 0; i < 6; ++i)
        {
            char buf[7];
            std::snprintf(buf, sizeof(buf), "%02d%02d00", 9, i * 3);
            CHECK(agg.on_tick(tick(buf, 100.0 + i, 1, i + 1)));
        }

        CHECK(n_closed == 5 && agg.closed_count("005930") == 3);
        auto snap = agg.snapshot("005930");
        CHECK(snap.size() == 4);
        CHECK(snap[0].close == 105.0 && snap[3].close == 102.0); // [0]=진행 중(09:15), 09:00·09:03은 버려짐
        // 시드로 더 넣어도 상한을 넘기지 않는다.
        std::vector<MarketData> old = {rest_bar("090200", 1, 1, 1, 1, 1)};
        agg.seed("005930", old);
        CHECK(agg.closed_count("005930") == 3 && agg.snapshot("005930")[3].close == 102.0);
    }

    // ── 자정을 넘긴 두 날은 같은 버킷 번호라도 다른 봉 ──────────────────────────
    {
        BarAggregator agg(BarAggregator::Config{});
        TradeData d1 = tick("090000", 10.0, 1, 1);
        TradeData d2 = tick("090000", 20.0, 1, 1);
        d2.timestamp += std::chrono::hours(24);
        CHECK(agg.on_tick(d1));
        CHECK(agg.on_tick(d2));
        CHECK(agg.closed_count("005930") == 1);
        auto snap = agg.snapshot("005930");
        CHECK(snap[0].close == 20.0 && snap[1].close == 10.0);
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
        int         acml   = 0;

        for (int i = 0; i < 7; ++i)
        {
            const std::string m  = mins[i];
            const double      px = 100.0 + i;
            acml += 5;
            CHECK(one.on_tick(tick(m + "10", px + 0.5, 5, acml)));
            CHECK(three.on_tick(tick(m + "10", px + 0.5, 5, acml)));
            acml += 7;
            CHECK(one.on_tick(tick(m + "40", px, 7, acml)));
            CHECK(three.on_tick(tick(m + "40", px, 7, acml)));
        }

        const auto s1 = one.snapshot("005930");
        CHECK(s1.size() == 7); // 진행 중 09:07 + 닫힌 6
        const auto r3 = bars::resample(s1, 3);
        const auto d3 = three.snapshot("005930");
        CHECK(r3.size() == 3 && d3.size() == 3); // 09:06(진행 중)·09:03·09:00

        for (size_t i = 0; i < 3; ++i)
        {
            CHECK(r3[i].open == d3[i].open && r3[i].high == d3[i].high && r3[i].low == d3[i].low &&
                  r3[i].close == d3[i].close && r3[i].volume == d3[i].volume);
            CHECK(r3[i].bar_index == static_cast<int>(i));
        }

        CHECK(r3[2].open == 100.5 && r3[2].close == 102.0 && r3[2].high == 102.5 && r3[2].low == 100.0);
        CHECK(r3[1].volume == 24); // 09:03·09:05 두 분(09:04 없음) × 12
        // timestamp는 버킷 마지막 분(aggregate_minutes와 같다).
        CHECK(std::chrono::system_clock::to_time_t(r3[1].timestamp) == utc_of("090500"));
        // max_count는 최신부터 자른다. interval 1은 복사·bar_index 재부여.
        CHECK(bars::resample(s1, 3, 2).size() == 2 && bars::resample(s1, 3, 2)[1].close == r3[1].close);
        const auto same = bars::resample(s1, 1, 4);
        CHECK(same.size() == 4 && same[3].close == s1[3].close && same[3].bar_index == 3);
        CHECK(bars::resample({}, 3).empty());
        // REST 1분봉(진짜 UTC timestamp)을 접어도 같은 자리에 떨어진다.
        std::vector<MarketData> rest = {rest_bar("090100", 1, 2, 1, 2, 1), rest_bar("090000", 1, 1, 1, 1, 1),
                                        rest_bar("085900", 9, 9, 9, 9, 1)};
        const auto rr = bars::resample(rest, 3);
        CHECK(rr.size() == 2 && rr[0].open == 1 && rr[0].close == 2 && rr[0].volume == 2 && rr[1].open == 9);
    }

    std::cout << "test_bar_aggregator: " << g_checks << " checks passed\n";
    return 0;
}
