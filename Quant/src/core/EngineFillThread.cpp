// 체결 쪽 — 체결통보를 원장에 반영하는 스레드.
//  Engine 클래스는 그대로다. Engine.cpp 가 2,800줄을 넘겨 열기 어려워 이 스레드 본체만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  fill_thread_fn() : spawn_threads() 가 fill 스레드로 띄운다. 체결통보 큐가 비면 WS 콜백이 깨울 때까지 잔다.

#include "core/Engine.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <chrono>
#include <ctime>
#include <exception>
#include <optional>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

// 체결통보 소비 전용 스레드. 주문 스레드에 얹지 않은 이유: 주문 스레드는 KIS 발주(REST, 수십~수백 milliseconds)와 발주 간격
//  대기에 묶여 있는 시간이 길어, 그 뒤에 선 체결이 원장에 늦게 들어가고 다음 SELL의 보유 수량 판단이 그만큼 낡는다.
//  큐가 비면 condvar에서 자고 WS 콜백이 깨운다 — 1ms 폴링은 Windows에서 실측 8~15ms 늦었다(test_pipeline_stress).
//  on_fill이 던지면 스레드가 죽어 이후 체결이 전부 큐에 쌓이므로 건마다 잡아 로그로 남긴다. [why D-056]
void Engine::fill_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("Fill");
    LOG_INFO("[FillThread] 시작");

    // 갈라 띄운 날의 체결통보는 건너편 시세 프로세스가 통로로 넘긴 것이다. 한 프로세스로 돌면 예전처럼
    //  프로세스 안 큐에서 꺼낸다 — 두 길의 나머지(원장 반영·방송)는 같다. [why D-114 단계 5]
    const bool from_channel = role_ == ProcessRole::Order;
    const ipc::FillLimits fill_limits;

    // 통로로 오는 날에도 프로세스 안 큐를 같이 본다 — 브로커를 끊고 도는 판(부하시험·리플레이)의 모의
    //  체결기가 이 프로세스에 있고, 그 체결은 통로를 거치지 않는다. [why D-114 단계 5]
    const bool from_queue = !from_channel || feed_.paper != nullptr;

    // 큐가 비었는가 — 어느 길인지에 따라 보는 자리가 다르다. 잠드는 조건과 정지 뒤 비우기가 같이 쓴다.
    auto fill_queue_empty = [this, from_channel, from_queue]
    {
        return (!from_channel || layout_.fills().readable() == 0) && (!from_queue || pipeline_.fill_queue.empty());
    };

    // 정지 요청 뒤에도 큐를 비운다 — stop()이 WS를 끊은 다음 join하므로 남은 통보가 여기서 빠진다.
    while (!stop_token.stop_requested() || !fill_queue_empty())
    {
        std::optional<FillNotification> option;

        if (from_channel)
        {
            if (ipc::FillNotice notice; layout_.fills().pop(fill_limits, notice))
            {
                option = ipc::to_fill(notice);
            }
        }

        if (!option && from_queue)
        {
            if (const auto notice = pipeline_.fill_queue.pop())
            {
                option = ipc::to_fill(*notice);
            }
        }

        if (!option)
        {
            // 상한 100ms는 신호가 샐 때의 보험이다. 깨우는 것은 WS 콜백의 notify와 정지 요청. 건너편
            //  프로세스는 깨울 수 없으므로 갈라 띄운 날에는 이 상한이 곧 폴링 간격이다. [why D-114 단계 5]
            pipeline_.fill_wake.wait_for(100ms, stop_token, fill_queue_empty);
            continue;
        }

        const FillNotification& fill_notification = *option;

        try
        {
            if (order_router_)
            {
                order_router_->on_fill(fill_notification);
            }

            // 체결 직후 몇 초는 잔고 대조를 미룬다 — 잔고 스냅샷이 체결을 따라오기 전이다. [why D-074]
            if (ledger_)
            {
                ledger_->note_fill(std::time(nullptr));
            }

            if (ops_.server && ops_.server->client_count() > 0)
            {
                ops_.server->broadcast(ops::OpsMsg::FILL_NTF,
                                       nlohmann::json{{"odno", fill_notification.kis_order_no},
                                                      {"ticker", fill_notification.ticker},
                                                      {"side", fill_notification.side == OrderSide::BUY ? "BUY" : "SELL"},
                                                      {"qty", fill_notification.filled_quantity},
                                                      {"price", fill_notification.filled_price},
                                                      {"time", fill_notification.fill_time}}
                                           .dump());
            }
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[FillThread] 체결 반영 예외 " + fill_notification.ticker + " ODNO=" + fill_notification.kis_order_no + ": " + exception.what());
        }

        // 체결로 보유·평단·매도가능이 바뀌었다. 접수 쪽 발행과는 OrderGate의 발행 잠금이 줄을 세운다.
        order_gate_.publish_ledger(*ledger_snapshot_);
    }

    LOG_INFO("[FillThread] 종료 (드롭 " + std::to_string(pipeline_.fill_dropped.load(std::memory_order_relaxed)) + "건)");
}
