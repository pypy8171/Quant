// OrderGate 단위 테스트
// 빌드: cmake --build <directory> --target test_order_gate
// 실행: ./test_order_gate
//
// 테스트 항목:
//   1. Kill switch 차단
//   2. 포지션 한도 초과 차단
//   3. 일일 손실 한도 차단
//   4. 초당 Rate limit 차단
//   5. 중복 신호 차단
//   6. 정상 주문 통과
//   7. SELL은 포지션 한도 미적용
//   8. 중복 신호는 rate slot 소모 안 함
//  18. 바스켓 슬롯 제외 — 제외 종목은 동시보유 상한·빈 슬롯 수·교체 후보에 안 들고 snapshot에 표시된다 (D-109)
//  17. 시장가 1주문 명목 백스톱 — BUY는 ref_price로 거부, SELL은 경고만 하고 통과,
//      reference_price 없으면 검사 자체가 없음(게이트가 못 잡는 현행을 기록)
//  22. 진입 정지 원천 셋(국면·사람·전략 사망)이 서로를 안 지운다 (D-114)
//      원장만 보는 테스트(9·10·19~22)는 tests/test_position_ledger.cpp
//  23. 장부 사본이 원본과 같은 값을 싣는다 — 보유·선점·매도가능·평단·면제·전역값 전부 (D-114)
//  24. 판정은 거부 코드와 숫자만 돌려주고 문장은 describe가 만든다
//  25. 하루 리셋은 거래일당 한 번 — 장중 재기동·US 개장이 되살린 선점과 당일 손익을 안 지운다 (전수조사 A-4)
//  26. 총노출(§3d)은 보유를 잔고 현재가로 잰다 — 오른 보유분이 한도에서 빠지지 않고, 현재가가 없으면 평단 (전수조사 B1-6)
//
// 시험 본문은 주제별 test_order_gate_<주제>.cpp, 공용 도우미는 test_order_gate_support.h/.cpp에 있다.
//  main은 주제 묶음(run_<주제>_tests)을 원래 시험 순서대로 부른다.

#include "test_order_gate_support.h"

#include <iostream>

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== OrderGate Unit Tests ===\n";
    run_basic_tests();
    run_displace_tests();
    run_session_tests();
    run_ledger_tests();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
