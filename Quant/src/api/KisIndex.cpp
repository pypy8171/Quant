// api/KisIndex.cpp — 투자자 매매동향, 지수 현재값, 국내 선물 시세·전광판.
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#include "api/KisRestDecode.h"

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
            try
            {
                return std::stoll(node.value(key, "0"));
            }
            catch (...)
            {
                return 0;
            }
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
            try
            {
                return std::stod((*node).value(key, "0"));
            }
            catch (...)
            {
                return 0.0;
            }
        };
        auto sll = [&](const char* key) -> int64_t {
            try
            {
                return std::stoll((*node).value(key, "0"));
            }
            catch (...)
            {
                return 0;
            }
        };

        future_price.price         = number_of("futs_prpr");
        future_price.change        = number_of("futs_prdy_vrss");
        future_price.change_rate   = number_of("futs_prdy_ctrt");

        try
        {
            future_price.sign = std::stoi((*node).value("prdy_vrss_sign", "3"));
        }
        catch (...)
        {
            future_price.sign = 3;
        }

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
