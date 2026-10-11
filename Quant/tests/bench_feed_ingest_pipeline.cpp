// bench_feed_ingest — 코스콤 송신기와 수신 → 전략 → 주문 스레드, self 모드 한 번.
//  공용 준비는 bench_feed_ingest_support.h, 모드별 실행은 bench_feed_ingest.cpp의 main.
#include "bench_feed_ingest_support.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

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
// 코스콤 송신기 — 연결된 소켓으로 Zipf 팬아웃을 offered rate로 방출(busy-wait 간격 조절).
//   blocking send_all → 수신측이 못 따라가면 TCP 흐름제어가 send를 지연시킴(밀림 처리).
//   그 결과 achieved rate < offered면 소켓 경로가 천장에 닿은 것.
// ─────────────────────────────────────────────────────────────────────────────
void koscom_send_fn(socket_t socket_handle,
                    const std::vector<std::string>& tickers,
                    const ZipfPicker& zipf,
                    Stats& statistics,
                    std::atomic<bool>& stop,
                    int64_t total_rate,
                    double order_book_ratio,
                    int64_t duration_ns)
{
    std::mt19937_64 random_engine(0xC0FFEE);
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);
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

        const size_t ticker_index = zipf.pick(uniform01(random_engine));
        WireMsg wire_message{};
        wire_message.magic = kMagic;
        wire_message.type = (uniform01(random_engine) < order_book_ratio) ? 0 : 1;
        std::memcpy(wire_message.ticker, tickers[ticker_index].c_str(), 6);
        wire_message.ticker[6] = '\0';
        wire_message.sequence = sequence++;

        if (wire_message.type == 0)
        {
            for (int index = 0; index < 5; ++index)
            {
                wire_message.price[index] = 70000.0 + index * 10;
                wire_message.quantity[index] = 100 * (index + 1);
            }
        }
        else
        {
            wire_message.price[0] = 70000.0 + (sequence % 100);
            wire_message.quantity[0] = 10 + (sequence % 50);
        }

        wire_message.send_ts_ns = now_ns(); // 소켓 진입 직전 stamp

        if (!send_all(socket_handle, reinterpret_cast<const char*>(&wire_message), kRecordBytes))
        {
            break; // 상대 종료
        }

        statistics.wire_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신 recv 스레드 — 소켓에서 청크로 읽어 고정 프레임으로 파싱, order_book_queue/td_q로 라우팅.
//   부분 수신(TCP는 스트림)은 leftover 이월로 처리. net 지연은 이 스레드가 수집.
// ─────────────────────────────────────────────────────────────────────────────
static void recv_fn(socket_t socket_handle,
                    RingBuffer<MockOrderBook>& order_book_queue,
                    RingBuffer<MockTradeData>& trade_queue,
                    Stats& statistics,
                    std::atomic<bool>& stop,
                    std::atomic<bool>& measuring,
                    size_t latency_cap)
{
    statistics.net_ns.reserve(latency_cap);
    const size_t kChunkBytes = kRecordBytes * 512;
    std::vector<char> buffer(kChunkBytes);
    size_t received_count = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        int result = ::recv(socket_handle, buffer.data() + received_count, static_cast<int>(buffer.size() - received_count), 0);

        if (result <= 0)
        {
            break; // 상대 종료 or 오류
        }

        received_count += static_cast<size_t>(result);
        statistics.bytes_recv.fetch_add(static_cast<uint64_t>(result), std::memory_order_relaxed);

        size_t offset = 0;

        while (received_count - offset >= kRecordBytes)
        {
            WireMsg wire_message;
            std::memcpy(&wire_message, buffer.data() + offset, kRecordBytes);
            offset += kRecordBytes;
            const int64_t run_start_ns = now_ns();

            if (wire_message.magic != kMagic)
            {
                statistics.bad_magic.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            statistics.wire_recv.fetch_add(1, std::memory_order_relaxed);

            if (measuring.load(std::memory_order_relaxed) && statistics.net_ns.size() < latency_cap)
            {
                statistics.net_ns.push_back(run_start_ns - wire_message.send_ts_ns);
            }

            if (wire_message.type == 0)
            {
                MockOrderBook order_book{};
                std::memcpy(order_book.ticker, wire_message.ticker, 7);
                order_book.send_ts_ns = wire_message.send_ts_ns;
                order_book.recv_ts_ns = run_start_ns;
                order_book.sequence = wire_message.sequence;

                for (int index = 0; index < 5; ++index)
                {
                    order_book.ask_price[index] = wire_message.price[index];
                    order_book.ask_quantity[index] = wire_message.quantity[index];
                    order_book.bid_price[index] = wire_message.price[index] - 20;
                    order_book.bid_quantity[index] = wire_message.quantity[index];
                }

                if (order_book_queue.push(order_book))
                {
                    bump_high_water_mark(statistics.order_book_high_water_mark, order_book_queue.size());
                }
                else
                {
                    statistics.order_book_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                MockTradeData trade{};
                std::memcpy(trade.ticker, wire_message.ticker, 7);
                trade.send_ts_ns = wire_message.send_ts_ns;
                trade.recv_ts_ns = run_start_ns;
                trade.sequence = wire_message.sequence;
                trade.price = wire_message.price[0];
                trade.quantity = wire_message.quantity[0];

                if (trade_queue.push(trade))
                {
                    bump_high_water_mark(statistics.trade_high_water_mark, trade_queue.size());
                }
                else
                {
                    statistics.trade_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (offset > 0 && offset < received_count)
        {
            std::memmove(buffer.data(), buffer.data() + offset, received_count - offset);
        }

        received_count -= offset;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy 스레드 — firehose와 동일(OB 100:1, TD 200:1 신호). recv_ts 이월.
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& order_book_queue,
                        RingBuffer<MockTradeData>& trade_queue,
                        RingBuffer<MockOrderSignal>& order_queue,
                        Stats& statistics,
                        std::atomic<bool>& stop)
{
    uint64_t order_book_count = 0, trade_count = 0;

    while (!stop.load(std::memory_order_relaxed) || !order_book_queue.empty() || !trade_queue.empty())
    {
        while (auto option = order_book_queue.pop())
        {
            statistics.order_book_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;

            for (int index = 0; index < 5; ++index)
            {
                sink = sink + option->ask_price[index] - option->bid_price[index];
            }

            (void)sink;

            if (++order_book_count % 100 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.recv_ts_ns = option->recv_ts_ns;
                signal.strategy_ts_ns = now_ns();
                signal.origin_sequence = option->sequence;
                signal.side = 0;
                signal.quantity = 10;

                if (order_queue.push(signal))
                {
                    statistics.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_high_water_mark(statistics.order_high_water_mark, order_queue.size());
                }
                else
                {
                    statistics.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        while (auto option = trade_queue.pop())
        {
            statistics.trade_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = option->price * option->quantity;
            (void)sink;

            if (++trade_count % 200 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.recv_ts_ns = option->recv_ts_ns;
                signal.strategy_ts_ns = now_ns();
                signal.origin_sequence = option->sequence;
                signal.side = 1;
                signal.quantity = 5;

                if (order_queue.push(signal))
                {
                    statistics.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_high_water_mark(statistics.order_high_water_mark, order_queue.size());
                }
                else
                {
                    statistics.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order 스레드 — proc(recv→order)·e2e(send→order) 수집(단독 스레드).
// ─────────────────────────────────────────────────────────────────────────────
static void order_fn(RingBuffer<MockOrderSignal>& order_queue,
                     Stats& statistics,
                     std::atomic<bool>& stop,
                     std::atomic<bool>& measuring,
                     size_t latency_cap)
{
    statistics.proc_ns.reserve(latency_cap);
    statistics.e2e_ns.reserve(latency_cap);

    while (!stop.load(std::memory_order_relaxed) || !order_queue.empty())
    {
        auto option = order_queue.pop();

        if (!option)
        {
            continue;
        }

        const int64_t time_value = now_ns();

        if (measuring.load(std::memory_order_relaxed))
        {
            if (statistics.proc_ns.size() < latency_cap)
            {
                statistics.proc_ns.push_back(time_value - option->recv_ts_ns);
            }

            if (statistics.e2e_ns.size() < latency_cap)
            {
                statistics.e2e_ns.push_back(time_value - option->send_ts_ns);
            }
        }

        statistics.orders.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 수신측 파이프라인 구동(연결된 소켓 하나에 대해) — recv→strategy→order.
//   측정창(measuring) 종료 후 소비분은 드레인이라 percentile에서 제외.
// ─────────────────────────────────────────────────────────────────────────────
void run_receiver_pipeline(socket_t connection, Stats& statistics, std::atomic<bool>& stop,
                           std::atomic<bool>& measuring, size_t latency_cap,
                           size_t order_book_capacity, size_t trade_capacity, size_t order_capacity,
                           std::thread& receive_thread, std::thread& strategy_thread, std::thread& order_thread,
                           RingBuffer<MockOrderBook>& order_book_queue,
                           RingBuffer<MockTradeData>& trade_queue,
                           RingBuffer<MockOrderSignal>& order_queue)
{
    (void)order_book_capacity; (void)trade_capacity; (void)order_capacity;
    set_nodelay(connection);
    order_thread = std::thread(order_fn, std::ref(order_queue), std::ref(statistics), std::ref(stop),
                        std::ref(measuring), latency_cap);
    strategy_thread = std::thread(strategy_fn, std::ref(order_book_queue), std::ref(trade_queue), std::ref(order_queue),
                        std::ref(statistics), std::ref(stop));
    receive_thread = std::thread(recv_fn, connection, std::ref(order_book_queue), std::ref(trade_queue), std::ref(statistics),
                         std::ref(stop), std::ref(measuring), latency_cap);
}

// self 모드: 한 프로세스에서 서버+코스콤을 loopback으로 연결해 자체시험.
RunResult run_self(const std::vector<std::string>& tickers, const ZipfPicker& zipf,
                   uint16_t port, int64_t rate, double order_book_ratio, int duration_sec,
                   size_t order_book_capacity, size_t trade_capacity, size_t order_capacity, size_t latency_cap)
{
    RunResult run_result;
    socket_t listen_socket = make_listener(port);

    if (listen_socket == kBadSock)
    {
        std::printf("[self] listener bind 실패 (port %u 사용중?)\n", port);
        return run_result;
    }

    Stats statistics;
    std::atomic<bool> stop{false};
    std::atomic<bool> measuring{true};
    RingBuffer<MockOrderBook>   order_book_queue(order_book_capacity);
    RingBuffer<MockTradeData>   trade_queue(trade_capacity);
    RingBuffer<MockOrderSignal> order_queue(order_capacity);
    std::thread receive_thread, strategy_thread, order_thread;

    // 별도 스레드에서 accept가 걸리는 동안 메인은 connect.
    socket_t connection = kBadSock;
    std::thread accept_thread([&]
    {
        connection = ::accept(listen_socket, nullptr, nullptr);
    });

    socket_t client = kBadSock;

    for (int tries = 0; tries < 200 && client == kBadSock; ++tries)
    {
        client = connect_to("127.0.0.1", port);

        if (client == kBadSock)
        {
            busy_wait_until_ns(now_ns() + 1'000'000); // 1ms 후 재시도
        }
    }

    accept_thread.join();

    if (connection == kBadSock || client == kBadSock)
    {
        std::printf("[self] 연결 수립 실패\n");
        socket_close(listen_socket);
        socket_close(client);
        socket_close(connection);
        return run_result;
    }

    set_nodelay(client);

    const int64_t start_time = now_ns();
    run_receiver_pipeline(connection, statistics, stop, measuring, latency_cap, order_book_capacity, trade_capacity, order_capacity,
                          receive_thread, strategy_thread, order_thread, order_book_queue, trade_queue, order_queue);
    std::thread send_thread(koscom_send_fn, client, std::cref(tickers), std::cref(zipf), std::ref(statistics),
                       std::ref(stop), rate, order_book_ratio, static_cast<int64_t>(duration_sec) * 1'000'000'000LL);

    send_thread.join();                                     // 방출 종료
    measuring.store(false, std::memory_order_relaxed); // 이후는 드레인
    // 잔여 in-flight를 소켓으로부터 마저 받도록 잠깐 여유 후 종료.
    busy_wait_until_ns(now_ns() + 50'000'000); // 50ms 드레인
    stop.store(true, std::memory_order_relaxed);
    socket_close(client); // recv가 0을 받아 빠져나오게

    if (receive_thread.joinable())
    {
        receive_thread.join();
    }

    if (strategy_thread.joinable())
    {
        strategy_thread.join();
    }

    if (order_thread.joinable())
    {
        order_thread.join();
    }

    socket_close(connection);
    socket_close(listen_socket);

    run_result.elapsed_sec = (now_ns() - start_time) / 1e9;
    run_result.net = percentiles(statistics.net_ns);
    run_result.proc = percentiles(statistics.proc_ns);
    run_result.e2e = percentiles(statistics.e2e_ns);
    run_result.wire_sent = statistics.wire_sent.load();
    run_result.wire_recv = statistics.wire_recv.load();
    run_result.bytes_recv = statistics.bytes_recv.load();
    run_result.signals = statistics.signals.load();
    run_result.orders = statistics.orders.load();
    run_result.drops = statistics.order_book_drops.load() + statistics.trade_drops.load() + statistics.order_drops.load();
    run_result.bad_magic = statistics.bad_magic.load();
    run_result.order_book_high_water_mark = statistics.order_book_high_water_mark.load();
    run_result.trade_high_water_mark = statistics.trade_high_water_mark.load();
    run_result.order_high_water_mark = statistics.order_high_water_mark.load();
    run_result.recv_rate = run_result.wire_recv / (run_result.elapsed_sec > 0 ? run_result.elapsed_sec : 1);
    run_result.lossless = (run_result.drops == 0) && (run_result.bad_magic == 0) && (run_result.signals == run_result.orders);
    return run_result;
}
