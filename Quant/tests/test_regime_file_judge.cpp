// 매크로 국면 파일 판정기(core/RegimeFileJudge.h) 단위 테스트. entry_halt 전이 1회 로그·stale 1회 경고·
// 판정 보류(valid=false) 불변·시간 상자(개장 후 N분, 하루 리셋, 파장 뒤 무효, force_liquidate 제외)·
// force_liquidate 플래그의 "그대로 둔다" 규칙·JSON 형 불량 처리를 고정한다. 헤더 전용이라 파일·로그 없이 돈다.
// 관련 결정: D-033(시간 상자), D-060(분리), D-083(매수 비율), D-084(전략 선택 라벨).
// 빌드: cmake --build <directory> --target test_regime_file_judge
#include "core/RegimeFileJudge.h"

#include <cassert>
#include <iostream>

using namespace regime_file;
using nlohmann::json;

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

Observation fresh(bool valid, bool halt, bool liquidation, std::optional<double> scale = std::nullopt)
{
    Observation observation;
    observation.state                = FileState::kFresh;
    observation.snapshot.valid           = valid;
    observation.snapshot.entry_halt      = halt;
    observation.snapshot.force_liquidate = liquidation;
    observation.snapshot.entry_scale     = scale;
    return observation;
}

Observation stale(long long age)
{
    Observation observation;
    observation.state   = FileState::kStale;
    observation.age_sec = age;
    return observation;
}

// 개장 후 m분(음수는 개장 전), 같은 날.
KstClock at(int row, int yesterday = 100)
{
    return KstClock{yesterday, row};
}

bool quiet(const Outcome& outcome)
{
    return !outcome.entry_halt && !outcome.force_liquidate && !outcome.log_expiry && !outcome.log_stale && !outcome.log_halt_transition &&
           !outcome.log_liquidation_on && !outcome.log_liquidation_off && !outcome.entry_scale && !outcome.log_scale_change && !outcome.selection;
}

Observation labeled(const char* label, bool valid = true)
{
    Observation observation = fresh(valid, false, false);
    observation.snapshot.regime  = label;
    return observation;
}

// 전략 선택 국면(D-084): 라벨이 바뀐 회차에만 실린다. stale·무효·모르는 라벨은 이전 선택 유지.
int test_selection()
{
    CHECK(selection_of("RISK_ON") == Regime::BULL && selection_of("NEUTRAL") == Regime::NEUTRAL &&
          selection_of("RISK_OFF") == Regime::BEAR && selection_of("UNKNOWN") == Regime::UNKNOWN &&
          selection_of("bull") == Regime::UNKNOWN);

    RegimeFileJudge bridge;
    CHECK(bridge.selection_now() == Regime::UNKNOWN);
    Outcome outcome = bridge.step(labeled("NEUTRAL"), at(10));
    CHECK(outcome.selection && *outcome.selection == Regime::NEUTRAL && bridge.selection_now() == Regime::NEUTRAL);
    outcome = bridge.step(labeled("NEUTRAL"), at(11)); // 같은 라벨은 다시 안 싣는다
    CHECK(!outcome.selection);
    outcome = bridge.step(labeled("RISK_ON"), at(12));
    CHECK(outcome.selection && *outcome.selection == Regime::BULL);
    outcome = bridge.step(labeled("UNKNOWN"), at(13)); // 판정 보류 라벨 → 유지
    CHECK(!outcome.selection && bridge.selection_now() == Regime::BULL);
    outcome = bridge.step(labeled("RISK_OFF", /*valid=*/false), at(14)); // 무효 파일 → 유지
    CHECK(!outcome.selection && bridge.selection_now() == Regime::BULL);
    outcome = bridge.step(stale(700), at(15));
    CHECK(!outcome.selection && bridge.selection_now() == Regime::BULL);
    Observation missing;
    CHECK(!bridge.step(missing, at(16)).selection);
    outcome = bridge.step(labeled("RISK_OFF"), at(17));
    CHECK(outcome.selection && *outcome.selection == Regime::BEAR);
    return 0;
}

// 매수 비율(D-083): 파일 값이 바뀐 회차에만 실리고, halt·청산이면 0, 없으면 1, 만료로 풀리면 1.
int test_entry_scale()
{
    RegimeFileJudge bridge;
    Outcome outcome = bridge.step(fresh(true, false, false, 0.7), at(10));
    CHECK(outcome.entry_scale && *outcome.entry_scale == 0.7 && outcome.log_scale_change && bridge.scale_now() == 0.7);
    outcome = bridge.step(fresh(true, false, false, 0.7), at(11));
    CHECK(!outcome.entry_scale && !outcome.log_scale_change);
    outcome = bridge.step(fresh(true, false, false, 0.44), at(12)); // 0.1 단위로 끊는다
    CHECK(outcome.entry_scale && *outcome.entry_scale == 0.4);
    outcome = bridge.step(fresh(true, true, false, 0.4), at(13));   // halt면 파일 값과 무관하게 0
    CHECK(outcome.entry_scale && *outcome.entry_scale == 0.0 && outcome.entry_halt && *outcome.entry_halt);
    outcome = bridge.step(fresh(true, false, false), at(14));       // 키 없음 → 1
    CHECK(outcome.entry_scale && *outcome.entry_scale == 1.0 && !bridge.halt_on());
    outcome = bridge.step(fresh(true, false, true, 0.9), at(15));   // 청산이면 0
    CHECK(outcome.entry_scale && *outcome.entry_scale == 0.0);
    outcome = bridge.step(fresh(false, false, false, 0.5), at(16)); // 무효면 비율도 불변
    CHECK(!outcome.entry_scale && bridge.scale_now() == 0.0);
    Observation missing;
    CHECK(!bridge.step(missing, at(17)).entry_scale);

    // 시간 상자로 halt가 풀리면 비율도 1로 돌아온다.
    RegimeFileJudge test_bridge;
    test_bridge.set_halt_expire_min(60);
    (void) test_bridge.step(fresh(true, true, false, 0.0), at(10));
    outcome = test_bridge.step(missing, at(60));
    CHECK(outcome.log_expiry && outcome.entry_scale && *outcome.entry_scale == 1.0);

    // 파싱: 숫자만 받고 0~1로 자른다. null·문자열은 없음.
    CHECK(parse_snapshot(json{{"entry_scale", 1.7}}).entry_scale == 1.0);
    CHECK(parse_snapshot(json{{"entry_scale", nullptr}}).entry_scale == std::nullopt);
    CHECK(parse_snapshot(json{{"entry_scale", "0.5"}}).entry_scale == std::nullopt);
    return 0;
}

int test_parse_snapshot()
{
    Snapshot parsed_snapshot = parse_snapshot(json{{"valid", true}, {"entry_halt", true}, {"force_liquidate", false},
                                     {"regime", "RISK_OFF"}, {"risk_score", 3}});
    CHECK(parsed_snapshot.valid && parsed_snapshot.entry_halt && !parsed_snapshot.force_liquidate && parsed_snapshot.regime == "RISK_OFF" && parsed_snapshot.risk_score == 3);

    // 키 없음·형 불량은 기본값 — "true" 문자열·"3" 문자열은 없는 것으로 본다(예외 없음).
    Snapshot snapshot = parse_snapshot(json{{"valid", "true"}, {"entry_halt", 1}, {"regime", 7}, {"risk_score", "3"}});
    CHECK(!snapshot.valid && !snapshot.entry_halt && !snapshot.force_liquidate && snapshot.regime == "?" && snapshot.risk_score == 0);
    CHECK(!parse_snapshot(json::object()).valid);
    return 0;
}

int test_halt_transition()
{
    RegimeFileJudge bridge;
    bridge.set_halt_expire_min(0); // 시간 상자 끔
    Outcome outcome = bridge.step(fresh(true, true, false), at(10));
    CHECK(outcome.entry_halt && *outcome.entry_halt && outcome.log_halt_transition && bridge.halt_on());
    CHECK(outcome.force_liquidate && !*outcome.force_liquidate);

    // 같은 값이 다시 오면 set·로그 없음.
    outcome = bridge.step(fresh(true, true, false), at(11));
    CHECK(!outcome.entry_halt && !outcome.log_halt_transition && outcome.force_liquidate && !*outcome.force_liquidate);

    outcome = bridge.step(fresh(true, false, false), at(12));
    CHECK(outcome.entry_halt && !*outcome.entry_halt && outcome.log_halt_transition && !bridge.halt_on());
    return 0;
}

int test_missing_stale_invalid_keep_gate()
{
    RegimeFileJudge bridge;
    bridge.set_halt_expire_min(0);
    (void) bridge.step(fresh(true, true, false), at(10));
    CHECK(bridge.halt_on());

    // 파일 없음·읽기 실패·판정 보류 — 게이트도 force_liquidate도 그대로.
    Observation missing;
    CHECK(quiet(bridge.step(missing, at(11))) && bridge.halt_on());
    Observation bad;
    bad.state = FileState::kUnreadable;
    CHECK(quiet(bridge.step(bad, at(12))) && bridge.halt_on());
    CHECK(quiet(bridge.step(fresh(false, false, false), at(13))) && bridge.halt_on());

    // stale은 첫 진입에만 경고, 신선한 파일이 오면 경고 상태가 풀려 다음 stale에 다시 경고.
    Outcome outcome = bridge.step(stale(700), at(14));
    CHECK(outcome.log_stale && !outcome.entry_halt && bridge.halt_on());
    CHECK(quiet(bridge.step(stale(730), at(15))));
    (void) bridge.step(fresh(true, true, false), at(16));
    CHECK(bridge.step(stale(800), at(17)).log_stale);
    return 0;
}

int test_time_box()
{
    RegimeFileJudge bridge;
    bridge.set_halt_expire_min(60);

    // 개장 후 10분: halt 걸림. 59분: 아직. 60분: 만료 — 해제 + 하루 1회 로그.
    Outcome outcome = bridge.step(fresh(true, true, false), at(10));
    CHECK(outcome.entry_halt && *outcome.entry_halt);
    CHECK(!bridge.step(fresh(true, true, false), at(59)).entry_halt && bridge.halt_on());
    outcome = bridge.step(fresh(true, true, false), at(60));
    CHECK(outcome.entry_halt && !*outcome.entry_halt && outcome.log_expiry && !bridge.halt_on());

    // 만료 뒤 파일이 계속 halt를 말해도 다시 걸지 않고, 로그도 다시 안 찍는다.
    outcome = bridge.step(fresh(true, true, false), at(61));
    CHECK(!outcome.entry_halt && !outcome.log_expiry && !bridge.halt_on());

    // 파일이 없어도 진입부에서 푼다(09-10 실패 모양). 먼저 새 날로 halt를 다시 건다.
    (void) bridge.step(fresh(true, true, false), at(5, 101));
    CHECK(bridge.halt_on());
    Observation missing;
    outcome = bridge.step(missing, at(70, 101));
    CHECK(outcome.entry_halt && !*outcome.entry_halt && outcome.log_expiry && !bridge.halt_on());

    // 파장 뒤(390분 이상)·개장 전(음수)은 만료가 성립하지 않는다.
    (void) bridge.step(fresh(true, true, false), at(5, 102));
    CHECK(!bridge.step(fresh(true, true, false), at(395, 102)).entry_halt && bridge.halt_on());
    CHECK(!bridge.step(fresh(true, true, false), at(-30, 103)).entry_halt && bridge.halt_on());
    return 0;
}

int test_force_liquidate()
{
    RegimeFileJudge bridge;
    bridge.set_halt_expire_min(60);

    // entry_halt=false여도 force_liquidate면 halt를 건다. 켜짐 로그 1회.
    Outcome outcome = bridge.step(fresh(true, false, true), at(10));
    CHECK(outcome.entry_halt && *outcome.entry_halt && outcome.log_liquidation_on && outcome.force_liquidate && *outcome.force_liquidate);
    outcome = bridge.step(fresh(true, false, true), at(11));
    CHECK(!outcome.log_liquidation_on && !outcome.entry_halt);

    // 청산 중에는 시간 상자가 풀지 않는다 — 파일이 있어도, 없어도.
    outcome = bridge.step(fresh(true, false, true), at(90));
    CHECK(!outcome.entry_halt && !outcome.log_expiry && bridge.halt_on());
    Observation missing;
    outcome = bridge.step(missing, at(91));
    CHECK(quiet(outcome) && bridge.halt_on());

    // 해제: 꺼짐 로그 1회 + halt 전이. 그 뒤 90분이라 halt는 다시 안 걸린다.
    outcome = bridge.step(fresh(true, false, false), at(92));
    CHECK(outcome.log_liquidation_off && outcome.entry_halt && !*outcome.entry_halt && outcome.force_liquidate && !*outcome.force_liquidate);
    CHECK(!bridge.step(fresh(true, true, false), at(93)).entry_halt && !bridge.halt_on());
    return 0;
}

int test_stale_sec_setter()
{
    RegimeFileJudge bridge;
    CHECK(bridge.stale_sec() == kDefaultRegimeStaleSec);
    bridge.set_stale_sec(0); // 0 이하는 무시
    CHECK(bridge.stale_sec() == kDefaultRegimeStaleSec);
    bridge.set_stale_sec(120);
    CHECK(bridge.stale_sec() == 120);
    return 0;
}
} // namespace

int main()
{
    if (test_parse_snapshot() || test_halt_transition() || test_missing_stale_invalid_keep_gate() || test_time_box() ||
        test_force_liquidate() || test_stale_sec_setter() || test_entry_scale() || test_selection())
    {
        return 1;
    }

    std::cout << "test_regime_file_judge: " << g_checks << " checks passed\n";
    return 0;
}
