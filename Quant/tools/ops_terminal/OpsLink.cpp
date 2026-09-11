// 운영단말 소켓 작업자 구현. 스레드 소유권은 OpsLink.h 머리 참조. [why D-043]
#include "pch.h"

#include "OpsLink.h"

#include <chrono>

namespace
{

using Clock = std::chrono::steady_clock;

int64_t ms_since(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// 소켓이 없거나 hwnd가 죽었을 때 PostMessage가 실패하면 넘긴 포인터를 우리가 지운다.
template <typename T>
void post_or_delete(HWND h, UINT msg, T* p)
{
    if (h == nullptr || !::PostMessage(h, msg, 0, reinterpret_cast<LPARAM>(p)))
    {
        delete p;
    }
}

} // namespace

OpsLink::OpsLink()
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
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
    th_    = std::thread(&OpsLink::thread_fn, this);
}

void OpsLink::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    wake_.store(true);
    q_cv_.notify_all();

    if (th_.joinable())
    {
        th_.join();
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
        std::lock_guard<std::mutex> lk(q_mtx_);
        q_.push_back(std::move(bytes));
    }

    wake_.store(true);
    q_cv_.notify_one();
    return true;
}

void OpsLink::post_state(LinkState s, const std::string& detail)
{
    post_or_delete(hwnd_, WM_OPS_STATE, new OpsStateMsg{s, detail});
}

void OpsLink::post_frame(const ops::Frame& f)
{
    post_or_delete(hwnd_, WM_OPS_FRAME, new ops::Frame(f));
}

void OpsLink::close_socket()
{
    if (fd_ != INVALID_SOCKET)
    {
        ::shutdown(fd_, SD_BOTH);
        ::closesocket(fd_);
        fd_ = INVALID_SOCKET;
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
                std::lock_guard<std::mutex> lk(q_mtx_);
                q_.clear(); // 끊긴 연결에 쌓인 송신분은 버린다 — 주문은 사용자가 결과를 보고 다시 낸다
            }
        }

        if (!running_.load())
        {
            break;
        }

        post_state(LinkState::Disconnected, std::to_string(backoff / 1000) + "초 뒤 재접속");

        // backoff 대기. stop()이 깨운다.
        std::unique_lock<std::mutex> lk(q_mtx_);
        q_cv_.wait_for(lk, std::chrono::milliseconds(backoff), [this] { return !running_.load(); });
        backoff = (backoff * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff * 2;
    }

    post_state(LinkState::Disconnected, "중지");
}

bool OpsLink::connect_once()
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;

    if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || res == nullptr)
    {
        post_state(LinkState::Disconnected, "주소 해석 실패 " + host_);
        return false;
    }

    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (fd_ == INVALID_SOCKET)
    {
        ::freeaddrinfo(res);
        return false;
    }

    // 연결 자체는 논블로킹 + select로 3초 상한을 둔다 — 서버가 죽어 있을 때 스레드가 오래 묶이지 않게.
    u_long nb = 1;
    ::ioctlsocket(fd_, FIONBIO, &nb);
    ::connect(fd_, res->ai_addr, static_cast<int>(res->ai_addrlen));
    ::freeaddrinfo(res);

    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd_, &w);
    fd_set e = w;
    timeval tv{3, 0};

    if (::select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(fd_, &e))
    {
        close_socket();
        return false;
    }

    int err    = 0;
    int errlen = sizeof(err);
    ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errlen);

    if (err != 0)
    {
        close_socket();
        return false;
    }

    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return true;
}

void OpsLink::session_loop()
{
    ops::FrameReader     reader;
    std::vector<uint8_t> out;           // 아직 못 보낸 바이트
    auto                 last_rx   = Clock::now();
    auto                 last_ping = Clock::now();
    uint8_t              buf[16384];

    while (running_.load())
    {
        // 송신 큐를 out으로 옮긴다
        {
            std::lock_guard<std::mutex> lk(q_mtx_);

            for (auto& b : q_)
            {
                out.insert(out.end(), b.begin(), b.end());
            }

            q_.clear();
        }

        wake_.store(false);

        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd_, &r);
        fd_set w;
        FD_ZERO(&w);

        if (!out.empty())
        {
            FD_SET(fd_, &w);
        }

        // 200ms — stop()·send()가 깨우는 지연 상한. 하트비트 정밀도로도 충분하다.
        timeval tv{0, 200'000};
        const int n = ::select(0, &r, out.empty() ? nullptr : &w, nullptr, &tv);

        if (n < 0)
        {
            post_state(LinkState::Disconnected, "select 실패 err=" + std::to_string(WSAGetLastError()));
            return;
        }

        if (n > 0 && FD_ISSET(fd_, &r))
        {
            const int got = ::recv(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);

            if (got == 0)
            {
                post_state(LinkState::Disconnected, "서버가 연결을 닫음");
                return;
            }

            if (got < 0)
            {
                const int err = WSAGetLastError();

                if (err != WSAEWOULDBLOCK)
                {
                    post_state(LinkState::Disconnected, "recv 실패 err=" + std::to_string(err));
                    return;
                }
            }
            else
            {
                last_rx = Clock::now();
                reader.feed(buf, static_cast<size_t>(got));
                ops::Frame f;

                while (reader.next(f))
                {
                    if (f.type == static_cast<uint8_t>(ops::OpsMsg::WELCOME))
                    {
                        post_state(LinkState::Ready, "WELCOME");
                    }

                    post_frame(f);
                }

                if (reader.bad())
                {
                    post_state(LinkState::Disconnected, "프레임 규약 위반 — 끊음");
                    return;
                }
            }
        }

        if (!out.empty() && (n > 0 && FD_ISSET(fd_, &w)))
        {
            const int sent = ::send(fd_, reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), 0);

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
            auto p = ops::encode(ops::OpsMsg::PING, "{}");
            out.insert(out.end(), p.begin(), p.end());
            last_ping = Clock::now();
        }

        if (ms_since(last_rx) >= kDeadAfterMs)
        {
            post_state(LinkState::Disconnected, "30초 무응답 — 끊고 재접속");
            return;
        }
    }
}
