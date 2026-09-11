// api/WsSocketWin.cpp — WsSocket의 WinHTTP 구현 + Windows 전용 HTTP POST·AES 복호화. Windows에서만 컴파일된다.
//  수신은 recv_loop 스레드, close()는 disconnect()를 부른 스레드가 동시에 부를 수 있다 — 핸들은 atomic으로 바꾼다.
//  [why D-049]
#include "WsSocket.h"
#include "utils/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR // wingdi.h — LogLevel::ERROR와 부딪힌다
#endif

#include <atomic>
#include <iterator>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{

std::wstring to_wide(const std::string& s)
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

class WinHttpWsSocket final : public WsSocket
{
public:
    WinHttpWsSocket() : buf_(128 * 1024)
    {
    }

    ~WinHttpWsSocket() override
    {
        close();
        release_parents();
    }

    bool open(const std::string& host, int port) override
    {
        hSession_ = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);

        if (!hSession_)
        {
            LOG_ERROR("[WS] WinHttpOpen 실패");
            return false;
        }

        hConnect_ = WinHttpConnect(hSession_, to_wide(host).c_str(), static_cast<INTERNET_PORT>(port), 0);

        if (!hConnect_)
        {
            LOG_ERROR("[WS] WinHttpConnect 실패");
            release_parents();
            return false;
        }

        HINTERNET hReq = WinHttpOpenRequest(hConnect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

        if (!hReq)
        {
            LOG_ERROR("[WS] WinHttpOpenRequest 실패");
            release_parents();
            return false;
        }

        WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(hReq, nullptr))
        {
            LOG_ERROR("[WS] WebSocket 업그레이드 요청 실패: " + std::to_string(GetLastError()));
            WinHttpCloseHandle(hReq);
            release_parents();
            return false;
        }

        HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
        WinHttpCloseHandle(hReq);

        if (!hWs)
        {
            LOG_ERROR("[WS] WinHttpWebSocketCompleteUpgrade 실패: " + std::to_string(GetLastError()));
            release_parents();
            return false;
        }

        hWebSocket_.store(hWs);
        return true;
    }

    void send_text(const std::string& msg) override
    {
        HINTERNET h = hWebSocket_.load();

        if (!h)
        {
            return;
        }

        WinHttpWebSocketSend(h, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)msg.data(), (DWORD)msg.size());
    }

    bool recv_message(std::string& out) override
    {
        out.clear();

        while (true)
        {
            // close()가 핸들을 비운 뒤엔 여기서 끝난다. 이미 Receive에 들어간 호출은 닫힌 핸들 오류로 돌아온다.
            HINTERNET h = hWebSocket_.load();

            if (!h)
            {
                last_error_ = "closed";
                return false;
            }

            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType{};
            DWORD rc = WinHttpWebSocketReceive(h, buf_.data(), (DWORD)buf_.size(), &bytesRead, &bufType);

            if (rc != ERROR_SUCCESS)
            {
                last_error_ = "code=" + std::to_string(rc);
                return false;
            }

            if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                last_error_ = "서버 close 프레임";
                return false;
            }

            // FRAGMENT는 모으고 MESSAGE에서 완성한다. 바이너리 타입은 KIS가 쓰지 않는다 — 버린다.
            if (bufType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                out.append(reinterpret_cast<const char*>(buf_.data()), bytesRead);
            }
            else if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            {
                out.append(reinterpret_cast<const char*>(buf_.data()), bytesRead);
                return true;
            }
        }
    }

    void close() override
    {
        HINTERNET h = hWebSocket_.exchange(nullptr);

        if (!h)
        {
            return;
        }

        // close 프레임 없이 핸들만 닫으면 KIS가 세션을 붙잡아 다음 접속이 rt=9로 거부된다.
        WinHttpWebSocketClose(h, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(h);
    }

    bool is_open() const override
    {
        return hWebSocket_.load() != nullptr;
    }

    std::string last_error() const override
    {
        return last_error_;
    }

private:
    // 세션·커넥션 핸들은 WS 핸들의 부모라 WS를 닫은 뒤에 닫는다 — 소멸자와 open 실패 경로에서만.
    void release_parents()
    {
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
    }

    HINTERNET hSession_ = nullptr;
    HINTERNET hConnect_ = nullptr;
    std::atomic<HINTERNET> hWebSocket_{nullptr}; // close()(다른 스레드)와 recv_message가 같이 본다
    std::vector<BYTE> buf_;
    std::string last_error_; // recv_message를 부른 스레드만 쓴다
};

} // namespace

std::unique_ptr<WsSocket> ws_platform::make_socket()
{
    return std::make_unique<WinHttpWsSocket>();
}

std::string ws_platform::http_post_json(const std::string& url, const std::string& body)
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
    {
        return "";
    }

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

// ─── 체결통보 복호화: BCrypt(CNG) — AES-256-CBC, PKCS7 패딩 제거 ───────────
// Windows: BCrypt(CNG) — AES-256-CBC, PKCS7 패딩 제거
std::string ws_platform::aes_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& iv)
{
    if (cipher.empty() || cipher.size() % 16 != 0 || key.size() != 32 || iv.size() != 16)
    {
        return "";
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
    {
        return "";
    }

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

    if (hKey)
    {
        BCryptDestroyKey(hKey);
    }

    if (hAlg)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    return result;
}
