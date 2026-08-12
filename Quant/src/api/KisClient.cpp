#include "api/KisClient.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#ifndef _WIN32
#include <fcntl.h>    // open — fsync용 (V-2)
#include <sys/stat.h> // chmod — 토큰 캐시 0600 (W-4)
#include <unistd.h>   // fsync, close (V-2)
#endif

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

static std::wstring to_wstring(const std::string& s)
{
    if (s.empty())
        return {};
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

// 단발 시도. transport_ok = HTTP 응답을 실제로 받았는가(상태코드 무관, 4xx/5xx도 true).
//  false = 전송 계층 실패(핸들 생성/SendRequest/ReceiveResponse 실패 — 예: 12152). 이때만 재시도 대상.
static std::string winhttp_request_once(const std::string& method, const std::string& url,
                                        const std::vector<std::string>& headers, const std::string& body,
                                        bool& transport_ok, int& status_code)
{
    transport_ok = false;
    status_code = 0;
    auto c = crack_url(url);

    HINTERNET hSession = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return "";

    // 명시적 타임아웃(ms): resolve/connect/send/receive. 기본값(무한대급)에서 하향해
    // 전송 계층 히컵이 스레드를 장시간 잡는 것을 막는다.
    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, c.host.c_str(), c.port, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD flags = c.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, to_wstring(method).c_str(), c.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
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
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }
    if (!WinHttpReceiveResponse(hReq, nullptr))
    {
        DWORD err = GetLastError();
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[WinHTTP] ReceiveResponse 실패: %lu", err);
        LOG_ERROR(std::string(errbuf) + "  url=" + url);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
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

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// 재시도 래퍼. ⚠ 멱등 요청(GET)만 재시도한다 — (a) 전송 계층 실패(12152 등), (b) 5xx 서버 일시장애.
//  KIS 시세/일봉 TR은 부하 시 간헐 HTTP 500을 뱉는데(전송은 정상, transport_ok=true), 이때 일봉이 <60봉으로
//  잘려 스캔 후보가 통째로 탈락한다 → 멱등 GET에 한해 5xx도 재시도해 후보 유실을 막는다.
//  주문 등 POST는 재시도하지 않는다 — 빈 응답(12152)이 "미접수"라는 보장이 없어(서버엔 접수됐을 수 있음)
//  블라인드 재시도는 이중주문 위험. POST 실패는 호출자가 리컨사일로 확정해야 한다.
static std::string winhttp_request(const std::string& method, const std::string& url,
                                   const std::vector<std::string>& headers, const std::string& body)
{
    const bool idempotent = (method == "GET");
    const int max_attempts = idempotent ? 3 : 1;
    std::string resp;
    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        resp = winhttp_request_once(method, url, headers, body, transport_ok, status);
        // 재시도 대상: 전송 실패(항상) 또는 멱등 GET의 5xx. 그 외(2xx/4xx)는 즉시 반환.
        const bool retryable = !transport_ok || (idempotent && status >= 500);
        if (!retryable)
            return resp;
        if (attempt < max_attempts)
        {
            LOG_WARN("[WinHTTP] " + std::string(transport_ok ? "HTTP " + std::to_string(status) : "전송 실패") +
                     " — 재시도 " + std::to_string(attempt + 1) + "/" + std::to_string(max_attempts) +
                     " (GET 멱등)  url=" + url);
            Sleep(150u * attempt); // 선형 백오프: 150ms, 300ms
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
        return "";

    std::string response;
    curl_slist* hlist = nullptr;
    for (auto& h : headers)
        hlist = curl_slist_append(hlist, h.c_str());

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
        LOG_ERROR(std::string("[CURL] 요청 실패: ") + curl_easy_strerror(rc));
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

// 재시도 래퍼. ⚠ 멱등 요청(GET)만 재시도 — 전송 계층 실패 또는 5xx(WinHTTP 경로와 동일 규약 — 주문 POST 제외).
static std::string curl_request(const std::string& method, const std::string& url,
                                const std::vector<std::string>& headers, const std::string& body)
{
    const bool idempotent = (method == "GET");
    const int max_attempts = idempotent ? 3 : 1;
    std::string resp;
    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        bool transport_ok = false;
        int status = 0;
        resp = curl_request_once(method, url, headers, body, transport_ok, status);
        const bool retryable = !transport_ok || (idempotent && status >= 500);
        if (!retryable)
            return resp;
        if (attempt < max_attempts)
        {
            LOG_WARN("[CURL] " + std::string(transport_ok ? "HTTP " + std::to_string(status) : "전송 실패") +
                     " — 재시도 " + std::to_string(attempt + 1) + "/" + std::to_string(max_attempts) +
                     " (GET 멱등)  url=" + url);
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
        }
    }
    return resp;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  KisClient 구현
// ═══════════════════════════════════════════════════════════════════════════

KisClient::KisClient(const KisConfig& cfg) : cfg_(cfg)
{
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

KisClient::~KisClient()
{
#ifndef _WIN32
    curl_global_cleanup();
#endif
}

// 토큰 캐시 파일: app_key 앞 8자리로 구분 (실계좌/모의 혼용 방지).
// KIS_TOKEN_CACHE_DIR(docker 공유 볼륨) 설정 시 그 경로에 저장 → Python balance가 재사용.
static std::string token_cache_path(const std::string& app_key)
{
    const char* dir = std::getenv("KIS_TOKEN_CACHE_DIR");
    std::string prefix = (dir && *dir) ? std::string(dir) + "/" : "";
    return prefix + "kis_token_" + app_key.substr(0, 8) + ".json";
}

void KisClient::ensure_authenticated()
{
    // 만료 5분 전이면 재발급
    auto now = std::chrono::system_clock::now();
    auto margin = std::chrono::minutes(5);
    if (access_token_.empty() || now + margin >= token_expires_at_)
    {
        LOG_INFO("[KIS] 토큰 갱신 시작");
        authenticate();
    }
}

bool KisClient::authenticate()
{
    // ── 캐시 파일에 유효한 토큰이 있으면 재사용 ──────────────────────────
    std::string cache_path = token_cache_path(cfg_.app_key);
    {
        std::ifstream f(cache_path);
        if (f.is_open())
        {
            try
            {
                auto j = json::parse(f);
                std::string token = j.value("access_token", "");
                std::string expires = j.value("expires_at", ""); // ISO "YYYY-MM-DD HH:MM:SS"

                if (!token.empty() && !expires.empty())
                {
                    // 만료 시각 파싱
                    struct tm tm_exp
                    {
                    };
                    int y, mo, d, h, mi, s;
                    if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
                    {
                        tm_exp.tm_year = y - 1900;
                        tm_exp.tm_mon = mo - 1;
                        tm_exp.tm_mday = d;
                        tm_exp.tm_hour = h;
                        tm_exp.tm_min = mi;
                        tm_exp.tm_sec = s;
                        tm_exp.tm_isdst = -1;
                        auto exp_t = std::mktime(&tm_exp);
                        auto now_t = std::time(nullptr);
                        // 만료 10분 전까지 사용
                        if (exp_t - now_t > 600)
                        {
                            access_token_ = token;
                            token_expires_at_ = std::chrono::system_clock::from_time_t(exp_t);
                            LOG_INFO("[KIS] 캐시 토큰 재사용 (만료: " + expires + ")");
                            return true;
                        }
                    }
                }
            }
            catch (...)
            {
            }
        }
    }

    // ── 새 토큰 발급 ─────────────────────────────────────────────────────
    json body = {{"grant_type", "client_credentials"}, {"appkey", cfg_.app_key}, {"appsecret", cfg_.app_secret}};

    std::string url = base_url() + "/oauth2/tokenP";
    std::string resp = http_post(url, {"Content-Type: application/json"}, body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] 토큰 발급 요청 실패");
        return false;
    }

    try
    {
        auto j = json::parse(resp);
        access_token_ = j["access_token"].get<std::string>();

        // 만료 시각 저장 (KIS 응답 필드: access_token_token_expired)
        std::string expires = j.value("access_token_token_expired", "");

        // 인메모리 만료 시각 설정
        {
            struct tm tm_exp
            {
            };
            int y, mo, d, h, mi, s;
            if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
            {
                tm_exp.tm_year = y - 1900;
                tm_exp.tm_mon = mo - 1;
                tm_exp.tm_mday = d;
                tm_exp.tm_hour = h;
                tm_exp.tm_min = mi;
                tm_exp.tm_sec = s;
                tm_exp.tm_isdst = -1;
                token_expires_at_ = std::chrono::system_clock::from_time_t(std::mktime(&tm_exp));
            }
            else
            {
                // 파싱 실패 시 24시간 후로 설정
                token_expires_at_ = std::chrono::system_clock::now() + std::chrono::hours(24);
            }
        }

        // 캐시 파일에 저장 — temp 작성 후 atomic rename (C-2).
        // 읽는 쪽(Python balance)이 스트리밍 중인 truncated JSON을 보지 않게 한다.
        json cache_j = {{"access_token", access_token_}, {"expires_at", expires}};
        std::string tmp_path = cache_path + ".tmp";
        {
            std::ofstream cf(tmp_path);
            if (cf.is_open())
                cf << cache_j.dump(2);
        }
#ifdef _WIN32
        MoveFileExA(tmp_path.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        ::chmod(tmp_path.c_str(), S_IRUSR | S_IWUSR); // 0600 — 공유 볼륨 평문 토큰 보호 (W-4)
        // rename은 원자적이나 durability는 보장 안 함 — 크래시 시 0바이트 캐시→재발급(403).
        // tmp를 fsync해 데이터를 디스크에 내린 뒤 rename (V-2).
        {
            int fd = ::open(tmp_path.c_str(), O_RDONLY);
            if (fd >= 0) { ::fsync(fd); ::close(fd); }
        }
        std::rename(tmp_path.c_str(), cache_path.c_str()); // POSIX atomic
#endif

        LOG_INFO("[KIS] 토큰 발급 성공 (만료: " + expires + ")");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[KIS] 토큰 파싱 오류: ") + e.what());
        return false;
    }
}

std::vector<MarketData> KisClient::get_daily_ohlcv(const std::string& ticker, int count)
{
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
    time_t end_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600; // KST 오늘
    // count 거래일 확보를 위해 달력일 여유(주말·휴일 ~1.6배 + 헤드룸), 최소 30일.
    int window_days = (std::max)(30, static_cast<int>(count * 1.7) + 10); // (): windows.h max 매크로 회피
    std::string d2 = fmt_date(end_t);
    std::string d1 = fmt_date(end_t - static_cast<time_t>(window_days) * 86400);

    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice" +
                      "?FID_COND_MRKT_DIV_CODE=J" + "&FID_INPUT_ISCD=" + ticker + "&FID_INPUT_DATE_1=" + d1 +
                      "&FID_INPUT_DATE_2=" + d2 + "&FID_PERIOD_DIV_CODE=D" + "&FID_ORG_ADJ_PRC=0";

    std::vector<std::string> headers = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                        "appsecret: " + cfg_.app_secret, "tr_id: FHKST03010100"};

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
            if (fetched++ >= count)
                break;
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

    return result;
}

std::vector<MarketData> KisClient::get_minute_ohlcv(const std::string& ticker, int count, int interval_min)
{
    ensure_authenticated();
    std::vector<MarketData> result;
    if (count <= 0) return result;
    if (interval_min < 1) interval_min = 1;

    auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
        try { return std::stod(o.value(k, "0")); } catch (...) { return 0.0; }
    };
    // YYYYMMDD + HHMMSS → time_t (서버 TZ 독립: gmtime 계열로 통일, KST 오프셋은 호출 무관)
    auto parse_dt = [](const std::string& d, const std::string& t) -> time_t {
        if (d.size() != 8 || t.size() < 6) return 0;
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
    };

    // 기준시각: 현재 KST(장중)이면 지금, 장전/장후면 15:30에서 역조회.
    time_t now_kst = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600;
    struct tm ntm{};
#ifdef _WIN32
    gmtime_s(&ntm, &now_kst);
#else
    gmtime_r(&now_kst, &ntm);
#endif
    char hbuf[7];
    std::snprintf(hbuf, sizeof(hbuf), "%02d%02d%02d", ntm.tm_hour, ntm.tm_min, ntm.tm_sec);
    std::string hour = hbuf;
    if (hour < "090000" || hour > "153000") hour = "153000";

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST03010200",
    };

    // 필요한 1분봉 수 = count*interval_min. 1콜당 ~30봉 → 여유롭게 페이지 상한.
    const int need_1min  = count * interval_min;
    const int kMaxPages  = (std::min)(20, need_1min / 25 + 3); // (): windows.h min 매크로 회피

    struct Raw { std::string date, hour; double o = 0, h = 0, l = 0, c = 0; int64_t v = 0; };
    std::vector<Raw> raws;
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
        if (resp.empty()) break;
        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded() || !j.contains("output2")) break;
        auto& arr = j["output2"];
        if (arr.empty()) break;

        std::string page_earliest; // 이 페이지에서 가장 이른 시각(HHMMSS)
        int added = 0;
        for (const auto& item : arr) // output2: 최신→과거
        {
            std::string d = item.value("stck_bsop_date", "");
            std::string t = item.value("stck_cntg_hour", "");
            if (t.size() < 6) continue;
            if (page_earliest.empty() || t < page_earliest) page_earliest = t;
            std::string key = d + t;
            if (!seen.insert(key).second) continue; // 중복
            Raw r;
            r.date = d; r.hour = t;
            r.o = sd(item, "stck_oprc");
            r.h = sd(item, "stck_hgpr");
            r.l = sd(item, "stck_lwpr");
            r.c = sd(item, "stck_prpr");
            r.v = static_cast<int64_t>(sd(item, "cntg_vol"));
            raws.push_back(r);
            ++added;
        }
        if (page_earliest.empty()) break;
        int prev = std::stoi(page_earliest) - 100; // 1분(=HHMMSS 100) 이전으로 밀기
        if (prev < 90000) break;                   // 당일 장중만 수집
        char nb[7];
        std::snprintf(nb, sizeof(nb), "%06d", prev);
        hour = nb;
        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // rate limit 여유
    }

    if (raws.empty()) return result;

    // 1분봉 오름차순(과거→최신) 정렬 후 interval_min 버킷 집계.
    std::sort(raws.begin(), raws.end(), [](const Raw& a, const Raw& b) {
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
            md.timestamp = std::chrono::system_clock::from_time_t(parse_dt(r.date, r.hour));
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
            md.timestamp = std::chrono::system_clock::from_time_t(parse_dt(r.date, r.hour));
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

double KisClient::get_current_price(const std::string& ticker)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/quotations/inquire-price" + "?FID_COND_MRKT_DIV_CODE=J" +
                      "&FID_INPUT_ISCD=" + ticker;

    std::vector<std::string> headers = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                        "appsecret: " + cfg_.app_secret, "tr_id: FHKST01010100"};

    std::string resp = http_get(url, headers);
    if (resp.empty())
        return 0.0;

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

    std::vector<std::string> headers = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                        "appsecret: " + cfg_.app_secret, "tr_id: FHKST01010100"};

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, headers);
    if (resp.empty())
        return f;

    try
    {
        auto j = json::parse(resp);
        auto& out = j["output"];
        auto parse_d = [&](const std::string& key) -> double
        {
            std::string s = out.value(key, "");
            if (s.empty())
                return 0.0;
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

bool KisClient::send_order(const OrderSignal& signal)
{
    bool is_us = (signal.market == Market::US);
    std::string tr_id;
    std::string url;

    if (is_us)
    {
        // 해외주식 주문: NAS/NYS
        if (signal.side == OrderSide::BUY)
            tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
        else
            tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        // 국내주식 현금 주문
        if (signal.side == OrderSide::BUY)
            tr_id = cfg_.is_paper ? "VTTC0802U" : "TTTC0802U";
        else
            tr_id = cfg_.is_paper ? "VTTC0801U" : "TTTC0801U";
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;
    if (is_us)
    {
        body = {{"CANO", cfg_.account_no},
                {"ACNT_PRDT_CD", cfg_.account_type},
                {"OVRS_EXCG_CD", signal.exchange},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "00" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    }
    else
    {
        body = {{"CANO", cfg_.account_no},
                {"ACNT_PRDT_CD", cfg_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "01" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string((int)signal.price) : "0"}};
    }

    std::string resp = http_post(url,
                                 {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                  "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
                                 body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] 주문 실패: " + signal.ticker);
        return false;
    }

    auto j = json::parse(resp);
    bool ok = (j["rt_cd"].get<std::string>() == "0");
    if (ok)
    {
        LOG_INFO("[KIS] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    }
    else
    {
        LOG_ERROR("[KIS] 주문 오류: " + j["msg1"].get<std::string>());
    }
    return ok;
}

// ─── FEP: 주문 제출 — ODNO(접수번호) 반환 ────────────────────────────────
std::string KisClient::submit_order(const OrderSignal& signal)
{
    // send_order와 동일한 본문을 재사용하되, 성공 시 ODNO를 추출해 반환
    bool is_us = (signal.market == Market::US);
    std::string tr_id, url;

    if (is_us)
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTT1002U" : "TTTT1002U")
                                                 : (cfg_.is_paper ? "VTTT1006U" : "TTTT1006U");
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTC0802U" : "TTTC0802U")
                                                 : (cfg_.is_paper ? "VTTC0801U" : "TTTC0801U");
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;
    if (is_us)
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"OVRS_EXCG_CD", signal.exchange}, {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    else
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "01" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string((int)signal.price) : "0"}};

    std::string resp = http_post(url,
        {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
         "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] submit_order 실패: " + signal.ticker);
        return "";
    }

    auto j = json::parse(resp);
    if (j["rt_cd"].get<std::string>() != "0")
    {
        LOG_ERROR("[KIS] 주문 오류: " + j["msg1"].get<std::string>());
        return "";
    }

    std::string odno = j.value("output", json::object()).value("ODNO", "");
    LOG_INFO("[KIS] 주문 접수: " + signal.ticker +
             (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(signal.quantity) + "주  ODNO=" + odno);
    return odno;
}

// ─── MM-1: 신규 주문 + KRX 조직번호 캡처 ──────────────────────────────────
//  submit_order와 본문·tr_id 동일. 응답에서 ODNO에 더해 KRX_FWDG_ORD_ORGNO를
//  추출해 반환한다(정정/취소 시 원주문 조직번호로 재입력해야 함).
//  HTTP 플랫폼 분기(WinHTTP/libcurl)는 http_post 내부에 이미 캡슐화됨.
OrderAck KisClient::submit_order_ack(const OrderSignal& signal)
{
    last_order_msg_cd_.clear(); // 이번 호출 결과로만 채운다(직전 값 오귀속 방지)
    bool is_us = (signal.market == Market::US);
    std::string tr_id, url;

    if (is_us)
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTT1002U" : "TTTT1002U")
                                                 : (cfg_.is_paper ? "VTTT1006U" : "TTTT1006U");
        url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    }
    else
    {
        tr_id = (signal.side == OrderSide::BUY) ? (cfg_.is_paper ? "VTTC0802U" : "TTTC0802U")
                                                 : (cfg_.is_paper ? "VTTC0801U" : "TTTC0801U");
        url = base_url() + "/uapi/domestic-stock/v1/trading/order-cash";
    }

    json body;
    if (is_us)
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"OVRS_EXCG_CD", signal.exchange}, {"PDNO", signal.ticker},
                {"ORD_DVSN", "00"}, {"ORD_QTY", std::to_string(signal.quantity)},
                {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};
    else
        body = {{"CANO", cfg_.account_no}, {"ACNT_PRDT_CD", cfg_.account_type},
                {"PDNO", signal.ticker},
                {"ORD_DVSN", signal.type == OrderType::MARKET ? "01" : "00"},
                {"ORD_QTY", std::to_string(signal.quantity)},
                {"ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string((int)signal.price) : "0"}};

    std::string resp = http_post(url,
        {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
         "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] submit_order_ack 실패: " + signal.ticker);
        return OrderAck{};
    }

    auto j = json::parse(resp);
    if (j["rt_cd"].get<std::string>() != "0")
    {
        last_order_msg_cd_ = j.value("msg_cd", std::string(""));
        LOG_ERROR("[KIS] 주문 오류: " + j["msg1"].get<std::string>());
        return OrderAck{};
    }

    auto out = j.value("output", json::object());
    OrderAck ack;
    ack.odno      = out.value("ODNO", "");
    ack.krx_orgno = out.value("KRX_FWDG_ORD_ORGNO", "");
    LOG_INFO("[KIS] 주문 접수: " + signal.ticker +
             (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(signal.quantity) + "주  ODNO=" + ack.odno +
             " ORGNO=" + ack.krx_orgno);
    return ack;
}

// ─── MM-1: 정정/취소 (국내 order-rvsecncl) ─────────────────────────────────
//  RVSE_CNCL_DVSN_CD: "02"=취소, "01"=정정. 성공 시 응답 ODNO(취소/정정 접수번호) 반환.
//  KRX_FWDG_ORD_ORGNO(원주문 조직번호)와 ORGN_ODNO(원주문번호)가 필수 입력.
//  주의: 국내 현금 주문 전용. 해외(overseas) 정정/취소는 별도 tr_id/URL — 미구현(TODO).
std::string KisClient::cancel_order(const std::string& ticker, const std::string& orig_odno,
                                    const std::string& krx_orgno, int qty, bool all_remaining)
{
    last_order_msg_cd_.clear();
    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] cancel_order 원주문번호(ODNO) 없음 — " + ticker);
        return "";
    }
    std::string tr_id = cfg_.is_paper ? "VTTC0803U" : "TTTC0803U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_orgno},              // 원주문 조직번호
                 {"ORGN_ODNO", orig_odno},                       // 원주문번호
                 {"ORD_DVSN", "00"},                             // 지정가 (취소도 원주문 구분 통상 "00")
                 {"RVSE_CNCL_DVSN_CD", "02"},                    // 02=취소
                 {"ORD_QTY", std::to_string(qty)},               // 취소 수량 (QTY_ALL_ORD_YN=Y면 무시됨)
                 {"ORD_UNPR", "0"},                              // 취소는 단가 0
                 {"QTY_ALL_ORD_YN", all_remaining ? "Y" : "N"}}; // 잔량 전체 취소

    std::string resp = http_post(url,
        {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
         "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] cancel_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return "";
    }
    auto j = json::parse(resp);
    if (j["rt_cd"].get<std::string>() != "0")
    {
        last_order_msg_cd_ = j.value("msg_cd", std::string(""));
        // 이미 체결/취소된 주문이면 KIS가 거부 → 자가치유(호출부가 reserved 미변경). 로그만.
        LOG_WARN("[KIS] 취소 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 j.value("msg1", std::string("")));
        return "";
    }
    std::string cancel_odno = j.value("output", json::object()).value("ODNO", "");
    LOG_INFO("[KIS] 취소 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 취소ODNO=" + cancel_odno);
    return cancel_odno;
}

std::string KisClient::revise_order(const std::string& ticker, const std::string& orig_odno,
                                    const std::string& krx_orgno, int new_qty, double new_price)
{
    last_order_msg_cd_.clear();
    if (orig_odno.empty())
    {
        LOG_ERROR("[KIS] revise_order 원주문번호(ODNO) 없음 — " + ticker);
        return "";
    }
    std::string tr_id = cfg_.is_paper ? "VTTC0803U" : "TTTC0803U";
    std::string url   = base_url() + "/uapi/domestic-stock/v1/trading/order-rvsecncl";

    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"KRX_FWDG_ORD_ORGNO", krx_orgno},
                 {"ORGN_ODNO", orig_odno},
                 {"ORD_DVSN", "00"},                              // 지정가
                 {"RVSE_CNCL_DVSN_CD", "01"},                     // 01=정정
                 {"ORD_QTY", std::to_string(new_qty)},            // 정정 수량 (0이면 잔량 유지 규약도 있으나 명시)
                 {"ORD_UNPR", std::to_string((int)new_price)},    // 정정 단가
                 {"QTY_ALL_ORD_YN", "Y"}};                        // 잔량 전체 정정

    std::string resp = http_post(url,
        {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
         "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
        body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] revise_order 전송 실패: " + ticker + " ODNO=" + orig_odno);
        return "";
    }
    auto j = json::parse(resp);
    if (j["rt_cd"].get<std::string>() != "0")
    {
        last_order_msg_cd_ = j.value("msg_cd", std::string(""));
        LOG_WARN("[KIS] 정정 거부: " + ticker + " ODNO=" + orig_odno + " — " +
                 j.value("msg1", std::string("")));
        return "";
    }
    // 정정 성공 시 새 ODNO 발급 → 반환 (호출부가 kis_order_no 갱신)
    std::string new_odno = j.value("output", json::object()).value("ODNO", "");
    LOG_INFO("[KIS] 정정 접수: " + ticker + " 원ODNO=" + orig_odno +
             " 새ODNO=" + new_odno + " @" + std::to_string((int)new_price));
    return new_odno;
}

// ─── 잔고 조회 (체결 확인용) — inquire-balance ────────────────────────────
//  output1 = 보유종목 배열(pdno·hldg_qty·pchs_avg_pric), output2 = 계좌 요약.
//  체결 후 보유수량 변화로 체결을 확인한다. (모의: VTTC8434R / 실거래: TTTC8434R)
nlohmann::json KisClient::get_balance()
{
    std::string tr_id = cfg_.is_paper ? "VTTC8434R" : "TTTC8434R";

    // 연속조회(페이지네이션): 잔고는 페이지당 ~20종목만 반환하고, 더 있으면 응답 body의
    //  ctx_area_nk100(다음페이지 키)가 채워진다. 이를 CTX_AREA_FK100/NK100로 되넣고
    //  요청헤더 tr_cont:N으로 다음 페이지를 받아 output1을 전부 누적한다. 트림 후 raw 연결로
    //  충분(모의계좌 실측: 20+11=31종목 정상 수신). output2/최상위는 첫 페이지 것을 유지.
    auto rtrim = [](std::string s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        return s;
    };

    nlohmann::json result;
    nlohmann::json out1 = nlohmann::json::array();
    std::string fk, nk, cont;
    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url() + "/uapi/domestic-stock/v1/trading/inquire-balance" +
                          "?CANO=" + cfg_.account_no + "&ACNT_PRDT_CD=" + cfg_.account_type +
                          "&AFHR_FLPR_YN=N&OFL_YN=&INQR_DVSN=02&UNPR_DVSN=01" +
                          "&FUND_STTL_ICLD_YN=N&FNCG_AMT_AUTO_RDPT_YN=N&PRCS_DVSN=00" +
                          "&CTX_AREA_FK100=" + fk + "&CTX_AREA_NK100=" + nk;

        std::vector<std::string> headers = {"authorization: Bearer " + access_token_,
                                            "appkey: " + cfg_.app_key,
                                            "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id,
                                            "tr_cont: " + cont};

        std::string resp = http_get(url, headers);
        if (resp.empty())
            break;
        nlohmann::json j;
        try
        {
            j = json::parse(resp);
        }
        catch (...)
        {
            break;
        }

        if (page == 0)
            result = j; // output2(계좌요약)·최상위 필드는 첫 페이지 기준
        if (j.contains("output1") && j["output1"].is_array())
            for (auto& h : j["output1"])
                out1.push_back(h);

        std::string nk_next = rtrim(j.value("ctx_area_nk100", ""));
        if (nk_next.empty())
            break; // 다음 페이지 없음
        fk = rtrim(j.value("ctx_area_fk100", ""));
        nk = nk_next;
        cont = "N";
    }

    if (result.is_null())
        return json::object();
    result["output1"] = out1;
    return result;
}

// ─── 미체결(정정취소 가능) 예약주문 조회 — inquire-psbl-rvsecncl ─────────────
//  장중 청산이 40240000(주문가능분 없음)으로 막힐 때, 그 종목의 예약매도를 찾아
//  취소→재매도로 자가정리하기 위한 조회. (모의: VTTC0084R / 실거래: TTTC0084R)
//  응답 output(array) 필드는 소문자: odno·ord_gno_brno·pdno·prdt_name·psbl_qty·
//  ord_unpr·sll_buy_dvsn_cd(01매도/02매수). 수량·단가는 문자열이라 파싱 가드.
//  잔고처럼 ctx_area(FK/NK)로 페이지네이션한다.
std::vector<OpenOrder> KisClient::get_open_orders()
{
    std::string tr_id = cfg_.is_paper ? "VTTC0084R" : "TTTC0084R";

    auto to_int = [](const std::string& s) -> int
    { try { return s.empty() ? 0 : std::stoi(s); } catch (...) { return 0; } };
    auto to_dbl = [](const std::string& s) -> double
    { try { return s.empty() ? 0.0 : std::stod(s); } catch (...) { return 0.0; } };
    auto rtrim = [](std::string s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        return s;
    };

    std::vector<OpenOrder> result;
    std::string fk, nk, cont;
    for (int page = 0; page < 30; ++page) // 안전 상한(무한루프 방지)
    {
        std::string url = base_url() +
                          "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl" +
                          "?CANO=" + cfg_.account_no + "&ACNT_PRDT_CD=" + cfg_.account_type +
                          "&INQR_DVSN_1=0&INQR_DVSN_2=0" +
                          "&CTX_AREA_FK100=" + fk + "&CTX_AREA_NK100=" + nk;

        std::vector<std::string> headers = {"authorization: Bearer " + access_token_,
                                            "appkey: " + cfg_.app_key,
                                            "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id,
                                            "tr_cont: " + cont};

        std::string resp = http_get(url, headers);
        if (resp.empty())
            break;
        nlohmann::json j;
        try
        {
            j = json::parse(resp);
        }
        catch (...)
        {
            break;
        }
        if (j.value("rt_cd", std::string("")) != "0")
        {
            LOG_WARN("[KIS] 미체결 조회 오류: " + j.value("msg1", std::string("")));
            break;
        }

        if (j.contains("output") && j["output"].is_array())
        {
            for (auto& o : j["output"])
            {
                OpenOrder oo;
                oo.ticker    = o.value("pdno", std::string(""));
                oo.name      = o.value("prdt_name", std::string(""));
                oo.odno      = o.value("odno", o.value("ODNO", std::string("")));
                oo.krx_orgno = o.value("ord_gno_brno", std::string(""));
                oo.psbl_qty  = to_int(o.value("psbl_qty", std::string("")));
                oo.ord_unpr  = to_dbl(o.value("ord_unpr", std::string("")));
                std::string sb = o.value("sll_buy_dvsn_cd", std::string(""));
                oo.side = (sb == "01") ? OrderSide::SELL
                        : (sb == "02") ? OrderSide::BUY
                                       : OrderSide::NONE;
                if (!oo.ticker.empty() && oo.psbl_qty > 0)
                    result.push_back(oo);
            }
        }

        std::string nk_next = rtrim(j.value("ctx_area_nk100", ""));
        if (nk_next.empty())
            break; // 다음 페이지 없음
        fk = rtrim(j.value("ctx_area_fk100", ""));
        nk = nk_next;
        cont = "N";
    }
    return result;
}

// ─── HTTP 래퍼 ────────────────────────────────────────────────────────────

std::string KisClient::http_get(const std::string& url, const std::vector<std::string>& headers)
{
    // oauth2 토큰 발급 엔드포인트가 아닌 경우에만 자동 갱신 (재귀 방지)
    if (url.find("oauth2") == std::string::npos)
        ensure_authenticated();

    // KIS API는 GET에도 Content-Type: application/json 요구
    auto hdrs = headers;
    bool has_ct = false;
    for (auto& h : hdrs)
        if (h.find("Content-Type") != std::string::npos)
        {
            has_ct = true;
            break;
        }
    if (!has_ct)
        hdrs.push_back("Content-Type: application/json; charset=utf-8");
#ifdef _WIN32
    return winhttp_request("GET", url, hdrs, "");
#else
    return curl_request("GET", url, hdrs, "");
#endif
}

std::string KisClient::http_post(const std::string& url, const std::vector<std::string>& headers,
                                 const std::string& body)
{
    if (url.find("oauth2") == std::string::npos)
        ensure_authenticated();

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
std::vector<KisClient::RankingStock> KisClient::fetch_kr_ranking(int count, const std::string& market_div)
{
    std::string url = base_url() + "/uapi/domestic-stock/v1/ranking/market-cap" +
                      "?FID_COND_MRKT_DIV_CODE=" + market_div + "&FID_COND_SCR_DIV_CODE=20171" +
                      "&FID_INPUT_ISCD=0000" + "&FID_DIV_CLS_CODE=1" + "&FID_BLNG_CLS_CODE=0" + "&FID_TRGT_CLS_CODE=0" +
                      "&FID_TRGT_EXLS_CLS_CODE=0" + "&FID_RANK_SORT_CLS_CODE=0" +
                      "&FID_INPUT_PRICE_1=" + "&FID_INPUT_PRICE_2=" + "&FID_VOL_CNT=" + "&FID_INPUT_DATE_1=";

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: FHPST01720000"};

    std::string resp = http_get(url, hdrs);
    if (resp.empty())
    {
        LOG_WARN("[KIS] 랭킹 조회 실패 (" + market_div + ")");
        return {};
    }
    LOG_INFO("[KIS] 랭킹 응답: " + resp.substr(0, 400));

    std::vector<RankingStock> result;
    try
    {
        auto j = json::parse(resp);
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");
            if (s.empty())
                return 0.0;
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
                return 0;
            try
            {
                return std::stoll(s);
            }
            catch (...)
            {
                return 0;
            }
        };

        // ETF/ETN/ELW 제외: 이름 접두사 + 비정상 티커(6자리 숫자 아닌 것) 필터
        static const std::vector<std::string> ETF_PREFIXES = {
            "KODEX",    "TIGER", "KINDEX", "KOSEF",  "ARIRANG",  "ACE",       "SOL",  "HANARO",
            "FOCUS",    "TREX",  "WON",    "PLUS",   "KoAct",    "TIMEFOLIO", "KTOP", "BIG",
            "히어로즈", "KCGI",  "파워",   "KBSTAR", "마이다스", "RISE",      "TRUE", "MASTER"};
        auto is_etf_name = [&](const std::string& name)
        {
            for (const auto& pfx : ETF_PREFIXES)
                if (name.rfind(pfx, 0) == 0)
                    return true;
            return false;
        };
        // KOSPI 보통주 티커는 반드시 6자리 숫자
        auto is_normal_ticker = [](const std::string& t)
        {
            if (t.size() != 6)
                return false;
            for (char c : t)
                if (c < '0' || c > '9')
                    return false;
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
        std::sort(result.begin(), result.end(),
                  [](const RankingStock& a, const RankingStock& b)
                  { return (a.price * static_cast<double>(a.volume)) > (b.price * static_cast<double>(b.volume)); });

        // count개로 자르고 순위 재부여
        if (static_cast<int>(result.size()) > count)
            result.resize(count);
        for (int i = 0; i < static_cast<int>(result.size()); ++i)
            result[i].rank = i + 1;
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

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: FHPST01710000"};

    std::string resp = http_get(url, hdrs);
    if (resp.empty())
    {
        LOG_WARN("[KIS] 거래대금 랭킹 조회 실패 (" + market_div + ")");
        return {};
    }
    LOG_INFO("[KIS] 거래대금 랭킹 응답: " + resp.substr(0, 400));

    std::vector<RankingStock> result;
    try
    {
        auto j = json::parse(resp);
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");
            if (s.empty())
                return 0.0;
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
                return 0;
            try
            {
                return std::stoll(s);
            }
            catch (...)
            {
                return 0;
            }
        };

        // ETF/ETN/ELW 제외: 이름 접두사 + 6자리 숫자 티커만 허용(fetch_kr_ranking과 동일 규칙)
        static const std::vector<std::string> ETF_PREFIXES = {
            "KODEX",    "TIGER", "KINDEX", "KOSEF",  "ARIRANG",  "ACE",       "SOL",  "HANARO",
            "FOCUS",    "TREX",  "WON",    "PLUS",   "KoAct",    "TIMEFOLIO", "KTOP", "BIG",
            "히어로즈", "KCGI",  "파워",   "KBSTAR", "마이다스", "RISE",      "TRUE", "MASTER"};
        auto is_etf_name = [&](const std::string& name)
        {
            for (const auto& pfx : ETF_PREFIXES)
                if (name.rfind(pfx, 0) == 0)
                    return true;
            return false;
        };
        auto is_normal_ticker = [](const std::string& t)
        {
            if (t.size() != 6)
                return false;
            for (char c : t)
                if (c < '0' || c > '9')
                    return false;
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
                ticker = item.value("stck_shrn_iscd", "");
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
            std::sort(result.begin(), result.end(),
                      [](const RankingStock& a, const RankingStock& b) { return a.trade_value > b.trade_value; });

        if (static_cast<int>(result.size()) > count)
            result.resize(count);
        for (int i = 0; i < static_cast<int>(result.size()); ++i)
            result[i].rank = i + 1;
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

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: FHPTJ04400000"};

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
                return 0;
            try { return std::stoll(s); } catch (...) { return 0; }
        };
        auto safe_d = [](const nlohmann::json& o, const std::string& k) -> double
        {
            std::string s = o.value(k, "");
            if (s.empty())
                return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };

        const nlohmann::json* arr = nullptr;
        if (j.contains("output"))
            arr = &j["output"];
        else if (j.contains("output1"))
            arr = &j["output1"];
        else if (j.contains("output2"))
            arr = &j["output2"];

        if (arr && arr->is_array())
        {
            for (const auto& item : *arr)
            {
                EstInvestorFlow f;
                f.ticker = item.value("mksc_shrn_iscd", "");
                if (f.ticker.empty())
                    f.ticker = item.value("stck_shrn_iscd", "");
                f.name = item.value("hts_kor_isnm", "");
                f.foreign_net_qty = safe_i(item, "frgn_ntby_qty");
                f.inst_net_qty    = safe_i(item, "orgn_ntby_qty");
                f.foreign_net_amt = safe_d(item, "frgn_ntby_tr_pbmn");
                f.inst_net_amt    = safe_d(item, "orgn_ntby_tr_pbmn");
                if (!f.ticker.empty())
                    result.push_back(std::move(f));
            }
        }

        if (result.empty())
            LOG_WARN("[KIS] 당일 수급 랭킹 파싱 0건 — 스키마 확인용 원문: " + resp.substr(0, 500));
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

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: FHPST01720000"};

    std::string resp = http_get(url, hdrs);
    if (resp.empty())
    {
        LOG_WARN("[KIS] Universe 조회 실패 (" + market_div + ")");
        return {};
    }

    LOG_INFO("[KIS] Universe(" + market_div + ") 응답: " + resp.substr(0, 300));

    std::vector<std::string> result;
    try
    {
        auto j = json::parse(resp);
        auto& arr2 = j.contains("output2") ? j["output2"] : j["output"];
        for (const auto& item : arr2)
        {
            std::string ticker = item.value("mksc_shrn_iscd", "");
            if (ticker.empty())
                continue;

            // PBR 필터: 유효한 값이 있을 때만 적용 (0.00 = 데이터 없음 → 통과)
            std::string pbr_s = item.value("hts_pbr", "");
            if (!pbr_s.empty() && pbr_s != "0" && pbr_s != "0.00")
            {
                try
                {
                    double pbr = std::stod(pbr_s);
                    if (pbr > max_pbr)
                        continue;
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
//  해외 주식 일봉 조회
//  tr_id: HHDFS76240000
// ═══════════════════════════════════════════════════════════════════════════
std::vector<MarketData> KisClient::get_us_daily_ohlcv(const std::string& ticker, int count, const std::string& exchange)
{
    std::string url = base_url() + "/uapi/overseas-price/v1/quotations/dailyprice" + "?AUTH=" + "&EXCD=" + exchange +
                      "&SYMB=" + ticker + "&GUBN=0" + "&BYMD=" + "&MODP=0";

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: HHDFS76240000", "custtype: P"};

    std::string resp = http_get(url, hdrs);
    if (resp.empty())
    {
        LOG_WARN("[KIS-US] OHLCV 응답 없음: " + ticker);
        return {};
    }
    LOG_INFO("[KIS-US] OHLCV 응답(" + ticker + "): " + resp.substr(0, 300));

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
                    return 0.0;
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
                    md.volume = std::stoll(v);
            }
            catch (...)
            {
            }
            md.timestamp = std::chrono::system_clock::now();
            if (md.close > 0.0)
                result.push_back(md);
            if ((int)result.size() >= count)
                break;
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

    std::vector<std::string> hdrs = {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                     "appsecret: " + cfg_.app_secret, "tr_id: HHDFS00000300", "custtype: P"};

    Fundamentals f;
    f.ticker = ticker;

    std::string resp = http_get(url, hdrs);
    if (resp.empty())
    {
        LOG_WARN("[KIS-US] Fundamentals 응답 없음: " + ticker);
        return f;
    }
    LOG_INFO("[KIS-US] Fundamentals 응답(" + ticker + "): " + resp.substr(0, 300));

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
                return 0.0;
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
                return 0;
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
            f.pbr = parse_dbl("p_b_rate");
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

// ═══════════════════════════════════════════════════════════════════════════
//  해외 주식 주문
//  매수: TTTT1002U(실거래) / VTTT1002U(모의)
//  매도: TTTT1006U(실거래) / VTTT1006U(모의)
// ═══════════════════════════════════════════════════════════════════════════
bool KisClient::send_us_order(const OrderSignal& signal)
{
    std::string tr_id;
    if (signal.side == OrderSide::BUY)
        tr_id = cfg_.is_paper ? "VTTT1002U" : "TTTT1002U";
    else
        tr_id = cfg_.is_paper ? "VTTT1006U" : "TTTT1006U";

    // KIS 해외주식 주문: 시장가 = ORD_DVSN "00", 가격 "0"
    json body = {{"CANO", cfg_.account_no},
                 {"ACNT_PRDT_CD", cfg_.account_type},
                 {"OVRS_EXCG_CD", signal.exchange.empty() ? "NASD" : signal.exchange},
                 {"PDNO", signal.ticker},
                 {"ORD_DVSN", signal.type == OrderType::LIMIT ? "00" : "00"},
                 {"ORD_QTY", std::to_string(signal.quantity)},
                 {"OVRS_ORD_UNPR", signal.type == OrderType::LIMIT ? std::to_string(signal.price) : "0"}};

    std::string url = base_url() + "/uapi/overseas-stock/v1/trading/order";
    std::string resp = http_post(url,
                                 {"authorization: Bearer " + access_token_, "appkey: " + cfg_.app_key,
                                  "appsecret: " + cfg_.app_secret, "tr_id: " + tr_id, "Content-Type: application/json"},
                                 body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS-US] 주문 실패: " + signal.ticker);
        return false;
    }

    auto j = json::parse(resp);
    bool ok = (j["rt_cd"].get<std::string>() == "0");
    if (ok)
        LOG_INFO("[KIS-US] 주문 성공: " + signal.ticker + (signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(signal.quantity) + "주");
    else
        LOG_ERROR("[KIS-US] 주문 오류: " + j.value("msg1", "unknown"));
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
//  미국 Universe — 내장 S&P 500 주요 100종목 + 3일 하락 필터
//  PBR은 KIS 해외주식 API에서 미제공 시 스킵 (pbr_max=0 → PBR 조건 무시)
// ═══════════════════════════════════════════════════════════════════════════
std::vector<std::string> KisClient::fetch_us_universe_by_pbr(double max_pbr, const std::string& exchange)
{
    // S&P 500 핵심 100종목 (가치주·성장주 혼합)
    static const std::vector<std::string> NAS_LIST = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AVGO",
                                                      "ORCL", "ADBE", "AMD",  "QCOM", "TXN",   "MU",   "INTC", "AMAT",
                                                      "LRCX", "KLAC", "MRVL", "SNPS", "V",     "MA",   "PYPL", "INTU",
                                                      "CSCO", "NFLX", "COST", "SBUX", "PEP",   "MDLZ"};
    static const std::vector<std::string> NYS_LIST = {
        "BRK-B", "JPM", "BAC", "WFC", "GS",  "MS",  "C",   "AXP", "JNJ", "LLY",   "ABBV", "MRK", "PFE", "BMY", "UNH",
        "CVS",   "PG",  "KO",  "WMT", "HD",  "MCD", "NKE", "PEP", "CL",  "XOM",   "CVX",  "OXY", "COP", "SLB", "GE",
        "CAT",   "HON", "BA",  "MMM", "UPS", "FDX", "T",   "VZ",  "DIS", "CMCSA", "BX",   "KKR", "APO"};

    const auto& src = (exchange == "NYS") ? NYS_LIST : NAS_LIST;
    std::vector<std::string> result;

    for (const auto& tk : src)
    {
        if (max_pbr > 0.0)
        {
            auto f = get_us_fundamentals(tk, exchange);
            // KIS가 pbr 미제공(0.0)이면 PBR 조건 무시, per로 대리 (per=0이면 pass)
            if (f.pbr > 0.0 && f.pbr > max_pbr)
                continue;
            if (f.pbr == 0.0 && f.per > 0.0 && f.per > max_pbr * 10.0)
                continue;
        }
        result.push_back(tk);
        // KIS 초당 거래건수 제한 회피 (1req/200ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("[KIS-US] Universe(" + exchange + ") PBR<=" + std::to_string(max_pbr) +
             " 통과: " + std::to_string(result.size()) + "종목");
    return result;
}

// ─── 업종 지수 일봉 ──────────────────────────────────────────────────────────
std::vector<MarketData> KisClient::get_index_daily_ohlcv(const std::string& sector_code, int count)
{
    ensure_authenticated();

    // 이 TR(FHKUP03500100)은 응답 헤더(tr_cont) 페이지네이션 대신 날짜범위 방식.
    // 1콜당 ~50봉만 오므로(라이브 확인), DATE_2를 과거로 밀며 count봉까지 누적한다.
    // (Python get_historical_ohlcv와 동일 패턴). 결과는 최신→과거(timestamp 내림차순) 정렬.
    std::vector<MarketData> result;
    if (count <= 0) return result;

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
        if (s.size() != 8) return 0;
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

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKUP03500100",
    };

    time_t end_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
                   + 9 * 3600;                // KST 기준 '오늘'
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
            if (resp.empty()) break;
            auto j = json::parse(resp, nullptr, false);
            if (j.is_discarded() || !j.contains("output2")) break;
            auto& arr = j["output2"];
            if (arr.empty()) break;

            std::string page_oldest;
            int added = 0;
            for (const auto& item : arr)              // output2는 최신→과거 순
            {
                std::string bd = item.value("stck_bsop_date", "");
                // 페이지 경계 중복 방지: 이미 받은 범위(>= oldest_seen)는 스킵
                if (!oldest_seen.empty() && !bd.empty() && bd >= oldest_seen) continue;
                MarketData d;
                d.ticker = sector_code;
                d.close  = sd(item, "bstp_nmix_prpr");
                d.open   = sd(item, "bstp_nmix_oprc");
                d.high   = sd(item, "bstp_nmix_hgpr");
                d.low    = sd(item, "bstp_nmix_lwpr");
                d.volume = static_cast<int64_t>(sd(item, "acml_vol"));
                if (!bd.empty())
                    d.timestamp = std::chrono::system_clock::from_time_t(parse_ymd(bd));
                result.push_back(d);
                ++added;
                if (!bd.empty() && (page_oldest.empty() || bd < page_oldest)) page_oldest = bd;
                if (static_cast<int>(result.size()) >= count) break;
            }

            if (page_oldest.empty() || added == 0) break;   // 새 데이터 없음 → 종료
            oldest_seen = page_oldest;
            time_t ot = parse_ymd(page_oldest);
            if (ot == 0) break;
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
        LOG_WARN("[KIS] 업종 일봉(" + sector_code + ") 요청 " + std::to_string(count) +
                 "봉 중 " + std::to_string(result.size()) + "봉만 수집 (페이지 한계/데이터 부족)");
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
        "&FID_INPUT_CNT_1=" + std::to_string(count) +
        "&FID_PRC_CLS_CODE=0"
        "&FID_TRGT_CLS_CODE=0"
        "&FID_TRGT_EXLS_CLS_CODE=0"
        "&FID_INPUT_PRICE_1=&FID_INPUT_PRICE_2="
        "&FID_VOL_CNT=&FID_INPUT_DATE_1=";

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHPST01700000",
    };

    std::vector<RankingStock> result;
    try
    {
        auto resp = http_get(url, hdrs);
        if (resp.empty()) return result;

        LOG_INFO("[KIS] 업종 등락률 응답(" + sector_code + "): " + resp.substr(0, 300));

        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded()) return result;

        auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
            try { return std::stod(o.value(k, "0")); } catch (...) { return 0.0; }
        };
        auto si = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            try { return std::stoll(o.value(k, "0")); } catch (...) { return 0; }
        };
        auto is_normal_ticker = [](const std::string& t) {
            if (t.size() != 6) return false;
            for (char c : t) if (c < '0' || c > '9') return false;
            return true;
        };

        auto& arr = j.contains("output2") ? j["output2"] : j["output"];
        for (const auto& item : arr)
        {
            std::string ticker = item.value("mksc_shrn_iscd", "");
            if (!is_normal_ticker(ticker)) continue;

            RankingStock s;
            s.ticker      = ticker;
            s.name        = item.value("hts_kor_isnm", "");
            s.price       = sd(item, "stck_prpr");
            s.change_rate = sd(item, "prdy_ctrt");
            s.volume      = si(item, "acml_vol");
            result.push_back(s);
            if (static_cast<int>(result.size()) >= count) break;
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[KIS] 업종 등락률(" + sector_code + ") 오류: " + e.what());
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

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST01010900",
    };

    InvestorTrend result;
    result.ticker = ticker;
    try
    {
        auto resp = http_get(url, hdrs);
        if (resp.empty()) return result;

        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded() || !j.contains("output")) return result;

        // output 배열 최신(오늘) 데이터만 사용
        auto& arr = j["output"];
        if (arr.empty()) return result;
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

    std::vector<std::string> hdrs = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHKST01010900",
    };

    std::vector<InvestorFlow> result;
    try
    {
        auto resp = http_get(url, hdrs);
        if (resp.empty()) return result;

        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded() || !j.contains("output")) return result;

        auto si = [](const nlohmann::json& o, const std::string& k) -> int64_t {
            std::string s = o.value(k, "");
            if (s.empty()) return 0;
            // KIS 부호 있는 수치 문자열: 앞에 + / - 포함 가능
            try { return std::stoll(s); } catch (...) { return 0; }
        };
        auto sd = [](const nlohmann::json& o, const std::string& k) -> double {
            std::string s = o.value(k, "");
            if (s.empty()) return 0.0;
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
                result.push_back(std::move(f));
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

    std::vector<std::string> headers = {
        "authorization: Bearer " + access_token_,
        "appkey: " + cfg_.app_key,
        "appsecret: " + cfg_.app_secret,
        "tr_id: FHPUP02100000",
        "Content-Type: application/json",
    };

    IndexPrice ip;
    ip.ticker = ticker;

    try
    {
        auto resp = http_get(url, headers);
        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded() || !j.contains("output"))
            return ip;

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
