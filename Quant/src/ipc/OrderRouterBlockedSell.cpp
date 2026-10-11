// 주문 라우터 — 청산차단 자가정리. 매도가 주문가능분 없음(40240000)으로 막히면 그 종목의 예약매도를 찾아 취소하고
//  시장가 매도를 한 번 다시 낸다. 발주 경로는 OrderRouterSubmit.cpp가 부른다.
#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

// 이번 세션에 낸 매도가 그 종목에 아직 살아 있는가 — 접수됐고, 주문번호가 있고, 미체결 잔량이 남았다.
static bool is_open_session_sell(const ManagedOrder& managed_order, symbol::SymbolId symbol_id)
{
    return managed_order.status == OrderStatus::ACCEPTED && managed_order.signal.side == OrderSide::SELL &&
           managed_order.signal.symbol_id == symbol_id && managed_order.kis_order_number != 0 &&
           outstanding_of(managed_order) > 0;
}

// ─── 청산차단 자가정리 — 예약매도 취소 후 시장가 재매도 (장중) ─────────────
//  전제: SELL이 40240000(주문가능분 없음)으로 막힌 직후 호출. 그 종목의 미체결 예약매도가
//  보유수량을 묶어 ord_psbl_qty=0이 된 상황을 KIS 미체결 조회로 규명하고, 예약을 취소해
//  수량을 풀어준 뒤 시장가 매도를 1회 재시도한다. 취소한 예약이 이번 세션 주문(history_에
//  ODNO가 있음)이면 CANCELLED로 닫고 장부(PositionLedger) 선점(reserved_)을 풀어 장부 행을 남긴다 — 그러지
//  않으면 선점이 스윕 때까지 남아 한도 계산을 조인다(C-2). 이전 세션·수동 예약은 history_에
//  없으므로 선점은 건드리지 않고 매도가능수량만 되돌린다(포지션 정합은 체결통보로).
OrderAck OrderRouter::reconcile_blocked_sell(const OrderSignal& signal, const OrderGate::OrderRef& reference, bool& intent_taken)
{
    std::vector<OpenOrder> opens;

    // 브로커 미체결 조회를 정본으로 쓴다 — 실계좌는 TTTC0084R, 모의는 일별주문체결조회 VTTC0081R의 잔여수량
    //  (KIS 공식 샘플 inquire_daily_ccld, demo·inner = VTTC0081R, 2026-10-07 MCP 확인).
    //  예전에 모의는 이력만 봤다. 그러면 전송 시간초과(E_TRANSPORT)로 거부 처리됐지만 KIS에는 접수된 매도가
    //  안 보여, 그 매도가 수량을 묶은 채 익절·손절 매도가 하루 종일 40240000으로 막혔다
    //  (10-07 09:00 071320 ORD-000024 RTT=18771ms, 같은 날 003490·033780·060720까지 미해소 7건).
    //  조회가 실패할 때만 이력과 이전 세션 줄로 물러난다 — 이번 세션이 접수를 확인한 주문만 보이지만 아무것도
    //  안 하는 것보다 낫다.
    int cancelled = 0;

    if (fetch_open_orders(opens, "[OrderRouter] 미체결 조회 실패 — 이력으로 찾는다: ",
                          "[OrderRouter] 미체결 조회 예외 — 이력으로 찾는다: "))
    {
        cancelled = close_vanished_session_sells(signal, opens);
    }
    else
    {
        opens.clear();
        collect_session_sells(signal, opens);
    }

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

    LOG_INFO(std::format("[OrderRouter] 예약매도 {}건 취소 완료 → {} 매도 신호 그대로 재매도", cancelled, signal.ticker));

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
        if (!is_open_session_sell(managed_order, signal.symbol_id))
        {
            continue;
        }

        const int outstanding = outstanding_of(managed_order);

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

    if (!close_session_order(digits_to_number(open.kis_order_no), "청산차단 해소 취소"))
    {
        // 이전 세션 줄 — 부속 파일에서 빼고, 취소로 풀린 수량을 장부 매도가능수량에 되돌린다(기동 취소와 같은 처리).
        if (erase_carry_row(open.kis_order_no))
        {
            rewrite_open_orders();
        }

        ledger.restore_sellable(signal.account_id, signal.ticker, open.psbl_qty);
    }

    return true;
}

// 이번 세션 주문 한 건을 CANCELLED로 닫고 선점을 푼다(매도면 매도가능수량도 되돌린다). 이력에 ACCEPTED로 있었으면 참.
bool OrderRouter::close_session_order(uint64_t kis_order_number, const char* reason)
{
    auto& ledger = gate_.ledger();

    // 락 안에서는 이력만 닫고 값을 뜬다. 장부 기록(선점 해제·CANCEL 저널 디스크 쓰기)은 락을 푼 뒤에 한다(CODE_CONVENTIONS 4.5).
    //  closed는 락 안에서 뜬 사본이다 — history_ 원소는 축출로 참조가 죽을 수 있다. 해제량은 락 안의 confirmed_quantity로
    //  정했고, 장부의 선점 빼기는 0에서 멈추는 빼기라 그사이 같은 주문의 체결이 장부에 먼저 들어가도 결과가 같다.
    //  handoff는 장부 기록이 끝날 때까지 선점 정리를 미룬다(LedgerHandoff 주석).
    ManagedOrder                 closed;
    bool                         found   = false;
    int                          release = 0;
    std::optional<LedgerHandoff> handoff;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* managed_order = history_.find_by_order_number(kis_order_number);

        if (managed_order && managed_order->status == OrderStatus::ACCEPTED)
        {
            release                      = outstanding_of(*managed_order);
            managed_order->status        = OrderStatus::CANCELLED;
            managed_order->reject_reason = reason;
            managed_order->updated_at    = std::chrono::system_clock::now();
            closed                       = *managed_order;
            found                        = true;
            handoff.emplace(*this);
        }
    }

    if (!found)
    {
        return false;
    }

    // 저널에는 내부 주문번호(정수)를 적는다. order_id는 "ORD-" 머리글이 붙어 digits_to_number로 읽으면 늘 0이 되고,
    //  리플레이가 이 CANCEL을 INTENT와 짝짓지 못해 재기동 때 닫힌 주문이 미결로 되살아났다.
    if (release > 0)
    {
        ledger.on_cancel(closed.signal.account_id, closed.signal.ticker, closed.signal.side, release,
                         OrderGate::OrderRef{order_number_of(closed.order_id), kis_order_number, closed.signal.type});
    }

    // 선점(reserved_)만 풀면 잔고 시드값 sellable_(취소 전 스냅샷, 주문가능 0)이 그대로라 다음 매도도 0으로
    //  깎인다 — 09-14 15:00 096770 은 익절 취소 뒤 재매도가 유량한도에 막히자 재시도 3회가 전부 "매도가능 0".
    //  브로커에서 풀린 수량만큼 되돌린다(이전 세션 줄과 같은 처리).
    if (closed.signal.side == OrderSide::SELL)
    {
        ledger.restore_sellable(closed.signal.account_id, closed.signal.ticker, release);
    }

    journal_.write_trade_row("", closed, 0, 0.0);
    return true;
}

// 브로커 미체결 목록에 없는데 이력에는 아직 ACCEPTED로 남은 이 종목 매도를 닫는다. 닫은 건수를 돌려준다.
//  10-06 237690: 13:46 SOR 익절 지정가 1주가 체결·취소 통보 없이 브로커에서 사라졌는데 이력은 계속 미체결로 세어,
//  15:42 재매도가 "매도가능 0"으로 4번 막혔다(잔고 주문가능 1주, 원주문 취소는 "정정취소 가능수량이 없습니다").
//  미체결 조회에 안 보이는 주문은 수량을 묶고 있지 않다. 체결 통보가 늦게 오면 미연결 체결로 장부에 들어간다.
//  이력의 ACCEPTED는 KIS가 주문번호를 돌려준 뒤라 그 뒤의 미체결 조회에는 이미 보인다 — 유예를 두지 않는다.
int OrderRouter::close_vanished_session_sells(const OrderSignal& signal, const std::vector<OpenOrder>& opens)
{
    std::vector<uint64_t> vanished;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        for (const auto& managed_order : history_)
        {
            if (!is_open_session_sell(managed_order, signal.symbol_id))
            {
                continue;
            }

            const bool open_at_broker = std::ranges::any_of(opens, [&managed_order](const OpenOrder& open)
            {
                return digits_to_number(open.kis_order_no) == managed_order.kis_order_number;
            });

            if (!open_at_broker)
            {
                vanished.push_back(managed_order.kis_order_number);
            }
        }
    }

    int closed = 0;

    for (const uint64_t kis_order_number : vanished)
    {
        if (close_session_order(kis_order_number, "브로커 미체결 목록에 없음 — 통보 없이 사라짐"))
        {
            LOG_WARN(std::format("[OrderRouter] 사라진 매도 정리 {} ODNO={:010} — 브로커 미체결 목록에 없어 선점을 푼다",
                                 signal.ticker, kis_order_number));
            ++closed;
        }
    }

    return closed;
}
