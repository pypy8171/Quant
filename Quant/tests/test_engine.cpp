// Engine 한 바퀴 단위 테스트. 시험용 피드 소스를 주입해 KIS·소켓 없이 틱→샤드→전략→디스패치→주문 스레드→모의 체결→
// 체결 소비 스레드→원장(보유)까지 도는지, 주입 모드가 브로커 없이 기동·종료하는지 고정한다. 케이스는 둘 — 수신 스레드 1×샤드 1과
// 수신 스레드 2×샤드 2(종목 둘이 서로 다른 수신 스레드에서 들어와 서로 다른 열에서 판단된다). 관련 결정: D-071(Phase 3·Phase 4 앞단계).
// 스레드: 테스트 스레드가 피드 소스의 수신 스레드 역할(수신 스레드 0..N-1)을 하고 나머지는 Engine이 띄운다.
// 케이스 하나 더 — 보호 주문 한 주기를 두 스레드가 같이 잡지 못하는지(D-114 전략 사망 마무리).
// 케이스 하나 더 — 티커가 종목 번호가 되는 자리 둘(D-114 단계 4): 넣는 쪽은 주문 프로세스 하나고, 잦은 자리는 없는 티커를 만들지 않는다.
// 케이스 하나 더 — 주문 쪽 스위치 다섯(D-114 단계 4): 전략 역할이면 제어 요청을 거쳐 주문 스레드가 고친다.
// 케이스 하나 더 — 역할대로 제 스레드만 띄우는지(D-114 단계 4): 전략 역할은 주문 스레드가 없고, 주문 역할은 전략을 올리지 않는다.
// 케이스 셋 더 — 유니버스 점수 쪽 순수 함수: 비중 배수의 spread 상한, 동점 순서, 시세 표를 다시 채울 때 옛 값 비우기.
// 케이스 하나 더 — 자물쇠를 잡고 바뀌는 목록을 한 스레드가 사본으로 순회하는지(GenerationCopy, 데이터 스레드 감시 목록).
// 빌드: cmake --build <directory> --target test_engine
// 케이스 본문은 주제별 test_engine_<주제>.cpp, 공용 피드·전략·CHECK는 test_engine_support.h/.cpp에 있다.
#include "test_engine_support.h"

#include <iostream>

using namespace engine_test;

int main()
{
    // 산출물은 실행파일 옆 logs_test/ — 기본 logs/는 트레이더·부하 하네스와 같은 폴더라, 거기 남은 open_orders.txt를
    //  OrderRouter가 이전 세션 미체결로 읽어 수만 건 취소에 매달리고 이 테스트의 주문이 5초 안에 안 나온다(09-22 실제로 났다).
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");

    if (const int result_code = run_case(1, 1, {"005930"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_case(2, 2, {"005930", "000660"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_spanning_case(2, 4, {"005930", "000660", "005380"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_replay_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_protective_claim_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_stale_entry_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_symbol_role_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_switch_role_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_split_start_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_manual_order_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_score_spread_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_score_tie_order_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_quote_table_reload_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_generation_copy_case(); result_code != 0)
    {
        return result_code;
    }

    std::cout << "test_engine OK (" << g_checks << " checks)\n";
    return 0;
}
