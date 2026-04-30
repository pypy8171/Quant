#include "api/KisClient.h"
#include "utils/Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

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
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pBody, cbBody, cbBody, 0)
        || !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
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

bool KisClient::authenticate() {
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
        LOG_INFO("[KIS] 토큰 발급 성공");
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

bool KisClient::send_order(const OrderSignal& signal) {
    std::string tr_id;
    if (signal.side == OrderSide::BUY) {
        tr_id = cfg_.is_paper ? "VTTC0802U" : "TTTC0802U";
    } else {
        tr_id = cfg_.is_paper ? "VTTC0801U" : "TTTC0801U";
    }

    json body = {
        {"CANO",         cfg_.account_no},
        {"ACNT_PRDT_CD", cfg_.account_type},
        {"PDNO",         signal.ticker},
        {"ORD_DVSN",     signal.type == OrderType::MARKET ? "01" : "00"},
        {"ORD_QTY",      std::to_string(signal.quantity)},
        {"ORD_UNPR",     signal.type == OrderType::LIMIT
                             ? std::to_string((int)signal.price) : "0"}
    };

    std::string url  = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
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
#ifdef _WIN32
    return winhttp_request("GET", url, headers, "");
#else
    return curl_request("GET", url, headers, "");
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
