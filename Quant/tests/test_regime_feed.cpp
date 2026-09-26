// 국면 판정 피드(regime/RegimeFeed.h)의 순수 함수 검사 — 응답 읽기·방향표·매수 비율·장초 기준점·regime.json 문서.
//  네트워크는 쓰지 않는다. 기대값은 파이썬 원본(PYQuant/tools/macro_regime_feed.py)이 같은 입력에서 낸 값이다. [why D-147]
#include "regime/RegimeFeed.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace regime_feed;

namespace
{

constexpr std::time_t kEvening0923 = 1790160540; // 2026-09-23 19:49 KST
constexpr std::time_t kMorning0924 = 1790206200; // 2026-09-24 08:30 KST

bool near(double left, double right)
{
    return std::abs(left - right) < 1e-9;
}

Change change_of(double percent, double price)
{
    Change change;
    change.percent    = percent;
    change.price  = price;
    change.source = "yahoo";
    return change;
}

void check_naver_index()
{
    const std::string body = R"({"datas":[
        {"itemCode":"KOSPI","marketStatus":"OPEN","fluctuationsRatioRaw":"-1.25","closePriceRaw":"3401.5"},
        {"itemCode":"KOSDAQ","marketStatus":"CLOSE","fluctuationsRatioRaw":0.8,"closePriceRaw":850.2},
        {"itemCode":"KPI200","marketStatus":"OPEN","closePriceRaw":"400"}
    ]})";
    const auto indexes = parse_naver_index(body);
    assert(indexes.size() == 2); // 등락 없는 줄은 뺀다
    const Change& kospi = indexes.at("KOSPI");
    assert(near(*kospi.percent, -1.25) && near(*kospi.price, 3401.5) && !kospi.premarket && kospi.source == "naver");
    const Change& kosdaq = indexes.at("KOSDAQ");
    assert(near(*kosdaq.percent, 0.0) && near(*kosdaq.previous_percent, 0.8) && kosdaq.premarket);

    assert(parse_naver_index("").empty());
    assert(parse_naver_index("<html>").empty());
}

void check_yahoo_chart()
{
    const std::string body = R"({"chart":{"result":[{"meta":{"regularMarketPrice":101.0,"previousClose":100.0,
        "currentTradingPeriod":{"regular":{"start":1790160000,"end":1790183400}}}}]}})";
    const Change open = parse_yahoo_chart(body, kEvening0923);
    assert(near(*open.percent, 1.0) && near(*open.price, 101.0) && !open.premarket);

    const Change early = parse_yahoo_chart(body, 1790150000); // 정규장 시작 전
    assert(near(*early.percent, 0.0) && near(*early.previous_percent, 1.0) && early.premarket);

    // previousClose가 0이면 chartPreviousClose로 넘어간다(파이썬 `or`와 같다).
    const Change fallback = parse_yahoo_chart(
        R"({"chart":{"result":[{"meta":{"regularMarketPrice":99.0,"previousClose":0,"chartPreviousClose":100.0}}]}})",
        kEvening0923);
    assert(near(*fallback.percent, -1.0));

    const Change missing = parse_yahoo_chart(R"({"chart":{"result":[{"meta":{"previousClose":100.0}}]}})", kEvening0923);
    assert(!missing.percent && missing.error == "yahoo_no_meta");
    assert(parse_yahoo_chart("", kEvening0923).error == "yahoo:parse");
}

void check_last_session_percent()
{
    // 정규장 6.5시간. 마지막 봉은 아직 안 끝났으니 그 앞 두 봉(100 → 102)의 등락이다. null 종가는 건너뛴다.
    const std::string body = R"({"chart":{"result":[{"meta":{"currentTradingPeriod":{"regular":
        {"start":1000000,"end":1023400}}},
        "timestamp":[1000,90000,180000,1790150000],
        "indicators":{"quote":[{"close":[100.0,null,102.0,110.0]}]}}]}})";
    const auto percent = parse_last_session_percent(body, kEvening0923);
    assert(percent && near(*percent, 2.0));

    assert(!parse_last_session_percent(R"({"chart":{"result":[{"meta":{}}]}})", kEvening0923));
    assert(!parse_last_session_percent("", kEvening0923));
}

void check_fred_csv()
{
    const Change change = parse_fred_csv("observation_date,DGS30\n2026-09-18,5.20\n2026-09-19,.\n2026-09-22,5.25\n");
    assert(near(*change.price, 5.25) && std::abs(*change.percent - 0.9615384615) < 1e-6 && change.source == "fred");
    assert(parse_fred_csv("observation_date,DGS30\n2026-09-18,5.20\n").error == "no_data");
    assert(parse_fred_csv("d,v\n2026-09-18,0\n2026-09-19,1\n").error == "zero_prev");
}

void check_votes_and_scale()
{
    assert(vote_for("NQ_F", 2.242) == 2);
    assert(vote_for("NQ_F", 0.5) == 1);
    assert(vote_for("NQ_F", 0.39) == 0);
    assert(vote_for("USDKRW", 0.677) == -1); // 원화 약세는 위험회피
    assert(vote_for("VIX", -9.5) == 2);
    assert(vote_for("KOSPI", -1.5) == -2);
    assert(vote_for("UNKNOWN", 5.0) == 0);

    const Thresholds thresholds;
    const double expected[] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.0};

    for (int score = -7; score <= 4; ++score)
    {
        assert(near(entry_scale(score, thresholds), expected[score + 7]));
    }

    assert(near(entry_scale(-20, thresholds), 0.0));
}

void check_open_reference()
{
    Changes changes;
    changes["NQ_F"]  = change_of(1.0, 25000.0);
    changes["TNX10"] = change_of(0.1, 4.2);
    bool should_save = false;

    // 오늘 날짜 파일이 있으면 그대로 쓴다.
    const OpenReference kept = choose_open_reference(
        "{\"date\":\"2026-09-23\",\"ts\":\"2026-09-23T09:00:05+09:00\",\"prices\":{\"NQ_F\":24900.0}}", changes, kEvening0923,
        should_save);
    assert(!should_save && kept.timestamp == "2026-09-23T09:00:05+09:00" && near(kept.prices.at("NQ_F"), 24900.0));

    // 어제 파일이면 지금 가격으로 새로 잡는다.
    const OpenReference fresh = choose_open_reference(R"({"date":"2026-09-22","prices":{"NQ_F":1.0}})", changes,
                                                      kEvening0923, should_save);
    assert(should_save && fresh.date == "2026-09-23" && fresh.prices.size() == 2 && fresh.timestamp == "2026-09-23T19:49:00+09:00");

    // 09:00 전에는 잡지 않는다.
    const OpenReference early = choose_open_reference("", changes, kMorning0924, should_save);
    assert(!should_save && early.date.empty() && early.prices.empty());
}

// 09-23 19:49 regime.json 실측과 같은 입력 — 파이썬이 낸 점수 +3·RISK_ON·1.0과 수준 문구가 같아야 한다.
void check_build_regime_golden()
{
    Changes changes;
    Change  kospi = change_of(0.0, 3450.0);
    kospi.previous_percent = 0.9;
    kospi.premarket    = true;
    kospi.source       = "naver";
    changes["KOSPI"]   = kospi;
    Change kosdaq      = kospi;
    kosdaq.price       = 860.0;
    changes["KOSDAQ"]  = kosdaq;

    Change nasdaq           = change_of(2.242, 25000.0);
    nasdaq.previous_percent     = 1.9;
    nasdaq.since_settle_percent = 0.342;
    changes["NQ_F"]         = nasdaq;
    changes["ES_F"]         = change_of(1.55, 6700.0);

    Change rate10       = change_of(0.0, 4.968);
    rate10.previous_percent = 0.4;
    rate10.premarket    = true;
    changes["TNX10"]    = rate10;
    changes["VIX"]      = change_of(-0.352, 16.0);
    changes["USDKRW"]   = change_of(0.677, 1390.0);
    changes["WTI"]      = change_of(-0.961, 70.0);
    changes["TYX30"]    = change_of(0.1, 5.29);
    changes["DGS2"]     = change_of(0.0, 4.76);
    Change high_yield;
    high_yield.error = "no_data";
    changes["HY"]    = high_yield;

    const Thresholds             thresholds;
    const nlohmann::ordered_json regime = build_regime(changes, OpenReference{}, thresholds, kEvening0923);

    assert(regime["ts"] == "2026-09-23T19:49:00+09:00");
    assert(regime["regime"] == "RISK_ON");
    assert(regime["risk_score"] == 3);
    assert(regime["entry_halt"] == false && regime["force_liquidate"] == false && regime["valid"] == true);
    assert(near(regime["entry_scale"].get<double>(), 1.0));
    assert(regime["open_ref_ts"].is_null());
    assert(regime["stale_after_sec"] == 600);
    assert(regime["thresholds"]["liq_score"] == -11);

    // 키 순서는 파이썬 원본과 같다 — 대시보드가 순서대로 그린다.
    const char* keys[] = {"ts", "regime", "entry_halt", "force_liquidate", "risk_score", "entry_scale", "open_ref_ts",
                          "valid", "stale_after_sec", "thresholds", "components", "assessment"};
    std::size_t index = 0;

    for (auto iterator = regime.begin(); iterator != regime.end(); ++iterator)
    {
        assert(iterator.key() == keys[index++]);
    }

    const auto& components = regime["components"];
    assert(components["NQ_F"]["vote"] == 2 && components["NQ_F"]["since_settle_pct"] == 0.342);
    assert(components["USDKRW"]["vote"] == -1);
    assert(components["KOSPI"]["premarket"] == true && components["KOSPI"]["src"] == "naver");
    assert(components["HY"]["pct"].is_null() && components["HY"]["tier"] == "info" && components["HY"]["err"] == "no_data");
    assert(components["TYX30"]["note"] == "5% 위. 장기 할인율 부담이 큰 구간 · 30Y-10Y +32bp");
    assert(components["DGS2"]["note"] == "4.5% 위. 긴축 기대 유지 · 10Y-2Y +21bp");
    assert(regime["assessment"]["summary"] ==
           "수준 경고 2/6 (30Y 미국채금리 (FRED, 1~2일 지연), 10Y 미국채금리) · 등락 score +3 · "
           "부담 요인이 있다. 당일 방향은 등락 표(score)로 본다");

    const nlohmann::json history = nlohmann::json::parse(history_line(regime, thresholds));
    assert(history["risk_score"] == 3 && history["halt_score"] == -7 && history["vote"]["NQ_F"] == 2);
    assert(history["pct"]["HY"].is_null());
    assert(summary_line(regime).find("[국면] RISK_ON score=+3") == 0);
}

// 장초 대비 방향표가 점수에 더해지고, 표결 지표가 절반 미만이면 판정을 보류한다.
void check_intra_and_invalid()
{
    Changes changes;
    changes["NQ_F"]  = change_of(0.0, 25100.0);
    changes["TNX10"] = change_of(0.0, 4.0);
    changes["VIX"]   = change_of(0.0, 16.0);
    changes["WTI"]   = change_of(0.0, 70.0);
    OpenReference reference;
    reference.date           = "2026-09-23";
    reference.timestamp             = "2026-09-23T09:00:05+09:00";
    reference.prices["NQ_F"]  = 25000.0; // +0.4% → +1
    reference.prices["TNX10"] = 4.1;     // −2.4% → 금리 하락이라 +1

    const Thresholds             thresholds;
    const nlohmann::ordered_json regime = build_regime(changes, reference, thresholds, kEvening0923);
    assert(regime["risk_score"] == 2 && regime["regime"] == "NEUTRAL" && regime["valid"] == true);
    assert(regime["components"]["NQ_F"]["intra"]["vote"] == 1);
    assert(regime["components"]["TNX10"]["intra"]["vote"] == 1);
    assert(regime["open_ref_ts"] == "2026-09-23T09:00:05+09:00");

    changes.erase("WTI");
    const nlohmann::ordered_json invalid = build_regime(changes, reference, thresholds, kEvening0923);
    assert(invalid["valid"] == false && invalid["regime"] == "UNKNOWN" && invalid["entry_scale"].is_null());
    assert(invalid["components"]["WTI"]["pct"].is_null() && invalid["components"]["WTI"]["err"].is_null());

    // 청산선 이하면 매수 비율 0.
    Changes crash;

    for (const char* key : {"KOSPI", "KOSDAQ", "NQ_F", "ES_F"})
    {
        crash[key] = change_of(-3.0, 100.0);
    }

    crash["VIX"] = change_of(12.0, 30.0);
    const nlohmann::ordered_json liquidate = build_regime(crash, OpenReference{}, thresholds, kEvening0923);
    assert(liquidate["risk_score"] == -10 && liquidate["entry_halt"] == true && liquidate["force_liquidate"] == false);
    Thresholds strict;
    strict.liquidate_score = -10;
    const nlohmann::ordered_json strict_regime = build_regime(crash, OpenReference{}, strict, kEvening0923);
    assert(strict_regime["force_liquidate"] == true && near(strict_regime["entry_scale"].get<double>(), 0.0));
}

} // namespace

int main()
{
    check_naver_index();
    check_yahoo_chart();
    check_last_session_percent();
    check_fred_csv();
    check_votes_and_scale();
    check_open_reference();
    check_build_regime_golden();
    check_intra_and_invalid();
    std::printf("test_regime_feed: 전부 통과\n");
    return 0;
}
