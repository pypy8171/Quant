// 주문 쪽 — 주문 실행 스레드, 보호 주문 표 한 주기, 전략 생존 추적.
//  Engine 클래스는 그대로다. Engine.cpp 가 3,200줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  order_thread_fn()          : spawn_threads() 가 order 스레드로 띄운다
//  run_protective_orders()    : strategy_thread_fn() 가 한 바퀴마다(주기는 claim_protective_cycle 이 거른다)
//  track_strategy_liveness()  : order_thread_fn() 가 전략 박동을 볼 때마다

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "risk/DisplacementDesk.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <thread>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

// 보호 주문 표 한 주기. 전략이 등록해 둔 규칙과 원장 보유·현재가만으로 청산을 만든다 — 전략 코드를 한 줄도 안 봐도 된다는 것이
//  이 단계의 요점이다. 프로세스를 가르면 이 함수가 주문 프로세스로 간다(단계 4). [why D-114]
//  시퀀서 하나만 부른다 — strategy_thread 전용이라 protective_next_는 잠금이 필요 없다. [inv]
bool Engine::claim_protective_cycle(std::chrono::steady_clock::time_point now)
{
    const auto now_ticks      = now.time_since_epoch().count();
    const auto interval_ticks =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(protective_interval_).count();
    auto       due_ticks      = protective_next_ticks_.load(std::memory_order_relaxed);

    while (now_ticks >= due_ticks)
    {
        // 잡은 쪽만 참을 받는다. 진 쪽은 due_ticks가 갱신돼 다시 재면 이미 미래라 그대로 빠져나간다.
        if (protective_next_ticks_.compare_exchange_weak(due_ticks, now_ticks + interval_ticks,
                                                         std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return true;
        }
    }

    return false;
}

std::vector<OrderSignal> Engine::build_protective_orders(std::chrono::steady_clock::time_point now)
{
    const auto price_of = [this](symbol::SymbolId symbol) { return last_price(symbol); };

    // 원장을 쥔 역할(한 프로세스·주문)은 원장을 바로 본다.
    if (runs_order_side())
    {
        const auto& ledger = order_gate_.ledger();
        return protective_book_.evaluate(
            ledger.snapshot_positions(), price_of,
            [&ledger](const std::string& account, symbol::SymbolId symbol) { return ledger.reserved(account, symbol); }, now);
    }

    // 갈라 띄운 전략 역할의 원장은 체결을 받지 않아 늘 비어 있다 — 주문 쪽이 내는 장부 사본을 한 판 읽어 보유·평단·
    //  선점을 채운다. 사본은 한 계좌만 싣는다. ticker는 채우지 않는다 — evaluate는 계좌와 종목 번호로만 찾는다. [why D-114]
    std::vector<symbol::SymbolId> ledger_ids;
    std::vector<ipc::LedgerRow>   ledger_rows;
    ipc::collect_all_rows(*ledger_snapshot_, ledger_ids, ledger_rows);
    const std::string account = ledger_snapshot_->globals().account;

    std::vector<OrderGate::HeldPos> held;
    held.reserve(ledger_rows.size());

    for (size_t index = 0; index < ledger_rows.size(); ++index)
    {
        const ipc::LedgerRow& row = ledger_rows[index];

        if (row.position == 0)
        {
            continue;
        }

        OrderGate::HeldPos holding;
        holding.account       = account;
        holding.quantity      = row.position;
        holding.average_price = row.average_price;
        holding.symbol        = ledger_ids[index];
        holding.slot_exempt   = row.slot_exempt != 0;
        held.push_back(std::move(holding));
    }

    const auto reserved_of = [&account, &ledger_ids, &ledger_rows](const std::string& row_account, symbol::SymbolId symbol)
    {
        if (row_account != account)
        {
            return 0;
        }

        const auto iterator = std::find(ledger_ids.begin(), ledger_ids.end(), symbol);
        return iterator != ledger_ids.end()
                   ? static_cast<int>(ledger_rows[static_cast<size_t>(iterator - ledger_ids.begin())].reserved)
                   : 0;
    };

    return protective_book_.evaluate(held, price_of, reserved_of, now);
}

void Engine::run_protective_orders(SignalDispatcher& dispatcher, std::chrono::steady_clock::time_point now)
{
    if (!protective_book_.enabled() || !claim_protective_cycle(now))
    {
        return;
    }

    for (auto& protective_signal : build_protective_orders(now))
    {
        dispatcher.submit(std::move(protective_signal));
    }
}

// 시세가 죽으면 새 진입만 끊는다. 전략 쪽 마무리와 달리 보호 주문은 걸지 않는다 — 현재가가 멎어
//  청산선을 판단할 근거가 없고, 낡은 값으로 시장가를 내면 그쪽이 더 나쁘다. 보유분은 그대로 둔다.
//  [inv] order_thread 전용. [why D-137]
void Engine::track_feed_liveness(ipc::HeartbeatMonitor::Step step, bool just_died)
{
    // 박동이 돌아왔다 — 감시견이 시세를 다시 띄웠거나 멈췄던 바퀴가 돌기 시작했다.
    //  정지를 안 풀면 그날 내내 못 산다.
    if (step == ipc::HeartbeatMonitor::Step::kHealthy &&
        feed_wound_down_.exchange(false, std::memory_order_relaxed))
    {
        order_gate_.set_feed_down_halt(false);
        LOG_WARN("[마무리] 시세 박동이 돌아왔다 — 신규 진입 정지를 푼다");
    }

    if (just_died)
    {
        order_gate_.set_feed_down_halt(true);
        feed_wound_down_.store(true, std::memory_order_relaxed);
        LOG_ERROR("[마무리] 시세 박동이 끊겼다 — 신규 진입 정지. 현재가가 멎어 진입 판단의 근거가 낡았고, "
                  "체결통보도 같은 소켓에 실려 예약 수량이 안 풀린다. 청산·취소는 그대로 나간다");
    }
}

// 마무리 순서: ① 새 진입을 끊고 ② 감시견에 알리고 ③ 보유분은 보호 주문 표가 지킨다.
//  저널은 여기서 따로 안 민다 — 표를 들고 있는 쪽(주문·원장)이 살아 있고 append마다 이미 fflush한다.
//  [inv] order_thread 전용. 여기서 부르는 OrderRouter::submit이 단일 스레드를 전제한다. [why D-114]
void Engine::track_strategy_liveness(ipc::HeartbeatMonitor::Step step, bool just_died,
                                     std::chrono::steady_clock::time_point now)
{
    // 박동이 돌아왔다 — 감시견이 전략을 다시 띄웠거나 멈췄던 스레드가 깨어났다. 정지를 안 풀면 그날 내내 못 산다.
    if (step == ipc::HeartbeatMonitor::Step::kHealthy &&
        strategy_wound_down_.exchange(false, std::memory_order_relaxed))
    {
        order_gate_.set_strategy_down_halt(false);
        LOG_WARN("[마무리] 전략 박동이 돌아왔다 — 신규 진입 정지를 푼다");
    }

    if (just_died)
    {
        // 새 진입을 끊는다. 막는 자리는 게이트가 아니라 전략 쪽 창구(set_entry_halt_provider)라, 샤드 스레드에서
        //  도는 전략들이 신규 매수를 더 만들지 않는다 — 멈췄던 전략 스레드가 깨어나 밀린 신호를 쏟는 것을 막는다.
        //  청산(SELL)·취소는 그대로 통과한다 — 급락장에 청산이 미완료로 남지 않게(entry_halt와 같은 규칙).
        order_gate_.set_strategy_down_halt(true);
        strategy_wound_down_.store(true, std::memory_order_relaxed);
        LOG_ERROR("[마무리] 전략 박동이 끊겼다 — 신규 진입 정지, 보호 주문은 주문 스레드가 이어받는다");
    }

    if (!strategy_wound_down_.load(std::memory_order_relaxed) || order_router_ == nullptr ||
        !protective_book_.enabled())
    {
        return;
    }

    if (!claim_protective_cycle(now))
    {
        return;
    }

    // 전략 코드를 한 줄도 안 보고 표와 원장 보유·현재가만으로 청산을 만든다. 디스패처를 안 거치는 이유는
    //  그것이 전략 스레드 소유이기 때문이다 — 게이트 판정은 OrderRouter::submit 안에서 그대로 돈다.
    for (auto& protective_signal : build_protective_orders(now))
    {
        try
        {
            const ManagedOrder managed_order = order_router_->submit(protective_signal);

            // 게이트가 막으면 예외가 아니라 거부 상태로 돌아온다. 결과를 버리면 손절·청산이 한 건도
            //  안 나간 채로 조용히 넘어간다 — 보호 주문만은 못 나간 사실을 반드시 남긴다. 2026-09-23
            if (managed_order.status == OrderStatus::REJECTED)
            {
                LOG_ERROR("[마무리] 보호 주문 거부 " + protective_signal.ticker + ": " +
                          (managed_order.reject_reason.empty() ? "사유 없음" : managed_order.reject_reason));
            }
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[마무리] 보호 주문 발주 실패 " + protective_signal.ticker + ": " + exception.what());
        }
    }
}

// ─── 주문 실행 스레드 ─────────────────────────────────────────────────────
void Engine::order_thread_fn(std::stop_token stop_token)
{
    auto& ledger = order_gate_.ledger();

    using std::chrono::steady_clock;
    thread_name::set_current("Order");
    LOG_INFO("[OrderThread] 시작");

    // 발주 간격과 거부 재시도는 이 스레드 소유라 조절기를 여기에 둔다. pipeline_.requests는 SPSC(생산자=전략 스레드)라
    //  되밀 수 없어 재시도는 조절기의 전용 버퍼에 산다. [why D-065]
    OrderRateLimiter rate_limiter({order_min_interval_ms_, order_max_retries_}, steady_clock::now());
    rate_limiter.set_position([&ledger](const std::string& argument, const std::string& ticker) { return ledger.position(argument, ticker); });

    // 구간 지연 CSV. 이 스레드만 쓰므로 지역 객체로 두고, 첫 주문 때 파일을 연다. [why D-071]
    trace::LatencyTrace latency_trace(Logger::instance().path_for("latency_trace.csv"));

    // 같은 순번을 두 번 받으면 이중 발주다(A등급). 생산자가 하나인 지금은 안 생기지만 단계 4에서
    //  공유메모리로 바뀌면 재전송이 생긴다 — 거르는 자리를 먼저 둔다. 창은 요청 큐 크기다. [why D-114]
    ipc::DuplicateFilter duplicate_filter(ShardPipeline::kOrderQueueCapacity);
    ipc::HeartbeatMonitor strategy_monitor;

    // 시세 쪽 생사를 보는 눈. 문턱이 전략 쪽(250ms·1s)보다 훨씬 헐거운 것은 시세 박동이 hot loop 가 아니라
    //  제어 스레드의 5초 바퀴에서 찍히기 때문이다. 그 바퀴에는 구독 걸기·칸 재배정이 같이 들어 있어
    //  2,700종목을 올리는 기동 중에는 시세 쪽이 번호를 다 받기까지 27초가 걸린다(2026-09-25 부하시험
    //  실측). 사망 문턱을 그보다 낮게 잡으면 멀쩡한 기동을 죽었다고 읽으므로 그 위에서 시작하고,
    //  feed_beat_gap_max_ns 에 쌓이는 실측으로 뒤에 좁힌다. [why D-137]
    constexpr int64_t     kFeedBeatPeriodMs  = 5'000;
    constexpr int64_t     kFeedBeatSuspectMs = 30'000;
    constexpr int64_t     kFeedBeatDeadMs    = 90'000;
    ipc::HeartbeatMonitor feed_monitor(
        ipc::HeartbeatConfig{kFeedBeatPeriodMs, kFeedBeatSuspectMs, kFeedBeatDeadMs});

    // 꺼낸 값이 표 밖을 짚지 않는지 보는 기준. 지금 든 수가 아니라 표가 받을 수 있는 칸 수를 쓴다 —
    //  종목 표는 장중에도 늘어나서(처음 보는 종목) 지금 든 수로 재면 방금 올라온 종목이 걸린다. [why D-114]
    const ipc::RequestLimits request_limits{static_cast<uint32_t>(ledger.symbols().capacity()),
                                            static_cast<uint32_t>(ledger.strategy_table().capacity())};

    // 교체 진입 — 최약체 고르기·매도 발주·쿨다운 기록·매수 보류가 여기 한 덩어리로 있다. 전략 쪽에 두면
    //  고르는 시점과 예약하는 시점이 갈려 둘이 같은 종목을 두 번 판다. [why D-114]
    risk::DisplacementDesk displace_desk(order_gate_);
    displace_desk.set_label([this](const std::string& ticker) { return ticker_label(ticker); });
    std::vector<OrderSignal> displace_expired;

    // 주문이 없는 회차에도 사본을 이 간격으로는 낸다(아래 대기 구간). 100ms는 대기 상한과 같은 값이라
    //  쉬는 동안 회차마다 한 번꼴이고, 전략이 보는 장부가 그보다 더 낡지 않는다.
    constexpr auto kIdlePublishInterval = 100ms;
    auto           last_idle_publish    = steady_clock::now();

    // 결과를 전략 쪽으로 돌려준다. 지금은 같은 프로세스의 큐고, 단계 4에서 공유메모리로 바뀌어도
    //  레코드는 그대로다. [inv] 순번 0은 통로 밖에서 들어온 신호라 맞출 짝이 없어 답하지 않는다. [why D-114]
    auto answer = [this](uint64_t sequence, ipc::OrderResult result, uint64_t kis_order_number, std::string_view reason)
    {
        if (sequence == 0)
        {
            return;
        }

        if (!pipeline_.order_responses->push(
                ipc::make_response(sequence, result, kis_order_number, reason, trace::now_ns())))
        {
            pipeline_.order_response_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // 발주 대상이 되기 전 지나는 한 자리 — 종목 번호를 채우고 교체 창구의 판정을 받는다. 통로로 온
    //  신호와 운영단말이 낸 수동주문이 같은 자리를 지나게 한다. [why D-114]
    auto admit = [this, &ledger, &displace_desk](OrderSignal&& signal) -> std::optional<OrderRateLimiter::Pending>
    {
        // 번호는 주문 쪽이 준다 — 전략 쪽은 종목 표를 찾기만 하고, 처음 보는 종목은 받는 이 자리에서
        //  표에 올린다. 표를 고치는 쪽을 하나로 두는 것이 원칙 4다. [why D-114]
        if (signal.symbol_id == symbol::kNone && !signal.ticker.empty())
        {
            signal.symbol_id = ledger.intern_symbol(signal.ticker);
        }

        // 자리가 꽉 찬 책에 새 종목 매수가 왔는가 — 최약체 매도를 앞세우고 이 매수는 창구가 든다.
        //  kSellFirst면 signal 자리에 교체 매도가 들어와 있다(순번이 0이라 답하지 않는다). 원래의
        //  매수는 창구가 들고 있다가 자리가 나면 내고, 못 내면 expire가 답한다. kHold면 이 회차에
        //  낼 것이 없다.
        if (displace_desk.consider(signal, steady_clock::now()) == risk::DisplacementDesk::Verdict::kHold)
        {
            return std::nullopt;
        }

        return OrderRateLimiter::Pending{std::move(signal), 0};
    };

    // 할 일이 없을 때 자는 상한. 갈라 띄우면 건너편 프로세스가 이 스레드를 깨울 방법이 없다 — WakeGate는
    //  이 프로세스 안의 condvar라 notify가 경계를 못 넘는다. 그래서 이 상한이 곧 등록 한 건의 지연이 된다:
    //  100ms면 종목·전략 2,700쌍에 9분이 걸리고, 시세 쪽이 5분 시한에 먼저 걸려 종목 순번표를 아예 못 적는다
    //  (2026-09-25 부하시험 실측 249/2700). 한 프로세스판은 같은 프로세스의 전략 스레드가 notify로 깨우므로
    //  줄일 까닭이 없다 — 깨어나는 횟수만 늘어난다. [why D-114]
    const auto idle_capture = (runs_order_side() && !runs_strategy_side()) ? 2ms : 100ms;

    while (!stop_token.stop_requested())
    {
        // 살아 있다고 찍는다 — 전략 쪽이 이 값의 공백만 보고 판정한다. 아래 KIS 왕복이 이 자리를 몇 초
        //  붙잡으므로 공백에는 그 시간이 그대로 들어간다. 그래서 전략 쪽 문턱이 훨씬 헐겁다. [why D-114]
        pipeline_.order_heartbeat->beat(trace::now_ns());

        // 전략이 살아 있는가 — 박동 공백만 본다. 사망이어도 주문 스레드는 안 내려간다(보유분을 지켜야 한다).
        // 표 고치기가 주문보다 먼저다 — 슬롯 면제·우선순위가 낡은 채로 이 회차의 주문을 거르면
        //  전략이 이미 반영된 줄 알고 낸 신호가 옛 표에 걸린다. [why D-114]
        control_plane_.apply();

        const auto step = strategy_monitor.observe(trace::now_ns(), pipeline_.strategy_heartbeat->last_ns());
        pipeline_.strategy_beat_gap_max_ns.store(strategy_monitor.max_gap_ns(), std::memory_order_relaxed);
        track_strategy_liveness(step, strategy_monitor.take_dead_once(), steady_clock::now());

        // 시세가 살아 있는가 — 같은 자리에서 본다. 전략 쪽과 달리 아직 한 번도 안 뛴 칸(0)은 정상으로
        //  읽히므로(HeartbeatMonitor::observe), 시세 프로세스가 아직 안 뜬 기동 초반을 사망으로 보지 않는다.
        //  [why D-137]
        const auto feed_step = feed_monitor.observe(trace::now_ns(), pipeline_.feed_heartbeat->last_ns());
        pipeline_.feed_beat_gap_max_ns.store(feed_monitor.max_gap_ns(), std::memory_order_relaxed);
        track_feed_liveness(feed_step, feed_monitor.take_dead_once());

        // 교체 보류 시한이 지난 매수는 버리고 그 순번에 답을 돌려준다 — 답이 없으면 전략 쪽 PendingRequests가 샌다.
        displace_desk.expire(steady_clock::now(), displace_expired);

        for (const OrderSignal& dropped : displace_expired)
        {
            answer(dropped.sequence, ipc::OrderResult::kRejected, 0, "교체 보류 만료");
        }

        displace_expired.clear();

        // 발주 대상 선택: 만기된 재시도분 우선, 사람이 낸 수동주문, 자리가 나 풀린 교체 보류분, 없으면 신규 큐
        std::optional<OrderRateLimiter::Pending> next = rate_limiter.take_due_retry(steady_clock::now());
        int64_t                            pop_ns = 0;

        if (!next)
        {
            // 운영단말 수동주문 — 소켓 스레드가 넣은 것을 주문 쪽이 바로 꺼낸다. 전략 프로세스를 거치지
            //  않아야 전략이 멎어도 사람이 손으로 낼 수 있다. 통로 밖에서 온 것이라 순번은 0이고,
            //  단말에는 발주 결과를 ORDER_RESULT_NTF로 따로 알린다. [why D-114][why D-043]
            if (OrderSignal manual_signal; take_manual_order(manual_signal))
            {
                pop_ns = trace::now_ns();
                next   = admit(std::move(manual_signal));
            }
        }

        if (!next)
        {
            if (OrderSignal ready; displace_desk.take_ready(ready))
            {
                next   = OrderRateLimiter::Pending{std::move(ready), 0};
                pop_ns = trace::now_ns();
            }
        }

        if (!next)
        {
            if (ipc::OrderRequest request; pipeline_.requests->pop(request))
            {
                // 건너편이 망가졌거나 칸이 덮였으면 여기서 멎는다 — 그 값으로 낸 주문은 되돌릴 수 없다.
                if (!ipc::is_plausible(request, request_limits))
                {
                    const auto count = pipeline_.order_implausible.fetch_add(1, std::memory_order_relaxed) + 1;
                    LOG_ERROR("[주문] 값이 말이 안 되는 요청을 버린다 순번=" + std::to_string(request.sequence) +
                              " 종목=" + std::to_string(request.symbol_id) + " 전략=" +
                              std::to_string(request.strategy_index) + " 수량=" + std::to_string(request.quantity) +
                              " (누적 " + std::to_string(count) + ")");
                    answer(request.sequence, ipc::OrderResult::kInvalid, 0, "값이 말이 안 된다");
                    continue;
                }

                if (request.sequence != 0 && !duplicate_filter.accept(request.sequence))
                {
                    const auto count = pipeline_.order_duplicate.fetch_add(1, std::memory_order_relaxed) + 1;
                    LOG_WARN("[주문] 같은 순번을 다시 받아 거른다 순번=" + std::to_string(request.sequence) +
                             " (누적 " + std::to_string(count) + ")");
                    answer(request.sequence, ipc::OrderResult::kDuplicate, 0, "같은 순번");
                    continue;
                }

                // 여기서 신호 모양으로 되살린다 — 아래 사슬(교체 창구·발주 조절기·라우터)은 그대로 OrderSignal을
                //  받는다. 전략 이름은 레코드에 없어 번호로 표에서 찾는다. [why D-114]
                const strategy_table::StrategyName strategy_name =
                    ledger.strategy_table().name(request.strategy_index);
                OrderSignal signal = ipc::to_signal(request, strategy_name.view());

                pop_ns = trace::now_ns();

                // 오래 기다린 신규 매수는 여기서 버린다 — 큐가 찬 동안 증권사 초당한도는 그대로라, 이 한 건을
                //  보내면 그만큼 방금 만든 판단이 못 나간다. 조건이 남아 있으면 다음 틱·봉이 다시 만든다.
                //  취소·정정과 매도는 나이를 안 본다(손절·청산은 늦어도 나가야 한다). 신호를 낸 시각은
                //  통로 레코드의 sent_at_ns 가 실어 오므로 갈라 띄워도 같은 나이로 잰다. [why D-127][why D-114]
                if (is_stale_entry(signal, pop_ns))
                {
                    const auto waited_ms = (pop_ns - signal.signal_at_ns) / kNanosecondsPerMillisecond;
                    const auto count = pipeline_.order_stale.fetch_add(1, std::memory_order_relaxed) + 1;

                    if (count == 1 || count % ShardPipeline::kDropLogEvery == 0)
                    {
                        LOG_WARN("[주문] 큐에서 " + std::to_string(waited_ms) + "ms 기다린 신규 매수를 버린다 " +
                                 signal.ticker + " (누적 " + std::to_string(count) + ")");
                    }

                    answer(request.sequence, ipc::OrderResult::kStale, 0, "큐 대기가 길어 버림");
                    continue;
                }

                next = admit(std::move(signal));

                // 창구가 들고 있기로 했으면 이 회차에 낼 것이 없다.
                if (!next)
                {
                    continue;
                }
            }
        }

        if (!next)
        {
            // 재시도 만기가 있으면 그 시각까지, 없으면 idle_capture 상한(종료 확인). 신규 신호는 전략
            //  스레드의 notify가, 수동주문은 운영단말 서버 스레드의 notify가 깨운다.
            const auto deadline = rate_limiter.next_retry_at().value_or(steady_clock::now() + idle_capture);
            // 제어 요청·수동주문도 이 스레드가 처리하므로 잠드는 조건에 같이 넣는다 — 안 넣으면 표 고치기와
            //  사람이 누른 주문이 다음 주문이나 100ms 만기까지 밀린다. [why D-114]
            pipeline_.order_wake.wait_until(deadline, stop_token, [this] {
                return pipeline_.requests->readable() == 0 && control_plane_.order_lane_empty() &&
                       ops_.manual_inbox.empty();
            });

            // 주문이 없어도 장부는 바뀐다 — 잔고 재시드·진입 정지·평가금·슬롯 면제 집합은 다른 스레드가 고친다.
            //  그 변화가 사본에 닿는 시간을 100ms 안으로 묶는다. 매 회차 내면 읽는 쪽이 밀리므로 간격을 둔다. [why D-114]
            if (const auto now = steady_clock::now(); now - last_idle_publish >= kIdlePublishInterval)
            {
                last_idle_publish = now;
                order_gate_.publish_ledger(*ledger_snapshot_);
            }

            continue;
        }

        // 호출 간격 조절 — 직전 KIS 발주 후 min_interval 경과 보장(초당한도 하회로 EGW00201 회피)
        if (const auto wait = rate_limiter.wait_before_send(steady_clock::now()); wait > steady_clock::duration::zero())
        {
            std::this_thread::sleep_for(wait);
        }

        // 이 sleep은 우리가 스스로 줄 세운 시간이다 — pop→반환 한 덩이에 섞어 두면 증권사가 느린 것처럼 읽힌다. [why D-071]
        const int64_t      send_ready_ns = pop_ns != 0 ? trace::now_ns() : 0;
        const OrderSignal& signal        = next->signal;
        // next는 아래에서 재시도 버퍼로 옮겨진다(sink). 답할 순번은 그 전에 챙겨 둔다.
        const uint64_t     request_sequence = signal.sequence;

        try
        {
            // 간격은 KIS를 실제로 부른 뒤에만 센다. 로컬 거부(게이트·ENTRY_HALT)는 한도와 무관하다.
            const uint64_t calls_before = order_router_->kis_calls();
            auto managed_order = order_router_->submit(signal);
            const bool kis_called = order_router_->kis_calls() != calls_before;

            if (kis_called)
            {
                rate_limiter.note_sent(steady_clock::now());
            }

            // 재시도 건은 pop 시각이 첫 시도 것이라 구간이 부풀지 않게 첫 시도만 남긴다.
            if (next->attempts == 0)
            {
                const trace::Marks marks{signal.tick_at_ns, signal.signal_at_ns, pop_ns, send_ready_ns,
                                         trace::now_ns()};
                latency_trace.record(signal, marks, managed_order.stages, kis_called,
                                     managed_order.status == OrderStatus::ACCEPTED);
                // 같은 값을 분포로도 — HEALTH가 분위수를 싣는다. 라우터 안 구간은 managed_order가 실어 왔다.
                pipeline_latency_.add(marks, managed_order.stages);
            }

            // 갈라 띄운 주문 프로세스만 하는 일 — 접수가 끝난 뒤에 모의 체결기에 틱을 먹인다. 접수 안에서
            //  체결을 내면 라우터가 ODNO를 적기 전이라 통보가 "미매핑 체결"로 빠진다. [why D-114 단계 5]
            if (managed_order.status == OrderStatus::ACCEPTED)
            {
                feed_paper_fill_tick(signal);
            }

            // 단말이 없으면 JSON 직렬화를 건너뛴다 — 주문 스레드 hot path에서 받는 이 없는 문자열을 만들지 않는다.
            //  client_count()는 뮤텍스 한 번이지만 직렬화보다 싸다. [why D-071]
            if (ops_.server && ops_.server->client_count() > 0)
            {
                // 게이트·브로커를 지난 최종 결과. 단말은 cid로 자기 ORDER_ACK와 잇고, 전략 주문도
                //  같은 채널로 보여 운영 화면이 자동매매를 함께 본다.
                ops_.server->broadcast(ops::OpsMsg::ORDER_RESULT_NTF,
                                       nlohmann::json{{"cid", signal.client_order_id},
                                                      {"order_id", managed_order.order_id},
                                                      {"odno", managed_order.kis_order_no},
                                                      {"strategy", signal.strategy_id},
                                                      {"ticker", signal.ticker},
                                                      {"side", signal.side == OrderSide::BUY ? "BUY" : "SELL"},
                                                      {"qty", signal.quantity},
                                                      {"price", signal.price},
                                                      {"ok", managed_order.status == OrderStatus::ACCEPTED},
                                                      {"msg", managed_order.reject_reason}}
                                           .dump());
            }

            if (managed_order.status == OrderStatus::ACCEPTED)
            {
                ++order_count_;
                answer(request_sequence, ipc::OrderResult::kAccepted, ipc::to_order_number(managed_order.kis_order_no), "");
            }
            else
            {
                // 재시도를 예약했으면 아직 끝이 아니다 — 답은 마지막 한 번만 보낸다(전략은 답 하나로 기다림을 지운다).
                const bool will_retry = rate_limiter.on_rejected(std::move(*next), managed_order.status,
                                                                 managed_order.reject_reason, steady_clock::now());

                if (!will_retry)
                {
                    answer(request_sequence, ipc::OrderResult::kRejected, 0, managed_order.reject_reason);
                }
            }
        }
        catch (const std::exception& exception)
        {
            rate_limiter.note_sent(steady_clock::now());
            LOG_ERROR("[OrderThread] 예외: " + std::string(exception.what()));
            answer(request_sequence, ipc::OrderResult::kFailed, 0, exception.what());
        }

        // 장부가 바뀌었으니 사본을 한 판 낸다. 큐가 비어 쉬는 회차는 위에서 continue로 빠지므로
        //  여기는 실제로 주문을 다룬 회차뿐이다 — 쉼 없이 판을 내면 읽는 쪽이 밀린다. [why D-114]
        order_gate_.publish_ledger(*ledger_snapshot_);
    }

    LOG_INFO("[OrderThread] 종료");
}

// 갈라 띄운 주문 프로세스에는 시세가 오지 않아 모의 체결기의 대기 주문이 영영 안 찬다 — 산 적이 없으니
//  팔 것도 없어 매도가 전량 거부된다. 접수된 주문 한 건을 그 값의 체결 하나로 지어 먹여, 한 프로세스로
//  돌 때 진짜 틱이 하던 일을 대신한다. 시세가 오는 판에서는 아무것도 하지 않는다. [why D-114 단계 5]
void Engine::feed_paper_fill_tick(const OrderSignal& signal)
{
    if (!feed_.paper || runs_feed_side())
    {
        return;
    }

    const double price = signal.price > 0.0 ? signal.price : signal.reference_price;

    if (price <= 0.0)
    {
        return;
    }

    TradeData trade;
    trade.ticker    = signal.ticker;
    trade.symbol_id = signal.symbol_id;
    trade.hhmmss    = kst::hhmmss_int(std::time(nullptr));
    trade.price     = price;
    trade.quantity  = signal.quantity;
    trade.timestamp = std::chrono::system_clock::now();
    feed_.paper->on_tick(trade);
}
