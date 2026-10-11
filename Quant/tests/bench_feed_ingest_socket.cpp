// bench_feed_ingest — Winsock/BSD 소켓 이식 래퍼와 수신 서버·송신 연결 준비.
//  공용 준비는 bench_feed_ingest_support.h, 모드별 실행은 bench_feed_ingest.cpp의 main.
#include "bench_feed_ingest_support.h"

// ─────────────────────────────────────────────────────────────────────────────
// Winsock/BSD 소켓 이식 래퍼
// ─────────────────────────────────────────────────────────────────────────────
void socket_startup()
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

void socket_cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

void socket_close(socket_t socket_handle)
{
    if (socket_handle == kBadSock)
    {
        return;
    }
#ifdef _WIN32
    closesocket(socket_handle);
#else
    ::close(socket_handle);
#endif
}

void set_nodelay(socket_t nodelay)
{
    int one = 1;
    setsockopt(nodelay, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신 서버 소켓 준비: listen → accept 1건.
// ─────────────────────────────────────────────────────────────────────────────
socket_t make_listener(uint16_t port)
{
    socket_t listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listen_socket == kBadSock)
    {
        return kBadSock;
    }

    int one = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (::bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        socket_close(listen_socket);
        return kBadSock;
    }

    if (::listen(listen_socket, 1) != 0)
    {
        socket_close(listen_socket);
        return kBadSock;
    }

    return listen_socket;
}

socket_t connect_to(const std::string& host, uint16_t port)
{
    socket_t socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (socket_handle == kBadSock)
    {
        return kBadSock;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &address.sin_addr);

    if (::connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        socket_close(socket_handle);
        return kBadSock;
    }

    return socket_handle;
}
