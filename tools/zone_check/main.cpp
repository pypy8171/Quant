// zone_check — 네이버 일봉으로 엔진과 같은 순서를 재현해 보는 단독 실행 파일.
//
// 흐름은 엔진과 같다: 일봉 수집 → 정배열 프리필터 → 횡단면 z-score 점수 →
// 순위·비중배수 → 존 판정. 계좌에 접속하지 않고 발주도 하지 않는다.
//
// 원본 대응:
//   정배열·피처   Quant/src/universe/UniverseScanner.cpp L971-1010
//   횡단면 점수   Quant/src/universe/UniverseScanner.cpp L1028-1122
//   비중 배수     Quant/include/universe/ScoreWeight.h
//   존 판정       Quant/include/strategy/DeviationScaleStrategy.h L264-283

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

struct Feat
{
    std::string ticker;
    std::string name;
    double px = 0.0;
    double s5 = 0.0, s10 = 0.0, s20 = 0.0, s60 = 0.0, s120 = 0.0;
    double trend = 0.0;     // (s5 - s60) / s60
    double pull = 0.0;      // (px - s20) / s20  — 부호 유지
    double vol = 0.0;       // ATR 대용: 20일 (고-저)/종가 평균, %
    double turnover = 0.0;  // log(거래대금)
    double score = 0.0;
    double mult = 0.0;
    bool aligned = false;
    int bars = 0;
};

// 슬리브 파라미터. config_dev_paper.json 값에 맞춰 둔다.
struct Sleeve
{
    const char* id;
    double entry_lower_pct;   // 존 하단(>0이면 SMA20 위)
    double entry_upper_pct;   // 존 상단
    double pullback_pct;      // entry_lower_pct<=0일 때의 하단 깊이
    double zone_hyst_pct;     // 히스테리시스
    double min_dev_pct;       // 프리필터 하한(비율)
    double max_dev_pct;       // 프리필터 상한(비율)
    double base_pct;          // 1회차 명목 비중
    double target_total_pct;  // 슬리브 총 명목 목표
    int    slots;             // 슬리브 슬롯 수
    double w_liq;             // 거래대금 가중
};

const Sleeve kDevscale{"DEVSCALE", 0.0, 5.0, 8.0, 4.0, -0.12, 0.09, 0.05, 0.80, 17, 0.0};
const Sleeve kTrendx{"TRENDX", 5.0, 35.0, 0.0, 4.0, 0.01, 0.39, 0.015, 0.10, 25, 0.7};

std::wstring widen(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }

    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool http_get(const std::string& host, const std::string& path, std::string& out)
{
    out.clear();
    HINTERNET ses = WinHttpOpen(L"zone_check/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!ses)
    {
        return false;
    }

    HINTERNET con = WinHttpConnect(ses, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (!con)
    {
        WinHttpCloseHandle(ses);
        return false;
    }

    HINTERNET req = WinHttpOpenRequest(con, L"GET", widen(path).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    bool ok = false;

    if (req &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr))
    {
        DWORD avail = 0;

        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0)
        {
            std::string buf(avail, ' ');
            DWORD got = 0;
            WinHttpReadData(req, &buf[0], avail, &got);
            out.append(buf.data(), got);
        }

        ok = !out.empty();
    }

    if (req)
    {
        WinHttpCloseHandle(req);
    }

    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// 네이버 siseJson 응답은 JSON이 아니라 홑따옴표가 섞인 배열 문자열이다.
//  숫자 행만 골라 읽는다. 행 형태: ["20260909", 시가, 고가, 저가, 종가, 거래량, 외국인소진율]
std::vector<Bar> parse_sise(const std::string& body)
{
    std::vector<Bar> bars;
    size_t i = 0;

    while ((i = body.find("[\"", i)) != std::string::npos)
    {
        size_t j = body.find(']', i);

        if (j == std::string::npos)
        {
            break;
        }

        std::string row = body.substr(i + 1, j - i - 1);
        std::vector<std::string> cell;
        size_t p = 0;

        while (p <= row.size())
        {
            size_t q = row.find(',', p);
            std::string c = row.substr(p, q == std::string::npos ? std::string::npos : q - p);
            size_t a = c.find_first_not_of(" \t\"");
            size_t b = c.find_last_not_of(" \t\"");
            cell.push_back(a == std::string::npos ? std::string() : c.substr(a, b - a + 1));

            if (q == std::string::npos)
            {
                break;
            }

            p = q + 1;
        }

        if (cell.size() >= 6 && cell[0].size() == 8 && cell[0][0] == '2')
        {
            Bar b;
            b.date   = cell[0];
            b.open   = atof(cell[1].c_str());
            b.high   = atof(cell[2].c_str());
            b.low    = atof(cell[3].c_str());
            b.close  = atof(cell[4].c_str());
            b.volume = atof(cell[5].c_str());

            if (b.close > 0.0)
            {
                bars.push_back(b);
            }
        }

        i = j + 1;
    }

    // 네이버는 과거→최근 순으로 준다. 엔진은 [0]이 최신이라 뒤집는다.
    std::reverse(bars.begin(), bars.end());
    return bars;
}

double sma_close(const std::vector<Bar>& d, int n)
{
    if (n <= 0 || (int)d.size() < n)
    {
        return 0.0;
    }

    double s = 0.0;

    for (int k = 0; k < n; ++k)
    {
        s += d[k].close;
    }

    return s / n;
}

std::string ymd_offset(int days_back)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)days_back * 24ULL * 3600ULL * 10000000ULL;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char b[16];
    sprintf_s(b, "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

// 횡단면 z-score. 표준편차가 사실상 0이면 전부 0(동일가중 폴백). 원본과 같이 ±2로 자른다.
void zscore(std::vector<Feat>& v, double Feat::*f, bool invert, std::vector<double>& z)
{
    const size_t n = v.size();
    z.assign(n, 0.0);

    if (n < 2)
    {
        return;
    }

    double mean = 0.0;

    for (auto& e : v)
    {
        mean += e.*f;
    }

    mean /= (double)n;
    double var = 0.0;

    for (auto& e : v)
    {
        const double d0 = e.*f - mean;
        var += d0 * d0;
    }

    var /= (double)n;
    const double sd = std::sqrt(var);

    if (!(sd > 1e-12))
    {
        return;
    }

    for (size_t i = 0; i < n; ++i)
    {
        double x = (v[i].*f - mean) / sd;
        x = (std::max)(-2.0, (std::min)(2.0, x));
        z[i] = invert ? -x : x;
    }
}

void run_sleeve(const Sleeve& s, std::vector<Feat> all)
{
    printf("\n================ %s ================\n", s.id);
    printf("진입밴드 %.1f%% ~ %.1f%%  히스테리시스 %.1f%%  슬롯 %d  base %.1f%%  총목표 %.0f%%\n",
           s.entry_lower_pct > 0.0 ? s.entry_lower_pct : -s.pullback_pct,
           s.entry_upper_pct, s.zone_hyst_pct, s.slots,
           s.base_pct * 100.0, s.target_total_pct * 100.0);

    std::vector<Feat> pass;
    int cut_align = 0, cut_ext = 0, cut_bars = 0;

    for (auto& f : all)
    {
        if (f.bars < 60)
        {
            ++cut_bars;
            continue;
        }

        if (!f.aligned)
        {
            ++cut_align;
            continue;
        }

        if (s.max_dev_pct > 0.0 && f.pull > s.max_dev_pct)
        {
            ++cut_ext;
            continue;
        }

        if (s.min_dev_pct > 0.0 && f.pull < s.min_dev_pct)
        {
            ++cut_ext;
            continue;
        }

        pass.push_back(f);
    }

    printf("프리필터: 입력=%d 역배열컷=%d 데이터부족=%d 과확장컷=%d 통과=%d\n",
           (int)all.size(), cut_align, cut_bars, cut_ext, (int)pass.size());

    if (pass.empty())
    {
        return;
    }

    // S = 1.0*z(trend) + 1.0*z(-pull) - 0.0*z(vol) + w_liq*z(log 거래대금)
    std::vector<double> zt, zp, zv, zl;
    zscore(pass, &Feat::trend, false, zt);
    zscore(pass, &Feat::pull, true, zp);
    zscore(pass, &Feat::vol, false, zv);

    if (s.w_liq != 0.0)
    {
        zscore(pass, &Feat::turnover, false, zl);
    }
    else
    {
        zl.assign(pass.size(), 0.0);
    }

    for (size_t i = 0; i < pass.size(); ++i)
    {
        pass[i].score = 1.0 * zt[i] + 1.0 * zp[i] - 0.0 * zv[i] + s.w_liq * zl[i];
    }

    std::sort(pass.begin(), pass.end(),
              [](const Feat& a, const Feat& b) { return a.score > b.score; });

    // 비중 배수(ScoreWeight.h):
    //  z = clamp((S-mu)/sd, +-2), raw = 1 + 0.6*z/2, scale = target / (base * sum(상위 raw))
    double mu = 0.0;

    for (auto& f : pass)
    {
        mu += f.score;
    }

    mu /= (double)pass.size();
    double var = 0.0;

    for (auto& f : pass)
    {
        var += (f.score - mu) * (f.score - mu);
    }

    const double sd = std::sqrt(var / (double)pass.size());
    std::vector<double> raw(pass.size(), 1.0);

    for (size_t i = 0; i < pass.size(); ++i)
    {
        double z = sd > 1e-12 ? (pass[i].score - mu) / sd : 0.0;
        z = (std::max)(-2.0, (std::min)(2.0, z));
        raw[i] = 1.0 + 0.6 * z / 2.0;
    }

    const int take = (std::min)((int)pass.size(), s.slots);
    double sum_top = 0.0;

    for (int i = 0; i < take; ++i)
    {
        sum_top += raw[i];
    }

    const double scale = sum_top > 0.0 ? s.target_total_pct / (s.base_pct * sum_top) : 1.0;

    for (size_t i = 0; i < pass.size(); ++i)
    {
        pass[i].mult = raw[i] * scale;
    }

    // 존 판정. 신규 진입 시점(in_zone_=false) 기준이라 히스테리시스는 붙지 않는다.
    const double up_th  = s.entry_upper_pct;
    const double low_th = s.entry_lower_pct > 0.0 ? s.entry_lower_pct : -s.pullback_pct;

    printf("%-6s %-8s %-16s %10s %10s %8s %8s %6s %6s\n",
           "순위", "종목", "이름", "현재가", "SMA20", "이격%", "점수", "배수", "존");
    printf("-------------------------------------------------------------------------------------\n");

    for (int i = 0; i < (int)pass.size(); ++i)
    {
        const Feat& f = pass[i];
        const double s_dev = f.s20 > 0.0 ? (f.px - f.s20) / f.s20 * 100.0 : 999.0;
        const bool zone = f.aligned && f.s20 > 0.0 && s_dev <= up_th && s_dev >= low_th;
        printf("%2d/%-3d %-8s %-16s %10.0f %10.1f %8.2f %8.3f %6.2f %6s%s\n",
               i + 1, (int)pass.size(), f.ticker.c_str(), f.name.c_str(),
               f.px, f.s20, s_dev, f.score, f.mult,
               zone ? "활성" : "대기", i < take ? "" : "  (슬롯밖)");
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

    std::vector<Target> cli;

    for (int i = 1; i < argc; ++i)
    {
        cli.push_back({argv[i], argv[i]});
    }

    if (!cli.empty())
    {
        targets = cli;
    }

    const std::string start = ymd_offset(400);
    const std::string end   = ymd_offset(0);
    printf("네이버 일봉 수집: %s ~ %s, %d종목\n", start.c_str(), end.c_str(), (int)targets.size());

    std::vector<Feat> all;

    for (const auto& t : targets)
    {
        const std::string path = "/siseJson.naver?symbol=" + std::string(t.code) +
                                 "&requestType=1&startTime=" + start +
                                 "&endTime=" + end + "&timeframe=day";
        std::string body;

        if (!http_get("api.finance.naver.com", path, body))
        {
            printf("  %s 수집 실패\n", t.code);
            continue;
        }

        std::vector<Bar> bars = parse_sise(body);

        if (bars.size() < 60)
        {
            printf("  %s 일봉 %d개 — 60개 미만이라 제외\n", t.code, (int)bars.size());
            continue;
        }

        Feat f;
        f.ticker  = t.code;
        f.name    = t.name;
        f.bars    = (int)bars.size();
        f.px      = bars[0].close;
        f.s5      = sma_close(bars, 5);
        f.s10     = sma_close(bars, 10);
        f.s20     = sma_close(bars, 20);
        f.s60     = sma_close(bars, 60);
        f.s120    = sma_close(bars, 120);
        f.aligned = f.s5 > f.s10 && f.s10 > f.s20 && f.s20 > f.s60;
        f.trend   = f.s60 > 0.0 ? (f.s5 - f.s60) / f.s60 : 0.0;
        f.pull    = f.s20 > 0.0 ? (f.px - f.s20) / f.s20 : 0.0;

        double v = 0.0;
        int vn = 0;

        for (int k = 0; k < 20 && k < (int)bars.size(); ++k)
        {
            if (bars[k].close > 0.0)
            {
                v += (bars[k].high - bars[k].low) / bars[k].close;
                ++vn;
            }
        }

        f.vol = vn > 0 ? v / vn * 100.0 : 0.0;
        const double turnover = bars[0].close * bars[0].volume;
        f.turnover = turnover > 0.0 ? std::log(turnover) : 0.0;

        printf("  %s %-16s 일봉 %3d개  종가 %9.0f  SMA20 %9.1f  이격 %+7.2f%%  정배열 %s\n",
               f.ticker.c_str(), f.name.c_str(), f.bars, f.px, f.s20,
               f.pull * 100.0, f.aligned ? "Y" : "N");
        all.push_back(f);
        Sleep(120);  // 네이버 호출 간격
    }

    if (all.empty())
    {
        printf("수집된 종목이 없다. 네트워크나 종목코드를 확인한다.\n");
        return 1;
    }

    run_sleeve(kDevscale, all);
    run_sleeve(kTrendx, all);
    return 0;
}
