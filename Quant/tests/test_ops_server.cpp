// 운영단말 서버(ipc/OpsServer) 소켓 왕복 테스트. 실제 TCP로 붙어 HELLO 순서·토큰·조회·주문 인테이크·
// push·토큰 없는 서버의 읽기 전용, 루프백 밖 무토큰 bind 거부, 같은 포트 이중 bind 거부를 고정한다. KIS 없이 돈다. 관련 결정: D-043.
#include "ipc/OpsServer.h"
#include "core/Types.h"
#include "utils/Logger.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
using socket_handle_t = SOCKET;
#define SOCK_BAD INVALID_SOCKET
#define socket_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle_t = int;
#define SOCK_BAD (-1)
#define socket_close ::close
#endif

using ops::Frame;
using ops::OpsMsg;

namespace
{

constexpr int kPort = 17100; // 운영 기본(7100)과 겹치지 않게

struct Client
{
    socket_handle_t           descriptor = SOCK_BAD;
    ops::FrameReader reader;

    bool open()
    {
        descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in socket_address{};
        socket_address.sin_family      = AF_INET;
        socket_address.sin_port        = htons(kPort);
        socket_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(descriptor, reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)) == 0;
    }

    void send(OpsMsg ops_message, const std::string& body)
    {
        auto encoded = ops::encode(ops_message, body);
        ::send(descriptor, reinterpret_cast<const char*>(encoded.data()), static_cast<int>(encoded.size()), 0);
    }

    // 1 수신 / 0 상대 종료 / -1 시간 초과
    int recv(Frame& frame, int timeout_ms = 2000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            if (reader.next(frame))
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
            FD_SET(descriptor, &read_set);
            timeval time_value{};
            time_value.tv_sec  = static_cast<long>(left.count() / 1000);
            time_value.tv_usec = static_cast<long>((left.count() % 1000) * 1000);

            if (::select(static_cast<int>(descriptor) + 1, &read_set, nullptr, nullptr, &time_value) <= 0)
            {
                return -1;
            }

            uint8_t buffer[4096];
            const int count = ::recv(descriptor, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

            if (count <= 0)
            {
                return 0;
            }

            reader.feed(buffer, static_cast<size_t>(count));
        }
    }

    // wanted_type 타입이 올 때까지 다른 타입은 건너뛴다
    bool expect(OpsMsg wanted_type, Frame& frame)
    {
        for (int index = 0; index < 10; ++index)
        {
            if (recv(frame) != 1)
            {
                return false;
            }

            if (frame.type == static_cast<uint8_t>(wanted_type))
            {
                return true;
            }
        }

        return false;
    }

    ~Client()
    {
        if (descriptor != SOCK_BAD)
        {
            socket_close(descriptor);
        }
    }
};

struct Fake
{
    std::mutex               mutex;
    std::vector<OpsOrderReq> orders;
    int                      kills = 0;
    int                      shutdowns = 0;
    std::string              shutdown_who;
    std::string              positions = "{\"positions\":[{\"ticker\":\"005930\",\"qty\":3}]}";

    void wire(OpsServer& ops_server)
    {
        ops_server.set_status_provider([]
        {
            return std::string("{\"running\":true}");
        });
        ops_server.set_positions_provider(
            [this]
            {
                std::lock_guard<std::mutex> lock(mutex);
                return positions;
            });
        ops_server.set_order_handler(
            [this](const OpsOrderReq& ops_order_request)
            {
                std::lock_guard<std::mutex> lock(mutex);
                orders.push_back(ops_order_request);
                return ops_order_request.quantity > 0 ? std::string() : std::string("qty<=0");
            });
        ops_server.set_kill_handler(
            [this]
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++kills;
            });
        ops_server.set_shutdown_handler(
            [this](const std::string& who)
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++shutdowns;
                shutdown_who = who;
            });
    }
};

bool has(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

// assert는 Release(NDEBUG)에서 빠진다. 새 케이스는 빌드 형태와 무관하게 멈추도록 이것을 쓴다.
void require(bool condition, const char* name)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << name << "\n";
        std::abort();
    }
}

// side 해석은 컴파일 때도 맞아야 한다 — 빈 값·오타가 매수로 읽히던 from_string과 다른 점이다.
static_assert(OrderSide::parse("BUY") == std::optional<OrderSide>(OrderSide::BUY));
static_assert(OrderSide::parse("sell") == std::optional<OrderSide>(OrderSide::SELL));
static_assert(OrderSide::parse("Sell") == std::optional<OrderSide>(OrderSide::SELL));
static_assert(!OrderSide::parse(""));
static_assert(!OrderSide::parse("HOLD"));
static_assert(!OrderSide::parse("SELLX"));

} // namespace

// 첫 프레임이 HELLO가 아니면 ERROR 뒤 끊김
static void t_hello_first(OpsServer&)
{
    Client client;
    assert(client.open());
    client.send(OpsMsg::STATUS_REQ, "{}");
    Frame frame;
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_NTF));
    assert(has(frame.body, "HELLO"));
    assert(client.recv(frame) == 0);
}

static void t_bad_token(OpsServer&)
{
    Client client;
    assert(client.open());
    client.send(OpsMsg::HELLO_REQ, "{\"token\":\"wrong\",\"client\":\"t\"}");
    Frame frame;
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_NTF));
    assert(client.recv(frame) == 0);
}

static void t_happy_path(OpsServer& ops_server, Fake& forward_key)
{
    Client client;
    assert(client.open());
    client.send(OpsMsg::HELLO_REQ, "{\"token\":\"secret\",\"client\":\"t\"}");
    Frame frame;
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::HELLO_ACK));
    assert(has(frame.body, "\"auth\":true"));
    // HELLO_ACK 직후 스냅샷이 먼저 온다
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::POSITIONS_NTF));
    assert(has(frame.body, "005930"));

    client.send(OpsMsg::STATUS_REQ, "{}");
    assert(client.expect(OpsMsg::STATUS_ACK, frame) && has(frame.body, "running"));

    client.send(OpsMsg::PING_REQ, "{}");
    assert(client.expect(OpsMsg::PING_ACK, frame) && has(frame.body, "ts"));

    client.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c1\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":2,\"price\":0}");
    assert(client.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":true") && has(frame.body, "c1"));

    client.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c2\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":0}");
    assert(client.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":false") && has(frame.body, "qty<=0"));

    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        assert(forward_key.orders.size() == 2);
        assert(forward_key.orders[0].client_id == "c1" && forward_key.orders[0].ticker == "005930" && forward_key.orders[0].side == "SELL" &&
               forward_key.orders[0].quantity == 2);
    }

    // 다른 스레드의 broadcast가 인증된 연결에 push된다
    ops_server.broadcast(OpsMsg::ORDER_RESULT_NTF, "{\"cid\":\"c1\",\"ok\":true}");
    assert(client.expect(OpsMsg::ORDER_RESULT_NTF, frame) && has(frame.body, "c1"));

    // 포지션 문자열이 바뀌면 1초 주기로 push
    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        forward_key.positions = "{\"positions\":[]}";
    }

    assert(client.expect(OpsMsg::POSITIONS_NTF, frame) && frame.body == "{\"positions\":[]}");

    // 알 수 없는 타입은 ERROR로 답하고 연결은 유지
    client.send(static_cast<OpsMsg>(0x55), "{}");
    assert(client.expect(OpsMsg::ERROR_NTF, frame));
    client.send(OpsMsg::PING_REQ, "{}");
    assert(client.expect(OpsMsg::PING_ACK, frame));

    // 곱게 내리기는 부른 이름을 그대로 달고 돌아온다 — 종료 사유 한 줄에 그 이름이 실린다. [why D-114]
    client.send(OpsMsg::SHUTDOWN_REQ, "{\"who\":\"deploy_trader\"}");
    assert(client.expect(OpsMsg::SHUTDOWN_ACK, frame) && has(frame.body, "\"ok\":true"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        assert(forward_key.shutdowns == 1 && forward_key.shutdown_who == "deploy_trader");
    }

    client.send(OpsMsg::KILL_REQ, "{}");
    assert(client.expect(OpsMsg::KILL_ACK, frame) && has(frame.body, "\"ok\":true"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        assert(forward_key.kills == 1);
    }
}

// side가 빠졌거나 BUY/SELL이 아니면 핸들러를 부르지 않고 거부한다 — 예전에는 그대로 넘겨, 뒤쪽이 매수로 읽을 수 있었다.
//  대소문자는 가리지 않고, 핸들러에는 대문자로 넘긴다. client_count()는 서버 스레드가 연결을 쥔 동안에도 바로 답한다.
static void t_order_side(OpsServer& ops_server, Fake& forward_key)
{
    Client client;
    require(client.open(), "연결");
    client.send(OpsMsg::HELLO_REQ, "{\"token\":\"secret\",\"client\":\"t\"}");
    Frame frame;
    require(client.expect(OpsMsg::HELLO_ACK, frame), "HELLO_ACK");
    require(ops_server.client_count() >= 1, "연결 수가 보인다");

    size_t orders_before = 0;

    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        orders_before = forward_key.orders.size();
    }

    const char* const bad_orders[] = {
        "{\"cid\":\"s1\",\"ticker\":\"005930\",\"qty\":1}",
        "{\"cid\":\"s2\",\"ticker\":\"005930\",\"side\":\"HOLD\",\"qty\":1}",
        "{\"cid\":\"s3\",\"ticker\":\"005930\",\"side\":\"\",\"qty\":1}",
    };

    for (const char* body : bad_orders)
    {
        client.send(OpsMsg::ORDER_REQ, body);
        require(client.expect(OpsMsg::ORDER_ACK, frame), "ORDER_ACK(잘못된 side)");
        require(has(frame.body, "\"accepted\":false") && has(frame.body, "side"), "잘못된 side는 거부");
    }

    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        require(forward_key.orders.size() == orders_before, "잘못된 side는 핸들러까지 안 간다");
    }

    client.send(OpsMsg::ORDER_REQ, "{\"cid\":\"s4\",\"ticker\":\"005930\",\"side\":\"sell\",\"qty\":1}");
    require(client.expect(OpsMsg::ORDER_ACK, frame) && has(frame.body, "\"accepted\":true"), "소문자 sell은 받는다");

    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        require(forward_key.orders.size() == orders_before + 1, "소문자 sell은 핸들러로 간다");
        require(forward_key.orders.back().side == "SELL", "핸들러에는 대문자 SELL로 넘긴다");
    }
}

// 본문이 JSON이 아니면 ERROR 뒤 끊김
static void t_bad_json(OpsServer&)
{
    Client client;
    assert(client.open());
    client.send(OpsMsg::HELLO_REQ, "not json");
    Frame frame;
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_NTF));
    assert(client.recv(frame) == 0);
}

// 형식이 틀린 본문 — 빈 본문·배열·필드 타입 틀림 — 은 그 연결만 끊고 서버는 산다. [why 전수조사 B2b-1]
static void t_malformed_body(OpsServer&)
{
    const char* const bad_hellos[] = {"[1,2]", "7", "{\"token\":7}"};

    for (const char* body : bad_hellos)
    {
        Client client;
        assert(client.open());
        client.send(OpsMsg::HELLO_REQ, body);
        Frame frame;
        assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_NTF));
        assert(client.recv(frame) == 0);
    }

    // 빈 본문 HELLO 는 토큰 없음으로 읽혀 거절된다(예외 아님)
    {
        Client client;
        assert(client.open());
        client.send(OpsMsg::HELLO_REQ, "");
        Frame frame;
        assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::ERROR_NTF));
        assert(has(frame.body, "token"));
        assert(client.recv(frame) == 0);
    }

    // 인증 뒤 주문 수량을 문자열로 보내도 끊기기만 한다
    {
        Client client;
        assert(client.open());
        client.send(OpsMsg::HELLO_REQ, "{\"token\":\"secret\",\"client\":\"t\"}");
        Frame frame;
        assert(client.expect(OpsMsg::HELLO_ACK, frame));
        client.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c7\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":\"10\"}");
        assert(client.expect(OpsMsg::ERROR_NTF, frame) && has(frame.body, "형식"));
    }

    // 서버는 여전히 받는다
    Client client;
    assert(client.open());
    client.send(OpsMsg::HELLO_REQ, "{\"token\":\"secret\",\"client\":\"t\"}");
    Frame frame;
    assert(client.expect(OpsMsg::HELLO_ACK, frame));
}

// 토큰 없는 서버: 누구나 붙되 주문·KILL은 거부
static void t_readonly_without_token()
{
    OpsServer ops_server;
    ops_server.set_bind("127.0.0.1", kPort);
    Fake forward_key;
    forward_key.wire(ops_server);
    assert(ops_server.start());

    Client client;
    assert(client.open());
    client.send(OpsMsg::HELLO_REQ, "{\"client\":\"t\"}");
    Frame frame;
    assert(client.recv(frame) == 1 && frame.type == static_cast<uint8_t>(OpsMsg::HELLO_ACK));
    assert(has(frame.body, "\"auth\":false"));

    client.send(OpsMsg::ORDER_REQ, "{\"cid\":\"c9\",\"ticker\":\"005930\",\"side\":\"SELL\",\"qty\":1}");
    assert(client.expect(OpsMsg::ORDER_ACK, frame));
    assert(has(frame.body, "\"accepted\":false") && has(frame.body, "ops_token"));

    client.send(OpsMsg::KILL_REQ, "{}");
    assert(client.expect(OpsMsg::KILL_ACK, frame) && has(frame.body, "\"ok\":false"));

    // 곱게 내리기도 토큰이 있어야 한다 — 누구나 부르면 아무 때나 매매가 멈춘다.
    client.send(OpsMsg::SHUTDOWN_REQ, "{\"who\":\"t\"}");
    assert(client.expect(OpsMsg::SHUTDOWN_ACK, frame) && has(frame.body, "\"ok\":false"));
    {
        std::lock_guard<std::mutex> lock(forward_key.mutex);
        assert(forward_key.orders.empty() && forward_key.kills == 0 && forward_key.shutdowns == 0);
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
    Logger::instance().initialize("logs/test_ops_server.log", LogLevel::WARN);

    {
        OpsServer ops_server;
        ops_server.set_bind("127.0.0.1", kPort);
        ops_server.set_token("secret");
        Fake forward_key;
        forward_key.wire(ops_server);
        assert(ops_server.start());
        t_hello_first(ops_server);
        t_bad_token(ops_server);
        t_bad_json(ops_server);
        t_malformed_body(ops_server);
        t_happy_path(ops_server, forward_key);
        t_order_side(ops_server, forward_key);
        ops_server.stop();
        assert(!ops_server.running());
    }

    t_readonly_without_token();
    t_remote_bind_needs_token();
    t_second_bind_refused();
    Logger::instance().flush();
    std::cout << "test_ops_server: 9/9 PASS\n";
    return 0;
}
