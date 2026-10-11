// test_engine — 유니버스 점수 순수 함수 — spread 상한·동점 순서·시세 표 다시 채우기.
//  공용 준비는 test_engine_support.h, 실행 순서는 test_engine.cpp의 main.
#include "test_engine_support.h"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace engine_test
{

// spread가 1을 넘어도 배수가 음수가 되지 않는다. 음수가 나오면 팩토리가 0 이하를 버리고 기본 배수 1.0으로 되돌아가
//  점수 최하위 종목을 도리어 크게 산다. 1로 자른 결과와 같아야 한다.
int run_score_spread_case()
{
    symbol::SymbolTable symbols(64);
    universe::ScoreList scores;

    for (int index = 0; index < 6; ++index)
    {
        scores.push_back({symbols.intern("00000" + std::to_string(index)), static_cast<double>(index)});
    }

    scores.push_back({symbols.intern("000009"), -40.0}); // 하위로 크게 떨어진 종목 — z가 −2에 걸린다

    std::vector<double> multiplier;
    std::vector<double> sorted_raw;
    universe::score_to_mult(scores, 3.0, 0.8, 0.05, 5, multiplier, sorted_raw);
    CHECK(multiplier.size() == scores.size());

    for (const double value : multiplier)
    {
        CHECK(value >= 0.0);
    }

    std::vector<double> clamped;
    universe::score_to_mult(scores, 1.0, 0.8, 0.05, 5, clamped, sorted_raw);

    for (size_t index = 0; index < multiplier.size(); ++index)
    {
        CHECK(std::abs(multiplier[index] - clamped[index]) < 1e-12);
    }

    // 같은 버퍼를 두 번째 호출에 넘겨도 앞 결과가 섞이지 않는다.
    universe::score_to_mult(universe::ScoreList{}, 0.6, 0.8, 0.05, 5, multiplier, sorted_raw);
    CHECK(multiplier.empty());
    return 0;
}

// 동점은 티커 사전순이다. 순위 계산과 등록 상한 자르기가 같은 비교(ranks_before)를 쓰므로 여기서 한 번 고정한다.
int run_score_tie_order_case()
{
    symbol::SymbolTable symbols(64);
    const symbol::SymbolId later   = symbols.intern("035720"); // 먼저 들어가 번호는 작지만 사전순은 뒤다
    const symbol::SymbolId earlier = symbols.intern("005930");
    const symbol::SymbolId top     = symbols.intern("000660");

    CHECK(universe::ranks_before(1.0, earlier, 1.0, later, symbols));
    CHECK(!universe::ranks_before(1.0, later, 1.0, earlier, symbols));
    CHECK(!universe::ranks_before(1.0, earlier, 1.0, earlier, symbols));
    CHECK(universe::ranks_before(2.0, later, 1.0, earlier, symbols));

    const universe::ScoreList scores = {{later, 1.0}, {earlier, 1.0}, {top, 2.0}};
    std::vector<int>    rank;
    std::vector<size_t> order;
    universe::score_to_rank(scores, symbols, rank, order);
    CHECK(rank.size() == 3);
    CHECK(rank[2] == 1);
    CHECK(rank[1] == 2);
    CHECK(rank[0] == 3);
    return 0;
}

// 시세 표를 재스캔 사이에 이어 쓸 때, 이번 판에 없는 종목 칸은 비워진다(옛 가격·이름이 남지 않는다).
int run_quote_table_reload_case()
{
    symbol::SymbolTable  symbols(64);
    universe::QuoteTable quotes;
    universe::BoardSnapshot board;
    board.quotes = {{.code = "005930", .name = "삼성전자", .price = 70000, .volume = 10, .value = 1000},
                    {.code = "000660", .name = "SK하이닉스", .price = 200000, .volume = 20, .value = 2000}};
    board.received_at = std::time(nullptr);
    universe::load_quote_table(board, quotes, symbols);
    const symbol::SymbolId samsung = symbols.lookup("005930");
    const symbol::SymbolId hynix   = symbols.lookup("000660");
    CHECK(samsung != symbol::kNone);
    CHECK(hynix != symbol::kNone);
    CHECK(quotes.size() == symbols.capacity());
    CHECK(quotes[hynix].price == 200000.0);
    CHECK(quotes[hynix].name == "SK하이닉스");

    board.quotes = {{.code = "005930", .name = "삼성전자", .price = 71000, .volume = 15, .value = 1500}};
    universe::load_quote_table(board, quotes, symbols);
    CHECK(quotes[samsung].price == 71000.0);
    CHECK(quotes[samsung].value == 1500.0);
    CHECK(quotes[samsung].name == "삼성전자");
    CHECK(quotes[hynix].price == 0.0);
    CHECK(quotes[hynix].value == 0.0);
    CHECK(quotes[hynix].volume == 0.0);
    CHECK(quotes[hynix].name.empty());

    // 판이 없으면(시세판 꺼짐·첫 판 전) 전 칸이 비워진다.
    universe::clear_quote_table(quotes, symbols);
    CHECK(quotes[samsung].price == 0.0);
    CHECK(quotes[samsung].name.empty());
    return 0;
}

} // namespace engine_test
