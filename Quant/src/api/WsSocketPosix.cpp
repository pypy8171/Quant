// api/WsSocketPosix.cpp — WsSocket의 POSIX 소켓 + 손수 짠 RFC 6455 프레이밍 구현, libcurl HTTP POST, OpenSSL AES 복호화.
//  Linux에서만 컴파일된다. 수신은 recv_loop 스레드, close()는 disconnect()를 부른 스레드가 동시에 부를 수 있다 —
//  fd는 atomic으로 바꾼다. 모든 send는 MSG_NOSIGNAL — 죽은 소켓에 쓰면 SIGPIPE로 프로세스가 죽는다. [why D-049]
#include "WsSocket.h"
#include "utils/Logger.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <curl/curl.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace
{

size_t curl_write_cb(char* cursor, size_t sz, size_t nm, std::string* out)
{
    out->append(cursor, sz * nm);
    return sz * nm;
}


// ─── POSIX socket 헬퍼 ────────────────────────────────────────────────────

bool sock_recv_all(int fd, void* buffer, size_t length)
{
    auto* cursor = static_cast<char*>(buffer);

    while (length > 0)
    {
        ssize_t received = ::recv(fd, cursor, length, 0);

        if (received <= 0)
        {
            return false;
        }

        cursor += received;
        length -= static_cast<size_t>(received);
    }

    return true;
}

// RFC 6455 WebSocket TCP 연결 + HTTP Upgrade 핸드셰이크
bool ws_tcp_connect(const std::string& host, int port, int& out_fd)
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

    for (auto* address_info = res; address_info; address_info = address_info->ai_next)
    {
        fd = ::socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);

        if (fd < 0)
        {
            continue;
        }

        if (::connect(fd, address_info->ai_addr, address_info->ai_addrlen) == 0)
        {
            break;
        }

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

    if (::send(fd, req.c_str(), req.size(), MSG_NOSIGNAL) < 0)
    {
        LOG_WARN(std::string("[WS] 핸드셰이크 send 실패: ") + std::strerror(errno));
        ::close(fd);
        return false;
    }

    // 응답을 헤더 끝(\r\n\r\n)까지 읽기 (부분 수신 대비)
    std::string response;
    char buffer[1024];

    for (int index = 0; index < 8; ++index)
    {
        ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);

        if (received <= 0)
        {
            break;
        }

        response.append(buffer, static_cast<size_t>(received));

        if (response.find("\r\n\r\n") != std::string::npos)
        {
            break;
        }
    }

    if (response.find("101") == std::string::npos)
    {
        LOG_WARN("[WS] 핸드셰이크 응답에 101 없음 (recv " + std::to_string(response.size()) +
                 "B): " + response.substr(0, 120));
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
void ws_send_text_linux(int fd, const std::string& data)
{
    size_t length = data.size();
    std::vector<uint8_t> frame;
    frame.reserve(6 + length);

    frame.push_back(0x81); // FIN=1, opcode=1(Text)

    if (length <= 125)
    {
        frame.push_back(uint8_t(0x80 | length));
    }
    else if (length <= 65535)
    {
        frame.push_back(0x80 | 126);
        frame.push_back(uint8_t(length >> 8));
        frame.push_back(uint8_t(length));
    }
    else
    {
        frame.push_back(0x80 | 127);

        for (int index = 7; index >= 0; --index)
        {
            frame.push_back(uint8_t(length >> (8 * index)));
        }
    }

    // 마스킹 키. RFC 6455는 프레임마다 새 난수 마스크를 요구하지만, 여기선 고정 키를 쓴다.
    // WS 마스킹은 보안이 아니라 프록시 캐시 오염 방지용 XOR 난독화라 KIS 서버는 값을 검증하지
    // 않아 실동작에 무해하다. 규격 엄밀성을 맞추려면 프레임별 난수로 바꿔야 한다(보류 목록).
    const uint8_t mk[4] = {0x37, 0x1A, 0xC5, 0x4F};
    frame.insert(frame.end(), mk, mk + 4);

    for (size_t length_index = 0; length_index < length; ++length_index)
    {
        frame.push_back(uint8_t(data[length_index]) ^ mk[length_index % 4]);
    }

    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
}

// RFC 6455 프레임 수신 → 메시지 하나를 out에 채운다(false = 연결 종료/오류, 이유는 error).
//  FIN=0 프레임과 뒤따르는 continuation(0x0)을 한 메시지로 모은다 — Windows 경로의 UTF8_FRAGMENT
//  누적과 같은 동작이어야 parse_message가 잘린 문자열을 받지 않는다. 제어 프레임(ping/pong)은
//  분할 메시지 사이에 끼어들 수 있으므로 누적을 끊지 않는다. 재귀 대신 루프 — ping이 연속으로
//  오면 스택이 자란다.
bool ws_recv_frame_linux(int fd, std::string& out, std::string& error)
{
    // 손상된 길이 필드 하나로 거대 할당이 일어나지 않게 상한을 둔다. KIS 실시간 프레임은 KB 단위다.
    constexpr uint64_t kMaxMessageBytes = uint64_t(1) << 20;
    std::string& message = out;
    message.clear();
    bool in_message = false;

    while (true)
    {
        uint8_t hdr[2];

        if (!sock_recv_all(fd, hdr, 2))
        {
            error = "recv 실패/종료";
            return false;
        }

        const bool fin = (hdr[0] & 0x80) != 0;
        const uint8_t opcode = hdr[0] & 0x0F;
        const bool masked = ((hdr[1] >> 7) & 1) != 0;
        uint64_t length = hdr[1] & 0x7F;

        if (length == 126)
        {
            uint8_t ext[2];

            if (!sock_recv_all(fd, ext, 2))
            {
                error = "recv 실패/종료";
                return false;
            }

            length = (uint64_t(ext[0]) << 8) | ext[1];
        }
        else if (length == 127)
        {
            uint8_t ext[8];

            if (!sock_recv_all(fd, ext, 8))
            {
                error = "recv 실패/종료";
                return false;
            }

            length = 0;

            for (int index = 0; index < 8; ++index)
            {
                length = (length << 8) | ext[index];
            }
        }

        if (length > kMaxMessageBytes || message.size() + length > kMaxMessageBytes)
        {
            error = "프레임 길이 상한 초과 (" + std::to_string(length) + "B)";
            return false;
        }

        uint8_t mk[4]{};

        if (masked && !sock_recv_all(fd, mk, 4))
        {
            error = "recv 실패/종료";
            return false;
        }

        std::vector<uint8_t> payload(static_cast<size_t>(length));

        if (length > 0 && !sock_recv_all(fd, payload.data(), static_cast<size_t>(length)))
        {
            error = "recv 실패/종료";
            return false;
        }

        if (masked)
        {
            for (size_t payload_index = 0; payload_index < payload.size(); ++payload_index)
            {
                payload[payload_index] ^= mk[payload_index % 4];
            }
        }

        if (opcode == 0x8)
        {
            error = "서버 close 프레임";
            return false;
        }

        if (opcode == 0x9)
        {
            // Ping(0x9) → Pong(0xA). 제어 프레임 payload는 125B 이하라 1바이트 길이로 충분하다.
            std::vector<uint8_t> pong = {0x8A, uint8_t(0x80 | (length & 0x7F)), 0x00, 0x00, 0x00, 0x00};
            pong.insert(pong.end(), payload.begin(), payload.end());
            ::send(fd, pong.data(), pong.size(), MSG_NOSIGNAL);
            continue;
        }

        if (opcode == 0x1 || opcode == 0x2)
        {
            message.assign(payload.begin(), payload.end());
            in_message = true;
        }
        else if (opcode == 0x0)
        {
            if (!in_message)
            {
                continue; // 시작 프레임 없는 continuation — 버린다
            }

            message.append(payload.begin(), payload.end());
        }
        else
        {
            continue; // pong(0xA)·예약 opcode — 메시지가 아니다
        }

        if (fin)
        {
            return true;
        }
    }
}

class PosixWsSocket final : public WsSocket
{
public:
    ~PosixWsSocket() override
    {
        close();
    }

    bool open(const std::string& host, int port) override
    {
        int fd = -1;

        if (!ws_tcp_connect(host, port, fd))
        {
            return false;
        }

        fd_.store(fd);
        return true;
    }

    void send_text(const std::string& message) override
    {
        int fd = fd_.load();

        if (fd < 0)
        {
            return;
        }

        ws_send_text_linux(fd, message);
    }

    bool recv_message(std::string& out) override
    {
        int fd = fd_.load();

        if (fd < 0)
        {
            last_error_ = "closed";
            return false;
        }

        return ws_recv_frame_linux(fd, out, last_error_);
    }

    void close() override
    {
        int fd = fd_.exchange(-1);

        if (fd < 0)
        {
            return;
        }

        // close 프레임(0x88, MASK=1, 빈 payload) — Windows 경로와 같은 이유로 KIS가 세션을 바로 놓게 한다.
        const uint8_t close_frame[6] = {0x88, 0x80, 0x37, 0x1A, 0xC5, 0x4F};
        ::send(fd, close_frame, sizeof(close_frame), MSG_NOSIGNAL);
        // W-1: close()는 다른 스레드가 recv() 블로킹 중일 때 깨운다는 보장이 없다(POSIX).
        // shutdown(SHUT_RDWR)은 블로킹된 recv를 즉시 깨워 half-open 교착(join 무한대기)을 막는다.
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    bool is_open() const override
    {
        return fd_.load() >= 0;
    }

    std::string last_error() const override
    {
        return last_error_;
    }

private:
    std::atomic<int> fd_{-1}; // close()(다른 스레드)와 recv_message가 같이 본다
    std::string last_error_;  // recv_message를 부른 스레드만 쓴다
};

} // namespace

std::unique_ptr<WsSocket> ws_platform::make_socket()
{
    return std::make_unique<PosixWsSocket>();
}

std::string ws_platform::http_post_json(const std::string& url, const std::string& body)
{
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        return "";
    }

    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK)
    {
        LOG_ERROR(std::string("[WS] HTTP POST 오류: ") + curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ─── 체결통보 복호화: OpenSSL EVP — AES-256-CBC, PKCS7 패딩 제거(DecryptFinal) ──
std::string ws_platform::aes_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& iv)
{
    if (cipher.empty() || cipher.size() % 16 != 0 || key.size() != 32 || iv.size() != 16)
    {
        return "";
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();

    if (!ctx)
    {
        return "";
    }

    std::string out(cipher.size() + 16, '\0');
    int length = 0, total = 0;
    std::string result;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(iv.data())) == 1 &&
        EVP_DecryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(&out[0]), &length,
            reinterpret_cast<const unsigned char*>(cipher.data()),
            static_cast<int>(cipher.size())) == 1)
    {
        total = length;

        if (EVP_DecryptFinal_ex(ctx,
                reinterpret_cast<unsigned char*>(&out[0]) + total, &length) == 1)
        {
            total += length;
            result.assign(out.data(), total);
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    return result;
}
