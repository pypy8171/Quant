// RegimeController 단위 테스트 (KIS 불필요) — 점수/분류 순수 함수와, 가짜 시세 소스(IMarketDataSource)를 꽂은
//  evaluate()의 당일봉 제외·확정봉 부족·실패 재시도·같은 날 캐시. 관련 결정: D-066.
// 빌드: cmake --build <dir> --target test_regime
//
// v0 규칙: 축1(지수>200일선 ±1) + 축2(정배열+1/역배열-1/혼조0) → score ∈ {-2..+2}
//   BULL(+2) / BEAR(-2) / NEUTRAL(그 외)

#include "api/IMarketDataSource.h"
#include "core/RegimeController.h"
#include "utils/Logger.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

static RegimeSnapshot snap(bool above200, bool bull, bool bear)
{
    RegimeSnapshot s;
    s.above_ma200  = above200;
    s.aligned_bull = bull;
    s.aligned_bear = bear;
    return s;
}

static void PASS(const std::string& n) { std::cout << "[PASS] " << n << "\n"; }

// 지수 일봉만 돌려주는 가짜 소스. 나머지는 쓰이지 않는다.
struct FakeSource : IMarketDataSource
{
    std::vector<MarketData> bars;
    int                     calls = 0;

    double get_current_price(const std::string&) override { return 0.0; }
    std::vector<MarketData> get_daily_ohlcv(const std::string&, int, bool) override { return {}; }
    std::vector<MarketData> get_minute_ohlcv(const std::string&, int, int) override { return {}; }
    std::vector<MarketData> get_index_daily_ohlcv(const std::string&, int) override
    {
        ++calls;
        return bars;
    }

    IndexPrice get_index_price(const std::string&) override { return {}; }
    std::vector<MarketData> get_us_daily_ohlcv(const std::string&, int, const std::string&) override { return {}; }
};

// 최신→과거 n봉. close[i] = first + step*i. bars[0]는 오늘(KST) 시각이라 evaluate()가 미완성봉으로 뺀다.
//  today_kst()가 UTC+9를 더한 뒤 UTC 날짜를 읽으므로, 같은 식(now+9h)의 UTC 날짜가 곧 오늘이다.
static std::vector<MarketData> series(int n, double first, double step)
{
    using namespace std::chrono;
    const auto today = system_clock::now() + hours(9);
    std::vector<MarketData> v(n);

    for (int i = 0; i < n; ++i)
    {
        v[i].close     = first + step * i;
        v[i].timestamp = today - hours(24) * i;
    }

    return v;
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_pacer와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* env = std::getenv("QUANT_LOG_DIR"); !env || !*env)
    {
        Logger::instance().set_base_dir(Logger::executable_dir() / "logs_test");
    }

    std::cout << "=== RegimeController Unit Tests ===\n";
    RegimeController::Config cfg; // bull_th=+2, bear_th=-2

    // ── 점수 계산 ─────────────────────────────────────────────────────────
    assert(RegimeController::compute_score(snap(true,  true,  false), cfg) ==  2); // +1+1
    assert(RegimeController::compute_score(snap(false, false, true ), cfg) == -2); // -1-1
    assert(RegimeController::compute_score(snap(true,  false, false), cfg) ==  1); // +1+0 혼조
    assert(RegimeController::compute_score(snap(false, false, false), cfg) == -1); // -1+0
    assert(RegimeController::compute_score(snap(true,  false, true ), cfg) ==  0); // +1-1
    assert(RegimeController::compute_score(snap(false, true,  false), cfg) ==  0); // -1+1
    PASS("compute_score");

    // ── 분류 (임계 ±2) ───────────────────────────────────────────────────
    assert(RegimeController::classify( 2, cfg) == Regime::BULL);
    assert(RegimeController::classify(-2, cfg) == Regime::BEAR);
    assert(RegimeController::classify( 1, cfg) == Regime::NEUTRAL);
    assert(RegimeController::classify( 0, cfg) == Regime::NEUTRAL);
    assert(RegimeController::classify(-1, cfg) == Regime::NEUTRAL);
    PASS("classify");

    // ── 만장일치만 강한 국면 (경합 쟁점3: 보수성 확인) ────────────────────
    // 전항목 동의 시에만 BULL/BEAR, 하나라도 어긋나면 NEUTRAL
    assert(RegimeController::classify(
        RegimeController::compute_score(snap(true, true, false), cfg), cfg) == Regime::BULL);
    assert(RegimeController::classify(
        RegimeController::compute_score(snap(true, false, false), cfg), cfg) == Regime::NEUTRAL);
    PASS("conservative_unanimous");

    // ── 임계값 오버라이드 (D-15b 드릴 경로) ───────────────────────────────
    // config "regime_tuning"으로 임계값을 낮추면 실제 시장 점수 그대로 BULL/BEAR에
    //  도달한다. 2026-09-07 코스피 실측 score=1(혼조)이 이 두 설정의 기준점이다.
    RegimeController::Config bull_th1 = cfg; bull_th1.score_bull_threshold = 1;
    assert(RegimeController::classify(1, bull_th1) == Regime::BULL);
    assert(RegimeController::classify(0, bull_th1) == Regime::NEUTRAL);

    RegimeController::Config bear_th1 = cfg; bear_th1.score_bear_threshold = 1;
    // classify()가 BULL을 먼저 보므로, bull은 기본 +2로 두어야 score=1이 BEAR로 떨어진다.
    assert(bear_th1.score_bull_threshold == 2);
    assert(RegimeController::classify(1, bear_th1) == Regime::BEAR);
    assert(RegimeController::classify(2, bear_th1) == Regime::BULL);
    PASS("threshold_override");

    // ── evaluate(): 가짜 소스 ─────────────────────────────────────────────
    // 소스가 없으면 첫 실패 → NEUTRAL 안전판(W1), 스냅샷은 값 없음
    {
        RegimeController rc;
        auto s = rc.evaluate();
        assert(s.regime == Regime::NEUTRAL && s.index_close == 0.0);
        assert(rc.current() == Regime::NEUTRAL);
        PASS("evaluate_no_source");
    }

    // 상승 계열(최신이 가장 높다): 오늘봉을 빼고 close=998 > ma200=799, ma20>ma60>ma120 → BULL
    {
        FakeSource src;
        src.bars = series(211, 1000.0, -2.0);
        src.bars[0].close = 1.0; // 오늘 미완성봉은 이상값이어도 판정에 들어가지 않아야 한다
        RegimeController rc;
        rc.set_source(&src);
        auto s = rc.evaluate();
        assert(s.regime == Regime::BULL && s.score == 2);
        assert(s.index_close == 998.0 && s.ma200 == 799.0 && s.ma20 == 979.0 && s.ma60 == 939.0 &&
               s.ma120 == 879.0);
        assert(s.above_ma200 && s.aligned_bull && !s.aligned_bear);
        assert(rc.current() == Regime::BULL);
        // 같은 날 두 번째 호출은 캐시 — 소스를 다시 부르지 않는다
        auto s2 = rc.evaluate();
        assert(s2.regime == Regime::BULL && src.calls == 1);
        PASS("evaluate_bull_today_excluded_cached");
    }

    // 하락 계열: close=402 < ma200=601, 역배열 → BEAR. 수평 계열: 종가=ma200(초과 아님)·혼조 → -1 → NEUTRAL
    {
        FakeSource src;
        src.bars = series(211, 400.0, +2.0);
        RegimeController rc;
        rc.set_source(&src);
        auto s = rc.evaluate();
        assert(s.regime == Regime::BEAR && s.score == -2 && !s.above_ma200 && s.aligned_bear);

        FakeSource flat;
        flat.bars = series(211, 500.0, 0.0);
        RegimeController rc2;
        rc2.set_source(&flat);
        auto f = rc2.evaluate();
        assert(f.regime == Regime::NEUTRAL && f.score == -1 && !f.aligned_bull && !f.aligned_bear);
        PASS("evaluate_bear_and_flat");
    }

    // 실패(빈 응답·확정봉 부족)는 캐시되지 않아 다음 호출이 다시 묻고, 성공하면 그때부터 캐시된다
    {
        FakeSource src;
        RegimeController rc;
        rc.set_source(&src);
        auto s = rc.evaluate();
        assert(s.regime == Regime::NEUTRAL && s.index_close == 0.0 && src.calls == 1);
        src.bars = series(150, 1000.0, -2.0); // 오늘봉 빼면 149 < 200
        s = rc.evaluate();
        assert(s.regime == Regime::NEUTRAL && s.index_close == 0.0 && src.calls == 2);
        src.bars = series(211, 1000.0, -2.0);
        s = rc.evaluate();
        assert(s.regime == Regime::BULL && src.calls == 3);
        s = rc.evaluate();
        assert(s.regime == Regime::BULL && src.calls == 3);
        PASS("evaluate_fail_retry_then_cache");
    }

    Logger::instance().flush();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
