// 운영단말 TCP 서버 구현. 스레드 소유권: 소켓 전부 srv_thread_. [why D-043]
#include "ipc/OpsServer.h"
#include "utils/Logger.h"

#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define OPS_INVALID INVALID_SOCKET
#define ops_close   closesocket
static int ops_errno() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define OPS_INVALID (-1)
#define ops_close   ::close
static int ops_errno() { return errno; }
#endif

using json = nlohmann::json;

namespace
{

bool is_loopback(const std::string& addr)
{
    return addr == "127.0.0.1" || addr == "localhost" || addr == "::1";
}

void set_nonblocking(ops_socket_t fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

bool would_block(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

bool is_reset(int error)
{
#ifdef _WIN32
    return error == WSAECONNRESET;
#else
    return error == ECONNRESET;
#endif
}

std::string err_body(const std::string& message)
{
    return json{{"msg", message}}.dump();
}

} // namespace

OpsServer::OpsServer() : listen_fd_(OPS_INVALID) {}

OpsServer::~OpsServer()
{
    stop();
}

void OpsServer::set_bind(const std::string& addr, int port)
{
    if (!addr.empty())
    {
        bind_addr_ = addr;
    }

    if (port > 0 && port < 65536)
    {
        port_ = port;
    }
}

bool OpsServer::start()
{
    if (running_.load())
    {
        return true;
    }

    if (!is_loopback(bind_addr_) && token_.empty())
    {
        // 토큰 없는 원격 노출은 누구나 KILL·주문을 낼 수 있다는 뜻이다. 시작 자체를 막는다.
        LOG_ERROR("[Ops] bind=" + bind_addr_ + " 는 루프백이 아닌데 ops_token이 비어 있다 — 서버를 열지 않음");
        return false;
    }

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd_ == OPS_INVALID)
    {
        LOG_ERROR("[Ops] socket() 실패 err=" + std::to_string(ops_errno()));
        return false;
    }

    int one = 1;
#ifdef _WIN32
    // Windows의 SO_REUSEADDR는 같은 포트에 두 번째 listen도 허용한다 — 엔진이 둘 뜨면 둘 다 7100을
    //  잡고 아무도 모른다. 배타 옵션으로 두 번째 기동의 bind가 실패하게 해 중복 기동 신호로 쓴다.
    setsockopt(listen_fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port_));

    if (bind_addr_ == "localhost")
    {
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    else if (inet_pton(AF_INET, bind_addr_.c_str(), &sa.sin_addr) != 1)
    {
        LOG_ERROR("[Ops] bind 주소 해석 실패: " + bind_addr_);
        ops_close(listen_fd_);
        listen_fd_ = OPS_INVALID;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(listen_fd_, 8) != 0)
    {
        LOG_ERROR("[Ops] bind/listen 실패 " + bind_addr_ + ":" + std::to_string(port_) +
                  " err=" + std::to_string(ops_errno()));
        ops_close(listen_fd_);
        listen_fd_ = OPS_INVALID;
        return false;
    }

    set_nonblocking(listen_fd_);
    running_.store(true);
    srv_thread_ = std::thread(&OpsServer::thread_fn, this);
    LOG_INFO("[Ops] 운영단말 서버 대기 " + bind_addr_ + ":" + std::to_string(port_) +
             (token_.empty() ? " (token 없음 — 조회만 허용)" : " (token 인증)"));
    return true;
}

void OpsServer::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (srv_thread_.joinable())
    {
        srv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mtx_);

        for (auto& entry : clients_)
        {
            ops_close(entry.first);
        }

        clients_.clear();
    }

    if (listen_fd_ != OPS_INVALID)
    {
        ops_close(listen_fd_);
        listen_fd_ = OPS_INVALID;
    }

    LOG_INFO("[Ops] 운영단말 서버 종료");
}

size_t OpsServer::client_count() const
{
    std::lock_guard<std::mutex> lock(clients_mtx_);
    return clients_.size();
}

void OpsServer::broadcast(ops::OpsMsg type, const std::string& body)
{
    if (!running_.load())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(bcast_mtx_);
    bcast_.emplace_back(type, body);
}

// ─── 서버 스레드 ─────────────────────────────────────────────────────────────

void OpsServer::thread_fn()
{
    auto last_pos_push = std::chrono::steady_clock::now();

    while (running_.load())
    {
        fd_set rd;
        fd_set wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(listen_fd_, &rd);
        ops_socket_t maxfd = listen_fd_;

        {
            std::lock_guard<std::mutex> lock(clients_mtx_);

            for (auto& entry : clients_)
            {
                FD_SET(entry.first, &rd);

                if (!entry.second.out.empty())
                {
                    FD_SET(entry.first, &wr);
                }

#ifndef _WIN32
                if (entry.first > maxfd)
                {
                    maxfd = entry.first;
                }
#endif
            }
        }

        // 50ms — broadcast 큐와 포지션 push를 이 주기로 돌린다. 운영 화면에는 충분하다.
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;
        const int count = ::select(static_cast<int>(maxfd) + 1, &rd, &wr, nullptr, &tv);

        if (count < 0)
        {
            const int error = ops_errno();
#ifdef _WIN32
            if (error == WSAEINTR)
#else
            if (error == EINTR)
#endif
            {
                continue;
            }

            LOG_ERROR("[Ops] select 실패 err=" + std::to_string(error));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (FD_ISSET(listen_fd_, &rd))
        {
            accept_one();
        }

        std::vector<ops_socket_t> dead;

        {
            std::lock_guard<std::mutex> lock(clients_mtx_);

            for (auto& entry : clients_)
            {
                Client& client = entry.second;

                if (FD_ISSET(entry.first, &rd))
                {
                    on_readable(client);
                }

                if (client.fd == OPS_INVALID)
                {
                    dead.push_back(entry.first);
                    continue;
                }

                if (FD_ISSET(entry.first, &wr) || !client.out.empty())
                {
                    flush(client);
                }

                if (client.fd == OPS_INVALID)
                {
                    dead.push_back(entry.first);
                }
            }
        }

        for (auto fd : dead)
        {
            close_client(fd);
        }

        // 다른 스레드가 쌓아둔 push
        std::vector<std::pair<ops::OpsMsg, std::string>> pending;

        {
            std::lock_guard<std::mutex> lock(bcast_mtx_);
            pending.swap(bcast_);
        }

        if (!pending.empty())
        {
            std::lock_guard<std::mutex> lock(clients_mtx_);

            for (auto& entry : clients_)
            {
                if (!entry.second.auth && token_.empty() == false)
                {
                    continue; // 토큰 인증이 켜진 서버에서 미인증 연결에는 push하지 않는다
                }

                for (auto& pending_entry : pending)
                {
                    send(entry.second, pending_entry.first, pending_entry.second);
                }
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (now - last_pos_push >= std::chrono::seconds(1))
        {
            last_pos_push = now;
            push_positions_if_changed();
        }
    }
}

void OpsServer::accept_one()
{
    sockaddr_in peer{};
#ifdef _WIN32
    int plen = sizeof(peer);
#else
    socklen_t plen = sizeof(peer);
#endif
    ops_socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);

    if (fd == OPS_INVALID)
    {
        return;
    }

    set_nonblocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    char ip[64] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    Client client;
    client.fd   = fd;
    client.name = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

    {
        std::lock_guard<std::mutex> lock(clients_mtx_);

        if (clients_.size() >= 8)
        {
            // 운영단말이 8개를 넘을 일은 없다 — 넘으면 소켓 누수나 스캐너다.
            LOG_WARN("[Ops] 연결 상한(8) — 거부 " + client.name);
            ops_close(fd);
            return;
        }

        clients_.emplace(fd, std::move(client));
    }

    LOG_INFO("[Ops] 연결 " + std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port)));
}

void OpsServer::on_readable(Client& client)
{
    uint8_t buffer[16384];

    while (true)
    {
        const int result = ::recv(client.fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

        if (result > 0)
        {
            client.reader.feed(buffer, static_cast<size_t>(result));
            continue;
        }

        if (result == 0)
        {
            LOG_INFO("[Ops] 상대 종료 " + client.name);
            ops_close(client.fd);
            client.fd = OPS_INVALID;
            return;
        }

        if (would_block(ops_errno()))
        {
            break;
        }

        // 단말이 shutdown 없이 닫으면 RST(10054/ECONNRESET)로 온다 — 결함이 아니라 끊김이다.
        const int error = ops_errno();

        if (is_reset(error))
        {
            LOG_INFO("[Ops] 연결 끊김(RST) " + client.name);
        }
        else
        {
            LOG_WARN("[Ops] recv 실패 " + client.name + " err=" + std::to_string(error));
        }

        ops_close(client.fd);
        client.fd = OPS_INVALID;
        return;
    }

    ops::Frame frame;

    while (client.reader.next(frame))
    {
        if (!on_frame(client, frame))
        {
            flush(client); // 거부 사유를 보내고 끊는다
            ops_close(client.fd);
            client.fd = OPS_INVALID;
            return;
        }
    }

    if (client.reader.bad())
    {
        LOG_WARN("[Ops] 프레임 규약 위반 — 끊음 " + client.name);
        ops_close(client.fd);
        client.fd = OPS_INVALID;
    }
}

bool OpsServer::on_frame(Client& client, const ops::Frame& frame)
{
    using ops::OpsMsg;
    const auto type = static_cast<OpsMsg>(frame.type);
    json body;

    if (!frame.body.empty())
    {
        body = json::parse(frame.body, nullptr, false);

        if (body.is_discarded())
        {
            send(client, OpsMsg::ERROR_MSG, err_body("본문 JSON 파싱 실패"));
            return false;
        }
    }

    if (!client.hello)
    {
        if (type != OpsMsg::HELLO)
        {
            send(client, OpsMsg::ERROR_MSG, err_body("첫 프레임은 HELLO여야 한다"));
            return false;
        }

        const std::string tok = body.value("token", std::string());
        client.hello = true;
        client.auth  = token_.empty() ? false : (tok == token_);

        if (!token_.empty() && !client.auth)
        {
            LOG_WARN("[Ops] 토큰 불일치 — 끊음 " + client.name);
            send(client, OpsMsg::ERROR_MSG, err_body("token 불일치"));
            return false;
        }

        const std::string who = body.value("client", std::string("?"));
        LOG_INFO("[Ops] HELLO " + client.name + " client=" + who + (client.auth ? " auth" : " read-only"));
        send(client, OpsMsg::WELCOME, json{{"ok", true}, {"auth", client.auth}, {"engine", "quant_trader"}, {"paper", paper_}}.dump());

        if (positions_)
        {
            send(client, OpsMsg::POSITIONS, positions_());
        }

        return true;
    }

    switch (type)
    {
        case OpsMsg::PING:
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            send(client, OpsMsg::PONG, json{{"ts", ms}}.dump());
            return true;
        }

        case OpsMsg::STATUS_REQ:
            send(client, OpsMsg::STATUS, status_ ? status_() : "{}");
            return true;

        case OpsMsg::POS_REQ:
            send(client, OpsMsg::POSITIONS, positions_ ? positions_() : "{\"positions\":[]}");
            return true;

        case OpsMsg::ORDER_REQ:
        {
            OpsOrderReq ops_order_req;
            ops_order_req.cid       = body.value("cid", std::string());
            ops_order_req.ticker    = body.value("ticker", std::string());
            ops_order_req.side      = body.value("side", std::string());
            ops_order_req.quantity       = body.value("qty", 0);
            ops_order_req.price     = body.value("price", 0.0);
            ops_order_req.ref_price = body.value("ref_price", 0.0);
            ops_order_req.account   = body.value("account", std::string());

            std::string why;

            if (!client.auth)
            {
                why = token_.empty() ? "서버 ops_token 미설정 — 주문 불가" : "미인증";
            }
            else if (!on_order_)
            {
                why = "주문 핸들러 없음";
            }
            else
            {
                why = on_order_(ops_order_req);
            }

            send(client, OpsMsg::ORDER_ACK, json{{"cid", ops_order_req.cid}, {"accepted", why.empty()}, {"msg", why}}.dump());
            return true;
        }

        case OpsMsg::KILL:
        {
            if (!client.auth)
            {
                send(client, OpsMsg::KILL_ACK, json{{"ok", false}, {"msg", "미인증"}}.dump());
                return true;
            }

            LOG_WARN("[Ops] KILL 수신 " + client.name);
            send(client, OpsMsg::KILL_ACK, json{{"ok", true}}.dump());

            if (on_kill_)
            {
                on_kill_();
            }

            return true;
        }

        default:
            send(client, OpsMsg::ERROR_MSG, err_body(std::string("지원하지 않는 타입 ") + std::to_string(frame.type)));
            return true;
    }
}

void OpsServer::send(Client& client, ops::OpsMsg type, const std::string& body)
{
    auto bytes = ops::encode(type, body);

    if (bytes.empty() && !body.empty())
    {
        LOG_WARN("[Ops] 본문 상한 초과 — 드롭 " + std::string(ops::msg_name(static_cast<uint8_t>(type))));
        return;
    }

    if (client.out.size() > (4u << 20))
    {
        // 안 읽는 단말에 무한히 쌓지 않는다. 4 MiB면 이미 화면이 죽은 것이다.
        LOG_WARN("[Ops] 송신 적체 4MiB — 끊음 " + client.name);
        ops_close(client.fd);
        client.fd = OPS_INVALID;
        return;
    }

    client.out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    flush(client);
}

void OpsServer::flush(Client& client)
{
    while (client.fd != OPS_INVALID && !client.out.empty())
    {
        const int width = ::send(client.fd, client.out.data(), static_cast<int>(client.out.size()), 0);

        if (width > 0)
        {
            client.out.erase(0, static_cast<size_t>(width));
            continue;
        }

        if (width < 0 && would_block(ops_errno()))
        {
            return; // 다음 select에서 wr로 깨운다
        }

        LOG_WARN("[Ops] send 실패 " + client.name + " err=" + std::to_string(ops_errno()));
        ops_close(client.fd);
        client.fd = OPS_INVALID;
    }
}

void OpsServer::close_client(ops_socket_t fd)
{
    std::lock_guard<std::mutex> lock(clients_mtx_);
    clients_.erase(fd);
}

void OpsServer::push_positions_if_changed()
{
    if (!positions_)
    {
        return;
    }

    std::string now = positions_();

    if (now == last_positions_json_)
    {
        return;
    }

    last_positions_json_ = now;
    std::lock_guard<std::mutex> lock(clients_mtx_);

    for (auto& entry : clients_)
    {
        if (entry.second.hello)
        {
            send(entry.second, ops::OpsMsg::POSITIONS, now);
        }
    }
}
