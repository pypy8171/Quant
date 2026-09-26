// 장부 사본 발행 전용 스레드.
//  Engine 클래스는 그대로다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다.
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  ledger_thread_fn() : spawn_threads() 가 주문 쪽 프로세스에서만 띄운다.

#include "core/Engine.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <chrono>

using namespace std::chrono_literals;

// 읽는 쪽에 주는 사본을 내는 스레드. 전에는 주문 스레드가 주문 하나마다 한 판씩 냈는데, 2,700종목을
//  들고 있으면 그 한 판이 98microseconds다(09-26 부하시험 실측, 보유 하나당 33nanoseconds로 전부 훑는다).
//  같은 회차에서 주문 하나 몫이 160microseconds였으니 발주와 무관한 일에 고리의 61%를 쓰고 있었던 셈이고,
//  그 때문에 요청 큐 1,024칸이 넘쳐 주문을 버렸다. 발행을 여기로 떼면 주문 스레드는 34microseconds짜리
//  제 일만 한다.
//
//  간격을 두고 무조건 내는 이유: "바뀌었을 때만"으로는 모자란다. 주문이 없어도 장부는 바뀐다 —
//  잔고 재시드·진입 정지·평가금·슬롯 면제 집합은 다른 스레드가 고치고, 그 변화는 주문 수를 세는
//  어떤 표시에도 안 잡힌다. 주문 스레드가 쉴 때 하던 일이 바로 이 무조건 발행이었고(같은 간격),
//  이제 바쁠 때도 같은 간격을 쓴다 — 읽는 쪽 약속이 "100milliseconds 안에 닿는다" 하나로 통일된다. [why D-114]
void Engine::ledger_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("Ledger");
    LOG_INFO("[LedgerThread] 시작 — 장부 사본을 " +
             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(kLedgerPublishInterval).count()) +
             "ms 간격으로 낸다");

    while (!stop_token.stop_requested())
    {
        // 발행이 던지면 스레드가 죽고 읽는 쪽이 영영 낡은 판을 본다 — 건마다 잡아 로그로 남기고 계속 돈다.
        try
        {
            order_gate_.publish_ledger(*ledger_snapshot_);
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[LedgerThread] 사본 발행 예외: " + std::string(exception.what()));
        }

        if (!wake::sleep_unless_stopped(stop_token, kLedgerPublishInterval))
        {
            break;
        }
    }

    // 마지막 한 판 — 주문·체결 스레드가 선 뒤 마지막 변화까지 사본에 닿게 한다.
    try
    {
        order_gate_.publish_ledger(*ledger_snapshot_);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR("[LedgerThread] 마지막 사본 발행 예외: " + std::string(exception.what()));
    }

    LOG_INFO("[LedgerThread] 종료");
}
