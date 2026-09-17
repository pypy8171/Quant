// tests/bench_feed_ingest.cpp
// 시세 피드 수신·처리 부하테스트 — 코스콤(KOSCOM)→증권사 시세 흐름을 실제 TCP loopback으로
//  통과시켜, wire 송신 시각에서 주문 결정 시각까지를 네트워크 구간과 처리 구간으로 분해한다.
//  실행 절차의 정본은 `docs/guides/LOAD_TEST_GUIDE.market_data` §2, 결과는
//  `docs/reports/PIPELINE_LATENCY_REPORT.market_data`.
//
// [inv] 측정 범위 — loopback에는 물리 회선(WAN/전용선) 지연이 없다. 재는 것은 "동일 머신 TCP
//   스택 비용 + 소켓 도착 후 주문 결정까지"다. 이 수치를 실 회선 지연으로 옮겨 적으면 안 된다.
//
// 토폴로지:
//   [코스콤 emul] --TCP--> [수신 recv] → ob_q/td_q → strategy → order_q → order
//    (Zipf 팬아웃)          (파싱)         RingBuffer     신호       RingBuffer   주문접수
//
// 지연 분해:
//   net  = recv_ts - send_ts      (송신→커널 TCP→수신 파싱; 동일머신 스택 비용)
//   proc = order_ts - recv_ts     (수신 후 내부 처리단: 큐+전략+주문)
//   e2e  = order_ts - send_ts     (전체)
//
// 사용법:
//   bench_feed_ingest self  [--universe path] [--tickers N] [--rate MSGS/s]
//                           [--duration SEC] [--ob-ratio R] [--zipf S] [--port P]
//   bench_feed_ingest serve [--port P] [--tickers N] [--universe path]
//   bench_feed_ingest send  [--host H] [--port P] [--tickers N] [--universe path]
//                           [--rate MSGS/s] [--duration SEC] [--ob-ratio R] [--zipf S]
//   bench_feed_ingest sweep [--tickers N] [--start R] [--step R] [--max R] [--dwell SEC]
//   기본값: self --tickers 2600 --rate 200000 --duration 20 --ob-ratio 0.7 --zipf 1.0 --port 47001
//
//   serve/send를 두 콘솔로 나눠 돌려도 steady_clock은 QPC 기반(부팅 기준 시스템 전역)이라
//   같은 머신이면 프로세스 간에도 타임스탬프가 정합이다.

#include "core/RingBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_t = SOCKET;
static const socket_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBadSock = -1;
#endif

using clk = std::chrono::steady_clock;

static inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
}

static inline void busy_wait_until_ns(int64_t deadline_ns)
{
    while (now_ns() < deadline_ns)
    {
        /* pure spin — Windows scheduler 회피 */
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Winsock/BSD 소켓 이식 래퍼
// ─────────────────────────────────────────────────────────────────────────────
static void sock_startup()
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

static void sock_cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static void sock_close(socket_t socket_handle)
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

static void set_nodelay(socket_t nodelay)
{
    int one = 1;
    setsockopt(nodelay, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

// 전량 송신(부분 송신 루프). 실패 시 false.
static bool send_all(socket_t socket_handle, const char* cursor, size_t count)
{
    size_t sent = 0;

    while (sent < count)
    {
        int result = ::send(socket_handle, cursor + sent, static_cast<int>(count - sent), 0);

        if (result <= 0)
        {
            return false;
        }

        sent += static_cast<size_t>(result);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Wire 프레임 — 고정 크기 이진 레코드(길이 프리픽스 불필요). 동일 빌드 양단이므로
//   자연 정렬 일관. magic으로 스트림 desync 방지.
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct WireMsg
{
    uint32_t magic;      // 0xC05C0F01 — KOSCOM feed sync
    uint8_t  type;       // 0=OrderBook, 1=TradeData
    char     ticker[7];  // 6자리 코드 + NUL
    int64_t  send_ts_ns; // 코스콤 송신 직전 stamp (전 구간(E2E) 기준점)
    uint64_t sequence;
    double   price[5];      // OB: ask/bid 대표가 / TD: [0]=체결가
    int64_t  quantity[5];     // OB: 잔량 / TD: [0]=체결량
};
#pragma pack(pop)
static constexpr uint32_t kMagic = 0xC05C0F01u;
static constexpr size_t   kRec = sizeof(WireMsg);

// ─────────────────────────────────────────────────────────────────────────────
// 수신 후 내부 파이프라인 메시지 (recv_ts 부착 → 처리단 분해)
// ─────────────────────────────────────────────────────────────────────────────
struct MockOrderBook
{
    char     ticker[8];
    int64_t  send_ts_ns;
    int64_t  recv_ts_ns;
    uint64_t sequence;
    double   ask_price[5];
    int64_t  ask_qty[5];
    double   bid_price[5];
    int64_t  bid_qty[5];
};
struct MockTradeData
{
    char     ticker[8];
    int64_t  send_ts_ns;
    int64_t  recv_ts_ns;
    uint64_t sequence;
    double   price;
    int64_t  quantity;
};
struct MockOrderSignal
{
    char     ticker[8];
    int64_t  send_ts_ns;  // 원본 wire 송신 시각 (E2E)
    int64_t  recv_ts_ns;  // 수신 파싱 시각 (proc 분해)
    int64_t  strat_ts_ns; // strategy push 시각 (strategy→order 분해)
    uint64_t origin_seq;
    int      side;
    int      quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// 티커 로드 (universe_full.json 코드배열 → 없으면 합성 6자리) — firehose와 동일 규약
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> load_universe(const std::string& path, int fallback_n)
{
    std::vector<std::string> out;

    if (!path.empty())
    {
        std::ifstream file(path);

        if (file)
        {
            std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            size_t start = body.find("\"codes\"");
            std::string scan = (start != std::string::npos) ? body.substr(start) : body;

            for (size_t index = 0; index + 1 < scan.size(); ++index)
            {
                if (scan[index] != '"')
                {
                    continue;
                }

                size_t inner_index = index + 1, digits = 0;

                while (inner_index < scan.size() && scan[inner_index] >= '0' && scan[inner_index] <= '9')
                {
                    ++inner_index;
                    ++digits;
                }

                if (digits == 6 && inner_index < scan.size() && scan[inner_index] == '"')
                {
                    out.emplace_back(scan.substr(index + 1, 6));
                    index = inner_index;
                }
            }
        }
    }

    if (out.empty())
    {
        out.reserve(fallback_n);

        for (int fallback_index = 1; fallback_index <= fallback_n; ++fallback_index)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%06d", fallback_index);
            out.emplace_back(buffer);
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zipf 가중 선택기 (rank r 확률 ∝ 1/r^s) — firehose와 동일
// ─────────────────────────────────────────────────────────────────────────────
struct ZipfPicker
{
    std::vector<double> cdf;
    explicit ZipfPicker(size_t count, double sum)
    {
        cdf.resize(count);
        double acc = 0.0;

        for (size_t index = 0; index < count; ++index)
        {
            acc += 1.0 / std::pow(static_cast<double>(index + 1), sum);
            cdf[index] = acc;
        }

        for (auto& cumulative : cdf)
        {
            cumulative /= acc;
        }
    }

    size_t pick(double upper) const
    {
        auto iterator = std::lower_bound(cdf.begin(), cdf.end(), upper);
        size_t index = static_cast<size_t>(iterator - cdf.begin());
        return (index < cdf.size()) ? index : cdf.size() - 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 공유 통계
// ─────────────────────────────────────────────────────────────────────────────
struct Stats
{
    std::atomic<uint64_t> wire_sent{0};    // 코스콤이 소켓에 실제 방출
    std::atomic<uint64_t> wire_recv{0};    // 수신·파싱 완료
    std::atomic<uint64_t> bytes_recv{0};
    std::atomic<uint64_t> ob_consumed{0};
    std::atomic<uint64_t> td_consumed{0};
    std::atomic<uint64_t> signals{0};
    std::atomic<uint64_t> orders{0};
    std::atomic<uint64_t> ob_drops{0};     // 수신측 큐 밀림 처리 드롭
    std::atomic<uint64_t> td_drops{0};
    std::atomic<uint64_t> order_drops{0};
    std::atomic<uint64_t> ob_hwm{0};
    std::atomic<uint64_t> td_hwm{0};
    std::atomic<uint64_t> order_hwm{0};
    std::atomic<uint64_t> bad_magic{0};

    std::vector<int64_t> net_ns;  // recv 스레드가 채움 (recv_ts - send_ts)
    std::vector<int64_t> proc_ns; // order 스레드가 채움 (order_ts - recv_ts)
    std::vector<int64_t> e2e_ns;  // order 스레드가 채움 (order_ts - send_ts)
};

static inline void bump_hwm(std::atomic<uint64_t>& hwm, uint64_t value)
{
    uint64_t cur = hwm.load(std::memory_order_relaxed);

    while (value > cur && !hwm.compare_exchange_weak(cur, value, std::memory_order_relaxed))
    {
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 코스콤 송신기 — 연결된 소켓으로 Zipf 팬아웃을 offered rate로 방출(busy-wait pacing).
//   blocking send_all → 수신측이 못 따라가면 TCP 흐름제어가 send를 지연시킴(밀림 처리).
//   그 결과 achieved rate < offered면 소켓 경로가 천장에 닿은 것.
// ─────────────────────────────────────────────────────────────────────────────
static void koscom_send_fn(socket_t socket_handle,
                           const std::vector<std::string>& tickers,
                           const ZipfPicker& zipf,
                           Stats& stop_token,
                           std::atomic<bool>& stop,
                           int64_t total_rate,
                           double ob_ratio,
                           int64_t duration_ns)
{
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    const int64_t interval_ns = (total_rate > 0) ? static_cast<int64_t>(1e9 / total_rate) : 0;
    const int64_t start_time = now_ns();
    const int64_t t_end = start_time + duration_ns;
    int64_t next_emit = start_time;
    uint64_t sequence = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        if (now_ns() >= t_end)
        {
            break;
        }

        if (interval_ns > 0)
        {
            if (now_ns() < next_emit)
            {
                busy_wait_until_ns(next_emit);
            }

            next_emit += interval_ns;
        }

        const size_t ticker_index = zipf.pick(u01(rng));
        WireMsg wire_msg{};
        wire_msg.magic = kMagic;
        wire_msg.type = (u01(rng) < ob_ratio) ? 0 : 1;
        std::memcpy(wire_msg.ticker, tickers[ticker_index].c_str(), 6);
        wire_msg.ticker[6] = '\0';
        wire_msg.sequence = sequence++;

        if (wire_msg.type == 0)
        {
            for (int index = 0; index < 5; ++index)
            {
                wire_msg.price[index] = 70000.0 + index * 10;
                wire_msg.quantity[index] = 100 * (index + 1);
            }
        }
        else
        {
            wire_msg.price[0] = 70000.0 + (sequence % 100);
            wire_msg.quantity[0] = 10 + (sequence % 50);
        }

        wire_msg.send_ts_ns = now_ns(); // 소켓 진입 직전 stamp

        if (!send_all(socket_handle, reinterpret_cast<const char*>(&wire_msg), kRec))
        {
            break; // 상대 종료
        }

        stop_token.wire_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신 recv 스레드 — 소켓에서 청크로 읽어 고정 프레임으로 파싱, ob_q/td_q로 라우팅.
//   부분 수신(TCP는 스트림)은 leftover 이월로 처리. net 지연은 이 스레드가 수집.
// ─────────────────────────────────────────────────────────────────────────────
static void recv_fn(socket_t socket_handle,
                    RingBuffer<MockOrderBook>& ob_q,
                    RingBuffer<MockTradeData>& td_q,
                    Stats& stop_token,
                    std::atomic<bool>& stop,
                    std::atomic<bool>& measuring,
                    size_t lat_cap)
{
    stop_token.net_ns.reserve(lat_cap);
    const size_t CHUNK = kRec * 512;
    std::vector<char> buffer(CHUNK);
    size_t have = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        int result = ::recv(socket_handle, buffer.data() + have, static_cast<int>(buffer.size() - have), 0);

        if (result <= 0)
        {
            break; // 상대 종료 or 오류
        }

        have += static_cast<size_t>(result);
        stop_token.bytes_recv.fetch_add(static_cast<uint64_t>(result), std::memory_order_relaxed);

        size_t off = 0;

        while (have - off >= kRec)
        {
            WireMsg wire_msg;
            std::memcpy(&wire_msg, buffer.data() + off, kRec);
            off += kRec;
            const int64_t rts = now_ns();

            if (wire_msg.magic != kMagic)
            {
                stop_token.bad_magic.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            stop_token.wire_recv.fetch_add(1, std::memory_order_relaxed);

            if (measuring.load(std::memory_order_relaxed) && stop_token.net_ns.size() < lat_cap)
            {
                stop_token.net_ns.push_back(rts - wire_msg.send_ts_ns);
            }

            if (wire_msg.type == 0)
            {
                MockOrderBook order_book{};
                std::memcpy(order_book.ticker, wire_msg.ticker, 7);
                order_book.send_ts_ns = wire_msg.send_ts_ns;
                order_book.recv_ts_ns = rts;
                order_book.sequence = wire_msg.sequence;

                for (int index = 0; index < 5; ++index)
                {
                    order_book.ask_price[index] = wire_msg.price[index];
                    order_book.ask_qty[index] = wire_msg.quantity[index];
                    order_book.bid_price[index] = wire_msg.price[index] - 20;
                    order_book.bid_qty[index] = wire_msg.quantity[index];
                }

                if (ob_q.push(order_book))
                {
                    bump_hwm(stop_token.ob_hwm, ob_q.size());
                }
                else
                {
                    stop_token.ob_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                MockTradeData trade{};
                std::memcpy(trade.ticker, wire_msg.ticker, 7);
                trade.send_ts_ns = wire_msg.send_ts_ns;
                trade.recv_ts_ns = rts;
                trade.sequence = wire_msg.sequence;
                trade.price = wire_msg.price[0];
                trade.quantity = wire_msg.quantity[0];

                if (td_q.push(trade))
                {
                    bump_hwm(stop_token.td_hwm, td_q.size());
                }
                else
                {
                    stop_token.td_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (off > 0 && off < have)
        {
            std::memmove(buffer.data(), buffer.data() + off, have - off);
        }

        have -= off;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy 스레드 — firehose와 동일(OB 100:1, TD 200:1 신호). recv_ts 이월.
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& ob_q,
                        RingBuffer<MockTradeData>& td_q,
                        RingBuffer<MockOrderSignal>& order_q,
                        Stats& stop_token,
                        std::atomic<bool>& stop)
{
    uint64_t obc = 0, tdc = 0;

    while (!stop.load(std::memory_order_relaxed) || !ob_q.empty() || !td_q.empty())
    {
        while (auto option = ob_q.pop())
        {
            stop_token.ob_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;

            for (int index = 0; index < 5; ++index)
            {
                sink = sink + option->ask_price[index] - option->bid_price[index];
            }

            (void)sink;

            if (++obc % 100 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.recv_ts_ns = option->recv_ts_ns;
                signal.strat_ts_ns = now_ns();
                signal.origin_seq = option->sequence;
                signal.side = 0;
                signal.quantity = 10;

                if (order_q.push(signal))
                {
                    stop_token.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_hwm(stop_token.order_hwm, order_q.size());
                }
                else
                {
                    stop_token.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        while (auto option = td_q.pop())
        {
            stop_token.td_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = option->price * option->quantity;
            (void)sink;

            if (++tdc % 200 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.recv_ts_ns = option->recv_ts_ns;
                signal.strat_ts_ns = now_ns();
                signal.origin_seq = option->sequence;
                signal.side = 1;
                signal.quantity = 5;

                if (order_q.push(signal))
                {
                    stop_token.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_hwm(stop_token.order_hwm, order_q.size());
                }
                else
                {
                    stop_token.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order 스레드 — proc(recv→order)·e2e(send→order) 수집(단독 스레드).
// ─────────────────────────────────────────────────────────────────────────────
static void order_fn(RingBuffer<MockOrderSignal>& order_q,
                     Stats& stop_token,
                     std::atomic<bool>& stop,
                     std::atomic<bool>& measuring,
                     size_t lat_cap)
{
    stop_token.proc_ns.reserve(lat_cap);
    stop_token.e2e_ns.reserve(lat_cap);

    while (!stop.load(std::memory_order_relaxed) || !order_q.empty())
    {
        auto option = order_q.pop();

        if (!option)
        {
            continue;
        }

        const int64_t time_value = now_ns();

        if (measuring.load(std::memory_order_relaxed))
        {
            if (stop_token.proc_ns.size() < lat_cap)
            {
                stop_token.proc_ns.push_back(time_value - option->recv_ts_ns);
            }

            if (stop_token.e2e_ns.size() < lat_cap)
            {
                stop_token.e2e_ns.push_back(time_value - option->send_ts_ns);
            }
        }

        stop_token.orders.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Percentile 유틸
// ─────────────────────────────────────────────────────────────────────────────
struct Pctl
{
    int64_t p50 = 0, p99 = 0, p999 = 0, mx = 0;
    size_t  count = 0;
};
static Pctl percentiles(std::vector<int64_t>& values)
{
    Pctl pctl;
    pctl.count = values.size();

    if (values.empty())
    {
        return pctl;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double price) { return values[static_cast<size_t>(price * (values.size() - 1))]; };
    pctl.p50 = at(0.50);
    pctl.p99 = at(0.99);
    pctl.p999 = at(0.999);
    pctl.mx = values.back();
    return pctl;
}

static std::string fmt_ns(int64_t count)
{
    char byte_value[32];

    if (count < 1000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%lld ns", static_cast<long long>(count));
    }
    else if (count < 1'000'000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f us", count / 1000.0);
    }
    else
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f ms", count / 1'000'000.0);
    }

    return std::string(byte_value);
}

static const char* build_type()
{
#ifdef NDEBUG
    return "Release (NDEBUG)";
#else
    return "Debug (⚠ 측정 무의미 — Release로 재빌드)";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신 서버 소켓 준비: listen → accept 1건.
// ─────────────────────────────────────────────────────────────────────────────
static socket_t make_listener(uint16_t port)
{
    socket_t ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (ls == kBadSock)
    {
        return kBadSock;
    }

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        sock_close(ls);
        return kBadSock;
    }

    if (::listen(ls, 1) != 0)
    {
        sock_close(ls);
        return kBadSock;
    }

    return ls;
}

static socket_t connect_to(const std::string& host, uint16_t port)
{
    socket_t socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (socket_handle == kBadSock)
    {
        return kBadSock;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(socket_handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        sock_close(socket_handle);
        return kBadSock;
    }

    return socket_handle;
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신측 파이프라인 구동(연결된 소켓 하나에 대해) — recv→strategy→order.
//   측정창(measuring) 종료 후 소비분은 드레인이라 percentile에서 제외.
// ─────────────────────────────────────────────────────────────────────────────
struct RunResult
{
    Pctl net, proc, e2e;
    uint64_t wire_sent = 0, wire_recv = 0, bytes_recv = 0;
    uint64_t signals = 0, orders = 0;
    uint64_t drops = 0, bad_magic = 0;
    uint64_t ob_hwm = 0, td_hwm = 0, order_hwm = 0;
    double elapsed_sec = 0;
    double recv_rate = 0;
    bool lossless = false;
};

static void run_receiver_pipeline(socket_t conn, Stats& stop_token, std::atomic<bool>& stop,
                                  std::atomic<bool>& measuring, size_t lat_cap,
                                  size_t ob_cap, size_t td_cap, size_t order_cap,
                                  std::thread& t_recv, std::thread& t_str, std::thread& t_ord,
                                  RingBuffer<MockOrderBook>& ob_q,
                                  RingBuffer<MockTradeData>& td_q,
                                  RingBuffer<MockOrderSignal>& order_q)
{
    (void)ob_cap; (void)td_cap; (void)order_cap;
    set_nodelay(conn);
    t_ord = std::thread(order_fn, std::ref(order_q), std::ref(stop_token), std::ref(stop),
                        std::ref(measuring), lat_cap);
    t_str = std::thread(strategy_fn, std::ref(ob_q), std::ref(td_q), std::ref(order_q),
                        std::ref(stop_token), std::ref(stop));
    t_recv = std::thread(recv_fn, conn, std::ref(ob_q), std::ref(td_q), std::ref(stop_token),
                         std::ref(stop), std::ref(measuring), lat_cap);
}

// self 모드: 한 프로세스에서 서버+코스콤을 loopback으로 연결해 자체시험.
static RunResult run_self(const std::vector<std::string>& tickers, const ZipfPicker& zipf,
                          uint16_t port, int64_t rate, double ob_ratio, int duration_sec,
                          size_t ob_cap, size_t td_cap, size_t order_cap, size_t lat_cap)
{
    RunResult rr;
    socket_t ls = make_listener(port);

    if (ls == kBadSock)
    {
        std::printf("[self] listener bind 실패 (port %u 사용중?)\n", port);
        return rr;
    }

    Stats stop_token;
    std::atomic<bool> stop{false};
    std::atomic<bool> measuring{true};
    RingBuffer<MockOrderBook>   ob_q(ob_cap);
    RingBuffer<MockTradeData>   td_q(td_cap);
    RingBuffer<MockOrderSignal> order_q(order_cap);
    std::thread t_recv, t_str, t_ord;

    // 별도 스레드에서 accept가 걸리는 동안 메인은 connect.
    socket_t conn = kBadSock;
    std::thread t_acc([&] { conn = ::accept(ls, nullptr, nullptr); });

    socket_t cli = kBadSock;

    for (int tries = 0; tries < 200 && cli == kBadSock; ++tries)
    {
        cli = connect_to("127.0.0.1", port);

        if (cli == kBadSock)
        {
            busy_wait_until_ns(now_ns() + 1'000'000); // 1ms 후 재시도
        }
    }

    t_acc.join();

    if (conn == kBadSock || cli == kBadSock)
    {
        std::printf("[self] 연결 수립 실패\n");
        sock_close(ls);
        sock_close(cli);
        sock_close(conn);
        return rr;
    }

    set_nodelay(cli);

    const int64_t start_time = now_ns();
    run_receiver_pipeline(conn, stop_token, stop, measuring, lat_cap, ob_cap, td_cap, order_cap,
                          t_recv, t_str, t_ord, ob_q, td_q, order_q);
    std::thread t_send(koscom_send_fn, cli, std::cref(tickers), std::cref(zipf), std::ref(stop_token),
                       std::ref(stop), rate, ob_ratio, static_cast<int64_t>(duration_sec) * 1'000'000'000LL);

    t_send.join();                                     // 방출 종료
    measuring.store(false, std::memory_order_relaxed); // 이후는 드레인
    // 잔여 in-flight를 소켓으로부터 마저 받도록 잠깐 여유 후 종료.
    busy_wait_until_ns(now_ns() + 50'000'000); // 50ms 드레인
    stop.store(true, std::memory_order_relaxed);
    sock_close(cli); // recv가 0을 받아 빠져나오게

    if (t_recv.joinable())
    {
        t_recv.join();
    }

    if (t_str.joinable())
    {
        t_str.join();
    }

    if (t_ord.joinable())
    {
        t_ord.join();
    }

    sock_close(conn);
    sock_close(ls);

    rr.elapsed_sec = (now_ns() - start_time) / 1e9;
    rr.net = percentiles(stop_token.net_ns);
    rr.proc = percentiles(stop_token.proc_ns);
    rr.e2e = percentiles(stop_token.e2e_ns);
    rr.wire_sent = stop_token.wire_sent.load();
    rr.wire_recv = stop_token.wire_recv.load();
    rr.bytes_recv = stop_token.bytes_recv.load();
    rr.signals = stop_token.signals.load();
    rr.orders = stop_token.orders.load();
    rr.drops = stop_token.ob_drops.load() + stop_token.td_drops.load() + stop_token.order_drops.load();
    rr.bad_magic = stop_token.bad_magic.load();
    rr.ob_hwm = stop_token.ob_hwm.load();
    rr.td_hwm = stop_token.td_hwm.load();
    rr.order_hwm = stop_token.order_hwm.load();
    rr.recv_rate = rr.wire_recv / (rr.elapsed_sec > 0 ? rr.elapsed_sec : 1);
    rr.lossless = (rr.drops == 0) && (rr.bad_magic == 0) && (rr.signals == rr.orders);
    return rr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 인자 파서
// ─────────────────────────────────────────────────────────────────────────────
static std::string arg_str(int argc, char** argv, const char* key, const std::string& def)
{
    for (int index = 2; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], key) == 0)
        {
            return argv[index + 1];
        }
    }

    return def;
}

static int64_t arg_i64(int argc, char** argv, const char* key, int64_t def)
{
    std::string text = arg_str(argc, argv, key, "");
    return text.empty() ? def : std::atoll(text.c_str());
}

static double arg_dbl(int argc, char** argv, const char* key, double def)
{
    std::string text = arg_str(argc, argv, key, "");
    return text.empty() ? def : std::atof(text.c_str());
}

static void print_banner(const char* mode, const std::vector<std::string>& tickers,
                         const std::string& uni_path, double ob_ratio, double zipf_s)
{
    std::printf("=== Feed Ingest Bench (TCP loopback) — mode=%s ===\n", mode);
    std::printf("build           : %s\n", build_type());
    std::printf("wire frame       : %zu bytes/msg\n", kRec);
    std::printf("tickers         : %zu  (source: %s)\n", tickers.size(),
                uni_path.empty() ? "synthetic" : uni_path.c_str());
    std::printf("ob:td ratio     : %.2f : %.2f\n", ob_ratio, 1.0 - ob_ratio);
    std::printf("zipf s          : %.2f  (0=uniform, 1=대형주 편중)\n", zipf_s);
    std::printf("transport       : TCP loopback, TCP_NODELAY on (Nagle off)\n");
    std::printf("NOTE: 커널 TCP 스택(send→recv)·직렬화·프레이밍은 실제 통과. 물리 회선 지연은\n");
    std::printf("      이 머신에 없음(loopback). 실 코스콤 데이터는 KIS WS 소량 병행 실증(별도).\n\n");
}

static void print_run(const RunResult& result, int64_t offered)
{
    std::printf("=== Throughput ===\n");
    std::printf("elapsed          : %.2f sec\n", result.elapsed_sec);
    std::printf("wire sent/recv   : %llu / %llu  (bad_magic %llu)\n",
                static_cast<unsigned long long>(result.wire_sent), static_cast<unsigned long long>(result.wire_recv),
                static_cast<unsigned long long>(result.bad_magic));
    std::printf("bytes recv       : %.1f MB\n", result.bytes_recv / 1e6);
    std::printf("offered/achieved : %lld / %.0f msg/sec\n", static_cast<long long>(offered), result.recv_rate);
    std::printf("signals/orders   : %llu / %llu  (order_q hwm %llu)\n",
                static_cast<unsigned long long>(result.signals), static_cast<unsigned long long>(result.orders),
                static_cast<unsigned long long>(result.order_hwm));
    std::printf("recv-q hwm ob/td : %llu / %llu   drops(밀림 처리): %llu\n\n",
                static_cast<unsigned long long>(result.ob_hwm), static_cast<unsigned long long>(result.td_hwm),
                static_cast<unsigned long long>(result.drops));

    std::printf("=== Latency (wire 송신 → 주문 결정) ===\n");
    auto line = [](const char* lbl, const Pctl& pctl) {
        std::printf("%-28s n=%-9zu p50=%-10s p99=%-10s p999=%-10s max=%s\n", lbl, pctl.count,
                    fmt_ns(pctl.p50).c_str(), fmt_ns(pctl.p99).c_str(), fmt_ns(pctl.p999).c_str(),
                    fmt_ns(pctl.mx).c_str());
    };
    line("net  (send->recv, TCP)", result.net);
    line("proc (recv->order)", result.proc);
    line("e2e  (send->order)", result.e2e);
    std::printf("\n[%s] no drops, signals==orders (수신·처리 무손실 판정)\n",
                result.lossless ? "PASS" : "FAIL");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    sock_startup();

    std::string mode = (argc > 1) ? argv[1] : "self";
    const std::string uni_path = arg_str(argc, argv, "--universe", "");
    const int fallback_n = static_cast<int>(arg_i64(argc, argv, "--tickers", 2600));
    const double zipf_s = arg_dbl(argc, argv, "--zipf", 1.0);
    const double ob_ratio = arg_dbl(argc, argv, "--ob-ratio", 0.7);
    const uint16_t port = static_cast<uint16_t>(arg_i64(argc, argv, "--port", 47001));

    std::vector<std::string> tickers = load_universe(uni_path, fallback_n);
    ZipfPicker zipf(tickers.size(), zipf_s);

    const size_t OB_CAP = 1u << 16;
    const size_t TD_CAP = 1u << 16;
    const size_t ORDER_CAP = 1u << 12;
    const size_t LAT_CAP = 1u << 23;

    int rc = 0;

    if (mode == "self")
    {
        const int64_t rate = arg_i64(argc, argv, "--rate", 200000);
        const int duration = static_cast<int>(arg_i64(argc, argv, "--duration", 20));
        print_banner("self", tickers, uni_path, ob_ratio, zipf_s);
        std::printf("offered rate     : %lld msg/sec\n", static_cast<long long>(rate));
        std::printf("duration         : %d sec\n", duration);
        std::printf("port             : %u\n\n", port);
        RunResult result = run_self(tickers, zipf, port, rate, ob_ratio, duration,
                               OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);
        print_run(result, rate);
        rc = result.lossless ? 0 : 1;
    }
    else if (mode == "sweep")
    {
        const int64_t start = arg_i64(argc, argv, "--start", 100000);
        const int64_t step = arg_i64(argc, argv, "--step", 200000);
        const int64_t maxr = arg_i64(argc, argv, "--max", 3'000'000);
        const int dwell = static_cast<int>(arg_i64(argc, argv, "--dwell", 3));
        print_banner("sweep", tickers, uni_path, ob_ratio, zipf_s);
        std::printf("sweep: offered %lld → %lld step %lld, dwell %ds/step (port %u)\n\n",
                    static_cast<long long>(start), static_cast<long long>(maxr), static_cast<long long>(step), dwell, port);
        std::printf("%-12s %-11s %-10s %-10s %-10s %-8s %-9s\n",
                    "offered/s", "achieved/s", "e2e_p50", "e2e_p99", "e2e_p999", "drops", "lossless");
        std::printf("%s\n", std::string(76, '-').c_str());
        int64_t ceiling = 0;

        for (int64_t rate = start; rate <= maxr; rate += step)
        {
            RunResult result = run_self(tickers, zipf, port, rate, ob_ratio, dwell,
                                   OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);
            std::printf("%-12lld %-11.0f %-10s %-10s %-10s %-8llu %-9s\n",
                        static_cast<long long>(rate), result.recv_rate, fmt_ns(result.e2e.p50).c_str(),
                        fmt_ns(result.e2e.p99).c_str(), fmt_ns(result.e2e.p999).c_str(),
                        static_cast<unsigned long long>(result.drops), result.lossless ? "yes" : "NO");
            std::printf("CSV,%lld,%.0f,%lld,%lld,%lld,%llu,%d\n",
                        static_cast<long long>(rate), result.recv_rate, static_cast<long long>(result.e2e.p50),
                        static_cast<long long>(result.e2e.p99), static_cast<long long>(result.e2e.p999),
                        static_cast<unsigned long long>(result.drops), result.lossless ? 1 : 0);

            if (result.lossless)
            {
                ceiling = rate;
            }
            else
            {
                break;
            }
        }

        std::printf("\n용량 천장 (최대 무손실 offered rate): %lld msg/sec\n", static_cast<long long>(ceiling));
    }
    else if (mode == "serve")
    {
        // 수신 서버만: 다른 콘솔의 send를 기다렸다가 연결 종료 시 통계 출력.
        print_banner("serve", tickers, uni_path, ob_ratio, zipf_s);
        socket_t ls = make_listener(port);

        if (ls == kBadSock)
        {
            std::printf("[serve] bind 실패(port %u)\n", port);
            sock_cleanup();
            return 1;
        }

        std::printf("[serve] port %u 에서 코스콤(send) 대기중...\n", port);
        socket_t conn = ::accept(ls, nullptr, nullptr);

        if (conn == kBadSock)
        {
            std::printf("[serve] accept 실패\n");
            sock_close(ls);
            sock_cleanup();
            return 1;
        }

        std::printf("[serve] 연결 수립. 수신·처리 시작.\n\n");

        Stats stop_token;
        std::atomic<bool> stop{false};
        std::atomic<bool> measuring{true};
        RingBuffer<MockOrderBook>   ob_q(OB_CAP);
        RingBuffer<MockTradeData>   td_q(TD_CAP);
        RingBuffer<MockOrderSignal> order_q(ORDER_CAP);
        std::thread t_recv, t_str, t_ord;
        const int64_t start_time = now_ns();
        run_receiver_pipeline(conn, stop_token, stop, measuring, LAT_CAP, OB_CAP, TD_CAP, ORDER_CAP,
                              t_recv, t_str, t_ord, ob_q, td_q, order_q);

        if (t_recv.joinable())
        {
            t_recv.join();  // 상대가 끊으면 recv가 반환
        }

        measuring.store(false, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);

        if (t_str.joinable())
        {
            t_str.join();
        }

        if (t_ord.joinable())
        {
            t_ord.join();
        }

        sock_close(conn);
        sock_close(ls);

        RunResult result;
        result.elapsed_sec = (now_ns() - start_time) / 1e9;
        result.net = percentiles(stop_token.net_ns);
        result.proc = percentiles(stop_token.proc_ns);
        result.e2e = percentiles(stop_token.e2e_ns);
        result.wire_recv = stop_token.wire_recv.load();
        result.bytes_recv = stop_token.bytes_recv.load();
        result.signals = stop_token.signals.load();
        result.orders = stop_token.orders.load();
        result.drops = stop_token.ob_drops.load() + stop_token.td_drops.load() + stop_token.order_drops.load();
        result.bad_magic = stop_token.bad_magic.load();
        result.ob_hwm = stop_token.ob_hwm.load();
        result.td_hwm = stop_token.td_hwm.load();
        result.order_hwm = stop_token.order_hwm.load();
        result.recv_rate = result.wire_recv / (result.elapsed_sec > 0 ? result.elapsed_sec : 1);
        result.lossless = (result.drops == 0) && (result.bad_magic == 0) && (result.signals == result.orders);
        print_run(result, 0);
        rc = result.lossless ? 0 : 1;
    }
    else if (mode == "send")
    {
        const std::string host = arg_str(argc, argv, "--host", "127.0.0.1");
        const int64_t rate = arg_i64(argc, argv, "--rate", 200000);
        const int duration = static_cast<int>(arg_i64(argc, argv, "--duration", 20));
        print_banner("send", tickers, uni_path, ob_ratio, zipf_s);
        std::printf("[send] %s:%u 로 연결 시도...\n", host.c_str(), port);
        socket_t socket_handle = connect_to(host, port);

        if (socket_handle == kBadSock)
        {
            std::printf("[send] 연결 실패 — serve가 먼저 떠 있어야 함.\n");
            sock_cleanup();
            return 1;
        }

        set_nodelay(socket_handle);
        std::printf("[send] 연결됨. %lld msg/s, %ds 방출.\n", static_cast<long long>(rate), duration);
        Stats stop_token;
        std::atomic<bool> stop{false};
        koscom_send_fn(socket_handle, tickers, zipf, stop_token, stop, rate, ob_ratio,
                       static_cast<int64_t>(duration) * 1'000'000'000LL);
        std::printf("[send] 방출 완료: %llu msg.\n", static_cast<unsigned long long>(stop_token.wire_sent.load()));
        sock_close(socket_handle);
    }
    else
    {
        std::printf("unknown mode '%s' (self|serve|send|sweep)\n", mode.c_str());
        rc = 2;
    }

    sock_cleanup();
    return rc;
}
