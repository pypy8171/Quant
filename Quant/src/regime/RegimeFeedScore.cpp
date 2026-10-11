// 국면 판정 점수 — 방향표·매수 비율·장초 기준점·regime.json 문서·이력 줄·로그 요약 줄. 네트워크·파일 없음.
//  판정식·임계·문구는 파이썬 macro_regime_feed.py(09-26 기준, 09-27 삭제)를 그대로 옮겼다. 임계는 전부 검증 전
//  잠정값이다(D-033·D-083) — 바꿀 때는 파일 문서의 thresholds와 대시보드 표시가 같이 바뀐다.
//  선언은 regime/RegimeFeed.h. 스레드: 국면 피드 스레드(RegimeFeed::cycle)와 테스트가 부른다. 상태 없음. [why D-147]
#include "regime/RegimeFeed.h"

#include "detail/RegimeTables.h"

#include "core/KstTime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace regime_feed
{

using detail::child;
using detail::InfoSymbol;
using detail::IntraSymbol;
using detail::kInfoSymbols;
using detail::kIntraSymbols;
using detail::kVotingSymbols;
using detail::VotingSymbol;

namespace
{

// [formula] 수준 경계. 게이트가 아니라 설명용 note다. 근거는 통상 인용되는 구간이지 이 시스템에서 검증한 값이 아니다.
struct Band
{
    double           low;
    std::string_view text;
};

struct LevelTable
{
    Indicator         key;
    std::vector<Band> bands;
};

const std::vector<LevelTable>& level_tables()
{
    static const std::vector<LevelTable> tables{
        {Indicator::Tyx30, {{5.0, "5% 위. 장기 할인율 부담이 큰 구간"}, {4.5, "4.5~5%. 고점권"}}},
        {Indicator::Tnx10, {{4.5, "4.5% 위. 고점권"}, {4.0, "4~4.5%"}}},
        {Indicator::Vix, {{30.0, "30 위. 공포 구간"}, {20.0, "20~30. 경계 구간"}, {15.0, "15~20. 보통"}}},
        {Indicator::UsdKrw, {{1400.0, "1400 위. 원화 약세 경계"}, {1350.0, "1350~1400. 약세 구간"}}},
        {Indicator::Wti, {{100.0, "100달러 위. 인플레 재점화 우려"}, {90.0, "90~100달러. 부담 구간"}}},
        {Indicator::HighYield,
         {{5.0, "5%p 위. 신용 스트레스 확대"},
          {4.0, "4~5%p. 확대 조짐"},
          {3.0, "3~4%p. 보통"},
          {0.0, "3%p 아래. 신용 시장 평온"}}},
        {Indicator::Dgs2, {{4.5, "4.5% 위. 긴축 기대 유지"}, {4.0, "4~4.5%"}}},
    };
    return tables;
}

// 종합 경고에 세는 경계(지표당 1개). 개수만 세고 점수에는 더하지 않는다.
struct WarnLevel
{
    Indicator key;
    double    level;
};

constexpr std::array<WarnLevel, 6> kWarnLevels{{{Indicator::Tyx30, 5.0},
                                                {Indicator::Tnx10, 4.5},
                                                {Indicator::Vix, 20.0},
                                                {Indicator::UsdKrw, 1400.0},
                                                {Indicator::Wti, 90.0},
                                                {Indicator::HighYield, 4.0}}};

constexpr int kStaleAfterSec      = 600;  // 문서에 적는 값. 엔진은 자기 regime_stale_sec(파일 수정 시각)으로 본다
constexpr int kOpenReferenceHour  = 9;

// 소수 자릿수로 끊는다. round(x*1000)/1000은 0.47300000000000003처럼 남아 파일에 그대로 찍히므로, 파이썬 round()처럼
//  십진 문자열을 거쳐 가장 가까운 double로 되돌린다.
double round_digits(double value, int digits)
{
    return std::strtod(std::format("{:.{}f}", value, digits).c_str(), nullptr);
}

double round3(double value)
{
    return round_digits(value, 3);
}

nlohmann::ordered_json optional_number(const std::optional<double>& value)
{
    return value ? nlohmann::ordered_json(*value) : nlohmann::ordered_json();
}

nlohmann::ordered_json error_or_null(const std::string& error)
{
    return error.empty() ? nlohmann::ordered_json() : nlohmann::ordered_json(error);
}

std::string iso_kst(std::time_t now_utc)
{
    std::string text = kst::datetime(now_utc); // YYYY-MM-DD HH:MM:SS
    text[10]         = 'T';
    return text + "+09:00";
}

// regime.json components의 지표 칸. 없으면 만든다.
nlohmann::ordered_json& component_of(nlohmann::ordered_json& components, Indicator indicator)
{
    return components[std::string(indicator_name(indicator))];
}

// components[지표 이름]의 price. 없거나 숫자가 아니면 비움.
std::optional<double> component_price(const nlohmann::ordered_json& components, Indicator indicator)
{
    const auto found = components.find(std::string(indicator_name(indicator)));

    if (found == components.end())
    {
        return std::nullopt;
    }

    const auto price = found->find("price");
    return (price != found->end() && price->is_number()) ? std::optional<double>(price->get<double>())
                                                           : std::nullopt;
}

// 기존 note 뒤에 " · "로 잇는다.
void append_note(nlohmann::ordered_json& component, const std::string& addition)
{
    const auto        found    = component.find("note");
    const std::string existing = (found != component.end() && found->is_string()) ? found->get<std::string>() : "";
    component["note"]          = existing.empty() ? addition : existing + " · " + addition;
}

// 수준표가 있는 지표마다 구간 note(금리는 전일비 bp도)를 채우고, 경고 경계를 넘은 지표 이름을 flags에 더한다.
void note_level_bands(nlohmann::ordered_json& components, nlohmann::ordered_json& flags)
{
    for (const LevelTable& table : level_tables())
    {
        const std::optional<double> price = component_price(components, table.key);

        if (!price)
        {
            continue;
        }

        nlohmann::ordered_json& component = component_of(components, table.key);
        std::string             note;

        for (const Band& band : table.bands)
        {
            if (*price >= band.low)
            {
                note = band.text;
                break;
            }
        }

        // 금리는 %등락보다 bp가 읽기 쉽다. pct와 price로 전일값을 되살려 bp를 붙인다.
        const bool is_rate = table.key == Indicator::Tyx30 || table.key == Indicator::Tnx10 ||
                             table.key == Indicator::Dgs2;
        const auto percent     = component.find("pct");

        if (is_rate && percent != component.end() && percent->is_number())
        {
            const double previous = *price / (1.0 + percent->get<double>() / 100.0);
            const double basis_points       = (*price - previous) * 100.0;

            if (std::abs(basis_points) >= 5.0)
            {
                note = (note.empty() ? "" : note + " · ") + std::format("전일비 {:+.0f}bp", basis_points);
            }
        }

        if (!note.empty())
        {
            component["note"] = note;
        }

        for (const WarnLevel& warn : kWarnLevels)
        {
            if (warn.key == table.key && *price >= warn.level)
            {
                flags.push_back(component["label"]);
            }
        }
    }
}

// 30Y-10Y·10Y-2Y 금리차를 TYX30·DGS2 note에 덧붙인다. 두 금리가 다 있을 때만.
void note_rate_spreads(nlohmann::ordered_json& components)
{
    const std::optional<double> rate30 = component_price(components, Indicator::Tyx30);
    const std::optional<double> rate10 = component_price(components, Indicator::Tnx10);
    const std::optional<double> rate2  = component_price(components, Indicator::Dgs2);

    if (rate30 && rate10)
    {
        const double spread   = *rate30 - *rate10;
        std::string  addition = std::format("30Y-10Y {:+.0f}bp", spread * 100.0);

        if (spread >= 0.5)
        {
            addition += " (장기 프리미엄 확대)";
        }
        else if (spread <= 0.0)
        {
            addition += " (역전 근접)";
        }

        append_note(component_of(components, Indicator::Tyx30), addition);
    }

    if (rate10 && rate2)
    {
        const double spread = *rate10 - *rate2;
        append_note(component_of(components, Indicator::Dgs2),
                    std::format("10Y-2Y {:+.0f}bp", spread * 100.0) + (spread < 0.0 ? " (역전)" : ""));
    }
}

// 경고 개수와 등락 score로 종합 한 줄.
std::string level_summary(const nlohmann::ordered_json& flags, int score)
{
    const std::size_t count = flags.size();
    std::string       judge;

    if (count >= 3)
    {
        judge = "수준 부담이 겹쳐 있다. 신규 진입은 비중을 줄이고, 청산은 score 기준을 따른다";
    }
    else if (count >= 1)
    {
        judge = "부담 요인이 있다. 당일 방향은 등락 표(score)로 본다";
    }
    else
    {
        judge = "수준 경고 없음. 당일 등락 표(score)로 본다";
    }

    std::string summary = std::format("수준 경고 {}/{}", count, kWarnLevels.size());

    if (count > 0)
    {
        std::string joined;

        for (const auto& flag : flags)
        {
            joined += (joined.empty() ? "" : ", ") + flag.get<std::string>();
        }

        summary += " (" + joined + ")";
    }

    summary += std::format(" · 등락 score {:+d} · ", score) + judge;
    return summary;
}

// 수준 note와 종합 한 줄. 게이트(score)는 건드리지 않는다. 각 component에 note를 채운다.
nlohmann::ordered_json assess_levels(nlohmann::ordered_json& components, int score)
{
    nlohmann::ordered_json flags = nlohmann::ordered_json::array();
    note_level_bands(components, flags);
    note_rate_spreads(components);

    nlohmann::ordered_json assessment;
    assessment["flags"]   = flags;
    assessment["summary"] = level_summary(flags, score);
    return assessment;
}

} // namespace

int vote_for(Indicator indicator, double percent)
{
    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        if (symbol.key != indicator)
        {
            continue;
        }

        const double magnitude = std::abs(percent);
        const int    strength  = magnitude >= symbol.strong ? 2 : (magnitude >= symbol.warn ? 1 : 0);

        if (strength == 0)
        {
            return 0;
        }

        return symbol.vote_direction * (percent > 0.0 ? 1 : -1) * strength;
    }

    return 0;
}

// [formula] 정지선 0, 정지선 절반 0.4, 0점 0.7, on_score 1.0을 잇는 직선. 0.1 단위로 끊어 3분마다 미세하게
//  바뀌어 분할 매수가 재구성되는 일을 막는다. NEUTRAL이면 최대 90%, RISK_ON일 때만 100%(09-18). [why D-083]
double entry_scale(int score, const Thresholds& thresholds)
{
    const double halt = static_cast<double>(thresholds.halt_score);
    const std::array<std::pair<double, double>, 4> points{
        {{halt, 0.0}, {halt / 2.0, 0.4}, {0.0, 0.7}, {static_cast<double>(thresholds.on_score), 1.0}}};
    const double value = static_cast<double>(score);

    if (value <= points.front().first)
    {
        return 0.0;
    }

    if (value >= points.back().first)
    {
        return 1.0;
    }

    for (std::size_t index = 0; index + 1 < points.size(); ++index)
    {
        const auto& [left_x, left_y]   = points[index];
        const auto& [right_x, right_y] = points[index + 1];

        if (left_x <= value && value <= right_x)
        {
            return round_digits(left_y + (right_y - left_y) * (value - left_x) / (right_x - left_x), 1);
        }
    }

    return 1.0;
}

OpenReference choose_open_reference(std::string_view existing_text, const Changes& changes, std::time_t now_utc,
                                    bool& should_save)
{
    should_save              = false;
    const std::string today  = kst::datetime(now_utc).substr(0, 10);
    const nlohmann::json doc = nlohmann::json::parse(existing_text, nullptr, false);
    const nlohmann::json* date   = child(doc, "date");
    const nlohmann::json* prices = child(doc, "prices");

    if (date != nullptr && date->is_string() && date->get<std::string>() == today && prices != nullptr &&
        prices->is_object() && !prices->empty())
    {
        OpenReference reference;
        reference.date = today;
        const nlohmann::json* stamp = child(doc, "ts");
        reference.timestamp   = (stamp != nullptr && stamp->is_string()) ? stamp->get<std::string>() : "";

        // 지표 이름표에 없는 키는 버린다(장초 방향표는 표에 있는 지표만 본다).
        for (auto iterator = prices->begin(); iterator != prices->end(); ++iterator)
        {
            const std::optional<Indicator> indicator = indicator_from_name(iterator.key());

            if (indicator && iterator.value().is_number())
            {
                reference.prices[*indicator] = iterator.value().get<double>();
            }
        }

        return reference;
    }

    if (kst::to_tm(now_utc).tm_hour < kOpenReferenceHour)
    {
        return {};
    }

    OpenReference reference;

    for (const IntraSymbol& symbol : kIntraSymbols)
    {
        const Change* found = changes.find(symbol.key);

        if (found != nullptr && found->price)
        {
            reference.prices[symbol.key] = *found->price;
        }
    }

    if (reference.prices.empty())
    {
        return {};
    }

    reference.date = today;
    reference.timestamp   = iso_kst(now_utc);
    should_save    = true;
    return reference;
}

namespace
{

// 표결 지표를 센 결과. build_regime의 단계 함수 사이에서만 오간다.
struct VoteTally
{
    int score       = 0;
    int valid_count = 0;
};

// 표결 지표 8개의 component를 채우고 전일 대비 표를 센다. 등락이 없는 지표는 0표·err만 적는다.
void add_voting_components(const Changes& changes, nlohmann::ordered_json& components, VoteTally& tally)
{
    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        const std::string key(indicator_name(symbol.key));
        const Change*     found = changes.find(symbol.key);
        const Change      empty;
        const Change&     change = found == nullptr ? empty : *found;
        nlohmann::ordered_json component;
        component["label"] = symbol.label;

        if (!change.percent)
        {
            component["pct"]  = nullptr;
            component["vote"] = 0;
            component["err"]  = error_or_null(change.error);
            components[key]   = component;
            continue;
        }

        const int vote = vote_for(symbol.key, *change.percent);
        tally.score += vote;
        ++tally.valid_count;
        component["pct"]   = round3(*change.percent);
        component["vote"]  = vote;
        component["price"] = optional_number(change.price);
        component["src"]   = change.source;

        if (change.premarket)
        {
            component["premarket"] = true;
        }

        if (change.previous_percent)
        {
            component["prev_pct"] = round3(*change.previous_percent);
        }

        if (change.since_settle_percent)
        {
            component["since_settle_pct"] = round3(*change.since_settle_percent);
        }

        components[key] = component;
    }
}

// 장초 대비 방향표. 기준점이 없으면(개장 전·첫 계산) 0표. 더한 표의 합을 돌려준다.
int add_intra_votes(const OpenReference& open_reference, nlohmann::ordered_json& components)
{
    int score = 0;

    for (const IntraSymbol& symbol : kIntraSymbols)
    {
        const double* base = open_reference.prices.find(symbol.key);

        if (base == nullptr || *base == 0.0)
        {
            continue;
        }

        nlohmann::ordered_json& component = component_of(components, symbol.key);
        const auto              price     = component.find("price");

        if (price == component.end() || !price->is_number())
        {
            continue;
        }

        const double intra_percent  = (price->get<double>() - *base) / *base * 100.0;
        int          intra_vote = 0;

        if (std::abs(intra_percent) >= symbol.warn)
        {
            intra_vote = symbol.vote_direction * (intra_percent > 0.0 ? 1 : -1);
        }

        score += intra_vote;
        nlohmann::ordered_json intra;
        intra["pct"]       = round3(intra_percent);
        intra["vote"]      = intra_vote;
        intra["base"]      = *base;
        component["intra"] = intra;
    }

    return score;
}

// 참고 지표 3개의 component. 표 없음(tier "info").
void add_info_components(const Changes& changes, nlohmann::ordered_json& components)
{
    for (const InfoSymbol& symbol : kInfoSymbols)
    {
        const std::string key(indicator_name(symbol.key));
        const Change*     found = changes.find(symbol.key);
        const Change      empty;
        const Change&     change = found == nullptr ? empty : *found;
        nlohmann::ordered_json component;
        component["label"] = symbol.label;

        if (!change.percent)
        {
            component["pct"]  = nullptr;
            component["vote"] = 0;
            component["tier"] = "info";
            component["err"]  = error_or_null(change.error);
        }
        else
        {
            component["pct"]   = round3(*change.percent);
            component["vote"]  = 0;
            component["tier"]  = "info";
            component["price"] = optional_number(change.price);
        }

        components[key] = component;
    }
}

// 판정 이름. 유효 지표가 모자라면 UNKNOWN.
std::string regime_label(bool valid, int score, const Thresholds& thresholds)
{
    if (!valid)
    {
        return "UNKNOWN";
    }

    if (score <= thresholds.halt_score)
    {
        return "RISK_OFF";
    }

    if (score >= thresholds.on_score)
    {
        return "RISK_ON";
    }

    return "NEUTRAL";
}

} // namespace

nlohmann::ordered_json build_regime(const Changes& changes, const OpenReference& open_reference,
                                    const Thresholds& thresholds, std::time_t now_utc)
{
    nlohmann::ordered_json components = nlohmann::ordered_json::object();
    VoteTally              tally;
    add_voting_components(changes, components, tally);
    tally.score += add_intra_votes(open_reference, components);
    add_info_components(changes, components);

    const int              score      = tally.score;
    nlohmann::ordered_json assessment = assess_levels(components, score);

    // 유효 지표가 절반 미만이면 판정 보류 — 데이터 공백에 게이트가 움직이지 않게.
    const int  needed          = std::max(1, (static_cast<int>(kVotingSymbols.size()) + 1) / 2);
    const bool valid           = tally.valid_count >= needed;
    const bool entry_halt      = valid && score <= thresholds.halt_score;
    const bool force_liquidate = valid && score <= thresholds.liquidate_score;
    const std::string label    = regime_label(valid, score, thresholds);

    nlohmann::ordered_json regime;
    regime["ts"]              = iso_kst(now_utc);
    regime["regime"]          = label;
    regime["entry_halt"]      = entry_halt;
    regime["force_liquidate"] = force_liquidate;
    regime["risk_score"]      = score;
    regime["entry_scale"] =
        valid ? nlohmann::ordered_json(force_liquidate ? 0.0 : entry_scale(score, thresholds)) : nlohmann::ordered_json();
    regime["open_ref_ts"]     = open_reference.timestamp.empty() ? nlohmann::ordered_json() : nlohmann::ordered_json(open_reference.timestamp);
    regime["valid"]           = valid;
    regime["stale_after_sec"] = kStaleAfterSec;

    nlohmann::ordered_json threshold_node;
    threshold_node["halt_score"] = thresholds.halt_score;
    threshold_node["liq_score"]  = thresholds.liquidate_score;
    threshold_node["on_score"]   = thresholds.on_score;
    regime["thresholds"]         = threshold_node;
    regime["components"]         = components;
    regime["assessment"]         = assessment;
    return regime;
}

// 임계값을 다시 정할 때 필요한 최소 단위(시각·점수·판정·지표별 등락)만 적는다. [why D-033]
std::string history_line(const nlohmann::ordered_json& regime, const Thresholds& thresholds)
{
    nlohmann::ordered_json row;

    for (const char* key : {"ts", "regime", "risk_score", "entry_halt", "force_liquidate", "valid"})
    {
        row[key] = regime.value(key, nlohmann::ordered_json());
    }

    row["halt_score"]  = thresholds.halt_score;
    row["liq_score"]   = thresholds.liquidate_score;
    row["entry_scale"] = regime.value("entry_scale", nlohmann::ordered_json());

    nlohmann::ordered_json intra = nlohmann::ordered_json::object();
    nlohmann::ordered_json percent   = nlohmann::ordered_json::object();
    nlohmann::ordered_json vote  = nlohmann::ordered_json::object();
    const auto             found = regime.find("components");

    if (found != regime.end())
    {
        for (auto iterator = found->begin(); iterator != found->end(); ++iterator)
        {
            const auto& component = iterator.value();
            const auto  intra_node = component.find("intra");

            if (intra_node != component.end())
            {
                intra[iterator.key()] = intra_node->value("vote", 0);
            }

            percent[iterator.key()]  = component.value("pct", nlohmann::ordered_json());
            vote[iterator.key()] = component.value("vote", 0);
        }
    }

    row["intra"] = intra;
    row["pct"]   = percent;
    row["vote"]  = vote;
    return row.dump();
}

std::string summary_line(const nlohmann::ordered_json& regime)
{
    std::string line = "[국면] " + regime.value("regime", std::string()) +
                       std::format(" score={:+d}", regime.value("risk_score", 0)) +
                       " scale=" + regime["entry_scale"].dump() +
                       " halt=" + (regime.value("entry_halt", false) ? "true" : "false") +
                       " liq=" + (regime.value("force_liquidate", false) ? "true" : "false") + " |";

    for (auto iterator = regime["components"].begin(); iterator != regime["components"].end(); ++iterator)
    {
        const auto& component = iterator.value();

        if (!component["pct"].is_number())
        {
            line += " " + iterator.key() + "=NA";
            continue;
        }

        line += " " + iterator.key() + std::format("={:.3f}%({:+d})", component["pct"].get<double>(),
                                                   component.value("vote", 0));
        const auto intra = component.find("intra");

        if (intra != component.end())
        {
            line += std::format("[장초{:+.2f}%({:+d})]", intra->value("pct", 0.0), intra->value("vote", 0));
        }
    }

    return line + " | " + regime["assessment"].value("summary", std::string());
}

} // namespace regime_feed
