// test_engine — 자물쇠 아래 바뀌는 목록을 판 번호로 사본 순회(GenerationCopy).
//  공용 준비는 test_engine_support.h, 실행 순서는 test_engine.cpp의 main.
#include "test_engine_support.h"

namespace engine_test
{

// 여러 스레드가 자물쇠를 잡고 목록을 바꾸는 동안, 한 스레드가 GenerationCopy 사본을 자물쇠 없이 순회한다
//  (CONSOLIDATED 2절 #1 — 데이터 스레드가 watch_specifications_를 자물쇠 없이 돌던 경쟁). 순회 중 원본이 바뀌어도
//  사본은 그대로라 끊기지 않고, 쓰기가 끝난 뒤 한 번 더 refresh하면 원본과 같아야 한다.
//  판 번호가 그대로면 다시 복사하지 않는지도 본다(같은 주소·같은 내용).
int run_generation_copy_case()
{
    std::vector<int>         source;
    std::mutex               source_mutex;
    std::atomic<uint64_t>    source_generation{0};
    GenerationCopy<std::vector<int>> data_thread_copy;
    std::atomic<int>         writers_finished{0};
    constexpr int            kWriterCount   = 2;
    constexpr int            kWritesPerWriter = 2000;

    std::vector<std::thread> writers;

    for (int writer_index = 0; writer_index < kWriterCount; ++writer_index)
    {
        writers.emplace_back([&, writer_index]
        {
            for (int write_index = 0; write_index < kWritesPerWriter; ++write_index)
            {
                std::lock_guard<std::mutex> lock(source_mutex);
                source.push_back(writer_index * kWritesPerWriter + write_index);

                if (write_index % 3 == 0)
                {
                    std::erase_if(source, [](int value)
                    {
                        return value % 5 == 0;
                    });
                }

                source_generation.fetch_add(1, std::memory_order_relaxed);
            }

            writers_finished.fetch_add(1, std::memory_order_release);
        });
    }

    // 읽는 쪽 — 쓰는 스레드가 모두 끝날 때까지 사본을 다시 뜨고 끝까지 순회한다.
    int64_t refreshes       = 0;
    int64_t negative_values = 0;

    while (writers_finished.load(std::memory_order_acquire) < kWriterCount)
    {
        const std::vector<int>& copy = data_thread_copy.refresh(source, source_mutex, source_generation);
        int64_t                 negative = 0;

        for (const int value : copy)
        {
            negative += value < 0 ? 1 : 0;
        }

        negative_values += negative;
        ++refreshes;
    }

    for (std::thread& writer : writers)
    {
        writer.join();
    }

    // 순회 도중에 CHECK로 빠져나가면 아직 도는 스레드를 부숴 std::terminate가 나므로 확인은 합류 뒤에 한다.
    CHECK(refreshes > 0);
    CHECK(negative_values == 0);

    const std::vector<int>& final_copy = data_thread_copy.refresh(source, source_mutex, source_generation);
    CHECK(final_copy == source);

    // 판 번호가 그대로면 다시 복사하지 않는다 — 판 번호를 올리지 않고 원본만 바꾸면 사본은 옛 내용 그대로다.
    const std::vector<int> before_change = final_copy;
    source.push_back(-1);
    CHECK(data_thread_copy.refresh(source, source_mutex, source_generation) == before_change);

    source_generation.fetch_add(1, std::memory_order_relaxed);
    CHECK(data_thread_copy.refresh(source, source_mutex, source_generation) == source);
    return 0;
}

} // namespace engine_test
