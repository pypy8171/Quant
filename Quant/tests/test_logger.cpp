// tests/test_logger.cpp
// 비동기 Logger(MPSC 큐, D-045) 검증.
//
// 검증 항목:
//   ① 무손실·순번 : N 스레드가 각 M줄 로그 → 파일에 (N*M − dropped)줄, 스레드별 sequence는 단조 증가
//   ② flush 계약  : flush() 반환 시점에 그 전에 반환된 log()가 전부 파일에 있다
//   ③ 드롭 계수   : 가득 차서 버린 수가 dropped()에 잡히고, 기록 + 드롭 = 발행 수
//
// 사용법: test_logger   (cwd 하위 logs_test/ 에 파일을 만들고 끝나면 지운다)

#include "utils/Logger.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

const std::filesystem::path kDir = "logs_test";
const std::filesystem::path kFile = kDir / "test_logger.log";

// 파일에서 "T<t> S<sequence>" 꼴 줄을 세고 스레드별 sequence 단조 증가를 확인한다.
size_t count_and_check_order(int threads)
{
    std::ifstream in(kFile);
    std::vector<long> last(threads, -1);
    size_t lines = 0;
    std::string line;

    while (std::getline(in, line))
    {
        const auto found = line.find(" T");

        if (found == std::string::npos)
        {
            continue;
        }

        int thread_id = 0;
        long sequence = 0;

        if (std::sscanf(line.c_str() + found, " T%d S%ld", &thread_id, &sequence) != 2)
        {
            continue;
        }

        assert(thread_id >= 0 && thread_id < threads);
        assert(sequence > last[thread_id]); // 스레드별 FIFO — 역전·중복 없음
        last[thread_id] = sequence;
        ++lines;
    }

    return lines;
}

void test_multi_producer_no_loss()
{
    static constexpr int kThreads = 4;
    static constexpr int kPerThread = 20000;
    auto& logger = Logger::instance();
    logger.initialize(kFile, LogLevel::INFO);

    const uint64_t dropped_before = logger.dropped();
    std::vector<std::thread> threads;

    for (int thread_index = 0; thread_index < kThreads; ++thread_index)
    {
        threads.emplace_back([thread_index, &logger]
        {
            for (int per_thread_index = 0; per_thread_index < kPerThread; ++per_thread_index)
            {
                logger.info("T" + std::to_string(thread_index) + " S" + std::to_string(per_thread_index));
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    logger.flush();
    const uint64_t dropped = logger.dropped() - dropped_before;
    const size_t written = count_and_check_order(kThreads);
    std::printf("written=%zu dropped=%llu\n", written, static_cast<unsigned long long>(dropped));
    assert(written + dropped == static_cast<size_t>(kThreads) * kPerThread);
    std::printf("[PASS] multi-producer no loss / per-thread order\n");
}

void test_flush_contract()
{
    auto& logger = Logger::instance();

    for (int round = 0; round < 50; ++round)
    {
        const std::string tag = "FLUSHMARK-" + std::to_string(round);
        logger.info(tag);
        logger.flush();

        std::ifstream in(kFile);
        std::string line;
        bool found = false;

        while (std::getline(in, line))
        {
            if (line.find(tag) != std::string::npos)
            {
                found = true;
                break;
            }
        }

        assert(found); // flush() 반환 전에 기록돼 있어야 한다
    }

    std::printf("[PASS] flush contract\n");
}

} // namespace

int main()
{
    std::error_code error_code;
    std::filesystem::create_directories(kDir, error_code);
    std::filesystem::remove(kFile, error_code);

    test_multi_producer_no_loss();
    test_flush_contract();

    Logger::instance().flush();
    std::printf("ALL PASS\n");
    return 0;
}
