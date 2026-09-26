// 제어 쪽 — 제어 스레드(감시·피드 끊김 폴백·마감 자기 종료)와 통계 모으기.
//  Engine 클래스는 그대로다. Engine.cpp 가 2,700줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  control_thread_fn()          : spawn_threads() 가 control 스레드로 띄운다
//  activate_rest_fallback()     : connect_feed() 가 첫 연결에 실패할 때, 그리고 control_thread_fn() 가 재연결이 거듭 실패할 때
//  step_session_end()           : control_thread_fn() 가 한 바퀴마다
//  write_state_marker()         : 마감 자기 종료·ZMQ KILL·운영단말 KILL
//  queue_statistics()           : 부하 측정(bench_engine_load) — 읽기 전용이라 어느 스레드에서 불러도 된다
//  print_statistics()           : stop()

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace std::chrono_literals;

// 큐 수위·버린 건수를 한 번에 모은다. 읽기 전용이라 어느 스레드에서 불러도 된다 — 값마다 relaxed로 읽으므로
//  한 시점의 일관된 단면은 아니다. 수위·버린 수는 추세만 보면 되는 값이라 그걸로 충분하다.
Engine::QueueStatistics Engine::queue_statistics() const
{
    QueueStatistics statistics;

    for (const auto& shard : pipeline_.shards)
    {
        statistics.shard_high_water = std::max(statistics.shard_high_water, shard->high_water());
    }

    statistics.shard_out_size   = pipeline_.shard_out.size();
    statistics.trade_dropped    = trade_drop_count_.load(std::memory_order_relaxed);
    statistics.order_high_water = static_cast<size_t>(pipeline_.order_high_water.load(std::memory_order_relaxed));
    statistics.fill_high_water  = pipeline_.fill_queue.high_water();
    statistics.shard_dropped    = pipeline_.shard_dropped.load(std::memory_order_relaxed);
    statistics.order_dropped    = pipeline_.order_dropped.load(std::memory_order_relaxed);
    statistics.order_stale      = pipeline_.order_stale.load(std::memory_order_relaxed);
    statistics.fill_dropped     = pipeline_.fill_dropped.load(std::memory_order_relaxed);
    statistics.order_duplicate  = pipeline_.order_duplicate.load(std::memory_order_relaxed);
    statistics.order_response_dropped = pipeline_.order_response_dropped.load(std::memory_order_relaxed);
    statistics.strategy_beat_gap_max_ns = pipeline_.strategy_beat_gap_max_ns.load(std::memory_order_relaxed);
    statistics.order_beat_gap_max_ns    = pipeline_.order_beat_gap_max_ns.load(std::memory_order_relaxed);
    statistics.feed_beat_gap_max_ns     = pipeline_.feed_beat_gap_max_ns.load(std::memory_order_relaxed);
    statistics.order_answer_overdue     = pipeline_.order_answer_overdue.load(std::memory_order_relaxed);
    return statistics;
}

void Engine::print_statistics() const
{
    LOG_INFO("[Engine] 수집: " + std::to_string(data_count_.load()) +
             "  신호: " + std::to_string(signal_count_.load()) + "  주문: " + std::to_string(order_count_.load()));
}

// ─── ZMQ 제어 스레드 ──────────────────────────────────────────────────────
// ZmqBridge 자체 스레드가 REP 소켓을 처리하므로 이 스레드는
// running_ 감시 + WebSocket stale 감지를 담당
// ─── WS 피드 끊김 시 REST 폴링으로 낮추기 / 복귀 ──────────────────────────────
//  WS 모드에서 호가·체결이 끊기면 전략은 입력을 하나도 못 받는다(폴링 분기가 꺼져 있으므로).
//  매매를 세우는 대신 data_thread의 폴링 분기를 켜서 30초 주기 현재가 틱으로 이어간다.
//  느려진 대가는 있지만 눈이 아주 감기는 것보다는 낫다는 판단이다.
bool Engine::activate_rest_fallback(const std::string& reason)
{
    if (feed_.rest_price_feed)
    {
        return true; // 처음부터 폴링 — 낮출 것이 없다
    }

    // 폴링이 쓸 시세 소스. 모의 도메인은 시세 REST가 HTTP 500이라 실전 시세 클라이언트가
    //  없고 주문계좌마저 모의면 낮춰봐야 틱이 안 나온다. 그때는 거짓 안심을 주지 않는다.
    //  브로커 없는 기동(피드 주입)도 같다 — 낮출 REST가 없다.
    if (!feed_.kis || (!feed_.quote_kis && kis_config_.is_paper))
    {
        return false;
    }

    if (!feed_.rest_fallback_engaged)
    {
        feed_.rest_fallback_engaged = true;
        feed_.rest_feed_active.store(true, std::memory_order_relaxed);
        LOG_ERROR("[Feed] WS → REST 폴링 폴백 (" + reason + ") — 틱 주기가 " +
                  std::to_string(fetch_interval_sec_) + "초로 떨어집니다. WS 복귀 시 자동 원복");
    }

    return true;
}

void Engine::deactivate_rest_fallback()
{
    if (!feed_.rest_fallback_engaged)
    {
        return;
    }

    feed_.rest_fallback_engaged = false;
    feed_.rest_feed_active.store(feed_.rest_price_feed, std::memory_order_relaxed);
    LOG_INFO("[Feed] WS 수신 정상 — REST 폴링 폴백 해제, 실시간 피드로 복귀");
}

void Engine::control_thread_fn(std::stop_token stop_token)
{
    thread_name::set_current("Control");
    using namespace std::chrono_literals;
    constexpr int kCheckIntervalSec = 5;
    // 큐 고수위는 장 외에도 찍는다 — 큐 크기가 맞는지의 근거가 되므로 WS 유무·개장 여부와 무관하다. [why D-071]
    constexpr int kHighWaterEvery = 12; // 5초 × 12 = 1분
    // 박동 공백은 나노초로 들고 다니다가 찍을 때만 밀리초로 줄인다.
    constexpr int64_t kNanosecondsPerMillisecond = 1'000'000;
    int high_water_tick = 0;
    constexpr int kTokenEvery = 60; // 5초 × 60 = 5분
    int token_tick = 0;

    while (wake::sleep_unless_stopped(stop_token, std::chrono::seconds(kCheckIntervalSec)))
    {
        flush_fill_overflow();

        // 붙어 있는 쪽지의 기동 번호가 바뀌었으면 건너편이 죽고 다시 떴다는 뜻이다. 그 판의 큐·장부
        //  사본은 내가 아는 것이 아니라, 그대로 두면 신호가 허공으로 나간다 — 같이 내려가 감시견이
        //  짝을 다시 띄우게 한다. 붙은 쪽(전략 프로세스)에서만 0이 아니다. [why D-114]
        if (peer_boot_generation_ != 0)
        {
            if (const uint64_t now_generation = layout_region_.boot_generation();
                now_generation != peer_boot_generation_)
            {
                LOG_ERROR("[Control] 건너편이 다시 떴다 — 공유 쪽지 기동 번호 " +
                          std::to_string(peer_boot_generation_) + " → " + std::to_string(now_generation) +
                          ". 옛 판을 들고 주문을 내지 않도록 같이 내려간다");
                request_shutdown("건너편 프로세스 재기동(공유 쪽지 기동 번호가 바뀌었다)");
                break;
            }

            // 건너편이 종료 사유를 적고 나갔으면 여기도 따라 내려간다. 예전에는 감시견이 남은 쪽을
            //  강제로 죽였고, 그러면 stop()이 안 돌아 이쪽 종료 사유 칸이 빈 채로 남아 정상 배포와
            //  크래시가 갈리지 않았다. 이제 스스로 나가고, 감시견은 그동안 기다렸다가 안 나가면
            //  그때 강제로 죽인다. 붙은 쪽(전략 프로세스)에서만 돌린다. [why D-114]
            if (const auto peer_reason = layout_region_.shutdown_reason();
                peer_reason != ipc::SharedShutdownReason::kNone)
            {
                LOG_WARN("[Control] 건너편이 종료 사유를 적고 나갔다(사유 번호 " +
                         std::to_string(static_cast<uint32_t>(peer_reason)) + ") — 옛 판을 들고 있지 않도록 같이 내려간다");
                request_shutdown("건너편 프로세스 종료(공유 쪽지에 사유가 적혔다)",
                                 ipc::SharedShutdownReason::kOperator);
                break;
            }
        }

        if (++high_water_tick >= kHighWaterEvery)
        {
            high_water_tick = 0;
            std::string shard_high_water; // 샤드마다 세 열 가운데 가장 높았던 셀

            for (const auto& sh : pipeline_.shards)
            {
                if (!shard_high_water.empty())
                {
                    shard_high_water += ',';
                }

                shard_high_water += std::to_string(sh->high_water());
            }

            LOG_INFO("[큐 고수위] shard=" + shard_high_water + "/" + std::to_string(ShardPipeline::kTickCellCapacity) + " shard_out=" + std::to_string(pipeline_.shard_out.size()) + "/" +
                     std::to_string(pipeline_.shard_out.capacity()) + " shard_dropped=" +
                     std::to_string(pipeline_.shard_dropped.load(std::memory_order_relaxed)) + " order=" +
                     std::to_string(pipeline_.order_high_water.load(std::memory_order_relaxed)) + "/" +
                     std::to_string(pipeline_.requests->capacity()) +
                     " fill=" + std::to_string(pipeline_.fill_queue.high_water()) + "/" + std::to_string(pipeline_.fill_queue.capacity()) +
                     " fill_dropped=" + std::to_string(pipeline_.fill_dropped.load(std::memory_order_relaxed)) +
                     " fill_overflowed=" + std::to_string(pipeline_.fill_overflowed.load(std::memory_order_relaxed)) +
                     " order_dropped=" + std::to_string(pipeline_.order_dropped.load(std::memory_order_relaxed)) +
                     " order_stale=" + std::to_string(pipeline_.order_stale.load(std::memory_order_relaxed)) +
                     " order_duplicate=" + std::to_string(pipeline_.order_duplicate.load(std::memory_order_relaxed)) +
                     " order_implausible=" +
                     std::to_string(pipeline_.order_implausible.load(std::memory_order_relaxed)) +
                     " order_truncated=" +
                     std::to_string(pipeline_.order_reason_truncated.load(std::memory_order_relaxed)) +
                     " order_response_dropped=" +
                     std::to_string(pipeline_.order_response_dropped.load(std::memory_order_relaxed)) +
                     " beat_gap_max=" +
                     std::to_string(pipeline_.strategy_beat_gap_max_ns.load(std::memory_order_relaxed) /
                                    kNanosecondsPerMillisecond) + "ms" +
                     // 주문 쪽 박동과 답 없는 요청 — 앞엣것에는 증권사 왕복이 들어 있고, 뒤엣것은 0이어야 한다.
                     " order_beat_gap_max=" +
                     std::to_string(pipeline_.order_beat_gap_max_ns.load(std::memory_order_relaxed) /
                                    kNanosecondsPerMillisecond) + "ms" +
                     " order_answer_overdue=" +
                     std::to_string(pipeline_.order_answer_overdue.load(std::memory_order_relaxed)) +
                     // 시세 쪽 박동 — 제어 바퀴 5초가 그대로 들어온다. 이 값이 사망 문턱의 근거다. [why D-137]
                     " feed_beat_gap_max=" +
                     std::to_string(pipeline_.feed_beat_gap_max_ns.load(std::memory_order_relaxed) /
                                    kNanosecondsPerMillisecond) + "ms" +
                     // 장부 사본 — 몇 판 나왔는지와 못 실은 남의 계좌 줄 수. 뒤엣것은 0이어야 한다. [why D-114]
                     " ledger_gen=" + std::to_string(ledger_snapshot_->generation()) +
                     " ledger_foreign=" + std::to_string(order_gate_.ledger().ledger_foreign_account_rows()) +
                     // 제어 요청 — 못 보낸 줄, 경계 너머로 못 옮긴 줄, 반쪽 표로 보고 버린 줄. 셋 다 0이어야 한다. [why D-114]
                     " control_dropped=" + std::to_string(control_plane_.dropped()) +
                     " control_relay_dropped=" +
                     std::to_string(control_plane_.relay_dropped()) +
                     " control_discarded=" +
                     std::to_string(control_plane_.discarded()) +
                     // 티커→번호 — 등록을 주문 쪽에서 못 받은 수, 표에 없는 티커로 잦은 자리가 불린 수. 둘 다 0이어야 한다. [why D-114]
                     " symbol_register_timeout=" + std::to_string(symbol_register_timeouts()) +
                     " symbol_lookup_miss=" + std::to_string(symbol_lookup_misses()) +
                     // 전략 이름→번호 — 이름표 등록을 못 받은 수. 0이 아니면 그 전략 손익이 빈 칸에 붙는다. [why D-114]
                     " strategy_register_timeout=" + std::to_string(strategy_register_timeouts()) +
                     // 구독 — 상한에 밀려 소켓에 못 건 종목 수. 0이어야 한다(밀린 종목은 WS 틱이 없다). [why D-114]
                     " watch_overflow=" + std::to_string(watch_overflows()) +
                     // 시세 통로 — 큐가 차서 못 넘긴 건수와, 값이 말이 안 돼 꺼내는 쪽이 버린 건수. 둘 다 0이어야 한다. [why D-114]
                     " feed_channel_overflow=" + std::to_string(feed_channel_overflows()) +
                     " feed_channel_discarded=" + std::to_string(feed_channel_discarded()) +
                     // 경계를 실제로 넘은 건수 — 공유 칸에서 읽어 어느 역할에서 봐도 같은 값이다. 갈라 띄운 날에
                     //  둘 다 0 이면 시세가 한 건도 안 넘어간 것이다(한 프로세스로 돌면 원래 둘 다 0 이다). [why D-114]
                     " feed_channel_sent=" + std::to_string(feed_channel_sent()) +
                     " feed_channel_received=" + std::to_string(feed_channel_received()) +
                     // 체결 통로 — 큐가 차서 못 넘긴 건수와 값이 말이 안 돼 버린 건수. 둘 다 0이어야 한다.
                     //  0이 아니면 주문 쪽 예약 수량이 안 풀려 총노출을 이중계상한다. [why D-114 단계 5]
                     " fill_channel_overflow=" + std::to_string(fill_channel_overflows()) +
                     " fill_channel_discarded=" + std::to_string(fill_channel_discarded()) +
                     // 체결이 실제로 경계를 넘었는지 — 공유 칸에서 읽어 어느 역할에서 봐도 같은 값이다.
                     //  셋으로 갈라 띄운 날에 체결이 있었는데 둘 다 0 이면 통로가 막힌 것이다. [why D-114 단계 5]
                     " fill_channel_sent=" + std::to_string(fill_channel_sent()) +
                     " fill_channel_received=" + std::to_string(fill_channel_received()));
        }

        if (++token_tick >= kTokenEvery)
        {
            token_tick = 0;

            // 만료 30분 전에 여기서 미리 갱신한다. 발급 왕복을 파이프라인 밖 스레드가 떠안아야
            //  전략·데이터 스레드의 http_get이 5분 margin에 걸리지 않는다. [why D-073]
            if (feed_.kis)
            {
                feed_.kis->refresh_token(std::chrono::minutes(30));
            }

            if (feed_.quote_kis)
            {
                feed_.quote_kis->refresh_token(std::chrono::minutes(30));
            }
        }

        // 마감 자기 종료 — WS 유무와 무관하게 매 주기 [why D-098]. 판정이 주문 큐를 보므로 주문 쪽이 한다. [why D-114]
        if (runs_order_side())
        {
            step_session_end();
        }

        // 재연결은 소켓을 쥔 쪽이 본다 — 단계 5부터 소켓은 시세 프로세스에 있다(앱키 하나에 세션 하나라
        //  체결통보도 같은 소켓에 실린다). 주문·전략 프로세스는 이 포인터가 비어 있어 아래를 통째로
        //  건너뛴다. [why D-114 단계 5]
        if (!feed_.websocket)
        {
            continue;
        }

        // 살아 있다고 찍는다. 시세가 죽으면 체결통보가 주문 쪽에 안 들어와 예약 수량이 안 풀리므로
        //  (총노출 이중계상) 이 칸의 공백이 그 사고를 가장 먼저 알린다. 찍는 간격은 이 바퀴의 5초다 —
        //  전략·주문 칸과 달리 hot loop 가 아니라 소켓이 깨우는 쪽이라, 문턱은 그 간격 위에서 잡는다.
        //  [why D-114 단계 5]
        pipeline_.feed_heartbeat->beat(trace::now_ns());

        // 전략 쪽이 보낸 구독 요청을 꺼내 목록에 올리고, 이어서 소켓에 건다. 꺼내는 자리가 여기 하나라
        //  시세 제어 줄은 받는 쪽이 하나로 선다(SPSC). [why D-114 단계 5]
        apply_feed_control_requests();
        drain_pending_subscriptions();
        rebalance_websocket_slots();

        // 장 외 시간에는 stale이 정상 — 장 중에만 묻는다. 전이 판정은 감독기, 소켓·폴백 적용은 여기. [why D-071]
        const bool market_open = ::kst::any_market_open(std::time(nullptr));
        const bool stale       = market_open && feed_.websocket->is_stale(feed_.feed_sup.config().stale_sec);
        const auto step        = feed_.feed_sup.observe(market_open, stale, std::chrono::steady_clock::now());

        if (step == feed::Supervisor::Step::kHealthy)
        {
            deactivate_rest_fallback(); // 폴백으로 낮춰 뒀다면 WS로 되돌린다(전이 시에만 동작)
            continue;
        }

        if (step != feed::Supervisor::Step::kReconnect)
        {
            continue;
        }

        LOG_WARN("[Control] WebSocket " + std::to_string(feed_.feed_sup.config().stale_sec) +
                 "초 이상 시세 미수신 — 재연결 시도");
        std::vector<WatchSpec> specifications_copy;
        {
            std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_); // data_thread의 재스캔 push_back과 겹친다
            specifications_copy = watch_specifications_;
        }

        // 다시 걸 때 칸이 모자라면 뒤에 선 종목이 밀린다 — 우선순위 순으로 세워 보유 종목이 먼저 칸을 받게 한다. [why D-132]
        std::stable_sort(specifications_copy.begin(), specifications_copy.end(),
                         [this](const WatchSpec& left, const WatchSpec& right) { return websocket_slot_priority(left) < websocket_slot_priority(right); });

        // 소켓이 여럿이면 멈춘 것만 다시 잇는다 — 살아 있는 소켓의 종목은 그 사이에도 틱이 흐른다. 하나면 끊고 다시 잇는 것.
        const bool ok    = feed_.websocket->reconnect_stale(specifications_copy, feed_.feed_sup.config().stale_sec);
        const auto after = feed_.feed_sup.on_reconnect(ok, std::chrono::steady_clock::now());

        if (ok)
        {
            LOG_INFO("[Control] WebSocket 재연결 성공");
            continue;
        }

        LOG_ERROR("[Control] WebSocket 재연결 실패(" + std::to_string(feed_.feed_sup.fail_streak()) + "회) — " +
                  std::to_string(feed_.feed_sup.last_backoff_sec()) + "초 후 재시도");

        // 반복 실패 시에만 대응한다(1회 실패로 즉시 조치하면 순간 장애에도 흔들린다).
        //  먼저 REST 폴링으로 낮춰 매매를 이어가고, 그것마저 불가할 때 kill switch로 멈춘다.
        //  예전에는 곧장 kill switch였다 — 시세 경로가 하나 죽었다고 매매 전체를 세울 이유는 없다.
        if (after == feed::Supervisor::After::kFallback)
        {
            if (!activate_rest_fallback("재연결 " + std::to_string(feed_.feed_sup.fail_streak()) + "회 실패"))
            {
                LOG_ERROR("[Control] 폴링 폴백 불가(시세 소스 없음) — kill switch 작동");
                request_kill_switch(true);
            }
        }
    }
}

// ─── 마감 자기 종료 (D-098) ───────────────────────────────────────────────
//  판정은 SessionEndJudge, 여기는 적용만. "주문 큐가 비었다"는 요청 면 기준이다 — order_thread가 꺼낸 뒤 KIS 왕복
//  중인 한 건은 stop()의 join이 끝까지 기다리고, OrderRateLimiter의 재시도 큐는 세션 창 밖이라 게이트가 어차피 막는다.
void Engine::step_session_end()
{
    const int  now_sec_of_day = ::kst::sec_of_day(std::time(nullptr));
    // 이 자리는 보내는 쪽도 받는 쪽도 아니다(감시 스레드) — 공유 칸만 보는 in_flight() 로 묻는다.
    //  pending() 은 내가 보낸 수라 여기서는 늘 0이고 큐에 주문이 남았는데도 비었다고 본다. readable() 은
    //  받는 쪽 제 자리 값을 읽어, 주문 스레드가 꺼내는 것과 겹친다(TSAN 확인 2026-09-24). [why D-114]
    const bool orders_pending = pipeline_.requests->in_flight() > 0;
    const auto step           = session_end_.observe(now_sec_of_day, orders_pending);

    switch (step)
    {
    case session_end::Judge::Step::kNone:
        return;

    case session_end::Judge::Step::kClosed:
        LOG_INFO("[Engine] 마지막 매매 창 닫힘 — " + std::to_string(session_end_.config().grace_sec) +
                 "초 유예 뒤 주문 큐가 비면 스스로 종료한다");
        return;

    case session_end::Judge::Step::kShutdown:
        write_state_marker("session_done", "마감 자기 종료(주문 큐 비움)");
        request_shutdown("마감 자기 종료 — 창 닫힘 + 유예 " + std::to_string(session_end_.config().grace_sec) + "초, 주문 큐 비움",
                         ipc::SharedShutdownReason::kSessionEnd);
        return;

    case session_end::Judge::Step::kShutdownForced:
        LOG_ERROR("[Engine] 마감 뒤 " + std::to_string(session_end_.config().drain_limit_sec) + "초가 지나도 주문 큐 " +
                  std::to_string(pipeline_.requests->in_flight()) + "건이 남아 강제 종료한다");
        write_state_marker("session_done", "마감 자기 종료(배출 한도 초과, 강제)");
        request_shutdown("마감 자기 종료 — 배출 한도 초과(강제)", ipc::SharedShutdownReason::kSessionEnd);
        return;
    }
}

// 표지 파일은 repo 루트 기준 상대 경로다 — 트레이더는 반드시 repo 루트에서 띄운다(감시견도 같은 경로를 본다).
//  실패해도 엔진은 멈추지 않는다 — 그러면 감시견이 -Until 마감 판정으로 되돌아갈 뿐이다.
//  instance_가 있으면 이름에 붙인다(session_done_live_2026-09-29) — 계좌를 둘 돌릴 때 모의가 15:30에
//  남긴 마감 표지로 실계좌 감시견이 멈추던 것을 막는다. 감시견도 -Instance 로 같은 이름을 본다. [why D-122]
void Engine::write_state_marker(std::string_view name, std::string_view body) const
{
    const std::string now_text = ::kst::datetime(std::time(nullptr)); // "YYYY-MM-DD HH:MM:SS"
    std::string file_name(name);

    if (!instance_.empty())
    {
        file_name += "_";
        file_name += instance_;
    }

    file_name += "_";
    file_name += now_text.substr(0, 10);
    const std::filesystem::path path = std::filesystem::path("_private") / "state" / file_name;

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::app);

    if (!out)
    {
        LOG_ERROR("[Engine] 표지 파일 쓰기 실패: " + path.string());
        return;
    }

    out << body << " " << now_text.substr(11) << '\n';
    LOG_INFO("[Engine] 표지 파일 기록: " + path.string() + " — " + std::string(body));
}
