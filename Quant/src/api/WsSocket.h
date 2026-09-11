// api/WsSocket.h — KisWebSocket이 쓰는 실시간 소켓의 플랫폼 경계.
//  구현은 WsSocketWin.cpp(WinHTTP)·WsSocketPosix.cpp(POSIX 소켓 + RFC 6455) 중 플랫폼당 하나만 링크된다
//  (Quant/CMakeLists.txt KIS_WS_SOURCES). 재연결·백오프·구독 복원은 KisWebSocket::recv_loop 한 벌이 맡고,
//  여기는 열고·보내고·받고·닫는 것만 한다. 공개 헤더가 아니라 Quant/src/api에 둔다. [why D-049]
#pragma once
#include <memory>
#include <string>

class WsSocket
{
public:
    virtual ~WsSocket() = default;

    // host:port에 TCP 연결 후 WebSocket 업그레이드까지. 실패는 로그에 남기고 false — 부분 자원은 스스로 닫는다.
    // 한 객체에 한 번만 부른다. 재연결은 새 객체를 연다.
    virtual bool open(const std::string& host, int port) = 0;
    // 텍스트 프레임 하나. 직렬화는 호출자(KisWebSocket::send_mtx_) 책임이다. 닫힌 뒤엔 조용히 버린다.
    virtual void send_text(const std::string& msg) = 0;
    // 메시지 하나가 완성될 때까지 블로킹. 분할 프레임은 여기서 합친다 — parse_message가 잘린 문자열을 받지 않는다.
    // false = 오류·서버 종료·close()로 깨어남. 이유는 last_error()에 남는다.
    virtual bool recv_message(std::string& out) = 0;
    // close 프레임을 먼저 보내고 닫는다. KIS가 이 approval_key 세션을 즉시 놓아야 다음 접속이
    // "ALREADY IN USE appkey"(rt=9)로 거부되지 않는다. 다른 스레드가 recv_message에 블로킹 중이면 깨운다.
    // 두 번 불러도 된다.
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual std::string last_error() const = 0;
};

namespace ws_platform
{
std::unique_ptr<WsSocket> make_socket();
// approval key 발급용 최소 HTTP POST(JSON body). 응답 body, 실패면 빈 문자열.
std::string http_post_json(const std::string& url, const std::string& body);
// 체결통보(H0STCNI) AES-256-CBC 복호화, PKCS7 패딩 제거. Windows는 BCrypt(CNG), Linux는 OpenSSL EVP. 실패면 빈 문자열.
std::string aes_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& iv);
} // namespace ws_platform
