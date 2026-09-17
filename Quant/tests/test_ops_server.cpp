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

    void send(OpsMsg ops_msg, const std::string& body)
    {
        auto encoded = ops::encode(ops_msg, body);
        ::send(fd, reinterpret_cast<const char*>(encoded.data()), static_cast<int>(encoded.size()), 0);
    }

    // 1 수신 / 0 상대 종료 / -1 시간 초과
    int recv(Frame& frame, int timeout_ms = 2000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            if (rd.next(frame))
            {
                return 1;
            }

            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

            if (left.count() <= 0)
            {
                return -1;
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);
            timeval tv{};
            tv.tv_sec  = static_cast<long>(left.count() / 1000);
            tv.tv_usec = static_cast<long>((left.count() % 1000) * 1000);

            if (::select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr, &tv) <= 0)
            {
                return -1;
            }

            uint8_t buffer[4096];
            const int count = ::recv(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

            if (count <= 0)
            {
                return 0;
            }

            rd.feed(buffer, static_cast<size_t>(count));
        }
    }

    // want 타입이 올 때까지 다른 타입은 건너뛴다
    bool expect(OpsMsg want, Frame& frame)
    {
        for (int index = 0; index < 10; ++index)
        {
            if (recv(frame) != 1)
            {
                return false;
            }

            if (frame.type == static_cast<uint8_t>(want))
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
    std::mutex               mutex;
    std::vector<OpsOrderReq> orders;
    int                      kills = 0;
    std::string              positions = "{\"positions\":[{\"ticker\":\"005930\",\"qty\":3}]}";

    void wire(OpsServer& ops_server)
    {
        ops_server.set_status_provider([] { return std::string("{\"running\":true}"); });
        ops_server.set_positions_provider(
            [this]
            {
                std::lock_guard<std::mutex> lock(mutex);
                return positions;
            });
        ops_server.set_order_handler(
            [this](const OpsOrderReq& ops_order_req)
            {
                std::lock_guard<std::mutex> lock(mutex);
                orders.push_back(ops_order_req);
                return ops_order_req.quantity > 0 ? std::string() : std::string("qty<=0");
            });
        ops_server.set_kill_handler(
            [this]
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++kills;
            });
    }
};

bool has(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

// 첫 프레임이 HELLO가 아니면 ERROR 뒤 끊김
static void t_hello_first(OpsServer&)
{
    Cli cli;
    assert(cli.open());
    cli.send(OpsMsg::STATUS_REQ, "{}");
    Frame frame;
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(has(frame.body, "HELLO"));
    assert(cli.recv(frame) == 0);
}

static void t_bad_token(OpsServer&)
{
    Cli cli;
    assert(cli.open());
    cli.send(OpsMsg::HELLO, "{\"token\":\"wrong\",\"client\":\"t\"}");
    Frame frame;
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(cli.recv(frame) == 0);
}

static void t_happy_path(OpsServer& ops_server, Fake& fk)
{
    Cli cli;
    assert(cli.open());
    cli.send(OpsMsg::HELLO, "{\"token\":\"secret\",\"client\":\"t\"}");
    Frame frame;
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::WELCOME));
    assert(has(frame.body, "\"auth\":true"));
    // WELCOME 직후 스냅샷이 먼저 온다
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::POSITIONS));
    assert(has(frame.body, "005930"));

    cli.send(OpsMsg::STATUS_REQ, "{}");
    assert(cli.expect(OpsMsg::STATUS, frame) && has(frame.body, "running"));

    cli.send(OpsMsg::PING, "{}");
    assert(cli.expect(OpsMsg::PONG, frame) && has(frame.body, "ts"));

    cli.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c1\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":2,\"price\":0}");
    assert(cli.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":true") && has(frame.body, "c1"));

    cli.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c2\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":0}");
    assert(cli.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":false") && has(frame.body, "qty<=0"));

    {
        std::lock_guard<std::mutex> lock(fk.mutex);
        assert(fk.orders.size() == 2);
        assert(fk.orders[0].cid == "c1" && fk.orders[0].ticker == "005930" && fk.orders[0].side == "SELL" &&
               fk.orders[0].quantity == 2);
    }

    // 다른 스레드의 broadcast가 인증된 연결에 push된다
    ops_server.broadcast(OpsMsg::ORDER_RESULT, "{\"cid\":\"c1\",\"ok\":true}");
    assert(cli.expect(OpsMsg::ORDER_RESULT, frame) && has(frame.body, "c1"));

    // 포지션 문자열이 바뀌면 1초 주기로 push
    {
        std::lock_guard<std::mutex> lock(fk.mutex);
        fk.positions = "{\"positions\":[]}";
    }

    assert(cli.expect(OpsMsg::POSITIONS, frame) && frame.body == "{\"positions\":[]}");

    // 알 수 없는 타입은 ERROR로 답하고 연결은 유지
    cli.send(static_cast<OpsMsg>(0x55), "{}");
    assert(cli.expect(OpsMsg::ERROR_MSG, frame));
    cli.send(OpsMsg::PING, "{}");
    assert(cli.expect(OpsMsg::PONG, frame));

    cli.send(OpsMsg::KILL, "{}");
    assert(cli.expect(OpsMsg::KILL_ACK, frame) && has(frame.body, "\"ok\":true"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(fk.mutex);
        assert(fk.kills == 1);
    }
}

// 본문이 JSON이 아니면 ERROR 뒤 끊김
static void t_bad_json(OpsServer&)
{
    Cli cli;
    assert(cli.open());
    cli.send(OpsMsg::HELLO, "not json");
    Frame frame;
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_MSG));
    assert(cli.recv(frame) == 0);
}

// 토큰 없는 서버: 누구나 붙되 주문·KILL은 거부
static void t_readonly_without_token()
{
    OpsServer ops_server;
    ops_server.set_bind("127.0.0.1", kPort);
    Fake fk;
    fk.wire(ops_server);
    assert(ops_server.start());

    Cli cli;
    assert(cli.open());
    cli.send(OpsMsg::HELLO, "{\"client\":\"t\"}");
    Frame frame;
    assert(cli.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::WELCOME));
    assert(has(frame.body, "\"auth\":false"));

    cli.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c9\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":1}");
    assert(cli.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":false") && has(frame.body, "ops_token"));

    cli.send(OpsMsg::KILL, "{}");
    assert(cli.expect(OpsMsg::KILL_ACK, frame) && has(frame.body, "\"ok\":false"));
    {
        std::lock_guard<std::mutex> lock(fk.mutex);
        assert(fk.orders.empty() && fk.kills == 0);
    }

    ops_server.stop();
}

// 같은 포트에 두 번째 서버는 뜨지 않아야 한다 — 엔진 중복 기동의 유일한 가시 신호다
static void t_second_bind_refused()
{
    OpsServer ops_server_a;
    ops_server_a.set_bind("127.0.0.1", kPort);
    assert(ops_server_a.start());
    OpsServer ops_server_b;
    ops_server_b.set_bind("127.0.0.1", kPort);
    assert(!ops_server_b.start());
    ops_server_a.stop();
}

static void t_remote_bind_needs_token()
{
    OpsServer ops_server;
    ops_server.set_bind("0.0.0.0", kPort);
    assert(!ops_server.start());
    assert(!ops_server.running());
}

int main()
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    Logger::instance().init("logs/test_ops_server.log", LogLevel::WARN);

    {
        OpsServer ops_server;
        ops_server.set_bind("127.0.0.1", kPort);
        ops_server.set_token("secret");
        Fake fk;
        fk.wire(ops_server);
        assert(ops_server.start());
        t_hello_first(ops_server);
        t_bad_token(ops_server);
        t_bad_json(ops_server);
        t_happy_path(ops_server, fk);
        ops_server.stop();
        assert(!ops_server.running());
    }

    t_readonly_without_token();
    t_remote_bind_needs_token();
    t_second_bind_refused();
    Logger::instance().flush();
    std::cout << "test_ops_server: 7/7 PASS\n";
    return 0;
}
