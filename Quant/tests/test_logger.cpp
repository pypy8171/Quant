// tests/test_logger.cpp
// 비동기 Logger(MPSC 큐, D-045) 검증.
//
// 검증 항목:
//   ① 무손실·순번 : N 스레드가 각 M줄 로그 → 파일에 (N*M − dropped)줄, 스레드별 sequence는 단조 증가
//   ② flush 계약  : flush() 반환 시점에 그 전에 반환된 log()가 전부 파일에 있다
//   ③ 드롭 계수   : 가득 차서 버린 수가 dropped()에 잡히고, 기록 + 드롭 = 발행 수
//   ④ 작업 이름   : ScopedName이 끝나면 스레드 이름이 "T" + 스레드 번호로 돌아간다(풀 스레드가 옛 이름을 쥐지 않는다)
//   ⑤ 값 전달   : 넘긴 변수는 그대로 남고, 긴 임시 문자열도 잘리지 않고 기록되며, 임계값 아래 레벨은 버린다
//
// 사용법: test_logger   (cwd 하위 logs_test/ 에 파일을 만들고 끝나면 지운다)

#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

const std::filesystem::path kLogDirectory = "logs_test";
const std::filesystem::path kFile = kLogDirectory / "test_logger.log";

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

void test_scoped_name_clears()
{
    std::thread worker([]
    {
        {
            const thread_name::ScopedName name("BalanceFetch");
            assert(std::strcmp(thread_name::current(), "BalanceFetch") == 0);
        }

        assert(thread_name::current()[0] == 'T');
    });
    worker.join();
    std::printf("[PASS] scoped name clears\n");
}

// ⑤ 문자열을 값으로 받아 옮긴다: 넘긴 변수는 그대로 남고, 조립한 임시 문자열(작은 문자열 최적화보다 긴 것)은
//  잘리지 않고 파일에 그대로 실린다. 임계값 아래 레벨은 여전히 버린다.
bool file_has(const std::string& text)
{
    std::ifstream in(kFile);
    std::string   line;

    while (std::getline(in, line))
    {
        if (line.find(text) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

void test_message_moved_into_record()
{
    constexpr size_t kLongLength = 200;
    auto&            logger      = Logger::instance();

    const std::string kept = "SINKKEPT-lvalue";
    logger.info(kept);
    assert(kept == "SINKKEPT-lvalue"); // 값 전달이라 호출자 변수는 그대로다

    const std::string long_body(kLongLength, 'x');
    logger.warn("SINKLONG-" + long_body);

    logger.set_min_level(LogLevel::WARN);
    logger.info("SINKFILTERED");
    logger.set_min_level(LogLevel::INFO);
    logger.flush();

    assert(file_has(kept));
    assert(file_has("SINKLONG-" + long_body));
    assert(!file_has("SINKFILTERED"));
    std::printf("[PASS] message moved into record\n");
}

} // namespace

int main()
{
    std::error_code error_code;
    std::filesystem::create_directories(kLogDirectory, error_code);
    std::filesystem::remove(kFile, error_code);

    test_multi_producer_no_loss();
    test_flush_contract();
    test_scoped_name_clears();
    test_message_moved_into_record();

    Logger::instance().flush();
    std::printf("ALL PASS\n");
    return 0;
}
