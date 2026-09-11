// api/KisMarket.cpp — 국내·해외 주식 시세: 일봉·분봉·현재가·펀더멘털. 분봉 페이지 병합 헬퍼는 익명 ns.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"

std::vector<MarketData> KisClient::get_daily_ohlcv(const std::string& ticker, int count, bool include_today)
{
    if (count <= 0)
    {
        return {};
    }

    // 캐시 키에 절단 여부를 넣는다. ticker만으로 키를 잡으면 절단본과 미절단본이 서로를
    //  덮어써서 호출자가 뭘 받을지 호출 순서에 달리게 된다(D-005).
    const std::string ckey = ticker + (include_today ? "|T" : "|F");

    // 캐시 조회 — 유효시간 안이고 요청한 만큼 담겨 있으면 그대로 쓴다. 최신봉이 앞이라
    //  더 짧은 요청은 앞에서 잘라 답한다. timestamp는 받아온 시각이라 지금으로 다시 찍는다.
    if (cfg_.daily_cache_ttl_sec > 0)
    {
        std::lock_guard<std::mutex> lk(daily_cache_mtx_);
        auto it = daily_cache_.find(ckey);

        if (it != daily_cache_.end() && it->second.requested >= count &&
            std::chrono::steady_clock::now() - it->second.at <
                std::chrono::seconds(cfg_.daily_cache_ttl_sec))
        {
            size_t n = (std::min)(static_cast<size_t>(count), it->second.bars.size());
            std::vector<MarketData> hit(it->second.bars.begin(), it->second.bars.begin() + n);
            auto now = std::chrono::system_clock::now();

            for (auto& md : hit)
            {
                md.timestamp = now;
            }

            return hit;
        }
    }

    // G1 수정: 날짜 하드코딩(19000101~99991231)은 모의서버 500 → 유한창(오늘−N일 ~ 오늘, KST).
    //   1콜 ~100봉이면 충분(정배열/추세 판정 60~120일). count>≈100은 페이지네이션 미구현(초기 단일콜).
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
    time_t end_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + kKstOffsetSec; // KST 오늘
    // count 거래일 확보를 위해 달력일 여유(주말·휴일 감안 1.7배 + 헤드룸 10일), 최소 30일.
    int window_days = (std::max)(30, static_cast<int>(count * 1.7) + 10); // (): windows.h max 매크로 회피
    std::string d2 = fmt_date(end_t);
    std::string d1 = fmt_date(end_t - static_cast<time_t>(window_days) * 86400);

    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice" +
                      "?FID_COND_MRKT_DIV_CODE=J" + "&FID_INPUT_ISCD=" + ticker + "&FID_INPUT_DATE_1=" + d1 +
                      "&FID_INPUT_DATE_2=" + d2 + "&FID_PERIOD_DIV_CODE=D" + "&FID_ORG_ADJ_PRC=0";

    std::vector<std::string> headers = auth_headers("FHKST03010100");

    std::string resp = http_get(url, headers);
    std::vector<MarketData> result;

    if (resp.empty())
    {
        LOG_ERROR("[KIS] 일봉 조회 실패: " + ticker);
        return result;
    }

    try
    {
        auto j = json::parse(resp);
        auto& arr = j["output2"];
        int fetched = 0;

        for (auto& item : arr)
        {
            if (fetched >= count)
            {
                break;
            }

            // 당일 미완성 봉 절단(D-005). 응답에서 날짜로 거른다 — FID_INPUT_DATE_2를 전일로
            //  당기는 방식은 휴장·반차 캘린더가 필요해서 쓰지 않는다.
            //  드롭한 행은 fetched로 세지 않는다. 세면 봉이 하나 모자란다.
            if (!include_today && item.value("stck_bsop_date", std::string()) == d2)
            {
                continue;
            }

            ++fetched;
            MarketData md;
            md.ticker = ticker;
            md.close = std::stod(item["stck_clpr"].get<std::string>());
            md.open = std::stod(item["stck_oprc"].get<std::string>());
            md.high = std::stod(item["stck_hgpr"].get<std::string>());
            md.low = std::stod(item["stck_lwpr"].get<std::string>());
            md.volume = std::stoll(item["acml_vol"].get<std::string>());
            md.timestamp = std::chrono::system_clock::now();
            result.push_back(md);
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[KIS] 일봉 파싱 오류: ") + e.what());
    }

    // 빈 결과는 캐시하지 않는다(일시적 500·파싱 실패를 TTL 동안 굳히지 않기 위해).
    if (cfg_.daily_cache_ttl_sec > 0 && !result.empty())
    {
        std::lock_guard<std::mutex> lk(daily_cache_mtx_);
        auto& e = daily_cache_[ckey];
        e.at = std::chrono::steady_clock::now();
        e.requested = count;
        e.bars = result;
    }

    return result;
}

namespace {

// 분봉 응답의 문자열 숫자 필드 → double (빈 값·파싱 실패는 0).
double kis_num(const nlohmann::json& o, const std::string& k)
{
    try { return std::stod(o.value(k, "0")); } catch (...) { return 0.0; }
}

// YYYYMMDD + HHMMSS → time_t (서버 TZ 독립: gmtime 계열로 통일, KST 오프셋은 호출 무관)
time_t kis_parse_dt(const std::string& d, const std::string& t)
{
    if (d.size() != 8 || t.size() < 6)
    {
        return 0;
    }

    struct tm tmv{};

    try {
        tmv.tm_year = std::stoi(d.substr(0, 4)) - 1900;
        tmv.tm_mon  = std::stoi(d.substr(4, 2)) - 1;
        tmv.tm_mday = std::stoi(d.substr(6, 2));
        tmv.tm_hour = std::stoi(t.substr(0, 2));
        tmv.tm_min  = std::stoi(t.substr(2, 2));
        tmv.tm_sec  = std::stoi(t.substr(4, 2));
    } catch (...) { return 0; }
#ifdef _WIN32
    return _mkgmtime(&tmv);
#else
    return timegm(&tmv);
#endif
}

// 1분봉 원본 한 행. 당일 분봉(FHKST03010200)과 과거일 분봉(FHKST03010230)이 같은 필드명을 쓴다.
struct KisRawMinute
{
    std::string date, hour;
    double o = 0, h = 0, l = 0, c = 0;
    int64_t v = 0;
};

// 1분봉 원본 → interval_min 집계봉. 반환은 최신→과거(result[0]=최신), 최대 count봉.
//  두 분봉 TR이 같은 집계를 쓰므로 한 곳에 둔다(따로 두면 한쪽만 고쳐지는 드리프트가 난다).
std::vector<MarketData> kis_aggregate_minutes(std::vector<KisRawMinute>& raws,
                                              const std::string& ticker,
                                              int interval_min, int count)
{
    std::vector<MarketData> result;

    if (raws.empty())
    {
        return result;
    }

    std::sort(raws.begin(), raws.end(), [](const KisRawMinute& a, const KisRawMinute& b) {
        return a.date != b.date ? a.date < b.date : a.hour < b.hour;
    });

    std::vector<MarketData> asc; // 과거→최신 집계봉
    std::string cur_key;

    for (const auto& r : raws)
    {
        int hh = 0, mm = 0;

        try { hh = std::stoi(r.hour.substr(0, 2)); mm = std::stoi(r.hour.substr(2, 2)); } catch (...) { continue; }
        int bucket = (hh * 60 + mm) / interval_min;        // 시계 정렬 버킷
        std::string key = r.date + ":" + std::to_string(bucket);

        if (key != cur_key)
        {
            MarketData md;
            md.ticker = ticker;
            md.market = Market::KR;
            md.open = r.o; md.high = r.h; md.low = r.l; md.close = r.c;
            md.volume = r.v;
            md.timestamp = std::chrono::system_clock::from_time_t(kis_parse_dt(r.date, r.hour));
            asc.push_back(md);
            cur_key = key;
        }
        else
        {
            MarketData& md = asc.back();
            md.high = (std::max)(md.high, r.h); // (): windows.h max 매크로 회피
            md.low  = (std::min)(md.low, r.l);
            md.close = r.c;                                // 버킷 내 최신 마감
            md.volume += r.v;
            md.timestamp = std::chrono::system_clock::from_time_t(kis_parse_dt(r.date, r.hour));
        }
    }

    // 최신→과거(result[0]=최신)로 뒤집고 count봉만.
    for (auto it = asc.rbegin(); it != asc.rend() && static_cast<int>(result.size()) < count; ++it)
    {
        MarketData md = *it;
        md.bar_index = static_cast<int>(result.size()); // 0=최신
        result.push_back(md);
    }

    return result;
}

// output2(최신→과거) 파싱 공통부. seen으로 페이지 경계 중복을 걸러내고,
//  이 페이지에서 가장 이른 HHMMSS를 돌려준다(역페이징 커서).
//  date_filter가 비어있지 않으면 그 날짜 행만 취한다.
std::string kis_parse_minute_page(const nlohmann::json& arr,
                                  std::vector<KisRawMinute>& raws,
                                  std::unordered_set<std::string>& seen,
                                  const std::string& date_filter,
                                  int& added_out)
{
    std::string page_earliest;
    added_out = 0;

    for (const auto& item : arr)
    {
        std::string d = item.value("stck_bsop_date", "");
        std::string t = item.value("stck_cntg_hour", "");

        if (t.size() < 6)
        {
            continue;
        }

        if (page_earliest.empty() || t < page_earliest)
        {
            page_earliest = t;
        }

        if (!date_filter.empty() && d != date_filter)
        {
            continue;  // 요청 날짜 밖 행 방어
        }

        std::string key = d + t;

        if (!seen.insert(key).second)
        {
            continue;  // 중복
        }

        KisRawMinute r;
        r.date = d; r.hour = t;
        r.o = kis_num(item, "stck_oprc");
        r.h = kis_num(item, "stck_hgpr");
        r.l = kis_num(item, "stck_lwpr");
        r.c = kis_num(item, "stck_prpr");
        r.v = static_cast<int64_t>(kis_num(item, "cntg_vol"));
        raws.push_back(r);
        ++added_out;
    }

    return page_earliest;
}

} // namespace

std::vector<MarketData> KisClient::get_minute_ohlcv(const std::string& ticker, int count, int interval_min)
{
    ensure_authenticated();
    std::vector<MarketData> result;

    if (count <= 0)
    {
        return result;
    }

    if (interval_min < 1)
    {
        interval_min = 1;
    }

    // 기준시각: 현재 KST(장중)이면 지금, 장전/장후면 15:30에서 역조회.
    time_t now_kst = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + kKstOffsetSec;
    struct tm ntm{};
#ifdef _WIN32
    gmtime_s(&ntm, &now_kst);
#else
    gmtime_r(&now_kst, &ntm);
#endif
    char hbuf[7];
    std::snprintf(hbuf, sizeof(hbuf), "%02d%02d%02d", ntm.tm_hour, ntm.tm_min, ntm.tm_sec);
    std::string hour = hbuf;

    if (hour < "090000" || hour > "153000")
    {
        hour = "153000";
    }

    std::vector<std::string> hdrs = auth_headers("FHKST03010200");

    // 필요한 1분봉 수 = count*interval_min. 1콜당 ~30봉 → 여유롭게 페이지 상한.
    const int need_1min  = count * interval_min;
    const int kMaxPages  = (std::min)(20, need_1min / 25 + 3); // (): windows.h min 매크로 회피

    std::vector<KisRawMinute> raws;
    std::unordered_set<std::string> seen; // date+hour 중복(페이지 경계) 제거

    for (int page = 0; page < kMaxPages && static_cast<int>(raws.size()) < need_1min; ++page)
    {
        std::string url = base_url() +
            "/uapi/domestic-stock/v1/quotations/inquire-time-itemchartprice"
            "?FID_ETC_CLS_CODE="
            "&FID_COND_MRKT_DIV_CODE=J"
            "&FID_INPUT_ISCD=" + ticker +
            "&FID_INPUT_HOUR_1=" + hour +
            "&FID_PW_DATA_INCU_YN=N";

        std::string resp = http_get(url, hdrs);

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

        int added = 0;
        std::string page_earliest = kis_parse_minute_page(arr, raws, seen, "", added);

        if (page_earliest.empty())
        {
            break;
        }

        // 09:00 봉이 이번 페이지에 들어왔으면 더 앞은 없다. 커서는 1분 앞 시각이되 09:00 아래로는 내리지
        // 않는다(09:00 봉 하나만 남은 페이지를 놓치지 않으려고).
        if (page_earliest <= "090000")
        {
            break;
        }

        std::string prev = kis_hhmmss_minus_minutes(page_earliest, 1);

        if (prev.empty())
        {
            break;
        }

        if (prev < "090000")
        {
            prev = "090000";
        }

        hour = prev;
        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // rate limit 여유
    }

    return kis_aggregate_minutes(raws, ticker, interval_min, count);
}

// 지정 날짜(과거일 포함)의 분봉. TR FHKST03010230 (inquire-time-dailychartprice).
//  당일 분봉 TR(FHKST03010200)은 날짜 인자가 없어 오늘에 갇힌다. 이 TR은 FID_INPUT_DATE_1을 받아
//  과거 날짜를 조회할 수 있고 1콜에 1분봉 120개(=130분)를 준다.
//  주의: 이 TR의 output1은 요청 날짜가 아니라 실시간 현재 스냅샷이라 쓰지 않는다. output2만 쓴다.
std::vector<MarketData> KisClient::get_daily_minute_ohlcv(const std::string& ticker,
                                                          const std::string& yyyymmdd,
                                                          int count, int interval_min,
                                                          const std::string& end_hhmmss)
{
    ensure_authenticated();
    std::vector<MarketData> result;

    if (count <= 0 || yyyymmdd.size() != 8)
    {
        return result;
    }

    if (interval_min < 1)
    {
        interval_min = 1;
    }

    std::string hour = end_hhmmss.size() == 6 ? end_hhmmss : std::string("153000");

    std::vector<std::string> hdrs = auth_headers("FHKST03010230", {"custtype: P"});

    // 1콜당 최대 120봉. 09:00~15:30이 390분이라 하루치 전체도 4콜이면 찬다.
    const int need_1min = count * interval_min;
    const int kMaxPages = (std::min)(6, need_1min / 110 + 2); // (): windows.h min 매크로 회피

    std::vector<KisRawMinute> raws;
    std::unordered_set<std::string> seen;

    for (int page = 0; page < kMaxPages && static_cast<int>(raws.size()) < need_1min; ++page)
    {
        std::string url = base_url() +
            "/uapi/domestic-stock/v1/quotations/inquire-time-dailychartprice"
            "?FID_COND_MRKT_DIV_CODE=J"
            "&FID_INPUT_ISCD=" + ticker +
            "&FID_INPUT_HOUR_1=" + hour +
            "&FID_INPUT_DATE_1=" + yyyymmdd +
            "&FID_PW_DATA_INCU_YN=Y"
            "&FID_FAKE_TICK_INCU_YN=N";

        std::string resp = http_get(url, hdrs);

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

        int added = 0;
        std::string page_earliest = kis_parse_minute_page(arr, raws, seen, yyyymmdd, added);

        if (page_earliest.empty())
        {
            break;
        }

        if (page_earliest <= "090000")
        {
            break; // 그날 장 시작에 도달
        }

        std::string prev = kis_hhmmss_minus_minutes(page_earliest, 1);

        if (prev.empty())
        {
            break;
        }

        if (prev < "090000")
        {
            prev = "090000";
        }

        hour = prev;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    return kis_aggregate_minutes(raws, ticker, interval_min, count);
}

double KisClient::get_current_price(const std::string& ticker)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-price" + "?FID_COND_MRKT_DIV_CODE=J" +
                      "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = auth_headers("FHKST01010100");

    std::string resp = http_get(url, headers);

    if (resp.empty())
    {
        return 0.0;
    }

    try
    {
        auto j = json::parse(resp);
        return std::stod(j["output"]["stck_prpr"].get<std::string>());
    }
    catch (...)
    {
        return 0.0;
    }
}

Fundamentals KisClient::get_fundamentals(const std::string& ticker)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-price" + "?FID_COND_MRKT_DIV_CODE=J" +
                      "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = auth_headers("FHKST01010100");

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, headers);

    if (resp.empty())
    {
        return f;
    }

    try
    {
        auto j = json::parse(resp);
        auto& out = j["output"];
        auto parse_d = [&](const std::string& key) -> double
        {
            std::string s = out.value(key, "");

            if (s.empty())
            {
                return 0.0;
            }

            try
            {
                return std::stod(s);
            }
            catch (...)
            {
                return 0.0;
            }
        };
        f.last = parse_d("stck_prpr"); // 현재가
        f.diff = parse_d("prdy_vrss"); // 전일대비
        f.rate = parse_d("prdy_ctrt"); // 등락률(%)
        f.open = parse_d("stck_oprc"); // 시가
        f.high = parse_d("stck_hgpr"); // 고가
        f.low = parse_d("stck_lwpr");  // 저가
        f.pbr        = parse_d("pbr");
        f.per        = parse_d("per");
        f.market_cap = parse_d("hts_avls"); // 시가총액 (억원)
    }
    catch (...)
    {
    }

    return f;
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 일봉 조회
//  tr_id: HHDFS76240000
// ═══════════════════════════════════════════════════════════════════════════
std::vector<MarketData> KisClient::get_us_daily_ohlcv(const std::string& ticker, int count, const std::string& exchange)
{
    std::string url = base_url() + "/uapi/overseas-price/v1/quotations/dailyprice" + "?AUTH=" + "&EXCD=" + exchange +
                      "&SYMB=" + ticker + "&GUBN=0" + "&BYMD=" + "&MODP=0";

    std::vector<std::string> hdrs = auth_headers("HHDFS76240000", {"custtype: P"});

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS-US] OHLCV 응답 없음: " + ticker);
        return {};
    }

    LOG_DEBUG("[KIS-US] OHLCV 응답(" + ticker + "): " + resp.substr(0, 300));

    std::vector<MarketData> result;

    try
    {
        auto j = json::parse(resp);

        if (!j.contains("output2") || !j["output2"].is_array())
        {
            LOG_WARN("[KIS-US] output2 없음: " + ticker);
            return {};
        }

        for (const auto& item : j["output2"])
        {
            MarketData md;
            md.ticker = ticker;
            md.market = Market::US;
            // KIS 해외 일봉 필드: clos/open/high/low/tvol
            auto parse_d = [](const json& o, const std::string& k) -> double
            {
                if (!o.contains(k))
                {
                    return 0.0;
                }

                std::string s = o[k].is_string() ? o[k].get<std::string>() : o[k].dump();

                try
                {
                    return s.empty() ? 0.0 : std::stod(s);
                }
                catch (...)
                {
                    return 0.0;
                }
            };
            md.close = parse_d(item, "clos");
            md.open = parse_d(item, "open");
            md.high = parse_d(item, "high");
            md.low = parse_d(item, "low");
            md.volume = 0;

            try
            {
                std::string v = item.value("tvol", "0");

                if (!v.empty())
                {
                    md.volume = std::stoll(v);
                }
            }
            catch (...)
            {
            }

            md.timestamp = std::chrono::system_clock::now();

            if (md.close > 0.0)
            {
                result.push_back(md);
            }

            if ((int)result.size() >= count)
            {
                break;
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS-US] OHLCV 파싱 오류 " + ticker + ": " + e.what());
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 펀더멘털 (PER / 현재가)
//  tr_id: HHDFS00000300
//  NOTE: KIS 해외주식 API는 pbr 미제공 → per 필드 활용
// ═══════════════════════════════════════════════════════════════════════════
Fundamentals KisClient::get_us_fundamentals(const std::string& ticker, const std::string& exchange)
{
    std::string url =
        base_url() + "/uapi/overseas-price/v1/quotations/price-detail" + "?AUTH=&EXCD=" + exchange + "&SYMB=" + ticker;

    std::vector<std::string> hdrs = auth_headers("HHDFS00000300", {"custtype: P"});

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS-US] Fundamentals 응답 없음: " + ticker);
        return f;
    }

    LOG_DEBUG("[KIS-US] Fundamentals 응답(" + ticker + "): " + resp.substr(0, 300));

    try
    {
        auto j = json::parse(resp);
        // KIS 해외주식 현재가상세는 output1 키 사용
        const char* out_key = j.contains("output1") ? "output1" : j.contains("output") ? "output" : nullptr;

        if (!out_key)
        {
            LOG_WARN("[KIS-US] output 키 없음: " + ticker);
            return f;
        }

        const auto& out = j[out_key];
        auto parse_dbl = [&](const std::string& key) -> double
        {
            std::string s = out.value(key, "");

            if (s.empty())
            {
                return 0.0;
            }

            try
            {
                return std::stod(s);
            }
            catch (...)
            {
                return 0.0;
            }
        };
        auto parse_i64 = [&](const std::string& key) -> int64_t
        {
            std::string s = out.value(key, "");

            if (s.empty())
            {
                return 0;
            }

            try
            {
                return std::stoll(s);
            }
            catch (...)
            {
                return 0;
            }
        };
        f.per = parse_dbl("per");
        f.pbr = parse_dbl("pbr");

        if (f.pbr == 0.0)
        {
            f.pbr = parse_dbl("p_b_rate");
        }

        f.last = parse_dbl("last");
        f.open = parse_dbl("open");
        f.high = parse_dbl("high");
        f.low = parse_dbl("low");
        f.pbid = parse_dbl("pbid");
        f.pask = parse_dbl("pask");
        f.vbid = parse_i64("vbid");
        f.vask = parse_i64("vask");
        f.diff = parse_dbl("diff");
        f.rate = parse_dbl("rate");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS-US] Fundamentals 파싱 오류 " + ticker + ": " + e.what());
    }

    return f;
}
