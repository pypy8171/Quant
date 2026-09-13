// ShardMatrix 단위 테스트 — 종목 해시 샤딩의 균형, 셀 가득 참, 라운드로빈, N 생산자 × M 소비자에서 종목 안 순서 보존,
//  1×1·2×2·4×4 처리량 측정(원칙 7), 기동 전 reshape.
// 빌드: cmake --build <dir> --target test_shard_matrix
#include "core/ShardMatrix.h"
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 시험용 항목 — 종목 id와 그 종목 안 순번.
struct Item
{
    sym::SymbolId sym = sym::kNone;
    uint32_t      seq = 0;
};

// N 생산자 × M 소비자. 종목 s는 생산자 s % N에만 있다(FeedMux가 종목을 소켓 하나에만 두는 것과 같다).
//  생산자는 자기 종목을 돌아가며 kPer번씩 push하고, 소비자는 자기 열이 빌 때까지 pop한다.
//  돌려주는 값은 소비자 하나가 항목 하나를 꺼내는 데 든 평균 ns(전체 벽시계 / 소비자당 항목 수).
struct RunResult
{
    bool     order_ok = true; // 종목마다 seq가 0,1,2,…로 왔나
    bool     shard_ok = true; // 소비자 m이 shard_of(sym) == m인 종목만 받았나
    uint64_t received = 0;
    double   ns_per   = 0.0; // 소비자 하나가 항목 하나를 받는 데 든 평균 ns(벽시계 / 소비자당 건수)
    double   wall_ms  = 0.0; // 전체 벽시계 — 같은 총량을 N×M이 얼마나 빨리 끝내나
};

RunResult run_matrix(uint32_t N, uint32_t M, uint32_t symbols, uint32_t per_symbol, size_t capacity)
{
    shard::Matrix<Item> mx(N, M, capacity);
    const uint64_t      total = static_cast<uint64_t>(symbols) * per_symbol;
    std::atomic<bool>   go{false};
    // 소비자마다 받을 건수를 미리 안다 — 공유 카운터를 항목마다 건드리면 그 경합이 측정에 섞인다.
    std::vector<uint64_t> expected(M, 0), received(M, 0);

    for (uint32_t s = 1; s <= symbols; ++s)
    {
        expected[shard::shard_of(s, M)] += per_symbol;
    }

    std::vector<std::vector<uint32_t>> last_seq(M, std::vector<uint32_t>(symbols + 1, 0));
    std::vector<int>    order_bad(M, 0), shard_bad(M, 0);

    std::vector<std::thread> consumers;

    for (uint32_t m = 0; m < M; ++m)
    {
        consumers.emplace_back([&, m]
        {
            while (!go.load(std::memory_order_acquire))
            {
            }

            while (received[m] < expected[m])
            {
                auto it = mx.pop(m);

                if (!it)
                {
                    std::this_thread::yield();
                    continue;
                }

                if (shard::shard_of(it->sym, M) != m)
                {
                    ++shard_bad[m];
                }

                uint32_t& expect = last_seq[m][it->sym];

                if (it->seq != expect)
                {
                    ++order_bad[m];
                }

                expect = it->seq + 1;
                ++received[m];
            }
        });
    }

    std::vector<std::thread> producers;

    for (uint32_t n = 0; n < N; ++n)
    {
        producers.emplace_back([&, n]
        {
            std::vector<sym::SymbolId> mine;

            for (uint32_t s = 1; s <= symbols; ++s)
            {
                if ((s - 1) % N == n)
                {
                    mine.push_back(s);
                }
            }

            while (!go.load(std::memory_order_acquire))
            {
            }

            for (uint32_t k = 0; k < per_symbol; ++k)
            {
                for (auto s : mine)
                {
                    Item it{s, k};

                    while (!mx.push(n, s, it))
                    {
                        std::this_thread::yield(); // 시험은 잃지 않는다 — 배선은 버리고 센다
                    }
                }
            }
        });
    }

    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& t : producers)
    {
        t.join();
    }

    for (auto& t : consumers)
    {
        t.join();
    }

    const auto t1 = std::chrono::steady_clock::now();
    RunResult  r;

    for (uint32_t m = 0; m < M; ++m)
    {
        r.received += received[m];
        r.order_ok = r.order_ok && order_bad[m] == 0;
        r.shard_ok = r.shard_ok && shard_bad[m] == 0;
    }

    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    r.ns_per        = ns / (static_cast<double>(total) / M);
    r.wall_ms       = ns / 1e6;
    return r;
}
} // namespace

int main()
{
    // 1. 해시 샤딩 — 결정적이고, 1샤드면 0, 2,500 종목을 4샤드에 나누면 어느 샤드도 500~750 사이.
    {
        CHECK(shard::shard_of(7, 1) == 0);
        CHECK(shard::shard_of(7, 4) == shard::shard_of(7, 4));
        std::vector<int> count(4, 0);

        for (sym::SymbolId s = 1; s <= 2500; ++s)
        {
            const auto m = shard::shard_of(s, 4);
            CHECK(m < 4);
            ++count[m];
        }

        for (int c : count)
        {
            CHECK(c >= 500 && c <= 750);
        }

        // 촘촘한 연속 id(현물 뒤 선물처럼 몰린 구간)도 흩어진다 — 연속 8개가 한 샤드에 몰리지 않는다.
        std::vector<int> run(4, 0);

        for (sym::SymbolId s = 100; s < 108; ++s)
        {
            ++run[shard::shard_of(s, 4)];
        }

        for (int c : run)
        {
            CHECK(c < 8);
        }
    }

    // 2. 셀 가득 참 — 용량 4면 네 번째까지 들어가고 다섯 번째는 false. 소비자가 비우면 다시 들어간다.
    {
        shard::Matrix<Item> mx(1, 1, 4);
        CHECK(mx.producers() == 1 && mx.consumers() == 1);
        CHECK(mx.empty(0));

        for (uint32_t k = 0; k < 4; ++k)
        {
            CHECK(mx.push(0, 1, Item{1, k}));
        }

        CHECK(!mx.push(0, 1, Item{1, 4}));
        CHECK(!mx.empty(0));
        CHECK(mx.high_water(0) == 4);
        auto got = mx.pop(0);
        CHECK(got && got->seq == 0);
        CHECK(mx.push(0, 1, Item{1, 4}));

        for (uint32_t k = 1; k <= 4; ++k)
        {
            got = mx.pop(0);
            CHECK(got && got->seq == k);
        }

        CHECK(!mx.pop(0));
        CHECK(mx.empty(0));
    }

    // 3. 라운드로빈 — 생산자 0이 셋, 생산자 1이 하나를 같은 샤드에 넣으면 0,1,0,0 순으로 나온다.
    //  push_to로 샤드를 직접 고른다.
    {
        shard::Matrix<Item> mx(2, 3, 8);
        CHECK(mx.push_to(0, 2, Item{10, 0}));
        CHECK(mx.push_to(0, 2, Item{10, 1}));
        CHECK(mx.push_to(0, 2, Item{10, 2}));
        CHECK(mx.push_to(1, 2, Item{11, 0}));
        CHECK(mx.empty(0) && mx.empty(1) && !mx.empty(2));
        auto a = mx.pop(2);
        auto b = mx.pop(2);
        auto c = mx.pop(2);
        auto d = mx.pop(2);
        CHECK(a && a->sym == 10 && a->seq == 0);
        CHECK(b && b->sym == 11);
        CHECK(c && c->sym == 10 && c->seq == 1);
        CHECK(d && d->sym == 10 && d->seq == 2);
        CHECK(!mx.pop(2));
        // 종목 해시 push는 shard_of가 고른 열에만 들어간다.
        CHECK(mx.push(1, 5, Item{5, 0}));
        const auto m5 = shard::shard_of(5, 3);

        for (uint32_t m = 0; m < 3; ++m)
        {
            CHECK(mx.empty(m) == (m != m5));
        }
    }

    // 4. 스레드 — 2 생산자 × 3 소비자, 종목 60개 × 2,000건. 종목 안 순서가 지켜지고 샤드가 어긋나지 않고 하나도 안 잃는다.
    {
        const auto r = run_matrix(2, 3, 60, 2000, 256);
        CHECK(r.received == 60u * 2000u);
        CHECK(r.order_ok);
        CHECK(r.shard_ok);
    }

    // 5. 측정(원칙 7) — 같은 총량(종목 240 × 4,000건 = 96만)을 1×1·2×2·4×4가 나눠 받을 때 소비자당 항목 비용과 전체 벽시계.
    //  코어 수에 따라 달라 값은 판정하지 않는다. 09-13 이 머신(논리 코어 16, Release, 셀 1024) 세 번 기록:
    //  1×1 17ns/건·16ms, 2×2 31ns/건·15ms, 4×4 45ns/건·11ms. 항목 하나의 비용은 N이 늘수록 오른다(생산자가 M개 셀에
    //  흩뿌리고 소비자가 N개 셀을 훑는다) — 행렬이 주는 것은 전략 계산을 M으로 나누는 것이지 큐 자체의 속도가 아니다.
    {
        std::cout << "[측정] hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';

        for (uint32_t k : {1u, 2u, 4u})
        {
            const auto r = run_matrix(k, k, 240, 4000, 1024);
            CHECK(r.received == 240u * 4000u);
            CHECK(r.order_ok && r.shard_ok);
            std::cout << "[측정] " << k << "x" << k << " 소비자당 " << (240u * 4000u / k) << "건: " << r.ns_per
                      << "ns/건, 전체 " << r.wall_ms << "ms\n";
        }
    }

    // 6. reshape — 1×1로 만든 행렬을 config가 정한 2×3으로 다시 잡는다. 셀은 새로 만들어지고(옛 항목은 버린다) 열 선택은
    //  새 열 수로 간다. 스레드가 없을 때만 부른다는 제약은 호출 쪽 몫이다.
    {
        shard::Matrix<Item> mx(1, 1, 8);
        CHECK(mx.push_to(0, 0, Item{5, 0}));
        mx.reshape(2, 3, 4);
        CHECK(mx.producers() == 2 && mx.consumers() == 3);
        CHECK(mx.empty(0) && mx.empty(1) && mx.empty(2));

        for (sym::SymbolId sym = 1; sym <= 30; ++sym)
        {
            CHECK(mx.consumer_of(sym) == shard::shard_of(sym, 3));
        }

        const auto m = mx.consumer_of(9);
        CHECK(mx.push_to(1, m, Item{9, 0}));
        CHECK(mx.push_to(1, m, Item{9, 1}));
        const auto a = mx.pop(m);
        const auto b = mx.pop(m);
        CHECK(a && b && a->seq == 0 && b->seq == 1);
        CHECK(!mx.pop(m));
    }

    std::cout << "test_shard_matrix: " << g_checks << " checks passed\n";
    return 0;
}
