// 운영단말 서버(ipc/OpsServer) 소켓 왕복 테스트. 실제 TCP로 붙어 HELLO 순서·토큰·조회·주문 인테이크·
// push·토큰 없는 서버의 읽기 전용, 루프백 밖 무토큰 bind 거부, 같은 포트 이중 bind 거부를 고정한다. KIS 없이 돈다. 관련 결정: D-043.
#include "ipc/OpsServer.h"
#include "utils/Logger.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define SOCK_BAD (-1)
#define sock_close ::close
#endif

using ops::Frame;
using ops::OpsMsg;

namespace
{

constexpr int kPort = 17100; // 운영 기본(7100)과 겹치지 않게

struct Cli
{
    sock_t           fd = SOCK_BAD;
    ops::FrameReader rd;

    bool open()
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(kPort);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
    }

    void send(OpsMsg t, const std::string& body)
    {
        auto b = ops::encode(t, body);
        ::send(fd, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0);
    }

    // 1 수신 / 0 상대 종료 / -1 시간 초과
    int recv(Frame& f, int timeout_ms = 2000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            if (rd.next(f))
            {
                return 1;
            }

            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

            if (left.count() <= 0)
            {
                return -1;
            }

            fd_set r;
            FD_ZERO(&r);
            FD_SET(fd, &r);
            timeval tv{};
            tv.tv_sec  = static_cast<long>(left.count() / 1000);
            tv.tv_usec = static_cast<long>((left.count() % 1000) * 1000);

            if (::select(static_cast<int>(fd) + 1, &r, nullptr, nullptr, &tv) <= 0)
            {
                return -1;
            }

            uint8_t buf[4096];
            const int n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);

            if (n <= 0)
            {
                return 0;
            }

            rd.feed(buf, static_cast<size_t>(n));
        }
    }

    // want 타입이 올 때까지 다른 타입은 건너뛴다
    bool expect(OpsMsg want, Frame& f)
    {
        for (int i = 0; i < 10; ++i)
        {
            if (recv(f) != 1)
            {
                return false;
            }

            if (f.type == static_cast<uint8_t>(want))
            {
                return true;
            }
        }

        return false;
    }

    ~Cli()
    {
        if (fd != SOCK_BAD)
        {
            sock_close(fd);
        }
    }
};

struct Fake
{
    std::mutex               mtx;
    std::vector<OpsOrderReq> orders;
    int                      kills = 0;
    std::string              positions = "{\"positions\":[{\"ticker\":\"005930\",\"qty\":3}]}";

    void wire(OpsServer& s)
    {
        s.set_status_provider([] { return std::string("{\"running\":true}"); });
        s.set_positions_provider(
            [this]
            {
                std::lock_guard<std::mutex> lk(mtx);
                return positions;
            });
        s.set_order_handler(
            [this](const OpsOrderReq& r)
            {
                std::lock_guard<std::mutex> lk(mtx);
                orders.push_back(r);
                return r.qty > 0 ? std::string() : std::string("qty<=0");
            });
        s.set_kill_handler(
            [this]
            {
                std::lock_guard<std::mutex> lk(mtx);
                ++kills;
            });
    }
};

bool has(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

} // namespace

// 첫 프레임이 HELLO가 아니면 ERROR 뒤 끊김
static void t_hello_first(OpsServer&)
{
    Cli c;
    assert(c.open());
    c.send(OpsMsg::STATUS_REQ, "{}");
    Frame f;
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(has(f.body, "HELLO"));
    assert(c.recv(f) == 0);
}

static void t_bad_token(OpsServer&)
{
    Cli c;
    assert(c.open());
    c.send(OpsMsg::HELLO, "{\"token\":\"wrong\",\"client\":\"t\"}");
    Frame f;
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(c.recv(f) == 0);
}

static void t_happy_path(OpsServer& s, Fake& fk)
{
    Cli c;
    assert(c.open());
    c.send(OpsMsg::HELLO, "{\"token\":\"secret\",\"client\":\"t\"}");
    Frame f;
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::WELCOME));
    assert(has(f.body, "\"auth\":true"));
    // WELCOME 직후 스냅샷이 먼저 온다
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::POSITIONS));
    assert(has(f.body, "005930"));

    c.send(OpsMsg::STATUS_REQ, "{}");
    assert(c.expect(OpsMsg::STATUS, f) && has(f.body, "running"));

    c.send(OpsMsg::PING, "{}");
    assert(c.expect(OpsMsg::PONG, f) && has(f.body, "ts"));

    c.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c1\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":2,\"price\":0}");
    assert(c.expect(OpsMsg::ORDER_ACK, f));
    assert(has(f.body, "\"accepted\":true") && has(f.body, "c1"));

    c.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c2\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":0}");
    assert(c.expect(OpsMsg::ORDER_ACK, f));
    assert(has(f.body, "\"accepted\":false") && has(f.body, "qty<=0"));

    {
        std::lock_guard<std::mutex> lk(fk.mtx);
        assert(fk.orders.size() == 2);
        assert(fk.orders[0].cid == "c1" && fk.orders[0].ticker == "005930" && fk.orders[0].side == "SELL" &&
               fk.orders[0].qty == 2);
    }

    // 다른 스레드의 broadcast가 인증된 연결에 push된다
    s.broadcast(OpsMsg::ORDER_RESULT, "{\"cid\":\"c1\",\"ok\":true}");
    assert(c.expect(OpsMsg::ORDER_RESULT, f) && has(f.body, "c1"));

    // 포지션 문자열이 바뀌면 1초 주기로 push
    {
        std::lock_guard<std::mutex> lk(fk.mtx);
        fk.positions = "{\"positions\":[]}";
    }

    assert(c.expect(OpsMsg::POSITIONS, f) && f.body == "{\"positions\":[]}");

    // 알 수 없는 타입은 ERROR로 답하고 연결은 유지
    c.send(static_cast<OpsMsg>(0x55), "{}");
    assert(c.expect(OpsMsg::ERROR_MSG, f));
    c.send(OpsMsg::PING, "{}");
    assert(c.expect(OpsMsg::PONG, f));

    c.send(OpsMsg::KILL, "{}");
    assert(c.expect(OpsMsg::KILL_ACK, f) && has(f.body, "\"ok\":true"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lk(fk.mtx);
        assert(fk.kills == 1);
    }
}

// 본문이 JSON이 아니면 ERROR 뒤 끊김
static void t_bad_json(OpsServer&)
{
    Cli c;
    assert(c.open());
    c.send(OpsMsg::HELLO, "not json");
    Frame f;
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(c.recv(f) == 0);
}

// 토큰 없는 서버: 누구나 붙되 주문·KILL은 거부
static void t_readonly_without_token()
{
    OpsServer s;
    s.set_bind("127.0.0.1", kPort);
    Fake fk;
    fk.wire(s);
    assert(s.start());

    Cli c;
    assert(c.open());
    c.send(OpsMsg::HELLO, "{\"client\":\"t\"}");
    Frame f;
    assert(c.recv(f) == 1 && f.type == static_cast<uint8_t>(OpsMsg::WELCOME));
    assert(has(f.body, "\"auth\":false"));

    c.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c9\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":1}");
    assert(c.expect(OpsMsg::ORDER_ACK, f));
    assert(has(f.body, "\"accepted\":false") && has(f.body, "ops_token"));

    c.send(OpsMsg::KILL, "{}");
    assert(c.expect(OpsMsg::KILL_ACK, f) && has(f.body, "\"ok\":false"));
    {
        std::lock_guard<std::mutex> lk(fk.mtx);
        assert(fk.orders.empty() && fk.kills == 0);
    }

    s.stop();
}

// 같은 포트에 두 번째 서버는 뜨지 않아야 한다 — 엔진 중복 기동의 유일한 가시 신호다
static void t_second_bind_refused()
{
    OpsServer a;
    a.set_bind("127.0.0.1", kPort);
    assert(a.start());
    OpsServer b;
    b.set_bind("127.0.0.1", kPort);
    assert(!b.start());
    a.stop();
}

static void t_remote_bind_needs_token()
{
    OpsServer s;
    s.set_bind("0.0.0.0", kPort);
    assert(!s.start());
    assert(!s.running());
}

int main()
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    Logger::instance().init("logs/test_ops_server.log", LogLevel::WARN);

    {
        OpsServer s;
        s.set_bind("127.0.0.1", kPort);
        s.set_token("secret");
        Fake fk;
        fk.wire(s);
        assert(s.start());
        t_hello_first(s);
        t_bad_token(s);
        t_bad_json(s);
        t_happy_path(s, fk);
        s.stop();
        assert(!s.running());
    }

    t_readonly_without_token();
    t_remote_bind_needs_token();
    t_second_bind_refused();
    Logger::instance().flush();
    std::cout << "test_ops_server: 7/7 PASS\n";
    return 0;
}
