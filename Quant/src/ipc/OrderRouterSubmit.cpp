// 주문 라우터 — 발주 경로(신규·취소·정정), 청산차단 자가정리, 이력 저장.
#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>

#include <format>
#include <string_view>
#include <vector>

// ─── 거부 사유에 KIS 오류코드 꼬리표 부착 ───────────────────────────────
//  order_thread가 EGW00201(초당 거래건수 초과)을 문자열로 판별해 적응적 재시도를 걸 수 있게,
//  (근거: EGW00201 뜻은 실측 응답 msg1 "초당 거래건수를 초과하였습니다." — logs/quant_trader.log 2026-08-10, D-065)
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
void OrderRouter::NewRoute::stamp_gate_stages()
{
    managed_order.stages.gate_us              = (trace::now_ns() - entered_ns - history_guard_ns) / 1000;
    managed_order.stages.history_guard_us     = history_guard_ns / 1000;
    managed_order.stages.history_lock_wait_us = history_lock_wait_ns / 1000;
}

// ─── 발주 경로 공통 조각 ─────────────────────────────────────────────────
ManagedOrder OrderRouter::make_pending_order(const OrderSignal& signal, std::chrono::system_clock::time_point now)
{
    ManagedOrder managed_order;
    managed_order.order_id     = next_id();
    managed_order.signal       = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;
    ++total_count_;
    return managed_order;
}

void OrderRouter::mark_rejected(ManagedOrder& managed_order, std::string reason)
{
    managed_order.status        = OrderStatus::REJECTED;
    managed_order.reject_reason = std::move(reason);
    ++rejected_count_;
}

void OrderRouter::record_before_transport(ManagedOrder& managed_order)
{
    managed_order.stages.record_us        = record(managed_order, &managed_order.stages.open_orders_us);
    managed_order.stages.history_store_us = managed_order.stages.record_us;
}

void OrderRouter::publish_order_result(const OrderSignal& signal, bool accepted)
{
#ifdef HAS_ZMQ
    if (zmq_)
    {
        zmq_->publish_order(signal, accepted);
    }
#else
    (void)signal;
    (void)accepted;
#endif
}

OrderAck OrderRouter::send_cancel(const std::string& ticker, const std::string& kis_order_no,
                                  const std::string& krx_forwarding_org_no, int quantity)
{
    ++kis_calls_;
    return kis_.cancel_order(ticker, kis_order_no, krx_forwarding_org_no, quantity, /*all_remaining=*/true);
}

bool OrderRouter::fetch_open_orders(std::vector<OpenOrder>& open_orders, std::string_view failure_message,
                                    std::string_view exception_message)
{
    try
    {
        auto fetched = kis_.get_open_orders();

        if (!fetched)
        {
            LOG_WARN(std::string(failure_message) + error_text(fetched));
            return false;
        }

        open_orders = std::move(*fetched);
        return true;
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string(exception_message) + exception.what());
        return false;
    }
}

bool OrderRouter::erase_carry_row(const std::string& kis_order_no)
{
    std::lock_guard<std::mutex> carry_lock(carry_mutex_);
    const auto before = carry_rows_.size();
    carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                     [&kis_order_no](const CarryRow& parts)
                                     {
                                         return parts[0] == kis_order_no;
                                     }),
                      carry_rows_.end());
    return carry_rows_.size() != before;
}

std::optional<int> OrderRouter::parse_quantity(const std::string& text)
{
    try
    {
        return std::stoi(text);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// ─── 주문 제출 — action에 따라 라우팅 (MM-1) ─────────────────────────────
//  전 경로가 주문 스레드 하나(Engine::order_thread_fn)에서만 실행된다 — OrderGate C6의
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다. 전송 스레드는 send_new만
//  부른다(게이트·원장·이력은 안 만진다). [why D-151]
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
//  열기 → 전송 → 닫기를 한 스레드에서 잇는다. 전송 스레드를 쓰는 주문 스레드는 셋을 따로 부른다. [why D-151]
ManagedOrder OrderRouter::new_route(const OrderSignal& in_signal)
{
    auto opened = open_new(in_signal);

    if (auto* finished = std::get_if<ManagedOrder>(&opened))
    {
        return std::move(*finished);
    }

    auto& send = std::get<NewOrderSend>(opened);
    send_new(send);
    return close_new(std::move(send));
}

// 단계: 클램프 → 이력 가드 둘 → 게이트 → INTENT. 단계가 거짓이면 그 단계가 거부로 닫고 이력까지 적었다.
std::variant<ManagedOrder, OrderRouter::NewOrderSend> OrderRouter::open_new(const OrderSignal& in_signal)
{
    const auto now = std::chrono::system_clock::now();
    NewRoute   route;
    // 구간 계측 시작. 여기부터 게이트 판정 끝까지가 gate_us — 주문 스레드가 HEALTH 분포에 넣는다. [why D-117]
    route.entered_ns       = trace::now_ns();
    route.signal           = in_signal;                // 사본 — 클램프가 수량을 잘라 고친다
    route.signal.symbol_id = symbol_of(route.signal); // 이력 항목이 종목 id를 들게 — 아래 비교·색인이 문자열을 안 본다

    clamp_new_order(route);
    route.managed_order = make_pending_order(route.signal, now);

    if (hold_after_cancel_miss(route) || skip_duplicate_market_sell(route))
    {
        return std::move(route.managed_order);
    }

    route.order_reference = OrderGate::OrderRef{digits_to_number(route.managed_order.order_id), 0, route.signal.type};
    // 여기부터 close_new의 이력 기록까지 이 종목의 선점을 정리가 풀지 못하게 건다 — INTENT 두 자리(청산 재매도,
    //  신규 전송) 모두 이 안이다. [why D-113]
    auto in_flight = std::make_unique<InFlightMark>(*this, route.signal.symbol_id != symbol::kNone
                                                               ? route.signal.symbol_id
                                                               : gate_.ledger().intern_symbol(route.signal.ticker));

    if (!pass_gate(route) || !prepare_transmit(route))
    {
        return std::move(route.managed_order);
    }

    if (route.freed) // 예약매도 취소 뒤 재발주가 이미 접수됐다 — 보낼 것 없이 그 결과로 닫는다
    {
        finalize_new_order(route);
        return std::move(route.managed_order);
    }

    // 여기서부터 close_new까지 ODNO를 모르는 주문이다 — 그 사이 온 체결은 on_fill이 붙들어 둔다. [why D-151]
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ++sending_new_orders_;
    }

    return NewOrderSend{std::move(route), std::move(in_flight)};
}

// 닫기 — 표시는 이력에 적은 뒤 풀리도록 지역으로 옮겨 둔다(지역 변수는 선언 역순으로 소멸).
ManagedOrder OrderRouter::close_new(NewOrderSend&& send)
{
    const std::unique_ptr<InFlightMark> in_flight = std::move(send.in_flight);
    NewRoute&                           route     = send.route;

    try
    {
        if (route.transport_failed)
        {
            close_transport_failure(route);
        }
        else
        {
            finalize_new_order(route);
        }
    }
    catch (...)
    {
        // 수를 안 내리면 그 뒤 연결 안 되는 체결을 끝없이 붙든다.
        finish_sending_new();
        throw;
    }

    // ODNO가 이력에 들어간 뒤라 붙든 체결 중 이 주문 것은 이제 연결된다.
    finish_sending_new();
    return std::move(route.managed_order);
}

// 0. 한도 클램프 — 한도를 넘치면 거부 대신 한도 안으로 줄여 낸다.
//    분할 매수 전략은 매 틱 같은 분할 단계를 다시 내므로, 넘친다고 버리면 그 종목은 하루 종일
//    한 주도 못 나가면서 초당 주문 예산만 태운다(09-08 오전 126640·293490 반복 거부).
//    여유가 0이면 손대지 않는다 — 아래 check()가 어느 한도에 걸렸는지 그대로 남기게 둔다.
void OrderRouter::clamp_new_order(NewRoute& route)
{
    OrderSignal& signal = route.signal;
    route.allowed       = gate_.clamp_buy_quantity(signal);

    if (route.allowed == 0 && signal.side == OrderSide::SELL && signal.action == OrderAction::NEW &&
        signal.quantity > 0)
    {
        // 매도가능수량이 0이면 여기서 끊는다. BUY와 달리 아래 check()는 매도가능수량을 모르므로
        //  그대로 통과시키고, KIS가 주문을 통째로 40240000(모의투자 잔고내역이 없습니다)으로
        //  거부한다 — 한 주도 못 빠져나오면서 초당 주문 예산만 태운다(09-08 001450 105주·
        //  086450 486주·047050 254주/381주가 모두 이 경로로 전량 거부됐다).
        route.sell_no_quantity = true;
    }
    else if (route.allowed > 0 && route.allowed < signal.quantity && signal.side == OrderSide::SELL &&
             gate_.ledger().sellable_view(signal.account_id, signal.ticker).pending > 0)
    {
        // 매도가능이 모자란 이유가 이 세션의 예약매도(익절 지정가)라면 잘라 내지 않고, 그 예약을 취소해
        //  수량을 풀고 전량을 낸다 — 아래 sell_no_quantity 와 같은 길. 잘라 내면 나머지는 전략이 취소를 낸 뒤
        //  다음 백오프(30초 뒤)에야 나간다(09-14 15:15 012210 115주 중 100주만, 나머지는 15:16 뒤). [why D-082]
        route.sell_no_quantity = true;
    }
    else if (route.allowed > 0 && route.allowed < signal.quantity)
    {
        LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주", signal.ticker, signal.quantity, route.allowed));
        signal.quantity = route.allowed;
    }
}

// 방금 이 종목의 취소가 "취소 대상 없음"으로 되돌아왔다면, 원주문이 이미 체결됐을 수
//  있다. 전략은 그 결과를 보지 못한 채 대체 주문을 이어 내므로 그대로 두면 중복 매수가
//  된다. 다음 재구성 주기에 전략이 실제 보유수량을 다시 읽을 때까지만 막는다.
//  매도는 막지 않는다 — 노출을 줄이는 쪽이고, 늦추면 손실이 커진다.
bool OrderRouter::hold_after_cancel_miss(NewRoute& route)
{
    const OrderSignal& signal = route.signal;

    if (signal.side != OrderSide::BUY)
    {
        return false;
    }

    bool          blocked          = false;
    const int64_t guard_started_ns = trace::now_ns();
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        route.history_lock_wait_ns += trace::now_ns() - guard_started_ns;

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

    route.history_guard_ns += trace::now_ns() - guard_started_ns;

    if (!blocked)
    {
        return false;
    }

    ManagedOrder& managed_order = route.managed_order;
    mark_rejected(managed_order, "직전 취소가 대상 없음 — 보유수량 재확인까지 보류");
    route.stamp_gate_stages();
    LOG_WARN("[OrderRouter] 대체 주문 보류 [" + managed_order.order_id + "] " + signal.ticker +
             " " + managed_order.reject_reason);
    record_before_transport(managed_order);
    return true;
}

// 같은 청산의 중복 발주 차단 — 시장가 매도가 접수돼 아직 체결 통보가 없는데(발주 스레드가 밀리면 몇 분)
//  전략이 백오프마다 같은 매도를 다시 낸다(09-14 15:15 036930 SELL 9 가 30초·60초 뒤 두 번 더 큐에 쌓임).
//  라우터 이력에 같은 종목·같은 전략의 시장가 매도가 미체결 잔량을 들고 살아 있으면 KIS 로 보내지 않는다.
//  KIS 호출이 없으니 발주 스레드 예산을 안 쓴다. [why D-082]
bool OrderRouter::skip_duplicate_market_sell(NewRoute& route)
{
    const OrderSignal& signal = route.signal;

    if (signal.side != OrderSide::SELL || signal.action != OrderAction::NEW || signal.type != OrderType::MARKET)
    {
        return false;
    }

    std::string   duplicate;
    const int64_t guard_started_ns = trace::now_ns();
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        route.history_lock_wait_ns += trace::now_ns() - guard_started_ns;
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

            duplicate = history_entry.order_id + " 미체결 " + std::to_string(outstanding_of(history_entry)) + "주";
            break;
        }
    }

    route.history_guard_ns += trace::now_ns() - guard_started_ns;

    if (duplicate.empty())
    {
        return false;
    }

    ManagedOrder& managed_order = route.managed_order;
    mark_rejected(managed_order, "같은 시장가 매도 진행 중 [" + duplicate + "] — 중복 발주 생략");
    route.stamp_gate_stages();
    LOG_INFO("[OrderRouter] 중복 생략 [" + managed_order.order_id + "] " + signal.ticker + " " +
             std::to_string(signal.quantity) + "주 → " + managed_order.reject_reason);
    record_before_transport(managed_order);
    return true;
}

// 1. OrderGate 검증
bool OrderRouter::pass_gate(NewRoute& route)
{
    OrderSignal&  signal        = route.signal;
    ManagedOrder& managed_order = route.managed_order;

    if (route.sell_no_quantity)
    {
        // 왜 0인지 남기고, 이 프로세스가 아는 예약매도(이번 세션 이력·이전 세션 부속 파일)를 취소해
        //  수량을 풀어 본다. 거부만 하고 끝내면 예약매도가 브로커에 남은 채 청산이 하루 종일 막힌다
        //  (09-11 014530: 09:17 익절 지정가 118주가 취소 한도거부로 잔존, 이후 재기동 8회 내내 거부).
        //  풀리면 그 자리에서 재발주한 접수로 이어간다. [why D-055]
        const auto sellable_view = gate_.ledger().sellable_view(signal.account_id, signal.ticker);
        LOG_WARN(std::format("[OrderRouter] 매도가능 {}/{}주 {} — 원장 보유 {}주, 잔고 주문가능 {}주, 이 세션 미체결 매도 {}주 → 예약매도 취소 시도",
                             route.allowed, signal.quantity, signal.ticker, sellable_view.held, sellable_view.possible_quantity_cap, sellable_view.pending));
        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal, route.order_reference, route.intent_taken);

        if (reconcile_acknowledgement.ok())
        {
            route.acknowledgement = std::move(reconcile_acknowledgement);
            route.freed           = true;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty && route.allowed > 0)
        {
            // 취소할 예약매도를 못 찾았지만 일부는 나갈 수 있다 — 잘라서라도 낸다(예전 클램프 경로).
            LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주 (예약매도 취소 불발)", signal.ticker, signal.quantity, route.allowed));
            signal.quantity      = route.allowed;
            managed_order.signal = signal;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty)
        {
            route.reject_reason = "매도가능수량 0 (미체결 매도·미결제분) — 취소할 예약매도 없음, 발주 생략";
        }
        else
        {
            // 예약매도는 취소됐는데 재매도가 거부(유량한도·전송 실패)된 것 — 수량은 풀렸으니 재시도가 낸다.
            //  09-14 15:00 096770: 취소 뒤 재매도가 EGW00201 에 걸렸는데 "취소할 예약매도 없음"으로 남아 원인을 잘못 짚었다.
            route.reject_reason = "예약매도 취소 뒤 재매도 실패 [" + reconcile_acknowledgement.error_code + "] — 재시도 대상";
        }
    }

    if (route.reject_reason.empty() && (route.freed || gate_.check(signal, route.reject_reason)))
    {
        return true;
    }

    mark_rejected(managed_order, route.reject_reason);
    route.stamp_gate_stages();
    LOG_WARN("[OrderRouter] 거부 [" + managed_order.order_id + "] " +
             signal.ticker + " → " + route.reject_reason);
    publish_order_result(signal, false);
    record_before_transport(managed_order);
    return false;
}

// 2. KIS 주문 전송 (submit_order_acknowledgement로 ODNO + KRX 조직번호 캡처 — 정정/취소 준비)
//    접수 왕복지연(RTT)을 재서 접수 로그에 남긴다 → log_report.py가 중앙값(p50)·상위 1%(p99) 집계.
//    RTT 안에는 초당 한도 버킷 대기(rate_limit_acquire)가 섞여 있어 그 몫을 따로 적는다 — 09-14~18 RTT p50 2초가
//    망 지연인지 버킷 줄서기인지 이 숫자 없이는 못 가른다. 전송 스레드 분리(T-13-2)는 이 값을 보고 정했다. [why D-117]
//    여기는 채비까지다 — INTENT를 적고 호출 수를 센다. 보내기는 send_new. [why D-151]
bool OrderRouter::prepare_transmit(NewRoute& route)
{
    const OrderSignal& signal        = route.signal;
    ManagedOrder&      managed_order = route.managed_order;

    managed_order.status = OrderStatus::SUBMITTED;
    route.stamp_gate_stages();
    route.send_started = std::chrono::steady_clock::now();

    const int64_t journal_started_ns = trace::now_ns();

    // 원장 먼저, 전송은 그 다음 — 적히지 않은 주문은 나가지 않는다. 재기동은 이 INTENT로 미결 주문을 안다. [why D-113]
    if (!route.freed && !take_intent(signal, route.order_reference))
    {
        mark_rejected(managed_order, "원장 저널 기록 실패 — 전송 생략");
        publish_order_result(signal, false);
        record_before_transport(managed_order);
        return false;
    }

    if (!route.freed)
    {
        route.intent_taken = true;
        ++kis_calls_; // 셈은 주문 스레드가 한다 — 주문 스레드가 이 수의 증가로 호출 여부를 가른다
    }

    managed_order.stages.journal_us = (trace::now_ns() - journal_started_ns) / 1000;
    return true;
}

// KIS로 보낸다. 버킷 대기는 스레드별 누계라 보내는 스레드에서 전후를 잰다. RTT는 채비 시각부터라 전송 스레드에
//  넘겨지기를 기다린 몫도 들어간다 — 그 대기도 접수 지연이다. [inv] kis_와 send 밖은 만지지 않는다. [why D-151]
void OrderRouter::send_new(NewOrderSend& send) const noexcept
{
    NewRoute&           route                 = send.route;
    const std::uint64_t bucket_wait_before_ns = kis_.rate_limit_wait_ns_this_thread();
    const int64_t       transport_started_ns  = trace::now_ns();

    try
    {
        route.acknowledgement = kis_.submit_order_acknowledgement(route.signal);
    }
    catch (const std::exception& exception)
    {
        route.transport_failed = true;
        route.transport_error  = exception.what();
    }
    catch (...)
    {
        route.transport_failed = true;
        route.transport_error  = "알 수 없는 예외";
    }

    route.rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - route.send_started)
                       .count();
    const std::uint64_t bucket_wait_ns = kis_.rate_limit_wait_ns_this_thread() - bucket_wait_before_ns;
    route.bucket_wait_ms               = static_cast<std::chrono::milliseconds::rep>(bucket_wait_ns / 1000000ULL);

    // 같은 대기를 us로도 남긴다 — ms로 자르면 버킷 대기가 0인지 0.9ms인지 구분이 안 된다.
    //  전송 시간은 버킷 줄서기를 뺀 몫이다. 뺀 값이 음수면(시계 해상도) 0으로 둔다.
    const int64_t bucket_wait_us               = static_cast<int64_t>(bucket_wait_ns / 1000ULL);
    route.managed_order.stages.bucket_wait_us = bucket_wait_us;
    route.managed_order.stages.transport_us =
        std::max<int64_t>(0, (trace::now_ns() - transport_started_ns) / 1000 - bucket_wait_us);
}

// 전송 예외는 접수 여부를 모른다. 선점을 풀고 REJECT를 적는다 — 실제로 접수됐다면 체결통보·잔고 대조가
//  원장을 되맞춘다(선점을 붙잡아 두면 그 종목이 하루 종일 막힌다). [why D-113]
void OrderRouter::close_transport_failure(NewRoute& route)
{
    const OrderSignal& signal        = route.signal;
    ManagedOrder&      managed_order = route.managed_order;

    mark_rejected(managed_order, "KIS 예외: " + route.transport_error);
    gate_.ledger().on_reject(signal.account_id, signal.ticker, signal.side, signal.quantity, route.order_reference,
                             managed_order.reject_reason);
    LOG_ERROR("[OrderRouter] KIS 예외 [" + managed_order.order_id + "] " + signal.ticker + " — " + route.transport_error);
    publish_order_result(signal, false);
    record_before_transport(managed_order);
}

// 3. 전송 뒤 마무리 — 접수 확정(원장 ACCEPT 기록)·발행·이력 저장·파일 넘기기. 여기부터가 record_us다.
//    왕복만 재고 끝내면 남은 시간이 어디로 갔는지 말할 수 없다 — 파일 쓰기가 여기서 드러나 쓰기 스레드로 옮겼다(D-123·D-124). [why D-117]
void OrderRouter::finalize_new_order(NewRoute& route)
{
    auto&              ledger          = gate_.ledger();
    const OrderSignal& signal          = route.signal;
    ManagedOrder&      managed_order   = route.managed_order;
    OrderAck&          acknowledgement = route.acknowledgement;
    const int64_t      record_started_ns = trace::now_ns();

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

        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal, route.order_reference, route.intent_taken);

        if (reconcile_acknowledgement.ok())
        {
            acknowledgement = std::move(reconcile_acknowledgement);
        }
    }

    const bool accepted = !acknowledgement.kis_order_no.empty();

    if (accepted)
    {
        managed_order.status                = OrderStatus::ACCEPTED;
        managed_order.recoverable           = true; // 이 프로세스가 낸 주문 — 끊긴 사이 체결을 조회로 되찾을 수 있다 [why D-149]
        managed_order.kis_order_no          = std::move(acknowledgement.kis_order_no);
        managed_order.kis_order_number      = digits_to_number(managed_order.kis_order_no); // 전문 문자열이 정수가 되는 자리
        managed_order.krx_forwarding_org_no = std::move(acknowledgement.krx_forwarding_org_no); // 정정/취소 시 원주문 조직번호로 재입력
        ++accepted_count_;
        // 선점은 전송 직전 INTENT에서 이미 잡혔다. 여기서는 원장에 ACCEPT(주문번호 확보)만 적는다 — 재기동
        //  리플레이가 "보냈고 접수됐다"를 "보냈는데 응답을 못 봤다"와 구분한다. [why D-113]
        route.order_reference.kis_order_number = managed_order.kis_order_number;
        ledger.on_accepted(signal.account_id, signal.ticker, signal.side, signal.quantity, route.order_reference);

        LOG_INFO(std::format("[OrderRouter] 접수 [{}] ODNO={} {} {} {}주 RTT={}ms 버킷대기={}ms", managed_order.order_id, managed_order.kis_order_no,
                             signal.ticker, signal.side == OrderSide::BUY ? "BUY" : "SELL", signal.quantity, route.rtt_ms, route.bucket_wait_ms));
    }
    else
    {
        mark_rejected(managed_order, "KIS API 거부 (빈 ODNO)" + kis_error_suffix(acknowledgement));

        if (route.intent_taken)
        {
            ledger.on_reject(signal.account_id, signal.ticker, signal.side, signal.quantity, route.order_reference,
                             managed_order.reject_reason);
        }

        // 거부도 같은 왕복을 치르므로 함께 남긴다(09-14 KIS 호출 797건 중 거부 279건).
        LOG_ERROR(std::format("[OrderRouter] KIS 거부 [{}] {}{} RTT={}ms 버킷대기={}ms", managed_order.order_id, signal.ticker,
                              managed_order.reject_reason, route.rtt_ms, route.bucket_wait_ms));

        // 전송 타임아웃은 거부가 아니라 '모름'이다. 기동 때만 되묻던 것으로는 부족했다 —
        //  2026-09-23 12:51 001120 매도 32주가 접수돼(ODNO=0000022490) 보유 전량을 묶었는데
        //  다음 기동까지 아무도 몰랐다. 그 자리에서 브로커에 되물어 맞춘다. [why D-101]
        if (acknowledgement.error_code == kis_error::kTransport)
        {
            reconcile_unknown_order_async(signal.ticker);
        }
    }

    const int64_t publish_started_ns = trace::now_ns();
    managed_order.stages.accept_us   = (publish_started_ns - record_started_ns) / 1000;
    publish_order_result(signal, accepted);
    managed_order.stages.publish_us = (trace::now_ns() - publish_started_ns) / 1000;

    // 이력 저장 몫 — 이력 잠금·미결주문 스냅숏·두 파일 넘기기. 접수 확정·발행과 갈라 둬야 다음에
    //  어디를 손댈지 고를 수 있다(회차 H에서 record 잔여가 1,096us였다). [why D-126]
    const int64_t history_store_started_ns = trace::now_ns();
    record(managed_order, &managed_order.stages.open_orders_us);
    managed_order.stages.history_store_us = (trace::now_ns() - history_store_started_ns) / 1000;
    managed_order.stages.record_us        = (trace::now_ns() - record_started_ns) / 1000;
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
        collect_session_sells(signal, opens);
    }
    else if (!fetch_open_orders(opens, "[OrderRouter] 미체결 조회 실패 — ", "[OrderRouter] 미체결 조회 예외 — "))
    {
        return OrderAck::fail(kis_error::kTransport);
    }

    int cancelled = 0;

    for (const auto& open : opens)
    {
        if (open.ticker != signal.ticker || open.side != OrderSide::SELL)
        {
            continue; // 해당 종목의 예약'매도'만 대상
        }

        if (cancel_blocking_sell(signal, open))
        {
            ++cancelled;
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

void OrderRouter::collect_session_sells(const OrderSignal& signal, std::vector<OpenOrder>& opens)
{
    std::lock_guard<std::mutex> lock(history_mutex_);

    for (const auto& managed_order : history_)
    {
        if (managed_order.status != OrderStatus::ACCEPTED || managed_order.signal.side != OrderSide::SELL ||
            managed_order.signal.symbol_id != signal.symbol_id || managed_order.kis_order_number == 0)
        {
            continue;
        }

        const int outstanding = outstanding_of(managed_order);

        if (outstanding <= 0)
        {
            continue;
        }

        OpenOrder open_order;
        open_order.ticker                = managed_order.signal.ticker;
        open_order.kis_order_no          = managed_order.kis_order_no;
        open_order.krx_forwarding_org_no = managed_order.krx_forwarding_org_no;
        open_order.psbl_qty              = outstanding;
        open_order.ord_unpr              = managed_order.signal.price;
        open_order.side                  = OrderSide::SELL;
        opens.push_back(std::move(open_order));
    }

    // 이전 세션이 남긴 미체결(부속 파일)도 후보다 — 기동 스윕이 아직 못 지웠거나 한도 거부로
    //  남긴 줄이 이 종목의 수량을 묶고 있을 수 있다. 취소되면 cancel_blocking_sell이 줄을 지운다.
    std::lock_guard<std::mutex> carry_lock(carry_mutex_);

    for (const auto& carry_row : carry_rows_)
    {
        if (carry_row[2] != signal.ticker || carry_row[3] != "SELL")
        {
            continue;
        }

        const std::optional<int> quantity = parse_quantity(carry_row[4]);

        if (!quantity)
        {
            continue;
        }

        OpenOrder open_order;
        open_order.ticker                = carry_row[2];
        open_order.kis_order_no          = carry_row[0];
        open_order.krx_forwarding_org_no = carry_row[1];
        open_order.psbl_qty              = *quantity;
        open_order.side                  = OrderSide::SELL;
        opens.push_back(std::move(open_order));
    }
}

bool OrderRouter::cancel_blocking_sell(const OrderSignal& signal, const OpenOrder& open)
{
    auto& ledger = gate_.ledger();

    LOG_WARN(std::format("[OrderRouter] 청산차단 해소 {} 예약매도 {}주 ODNO={} @{} → 취소 시도", signal.ticker,
                         open.psbl_qty, open.kis_order_no, static_cast<int>(open.ord_unpr)));
    OrderAck cancel;

    try
    {
        cancel = send_cancel(open.ticker, open.kis_order_no, open.krx_forwarding_org_no, open.psbl_qty);
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[OrderRouter] 예약취소 예외 " + signal.ticker + " — " + std::string(exception.what()));
        return false;
    }

    if (!cancel.ok())
    {
        return false;
    }

    // 이번 세션 주문이면 이력·선점을 같이 정리한다. 잠금 순서 history_mutex_ → 원장 positions_mutex_는 cancel_route와 같다.
    //  closed는 락 안에서 뜬 사본 — 락 밖의 원장 기록에 쓰고, history_ 원소는 축출로 참조가 죽을 수 있다.
    ManagedOrder closed;
    bool         found   = false;
    int          release = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* managed_order = history_.find_by_order_number(digits_to_number(open.kis_order_no));

        if (managed_order && managed_order->status == OrderStatus::ACCEPTED)
        {
            release                      = outstanding_of(*managed_order);
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
        journal_.write_trade_row("", closed, 0, 0.0);
    }
    else
    {
        // 이전 세션 줄 — 부속 파일에서 빼고, 취소로 풀린 수량을 원장 매도가능수량에 되돌린다(기동 취소와 같은 처리).
        if (erase_carry_row(open.kis_order_no))
        {
            rewrite_open_orders();
        }

        ledger.restore_sellable(signal.account_id, signal.ticker, open.psbl_qty);
    }

    return true;
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
        history_.push(managed_order);
        history_.evict_to(config_.max_history); // ACCEPTED(체결 대기 중) 주문은 남긴다

        open_orders = snapshot_open_orders_locked();
        sequence         = ++open_orders_sequence_;
    }

    // 파일 I/O는 history_mutex_ 밖에서 — 디스크가 느린 순간 체결(on_fill)·발주(submit)가 같이 밀리지 않게(W-8).
    // 거래 원장 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → managed_order.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    journal_.write_trade_row("", managed_order, 0, 0.0);

    const int64_t open_orders_started_ns = trace::now_ns();
    journal_.queue_open_orders_file(std::move(open_orders), sequence);

    if (open_orders_us != nullptr)
    {
        *open_orders_us = (trace::now_ns() - open_orders_started_ns) / 1000;
    }

    journal_.append_order_reason(managed_order);
    return (trace::now_ns() - started_ns) / 1000;
}

// ─── 취소·정정 공통 — 원주문 스냅샷과 닫기 ────────────────────────────────
bool OrderRouter::snapshot_live_original_locked(uint64_t client_order_number, OriginalOrder& original)
{
    const ManagedOrder* live = history_.find_live(client_order_number);

    if (!live)
    {
        return false;
    }

    original.ticker                = live->signal.ticker;
    original.kis_order_no          = live->kis_order_no;
    original.krx_forwarding_org_no = live->krx_forwarding_org_no;
    original.account               = live->signal.account_id;
    original.side                  = live->signal.side;
    original.outstanding           = outstanding_of(*live);
    return true;
}

void OrderRouter::close_live_original_locked(const OrderSignal& signal, const OriginalOrder& original, int release_if_gone)
{
    ManagedOrder* live    = history_.find_live(signal.original_client_order_number);
    int           release = release_if_gone;

    if (live)
    {
        release          = outstanding_of(*live); // 접수된 시점 실제 미체결
        live->status     = OrderStatus::CANCELLED;
        live->updated_at = std::chrono::system_clock::now();
    }

    // 원장 positions_mutex_는 history_mutex_와 별개다. 잠금 순서 history_mutex_ → positions_mutex_는 on_fill과 같다(데드락 없음).
    if (release > 0)
    {
        gate_.ledger().on_cancel(original.account, original.ticker, original.side, release,
                                 OrderGate::OrderRef{live ? digits_to_number(live->order_id) : 0,
                                                     digits_to_number(original.kis_order_no), signal.type});
    }
}

// history_.find_live는 살아있는 주문만 보므로 원주문이 없다는 것은 세 경우를 뭉뚱그린다 —
//  체결됨 / 이미 취소됨 / 애초에 접수된 적 없음(REJECTED·이력 없음). 중복 매수 위험은 첫째에만 있다. [why D-035]
bool OrderRouter::classify_missing_original_locked(uint64_t client_order_number, const char*& gone_why)
{
    const ManagedOrder* history_entry = history_.find_by_client_number(client_order_number);

    if (!history_entry)
    {
        return false;
    }

    if (history_entry->status == OrderStatus::FILLED ||
        (history_entry->status == OrderStatus::ACCEPTED && history_entry->confirmed_quantity > 0))
    {
        gone_why = "이미 체결";
        return true;
    }

    if (history_entry->status == OrderStatus::CANCELLED)
    {
        gone_why = "이미 취소";
    }
    else if (history_entry->status == OrderStatus::REJECTED)
    {
        gone_why = "접수된 적 없음(거부)";
    }

    return false;
}

// 취소할 것이 없는 것은 거부가 아니라 끝난 상태다 — 전략은 취소 결과를 안 보고 계획을
//  다시 짜므로, 거부된 분할 단계·이미 취소된 분할 단계를 다시 취소하는 요청이 재구성마다 온다
//  (09-10~11 이틀 323건, 그중 체결 흔적은 21건). REJECTED로 세면 거부 통계와 경고가
//  실제 문제(게이트·KIS 거부)를 덮는다. CANCELLED로 닫고, 체결 가능성이 있는 경우만
//  경고와 매수 보류를 남긴다. [why D-035]
void OrderRouter::close_cancel_without_target(ManagedOrder& managed_order, const OrderSignal& signal,
                                              bool original_may_have_filled, const char* gone_why)
{
    managed_order.status        = OrderStatus::CANCELLED;
    managed_order.reject_reason = std::string("취소 대상 없음 (") + gone_why + ") oid=" + signal.original_client_order_id;

    if (!original_may_have_filled)
    {
        LOG_INFO("[OrderRouter] 취소 불요 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        return;
    }

    // 체결 흔적이 있을 때만 매수를 잠근다. 접수된 적 없는 order_id(전략이 거부된 주문을
    //  live로 들고 있는 경우)에도 잠그면 매 재구성 주기마다 취소 빗나감 → 매수 거부 →
    //  거부된 oid가 다시 live로 → 다음 주기에 또 취소 빗나감으로 되돌아, 창이 계속
    //  갱신되며 그 종목 매수가 영구히 막힌다. 2026-09-10 006910이 이 모양으로
    //  보유 0인 채 57분간 한 주도 못 샀다(대체 주문 보류 176건). [why D-035]
    //  이미 무장돼 있으면 시각을 갱신하지 않는다 — 창은 연장되지 않는다.
    {
        const symbol::SymbolId      symbol = symbol_of(signal);
        std::lock_guard<std::mutex> lock(history_mutex_);

        if (symbol >= cancel_miss_.size())
        {
            cancel_miss_.resize(std::max(gate_.ledger().symbols().capacity(), static_cast<size_t>(symbol) + 1));
        }

        if (cancel_miss_[symbol] == std::chrono::steady_clock::time_point{})
        {
            cancel_miss_[symbol] = std::chrono::steady_clock::now();
        }
    }

    LOG_WARN("[OrderRouter] 취소 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason +
             " — 체결 가능성 있어 신규매수 보류");
}

// ─── 취소 라우팅 (action=CANCEL) ──────────────────────────────────────────
//  1) original_client_order_number로 live 주문 조회 → 원 ODNO/조직번호/미체결 잔량 스냅샷
//  2) lock 밖에서 KIS 취소 호출(네트워크)
//  3) 성공 시에만 lock 재획득 → 미체결 잔량을 '그 시점 confirmed_quantity로 재계산'해 reserved 해제
//     (2)와 (3) 사이 체결 스레드의 on_fill이 confirmed_quantity를 올릴 수 있으므로 재계산이 이중해제를 막는다.
ManagedOrder OrderRouter::cancel_route(const OrderSignal& signal)
{
    ManagedOrder managed_order = make_pending_order(signal, std::chrono::system_clock::now());

    // 1) 원주문 스냅샷 (record()는 history_mutex_를 재획득하므로 lock 스코프 밖에서만 호출)
    //  취소가 빗나갔을 때 "원주문이 이미 체결됐을 수 있나"를 같은 락 안에서 답해 둔다. [why D-035]
    OriginalOrder original;
    bool          found                    = false;
    bool          original_may_have_filled = false;
    const char*   gone_why                 = "이력 없음(재기동·이력초과)";
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        found = snapshot_live_original_locked(signal.original_client_order_number, original);

        if (!found)
        {
            original_may_have_filled = classify_missing_original_locked(signal.original_client_order_number, gone_why);
        }
    }

    if (!found)
    {
        close_cancel_without_target(managed_order, signal, original_may_have_filled, gone_why);
        record(managed_order);
        return managed_order;
    }

    // 2) KIS 취소 (lock 밖)
    OrderAck cancel;

    try
    {
        cancel = send_cancel(original.ticker, original.kis_order_no, original.krx_forwarding_org_no, original.outstanding);
    }
    catch (const std::exception& exception)
    {
        mark_rejected(managed_order, std::string("KIS 취소 예외: ") + exception.what());
        LOG_ERROR("[OrderRouter] 취소 예외 [" + managed_order.order_id + "] " + original.ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!cancel.ok())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        mark_rejected(managed_order, "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(cancel));
        LOG_WARN("[OrderRouter] 취소 거부 [" + managed_order.order_id + "] " + original.ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 3) 성공 — reserved 해제(잔량 재계산) + 원주문 CANCELLED 표기 + 인덱스 정리
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        close_live_original_locked(signal, original, 0);
    }

    managed_order.status           = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    managed_order.kis_order_no     = std::move(cancel.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.updated_at       = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + managed_order.order_id + "] " + original.ticker +
             " 원oid=" + signal.original_client_order_id + " 취소ODNO=" + managed_order.kis_order_no);
    record(managed_order);
    return managed_order;
}

// ─── 정정 라우팅 (action=REPLACE) ─────────────────────────────────────────
//  KIS 정정 1콜 = cancel-replace. 성공 시 새 ODNO 발급.
//  근거 없음(2026-09-27): 공식 샘플 order_rvsecncl은 정정을 '단가·주문구분 변경, 수량은 원주문 이하'로만 적고 응답 컬럼
//  설명이 없다(2026-09-27 MCP 확인). 'cancel-replace'와 '새 ODNO 발급'을 적은 샘플·실측 기록은 찾지 못했다.
//  reserved 조정: new_quantity는 전송 전 INTENT에서 선점하고, 접수되면 원주문 미체결 잔량을 해제한다(같은 side). 원주문은 CANCELLED,
//  정정 결과를 새 ManagedOrder(ACCEPTED)로 추적(새 ODNO/새 client_order_id).
//  ⚠ 첫 컷 한계: 부분체결 상태 정정은 수량 정합이 복잡 → MM은 REPLACE 미사용(CANCEL+NEW 사용).
//     본 경로는 미체결 전량 대상 정정만 안전. 부분체결분 정정은 Phase 2에서 정밀화.
ManagedOrder OrderRouter::replace_route(const OrderSignal& signal)
{
    auto&        ledger        = gate_.ledger();
    ManagedOrder managed_order = make_pending_order(signal, std::chrono::system_clock::now());

    OriginalOrder original;
    bool          found = false;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        found = snapshot_live_original_locked(signal.original_client_order_number, original);
    }

    if (!found)
    {
        mark_rejected(managed_order, "정정 대상 없음 oid=" + signal.original_client_order_id);
        LOG_WARN("[OrderRouter] 정정 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        record(managed_order);
        return managed_order;
    }

    const int new_quantity = (signal.quantity > 0) ? signal.quantity : original.outstanding;

    // 정정도 전송 전에 원장에 적는다 — 새 수량을 INTENT로 선점하고, 원주문 잔량은 접수된 뒤에 푼다.
    //  못 적으면 보내지 않는다(적히지 않은 주문은 나가지 않는다). [why D-113]
    OrderSignal reserve_signal = signal;
    reserve_signal.ticker      = original.ticker;
    reserve_signal.account_id  = original.account;
    reserve_signal.side        = original.side;
    reserve_signal.quantity    = new_quantity;
    const OrderGate::OrderRef order_reference{digits_to_number(managed_order.order_id), 0, signal.type};
    const InFlightMark        in_flight{*this, ledger.intern_symbol(original.ticker)};

    if (!take_intent(reserve_signal, order_reference))
    {
        mark_rejected(managed_order, "원장 저널 기록 실패 — 정정 전송 생략");
        record(managed_order);
        return managed_order;
    }

    OrderAck revise_acknowledgement;

    try
    {
        ++kis_calls_;
        revise_acknowledgement = kis_.revise_order(original.ticker, original.kis_order_no, original.krx_forwarding_org_no,
                                                   new_quantity, signal.price);
    }
    catch (const std::exception& exception)
    {
        mark_rejected(managed_order, std::string("KIS 정정 예외: ") + exception.what());
        ledger.on_reject(original.account, original.ticker, original.side, new_quantity, order_reference,
                         managed_order.reject_reason);
        LOG_ERROR("[OrderRouter] 정정 예외 [" + managed_order.order_id + "] " + original.ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!revise_acknowledgement.ok())
    {
        mark_rejected(managed_order, "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(revise_acknowledgement));
        ledger.on_reject(original.account, original.ticker, original.side, new_quantity, order_reference,
                         managed_order.reject_reason);
        LOG_WARN("[OrderRouter] 정정 거부 [" + managed_order.order_id + "] " + original.ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 성공 — 원 미체결 잔량 해제, 원주문 CANCELLED, 정정본 ACCEPTED 추적(선점은 INTENT에서 이미 잡혔다)
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        close_live_original_locked(signal, original, original.outstanding);

        // 정정본 선점은 위 INTENT에서 이미 잡혔다 — 여기서는 새 주문번호로 ACCEPT만 적는다.
        ledger.on_accepted(original.account, original.ticker, original.side, new_quantity,
                           OrderGate::OrderRef{digits_to_number(managed_order.order_id),
                                               digits_to_number(revise_acknowledgement.kis_order_no), signal.type});
    }

    managed_order.status                = OrderStatus::ACCEPTED;
    managed_order.recoverable           = true; // 정정본은 새 주문번호라 누적 체결이 0에서 시작한다 [why D-149]
    managed_order.kis_order_no          = std::move(revise_acknowledgement.kis_order_no);
    managed_order.kis_order_number      = digits_to_number(managed_order.kis_order_no);
    managed_order.krx_forwarding_org_no = std::move(original.krx_forwarding_org_no); // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    managed_order.signal.side           = original.side; // NONE 방지: 원주문 side 승계
    managed_order.updated_at            = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + managed_order.order_id + "] " + original.ticker +
             " 원oid=" + signal.original_client_order_id + " 새ODNO=" + managed_order.kis_order_no +
             std::format(" qty={} @{}", new_quantity, static_cast<int>(signal.price)));
    record(managed_order);
    return managed_order;
}
