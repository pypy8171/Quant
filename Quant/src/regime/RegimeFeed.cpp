// 국면 판정 피드 구현 — 지표 받기·점수·파일 쓰기. 왜 있는지는 regime/RegimeFeed.h 머리말.
//  판정식·임계·문구는 PYQuant/tools/macro_regime_feed.py(09-26 기준)를 그대로 옮겼다. 임계는 전부 검증 전
//  잠정값이다(D-033·D-083) — 바꿀 때는 파일 문서의 thresholds와 대시보드 표시가 같이 바뀐다.
#include "regime/RegimeFeed.h"

#include "api/HttpGet.h"
#include "core/KstTime.h"
#include "utils/AtomicFile.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace regime_feed
{
namespace
{

// 표결 지표 8개. vote_direction은 지표가 오를 때 위험선호(+1)인지 위험회피(−1)인지.
//  코스피·코스닥은 네이버 실시간 지수가 먼저다(Yahoo는 개장 직후 몇 분 전날 값에 멈춘다, 09-15).
//  나스닥·S&P는 선물 현재가에 현물 직전 세션 등락을 더한다 — 선물만 보면 미국 정규장 하루치가 빠진다(09-18).
struct VotingSymbol
{
    std::string_view key;
    std::string_view yahoo;
    std::string_view label;
    int              vote_direction;
    double           warn;
    double           strong;
    std::string_view naver_code;     // 비면 네이버를 안 쓴다
    std::string_view cash_reference; // 비면 현물 보정 없음
};

constexpr std::array<VotingSymbol, 8> kVotingSymbols{{
    {"KOSPI", "^KS11", "코스피", +1, 0.7, 1.5, "KOSPI", ""},
    {"KOSDAQ", "^KQ11", "코스닥", +1, 0.8, 1.8, "KOSDAQ", ""},
    {"NQ_F", "NQ=F", "나스닥 (마감+선물)", +1, 0.4, 0.9, "", "^IXIC"},
    {"ES_F", "ES=F", "S&P500 (마감+선물)", +1, 0.4, 0.9, "", "^GSPC"},
    {"TNX10", "^TNX", "10Y 미국채금리", -1, 1.5, 3.0, "", ""},
    {"VIX", "^VIX", "VIX", -1, 4.0, 9.0, "", ""},
    {"USDKRW", "KRW=X", "USD/KRW", -1, 0.4, 0.9, "", ""},
    {"WTI", "CL=F", "WTI 유가", -1, 2.0, 4.0, "", ""},
}};

// 장초 대비 방향표 — 전일 대비 표는 개장 때 이미 빨간 날 장중에 돌아서는 것을 못 본다. warn은 기준점 대비 %.
struct IntraSymbol
{
    std::string_view key;
    double           warn;
    int              vote_direction;
};

constexpr std::array<IntraSymbol, 2> kIntraSymbols{{{"NQ_F", 0.3, +1}, {"TNX10", 0.6, -1}}};

// 참고 지표 — 표 없음, valid 개수에도 안 들어간다. FRED 일별 시리즈라 1~2일 늦다.
struct InfoSymbol
{
    std::string_view key;
    std::string_view fred_id;
    std::string_view label;
};

constexpr std::array<InfoSymbol, 3> kInfoSymbols{{
    {"TYX30", "DGS30", "30Y 미국채금리 (FRED, 1~2일 지연)"},
    {"DGS2", "DGS2", "2Y 미국채금리 (FRED, 1~2일 지연)"},
    {"HY", "BAMLH0A0HYM2", "미국 하이일드 스프레드 (FRED, 1~2일 지연)"},
}};

// [formula] 수준 경계. 게이트가 아니라 설명용 note다. 근거는 통상 인용되는 구간이지 이 시스템에서 검증한 값이 아니다.
struct Band
{
    double           low;
    std::string_view text;
};

struct LevelTable
{
    std::string_view  key;
    std::vector<Band> bands;
};

const std::vector<LevelTable>& level_tables()
{
    static const std::vector<LevelTable> tables{
        {"TYX30", {{5.0, "5% 위. 장기 할인율 부담이 큰 구간"}, {4.5, "4.5~5%. 고점권"}}},
        {"TNX10", {{4.5, "4.5% 위. 고점권"}, {4.0, "4~4.5%"}}},
        {"VIX", {{30.0, "30 위. 공포 구간"}, {20.0, "20~30. 경계 구간"}, {15.0, "15~20. 보통"}}},
        {"USDKRW", {{1400.0, "1400 위. 원화 약세 경계"}, {1350.0, "1350~1400. 약세 구간"}}},
        {"WTI", {{100.0, "100달러 위. 인플레 재점화 우려"}, {90.0, "90~100달러. 부담 구간"}}},
        {"HY",
         {{5.0, "5%p 위. 신용 스트레스 확대"},
          {4.0, "4~5%p. 확대 조짐"},
          {3.0, "3~4%p. 보통"},
          {0.0, "3%p 아래. 신용 시장 평온"}}},
        {"DGS2", {{4.5, "4.5% 위. 긴축 기대 유지"}, {4.0, "4~4.5%"}}},
    };
    return tables;
}

// 종합 경고에 세는 경계(지표당 1개). 개수만 세고 점수에는 더하지 않는다.
struct WarnLevel
{
    std::string_view key;
    double           level;
};

constexpr std::array<WarnLevel, 6> kWarnLevels{
    {{"TYX30", 5.0}, {"TNX10", 4.5}, {"VIX", 20.0}, {"USDKRW", 1400.0}, {"WTI", 90.0}, {"HY", 4.0}}};

constexpr int kStaleAfterSec      = 600;  // 문서에 적는 값. 엔진은 자기 regime_stale_sec(파일 수정 시각)으로 본다
constexpr int kInfoRefreshSec     = 1800; // FRED는 하루 한 번 바뀌어 30분에 한 번만 받는다
constexpr int kFredLookbackDays   = 15;   // 휴일이 이어져도 값 두 개가 남는 폭
constexpr int kSecondsPerDay      = 86400;
constexpr int kStopPollMs         = 200;  // 멈추라는 신호를 기다리는 한 번의 잠
constexpr int kOwnerGraceSec      = 30;   // 다른 엔진의 사이클이 조금 늦어도 넘겨받지 않는 여유
constexpr int kOpenReferenceHour  = 9;

constexpr std::string_view kNaverIndexUrl = "https://polling.finance.naver.com/api/realtime/domestic/index/KOSPI,KOSDAQ";

const std::vector<std::string>& naver_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0", "Referer: https://finance.naver.com/"};
    return headers;
}

// 기본 UA는 Yahoo가 429·403을 준다.
const std::vector<std::string>& browser_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0"};
    return headers;
}

// FRED 앞단은 헤더 조합으로 요청을 거른다 — 걸리면 응답 없이 연결을 붙잡는다(12002 시간 초과). WinHTTP 기본(UA·
//  Connection만)과 브라우저 UA는 걸리고, 파이썬 requests가 보내는 조합(Accept·Accept-Encoding)은 통과한다(09-26 실측).
//  CSV가 작아 압축하지 않고 보낸다.
const std::vector<std::string>& fred_headers()
{
    static const std::vector<std::string> headers{"Accept: */*", "Accept-Encoding: gzip, deflate"};
    return headers;
}

// 숫자로 오든 숫자 문자열로 오든 읽는다. 없거나 못 읽으면 비움.
std::optional<double> number_field(const nlohmann::json& node, const char* key)
{
    if (!node.is_object())
    {
        return std::nullopt;
    }

    const auto found = node.find(key);

    if (found == node.end())
    {
        return std::nullopt;
    }

    if (found->is_number())
    {
        return found->get<double>();
    }

    if (!found->is_string())
    {
        return std::nullopt;
    }

    const std::string& text = found->get_ref<const std::string&>();
    char*              end  = nullptr;
    const double       value = std::strtod(text.c_str(), &end);

    if (end == text.c_str() || !std::isfinite(value))
    {
        return std::nullopt;
    }

    return value;
}

const nlohmann::json* child(const nlohmann::json& node, const char* key)
{
    if (!node.is_object())
    {
        return nullptr;
    }

    const auto found = node.find(key);
    return found == node.end() ? nullptr : &*found;
}

// chart.result[0]. 없으면 nullptr.
const nlohmann::json* chart_result(const nlohmann::json& document)
{
    const nlohmann::json* chart = child(document, "chart");
    const nlohmann::json* results = chart ? child(*chart, "result") : nullptr;

    if (results == nullptr || !results->is_array() || results->empty())
    {
        return nullptr;
    }

    return &(*results)[0];
}

// meta.currentTradingPeriod.regular. 없으면 nullptr.
const nlohmann::json* regular_period(const nlohmann::json& result)
{
    const nlohmann::json* meta   = child(result, "meta");
    const nlohmann::json* period = meta ? child(*meta, "currentTradingPeriod") : nullptr;
    return period ? child(*period, "regular") : nullptr;
}

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

// 차트 주소에 넣는 기호(^·=)는 퍼센트로 바꾼다.
std::string encode_symbol(std::string_view symbol)
{
    std::string encoded;

    for (const char character : symbol)
    {
        if (character == '^')
        {
            encoded += "%5E";
        }
        else if (character == '=')
        {
            encoded += "%3D";
        }
        else
        {
            encoded += character;
        }
    }

    return encoded;
}

// 수준 note와 종합 한 줄. 게이트(score)는 건드리지 않는다. 각 component에 note를 채운다.
nlohmann::ordered_json assess_levels(nlohmann::ordered_json& components, int score)
{
    nlohmann::ordered_json flags = nlohmann::ordered_json::array();

    auto price_of = [&components](std::string_view key) -> std::optional<double>
    {
        const auto found = components.find(std::string(key));

        if (found == components.end())
        {
            return std::nullopt;
        }

        const auto price = found->find("price");
        return (price != found->end() && price->is_number()) ? std::optional<double>(price->get<double>())
                                                               : std::nullopt;
    };

    auto append_note = [](nlohmann::ordered_json& component, const std::string& addition)
    {
        const auto        found    = component.find("note");
        const std::string existing = (found != component.end() && found->is_string()) ? found->get<std::string>() : "";
        component["note"]          = existing.empty() ? addition : existing + " · " + addition;
    };

    for (const LevelTable& table : level_tables())
    {
        const std::optional<double> price = price_of(table.key);

        if (!price)
        {
            continue;
        }

        nlohmann::ordered_json& component = components[std::string(table.key)];
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
        const bool is_rate = table.key == "TYX30" || table.key == "TNX10" || table.key == "DGS2";
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

    const std::optional<double> rate30 = price_of("TYX30");
    const std::optional<double> rate10 = price_of("TNX10");
    const std::optional<double> rate2  = price_of("DGS2");

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

        append_note(components["TYX30"], addition);
    }

    if (rate10 && rate2)
    {
        const double spread = *rate10 - *rate2;
        append_note(components["DGS2"],
                    std::format("10Y-2Y {:+.0f}bp", spread * 100.0) + (spread < 0.0 ? " (역전)" : ""));
    }

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

    nlohmann::ordered_json assessment;
    assessment["flags"]   = flags;
    assessment["summary"] = summary;
    return assessment;
}

void append_line(const std::string& path, const std::string& line)
{
    std::error_code error;
    const auto      parent = std::filesystem::path(path).parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
    }

    std::ofstream file(path, std::ios::binary | std::ios::app);

    if (file)
    {
        file << line << '\n';
    }
}

std::string read_whole(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_with_parent(const std::string& path, const std::string& text)
{
    std::error_code error;
    const auto      parent = std::filesystem::path(path).parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
    }

    return file_io::write_atomically(path, text);
}

} // namespace

std::map<std::string, Change> parse_naver_index(std::string_view body)
{
    std::map<std::string, Change> indexes;
    const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);
    const nlohmann::json* datas   = child(document, "datas");

    if (datas == nullptr || !datas->is_array())
    {
        return indexes;
    }

    for (const auto& row : *datas)
    {
        const auto                  code  = child(row, "itemCode");
        const std::optional<double> percent   = number_field(row, "fluctuationsRatioRaw");
        const std::optional<double> price = number_field(row, "closePriceRaw");

        if (code == nullptr || !code->is_string() || !percent || !price)
        {
            continue;
        }

        const auto status = child(row, "marketStatus");
        const bool open   = status != nullptr && status->is_string() && status->get<std::string>() == "OPEN";
        Change     change;
        change.price  = price;
        change.source = "naver";

        if (open)
        {
            change.percent = percent;
        }
        else
        {
            change.percent          = 0.0; // 개장 전 — 오늘은 아직 안 움직였다. 받은 값은 어제 등락이다
            change.previous_percent = percent;
            change.premarket    = true;
        }

        indexes[code->get<std::string>()] = change;
    }

    return indexes;
}

Change parse_yahoo_chart(std::string_view body, std::time_t now_utc)
{
    Change change;
    change.source = "yahoo";
    const nlohmann::json  document = nlohmann::json::parse(body, nullptr, false);
    const nlohmann::json* result   = chart_result(document);
    const nlohmann::json* meta     = result ? child(*result, "meta") : nullptr;

    if (meta == nullptr)
    {
        change.error = "yahoo:parse";
        return change;
    }

    const std::optional<double> last     = number_field(*meta, "regularMarketPrice");
    std::optional<double>       previous = number_field(*meta, "previousClose");

    if (!previous || *previous == 0.0)
    {
        previous = number_field(*meta, "chartPreviousClose");
    }

    if (!last || !previous || *previous == 0.0)
    {
        change.error = "yahoo_no_meta";
        return change;
    }

    const double                raw_percent = (*last - *previous) / *previous * 100.0;
    const nlohmann::json*       regular = regular_period(*result);
    const std::optional<double> start   = regular ? number_field(*regular, "start") : std::nullopt;
    change.price                        = last;

    // 하루 몇 시간만 여는 시장은 정규장이 아직 안 열렸으면 어제 하루치 등락에 멈춰 있다(09-15 08:34 코스피 사고).
    if (start && static_cast<double>(now_utc) < *start)
    {
        change.percent          = 0.0;
        change.previous_percent = raw_percent;
        change.premarket    = true;
        return change;
    }

    change.percent = raw_percent;
    return change;
}

std::optional<double> parse_last_session_percent(std::string_view body, std::time_t now_utc)
{
    const nlohmann::json  document = nlohmann::json::parse(body, nullptr, false);
    const nlohmann::json* result   = chart_result(document);
    const nlohmann::json* regular  = result ? regular_period(*result) : nullptr;

    if (regular == nullptr)
    {
        return std::nullopt;
    }

    const double session_length = number_field(*regular, "end").value_or(0.0) - number_field(*regular, "start").value_or(0.0);

    if (session_length <= 0.0)
    {
        return std::nullopt;
    }

    const nlohmann::json* timestamps = child(*result, "timestamp");
    const nlohmann::json* indicators = child(*result, "indicators");
    const nlohmann::json* quotes     = indicators ? child(*indicators, "quote") : nullptr;

    if (timestamps == nullptr || !timestamps->is_array() || quotes == nullptr || !quotes->is_array() || quotes->empty())
    {
        return std::nullopt;
    }

    const nlohmann::json* closes = child((*quotes)[0], "close");

    if (closes == nullptr || !closes->is_array())
    {
        return std::nullopt;
    }

    // 일봉 타임스탬프는 그 세션의 정규장 시작이라, 시작+정규장 길이가 지금보다 앞이면 완결된 봉이다.
    std::vector<double> completed;
    const std::size_t   count = std::min(timestamps->size(), closes->size());

    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& stamp = (*timestamps)[index];
        const auto& close = (*closes)[index];

        if (stamp.is_number() && close.is_number() &&
            stamp.get<double>() + session_length <= static_cast<double>(now_utc))
        {
            completed.push_back(close.get<double>());
        }
    }

    if (completed.size() < 2 || completed[completed.size() - 2] == 0.0)
    {
        return std::nullopt;
    }

    const double last     = completed.back();
    const double previous = completed[completed.size() - 2];
    return (last - previous) / previous * 100.0;
}

Change parse_fred_csv(std::string_view body)
{
    Change change;
    change.source = "fred";
    std::vector<double> values;
    std::size_t         line_start = 0;

    while (line_start < body.size())
    {
        std::size_t line_end = body.find('\n', line_start);

        if (line_end == std::string_view::npos)
        {
            line_end = body.size();
        }

        const std::string_view line  = body.substr(line_start, line_end - line_start);
        const std::size_t      comma = line.find(',');
        line_start                   = line_end + 1;

        if (comma == std::string_view::npos)
        {
            continue;
        }

        const std::string text(line.substr(comma + 1));
        char*             end   = nullptr;
        const double      value = std::strtod(text.c_str(), &end);

        if (end != text.c_str() && std::isfinite(value)) // 머리줄·휴일(".")은 여기서 걸러진다
        {
            values.push_back(value);
        }
    }

    if (values.size() < 2)
    {
        change.error = "no_data";
        return change;
    }

    const double last     = values.back();
    const double previous = values[values.size() - 2];

    if (previous == 0.0)
    {
        change.error = "zero_prev";
        return change;
    }

    change.price = last;
    change.percent   = (last - previous) / previous * 100.0;
    return change;
}

int vote_for(std::string_view key, double percent)
{
    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        if (symbol.key != key)
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

        for (auto iterator = prices->begin(); iterator != prices->end(); ++iterator)
        {
            if (iterator.value().is_number())
            {
                reference.prices[iterator.key()] = iterator.value().get<double>();
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
        const auto found = changes.find(std::string(symbol.key));

        if (found != changes.end() && found->second.price)
        {
            reference.prices[std::string(symbol.key)] = *found->second.price;
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

nlohmann::ordered_json build_regime(const Changes& changes, const OpenReference& open_reference,
                                    const Thresholds& thresholds, std::time_t now_utc)
{
    nlohmann::ordered_json components = nlohmann::ordered_json::object();
    int                    score       = 0;
    int                    valid_count = 0;

    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        const std::string key(symbol.key);
        const auto        found = changes.find(key);
        const Change      empty;
        const Change&     change = found == changes.end() ? empty : found->second;
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
        score += vote;
        ++valid_count;
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

    // 장초 대비 방향표. 기준점이 없으면(개장 전·첫 계산) 0표.
    for (const IntraSymbol& symbol : kIntraSymbols)
    {
        const std::string key(symbol.key);
        const auto        base = open_reference.prices.find(key);

        if (base == open_reference.prices.end() || base->second == 0.0)
        {
            continue;
        }

        nlohmann::ordered_json& component = components[key];
        const auto              price     = component.find("price");

        if (price == component.end() || !price->is_number())
        {
            continue;
        }

        const double intra_percent  = (price->get<double>() - base->second) / base->second * 100.0;
        int          intra_vote = 0;

        if (std::abs(intra_percent) >= symbol.warn)
        {
            intra_vote = symbol.vote_direction * (intra_percent > 0.0 ? 1 : -1);
        }

        score += intra_vote;
        nlohmann::ordered_json intra;
        intra["pct"]       = round3(intra_percent);
        intra["vote"]      = intra_vote;
        intra["base"]      = base->second;
        component["intra"] = intra;
    }

    for (const InfoSymbol& symbol : kInfoSymbols)
    {
        const std::string key(symbol.key);
        const auto        found = changes.find(key);
        const Change      empty;
        const Change&     change = found == changes.end() ? empty : found->second;
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

    nlohmann::ordered_json assessment = assess_levels(components, score);

    // 유효 지표가 절반 미만이면 판정 보류 — 데이터 공백에 게이트가 움직이지 않게.
    const int  needed          = std::max(1, (static_cast<int>(kVotingSymbols.size()) + 1) / 2);
    const bool valid           = valid_count >= needed;
    const bool entry_halt      = valid && score <= thresholds.halt_score;
    const bool force_liquidate = valid && score <= thresholds.liquidate_score;
    std::string label;

    if (!valid)
    {
        label = "UNKNOWN";
    }
    else if (score <= thresholds.halt_score)
    {
        label = "RISK_OFF";
    }
    else if (score >= thresholds.on_score)
    {
        label = "RISK_ON";
    }
    else
    {
        label = "NEUTRAL";
    }

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

// ── 스레드 ──

RegimeFeed::~RegimeFeed()
{
    stop();
}

void RegimeFeed::start(const FeedConfig& config)
{
    if (config.out_path.empty() || running_.exchange(true))
    {
        return;
    }

    config_ = config;
    worker_ = std::thread([this]
    {
        run();
    });
}

void RegimeFeed::stop()
{
    running_.store(false, std::memory_order_release);

    if (worker_.joinable())
    {
        worker_.join();
    }
}

void RegimeFeed::run_once(const FeedConfig& config)
{
    config_ = config;
    cycle();
}

void RegimeFeed::run()
{
    thread_name::set_current("RegimeFeed");
    LOG_INFO("[국면] 국면 판정 피드 시작 — 출력 " + config_.out_path + ", 주기 " + std::to_string(config_.interval_sec) +
             "초, 정지 " + std::to_string(config_.thresholds.halt_score) + "·청산 " +
             std::to_string(config_.thresholds.liquidate_score));

    while (running_.load(std::memory_order_acquire))
    {
        if (!another_writer_owns_file())
        {
            try
            {
                cycle();
            }
            catch (const std::exception& exception)
            {
                LOG_WARN(std::string("[국면] 사이클 실패 — 다음 주기에 다시: ") + exception.what());
            }
        }

        // 멈추라는 신호를 곧 받게 0.2초씩 나눠 잔다.
        const auto wake_at = std::chrono::steady_clock::now() + std::chrono::seconds(config_.interval_sec);

        while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < wake_at)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStopPollMs));
        }
    }
}

// 같은 출력 파일을 다른 엔진(모의·실계좌)이나 옛 파이썬 피드가 쓰고 있으면 쉰다. 파일이 주기+여유보다 오래
//  안 바뀌면 그쪽이 멈춘 것이라 넘겨받는다.
bool RegimeFeed::another_writer_owns_file()
{
    std::error_code error;
    const auto      written = std::filesystem::last_write_time(config_.out_path, error);

    if (error || (last_written_ && *last_written_ == written))
    {
        return false;
    }

    const auto age = std::filesystem::file_time_type::clock::now() - written;

    if (age < std::chrono::seconds(config_.interval_sec + kOwnerGraceSec))
    {
        if (!resting_logged_)
        {
            LOG_INFO("[국면] " + config_.out_path + "를 다른 프로세스가 쓰고 있어 이 엔진은 쉰다(멈추면 넘겨받는다)");
            resting_logged_ = true;
        }

        return true;
    }

    if (resting_logged_)
    {
        LOG_INFO("[국면] " + config_.out_path + "가 " + std::to_string(config_.interval_sec + kOwnerGraceSec) +
                 "초 넘게 안 바뀌어 이 엔진이 넘겨받는다");
        resting_logged_ = false;
    }

    return false;
}

Changes RegimeFeed::fetch_changes()
{
    Changes                             changes;
    const std::time_t                   now     = std::time(nullptr);
    const std::map<std::string, Change> indexes = parse_naver_index(http::get(std::string(kNaverIndexUrl), naver_headers()));

    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        const std::string key(symbol.key);

        if (!symbol.naver_code.empty())
        {
            const auto found = indexes.find(std::string(symbol.naver_code));

            if (found != indexes.end())
            {
                changes[key] = found->second;
                continue;
            }

            LOG_WARN("[국면] " + key + " 네이버 지수 실패 — Yahoo로 받는다");
        }

        const std::string body = http::get("https://query1.finance.yahoo.com/v8/finance/chart/" +
                                               encode_symbol(symbol.yahoo) + "?range=1d&interval=5m",
                                           browser_headers());
        Change change;

        if (body.empty())
        {
            change.source = "yahoo";
            change.error  = "yahoo:no_response";
        }
        else
        {
            change = parse_yahoo_chart(body, now);
        }

        // 현물 직전 세션 + 선물 정산 뒤 변동. 현물을 못 받으면 선물 변동만으로 표를 낸다.
        if (change.percent && !symbol.cash_reference.empty())
        {
            const std::optional<double> cash_percent = parse_last_session_percent(
                http::get("https://query1.finance.yahoo.com/v8/finance/chart/" + encode_symbol(symbol.cash_reference) +
                              "?range=10d&interval=1d",
                          browser_headers()),
                now);
            change.since_settle_percent = change.percent;
            change.previous_percent     = cash_percent;

            if (cash_percent)
            {
                change.percent = *cash_percent + *change.since_settle_percent;
            }
            else
            {
                LOG_WARN("[국면] " + key + " 현물 " + std::string(symbol.cash_reference) +
                         " 직전 세션을 못 받아 선물 변동만 센다");
            }
        }

        if (!change.percent)
        {
            LOG_WARN("[국면] " + key + " 받기 실패(" + change.error + ") — 이번 사이클 표결에서 빠진다");
        }

        changes[key] = change;
    }

    return changes;
}

void RegimeFeed::fetch_info_changes(Changes& changes)
{
    const std::time_t now = std::time(nullptr);

    if (now - info_fetched_at_ >= kInfoRefreshSec)
    {
        const std::string since = kst::datetime(now - static_cast<std::time_t>(kFredLookbackDays) * kSecondsPerDay).substr(0, 10);
        bool              all_ok = true;

        for (const InfoSymbol& symbol : kInfoSymbols)
        {
            const std::string body = http::get("https://fred.stlouisfed.org/graph/fredgraph.csv?id=" +
                                                   std::string(symbol.fred_id) + "&cosd=" + since,
                                               fred_headers());
            Change change = body.empty() ? Change{} : parse_fred_csv(body);

            if (body.empty())
            {
                change.source = "fred";
                change.error  = "fred:no_response";
            }

            all_ok = all_ok && change.percent.has_value();
            info_cache_[std::string(symbol.key)] = change;
        }

        if (all_ok)
        {
            info_fetched_at_ = now; // 하나라도 실패하면 다음 사이클에 다시 받는다
        }
    }

    for (const auto& [key, change] : info_cache_)
    {
        changes[key] = change;
    }
}

void RegimeFeed::cycle()
{
    Changes changes = fetch_changes();
    fetch_info_changes(changes);

    const std::time_t now         = std::time(nullptr);
    bool              should_save = false;
    const OpenReference open_reference =
        choose_open_reference(read_whole(config_.open_reference_path), changes, now, should_save);

    if (should_save && !config_.open_reference_path.empty())
    {
        nlohmann::ordered_json document;
        document["date"]   = open_reference.date;
        document["ts"]     = open_reference.timestamp;
        document["prices"] = open_reference.prices;

        if (!write_with_parent(config_.open_reference_path, document.dump(1)))
        {
            LOG_WARN("[국면] 장초 기준점 저장 실패: " + config_.open_reference_path);
        }
    }

    const nlohmann::ordered_json regime = build_regime(changes, open_reference, config_.thresholds, now);

    if (!write_with_parent(config_.out_path, regime.dump(2)))
    {
        LOG_WARN("[국면] " + config_.out_path + " 쓰기 실패 — 다음 주기에 다시");
        return;
    }

    std::error_code error;
    const auto      written = std::filesystem::last_write_time(config_.out_path, error);

    if (!error)
    {
        last_written_ = written;
    }

    if (!config_.history_path.empty())
    {
        append_line(config_.history_path, history_line(regime, config_.thresholds));
    }

    LOG_INFO(summary_line(regime));
}

} // namespace regime_feed
