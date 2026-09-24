// 전략 쪽 — 전략 등록(기동·장중), 전략 시작, 전략 처리 스레드와 샤드 스레드.
//  Engine 클래스는 그대로다. Engine.cpp 가 3,700줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  add_strategy() · assign_strategy_identity() : 기동 때 전략 적재(main 스레드)
//  register_strategy_runtime() : 유니버스 재스캔이 장중에 새 전략을 붙일 때
//  start_strategies()          : start() 가 스레드를 띄우기 전에
//  strategy_thread_fn() · shard_thread_fn() : spawn_threads() 가 strategy 스레드와 샤드 M개로 띄운다

#include "core/Engine.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)
{
    LOG_INFO("[Engine] 전략 등록: " + strategy->describe());
    assign_strategy_identity(*strategy);
    strategy_.list.push_back(std::move(strategy));
}

void Engine::assign_strategy_identity(StrategyBase& strategy)
{
    // 이름은 여기서만 본다 — 접두 "ITB_"가 청산 관리 전략(청산 관리 보유 종목 차단 면제)이다.
    // [inv] 기동 등록은 스레드 전이고, 장중 재스캔 등록은 데이터 스레드다. 표 쓰기는 StrategyTable이
    //  뮤텍스로 막아 지금은 안전하다. 프로세스를 가르면 번호가 갈리는 자리라 단계 4에서 이름↔번호
    //  사전을 공유 쪽지에 올린다 — 여기서 답을 기다리게 만들면 재스캔이 주문 한 바퀴에 묶인다. [why D-114]
    strategy.set_strategy_index(order_gate_.ledger().strategy_index_of(strategy.id()));
    strategy.set_exit_manager(strategy.id().starts_with("ITB_"));
}

// 런타임(장중) 전략 등록. start()의 초기화 루프와 동일한 준비를 하되, strategy_.list
// push_back은 strategy_.mutex 하에 수행하고 strategy_.version을 증가시켜 strategy_thread가
// 스냅샷을 재구성하도록 한다. watch_specs_는 control_thread가 재연결 때 읽으므로 watch_specs_mtx_로 감싼다.
void Engine::register_strategy_runtime(std::unique_ptr<StrategyBase> strategy)
{
    if (!strategy)
    {
        return;
    }

    // 런타임 등록 전략도 차트 조회는 실전 시세키로(분봉 모의 HTTP500 회피) — start()와 동일 패턴.
    strategy->set_kis(feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get());
    strategy->set_account_kis(feed_.kis.get()); // 잔고·매도가능수량은 계좌를 가진 주문 클라이언트로
    strategy->set_prefetch_pool(&prefetch_pool_);  // 프리페치는 전략마다 스레드를 띄우지 않고 공용 풀이 돌린다 [why D-071]
    // 전략이 보는 보유·진입정지·매수비율은 전부 장부 사본에서 읽는다. 주문 쪽 장부를 직접 부르면
    //  단계 4에서 프로세스가 갈릴 때 이 네 자리가 한꺼번에 막힌다. [why D-114]
    strategy->set_position_provider([this](const std::string&, const std::string& ticker) {
        return ledger_position(ticker);
    });
    strategy->set_position_provider_by_id([this](const std::string&, symbol::SymbolId symbol) {
        return ledger_snapshot_->row(symbol).position;
    });
    strategy->set_entry_halt_provider([this] { return ledger_snapshot_->globals().entry_halted != 0; });
    strategy->set_entry_scale_provider([this] { return ledger_snapshot_->globals().entry_scale; });
    strategy->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
        return ledger_sellable(account, ticker);
    });
    strategy->set_symbol_resolver([this](std::string_view ticker) { return register_symbol(ticker); });
    strategy->set_protective_registry(&protective_requests_);
    assign_strategy_identity(*strategy);

    try
    {
        strategy->on_start();
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR("[Engine] 재스캔 on_start 예외 [" + strategy->id() + "]: " + exception.what() + " — 등록 건너뜀");
        return;
    }
    catch (...)
    {
        LOG_ERROR("[Engine] 재스캔 on_start 알 수 없는 예외 [" + strategy->id() + "] — 등록 건너뜀");
        return;
    }

    // 새 전략의 초기 활성은 data_thread가 재스캔 직후 apply_regime_selection(strategy_.last_selected_regime)로
    //  맵 기준으로 다시 정한다. 선택 국면을 아직 모르면(기동 직후) 기본 활성. [why D-084]

    // 구독 스펙 추가 (control_thread 재연결 읽기와 겹치므로 watch_specifications_mutex_).
    //  REST 폴링 모드면 다음 폴링 사이클부터 현재가를 받는다. WS 모드는 connect()가 기동 때
    //  한 번만 돌아서, 여기서 늘어난 종목은 목록에 넣는 것만으로는 틱이 오지 않는다. 살아 있는
    //  연결에 증분 구독을 걸어 둔다. 이게 없으면 재스캔으로 등록된 전략이 on_data를 한 번도
    //  못 받아 조용히 매매하지 않는다(등록 로그만 남아 정상으로 보인다).
    for (auto& specification : strategy->get_watch_specifications())
    {
        if (specification.market == Market::KR)
        {
            rescan_set_registered(register_symbol(specification.ticker), true);
        }

        if (add_watch_specification(specification))
        {
            send_watch_request(specification);
        }
    }

    {
        std::lock_guard<std::mutex> lock(strategy_.mutex);
        assign_shard(*strategy);
        strategy_.list.push_back(std::move(strategy));
        strategy_.version.fetch_add(1, std::memory_order_release);
        rebuild_routes_locked();
    }
}

void Engine::start_strategies()
{
    // 전략 초기화 (시세 클라이언트 주입 → on_start 내부에서 Universe 조회)
    // 전략의 feed_.kis는 차트(일봉·분봉)·랭킹 등 "읽기 전용 시세 조회"에만 쓰인다(실제 주문 발주는 OrderThread가 담당).
    // 분봉 TR(inquire-time-itemchartprice)은 모의 도메인에서 HTTP 500 → 시세 전용 실전 클라이언트가
    //  있으면 그걸로 조회(regime_·스캐너와 동일 패턴). 없으면 모의로 폴백.
    for (auto& strategy : strategy_.list)
    {
        strategy->set_kis(feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get());
        // 잔고 조회는 시세 클라이언트가 아니라 계좌를 가진 주문 클라이언트로 한다.
        //  시세 전용 클라이언트에는 account_no가 없어 has_account()가 false가 되고,
        //  그러면 매도가능수량이 항상 0으로 떨어져 익절·존이탈청산·장 마감청산이 전부 발주되지 않는다.
        strategy->set_account_kis(feed_.kis.get());
        strategy->set_prefetch_pool(&prefetch_pool_); // 프리페치는 전략마다 스레드를 띄우지 않고 공용 풀이 돌린다 [why D-071]
        // D2: 확정 포지션 접근자 주입 — 전략이 원장을 진실원천으로 읽는다. 그 원장을 이제는 사본으로 본다:
        //  주문 쪽 장부를 직접 부르면 단계 4에서 프로세스가 갈릴 때 이 네 자리가 한꺼번에 막힌다. [why D-114]
        strategy->set_position_provider([this](const std::string&, const std::string& ticker) {
            return ledger_position(ticker);
        });
        strategy->set_position_provider_by_id([this](const std::string&, symbol::SymbolId symbol) {
            return ledger_snapshot_->row(symbol).position;
        });
        strategy->set_entry_halt_provider([this] { return ledger_snapshot_->globals().entry_halted != 0; });
        strategy->set_entry_scale_provider([this] { return ledger_snapshot_->globals().entry_scale; });
        strategy->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
            return ledger_sellable(account, ticker);
        });
        strategy->set_symbol_resolver([this](std::string_view ticker) { return register_symbol(ticker); });
        strategy->set_protective_registry(&protective_requests_);

        try
        {
            strategy->on_start();
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[Engine] on_start 예외 [" + strategy->id() + "]: " + exception.what() + " — 전략 건너뜀");
        }
        catch (...)
        {
            LOG_ERROR("[Engine] on_start 알 수 없는 예외 [" + strategy->id() + "] — 전략 건너뜀");
        }
    }
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
// order_book_queue_(호가) → trade_queue_(체결) → market_queue_(일봉) 순 우선처리
// 아이들 시 100µs 슬립 → 저지연 유지
void Engine::strategy_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("Strategy");
    LOG_INFO("[StrategyThread] 시작");

    // 주문 쪽에 보내 놓고 답을 기다리는 순번. 답이 오면 지우고, 시한을 넘긴 것은 아래에서 찍는다.
    //  다시 보내는 데는 쓰지 않는다 — KIS 주식주문(현금) 요청 전문에 우리가 채우는 식별자 칸이 없어
    //  (CANO·ACNT_PRDT_CD·PDNO·ORD_DVSN·ORD_QTY·ORD_UNPR·EXCG_ID_DVSN_CD·SLL_TYPE·CNDT_PRIC,
    //  2026-09-24 확인) 증권사가 같은 주문을 걸러 주지 못한다. 이미 접수된 주문을 다시 보내면 두 건이
    //  된다. 보낸 뒤 답만 못 받은 갈래는 주문 쪽이 그 자리에서 브로커에 되묻는다. 상한은 요청 큐와 같다.
    //  [why D-114]
    ipc::PendingRequests pending_requests(ShardPipeline::kOrderQueueCapacity);

    // 주문 쪽 생사를 보는 눈. 문턱이 전략 쪽(250ms·1s)보다 훨씬 헐거운 것은 이 공백에 증권사 왕복이
    //  그대로 들어오기 때문이다 — 한 번 부르는 데 윈도는 전송 10초·수신 15초(KisTransport.cpp의
    //  WinHttpSetTimeouts), 리눅스는 10초(CURLOPT_TIMEOUT)까지 간다. 그래서 첫 값은 실측이 아니라
    //  그 상한에서 잡았고, order_beat_gap_max_ns 에 쌓이는 실측으로 뒤에 좁힌다. [why D-114]
    constexpr int64_t     kOrderBeatPeriodMs  = 50;
    constexpr int64_t     kOrderBeatSuspectMs = 30'000;
    constexpr int64_t     kOrderBeatDeadMs    = 60'000;
    ipc::HeartbeatMonitor order_monitor(
        ipc::HeartbeatConfig{kOrderBeatPeriodMs, kOrderBeatSuspectMs, kOrderBeatDeadMs});
    bool                  order_beat_lost = false;

    // 답 없는 요청 훑기는 든 수에 비례하므로 한 바퀴마다 하지 않는다. 시한도 같은 이유로 넉넉하다 —
    //  주문 하나가 증권사 왕복에 묶이는 동안 뒤엣것은 줄을 서서 기다리는 것이 정상이다. [why D-114]
    constexpr auto    kOverdueScanInterval  = std::chrono::seconds(5);
    constexpr int64_t kOrderAnswerTimeoutNs = 60LL * 1'000'000'000;
    auto              last_overdue_scan     = std::chrono::steady_clock::now();

    // 신호 순번·교체 보류·차단 로그는 이 스레드 소유라 디스패처를 여기에 둔다. 싱크가 pipeline_.requests에 넣는 유일한
    //  자리 — 단일 생산자 규약은 이 람다가 이 스레드에서만 불린다는 데 기댄다. [why D-063]
    SignalDispatcher dispatcher(
        order_gate_,
        *ledger_snapshot_,
        [this, &pending_requests](const OrderSignal& signal)
        {
            ++signal_count_;
#ifdef HAS_ZMQ
            if (zmq_bridge_)
            {
                zmq_bridge_->publish_signal(signal);
            }
#endif
            // 경계를 건너는 모양으로 바꾼다. 신호 안의 글자 칸 넷(종목코드·계좌·주문 이름·판단 근거)은
            //  std::string 이라 포인터를 물고 있어 그대로는 공유 쪽지를 건널 수 없다. [why D-114]
            bool                    truncated = false;
            const ipc::OrderRequest request   = ipc::to_request(signal, &truncated);

            if (truncated)
            {
                // 잘린 것은 판단 근거나 주문 이름이다. 주문은 그대로 나가지만 나중에 "왜 샀나"를 읽을 때
                //  글이 짧아져 있다 — 몇 건이 그랬는지는 남긴다.
                pipeline_.order_reason_truncated.fetch_add(1, std::memory_order_relaxed);
            }

            if (!pipeline_.requests->push(request))
            {
                // 주문 스레드가 KIS 왕복에 묶여 큐가 찬 상태. 여기서 빌 때까지 돌면 전략 스레드가 서고 그 뒤로
                //  호가·체결 큐까지 밀려 판단이 옛 틱으로 흐른다 — 신호를 버리고 센다. 잃는 것은 신호 하나고
                //  조건이 남아 있으면 다음 틱·봉이 다시 만든다(FORCE_LIQ는 2초마다 재발주). [why D-073]
                const auto count = pipeline_.order_dropped.fetch_add(1, std::memory_order_relaxed) + 1;

                if (count == 1 || count % ShardPipeline::kDropLogEvery == 0)
                {
                    LOG_WARN("[전략] 주문 큐 가득 — 신호 버림 " + signal.ticker + " " +
                             (signal.side == OrderSide::BUY ? "BUY" : "SELL") + " (누적 " + std::to_string(count) + ")");
                }

                return;
            }

            // 링이 스스로 고수위를 재지 않아 넣은 쪽이 한 번 본다. [inv] 넣는 쪽이 이 스레드 하나다.
            if (const auto pending = static_cast<uint64_t>(pipeline_.requests->pending());
                pending > pipeline_.order_high_water.load(std::memory_order_relaxed))
            {
                pipeline_.order_high_water.store(pending, std::memory_order_relaxed);
            }

            pipeline_.order_wake.notify();
            // 보냈다고 적는다. 답이 오면 지워지고, 문턱을 넘게 안 오면 재전송 후보로 나온다. [why D-114]
            pending_requests.note_sent(signal.sequence, trace::now_ns());
        },
        std::chrono::steady_clock::now(),
        SignalDispatcher::SystemIds{force_liquidation_index_, limit_trim_index_});
    dispatcher.set_label([this](const std::string& ticker) { return ticker_label(ticker); });
    dispatcher.set_exit_managed_check([this](symbol::SymbolId symbol) { return is_exit_managed(symbol); });

    // 틱은 샤드 스레드가 돌린다(shard_thread_fn). 여기는 샤드가 보낸 봉투를 디스패처 한 곳으로 모아
    //  순번·슬롯·교체·강제청산 같은 종목 횡단 판단을 한 스레드에서 한다(원칙 4). 사람이 낸 수동주문은
    //  이 스레드를 안 지난다 — 주문 쪽이 자기 스레드에서 꺼낸다. [why D-071][why D-114]
    // 유휴 전이: 마지막 일 뒤 이 시간은 yield로 돌고, 넘기면 pipeline_.strategy_wake에서 잔다. [why D-071]
    constexpr auto kStratSpinBudget = std::chrono::microseconds(200);
    std::chrono::steady_clock::time_point idle_since{};
    // 봉투가 몰리면 아래 비우기 한 번이 길어진다 — 2,700종목·초당 1,400만 건에서 3,702ms를 쟀다(한 바퀴 머리에서만
    //  찍었을 때). 그동안 주문 쪽이 멀쩡한 전략을 죽었다고 본다. 비우는 중에도 이 건수마다 한 번 찍는다 —
    //  relaxed 저장 하나가 봉투 256개에 나뉘어 한 건당 비용은 사실상 없다. [why D-114]
    constexpr uint32_t kBeatEveryEnvelopes = 256;

    while (!stop_token.stop_requested())
    {
        // 살아 있다고 찍는다 — 주문 쪽이 이 값의 공백만 보고 판정한다. 한 바퀴가 길어지면 공백도 길어지니
        //  부하 아래 실측(HeartbeatMonitor::max_gap_ns)으로 문턱을 정한다. [why D-114]
        pipeline_.strategy_heartbeat->beat(trace::now_ns());

        // 주문 쪽 답을 걷어 기다리던 것에서 지운다.
        ipc::OrderResponse response;

        while (pipeline_.order_responses->pop(response))
        {
            pending_requests.note_response(response.sequence);
        }

        // 주문 쪽이 살아 있는가 — 박동 공백만 본다. 찍고 세기만 하고 아무것도 멈추지 않는다: 문턱이
        //  증권사 왕복 상한에서 온 값이라 좁히기 전에 조치를 붙이면 멀쩡한 주문 스레드를 죽었다고 읽는다.
        //  이것으로 보이는 것은 '주문 쪽이 죽고 다시 안 뜬' 갈래다 — 기동 번호는 다시 떠야 바뀌므로
        //  제어 스레드의 감시가 못 잡고, 요청 큐가 가득 찬 뒤 뜨는 경고는 주문 쪽이 바쁘다는 뜻이라
        //  원인을 반대로 가리킨다. [why D-114]
        const auto order_step = order_monitor.observe(trace::now_ns(), pipeline_.order_heartbeat->last_ns());
        pipeline_.order_beat_gap_max_ns.store(order_monitor.max_gap_ns(), std::memory_order_relaxed);

        if (order_monitor.take_dead_once())
        {
            order_beat_lost = true;
            LOG_ERROR("[전략] 주문 박동이 끊겼다 — 여기서 낸 주문은 나가지 않는다 (가장 긴 공백 " +
                      std::to_string(order_monitor.max_gap_ns() / kNanosecondsPerMillisecond) + "ms)");
        }
        else if (order_step == ipc::HeartbeatMonitor::Step::kHealthy && order_beat_lost)
        {
            order_beat_lost = false;
            LOG_WARN("[전략] 주문 박동이 돌아왔다");
        }

        // 전략 쪽 생산자들이 넣은 제어 요청을 경계 너머로 옮긴다 — 보내는 쪽이 하나여야 하는 자리다. [why D-114]
        relay_control_requests();

        const auto loop_now = std::chrono::steady_clock::now();

        // 답이 안 오는 요청을 보는 자리. 저울은 처음부터 있었지만 시한을 묻는 데가 없어 아무도 못 보고
        //  있었다. 고치지는 않는다 — 위 주석대로 다시 보내면 이중 발주다. 세고 찍어 판정 행이 본다.
        //  [why D-114]
        if (loop_now - last_overdue_scan >= kOverdueScanInterval)
        {
            last_overdue_scan = loop_now;

            if (const auto overdue = pending_requests.oldest_overdue(trace::now_ns(), kOrderAnswerTimeoutNs))
            {
                const auto count = pipeline_.order_answer_overdue.fetch_add(1, std::memory_order_relaxed) + 1;

                if (count == 1 || count % ShardPipeline::kDropLogEvery == 0)
                {
                    LOG_WARN("[전략] 답이 없는 주문 요청 순번=" + std::to_string(*overdue) + " (누적 " +
                             std::to_string(count) + ", 자리가 모자라 버린 기다림 " +
                             std::to_string(pending_requests.evicted()) + ")");
                }
            }
        }

        // G3 강제청산 — force_liquidate 동안 보유 전량(미체결 매도 제외) 시장가 매도를 2초마다 다시 낸다.
        if (force_liquidate_.load(std::memory_order_relaxed))
        {
            dispatcher.force_liquidate(loop_now);
        }

        // 종목당 명목 한도 초과분 정리 — 기동 20초 뒤(잔고 시드가 끝난 뒤) 한 번.
        dispatcher.trim_excess_once(loop_now);

        // 보호 주문 표 — 전략이 등록해 둔 손절·트레일을 주문 쪽에서 본다(전략이 멈춰도 나간다).
        run_protective_orders(dispatcher, loop_now);

        bool did_work = false;

        try
        {
            // 샤드가 보낸 신호 봉투 — 국면 게이트(비활성 전략의 신규 매수)와 청산 관리 티커 차단은 디스패처가 한다.
            uint32_t envelopes_since_beat = 0;

            while (auto entry = pipeline_.shard_out.pop())
            {
                dispatcher.from_strategy(entry->active, entry->exit_manager, entry->signal);
                did_work = true;

                if (++envelopes_since_beat >= kBeatEveryEnvelopes)
                {
                    envelopes_since_beat = 0;
                    pipeline_.strategy_heartbeat->beat(trace::now_ns());
                }
            }
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[StrategyThread] 예외: " + std::string(exception.what()));
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        // 유휴: 짧게 돌다가 잔다. 틱이 몰리는 구간은 spin 안에서 받고, 조용하면 생산자 notify로 깬다.
        //  상한 10ms는 flush_held·강제청산 2초 주기 같은 시각 기반 작업의 해상도다(예전 100us sleep이 실측 15.6ms였다).
        const auto now_i = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_i;
        }

        if (now_i - idle_since < kStratSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        pipeline_.strategy_wake.wait_for(10ms, stop_token, [this] { return pipeline_.shard_out.empty(); });
    }

    LOG_INFO("[StrategyThread] 종료");
}

void Engine::shard_thread_fn(std::stop_token stop_token, uint32_t row)
{
    auto& shard = *pipeline_.shards[row];
    thread_name::set_current("Shard " + std::to_string(row));
    LOG_INFO("[Shard " + std::to_string(row) + "] 시작");

    // strategy_.list 무락 순회용 StrategyBase* 스냅샷. data_thread의 재스캔 등록·해제가
    // strategy_.version을 올릴 때만 락 하에 재구성한다(틱마다 락 회피). 뗀 전략은 strategy_.retired가
    // 붙들고 있어 재구성 전의 옛 포인터도 유효하다(reap_retired가 seen 버전을 보고 파기).
    std::vector<StrategyBase*> snapshot;
    uint64_t                   seen_version = static_cast<uint64_t>(-1);
    const auto                 symbol_id_of   = [this](std::string_view ticker) { return lookup_symbol(ticker); };

    // 신호 봉투 — 전략 상태(active·id)는 여기서 읽는다. 전략 스레드는 전략 객체를 보지 않는다.
    //  tick_ns는 체결 경로만 0이 아니다 — 봉·호가는 CSV에서 -1(측정 불가)로 남는다.
    const auto emit = [this, row](StrategyBase* strategy, const OrderSignal& signal, int64_t tick_ns)
    {
        strategy::Emitted emitted;
        emitted.signal           = signal;
        emitted.signal.tick_at_ns = tick_ns;

        // 전략이 안 찍었으면 여기서 한 번. 신호 종목이 지금 틱과 다를 수 있어(테마·청산) 틱 id를 그대로 쓰지 않는다.
        if (emitted.signal.symbol_id == symbol::kNone)
        {
            emitted.signal.symbol_id = lookup_symbol(emitted.signal.ticker);
        }

        emitted.signal.strategy_index = strategy->strategy_index();
        emitted.active                = strategy->is_active();
        emitted.exit_manager          = strategy->is_exit_manager();

        // 전략 스레드가 정체돼 봉투 큐가 찬 상태 — 기다리면 이 샤드의 틱이 밀린다. 신호를 버리고 센다(D-073과 같은 규칙).
        if (!pipeline_.shard_out.push(std::move(emitted)))
        {
            const auto count = pipeline_.shard_dropped.fetch_add(1, std::memory_order_relaxed) + 1;

            if (count == 1 || count % ShardPipeline::kDropLogEvery == 0)
            {
                LOG_WARN("[Shard " + std::to_string(row) + "] 봉투 큐 가득 — 신호 버림 " + signal.ticker + " (누적 " +
                         std::to_string(count) + ")");
            }

            return;
        }

        pipeline_.strategy_wake.notify();
    };
    // 운영단말 현재가용 캐시 — id 배열에 relaxed store 둘. 종목은 샤드 하나만 지나므로 쓰는 스레드도 하나다.
    const auto on_price = [this](symbol::SymbolId id, double price) { set_last_price(id, price); };

    // 유휴 전이: 전략 스레드와 같은 정책 — 200us yield 뒤 자기 게이트에서 잔다. [why D-071]
    constexpr auto                        kSpinBudget = std::chrono::microseconds(200);
    std::chrono::steady_clock::time_point idle_since{};

    while (!stop_token.stop_requested())
    {
        const uint64_t version = strategy_.version.load(std::memory_order_acquire);

        if (version != seen_version)
        {
            {
                std::lock_guard<std::mutex> lock(strategy_.mutex);
                snapshot.clear();
                snapshot.reserve(strategy_.list.size());

                // 자기 샤드가 소유한 전략만 — 다른 샤드의 전략 객체는 여기서 만지지 않는다. M=1이면 전부.
                for (auto& strategy : strategy_.list)
                {
                    if (strategy->shard_index() == row)
                    {
                        snapshot.push_back(strategy.get());
                    }
                }

                shard.rebuild(snapshot, version, symbol_id_of);
            }

            LOG_INFO("[Shard " + std::to_string(row) + "] 라우팅 재구성: 전략 " + std::to_string(snapshot.size()) +
                     "개, 종목 배정 " + std::to_string(shard.router().routes()) + "건, 전부 받는 전략 " +
                     std::to_string(shard.router().all_count()) + "개");
            seen_version = version;
            // 뗀 전략은 모든 샤드가 새 스냅샷을 본 뒤에야 파기한다 — 가장 뒤처진 샤드의 버전을 알린다.
            uint64_t min_seen = version;

            for (const auto& other : pipeline_.shards)
            {
                min_seen = std::min(min_seen, other->seen_version());
            }

            strategy_.seen_version.store(min_seen, std::memory_order_release);
        }

        bool did_work = false;

        try
        {
            did_work = shard.step(emit, on_price, symbol_id_of);
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[Shard " + std::to_string(row) + "] 예외: " + std::string(exception.what()));
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        const auto now_i = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_i;
        }

        if (now_i - idle_since < kSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        shard.wake().wait_for(10ms, stop_token, [&shard] { return shard.empty(); });
    }

    LOG_INFO("[Shard " + std::to_string(row) + "] 종료");
}
