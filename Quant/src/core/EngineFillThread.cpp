// 체결 쪽 — 체결통보를 장부에 반영하는 스레드.
//  Engine 클래스는 그대로다. Engine.cpp 가 길어 열기 어려워 이 스레드 본체만 따로 낸 것이다
//  (같은 Engine 의 멤버 함수 본체다. 스레드 한 바퀴는 단계 함수로 나눴고, 그 선언은 Engine.h 의 private 절에 있다).
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

// 체결 스레드가 고리 내내 들고 있는 값. 어느 길에서 꺼낼지는 시작 때 정해지고 바뀌지 않는다.
struct Engine::FillLoopState
{
    // 갈라 띄운 날의 체결통보는 건너편 시세 프로세스가 통로로 넘긴 것이다. 한 프로세스로 돌면 예전처럼
    //  프로세스 안 큐에서 꺼낸다 — 두 길의 나머지(장부 반영·방송)는 같다. [why D-114 단계 5]
    bool from_channel = false;
    // 통로로 오는 날에도 프로세스 안 큐를 같이 본다 — 브로커를 끊고 도는 판(부하시험·리플레이)의 모의
    //  체결기가 이 프로세스에 있고, 그 체결은 통로를 거치지 않는다. [why D-114 단계 5]
    bool                  from_queue = false;
    const ipc::FillLimits fill_limits;
    // 정지 뒤 비우기를 시작한 시각. 정지 전에는 비어 있다.
    std::optional<std::chrono::steady_clock::time_point> drain_started;
};

// 체결통보 소비 전용 스레드. 주문 스레드에 얹지 않은 이유: 주문 스레드는 KIS 발주(REST, 수십~수백 milliseconds)와 발주 간격
//  대기에 묶여 있는 시간이 길어, 그 뒤에 선 체결이 장부에 늦게 들어가고 다음 SELL의 보유 수량 판단이 그만큼 낡는다.
//  큐가 비면 condvar에서 자고 WS 콜백이 깨운다 — 1ms 폴링은 Windows에서 실측 8~15ms 늦었다(test_pipeline_stress).
//  on_fill이 던지면 스레드가 죽어 이후 체결이 전부 큐에 쌓이므로 건마다 잡아 로그로 남긴다. [why D-056]
void Engine::fill_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("Fill");
    LOG_INFO("[FillThread] 시작");

    FillLoopState state;
    state.from_channel = role_ == ProcessRole::Order;
    state.from_queue   = !state.from_channel || feed_.paper != nullptr;

    // 잠드는 조건 — 어느 길에서든 꺼낼 것이 생기면 깬다.
    auto fill_queue_empty = [this, &state]
    {
        return fill_sources_empty(state);
    };

    // 넘침 목록에 남은 체결통보. 정지 뒤에는 제어 스레드가 없으니 여기서 큐로 옮겨 마저 반영한다.
    auto overflow_waiting = [this]
    {
        return pipeline_.fill_overflow_waiting.load(std::memory_order_relaxed) != 0;
    };

    // 정지 요청 뒤에도 큐를 비운다 — stop()이 WS를 끊은 다음 join하므로 남은 통보가 여기서 빠진다.
    while (!stop_token.stop_requested() || !fill_queue_empty() || overflow_waiting())
    {
        if (stop_token.stop_requested() && stop_drain_expired(state))
        {
            break;
        }

        const std::optional<FillNotification> option = pop_fill_notification(state);

        if (!option)
        {
            // 상한 100ms는 신호가 샐 때의 보험이다. 깨우는 것은 WS 콜백의 notify와 정지 요청. 건너편
            //  프로세스는 깨울 수 없으므로 갈라 띄운 날에는 이 상한이 곧 폴링 간격이다. [why D-114 단계 5]
            pipeline_.fill_wake.wait_for(100ms, stop_token, fill_queue_empty);
            continue;
        }

        apply_fill_notification(*option);

        // 사본은 여기서 내지 않는다 — 체결 하나마다 2,700종목을 훑으면 다음 체결이 그만큼 늦게 장부에
        //  들어가고, 그게 다음 SELL의 보유 수량 판단을 낡게 한다. 발행은 ledger_thread_fn이 간격을 두고 낸다.
    }

    LOG_INFO("[FillThread] 종료 (드롭 " + std::to_string(pipeline_.fill_dropped.load(std::memory_order_relaxed)) + "건)");
}

// 체결 스레드 단계 — 큐가 비었는가. 어느 길인지에 따라 보는 자리가 다르다. 잠드는 조건과 정지 뒤 비우기가 같이 쓴다.
bool Engine::fill_sources_empty(const FillLoopState& state)
{
    return (!state.from_channel || layout_.fills().readable() == 0) && (!state.from_queue || pipeline_.fill_queue.empty());
}

// 체결 스레드 단계 — 정지 뒤 비우기. 넘침 목록을 큐로 옮기고, 비우기가 시한을 넘겼으면 참을 돌려 고리를 끝낸다.
//  시한을 두는 이유: 공유 구역이 다시 놓여 통로 순번이 어긋나면 readable()은 0이 아닌데 pop()은 계속 실패해,
//  시한이 없으면 종료가 영영 안 끝난다.
bool Engine::stop_drain_expired(FillLoopState& state)
{
    constexpr auto kStopDrainLimit = 2s;

    flush_fill_overflow();
    const auto now = std::chrono::steady_clock::now();

    if (!state.drain_started)
    {
        state.drain_started = now;
        return false;
    }

    if (now - *state.drain_started > kStopDrainLimit)
    {
        LOG_WARN("[FillThread] 정지 뒤 비우기 2초 초과 — 남은 체결통보를 두고 내린다 (통로 readable=" +
                 std::to_string(state.from_channel ? layout_.fills().readable() : 0) + ")");
        return true;
    }

    return false;
}

// 체결 스레드 단계 — 통로를 먼저, 비었으면 프로세스 안 큐를 본다. 둘 다 비었으면 nullopt.
std::optional<FillNotification> Engine::pop_fill_notification(FillLoopState& state)
{
    std::optional<FillNotification> option;

    if (state.from_channel)
    {
        if (ipc::FillNotice notice; layout_.fills().pop(state.fill_limits, notice))
        {
            option = ipc::to_fill(notice);
        }
    }

    if (!option && state.from_queue)
    {
        if (const auto notice = pipeline_.fill_queue.pop())
        {
            option = ipc::to_fill(*notice);
        }
    }

    return option;
}

// 체결 스레드 단계 — 체결통보 한 건을 장부에 반영하고 운영단말에 방송한다. 예외는 건마다 잡아 로그로 남긴다.
void Engine::apply_fill_notification(const FillNotification& fill_notification)
{
    // 체결통보 구독이 새로 붙었다는 표지 — 장부·단말에 넣을 체결이 아니다. 라우터가 끊긴 사이 놓친 체결을
    //  조회로 되찾게 한다(따로 도는 스레드라 여기는 바로 돌아온다). [why D-149]
    if (fill_notification.kind == FillKind::SessionResumed)
    {
        if (order_router_)
        {
            order_router_->on_session_resumed(fill_notification.session_generation);
        }

        return;
    }

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
}
