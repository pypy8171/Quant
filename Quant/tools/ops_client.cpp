// tools/ops_client.cpp
// 운영단말 콘솔 클라이언트 — 돌고 있는 quant_trader의 OpsServer에 붙어 상태·보유를 읽고
// 수동주문을 낸다. MFC 단말을 만들기 전 프로토콜 왕복을 검증하는 용도이자, MFC 없이도
// 쓸 수 있는 최소 단말이다. 프레이밍은 ipc/OpsProtocol.h를 서버와 같이 컴파일한다. [why D-043]
//
//   사용법:
//     ops_client [--host 127.0.0.1] [--port 7100] [--token T] <명령>
//       status                       엔진 상태
//       positions                    보유 목록(원장 기준)
//       sell <ticker> <qty> [price]  수동 매도(price 생략=시장가). 결과(ORDER_RESULT)까지 기다린다
//       buy  <ticker> <qty> [price]  수동 매수
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

using json = nlohmann::json;

namespace
{

struct Conn
{
    sock_t           fd = SOCK_BAD;
    ops::FrameReader reader;

    // 프로세스 종료로 소켓을 놓으면 서버가 RST(10054)를 받는다 — 정상 종료는 shutdown 뒤 close.
    ~Conn()
    {
        if (fd != SOCK_BAD)
        {
#ifdef _WIN32
            ::shutdown(fd, SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
            sock_close(fd);
        }
    }

    bool send_frame(ops::OpsMsg t, const std::string& body)
    {
        auto bytes = ops::encode(t, body);
        size_t off = 0;

        while (off < bytes.size())
        {
            const int w = ::send(fd, reinterpret_cast<const char*>(bytes.data() + off),
                                 static_cast<int>(bytes.size() - off), 0);

            if (w <= 0)
            {
                return false;
            }

            off += static_cast<size_t>(w);
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

            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd, &rd);
            timeval tv{};
            tv.tv_sec  = static_cast<long>(left.count() / 1000);
            tv.tv_usec = static_cast<long>((left.count() % 1000) * 1000);
            const int n = ::select(static_cast<int>(fd) + 1, &rd, nullptr, nullptr, &tv);

            if (n == 0)
            {
                return -1;
            }

            if (n < 0)
            {
                return 0;
            }

            uint8_t buf[16384];
            const int r = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);

            if (r <= 0)
            {
                return 0;
            }

            reader.feed(buf, static_cast<size_t>(r));
        }
    }
};

bool connect_to(Conn& c, const std::string& host, int port)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    c.fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (c.fd == SOCK_BAD)
    {
        return false;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));

    if (host == "localhost")
    {
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    else if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1)
    {
        std::cerr << "호스트 해석 실패: " << host << "\n";
        return false;
    }

    return ::connect(c.fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
}

void print_positions(const json& j)
{
    std::cout << "account  ticker  name              qty  avg_price  reserved\n";

    for (const auto& p : j.value("positions", json::array()))
    {
        std::cout << (p.value("account", std::string()).empty() ? "-" : p.value("account", std::string())) << "  "
                  << p.value("ticker", std::string()) << "  " << p.value("name", std::string()) << "  "
                  << p.value("qty", 0) << "  " << p.value("avg_price", 0.0) << "  " << p.value("reserved", 0) << "\n";
    }
}

void print_push(const ops::Frame& f)
{
    std::cout << "[" << ops::msg_name(f.type) << "] " << f.body << "\n";
}

std::string make_cid()
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "cli-" + std::to_string(ms);
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

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];

        if (a == "--host" && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if (a == "--port" && i + 1 < argc)
        {
            port = std::atoi(argv[++i]);
        }
        else if (a == "--token" && i + 1 < argc)
        {
            token = argv[++i];
        }
        else
        {
            rest.push_back(a);
        }
    }

    if (rest.empty())
    {
        std::cerr << "사용법: ops_client [--host H] [--port P] [--token T] status|positions|sell|buy|watch|kill\n";
        return 1;
    }

    const std::string cmd = rest[0];
    Conn c;

    if (!connect_to(c, host, port))
    {
        std::cerr << "연결 실패 " << host << ":" << port << " — quant_trader가 ops_port로 떠 있는지 확인\n";
        return 1;
    }

    c.send_frame(ops::OpsMsg::HELLO, json{{"token", token}, {"client", "ops_client/0.1"}}.dump());
    ops::Frame f;

    if (c.recv_frame(f, 3000) != 1 || f.type != static_cast<uint8_t>(ops::OpsMsg::WELCOME))
    {
        std::cerr << "WELCOME 없음: " << (f.body.empty() ? "(응답 없음)" : f.body) << "\n";
        return 1;
    }

    const json welcome = json::parse(f.body, nullptr, false);
    const bool auth    = welcome.value("auth", false);
    std::cout << "연결됨 paper=" << welcome.value("paper", true) << " auth=" << auth << "\n";

    // WELCOME 직후 서버가 POSITIONS 스냅샷을 먼저 보낸다 — 명령 응답을 기다릴 때 섞여 들어오므로
    //  타입으로 걸러 받는다.
    auto wait_type = [&](ops::OpsMsg want, int timeout_ms, ops::Frame& out) -> int
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true)
        {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

            if (left.count() <= 0)
            {
                return -1;
            }

            const int rc = c.recv_frame(out, static_cast<int>(left.count()));

            if (rc != 1)
            {
                return rc;
            }

            if (out.type == static_cast<uint8_t>(want))
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

    if (cmd == "status")
    {
        c.send_frame(ops::OpsMsg::STATUS_REQ, "{}");

        if (wait_type(ops::OpsMsg::STATUS, 3000, f) != 1)
        {
            return 1;
        }

        std::cout << json::parse(f.body).dump(2) << "\n";
        return 0;
    }

    if (cmd == "positions")
    {
        c.send_frame(ops::OpsMsg::POS_REQ, "{}");

        if (wait_type(ops::OpsMsg::POSITIONS, 3000, f) != 1)
        {
            return 1;
        }

        print_positions(json::parse(f.body));
        return 0;
    }

    if (cmd == "sell" || cmd == "buy")
    {
        if (rest.size() < 3)
        {
            std::cerr << "사용법: " << cmd << " <ticker> <qty> [price]\n";
            return 1;
        }

        if (!auth)
        {
            std::cerr << "주문에는 --token이 필요하다(서버 ops_token과 일치)\n";
            return 2;
        }

        const std::string cid = make_cid();
        json req{{"cid", cid},
                 {"ticker", rest[1]},
                 {"side", cmd == "sell" ? "SELL" : "BUY"},
                 {"qty", std::atoi(rest[2].c_str())},
                 {"price", rest.size() > 3 ? std::atof(rest[3].c_str()) : 0.0}};
        c.send_frame(ops::OpsMsg::ORDER_REQ, req.dump());

        if (wait_type(ops::OpsMsg::ORDER_ACK, 3000, f) != 1)
        {
            std::cerr << "ORDER_ACK 없음\n";
            return 1;
        }

        json ack = json::parse(f.body);

        if (!ack.value("accepted", false))
        {
            std::cerr << "인테이크 거부: " << ack.value("msg", std::string()) << "\n";
            return 2;
        }

        std::cout << "인테이크 적재 cid=" << cid << " — 게이트·브로커 결과 대기…\n";

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

            const int rc = c.recv_frame(f, static_cast<int>(left.count()));

            if (rc == 0)
            {
                return 1;
            }

            if (rc != 1)
            {
                continue;
            }

            if (f.type == static_cast<uint8_t>(ops::OpsMsg::ORDER_RESULT))
            {
                json r = json::parse(f.body, nullptr, false);

                if (r.value("cid", std::string()) == cid)
                {
                    const bool ok = r.value("ok", false);
                    std::cout << (ok ? "접수 " : "거부 ") << r.value("order_id", std::string()) << " odno="
                              << r.value("odno", std::string()) << (ok ? "" : " — " + r.value("msg", std::string()))
                              << "\n";
                    return ok ? 0 : 2;
                }
            }

            print_push(f);
        }
    }

    if (cmd == "watch")
    {
        std::cout << "push 대기 (Ctrl+C로 종료)\n";
        auto last_ping = std::chrono::steady_clock::now();

        while (true)
        {
            const int rc = c.recv_frame(f, 1000);

            if (rc == 0)
            {
                std::cerr << "연결 끊김\n";
                return 1;
            }

            if (rc == 1)
            {
                if (f.type == static_cast<uint8_t>(ops::OpsMsg::POSITIONS))
                {
                    print_positions(json::parse(f.body, nullptr, false));
                }
                else
                {
                    print_push(f);
                }
            }

            if (std::chrono::steady_clock::now() - last_ping >= std::chrono::seconds(10))
            {
                last_ping = std::chrono::steady_clock::now();
                c.send_frame(ops::OpsMsg::PING, "{}");
            }
        }
    }

    if (cmd == "kill")
    {
        c.send_frame(ops::OpsMsg::KILL, "{}");

        if (wait_type(ops::OpsMsg::KILL_ACK, 3000, f) != 1)
        {
            return 1;
        }

        std::cout << f.body << "\n";
        return json::parse(f.body).value("ok", false) ? 0 : 2;
    }

    std::cerr << "알 수 없는 명령: " << cmd << "\n";
    return 1;
}
