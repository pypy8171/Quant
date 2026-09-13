// SymbolTable 단위 테스트 — id 부여 순서, 재조회 동일성, 상한, 스레드 여럿이 같은 종목을 동시에 넣을 때의 일관성.
// 빌드: cmake --build <dir> --target test_symbol_table
#include "core/SymbolTable.h"

#include <iostream>
#include <string>
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

} // namespace

int main()
{
    // 1. 빈 테이블: 모르는 종목은 kNone, 이름은 빈 문자열.
    sym::SymbolTable t;
    CHECK(t.size() == 0);
    CHECK(t.lookup("005930") == sym::kNone);
    CHECK(t.name(sym::kNone).empty());
    CHECK(t.name(99).empty());

    // 2. 등록 순서대로 1부터, 같은 종목은 같은 id, 이름 왕복.
    const auto a = t.intern("005930");
    const auto b = t.intern("000660");
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(t.intern("005930") == a);
    CHECK(t.lookup("000660") == b);
    CHECK(t.name(a) == "005930");
    CHECK(t.size() == 2);

    // 3. string_view로 찾아도 복사 없이 같은 답.
    const std::string     s = "005930";
    const std::string_view v(s.data(), 6);
    CHECK(t.lookup(v) == a);

    // 4. 상한: capacity 4면 id 1~3까지, 넷째는 kNone이고 기존 것은 그대로.
    sym::SymbolTable small(4);
    CHECK(small.intern("A") == 1);
    CHECK(small.intern("B") == 2);
    CHECK(small.intern("C") == 3);
    CHECK(small.intern("D") == sym::kNone);
    CHECK(small.intern("B") == 2);
    CHECK(small.size() == 3);

    // 5. 스레드 4개가 같은 200종목을 동시에 넣어도 종목마다 id가 하나고 총 200개다.
    sym::SymbolTable         shared;
    std::vector<std::thread> ths;
    std::vector<std::vector<sym::SymbolId>> seen(4, std::vector<sym::SymbolId>(200));

    for (int k = 0; k < 4; ++k)
    {
        ths.emplace_back(
            [&, k]
            {
                for (int i = 0; i < 200; ++i)
                {
                    seen[k][i] = shared.intern("T" + std::to_string((i * 7 + k * 13) % 200));
                }
            });
    }

    for (auto& th : ths)
    {
        th.join();
    }

    CHECK(shared.size() == 200);

    for (int i = 0; i < 200; ++i)
    {
        const std::string name = "T" + std::to_string(i);
        const auto        id   = shared.lookup(name);
        CHECK(id != sym::kNone);
        CHECK(shared.name(id) == name);
    }

    for (int k = 0; k < 4; ++k)
    {
        for (int i = 0; i < 200; ++i)
        {
            const std::string name = "T" + std::to_string((i * 7 + k * 13) % 200);
            CHECK(seen[k][i] == shared.lookup(name));
        }
    }

    std::cout << "test_symbol_table: " << g_checks << " checks passed\n";
    return 0;
}
