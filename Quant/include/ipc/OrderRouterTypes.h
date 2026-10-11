// 주문 라우터의 설정과 밖으로 돌려주는 결과 타입(통계·재기동 대조 결과). 클래스 본체는 ipc/OrderRouter.h. [why D-113]
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

struct OrderRouterConfig
{
    int max_history = 500; // 보관할 최대 주문 이력 건수
    // 전송 타임아웃 재확인 — 조회 전 기다림, 그리고 조회가 실패했을 때 다시 조회하기까지의 간격(차례로 쓰고 끝 값을 되풀이).
    //  10·30·60초는 2026-10-07 09:00 모의 서버가 주문·조회 모두 15초 넘게 늦던 1–2분을 넘기려고 고른 값이다. 시험은 짧게 준다.
    std::chrono::milliseconds                reconcile_settle{3000};
    std::array<std::chrono::milliseconds, 3> reconcile_retry{std::chrono::seconds(10), std::chrono::seconds(30),
                                                             std::chrono::seconds(60)};
};

namespace order_router
{

// 주문 수 통계. OrderRouter::statistics가 돌려준다(OrderRouter::Stats는 이 타입의 별칭).
struct Stats
{
    uint64_t total    = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
};

// 재기동 대조(OrderRouter::adopt_open_intents) 결과. OrderRouter::AdoptResult는 이 타입의 별칭.
struct AdoptResult
{
    int restored = 0; // history_에 되살린 미체결 주문
    int released = 0; // 선점만 푼 주문(KIS가 모르는 것)
};

} // namespace order_router
