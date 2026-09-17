// 운영단말 소켓 작업자 구현. 스레드 소유권은 OpsLink.h 머리 참조. [why D-043]
#include "pch.h"

#include "OpsLink.h"

#include <chrono>

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
}

OpsLink::~OpsLink()
{
    stop();
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

    wake_.store(true);
    queue_condition_variable_.notify_all();

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

    wake_.store(true);
    queue_condition_variable_.notify_one();
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
            connected_.store(true);
            post_state(LinkState::Connected, "TCP 연결 — HELLO 전송");
            send(ops::OpsMsg::HELLO, "{\"token\":\"" + token_ + "\",\"client\":\"ops_terminal/0.1\"}");
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
        queue_condition_variable_.wait_for(lock, std::chrono::milliseconds(backoff), [this] { return !running_.load(); });
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

void OpsLink::session_loop()
{
    ops::FrameReader     reader;
    std::vector<uint8_t> out;           // 아직 못 보낸 바이트
    auto                 last_rx   = Clock::now();
    auto                 last_ping = Clock::now();
    uint8_t              buffer[16384];

    while (running_.load())
    {
        // 송신 큐를 out으로 옮긴다
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            for (auto& queued : queue_)
            {
                out.insert(out.end(), queued.begin(), queued.end());
            }

            queue_.clear();
        }

        wake_.store(false);

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(descriptor_, &read_set);
        fd_set write_set;
        FD_ZERO(&write_set);

        if (!out.empty())
        {
            FD_SET(descriptor_, &write_set);
        }

        // 200ms — stop()·send()가 깨우는 지연 상한. 하트비트 정밀도로도 충분하다.
        timeval time_value{0, 200'000};
        const int count = ::select(0, &read_set, out.empty() ? nullptr : &write_set, nullptr, &time_value);

        if (count < 0)
        {
            post_state(LinkState::Disconnected, "select 실패 err=" + std::to_string(WSAGetLastError()));
            return;
        }

        if (count > 0 && FD_ISSET(descriptor_, &read_set))
        {
            const int received = ::recv(descriptor_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);

            if (received == 0)
            {
                post_state(LinkState::Disconnected, "서버가 연결을 닫음");
                return;
            }

            if (received < 0)
            {
                const int error = WSAGetLastError();

                if (error != WSAEWOULDBLOCK)
                {
                    post_state(LinkState::Disconnected, "recv 실패 err=" + std::to_string(error));
                    return;
                }
            }
            else
            {
                last_rx = Clock::now();
                reader.feed(buffer, static_cast<size_t>(received));
                ops::Frame frame;

                while (reader.next(frame))
                {
                    if (frame.type == static_cast<uint8_t>(ops::OpsMsg::WELCOME))
                    {
                        post_state(LinkState::Ready, "WELCOME");
                    }

                    post_frame(frame);
                }

                if (reader.bad())
                {
                    post_state(LinkState::Disconnected, "프레임 규약 위반 — 끊음");
                    return;
                }
            }
        }

        if (!out.empty() && (count > 0 && FD_ISSET(descriptor_, &write_set)))
        {
            const int sent = ::send(descriptor_, reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), 0);

            if (sent > 0)
            {
                out.erase(out.begin(), out.begin() + sent);
            }
            else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
            {
                post_state(LinkState::Disconnected, "send 실패 err=" + std::to_string(WSAGetLastError()));
                return;
            }
        }

        // 하트비트. PING은 큐를 거치지 않고 out에 직접 붙인다(connected_ 여부와 무관).
        if (ms_since(last_ping) >= kPingEveryMs)
        {
            auto ping_frame = ops::encode(ops::OpsMsg::PING, "{}");
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
