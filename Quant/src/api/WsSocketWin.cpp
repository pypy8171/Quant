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

std::wstring to_wide(const std::string& text)
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

class WinHttpWsSocket final : public WsSocket
{
public:
    WinHttpWsSocket() : buffer_(128 * 1024)
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

        HINTERNET request_handle = WinHttpOpenRequest(hConnect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

        if (!request_handle)
        {
            LOG_ERROR("[WS] WinHttpOpenRequest 실패");
            release_parents();
            return false;
        }

        WinHttpSetOption(request_handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        if (!WinHttpSendRequest(request_handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(request_handle, nullptr))
        {
            LOG_ERROR("[WS] WebSocket 업그레이드 요청 실패: " + std::to_string(GetLastError()));
            WinHttpCloseHandle(request_handle);
            release_parents();
            return false;
        }

        HINTERNET websocket_handle = WinHttpWebSocketCompleteUpgrade(request_handle, 0);
        WinHttpCloseHandle(request_handle);

        if (!websocket_handle)
        {
            LOG_ERROR("[WS] WinHttpWebSocketCompleteUpgrade 실패: " + std::to_string(GetLastError()));
            release_parents();
            return false;
        }

        hWebSocket_.store(websocket_handle);
        return true;
    }

    void send_text(const std::string& message) override
    {
        HINTERNET handle = hWebSocket_.load();

        if (!handle)
        {
            return;
        }

        WinHttpWebSocketSend(handle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)message.data(), (DWORD)message.size());
    }

    bool recv_message(std::string& out) override
    {
        out.clear();

        while (true)
        {
            // close()가 핸들을 비운 뒤엔 여기서 끝난다. 이미 Receive에 들어간 호출은 닫힌 핸들 오류로 돌아온다.
            HINTERNET handle = hWebSocket_.load();

            if (!handle)
            {
                last_error_ = "closed";
                return false;
            }

            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType{};
            DWORD result_code = WinHttpWebSocketReceive(handle, buffer_.data(), (DWORD)buffer_.size(), &bytesRead, &bufType);

            if (result_code != ERROR_SUCCESS)
            {
                last_error_ = "code=" + std::to_string(result_code);
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
                out.append(reinterpret_cast<const char*>(buffer_.data()), bytesRead);
            }
            else if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            {
                out.append(reinterpret_cast<const char*>(buffer_.data()), bytesRead);
                return true;
            }
        }
    }

    void close() override
    {
        HINTERNET handle = hWebSocket_.exchange(nullptr);

        if (!handle)
        {
            return;
        }

        // close 프레임 없이 핸들만 닫으면 KIS가 세션을 붙잡아 다음 접속이 rt=9로 거부된다.
        WinHttpWebSocketClose(handle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(handle);
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
    std::vector<BYTE> buffer_;
    std::string last_error_; // recv_message를 부른 스레드만 쓴다
};

} // namespace

std::unique_ptr<WsSocket> websocket_platform::make_socket()
{
    return std::make_unique<WinHttpWsSocket>();
}

std::string websocket_platform::http_post_json(const std::string& url, const std::string& body)
{
    std::wstring wide_url = to_wide(url);
    wchar_t host[512]{}, path[4096]{};
    URL_COMPONENTS url_components{};
    url_components.dwStructSize = sizeof(url_components);
    url_components.lpszHostName = host;
    url_components.dwHostNameLength = (DWORD)std::size(host);
    url_components.lpszUrlPath = path;
    url_components.dwUrlPathLength = (DWORD)std::size(path);
    WinHttpCrackUrl(wide_url.c_str(), 0, 0, &url_components);

    HINTERNET session_handle = WinHttpOpen(L"QuantTrader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session_handle)
    {
        return "";
    }

    HINTERNET connection_handle = WinHttpConnect(session_handle, host, url_components.nPort, 0);

    if (!connection_handle)
    {
        WinHttpCloseHandle(session_handle);
        return "";
    }

    DWORD flags = (url_components.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request_handle =
        WinHttpOpenRequest(connection_handle, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!request_handle)
    {
        WinHttpCloseHandle(connection_handle);
        WinHttpCloseHandle(session_handle);
        return "";
    }

    WinHttpAddRequestHeaders(request_handle, L"Content-Type: application/json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(request_handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(request_handle, nullptr);

    std::string response;

    if (ok)
    {
        DWORD avail = 0;

        while (WinHttpQueryDataAvailable(request_handle, &avail) && avail > 0)
        {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            WinHttpReadData(request_handle, &chunk[0], avail, &read);
            response.append(chunk, 0, read);
        }
    }

    WinHttpCloseHandle(request_handle);
    WinHttpCloseHandle(connection_handle);
    WinHttpCloseHandle(session_handle);
    return response;
}

// ─── 체결통보 복호화: BCrypt(CNG) — AES-256-CBC, PKCS7 패딩 제거 ───────────
// Windows: BCrypt(CNG) — AES-256-CBC, PKCS7 패딩 제거
std::string websocket_platform::aes_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& initialization_vector)
{
    if (cipher.empty() || cipher.size() % 16 != 0 || key.size() != 32 || initialization_vector.size() != 16)
    {
        return "";
    }

    BCRYPT_ALG_HANDLE algorithm_handle = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&algorithm_handle, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
    {
        return "";
    }

    BCryptSetProperty(algorithm_handle, BCRYPT_CHAINING_MODE,
                      (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);

    if (BCryptGenerateSymmetricKey(algorithm_handle, &key_handle, nullptr, 0,
                                   (PUCHAR)key.data(), 32, 0) == 0)
    {
        std::vector<UCHAR> ivbuf(initialization_vector.begin(), initialization_vector.begin() + 16); // BCrypt가 IV를 갱신하므로 복사
        std::string out(cipher.size(), '\0');
        ULONG outLen = 0;

        if (BCryptDecrypt(key_handle,
                          (PUCHAR)cipher.data(), (ULONG)cipher.size(),
                          nullptr, ivbuf.data(), (ULONG)ivbuf.size(),
                          (PUCHAR)&out[0], (ULONG)out.size(), &outLen,
                          BCRYPT_BLOCK_PADDING) == 0)
        {
            result.assign(out.data(), outLen);
        }
    }

    if (key_handle)
    {
        BCryptDestroyKey(key_handle);
    }

    if (algorithm_handle)
    {
        BCryptCloseAlgorithmProvider(algorithm_handle, 0);
    }

    return result;
}
