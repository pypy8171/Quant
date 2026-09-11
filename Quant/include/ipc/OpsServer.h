#pragma once
// 운영단말 TCP 서버 — 수동 주문·상태 조회·킬스위치를 엔진 밖 단말에 연다. 소켓 전부는
//  전용 srv_thread_ 하나가 select()로 다룬다(클라이언트는 몇 명 안 된다). 다른 스레드는
//  broadcast()로 송신 큐에 넣기만 한다. 프레이밍은 ipc/OpsProtocol.h. [why D-043]
//
//  인증: HELLO의 token이 설정값과 같아야 주문·KILL을 받는다. token이 비어 있으면 조회만
//  허용하고, 루프백이 아닌 주소에 token 없이 bind하는 것은 start()가 거부한다.

#include "ipc/OpsProtocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// winsock2.h는 windows.h(비-LEAN)보다 먼저 와야 winsock.h와 충돌하지 않는다.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
// wingdi.h의 ERROR 매크로가 Logger::Level::ERROR를 깨뜨린다 — KisWebSocket.h와 같은 처리.
#ifdef ERROR
#undef ERROR
#endif
using ops_socket_t = SOCKET;
#else
using ops_socket_t = int;
#endif

// 단말이 낸 주문 한 건. side는 "BUY"/"SELL", price 0=시장가, ref_price 0=엔진이 평단으로 대체.
struct OpsOrderReq
{
    std::string cid;       // 단말이 붙인 식별자 — 같은 cid의 재전송은 한 번만 처리한다
    std::string ticker;
    std::string side;
    int         qty       = 0;
    double      price     = 0.0;
    double      ref_price = 0.0;
    std::string account;
};

class OpsServer
{
public:
    OpsServer();
    ~OpsServer();
    OpsServer(const OpsServer&)            = delete;
    OpsServer& operator=(const OpsServer&) = delete;

    // start() 전에만. 빈 주소는 무시한다.
    void set_bind(const std::string& addr, int port);
    void set_token(std::string token) { token_ = std::move(token); }
    void set_paper(bool paper) { paper_ = paper; }

    // 주문 인테이크. 빈 문자열이면 적재됨, 아니면 거부 사유. 서버 스레드에서 불린다 —
    //  큐에 넣고 바로 돌아와야 한다(게이트·브로커는 엔진 스레드 몫).
    using OrderHandler = std::function<std::string(const OpsOrderReq&)>;
    // JSON 문자열을 돌려주는 조회기 둘. 서버 스레드에서 1초마다도 불리니 락을 오래 잡지 않는다.
    using JsonProvider = std::function<std::string()>;
    using KillHandler  = std::function<void()>;

    void set_order_handler(OrderHandler h) { on_order_ = std::move(h); }
    void set_status_provider(JsonProvider p) { status_ = std::move(p); }
    void set_positions_provider(JsonProvider p) { positions_ = std::move(p); }
    void set_kill_handler(KillHandler h) { on_kill_ = std::move(h); }

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    int  port() const { return port_; }

    // 모든 인증된 연결로 push. 어느 스레드에서든 부를 수 있다.
    void broadcast(ops::OpsMsg type, const std::string& body);

    size_t client_count() const;

private:
    struct Client
    {
        ops_socket_t     fd;
        ops::FrameReader reader;
        std::string      out;   // 아직 못 보낸 바이트
        bool             hello = false;
        bool             auth  = false;
        std::string      name;
    };

    void thread_fn();
    void accept_one();
    void on_readable(Client& c);
    bool on_frame(Client& c, const ops::Frame& f); // false면 끊는다
    void send(Client& c, ops::OpsMsg type, const std::string& body);
    void flush(Client& c);
    void close_client(ops_socket_t fd);
    void push_positions_if_changed();

    std::string bind_addr_ = "127.0.0.1";
    int         port_      = 7100;
    std::string token_;
    bool        paper_ = true;

    OrderHandler on_order_;
    JsonProvider status_;
    JsonProvider positions_;
    KillHandler  on_kill_;

    std::atomic<bool> running_{false};
    std::thread       srv_thread_;
    ops_socket_t      listen_fd_;

    // fd→Client. 서버 스레드만 만지지만 client_count()가 다른 스레드에서 읽어 뮤텍스를 둔다.
    mutable std::mutex                          clients_mtx_;
    std::unordered_map<ops_socket_t, Client>    clients_;

    // 다른 스레드가 넣고 서버 스레드가 빼는 push 큐
    std::mutex                                        bcast_mtx_;
    std::vector<std::pair<ops::OpsMsg, std::string>>  bcast_;

    std::string last_positions_json_;
};
