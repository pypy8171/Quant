// 운영단말 소켓 작업자 구현. 스레드 소유권은 OpsLink.h 머리 참조. [why D-043]
#include "pch.h"

#include "OpsLink.h"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>

namespace
{

using Clock = std::chrono::steady_clock;

int64_t ms_since(Clock::time_point time_point)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - time_point).count();
}

// 소켓이 없거나 hwnd가 죽었을 때 PostMessage가 실패하면 넘긴 포인터를 우리가 지운다.
template <typename T>
void post_or_delete(HWND window, UINT message, T* item)
{
    if (window == nullptr || !::PostMessage(window, message, 0, reinterpret_cast<LPARAM>(item)))
    {
        delete item;
    }
}

} // namespace

OpsLink::OpsLink()
{
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    network_event_ = ::WSACreateEvent();
    wake_event_    = ::WSACreateEvent();
}

OpsLink::~OpsLink()
{
    stop();

    if (network_event_ != WSA_INVALID_EVENT)
    {
        ::WSACloseEvent(network_event_);
    }

    if (wake_event_ != WSA_INVALID_EVENT)
    {
        ::WSACloseEvent(wake_event_);
    }

    WSACleanup();
}

void OpsLink::start(HWND notify, const std::string& host, int port, const std::string& token)
{
    if (running_.exchange(true))
    {
        return;
    }

    hwnd_  = notify;
    host_  = host;
    port_  = port;
    token_ = token;
    thread_    = std::thread(&OpsLink::thread_fn, this);
}

void OpsLink::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    ::WSASetEvent(wake_event_);             // 세션 대기를 깨운다
    queue_condition_variable_.notify_all(); // 재접속 대기를 깨운다

    if (thread_.joinable())
    {
        thread_.join();
    }

    close_socket();
    connected_.store(false);
}

bool OpsLink::send(ops::OpsMsg type, const std::string& body)
{
    if (!connected_.load())
    {
        return false;
    }

    auto bytes = ops::encode(type, body);

    if (bytes.empty())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(bytes));
    }

    ::WSASetEvent(wake_event_); // 작업자가 세션 대기 중이면 바로 깨어 보낸다
    return true;
}

void OpsLink::post_state(LinkState link_state, const std::string& detail)
{
    post_or_delete(hwnd_, WM_OPS_STATE, new OpsStateMsg{link_state, detail});
}

void OpsLink::post_frame(const ops::Frame& frame)
{
    post_or_delete(hwnd_, WM_OPS_FRAME, new ops::Frame(frame));
}

void OpsLink::close_socket()
{
    if (descriptor_ != INVALID_SOCKET)
    {
        ::shutdown(descriptor_, SD_BOTH);
        ::closesocket(descriptor_);
        descriptor_ = INVALID_SOCKET;
    }
}

void OpsLink::thread_fn()
{
    int backoff = 1000;

    while (running_.load())
    {
        post_state(LinkState::Connecting, host_ + ":" + std::to_string(port_));

        if (connect_once())
        {
            backoff = 1000;

            // [inv] HELLO가 큐 맨 앞이어야 한다 — connected_를 먼저 켜면 UI 스레드의 주문이 인증 전에 끼어든다.
            // 토큰은 json 라이브러리로 감싸 따옴표·역슬래시가 본문을 깨지 않게 한다.
            const std::string hello_body = nlohmann::json{{"token", token_}, {"client", "ops_terminal/0.1"}}.dump();

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_.clear();
                queue_.push_back(ops::encode(ops::OpsMsg::HELLO_REQ, hello_body));
            }

            connected_.store(true);
            post_state(LinkState::Connected, "TCP 연결 — HELLO 전송");
            session_loop();
            connected_.store(false);
            close_socket();

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_.clear(); // 끊긴 연결에 쌓인 송신분은 버린다 — 주문은 사용자가 결과를 보고 다시 낸다
            }
        }

        if (!running_.load())
        {
            break;
        }

        post_state(LinkState::Disconnected, std::to_string(backoff / 1000) + "초 뒤 재접속");

        // backoff 대기. stop()이 깨운다.
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_condition_variable_.wait_for(lock, std::chrono::milliseconds(backoff), [this]
        {
            return !running_.load();
        });
        backoff = (backoff * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff * 2;
    }

    post_state(LinkState::Disconnected, "중지");
}

bool OpsLink::connect_once()
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result     = nullptr;

    if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &result) != 0 || result == nullptr)
    {
        post_state(LinkState::Disconnected, "주소 해석 실패 " + host_);
        return false;
    }

    descriptor_ = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);

    if (descriptor_ == INVALID_SOCKET)
    {
        ::freeaddrinfo(result);
        return false;
    }

    // 연결 자체는 논블로킹 + select로 3초 상한을 둔다 — 서버가 죽어 있을 때 스레드가 오래 묶이지 않게.
    u_long nonblocking = 1;
    ::ioctlsocket(descriptor_, FIONBIO, &nonblocking);
    ::connect(descriptor_, result->ai_addr, static_cast<int>(result->ai_addrlen));
    ::freeaddrinfo(result);

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(descriptor_, &write_set);
    fd_set error_set = write_set;
    timeval time_value{3, 0};

    if (::select(0, nullptr, &write_set, &error_set, &time_value) <= 0 || FD_ISSET(descriptor_, &error_set))
    {
        close_socket();
        return false;
    }

    int error    = 0;
    int errlen = sizeof(error);
    ::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errlen);

    if (error != 0)
    {
        close_socket();
        return false;
    }

    int one = 1;
    ::setsockopt(descriptor_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return true;
}

// 연결 하나의 수명. 소켓 이벤트(network_event_)와 송신 깨움(wake_event_)을 함께 기다린다 — send()가 넣은 프레임은
//  대기 시한을 기다리지 않고 바로 나간다. 대기 시한은 다음 PING이나 무응답 판정까지 남은 시간이다.
void OpsLink::session_loop()
{
    ops::FrameReader     reader;
    std::vector<uint8_t> out;           // 아직 못 보낸 바이트
    auto                 last_rx   = Clock::now();
    auto                 last_ping = Clock::now();
    uint8_t              buffer[kReceiveBufferBytes];

    // FD_WRITE는 송신 버퍼가 찼다가(WSAEWOULDBLOCK) 비었을 때만 다시 온다. 그래서 보낼 것이 있으면 먼저 send를
    //  해 보고, 막혔을 때만 FD_WRITE를 기다린다.
    if (::WSAEventSelect(descriptor_, network_event_, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR)
    {
        post_state(LinkState::Disconnected, "WSAEventSelect 실패 err=" + std::to_string(WSAGetLastError()));
        return;
    }

    const WSAEVENT events[2] = {network_event_, wake_event_};

    while (running_.load())
    {
        // [inv] 큐를 비우기 전에 깨움을 내린다. 그 뒤에 들어온 send()는 이벤트를 다시 세우므로 놓치지 않는다.
        ::WSAResetEvent(wake_event_);

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            for (const auto& queued : queue_)
            {
                out.insert(out.end(), queued.begin(), queued.end());
            }

            queue_.clear();
        }

        if (!send_pending(out))
        {
            return;
        }

        // stop()은 running_을 내린 뒤 이벤트를 세운다. 위에서 그 이벤트를 내렸어도 여기서 running_이 false로 보인다.
        if (!running_.load())
        {
            return;
        }

        const int64_t until_ping = kPingEveryMs - ms_since(last_ping);
        const int64_t until_dead = kDeadAfterMs - ms_since(last_rx);
        const int64_t wait_ms    = std::clamp<int64_t>((std::min)(until_ping, until_dead), 0, kPingEveryMs);
        const DWORD   waited     = ::WSAWaitForMultipleEvents(2, events, FALSE, static_cast<DWORD>(wait_ms), FALSE);

        if (waited == WSA_WAIT_FAILED)
        {
            post_state(LinkState::Disconnected, "이벤트 대기 실패 err=" + std::to_string(WSAGetLastError()));
            return;
        }

        if (!running_.load())
        {
            return;
        }

        WSANETWORKEVENTS network_events{};

        if (::WSAEnumNetworkEvents(descriptor_, network_event_, &network_events) == SOCKET_ERROR)
        {
            post_state(LinkState::Disconnected, "WSAEnumNetworkEvents 실패 err=" + std::to_string(WSAGetLastError()));
            return;
        }

        // FD_CLOSE 때도 남은 바이트를 먼저 읽는다. 다 읽으면 recv가 0을 돌려 끊김으로 처리된다.
        if ((network_events.lNetworkEvents & (FD_READ | FD_CLOSE)) != 0)
        {
            const int received = receive_available(reader, buffer, kReceiveBufferBytes);

            if (received < 0)
            {
                return;
            }

            if (received > 0)
            {
                last_rx = Clock::now();
            }
        }

        // 하트비트. PING은 큐를 거치지 않고 out에 직접 붙인다(connected_ 여부와 무관). 다음 바퀴 첫머리에 나간다.
        if (ms_since(last_ping) >= kPingEveryMs)
        {
            auto ping_frame = ops::encode(ops::OpsMsg::PING_REQ, "{}");
            out.insert(out.end(), ping_frame.begin(), ping_frame.end());
            last_ping = Clock::now();
        }

        if (ms_since(last_rx) >= kDeadAfterMs)
        {
            post_state(LinkState::Disconnected, "30초 무응답 — 끊고 재접속");
            return;
        }
    }
}

// 소켓에 쌓인 바이트를 WSAEWOULDBLOCK까지 읽어 프레임으로 올린다. 읽은 바이트 수, 끊겼으면 -1.
int OpsLink::receive_available(ops::FrameReader& reader, uint8_t* buffer, int capacity)
{
    int total = 0;

    while (true)
    {
        const int received = ::recv(descriptor_, reinterpret_cast<char*>(buffer), capacity, 0);

        if (received == 0)
        {
            post_state(LinkState::Disconnected, "서버가 연결을 닫음");
            return -1;
        }

        if (received < 0)
        {
            const int error = WSAGetLastError();

            if (error == WSAEWOULDBLOCK)
            {
                return total;
            }

            post_state(LinkState::Disconnected, "recv 실패 err=" + std::to_string(error));
            return -1;
        }

        total += received;
        reader.feed(buffer, static_cast<size_t>(received));
        ops::Frame frame;

        while (reader.next(frame))
        {
            if (frame.type == static_cast<uint8_t>(ops::OpsMsg::HELLO_ACK))
            {
                post_state(LinkState::Ready, "HELLO_ACK");
            }

            post_frame(frame);
        }

        if (reader.bad())
        {
            post_state(LinkState::Disconnected, "프레임 규약 위반 — 끊음");
            return -1;
        }
    }
}

// out을 보낼 수 있는 만큼 보낸다. 송신 버퍼가 차면(WSAEWOULDBLOCK) 남기고 돌아간다 — FD_WRITE가 다시 깨운다.
//  false면 끊긴 것.
bool OpsLink::send_pending(std::vector<uint8_t>& out)
{
    while (!out.empty())
    {
        const int sent = ::send(descriptor_, reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), 0);

        if (sent > 0)
        {
            out.erase(out.begin(), out.begin() + sent);
            continue;
        }

        const int error = WSAGetLastError();

        if (error == WSAEWOULDBLOCK)
        {
            return true;
        }

        post_state(LinkState::Disconnected, "send 실패 err=" + std::to_string(error));
        return false;
    }

    return true;
}
