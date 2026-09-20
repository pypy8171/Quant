// api/KisIndex.cpp — 지수·업종 일봉, 투자자 매매동향·수급, 지수 현재값, 국내 선물 시세·전광판.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#include "api/KisRestDecode.h"

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

    // 조회창의 초는 KST 자리값을 UTC로 읽은 값이라 옮기지 않고 날짜로 찍는다. '오늘'은 KST 기준
    //  (KST 자정~오전 실행 시 최신봉 누락 방지). 서버 TZ와 무관하다.
    auto format_date = [](time_t time_value) -> std::string { return kst::format_ymd(kst::utc_date(time_value)); };
    // 일봉 timestamp는 그 날짜의 UTC 정오 — 날짜 경계 회피. 읽는 쪽도 같은 기준으로 날짜를 뽑는다.
    auto parse_ymd = [](const std::string& text) -> time_t { return kis_rest::parse_dt(text, "120000"); };
    auto number_of = [](const nlohmann::json& node, const char* key) -> double {
        try { return std::stod(node.value(key, "0")); } catch (...) { return 0.0; }
    };

    std::vector<std::string> headers = authentication_headers("FHKUP03500100");

    time_t end_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
                   + kKstOffsetSec;           // KST 기준 '오늘'
    std::string oldest_seen;                  // 직전까지 받은 가장 오래된 날짜
    constexpr int kMaxPages   = 10;
    constexpr int kWindowDays = 130;          // 1콜 ~50봉 커버 위해 넉넉히

    for (int page = 0; page < kMaxPages && static_cast<int>(result.size()) < count; ++page)
    {
        std::string to_date = format_date(end_t);
        std::string from_date = format_date(end_t - static_cast<time_t>(kWindowDays) * 86400);
        std::string url = base_url() +
            "/uapi/domestic-stock/v1/quotations/inquire-daily-indexchartprice"
            "?FID_COND_MRKT_DIV_CODE=U"
            "&FID_INPUT_ISCD=" + sector_code +
            "&FID_INPUT_DATE_1=" + from_date +
            "&FID_INPUT_DATE_2=" + to_date +
            "&FID_PERIOD_DIV_CODE=D";

        try
        {
            auto response = http_get(url, headers);

            if (response.empty())
            {
                break;
            }

            auto document = json::parse(response, nullptr, false);

            if (document.is_discarded() || !document.contains("output2"))
            {
                break;
            }

            auto& array = document["output2"];

            if (array.empty())
            {
                break;
            }

            std::string page_oldest;
            int added = 0;

            for (const auto& item : array)              // output2는 최신→과거 순
            {
                std::string business_date = item.value("stck_bsop_date", "");

                // 페이지 경계 중복 방지: 이미 받은 범위(>= oldest_seen)는 스킵
                if (!oldest_seen.empty() && !business_date.empty() && business_date >= oldest_seen)
                {
                    continue;
                }

                MarketData market_data;
                market_data.ticker = sector_code;
                market_data.close  = number_of(item, "bstp_nmix_prpr");
                market_data.open   = number_of(item, "bstp_nmix_oprc");
                market_data.high   = number_of(item, "bstp_nmix_hgpr");
                market_data.low    = number_of(item, "bstp_nmix_lwpr");
                market_data.volume = static_cast<int64_t>(number_of(item, "acml_vol"));

                if (!business_date.empty())
                {
                    market_data.timestamp = std::chrono::system_clock::from_time_t(parse_ymd(business_date));
                }

                result.push_back(std::move(market_data));
                ++added;

                if (!business_date.empty() && (page_oldest.empty() || business_date < page_oldest))
                {
                    page_oldest = std::move(business_date);
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

            time_t ot = parse_ymd(page_oldest);
            oldest_seen = std::move(page_oldest);

            if (ot == 0)
            {
                break;
            }

            end_t = ot - 86400;                              // 다음 윈도우: 최古일 -1일
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[KIS] 업종 일봉(" + sector_code + ") 오류: " + exception.what());
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // rate limit 여유
    }

    // 호출자(ThemeStrategy: bars[0]=최신)·MA 계산이 정렬에 의존 → KIS 응답 순서와
    // 무관하게 최신→과거(timestamp 내림차순)로 명시 보장.
    std::ranges::sort(result, std::ranges::greater{}, &MarketData::timestamp);

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

    InvestorTrend result;
    result.ticker = ticker;

    try
    {
        auto response = http_get(url, authentication_headers("FHKST01010900"));

        if (response.empty())
        {
            return result;
        }

        auto document = json::parse(response, nullptr, false);

        if (document.is_discarded() || !document.contains("output"))
        {
            return result;
        }

        // output 배열 최신(오늘) 데이터만 사용
        auto& array = document["output"];

        if (array.empty())
        {
            return result;
        }

        const auto& latest = array[0];

        auto to_int64 = [](const nlohmann::json& node, const char* key) -> int64_t {
            try { return std::stoll(node.value(key, "0")); } catch (...) { return 0; }
        };
        result.foreign_net = to_int64(latest, "frgn_ntby_qty");  // 외국인 순매수
        result.institution_net    = to_int64(latest, "orgn_ntby_qty");   // 기관 순매수
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[KIS] 투자자동향(" + ticker + ") 오류: " + exception.what());
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

    std::vector<InvestorFlow> result;

    try
    {
        auto response = http_get(url, authentication_headers("FHKST01010900"));

        if (response.empty())
        {
            return result;
        }

        auto document = json::parse(response, nullptr, false);

        if (document.is_discarded() || !document.contains("output"))
        {
            return result;
        }

        auto to_int64 = [](const nlohmann::json& node, const char* key) -> int64_t {
            std::string text = node.value(key, "");

            if (text.empty())
            {
                return 0;
            }

            // KIS 부호 있는 수치 문자열: 앞에 + / - 포함 가능
            try { return std::stoll(text); } catch (...) { return 0; }
        };
        auto number_of = [](const nlohmann::json& node, const char* key) -> double {
            std::string text = node.value(key, "");

            if (text.empty())
            {
                return 0.0;
            }

            try { return std::stod(text); } catch (...) { return 0.0; }
        };

        for (const auto& row : document["output"])
        {
            InvestorFlow investor_flow;
            investor_flow.date        = row.value("stck_bsop_date", "");
            investor_flow.foreign_net = to_int64(row, "frgn_ntby_qty");
            investor_flow.institution_net    = to_int64(row, "orgn_ntby_qty");
            investor_flow.individual_net   = to_int64(row, "prsn_ntby_qty");
            investor_flow.close       = number_of(row, "stck_clpr");

            if (!investor_flow.date.empty())
            {
                result.push_back(std::move(investor_flow));
            }
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[KIS] 투자자 시계열(" + ticker + ") 오류: " + exception.what());
    }

    return result; // [0]=가장 최근, look-ahead 방지는 호출측 책임
}

// ─── 지수 현재값 (코스피 "0001", 코스닥 "1001", KOSPI200 "2001") ────────────
KisClient::IndexPrice KisClient::get_index_price(const std::string& ticker)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-index-price"
                      + "?FID_COND_MRKT_DIV_CODE=U&FID_INPUT_ISCD=" + ticker;

    IndexPrice index_price;
    index_price.ticker = ticker;

    try
    {
        auto response = http_get(url, authentication_headers("FHPUP02100000", {"Content-Type: application/json"}));
        auto document = json::parse(response, nullptr, false);

        if (document.is_discarded() || !document.contains("output"))
        {
            return index_price;
        }

        auto& output_node = document["output"];
        auto number_of = [&](const char* key) -> double
        {
            try
            {
                return std::stod(output_node.value(key, "0"));
            }
            catch (...)
            {
                return 0.0;
            }
        };
        index_price.price = number_of("bstp_nmix_prpr");
        index_price.change = number_of("bstp_nmix_prdy_vrss");
        index_price.change_rate = number_of("bstp_nmix_prdy_ctrt");

        try
        {
            index_price.sign = std::stoi(output_node.value("prdy_vrss_sign", "3"));
        }
        catch (...)
        {
            index_price.sign = 3;
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[KIS] get_index_price(" + ticker + ") 실패: " + exception.what());
    }

    return index_price;
}

KisClient::FuturePrice KisClient::get_future_price(const std::string& issue_code, const std::string& market_div)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-futureoption/v1/quotations/inquire-price"
                      + "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_INPUT_ISCD=" + issue_code;

    FuturePrice future_price;
    future_price.issue_code = issue_code;

    try
    {
        auto response = http_get(url, authentication_headers("FHMIF10000000", {"Content-Type: application/json"}));
        auto document = json::parse(response, nullptr, false);

        if (document.is_discarded())
        {
            LOG_WARN("[KIS] get_future_price(" + issue_code + ") JSON 파싱 불가: " + response.substr(0, 200));
            return future_price;
        }

        // 스키마 확정됨(2026-09-03). 첫 호출 1회 raw output 덤프 유지 — 스키마 변동/디버그 대비.
        static bool dumped = false;

        if (!dumped)
        {
            dumped = true;
            std::string dump;

            for (const char* key : {"output1", "output2", "output3", "output"})
            {
                if (document.contains(key))
                {
                    dump.append(key).append("=").append(document[key].dump()).append("  ");
                }
            }

            LOG_INFO("[KIS] get_future_price RAW " + (dump.empty() ? response.substr(0, 500) : dump));
        }

        // 시세를 담은 output 객체 탐색: 가격 필드(futs_prpr)를 가진 객체를 우선 확정,
        // 없으면 output2→output1→output 순서의 첫 객체.
        const json* node = nullptr;

        for (const char* key : {"output2", "output1", "output"})
        {
            if (document.contains(key) && document[key].is_object())
            {
                if (!node)
                {
                    node = &document[key];
                }

                if (document[key].contains("futs_prpr"))
                {
                    node = &document[key];
                    break;
                }
            }
        }

        if (!node)
        {
            return future_price;
        }

        auto number_of = [&](const char* key) -> double {
            try { return std::stod((*node).value(key, "0")); } catch (...) { return 0.0; }
        };
        auto sll = [&](const char* key) -> int64_t {
            try { return std::stoll((*node).value(key, "0")); } catch (...) { return 0; }
        };

        future_price.price         = number_of("futs_prpr");
        future_price.change        = number_of("futs_prdy_vrss");
        future_price.change_rate   = number_of("futs_prdy_ctrt");

        try { future_price.sign = std::stoi((*node).value("prdy_vrss_sign", "3")); } catch (...) { future_price.sign = 3; }
        future_price.open          = number_of("futs_oprc");
        future_price.high          = number_of("futs_hgpr");
        future_price.low           = number_of("futs_lwpr");
        future_price.volume        = sll("acml_vol");
        future_price.open_interest = sll("hts_otst_stpl_qty");
        future_price.ok            = (future_price.price != 0.0);
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[KIS] get_future_price(" + issue_code + ") 실패: " + exception.what());
    }

    return future_price;
}

KisResult<std::vector<FutureContract>> KisClient::get_future_board(const std::string& market_cls,
                                                                   const std::string& market_div)
{
    ensure_authenticated();

    std::string url = base_url() + "/uapi/domestic-futureoption/v1/quotations/display-board-futures"
                      + "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20503"
                      + "&FID_COND_MRKT_CLS_CODE=" + market_cls;

    try
    {
        auto response = http_get(url, authentication_headers("FHPIF05030200", {"Content-Type: application/json"}));
        auto document = json::parse(response, nullptr, false);

        if (document.is_discarded())
        {
            LOG_WARN("[KIS] get_future_board JSON 파싱 불가: " + response.substr(0, 200));
            return kis_fail("parse", response.substr(0, 200));
        }

        if (document.value("rt_cd", "0") != "0")
        {
            LOG_WARN("[KIS] get_future_board 응답 오류 " + document.value("msg_cd", "") + " " + document.value("msg1", ""));
            return kis_fail(document.value("msg_cd", "rt_cd"), document.value("msg1", ""));
        }

        // 스키마 변동 대비: 프로세스당 첫 응답 한 번은 raw를 남긴다(get_future_price와 같은 규칙).
        static bool dumped = false;

        if (!dumped)
        {
            dumped = true;
            LOG_INFO("[KIS] get_future_board RAW " + response.substr(0, 500));
        }

        return kis_rest::decode_future_board(document);
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[KIS] get_future_board 실패: ") + exception.what());
        return kis_fail("transport", exception.what());
    }
}
