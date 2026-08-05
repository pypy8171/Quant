#include "api/KisWebSocket.h"
#include "utils/Logger.h"
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

// 체결통보(H0STCNI) AES-256-CBC 복호화용
#ifdef _WIN32
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
//  공통 유틸
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> KisWebSocket::split_str(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::string tok;
    for (char c : s)
    {
        if (c == delim)
        {
            out.push_back(tok);
            tok.clear();
        }
        else
        {
            tok += c;
        }
    }
    out.push_back(tok);
    return out;
}

// ─── 체결통보 복호화 (KIS H0STCNI: base64 → AES-256-CBC) ────────────────────
// 시세 채널은 평문이나 체결통보는 암호화 전송. key/iv는 구독 응답 body.output에서 획득.
std::string KisWebSocket::base64_decode(const std::string& in)
{
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int T[256];
    for (int i = 0; i < 256; ++i)
        T[i] = -1;
    for (int i = 0; i < 64; ++i)
        T[static_cast<unsigned char>(chars[i])] = i;

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in)
    {
        if (T[c] == -1) // '=' / 개행 / 공백 무시
            continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

#ifdef _WIN32
// Windows: BCrypt(CNG) — AES-256-CBC, PKCS7 패딩 제거
std::string KisWebSocket::aes_cbc_decrypt(const std::string& cipher,
                                          const std::string& key,
                                          const std::string& iv)
{
    if (cipher.empty() || cipher.size() % 16 != 0 || key.size() != 32 || iv.size() != 16)
        return "";

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return "";
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                      (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);

    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   (PUCHAR)key.data(), 32, 0) == 0)
    {
        std::vector<UCHAR> ivbuf(iv.begin(), iv.begin() + 16); // BCrypt가 IV를 갱신하므로 복사
        std::string out(cipher.size(), '\0');
        ULONG outLen = 0;
        if (BCryptDecrypt(hKey,
                          (PUCHAR)cipher.data(), (ULONG)cipher.size(),
                          nullptr, ivbuf.data(), (ULONG)ivbuf.size(),
                          (PUCHAR)&out[0], (ULONG)out.size(), &outLen,
                          BCRYPT_BLOCK_PADDING) == 0)
        {
            result.assign(out.data(), outLen);
        }
    }

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}
#else
// Linux: OpenSSL EVP — AES-256-CBC, PKCS7 패딩 제거(DecryptFinal)
std::string KisWebSocket::aes_cbc_decrypt(const std::string& cipher,
                                          const std::string& key,
                                          const std::string& iv)
{
    if (cipher.empty() || cipher.size() % 16 != 0 || key.size() != 32 || iv.size() != 16)
        return "";

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return "";

    std::string out(cipher.size() + 16, '\0');
    int len = 0, total = 0;
    std::string result;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(iv.data())) == 1 &&
        EVP_DecryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(&out[0]), &len,
            reinterpret_cast<const unsigned char*>(cipher.data()),
            static_cast<int>(cipher.size())) == 1)
    {
        total = len;
        if (EVP_DecryptFinal_ex(ctx,
                reinterpret_cast<unsigned char*>(&out[0]) + total, &len) == 1)
        {
            total += len;
            result.assign(out.data(), total);
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    return result;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  플랫폼별 구현
// ═══════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
// ─── Windows: WinHTTP ───────────────────────────────────────────────────────
#pragma comment(lib, "winhttp.lib")

std::wstring KisWebSocket::to_wide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string KisWebSocket::http_post_json(const std::string& url, const std::string& body)
{
    std::wstring wurl = to_wide(url);
    wchar_t host[512]{}, path[4096]{};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc);

    HINTERNET hSess = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess)
        return "";

    HINTERNET hConn = WinHttpConnect(hSess, host, uc.nPort, 0);
    if (!hConn)
    {
        WinHttpCloseHandle(hSess);
        return "";
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq =
        WinHttpOpenRequest(hConn, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq)
    {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return "";
    }

    WinHttpAddRequestHeaders(hReq, L"Content-Type: application/json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(hReq, nullptr);

    std::string resp;
    if (ok)
    {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
        {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            WinHttpReadData(hReq, &chunk[0], avail, &read);
            resp.append(chunk, 0, read);
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return resp;
}

// ─── Windows WebSocket 연결 ──────────────────────────────────────────────────
bool KisWebSocket::connect(const std::vector<WatchSpec>& specs)
{
    specs_ = specs;
    if (!get_approval_key())
        return false;

    const wchar_t* ws_host = L"ops.koreainvestment.com";
    INTERNET_PORT ws_port = cfg_.is_paper ? 31000 : 21000;

    hSession_ = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession_)
    {
        LOG_ERROR("[WS] WinHttpOpen 실패");
        return false;
    }

    hConnect_ = WinHttpConnect(hSession_, ws_host, ws_port, 0);
    if (!hConnect_)
    {
        LOG_ERROR("[WS] WinHttpConnect 실패");
        return false;
    }

    HINTERNET hReq =
        WinHttpOpenRequest(hConnect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq)
    {
        LOG_ERROR("[WS] WinHttpOpenRequest 실패");
        return false;
    }

    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr))
    {
        LOG_ERROR("[WS] WebSocket 업그레이드 요청 실패: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hReq);
        return false;
    }

    hWebSocket_ = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    WinHttpCloseHandle(hReq);

    if (!hWebSocket_)
    {
        LOG_ERROR("[WS] WinHttpWebSocketCompleteUpgrade 실패: " + std::to_string(GetLastError()));
        return false;
    }

    LOG_INFO(std::string("[WS] WebSocket 연결 성공 (") + (cfg_.is_paper ? "모의투자" : "실거래") + ")");
    connected_.store(true);

    bool has_kr = false;
    for (const auto& spec : specs_)
    {
        if (spec.market == Market::KR)
        {
            has_kr = true;
            if (!spec.trade_only)
                send_subscribe("H0STASP0", spec.ticker);
            send_subscribe("H0STCNT0", spec.ticker);
        }
        else
        {
            // 미국: HDFSCNT0, tr_key = "EXCH|SYMBOL"
            std::string exch = spec.exchange.empty() ? "NAS" : spec.exchange;
            send_subscribe("HDFSCNT0", exch + "|" + spec.ticker);
        }
    }

    // 체결통보 구독 — hts_id가 명시된 경우에만 (비어있으면 건너뜀)
    if (has_kr && on_fill_ && !cfg_.hts_id.empty())
    {
        std::string fill_tr = cfg_.is_paper ? "H0STCNI9" : "H0STCNI0";
        send_subscribe(fill_tr, cfg_.hts_id);
    }
    else if (has_kr && on_fill_ && cfg_.hts_id.empty())
    {
        LOG_WARN("[WS] hts_id 미설정 — 체결통보(H0STCNI9/0) 구독 건너뜀. 주문 실행은 정상 동작.");
    }

    // 구 수신 스레드가 자체 종료(connected_=false)로 join되지 않은 채 남아 있을 수 있다.
    // joinable 상태에서 재대입하면 std::terminate → 재대입 전 반드시 reap.
    if (recv_thread_.joinable())
        recv_thread_.join();
    recv_thread_ = std::thread(&KisWebSocket::recv_loop, this);
    return true;
}

void KisWebSocket::send_text(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!hWebSocket_)
        return;
    WinHttpWebSocketSend(hWebSocket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)msg.data(),
                         (DWORD)msg.size());
}

void KisWebSocket::recv_loop()
{
    LOG_INFO("[WS] 수신 스레드 시작");
    std::vector<BYTE> buf(128 * 1024);
    int retry_sec = 1;

    while (connected_.load())
    {
        // ── 수신 루프 ───────────────────────────────────────────────────────
        std::string accumulated;
        bool recv_ok = true;
        // 백오프는 "메시지 수신"이 아니라 "연결 유지 시간"으로 판단한다. 재연결 직후
        // 구독응답/에러프레임(ALREADY IN USE)이 곧바로 수신되면 메시지 기반 리셋은
        // 백오프를 매번 1초로 되돌려 폭주한다. 연결이 얼마나 살아있었는지로 구분한다.
        const auto conn_start = std::chrono::steady_clock::now();

        while (connected_.load())
        {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType{};
            DWORD rc = WinHttpWebSocketReceive(hWebSocket_, buf.data(), (DWORD)buf.size(), &bytesRead, &bufType);

            if (rc != ERROR_SUCCESS)
            {
                if (connected_.load())
                    LOG_WARN("[WS] 수신 오류 (code=" + std::to_string(rc) + ") — 재연결 준비");
                recv_ok = false;
                break;
            }

            std::string chunk(reinterpret_cast<char*>(buf.data()), bytesRead);
            if (bufType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                accumulated += chunk;
            else if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            {
                accumulated += chunk;
                parse_message(accumulated);
                accumulated.clear();
            }
        }

        if (!recv_ok && connected_.load())
        {
            // 충분히 오래(≥5s) 유지된 연결이 끊긴 것이면 일시 장애로 보고 백오프 리셋 후
            // 빠르게 재시도. 즉시 죽는 연결(=서버가 appkey 세션 미해제/off-hours abort)은
            // 지수적으로 물러서서 서버가 직전 세션을 놓을 시간을 준다.
            if (std::chrono::steady_clock::now() - conn_start > std::chrono::seconds(5))
                retry_sec = 1;

            // ── 지수 백오프 재연결 ─────────────────────────────────────────
            LOG_WARN("[WS] " + std::to_string(retry_sec) + "초 후 재연결 시도");
            std::this_thread::sleep_for(std::chrono::seconds(retry_sec));
            retry_sec = std::min(retry_sec * 2, 30);

            // 기존 핸들 정리
            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                if (hWebSocket_)
                {
                    WinHttpCloseHandle(hWebSocket_);
                    hWebSocket_ = nullptr;
                }
            }
            if (hConnect_)
            {
                WinHttpCloseHandle(hConnect_);
                hConnect_ = nullptr;
            }
            if (hSession_)
            {
                WinHttpCloseHandle(hSession_);
                hSession_ = nullptr;
            }

            // Approval key 재사용: 재연결마다 신규 발급하면 KIS가 직전 세션의 appkey를
            // 아직 해제하지 않은 상태에서 새 키로 접속 → "ALREADY IN USE appkey"(rt=9)
            // 충돌이 반복돼 재연결 폭주가 된다. 최초 연결의 approval_key_를 유지하고,
            // 비어있을 때만(발급 실패 이력 등) 재발급한다.
            if (approval_key_.empty() && !get_approval_key())
            {
                LOG_ERROR("[WS] approval key 발급 실패");
                continue;
            }

            // WinHTTP 재연결
            const wchar_t* ws_host = L"ops.koreainvestment.com";
            INTERNET_PORT ws_port = cfg_.is_paper ? 31000 : 21000;

            hSession_ = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession_)
            {
                LOG_ERROR("[WS] 재연결: WinHttpOpen 실패");
                continue;
            }

            hConnect_ = WinHttpConnect(hSession_, ws_host, ws_port, 0);
            if (!hConnect_)
            {
                WinHttpCloseHandle(hSession_);
                hSession_ = nullptr;
                LOG_ERROR("[WS] 재연결: WinHttpConnect 실패");
                continue;
            }

            HINTERNET hReq = WinHttpOpenRequest(hConnect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (!hReq)
            {
                LOG_ERROR("[WS] 재연결: OpenRequest 실패");
                continue;
            }

            WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
            if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
                !WinHttpReceiveResponse(hReq, nullptr))
            {
                WinHttpCloseHandle(hReq);
                LOG_ERROR("[WS] 재연결: 업그레이드 실패: " + std::to_string(GetLastError()));
                continue;
            }

            HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
            WinHttpCloseHandle(hReq);
            if (!hWs)
            {
                LOG_ERROR("[WS] 재연결: CompleteUpgrade 실패");
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                hWebSocket_ = hWs;
            }

            // 재연결: 옛 연결의 체결통보 key/iv는 무효 — 새 구독응답 도착 전까지
            // 암호프레임을 drop해 stale 키 복호를 막는다 (C-3)
            aes_key_.clear();
            aes_iv_.clear();

            // 채널 재구독
            bool has_kr_rc = false;
            for (const auto& spec : specs_)
            {
                if (spec.market == Market::KR)
                {
                    has_kr_rc = true;
                    send_subscribe("H0STASP0", spec.ticker);
                    send_subscribe("H0STCNT0", spec.ticker);
                }
                else
                {
                    std::string exch = spec.exchange.empty() ? "NAS" : spec.exchange;
                    send_subscribe("HDFSCNT0", exch + "|" + spec.ticker);
                }
            }
            // 체결통보 채널 재구독 — hts_id 설정 시에만
            if (has_kr_rc && on_fill_ && !cfg_.hts_id.empty())
            {
                std::string fill_tr = cfg_.is_paper ? "H0STCNI9" : "H0STCNI0";
                send_subscribe(fill_tr, cfg_.hts_id);
            }
            LOG_INFO("[WS] 재연결 성공");
            // 여기서 retry_sec을 리셋하지 않는다. '재연결 성공'은 소켓 업그레이드 성공일
            // 뿐, 직후 곧바로 끊기는(ALREADY IN USE) 경우 백오프가 매번 1초로 되돌아가
            // 폭주한다. 백오프 리셋은 연결이 실제로 ≥5s 유지됐을 때만(위 uptime 판정).
        }
    }

    connected_.store(false);
    LOG_INFO("[WS] 수신 스레드 종료");
}

void KisWebSocket::disconnect()
{
    if (!connected_.exchange(false))
        return;

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        if (hWebSocket_)
        {
            WinHttpCloseHandle(hWebSocket_);
            hWebSocket_ = nullptr;
        }
    }

    if (recv_thread_.joinable())
        recv_thread_.join();

    if (hConnect_)
    {
        WinHttpCloseHandle(hConnect_);
        hConnect_ = nullptr;
    }
    if (hSession_)
    {
        WinHttpCloseHandle(hSession_);
        hSession_ = nullptr;
    }

    LOG_INFO("[WS] WebSocket 연결 해제 완료");
}

#else
// ─── Linux: libcurl (HTTP) + POSIX socket (WebSocket) ─────────────────────
#include <cerrno>
#include <cstring>
#include <curl/curl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

std::wstring KisWebSocket::to_wide(const std::string&)
{
    return {};
}

static size_t curl_write_cb(char* p, size_t sz, size_t nm, std::string* out)
{
    out->append(p, sz * nm);
    return sz * nm;
}

std::string KisWebSocket::http_post_json(const std::string& url, const std::string& body)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return "";

    std::string resp;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        LOG_ERROR(std::string("[WS] HTTP POST 오류: ") + curl_easy_strerror(rc));

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

// ─── POSIX socket 헬퍼 ────────────────────────────────────────────────────

static bool sock_recv_all(int fd, void* buf, size_t len)
{
    auto* p = static_cast<char*>(buf);
    while (len > 0)
    {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// RFC 6455 WebSocket TCP 연결 + HTTP Upgrade 핸드셰이크
static bool ws_tcp_connect(const std::string& host, int port, int& out_fd)
{
    // AF_INET(IPv4) 강제: AF_UNSPEC면 getaddrinfo가 IPv6를 먼저 줄 수 있고,
    // Docker에서 IPv6 connect는 성공하나 KIS WS가 IPv6 핸드셰이크에 무응답 → recv 실패.
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0)
    {
        LOG_WARN(std::string("[WS] getaddrinfo 실패: ") + gai_strerror(gai));
        return false;
    }

    int fd = -1;
    for (auto* p = res; p; p = p->ai_next)
    {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
    {
        LOG_WARN("[WS] TCP connect 실패 (" + host + ":" + std::to_string(port) + "): " +
                 std::strerror(errno));
        return false;
    }

    // 핸드셰이크 응답 수신 타임아웃 (무응답 시 무한대기 방지)
    struct timeval tv{};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // HTTP/1.1 Upgrade 핸드셰이크
    std::string req = "GET / HTTP/1.1\r\n"
                      "Host: " +
                      host + ":" + std::to_string(port) +
                      "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";

    if (::send(fd, req.c_str(), req.size(), 0) < 0)
    {
        LOG_WARN(std::string("[WS] 핸드셰이크 send 실패: ") + std::strerror(errno));
        ::close(fd);
        return false;
    }

    // 응답을 헤더 끝(\r\n\r\n)까지 읽기 (부분 수신 대비)
    std::string resp;
    char buf[1024];
    for (int i = 0; i < 8; ++i)
    {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find("\r\n\r\n") != std::string::npos)
            break;
    }
    if (resp.find("101") == std::string::npos)
    {
        LOG_WARN("[WS] 핸드셰이크 응답에 101 없음 (recv " + std::to_string(resp.size()) +
                 "B): " + resp.substr(0, 120));
        ::close(fd);
        return false;
    }

    // 핸드셰이크 후 데이터 수신은 블로킹(타임아웃 해제) — recv_loop/disconnect가 관리
    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    out_fd = fd;
    return true;
}

// RFC 6455 텍스트 프레임 송신 (client→server, MASK=1 필수)
static void ws_send_text_linux(int fd, const std::string& data)
{
    size_t len = data.size();
    std::vector<uint8_t> frame;
    frame.reserve(6 + len);

    frame.push_back(0x81); // FIN=1, opcode=1(Text)

    if (len <= 125)
        frame.push_back(uint8_t(0x80 | len));
    else if (len <= 65535)
    {
        frame.push_back(0x80 | 126);
        frame.push_back(uint8_t(len >> 8));
        frame.push_back(uint8_t(len));
    }
    else
    {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(uint8_t(len >> (8 * i)));
    }

    // 마스킹 키
    const uint8_t mk[4] = {0x37, 0x1A, 0xC5, 0x4F};
    frame.insert(frame.end(), mk, mk + 4);
    for (size_t i = 0; i < len; ++i)
        frame.push_back(uint8_t(data[i]) ^ mk[i % 4]);

    ::send(fd, frame.data(), frame.size(), 0);
}

// RFC 6455 프레임 수신 → 텍스트 반환 (빈 문자열 = 연결 종료/오류)
static std::string ws_recv_frame_linux(int fd)
{
    uint8_t hdr[2];
    if (!sock_recv_all(fd, hdr, 2))
        return "";

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] >> 7) & 1;
    size_t len = hdr[1] & 0x7F;

    if (len == 126)
    {
        uint8_t ext[2];
        if (!sock_recv_all(fd, ext, 2))
            return "";
        len = (size_t(ext[0]) << 8) | ext[1];
    }
    else if (len == 127)
    {
        uint8_t ext[8];
        if (!sock_recv_all(fd, ext, 8))
            return "";
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | ext[i];
    }

    uint8_t mk[4]{};
    if (masked && !sock_recv_all(fd, mk, 4))
        return "";

    std::vector<uint8_t> payload(len);
    if (len > 0 && !sock_recv_all(fd, payload.data(), len))
        return "";
    if (masked)
        for (size_t i = 0; i < len; ++i)
            payload[i] ^= mk[i % 4];

    if (opcode == 0x8)
        return ""; // Close frame
    if (opcode != 0x1 && opcode != 0x2 && opcode != 0x0)
    {
        // Ping(0x9) → Pong(0xA)
        if (opcode == 0x9)
        {
            std::vector<uint8_t> pong = {0x8A, uint8_t(0x80 | (len & 0x7F)), 0x00, 0x00, 0x00, 0x00};
            pong.insert(pong.end(), payload.begin(), payload.end());
            ::send(fd, pong.data(), pong.size(), 0);
        }
        return ws_recv_frame_linux(fd); // 다음 프레임 수신
    }
    return std::string(payload.begin(), payload.end());
}

// ─── Linux WebSocket 연결 ─────────────────────────────────────────────────
bool KisWebSocket::connect(const std::vector<WatchSpec>& specs)
{
    specs_ = specs;
    if (!get_approval_key())
        return false;

    std::string host = "ops.koreainvestment.com";
    int port = cfg_.is_paper ? 31000 : 21000;

    if (!ws_tcp_connect(host, port, sock_fd_))
    {
        LOG_ERROR("[WS] TCP+WebSocket 연결 실패: " + host + ":" + std::to_string(port));
        return false;
    }

    LOG_INFO(std::string("[WS] WebSocket 연결 성공 (Linux, ") + (cfg_.is_paper ? "모의투자" : "실거래") + ")");
    connected_.store(true);

    bool has_kr = false;
    for (const auto& spec : specs_)
    {
        if (spec.market == Market::KR)
        {
            has_kr = true;
            if (!spec.trade_only)
                send_subscribe("H0STASP0", spec.ticker);
            send_subscribe("H0STCNT0", spec.ticker);
        }
        else
        {
            std::string exch = spec.exchange.empty() ? "NAS" : spec.exchange;
            send_subscribe("HDFSCNT0", exch + "|" + spec.ticker);
        }
    }

    // 체결통보 구독 — hts_id가 명시된 경우에만 (비어있으면 건너뜀)
    if (has_kr && on_fill_ && !cfg_.hts_id.empty())
    {
        std::string fill_tr = cfg_.is_paper ? "H0STCNI9" : "H0STCNI0";
        send_subscribe(fill_tr, cfg_.hts_id);
    }
    else if (has_kr && on_fill_ && cfg_.hts_id.empty())
    {
        LOG_WARN("[WS] hts_id 미설정 — 체결통보(H0STCNI9/0) 구독 건너뜀. 주문 실행은 정상 동작.");
    }

    // 구 수신 스레드가 자체 종료(connected_=false)로 join되지 않은 채 남아 있을 수 있다.
    // joinable 상태에서 재대입하면 std::terminate → 재대입 전 반드시 reap.
    if (recv_thread_.joinable())
        recv_thread_.join();
    recv_thread_ = std::thread(&KisWebSocket::recv_loop, this);
    return true;
}

void KisWebSocket::send_text(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (sock_fd_ < 0)
        return;
    ws_send_text_linux(sock_fd_, msg);
}

void KisWebSocket::recv_loop()
{
    LOG_INFO("[WS] 수신 스레드 시작");
    int retry_sec = 1;

    while (connected_.load())
    {
        // ── 수신 루프 ───────────────────────────────────────────────────────
        int fd;
        {
            std::lock_guard<std::mutex> lk(send_mtx_);
            fd = sock_fd_;
        }

        bool recv_ok = true;
        // 백오프는 연결 유지 시간으로 판단(메시지 기반 리셋은 재연결 직후 프레임에 매번
        // 리셋돼 폭주). Windows 경로와 동일 모델.
        const auto conn_start = std::chrono::steady_clock::now();
        while (connected_.load() && fd >= 0)
        {
            std::string frame = ws_recv_frame_linux(fd);
            if (frame.empty())
            {
                if (connected_.load())
                    LOG_WARN("[WS] 수신 오류 또는 연결 종료 — 재연결 준비");
                recv_ok = false;
                break;
            }
            parse_message(frame);
        }

        if (!recv_ok && connected_.load())
        {
            // 충분히 오래 유지된 연결이 끊긴 것이면 백오프 리셋 후 빠른 재시도,
            // 즉시 죽는 연결은 지수적으로 물러선다(서버 appkey 세션 해제 대기).
            if (std::chrono::steady_clock::now() - conn_start > std::chrono::seconds(5))
                retry_sec = 1;

            // ── 지수 백오프 재연결 ─────────────────────────────────────────
            LOG_WARN("[WS] " + std::to_string(retry_sec) + "초 후 재연결 시도");
            std::this_thread::sleep_for(std::chrono::seconds(retry_sec));
            retry_sec = std::min(retry_sec * 2, 30);

            // 기존 소켓 정리
            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                if (sock_fd_ >= 0)
                {
                    ::close(sock_fd_);
                    sock_fd_ = -1;
                }
            }

            // Approval key 재사용(재연결마다 신규 발급 시 appkey 세션 충돌 → 폭주).
            // Windows 경로와 동일: 비어있을 때만 재발급.
            if (approval_key_.empty() && !get_approval_key())
            {
                LOG_ERROR("[WS] approval key 발급 실패");
                continue;
            }

            std::string host = "ops.koreainvestment.com";
            int port = cfg_.is_paper ? 31000 : 21000;
            int new_fd = -1;
            if (!ws_tcp_connect(host, port, new_fd))
            {
                LOG_ERROR("[WS] 재연결: TCP+WebSocket 연결 실패");
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                sock_fd_ = new_fd;
            }

            // 재연결: 옛 연결의 체결통보 key/iv는 무효 — 새 구독응답 도착 전까지
            // 암호프레임을 drop해 stale 키 복호를 막는다 (C-3)
            aes_key_.clear();
            aes_iv_.clear();

            // 채널 재구독
            bool has_kr_rc = false;
            for (const auto& spec : specs_)
            {
                if (spec.market == Market::KR)
                {
                    has_kr_rc = true;
                    send_subscribe("H0STASP0", spec.ticker);
                    send_subscribe("H0STCNT0", spec.ticker);
                }
                else
                {
                    std::string exch = spec.exchange.empty() ? "NAS" : spec.exchange;
                    send_subscribe("HDFSCNT0", exch + "|" + spec.ticker);
                }
            }
            // 체결통보 채널 재구독 — hts_id 설정 시에만
            if (has_kr_rc && on_fill_ && !cfg_.hts_id.empty())
            {
                std::string fill_tr = cfg_.is_paper ? "H0STCNI9" : "H0STCNI0";
                send_subscribe(fill_tr, cfg_.hts_id);
            }
            LOG_INFO("[WS] 재연결 성공");
            // 여기서 retry_sec을 리셋하지 않는다. '재연결 성공'은 소켓 업그레이드 성공일
            // 뿐, 직후 곧바로 끊기는(ALREADY IN USE) 경우 백오프가 매번 1초로 되돌아가
            // 폭주한다. 백오프 리셋은 연결이 실제로 ≥5s 유지됐을 때만(위 uptime 판정).
        }
    }

    connected_.store(false);
    LOG_INFO("[WS] 수신 스레드 종료");
}

void KisWebSocket::disconnect()
{
    if (!connected_.exchange(false))
        return;

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        if (sock_fd_ >= 0)
        {
            // W-1: close()는 다른 스레드가 recv() 블로킹 중일 때 깨운다는 보장이 없다(POSIX).
            // shutdown(SHUT_RDWR)은 블로킹된 recv를 즉시 깨워 half-open 교착(join 무한대기)을 막는다.
            ::shutdown(sock_fd_, SHUT_RDWR);
            ::close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    if (recv_thread_.joinable())
        recv_thread_.join();
    LOG_INFO("[WS] WebSocket 연결 해제 완료");
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  공통 KisWebSocket 구현 (플랫폼 독립)
// ═══════════════════════════════════════════════════════════════════════════

KisWebSocket::KisWebSocket(const KisConfig& cfg) : cfg_(cfg)
{
}
KisWebSocket::~KisWebSocket()
{
    disconnect();
}

void KisWebSocket::set_callbacks(OrderBookCb on_ob, TradeCb on_trade)
{
    on_orderbook_ = std::move(on_ob);
    on_trade_     = std::move(on_trade);
}

void KisWebSocket::set_fill_callback(FillCb on_fill)
{
    on_fill_ = std::move(on_fill);
}

bool KisWebSocket::get_approval_key()
{
    std::string base =
        cfg_.is_paper ? "https://openapivts.koreainvestment.com:29443" : "https://openapi.koreainvestment.com:9443";

    json req = {{"grant_type", "client_credentials"}, {"appkey", cfg_.app_key}, {"secretkey", cfg_.app_secret}};

    std::string resp = http_post_json(base + "/oauth2/Approval", req.dump());
    if (resp.empty())
    {
        LOG_ERROR("[WS] Approval key 요청 실패");
        return false;
    }

    try
    {
        auto j = json::parse(resp);
        approval_key_ = j["approval_key"].get<std::string>();
        LOG_INFO("[WS] Approval key 발급 성공");
        return true;
    }
    catch (...)
    {
        LOG_ERROR("[WS] Approval key 파싱 실패: " + resp.substr(0, 300));
        return false;
    }
}

void KisWebSocket::send_subscribe(const std::string& tr_id, const std::string& ticker)
{
    json msg = {
        {"header", {{"approval_key", approval_key_}, {"custtype", "P"}, {"tr_type", "1"}, {"content-type", "utf-8"}}},
        {"body", {{"input", {{"tr_id", tr_id}, {"tr_key", ticker}}}}}};
    send_text(msg.dump());
    LOG_INFO("[WS] 구독: " + tr_id + " / " + ticker);
}

void KisWebSocket::parse_message(const std::string& msg)
{
    if (msg.empty())
        return;

    on_message_received(); // 모든 수신 메시지에서 stale 타이머 리셋

    if (msg[0] == '{')
    {
        try
        {
            auto j = json::parse(msg);
            std::string tr_id = j["header"].value("tr_id", "");

            if (tr_id == "PINGPONG")
            {
                send_text(msg);
                return;
            }

            if (j.contains("body"))
            {
                std::string rt = j["body"].value("rt_cd", "?");
                std::string msg1 = j["body"].value("msg1", "");
                std::string tk = j["header"].value("tr_key", "");
                LOG_INFO("[WS] " + tr_id + "(" + tk + ") rt=" + rt + " " + msg1);

                // 체결통보 구독 응답: AES key/iv 확보 → 이후 암호화 프레임 복호화에 사용
                if ((tr_id == "H0STCNI0" || tr_id == "H0STCNI9") &&
                    j["body"].contains("output"))
                {
                    const auto& out = j["body"]["output"];
                    std::string k = out.value("key", "");
                    std::string v = out.value("iv", "");
                    // AES-256-CBC: key는 정확히 32바이트, iv는 16바이트여야 함.
                    // 길이가 다르면(서버 포맷 변경 등) 앞 N바이트만 써서 잘못된 키로
                    // 복호→쓰레기 평문이 원장에 들어가므로 등호 검증 후 거부 (C-2)
                    if (k.size() == 32 && v.size() == 16)
                    {
                        aes_key_ = std::move(k);
                        aes_iv_  = std::move(v);
                        LOG_INFO("[WS] 체결통보 AES key/iv 확보 — 복호화 준비 완료");
                    }
                    else
                    {
                        aes_key_.clear();
                        aes_iv_.clear();
                        LOG_WARN("[WS] 체결통보 key/iv 길이 비정상 (key=" +
                                 std::to_string(k.size()) + " iv=" + std::to_string(v.size()) +
                                 ") — 복호화 불가, 암호프레임 drop");
                    }
                }
            }
        }
        catch (...)
        {
        }
        return;
    }

    // 데이터 메시지: TYPE|TR_ID|COUNT|DATA (^-구분 필드)
    auto parts = split_str(msg, '|');
    if (parts.size() < 4)
        return;

    const std::string& tr_id = parts[1];
    std::string data = parts[3]; // 암호화 시 복호문으로 교체되므로 값 복사

    // 암호화 프레임(체결통보 H0STCNI): parts[0]=="1" → base64 + AES-256-CBC 복호화
    if (parts[0] == "1")
    {
        if (aes_key_.empty() || aes_iv_.empty())
        {
            LOG_WARN("[WS] 암호화 프레임 수신했으나 key/iv 미확보 — drop tr_id=" + tr_id);
            return;
        }
        std::string plain = aes_cbc_decrypt(base64_decode(data), aes_key_, aes_iv_);
        if (plain.empty())
        {
            LOG_WARN("[WS] 체결통보 복호화 실패 tr_id=" + tr_id);
            return;
        }
        // 복호 평문은 ^구분 다필드(체결통보 23필드). 너무 적으면 키 불일치/손상 의심 (C-1)
        if (split_str(plain, '^').size() < 14)
        {
            LOG_WARN("[WS] 체결통보 복호 평문 비정상(필드부족) — 키 불일치/손상 의심 tr_id=" + tr_id);
            return;
        }
        data = std::move(plain);
    }

    auto fields = split_str(data, '^');

    if (tr_id == "H0STASP0")
        parse_orderbook(fields);
    else if (tr_id == "H0STCNT0")
        parse_kr_trade(fields);
    else if (tr_id == "HDFSCNT0")
        parse_us_trade(fields);
    else if (tr_id == "H0STCNI0" || tr_id == "H0STCNI9")
        parse_fill_notification(fields);
}

// ─── 호가 파싱 (H0STASP0) ────────────────────────────────────────────────
// KIS H0STASP0 실제 필드 구조 (10단계):
//  [0]종목코드 [1]시간 [2]시간구분
//  [3-12]  ASKP1-10   매도호가 10단계
//  [13-22] BIDP1-10   매수호가 10단계
//  [23-32] ASKP_RSQN1-10 매도호가잔량
//  [33-42] BIDP_RSQN1-10 매수호가잔량
//  → 5단계만 사용: asks[i] = f[3+i]/f[23+i], bids[i] = f[13+i]/f[33+i]
void KisWebSocket::parse_orderbook(const std::vector<std::string>& f)
{
    if (f.size() < 38)
    { // BIDP_RSQN5 = f[37]
        LOG_WARN("[WS] H0STASP0 필드 부족: " + std::to_string(f.size()) + " (38 필요)");
        return;
    }

    // 첫 수신 시 전체 필드 로그 (진단용)
    static std::set<std::string> first_logged;
    if (first_logged.find(f[0]) == first_logged.end())
    {
        first_logged.insert(f[0]);
        std::string dbg = "[WS] H0STASP0 첫 수신 [" + f[0] + "] 총 " + std::to_string(f.size()) + "필드:";
        for (size_t i = 0; i < f.size(); ++i)
            dbg += " [" + std::to_string(i) + "]=" + f[i];
        LOG_INFO(dbg);
    }

    OrderBook ob;
    ob.ticker = f[0];
    ob.time = f[1];
    ob.timestamp = std::chrono::system_clock::now();

    
    for (int i = 0; i < 5; ++i)
    {
        try
        {
            // 필드 기준을 ENUM으로 처리하는게 가독성 좋아보임 추후 변경때도 편할듯
            ob.asks[i].price = std::stod(f[3 + i]);      // ASKP1-5  [3-7]
            ob.asks[i].quantity = std::stoll(f[23 + i]); // ASKP_RSQN1-5  [23-27]
            ob.bids[i].price = std::stod(f[13 + i]);     // BIDP1-5  [13-17]
            ob.bids[i].quantity = std::stoll(f[33 + i]); // BIDP_RSQN1-5  [33-37]
        }
        catch (...)
        {
        }
    }

    if (on_orderbook_)
        on_orderbook_(ob);
}

// ─── 국내 체결 파싱 (H0STCNT0) ───────────────────────────────────────────
// [0]종목코드 [1]체결시간 [2]현재가 [12]체결량 [21]체결구분(1=매수,5=매도)
void KisWebSocket::parse_kr_trade(const std::vector<std::string>& f)
{
    if (f.size() < 22)
        return;

    static std::set<std::string> first_logged;
    if (first_logged.find(f[0]) == first_logged.end())
    {
        first_logged.insert(f[0]);
        std::string dbg = "[WS] H0STCNT0 첫 수신 [" + f[0] + "] 총 " + std::to_string(f.size()) + "필드:";
        for (size_t i = 0; i < std::min(f.size(), size_t(13)); ++i)
            dbg += "\n  [" + std::to_string(i) + "]=" + f[i];
        LOG_INFO(dbg);
    }

    TradeData td;
    td.ticker = f[0];
    td.time = f[1];
    td.market = Market::KR;
    td.timestamp = std::chrono::system_clock::now();
    try
    {
        td.price = std::stod(f[2]);
        td.quantity = std::stoll(f[12]);
        td.direction = std::stoi(f[21]);
    }
    catch (...)
    {
    }
    if (on_trade_)
        on_trade_(td);
}

// ─── 미국 체결 파싱 (HDFSCNT0) ───────────────────────────────────────────
// tr_key 형식: "NAS|AAPL" → ticker = "AAPL"
// [0]종목코드 [1]체결시간(KST) [2]현재가 [8]체결량 [?]방향
void KisWebSocket::parse_us_trade(const std::vector<std::string>& f)
{
    if (f.size() < 9)
        return;

    static std::set<std::string> first_us_logged;
    if (first_us_logged.find(f[0]) == first_us_logged.end())
    {
        first_us_logged.insert(f[0]);
        std::string dbg = "[WS] HDFSCNT0 첫 수신 [" + f[0] + "] 총 " + std::to_string(f.size()) + "필드:";
        for (size_t i = 0; i < std::min(f.size(), size_t(15)); ++i)
            dbg += "\n  [" + std::to_string(i) + "]=" + f[i];
        LOG_INFO(dbg);
    }

    TradeData td;
    td.ticker = f[0];
    td.time = f[1];
    td.market = Market::US;
    td.timestamp = std::chrono::system_clock::now();
    try
    {
        td.price = std::stod(f[2]);
        td.quantity = std::stoll(f[8]);
        // 방향 필드 위치 확인 후 조정 (진단 로그 참조)
        td.direction = (f.size() > 20) ? std::stoi(f[20]) : 0;
    }
    catch (...)
    {
    }
    if (on_trade_)
        on_trade_(td);
}

// ─── 체결통보 파싱 (H0STCNI0 실거래 / H0STCNI9 모의) ────────────────────
// 주요 필드:
//  [2]ODER_NO(주문번호)  [4]SELN_BYOV_CLS(01=매도,02=매수)
//  [8]STCK_SHRN_ISCD(종목코드)  [9]CNTG_QTY(체결수량)
//  [10]CNTG_UNPR(체결단가)  [11]STCK_CNTG_HOUR(HHMMSS)
//  [13]CNTG_YN(1=주문/정정/취소/거부 접수통보, 2=체결통보) — 모의 실측 확인
void KisWebSocket::parse_fill_notification(const std::vector<std::string>& f)
{
    if (f.size() < 14)
    {
        // 1~2필드: KIS 서버 제어 메시지(ack/heartbeat) — 정상 동작, DEBUG 수준
        if (f.size() > 2)
            LOG_WARN("[WS] H0STCNI 필드 부족: " + std::to_string(f.size()));
        return;
    }
    if (!on_fill_)     // 콜백 미등록 시 즉시 반환 (파싱 비용 절감)
        return;
    if (f[13] != "2")  // 접수/정정/취소/거부(1) 제외, 체결(2)만 처리
        return;

    FillNotification fn;
    fn.odno       = f[2];
    fn.ticker     = f[8];
    fn.side       = (f[4] == "02") ? OrderSide::BUY : OrderSide::SELL;
    fn.fill_time  = f[11];
    fn.timestamp  = std::chrono::system_clock::now();
    try
    {
        fn.filled_qty   = std::stoi(f[9]);
        fn.filled_price = std::stod(f[10]);
    }
    catch (...)
    {
        LOG_WARN("[WS] H0STCNI0 수량/단가 파싱 오류 ODNO=" + fn.odno);
        return;
    }

    LOG_INFO("[WS] 체결통보 ODNO=" + fn.odno + " " + fn.ticker +
             (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(fn.filled_qty) + "주 @" +
             std::to_string(static_cast<int>(fn.filled_price)));
    on_fill_(fn);
}
