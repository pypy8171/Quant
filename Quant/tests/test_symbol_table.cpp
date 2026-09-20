// SymbolTable 단위 테스트 — id 부여 순서, 재조회 동일성, 상한, 스레드 여럿이 같은 종목을 동시에 넣을 때의 일관성.
// 빌드: cmake --build <directory> --target test_symbol_table
#include "core/SymbolTable.h"

#include <atomic>
#include <iostream>
#include <string>
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

} // namespace

int main()
{
    // 1. 빈 테이블: 모르는 종목은 kNone, 이름은 빈 문자열.
    symbol::SymbolTable symbol_table;
    CHECK(symbol_table.size() == 0);
    CHECK(symbol_table.lookup("005930") == symbol::kNone);
    CHECK(symbol_table.name(symbol::kNone).empty());
    CHECK(symbol_table.name(99).empty());

    // 2. 등록 순서대로 1부터, 같은 종목은 같은 id, 이름 왕복.
    const auto first_id = symbol_table.intern("005930");
    const auto second_id = symbol_table.intern("000660");
    CHECK(first_id == 1);
    CHECK(second_id == 2);
    CHECK(symbol_table.intern("005930") == first_id);
    CHECK(symbol_table.lookup("000660") == second_id);
    CHECK(symbol_table.name(first_id) == "005930");
    CHECK(symbol_table.size() == 2);

    // 3. string_view로 찾아도 복사 없이 같은 답.
    const std::string     text = "005930";
    const std::string_view value(text.data(), 6);
    CHECK(symbol_table.lookup(value) == first_id);

    // 4. 상한: capacity 4면 id 1~3까지, 넷째는 kNone이고 기존 것은 그대로.
    symbol::SymbolTable small(4);
    CHECK(small.intern("A") == 1);
    CHECK(small.intern("B") == 2);
    CHECK(small.intern("C") == 3);
    CHECK(small.intern("D") == symbol::kNone);
    CHECK(small.intern("B") == 2);
    CHECK(small.size() == 3);

    // 5. 스레드 4개가 같은 200종목을 동시에 넣어도 종목마다 id가 하나고 총 200개다.
    symbol::SymbolTable         shared;
    std::vector<std::thread> threads;
    std::vector<std::vector<symbol::SymbolId>> seen(4, std::vector<symbol::SymbolId>(200));

    for (int innermost_index = 0; innermost_index < 4; ++innermost_index)
    {
        threads.emplace_back(
            [&, innermost_index]
            {
                for (int index = 0; index < 200; ++index)
                {
                    seen[innermost_index][index] = shared.intern("T" + std::to_string((index * 7 + innermost_index * 13) % 200));
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    CHECK(shared.size() == 200);

    for (int index = 0; index < 200; ++index)
    {
        const std::string name = "T" + std::to_string(index);
        const auto        id   = shared.lookup(name);
        CHECK(id != symbol::kNone);
        CHECK(shared.name(id) == name);
    }

    for (int innermost_index = 0; innermost_index < 4; ++innermost_index)
    {
        for (int index = 0; index < 200; ++index)
        {
            const std::string name = "T" + std::to_string((index * 7 + innermost_index * 13) % 200);
            CHECK(seen[innermost_index][index] == shared.lookup(name));
        }
    }

    // 6. 쓰기 1 + 읽기 3 동시 — 읽는 쪽은 락이 없으니, 넣는 중인 종목을 보면 kNone이거나 정확한 id·이름 왕복이어야 한다.
    //    (버킷 발행 순서가 어긋나면 id는 보이는데 이름이 비어 있거나 다른 이름이 나온다)
    {
        constexpr int             kCount = 4000;
        symbol::SymbolTable       racing(kCount + 1);
        std::atomic<bool>         writer_done{false};
        std::atomic<int>          mismatches{0};
        std::vector<std::thread>  readers;

        for (int reader_index = 0; reader_index < 3; ++reader_index)
        {
            readers.emplace_back(
                [&]
                {
                    while (!writer_done.load(std::memory_order_acquire))
                    {
                        for (int index = 0; index < kCount; ++index)
                        {
                            const std::string name = "R" + std::to_string(index);
                            const auto        id   = racing.lookup(name);

                            if (id != symbol::kNone && !(racing.name(id) == name))
                            {
                                mismatches.fetch_add(1);
                            }

                            if (id != symbol::kNone && id > racing.size())
                            {
                                mismatches.fetch_add(1);
                            }
                        }
                    }
                });
        }

        for (int index = 0; index < kCount; ++index)
        {
            CHECK(racing.intern("R" + std::to_string(index)) == static_cast<symbol::SymbolId>(index + 1));
        }

        writer_done.store(true, std::memory_order_release);

        for (auto& reader : readers)
        {
            reader.join();
        }

        CHECK(mismatches.load() == 0);
        CHECK(racing.size() == kCount);
    }

    // 7. 15자 넘는 티커는 잘린 채 저장된다 — 같은 앞 15자는 같은 id.
    {
        symbol::SymbolTable truncating;
        const auto          long_id = truncating.intern("ABCDEFGHIJKLMNOPQR");
        CHECK(long_id != symbol::kNone);
        CHECK(truncating.intern("ABCDEFGHIJKLMNO") == long_id);
        CHECK(truncating.name(long_id) == "ABCDEFGHIJKLMNO");
    }

    std::cout << "test_symbol_table: " << g_checks << " checks passed\n";
    return 0;
}
