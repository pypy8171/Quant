// ShardMatrix 단위 테스트 — 종목 해시 샤딩의 균형, 셀 가득 참, 라운드로빈, N 생산자 × M 소비자에서 종목 안 순서 보존,
//  1×1·2×2·4×4 처리량 측정(원칙 7), 기동 전 reshape.
// 빌드: cmake --build <directory> --target test_shard_matrix
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

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 시험용 항목 — 종목 id와 그 종목 안 순번.
struct Item
{
    symbol::SymbolId symbol_id = symbol::kNone;
    uint32_t      sequence = 0;
};

// N 생산자 × M 소비자. 종목 s는 생산자 s % N에만 있다(FeedMux가 종목을 소켓 하나에만 두는 것과 같다).
//  생산자는 자기 종목을 돌아가며 kPer번씩 push하고, 소비자는 자기 열이 빌 때까지 pop한다.
//  돌려주는 값은 소비자 하나가 항목 하나를 꺼내는 데 든 평균 nanoseconds(전체 벽시계 / 소비자당 항목 수).
struct RunResult
{
    bool     order_ok = true; // 종목마다 sequence가 0,1,2,…로 왔나
    bool     shard_ok = true; // 소비자 m이 shard_of(symbol_id) == m인 종목만 받았나
    uint64_t received = 0;
    double   ns_per   = 0.0; // 소비자 하나가 항목 하나를 받는 데 든 평균 nanoseconds(벽시계 / 소비자당 건수)
    double   wall_ms  = 0.0; // 전체 벽시계 — 같은 총량을 N×M이 얼마나 빨리 끝내나
};

RunResult run_matrix(uint32_t count, uint32_t row_count, uint32_t symbols, uint32_t per_symbol, size_t capacity)
{
    shard::Matrix<Item> matrix(count, row_count, capacity);
    const uint64_t      total = static_cast<uint64_t>(symbols) * per_symbol;
    std::atomic<bool>   go{false};
    // 소비자마다 받을 건수를 미리 안다 — 공유 카운터를 항목마다 건드리면 그 경합이 측정에 섞인다.
    std::vector<uint64_t> expected(row_count, 0), received(row_count, 0);

    for (uint32_t symbol_index = 1; symbol_index <= symbols; ++symbol_index)
    {
        expected[shard::shard_of(symbol_index, row_count)] += per_symbol;
    }

    std::vector<std::vector<uint32_t>> last_sequence(row_count, std::vector<uint32_t>(symbols + 1, 0));
    std::vector<int>    order_bad(row_count, 0), shard_bad(row_count, 0);

    std::vector<std::thread> consumers;

    for (uint32_t row = 0; row < row_count; ++row)
    {
        consumers.emplace_back([&, row]
        {
            while (!go.load(std::memory_order_acquire))
            {
            }

            while (received[row] < expected[row])
            {
                auto iterator = matrix.pop(row);

                if (!iterator)
                {
                    std::this_thread::yield();
                    continue;
                }

                if (shard::shard_of(iterator->symbol_id, row_count) != row)
                {
                    ++shard_bad[row];
                }

                uint32_t& expect = last_sequence[row][iterator->symbol_id];

                if (iterator->sequence != expect)
                {
                    ++order_bad[row];
                }

                expect = iterator->sequence + 1;
                ++received[row];
            }
        });
    }

    std::vector<std::thread> producers;

    for (uint32_t slot_index = 0; slot_index < count; ++slot_index)
    {
        producers.emplace_back([&, slot_index]
        {
            std::vector<symbol::SymbolId> mine;

            for (uint32_t symbol_index = 1; symbol_index <= symbols; ++symbol_index)
            {
                if ((symbol_index - 1) % count == slot_index)
                {
                    mine.push_back(symbol_index);
                }
            }

            while (!go.load(std::memory_order_acquire))
            {
            }

            for (uint32_t per_symbol_index = 0; per_symbol_index < per_symbol; ++per_symbol_index)
            {
                for (auto mine_entry : mine)
                {
                    Item iterator{mine_entry, per_symbol_index};

                    while (!matrix.push(slot_index, mine_entry, iterator))
                    {
                        std::this_thread::yield(); // 시험은 잃지 않는다 — 배선은 버리고 센다
                    }
                }
            }
        });
    }

    const auto start_time = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& producer : producers)
    {
        producer.join();
    }

    for (auto& consumer : consumers)
    {
        consumer.join();
    }

    const auto end_time = std::chrono::steady_clock::now();
    RunResult  result;

    for (uint32_t row = 0; row < row_count; ++row)
    {
        result.received += received[row];
        result.order_ok = result.order_ok && order_bad[row] == 0;
        result.shard_ok = result.shard_ok && shard_bad[row] == 0;
    }

    const double nanoseconds = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
    result.ns_per        = nanoseconds / (static_cast<double>(total) / row_count);
    result.wall_ms       = nanoseconds / 1e6;
    return result;
}
} // namespace

int main()
{
    // 1. 해시 샤딩 — 결정적이고, 1샤드면 0, 2,500 종목을 4샤드에 나누면 어느 샤드도 500~750 사이.
    {
        CHECK(shard::shard_of(7, 1) == 0);
        CHECK(shard::shard_of(7, 4) == shard::shard_of(7, 4));
        std::vector<int> count(4, 0);

        for (symbol::SymbolId symbol_id = 1; symbol_id <= 2500; ++symbol_id)
        {
            const auto shard = shard::shard_of(symbol_id, 4);
            CHECK(shard < 4);
            ++count[shard];
        }

        for (int total : count)
        {
            CHECK(total >= 500 && total <= 750);
        }

        // 촘촘한 연속 id(현물 뒤 선물처럼 몰린 구간)도 흩어진다 — 연속 8개가 한 샤드에 몰리지 않는다.
        std::vector<int> run(4, 0);

        for (symbol::SymbolId symbol_id = 100; symbol_id < 108; ++symbol_id)
        {
            ++run[shard::shard_of(symbol_id, 4)];
        }

        for (int count : run)
        {
            CHECK(count < 8);
        }
    }

    // 2. 셀 가득 참 — 용량 4면 네 번째까지 들어가고 다섯 번째는 false. 소비자가 비우면 다시 들어간다.
    {
        shard::Matrix<Item> matrix(1, 1, 4);
        CHECK(matrix.producers() == 1 && matrix.consumers() == 1);
        CHECK(matrix.empty(0));

        for (uint32_t innermost_index = 0; innermost_index < 4; ++innermost_index)
        {
            CHECK(matrix.push(0, 1, Item{1, innermost_index}));
        }

        CHECK(!matrix.push(0, 1, Item{1, 4}));
        CHECK(!matrix.empty(0));
        CHECK(matrix.high_water(0) == 4);
        auto received = matrix.pop(0);
        CHECK(received && received->sequence == 0);
        CHECK(matrix.push(0, 1, Item{1, 4}));

        for (uint32_t innermost_index = 1; innermost_index <= 4; ++innermost_index)
        {
            received = matrix.pop(0);
            CHECK(received && received->sequence == innermost_index);
        }

        CHECK(!matrix.pop(0));
        CHECK(matrix.empty(0));
    }

    // 3. 라운드로빈 — 생산자 0이 셋, 생산자 1이 하나를 같은 샤드에 넣으면 0,1,0,0 순으로 나온다.
    //  push_to로 샤드를 직접 고른다.
    {
        shard::Matrix<Item> matrix(2, 3, 8);
        CHECK(matrix.push_to(0, 2, Item{10, 0}));
        CHECK(matrix.push_to(0, 2, Item{10, 1}));
        CHECK(matrix.push_to(0, 2, Item{10, 2}));
        CHECK(matrix.push_to(1, 2, Item{11, 0}));
        CHECK(matrix.empty(0) && matrix.empty(1) && !matrix.empty(2));
        auto first = matrix.pop(2);
        auto second = matrix.pop(2);
        auto third = matrix.pop(2);
        auto fourth = matrix.pop(2);
        CHECK(first && first->symbol_id == 10 && first->sequence == 0);
        CHECK(second && second->symbol_id == 11);
        CHECK(third && third->symbol_id == 10 && third->sequence == 1);
        CHECK(fourth && fourth->symbol_id == 10 && fourth->sequence == 2);
        CHECK(!matrix.pop(2));
        // 종목 해시 push는 shard_of가 고른 열에만 들어간다.
        CHECK(matrix.push(1, 5, Item{5, 0}));
        const auto m5 = shard::shard_of(5, 3);

        for (uint32_t row = 0; row < 3; ++row)
        {
            CHECK(matrix.empty(row) == (row != m5));
        }
    }

    // 4. 스레드 — 2 생산자 × 3 소비자, 종목 60개 × 2,000건. 종목 안 순서가 지켜지고 샤드가 어긋나지 않고 하나도 안 잃는다.
    {
        const auto run_result = run_matrix(2, 3, 60, 2000, 256);
        CHECK(run_result.received == 60u * 2000u);
        CHECK(run_result.order_ok);
        CHECK(run_result.shard_ok);
    }

    // 5. 측정(원칙 7) — 같은 총량(종목 240 × 4,000건 = 96만)을 1×1·2×2·4×4가 나눠 받을 때 소비자당 항목 비용과 전체 벽시계.
    //  코어 수에 따라 달라 값은 판정하지 않는다. 09-13 이 머신(논리 코어 16, Release, 셀 1024) 세 번 기록:
    //  1×1 17ns/건·16ms, 2×2 31ns/건·15ms, 4×4 45ns/건·11ms. 항목 하나의 비용은 N이 늘수록 오른다(생산자가 M개 셀에
    //  흩뿌리고 소비자가 N개 셀을 훑는다) — 행렬이 주는 것은 전략 계산을 M으로 나누는 것이지 큐 자체의 속도가 아니다.
    {
        std::cout << "[측정] hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';

        for (uint32_t innermost_index : {1u, 2u, 4u})
        {
            const auto run_result = run_matrix(innermost_index, innermost_index, 240, 4000, 1024);
            CHECK(run_result.received == 240u * 4000u);
            CHECK(run_result.order_ok && run_result.shard_ok);
            std::cout << "[측정] " << innermost_index << "x" << innermost_index << " 소비자당 " << (240u * 4000u / innermost_index) << "건: " << run_result.ns_per
                      << "ns/건, 전체 " << run_result.wall_ms << "ms\n";
        }
    }

    // 6. reshape — 1×1로 만든 행렬을 config가 정한 2×3으로 다시 잡는다. 셀은 새로 만들어지고(옛 항목은 버린다) 열 선택은
    //  새 열 수로 간다. 스레드가 없을 때만 부른다는 제약은 호출 쪽 몫이다.
    {
        shard::Matrix<Item> matrix(1, 1, 8);
        CHECK(matrix.push_to(0, 0, Item{5, 0}));
        matrix.reshape(2, 3, 4);
        CHECK(matrix.producers() == 2 && matrix.consumers() == 3);
        CHECK(matrix.empty(0) && matrix.empty(1) && matrix.empty(2));

        for (symbol::SymbolId symbol_id = 1; symbol_id <= 30; ++symbol_id)
        {
            CHECK(matrix.consumer_of(symbol_id) == shard::shard_of(symbol_id, 3));
        }

        const auto consumer = matrix.consumer_of(9);
        CHECK(matrix.push_to(1, consumer, Item{9, 0}));
        CHECK(matrix.push_to(1, consumer, Item{9, 1}));
        const auto first = matrix.pop(consumer);
        const auto second = matrix.pop(consumer);
        CHECK(first && second && first->sequence == 0 && second->sequence == 1);
        CHECK(!matrix.pop(consumer));
    }

    std::cout << "test_shard_matrix: " << g_checks << " checks passed\n";
    return 0;
}
