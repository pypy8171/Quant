#pragma once
// bench_feed_ingest 공용 준비 — 소켓 이식 정의·시계·전문 프레임·내부 메시지·Zipf 선택기·통계와 함수 선언.
//  본문은 bench_feed_ingest_<주제>.cpp에 있고, bench_feed_ingest.cpp의 main이 모드별로 부른다.

#include "bench_common.h"
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

using bench::argument_double;
using bench::argument_int64;
using bench::argument_string;
using bench::build_type;
using bench::format_ns;
using bench::percentiles;
using bench::PercentileSummary;
using steady_clock = std::chrono::steady_clock;

// now_ns·busy_wait_until_ns·bump_high_water_mark는 측정 루프 안에서 부른다. 다른 번역 단위로 옮기면
//  호출 비용이 수치에 섞이므로 헤더에 inline으로 두어 원래처럼 호출 자리에 펼쳐지게 한다.
inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void busy_wait_until_ns(int64_t deadline_ns)
{
    while (now_ns() < deadline_ns)
    {
        /* pure spin — Windows scheduler 회피 */
    }
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
    int64_t  ask_quantity[5];
    double   bid_price[5];
    int64_t  bid_quantity[5];
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
    int64_t  strategy_ts_ns; // strategy push 시각 (strategy→order 분해)
    uint64_t origin_sequence;
    int      side;
    int      quantity;
};

// pick은 송신 측정 루프 안에서 부르므로 몸통을 구조체 안에 원래대로 둔다.
// ─────────────────────────────────────────────────────────────────────────────
// Zipf 가중 선택기 (rank r 확률 ∝ 1/r^s) — firehose와 동일
// ─────────────────────────────────────────────────────────────────────────────
struct ZipfPicker
{
    std::vector<double> cumulative_distribution;
    explicit ZipfPicker(size_t count, double sum)
    {
        cumulative_distribution.resize(count);
        double accumulator = 0.0;

        for (size_t index = 0; index < count; ++index)
        {
            accumulator += 1.0 / std::pow(static_cast<double>(index + 1), sum);
            cumulative_distribution[index] = accumulator;
        }

        for (auto& cumulative : cumulative_distribution)
        {
            cumulative /= accumulator;
        }
    }

    size_t pick(double upper) const
    {
        auto iterator = std::lower_bound(cumulative_distribution.begin(), cumulative_distribution.end(), upper);
        size_t index = static_cast<size_t>(iterator - cumulative_distribution.begin());
        return (index < cumulative_distribution.size()) ? index : cumulative_distribution.size() - 1;
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
    std::atomic<uint64_t> order_book_consumed{0};
    std::atomic<uint64_t> trade_consumed{0};
    std::atomic<uint64_t> signals{0};
    std::atomic<uint64_t> orders{0};
    std::atomic<uint64_t> order_book_drops{0};     // 수신측 큐 밀림 처리 드롭
    std::atomic<uint64_t> trade_drops{0};
    std::atomic<uint64_t> order_drops{0};
    std::atomic<uint64_t> order_book_high_water_mark{0};
    std::atomic<uint64_t> trade_high_water_mark{0};
    std::atomic<uint64_t> order_high_water_mark{0};
    std::atomic<uint64_t> bad_magic{0};

    std::vector<int64_t> net_ns;  // recv 스레드가 채움 (recv_ts - send_ts)
    std::vector<int64_t> proc_ns; // order 스레드가 채움 (order_ts - recv_ts)
    std::vector<int64_t> e2e_ns;  // order 스레드가 채움 (order_ts - send_ts)
};

inline void bump_high_water_mark(std::atomic<uint64_t>& high_water_mark, uint64_t value)
{
    uint64_t current = high_water_mark.load(std::memory_order_relaxed);

    while (value > current && !high_water_mark.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

struct RunResult
{
    PercentileSummary net, proc, e2e;
    uint64_t wire_sent = 0, wire_recv = 0, bytes_recv = 0;
    uint64_t signals = 0, orders = 0;
    uint64_t drops = 0, bad_magic = 0;
    uint64_t order_book_high_water_mark = 0, trade_high_water_mark = 0, order_high_water_mark = 0;
    double elapsed_sec = 0;
    double recv_rate = 0;
    bool lossless = false;
};

// ─── 함수 선언 — 정의가 있는 파일 이름을 묶음 위에 적었다 ───
// bench_feed_ingest_support.cpp
std::vector<std::string> load_universe(const std::string& path, int fallback_n);
// bench_feed_ingest_socket.cpp
void socket_startup();
void socket_cleanup();
void socket_close(socket_t socket_handle);
void set_nodelay(socket_t nodelay);
socket_t make_listener(uint16_t port);
socket_t connect_to(const std::string& host, uint16_t port);
// bench_feed_ingest_pipeline.cpp
void koscom_send_fn(socket_t socket_handle,
                    const std::vector<std::string>& tickers,
                    const ZipfPicker& zipf,
                    Stats& stop_token,
                    std::atomic<bool>& stop,
                    int64_t total_rate,
                    double order_book_ratio,
                    int64_t duration_ns);
void run_receiver_pipeline(socket_t connection, Stats& stop_token, std::atomic<bool>& stop,
                           std::atomic<bool>& measuring, size_t latency_cap,
                           size_t order_book_capacity, size_t trade_capacity, size_t order_capacity,
                           std::thread& receive_thread, std::thread& strategy_thread, std::thread& order_thread,
                           RingBuffer<MockOrderBook>& order_book_queue,
                           RingBuffer<MockTradeData>& trade_queue,
                           RingBuffer<MockOrderSignal>& order_queue);
RunResult run_self(const std::vector<std::string>& tickers, const ZipfPicker& zipf,
                   uint16_t port, int64_t rate, double order_book_ratio, int duration_sec,
                   size_t order_book_capacity, size_t trade_capacity, size_t order_capacity, size_t latency_cap);
