// tests/test_shared_strategy_dictionary.cpp
// 공유 쪽지 위의 전략 이름표 검증 (D-114 단계 4) — 프로세스를 갈라도 같은 전략에 같은 번호가 붙는가.
//
//   ① 자리 셈이 실제 배치와 맞는가(머리 + 이름 배열)
//   ② 같은 바이트에 붙은 손잡이 둘이 같은 번호를 보는가 — 이게 이 표를 만든 이유다
//   ③ 칸을 넘는 이름은 **잘라 넣지 않고 거절하는가**(잘라 넣으면 접두가 같은 전략 둘이 한 번호를 쓴다)
//   ④ 가득 차면 kNone을 주고 이미 준 번호는 그대로인가
//   ⑤ 머리가 다르면 붙지 않는가(옛 exe가 새 배치에 붙는 길이 없는가)
//   ⑥ 넣는 스레드가 여럿이어도 번호가 하나씩만 나가고, 그때 읽는 쪽이 본 번호의 이름이 비지 않는가
//   ⑦ strategy_table::StrategyTable과 같은 답을 내는가(알고리즘이 한 벌인 것)
//
//   사용법: test_shared_strategy_dictionary

#include "core/StrategyTable.h"
#include "ipc/SharedStrategyDictionary.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

constexpr size_t kStorageBytes      = 64 * 1024;
constexpr size_t kSmallStorageBytes = 4096;

alignas(ipc::kSharedCacheLine) std::byte g_storage[kStorageBytes];
alignas(ipc::kSharedCacheLine) std::byte g_small_storage[kSmallStorageBytes];

// 재스캔이 짓는 이름 꼴 그대로 — 접두가 길게 겹치고 뒤 여섯 자리만 다르다.
std::string strategy_of(size_t index)
{
    std::string digits = std::to_string(100000 + index % 900000);
    return "DEVSCALE_" + digits;
}

void test_size_math()
{
    const size_t capacity = 256;
    const size_t expected = sizeof(ipc::SharedStrategyControl) + capacity * sizeof(strategy_table::StrategyName);

    check(ipc::SharedStrategyDictionary::bytes_for(capacity) == expected, "자리 셈: 머리 + 이름 배열");
    check(sizeof(strategy_table::StrategyName) == strategy_table::StrategyName::kMax + 1,
          "이름 칸에 빈틈이 없다");
    check(ipc::SharedStrategyDictionary::bytes_for(capacity) < kStorageBytes, "256개가 시험 구역에 든다");
}

void test_two_handles_share_numbers()
{
    const size_t             capacity = 256;
    ipc::SharedStrategyDictionary writer;
    ipc::SharedStrategyDictionary reader;

    check(writer.create(g_storage, kStorageBytes, capacity), "놓기");
    check(reader.attach(g_storage, kStorageBytes, capacity), "붙기");

    const strategy_table::StrategyId devscale = writer.intern("DEVSCALE_005930");
    const strategy_table::StrategyId manual   = writer.intern("MANUAL");

    check(devscale == 1, "첫 번호는 1");
    check(manual == 2, "그다음은 2");
    check(reader.lookup("DEVSCALE_005930") == devscale, "건너편이 같은 번호를 본다");
    check(reader.lookup("MANUAL") == manual, "건너편이 같은 번호를 본다(둘째)");
    check(reader.name(devscale).view() == "DEVSCALE_005930", "번호 → 이름");
    check(reader.lookup("FORCE_LIQ") == strategy_table::kNone, "모르는 이름은 kNone");
    check(writer.intern("DEVSCALE_005930") == devscale, "같은 이름은 같은 번호");
    check(reader.size() == 2, "등록 수");
}

void test_long_name_is_refused()
{
    const size_t             capacity = 64;
    ipc::SharedStrategyDictionary table;

    check(table.create(g_storage, kStorageBytes, capacity), "놓기(긴 이름 시험)");

    const std::string exact(strategy_table::StrategyName::kMax, 'A');
    const std::string too_long(strategy_table::StrategyName::kMax + 1, 'A');

    check(table.intern(exact) != strategy_table::kNone, "31자는 든다");
    check(table.intern(too_long) == strategy_table::kNone, "32자는 거절한다");
    check(table.lookup(too_long) == strategy_table::kNone, "거절한 이름은 찾아도 없다");
    check(table.intern("") == strategy_table::kNone, "빈 이름은 kNone");
    check(table.size() == 1, "거절한 이름은 표를 늘리지 않는다");
}

void test_full()
{
    const size_t             capacity = 8;
    ipc::SharedStrategyDictionary table;

    check(table.create(g_storage, kStorageBytes, capacity), "놓기(가득참 시험)");

    std::vector<strategy_table::StrategyId> given;

    for (size_t index = 0; index < capacity - 1; ++index)
    {
        const strategy_table::StrategyId id = table.intern(strategy_of(index));
        check(id != strategy_table::kNone, "자리가 있으면 번호를 준다");
        given.push_back(id);
    }

    check(table.intern(strategy_of(capacity)) == strategy_table::kNone, "가득 차면 kNone");
    check(table.size() == capacity - 1, "가득 찬 뒤에도 등록 수는 그대로");

    for (size_t index = 0; index < given.size(); ++index)
    {
        check(table.lookup(strategy_of(index)) == given[index], "가득 찬 뒤에도 준 번호는 그대로");
    }
}

void test_attach_refusals()
{
    const size_t             capacity = 128;
    ipc::SharedStrategyDictionary table;

    check(table.create(g_storage, kStorageBytes, capacity), "놓기(붙기 거절 시험)");

    ipc::SharedStrategyDictionary other;

    check(!other.attach(g_storage, kStorageBytes, capacity * 2), "전략 수가 다르면 안 붙는다");
    check(!other.attach(g_small_storage, 64, capacity), "구역이 작으면 안 붙는다");
    check(!other.attach(g_storage + 8, kStorageBytes - 8, capacity), "경계가 어긋나면 안 붙는다");
    check(!other.attach(nullptr, kStorageBytes, capacity), "자리가 없으면 안 붙는다");
    check(!other.attach(g_storage, kStorageBytes, 1), "전략 수 1은 안 붙는다");
    check(!other.last_error().empty(), "거절 사유가 남는다");
    check(other.attach(g_storage, kStorageBytes, capacity), "머리가 같으면 붙는다");

    // 머리를 망가뜨리면 다음에 붙는 쪽이 걸러낸다
    auto* control  = reinterpret_cast<ipc::SharedStrategyControl*>(g_storage);
    control->magic = 0;

    ipc::SharedStrategyDictionary late;
    check(!late.attach(g_storage, kStorageBytes, capacity), "표식이 깨지면 안 붙는다");

    other.unbind();
    check(!other.is_bound(), "뗀 뒤에는 안 붙은 상태");
    check(other.intern("MANUAL") == strategy_table::kNone, "안 붙었으면 번호를 주지 않는다");
    check(other.size() == 0, "안 붙었으면 등록 수는 0");
}

void test_concurrent_intern()
{
    const size_t             capacity = 1024;
    ipc::SharedStrategyDictionary writer;
    ipc::SharedStrategyDictionary reader;

    check(writer.create(g_storage, kStorageBytes, capacity), "놓기(동시 등록 시험)");
    check(reader.attach(g_storage, kStorageBytes, capacity), "붙기(동시 등록 시험)");

    constexpr int    kWriters = 4;
    constexpr size_t kEach    = 200;

    std::atomic<bool> stop{false};
    std::atomic<int>  torn_names{0};

    std::thread watcher([&reader, &stop, &torn_names] {
        while (!stop.load(std::memory_order_acquire))
        {
            const size_t count = reader.size();

            for (strategy_table::StrategyId id = 1; id <= count; ++id)
            {
                // 번호를 본 순간 이름은 완성돼 있어야 한다 — 발행 순서를 뒤집으면 여기서 빈 이름이 나온다
                if (reader.name(id).view().empty())
                {
                    torn_names.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    std::vector<std::thread> writers;

    for (int worker = 0; worker < kWriters; ++worker)
    {
        writers.emplace_back([&writer, worker] {
            for (size_t index = 0; index < kEach; ++index)
            {
                (void)writer.intern(strategy_of(worker * kEach + index));
            }
        });
    }

    for (std::thread& thread : writers)
    {
        thread.join();
    }

    stop.store(true, std::memory_order_release);
    watcher.join();

    check(torn_names.load() == 0, "읽는 쪽이 빈 이름을 본 적이 없다");
    check(writer.size() == kWriters * kEach, "이름 수만큼만 번호가 나갔다");

    std::unordered_set<strategy_table::StrategyId> seen;

    for (int worker = 0; worker < kWriters; ++worker)
    {
        for (size_t index = 0; index < kEach; ++index)
        {
            const strategy_table::StrategyId id = reader.lookup(strategy_of(worker * kEach + index));
            check(id != strategy_table::kNone, "동시에 넣은 이름을 건너편이 찾는다");
            seen.insert(id);
        }
    }

    check(seen.size() == kWriters * kEach, "번호가 겹치지 않았다");
}

void test_same_answer_as_heap_table()
{
    const size_t             capacity = 512;
    ipc::SharedStrategyDictionary shared;
    strategy_table::StrategyTable heap(capacity);

    check(shared.create(g_storage, kStorageBytes, capacity), "놓기(힙 표 대조)");

    for (size_t index = 0; index < 300; ++index)
    {
        const std::string name = strategy_of(index);
        check(shared.intern(name) == heap.intern(name), "같은 답: 번호");
    }

    check(shared.size() == heap.size(), "같은 답: 등록 수");
    check(shared.name(7).view() == heap.name(7).view(), "같은 답: 이름");

    const std::string too_long(strategy_table::StrategyName::kMax + 1, 'B');
    check(shared.intern(too_long) == heap.intern(too_long), "같은 답: 긴 이름은 양쪽 다 거절");
}

} // namespace

int main()
{
    std::cout << "=== 공유 쪽지 위의 전략 이름표 (D-114 단계 4) ===\n";
    test_size_math();
    test_two_handles_share_numbers();
    test_long_name_is_refused();
    test_full();
    test_attach_refusals();
    test_concurrent_intern();
    test_same_answer_as_heap_table();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
