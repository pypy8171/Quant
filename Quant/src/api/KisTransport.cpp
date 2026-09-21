// api/KisTransport.cpp — HTTP 전송 한 겹: 플랫폼별 요청(WinHTTP/libcurl)·재시도·초당 한도·공용 인증 헤더.
//  모든 REST 호출은 http_get/http_post를 지난다. 스레드 공용(연결은 스레드별 캐시, 한도 버킷은 rate_mutex_).
//  KisClient 구현은 도메인별 7파일이다 — 목록은 Quant/source/api/KisClientInternal.h. [why D-048]
#include "KisClientInternal.h"

// 재시도 없이 즉시 실패 스코프 깊이(스레드별). 0보다 크면 조회 재시도를 하지 않는다.
static thread_local int g_fastfail_depth = 0;
// 직전 단발 시도가 "서버가 제한 시간 안에 답을 안 준" 실패였는가(WinHTTP 12002·curl 28). 재시도 래퍼가 읽는다.
//  이 실패는 이미 수신 제한 시간(15초)만큼 기다린 뒤라 다시 보내면 또 그만큼 걸린다 — 09-18 모의 서버가 잔고
//  조회에 195번 이렇게 답했고 재시도 3회가 한 조회를 45초 넘게 붙잡았다. 다른 전송 실패(빈 응답 12152·연결 끊김)는
//  바로 다시 보내면 대개 붙으므로 재시도를 유지한다.
static thread_local bool g_last_attempt_timed_out = false;

KisClient::FastFailScope::FastFailScope() { ++g_fastfail_depth; }
KisClient::FastFailScope::~FastFailScope() { --g_fastfail_depth; }

// ═══════════════════════════════════════════════════════════════════════════
//  플랫폼 공용 — 헤더 오버레이·초당 한도 판정 (WinHTTP·libcurl 둘 다 쓴다)
// ═══════════════════════════════════════════════════════════════════════════

// 전송 직전에 헤더 목록에 덧입히는 것. 호출자가 만든 헤더 벡터는 그대로 두고(복사 0), 전송부가 줄을 하나씩
//  만들 때 authorization 줄만 지금 토큰으로 바꿔 내보내고, 없으면 Content-Type 줄을 덧붙인다. [why D-108]
//  호출자들은 헤더를 먼저 조립하고 http_get/http_post가 그 뒤에 ensure_authenticated()를 부르므로, 갱신이
//  일어난 요청은 옛 토큰(기동 직후 첫 호출이면 빈 토큰)으로 나간다 — 그래서 전송 직전에 다시 찍는다.
struct HeaderOverlay
{
    // 지금 토큰으로 만든 "authorization: Bearer …" 줄. 있으면 헤더의 authorization 줄 대신 이것을 보낸다
    //  (oauth2 발급 요청은 nullptr). [inv] 전송이 끝날 때까지 살아 있는 문자열(http_get/http_post의 지역 변수)
    const std::string* bearer_line = nullptr;
    bool ensure_json_content_type = false; // Content-Type 줄이 없으면 하나 덧붙인다(KIS는 GET에도 요구)
};

// 헤더 벡터에 오버레이를 입혀 보낼 줄을 차례로 emit(const std::string&)에 넘긴다. 문자열을 새로 만들지 않는다.
template <typename Emit>
static void emit_header_lines(const std::vector<std::string>& headers, const HeaderOverlay& overlay, Emit&& emit)
{
    static const std::string kJsonContentType = "Content-Type: application/json; charset=utf-8";
    bool has_content_type = false;

    for (const auto& header : headers)
    {
        if (overlay.bearer_line && (header.starts_with("authorization:") || header.starts_with("Authorization:")))
        {
            emit(*overlay.bearer_line);
            continue;
        }

        if (header.find("Content-Type") != std::string::npos)
        {
            has_content_type = true;
        }

        emit(header);
    }

    if (overlay.ensure_json_content_type && !has_content_type)
    {
        emit(kJsonContentType);
    }
}

// 초당 호출 한도 초과 신호. KIS는 이걸 HTTP 500으로도 돌려줘서 상태코드만으로는 일시 서버
//  장애와 구분이 안 된다 — 바디의 코드로 가른다. 한도 초과에 즉시 재시도하면 호출량을 1→3배로
//  늘려 초과를 더 키운다(양의 되먹임). 한도 창이 1초라 150·300ms 백오프도 같은 창 안에 떨어진다.
static bool is_rate_limited(const std::string& body)
{
    return body.find("EGW00201") != std::string::npos ||
           body.find("초당 거래건수") != std::string::npos;
}

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

static std::wstring to_wstring(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide_text(count - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide_text[0], count);
    return wide_text;
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
    WinHttpResult win_http_result{};
    std::wstring wide_url = to_wstring(url);
    wchar_t host[512]{}, path[4096]{};
    URL_COMPONENTS url_components{};
    url_components.dwStructSize = sizeof(url_components);
    url_components.lpszHostName = host;
    url_components.dwHostNameLength = (DWORD)std::size(host);
    url_components.lpszUrlPath = path;
    url_components.dwUrlPathLength = (DWORD)std::size(path);
    WinHttpCrackUrl(wide_url.c_str(), 0, 0, &url_components);
    win_http_result.host = host;
    win_http_result.path = path;
    win_http_result.port = url_components.nPort;
    win_http_result.https = (url_components.nScheme == INTERNET_SCHEME_HTTPS);
    return win_http_result;
}

// ─── 커넥션 풀(P-2): 스레드별 WinHTTP 세션·연결 상주로 keep-alive 재사용 ────────
//  매 호출 session_handle/hConnect를 새로 열면 주문·조회마다 TCP+TLS 핸드셰이크를 재지불한다.
//  session_handle·hConnect를 thread_local로 상주시키고 hReq만 매번 생성한다. WINHTTP_DISABLE_KEEP_ALIVE를
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
static thread_local WinHttpConn thread_connection;

// 풀링 해제 스위치. 환경변수 QUANT_HTTP_NOPOOL=1이면 매 요청 뒤 상주 연결을 파기해
// 풀링 도입 전(요청마다 TCP+TLS 재수립) 거동을 그대로 재현한다. 측정용으로만 쓴다 —
// 바이너리 하나에서 변수 하나(풀링 유무)만 바꿔 before/after를 비교하려는 목적.
static bool http_nopool()
{
    static const bool disabled = [] {
        const char* end = std::getenv("QUANT_HTTP_NOPOOL");
        return end && *end == '1';
    }();
    return disabled;
}

// (host,port)에 대한 상주 connect_handle 확보. 실패 시 nullptr.
static HINTERNET acquire_connection(const WinHttpResult& win_http_result)
{
    if (thread_connection.session && thread_connection.connect && thread_connection.host == win_http_result.host && thread_connection.port == win_http_result.port)
    {
        return thread_connection.connect; // 워밍된 연결 재사용
    }

    thread_connection.reset(); // 최초 or 호스트 변경 → 재수립

    thread_connection.session = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);

    if (!thread_connection.session)
    {
        return nullptr;
    }

    // 명시적 타임아웃(milliseconds): resolve/connect/send/receive. 기본값(무한대급)에서 하향해
    // 전송 계층 히컵이 스레드를 오래 잡지 않게 한다.
    WinHttpSetTimeouts(thread_connection.session, 5000, 5000, 10000, 15000);

    thread_connection.connect = WinHttpConnect(thread_connection.session, win_http_result.host.c_str(), win_http_result.port, 0);

    if (!thread_connection.connect)
    {
        thread_connection.reset();
        return nullptr;
    }

    thread_connection.host = win_http_result.host;
    thread_connection.port = win_http_result.port;
    return thread_connection.connect;
}
} // namespace


// 단발 시도. transport_ok = HTTP 응답을 실제로 받았는가(상태코드 무관, 4xx/5xx도 true).
//  false = 전송 계층 실패(핸들 생성/SendRequest/ReceiveResponse 실패 — 예: 12152). 이때만 재시도 대상.
//  session_handle/hConnect는 상주(keep-alive)라 매 호출 hReq만 열고 닫는다. 전송 실패 시 상주 연결을 파기한다.
static std::string winhttp_request_once(const std::string& method, const std::string& url,
                                        const std::vector<std::string>& headers, const HeaderOverlay& overlay,
                                        const std::string& body, bool& transport_ok, int& status_code)
{
    transport_ok = false;
    status_code = 0;
    g_last_attempt_timed_out = false;
    auto other_crack_url = crack_url(url);

    HINTERNET connect_handle = acquire_connection(other_crack_url);

    if (!connect_handle)
    {
        return "";
    }

    DWORD flags = other_crack_url.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request_handle = WinHttpOpenRequest(connect_handle, to_wstring(method).c_str(), other_crack_url.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!request_handle)
    {
        thread_connection.reset(); // 상주 연결이 상해 있을 수 있음 → 파기, 다음 호출서 재수립
        return "";
    }

    emit_header_lines(headers, overlay, [request_handle](const std::string& line)
    {
        auto wide_line = to_wstring(line + "\r\n");
        WinHttpAddRequestHeaders(request_handle, wide_line.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    });

    LPVOID pBody = body.empty() ? nullptr : (LPVOID)body.c_str();
    DWORD cbBody = (DWORD)body.size();

    if (!WinHttpSendRequest(request_handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pBody, cbBody, cbBody, 0))
    {
        DWORD error = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] SendRequest 실패: %lu", error);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(request_handle);
        thread_connection.reset(); // 끊긴 keep-alive 가능 → 파기 후 재수립(GET이면 래퍼가 재시도)
        return "";
    }

    if (!WinHttpReceiveResponse(request_handle, nullptr))
    {
        DWORD error = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] ReceiveResponse 실패: %lu", error);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        g_last_attempt_timed_out = (error == ERROR_WINHTTP_TIMEOUT);
        WinHttpCloseHandle(request_handle);
        thread_connection.reset();
        return "";
    }

    // 여기 도달 = HTTP 응답 수신 성공(상태코드는 아래에서 확인). 전송 계층은 정상.
    transport_ok = true;

    // HTTP 상태 코드 확인 (4xx/5xx도 body를 읽어야 함)
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    status_code = static_cast<int>(statusCode);

    if (statusCode >= 400)
    {
        char errbuf[32];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] HTTP %lu", statusCode);
        LOG_WARN(std::string(errbuf) + "  url=" + url);
    }

    std::string response;
    DWORD avail = 0;

    while (WinHttpQueryDataAvailable(request_handle, &avail) && avail > 0)
    {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(request_handle, &chunk[0], avail, &read);
        response.append(chunk, 0, read);
    }

    WinHttpCloseHandle(request_handle); // hReq만 닫는다. connect_handle/hSession은 상주(keep-alive 재사용).

    if (http_nopool())
    {
        thread_connection.reset(); // 측정용 스위치 — 풀링 도입 전 거동(요청마다 재수립) 재현
    }

    return response;
}

// 재시도 래퍼. ⚠ 조회(GET) 요청(여러 번 보내도 서버 상태 불변이라 재시도 안전)만 재시도한다 — (a) 전송 계층 실패(12152 등, 제한 시간 초과 12002는 제외), (b) 5xx 서버 일시장애.
//  KIS 시세/일봉 TR은 부하 시 간헐 HTTP 500을 뱉는데(전송은 정상, transport_ok=true), 이때 일봉이 <60봉으로
//  잘려 스캔 후보가 통째로 탈락한다 → 조회(GET)에 한해 5xx도 재시도해 후보 유실을 막는다.
//  주문 등 POST는 재시도하지 않는다 — 빈 응답(12152)이 "미접수"라는 보장이 없어(서버엔 접수됐을 수 있음)
//  블라인드 재시도는 이중주문 위험. POST 실패는 호출자가 잔고 대조로 확정해야 한다.
static std::string winhttp_request(const std::string& method, const std::string& url,
                                   const std::vector<std::string>& headers, const HeaderOverlay& overlay,
                                   const std::string& body)
{
    constexpr int      kMaxGetAttempts    = 3;   // 조회(GET) 최대 시도(원 시도 + 재시도 2)
    constexpr unsigned kRetryBackoffMsBase = 500; // 선형 백오프 기준(attempt배: 500ms, 1000ms)
    const bool idempotent = (method == "GET");
    const int max_attempts = (idempotent && g_fastfail_depth == 0) ? kMaxGetAttempts : 1;
    std::string response;

    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        response = winhttp_request_once(method, url, headers, overlay, body, transport_ok, status);
        // 재시도 대상: 전송 실패(항상) 또는 조회(GET)의 5xx. 그 외(2xx/4xx)는 즉시 반환.
        const bool retryable = !transport_ok || (idempotent && status >= 500);

        if (!retryable)
        {
            return response;
        }

        // 한도 초과가 확인되면 한 번만 더 시도하고 그친다. 부하가 원인인 실패에 재시도를
        //  겹치면 부하를 더 얹는다.
        const bool rate_limited = is_rate_limited(response);

        if (rate_limited && attempt >= 2)
        {
            return response;
        }

        // 수신 제한 시간을 넘긴 실패는 재시도하지 않는다 — 느린 서버에 같은 요청을 다시 넣어 봐야 같은 시간이
        //  또 간다. 호출자(잔고 대조 등)가 자기 주기에 다시 부른다.
        if (!transport_ok && g_last_attempt_timed_out)
        {
            LOG_WARN("[WinHTTP] 수신 제한 시간 초과 — 재시도 없이 실패 처리  url=" + url);
            return response;
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

    return response; // 재시도 소진 — 마지막 응답(빈 문자열 또는 5xx 바디)
}

#else
// ─── Linux: libcurl ────────────────────────────────────────────────────────
#include <curl/curl.h>

static size_t write_callback(char* pointer, size_t size, size_t nmemb, std::string* data)
{
    data->append(pointer, size * nmemb);
    return size * nmemb;
}

// 단발 시도. transport_ok = HTTP 응답을 받았는가(CURLE_OK; 4xx/5xx도 true).
//  curl_easy_perform은 HTTP 응답을 받으면(상태코드 무관) CURLE_OK, 전송 실패(타임아웃·연결단절 등)만 비-OK.
static std::string curl_request_once(const std::string& method, const std::string& url,
                                     const std::vector<std::string>& headers, const HeaderOverlay& overlay,
                                     const std::string& body, bool& transport_ok, int& status_code)
{
    transport_ok = false;
    status_code = 0;
    g_last_attempt_timed_out = false;
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        return "";
    }

    std::string response;
    curl_slist* hlist = nullptr;

    emit_header_lines(headers, overlay, [&hlist](const std::string& line)
    {
        hlist = curl_slist_append(hlist, line.c_str());
    });

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

    CURLcode result_code = curl_easy_perform(curl);

    if (result_code != CURLE_OK)
    {
        LOG_ERROR(std::string("[CURL] 요청 실패: ") + curl_easy_strerror(result_code));
        g_last_attempt_timed_out = (result_code == CURLE_OPERATION_TIMEDOUT);
    }
    else
    {
        transport_ok = true;
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        status_code = static_cast<int>(http_code);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

// 재시도 래퍼. ⚠ 조회(GET) 요청만 재시도 — 전송 계층 실패 또는 5xx(WinHTTP 경로와 동일 규약 — 주문 POST 제외).
static std::string curl_request(const std::string& method, const std::string& url,
                                const std::vector<std::string>& headers, const HeaderOverlay& overlay,
                                const std::string& body)
{
    constexpr int kMaxGetAttempts     = 3;   // 조회(GET) 최대 시도(원 시도 + 재시도 2)
    constexpr int kRetryBackoffMsBase = 500; // 선형 백오프 기준(attempt배: 500ms, 1000ms)
    const bool idempotent = (method == "GET");
    const int max_attempts = (idempotent && g_fastfail_depth == 0) ? kMaxGetAttempts : 1;
    std::string response;

    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        response = curl_request_once(method, url, headers, overlay, body, transport_ok, status);
        const bool retryable = !transport_ok || (idempotent && status >= 500);

        if (!retryable)
        {
            return response;
        }

        const bool rate_limited = is_rate_limited(response); // 한도 초과면 한 번만 더 시도

        if (rate_limited && attempt >= 2)
        {
            return response;
        }

        if (!transport_ok && g_last_attempt_timed_out) // WinHTTP 경로와 같은 규약 — 제한 시간 초과는 재시도 없음
        {
            LOG_WARN("[CURL] 수신 제한 시간 초과 — 재시도 없이 실패 처리  url=" + url);
            return response;
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

    return response;
}
#endif

// ─── HTTP 래퍼 ────────────────────────────────────────────────────────────

// 초당 호출 한도를 넘지 않게 호출을 고르게 편다. KIS는 한도를 넘긴 요청에 HTTP 500이나
//  EGW00201을 돌려주는데, 어느 쪽이든 그 호출은 버려지고 재시도가 붙어 호출량이 더 는다.
//  버킷은 인스턴스(=app_key)마다 따로다 — 한도가 app_key 단위라 시세 클라이언트와 주문
//  클라이언트의 예산은 서로 무관하다. 총 호출량을 줄이지는 못하고 순서만 고르게 만든다.
namespace
{
thread_local std::uint64_t t_rate_wait_ns = 0;   // 스레드별 버킷 대기 누적 — rate_wait_ns_this_thread()
}

std::uint64_t KisClient::rate_wait_ns_this_thread()
{
    return t_rate_wait_ns;
}

void KisClient::rate_limit_acquire(const std::string& url)
{
    const auto acquire_start = std::chrono::steady_clock::now();
    // 실전 초당 20건, 모의 초당 2건이 공표 한도다. 재시도·토큰 갱신이 끼어들 여유를 남겨 낮게 잡는다.
    const double refill = config_.is_paper ? 2.0 : 15.0;
    const double capacity = refill; // 1초치까지만 모아둔다(그 이상 몰아치면 어차피 한도에 걸린다)
    // 주문·잔고 경로에는 예약분을 남긴다. 시세 조회가 버킷을 다 비운 순간 청산 주문이
    //  그 뒤에 줄서면 몇 백 ms가 늦는데, 그 지연은 조회 지연과 값이 다르다.
    const bool priority = url.find("/trading/") != std::string::npos;
    const double need = priority ? 1.0 : 2.0;

    while (true) // while (1)
    {
        double wait_sec = 0.0;
        {
            std::lock_guard<std::mutex> lock(rate_mutex_);
            auto now = std::chrono::steady_clock::now();

            if (rate_last_.time_since_epoch().count() == 0)
            {
                rate_last_ = now;
                rate_tokens_ = capacity; // 첫 호출은 기다리지 않는다
            }

            double elapsed = std::chrono::duration<double>(now - rate_last_).count();
            rate_last_ = now;
            rate_tokens_ = (std::min)(capacity, rate_tokens_ + elapsed * refill);

            if (rate_tokens_ >= need)
            {
                rate_tokens_ -= 1.0;
                t_rate_wait_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - acquire_start).count());
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
    std::lock_guard<std::mutex> lock(rate_mutex_);
    rate_tokens_ = 0.0; // 다음 호출은 리필을 기다린다(≈1초치)
}

std::string KisClient::http_get(const std::string& url, const std::vector<std::string>& headers)
{
    // KIS API는 GET에도 Content-Type: application/json 요구 — 없으면 전송부가 덧붙인다
    HeaderOverlay overlay;
    overlay.ensure_json_content_type = true;
    std::string bearer_line;

    // oauth2 토큰 발급 엔드포인트가 아닌 경우에만 자동 갱신 (재귀 방지)
    if (url.find("oauth2") == std::string::npos)
    {
        ensure_authenticated();
        bearer_line = "authorization: Bearer ";
        append_token(bearer_line);
        overlay.bearer_line = &bearer_line;
    }

    rate_limit_acquire(url);
#ifdef _WIN32
    std::string response = winhttp_request("GET", url, headers, overlay, "");
#else
    std::string response = curl_request("GET", url, headers, overlay, "");
#endif

    if (is_rate_limited(response))
    {
        note_rate_limited();
    }

    return response;
}

std::string KisClient::http_post(const std::string& url, const std::vector<std::string>& headers, const std::string& body)
{
    HeaderOverlay overlay;
    std::string   bearer_line;

    if (url.find("oauth2") == std::string::npos)
    {
        ensure_authenticated();
        bearer_line = "authorization: Bearer ";
        append_token(bearer_line);
        overlay.bearer_line = &bearer_line;
    }

    rate_limit_acquire(url);

#ifdef _WIN32
    std::string response = winhttp_request("POST", url, headers, overlay, body);
#else
    std::string response = curl_request("POST", url, headers, overlay, body);
#endif

    if (is_rate_limited(response))
    {
        note_rate_limited();
    }

    return response;
}

// 공용 인증 헤더 — bearer·appkey·appsecret·tr_id 네 줄에 호출별 항목을 더한다.
//  bearer는 http_get/http_post가 ensure_authenticated 뒤 최신 토큰으로 다시 찍는다(HeaderOverlay).
std::vector<std::string> KisClient::authentication_headers(const std::string& transaction_id,
                                                 std::initializer_list<std::string> extra) const
{
    std::vector<std::string> parts;
    parts.reserve(4 + extra.size());
    parts.emplace_back("authorization: Bearer ");
    append_token(parts.back());
    parts.emplace_back("appkey: " + config_.app_key);
    parts.emplace_back("appsecret: " + config_.app_secret);
    parts.emplace_back("tr_id: " + transaction_id);
    parts.insert(parts.end(), extra.begin(), extra.end());
    return parts;
}
