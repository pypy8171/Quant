// 주문 라우터 — 신규 주문 경로(열기·전송·닫기), 발주 경로 공통 조각, 이력 저장.
//  취소·정정은 OrderRouterModify.cpp, 청산차단 자가정리는 OrderRouterBlockedSell.cpp.
#include "ipc/OrderRouter.h"
#include "ipc/OrderRouterSendSteps.h"
#include "ipc/SideLabel.h"
#include "api/KisErrorCodes.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>

#include <format>
#include <string_view>
#include <vector>

using order_router::capture_transport;
using order_router::close_then_finish;

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

// 계측 구간(ns)을 단계 칸 단위(us)로 바꾼다. 나머지는 버린다.
static constexpr int64_t kNanosecondsPerMicrosecond = 1000;

static constexpr int64_t to_microseconds(int64_t nanoseconds)
{
    return nanoseconds / kNanosecondsPerMicrosecond;
}

void order_router::NewRoute::stamp_gate_stages()
{
    managed_order.stages.gate_us              = to_microseconds(trace::now_ns() - entered_ns - history_guard_ns);
    managed_order.stages.history_guard_us     = to_microseconds(history_guard_ns);
    managed_order.stages.history_lock_wait_us = to_microseconds(history_lock_wait_ns);
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
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다. 전송 스레드는 send_new·send_modify만
//  부른다(게이트·장부·이력은 안 만진다). [why D-151]
ManagedOrder OrderRouter::submit(const OrderSignal& signal)
{
    switch (signal.action)
    {
    case OrderAction::CANCEL:
    case OrderAction::REPLACE: return modify_route(signal);
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

    route.order_reference = OrderGate::OrderRef{order_number_of(route.managed_order.order_id), 0, route.signal.type};
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

    // ODNO가 이력에 들어간 뒤 수를 내리므로 붙든 체결 중 이 주문 것은 그때 연결된다.
    close_then_finish(
        [this, &route]
        {
            if (route.transport_failed)
            {
                close_transport_failure(route);
            }
            else
            {
                finalize_new_order(route);
            }
        },
        [this]
        {
            finish_sending_new();
        });
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
        LOG_WARN(std::format("[OrderRouter] 매도가능 {}/{}주 {} — 장부 보유 {}주, 잔고 주문가능 {}주, 이 세션 미체결 매도 {}주 → 예약매도 취소 시도",
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

    // 장부 먼저, 전송은 그 다음 — 적히지 않은 주문은 나가지 않는다. 재기동은 이 INTENT로 미결 주문을 안다. [why D-113]
    if (!route.freed && !take_intent(signal, route.order_reference))
    {
        mark_rejected(managed_order, "장부 저널 기록 실패 — 전송 생략");
        publish_order_result(signal, false);
        record_before_transport(managed_order);
        return false;
    }

    if (!route.freed)
    {
        route.intent_taken = true;
        ++kis_calls_; // 셈은 주문 스레드가 한다 — 주문 스레드가 이 수의 증가로 호출 여부를 가른다
    }

    managed_order.stages.journal_us = to_microseconds(trace::now_ns() - journal_started_ns);
    return true;
}

// KIS로 보낸다. 버킷 대기는 스레드별 누계라 보내는 스레드에서 전후를 잰다. RTT는 채비 시각부터라 전송 스레드에
//  넘겨지기를 기다린 몫도 들어간다 — 그 대기도 접수 지연이다. [inv] kis_와 send 밖은 만지지 않는다. [why D-151]
void OrderRouter::send_new(NewOrderSend& send) const noexcept
{
    NewRoute&           route                 = send.route;
    const std::uint64_t bucket_wait_before_ns = kis_.rate_limit_wait_ns_this_thread();
    const int64_t       transport_started_ns  = trace::now_ns();

    capture_transport(route, [this, &route]
    {
        route.acknowledgement = kis_.submit_order_acknowledgement(route.signal);
    });

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
        std::max<int64_t>(0, to_microseconds(trace::now_ns() - transport_started_ns) - bucket_wait_us);
}

// 전송 예외는 접수 여부를 모른다. 선점을 풀고 REJECT를 적는다 — 실제로 접수됐다면 체결통보·잔고 대조가
//  장부를 되맞춘다(선점을 붙잡아 두면 그 종목이 하루 종일 막힌다). [why D-113]
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

// 3. 전송 뒤 마무리 — 접수 확정(장부 ACCEPT 기록)·발행·이력 저장·파일 넘기기. 여기부터가 record_us다.
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
            // 게이트는 장부가 모르는 종목을 자르지 않고 KIS에 넘긴다 — 그 거부가 여기로 온다.
            LOG_WARN("[OrderRouter] 보유수량 0(장부 기준) " + signal.ticker + " — 매도 불가, KIS도 주문가능분 없음으로 거부");
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
        // 선점은 전송 직전 INTENT에서 이미 잡혔다. 여기서는 장부에 ACCEPT(주문번호 확보)만 적는다 — 재기동
        //  리플레이가 "보냈고 접수됐다"를 "보냈는데 응답을 못 봤다"와 구분한다. [why D-113]
        route.order_reference.kis_order_number = managed_order.kis_order_number;
        ledger.on_accepted(signal.account_id, signal.ticker, signal.side, signal.quantity, route.order_reference);

        LOG_INFO(std::format("[OrderRouter] 접수 [{}] ODNO={} {} {} {}주 RTT={}ms 버킷대기={}ms", managed_order.order_id, managed_order.kis_order_no,
                             signal.ticker, side_label(signal.side), signal.quantity, route.rtt_ms, route.bucket_wait_ms));
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

        // 전송 타임아웃은 거부가 아니라 '모름'이다. 기동 때만 다시 조회하던 것으로는 부족했다 —
        //  2026-09-23 12:51 001120 매도 32주가 접수돼(ODNO=0000022490) 보유 전량을 묶었는데
        //  다음 기동까지 아무도 몰랐다. 그 자리에서 브로커에 다시 조회해 맞춘다. [why D-101]
        if (acknowledgement.error_code == kis_error::kTransport)
        {
            reconcile_unknown_order_async(signal.ticker);
        }
    }

    const int64_t publish_started_ns = trace::now_ns();
    managed_order.stages.accept_us   = to_microseconds(publish_started_ns - record_started_ns);
    publish_order_result(signal, accepted);
    managed_order.stages.publish_us = to_microseconds(trace::now_ns() - publish_started_ns);

    // 이력 저장 몫 — 이력 잠금·미결주문 스냅숏·두 파일 넘기기. 접수 확정·발행과 갈라 둬야 다음에
    //  어디를 손댈지 고를 수 있다(회차 H에서 record 잔여가 1,096us였다). [why D-126]
    const int64_t history_store_started_ns = trace::now_ns();
    record(managed_order, &managed_order.stages.open_orders_us);
    managed_order.stages.history_store_us = to_microseconds(trace::now_ns() - history_store_started_ns);
    managed_order.stages.record_us        = to_microseconds(trace::now_ns() - record_started_ns);
}

// ─── 전송 직전 장부 기록 ────────────────────────────────────────────────────
//  선점가는 지정가=price, 시장가(0)=reference_price로 근사 stamp → §3d 총노출이 시장가 선점을 과소평가하지
//  않게(check()의 평가가와 대칭, 보수측). 거짓이면 선점도 되돌려져 있다. [why D-113]
bool OrderRouter::take_intent(const OrderSignal& signal, const OrderGate::OrderRef& reference)
{
    if (gate_.ledger().on_intent(signal.account_id, signal.ticker, signal.side, signal.quantity,
                        signal.price > 0.0 ? signal.price : signal.reference_price, reference, signal.strategy_index))
    {
        return true;
    }

    LOG_ERROR("[OrderRouter] 장부 저널 기록 실패 — 주문을 보내지 않는다: " + signal.ticker + " " +
              std::to_string(signal.quantity) + "주");
    return false;
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
    // 거래 장부 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → managed_order.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    journal_.write_trade_row("", managed_order, 0, 0.0);

    const int64_t open_orders_started_ns = trace::now_ns();
    journal_.queue_open_orders_file(std::move(open_orders), sequence);

    if (open_orders_us != nullptr)
    {
        *open_orders_us = to_microseconds(trace::now_ns() - open_orders_started_ns);
    }

    journal_.append_order_reason(managed_order);
    return to_microseconds(trace::now_ns() - started_ns);
}
