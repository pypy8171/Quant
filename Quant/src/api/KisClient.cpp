#include "api/KisClient.h"
#include "utils/Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
//  플랫폼별 HTTP 구현
// ═══════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
// ─── Windows: WinHTTP (Windows SDK 내장, 추가 설치 불필요) ─────────────────
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

static std::wstring to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

struct WinHttpResult { std::wstring host; std::wstring path; INTERNET_PORT port; bool https; };

static WinHttpResult crack_url(const std::string& url) {
    WinHttpResult r{};
    std::wstring wurl = to_wstring(url);
    wchar_t host[512]{}, path[4096]{};
    URL_COMPONENTS uc{};
    uc.dwStructSize    = sizeof(uc);
    uc.lpszHostName    = host; uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath     = path; uc.dwUrlPathLength  = (DWORD)std::size(path);
    WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc);
    r.host  = host;
    r.path  = path;
    r.port  = uc.nPort;
    r.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return r;
}

static std::string winhttp_request(const std::string& method,
                                   const std::string& url,
                                   const std::vector<std::string>& headers,
                                   const std::string& body) {
    auto c = crack_url(url);

    HINTERNET hSession = WinHttpOpen(L"QuantTrader/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, c.host.c_str(), c.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = c.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, to_wstring(method).c_str(),
        c.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    for (auto& h : headers) {
        auto wh = to_wstring(h + "\r\n");
        WinHttpAddRequestHeaders(hReq, wh.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    LPVOID pBody  = body.empty() ? nullptr : (LPVOID)body.c_str();
    DWORD  cbBody = (DWORD)body.size();
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pBody, cbBody, cbBody, 0)) {
        DWORD err = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] SendRequest 실패: %lu", err);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return "";
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD err = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] ReceiveResponse 실패: %lu", err);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return "";
    }
    // HTTP 상태 코드 확인 (4xx/5xx도 body를 읽어야 함)
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode >= 400) {
        char errbuf[32]; snprintf(errbuf, sizeof(errbuf), "[WinHTTP] HTTP %lu", statusCode);
        LOG_WARN(std::string(errbuf) + "  url=" + url);
    }

    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(hReq, &chunk[0], avail, &read);
        response.append(chunk, 0, read);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

#else
// ─── Linux: libcurl ────────────────────────────────────────────────────────
#include <curl/curl.h>

static size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string curl_request(const std::string& method,
                                const std::string& url,
                                const std::vector<std::string>& headers,
                                const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_slist* hlist = nullptr;
    for (auto& h : headers) hlist = curl_slist_append(hlist, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        LOG_ERROR(std::string("[CURL] 요청 실패: ") + curl_easy_strerror(rc));

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  KisClient 구현
// ═══════════════════════════════════════════════════════════════════════════

KisClient::KisClient(const KisConfig& cfg) : cfg_(cfg) {
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

KisClient::~KisClient() {
#ifndef _WIN32
    curl_global_cleanup();
#endif
}

// 토큰 캐시 파일: app_key 앞 8자리로 구분 (실계좌/모의 혼용 방지)
static std::string token_cache_path(const std::string& app_key) {
    return "kis_token_" + app_key.substr(0, 8) + ".json";
}

bool KisClient::authenticate() {
    // ── 캐시 파일에 유효한 토큰이 있으면 재사용 ──────────────────────────
    std::string cache_path = token_cache_path(cfg_.app_key);
    {
        std::ifstream f(cache_path);
        if (f.is_open()) {
            try {
                auto j = json::parse(f);
                std::string token   = j.value("access_token", "");
                std::string expires = j.value("expires_at", "");  // ISO "YYYY-MM-DD HH:MM:SS"

                if (!token.empty() && !expires.empty()) {
                    // 만료 시각 파싱
                    struct tm tm_exp{};
                    int y, mo, d, h, mi, s;
                    if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d",
                               &y, &mo, &d, &h, &mi, &s) == 6) {
                        tm_exp.tm_year = y - 1900;
                        tm_exp.tm_mon  = mo - 1;
                        tm_exp.tm_mday = d;
                        tm_exp.tm_hour = h;
                        tm_exp.tm_min  = mi;
                        tm_exp.tm_sec  = s;
                        tm_exp.tm_isdst = -1;
                        auto exp_t = std::mktime(&tm_exp);
                        auto now_t = std::time(nullptr);
                        // 만료 10분 전까지 사용
                        if (exp_t - now_t > 600) {
                            access_token_ = token;
                            LOG_INFO("[KIS] 캐시 토큰 재사용 (만료: " + expires + ")");
                            return true;
                        }
                    }
                }
            } catch (...) {}
        }
    }

    // ── 새 토큰 발급 ─────────────────────────────────────────────────────
    json body = {
        {"grant_type", "client_credentials"},
        {"appkey",     cfg_.app_key},
        {"appsecret",  cfg_.app_secret}
    };

    std::string url  = base_url() + "/oauth2/tokenP";
    std::string resp = http_post(url, {"Content-Type: application/json"}, body.dump());

    if (resp.empty()) {
        LOG_ERROR("[KIS] 토큰 발급 요청 실패");
        return false;
    }

    try {
        auto j = json::parse(resp);
        access_token_ = j["access_token"].get<std::string>();

        // 만료 시각 저장 (KIS 응답 필드: access_token_token_expired)
        std::string expires = j.value("access_token_token_expired", "");

        // 캐시 파일에 저장
        json cache_j = {
            {"access_token", access_token_},
            {"expires_at",   expires}
        };
        std::ofstream cf(cache_path);
        if (cf.is_open()) cf << cache_j.dump(2);

        LOG_INFO("[KIS] 토큰 발급 성공 (만료: " + expires + ")");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[KIS] 토큰 파싱 오류: ") + e.what());
        return false;
    }
}

std::vector<MarketData> KisClient::get_daily_ohlcv(const std::string& ticker, int count) {
    std::string url = base_url()
        + "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice"
        + "?FID_COND_MRKT_DIV_CODE=J"
        + "&FID_INPUT_ISCD=" + ticker
        + "&FID_INPUT_DATE_1=19000101"
        + "&FID_INPUT_DATE_2=99991231"
        + "&FID_PERIOD_DIV_CODE=D"
        + "&FID_ORG_ADJ_PRC=0";

    std::vector<std::string> headers = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST03010100"
    };

    std::string resp = http_get(url, headers);
    std::vector<MarketData> result;
    if (resp.empty()) {
        LOG_ERROR("[KIS] 일봉 조회 실패: " + ticker);
        return result;
    }

    try {
        auto j = json::parse(resp);
        auto& arr = j["output2"];
        int fetched = 0;
        for (auto& item : arr) {
            if (fetched++ >= count) break;
            MarketData md;
            md.ticker    = ticker;
            md.close     = std::stod(item["stck_clpr"].get<std::string>());
            md.open      = std::stod(item["stck_oprc"].get<std::string>());
            md.high      = std::stod(item["stck_hgpr"].get<std::string>());
            md.low       = std::stod(item["stck_lwpr"].get<std::string>());
            md.volume    = std::stoll(item["acml_vol"].get<std::string>());
            md.timestamp = std::chrono::system_clock::now();
            result.push_back(md);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[KIS] 일봉 파싱 오류: ") + e.what());
    }

    return result;
}

double KisClient::get_current_price(const std::string& ticker) {
    std::string url = base_url()
        + "/uapi/domestic-stock/v1/quotations/inquire-price"
        + "?FID_COND_MRKT_DIV_CODE=J"
        + "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST01010100"
    };

    std::string resp = http_get(url, headers);
    if (resp.empty()) return 0.0;

    try {
        auto j = json::parse(resp);
        return std::stod(j["output"]["stck_prpr"].get<std::string>());
    } catch (...) {
        return 0.0;
    }
}

Fundamentals KisClient::get_fundamentals(const std::string& ticker) {
    std::string url = base_url()
        + "/uapi/domestic-stock/v1/quotations/inquire-price"
        + "?FID_COND_MRKT_DIV_CODE=J"
        + "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST01010100"
    };

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, headers);
    if (resp.empty()) return f;

    try {
        auto j = json::parse(resp);
        auto& out = j["output"];
        auto parse_d = [&](const std::string& key) -> double {
            std::string s = out.value(key, "");
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };
        f.last = parse_d("stck_prpr");   // 현재가
        f.diff = parse_d("prdy_vrss");   // 전일대비
        f.rate = parse_d("prdy_ctrt");   // 등락률(%)
        f.open = parse_d("stck_oprc");   // 시가
        f.high = parse_d("stck_hgpr");   // 고가
        f.low  = parse_d("stck_lwpr");   // 저가
        f.pbr  = parse_d("pbr");
        f.per  = parse_d("per");
    } catch (...) {}

    return f;
}

bool KisClient::send_order(const OrderSignal& signal) {
    bool is_us = (signal.market == Market::US);
    std::string tr_id;
    std::string url;

    if (is_us) {
        // 해외주식 주문: NAS/NYS
        if (signal.side == OrderSide::BUY)
            tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
        else
            tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    } else {
        // 국내주식 현금 주문
        if (signal.side == OrderSide::BUY)
            tr_id = cfg_.is_paper ? "VTTC0802U" : "TTTC0802U";
        else
            tr_id = cfg_.is_paper ? "VTTC0801U" : "TTTC0801U";
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;
    if (is_us) {
        body = {
            {"CANO",         cfg_.account_no},
            {"ACNT_PRDT_CD", cfg_.account_type},
            {"OVRS_EXCG_CD", signal.exchange},
            {"PDNO",         signal.ticker},
            {"ORD_DVSN",     signal.type == OrderType::MARKET ? "00" : "00"},
            {"ORD_QTY",      std::to_string(signal.quantity)},
            {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT
                                  ? std::to_string(signal.price) : "0"}
        };
    } else {
        body = {
            {"CANO",         cfg_.account_no},
            {"ACNT_PRDT_CD", cfg_.account_type},
            {"PDNO",         signal.ticker},
            {"ORD_DVSN",     signal.type == OrderType::MARKET ? "01" : "00"},
            {"ORD_QTY",      std::to_string(signal.quantity)},
            {"ORD_UNPR",     signal.type == OrderType::LIMIT
                                 ? std::to_string((int)signal.price) : "0"}
        };
    }

    std::string resp = http_post(url,
        {
            "authorization: Bearer " + access_token_,
            "appkey: " + cfg_.app_key,
            "appsecret: " + cfg_.app_secret,
            "tr_id: " + tr_id,
            "Content-Type: application/json"
        },
        body.dump());

    if (resp.empty()) {
        LOG_ERROR("[KIS] 주문 실패: " + signal.ticker);
        return false;
    }

    auto j  = json::parse(resp);
    bool ok = (j["rt_cd"].get<std::string>() == "0");
    if (ok) {
        LOG_INFO("[KIS] 주문 성공: " + signal.ticker
            + (signal.side == OrderSide::BUY ? " BUY " : " SELL ")
            + std::to_string(signal.quantity) + "주");
    } else {
        LOG_ERROR("[KIS] 주문 오류: " + j["msg1"].get<std::string>());
    }
    return ok;
}

// ─── HTTP 래퍼 ────────────────────────────────────────────────────────────

std::string KisClient::http_get(const std::string& url,
                                const std::vector<std::string>& headers) {
    // KIS API는 GET에도 Content-Type: application/json 요구
    auto hdrs = headers;
    bool has_ct = false;
    for (auto& h : hdrs) if (h.find("Content-Type") != std::string::npos) { has_ct = true; break; }
    if (!has_ct) hdrs.push_back("Content-Type: application/json; charset=utf-8");
#ifdef _WIN32
    return winhttp_request("GET", url, hdrs, "");
#else
    return curl_request("GET", url, hdrs, "");
#endif
}

std::string KisClient::http_post(const std::string& url,
                                  const std::vector<std::string>& headers,
                                  const std::string& body) {
#ifdef _WIN32
    return winhttp_request("POST", url, headers, body);
#else
    return curl_request("POST", url, headers, body);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
//  국내 시가총액 순위 — 현재가·등락률·PBR 포함
//  tr_id: FHPST01720000
// ═══════════════════════════════════════════════════════════════════════════
std::vector<KisClient::RankingStock> KisClient::fetch_kr_ranking(
    int count, const std::string& market_div)
{
    std::string url = base_url()
        + "/uapi/domestic-stock/v1/ranking/market-cap"
        + "?FID_COND_MRKT_DIV_CODE=" + market_div
        + "&FID_COND_SCR_DIV_CODE=20171"
        + "&FID_INPUT_ISCD=0000"
        + "&FID_DIV_CLS_CODE=0"
        + "&FID_TRGT_CLS_CODE=0"
        + "&FID_RANK_SORT_CLS_CODE=0"
        + "&FID_INPUT_CNT_1=0"
        + "&FID_PBLICATN_NM=";

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHPST01720000"
    };

    std::string resp = http_get(url, hdrs);
    if (resp.empty()) {
        LOG_WARN("[KIS] 랭킹 조회 실패 (" + market_div + ")");
        return {};
    }
    LOG_INFO("[KIS] 랭킹 응답: " + resp.substr(0, 400));

    std::vector<RankingStock> result;
    try {
        auto j = json::parse(resp);
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double {
            std::string s = o.value(k, "");
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };
        auto safe_i = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            std::string s = o.value(k, "");
            if (s.empty()) return 0;
            try { return std::stoll(s); } catch (...) { return 0; }
        };

        int rank = 1;
        for (const auto& item : j["output2"]) {
            if (rank > count) break;
            RankingStock s;
            s.rank        = rank++;
            s.ticker      = item.value("mksc_shrn_iscd", "");
            s.name        = item.value("hts_kor_isnm", "");
            s.price       = safe_d(item, "stck_prpr");
            s.change      = safe_d(item, "prdy_vrss");
            s.change_rate = safe_d(item, "prdy_ctrt");
            s.volume      = safe_i(item, "acml_vol");
            s.pbr         = safe_d(item, "hts_pbr");
            if (!s.ticker.empty()) result.push_back(s);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[KIS] 랭킹 파싱 오류: " + std::string(e.what()));
    }

    LOG_INFO("[KIS] 랭킹 조회 완료: " + std::to_string(result.size()) + "종목");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  국내 Universe — PBR 필터 후 ticker 목록만 반환
// ═══════════════════════════════════════════════════════════════════════════
std::vector<std::string> KisClient::fetch_universe_by_pbr(
    double max_pbr, const std::string& market_div)
{
    std::string url = base_url()
        + "/uapi/domestic-stock/v1/ranking/market-cap"
        + "?FID_COND_MRKT_DIV_CODE=" + market_div
        + "&FID_COND_SCR_DIV_CODE=20171"
        + "&FID_INPUT_ISCD=0000"
        + "&FID_DIV_CLS_CODE=0"
        + "&FID_TRGT_CLS_CODE=0"
        + "&FID_RANK_SORT_CLS_CODE=0"
        + "&FID_INPUT_CNT_1=0"
        + "&FID_PBLICATN_NM=";

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHPST01720000"
    };

    std::string resp = http_get(url, hdrs);
    if (resp.empty()) {
        LOG_WARN("[KIS] Universe 조회 실패 (" + market_div + ")");
        return {};
    }

    LOG_INFO("[KIS] Universe(" + market_div + ") 응답: " + resp.substr(0, 300));

    std::vector<std::string> result;
    try {
        auto j = json::parse(resp);
        for (const auto& item : j["output2"]) {
            std::string ticker = item.value("mksc_shrn_iscd", "");
            if (ticker.empty()) continue;

            // PBR 필터: 유효한 값이 있을 때만 적용 (0.00 = 데이터 없음 → 통과)
            std::string pbr_s = item.value("hts_pbr", "");
            if (!pbr_s.empty() && pbr_s != "0" && pbr_s != "0.00") {
                try {
                    double pbr = std::stod(pbr_s);
                    if (pbr > max_pbr) continue;
                } catch (...) {}
            }
            result.push_back(ticker);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[KIS] Universe 파싱 오류: " + std::string(e.what()));
    }

    LOG_INFO("[KIS] Universe(" + market_div + ") PBR<=" + std::to_string(max_pbr)
             + " 통과: " + std::to_string(result.size()) + "종목");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 일봉 조회
//  tr_id: HHDFS76240000
// ═══════════════════════════════════════════════════════════════════════════
std::vector<MarketData> KisClient::get_us_daily_ohlcv(
    const std::string& ticker, int count, const std::string& exchange)
{
    std::string url = base_url()
        + "/uapi/overseas-price/v1/quotations/dailyprice"
        + "?AUTH="
        + "&EXCD=" + exchange
        + "&SYMB=" + ticker
        + "&GUBN=0"
        + "&BYMD="
        + "&MODP=0";

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: HHDFS76240000",
        "custtype: P"
    };

    std::string resp = http_get(url, hdrs);
    if (resp.empty()) { LOG_WARN("[KIS-US] OHLCV 응답 없음: " + ticker); return {}; }
    LOG_INFO("[KIS-US] OHLCV 응답(" + ticker + "): " + resp.substr(0, 300));

    std::vector<MarketData> result;
    try {
        auto j = json::parse(resp);
        if (!j.contains("output2") || !j["output2"].is_array()) {
            LOG_WARN("[KIS-US] output2 없음: " + ticker);
            return {};
        }
        for (const auto& item : j["output2"]) {
            MarketData md;
            md.ticker = ticker;
            md.market = Market::US;
            // KIS 해외 일봉 필드: clos/open/high/low/tvol
            auto parse_d = [](const json& o, const std::string& k) -> double {
                if (!o.contains(k)) return 0.0;
                std::string s = o[k].is_string() ? o[k].get<std::string>()
                                                  : o[k].dump();
                try { return s.empty() ? 0.0 : std::stod(s); } catch (...) { return 0.0; }
            };
            md.close  = parse_d(item, "clos");
            md.open   = parse_d(item, "open");
            md.high   = parse_d(item, "high");
            md.low    = parse_d(item, "low");
            md.volume = 0;
            try {
                std::string v = item.value("tvol", "0");
                if (!v.empty()) md.volume = std::stoll(v);
            } catch (...) {}
            md.timestamp = std::chrono::system_clock::now();
            if (md.close > 0.0) result.push_back(md);
            if ((int)result.size() >= count) break;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[KIS-US] OHLCV 파싱 오류 " + ticker + ": " + e.what());
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 펀더멘털 (PER / 현재가)
//  tr_id: HHDFS00000300
//  NOTE: KIS 해외주식 API는 pbr 미제공 → per 필드 활용
// ═══════════════════════════════════════════════════════════════════════════
Fundamentals KisClient::get_us_fundamentals(
    const std::string& ticker, const std::string& exchange)
{
    std::string url = base_url()
        + "/uapi/overseas-price/v1/quotations/price-detail"
        + "?AUTH=&EXCD=" + exchange + "&SYMB=" + ticker;

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: HHDFS00000300",
        "custtype: P"
    };

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, hdrs);
    if (resp.empty()) { LOG_WARN("[KIS-US] Fundamentals 응답 없음: " + ticker); return f; }
    LOG_INFO("[KIS-US] Fundamentals 응답(" + ticker + "): " + resp.substr(0, 300));

    try {
        auto j = json::parse(resp);
        // KIS 해외주식 현재가상세는 output1 키 사용
        const char* out_key = j.contains("output1") ? "output1"
                            : j.contains("output")  ? "output"
                            : nullptr;
        if (!out_key) { LOG_WARN("[KIS-US] output 키 없음: " + ticker); return f; }
        const auto& out = j[out_key];
        auto parse_dbl = [&](const std::string& key) -> double {
            std::string s = out.value(key, "");
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };
        auto parse_i64 = [&](const std::string& key) -> int64_t {
            std::string s = out.value(key, "");
            if (s.empty()) return 0;
            try { return std::stoll(s); } catch (...) { return 0; }
        };
        f.per  = parse_dbl("per");
        f.pbr  = parse_dbl("pbr");
        if (f.pbr == 0.0) f.pbr = parse_dbl("p_b_rate");
        f.last = parse_dbl("last");
        f.open = parse_dbl("open");
        f.high = parse_dbl("high");
        f.low  = parse_dbl("low");
        f.pbid = parse_dbl("pbid");
        f.pask = parse_dbl("pask");
        f.vbid = parse_i64("vbid");
        f.vask = parse_i64("vask");
        f.diff = parse_dbl("diff");
        f.rate = parse_dbl("rate");
    } catch (const std::exception& e) {
        LOG_ERROR("[KIS-US] Fundamentals 파싱 오류 " + ticker + ": " + e.what());
    }

    return f;
}

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 주문
//  매수: TTTT1002U(실거래) / VTTT1002U(모의)
//  매도: TTTT1006U(실거래) / VTTT1006U(모의)
// ═══════════════════════════════════════════════════════════════════════════
bool KisClient::send_us_order(const OrderSignal& signal) {
    std::string tr_id;
    if (signal.side == OrderSide::BUY)
        tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
    else
        tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";

    // KIS 해외주식 주문: 시장가 = ORD_DVSN "00", 가격 "0"
    json body = {
        {"CANO",         cfg_.account_no},
        {"ACNT_PRDT_CD", cfg_.account_type},
        {"OVRS_EXCG_CD", signal.exchange.empty() ? "NASD" : signal.exchange},
        {"PDNO",         signal.ticker},
        {"ORD_DVSN",     signal.type == OrderType::LIMIT ? "00" : "00"},
        {"ORD_QTY",      std::to_string(signal.quantity)},
        {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT
                              ? std::to_string(signal.price) : "0"}
    };

    std::string url  = base_url() + "/uapi/overseas-stock/v1/trading/order";
    std::string resp = http_post(url,
        {"authorization: Bearer " + access_token_,
         "appkey: " + cfg_.app_key,
         "appsecret: " + cfg_.app_secret,
         "tr_id: " + tr_id,
         "Content-Type: application/json"},
        body.dump());

    if (resp.empty()) { LOG_ERROR("[KIS-US] 주문 실패: " + signal.ticker); return false; }

    auto j  = json::parse(resp);
    bool ok = (j["rt_cd"].get<std::string>() == "0");
    if (ok)
        LOG_INFO("[KIS-US] 주문 성공: " + signal.ticker
                 + (signal.side == OrderSide::BUY ? " BUY " : " SELL ")
                 + std::to_string(signal.quantity) + "주");
    else
        LOG_ERROR("[KIS-US] 주문 오류: " + j.value("msg1", "unknown"));
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
//  미국 Universe — 내장 S&P 500 주요 100종목 + 3일 하락 필터
//  PBR은 KIS 해외주식 API에서 미제공 시 스킵 (pbr_max=0 → PBR 조건 무시)
// ═══════════════════════════════════════════════════════════════════════════
std::vector<std::string> KisClient::fetch_us_universe_by_pbr(
    double max_pbr, const std::string& exchange)
{
    // S&P 500 핵심 100종목 (가치주·성장주 혼합)
    static const std::vector<std::string> NAS_LIST = {
        "AAPL","MSFT","NVDA","AMZN","GOOGL","META","TSLA","AVGO","ORCL","ADBE",
        "AMD","QCOM","TXN","MU","INTC","AMAT","LRCX","KLAC","MRVL","SNPS",
        "V","MA","PYPL","INTU","CSCO","NFLX","COST","SBUX","PEP","MDLZ"
    };
    static const std::vector<std::string> NYS_LIST = {
        "BRK-B","JPM","BAC","WFC","GS","MS","C","AXP",
        "JNJ","LLY","ABBV","MRK","PFE","BMY","UNH","CVS",
        "PG","KO","WMT","HD","MCD","NKE","PEP","CL",
        "XOM","CVX","OXY","COP","SLB",
        "GE","CAT","HON","BA","MMM","UPS","FDX",
        "T","VZ","DIS","CMCSA",
        "BX","KKR","APO"
    };

    const auto& src = (exchange == "NYS") ? NYS_LIST : NAS_LIST;
    std::vector<std::string> result;

    for (const auto& tk : src) {
        if (max_pbr > 0.0) {
            auto f = get_us_fundamentals(tk, exchange);
            // KIS가 pbr 미제공(0.0)이면 PBR 조건 무시, per로 대리 (per=0이면 pass)
            if (f.pbr > 0.0 && f.pbr > max_pbr) continue;
            if (f.pbr == 0.0 && f.per > 0.0 && f.per > max_pbr * 10.0) continue;
        }
        result.push_back(tk);
        // KIS 초당 거래건수 제한 회피 (1req/200ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("[KIS-US] Universe(" + exchange + ") PBR<=" + std::to_string(max_pbr)
             + " 통과: " + std::to_string(result.size()) + "종목");
    return result;
}
