// 주문 라우터 — 취소·정정 경로(열기·전송·닫기)와 원주문 사본·닫기. submit이 action=CANCEL·REPLACE일 때 부른다.
#include "ipc/OrderRouter.h"
#include "ipc/OrderRouterSendSteps.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <optional>

using order_router::capture_transport;
using order_router::close_then_finish;

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

OrderRouter::OriginalRelease OrderRouter::close_live_original_locked(const OrderSignal& signal, int release_if_gone)
{
    ManagedOrder*   live = history_.find_live(signal.original_client_order_number);
    OriginalRelease release;
    release.quantity = release_if_gone;

    if (live)
    {
        release.quantity     = outstanding_of(*live); // 접수된 시점 실제 미체결
        release.order_number = order_number_of(live->order_id);
        live->status         = OrderStatus::CANCELLED;
        live->updated_at     = std::chrono::system_clock::now();
    }

    return release;
}

// 장부 기록은 history_mutex_ 밖에서 한다 — on_cancel은 저널을 디스크에 쓴다(CODE_CONVENTIONS 4.5). 해제량은 락 안에서
//  정했고, 장부의 선점 빼기는 0에서 멈추는 빼기라 그사이 같은 주문의 체결이 장부에 먼저 들어가도 결과가 같다.
void OrderRouter::release_original(const OrderSignal& signal, const OriginalOrder& original, const OriginalRelease& release)
{
    if (release.quantity <= 0)
    {
        return;
    }

    gate_.ledger().on_cancel(original.account, original.ticker, original.side, release.quantity,
                             OrderGate::OrderRef{release.order_number, digits_to_number(original.kis_order_no),
                                                 signal.type});
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

// ─── 취소·정정 라우팅 (action=CANCEL·REPLACE) ─────────────────────────────
//  열기 → 전송 → 닫기를 한 스레드에서 잇는다. 전송 스레드를 쓰는 주문 스레드는 셋을 따로 부른다. [why D-151]
//  1) 열기: original_client_order_number로 live 주문 조회 → 원 ODNO/조직번호/미체결 잔량 스냅샷
//  2) 전송: lock 밖에서 KIS 취소·정정 호출(네트워크)
//  3) 닫기: 성공 시에만 lock 재획득 → 미체결 잔량을 '그 시점 confirmed_quantity로 재계산'해 reserved 해제
//     (2)와 (3) 사이 체결 스레드의 on_fill이 confirmed_quantity를 올릴 수 있으므로 재계산이 이중해제를 막는다.
ManagedOrder OrderRouter::modify_route(const OrderSignal& signal)
{
    auto opened = open_modify(signal);

    if (auto* finished = std::get_if<ManagedOrder>(&opened))
    {
        return std::move(*finished);
    }

    auto& send = std::get<ModifyOrderSend>(opened);
    send_modify(send);
    return close_modify(std::move(send));
}

std::variant<ManagedOrder, OrderRouter::ModifyOrderSend> OrderRouter::open_modify(const OrderSignal& signal)
{
    const bool  is_cancel = signal.action == OrderAction::CANCEL;
    ModifyRoute route;
    route.signal                = signal;
    route.managed_order         = make_pending_order(signal, std::chrono::system_clock::now());
    ManagedOrder&  managed_order = route.managed_order;
    OriginalOrder& original      = route.original;

    // 원주문 스냅샷 (record()는 history_mutex_를 재획득하므로 lock 스코프 밖에서만 호출)
    //  취소가 빗나갔을 때 "원주문이 이미 체결됐을 수 있나"를 같은 락 안에서 답해 둔다. [why D-035]
    bool        found                    = false;
    bool        original_may_have_filled = false;
    const char* gone_why                 = "이력 없음(재기동·이력초과)";
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        found = snapshot_live_original_locked(signal.original_client_order_number, original);

        if (!found && is_cancel)
        {
            original_may_have_filled = classify_missing_original_locked(signal.original_client_order_number, gone_why);
        }
    }

    if (!found)
    {
        if (is_cancel)
        {
            close_cancel_without_target(managed_order, signal, original_may_have_filled, gone_why);
        }
        else
        {
            mark_rejected(managed_order, "정정 대상 없음 oid=" + signal.original_client_order_id);
            LOG_WARN("[OrderRouter] 정정 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        }

        record(managed_order);
        return std::move(managed_order);
    }

    if (is_cancel)
    {
        ++kis_calls_; // 셈은 주문 스레드가 한다 — 주문 스레드가 이 수의 증가로 호출 여부를 가른다
        return ModifyOrderSend{std::move(route), nullptr};
    }

    route.new_quantity = (signal.quantity > 0) ? signal.quantity : original.outstanding;

    // 정정도 전송 전에 장부에 적는다 — 새 수량을 INTENT로 선점하고, 원주문 잔량은 접수된 뒤에 푼다.
    //  못 적으면 보내지 않는다(적히지 않은 주문은 나가지 않는다). [why D-113]
    OrderSignal reserve_signal = signal;
    reserve_signal.ticker      = original.ticker;
    reserve_signal.account_id  = original.account;
    reserve_signal.side        = original.side;
    reserve_signal.quantity    = route.new_quantity;
    route.order_reference      = OrderGate::OrderRef{order_number_of(managed_order.order_id), 0, signal.type};
    // close_modify의 이력 기록까지 이 종목의 선점을 정리가 풀지 못하게 건다. [why D-113]
    auto in_flight = std::make_unique<InFlightMark>(*this, gate_.ledger().intern_symbol(original.ticker));

    if (!take_intent(reserve_signal, route.order_reference))
    {
        mark_rejected(managed_order, "장부 저널 기록 실패 — 정정 전송 생략");
        record(managed_order);
        return std::move(managed_order);
    }

    // 정정은 새 ODNO를 받는다 — close_modify까지 그 사이 온 체결은 on_fill이 붙들어 둔다. [why D-151]
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ++sending_new_orders_;
    }

    ++kis_calls_;
    return ModifyOrderSend{std::move(route), std::move(in_flight)};
}

// KIS 정정 1콜 = cancel-replace. 성공 시 새 ODNO 발급.
//  근거 없음(2026-09-27): 공식 샘플 order_rvsecncl은 정정을 '단가·주문구분 변경, 수량은 원주문 이하'로만 적고 응답 컬럼
//  설명이 없다(2026-09-27 MCP 확인). 'cancel-replace'와 '새 ODNO 발급'을 적은 샘플·실측 기록은 찾지 못했다.
//  [inv] kis_와 send 밖은 만지지 않는다. [why D-151]
void OrderRouter::send_modify(ModifyOrderSend& send) const noexcept
{
    ModifyRoute&         route    = send.route;
    const OriginalOrder& original = route.original;

    capture_transport(route, [this, &route, &original]
    {
        if (route.signal.action == OrderAction::CANCEL)
        {
            route.acknowledgement = kis_.cancel_order(original.ticker, original.kis_order_no,
                                                      original.krx_forwarding_org_no, original.outstanding,
                                                      /*all_remaining=*/true);
        }
        else
        {
            route.acknowledgement = kis_.revise_order(original.ticker, original.kis_order_no,
                                                      original.krx_forwarding_org_no, route.new_quantity,
                                                      route.signal.price);
        }
    });
}

// 닫기 — 표시는 이력에 적은 뒤 풀리도록 지역으로 옮겨 둔다(지역 변수는 선언 역순으로 소멸).
ManagedOrder OrderRouter::close_modify(ModifyOrderSend&& send)
{
    const std::unique_ptr<InFlightMark> in_flight = std::move(send.in_flight);
    ModifyRoute&                        route     = send.route;

    if (route.signal.action == OrderAction::CANCEL)
    {
        close_cancel(route);
        return std::move(route.managed_order);
    }

    // 새 ODNO가 이력에 들어간 뒤 수를 내리므로 붙든 체결 중 이 정정본 것은 그때 연결된다.
    close_then_finish(
        [this, &route]
        {
            close_replace(route);
        },
        [this]
        {
            finish_sending_new();
        });
    return std::move(route.managed_order);
}

void OrderRouter::close_cancel(ModifyRoute& route)
{
    const OrderSignal& signal        = route.signal;
    ManagedOrder&      managed_order = route.managed_order;
    OriginalOrder&     original      = route.original;

    if (route.transport_failed)
    {
        mark_rejected(managed_order, "KIS 취소 예외: " + route.transport_error);
        LOG_ERROR("[OrderRouter] 취소 예외 [" + managed_order.order_id + "] " + original.ticker + " — " +
                  route.transport_error);
        record(managed_order);
        return;
    }

    if (!route.acknowledgement.ok())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        mark_rejected(managed_order, "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(route.acknowledgement));
        LOG_WARN("[OrderRouter] 취소 거부 [" + managed_order.order_id + "] " + original.ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return;
    }

    // 성공 — 원주문 CANCELLED 표기(락 안) + reserved 해제(잔량 재계산, 락 밖). handoff는 장부 기록까지 선점 정리를 미룬다.
    {
        std::optional<LedgerHandoff> handoff;
        OriginalRelease              release;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            release = close_live_original_locked(signal, 0);
            handoff.emplace(*this);
        }

        release_original(signal, original, release);
    }

    managed_order.status           = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    managed_order.kis_order_no     = std::move(route.acknowledgement.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.updated_at       = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + managed_order.order_id + "] " + original.ticker +
             " 원oid=" + signal.original_client_order_id + " 취소ODNO=" + managed_order.kis_order_no);
    record(managed_order);
}

//  reserved 조정: new_quantity는 전송 전 INTENT에서 선점하고, 접수되면 원주문 미체결 잔량을 해제한다(같은 side). 원주문은 CANCELLED,
//  정정 결과를 새 ManagedOrder(ACCEPTED)로 추적(새 ODNO/새 client_order_id).
//  ⚠ 첫 컷 한계: 부분체결 상태 정정은 수량 정합이 복잡 → MM은 REPLACE 미사용(CANCEL+NEW 사용).
//     본 경로는 미체결 전량 대상 정정만 안전. 부분체결분 정정은 Phase 2에서 정밀화.
void OrderRouter::close_replace(ModifyRoute& route)
{
    auto&              ledger        = gate_.ledger();
    const OrderSignal& signal        = route.signal;
    ManagedOrder&      managed_order = route.managed_order;
    OriginalOrder&     original      = route.original;
    const int          new_quantity  = route.new_quantity;

    if (route.transport_failed || !route.acknowledgement.ok())
    {
        if (route.transport_failed)
        {
            mark_rejected(managed_order, "KIS 정정 예외: " + route.transport_error);
            LOG_ERROR("[OrderRouter] 정정 예외 [" + managed_order.order_id + "] " + original.ticker + " — " +
                      route.transport_error);
        }
        else
        {
            mark_rejected(managed_order,
                          "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(route.acknowledgement));
            LOG_WARN("[OrderRouter] 정정 거부 [" + managed_order.order_id + "] " + original.ticker +
                     " 원oid=" + signal.original_client_order_id);
        }

        ledger.on_reject(original.account, original.ticker, original.side, new_quantity, route.order_reference,
                         managed_order.reject_reason);
        record(managed_order);
        return;
    }

    // 성공 — 원 미체결 잔량 해제, 원주문 CANCELLED, 정정본 ACCEPTED 추적(선점은 INTENT에서 이미 잡혔다).
    //  이력은 락 안에서 닫고, 장부 기록(디스크 쓰기)은 락을 푼 뒤에 한다. handoff는 장부 기록까지 선점 정리를 미룬다.
    {
        std::optional<LedgerHandoff> handoff;
        OriginalRelease              release;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            release = close_live_original_locked(signal, original.outstanding);
            handoff.emplace(*this);
        }

        release_original(signal, original, release);

        // 정정본 선점은 위 INTENT에서 이미 잡혔다 — 여기서는 새 주문번호로 ACCEPT만 적는다. 정정본은 아직 이력에 없고
        //  open_modify가 건 종목 표시가 record() 뒤까지 살아 있어 정리가 그 선점을 풀지 않는다.
        ledger.on_accepted(original.account, original.ticker, original.side, new_quantity,
                           OrderGate::OrderRef{order_number_of(managed_order.order_id),
                                               digits_to_number(route.acknowledgement.kis_order_no), signal.type});
    }

    managed_order.status                = OrderStatus::ACCEPTED;
    managed_order.recoverable           = true; // 정정본은 새 주문번호라 누적 체결이 0에서 시작한다 [why D-149]
    managed_order.kis_order_no          = std::move(route.acknowledgement.kis_order_no);
    managed_order.kis_order_number      = digits_to_number(managed_order.kis_order_no);
    managed_order.krx_forwarding_org_no = std::move(original.krx_forwarding_org_no); // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    managed_order.signal.side           = original.side; // NONE 방지: 원주문 side 승계
    managed_order.updated_at            = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + managed_order.order_id + "] " + original.ticker +
             " 원oid=" + signal.original_client_order_id + " 새ODNO=" + managed_order.kis_order_no +
             std::format(" qty={} @{}", new_quantity, static_cast<int>(signal.price)));
    record(managed_order);
}
