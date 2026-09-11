// 매크로 국면 파일 판정기(core/RegimeFileBridge.h) 단위 테스트. entry_halt 전이 1회 로그·stale 1회 경고·
// 판정 보류(valid=false) 불변·시간 상자(개장 후 N분, 하루 리셋, 파장 뒤 무효, force_liquidate 제외)·
// force_liquidate 플래그의 "그대로 둔다" 규칙·JSON 형 불량 처리를 고정한다. 헤더 전용이라 파일·로그 없이 돈다.
// 관련 결정: D-033(시간 상자), D-060(분리).
// 빌드: cmake --build <dir> --target test_regime_bridge
#include "core/RegimeFileBridge.h"

#include <cassert>
#include <iostream>

using namespace regime_bridge;
using nlohmann::json;

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

Observation fresh(bool valid, bool halt, bool liq)
{
    Observation o;
    o.state                = FileState::kFresh;
    o.snap.valid           = valid;
    o.snap.entry_halt      = halt;
    o.snap.force_liquidate = liq;
    return o;
}

Observation stale(long long age)
{
    Observation o;
    o.state   = FileState::kStale;
    o.age_sec = age;
    return o;
}

// 개장 후 m분(음수는 개장 전), 같은 날.
KstClock at(int m, int yday = 100)
{
    return KstClock{yday, m};
}

bool quiet(const Outcome& o)
{
    return !o.entry_halt && !o.force_liquidate && !o.log_expiry && !o.log_stale && !o.log_halt_transition &&
           !o.log_liq_on && !o.log_liq_off;
}

int test_parse_snapshot()
{
    Snapshot s = parse_snapshot(json{{"valid", true}, {"entry_halt", true}, {"force_liquidate", false},
                                     {"regime", "BEAR"}, {"risk_score", 3}});
    CHECK(s.valid && s.entry_halt && !s.force_liquidate && s.regime == "BEAR" && s.risk_score == 3);

    // 키 없음·형 불량은 기본값 — "true" 문자열·"3" 문자열은 없는 것으로 본다(예외 없음).
    Snapshot d = parse_snapshot(json{{"valid", "true"}, {"entry_halt", 1}, {"regime", 7}, {"risk_score", "3"}});
    CHECK(!d.valid && !d.entry_halt && !d.force_liquidate && d.regime == "?" && d.risk_score == 0);
    CHECK(!parse_snapshot(json::object()).valid);
    return 0;
}

int test_halt_transition()
{
    RegimeFileBridge b;
    b.set_halt_expire_min(0); // 시간 상자 끔
    Outcome o = b.step(fresh(true, true, false), at(10));
    CHECK(o.entry_halt && *o.entry_halt && o.log_halt_transition && b.halt_on());
    CHECK(o.force_liquidate && !*o.force_liquidate);

    // 같은 값이 다시 오면 set·로그 없음.
    o = b.step(fresh(true, true, false), at(11));
    CHECK(!o.entry_halt && !o.log_halt_transition && o.force_liquidate && !*o.force_liquidate);

    o = b.step(fresh(true, false, false), at(12));
    CHECK(o.entry_halt && !*o.entry_halt && o.log_halt_transition && !b.halt_on());
    return 0;
}

int test_missing_stale_invalid_keep_gate()
{
    RegimeFileBridge b;
    b.set_halt_expire_min(0);
    (void) b.step(fresh(true, true, false), at(10));
    CHECK(b.halt_on());

    // 파일 없음·읽기 실패·판정 보류 — 게이트도 force_liquidate도 그대로.
    Observation missing;
    CHECK(quiet(b.step(missing, at(11))) && b.halt_on());
    Observation bad;
    bad.state = FileState::kUnreadable;
    CHECK(quiet(b.step(bad, at(12))) && b.halt_on());
    CHECK(quiet(b.step(fresh(false, false, false), at(13))) && b.halt_on());

    // stale은 첫 진입에만 경고, 신선한 파일이 오면 경고 상태가 풀려 다음 stale에 다시 경고.
    Outcome o = b.step(stale(700), at(14));
    CHECK(o.log_stale && !o.entry_halt && b.halt_on());
    CHECK(quiet(b.step(stale(730), at(15))));
    (void) b.step(fresh(true, true, false), at(16));
    CHECK(b.step(stale(800), at(17)).log_stale);
    return 0;
}

int test_time_box()
{
    RegimeFileBridge b;
    b.set_halt_expire_min(60);

    // 개장 후 10분: halt 걸림. 59분: 아직. 60분: 만료 — 해제 + 하루 1회 로그.
    Outcome o = b.step(fresh(true, true, false), at(10));
    CHECK(o.entry_halt && *o.entry_halt);
    CHECK(!b.step(fresh(true, true, false), at(59)).entry_halt && b.halt_on());
    o = b.step(fresh(true, true, false), at(60));
    CHECK(o.entry_halt && !*o.entry_halt && o.log_expiry && !b.halt_on());

    // 만료 뒤 파일이 계속 halt를 말해도 다시 걸지 않고, 로그도 다시 안 찍는다.
    o = b.step(fresh(true, true, false), at(61));
    CHECK(!o.entry_halt && !o.log_expiry && !b.halt_on());

    // 파일이 없어도 진입부에서 푼다(09-10 실패 모양). 먼저 새 날로 halt를 다시 건다.
    (void) b.step(fresh(true, true, false), at(5, 101));
    CHECK(b.halt_on());
    Observation missing;
    o = b.step(missing, at(70, 101));
    CHECK(o.entry_halt && !*o.entry_halt && o.log_expiry && !b.halt_on());

    // 파장 뒤(390분 이상)·개장 전(음수)은 만료가 성립하지 않는다.
    (void) b.step(fresh(true, true, false), at(5, 102));
    CHECK(!b.step(fresh(true, true, false), at(395, 102)).entry_halt && b.halt_on());
    CHECK(!b.step(fresh(true, true, false), at(-30, 103)).entry_halt && b.halt_on());
    return 0;
}

int test_force_liquidate()
{
    RegimeFileBridge b;
    b.set_halt_expire_min(60);

    // entry_halt=false여도 force_liquidate면 halt를 건다. 켜짐 로그 1회.
    Outcome o = b.step(fresh(true, false, true), at(10));
    CHECK(o.entry_halt && *o.entry_halt && o.log_liq_on && o.force_liquidate && *o.force_liquidate);
    o = b.step(fresh(true, false, true), at(11));
    CHECK(!o.log_liq_on && !o.entry_halt);

    // 청산 중에는 시간 상자가 풀지 않는다 — 파일이 있어도, 없어도.
    o = b.step(fresh(true, false, true), at(90));
    CHECK(!o.entry_halt && !o.log_expiry && b.halt_on());
    Observation missing;
    o = b.step(missing, at(91));
    CHECK(quiet(o) && b.halt_on());

    // 해제: 꺼짐 로그 1회 + halt 전이. 그 뒤 90분이라 halt는 다시 안 걸린다.
    o = b.step(fresh(true, false, false), at(92));
    CHECK(o.log_liq_off && o.entry_halt && !*o.entry_halt && o.force_liquidate && !*o.force_liquidate);
    CHECK(!b.step(fresh(true, true, false), at(93)).entry_halt && !b.halt_on());
    return 0;
}

int test_stale_sec_setter()
{
    RegimeFileBridge b;
    CHECK(b.stale_sec() == kDefaultRegimeStaleSec);
    b.set_stale_sec(0); // 0 이하는 무시
    CHECK(b.stale_sec() == kDefaultRegimeStaleSec);
    b.set_stale_sec(120);
    CHECK(b.stale_sec() == 120);
    return 0;
}
} // namespace

int main()
{
    if (test_parse_snapshot() || test_halt_transition() || test_missing_stale_invalid_keep_gate() || test_time_box() ||
        test_force_liquidate() || test_stale_sec_setter())
    {
        return 1;
    }

    std::cout << "test_regime_bridge: " << g_checks << " checks passed\n";
    return 0;
}
