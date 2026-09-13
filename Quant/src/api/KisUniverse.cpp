// api/KisUniverse.cpp — 유니버스 후보 조회: 시총·거래대금·추정수급·업종 순위, PBR 필터, 미국 내장 목록.
//  ETF 접두사·미국 종목 폴백 리스트는 config JSON을 먼저 읽는다. [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"

// ═══════════════════════════════════════════════════════════════════════════
//  하드코딩 리스트 externalize — 외부 JSON에서 로드, 부재·오류 시 내장 폴백.
//  프로젝트 패턴(universe_scan.json)과 동일: ifstream + 파싱 + LOG_WARN 폴백.
//  파일 위치: 환경변수 $QUANT_CONFIG_DIR, 미설정 시 기본 "Quant/config".
//  폴백은 아래 내장 상수와 동일하므로 파일이 없어도 동작은 바이트 동일.
// ═══════════════════════════════════════════════════════════════════════════
namespace
{
// ETF/ETN 제외용 이름 접두사 — Quant/config/etf_prefixes.json 미존재 시 폴백.
const std::vector<std::string> ETF_PREFIXES_FALLBACK = {
    "KODEX",    "TIGER", "KINDEX", "KOSEF",  "ARIRANG",  "ACE",       "SOL",  "HANARO",
    "FOCUS",    "TREX",  "WON",    "PLUS",   "KoAct",    "TIMEFOLIO", "KTOP", "BIG",
    "히어로즈", "KCGI",  "파워",   "KBSTAR", "마이다스", "RISE",      "TRUE", "MASTER"};

// 미국 유니버스 — Quant/config/us_universe.json {"nasdaq":[...],"nyse":[...]} 미존재 시 폴백.
const std::vector<std::string> US_NAS_FALLBACK = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AVGO",
                                                  "ORCL", "ADBE", "AMD",  "QCOM", "TXN",   "MU",   "INTC", "AMAT",
                                                  "LRCX", "KLAC", "MRVL", "SNPS", "V",     "MA",   "PYPL", "INTU",
                                                  "CSCO", "NFLX", "COST", "SBUX", "PEP",   "MDLZ"};
const std::vector<std::string> US_NYS_FALLBACK = {
    "BRK-B", "JPM", "BAC", "WFC", "GS",  "MS",  "C",   "AXP", "JNJ", "LLY",   "ABBV", "MRK", "PFE", "BMY", "UNH",
    "CVS",   "PG",  "KO",  "WMT", "HD",  "MCD", "NKE", "PEP", "CL",  "XOM",   "CVX",  "OXY", "COP", "SLB", "GE",
    "CAT",   "HON", "BA",  "MMM", "UPS", "FDX", "T",   "VZ",  "DIS", "CMCSA", "BX",   "KKR", "APO"};

// 외부 JSON에서 문자열 리스트 로드. key 비면 top-level 배열, 아니면 object[key] 배열을 읽는다.
// 파일 부재·형식오류·빈배열이면 fallback 반환(LOG_WARN). 호출측 static 으로 최초 1회만 로드.
std::vector<std::string> load_str_list(const std::string& filename, const std::string& key,
                                       const std::vector<std::string>& fallback, const char* what)
{
    const char* dir = std::getenv("QUANT_CONFIG_DIR");
    std::string base = (dir && *dir) ? std::string(dir) : std::string("Quant/config");
    std::string path = base + "/" + filename;
    std::ifstream f(path);

    if (!f.is_open())
    {
        LOG_WARN(std::string("[KIS] ") + what + " 외부파일 없음(" + path + ") — 내장 기본값 " +
                 std::to_string(fallback.size()) + "개 사용");
        return fallback;
    }

    try
    {
        auto j = json::parse(f);
        const json* arr = nullptr;

        if (key.empty() && j.is_array())
        {
            arr = &j;
        }
        else if (!key.empty() && j.is_object() && j.contains(key) && j[key].is_array())
        {
            arr = &j[key];
        }

        std::vector<std::string> out;

        if (arr)
        {
            for (const auto& e : *arr)
            {
                if (e.is_string())
                {
                    out.push_back(e.get<std::string>());
                }
            }
        }

        if (out.empty())
        {
            LOG_WARN(std::string("[KIS] ") + what + " 외부파일 형식오류/빈값(" + path + ") — 내장 기본값 사용");
            return fallback;
        }

        LOG_INFO(std::string("[KIS] ") + what + " 외부파일 로드 " + std::to_string(out.size()) + "개 (" + path + ")");
        return out;
    }
    catch (const std::exception& e)
    {
        LOG_WARN(std::string("[KIS] ") + what + " 외부파일 파싱실패(" + path + ": " + e.what() + ") — 내장 기본값 사용");
        return fallback;
    }
}
} // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  국내 시가총액 순위 — 현재가·등락률·PBR 포함
//  tr_id: FHPST01720000
// ═══════════════════════════════════════════════════════════════════════════
std::vector<KisClient::RankingStock> KisClient::fetch_kr_ranking(int count, const std::string& market_div)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/ranking/market-cap" +
                      "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20171" +
                      "&FID_INPUT_ISCD=0000" + "&FID_DIV_CLS_CODE=1" + "&FID_BLNG_CLS_CODE=0" + "&FID_TRGT_CLS_CODE=0" +
                      "&FID_TRGT_EXLS_CLS_CODE=0" + "&FID_RANK_SORT_CLS_CODE=0" +
                      "&FID_INPUT_PRICE_1=" + "&FID_INPUT_PRICE_2=" + "&FID_VOL_CNT=" + "&FID_INPUT_DATE_1=";

    std::vector<std::string> hdrs = auth_headers("FHPST01720000");

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS] 랭킹 조회 실패 (" + market_div + ")");
        return {};
    }

    LOG_DEBUG("[KIS] 랭킹 응답: " + resp.substr(0, 400));

    std::vector<RankingStock> result;

    try
    {
        auto j = json::parse(resp);
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");

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
        auto safe_i = [](const nlohmann::json& o, const std::string& k) -> int64_t
        {
            std::string s = o.value(k, "");

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

        // ETF/ETN/ELW 제외: 브랜드 접두사(경계검사) + 상품 토큰(채권·액티브·레버리지…) + 6자리 숫자 티커.
        //  접두사는 KODEX·TIGER 등 브랜드를, 토큰은 접두사 목록 밖 비브랜드 액티브(KIWOOM 단기채권ESG액티브 등)를 잡는다.
        static const std::vector<std::string> ETF_PREFIXES =
            load_str_list("etf_prefixes.json", "", ETF_PREFIXES_FALLBACK, "ETF 접두");
        static const std::vector<std::string> ETF_TOKENS =
            load_str_list("etf_name_tokens.json", "", etf_filter::default_tokens(), "ETF 토큰");
        auto is_etf_name = [&](const std::string& name)
        { return etf_filter::is_etf_like(name, ETF_PREFIXES, ETF_TOKENS); };
        // KOSPI 보통주 티커는 반드시 6자리 숫자
        auto is_normal_ticker = [](const std::string& t)
        {
            if (t.size() != 6)
            {
                return false;
            }

            for (char c : t)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }

            return true;
        };

        // API 응답 키: "output" (단일 배열)
        auto& arr = j.contains("output2") ? j["output2"] : j["output"];
        int drop_etf = 0, drop_ticker = 0; // 진단: raw 행이 어디서 새는지 계측

        for (const auto& item : arr)
        {
            std::string name = item.value("hts_kor_isnm", "");
            std::string ticker = item.value("mksc_shrn_iscd", "");

            if (!is_normal_ticker(ticker)) { ++drop_ticker; continue; }

            if (is_etf_name(name)) { ++drop_etf; continue; }

            RankingStock s;
            s.ticker = ticker;
            s.name = name;
            s.price = safe_d(item, "stck_prpr");
            s.change = safe_d(item, "prdy_vrss");
            s.change_rate = safe_d(item, "prdy_ctrt");
            s.volume = safe_i(item, "acml_vol");
            s.pbr = safe_d(item, "hts_pbr");
            s.per = safe_d(item, "hts_per");
            result.push_back(s);
        }

        // 진단: raw 행수 vs 필터 후. raw가 ~30 고정이면 페이지네이션 필요, ETF드롭이 크면 API단 제외로 회복.
        LOG_INFO("[KIS] 시총랭킹 진단: raw=" + std::to_string(arr.size()) +
                 " ETF드롭=" + std::to_string(drop_etf) + " 티커드롭=" + std::to_string(drop_ticker) +
                 " 생존=" + std::to_string(result.size()) + " (요청 count=" + std::to_string(count) + ")");

        // API 정렬 기준이 불명확하므로 거래대금(가격×거래량) 내림차순 정렬 — 시가총액 대용
        std::ranges::sort(result, std::ranges::greater{},
                          [](const RankingStock& s) { return s.price * static_cast<double>(s.volume); });

        // count개로 자르고 순위 재부여
        if (static_cast<int>(result.size()) > count)
        {
            result.resize(count);
        }

        for (int i = 0; i < static_cast<int>(result.size()); ++i)
        {
            result[i].rank = i + 1;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS] 랭킹 파싱 오류: " + std::string(e.what()));
    }

    LOG_INFO("[KIS] 랭킹 조회 완료: " + std::to_string(result.size()) + "종목");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  국내 거래대금 상위 순위 — volume-rank API
//  tr_id: FHPST01710000, FID_BLNG_CLS_CODE=3(거래금액순). acml_tr_pbmn 직접 사용.
//  ⚠️ 응답 스키마 변동 잦음 — 첫 400자 로깅으로 필드/행수 확인. 상위 ~30행 고정 반환.
// ═══════════════════════════════════════════════════════════════════════════
std::vector<KisClient::RankingStock> KisClient::fetch_value_ranking(int count, const std::string& market_div,
                                                                   const std::string& blng_cls)
{
    // FID_TRGT_CLS_CODE(대상 9자리)/EXLS(제외 6자리)는 전체 대상 기본값.
    // FID_BLNG_CLS_CODE 정렬축: 0=거래량 1=거래증가율 3=거래금액(기본) — 호출자가 지정.
    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/volume-rank" +
                      "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20171" +
                      "&FID_INPUT_ISCD=0000" + "&FID_DIV_CLS_CODE=0" + "&FID_BLNG_CLS_CODE=" + blng_cls +
                      "&FID_TRGT_CLS_CODE=111111111" + "&FID_TRGT_EXLS_CLS_CODE=000000" +
                      "&FID_INPUT_PRICE_1=" + "&FID_INPUT_PRICE_2=" + "&FID_VOL_CNT=" + "&FID_INPUT_DATE_1=";

    std::vector<std::string> hdrs = auth_headers("FHPST01710000");

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS] 거래대금 랭킹 조회 실패 (" + market_div + ")");
        return {};
    }

    LOG_DEBUG("[KIS] 거래대금 랭킹 응답: " + resp.substr(0, 400));

    std::vector<RankingStock> result;

    try
    {
        auto j = json::parse(resp);
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");

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
        auto safe_i = [](const nlohmann::json& o, const std::string& k) -> int64_t
        {
            std::string s = o.value(k, "");

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

        // ETF/ETN/ELW 제외: 브랜드 접두사(경계검사) + 상품 토큰 + 6자리 숫자 티커(fetch_kr_ranking과 동일 규칙)
        static const std::vector<std::string> ETF_PREFIXES =
            load_str_list("etf_prefixes.json", "", ETF_PREFIXES_FALLBACK, "ETF 접두");
        static const std::vector<std::string> ETF_TOKENS =
            load_str_list("etf_name_tokens.json", "", etf_filter::default_tokens(), "ETF 토큰");
        auto is_etf_name = [&](const std::string& name)
        { return etf_filter::is_etf_like(name, ETF_PREFIXES, ETF_TOKENS); };
        auto is_normal_ticker = [](const std::string& t)
        {
            if (t.size() != 6)
            {
                return false;
            }

            for (char c : t)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }

            return true;
        };

        // volume-rank 응답 배열 키: "output" (표준). output2도 방어적으로 수용.
        auto& arr = j.contains("output") ? j["output"] : j["output2"];
        int drop_etf = 0, drop_ticker = 0; // 진단: raw 행이 어디서 새는지 계측

        for (const auto& item : arr)
        {
            std::string name = item.value("hts_kor_isnm", "");
            // 티커 키가 mksc_shrn_iscd 또는 stck_shrn_iscd 둘 다 관측됨 → 양쪽 시도
            std::string ticker = item.value("mksc_shrn_iscd", "");

            if (ticker.empty())
            {
                ticker = item.value("stck_shrn_iscd", "");
            }

            if (!is_normal_ticker(ticker)) { ++drop_ticker; continue; }

            if (is_etf_name(name)) { ++drop_etf; continue; }

            RankingStock s;
            s.ticker = ticker;
            s.name = name;
            s.price = safe_d(item, "stck_prpr");
            s.change = safe_d(item, "prdy_vrss");
            s.change_rate = safe_d(item, "prdy_ctrt");
            s.volume = safe_i(item, "acml_vol");
            s.trade_value = safe_d(item, "acml_tr_pbmn"); // 누적 거래대금(원)
            result.push_back(s);
        }

        // 진단: raw 행수 vs 필터 후. raw가 ~30 고정이면 페이지네이션 필요, ETF드롭이 크면 API단 제외로 회복.
        LOG_INFO("[KIS] 거래대금랭킹 진단(축=" + blng_cls + "): raw=" + std::to_string(arr.size()) +
                 " ETF드롭=" + std::to_string(drop_etf) + " 티커드롭=" + std::to_string(drop_ticker) +
                 " 생존=" + std::to_string(result.size()) + " (요청 count=" + std::to_string(count) + ")");

        // 거래대금축(3)일 때만 acml_tr_pbmn 내림차순 재정렬. 다른 축(거래량0·거래증가율1)은
        // trade_value가 비어 있어 재정렬하면 순서가 망가지므로 API 순위 순서를 그대로 유지.
        if (blng_cls == "3")
        {
            std::ranges::sort(result, std::ranges::greater{}, &RankingStock::trade_value);
        }

        if (static_cast<int>(result.size()) > count)
        {
            result.resize(count);
        }

        for (int i = 0; i < static_cast<int>(result.size()); ++i)
        {
            result[i].rank = i + 1;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS] 거래대금 랭킹 파싱 오류: " + std::string(e.what()));
    }

    LOG_INFO("[KIS] 거래대금 랭킹 조회 완료: " + std::to_string(result.size()) + "종목");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  당일 장중 외국인·기관 "추정(가집계)" 순매수 랭킹 — 배치 1콜
//  tr_id: FHPTJ04400000 (foreign-institution-total). 실전 도메인 전용.
//  ⚠️ 파라미터/필드명 미확정 — 첫 성공 응답 1회를 원문 로깅해 스키마 확정할 것.
// ═══════════════════════════════════════════════════════════════════════════
std::vector<KisClient::EstInvestorFlow> KisClient::fetch_est_investor_ranking(
    const std::string& market, const std::string& sort, const std::string& etc_cls)
{
    // FID_COND_MRKT_DIV_CODE=V(장중 추정), SCR_DIV=16449, ISCD=market, DIV_CLS=0(수량),
    // RANK_SORT=sort(0 순매수상위/1 순매도상위), ETC_CLS=etc_cls(0 전체/1 외국인/2 기관)
    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/foreign-institution-total" +
                      "?FID_COND_MRKT_DIV_CODE=V" + "&FID_COND_SCR_DIV_CODE=16449" +
                      "&FID_INPUT_ISCD=" + market + "&FID_DIV_CLS_CODE=0" +
                      "&FID_RANK_SORT_CLS_CODE=" + sort + "&FID_ETC_CLS_CODE=" + etc_cls;

    std::vector<std::string> hdrs = auth_headers("FHPTJ04400000");

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS] 당일 수급 랭킹 조회 실패 (sort=" + sort + " etc=" + etc_cls + ")");
        return {};
    }

    std::vector<EstInvestorFlow> result;

    try
    {
        auto j = json::parse(resp);
        // 스키마 확정 전: 파싱 결과가 비면 원문을 로깅해 필드명/구조를 눈으로 확인한다.
        auto safe_i = [](const nlohmann::json& o, const std::string& k) -> int64_t
        {
            std::string s = o.value(k, "");

            if (s.empty())
            {
                return 0;
            }

            try { return std::stoll(s); } catch (...) { return 0; }
        };
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");

            if (s.empty())
            {
                return 0.0;
            }

            try { return std::stod(s); } catch (...) { return 0.0; }
        };

        const nlohmann::json* arr = nullptr;

        if (j.contains("output"))
        {
            arr = &j["output"];
        }
        else if (j.contains("output1"))
        {
            arr = &j["output1"];
        }
        else if (j.contains("output2"))
        {
            arr = &j["output2"];
        }

        if (arr && arr->is_array())
        {
            for (const auto& item : *arr)
            {
                EstInvestorFlow f;
                f.ticker = item.value("mksc_shrn_iscd", "");

                if (f.ticker.empty())
                {
                    f.ticker = item.value("stck_shrn_iscd", "");
                }

                f.name = item.value("hts_kor_isnm", "");
                f.foreign_net_qty = safe_i(item, "frgn_ntby_qty");
                f.inst_net_qty    = safe_i(item, "orgn_ntby_qty");
                f.foreign_net_amt = safe_d(item, "frgn_ntby_tr_pbmn");
                f.inst_net_amt    = safe_d(item, "orgn_ntby_tr_pbmn");

                if (!f.ticker.empty())
                {
                    result.push_back(std::move(f));
                }
            }
        }

        if (result.empty())
        {
            LOG_WARN("[KIS] 당일 수급 랭킹 파싱 0건 — 스키마 확인용 원문: " + resp.substr(0, 500));
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS] 당일 수급 랭킹 파싱 오류: " + std::string(e.what()) + " 원문: " + resp.substr(0, 300));
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  국내 Universe — PBR 필터 후 ticker 목록만 반환
// ═══════════════════════════════════════════════════════════════════════════
std::vector<std::string> KisClient::fetch_universe_by_pbr(double max_pbr, const std::string& market_div)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/ranking/market-cap" +
                      "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20171" +
                      "&FID_INPUT_ISCD=0000" + "&FID_DIV_CLS_CODE=1" + "&FID_BLNG_CLS_CODE=0" + "&FID_TRGT_CLS_CODE=0" +
                      "&FID_TRGT_EXLS_CLS_CODE=0" + "&FID_RANK_SORT_CLS_CODE=0" +
                      "&FID_INPUT_PRICE_1=" + "&FID_INPUT_PRICE_2=" + "&FID_VOL_CNT=" + "&FID_INPUT_DATE_1=";

    std::vector<std::string> hdrs = auth_headers("FHPST01720000");

    std::string resp = http_get(url, hdrs);

    if (resp.empty())
    {
        LOG_WARN("[KIS] Universe 조회 실패 (" + market_div + ")");
        return {};
    }

    LOG_DEBUG("[KIS] Universe(" + market_div + ") 응답: " + resp.substr(0, 300));

    std::vector<std::string> result;

    try
    {
        auto j = json::parse(resp);
        auto& arr2 = j.contains("output2") ? j["output2"] : j["output"];

        for (const auto& item : arr2)
        {
            std::string ticker = item.value("mksc_shrn_iscd", "");

            if (ticker.empty())
            {
                continue;
            }

            // PBR 필터: 유효한 값이 있을 때만 적용 (0.00 = 데이터 없음 → 통과)
            std::string pbr_s = item.value("hts_pbr", "");

            if (!pbr_s.empty() && pbr_s != "0" && pbr_s != "0.00")
            {
                try
                {
                    double pbr = std::stod(pbr_s);

                    if (pbr > max_pbr)
                    {
                        continue;
                    }
                }
                catch (...)
                {
                }
            }

            result.push_back(ticker);
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[KIS] Universe 파싱 오류: " + std::string(e.what()));
    }

    LOG_INFO("[KIS] Universe(" + market_div + ") PBR<=" + std::to_string(max_pbr) +
             " 통과: " + std::to_string(result.size()) + "종목");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  미국 Universe — 내장 S&P 500 주요 100종목 + 3일 하락 필터
//  PBR은 KIS 해외주식 API에서 미제공 시 스킵 (pbr_max=0 → PBR 조건 무시)
// ═══════════════════════════════════════════════════════════════════════════
std::vector<std::string> KisClient::fetch_us_universe_by_pbr(double max_pbr, const std::string& exchange)
{
    // S&P 500 핵심 100종목 (가치주·성장주 혼합) — 외부 us_universe.json {"nasdaq","nyse"} 로드/폴백
    static const std::vector<std::string> NAS_LIST =
        load_str_list("us_universe.json", "nasdaq", US_NAS_FALLBACK, "US NASDAQ 유니버스");
    static const std::vector<std::string> NYS_LIST =
        load_str_list("us_universe.json", "nyse", US_NYS_FALLBACK, "US NYSE 유니버스");

    const auto& src = (exchange == "NYS") ? NYS_LIST : NAS_LIST;
    std::vector<std::string> result;

    for (const auto& tk : src)
    {
        if (max_pbr > 0.0)
        {
            auto f = get_us_fundamentals(tk, exchange);

            // KIS가 pbr 미제공(0.0)이면 PBR 조건 무시, per로 대리 (per=0이면 pass)
            if (f.pbr > 0.0 && f.pbr > max_pbr)
            {
                continue;
            }

            if (f.pbr == 0.0 && f.per > 0.0 && f.per > max_pbr * 10.0)
            {
                continue;
            }
        }

        result.push_back(tk);
        // KIS 초당 거래건수 제한 회피 (1req/200ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("[KIS-US] Universe(" + exchange + ") PBR<=" + std::to_string(max_pbr) +
             " 통과: " + std::to_string(result.size()) + "종목");
    return result;
}

// ─── 업종별 등락률 순위 ────────────────────────────────────────────────────
std::vector<KisClient::RankingStock> KisClient::fetch_sector_ranking(
    const std::string& sector_code, int count)
{
    ensure_authenticated();

    // 등락률 순위 API — FID_INPUT_ISCD에 업종코드 지정
    std::string url = base_url() +
        "/uapi/domestic-stock/v1/ranking/fluctuation"
        "?FID_COND_MRKT_DIV_CODE=J"
        "&FID_COND_SCR_DIV_CODE=20170"
        "&FID_INPUT_ISCD=" + sector_code +
        "&FID_RANK_SORT_CLS_CODE=0"   // 상승률 내림차순
        // FID_INPUT_CNT_1은 행수가 아니다. 응답은 값과 무관하게 30행이고, 값을 넣으면
        //  정렬 자체가 뒤틀린다(09-08 실측 0013: =10이면 top3이 전부 음수이고 광전자
        //  +17.07%가 아예 빠진다, =0이면 정상 내림차순). 비워 둔다.
        "&FID_INPUT_CNT_1="
        "&FID_PRC_CLS_CODE=0"
        "&FID_TRGT_CLS_CODE=0"
        "&FID_TRGT_EXLS_CLS_CODE=0"
        "&FID_DIV_CLS_CODE=0"
        "&FID_INPUT_PRICE_1=&FID_INPUT_PRICE_2="
        "&FID_VOL_CNT="
        "&FID_RSFL_RATE1=&FID_RSFL_RATE2=";

    std::vector<std::string> hdrs = auth_headers("FHPST01700000");

    std::vector<RankingStock> result;

    try
    {
        auto resp = http_get(url, hdrs);

        if (resp.empty())
        {
            return result;
        }

        LOG_DEBUG("[KIS] 업종 등락률 응답(" + sector_code + "): " + resp.substr(0, 300));

        auto j = json::parse(resp, nullptr, false);

        if (j.is_discarded())
        {
            return result;
        }

        if (j.value("rt_cd", std::string("0")) != "0")
        {
            LOG_WARN("[KIS] 업종 등락률(" + sector_code + ") 거부: rt_cd=" +
                     j.value("rt_cd", std::string()) + " " + j.value("msg1", std::string()));
            return result;
        }

        auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
            try { return std::stod(o.value(k, "0")); } catch (...) { return 0.0; }
        };
        auto si = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            try { return std::stoll(o.value(k, "0")); } catch (...) { return 0; }
        };
        auto is_normal_ticker = [](const std::string& t) {
            if (t.size() != 6)
            {
                return false;
            }

            for (char c : t)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }

            return true;
        };

        auto& arr = j.contains("output2") ? j["output2"] : j["output"];

        for (const auto& item : arr)
        {
            std::string ticker = item.value("mksc_shrn_iscd", "");

            if (ticker.empty())
            {
                ticker = item.value("stck_shrn_iscd", "");
            }

            if (!is_normal_ticker(ticker))
            {
                continue;
            }

            RankingStock s;
            s.ticker      = ticker;
            s.name        = item.value("hts_kor_isnm", "");
            s.price       = sd(item, "stck_prpr");
            s.change_rate = sd(item, "prdy_ctrt");
            s.volume      = si(item, "acml_vol");
            result.push_back(s);
        }

        // 30행을 다 받아놓고 앞의 count행만 쓰면 그 업종 최고 상승주를 버린다(이미 지불한
        //  호출이다). API 정렬도 완전한 내림차순이 아니어서(09-08 실측 0019: 제주항공
        //  6.05 다음이 동양고속 2.09, 그다음이 진에어 4.53) 여기서 다시 정렬한 뒤 자른다.
        std::ranges::sort(result, std::ranges::greater{}, &RankingStock::change_rate);

        if (static_cast<int>(result.size()) > count)
        {
            result.resize(count);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] 업종 등락률(" + sector_code + ") 오류: " + e.what());
    }

    return result;
}
