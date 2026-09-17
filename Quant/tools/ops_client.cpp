// tools/ops_client.cpp
// 운영단말 콘솔 클라이언트 — 돌고 있는 quant_trader의 OpsServer에 붙어 상태·보유를 읽고
// 수동주문을 낸다. MFC 단말을 만들기 전 프로토콜 왕복을 검증하는 용도이자, MFC 없이도
// 쓸 수 있는 최소 단말이다. 프레이밍은 ipc/OpsProtocol.h를 서버와 같이 컴파일한다. [why D-043]
//
//   사용법:
//     ops_client [--host 127.0.0.1] [--port 7100] [--token T] <명령>
//       status                       엔진 상태
//       positions                    보유 목록(원장 기준)
//       sell <ticker> <quantity> [price]  수동 매도(price 생략=시장가). 결과(ORDER_RESULT)까지 기다린다
//       buy  <ticker> <quantity> [price]  수동 매수
//       watch                        접속을 유지하며 push(POSITIONS·ORDER_RESULT·FILL)를 출력
//       kill                         킬스위치 + 엔진 종료 (토큰 필요)
//
//   종료코드: 0 성공, 1 인자·연결 오류, 2 주문 거부(ACK 거부 또는 RESULT ok=false), 3 결과 대기 시간 초과

#include "ipc/OpsProtocol.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
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

using json = nlohmann::json;

namespace
{

struct Conn
{
    socket_handle_t           descriptor = SOCK_BAD;
    ops::FrameReader reader;

    // 프로세스 종료로 소켓을 놓으면 서버가 RST(10054)를 받는다 — 정상 종료는 shutdown 뒤 close.
    ~Conn()
    {
        if (descriptor != SOCK_BAD)
        {
#ifdef _WIN32
            ::shutdown(descriptor, SD_BOTH);
#else
            ::shutdown(descriptor, SHUT_RDWR);
#endif
            socket_close(descriptor);
        }
    }

    bool send_frame(ops::OpsMsg ops_message, const std::string& body)
    {
        auto bytes = ops::encode(ops_message, body);
        size_t offset = 0;

        while (offset < bytes.size())
        {
            const int width = ::send(descriptor, reinterpret_cast<const char*>(bytes.data() + offset),
                                 static_cast<int>(bytes.size() - offset), 0);

            if (width <= 0)
            {
                return false;
            }

            offset += static_cast<size_t>(width);
        }

        return true;
    }

    // timeout_ms 안에 프레임 하나. 0=끊김/오류, -1=시간 초과, 1=수신.
    int recv_frame(ops::Frame& out, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            if (reader.next(out))
            {
                return 1;
            }

            if (reader.bad())
            {
                std::cerr << "서버 프레임 규약 위반\n";
                return 0;
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
            const int count = ::select(static_cast<int>(descriptor) + 1, &read_set, nullptr, nullptr, &time_value);

            if (count == 0)
            {
                return -1;
            }

            if (count < 0)
            {
                return 0;
            }

            uint8_t buffer[16384];
            const int result = ::recv(descriptor, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

            if (result <= 0)
            {
                return 0;
            }

            reader.feed(buffer, static_cast<size_t>(result));
        }
    }
};

bool connect_to(Conn& connection, const std::string& host, int port)
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    connection.descriptor = ::socket(AF_INET, SOCK_STREAM, 0);

    if (connection.descriptor == SOCK_BAD)
    {
        return false;
    }

    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port   = htons(static_cast<uint16_t>(port));

    if (host == "localhost")
    {
        socket_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    else if (inet_pton(AF_INET, host.c_str(), &socket_address.sin_addr) != 1)
    {
        std::cerr << "호스트 해석 실패: " << host << "\n";
        return false;
    }

    return ::connect(connection.descriptor, reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)) == 0;
}

void print_positions(const json& document)
{
    std::cout << "account  ticker  name              qty  avg_price  reserved  last\n";

    for (const auto& position_node : document.value("positions", json::array()))
    {
        std::cout << (position_node.value("account", std::string()).empty() ? "-" : position_node.value("account", std::string())) << "  "
                  << position_node.value("ticker", std::string()) << "  " << position_node.value("name", std::string()) << "  "
                  << position_node.value("qty", 0) << "  " << position_node.value("avg_price", 0.0) << "  " << position_node.value("reserved", 0) << "  "
                  << position_node.value("last", 0.0) << "\n";
    }
}

void print_push(const ops::Frame& frame)
{
    std::cout << "[" << ops::message_name(frame.type) << "] " << frame.body << "\n";
}

std::string make_client_id()
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "cli-" + std::to_string(milliseconds);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string host = "127.0.0.1";
    int         port = 7100;
    std::string token;
    std::vector<std::string> rest;

    for (int index = 1; index < argc; ++index)
    {
        std::string argument = argv[index];

        if (argument == "--host" && index + 1 < argc)
        {
            host = argv[++index];
        }
        else if (argument == "--port" && index + 1 < argc)
        {
            port = std::atoi(argv[++index]);
        }
        else if (argument == "--token" && index + 1 < argc)
        {
            token = argv[++index];
        }
        else
        {
            rest.push_back(argument);
        }
    }

    if (rest.empty())
    {
        std::cerr << "사용법: ops_client [--host H] [--port P] [--token T] status|positions|sell|buy|watch|kill\n";
        return 1;
    }

    const std::string command = rest[0];
    Conn connection;

    if (!connect_to(connection, host, port))
    {
        std::cerr << "연결 실패 " << host << ":" << port << " — quant_trader가 ops_port로 떠 있는지 확인\n";
        return 1;
    }

    connection.send_frame(ops::OpsMsg::HELLO, json{{"token", token}, {"client", "ops_client/0.1"}}.dump());
    ops::Frame frame;

    if (connection.recv_frame(frame, 3000) != 1 || frame.type != static_cast<uint8_t>(ops::OpsMsg::WELCOME))
    {
        std::cerr << "WELCOME 없음: " << (frame.body.empty() ? "(응답 없음)" : frame.body) << "\n";
        return 1;
    }

    const json welcome = json::parse(frame.body, nullptr, false);
    const bool authentication    = welcome.value("auth", false);
    std::cout << "연결됨 paper=" << welcome.value("paper", true) << " auth=" << authentication << "\n";

    // WELCOME 직후 서버가 POSITIONS 스냅샷을 먼저 보낸다 — 명령 응답을 기다릴 때 섞여 들어오므로
    //  타입으로 걸러 받는다.
    auto wait_type = [&](ops::OpsMsg wanted_type, int timeout_ms, ops::Frame& out) -> int
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

            if (left.count() <= 0)
            {
                return -1;
            }

            const int result_code = connection.recv_frame(out, static_cast<int>(left.count()));

            if (result_code != 1)
            {
                return result_code;
            }

            if (out.type == static_cast<uint8_t>(wanted_type))
            {
                return 1;
            }

            if (out.type == static_cast<uint8_t>(ops::OpsMsg::ERROR_MSG))
            {
                std::cerr << "서버 오류: " << out.body << "\n";
                return 0;
            }
        }
    };

    if (command == "status")
    {
        connection.send_frame(ops::OpsMsg::STATUS_REQ, "{}");

        if (wait_type(ops::OpsMsg::STATUS, 3000, frame) != 1)
        {
            return 1;
        }

        std::cout << json::parse(frame.body).dump(2) << "\n";
        return 0;
    }

    if (command == "positions")
    {
        connection.send_frame(ops::OpsMsg::POS_REQ, "{}");

        if (wait_type(ops::OpsMsg::POSITIONS, 3000, frame) != 1)
        {
            return 1;
        }

        print_positions(json::parse(frame.body));
        return 0;
    }

    if (command == "sell" || command == "buy")
    {
        if (rest.size() < 3)
        {
            std::cerr << "사용법: " << command << " <ticker> <qty> [price]\n";
            return 1;
        }

        if (!authentication)
        {
            std::cerr << "주문에는 --token이 필요하다(서버 ops_token과 일치)\n";
            return 2;
        }

        const std::string client_id = make_client_id();
        json request{{"cid", client_id},
                 {"ticker", rest[1]},
                 {"side", command == "sell" ? "SELL" : "BUY"},
                 {"qty", std::atoi(rest[2].c_str())},
                 {"price", rest.size() > 3 ? std::atof(rest[3].c_str()) : 0.0}};
        connection.send_frame(ops::OpsMsg::ORDER_REQ, request.dump());

        if (wait_type(ops::OpsMsg::ORDER_ACK, 3000, frame) != 1)
        {
            std::cerr << "ORDER_ACK 없음\n";
            return 1;
        }

        json acknowledgement = json::parse(frame.body);

        if (!acknowledgement.value("accepted", false))
        {
            std::cerr << "인테이크 거부: " << acknowledgement.value("msg", std::string()) << "\n";
            return 2;
        }

        std::cout << "인테이크 적재 cid=" << client_id << " — 게이트·브로커 결과 대기…\n";

        // 같은 cid의 ORDER_RESULT만 기다린다(전략 주문 결과도 같은 채널로 온다). 브로커 왕복이
        //  3~5초라 15초까지 본다.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

        while (true)
        {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

            if (left.count() <= 0)
            {
                std::cerr << "ORDER_RESULT 대기 시간 초과 — 로그(quant_trader.log)에서 cid로 확인\n";
                return 3;
            }

            const int result_code = connection.recv_frame(frame, static_cast<int>(left.count()));

            if (result_code == 0)
            {
                return 1;
            }

            if (result_code != 1)
            {
                continue;
            }

            if (frame.type == static_cast<uint8_t>(ops::OpsMsg::ORDER_RESULT))
            {
                json node = json::parse(frame.body, nullptr, false);

                if (node.value("cid", std::string()) == client_id)
                {
                    const bool ok = node.value("ok", false);
                    std::cout << (ok ? "접수 " : "거부 ") << node.value("order_id", std::string()) << " odno="
                              << node.value("odno", std::string()) << (ok ? "" : " — " + node.value("msg", std::string()))
                              << "\n";
                    return ok ? 0 : 2;
                }
            }

            print_push(frame);
        }
    }

    if (command == "watch")
    {
        std::cout << "push 대기 (Ctrl+C로 종료)\n";
        auto last_ping = std::chrono::steady_clock::now();

        while (true)
        {
            const int result_code = connection.recv_frame(frame, 1000);

            if (result_code == 0)
            {
                std::cerr << "연결 끊김\n";
                return 1;
            }

            if (result_code == 1)
            {
                if (frame.type == static_cast<uint8_t>(ops::OpsMsg::POSITIONS))
                {
                    print_positions(json::parse(frame.body, nullptr, false));
                }
                else
                {
                    print_push(frame);
                }
            }

            if (std::chrono::steady_clock::now() - last_ping >= std::chrono::seconds(10))
            {
                last_ping = std::chrono::steady_clock::now();
                connection.send_frame(ops::OpsMsg::PING, "{}");
            }
        }
    }

    if (command == "kill")
    {
        connection.send_frame(ops::OpsMsg::KILL, "{}");

        if (wait_type(ops::OpsMsg::KILL_ACK, 3000, frame) != 1)
        {
            return 1;
        }

        std::cout << frame.body << "\n";
        return json::parse(frame.body).value("ok", false) ? 0 : 2;
    }

    std::cerr << "알 수 없는 명령: " << command << "\n";
    return 1;
}
