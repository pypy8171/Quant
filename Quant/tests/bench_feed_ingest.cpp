// tests/bench_feed_ingest.cpp
// 시세 피드 수신·처리 부하테스트 — 코스콤(KOSCOM)→증권사 시세 흐름을 실제 TCP loopback으로
//  통과시켜, wire 송신 시각에서 주문 결정 시각까지를 네트워크 구간과 처리 구간으로 분해한다.
//  실행 절차의 정본은 `docs/guides/LOAD_TEST_GUIDE.md` §2, 결과는
//  `docs/reports/PIPELINE_LATENCY_REPORT.md`.
//
// [inv] 측정 범위 — loopback에는 물리 회선(WAN/전용선) 지연이 없다. 재는 것은 "동일 머신 TCP
//   스택 비용 + 소켓 도착 후 주문 결정까지"다. 이 수치를 실 회선 지연으로 옮겨 적으면 안 된다.
//
// 토폴로지:
//   [코스콤 emul] --TCP--> [수신 recv] → order_book_queue/trade_queue → strategy → order_queue → order
//    (Zipf 팬아웃)          (파싱)         RingBuffer     신호       RingBuffer   주문접수
//
// 지연 분해:
//   net  = recv_ts - send_ts      (송신→커널 TCP→수신 파싱; 동일머신 스택 비용)
//   proc = order_ts - recv_ts     (수신 후 내부 처리단: 큐+전략+주문)
//   e2e  = order_ts - send_ts     (전체)
//
// 사용법:
//   bench_feed_ingest self  [--universe path] [--tickers N] [--rate MSGS/s]
//                           [--duration SEC] [--order_book-ratio R] [--zipf S] [--port P]
//   bench_feed_ingest serve [--port P] [--tickers N] [--universe path]
//   bench_feed_ingest send  [--host H] [--port P] [--tickers N] [--universe path]
//                           [--rate MSGS/s] [--duration SEC] [--order_book-ratio R] [--zipf S]
//   bench_feed_ingest sweep [--tickers N] [--start R] [--step R] [--max R] [--dwell SEC]
//   기본값: self --tickers 2600 --rate 200000 --duration 20 --order_book-ratio 0.7 --zipf 1.0 --port 47001
//
//   serve/send를 두 콘솔로 나눠 돌려도 steady_clock은 QPC 기반(부팅 기준 시스템 전역)이라
//   같은 머신이면 프로세스 간에도 타임스탬프가 정합이다.
//
// 소켓 래퍼는 bench_feed_ingest_socket.cpp, 송수신 스레드는 bench_feed_ingest_pipeline.cpp, 공용 정의는 bench_feed_ingest_support.h/.cpp에 있다.

#include "bench_feed_ingest_support.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static void print_banner(const char* mode, const std::vector<std::string>& tickers,
                         const std::string& universe_path, double order_book_ratio, double zipf_skew)
{
    std::printf("=== Feed Ingest Bench (TCP loopback) — mode=%s ===\n", mode);
    std::printf("build           : %s\n", build_type());
    std::printf("wire frame       : %zu bytes/msg\n", kRecordBytes);
    std::printf("tickers         : %zu  (source: %s)\n", tickers.size(),
                universe_path.empty() ? "synthetic" : universe_path.c_str());
    std::printf("ob:td ratio     : %.2f : %.2f\n", order_book_ratio, 1.0 - order_book_ratio);
    std::printf("zipf s          : %.2f  (0=uniform, 1=대형주 편중)\n", zipf_skew);
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
                static_cast<unsigned long long>(result.order_high_water_mark));
    std::printf("recv-q hwm ob/td : %llu / %llu   drops(밀림 처리): %llu\n\n",
                static_cast<unsigned long long>(result.order_book_high_water_mark), static_cast<unsigned long long>(result.trade_high_water_mark),
                static_cast<unsigned long long>(result.drops));

    std::printf("=== Latency (wire 송신 → 주문 결정) ===\n");
    auto line = [](const char* label, const PercentileSummary& percentiles) {
        std::printf("%-28s n=%-9zu p50=%-10s p99=%-10s p999=%-10s max=%s\n", label, percentiles.count,
                    format_ns(percentiles.p50).c_str(), format_ns(percentiles.p99).c_str(), format_ns(percentiles.p999).c_str(),
                    format_ns(percentiles.max_value).c_str());
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
    socket_startup();

    std::string mode = (argc > 1) ? argv[1] : "self";
    const std::string universe_path = argument_string(argc, argv, "--universe", "");
    const int fallback_count = static_cast<int>(argument_int64(argc, argv, "--tickers", 2600));
    const double zipf_skew = argument_double(argc, argv, "--zipf", 1.0);
    const double order_book_ratio = argument_double(argc, argv, "--ob-ratio", 0.7);
    const uint16_t port = static_cast<uint16_t>(argument_int64(argc, argv, "--port", 47001));

    std::vector<std::string> tickers = load_universe(universe_path, fallback_count);
    ZipfPicker zipf(tickers.size(), zipf_skew);

    constexpr size_t kOrderBookCapacity = 1u << 16;
    constexpr size_t kTradeCapacity = 1u << 16;
    constexpr size_t kOrderCapacity = 1u << 12;
    constexpr size_t kLatencyCapacity = 1u << 23;

    int result_code = 0;

    if (mode == "self")
    {
        const int64_t rate = argument_int64(argc, argv, "--rate", 200000);
        const int duration = static_cast<int>(argument_int64(argc, argv, "--duration", 20));
        print_banner("self", tickers, universe_path, order_book_ratio, zipf_skew);
        std::printf("offered rate     : %lld msg/sec\n", static_cast<long long>(rate));
        std::printf("duration         : %d sec\n", duration);
        std::printf("port             : %u\n\n", port);
        RunResult result = run_self(tickers, zipf, port, rate, order_book_ratio, duration,
                               kOrderBookCapacity, kTradeCapacity, kOrderCapacity, kLatencyCapacity);
        print_run(result, rate);
        result_code = result.lossless ? 0 : 1;
    }
    else if (mode == "sweep")
    {
        const int64_t start = argument_int64(argc, argv, "--start", 100000);
        const int64_t step = argument_int64(argc, argv, "--step", 200000);
        const int64_t max_rate_per_sec = argument_int64(argc, argv, "--max", 3'000'000);
        const int dwell = static_cast<int>(argument_int64(argc, argv, "--dwell", 3));
        print_banner("sweep", tickers, universe_path, order_book_ratio, zipf_skew);
        std::printf("sweep: offered %lld → %lld step %lld, dwell %ds/step (port %u)\n\n",
                    static_cast<long long>(start), static_cast<long long>(max_rate_per_sec), static_cast<long long>(step), dwell, port);
        std::printf("%-12s %-11s %-10s %-10s %-10s %-8s %-9s\n",
                    "offered/s", "achieved/s", "e2e_p50", "e2e_p99", "e2e_p999", "drops", "lossless");
        std::printf("%s\n", std::string(76, '-').c_str());
        int64_t ceiling = 0;

        for (int64_t rate = start; rate <= max_rate_per_sec; rate += step)
        {
            RunResult result = run_self(tickers, zipf, port, rate, order_book_ratio, dwell,
                                   kOrderBookCapacity, kTradeCapacity, kOrderCapacity, kLatencyCapacity);
            std::printf("%-12lld %-11.0f %-10s %-10s %-10s %-8llu %-9s\n",
                        static_cast<long long>(rate), result.recv_rate, format_ns(result.e2e.p50).c_str(),
                        format_ns(result.e2e.p99).c_str(), format_ns(result.e2e.p999).c_str(),
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
        print_banner("serve", tickers, universe_path, order_book_ratio, zipf_skew);
        socket_t listen_socket = make_listener(port);

        if (listen_socket == kBadSock)
        {
            std::printf("[serve] bind 실패(port %u)\n", port);
            socket_cleanup();
            return 1;
        }

        std::printf("[serve] port %u 에서 코스콤(send) 대기중...\n", port);
        socket_t connection = ::accept(listen_socket, nullptr, nullptr);

        if (connection == kBadSock)
        {
            std::printf("[serve] accept 실패\n");
            socket_close(listen_socket);
            socket_cleanup();
            return 1;
        }

        std::printf("[serve] 연결 수립. 수신·처리 시작.\n\n");

        Stats statistics;
        std::atomic<bool> stop{false};
        std::atomic<bool> measuring{true};
        RingBuffer<MockOrderBook>   order_book_queue(kOrderBookCapacity);
        RingBuffer<MockTradeData>   trade_queue(kTradeCapacity);
        RingBuffer<MockOrderSignal> order_queue(kOrderCapacity);
        std::thread receive_thread, strategy_thread, order_thread;
        const int64_t start_time = now_ns();
        run_receiver_pipeline(connection, statistics, stop, measuring, kLatencyCapacity, kOrderBookCapacity, kTradeCapacity, kOrderCapacity,
                              receive_thread, strategy_thread, order_thread, order_book_queue, trade_queue, order_queue);

        if (receive_thread.joinable())
        {
            receive_thread.join();  // 상대가 끊으면 recv가 반환
        }

        measuring.store(false, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);

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

        RunResult result;
        result.elapsed_sec = (now_ns() - start_time) / 1e9;
        result.net = percentiles(statistics.net_ns);
        result.proc = percentiles(statistics.proc_ns);
        result.e2e = percentiles(statistics.e2e_ns);
        result.wire_recv = statistics.wire_recv.load();
        result.bytes_recv = statistics.bytes_recv.load();
        result.signals = statistics.signals.load();
        result.orders = statistics.orders.load();
        result.drops = statistics.order_book_drops.load() + statistics.trade_drops.load() + statistics.order_drops.load();
        result.bad_magic = statistics.bad_magic.load();
        result.order_book_high_water_mark = statistics.order_book_high_water_mark.load();
        result.trade_high_water_mark = statistics.trade_high_water_mark.load();
        result.order_high_water_mark = statistics.order_high_water_mark.load();
        result.recv_rate = result.wire_recv / (result.elapsed_sec > 0 ? result.elapsed_sec : 1);
        result.lossless = (result.drops == 0) && (result.bad_magic == 0) && (result.signals == result.orders);
        print_run(result, 0);
        result_code = result.lossless ? 0 : 1;
    }
    else if (mode == "send")
    {
        const std::string host = argument_string(argc, argv, "--host", "127.0.0.1");
        const int64_t rate = argument_int64(argc, argv, "--rate", 200000);
        const int duration = static_cast<int>(argument_int64(argc, argv, "--duration", 20));
        print_banner("send", tickers, universe_path, order_book_ratio, zipf_skew);
        std::printf("[send] %s:%u 로 연결 시도...\n", host.c_str(), port);
        socket_t socket_handle = connect_to(host, port);

        if (socket_handle == kBadSock)
        {
            std::printf("[send] 연결 실패 — serve가 먼저 떠 있어야 함.\n");
            socket_cleanup();
            return 1;
        }

        set_nodelay(socket_handle);
        std::printf("[send] 연결됨. %lld msg/s, %ds 방출.\n", static_cast<long long>(rate), duration);
        Stats statistics;
        std::atomic<bool> stop{false};
        koscom_send_fn(socket_handle, tickers, zipf, statistics, stop, rate, order_book_ratio,
                       static_cast<int64_t>(duration) * 1'000'000'000LL);
        std::printf("[send] 방출 완료: %llu msg.\n", static_cast<unsigned long long>(statistics.wire_sent.load()));
        socket_close(socket_handle);
    }
    else
    {
        std::printf("unknown mode '%s' (self|serve|send|sweep)\n", mode.c_str());
        result_code = 2;
    }

    socket_cleanup();
    return result_code;
}
