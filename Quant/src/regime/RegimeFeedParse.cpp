// 국면 판정 피드의 응답 읽기 — 네이버 실시간 지수·Yahoo 차트·FRED CSV 본문을 지표 등락으로 바꾼다. 네트워크·파일 없음.
//  선언은 regime/RegimeFeed.h. 스레드: 국면 피드 스레드(RegimeFeed::fetch_changes)와 테스트가 부른다. 상태 없음. [why D-147]
#include "regime/RegimeFeed.h"

#include "detail/RegimeTables.h"

#include "core/KstTime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace regime_feed
{
namespace detail
{

const nlohmann::json* child(const nlohmann::json& node, const char* key)
{
    if (!node.is_object())
    {
        return nullptr;
    }

    const auto found = node.find(key);
    return found == node.end() ? nullptr : &*found;
}

} // namespace detail

using detail::child;

namespace
{

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

} // namespace

std::map<std::string, Change> parse_naver_index(std::string_view body, std::time_t now_utc)
{
    std::map<std::string, Change> indexes;
    const std::string             today = kst::datetime(now_utc).substr(0, 10);
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

        const auto status      = child(row, "marketStatus");
        const auto traded_at   = child(row, "localTradedAt");
        const bool open        = status != nullptr && status->is_string() && status->get<std::string>() == "OPEN";
        // 마감(CLOSE)은 개장 전과 장 마감 뒤에 같이 온다. 마감 뒤의 등락은 오늘 것인데 0으로 지워 애프터마켓
        //  내내 코스피·코스닥 표가 빠졌다(10-01 15:47~ 0.000%). 마지막 거래 날짜로 둘을 가른다.
        const bool traded_today = traded_at != nullptr && traded_at->is_string() &&
                                  traded_at->get<std::string>().starts_with(today);
        Change     change;
        change.price  = price;
        change.source = "naver";

        if (open || traded_today)
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

} // namespace regime_feed
