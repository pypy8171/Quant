#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include "core/WakeGate.h"
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// ─── 오늘 날짜 YYYYMMDD (KST) ────────────────────────────────────────────
//  날짜별 파일 이름에 쓴다. 원장 CSV 파일명·행 시각과 같은 기준(KST 고정, 머신 TZ 무관)이다. [why D-070]
static std::string today_ymd()
{
    return kst::date_yyyymmdd(std::time(nullptr));
}

// 거래일 YYYYMMDD 정수 — 체결통보 키의 날짜 칸. 문자열을 만들지 않는다.
static uint32_t trade_date_number(std::time_t now_utc)
{
    const std::chrono::year_month_day date = kst::date(now_utc);
    return static_cast<uint32_t>(static_cast<int>(date.year())) * 10000u + static_cast<unsigned>(date.month()) * 100u +
           static_cast<unsigned>(date.day());
}


// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    // 주문마다 부르는 곳이라 스트림을 쓰지 않는다(D-042). 6자리를 넘으면 자릿수만 늘어난다.
    return std::format("ORD-{:06}", ++sequence_);
}

// ─── 거부 사유에 KIS 오류코드 꼬리표 부착 ───────────────────────────────
//  order_thread가 EGW00201(초당 거래건수 초과)을 문자열로 판별해 적응적 재시도를 걸 수 있게,
//  응답의 err_code를 " [코드]" 형태로 reject_reason 끝에 붙인다. 코드 없으면 빈 문자열.
std::string OrderRouter::kis_error_suffix(const OrderAck& acknowledgement)
{
    return acknowledgement.error_code.empty() ? std::string() : (" [" + acknowledgement.error_code + "]");
}

// 취소가 "취소 대상 없음"으로 되돌아온 뒤 그 종목의 신규 매수를 막아 두는 시간(초).
//  전략의 재구성 주기(min_action_ms 3초 + 재조회)보다 길고, 존 이탈 청산을 늦출 만큼
//  길지는 않은 값. 이 창 안에 들어온 매수는 원주문 체결분과 겹칠 수 있다.
static constexpr int kCancelMissGuardSec = 10;
// 같은 종목·같은 전략의 시장가 매도가 접수된 뒤 체결 통보가 아직 없을 때, 같은 매도를 다시 KIS로 보내지 않는 시간(초).
//  발주 스레드가 밀리면 통보까지 몇 분이 걸릴 수 있어 전략 백오프(30초)보다 길게 잡는다. 지나면 통보를 잃은
//  것으로 보고 놓아준다 — 그 뒤는 게이트 선점 클램프·자가정리가 막는다.
static constexpr int kDupMarketSellGuardSec = 120;
// 기동 직후 유령 지정가를 하나씩 취소할 때 취소 사이에 두는 간격(ms). 초당 거래건수 상한(EGW00201)을 피할 만큼만.
static constexpr int kStaleCancelGapMs = 400;

// 게이트까지의 두 구간을 한 번에 찍는다 — 이력 가드 몫을 게이트에서 빼 둘이 겹치지 않게 한다. [why D-117]
static void stamp_gate_stages(ManagedOrder& managed_order, int64_t route_entered_ns, int64_t history_guard_ns,
                              int64_t history_lock_wait_ns)
{
    managed_order.stages.gate_us             = (trace::now_ns() - route_entered_ns - history_guard_ns) / 1000;
    managed_order.stages.history_guard_us    = history_guard_ns / 1000;
    managed_order.stages.history_lock_wait_us = history_lock_wait_ns / 1000;
}

// ─── 주문 제출 — action에 따라 라우팅 (MM-1) ─────────────────────────────
//  전 경로가 주문 스레드 하나(Engine::order_thread_fn)에서만 실행된다 — OrderGate C6의
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다.
ManagedOrder OrderRouter::submit(const OrderSignal& signal)
{
    switch (signal.action)
    {
    case OrderAction::CANCEL:  return cancel_route(signal);
    case OrderAction::REPLACE: return replace_route(signal);
    case OrderAction::NEW:
    default:                   return new_route(signal);
    }
}

// ─── 신규 주문 (기존 경로) ─────────────────────────────────────────────────
ManagedOrder OrderRouter::new_route(const OrderSignal& in_signal)
{
    auto& ledger = gate_.ledger();

    auto now = std::chrono::system_clock::now();
    // 구간 계측 시작. 여기부터 게이트 판정 끝까지가 gate_us — 주문 스레드가 HEALTH 분포에 넣는다. [why D-117]
    const int64_t route_entered_ns = trace::now_ns();
    // 이력 잠금·중복 가드에 쓴 시간 합. 아래 두 가드 블록이 더하고, gate_us에서 뺀다 — 둘을 한 칸에 두면
    //  선형 탐색 탓인지 한도 판정 탓인지 못 가른다(처방이 종목별 색인 대 분리로 서로 다르다). [why D-117]
    int64_t history_guard_ns = 0;
    // 그중 잠금을 기다린 몫. 나머지가 잠금을 쥐고 훑은 몫이다 — 처방이 서로 다르다(기다림은 잠금을 쪼개거나
    //  들고 있는 시간을 줄이는 쪽, 훑기는 종목별 색인 쪽). [why D-126]
    int64_t history_lock_wait_ns = 0;

    // 0. 한도 클램프 — 한도를 넘치면 거부 대신 한도 안으로 줄여 낸다.
    //    분할 매수 전략은 매 틱 같은 분할 단계를 다시 내므로, 넘친다고 버리면 그 종목은 하루 종일
    //    한 주도 못 나가면서 초당 주문 예산만 태운다(09-08 오전 126640·293490 반복 거부).
    //    여유가 0이면 손대지 않는다 — 아래 check()가 어느 한도에 걸렸는지 그대로 남기게 둔다.
    OrderSignal signal = in_signal; // 사본 — 아래에서 수량을 잘라 고친다
    signal.symbol_id   = symbol_of(signal); // 이력 항목이 종목 id를 들게 — 아래 비교·색인이 문자열을 안 본다
    bool sell_no_quantity = false;
    const int allowed = gate_.clamp_buy_quantity(signal);

    if (allowed == 0 && signal.side == OrderSide::SELL && signal.action == OrderAction::NEW &&
        signal.quantity > 0)
    {
        // 매도가능수량이 0이면 여기서 끊는다. BUY와 달리 아래 check()는 매도가능수량을 모르므로
        //  그대로 통과시키고, KIS가 주문을 통째로 40240000(모의투자 잔고내역이 없습니다)으로
        //  거부한다 — 한 주도 못 빠져나오면서 초당 주문 예산만 태운다(09-08 001450 105주·
        //  086450 486주·047050 254주/381주가 모두 이 경로로 전량 거부됐다).
        sell_no_quantity = true;
    }
    else if (allowed > 0 && allowed < signal.quantity && signal.side == OrderSide::SELL &&
             ledger.sellable_view(signal.account_id, signal.ticker).pending > 0)
    {
        // 매도가능이 모자란 이유가 이 세션의 예약매도(익절 지정가)라면 잘라 내지 않고, 그 예약을 취소해
        //  수량을 풀고 전량을 낸다 — 아래 sell_no_quantity 와 같은 길. 잘라 내면 나머지는 전략이 취소를 낸 뒤
        //  다음 백오프(30초 뒤)에야 나간다(09-14 15:15 012210 115주 중 100주만, 나머지는 15:16 뒤). [why D-082]
        sell_no_quantity = true;
    }
    else if (allowed > 0 && allowed < signal.quantity)
    {
        LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주", signal.ticker, signal.quantity, allowed));
        signal.quantity = allowed;
    }

    ManagedOrder managed_order;
    managed_order.order_id    = next_id();
    managed_order.signal      = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;

    ++total_count_;

    // 방금 이 종목의 취소가 "취소 대상 없음"으로 되돌아왔다면, 원주문이 이미 체결됐을 수
    //  있다. 전략은 그 결과를 보지 못한 채 대체 주문을 이어 내므로 그대로 두면 중복 매수가
    //  된다. 다음 재구성 주기에 전략이 실제 보유수량을 다시 읽을 때까지만 막는다.
    //  매도는 막지 않는다 — 노출을 줄이는 쪽이고, 늦추면 손실이 커진다.
    if (signal.side == OrderSide::BUY)
    {
        bool          blocked          = false;
        const int64_t guard_started_ns = trace::now_ns();
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_lock_wait_ns += trace::now_ns() - guard_started_ns;

            if (signal.symbol_id < cancel_miss_.size() && cancel_miss_[signal.symbol_id] != std::chrono::steady_clock::time_point{})
            {
                const auto age = std::chrono::steady_clock::now() - cancel_miss_[signal.symbol_id];

                if (age < std::chrono::seconds(kCancelMissGuardSec))
                {
                    blocked = true;
                }
                else
                {
                    cancel_miss_[signal.symbol_id] = {};
                }
            }
        }

        history_guard_ns += trace::now_ns() - guard_started_ns;

        if (blocked)
        {
            managed_order.status        = OrderStatus::REJECTED;
            managed_order.reject_reason = "직전 취소가 대상 없음 — 보유수량 재확인까지 보류";
            ++rejected_count_;
            stamp_gate_stages(managed_order, route_entered_ns, history_guard_ns, history_lock_wait_ns);
            LOG_WARN("[OrderRouter] 대체 주문 보류 [" + managed_order.order_id + "] " + signal.ticker +
                     " " + managed_order.reject_reason);
            managed_order.stages.record_us = record(managed_order, &managed_order.stages.open_orders_us);
            // 이 거부 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라 따로 세지 않는다. [why D-126]
            managed_order.stages.history_store_us = managed_order.stages.record_us;
            return managed_order;
        }
    }

    // 같은 청산의 중복 발주 차단 — 시장가 매도가 접수돼 아직 체결 통보가 없는데(발주 스레드가 밀리면 몇 분)
    //  전략이 백오프마다 같은 매도를 다시 낸다(09-14 15:15 036930 SELL 9 가 30초·60초 뒤 두 번 더 큐에 쌓임).
    //  라우터 이력에 같은 종목·같은 전략의 시장가 매도가 미체결 잔량을 들고 살아 있으면 KIS 로 보내지 않는다.
    //  KIS 호출이 없으니 발주 스레드 예산을 안 쓴다. [why D-082]
    if (signal.side == OrderSide::SELL && signal.action == OrderAction::NEW && signal.type == OrderType::MARKET)
    {
        std::string   duplicate;
        const int64_t guard_started_ns = trace::now_ns();
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_lock_wait_ns += trace::now_ns() - guard_started_ns;
            const auto now_sc = std::chrono::system_clock::now();

            for (const auto& history_entry : history_)
            {
                if (history_entry.signal.symbol_id != signal.symbol_id || history_entry.signal.strategy_index != signal.strategy_index ||
                    history_entry.signal.side != OrderSide::SELL || history_entry.signal.type != OrderType::MARKET ||
                    history_entry.signal.action != OrderAction::NEW)
                {
                    continue;
                }

                if ((history_entry.status != OrderStatus::ACCEPTED && history_entry.status != OrderStatus::SUBMITTED) ||
                    history_entry.confirmed_quantity >= history_entry.signal.quantity ||
                    now_sc - history_entry.submitted_at > std::chrono::seconds(kDupMarketSellGuardSec))
                {
                    continue;
                }

                duplicate = history_entry.order_id + " 미체결 " + std::to_string(history_entry.signal.quantity - history_entry.confirmed_quantity) + "주";
                break;
            }
        }

        history_guard_ns += trace::now_ns() - guard_started_ns;

        if (!duplicate.empty())
        {
            managed_order.status        = OrderStatus::REJECTED;
            managed_order.reject_reason = "같은 시장가 매도 진행 중 [" + duplicate + "] — 중복 발주 생략";
            ++rejected_count_;
            stamp_gate_stages(managed_order, route_entered_ns, history_guard_ns, history_lock_wait_ns);
            LOG_INFO("[OrderRouter] 중복 생략 [" + managed_order.order_id + "] " + signal.ticker + " " +
                     std::to_string(signal.quantity) + "주 → " + managed_order.reject_reason);
            managed_order.stages.record_us = record(managed_order, &managed_order.stages.open_orders_us);
            // 이 거부 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라 따로 세지 않는다. [why D-126]
            managed_order.stages.history_store_us = managed_order.stages.record_us;
            return managed_order;
        }
    }

    // 1. OrderGate 검증
    std::string reject_reason;
    OrderAck    acknowledgement;
    bool        freed = false;
    // 원장 레코드가 이 주문을 가리키는 이름표 — 내부 주문번호(ORD-NNNNNN의 숫자)와 주문 유형. ODNO는 접수 뒤에 붙는다.
    OrderGate::OrderRef order_reference{digits_to_number(managed_order.order_id), 0, signal.type};
    bool                intent_taken = false;

    if (sell_no_quantity)
    {
        // 왜 0인지 남기고, 이 프로세스가 아는 예약매도(이번 세션 이력·이전 세션 부속 파일)를 취소해
        //  수량을 풀어 본다. 거부만 하고 끝내면 예약매도가 브로커에 남은 채 청산이 하루 종일 막힌다
        //  (09-11 014530: 09:17 익절 지정가 118주가 취소 한도거부로 잔존, 이후 재기동 8회 내내 거부).
        //  풀리면 그 자리에서 재발주한 접수로 이어간다. [why D-055]
        const auto sellable_view = ledger.sellable_view(signal.account_id, signal.ticker);
        LOG_WARN(std::format("[OrderRouter] 매도가능 {}/{}주 {} — 원장 보유 {}주, 잔고 주문가능 {}주, 이 세션 미체결 매도 {}주 → 예약매도 취소 시도",
                             allowed, signal.quantity, signal.ticker, sellable_view.held, sellable_view.possible_quantity_cap, sellable_view.pending));
        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal, order_reference, intent_taken);

        if (reconcile_acknowledgement.ok())
        {
            acknowledgement = std::move(reconcile_acknowledgement);
            freed = true;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty && allowed > 0)
        {
            // 취소할 예약매도를 못 찾았지만 일부는 나갈 수 있다 — 잘라서라도 낸다(예전 클램프 경로).
            LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주 (예약매도 취소 불발)", signal.ticker, signal.quantity, allowed));
            signal.quantity = allowed;
            managed_order.signal    = signal;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty)
        {
            reject_reason = "매도가능수량 0 (미체결 매도·미결제분) — 취소할 예약매도 없음, 발주 생략";
        }
        else
        {
            // 예약매도는 취소됐는데 재매도가 거부(유량한도·전송 실패)된 것 — 수량은 풀렸으니 재시도가 낸다.
            //  09-14 15:00 096770: 취소 뒤 재매도가 EGW00201 에 걸렸는데 "취소할 예약매도 없음"으로 남아 원인을 잘못 짚었다.
            reject_reason = "예약매도 취소 뒤 재매도 실패 [" + reconcile_acknowledgement.error_code + "] — 재시도 대상";
        }
    }

    if (!reject_reason.empty() || (!freed && !gate_.check(signal, reject_reason)))
    {
        managed_order.status          = OrderStatus::REJECTED;
        managed_order.reject_reason   = reject_reason;
        stamp_gate_stages(managed_order, route_entered_ns, history_guard_ns, history_lock_wait_ns);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 거부 [" + managed_order.order_id + "] " +
                 signal.ticker + " → " + reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        managed_order.stages.record_us = record(managed_order, &managed_order.stages.open_orders_us);
        // 이 거부 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라 따로 세지 않는다. [why D-126]
        managed_order.stages.history_store_us = managed_order.stages.record_us;
        return managed_order;
    }

    // 2. KIS 주문 전송 (submit_order_acknowledgement로 ODNO + KRX 조직번호 캡처 — 정정/취소 준비)
    //    접수 왕복지연(RTT)을 재서 접수 로그에 남긴다 → log_report.py가 중앙값(p50)·상위 1%(p99) 집계.
    //    RTT 안에는 초당 한도 버킷 대기(rate_limit_acquire)가 섞여 있어 그 몫을 따로 적는다 — 09-14~18 RTT p50 2초가
    //    망 지연인지 버킷 줄서기인지 이 숫자 없이는 못 가른다. 전송 스레드 분리(T-13-2)는 이 값을 보고 정한다. [why D-117]
    managed_order.status = OrderStatus::SUBMITTED;
    stamp_gate_stages(managed_order, route_entered_ns, history_guard_ns, history_lock_wait_ns);
    const auto send_thread = std::chrono::steady_clock::now();
    const std::uint64_t bucket_wait_before_ns = kis_.rate_limit_wait_ns_this_thread();
    std::chrono::milliseconds::rep rtt_ms = 0; // count()의 타입 그대로 — MSVC는 long long이라 long이면 잘린다(C4244)
    std::chrono::milliseconds::rep bucket_wait_ms = 0;

    const int64_t journal_started_ns = trace::now_ns();

    // 원장 먼저, 전송은 그 다음 — 적히지 않은 주문은 나가지 않는다. 재기동은 이 INTENT로 미결 주문을 안다. [why D-113]
    if (!freed && !take_intent(signal, order_reference))
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "원장 저널 기록 실패 — 전송 생략";
        ++rejected_count_;
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        managed_order.stages.record_us = record(managed_order, &managed_order.stages.open_orders_us);
        // 이 거부 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라 따로 세지 않는다. [why D-126]
        managed_order.stages.history_store_us = managed_order.stages.record_us;
        return managed_order;
    }

    if (!freed)
    {
        intent_taken = true;
    }

    const int64_t transport_started_ns = trace::now_ns();
    managed_order.stages.journal_us    = (transport_started_ns - journal_started_ns) / 1000;

    try
    {
        if (!freed) // 예약매도 취소 뒤 재발주가 이미 접수됐으면 그 결과를 쓴다
        {
            ++kis_calls_;
            acknowledgement = kis_.submit_order_acknowledgement(signal);
        }

        rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - send_thread)
                     .count();
        bucket_wait_ms = static_cast<std::chrono::milliseconds::rep>(
            (kis_.rate_limit_wait_ns_this_thread() - bucket_wait_before_ns) / 1000000ULL);

        // 같은 대기를 us로도 남긴다 — ms로 자르면 버킷 대기가 0인지 0.9ms인지 구분이 안 된다.
        //  전송 시간은 버킷 줄서기를 뺀 몫이다. 뺀 값이 음수면(시계 해상도) 0으로 둔다.
        const int64_t bucket_wait_us = static_cast<int64_t>(
            (kis_.rate_limit_wait_ns_this_thread() - bucket_wait_before_ns) / 1000ULL);
        managed_order.stages.bucket_wait_us = bucket_wait_us;
        managed_order.stages.transport_us =
            std::max<int64_t>(0, (trace::now_ns() - transport_started_ns) / 1000 - bucket_wait_us);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 예외: ") + exception.what();
        ++rejected_count_;
        // 전송 예외는 접수 여부를 모른다. 선점을 풀고 REJECT를 적는다 — 실제로 접수됐다면 체결통보·잔고 대조가
        //  원장을 되맞춘다(선점을 붙잡아 두면 그 종목이 하루 종일 막힌다). [why D-113]
        ledger.on_reject(signal.account_id, signal.ticker, signal.side, signal.quantity, order_reference,
                        managed_order.reject_reason);
        LOG_ERROR("[OrderRouter] KIS 예외 [" + managed_order.order_id + "] " + signal.ticker + " — " + exception.what());
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        managed_order.stages.record_us = record(managed_order, &managed_order.stages.open_orders_us);
        // 이 거부 경로의 record_us는 record() 몫뿐이다 — 접수 확정·발행은 그 밖이라 따로 세지 않는다. [why D-126]
        managed_order.stages.history_store_us = managed_order.stages.record_us;
        return managed_order;
    }

    // 3. 전송 뒤 마무리 — 접수 확정(원장 ACCEPT 기록)·발행·이력 저장·파일 넘기기. 여기부터가 record_us다.
    //    왕복만 재고 끝내면 남은 시간이 어디로 갔는지 말할 수 없다 — 파일 쓰기가 여기서 드러나 쓰기 스레드로 옮겼다(D-123·D-124). [why D-117]
    const int64_t record_started_ns = trace::now_ns();

    managed_order.updated_at = std::chrono::system_clock::now();

    // 청산차단 자가정리 — SELL이 "주문가능분 없음"(40240000)으로 막히면, 그 종목의
    //  미체결 예약매도(이전 세션/수동 예약이 보유수량을 묶은 것)를 조회·취소하고 시장가로 1회
    //  재시도한다. 성공하면 아래 접수 블록이 그대로 처리(kis_order_no/ack가 재시도 결과로 갱신됨).
    if (!acknowledgement.ok() && signal.side == OrderSide::SELL && acknowledgement.error_code == kis_error::kNoSellableQty)
    {
        const auto sellable_view = ledger.sellable_view(signal.account_id, signal.ticker);

        if (sellable_view.held <= 0)
        {
            // 게이트는 원장이 모르는 종목을 자르지 않고 KIS에 넘긴다 — 그 거부가 여기로 온다.
            LOG_WARN("[OrderRouter] 보유수량 0(원장 기준) " + signal.ticker + " — 매도 불가, KIS도 주문가능분 없음으로 거부");
        }

        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal, order_reference, intent_taken);

        if (reconcile_acknowledgement.ok())
        {
            acknowledgement = std::move(reconcile_acknowledgement);
        }
    }

    if (!acknowledgement.kis_order_no.empty())
    {
        managed_order.status      = OrderStatus::ACCEPTED;
        managed_order.kis_order_no = std::move(acknowledgement.kis_order_no);
        managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no); // 전문 문자열이 정수가 되는 자리
        managed_order.krx_forwarding_org_no    = std::move(acknowledgement.krx_forwarding_org_no); // 정정/취소 시 원주문 조직번호로 재입력
        ++accepted_count_;
        // 선점은 전송 직전 INTENT에서 이미 잡혔다. 여기서는 원장에 ACCEPT(주문번호 확보)만 적는다 — 재기동
        //  리플레이가 "보냈고 접수됐다"를 "보냈는데 응답을 못 봤다"와 구분한다. [why D-113]
        order_reference.kis_order_number = managed_order.kis_order_number;
        ledger.on_accepted(signal.account_id, signal.ticker, signal.side, signal.quantity, order_reference);

        LOG_INFO(std::format("[OrderRouter] 접수 [{}] ODNO={} {} {} {}주 RTT={}ms 버킷대기={}ms", managed_order.order_id, managed_order.kis_order_no,
                             signal.ticker, signal.side == OrderSide::BUY ? "BUY" : "SELL", signal.quantity, rtt_ms, bucket_wait_ms));

        const int64_t publish_started_ns = trace::now_ns();
        managed_order.stages.accept_us   = (publish_started_ns - record_started_ns) / 1000;
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, true);
        }
#endif
        managed_order.stages.publish_us = (trace::now_ns() - publish_started_ns) / 1000;
    }
    else
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS API 거부 (빈 ODNO)" + kis_error_suffix(acknowledgement);
        ++rejected_count_;

        if (intent_taken)
        {
            ledger.on_reject(signal.account_id, signal.ticker, signal.side, signal.quantity, order_reference,
                            managed_order.reject_reason);
        }

        // 거부도 같은 왕복을 치르므로 함께 남긴다(09-14 KIS 호출 797건 중 거부 279건).
        LOG_ERROR(std::format("[OrderRouter] KIS 거부 [{}] {}{} RTT={}ms 버킷대기={}ms", managed_order.order_id, signal.ticker,
                              managed_order.reject_reason, rtt_ms, bucket_wait_ms));

        // 전송 타임아웃은 거부가 아니라 '모름'이다. 기동 때만 되묻던 것으로는 부족했다 —
        //  2026-09-23 12:51 001120 매도 32주가 접수돼(ODNO=0000022490) 보유 전량을 묶었는데
        //  다음 기동까지 아무도 몰랐다. 그 자리에서 브로커에 되물어 맞춘다. [why D-101]
        if (acknowledgement.error_code == kis_error::kTransport)
        {
            reconcile_unknown_order_async(signal.ticker);
        }

        const int64_t publish_started_ns = trace::now_ns();
        managed_order.stages.accept_us   = (publish_started_ns - record_started_ns) / 1000;
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        managed_order.stages.publish_us = (trace::now_ns() - publish_started_ns) / 1000;
    }

    // 이력 저장 몫 — 이력 잠금·미결주문 스냅숏·두 파일 넘기기. 접수 확정·발행과 갈라 둬야 다음에
    //  어디를 손댈지 고를 수 있다(회차 H에서 record 잔여가 1,096us였다). [why D-126]
    const int64_t history_store_started_ns = trace::now_ns();
    record(managed_order, &managed_order.stages.open_orders_us);
    managed_order.stages.history_store_us  = (trace::now_ns() - history_store_started_ns) / 1000;
    managed_order.stages.record_us         = (trace::now_ns() - record_started_ns) / 1000;
    return managed_order;
}

// ─── 전송 직전 원장 기록 ────────────────────────────────────────────────────
//  선점가는 지정가=price, 시장가(0)=reference_price로 근사 stamp → §3d 총노출이 시장가 선점을 과소평가하지
//  않게(check()의 평가가와 대칭, 보수측). 거짓이면 선점도 되돌려져 있다. [why D-113]
bool OrderRouter::take_intent(const OrderSignal& signal, const OrderGate::OrderRef& reference)
{
    if (gate_.ledger().on_intent(signal.account_id, signal.ticker, signal.side, signal.quantity,
                        signal.price > 0.0 ? signal.price : signal.reference_price, reference, signal.strategy_index))
    {
        return true;
    }

    LOG_ERROR("[OrderRouter] 원장 저널 기록 실패 — 주문을 보내지 않는다: " + signal.ticker + " " +
              std::to_string(signal.quantity) + "주");
    return false;
}

// ─── 청산차단 자가정리 — 예약매도 취소 후 시장가 재매도 (장중) ─────────────
//  전제: SELL이 40240000(주문가능분 없음)으로 막힌 직후 호출. 그 종목의 미체결 예약매도가
//  보유수량을 묶어 ord_psbl_qty=0이 된 상황을 KIS 미체결 조회로 규명하고, 예약을 취소해
//  수량을 풀어준 뒤 시장가 매도를 1회 재시도한다. 취소한 예약이 이번 세션 주문(history_에
//  ODNO가 있음)이면 CANCELLED로 닫고 원장(PositionLedger) 선점(reserved_)을 풀어 원장 행을 남긴다 — 그러지
//  않으면 선점이 스윕 때까지 남아 한도 계산을 조인다(C-2). 이전 세션·수동 예약은 history_에
//  없으므로 선점은 건드리지 않고 매도가능수량만 되돌린다(포지션 정합은 체결통보로).
OrderAck OrderRouter::reconcile_blocked_sell(const OrderSignal& signal, const OrderGate::OrderRef& reference, bool& intent_taken)
{
    auto& ledger = gate_.ledger();

    std::vector<OpenOrder> opens;

    // 모의투자는 정정취소가능조회(inquire-psbl-rvsecncl) TR을 미지원한다("없는 서비스 코드").
    //  대안으로 일별주문체결조회(VTTC8001R)도 붙여봤으나 기간을 어떻게 주든 output1이 0행이라
    //  미체결을 열거할 수 없었다(2026-09-07). 그래서 브로커 대신 라우터 이력을 정본으로 쓴다 —
    //  접수됐는데 아직 다 안 채워진 이 종목의 매도가 곧 수량을 묶고 있는 예약매도다.
    //  한계는 분명하다: 이번 세션이 낸 주문만 보인다. 이전 세션·수동 예약은 여전히 안 보이므로
    //  그때는 아래 "취소할 예약매도 없음"으로 떨어진다. 그래도 통째로 단락하는 것보다 낫다.
    //  지금은 get_open_orders가 모의에서도 VTTC0081R로 답하지만, 이 경로는 아직 이력을 쓴다.
    if (kis_.is_paper())
    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        for (const auto& managed_order : history_)
        {
            if (managed_order.status != OrderStatus::ACCEPTED || managed_order.signal.side != OrderSide::SELL ||
                managed_order.signal.symbol_id != signal.symbol_id || managed_order.kis_order_number == 0)
            {
                continue;
            }

            const int outstanding = managed_order.signal.quantity - managed_order.confirmed_quantity;

            if (outstanding <= 0)
            {
                continue;
            }

            OpenOrder open_order;
            open_order.ticker    = managed_order.signal.ticker;
            open_order.kis_order_no      = managed_order.kis_order_no;
            open_order.krx_forwarding_org_no = managed_order.krx_forwarding_org_no;
            open_order.psbl_qty  = outstanding;
            open_order.ord_unpr  = managed_order.signal.price;
            open_order.side      = OrderSide::SELL;
            opens.push_back(std::move(open_order));
        }

        // 이전 세션이 남긴 미체결(부속 파일)도 후보다 — 기동 스윕이 아직 못 지웠거나 한도 거부로
        //  남긴 줄이 이 종목의 수량을 묶고 있을 수 있다. 취소되면 아래에서 줄을 지운다.
        std::lock_guard<std::mutex> ck(carry_mutex_);

        for (const auto& carry_row : carry_rows_)
        {
            if (carry_row[2] != signal.ticker || carry_row[3] != "SELL")
            {
                continue;
            }

            OpenOrder open_order;
            open_order.ticker    = carry_row[2];
            open_order.kis_order_no      = carry_row[0];
            open_order.krx_forwarding_org_no = carry_row[1];

            try { open_order.psbl_qty = std::stoi(carry_row[4]); } catch (...) { continue; }
            open_order.side      = OrderSide::SELL;
            opens.push_back(std::move(open_order));
        }
    }
    else
    {
        try
        {
            opens = kis_.get_open_orders();
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 미체결 조회 예외 — " + std::string(exception.what()));
            return OrderAck::fail(kis_error::kTransport);
        }
    }

    int cancelled = 0;

    for (const auto& open : opens)
    {
        if (open.ticker != signal.ticker || open.side != OrderSide::SELL)
        {
            continue; // 해당 종목의 예약'매도'만 대상
        }

        LOG_WARN(std::format("[OrderRouter] 청산차단 해소 {} 예약매도 {}주 ODNO={} @{} → 취소 시도", signal.ticker,
                             open.psbl_qty, open.kis_order_no, static_cast<int>(open.ord_unpr)));
        OrderAck cancel;

        try
        {
            ++kis_calls_;
            cancel = kis_.cancel_order(open.ticker, open.kis_order_no, open.krx_forwarding_org_no, open.psbl_qty, /*all_remaining=*/true);
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 예약취소 예외 " + signal.ticker + " — " + std::string(exception.what()));
            continue;
        }

        if (!cancel.ok())
        {
            continue;
        }

        ++cancelled;

        // 이번 세션 주문이면 이력·선점을 같이 정리한다. 잠금 순서 history_mutex_ → 원장 positions_mutex_는 cancel_route와 같다.
        //  closed는 락 안에서 뜬 사본 — 락 밖의 원장 기록에 쓰고, history_ 원소는 축출로 참조가 죽을 수 있다.
        ManagedOrder closed;
        bool         found   = false;
        int          release = 0;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            ManagedOrder* managed_order = find_by_order_number_locked(digits_to_number(open.kis_order_no));

            if (managed_order && managed_order->status == OrderStatus::ACCEPTED)
            {
                release = managed_order->signal.quantity - managed_order->confirmed_quantity;

                if (release < 0)
                {
                    release = 0;
                }

                managed_order->status        = OrderStatus::CANCELLED;
                managed_order->reject_reason = "청산차단 해소 취소";
                managed_order->updated_at    = std::chrono::system_clock::now();
                closed                       = *managed_order;
                found                        = true;
            }

            if (release > 0)
            {
                ledger.on_cancel(closed.signal.account_id, closed.signal.ticker, OrderSide::SELL, release,
                                OrderGate::OrderRef{digits_to_number(closed.order_id), digits_to_number(open.kis_order_no),
                                                    closed.signal.type});
            }
        }

        if (found)
        {
            // 선점(reserved_)만 풀면 잔고 시드값 sellable_(취소 전 스냅샷, 주문가능 0)이 그대로라 다음 매도도 0으로
            //  깎인다 — 09-14 15:00 096770 은 익절 취소 뒤 재매도가 유량한도에 막히자 재시도 3회가 전부 "매도가능 0".
            //  취소로 브로커에서 풀린 수량만큼 되돌린다(이전 세션 줄과 같은 처리).
            ledger.restore_sellable(closed.signal.account_id, closed.signal.ticker, release);
            write_trade_row("", closed, 0, 0.0);
        }
        else
        {
            // 이전 세션 줄 — 부속 파일에서 빼고, 취소로 풀린 수량을 원장 매도가능수량에 되돌린다(기동 취소와 같은 처리).
            bool carried = false;
            {
                std::lock_guard<std::mutex> ck(carry_mutex_);
                const auto before = carry_rows_.size();
                carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                                 [&open](const std::array<std::string, 5>& parts) { return parts[0] == open.kis_order_no; }),
                                  carry_rows_.end());
                carried = carry_rows_.size() != before;
            }

            if (carried)
            {
                rewrite_open_orders();
            }

            ledger.restore_sellable(signal.account_id, signal.ticker, open.psbl_qty);
        }
    }

    if (cancelled == 0)
    {
        LOG_WARN("[OrderRouter] 청산차단 미해소 " + signal.ticker +
                 " — 취소할 예약매도 없음/취소 실패 (수동 확인 필요)");
        return OrderAck::fail(kis_error::kNoSellableQty); // 원인은 그대로 — 호출부가 거부 사유로 남긴다
    }

    LOG_INFO(std::format("[OrderRouter] 예약매도 {}건 취소 완료 → {} 시장가 매도 재시도", cancelled, signal.ticker));

    if (!intent_taken)
    {
        if (!take_intent(signal, reference))
        {
            return OrderAck::fail(kis_error::kLedgerWriteFailed);
        }

        intent_taken = true;
    }

    try
    {
        ++kis_calls_;
        return kis_.submit_order_acknowledgement(signal);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR("[OrderRouter] 청산 재매도 예외 " + signal.ticker + " — " + std::string(exception.what()));
        return OrderAck::fail(kis_error::kTransport);
    }
}

namespace
{
// 주문번호 없이 남은 INTENT를 KIS 미체결 한 건과 짝짓는다. KIS 주문 요청에는 우리 주문 id를 실을 칸이 없어
//  (order-cash 요청 필드, MCP 공식 예제 2026-09-25 확인) 종목·방향·수량·가격으로 맞춘다. 잔량은 죽은 사이
//  일부 체결됐을 수 있어 INTENT 수량 이하면 받는다. 시장가는 KIS 단가가 주문가와 달라 가격을 보지 않는다.
//  후보가 여럿이면 번호가 가장 작은 것 — INTENT를 주문 순서로 도니 먼저 낸 주문끼리 짝이 된다.
//  못 찾으면 nullptr. [why D-113]
const OpenOrder* match_unnumbered_intent(const OrderGate::OpenIntent& intent, const std::vector<OpenOrder>& open_orders,
                                         const std::unordered_set<uint64_t>& claimed_numbers)
{
    const OpenOrder* best        = nullptr;
    uint64_t         best_number = 0;

    for (const auto& open : open_orders)
    {
        const uint64_t number = digits_to_number(open.kis_order_no);

        if (number == 0 || claimed_numbers.count(number) > 0 || open.ticker != intent.ticker || open.side != intent.side ||
            open.psbl_qty <= 0 || open.psbl_qty > intent.remaining)
        {
            continue;
        }

        if (intent.type == OrderType::LIMIT && std::abs(open.ord_unpr - intent.price) >= 0.5)
        {
            continue;
        }

        if (best == nullptr || number < best_number)
        {
            best        = &open;
            best_number = number;
        }
    }

    return best;
}
} // namespace

// ─── 재기동 미결 주문 대조 ─────────────────────────────────────────────────
//  원장 저널의 미결 INTENT를 KIS 미체결과 맞춰 되살리거나 선점을 푼다. 세 갈래는 헤더 선언 주석에 있다. [why D-113]
OrderRouter::AdoptResult OrderRouter::adopt_open_intents(const std::vector<OrderGate::OpenIntent>& intents)
{
    auto& ledger = gate_.ledger();

    AdoptResult result;

    if (intents.empty())
    {
        return result;
    }

    // 주문번호 없는 INTENT = 전송 뒤 접수 응답 전에 죽은 주문일 수 있다. 모의는 그런 주문이 있을 때만 묻는다 —
    //  모의 미체결조회(VTTC0081R)로 ACCEPT를 본 주문까지 가리면 종전 판단이 바뀌므로 그 몫은 그대로 둔다.
    const bool has_unnumbered = std::any_of(intents.begin(), intents.end(),
                                            [](const OrderGate::OpenIntent& intent) { return intent.kis_order_number == 0; });
    std::vector<OpenOrder> open_orders;
    bool                   asked_broker = false;

    if (!kis_.is_paper() || has_unnumbered)
    {
        try
        {
            open_orders  = kis_.get_open_orders();
            asked_broker = true;
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 재기동 미체결 조회 예외 — " + std::string(exception.what()));
        }
    }

    // 주문번호 → 미체결 한 건. 번호를 아는 INTENT가 먼저 제 몫을 차지해, 번호 없는 INTENT가 남의 주문과 짝지어지지 않게 한다.
    std::unordered_map<uint64_t, const OpenOrder*> open_by_number;
    std::unordered_set<uint64_t>                   claimed_numbers;

    for (const auto& open : open_orders)
    {
        open_by_number.emplace(digits_to_number(open.kis_order_no), &open);
    }

    for (const auto& intent : intents)
    {
        if (intent.kis_order_number != 0)
        {
            claimed_numbers.insert(intent.kis_order_number);
        }
    }

    for (const auto& intent : intents)
    {
        uint64_t         kis_order_number = intent.kis_order_number;
        const OpenOrder* open             = nullptr;
        bool             live             = false;

        if (kis_order_number == 0)
        {
            open = asked_broker ? match_unnumbered_intent(intent, open_orders, claimed_numbers) : nullptr;
            live = open != nullptr;

            if (live)
            {
                kis_order_number = digits_to_number(open->kis_order_no);
                claimed_numbers.insert(kis_order_number);
                // 다음 재기동이 같은 짝짓기를 되풀이하지 않게 ACCEPT를 적어 번호를 남긴다.
                ledger.on_accepted(intent.account, intent.ticker, intent.side, intent.remaining,
                                   OrderGate::OrderRef{intent.order_id, kis_order_number, intent.type});
                LOG_WARN(std::format("[OrderRouter] 재기동 미결 주문 짝 [{}] 접수 응답 전에 끊긴 주문 — KIS 미체결 ODNO={} {} {} {}주로 되살림",
                                     intent.order_id, kis_order_number, intent.ticker,
                                     intent.side == OrderSide::BUY ? "BUY" : "SELL", open->psbl_qty));
            }
        }
        else if (asked_broker && !kis_.is_paper())
        {
            const auto found = open_by_number.find(kis_order_number);
            open             = found != open_by_number.end() ? found->second : nullptr;
            live             = open != nullptr;
        }
        else
        {
            // 브로커에 못 물어본 경우(모의·조회 예외)는 ACCEPT를 본 주문을 살아 있는 것으로 본다 — 접수된 주문을
            //  지레 풀어 같은 수량을 또 내는 쪽이 더 큰 사고다.
            live = intent.accepted;
        }

        if (!live)
        {
            LOG_WARN(std::format("[OrderRouter] 재기동 미결 주문 선점 해제 [{}] ODNO={} {} {} {}주 — {}",
                                 intent.order_id, intent.kis_order_number, intent.ticker,
                                 intent.side == OrderSide::BUY ? "BUY" : "SELL", intent.remaining,
                                 asked_broker ? "KIS 미체결에 없다" : "미체결 조회를 못 했고 접수 기록도 없다"));
            ledger.on_cancel(intent.account, intent.ticker, intent.side, intent.remaining,
                            OrderGate::OrderRef{intent.order_id, intent.kis_order_number, intent.type});
            ++result.released;
            continue;
        }

        ManagedOrder managed_order;
        managed_order.order_id               = std::format("ORD-{:06}", intent.order_id);
        managed_order.kis_order_no           = std::format("{:010}", kis_order_number);
        managed_order.kis_order_number       = kis_order_number;
        managed_order.status                 = OrderStatus::ACCEPTED;
        managed_order.signal.ticker          = intent.ticker;
        managed_order.signal.symbol_id       = ledger.intern_symbol(intent.ticker);
        managed_order.signal.account_id      = intent.account;
        managed_order.signal.side            = intent.side;
        managed_order.signal.type            = intent.type;
        managed_order.signal.quantity        = intent.remaining;
        managed_order.signal.price           = intent.price;
        managed_order.signal.strategy_id     = intent.strategy_name.empty() ? std::string("UNLINKED") : intent.strategy_name;
        managed_order.signal.strategy_index  = ledger.strategy_index_of(managed_order.signal.strategy_id);
        managed_order.signal.reason          = "재기동 복원(원장 저널 미결 주문)";
        managed_order.submitted_at           = std::chrono::system_clock::now();
        managed_order.updated_at             = managed_order.submitted_at;

        // 정정·취소에 필요한 원주문 조직번호는 저널에 없다 — 미체결조회에서 찾았으면 그 값을, 아니면 빈 값으로 두고
        //  라우터가 취소를 미체결조회 결과로 낸다.
        if (open != nullptr)
        {
            managed_order.krx_forwarding_org_no = open->krx_forwarding_org_no;
        }

        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            push_history_locked(managed_order);
        }

        ++result.restored;

        // 되살린 번호 위에서 이어 센다 — 같은 ORD-NNNNNN이 두 번 생기면 체결통보가 엉뚱한 주문에 붙는다.
        uint64_t seen = sequence_.load(std::memory_order_relaxed);

        while (seen < intent.order_id && !sequence_.compare_exchange_weak(seen, intent.order_id))
        {
        }
    }

    LOG_INFO(std::format("[OrderRouter] 재기동 미결 주문 대조: 되살림 {}건 · 선점 해제 {}건 (저널 {}건)", result.restored,
                         result.released, intents.size()));
    return result;
}

// ─── 유령 선점 정리 ───────────────────────────────────────────────────────
//  원장(PositionLedger)의 선점(reserved_)은 전송 직전 INTENT 때 생기고 체결·취소·거부로만 풀린다. 통보를 한 번
//  놓치면 그 선점이 슬롯을 물고 남아, 실제 보유가 한도에 못 미치는데 신규 진입이 막힌다
//  (09-09: 보유 20인데 "25 >= 25" 거부). 살아 있는 주문이 없는 종목의 선점을 푼다.
int OrderRouter::sweep_stale_reservations()
{
    auto& ledger = gate_.ledger();

    std::vector<bool> live(ledger.symbols().capacity(), false);
    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        // [inv] 이력이 비면 아무 것도 풀지 않는다. 이력만 비는 경우(재기동 직후, 상한 초과로 잘려 나간 뒤)에
        //  정본으로 믿으면 살아 있는 선점을 통째로 푼다.
        if (history_.empty())
        {
            return 0;
        }

        for (const auto& managed_order : history_)
        {
            if (managed_order.status == OrderStatus::ACCEPTED && managed_order.confirmed_quantity < managed_order.signal.quantity &&
                managed_order.signal.symbol_id < live.size())
            {
                live[managed_order.signal.symbol_id] = true;
            }
        }
    }

    const auto gone = ledger.prune_reservations(live);

    if (!gone.empty())
    {
        std::string list;

        for (const auto& gone_ticker : gone)
        {
            if (!list.empty())
            {
                list += ',';
            }

            list += gone_ticker;
        }

        LOG_WARN(std::format("[OrderRouter] 살아있는 주문 없는 선점 {}종목 해제 ({})", gone.size(), list));
    }

    return static_cast<int>(gone.size());
}

// ─── 이력 저장 (max_history 초과 시 체결 완료/거부된 것만 삭제) ───────────
//  쓴 시간(us)을 돌려준다 — 전송 전에 끝난 주문은 이 몫이 곧 record_us다. 값을 안 쓰는 호출자는 그냥 버린다.
//  open_orders_us를 주면 미결주문 파일 몫을 거기 따로 담는다. 지금은 대기함에 넘기는 시간이라 0에 가깝다 —
//  건당 전체 다시쓰기였을 때 이 값이 record_us의 절반(1,589us)이었고, 그래서 쓰기를 스레드로 뺐다. [why D-123]
int64_t OrderRouter::record(const ManagedOrder& managed_order, int64_t* open_orders_us)
{
    const int64_t started_ns = trace::now_ns();
    std::string open_orders;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        push_history_locked(managed_order);

        while (static_cast<int>(history_.size()) > config_.max_history)
        {
            // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
            if (history_.front().status == OrderStatus::ACCEPTED)
            {
                break;
            }

            pop_history_front_locked();
        }

        open_orders = snapshot_open_orders_locked();
        sequence         = ++open_orders_sequence_;
    }

    // 파일 I/O는 history_mutex_ 밖에서 — 디스크가 느린 순간 체결(on_fill)·발주(submit)가 같이 밀리지 않게(W-8).
    // 거래 원장 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → managed_order.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    write_trade_row("", managed_order, 0, 0.0);

    const int64_t open_orders_started_ns = trace::now_ns();
    queue_open_orders_file(std::move(open_orders), sequence);

    if (open_orders_us != nullptr)
    {
        *open_orders_us = (trace::now_ns() - open_orders_started_ns) / 1000;
    }

    append_order_reason(managed_order);
    return (trace::now_ns() - started_ns) / 1000;
}

// ─── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
//  형식: odno|ticker|side|수량|가격|기준가|전략|사유   (한 줄 한 주문, 헤더 없음)
//  접수된 신규·정정 주문만 남긴다. 취소는 체결되지 않으므로 대상이 아니다.
void OrderRouter::append_order_reason(const ManagedOrder& managed_order)
{
    if (managed_order.status != OrderStatus::ACCEPTED || managed_order.kis_order_no.empty() ||
        managed_order.signal.action == OrderAction::CANCEL || managed_order.signal.quantity <= 0)
    {
        return;
    }

    // 구분자와 줄바꿈은 공백으로 바꾼다 — 사유 문구에 무엇이 들어와도 한 줄을 유지한다.
    auto safe = [](std::string text)
    {
        for (char& character : text)
        {
            if (character == '|' || character == '\n' || character == '\r')
            {
                character = ' ';
            }
        }

        return text;
    };

    const std::string line = std::format("{}|{}|{}|{}|{}|{}|{}|{}\n", managed_order.kis_order_no, safe(managed_order.signal.ticker),
                                         managed_order.signal.side == OrderSide::BUY ? "BUY" : "SELL", managed_order.signal.quantity,
                                         static_cast<long long>(managed_order.signal.price),
                                         static_cast<long long>(managed_order.signal.reference_price), safe(managed_order.signal.strategy_id),
                                         safe(managed_order.signal.reason));

    PendingLine pending;
    pending.sink = PendingLine::Sink::REASON;
    pending.date = today_ymd();
    pending.text = line;   // 줄바꿈이 이미 붙어 있다
    queue_append_line(std::move(pending));
}

void OrderRouter::load_order_reasons_locked()
{
    if (order_reasons_loaded_)
    {
        return;
    }

    order_reasons_loaded_ = true;
    std::ifstream in(Logger::instance().path_for("order_reasons_" + today_ymd() + ".txt"));

    if (!in)
    {
        return;
    }

    std::string line;
    int count = 0;

    while (std::getline(in, line))
    {
        std::vector<std::string> fields;
        std::string token;
        std::istringstream stream(line);

        while (std::getline(stream, token, '|'))
        {
            fields.push_back(std::move(token));
        }

        if (fields.size() < 8 || fields[0].empty())
        {
            continue;
        }

        OrderReason order_reason;
        order_reason.ticker      = std::move(fields[1]);
        order_reason.side        = OrderSide::from_string(fields[2]);
        order_reason.strategy_id = std::move(fields[6]);
        order_reason.reason      = std::move(fields[7]);

        try
        {
            order_reason.quantity  = std::stoi(fields[3]);
            order_reason.price     = std::stod(fields[4]);
            order_reason.reference_price = std::stod(fields[5]);
        }
        catch (const std::exception&)
        {
            continue;   // 숫자가 깨진 줄은 버린다
        }

        if (order_reason.quantity <= 0)
        {
            continue;
        }

        const uint64_t order_number = digits_to_number(fields[0]);

        if (order_number == 0)
        {
            continue;
        }

        order_reasons_[order_number] = std::move(order_reason);   // 같은 ODNO가 여러 줄이면 마지막 것이 맞다
        ++count;
    }

    if (count > 0)
    {
        LOG_INFO("[OrderRouter] 주문 사유 기록 " + std::to_string(count) + "건 복원");
    }
}


// ─── 미체결 주문 부속 파일 ─────────────────────────────────────────────────
//  형식: odno|orgno|ticker|side|remaining  (한 줄 한 주문, 헤더 없음)
//  history_는 프로세스 메모리라 재기동으로 사라진다. 그래서 살아있는 주문을 파일에
//  남겨 두고 다음 기동이 그것을 취소한다 — 기동 취소는 이 파일을 먼저 읽고, 빠진 주문은
//  브로커 미체결 조회로 채운다(모의투자도 VTTC0081R로 답한다). 매 상태변화마다 통째로 덮어쓰되, 쓰기는 전담 스레드가 한다.
//  주문 스레드에서 바로 쓰던 때는 건당 1,589us로 record_us의 절반을 먹었다 — 살아있는 주문이 수십 건이라
//  비용이 무시할 만하다고 본 것은 라이브 기준이었고, 951줄이 쌓이면 그렇지 않았다(2026-09-23 회차 E). [why D-123]
std::string OrderRouter::snapshot_open_orders_locked() const
{
    std::string buffer;

    for (const auto& history_entry : history_)
    {
        if (history_entry.status != OrderStatus::ACCEPTED)
        {
            continue;
        }

        const int remain = history_entry.signal.quantity - history_entry.confirmed_quantity;

        if (remain <= 0 || history_entry.kis_order_no.empty())
        {
            continue;
        }

        std::format_to(std::back_inserter(buffer), "{}|{}|{}|{}|{}\n", history_entry.kis_order_no, history_entry.krx_forwarding_org_no, history_entry.signal.ticker,
                       history_entry.signal.side == OrderSide::BUY ? "BUY" : "SELL", remain);
    }

    // 이전 세션 줄은 아직 취소가 안 끝난 것만 남아 있다 — 이번 세션 줄과 합쳐 쓴다.
    {
        std::lock_guard<std::mutex> lock(carry_mutex_);

        for (const auto& carry_row : carry_rows_)
        {
            std::format_to(std::back_inserter(buffer), "{}|{}|{}|{}|{}\n", carry_row[0], carry_row[1], carry_row[2], carry_row[3], carry_row[4]);
        }
    }

    return buffer;
}

void OrderRouter::rewrite_open_orders()
{
    std::string body;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        body = snapshot_open_orders_locked();
        sequence  = ++open_orders_sequence_;
    }

    queue_open_orders_file(std::move(body), sequence);
}

void OrderRouter::write_open_orders_file(const std::string& body, uint64_t sequence)
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (sequence <= open_orders_written_sequence_)
    {
        return; // 더 새 스냅샷이 먼저 쓰였다 — 옛 것으로 덮으면 살아있는 주문이 사라진다
    }

    open_orders_written_sequence_ = sequence;

    fs::path path = Logger::instance().path_for("open_orders.txt");
    fs::path temporary  = Logger::instance().path_for("open_orders.tmp");

    // 임시파일에 쓰고 원자적으로 갈아끼운다 — 기동 중 크래시로 반쪽 파일을 읽지 않도록.
    {
        std::ofstream out(temporary, std::ios::trunc);

        if (!out)
        {
            return;
        }

        out << body;
    }

    fs::rename(temporary, path, error_code);

    if (error_code)
    {
        fs::remove(temporary, error_code);
    }
}

// ─── 부속 파일 쓰기 넘기기 ────────────────────────────────────────────────
void OrderRouter::queue_open_orders_file(std::string body, uint64_t sequence)
{
    {
        std::lock_guard<std::mutex> lock(open_orders_outbox_mutex_);

        if (sequence <= open_orders_pending_sequence_)
        {
            return; // 더 새 스냅샷이 이미 줄 서 있다 — 옛 것으로 덮으면 살아있는 주문이 사라진다
        }

        open_orders_pending_body_     = std::move(body);
        open_orders_pending_sequence_ = sequence;
    }

    open_orders_outbox_signal_.notify_one();
}

void OrderRouter::flush_open_orders_file()
{
    std::string body;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(open_orders_outbox_mutex_);

        if (open_orders_pending_sequence_ == 0)
        {
            return;
        }

        body     = std::move(open_orders_pending_body_);
        sequence = open_orders_pending_sequence_;
        open_orders_pending_body_.clear();
        open_orders_pending_sequence_ = 0;
    }

    write_open_orders_file(body, sequence);
}

void OrderRouter::open_orders_writer_loop(std::stop_token stop_token)
{
    while (!stop_token.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(open_orders_outbox_mutex_);
            open_orders_outbox_signal_.wait(lock, stop_token,
                                            [this] { return open_orders_pending_sequence_ != 0; });
        }

        flush_open_orders_file();
    }

    flush_open_orders_file(); // 멈추라는 말을 듣고도 마지막 스냅샷은 디스크에 남긴다
}

// ─── 원장 CSV·사유 덧붙이기 넘기기 ────────────────────────────────────────
void OrderRouter::queue_append_line(PendingLine line)
{
    bool backed_up = false;
    {
        std::lock_guard<std::mutex> lock(append_outbox_mutex_);
        append_outbox_.push_back(std::move(line));
        backed_up = append_outbox_.size() >= kAppendOutboxLimit;
    }

    append_outbox_signal_.notify_one();

    if (backed_up)
    {
        // 디스크가 못 따라간다. 여기서 기다리는 것은 고치기 전과 같은 상태지만, 큐가 메모리를
        //  끝없이 먹는 것보다 낫다. 순서는 flush_append_outbox가 io_mutex_ 안에서 지킨다.
        flush_append_outbox();
    }
}

void OrderRouter::flush_append_outbox()
{
    // io_mutex_를 먼저 잡고 그 안에서 꺼낸다 — 쓰기 스레드와 부른 쪽이 같이 비우더라도
    //  꺼낸 순서와 쓴 순서가 어긋나지 않는다. [lock-order] io_mutex_ → append_outbox_mutex_
    std::lock_guard<std::mutex> io_lock(io_mutex_);

    while (true)
    {
        std::deque<PendingLine> batch;
        {
            std::lock_guard<std::mutex> lock(append_outbox_mutex_);

            if (append_outbox_.empty())
            {
                return;
            }

            batch.swap(append_outbox_);
        }

        write_pending_lines_locked(batch);   // 쓰는 동안 들어온 줄은 다음 바퀴가 가져간다
    }
}

void OrderRouter::write_pending_lines_locked(const std::deque<PendingLine>& batch)
{
    bool wrote_trade  = false;
    bool wrote_reason = false;

    for (const PendingLine& pending : batch)
    {
        if (pending.sink == PendingLine::Sink::TRADE)
        {
            if (pending.date != trade_file_date_ || !trade_file_.is_open())
            {
                open_trade_file_locked(pending.date);
            }

            if (!trade_file_.is_open())
            {
                continue; // best-effort — 원장 정본은 OrderGate 저널이다(D-113)
            }

            trade_file_ << pending.text << '\n';
            wrote_trade = true;
        }
        else
        {
            if (pending.date != order_reason_file_date_ || !order_reason_file_.is_open())
            {
                open_order_reason_file_locked(pending.date);
            }

            if (!order_reason_file_)
            {
                continue;
            }

            order_reason_file_ << pending.text;
            wrote_reason = true;
        }
    }

    // flush는 묶음당 한 번이다 — 줄마다 하던 것을 줄여 쓰기 스레드가 큐에 밀리지 않게 한다.
    if (wrote_trade)
    {
        trade_file_.flush();
    }

    if (wrote_reason)
    {
        order_reason_file_.flush();
    }
}

void OrderRouter::open_order_reason_file_locked(const std::string& date)
{
    order_reason_file_.close();
    order_reason_file_.clear();
    order_reason_file_.open(Logger::instance().path_for("order_reasons_" + date + ".txt"), std::ios::app);
    order_reason_file_date_ = date;
}

void OrderRouter::append_writer_loop(std::stop_token stop_token)
{
    while (!stop_token.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(append_outbox_mutex_);
            append_outbox_signal_.wait(lock, stop_token, [this] { return !append_outbox_.empty(); });
        }

        flush_append_outbox();
    }

    flush_append_outbox(); // 멈추라는 말을 듣고도 줄 서 있던 것은 디스크에 남긴다
}

void OrderRouter::flush_file_writes()
{
    flush_append_outbox();
}

// ─── 소멸 — 스레드 회수 ───────────────────────────────────────────────────
OrderRouter::~OrderRouter()
{
    // jthread 소멸자가 같은 일을 하지만 그건 멤버 소멸 순서 안에서다 — 스레드가 쓰는 멤버가 먼저 죽지 않게 여기서 회수한다.
    stale_threshold_.request_stop();

    if (stale_threshold_.joinable())
    {
        stale_threshold_.join();
    }

    open_orders_writer_.request_stop();

    if (open_orders_writer_.joinable())
    {
        open_orders_writer_.join();
    }

    flush_open_orders_file(); // 스레드가 멈춘 뒤 남은 것이 있으면 여기서 쓴다

    append_writer_.request_stop();

    if (append_writer_.joinable())
    {
        append_writer_.join();
    }

    flush_append_outbox(); // 줄 서 있던 원장 행·사유 줄을 마저 쓴다
}

// ─── 전송 타임아웃 뒤 되묻기 ────────────────────────────────────────────────
//  응답을 못 받은 주문이 KIS에 접수돼 있으면 엔진 장부 밖에서 보유분을 묶는다. 부속 파일이
//  아는 번호와 견주어, 우리 것이 아닌 미체결만 지운다. 주문 스레드를 막지 않으려고 따로 돈다.
void OrderRouter::reconcile_unknown_order_async(std::string ticker)
{
    if (reconcile_busy_.exchange(true))
    {
        return;   // 앞 건이 돌고 있다 — 다음 타임아웃이나 다음 기동이 다시 잡는다
    }

    transport_reconcile_ = std::jthread([this, ticker = std::move(ticker)](std::stop_token stop_token)
    {
        // KIS가 접수를 조회에 반영할 틈을 준다. 곧바로 물으면 방금 낸 주문이 안 보인다.
        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (stop_token.stop_requested())
        {
            reconcile_busy_ = false;
            return;
        }

        try
        {
            std::vector<std::string> known;
            {
                std::ifstream in(Logger::instance().path_for("open_orders.txt"));
                std::string line;

                while (std::getline(in, line))
                {
                    const size_t bar = line.find('|');

                    if (bar != std::string::npos)
                    {
                        known.push_back(line.substr(0, bar));
                    }
                }
            }

            ++kis_calls_;

            for (const auto& open : kis_.get_open_orders())
            {
                if (open.ticker != ticker || open.kis_order_no.empty() || open.psbl_qty <= 0)
                {
                    continue;
                }

                if (std::find(known.begin(), known.end(), open.kis_order_no) != known.end())
                {
                    continue;   // 우리가 아는 주문이다
                }

                LOG_WARN("[OrderRouter] 전송 타임아웃 뒤 장부 밖 주문 발견 — 취소 " + open.ticker +
                         " ODNO=" + open.kis_order_no + " " + std::to_string(open.psbl_qty) + "주");
                ++kis_calls_;
                const OrderAck cancelled = kis_.cancel_order(open.ticker, open.kis_order_no,
                                                             open.krx_forwarding_org_no, open.psbl_qty,
                                                             /*all_remaining=*/true);

                if (cancelled.ok())
                {
                    if (open.side == OrderSide::SELL)
                    {
                        gate_.ledger().restore_sellable(std::string(), open.ticker, open.psbl_qty);
                    }
                }
                else
                {
                    LOG_WARN("[OrderRouter] 장부 밖 주문 취소 실패 — 다음 기동이 다시 지운다 " + open.ticker +
                             " ODNO=" + open.kis_order_no);
                }
            }
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 전송 타임아웃 되묻기 실패 — " + std::string(exception.what()));
        }

        reconcile_busy_ = false;
    });
}

// ─── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ──────────────────────
void OrderRouter::cancel_stale_orders_async()
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    fs::path path = Logger::instance().path_for("open_orders.txt");

    if (!fs::exists(path, error_code))
    {
        return;
    }

    std::vector<std::array<std::string, 5>> rows;
    {
        std::ifstream in(path);
        std::string line;

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty())
            {
                continue;
            }

            std::array<std::string, 5> fields;
            size_t position = 0;
            size_t index = 0;
            bool parsed = true;

            while (index < 5)
            {
                const size_t separator = line.find('|', position);

                if (index < 4 && separator == std::string::npos)
                {
                    parsed = false;
                    break;
                }

                // 마지막 칸은 구분자가 없어도 줄 끝까지 가져온다. 칸 번호를 늘리는 것과 읽는 것을
                //  한 식에 두면 어느 값으로 판단할지 정해지지 않아 마지막 칸이 잘릴 수도,
                //  끝까지 갈 수도 있었다(-Wsequence-point). 둘을 갈랐다.
                const size_t length = (index < 4 && separator != std::string::npos)
                                        ? separator - position : std::string::npos;
                fields[index] = line.substr(position, length);
                ++index;

                if (separator == std::string::npos)
                {
                    break;
                }

                position = separator + 1;
            }

            if (parsed && index == 5 && !fields[0].empty())
            {
                rows.push_back(std::move(fields));
            }
        }
    }

    // 부속 파일은 우리가 ODNO를 받은 주문만 안다. 전송이 타임아웃 나면 KIS에는 접수됐는데 우리는 ODNO를
    //  못 받아 파일에 못 적는다 — 그렇게 남은 주문은 아무도 취소해 주지 않는다(2026-09-23 09:26 021240,
    //  ODNO=0000007886 매도 18주가 살아남아 보유분이 묶이고 손절 불능이 됐다). 브로커에 직접 물어 빠진
    //  주문을 채운다. [why D-101]
    try
    {
        for (const auto& open : kis_.get_open_orders())
        {
            if (open.kis_order_no.empty() || open.psbl_qty <= 0)
            {
                continue;
            }

            const bool known = std::any_of(rows.begin(), rows.end(),
                                           [&open](const std::array<std::string, 5>& parts)
                                           { return parts[0] == open.kis_order_no; });

            if (known)
            {
                continue;
            }

            LOG_WARN("[OrderRouter] 부속 파일에 없는 미체결 — 브로커 조회로 보충 " + open.ticker + " ODNO=" +
                     open.kis_order_no + " " + std::to_string(open.psbl_qty) + "주");
            rows.push_back({open.kis_order_no, open.krx_forwarding_org_no, open.ticker,
                            open.side == OrderSide::SELL ? "SELL" : "BUY", std::to_string(open.psbl_qty)});
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[OrderRouter] 미체결 보충 조회 실패 — " + std::string(exception.what()));
    }

    if (rows.empty())
    {
        return;
    }

    // 파일은 비우지 않는다. 읽은 줄을 carry_rows_에 들고 있으면 이번 세션의 스냅샷마다 같이
    //  실리므로, 취소를 마치기 전에 죽거나 한도 거부로 남긴 주문도 다음 재기동에 그대로 넘어간다.
    {
        std::lock_guard<std::mutex> lock(carry_mutex_);
        carry_rows_.clear();

        for (const auto& row : rows)
        {
            int quantity = 0;

            try { quantity = std::stoi(row[4]); } catch (...) {}

            if (quantity > 0)
            {
                carry_rows_.push_back(row);   // 수량이 없는 줄은 취소할 것도 없다 — 넘기지 않는다
            }
        }
    }

    LOG_WARN("[OrderRouter] 이전 세션 미체결 " + std::to_string(rows.size()) +
             "건 발견 — 백그라운드 취소 시작(유령 주문이 현금을 묶고 청산 직후 재진입을 만든다)");

    if (stale_threshold_.joinable())
    {
        stale_threshold_.join();
    }

    // 취소는 건당 왕복 3~5초다. 기동 경로에서 돌리면 장중 재기동이 5분씩 멈춘다.
    //  잔고 시드는 이 스레드를 기다리지 않아도 된다 — 미체결 취소는 보유수량을 바꾸지
    //  않고 주문가능현금·매도가능수량만 푸는데, 둘 다 주기 잔고 대조가 다시 읽는다.
    //  rows는 스레드가 이 함수보다 오래 살아 옮겨 넣는다(참조로 잡으면 반환 뒤 사라진다).
    stale_threshold_ = std::jthread([this, rows = std::move(rows)](std::stop_token stop_token)
    {
        int cancelled = 0;

        for (const auto& row : rows)
        {
            if (stop_token.stop_requested())
            {
                LOG_WARN("[OrderRouter] 유령주문 취소 중단(종료 요청) — 남은 " +
                         std::to_string(rows.size() - static_cast<size_t>(cancelled)) + "건");
                return;
            }

            int quantity = 0;

            try { quantity = std::stoi(row[4]); } catch (...) { continue; }

            if (quantity <= 0)
            {
                continue;
            }

            OrderAck result;
            bool     rate_limited = false;
            bool     transport_unknown = false;

            // 한도 거부(EGW00201)는 "이미 종료"가 아니다. 같은 분기로 흘리면 유령 예약이 KIS에
            //  남은 채 전략이 같은 종목을 새로 깔아 체결 시 이중 포지션이 된다(09-11 09:17~09:18
            //  180640·005935·007660 6건). 한도는 1초 창이라 잠깐 쉬고 다시 보낸다.
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                try
                {
                    ++kis_calls_;
                    result = kis_.cancel_order(row[2], row[0], row[1], quantity, /*all_remaining=*/true);
                }
                catch (const std::exception& exception)
                {
                    LOG_WARN("[OrderRouter] 유령주문 취소 예외 " + row[2] + " ODNO=" + row[0] + " — " + exception.what());
                    break;
                }

                rate_limited = !result.ok() && result.error_code == kis_error::kRateLimit;
                // 전송 실패는 '취소됨'이 아니라 '모름'이다 — 응답만 못 받았을 뿐 취소가 안 갔을 수 있다.
                //  아래에서 한도 거부와 같이 잔존으로 다룬다.
                transport_unknown = !result.ok() && result.error_code == kis_error::kTransport;

                if ((!rate_limited && !transport_unknown) || stop_token.stop_requested())
                {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            }

            if (rate_limited || transport_unknown)
            {
                // 줄은 carry_rows_에 남긴다 — 다음 재기동이 다시 시도한다.
                //  전송 실패를 종료로 단정해 지우면 그 주문은 아무도 다시 지우지 않는다. 2026-09-23 09:43
                //  기동 취소에서 316140(ODNO=0000009703)·012750(ODNO=0000009712) 두 건이 그렇게 사라졌고,
                //  보유 37주·16주가 주문가능 0으로 묶여 손절이 닿아도 못 파는 상태가 됐다. [why D-101]
                LOG_WARN(std::string("[OrderRouter] 유령주문 취소 실패(") +
                         (rate_limited ? "한도 거부 반복" : "전송 실패 — 취소됐는지 모름") +
                         ") — KIS에 잔존 " + row[2] + " ODNO=" + row[0] + " " +
                         std::to_string(quantity) + "주 (부속 파일에 유지)");
                continue;
            }

            if (result.ok())
            {
                ++cancelled;
                LOG_INFO("[OrderRouter] 유령주문 취소 " + row[2] + " " + row[3] + " " +
                         std::to_string(quantity) + "주 ODNO=" + row[0]);

                // 취소로 브로커에서는 수량이 풀렸지만 원장(PositionLedger)의 sellable_은 잔고 시드값
                //  (ord_psbl_qty, 취소 전 스냅샷) 그대로다. 되돌리지 않으면 미체결이 없는데도
                //  자기 청산이 막힌다 — 09-09 000215은 13:45 취소 뒤 16분간 "매도가능수량 0"으로
                //  교체 진입이 네 번 무산됐다. 매도 취소만 해당한다(매수는 현금을 풀 뿐이다).
                if (row[3] == "SELL")
                {
                    gate_.ledger().restore_sellable(std::string(), row[2], quantity);
                }
            }
            else
            {
                // 이미 체결·취소됐으면 KIS가 거부한다 — 정상이다.
                LOG_INFO("[OrderRouter] 유령주문 취소 불가(이미 종료 추정) " + row[2] + " ODNO=" + row[0]);
            }

            // 취소 접수든 이미 종료든 이 줄은 끝났다 — 부속 파일에서 뺀다.
            {
                std::lock_guard<std::mutex> lock(carry_mutex_);
                carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                                 [&row](const std::array<std::string, 5>& parts) { return parts[0] == row[0]; }),
                                  carry_rows_.end());
            }

            rewrite_open_orders();

            // 초당 거래건수 상한(EGW00201)에 걸리지 않게 간격을 둔다. 정지 요청이 오면 바로 깬다.
            wake::sleep_unless_stopped(stop_token, std::chrono::milliseconds(kStaleCancelGapMs));
        }

        LOG_INFO("[OrderRouter] 이전 세션 미체결 정리 완료: " + std::to_string(cancelled) + "건 취소 접수");
    });
}

// ─── 거래 원장 CSV 적재 ───────────────────────────────────────────────────
//  실행 로그(quant_trader.log)와 별개로 매수·매도·거부·체결·잔고 대조를 구조적으로 남긴다.
//  logs/trades_YYYYMMDD.csv 에 한 줄씩 append(날짜별 파일). record()·on_fill()·record_reconcile()
//  이 줄을 만들어 줄 대기열에 넣고, 쓰기 스레드 하나가 파일에 쓴다(동시쓰기 없음). [why D-124]
//  원장 쓰기 실패는 매매를 막지 않는다(best-effort — 조용히 반환).
//  열 정본은 kTradeHeader 하나다. 열을 더할 때는 끝에 붙인다 — 스키마 승격이 옛 파일 행 끝에 빈 칸을
//  덧붙이는 방식이라 중간 삽입은 기존 행의 값을 엉뚱한 열로 밀어낸다. Python 판독기는 열 이름으로 읽는다.
static const std::string kTradeHeader =
    "ts_kst,event,order_id,odno,strategy,ticker,side,type,"
    "order_qty,order_price,fill_qty,fill_price,status,reason,entry_reason,realized_pnl,seq,"
    "strategy_realized_pnl";

// CSV 깨짐 방지: 콤마/개행 공백 치환
static std::string csv_safe(std::string text)
{
    for (char& character : text)
    {
        if (character == ',' || character == '\n' || character == '\r')
        {
            character = ' ';
        }
    }

    return text;
}

void OrderRouter::trade_row_timestamp(std::string& date, std::string& stamp)
{
    const std::time_t now_time = std::time(nullptr);
    date  = kst::date_yyyymmdd(now_time);
    stamp = kst::datetime(now_time);
}

void OrderRouter::open_trade_file_locked(const std::string& date)
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    // 실행 위치(cwd)와 무관하게 로그 폴더(main에서 고정)에 매매원장 append.
    fs::path path = Logger::instance().path_for(std::string("trades_") + date + ".csv");

    const bool need_header = !fs::exists(path, error_code);

    // 스키마 승격 — 같은 날 파일이 옛 헤더(열이 적음)면 새 열을 붙여 한 번 재작성한다.
    //  한 파일에 15열 헤더와 16열 데이터가 섞이면 판독기가 값을 어긋난 키로 읽는다.
    //  날짜가 바뀌어 파일을 새로 여는 순간에만 확인한다 — 이미 이번 세션에서 연 파일의
    //  헤더는 우리 자신이 썼으므로 매 줄마다 다시 볼 필요가 없다. [why D-094]
    if (!need_header)
    {
        std::ifstream in(path);
        std::string first;

        if (in && std::getline(in, first))
        {
            if (!first.empty() && first.back() == '\r')
            {
                first.pop_back();
            }

            if (first != kTradeHeader)
            {
                long add = static_cast<long>(std::count(kTradeHeader.begin(), kTradeHeader.end(), ',')) -
                           static_cast<long>(std::count(first.begin(), first.end(), ','));

                if (add < 0)
                {
                    add = 0;
                }

                std::vector<std::string> rows;

                for (std::string line; std::getline(in, line); )
                {
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    if (!line.empty())
                    {
                        line.append(static_cast<size_t>(add), ',');
                        rows.push_back(std::move(line));
                    }
                }

                in.close();
                std::ofstream out(path, std::ios::trunc);

                if (out)
                {
                    out << kTradeHeader << '\n';

                    for (const auto& row : rows)
                    {
                        out << row << '\n';
                    }
                }
            }
        }
    }

    trade_file_.close();
    trade_file_.clear();
    trade_file_.open(path, std::ios::app);

    if (trade_file_.is_open() && need_header)
    {
        trade_file_ << kTradeHeader << '\n';
        trade_file_.flush();
    }

    trade_file_date_ = date;
}

void OrderRouter::append_trade_line(const std::string& line)
{
    // 시각은 지금 박는다 — 쓰기 스레드가 언제 쓰든 행의 시각은 주문 스레드가 지나간 그 순간이어야 한다.
    std::string date, time_buffer;
    trade_row_timestamp(date, time_buffer);

    PendingLine pending;
    pending.sink = PendingLine::Sink::TRADE;
    pending.date = std::move(date);
    pending.text = time_buffer + ',' + line;
    queue_append_line(std::move(pending));
}

void OrderRouter::write_trade_row(const std::string& event, const ManagedOrder& managed_order,
                                  int fill_quantity, double fill_price, double realized_pnl,
                                  double strategy_realized_pnl)
{
    const OrderSignal& signal = managed_order.signal;

    auto side_string = [](OrderSide order_side) {
        return order_side == OrderSide::BUY ? "BUY" : (order_side == OrderSide::SELL ? "SELL" : "NONE");
    };
    auto type_string = [](OrderType order_type) { return order_type == OrderType::LIMIT ? "LIMIT" : "MARKET"; };
    auto status_string = [](OrderStatus status) -> const char* {
        switch (status)
        {
        case OrderStatus::PENDING:   return "PENDING";
        case OrderStatus::SUBMITTED: return "SUBMITTED";
        case OrderStatus::ACCEPTED:  return "ACCEPTED";
        case OrderStatus::REJECTED:  return "REJECTED";
        case OrderStatus::FILLED:    return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        default:                     return "?";
        }
    };

    // event 빈 문자열이면 상태 문자열을 사용
    const std::string_view event_text = event.empty() ? std::string_view(status_string(managed_order.status)) : std::string_view(event);
    // reason = 거부/봉쇄 사유(OrderGate·KIS), entry_reason = 진입 판단 근거(전략, G4) — 분리 컬럼.
    std::string reason       = csv_safe(managed_order.reject_reason);
    std::string entry_reason = csv_safe(signal.reason);

    std::string field = std::format("{},{},{},{},{},{},{},{},{:.2f},{},{:.2f},{},{},{},", event_text, managed_order.order_id,
                                managed_order.kis_order_no, signal.strategy_id, signal.ticker, side_string(signal.side), type_string(signal.type),
                                signal.quantity, signal.price, fill_quantity, fill_price, status_string(managed_order.status), reason,
                                entry_reason);

    // 실현손익은 매도 체결에서만 의미가 있다. 매수·접수·거부 행은 빈 칸으로 둬서
    //  0원 실현으로 오독되지 않게 한다.
    if (event == "FILL" && signal.side == OrderSide::SELL)
    {
        std::format_to(std::back_inserter(field), "{:.2f}", realized_pnl);
    }

    // sequence는 전략 스레드가 stamp한 신호 순번(C-2). 미부여(0)는 빈 칸 — 재기동 전 주문의 체결 등.
    field += ',';

    if (signal.sequence != 0)
    {
        field += std::to_string(signal.sequence);
    }

    // strategy_realized_pnl은 realized_pnl과 같은 조건(SELL 체결)에서만 채운다 — 열 끝 추가분(D-089).
    field += ',';

    if (event == "FILL" && signal.side == OrderSide::SELL)
    {
        std::format_to(std::back_inserter(field), "{:.2f}", strategy_realized_pnl);
    }

    append_trade_line(field);
}

// ─── 잔고 대조 기록 (C-2) ─────────────────────────────────────────────────
//  live_orders는 history_에서 센다(history_mutex_). 파일 쓰기는 락 밖.
void OrderRouter::record_reconcile(const ReconcileNote& reconcile_note)
{
    int live_orders = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        const symbol::SymbolId reconcile_symbol = gate_.ledger().symbol_id_of(reconcile_note.ticker); // 대조 메모는 문자열 — 한 번만 바꾼다

        for (const auto& managed_order : history_)
        {
            if (managed_order.status == OrderStatus::ACCEPTED && managed_order.signal.symbol_id == reconcile_symbol &&
                managed_order.signal.quantity - managed_order.confirmed_quantity > 0)
            {
                ++live_orders;
            }
        }
    }

    std::string reason = std::format("live_orders={} diff_qty={}", live_orders, reconcile_note.broker_quantity - reconcile_note.ledger_quantity);

    if (!reconcile_note.note.empty())
    {
        reason += ' ';
        reason += csv_safe(reconcile_note.note);
    }

    if (reconcile_note.action != "KEEP")
    {
        LOG_WARN(std::format("[OrderRouter] 잔고 대조 {} 원장 {}주@{} vs 브로커 {}주@{} → {} ({})", reconcile_note.ticker,
                             reconcile_note.ledger_quantity, static_cast<long long>(reconcile_note.ledger_average), reconcile_note.broker_quantity,
                             static_cast<long long>(reconcile_note.broker_average), reconcile_note.action, reason));
    }

    // 빈 칸: order_id·kis_order_no·strategy, side·type, 끝의 entry_reason·realized_pnl·sequence.
    append_trade_line(std::format("RECONCILE,,,,{},NONE,,{},{:.2f},{},{:.2f},{},{},,,", reconcile_note.ticker, reconcile_note.ledger_quantity,
                                  reconcile_note.ledger_average, reconcile_note.broker_quantity, reconcile_note.broker_average, csv_safe(reconcile_note.action), reason));
}

// ─── 주문 번호로 살아있는 주문 조회 (호출자가 history_mutex_ 보유) ────────────
//  live = ACCEPTED 이면서 미체결 잔량이 남은 주문(부분체결도 status는 ACCEPTED 유지).
//  FILLED/CANCELLED/REJECTED는 취소·정정 대상 아님.
ManagedOrder* OrderRouter::find_live_by_client_number(uint64_t client_order_number)
{
    ManagedOrder* managed_order = find_by_client_number_locked(client_order_number);

    if (managed_order && managed_order->status == OrderStatus::ACCEPTED &&
        managed_order->confirmed_quantity < managed_order->signal.quantity)
    {
        return managed_order;
    }

    return nullptr;
}

// ─── 이력 색인 (호출자가 history_mutex_ 보유) ─────────────────────────────────
void OrderRouter::push_history_locked(ManagedOrder managed_order)
{
    const uint64_t history_sequence = history_base_ + history_.size();

    if (managed_order.kis_order_number != 0)
    {
        slot_by_order_number_[managed_order.kis_order_number] = history_sequence;
    }

    if (managed_order.signal.client_order_number != 0)
    {
        slot_by_client_number_[managed_order.signal.client_order_number] = history_sequence;
    }

    history_.push_back(std::move(managed_order));
}

void OrderRouter::pop_history_front_locked()
{
    const ManagedOrder& front = history_.front();
    // 같은 키를 더 새 항목이 차지했으면 그 색인은 남긴다.
    const auto erase_if_mine = [this](std::unordered_map<uint64_t, uint64_t>& index, uint64_t key)
    {
        const auto iterator = index.find(key);

        if (iterator != index.end() && iterator->second == history_base_)
        {
            index.erase(iterator);
        }
    };

    if (front.kis_order_number != 0)
    {
        erase_if_mine(slot_by_order_number_, front.kis_order_number);
    }

    if (front.signal.client_order_number != 0)
    {
        erase_if_mine(slot_by_client_number_, front.signal.client_order_number);
    }

    history_.pop_front();
    ++history_base_;
}

ManagedOrder* OrderRouter::history_at_locked(uint64_t history_sequence)
{
    if (history_sequence < history_base_ || history_sequence - history_base_ >= history_.size())
    {
        return nullptr;
    }

    return &history_[history_sequence - history_base_];
}

ManagedOrder* OrderRouter::find_by_order_number_locked(uint64_t kis_order_number)
{
    if (kis_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_order_number_.find(kis_order_number);
    return iterator == slot_by_order_number_.end() ? nullptr : history_at_locked(iterator->second);
}

ManagedOrder* OrderRouter::find_by_client_number_locked(uint64_t client_order_number)
{
    if (client_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_client_number_.find(client_order_number);
    return iterator == slot_by_client_number_.end() ? nullptr : history_at_locked(iterator->second);
}

symbol::SymbolId OrderRouter::symbol_of(const OrderSignal& signal)
{
    return signal.symbol_id != symbol::kNone ? signal.symbol_id : gate_.ledger().intern_symbol(signal.ticker);
}

// ─── 취소 라우팅 (action=CANCEL) ──────────────────────────────────────────
//  1) original_client_order_number로 live 주문 조회 → 원 ODNO/조직번호/미체결 잔량 스냅샷
//  2) lock 밖에서 KIS 취소 호출(네트워크)
//  3) 성공 시에만 lock 재획득 → 미체결 잔량을 '그 시점 confirmed_quantity로 재계산'해 reserved 해제
//     (2)와 (3) 사이 체결 스레드의 on_fill이 confirmed_quantity를 올릴 수 있으므로 재계산이 이중해제를 막는다.
ManagedOrder OrderRouter::cancel_route(const OrderSignal& signal)
{
    auto& ledger = gate_.ledger();

    auto now = std::chrono::system_clock::now();
    ManagedOrder managed_order;
    managed_order.order_id     = next_id();
    managed_order.signal       = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;
    ++total_count_;

    // 1) 원주문 스냅샷 (record()는 history_mutex_를 재획득하므로 lock 스코프 밖에서만 호출)
    std::string ticker, kis_order_no, krx_forwarding_org_no, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    // 취소가 빗나갔을 때 "원주문이 이미 체결됐을 수 있나"를 같은 락 안에서 답해 둔다.
    //  find_live_by_client_number는 살아있는 주문만 보므로 !found는 세 경우를 뭉뚱그린다 —
    //  체결됨 / 이미 취소됨 / 애초에 접수된 적 없음(REJECTED·이력 없음).
    //  중복 매수 위험은 첫째에만 있다. [why D-035]
    bool original_may_have_filled = false;
    const char* gone_why = "이력 없음(재기동·이력초과)";
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);

        if (!original)
        {
            if (const ManagedOrder* history_entry = find_by_client_number_locked(signal.original_client_order_number))
            {
                if (history_entry->status == OrderStatus::FILLED ||
                    (history_entry->status == OrderStatus::ACCEPTED && history_entry->confirmed_quantity > 0))
                {
                    original_may_have_filled = true;
                    gone_why                 = "이미 체결";
                }
                else if (history_entry->status == OrderStatus::CANCELLED)
                {
                    gone_why = "이미 취소";
                }
                else if (history_entry->status == OrderStatus::REJECTED)
                {
                    gone_why = "접수된 적 없음(거부)";
                }
            }
        }

        if (original)
        {
            found        = true;
            ticker       = original->signal.ticker;
            kis_order_no = original->kis_order_no;
            krx_forwarding_org_no    = original->krx_forwarding_org_no;
            account      = original->signal.account_id;
            side         = original->signal.side;
            outstanding  = original->signal.quantity - original->confirmed_quantity;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        // 취소할 것이 없는 것은 거부가 아니라 끝난 상태다 — 전략은 취소 결과를 안 보고 계획을
        //  다시 짜므로, 거부된 분할 단계·이미 취소된 분할 단계를 다시 취소하는 요청이 재구성마다 온다
        //  (09-10~11 이틀 323건, 그중 체결 흔적은 21건). REJECTED로 세면 거부 통계와 경고가
        //  실제 문제(게이트·KIS 거부)를 덮는다. CANCELLED로 닫고, 체결 가능성이 있는 경우만
        //  경고와 매수 보류를 남긴다. [why D-035]
        managed_order.status        = OrderStatus::CANCELLED;
        managed_order.reject_reason = std::string("취소 대상 없음 (") + gone_why + ") oid=" + signal.original_client_order_id;

        // 체결 흔적이 있을 때만 매수를 잠근다. 접수된 적 없는 order_id(전략이 거부된 주문을
        //  live로 들고 있는 경우)에도 잠그면 매 재구성 주기마다 취소 빗나감 → 매수 거부 →
        //  거부된 oid가 다시 live로 → 다음 주기에 또 취소 빗나감으로 되돌아, 창이 계속
        //  갱신되며 그 종목 매수가 영구히 막힌다. 2026-09-10 006910이 이 모양으로
        //  보유 0인 채 57분간 한 주도 못 샀다(대체 주문 보류 176건). [why D-035]
        //  이미 무장돼 있으면 시각을 갱신하지 않는다 — 창은 연장되지 않는다.
        if (original_may_have_filled)
        {
            const symbol::SymbolId      symbol = symbol_of(signal);
            std::lock_guard<std::mutex> lock(history_mutex_);

            if (symbol >= cancel_miss_.size())
            {
                cancel_miss_.resize(std::max(ledger.symbols().capacity(), static_cast<size_t>(symbol) + 1));
            }

            if (cancel_miss_[symbol] == std::chrono::steady_clock::time_point{})
            {
                cancel_miss_[symbol] = std::chrono::steady_clock::now();
            }
        }

        if (original_may_have_filled)
        {
            LOG_WARN("[OrderRouter] 취소 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason +
                     " — 체결 가능성 있어 신규매수 보류");
        }
        else
        {
            LOG_INFO("[OrderRouter] 취소 불요 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        }

        record(managed_order);
        return managed_order;
    }

    // 2) KIS 취소 (lock 밖)
    OrderAck cancel;

    try
    {
        ++kis_calls_;
        cancel = kis_.cancel_order(ticker, kis_order_no, krx_forwarding_org_no, outstanding, /*all_remaining=*/true);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 취소 예외: ") + exception.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] 취소 예외 [" + managed_order.order_id + "] " + ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!cancel.ok())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(cancel);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 취소 거부 [" + managed_order.order_id + "] " + ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 3) 성공 — reserved 해제(잔량 재계산) + 원주문 CANCELLED 표기 + 인덱스 정리
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);
        int release = 0;

        if (original)
        {
            release = original->signal.quantity - original->confirmed_quantity; // 취소 성공 시점 실제 미체결

            if (release < 0)
            {
                release = 0;
            }

            original->status     = OrderStatus::CANCELLED;
            original->updated_at = std::chrono::system_clock::now();
        }

        // 원장 positions_mutex_는 history_mutex_와 별개다. 잠금 순서 history_mutex_ → positions_mutex_는 on_fill과 같다(데드락 없음).
        if (release > 0)
        {
            ledger.on_cancel(account, ticker, side, release,
                            OrderGate::OrderRef{original ? digits_to_number(original->order_id) : 0,
                                                digits_to_number(kis_order_no), signal.type});
        }
    }

    managed_order.status       = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    managed_order.kis_order_no = std::move(cancel.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + managed_order.order_id + "] " + ticker +
             " 원oid=" + signal.original_client_order_id + " 취소ODNO=" + managed_order.kis_order_no);
    record(managed_order);
    return managed_order;
}

// ─── 정정 라우팅 (action=REPLACE) ─────────────────────────────────────────
//  KIS 정정 1콜 = cancel-replace. 성공 시 새 ODNO 발급.
//  reserved 조정: new_quantity는 전송 전 INTENT에서 선점하고, 접수되면 원주문 미체결 잔량을 해제한다(같은 side). 원주문은 CANCELLED,
//  정정 결과를 새 ManagedOrder(ACCEPTED)로 추적(새 ODNO/새 client_order_id).
//  ⚠ 첫 컷 한계: 부분체결 상태 정정은 수량 정합이 복잡 → MM은 REPLACE 미사용(CANCEL+NEW 사용).
//     본 경로는 미체결 전량 대상 정정만 안전. 부분체결분 정정은 Phase 2에서 정밀화.
ManagedOrder OrderRouter::replace_route(const OrderSignal& signal)
{
    auto& ledger = gate_.ledger();

    auto now = std::chrono::system_clock::now();
    ManagedOrder managed_order;
    managed_order.order_id     = next_id();
    managed_order.signal       = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;
    ++total_count_;

    // 원주문 스냅샷 — 락 밖에서 KIS를 부르는 동안 history_ 원소가 축출될 수 있어 값으로 뜬다.
    std::string ticker, kis_order_no, krx_forwarding_org_no, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);

        if (original)
        {
            found        = true;
            ticker       = original->signal.ticker;
            kis_order_no = original->kis_order_no;
            krx_forwarding_org_no    = original->krx_forwarding_org_no;
            account      = original->signal.account_id;
            side         = original->signal.side;
            outstanding  = original->signal.quantity - original->confirmed_quantity;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "정정 대상 없음 oid=" + signal.original_client_order_id;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 정정 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        record(managed_order);
        return managed_order;
    }

    int new_quantity = (signal.quantity > 0) ? signal.quantity : outstanding;

    // 정정도 전송 전에 원장에 적는다 — 새 수량을 INTENT로 선점하고, 원주문 잔량은 접수된 뒤에 푼다.
    //  못 적으면 보내지 않는다(적히지 않은 주문은 나가지 않는다). [why D-113]
    OrderSignal reserve_signal = signal;
    reserve_signal.ticker      = ticker;
    reserve_signal.account_id  = account;
    reserve_signal.side        = side;
    reserve_signal.quantity    = new_quantity;
    const OrderGate::OrderRef order_reference{digits_to_number(managed_order.order_id), 0, signal.type};

    if (!take_intent(reserve_signal, order_reference))
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "원장 저널 기록 실패 — 정정 전송 생략";
        ++rejected_count_;
        record(managed_order);
        return managed_order;
    }

    OrderAck revise_acknowledgement;

    try
    {
        ++kis_calls_;
        revise_acknowledgement = kis_.revise_order(ticker, kis_order_no, krx_forwarding_org_no, new_quantity, signal.price);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 정정 예외: ") + exception.what();
        ++rejected_count_;
        ledger.on_reject(account, ticker, side, new_quantity, order_reference, managed_order.reject_reason);
        LOG_ERROR("[OrderRouter] 정정 예외 [" + managed_order.order_id + "] " + ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!revise_acknowledgement.ok())
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(revise_acknowledgement);
        ++rejected_count_;
        ledger.on_reject(account, ticker, side, new_quantity, order_reference, managed_order.reject_reason);
        LOG_WARN("[OrderRouter] 정정 거부 [" + managed_order.order_id + "] " + ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 성공 — 원 미체결 잔량 해제, 원주문 CANCELLED, 정정본 ACCEPTED 추적(선점은 INTENT에서 이미 잡혔다)
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);
        int release = outstanding;

        if (original)
        {
            release = original->signal.quantity - original->confirmed_quantity;

            if (release < 0)
            {
                release = 0;
            }

            original->status     = OrderStatus::CANCELLED;
            original->updated_at = std::chrono::system_clock::now();
        }

        if (release > 0)
        {
            ledger.on_cancel(account, ticker, side, release,
                            OrderGate::OrderRef{original ? digits_to_number(original->order_id) : 0,
                                                digits_to_number(kis_order_no), signal.type});
        }

        // 정정본 선점은 위 INTENT에서 이미 잡혔다 — 여기서는 새 주문번호로 ACCEPT만 적는다.
        ledger.on_accepted(account, ticker, side, new_quantity,
                          OrderGate::OrderRef{digits_to_number(managed_order.order_id),
                                              digits_to_number(revise_acknowledgement.kis_order_no), signal.type});
    }

    managed_order.status       = OrderStatus::ACCEPTED;
    managed_order.kis_order_no = std::move(revise_acknowledgement.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.krx_forwarding_org_no    = std::move(krx_forwarding_org_no); // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    managed_order.signal.side  = side;      // NONE 방지: 원주문 side 승계
    managed_order.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + managed_order.order_id + "] " + ticker +
             " 원oid=" + signal.original_client_order_id + " 새ODNO=" + managed_order.kis_order_no +
             std::format(" qty={} @{}", new_quantity, static_cast<int>(signal.price)));
    record(managed_order);
    return managed_order;
}

// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
void OrderRouter::on_fill(const FillNotification& fill_notification)
{
    auto& ledger = gate_.ledger();

    // unique_lock: 원장 갱신까지만 잡고, 파일 쓰기·publish 전에 푼다(W-8).
    std::unique_lock<std::mutex> lock(history_mutex_);
    // 중복 제거 — KIS 체결통보는 at-least-once(재전송/WS 재구독 시 중복 가능).
    // H0STCNI0 전문에 체결고유번호가 없어 kis_order_no+체결시각+수량+단가를 조합 키로 사용.
    // ODNO는 영업일 단위 재사용되고 fill_time은 HHMMSS(날짜 없음)라, 거래일(수신일)을
    // prefix로 붙여, 서로 다른 날의 동일키 충돌로 실체결을 오인해 drop하는 일을 막는다 (V-4).
    const uint64_t order_number = digits_to_number(fill_notification.kis_order_no); // 전문 문자열이 정수가 되는 자리
    const FillKey  fill_key{trade_date_number(std::chrono::system_clock::to_time_t(fill_notification.timestamp)), order_number,
                           static_cast<uint32_t>(digits_to_number(fill_notification.fill_time)), fill_notification.filled_quantity,
                           static_cast<int64_t>(fill_notification.filled_price * 100)};

    // 이 키는 유일하지 않다. 같은 초에 같은 수량·단가로 나뉘어 체결되면 서로 다른 실체결이
    //  같은 키를 갖는다(2026-09-07 ODNO 0000014893, 같은 초 2주 두 건 중 하나가 버려졌다). 그렇다고
    //  키가 겹치는 통보를 전부 받으면 재연결 뒤 재전송도 실체결로 쌓인다 — 잔량 상한은 총량만 막아
    //  체결가·시각 귀속이 틀어지고, 잔량이 취소되면 유령 보유가 남는다. 둘은 실어 온 세션으로 가른다.
    if (is_replayed_fill_locked(fill_key, fill_notification.session_generation))
    {
        lock.unlock();
        LOG_WARN(std::format("[OrderRouter] 재연결 뒤 같은 체결통보 — 재전송으로 보고 원장에 안 넣음 ODNO={} {} {}주 @{} time={} 세션={} (수량은 잔고 대조가 맞춘다)",
                             fill_notification.kis_order_no, fill_notification.ticker, fill_notification.filled_quantity,
                             static_cast<int>(fill_notification.filled_price), fill_notification.fill_time,
                             fill_notification.session_generation));
        return;
    }

    // 재기동 복원 — 이전 세션이 낸 주문이면 접수 때 남긴 사유 기록에서 되살린다.
    //  history_는 메모리라 재기동으로 비지만 기록 파일에는 ODNO·종목·수량·전략·사유가
    //  그대로 있다. 되살려 history_에 넣으면 아래 매칭 루프가 잔량 클램프까지 평소대로
    //  처리하므로, 전략 귀속을 잃는 미매핑 경로로 빠지지 않는다.
    if (order_number != 0)
    {
        if (!order_reasons_loaded_)
        {
            // 읽기 전에 줄 서 있는 사유를 디스크에 내린다 — 큐에 남은 줄은 아직 파일에 없어서다.
            //  아래 호출이 세션당 한 번만 읽으므로 이 비용도 한 번뿐이다. [why D-124]
            flush_append_outbox();
        }

        load_order_reasons_locked();   // 첫 체결통보 때 1회만 파일을 읽는다
        const bool known  = find_by_order_number_locked(order_number) != nullptr;
        auto       jitter = known ? order_reasons_.end() : order_reasons_.find(order_number);

        if (jitter != order_reasons_.end())
        {
            ManagedOrder record;
            record.order_id           = next_id();
            record.kis_order_no       = fill_notification.kis_order_no;
            record.kis_order_number   = order_number;
            record.status             = OrderStatus::ACCEPTED;
            record.confirmed_quantity      = 0;
            record.signal.ticker      = std::move(jitter->second.ticker); // 사유 기록은 아래에서 지우므로 옮겨 온다
            record.signal.symbol_id   = ledger.intern_symbol(record.signal.ticker); // 파일의 문자열 — 복원 때 한 번
            record.signal.side        = jitter->second.side;
            record.signal.type        = OrderType::LIMIT;
            record.signal.quantity    = jitter->second.quantity;
            record.signal.price       = jitter->second.price;
            record.signal.reference_price   = jitter->second.reference_price;
            record.signal.strategy_id = std::move(jitter->second.strategy_id);
            record.signal.strategy_index = ledger.strategy_index_of(record.signal.strategy_id); // 서브원장 귀속은 번호로
            record.signal.reason      = std::move(jitter->second.reason);
            record.submitted_at       = fill_notification.timestamp;
            record.updated_at         = fill_notification.timestamp;
            // 선점(reserved_)은 이전 세션과 함께 사라졌다. 아래 체결 처리가
            //  on_fill_confirmed로 선점을 깎으므로, 주문수량만큼 먼저 되살려 순변화를 맞춘다.
            //  일부만 체결되고 나머지가 취소되면 그만큼 선점이 남는데, 주기 잔고 대조의
            //  reset_reserved()가 실제 잔고로 되맞춘다.
            (void)ledger.on_intent(record.signal.account_id, record.signal.ticker, record.signal.side,
                                  record.signal.quantity,
                                  record.signal.price > 0.0 ? record.signal.price : record.signal.reference_price,
                                  OrderGate::OrderRef{digits_to_number(record.order_id), order_number, record.signal.type},
                                  record.signal.strategy_index);
            LOG_INFO("[OrderRouter] 재기동 복원 [" + record.order_id + "] ODNO=" + fill_notification.kis_order_no + " " +
                     record.signal.ticker +
                     (record.signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                     std::to_string(record.signal.quantity) + "주 전략=" + record.signal.strategy_id +
                     " (주문 사유 기록에서 복구)");
            push_history_locked(std::move(record));
            order_reasons_.erase(jitter);   // 같은 ODNO를 두 번 되살리지 않는다
        }
    }


    // 이미 주문수량을 다 채운 주문의 추가 통보인지 구분한다. 이걸 아래 미매핑 경로로
    //  흘려보내면 같은 체결이 포지션에 두 번 쌓인다(ODNO는 아는데 잔량만 없는 상태).
    bool          exhausted = false;
    ManagedOrder* matched   = find_by_order_number_locked(order_number); // ODNO 색인 한 번 — 이력을 훑지 않는다

    // 원주문번호로 한 번 더 찾는다. 정정이 나가면 KIS가 새 ODNO를 주고 이력의 ODNO를 그 값으로 바꾸는데,
    //  정정 응답을 못 받으면(전송 실패·타임아웃) 이력에는 옛 ODNO가 남는다. 그 뒤 체결통보는 새 ODNO로
    //  오므로 위 색인이 비고, 전략 귀속을 잃은 채 미매핑 경로로 떨어진다. 전문 [3]OODER_NO가 그 옛 ODNO라
    //  여기서 되찾는다. 원장에 쓰는 번호는 통보가 준 실제 ODNO 그대로다(바꾸지 않는다).
    if (!matched)
    {
        const uint64_t original_order_number = digits_to_number(fill_notification.original_order_no);

        if (original_order_number != 0 && original_order_number != order_number)
        {
            matched = find_by_order_number_locked(original_order_number);

            if (matched)
            {
                LOG_WARN(std::format("[OrderRouter] 원주문번호로 체결 연결 [{}] 통보ODNO={} 원주문ODNO={} {} {}주 (정정 응답 유실 추정)",
                                     matched->order_id, fill_notification.kis_order_no, fill_notification.original_order_no,
                                     fill_notification.ticker, fill_notification.filled_quantity));
            }
        }
    }

    // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용. 그 밖의 상태(취소·거부)는 미매핑 경로로.
    if (matched && (matched->status == OrderStatus::ACCEPTED || matched->status == OrderStatus::FILLED))
    {
        ManagedOrder& managed_order = *matched;

        // 이미 전량 체결 완료된 주문은 재처리 방지
        if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
        {
            exhausted = true;
        }
        else
        {

        // 주문 잔량 상한 — 누적 체결이 주문수량을 넘지 못하게 클램프한다.
        //  통보 재전송으로 같은 체결이 두 번 와도 과체결로 원장이 부풀지 않는다.
        const int outstanding = managed_order.signal.quantity - managed_order.confirmed_quantity;
        const int apply_quantity   = (fill_notification.filled_quantity > outstanding) ? outstanding : fill_notification.filled_quantity;

        managed_order.confirmed_quantity += apply_quantity;
        managed_order.updated_at     = fill_notification.timestamp;

        if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
        {
            managed_order.status = OrderStatus::FILLED;
        }


        // 포지션 원장 갱신 (average_price 재계산 + 실현손익) — 원주문의 계좌로 파티션.
        // 현재는 단일 CANO 전제라 ODNO가 유일 → managed_order.signal.account_id 매핑이 정확하다.
        // TODO(다계좌): 진짜 다중 CANO 라우팅 시 ODNO가 계좌별로 재사용되므로 체결 매칭 키를
        //   (kis_order_no + account) 또는 CANO별 H0STCNI 피드 분리로 확장해야 오적립을 막는다.
        auto result = ledger.on_fill_confirmed(managed_order.signal.account_id, fill_notification.ticker, fill_notification.side,
                                              apply_quantity, fill_notification.filled_price, managed_order.signal.strategy_index,
                                              OrderGate::OrderRef{digits_to_number(managed_order.order_id), order_number,
                                                                  managed_order.signal.type});

        // 락 밖에서 쓰려고 복사한다 — managed_order는 history_ 원소라 record()의 축출로 참조가 죽을 수 있다.
        const ManagedOrder snapshot        = managed_order;
        std::string        open_orders = snapshot_open_orders_locked(); // 잔량이 줄었으니 부속 파일을 다시 쓴다
        const uint64_t     sequence         = ++open_orders_sequence_;
        lock.unlock();

        // 로그 문장은 락을 푼 뒤 사본으로 만든다 — 체결마다 도는 자리라 history_mutex_를 잡은 채 문자열을
        //  잇지 않는다(CODE_REVIEW S-3).
        if (apply_quantity < fill_notification.filled_quantity)
        {
            LOG_WARN(std::format("[OrderRouter] 주문잔량 초과 체결통보 — 잔량으로 클램프 [{}] ODNO={} 통보={}주 잔량={}주",
                                 snapshot.order_id, fill_notification.kis_order_no, fill_notification.filled_quantity, outstanding));
        }

        LOG_INFO(std::format("[OrderRouter] 체결 확인 [{}] ODNO={} {} {} {}주 @{} (누적 {}/{}주)", snapshot.order_id,
                             fill_notification.kis_order_no, fill_notification.ticker,
                             fill_notification.side == OrderSide::BUY ? "BUY" : "SELL", apply_quantity,
                             static_cast<int>(fill_notification.filled_price), snapshot.confirmed_quantity,
                             snapshot.signal.quantity));

        if (result.basis_unknown)
        {
            LOG_WARN(std::format("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [{}] {} {}주 @{} (원장 재시드 필요)",
                                 snapshot.order_id, fill_notification.ticker, apply_quantity, static_cast<int>(fill_notification.filled_price)));
        }

        // 거래 원장 CSV — 실제 체결(부분/전량)을 한 줄로 영속화. 실현손익을 같이 남기려고
        //   gate_.ledger().on_fill_confirmed() 뒤에 쓴다(managed_order.status는 위에서 이미 갱신됨).
        write_trade_row("FILL", snapshot, apply_quantity, fill_notification.filled_price, result.realized_pnl,
                        result.strategy_realized_pnl);
        queue_open_orders_file(std::move(open_orders), sequence);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_fill(fill_notification, snapshot.signal.strategy_id, result.commission, result.tax,
                               result.average_price, result.net_quantity,
                               result.realized_pnl);
        }
#endif
        return;
        }
    }

    if (exhausted)
    {
        LOG_WARN(std::format("[OrderRouter] 주문수량 충족 후 추가 체결통보 무시 ODNO={} {} {}주 (통보 재전송 추정)", fill_notification.kis_order_no,
                             fill_notification.ticker, fill_notification.filled_quantity));
        return;
    }

    // ── ODNO 미매핑 체결 — 이 프로세스가 낸 주문이 아니다 ────────────────────
    //  history_는 메모리에만 있어서 장중 재시작하면 이전 세션의 미체결 주문이 사라진다.
    //  거래소 호가창에는 그 주문이 그대로 살아있으므로, 나중에 체결되면 여기로 떨어진다.
    //  2026-09-07 ODNO 0000014893이 이 경우다 — 09:58 접수, 10:38 재시작, 11:07 91주 전량
    //  체결이 통째로 버려져 원장·포지션이 91주(약 498만원) 어긋났다.
    //  체결 자체는 실재하므로 버리지 않고 원장·포지션에 반영한다. 전략 귀속만 알 수 없어
    //  strategy_id를 "UNLINKED"로 남긴다(사후 분석에서 구분 가능).
    //  선점(reserved_)은 이전 세션과 함께 사라졌다. on_fill_confirmed는 선점 해제를 전제로
    //  reserved_를 깎으므로, 그대로 부르면 음수 선점이 생겨 이후 한도 계산이 왜곡된다.
    //  같은 수량을 on_intent로 먼저 되살린 뒤 해제시켜 순변화를 0으로 맞춘다(원장에도 INTENT→FILL 두 줄로 남는다).
    //  미연결은 history_에 없어 우리 쪽 주문수량을 모른다. 대신 전문 [16]ODER_QTY가 그 주문의 총수량이라,
    //  있으면 연결된 주문과 같은 방식으로 누적 체결을 그 수량까지 묶는다. 상한이 키가 아니라 수량이 되므로
    //  같은 초·같은 수량·단가로 갈라진 진짜 분할체결도 잃지 않는다(종전 W-6의 손실을 되돌린다).
    //  전문이 그 칸을 안 주면(order_quantity==0) 종전대로 키(거래일:ODNO:시각:수량:단가) 중복 제거로 막는다 —
    //  분할체결을 잃을 수 있지만 두 번 쌓는 쪽이 더 큰 사고다(W-6).
    //  주문수량이 이번 통보 수량보다 작으면 전문을 믿지 않는다(칸이 밀렸거나 뜻이 다른 값).
    // [inv] 주문 단위 키 = 체결 건별 칸(시각·수량·단가)을 0으로 둔 FillKey. ODNO는 영업일마다 재사용되므로
    //  거래일을 같이 담는다(체결 건별 키와 같은 이유, V-4).
    const FillKey unlinked_order_key{fill_key.trade_date, fill_key.order_number, 0, 0, 0};
    UnlinkedOrder& unlinked_order = unlinked_orders_[unlinked_order_key];
    int            unlinked_quantity = fill_notification.filled_quantity;

    if (fill_notification.order_quantity >= fill_notification.filled_quantity && fill_notification.order_quantity > 0)
    {
        unlinked_order.order_quantity = fill_notification.order_quantity;
    }

    if (unlinked_order.order_quantity > 0)
    {
        const int outstanding = unlinked_order.order_quantity - unlinked_order.confirmed_quantity;

        if (outstanding <= 0)
        {
            LOG_WARN(std::format("[OrderRouter] 미매핑 주문수량 충족 후 추가 체결통보 무시 ODNO={} {} {}주 (주문수량 {}주 전량 반영 완료)",
                                 fill_notification.kis_order_no, fill_notification.ticker,
                                 fill_notification.filled_quantity, unlinked_order.order_quantity));
            return;
        }

        if (unlinked_quantity > outstanding)
        {
            LOG_WARN(std::format("[OrderRouter] 미매핑 주문잔량 초과 체결통보 — 잔량으로 클램프 ODNO={} {} 통보={}주 잔량={}주",
                                 fill_notification.kis_order_no, fill_notification.ticker,
                                 fill_notification.filled_quantity, outstanding));
            unlinked_quantity = outstanding;
        }
    }
    else if (!unlinked_fill_keys_.insert(fill_key).second)
    {
        LOG_WARN(std::format("[OrderRouter] 미매핑 체결 재통보 무시 ODNO={} {} {}주 time={} (주문수량 미상 — 같은 키 재수신)",
                             fill_notification.kis_order_no, fill_notification.ticker,
                             fill_notification.filled_quantity, fill_notification.fill_time));
        return;
    }

    unlinked_order.confirmed_quantity += unlinked_quantity;

    ManagedOrder unlinked_fill;
    unlinked_fill.order_id           = next_id();
    unlinked_fill.kis_order_no       = fill_notification.kis_order_no;
    unlinked_fill.status             = OrderStatus::FILLED;
    unlinked_fill.confirmed_quantity      = unlinked_quantity;
    unlinked_fill.signal.strategy_id    = "UNLINKED";
    unlinked_fill.signal.strategy_index = unlinked_strategy_index_;
    unlinked_fill.signal.ticker      = fill_notification.ticker;
    unlinked_fill.signal.side        = fill_notification.side;
    unlinked_fill.signal.type        = OrderType::LIMIT;
    unlinked_fill.signal.quantity    = unlinked_quantity;
    unlinked_fill.signal.price       = fill_notification.filled_price;
    unlinked_fill.signal.reason      = "이전 세션 주문 체결(ODNO 미매핑)";
    unlinked_fill.submitted_at       = fill_notification.timestamp;
    unlinked_fill.updated_at         = fill_notification.timestamp;

    const int unlinked_order_quantity = unlinked_order.order_quantity; // 로그용 — unlinked_orders_ 원소는 락 밖에서 안 읽는다

    const OrderGate::OrderRef unlinked_reference{digits_to_number(unlinked_fill.order_id), order_number,
                                                 unlinked_fill.signal.type};
    (void)ledger.on_intent(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                          unlinked_quantity, fill_notification.filled_price, unlinked_reference,
                          unlinked_fill.signal.strategy_index);
    auto result = ledger.on_fill_confirmed(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                                          unlinked_quantity, fill_notification.filled_price,
                                          unlinked_fill.signal.strategy_index, unlinked_reference);
    lock.unlock(); // 원장 갱신 끝 — 파일 쓰기는 락 밖에서

    LOG_WARN(std::format("[OrderRouter] 미매핑 체결 원장 반영 [{}] ODNO={} {} {} {}주 @{} (주문수량 {}) — 이전 세션 주문으로 추정(재시작 전 접수분)",
                         unlinked_fill.order_id, fill_notification.kis_order_no, fill_notification.ticker, fill_notification.side == OrderSide::BUY ? "BUY" : "SELL",
                         unlinked_quantity, static_cast<int>(fill_notification.filled_price),
                         unlinked_order_quantity > 0 ? std::to_string(unlinked_order_quantity) + "주" : std::string("미상")));

    if (result.basis_unknown)
    {
        LOG_WARN(std::format("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [{}] {} {}주 @{} (원장 재시드 필요)",
                             unlinked_fill.order_id, fill_notification.ticker, unlinked_quantity, static_cast<int>(fill_notification.filled_price)));
    }

    write_trade_row("FILL", unlinked_fill, unlinked_quantity, fill_notification.filled_price, result.realized_pnl,
                    result.strategy_realized_pnl);
#ifdef HAS_ZMQ
    if (zmq_)
    {
        zmq_->publish_fill(fill_notification, unlinked_fill.signal.strategy_id, result.commission, result.tax,
                           result.average_price, result.net_quantity,
                           result.realized_pnl);
    }
#else
    (void)result;
#endif
}

// 같은 세션 안에서 같은 키가 다시 오면 분할체결로 받는다. 세션이 바뀌면 앞 세션까지 받은 횟수를
//  기억해 두고, 새 세션에서 그 횟수 이하로 오는 통보는 재전송으로 본다 — 증권사가 재구독 뒤 옛 통보를
//  다시 보내면 한 건씩 한 번 오므로, 앞에서 받은 수를 넘는 몫만 새 체결이다.
//  잘못 거른 실체결(재연결과 같은 초에 같은 수량·단가로 난 체결)은 수량만 잔고 대조가 되찾는다.
bool OrderRouter::is_replayed_fill_locked(const FillKey& fill_key, uint32_t session_generation)
{
    FillSighting& sighting = fill_sightings_[fill_key];

    if (sighting.seen_in_session == 0 || sighting.session_generation != session_generation)
    {
        sighting.session_generation      = session_generation;
        sighting.accepted_before_session = sighting.accepted;
        sighting.seen_in_session         = 0;
    }

    ++sighting.seen_in_session;

    if (sighting.seen_in_session <= sighting.accepted_before_session)
    {
        replayed_fills_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    ++sighting.accepted;

    if (sighting.accepted > 1)
    {
        LOG_INFO(std::format("[OrderRouter] 동일키 분할체결 {}회차 ODNO={} time={} {}주", sighting.accepted,
                             fill_key.order_number, fill_key.fill_time, fill_key.quantity));
    }

    return false;
}

// ─── 일별 리셋 (장 시작 시 Engine이 호출) ─────────────────────────────────
// 체결 목격 기록(fill_sightings_)의 무한 증가를 해소. 거래일 prefix로 cross-day 충돌은 이미
// 차단되므로, 전일 키는 더 이상 필요 없다.
void OrderRouter::reset_daily()
{
    std::lock_guard<std::mutex> lock(history_mutex_);
    fill_sightings_.clear();
    unlinked_fill_keys_.clear();
    unlinked_orders_.clear();
    // 사유 기록도 거래일이 바뀌면 다시 읽는다(파일이 날짜별이라 어제 것을 들고 있으면 안 된다).
    order_reasons_.clear();
    order_reasons_loaded_ = false;
}

// ─── 통계 ─────────────────────────────────────────────────────────────────
OrderRouter::Stats OrderRouter::statistics() const
{
    return {total_count_.load(), accepted_count_.load(), rejected_count_.load()};
}

// ─── 최근 N건 이력 ────────────────────────────────────────────────────────
std::vector<ManagedOrder> OrderRouter::recent(int count) const
{
    // 락 안에서 뜬 사본을 돌려준다 — 호출자는 락 밖에서 읽고, history_ 원소는 축출로 사라질 수 있다.
    std::lock_guard<std::mutex> lock(history_mutex_);
    int start = std::max(0, static_cast<int>(history_.size()) - count);
    return {history_.begin() + start, history_.end()};
}
