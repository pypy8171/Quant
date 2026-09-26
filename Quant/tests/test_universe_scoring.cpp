// 유니버스 횡단면 점수(universe::detail::score_cross_section) 단위 테스트. 입력만 보는 순수 함수라 z-score 정규화·
//  ±2 절단·눌림 부호 반전·거래대금 결측 중앙값 대체·가중합을 손으로 계산한 값에 고정해 둔다. 관련 결정: D-018.
//  후보 수집의 거래대금 상위 축(take_turnover_top)도 시세 표만 보는 함수라 여기서 고정한다. 관련 결정: D-146.
#include "../src/universe/detail/Pipeline.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
using universe::DevScanCfg;
using universe::detail::Features;
using universe::detail::score_cross_section;
using universe::detail::CandidateSet;
using universe::detail::take_turnover_top;

bool near(double actual, double expected)
{
    return std::fabs(actual - expected) < 1e-9;
}

Features make_features(double trend, double pull, double atr_percent, double turnover)
{
    return Features{0, trend, pull, atr_percent, turnover, 0.0};
}

// 하나만 통과했거나 전 종목 값이 같으면 정규화할 분포가 없어 점수가 모두 0이다.
void check_degenerate()
{
    const DevScanCfg config;
    std::vector<Features> single{make_features(0.3, -0.1, 0.02, 1e9)};
    score_cross_section(config, single);
    assert(near(single[0].score, 0.0));

    std::vector<Features> same{make_features(0.1, -0.05, 0.03, 1e9), make_features(0.1, -0.05, 0.03, 1e9)};
    score_cross_section(config, same);
    assert(near(same[0].score, 0.0) && near(same[1].score, 0.0));
}

// 두 종목이면 z는 ±1이다. 추세는 그대로, 눌림은 부호를 뒤집고, 변동성은 감점으로 들어간다.
void check_weighted_sum()
{
    DevScanCfg config;
    config.score_weight_trend    = 1.0;
    config.score_weight_pullback = 2.0;
    config.score_weight_volume   = 0.5;

    // 첫 종목: 추세 높음(z=+1), 눌림 깊음(pull 작음 → 반전 z=+1), 변동성 높음(z=+1 → 감점).
    std::vector<Features> passed{make_features(0.2, -0.1, 0.04, 0.0), make_features(0.1, 0.1, 0.02, 0.0)};
    score_cross_section(config, passed);
    assert(near(passed[0].score, 1.0 + 2.0 - 0.5));
    assert(near(passed[1].score, -1.0 - 2.0 + 0.5));
}

// 열 종목 중 하나만 10이면 평균 1·표준편차 3이라 그 종목의 z는 3 — 2로 잘린다. 나머지는 -1/3.
void check_clip()
{
    DevScanCfg config;
    config.score_weight_pullback = 0.0;
    config.score_weight_volume   = 0.0;

    std::vector<Features> passed(10, make_features(0.0, 0.0, 0.0, 0.0));
    passed[9].trend = 10.0;
    score_cross_section(config, passed);
    assert(near(passed[9].score, 2.0));
    assert(near(passed[0].score, -1.0 / 3.0));
}

// 유동성 축은 가중치가 0이 아닐 때만 돈다. 거래대금은 로그를 취하고, 0(결측)은 알려진 값의 중앙값으로 채운다.
void check_liquidity()
{
    DevScanCfg config;
    config.score_weight_trend    = 0.0;
    config.score_weight_pullback = 0.0;
    config.score_weight_volume   = 0.0;

    std::vector<Features> untouched{make_features(0.0, 0.0, 0.0, 1e8), make_features(0.0, 0.0, 0.0, 1e10)};
    score_cross_section(config, untouched);
    assert(near(untouched[0].turnover, 1e8)); // 가중치 0이면 거래대금을 건드리지 않는다
    assert(near(untouched[0].score, 0.0));

    config.score_weight_liquidity = 1.0;

    // 로그 거래대금 ln1e8·ln1e10, 결측 하나는 중앙값(정렬 후 [1] = ln1e10)으로 → 값 {a, b, b}.
    std::vector<Features> passed{make_features(0.0, 0.0, 0.0, 1e8), make_features(0.0, 0.0, 0.0, 1e10),
                                 make_features(0.0, 0.0, 0.0, 0.0)};
    score_cross_section(config, passed);
    assert(near(passed[2].turnover, std::log(1e10)));
    // {a, b, b}의 z는 {-√2, 1/√2, 1/√2}.
    assert(near(passed[0].score, -std::sqrt(2.0)));
    assert(near(passed[1].score, 1.0 / std::sqrt(2.0)));
    assert(near(passed[2].score, 1.0 / std::sqrt(2.0)));
}
} // namespace

// 거래대금 상위 축: 가격·거래대금 하한·ETF 이름에 걸린 종목은 순위에서 빠지고 N에도 세지 않는다.
//  거래대금이 같으면 티커 순이다. market_map이 있으면 사전에 없는 종목은 보지 않는다.
void check_turnover_top()
{
    symbol::SymbolTable symbols(16);
    const symbol::SymbolId first  = symbols.intern("000001");
    const symbol::SymbolId second = symbols.intern("000002");
    const symbol::SymbolId cheap  = symbols.intern("000003");
    const symbol::SymbolId etf    = symbols.intern("000004");
    const symbol::SymbolId tie    = symbols.intern("000005");
    const symbol::SymbolId thin   = symbols.intern("000006");
    universe::QuoteTable quotes(symbols.capacity());
    quotes[first]  = {10000.0, 5e9, 0.0, "가나전자"};
    quotes[second] = {20000.0, 9e9, 0.0, "다라화학"};
    quotes[cheap]  = {3000.0, 1e10, 0.0, "싼종목"};      // min_price 5000 미만
    quotes[etf]    = {10000.0, 8e9, 0.0, "KODEX 200"};   // ETF 이름
    quotes[tie]    = {10000.0, 5e9, 0.0, "마바건설"};      // first와 거래대금 같음, 티커가 뒤
    quotes[thin]   = {10000.0, 1e8, 0.0, "얇은종목"};      // min_turnover 미만

    DevScanCfg config;
    config.min_turnover   = 1e9;
    config.turnover_top_n = 2;

    CandidateSet candidates(symbols.capacity());
    take_turnover_top(config, quotes, candidates, symbols);
    assert((candidates.symbols == std::vector<symbol::SymbolId>{second, first}));
    assert(candidates.etf_drop == 1);

    CandidateSet listed(symbols.capacity());
    listed.set_market(second, universe::detail::Market::Kospi);
    listed.have_market_map = true;
    take_turnover_top(config, quotes, listed, symbols);
    assert((listed.symbols == std::vector<symbol::SymbolId>{second}));

    config.turnover_top_n = 0;
    CandidateSet off(symbols.capacity());
    take_turnover_top(config, quotes, off, symbols);
    assert(off.symbols.empty());
}

int main()
{
    check_degenerate();
    check_weighted_sum();
    check_clip();
    check_liquidity();
    check_turnover_top();
    std::cout << "test_universe_scoring: all passed" << std::endl;
    return 0;
}
