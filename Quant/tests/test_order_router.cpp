// OrderRouter 통합 테스트
// 빌드: cmake --build <directory> --target test_order_router
// 실행: ./test_order_router
//
// 테스트 항목:
//   1. OrderGate 거부 → REJECTED, KIS 호출 없음
//   2. KIS 성공 (ODNO 반환) → ACCEPTED, ODNO 저장
//   3. KIS 실패 (빈 ODNO) → REJECTED with "KIS API 오류"
//   4. 혼합 제출 후 Stats(total/accepted/rejected) 검증
//   5. recent() 이력 N건 반환
//   6. order_id 순번 "ORD-000001" 포맷 검증
//
// 시험 본문은 주제별 test_order_router_<주제>.cpp, 공용 스텁·도우미는 test_order_router_support.h/.cpp에 있다.
//  main은 주제 묶음(run_<주제>_tests)을 원래 시험 순서대로 부른다.

#include "test_order_router_support.h"

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== OrderRouter Unit Tests ===\n";

    // 산출물을 라이브 원장과 갈라 둔다. 이 바이너리는 quant_trader.exe와 같은 build_win/에
    //  놓이고 Logger 기준 폴더가 실행파일 옆 logs/라, 그대로 두면 같은 trades_YYYYMMDD.csv에
    //  쓴다. 2026-09-09 원장 1364행 중 256행이 이렇게 섞였다(TEST 208 + 047050 48).
    //  QUANT_LOG_DIR이 이미 있으면 그쪽을 존중한다 — 산출물을 모아 보는 쪽 뜻이 우선이다.
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    // 사유 기록 파일은 append 전용이라 지난 실행분이 남으면 결과가 달라진다. 먼저 지운다.
    {
        std::time_t now_time = std::time(nullptr);
        std::tm     local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now_time);
#else
        localtime_r(&now_time, &local_time);
#endif
        char buffer[9];
        std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local_time);
        std::error_code error_code;
        std::filesystem::remove(
            Logger::instance().path_for(std::string("order_reasons_") + buffer + ".txt"), error_code);
    }

    run_reconcile_tests();
    run_submit_tests();
    run_fill_tests();
    run_send_tests();
    run_cancel_tests();
    run_journal_tests();
    run_blocked_sell_tests();
    run_restart_tests();
    run_recover_tests();
    run_session_tests();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
