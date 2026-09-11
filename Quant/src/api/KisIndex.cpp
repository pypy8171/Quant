// api/KisIndex.cpp — 지수·업종 일봉, 투자자 매매동향·수급, 지수 현재값, 국내 선물 시세·전광판.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"

// ─── 업종 지수 일봉 ──────────────────────────────────────────────────────────
std::vector<MarketData> KisClient::get_index_daily_ohlcv(const std::string& sector_code, int count)
{
    ensure_authenticated();

    // 이 TR(FHKUP03500100)은 응답 헤더(tr_cont) 페이지네이션 대신 날짜범위 방식.
    // 1콜당 ~50봉만 오므로(라이브 확인), DATE_2를 과거로 밀며 count봉까지 누적한다.
    // (Python get_historical_ohlcv와 동일 패턴). 결과는 최신→과거(timestamp 내림차순) 정렬.
    std::vector<MarketData> result;

    if (count <= 0)
    {
        return result;
    }

    // 날짜 산술은 서버 로컬 TZ에 독립적이어야 한다(UTC 클라우드 등). gmtime/timegm으로
    // 통일하고, '오늘'은 KST(+9h) 기준으로 잡는다(KST 자정~오전 실행 시 최신봉 누락 방지).
    auto fmt_date = [](time_t t) -> std::string {
        struct tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
        return std::string(buf);
    };
    auto parse_ymd = [](const std::string& s) -> time_t {
        if (s.size() != 8)
        {
            return 0;
        }

        struct tm tmv{};

        try {
            tmv.tm_year = std::stoi(s.substr(0, 4)) - 1900;
            tmv.tm_mon  = std::stoi(s.substr(4, 2)) - 1;
            tmv.tm_mday = std::stoi(s.substr(6, 2));
            tmv.tm_hour = 12; // 정오 기준 — 경계 회피
        } catch (...) { return 0; }
#ifdef _WIN32
        return _mkgmtime(&tmv);
#else
        return timegm(&tmv);
#endif
    };
    auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
        try { return std::stod(o.value(k, "0")); } catch (...) { return 0.0; }
    };

    std::vector<std::string> hdrs = auth_headers("FHKUP03500100");

    time_t end_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
                   + kKstOffsetSec;           // KST 기준 '오늘'
    std::string oldest_seen;                  // 직전까지 받은 가장 오래된 날짜
    constexpr int kMaxPages   = 10;
    constexpr int kWindowDays = 130;          // 1콜 ~50봉 커버 위해 넉넉히

    for (int page = 0; page < kMaxPages && static_cast<int>(result.size()) < count; ++page)
    {
        std::string d2 = fmt_date(end_t);
        std::string d1 = fmt_date(end_t - static_cast<time_t>(kWindowDays) * 86400);
        std::string url = base_url() +
            "/uapi/domestic-stock/v1/quotations/inquire-daily-indexchartprice"
            "?FID_COND_MRKT_DIV_CODE=U"
            "&FID_INPUT_ISCD=" + sector_code +
            "&FID_INPUT_DATE_1=" + d1 +
            "&FID_INPUT_DATE_2=" + d2 +
            "&FID_PERIOD_DIV_CODE=D";

        try
        {
            auto resp = http_get(url, hdrs);

            if (resp.empty())
            {
                break;
            }

            auto j = json::parse(resp, nullptr, false);

            if (j.is_discarded() || !j.contains("output2"))
            {
                break;
            }

            auto& arr = j["output2"];

            if (arr.empty())
            {
                break;
            }

            std::string page_oldest;
            int added = 0;

            for (const auto& item : arr)              // output2는 최신→과거 순
            {
                std::string bd = item.value("stck_bsop_date", "");

                // 페이지 경계 중복 방지: 이미 받은 범위(>= oldest_seen)는 스킵
                if (!oldest_seen.empty() && !bd.empty() && bd >= oldest_seen)
                {
                    continue;
                }

                MarketData d;
                d.ticker = sector_code;
                d.close  = sd(item, "bstp_nmix_prpr");
                d.open   = sd(item, "bstp_nmix_oprc");
                d.high   = sd(item, "bstp_nmix_hgpr");
                d.low    = sd(item, "bstp_nmix_lwpr");
                d.volume = static_cast<int64_t>(sd(item, "acml_vol"));

                if (!bd.empty())
                {
                    d.timestamp = std::chrono::system_clock::from_time_t(parse_ymd(bd));
                }

                result.push_back(d);
                ++added;

                if (!bd.empty() && (page_oldest.empty() || bd < page_oldest))
                {
                    page_oldest = bd;
                }

                if (static_cast<int>(result.size()) >= count)
                {
                    break;
                }
            }

            if (page_oldest.empty() || added == 0)
            {
                break;  // 새 데이터 없음 → 종료
            }

            oldest_seen = page_oldest;
            time_t ot = parse_ymd(page_oldest);

            if (ot == 0)
            {
                break;
            }

            end_t = ot - 86400;                              // 다음 윈도우: 최古일 -1일
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[KIS] 업종 일봉(" + sector_code + ") 오류: " + e.what());
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // rate limit 여유
    }

    // 호출자(ThemeStrategy: bars[0]=최신)·MA 계산이 정렬에 의존 → KIS 응답 순서와
    // 무관하게 최신→과거(timestamp 내림차순)로 명시 보장.
    std::sort(result.begin(), result.end(),
              [](const MarketData& a, const MarketData& b) { return a.timestamp > b.timestamp; });

    if (static_cast<int>(result.size()) < count)
    {
        LOG_WARN("[KIS] 업종 일봉(" + sector_code + ") 요청 " + std::to_string(count) +
                 "봉 중 " + std::to_string(result.size()) + "봉만 수집 (페이지 한계/데이터 부족)");
    }

    return result;
}

// ─── 투자자별 매매동향 (외국인·기관 순매수) ──────────────────────────────────
KisClient::InvestorTrend KisClient::get_investor_trend(const std::string& ticker)
{
    ensure_authenticated();

    std::string url = base_url() +
        "/uapi/domestic-stock/v1/quotations/inquire-investor"
        "?FID_COND_MRKT_DIV_CODE=J"
        "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> hdrs = auth_headers("FHKST01010900");

    InvestorTrend result;
    result.ticker = ticker;

    try
    {
        auto resp = http_get(url, hdrs);

        if (resp.empty())
        {
            return result;
        }

        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded() || !j.contains("output"))
        {
            return result;
        }

        // output 배열 최신(오늘) 데이터만 사용
        auto& arr = j["output"];

        if (arr.empty())
        {
            return result;
        }

        const auto& latest = arr[0];

        auto si = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            try { return std::stoll(o.value(k, "0")); } catch (...) { return 0; }
        };
        result.foreign_net = si(latest, "frgn_ntby_qty");  // 외국인 순매수
        result.inst_net    = si(latest, "orgn_ntby_qty");   // 기관 순매수
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] 투자자동향(" + ticker + ") 오류: " + e.what());
    }

    return result;
}

// ─── 투자자별 매매동향 일자별 시계열 ────────────────────────────────────────
// 동일 엔드포인트(FHKST01010900), output 배열 전체 파싱 (최대 30거래일)
// flows[0] = 가장 최근 거래일 (장 종료 후 확정치)
std::vector<InvestorFlow> KisClient::get_investor_flow(const std::string& ticker,
                                                        const std::string& market_div)
{
    ensure_authenticated();

    std::string url = base_url() +
        "/uapi/domestic-stock/v1/quotations/inquire-investor"
        "?FID_COND_MRKT_DIV_CODE=" + market_div +
        "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> hdrs = auth_headers("FHKST01010900");

    std::vector<InvestorFlow> result;

    try
    {
        auto resp = http_get(url, hdrs);

        if (resp.empty())
        {
            return result;
        }

        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded() || !j.contains("output"))
        {
            return result;
        }

        auto si = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            std::string s = o.value(k, "");

            if (s.empty())
            {
                return 0;
            }

            // KIS 부호 있는 수치 문자열: 앞에 + / - 포함 가능
            try { return std::stoll(s); } catch (...) { return 0; }
        };
        auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
            std::string s = o.value(k, "");

            if (s.empty())
            {
                return 0.0;
            }

            try { return std::stod(s); } catch (...) { return 0.0; }
        };

        for (const auto& row : j["output"])
        {
            InvestorFlow f;
            f.date        = row.value("stck_bsop_date", "");
            f.foreign_net = si(row, "frgn_ntby_qty");
            f.inst_net    = si(row, "orgn_ntby_qty");
            f.indiv_net   = si(row, "prsn_ntby_qty");
            f.close       = sd(row, "stck_clpr");

            if (!f.date.empty())
            {
                result.push_back(std::move(f));
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] 투자자 시계열(" + ticker + ") 오류: " + e.what());
    }

    return result; // [0]=가장 최근, look-ahead 방지는 호출측 책임
}

// ─── 지수 현재값 (코스피 "0001", 코스닥 "1001", KOSPI200 "2001") ────────────
KisClient::IndexPrice KisClient::get_index_price(const std::string& ticker)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-index-price"
                      + "?FID_COND_MRKT_DIV_CODE=U&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = auth_headers("FHPUP02100000", {"Content-Type: application/json"});

    IndexPrice ip;
    ip.ticker = ticker;

    try
    {
        auto resp = http_get(url, headers);
        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded() || !j.contains("output"))
        {
            return ip;
        }

        auto& o = j["output"];
        auto sd = [&](const char* k) -> double
        {
            try
            {
                return std::stod(o.value(k, "0"));
            }
            catch (...)
            {
                return 0.0;
            }
        };
        ip.price = sd("bstp_nmix_prpr");
        ip.change = sd("bstp_nmix_prdy_vrss");
        ip.change_rate = sd("bstp_nmix_prdy_ctrt");

        try
        {
            ip.sign = std::stoi(o.value("prdy_vrss_sign", "3"));
        }
        catch (...)
        {
            ip.sign = 3;
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] get_index_price(" + ticker + ") 실패: " + e.what());
    }

    return ip;
}

KisClient::FuturePrice KisClient::get_future_price(const std::string& iscd, const std::string& market_div)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-futureoption/v1/quotations/inquire-price"
                      + "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_INPUT_ISCD=" + iscd;

    std::vector<std::string> headers = auth_headers("FHMIF10000000", {"Content-Type: application/json"});

    FuturePrice fp;
    fp.iscd = iscd;

    try
    {
        auto resp = http_get(url, headers);
        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded())
        {
            LOG_WARN("[KIS] get_future_price(" + iscd + ") JSON 파싱 불가: " + resp.substr(0, 200));
            return fp;
        }

        // 스키마 확정됨(2026-09-03). 첫 호출 1회 raw output 덤프 유지 — 스키마 변동/디버그 대비.
        static bool dumped = false;

        if (!dumped)
        {
            dumped = true;
            std::string dump;

            for (const char* key : {"output1", "output2", "output3", "output"})
            {
                if (j.contains(key))
                {
                    dump += std::string(key) + "=" + j[key].dump() + "  ";
                }
            }

            LOG_INFO("[KIS] get_future_price RAW " + (dump.empty() ? resp.substr(0, 500) : dump));
        }

        // 시세를 담은 output 객체 탐색: 가격 필드(futs_prpr)를 가진 객체를 우선 확정,
        // 없으면 output2→output1→output 순서의 첫 객체.
        const json* o = nullptr;

        for (const char* key : {"output2", "output1", "output"})
        {
            if (j.contains(key) && j[key].is_object())
            {
                if (!o)
                {
                    o = &j[key];
                }

                if (j[key].contains("futs_prpr"))
                {
                    o = &j[key];
                    break;
                }
            }
        }

        if (!o)
        {
            return fp;
        }

        auto sd = [&](const char* k) -> double {
            try { return std::stod((*o).value(k, "0")); } catch (...) { return 0.0; }
        };
        auto sll = [&](const char* k) -> int64_t {
            try { return std::stoll((*o).value(k, "0")); } catch (...) { return 0; }
        };

        fp.price         = sd("futs_prpr");
        fp.change        = sd("futs_prdy_vrss");
        fp.change_rate   = sd("futs_prdy_ctrt");

        try { fp.sign = std::stoi((*o).value("prdy_vrss_sign", "3")); } catch (...) { fp.sign = 3; }
        fp.open          = sd("futs_oprc");
        fp.high          = sd("futs_hgpr");
        fp.low           = sd("futs_lwpr");
        fp.volume        = sll("acml_vol");
        fp.open_interest = sll("hts_otst_stpl_qty");
        fp.ok            = (fp.price != 0.0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] get_future_price(" + iscd + ") 실패: " + e.what());
    }

    return fp;
}

nlohmann::json KisClient::get_future_board(const std::string& market_cls, const std::string& market_div)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-futureoption/v1/quotations/display-board-futures"
                      + "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20503"
                      + "&FID_COND_MRKT_CLS_CODE=" + market_cls;

    std::vector<std::string> headers = auth_headers("FHPIF05030200", {"Content-Type: application/json"});

    try
    {
        auto resp = http_get(url, headers);
        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded())
        {
            LOG_WARN("[KIS] get_future_board JSON 파싱 불가: " + resp.substr(0, 200));
            return json::object();
        }

        return j;
    }
    catch (const std::exception& e)
    {
        LOG_WARN(std::string("[KIS] get_future_board 실패: ") + e.what());
        return json::object();
    }
}
