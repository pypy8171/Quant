// zone_check — 네이버 일봉으로 엔진과 같은 순서를 재현해 보는 단독 실행 파일.
//
// 흐름은 엔진과 같다: 일봉 수집 → 정배열 프리필터 → 횡단면 z-score 점수 →
// 순위·비중배수 → 존 판정. 계좌에 접속하지 않고 발주도 하지 않는다.
//
// 원본 대응:
//   정배열·피처   Quant/src/universe/UniverseFeatures.cpp lookup_and_filter
//   횡단면 점수   Quant/src/universe/UniverseScoring.cpp score_cross_section
//   비중 배수     Quant/include/universe/ScoreWeight.h
//   존 판정       Quant/src/strategy/DeviationScaleStrategy.cpp judge_zone

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{

struct Bar
{
    std::string date;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0, volume = 0.0;
};

struct SymbolFeature
{
    std::string ticker;
    std::string name;
    double price = 0.0;
    double average_5 = 0.0, average_10 = 0.0, average_20 = 0.0;
    double trend = 0.0;       // (average_5 - average_20) / average_20
    double pullback = 0.0;    // (price - average_20) / average_20  — 부호 유지
    double volatility = 0.0;  // ATR 대용: 20일 (고-저)/종가 평균, %
    double turnover = 0.0;    // log(거래대금)
    double score = 0.0;
    double multiplier = 0.0;
    bool aligned = false;
    int bars = 0;
};

// 슬리브 파라미터. config_dev_paper.json 값에 맞춰 둔다.
struct Sleeve
{
    const char* id;
    double entry_upper_percent;      // 존 상단
    double pullback_percent;         // 존 하단 깊이(SMA20 아래 %)
    double zone_hysteresis_percent;  // 히스테리시스
    double max_deviation_ratio;      // 프리필터 상한(비율)
    double base_ratio;               // 1회차 명목 비중
    double target_total_ratio;       // 슬리브 총 명목 목표
    int    slots;                    // 슬리브 슬롯 수
    double liquidity_weight;         // 거래대금 가중
};

const Sleeve kDevscale{"DEVSCALE", 5.0, 8.0, 4.0, 0.09, 0.05, 0.80, 17, 0.0};

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }

    const int text_length = static_cast<int>(text.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), text_length, nullptr, 0);
    std::wstring wide(wide_length, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), text_length, &wide[0], wide_length);
    return wide;
}

bool http_get(const std::string& host, const std::string& path, std::string& out)
{
    out.clear();
    HINTERNET session = WinHttpOpen(L"zone_check/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session)
    {
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (!connection)
    {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", widen(path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    bool succeeded = false;

    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD available = 0;

        while (WinHttpQueryDataAvailable(request, &available) && available > 0)
        {
            std::string buffer(available, ' ');
            DWORD bytes_read = 0;
            WinHttpReadData(request, &buffer[0], available, &bytes_read);
            out.append(buffer.data(), bytes_read);
        }

        succeeded = !out.empty();
    }

    if (request)
    {
        WinHttpCloseHandle(request);
    }

    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return succeeded;
}

// 네이버 siseJson 응답은 JSON이 아니라 홑따옴표가 섞인 배열 문자열이다.
//  숫자 행만 골라 읽는다. 행 형태: ["20260909", 시가, 고가, 저가, 종가, 거래량, 외국인소진율]
std::vector<Bar> parse_sise(const std::string& body)
{
    std::vector<Bar> bars;
    size_t row_start = 0;

    while ((row_start = body.find("[\"", row_start)) != std::string::npos)
    {
        const size_t row_end = body.find(']', row_start);

        if (row_end == std::string::npos)
        {
            break;
        }

        const std::string row = body.substr(row_start + 1, row_end - row_start - 1);
        std::vector<std::string> cells;
        size_t cell_start = 0;

        while (cell_start <= row.size())
        {
            const size_t comma = row.find(',', cell_start);
            const std::string cell = row.substr(cell_start, comma == std::string::npos ? std::string::npos : comma - cell_start);
            const size_t first = cell.find_first_not_of(" \t\"");
            const size_t last = cell.find_last_not_of(" \t\"");
            cells.push_back(first == std::string::npos ? std::string() : cell.substr(first, last - first + 1));

            if (comma == std::string::npos)
            {
                break;
            }

            cell_start = comma + 1;
        }

        if (cells.size() >= 6 && cells[0].size() == 8 && cells[0][0] == '2')
        {
            Bar bar;
            bar.date   = cells[0];
            bar.open   = atof(cells[1].c_str());
            bar.high   = atof(cells[2].c_str());
            bar.low    = atof(cells[3].c_str());
            bar.close  = atof(cells[4].c_str());
            bar.volume = atof(cells[5].c_str());

            if (bar.close > 0.0)
            {
                bars.push_back(bar);
            }
        }

        row_start = row_end + 1;
    }

    // 네이버는 과거→최근 순으로 준다. 엔진은 [0]이 최신이라 뒤집는다.
    std::reverse(bars.begin(), bars.end());
    return bars;
}

double average_close(const std::vector<Bar>& bars, int count)
{
    if (count <= 0 || static_cast<int>(bars.size()) < count)
    {
        return 0.0;
    }

    double sum = 0.0;

    for (int index = 0; index < count; ++index)
    {
        sum += bars[index].close;
    }

    return sum / count;
}

std::string ymd_offset(int days_back)
{
    SYSTEMTIME system_time;
    GetLocalTime(&system_time);
    FILETIME file_time;
    SystemTimeToFileTime(&system_time, &file_time);
    ULARGE_INTEGER ticks;
    ticks.LowPart  = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    ticks.QuadPart -= static_cast<ULONGLONG>(days_back) * 24ULL * 3600ULL * 10000000ULL;
    file_time.dwLowDateTime  = ticks.LowPart;
    file_time.dwHighDateTime = ticks.HighPart;
    FileTimeToSystemTime(&file_time, &system_time);
    char text[16];
    sprintf_s(text, "%04d%02d%02d", system_time.wYear, system_time.wMonth, system_time.wDay);
    return text;
}

// 횡단면 z-score. 표준편차가 사실상 0이면 전부 0(동일가중 폴백). 원본과 같이 ±2로 자른다.
void zscore(const std::vector<SymbolFeature>& features, double SymbolFeature::*member, bool invert,
            std::vector<double>& scores)
{
    const size_t count = features.size();
    scores.assign(count, 0.0);

    if (count < 2)
    {
        return;
    }

    double mean = 0.0;

    for (const auto& feature : features)
    {
        mean += feature.*member;
    }

    mean /= static_cast<double>(count);
    double variance = 0.0;

    for (const auto& feature : features)
    {
        const double deviation = feature.*member - mean;
        variance += deviation * deviation;
    }

    variance /= static_cast<double>(count);
    const double standard_deviation = std::sqrt(variance);

    if (!(standard_deviation > 1e-12))
    {
        return;
    }

    for (size_t index = 0; index < count; ++index)
    {
        double value = (features[index].*member - mean) / standard_deviation;
        value = (std::max)(-2.0, (std::min)(2.0, value));
        scores[index] = invert ? -value : value;
    }
}

void run_sleeve(const Sleeve& sleeve, const std::vector<SymbolFeature>& all_features)
{
    printf("\n================ %s ================\n", sleeve.id);
    printf("진입밴드 %.1f%% ~ %.1f%%  히스테리시스 %.1f%%  슬롯 %d  base %.1f%%  총목표 %.0f%%\n",
           -sleeve.pullback_percent,
           sleeve.entry_upper_percent, sleeve.zone_hysteresis_percent, sleeve.slots,
           sleeve.base_ratio * 100.0, sleeve.target_total_ratio * 100.0);

    std::vector<SymbolFeature> passed;
    int cut_aligned = 0, cut_extended = 0, cut_bars = 0;

    for (const auto& feature : all_features)
    {
        if (feature.bars < 20)
        {
            ++cut_bars;
            continue;
        }

        if (!feature.aligned)
        {
            ++cut_aligned;
            continue;
        }

        if (sleeve.max_deviation_ratio > 0.0 && feature.pullback > sleeve.max_deviation_ratio)
        {
            ++cut_extended;
            continue;
        }

        passed.push_back(feature);
    }

    printf("프리필터: 입력=%d 역배열컷=%d 데이터부족=%d 과확장컷=%d 통과=%d\n",
           static_cast<int>(all_features.size()), cut_aligned, cut_bars, cut_extended,
           static_cast<int>(passed.size()));

    if (passed.empty())
    {
        return;
    }

    // S = 1.0*z(trend) + 1.0*z(-pullback) - 0.0*z(volatility) + liquidity_weight*z(log 거래대금)
    std::vector<double> z_trend, z_pullback, z_volatility, z_liquidity;
    zscore(passed, &SymbolFeature::trend, false, z_trend);
    zscore(passed, &SymbolFeature::pullback, true, z_pullback);
    zscore(passed, &SymbolFeature::volatility, false, z_volatility);

    if (sleeve.liquidity_weight != 0.0)
    {
        zscore(passed, &SymbolFeature::turnover, false, z_liquidity);
    }
    else
    {
        z_liquidity.assign(passed.size(), 0.0);
    }

    for (size_t index = 0; index < passed.size(); ++index)
    {
        passed[index].score = 1.0 * z_trend[index] + 1.0 * z_pullback[index] - 0.0 * z_volatility[index] +
                              sleeve.liquidity_weight * z_liquidity[index];
    }

    std::sort(passed.begin(), passed.end(),
              [](const SymbolFeature& left, const SymbolFeature& right)
              {
                  return left.score > right.score;
              });

    // 비중 배수(ScoreWeight.h):
    //  z = clamp((S-mean)/sd, +-2), raw = 1 + 0.6*z/2, scale = target / (base * sum(상위 raw))
    double mean = 0.0;

    for (const auto& feature : passed)
    {
        mean += feature.score;
    }

    mean /= static_cast<double>(passed.size());
    double variance = 0.0;

    for (const auto& feature : passed)
    {
        variance += (feature.score - mean) * (feature.score - mean);
    }

    const double standard_deviation = std::sqrt(variance / static_cast<double>(passed.size()));
    std::vector<double> raw_weights(passed.size(), 1.0);

    for (size_t index = 0; index < passed.size(); ++index)
    {
        double z_value = standard_deviation > 1e-12 ? (passed[index].score - mean) / standard_deviation : 0.0;
        z_value = (std::max)(-2.0, (std::min)(2.0, z_value));
        raw_weights[index] = 1.0 + 0.6 * z_value / 2.0;
    }

    const int passed_count = static_cast<int>(passed.size());
    const int take_count = (std::min)(passed_count, sleeve.slots);
    double sum_top = 0.0;

    for (int index = 0; index < take_count; ++index)
    {
        sum_top += raw_weights[index];
    }

    const double scale = sum_top > 0.0 ? sleeve.target_total_ratio / (sleeve.base_ratio * sum_top) : 1.0;

    for (size_t index = 0; index < passed.size(); ++index)
    {
        passed[index].multiplier = raw_weights[index] * scale;
    }

    // 존 판정. 신규 진입 시점(in_zone_=false) 기준이라 히스테리시스는 붙지 않는다.
    const double upper_threshold = sleeve.entry_upper_percent;
    const double lower_threshold = -sleeve.pullback_percent;

    printf("%-6s %-8s %-16s %10s %10s %8s %8s %6s %6s\n",
           "순위", "종목", "이름", "현재가", "SMA20", "이격%", "점수", "배수", "존");
    printf("-------------------------------------------------------------------------------------\n");

    for (int index = 0; index < passed_count; ++index)
    {
        const SymbolFeature& feature = passed[index];
        const double deviation_percent = feature.average_20 > 0.0
                                             ? (feature.price - feature.average_20) / feature.average_20 * 100.0
                                             : 999.0;
        const bool zone = feature.aligned && feature.average_20 > 0.0 && deviation_percent <= upper_threshold &&
                          deviation_percent >= lower_threshold;
        printf("%2d/%-3d %-8s %-16s %10.0f %10.1f %8.2f %8.3f %6.2f %6s%s\n",
               index + 1, passed_count, feature.ticker.c_str(), feature.name.c_str(),
               feature.price, feature.average_20, deviation_percent, feature.score, feature.multiplier,
               zone ? "활성" : "대기", index < take_count ? "" : "  (슬롯밖)");
    }

    printf("\n존 활성이어도 발주까지는 OrderGate를 더 지난다 — 점수 우선순위 기준선,\n");
    printf("명목 한도, 슬롯 수, 중복 방지. 그래서 존 활성이 곧 매수는 아니다.\n");
}

struct Target
{
    const char* code;
    const char* name;
};

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    // 기본 종목. 인자를 주면 그대로 대체한다(코드만 주면 이름 자리에 코드를 표시).
    std::vector<Target> targets = {
        {"005930", "삼성전자"},     {"000660", "SK하이닉스"},   {"047050", "포스코인터"},
        {"036930", "주성엔지니어링"}, {"108490", "로보티즈"},     {"010950", "S-Oil"},
        {"033790", "스카이라이프"},  {"267270", "HD현대건설기계"}, {"003490", "대한항공"},
        {"066570", "LG전자"},       {"055550", "신한지주"},     {"086790", "하나금융지주"},
        {"030200", "KT"},           {"034020", "두산에너빌리티"}, {"003550", "LG"},
    };

    std::vector<Target> argument_targets;

    for (int index = 1; index < argc; ++index)
    {
        argument_targets.push_back({argv[index], argv[index]});
    }

    if (!argument_targets.empty())
    {
        targets = argument_targets;
    }

    const std::string start = ymd_offset(400);
    const std::string end   = ymd_offset(0);
    printf("네이버 일봉 수집: %s ~ %s, %d종목\n", start.c_str(), end.c_str(), static_cast<int>(targets.size()));

    std::vector<SymbolFeature> all_features;

    for (const auto& target : targets)
    {
        const std::string path = "/siseJson.naver?symbol=" + std::string(target.code) +
                                 "&requestType=1&startTime=" + start +
                                 "&endTime=" + end + "&timeframe=day";
        std::string body;

        if (!http_get("api.finance.naver.com", path, body))
        {
            printf("  %s 수집 실패\n", target.code);
            continue;
        }

        const std::vector<Bar> bars = parse_sise(body);
        const int bar_count = static_cast<int>(bars.size());

        if (bar_count < 20)
        {
            printf("  %s 일봉 %d개 — 20개 미만이라 제외\n", target.code, bar_count);
            continue;
        }

        SymbolFeature feature;
        feature.ticker     = target.code;
        feature.name       = target.name;
        feature.bars       = bar_count;
        feature.price      = bars[0].close;
        feature.average_5  = average_close(bars, 5);
        feature.average_10 = average_close(bars, 10);
        feature.average_20 = average_close(bars, 20);
        feature.aligned    = feature.average_5 > feature.average_10 && feature.average_10 > feature.average_20;
        feature.trend      = feature.average_20 > 0.0
                                 ? (feature.average_5 - feature.average_20) / feature.average_20
                                 : 0.0;
        feature.pullback   = feature.average_20 > 0.0
                                 ? (feature.price - feature.average_20) / feature.average_20
                                 : 0.0;

        double range_sum = 0.0;
        int range_count = 0;

        for (int index = 0; index < 20 && index < bar_count; ++index)
        {
            if (bars[index].close > 0.0)
            {
                range_sum += (bars[index].high - bars[index].low) / bars[index].close;
                ++range_count;
            }
        }

        feature.volatility = range_count > 0 ? range_sum / range_count * 100.0 : 0.0;
        const double turnover = bars[0].close * bars[0].volume;
        feature.turnover = turnover > 0.0 ? std::log(turnover) : 0.0;

        printf("  %s %-16s 일봉 %3d개  종가 %9.0f  SMA20 %9.1f  이격 %+7.2f%%  정배열 %s\n",
               feature.ticker.c_str(), feature.name.c_str(), feature.bars, feature.price, feature.average_20,
               feature.pullback * 100.0, feature.aligned ? "Y" : "N");
        all_features.push_back(feature);
        Sleep(120);  // 네이버 호출 간격
    }

    if (all_features.empty())
    {
        printf("수집된 종목이 없다. 네트워크나 종목코드를 확인한다.\n");
        return 1;
    }

    run_sleeve(kDevscale, all_features);
    return 0;
}
