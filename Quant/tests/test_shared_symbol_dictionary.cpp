// tests/test_shared_symbol_dictionary.cpp
// 공유 쪽지 위의 종목 표 검증 (D-114 단계 4) — 프로세스를 갈라도 같은 종목에 같은 번호가 붙는가.
//
//   ① 자리 셈이 실제 배치와 맞는가(머리 + 버킷 배열 + 이름 배열)
//   ② 같은 바이트에 붙은 손잡이 둘이 같은 번호를 보는가 — 이게 이 표를 만든 이유다
//   ③ 같은 티커는 같은 번호, 없는 티커는 kNone, 이름은 왕복하는가
//   ④ 가득 차면 kNone을 주고 이미 준 번호는 그대로인가
//   ⑤ 머리가 다르면 **붙지 않는가**(옛 exe가 새 배치에 붙는 길이 없는가)
//   ⑥ 넣는 스레드가 여럿이어도 번호가 하나씩만 나가고, 그때 읽는 쪽이 본 번호의 이름이 비지 않는가
//      (버킷을 count보다 먼저 놓으면 여기서 빈 티커가 나온다 — 09-22에 잡은 자리)
//   ⑦ symbol::SymbolTable과 같은 답을 내는가(알고리즘이 한 벌인 것)
//
//   사용법: test_shared_symbol_dictionary

#include "core/SymbolTable.h"
#include "ipc/SharedSymbolDictionary.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
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

constexpr size_t kStorageBytes = 64 * 1024;

alignas(ipc::kSharedCacheLine) std::byte g_storage[kStorageBytes];
alignas(ipc::kSharedCacheLine) std::byte g_small_storage[4096];

// 종목 코드 여섯 자리 — 실제 표에 들어오는 모양 그대로다(숫자만).
std::string ticker_of(size_t index)
{
    const std::string text = std::to_string(100000 + index);
    return text.substr(text.size() - 6);
}

// ① 자리 셈
void test_size_math()
{
    const size_t capacity     = 64;
    const size_t bucket_count = symbol::bucket_count_for(capacity);
    const size_t expected     = sizeof(ipc::SharedDictionaryControl) +
                            bucket_count * sizeof(std::atomic<symbol::SymbolId>) + capacity * sizeof(symbol::Ticker);

    check(ipc::SharedSymbolDictionary::bytes_for(capacity) == expected, "자리 셈: 머리 + 버킷 + 이름");
    check(sizeof(ipc::SharedDictionaryControl) % ipc::kSharedCacheLine == 0, "자리 셈: 머리는 캐시라인 배수");
    check(bucket_count >= capacity * 2, "자리 셈: 버킷은 종목 수의 두 배 이상");
}

// ②③ 놓고, 둘이 붙고, 같은 번호를 본다
void test_two_handles_share_numbers()
{
    const size_t capacity = 64;

    ipc::SharedSymbolDictionary writer;
    check(writer.create(g_storage, sizeof(g_storage), capacity), "놓기: create");
    check(writer.is_bound(), "놓기: 붙은 상태");
    check(writer.size() == 0, "놓기: 처음엔 비어 있다");
    check(writer.capacity() == capacity, "놓기: 종목 수");

    const symbol::SymbolId samsung = writer.intern("005930");
    const symbol::SymbolId hynix   = writer.intern("000660");
    check(samsung != symbol::kNone, "넣기: 번호를 받는다");
    check(samsung != hynix, "넣기: 다른 종목은 다른 번호");
    check(writer.intern("005930") == samsung, "넣기: 같은 종목은 같은 번호");
    check(writer.size() == 2, "넣기: 등록 수");

    // 건너편 — 같은 바이트에 붙기만 한 손잡이다(다른 프로세스가 같은 구역에 붙은 것과 같다)
    ipc::SharedSymbolDictionary reader;
    check(reader.attach(g_storage, sizeof(g_storage), capacity), "붙기: attach");
    check(reader.lookup("005930") == samsung, "붙기: 건너편이 같은 번호를 본다");
    check(reader.lookup("000660") == hynix, "붙기: 건너편이 같은 번호를 본다 2");
    check(reader.name(samsung) == "005930", "붙기: 번호 → 이름");
    check(reader.size() == 2, "붙기: 등록 수도 같다");

    check(reader.lookup("123456") == symbol::kNone, "없는 종목은 kNone");
    check(reader.name(9999).empty(), "모르는 번호는 빈 티커");
    check(reader.name(symbol::kNone).empty(), "0번은 빈 티커");

    // 건너편이 넣은 것도 처음 손잡이에 보인다 — 한 방향만이 아니라는 확인
    const symbol::SymbolId naver = reader.intern("035420");
    check(writer.lookup("035420") == naver, "붙기: 반대 방향도 보인다");
}

// ④ 가득 참 — 넘치면 kNone, 이미 준 번호는 그대로
void test_full()
{
    const size_t capacity = 8;

    ipc::SharedSymbolDictionary dictionary;
    check(dictionary.create(g_small_storage, sizeof(g_small_storage), capacity), "가득 참: create");

    std::vector<symbol::SymbolId> given;

    // 0번은 "번호 없음" 자리라 실제로 들어가는 것은 capacity - 1개다
    for (size_t index = 0; index < capacity - 1; ++index)
    {
        const symbol::SymbolId id = dictionary.intern(ticker_of(index));
        check(id != symbol::kNone, "가득 참: " + ticker_of(index) + " 들어간다");
        given.push_back(id);
    }

    check(dictionary.intern("999999") == symbol::kNone, "가득 참: 넘치면 kNone");

    for (size_t index = 0; index < given.size(); ++index)
    {
        check(dictionary.lookup(ticker_of(index)) == given[index], "가득 참: 이미 준 번호는 그대로 " + ticker_of(index));
    }
}

// ⑤ 붙기 거절 — 옛 exe가 새 배치에 붙는 길이 없어야 한다
void test_attach_refusals()
{
    const size_t capacity = 64;

    ipc::SharedSymbolDictionary writer;
    check(writer.create(g_storage, sizeof(g_storage), capacity), "거절: 먼저 놓는다");

    ipc::SharedSymbolDictionary wrong_capacity;
    check(!wrong_capacity.attach(g_storage, sizeof(g_storage), 128), "거절: 종목 수가 다르면 안 붙는다");
    check(!wrong_capacity.last_error().empty(), "거절: 사유가 남는다");
    check(!wrong_capacity.is_bound(), "거절: 안 붙은 상태");

    ipc::SharedSymbolDictionary too_small;
    check(!too_small.attach(g_storage, 64, capacity), "거절: 구역이 표보다 작으면 안 붙는다");

    ipc::SharedSymbolDictionary off_line;
    check(!off_line.attach(g_storage + 8, sizeof(g_storage) - 8, capacity), "거절: 캐시라인 경계가 아니면 안 붙는다");

    ipc::SharedSymbolDictionary no_base;
    check(!no_base.attach(nullptr, sizeof(g_storage), capacity), "거절: 자리가 없으면 안 붙는다");

    ipc::SharedSymbolDictionary too_few;
    check(!too_few.create(g_small_storage, sizeof(g_small_storage), 1), "거절: 종목 수 1은 안 받는다");

    // 머리를 망가뜨리면 붙지 않는다
    auto*          control = reinterpret_cast<ipc::SharedDictionaryControl*>(g_storage);
    const uint32_t magic   = control->magic;
    control->magic         = 0xDEADBEEF;

    ipc::SharedSymbolDictionary wrong_magic;
    check(!wrong_magic.attach(g_storage, sizeof(g_storage), capacity), "거절: 머리가 다르면 안 붙는다");
    control->magic = magic;

    ipc::SharedSymbolDictionary again;
    check(again.attach(g_storage, sizeof(g_storage), capacity), "거절: 머리를 되돌리면 붙는다");
    check(again.last_error().empty(), "거절: 붙으면 사유는 빈 문자열");

    again.unbind();
    check(!again.is_bound(), "거절: unbind 뒤엔 안 붙은 상태");
    check(again.lookup("005930") == symbol::kNone, "거절: 안 붙은 손잡이는 kNone");
    check(again.intern("005930") == symbol::kNone, "거절: 안 붙은 손잡이는 넣지도 않는다");
    check(again.size() == 0, "거절: 안 붙은 손잡이의 등록 수는 0");
}

// ⑥ 넣는 스레드 여럿 + 읽는 스레드 하나
void test_concurrent_intern()
{
    const size_t capacity     = 1024;
    const size_t writer_count = 4;
    const size_t ticker_count = 300;

    ipc::SharedSymbolDictionary writer;
    check(writer.create(g_storage, sizeof(g_storage), capacity), "부하: create");

    ipc::SharedSymbolDictionary reader;
    check(reader.attach(g_storage, sizeof(g_storage), capacity), "부하: attach");

    std::atomic<bool> running{true};
    std::atomic<int>  torn_names{0};

    // 건너편이 보는 길 — 번호를 받았는데 이름이 안 맞으면 발행 순서가 깨진 것이다
    std::thread watcher([&] {
        while (running.load(std::memory_order_relaxed))
        {
            for (size_t index = 0; index < ticker_count; ++index)
            {
                const std::string      ticker = ticker_of(index);
                const symbol::SymbolId id     = reader.lookup(ticker);

                if (id != symbol::kNone && !(reader.name(id) == ticker))
                {
                    torn_names.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    std::vector<std::thread> writers;

    for (size_t thread_index = 0; thread_index < writer_count; ++thread_index)
    {
        writers.emplace_back([&] {
            for (size_t index = 0; index < ticker_count; ++index)
            {
                (void)writer.intern(ticker_of(index));
            }
        });
    }

    for (std::thread& thread : writers)
    {
        thread.join();
    }

    running.store(false, std::memory_order_relaxed);
    watcher.join();

    check(torn_names.load() == 0, "부하: 번호를 본 순간 이름도 보인다");
    check(writer.size() == ticker_count, "부하: 스레드가 넷이어도 종목마다 번호 하나");

    std::unordered_set<symbol::SymbolId> seen;

    for (size_t index = 0; index < ticker_count; ++index)
    {
        const symbol::SymbolId id = reader.lookup(ticker_of(index));
        check(id != symbol::kNone, "부하: 다 들어갔다 " + ticker_of(index));
        seen.insert(id);
    }

    check(seen.size() == ticker_count, "부하: 번호가 겹치지 않는다");
}

// ⑦ 힙에 둔 표와 같은 답 — 알고리즘이 한 벌이어야 프로세스를 갈라도 번호가 안 갈린다
void test_same_answer_as_heap_table()
{
    const size_t capacity = 256;

    ipc::SharedSymbolDictionary shared;
    check(shared.create(g_storage, sizeof(g_storage), capacity), "같은 답: create");

    symbol::SymbolTable heap(capacity);

    for (size_t index = 0; index < 100; ++index)
    {
        const std::string ticker = ticker_of(index);
        check(shared.intern(ticker) == heap.intern(ticker), "같은 답: 번호 " + ticker);
    }

    check(shared.size() == heap.size(), "같은 답: 등록 수");
    check(shared.name(7) == heap.name(7), "같은 답: 이름");

    // 15자를 넘으면 양쪽이 같은 자리에서 자른다
    const std::string long_ticker = "ABCDEFGHIJKLMNOPQRST";
    check(shared.intern(long_ticker) == heap.intern(long_ticker), "같은 답: 긴 티커도 같은 번호");
    check(shared.name(shared.lookup(long_ticker)) == heap.name(heap.lookup(long_ticker)),
          "같은 답: 긴 티커는 같게 잘린다");
}

} // namespace

int main()
{
    std::cout << "=== 공유 쪽지 위의 종목 표 (D-114 단계 4) ===\n";
    test_size_math();
    test_two_handles_share_numbers();
    test_full();
    test_attach_refusals();
    test_concurrent_intern();
    test_same_answer_as_heap_table();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
