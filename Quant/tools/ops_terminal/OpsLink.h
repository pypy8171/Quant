#pragma once
// 운영단말(MFC)의 소켓 담당. 접속·재접속·프레임 송수신·하트비트를 작업자 스레드 하나가 맡고,
//  UI 스레드에는 PostMessage로만 넘긴다(MFC 컨트롤은 만든 스레드에서만 만진다). [why D-043]
//
//  스레드 소유권: 소켓과 FrameReader는 작업자 스레드만. UI는 send()로 송신 큐에 넣고,
//  WM_OPS_FRAME / WM_OPS_STATE 메시지로 결과를 받는다. 메시지의 LPARAM은 new로 만든 포인터라
//  받는 쪽이 delete한다.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "ipc/OpsProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// UI 스레드로 가는 메시지. WPARAM은 쓰지 않는다.
constexpr UINT WM_OPS_FRAME = WM_USER + 101; // LPARAM = ops::Frame* (수신 프레임)
constexpr UINT WM_OPS_STATE = WM_USER + 102; // LPARAM = OpsStateMsg*

enum class LinkState
{
    Disconnected,
    Connecting,
    Connected, // TCP 연결됨, WELCOME 대기
    Ready,     // WELCOME 수신
};

struct OpsStateMsg
{
    LinkState   state;
    std::string detail; // 사람이 읽는 한 줄
};

class OpsLink
{
public:
    OpsLink();
    ~OpsLink();
    OpsLink(const OpsLink&)            = delete;
    OpsLink& operator=(const OpsLink&) = delete;

    // 접속을 시작한다. 끊기면 backoff(1→2→4…최대 30초)로 다시 붙는다. 이미 돌고 있으면 무시.
    void start(HWND notify, const std::string& host, int port, const std::string& token);
    // 스레드를 내린다. 블로킹.
    void stop();

    bool running() const { return running_.load(); }

    // 어느 스레드에서든. 연결이 없으면 버리고 false.
    bool send(ops::OpsMsg type, const std::string& body);

private:
    void thread_fn();
    bool connect_once();
    void session_loop();   // 연결 하나의 수명. 돌아오면 끊긴 것
    void post_state(LinkState s, const std::string& detail);
    void post_frame(const ops::Frame& f);
    void close_socket();

    HWND        hwnd_ = nullptr;
    std::string host_;
    int         port_ = 0;
    std::string token_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread       th_;
    SOCKET            fd_ = INVALID_SOCKET;

    // 송신 큐. UI가 넣고 작업자가 뺀다. cv로 select 대기를 깨운다.
    std::mutex                          q_mtx_;
    std::deque<std::vector<uint8_t>>    q_;
    std::condition_variable             q_cv_;
    std::atomic<bool>                   wake_{false};

    // 하트비트: 10초마다 PING, 30초 무수신이면 죽은 연결로 본다. 단위 ms.
    static constexpr int kPingEveryMs   = 10'000;
    static constexpr int kDeadAfterMs   = 30'000;
    static constexpr int kBackoffMaxMs  = 30'000;
};
