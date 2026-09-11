// 정규장 시각 판정(core/MarketSession.h) 단위 테스트. 틱마다 불리는 순수 함수라 경계값을 고정해 둔다.
// 관련 결정: D-037.
#include "core/MarketSession.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace krx;

    // parse_hhmm: 앞 4자리만 본다. HHMMSS도 HHMM으로 접힌다.
    assert(parse_hhmm("0930") == 930);
    assert(parse_hhmm("093015") == 930);
    assert(parse_hhmm("1530") == 1530);
    assert(parse_hhmm("0000") == 0);

    // 짧거나 숫자가 아니면 0 — 0은 장 밖이라 판정이 보수적으로 떨어진다.
    assert(parse_hhmm("") == 0);
    assert(parse_hhmm("930") == 0);
    assert(parse_hhmm("ab12") == 0);
    assert(parse_hhmm("09:3") == 0);

    // in_session: 09:00 포함, 15:30 제외.
    assert(!in_session(kSessionOpenHHMM - 1));
    assert(in_session(kSessionOpenHHMM));
    assert(in_session(1200));
    assert(in_session(kSessionCloseHHMM - 1));
    assert(!in_session(kSessionCloseHHMM));
    assert(!in_session(0));

    // 문자열 경로는 둘을 잇는다.
    assert(in_session_str("090000"));
    assert(!in_session_str("085959"));
    assert(!in_session_str("153000"));
    assert(!in_session_str(""));

    std::cout << "test_market_session: all passed" << std::endl;
    return 0;
}
