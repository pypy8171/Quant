// api/KisTransport.cpp — HTTP 전송 한 겹: 플랫폼별 요청(WinHTTP/libcurl)·재시도·초당 한도·공용 인증 헤더.
//  모든 REST 호출은 http_get/http_post를 지난다. 스레드 공용(연결은 스레드별 캐시, 한도 버킷은 rate_mtx_).
//  KisClient 구현은 도메인별 7파일이다 — 목록은 Quant/src/api/KisClientInternal.h. [why D-048]
#include "KisClientInternal.h"

// 재시도 없이 즉시 실패 스코프 깊이(스레드별). 0보다 크면 조회 재시도를 하지 않는다.
static thread_local int g_fastfail_depth = 0;

KisClient::FastFailScope::FastFailScope() { ++g_fastfail_depth; }
KisClient::FastFailScope::~FastFailScope() { --g_fastfail_depth; }

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

static std::wstring to_wstring(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }

    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

struct WinHttpResult
{
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port;
    bool https;
};

static WinHttpResult crack_url(const std::string& url)
{
    WinHttpResult r{};
    std::wstring wurl = to_wstring(url);
    wchar_t host[512]{}, path[4096]{};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc);
    r.host = host;
    r.path = path;
    r.port = uc.nPort;
    r.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return r;
}

// ─── 커넥션 풀(P-2): 스레드별 WinHTTP 세션·연결 상주로 keep-alive 재사용 ────────
//  매 호출 hSession/hConnect를 새로 열면 주문·조회마다 TCP+TLS 핸드셰이크를 재지불한다.
//  hSession·hConnect를 thread_local로 상주시키고 hReq만 매번 생성한다. WINHTTP_DISABLE_KEEP_ALIVE를
//  걸지 않으므로 WinHTTP가 기저 TCP+TLS 연결을 keep-alive 풀에서 재사용한다.
//  스레드별 소유라 락이 없다(SPSC 파이프라인과 같은 기조: data·order 스레드가 각자 warm 연결을 가짐).
//  전송 계층 실패 시 캐시를 파기해 다음 호출이 새 연결을 맺는다(끊긴 keep-alive 복구).
namespace
{
struct WinHttpConn
{
    HINTERNET     session = nullptr;
    HINTERNET     connect = nullptr;
    std::wstring  host;
    INTERNET_PORT port = 0;

    ~WinHttpConn() { reset(); }
    void reset()
    {
        if (connect)
        {
            WinHttpCloseHandle(connect);
            connect = nullptr;
        }

        if (session)
        {
            WinHttpCloseHandle(session);
            session = nullptr;
        }

        host.clear();
        port = 0;
    }
};

// 스레드별 상주 연결. 호스트/포트가 바뀌면(현재는 사실상 단일 호스트라 최초 1회) 재수립한다.
static thread_local WinHttpConn t_conn;

// 풀링 해제 스위치. 환경변수 QUANT_HTTP_NOPOOL=1이면 매 요청 뒤 상주 연결을 파기해
// 풀링 도입 전(요청마다 TCP+TLS 재수립) 거동을 그대로 재현한다. 측정용으로만 쓴다 —
// 바이너리 하나에서 변수 하나(풀링 유무)만 바꿔 before/after를 비교하려는 목적.
static bool http_nopool()
{
    static const bool v = [] {
        const char* e = std::getenv("QUANT_HTTP_NOPOOL");
        return e && *e == '1';
    }();
    return v;
}

// (host,port)에 대한 상주 hConnect 확보. 실패 시 nullptr.
static HINTERNET acquire_connection(const WinHttpResult& c)
{
    if (t_conn.session && t_conn.connect && t_conn.host == c.host && t_conn.port == c.port)
    {
        return t_conn.connect; // 워밍된 연결 재사용
    }

    t_conn.reset(); // 최초 or 호스트 변경 → 재수립

    t_conn.session = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);

    if (!t_conn.session)
    {
        return nullptr;
    }

    // 명시적 타임아웃(ms): resolve/connect/send/receive. 기본값(무한대급)에서 하향해
    // 전송 계층 히컵이 스레드를 오래 잡지 않게 한다.
    WinHttpSetTimeouts(t_conn.session, 5000, 5000, 10000, 15000);

    t_conn.connect = WinHttpConnect(t_conn.session, c.host.c_str(), c.port, 0);

    if (!t_conn.connect)
    {
        t_conn.reset();
        return nullptr;
    }

    t_conn.host = c.host;
    t_conn.port = c.port;
    return t_conn.connect;
}
} // namespace

// 단발 시도. transport_ok = HTTP 응답을 실제로 받았는가(상태코드 무관, 4xx/5xx도 true).
//  false = 전송 계층 실패(핸들 생성/SendRequest/ReceiveResponse 실패 — 예: 12152). 이때만 재시도 대상.
//  hSession/hConnect는 상주(keep-alive)라 매 호출 hReq만 열고 닫는다. 전송 실패 시 상주 연결을 파기한다.
static std::string winhttp_request_once(const std::string& method, const std::string& url,
                                        const std::vector<std::string>& headers, const std::string& body,
                                        bool& transport_ok, int& status_code)
{
    transport_ok = false;
    status_code = 0;
    auto c = crack_url(url);

    HINTERNET hConnect = acquire_connection(c);

    if (!hConnect)
    {
        return "";
    }

    DWORD flags = c.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, to_wstring(method).c_str(), c.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hReq)
    {
        t_conn.reset(); // 상주 연결이 상해 있을 수 있음 → 파기, 다음 호출서 재수립
        return "";
    }

    for (auto& h : headers)
    {
        auto wh = to_wstring(h + "\r\n");
        WinHttpAddRequestHeaders(hReq, wh.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    LPVOID pBody = body.empty() ? nullptr : (LPVOID)body.c_str();
    DWORD cbBody = (DWORD)body.size();

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pBody, cbBody, cbBody, 0))
    {
        DWORD err = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] SendRequest 실패: %lu", err);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(hReq);
        t_conn.reset(); // 끊긴 keep-alive 가능 → 파기 후 재수립(GET이면 래퍼가 재시도)
        return "";
    }

    if (!WinHttpReceiveResponse(hReq, nullptr))
    {
        DWORD err = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] ReceiveResponse 실패: %lu", err);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(hReq);
        t_conn.reset();
        return "";
    }

    // 여기 도달 = HTTP 응답 수신 성공(상태코드는 아래에서 확인). 전송 계층은 정상.
    transport_ok = true;

    // HTTP 상태 코드 확인 (4xx/5xx도 body를 읽어야 함)
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    status_code = (int)statusCode;

    if (statusCode >= 400)
    {
        char errbuf[32];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] HTTP %lu", statusCode);
        LOG_WARN(std::string(errbuf) + "  url=" + url);
    }

    std::string response;
    DWORD avail = 0;

    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
    {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(hReq, &chunk[0], avail, &read);
        response.append(chunk, 0, read);
    }

    WinHttpCloseHandle(hReq); // hReq만 닫는다. hConnect/hSession은 상주(keep-alive 재사용).

    if (http_nopool())
    {
        t_conn.reset(); // 측정용 스위치 — 풀링 도입 전 거동(요청마다 재수립) 재현
    }

    return response;
}

// 초당 호출 한도 초과 신호. KIS는 이걸 HTTP 500으로도 돌려줘서 상태코드만으로는 일시 서버
//  장애와 구분이 안 된다 — 바디의 코드로 가른다. 한도 초과에 즉시 재시도하면 호출량을 1→3배로
//  늘려 초과를 더 키운다(양의 되먹임). 한도 창이 1초라 150·300ms 백오프도 같은 창 안에 떨어진다.
static bool is_rate_limited(const std::string& body)
{
    return body.find("EGW00201") != std::string::npos ||
           body.find("초당 거래건수") != std::string::npos;
}

// 재시도 래퍼. ⚠ 조회(GET) 요청(여러 번 보내도 서버 상태 불변이라 재시도 안전)만 재시도한다 — (a) 전송 계층 실패(12152 등), (b) 5xx 서버 일시장애.
//  KIS 시세/일봉 TR은 부하 시 간헐 HTTP 500을 뱉는데(전송은 정상, transport_ok=true), 이때 일봉이 <60봉으로
//  잘려 스캔 후보가 통째로 탈락한다 → 조회(GET)에 한해 5xx도 재시도해 후보 유실을 막는다.
//  주문 등 POST는 재시도하지 않는다 — 빈 응답(12152)이 "미접수"라는 보장이 없어(서버엔 접수됐을 수 있음)
//  블라인드 재시도는 이중주문 위험. POST 실패는 호출자가 잔고 대조로 확정해야 한다.
static std::string winhttp_request(const std::string& method, const std::string& url,
                                   const std::vector<std::string>& headers, const std::string& body)
{
    constexpr int      kMaxGetAttempts    = 3;   // 조회(GET) 최대 시도(원 시도 + 재시도 2)
    constexpr unsigned kRetryBackoffMsBase = 500; // 선형 백오프 기준(attempt배: 500ms, 1000ms)
    const bool idempotent = (method == "GET");
    const int max_attempts = (idempotent && g_fastfail_depth == 0) ? kMaxGetAttempts : 1;
    std::string resp;

    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        resp = winhttp_request_once(method, url, headers, body, transport_ok, status);
        // 재시도 대상: 전송 실패(항상) 또는 조회(GET)의 5xx. 그 외(2xx/4xx)는 즉시 반환.
        const bool retryable = !transport_ok || (idempotent && status >= 500);

        if (!retryable)
        {
            return resp;
        }

        // 한도 초과가 확인되면 한 번만 더 시도하고 그친다. 부하가 원인인 실패에 재시도를
        //  겹치면 부하를 더 얹는다.
        const bool rate_limited = is_rate_limited(resp);

        if (rate_limited && attempt >= 2)
        {
            return resp;
        }

        if (attempt < max_attempts)
        {
            LOG_WARN("[WinHTTP] " + std::string(transport_ok ? "HTTP " + std::to_string(status) : "전송 실패") +
                     (rate_limited ? " (초당 한도)" : "") +
                     " — 재시도 " + std::to_string(attempt + 1) + "/" + std::to_string(max_attempts) +
                     "  url=" + url);
            // 한도 창이 1초라 그보다 짧게 자면 같은 창에 다시 떨어진다.
            Sleep(rate_limited ? 1100u : kRetryBackoffMsBase * attempt);
        }
    }

    return resp; // 재시도 소진 — 마지막 응답(빈 문자열 또는 5xx 바디)
}

#else
// ─── Linux: libcurl ────────────────────────────────────────────────────────
#include <curl/curl.h>

static size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data)
{
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

// 단발 시도. transport_ok = HTTP 응답을 받았는가(CURLE_OK; 4xx/5xx도 true).
//  curl_easy_perform은 HTTP 응답을 받으면(상태코드 무관) CURLE_OK, 전송 실패(타임아웃·연결단절 등)만 비-OK.
static std::string curl_request_once(const std::string& method, const std::string& url,
                                     const std::vector<std::string>& headers, const std::string& body,
                                     bool& transport_ok, int& status_code)
{
    transport_ok = false;
    status_code = 0;
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        return "";
    }

    std::string response;
    curl_slist* hlist = nullptr;

    for (auto& h : headers)
    {
        hlist = curl_slist_append(hlist, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    if (method == "POST")
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK)
    {
        LOG_ERROR(std::string("[CURL] 요청 실패: ") + curl_easy_strerror(rc));
    }
    else
    {
        transport_ok = true;
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        status_code = (int)http_code;
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

// 재시도 래퍼. ⚠ 조회(GET) 요청만 재시도 — 전송 계층 실패 또는 5xx(WinHTTP 경로와 동일 규약 — 주문 POST 제외).
static std::string curl_request(const std::string& method, const std::string& url,
                                const std::vector<std::string>& headers, const std::string& body)
{
    constexpr int kMaxGetAttempts     = 3;   // 조회(GET) 최대 시도(원 시도 + 재시도 2)
    constexpr int kRetryBackoffMsBase = 500; // 선형 백오프 기준(attempt배: 500ms, 1000ms)
    const bool idempotent = (method == "GET");
    const int max_attempts = (idempotent && g_fastfail_depth == 0) ? kMaxGetAttempts : 1;
    std::string resp;

    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        resp = curl_request_once(method, url, headers, body, transport_ok, status);
        const bool retryable = !transport_ok || (idempotent && status >= 500);

        if (!retryable)
        {
            return resp;
        }

        const bool rate_limited = is_rate_limited(resp); // 한도 초과면 한 번만 더 시도

        if (rate_limited && attempt >= 2)
        {
            return resp;
        }

        if (attempt < max_attempts)
        {
            LOG_WARN("[CURL] " + std::string(transport_ok ? "HTTP " + std::to_string(status) : "전송 실패") +
                     (rate_limited ? " (초당 한도)" : "") +
                     " — 재시도 " + std::to_string(attempt + 1) + "/" + std::to_string(max_attempts) +
                     "  url=" + url);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(rate_limited ? 1100 : kRetryBackoffMsBase * attempt));
        }
    }

    return resp;
}
#endif

// ─── HTTP 래퍼 ────────────────────────────────────────────────────────────

// 초당 호출 한도를 넘지 않게 호출을 고르게 편다. KIS는 한도를 넘긴 요청에 HTTP 500이나
//  EGW00201을 돌려주는데, 어느 쪽이든 그 호출은 버려지고 재시도가 붙어 호출량이 더 는다.
//  버킷은 인스턴스(=app_key)마다 따로다 — 한도가 app_key 단위라 시세 클라이언트와 주문
//  클라이언트의 예산은 서로 무관하다. 총 호출량을 줄이지는 못하고 순서만 고르게 만든다.
void KisClient::rate_limit_acquire(const std::string& url)
{
    // 실전 초당 20건, 모의 초당 2건이 공표 한도다. 재시도·토큰 갱신이 끼어들 여유를 남겨 낮게 잡는다.
    const double refill = cfg_.is_paper ? 2.0 : 15.0;
    const double cap = refill; // 1초치까지만 모아둔다(그 이상 몰아치면 어차피 한도에 걸린다)
    // 주문·잔고 경로에는 예약분을 남긴다. 시세 조회가 버킷을 다 비운 순간 청산 주문이
    //  그 뒤에 줄서면 몇 백 ms가 늦는데, 그 지연은 조회 지연과 값이 다르다.
    const bool priority = url.find("/trading/") != std::string::npos;
    const double need = priority ? 1.0 : 2.0;

    while (true) // while (1)
    {
        double wait_sec = 0.0;
        {
            std::lock_guard<std::mutex> lk(rate_mtx_);
            auto now = std::chrono::steady_clock::now();

            if (rate_last_.time_since_epoch().count() == 0)
            {
                rate_last_ = now;
                rate_tokens_ = cap; // 첫 호출은 기다리지 않는다
            }

            double elapsed = std::chrono::duration<double>(now - rate_last_).count();
            rate_last_ = now;
            rate_tokens_ = (std::min)(cap, rate_tokens_ + elapsed * refill);

            if (rate_tokens_ >= need)
            {
                rate_tokens_ -= 1.0;
                return;
            }

            wait_sec = (need - rate_tokens_) / refill;
        }
        
        if (wait_sec > 0.2)
        {
            wait_sec = 0.2; // 재확인 주기 상한 — 먼저 기다리던 호출이 풀렸을 수 있다
        }

        std::this_thread::sleep_for(std::chrono::duration<double>(wait_sec));
    }
}

void KisClient::note_rate_limited()
{
    std::lock_guard<std::mutex> lk(rate_mtx_);
    rate_tokens_ = 0.0; // 다음 호출은 리필을 기다린다(≈1초치)
}

// 헤더 목록의 authorization 줄을 지금 토큰으로 덮어쓴다. 호출자들은 헤더를 먼저 조립하고
//  http_get/http_post가 그 뒤에 ensure_authenticated()를 부르므로, 갱신이 일어난 요청은 옛 토큰
//  (기동 직후 첫 호출이면 빈 토큰)으로 나간다. authorization 줄이 없는 헤더(oauth2)는 그대로 둔다.
static void kis_stamp_bearer(std::vector<std::string>& hdrs, const std::string& tok)
{
    for (auto& h : hdrs)
    {
        if (h.rfind("authorization:", 0) == 0 || h.rfind("Authorization:", 0) == 0)
        {
            h = "authorization: Bearer " + tok;
            return;
        }
    }
}

std::string KisClient::http_get(const std::string& url, const std::vector<std::string>& headers)
{
    auto hdrs = headers;

    // oauth2 토큰 발급 엔드포인트가 아닌 경우에만 자동 갱신 (재귀 방지)
    if (url.find("oauth2") == std::string::npos)
    {
        ensure_authenticated();
        kis_stamp_bearer(hdrs, token());
    }

    rate_limit_acquire(url);

    // KIS API는 GET에도 Content-Type: application/json 요구
    bool has_ct = false;

    for (auto& h : hdrs)
    {
        if (h.find("Content-Type") != std::string::npos)
        {
            has_ct = true;
            break;
        }
    }

    if (!has_ct)
    {
        hdrs.push_back("Content-Type: application/json; charset=utf-8");
    }
#ifdef _WIN32
    std::string resp = winhttp_request("GET", url, hdrs, "");
#else
    std::string resp = curl_request("GET", url, hdrs, "");
#endif

    if (is_rate_limited(resp))
    {
        note_rate_limited();
    }

    return resp;
}

std::string KisClient::http_post(const std::string& url, const std::vector<std::string>& headers,
                                 const std::string& body)
{
    auto hdrs = headers;

    if (url.find("oauth2") == std::string::npos)
    {
        ensure_authenticated();
        kis_stamp_bearer(hdrs, token());
    }

    rate_limit_acquire(url);

#ifdef _WIN32
    std::string resp = winhttp_request("POST", url, hdrs, body);
#else
    std::string resp = curl_request("POST", url, hdrs, body);
#endif

    if (is_rate_limited(resp))
    {
        note_rate_limited();
    }

    return resp;
}

// 공용 인증 헤더 — bearer·appkey·appsecret·tr_id 네 줄에 호출별 항목을 더한다.
//  bearer는 http_get/http_post가 ensure_authenticated 뒤 최신 토큰으로 다시 찍는다(kis_stamp_bearer).
std::vector<std::string> KisClient::auth_headers(const std::string& tr_id,
                                                 std::initializer_list<std::string> extra) const
{
    std::vector<std::string> h = {"authorization: Bearer " + token(), "appkey: " + cfg_.app_key,
                                  "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id};
    h.insert(h.end(), extra.begin(), extra.end());
    return h;
}
