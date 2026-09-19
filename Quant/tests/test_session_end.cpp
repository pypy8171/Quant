// 마감 자기 종료 판정(core/SessionEndJudge.h) 단위 테스트. 창 0이면 무판정·개장 전 무판정·닫힌 첫 관찰 한 번·
// 유예 안 대기·유예 뒤 큐 비면 종료·큐가 남으면 배출 한도까지 대기 뒤 강제·너무 늦은 기동 무판정·종료는 한 번만을
// 고정한다. 헤더 전용이라 시계 없이 돈다(시각은 자정부터의 초로 넣는다). 관련 결정: D-098.
// 빌드: cmake --build <directory> --target test_session_end
#include "core/SessionEndJudge.h"

#include <iostream>

using session_end::Config;
using session_end::Judge;
using Step = Judge::Step;

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

constexpr int kClose15h30 = 15 * 60 + 30; // 정규장 마감(분)
constexpr int kCloseSec   = kClose15h30 * 60;

Config regular()
{
    Config config;
    config.close_min       = kClose15h30;
    config.grace_sec       = 120;
    config.drain_limit_sec = 600;
    return config;
}
} // namespace

int main()
{
    // 1. 창이 0(리플레이·테스트)이면 언제 물어도 판정하지 않는다.
    {
        Judge judge;
        CHECK(judge.observe(kCloseSec + 1000, false) == Step::kNone);
    }

    // 2. 개장 전·장중은 kNone. 닫힌 첫 관찰만 kClosed, 그 뒤 유예 안은 kNone.
    {
        Judge judge(regular());
        CHECK(judge.observe(8 * 3600, false) == Step::kNone);
        CHECK(judge.observe(kCloseSec - 5, false) == Step::kNone);
        CHECK(judge.observe(kCloseSec, true) == Step::kClosed);
        CHECK(judge.observe(kCloseSec + 5, false) == Step::kNone);
        CHECK(judge.observe(kCloseSec + 119, false) == Step::kNone);
    }

    // 3. 유예가 끝나고 큐가 비면 kShutdown — 그 뒤로는 다시 답하지 않는다.
    {
        Judge judge(regular());
        CHECK(judge.observe(kCloseSec, false) == Step::kClosed);
        CHECK(judge.observe(kCloseSec + 120, false) == Step::kShutdown);
        CHECK(judge.observe(kCloseSec + 125, false) == Step::kNone);
        CHECK(judge.observe(kCloseSec + 130, true) == Step::kNone);
    }

    // 4. 유예 뒤에도 큐가 남으면 기다린다. 비는 순간 kShutdown.
    {
        Judge judge(regular());
        CHECK(judge.observe(kCloseSec, true) == Step::kClosed);
        CHECK(judge.observe(kCloseSec + 120, true) == Step::kNone);
        CHECK(judge.observe(kCloseSec + 300, true) == Step::kNone);
        CHECK(judge.observe(kCloseSec + 305, false) == Step::kShutdown);
    }

    // 5. 배출 한도(유예+600초)까지 안 비면 강제 종료. 한도 직전은 대기.
    {
        Judge judge(regular());
        CHECK(judge.observe(kCloseSec, true) == Step::kClosed);
        CHECK(judge.observe(kCloseSec + 719, true) == Step::kNone);
        CHECK(judge.observe(kCloseSec + 720, true) == Step::kShutdownForced);
        CHECK(judge.observe(kCloseSec + 725, true) == Step::kNone);
    }

    // 6. 창이 닫힌 지 유예+한도보다 오래된 첫 관찰(밤에 손으로 띄운 기동)은 판정하지 않는다 — 그날 내내.
    {
        Judge judge(regular());
        CHECK(judge.observe(kCloseSec + 721, false) == Step::kNone);
        CHECK(judge.observe(21 * 3600, false) == Step::kNone);
    }

    // 7. 감시견이 창 안에서 재기동한 엔진(첫 관찰이 유예 뒤)은 kClosed 한 번 → 다음 주기 kShutdown.
    {
        Judge judge(regular());
        CHECK(judge.observe(kCloseSec + 200, false) == Step::kClosed);
        CHECK(judge.observe(kCloseSec + 205, false) == Step::kShutdown);
    }

    // 8. 애프터마켓까지 있으면 마지막 창(20:00)이 기준 — 15:30에는 아무것도 하지 않는다.
    {
        Config config = regular();
        config.close_min = 20 * 60;
        Judge judge(config);
        CHECK(judge.observe(kCloseSec + 200, false) == Step::kNone);
        CHECK(judge.observe(20 * 3600, false) == Step::kClosed);
        CHECK(judge.observe(20 * 3600 + 120, false) == Step::kShutdown);
    }

    std::cout << "test_session_end OK (" << g_checks << " checks)\n";
    return 0;
}
