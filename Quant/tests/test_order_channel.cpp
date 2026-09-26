// tests/test_order_channel.cpp
// 전략↔주문 요청·응답 통로 검증 (D-114 단계 2) — 순번·중복 거름·답 기다리기.
//
//   큐도 스레드도 쓰지 않는다. 레코드와 두 규칙만 손으로 먹여 본다 — 프로세스가 갈려도 이 규칙은 그대로다.
//
//   ① 신호 하나가 요청 레코드로 그대로 옮겨지는가
//   ② 응답 사유가 칸을 넘어도 안전하게 잘리는가
//   ③ 같은 순번은 한 번만 받는가
//   ④ 창보다 오래된 순번은 거르는가(생산자가 하나라 정상 경로에 없다)
//   ⑤ 답이 오면 기다리는 것에서 지워지는가
//   ⑥ 답이 늦은 것을 재전송 후보로 꺼내는가
//   ⑦ 답이 계속 안 오면 상한에서 오래된 것부터 버리고 세는가
//   ⑧ 큐에서 꺼낸 요청·응답의 값이 말이 되는지 보는가(D-114 단계 4 — 건너편을 믿지 않는다)
//   ⑨ 신호가 레코드를 건너갔다 돌아와도 그대로인가 — 글자 칸까지(D-114 단계 4)
//   ⑩ 액션마다 기준이 다른가 — 취소는 수량 0·방향 NONE 도 맞는 주문이다
//   ⑪ 큐가 차서 못 넣은 매도를 종목·계좌마다 하나씩 들고 있다가 든 순서대로 넣는가(매수는 안 든다)
//
//   사용법: test_order_channel

#include "ipc/OrderChannel.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << std::endl; // 여기서 멈추므로 버퍼에 남겨 두지 않는다
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

constexpr int64_t kNanosecondsPerMillisecond = 1'000'000;

constexpr int64_t milliseconds(int64_t count)
{
    return count * kNanosecondsPerMillisecond;
}
} // namespace

int main()
{
    std::cout << "=== 전략↔주문 통로 시험 (D-114 단계 2) ===\n";

    // ── ① 신호 → 요청 레코드 ─────────────────────────────────────────────
    {
        OrderSignal signal;
        signal.sequence       = 42;
        signal.signal_at_ns   = milliseconds(7);
        signal.symbol_id      = 11;
        signal.strategy_index = 3;
        signal.quantity       = 25;
        signal.price          = 68'500.0;
        signal.side           = OrderSide::SELL;
        signal.type           = OrderType::LIMIT;
        signal.action         = OrderAction::NEW;

        const ipc::OrderRequest request = ipc::to_request(signal);

        check(request.sequence == 42, "순번이 그대로 간다");
        check(request.sent_at_ns == milliseconds(7), "보낸 시각이 그대로 간다");
        check(request.symbol_id == 11, "종목 id가 그대로 간다");
        check(request.strategy_index == 3, "전략 번호가 그대로 간다");
        check(request.quantity == 25, "수량이 그대로 간다");
        check(request.price == 68'500.0, "가격이 그대로 간다");
        check(request.side == static_cast<uint8_t>(OrderSide::SELL), "방향이 그대로 간다");
        check(request.order_type == static_cast<uint8_t>(OrderType::LIMIT), "주문 종류가 그대로 간다");

        // 레코드에 문자열도 포인터도 없어야 단계 4에서 공유메모리로 바이트 그대로 간다.
        check(std::is_trivially_copyable_v<ipc::OrderRequest>, "요청 레코드는 통째로 바이트 복사된다");
        check(std::is_trivially_copyable_v<ipc::OrderResponse>, "응답 레코드도 통째로 바이트 복사된다");
    }

    // ── ② 응답 사유 자르기 ───────────────────────────────────────────────
    {
        const auto accepted = ipc::make_response(7, ipc::OrderResult::kAccepted, 123456, "", milliseconds(9));
        check(accepted.sequence == 7, "응답이 요청 순번을 물고 온다");
        check(accepted.kis_order_number == 123456, "접수면 주문번호가 실린다");
        check(accepted.result == static_cast<uint8_t>(ipc::OrderResult::kAccepted), "결과가 실린다");
        check(accepted.reason[0] == '\0', "사유가 없으면 빈 문자열이다");

        const std::string long_reason(200, 'x');
        const auto        rejected = ipc::make_response(8, ipc::OrderResult::kRejected, 0, long_reason, milliseconds(9));

        check(std::strlen(rejected.reason) == ipc::kOrderReasonMax - 1, "칸을 넘는 사유는 잘린다");
        check(rejected.reason[ipc::kOrderReasonMax - 1] == '\0', "잘려도 항상 0으로 끝난다");

        // 증권사 주문번호는 앞에 0이 붙은 문자열이다. 레코드는 정수 손잡이만 들고 간다.
        check(ipc::to_order_number("0000123456") == 123456, "앞의 0을 떼고 숫자로 읽는다");
        check(ipc::to_order_number("") == 0, "빈 주문번호는 0");
        check(ipc::to_order_number("12A45") == 0, "숫자가 아니면 0 — 억지로 해석하지 않는다");
    }

    // ── ③ 같은 순번은 한 번만 ────────────────────────────────────────────
    {
        ipc::DuplicateFilter filter(16);

        check(filter.accept(1), "처음 보는 순번은 받는다");
        check(filter.accept(2), "다음 순번도 받는다");
        check(!filter.accept(2), "같은 순번은 두 번째부터 거른다");
        check(!filter.accept(1), "앞선 순번을 다시 보내도 거른다");
        check(filter.duplicates() == 2, "거른 수를 센다");
        check(filter.highest_seen() == 2, "지금까지 본 가장 큰 순번을 들고 있다");

        // 순번을 안 찍은 경로는 통로를 쓸 수 없다.
        check(!filter.accept(0), "순번 0은 받지 않는다");

        // 건너뛴 순번(중간이 큐에서 버려졌다)도 그대로 받는다 — 통로는 빠진 번호를 메우지 않는다.
        check(filter.accept(9), "중간이 비어도 새 순번은 받는다");
    }

    // ── ④ 창보다 오래된 순번 ─────────────────────────────────────────────
    {
        ipc::DuplicateFilter filter(4);

        for (uint64_t sequence = 1; sequence <= 10; ++sequence)
        {
            check(filter.accept(sequence), "단조 증가는 언제나 받는다");
        }

        // 창(4)에 남아 있는 것은 7~10이다. 3은 창 밖이라 판정할 근거가 없다 —
        //  같은 순번을 두 번 내는 것보다 한 번 놓치는 쪽이 낫다.
        check(!filter.accept(3), "창보다 오래된 순번은 거른다");
        check(!filter.accept(9), "창 안에 남아 있는 순번도 거른다");
    }

    // ── ⑤ 답이 오면 지워진다 ─────────────────────────────────────────────
    {
        ipc::PendingRequests pending(8);

        pending.note_sent(1, milliseconds(0));
        pending.note_sent(2, milliseconds(1));
        check(pending.size() == 2, "보낸 둘을 기다린다");

        check(pending.note_response(1), "기다리던 순번의 답은 참");
        check(pending.size() == 1, "답이 온 것은 지워진다");
        check(!pending.note_response(99), "기다리지 않던 순번의 답은 거짓");
        check(pending.size() == 1, "그때 기다리는 수는 그대로다");

        // 같은 순번을 다시 보내면(재전송) 기다린 시간만 다시 센다.
        pending.note_sent(2, milliseconds(500));
        check(pending.size() == 1, "재전송이 기다리는 수를 늘리지 않는다");
    }

    // ── ⑥ 답이 늦은 것 꺼내기 ────────────────────────────────────────────
    {
        ipc::PendingRequests pending(8);
        constexpr int64_t    timeout = milliseconds(300);

        pending.note_sent(5, milliseconds(0));
        pending.note_sent(6, milliseconds(100));

        check(!pending.oldest_overdue(milliseconds(200), timeout).has_value(), "문턱 안이면 재전송 후보가 없다");

        const auto overdue = pending.oldest_overdue(milliseconds(450), timeout);
        check(overdue.has_value(), "문턱을 넘기면 후보가 나온다");
        check(*overdue == 5, "가장 오래 기다린 것부터 나온다");

        // 6은 아직 350ms밖에 안 됐다 — 450 - 100 = 350 > 300이라 이것도 후보다. 둘 중 오래된 쪽이 5다.
        pending.note_response(5);
        const auto next_overdue = pending.oldest_overdue(milliseconds(450), timeout);
        check(next_overdue.has_value() && *next_overdue == 6, "앞선 것이 답을 받으면 다음 것이 나온다");

        // 재전송하면 기다린 시간이 다시 0부터다.
        pending.note_sent(6, milliseconds(450));
        check(!pending.oldest_overdue(milliseconds(500), timeout).has_value(), "재전송 뒤에는 다시 문턱 안이다");
    }

    // ── ⑦ 상한에서 버린다 ────────────────────────────────────────────────
    {
        ipc::PendingRequests pending(3);

        for (uint64_t sequence = 1; sequence <= 5; ++sequence)
        {
            pending.note_sent(sequence, milliseconds(static_cast<int64_t>(sequence)));
        }

        check(pending.size() == 3, "상한을 넘겨 자라지 않는다");
        check(pending.evicted() == 2, "버린 수를 센다 — 0이 아니면 주문 쪽이 답을 못 주고 있다");
        check(!pending.note_response(1), "버려진 순번의 답은 기다리던 것이 아니다");
        check(pending.note_response(5), "가장 최근 것은 남아 있다");

        // 순번 0은 답을 맞출 수 없으니 아예 안 적는다.
        const size_t before = pending.size();
        pending.note_sent(0, milliseconds(99));
        check(pending.size() == before, "순번 0은 기다리는 것에 안 넣는다");
    }

    // ── ⑧ 꺼낸 칸은 믿지 않는다 ─────────────────────────────────────────
    {
        const ipc::RequestLimits limits{.symbol_count = 100, .strategy_count = 8};

        ipc::OrderRequest request;
        request.sequence       = 7;
        request.sent_at_ns     = milliseconds(3);
        request.symbol_id      = 41;
        request.strategy_index = 2;
        request.quantity       = 10;
        request.price          = 71'200.0;
        request.side           = OrderSide::BUY;
        request.order_type     = static_cast<uint8_t>(OrderType::LIMIT);
        request.action         = static_cast<uint8_t>(OrderAction::NEW);
        check(ipc::is_plausible(request, limits), "성한 요청은 지나간다");

        ipc::OrderRequest market = request;
        market.price             = 0.0;
        market.order_type        = static_cast<uint8_t>(OrderType::MARKET);
        check(ipc::is_plausible(market, limits), "시장가는 가격 0이 정상이다");

        const auto rejects = [&limits](ipc::OrderRequest broken, const std::string& name) {
            check(!ipc::is_plausible(broken, limits), name);
        };

        ipc::OrderRequest no_sequence = request;
        no_sequence.sequence          = 0;
        rejects(no_sequence, "순번 0은 버린다");

        ipc::OrderRequest out_of_table = request;
        out_of_table.symbol_id        = 101;
        rejects(out_of_table, "종목 표 밖 번호는 버린다 — 그 값으로 배열을 짚지 않는다");

        ipc::OrderRequest no_symbol = request;
        no_symbol.symbol_id        = symbol::kNone;
        rejects(no_symbol, "종목 번호 0은 버린다");

        ipc::OrderRequest bad_strategy = request;
        bad_strategy.strategy_index   = 9;
        rejects(bad_strategy, "전략 표 밖 번호는 버린다");

        ipc::OrderRequest negative_quantity = request;
        negative_quantity.quantity         = -5;
        rejects(negative_quantity, "수량이 0 이하면 버린다");

        ipc::OrderRequest huge_quantity = request;
        huge_quantity.quantity         = 2'000'000;
        rejects(huge_quantity, "수량 상한을 넘으면 버린다");

        ipc::OrderRequest negative_price = request;
        negative_price.price           = -1.0;
        rejects(negative_price, "음수 가격은 버린다");

        ipc::OrderRequest not_a_number = request;
        not_a_number.price            = std::numeric_limits<double>::quiet_NaN();
        rejects(not_a_number, "NaN 가격은 버린다 — 비교가 전부 거짓이라 뒤집어 본다");

        ipc::OrderRequest bad_side = request;
        bad_side.side             = OrderSide::NONE;
        rejects(bad_side, "방향이 매수·매도가 아니면 버린다");

        ipc::OrderRequest bad_action = request;
        bad_action.action          = 9;
        rejects(bad_action, "모르는 주문 동작은 버린다");

        ipc::OrderResponse response =
            ipc::make_response(7, ipc::OrderResult::kAccepted, 123, "접수", milliseconds(4));
        check(ipc::is_plausible(response), "성한 응답은 지나간다");

        ipc::OrderResponse no_sequence_response = response;
        no_sequence_response.sequence           = 0;
        check(!ipc::is_plausible(no_sequence_response), "순번 0 응답은 버린다");

        ipc::OrderResponse bad_result = response;
        bad_result.result            = 9;
        check(!ipc::is_plausible(bad_result), "모르는 결과 값은 버린다");

        ipc::OrderResponse unterminated = response;
        std::memset(unterminated.reason, 'x', ipc::kOrderReasonMax);
        check(!ipc::is_plausible(unterminated), "사유 칸이 칸 안에서 안 끝나면 버린다");
    }

    // ── ⑨ 신호 → 레코드 → 신호 왕복 ─────────────────────────────────────
    {
        OrderSignal signal;
        signal.ticker                       = "005930";
        signal.symbol_id                    = 11;
        signal.side                         = OrderSide::BUY;
        signal.type                         = OrderType::LIMIT;
        signal.quantity                     = 7;
        signal.price                        = 68'500.0;
        signal.reference_price              = 68'900.0;
        signal.strategy_index               = 3;
        signal.market                       = Market::KR;
        signal.exchange                     = "NAS";
        signal.account_id                   = "5020";
        signal.action                       = OrderAction::NEW;
        signal.client_order_id              = "DEVSCALE_005930:B:12";
        signal.original_client_order_id     = "DEVSCALE_005930:B:11";
        signal.client_order_number          = 900123;
        signal.original_client_order_number = 900122;
        signal.reason                       = "평단 대비 -2.3% 눌림, 5분 거래대금 상위";
        signal.tick_at_ns                   = milliseconds(2);
        signal.signal_at_ns                 = milliseconds(3);
        signal.sequence                     = 77;
        signal.timestamp                    = std::chrono::system_clock::now();

        bool                    truncated = true;
        const ipc::OrderRequest request   = ipc::to_request(signal, &truncated);
        check(!truncated, "칸에 드는 글은 안 잘린다");

        const OrderSignal back = ipc::to_signal(request, "DEVSCALE_005930");
        check(back.ticker == signal.ticker, "종목 코드가 글자 그대로 돌아온다");
        check(back.symbol_id == signal.symbol_id, "종목 번호가 돌아온다");
        check(back.side == signal.side && back.type == signal.type, "방향·주문 종류가 돌아온다");
        check(back.quantity == signal.quantity && back.price == signal.price, "수량·가격이 돌아온다");
        check(back.reference_price == signal.reference_price, "참조가가 돌아온다");
        check(back.strategy_id == "DEVSCALE_005930", "전략 이름은 번호로 표에서 찾아 넣는다");
        check(back.strategy_index == signal.strategy_index, "전략 번호가 돌아온다");
        check(back.market == signal.market && back.exchange == signal.exchange, "시장·거래소가 돌아온다");
        check(back.account_id == signal.account_id, "계좌가 돌아온다");
        check(back.action == signal.action, "주문 동작이 돌아온다");
        check(back.client_order_id == signal.client_order_id, "주문 이름이 돌아온다");
        check(back.original_client_order_id == signal.original_client_order_id, "원주문 이름이 돌아온다");
        check(back.client_order_number == signal.client_order_number, "주문 번호가 돌아온다");
        check(back.original_client_order_number == signal.original_client_order_number, "원주문 번호가 돌아온다");
        check(back.reason == signal.reason, "판단 근거가 한글 그대로 돌아온다");
        check(back.tick_at_ns == signal.tick_at_ns && back.signal_at_ns == signal.signal_at_ns, "두 시각이 돌아온다");
        check(back.sequence == signal.sequence, "순번이 돌아온다");
        // system_clock 눈금(MSVC는 100나노초)보다 작은 자리는 버려진다 — 1마이크로초 안이면 같은 시각으로 본다.
        const auto time_gap = back.timestamp - signal.timestamp;
        check(time_gap < std::chrono::microseconds(1) && time_gap > std::chrono::microseconds(-1),
              "신호를 만든 시각이 눈금 안에서 돌아온다");

        // 한글은 한 글자가 세 바이트다. 바이트로 끊으면 반쪽 글자가 남아 원장 CSV·로그가 깨진다.
        OrderSignal long_reason = signal;
        long_reason.reason      = std::string(80, 'x') + std::string(60, ' ');

        for (int index = 0; index < 60; ++index)
        {
            long_reason.reason += "가";
        }

        const ipc::OrderRequest cut = ipc::to_request(long_reason, &truncated);
        check(truncated, "칸을 넘으면 잘렸다고 알린다");
        check(std::strlen(cut.reason) < ipc::kSignalReasonMax, "잘려도 칸 안에서 끝난다");

        const std::string kept = ipc::to_signal(cut, "DEVSCALE_005930").reason;
        check(long_reason.reason.compare(0, kept.size(), kept) == 0, "잘린 글은 앞쪽이 그대로다");
        // 자른 자리가 글자 경계인가 — 버린 첫 바이트가 글자 가운데(10xxxxxx)면 앞에 반쪽 글자를 남긴 것이다.
        //  남긴 글의 마지막 바이트로는 못 본다. 온전한 한글의 셋째 바이트도 10xxxxxx 라서 그렇다.
        check((static_cast<unsigned char>(long_reason.reason[kept.size()]) & 0xC0) != 0x80,
              "자르는 자리가 글자 경계다 — 반쪽 글자를 남기지 않는다");
    }

    // ── ⑩ 액션마다 기준이 다르다 ────────────────────────────────────────
    {
        const ipc::RequestLimits limits{.symbol_count = 100, .strategy_count = 8};

        ipc::OrderRequest cancel;
        cancel.sequence                     = 9;
        cancel.sent_at_ns                   = milliseconds(3);
        cancel.symbol_id                    = 41;
        cancel.strategy_index               = 2;
        cancel.action                       = static_cast<uint8_t>(OrderAction::CANCEL);
        cancel.quantity                     = 0;
        cancel.side                         = OrderSide::NONE;
        cancel.original_client_order_number = 900122;
        check(ipc::is_plausible(cancel, limits), "취소는 수량 0·방향 NONE 이어도 지나간다");

        ipc::OrderRequest by_name = cancel;
        by_name.original_client_order_number = 0;
        std::memcpy(by_name.original_client_order_id, "DEV:B:11", 9);
        check(ipc::is_plausible(by_name, limits), "원주문 이름만 있어도 지나간다");

        ipc::OrderRequest no_target                 = cancel;
        no_target.original_client_order_number      = 0;
        check(!ipc::is_plausible(no_target, limits), "취소인데 대상이 없으면 버린다");

        ipc::OrderRequest empty_new = cancel;
        empty_new.action            = static_cast<uint8_t>(OrderAction::NEW);
        check(!ipc::is_plausible(empty_new, limits), "신규인데 수량 0이면 버린다");

        // 처음 보는 종목은 번호가 없다 — 받는 쪽이 코드 글자로 표에 올린다.
        ipc::OrderRequest fresh = cancel;
        fresh.symbol_id         = symbol::kNone;
        fresh.ticker            = "068270";
        check(ipc::is_plausible(fresh, limits), "표에 없는 종목은 코드 글자로 지나간다");

        ipc::OrderRequest nameless = fresh;
        nameless.ticker           = "";
        check(!ipc::is_plausible(nameless, limits), "번호도 코드도 없으면 버린다");

        ipc::OrderRequest unterminated = cancel;
        std::memset(unterminated.reason, 'x', ipc::kSignalReasonMax);
        check(!ipc::is_plausible(unterminated, limits), "근거 칸이 칸 안에서 안 끝나면 버린다");
    }

    // ── ⑪ 큐가 차면 매도는 들고 있다가 먼저 넣는다 ─────────────────────
    {
        const auto make_request = [](uint64_t sequence, symbol::SymbolId symbol_id, uint8_t side, const char* account)
        {
            ipc::OrderRequest request;
            request.sequence  = sequence;
            request.symbol_id = symbol_id;
            request.quantity  = 10;
            request.side      = side;
            request.action    = static_cast<uint8_t>(OrderAction::NEW);
            std::strncpy(request.account_id, account, ipc::kAccountIdMax - 1);
            return request;
        };

        const ipc::OrderRequest sell_a = make_request(1, 41, OrderSide::SELL, "1111");
        const ipc::OrderRequest sell_b = make_request(2, 42, OrderSide::SELL, "1111");
        const ipc::OrderRequest buy_a  = make_request(3, 41, OrderSide::BUY, "1111");

        ipc::OrderRequest cancel_a = sell_a;
        cancel_a.action            = static_cast<uint8_t>(OrderAction::CANCEL);

        check(ipc::HeldSellRequests::holds(sell_a), "신규 매도는 든다");
        check(!ipc::HeldSellRequests::holds(buy_a), "매수는 안 든다 — 늦은 매수는 버린다");
        check(!ipc::HeldSellRequests::holds(cancel_a), "취소는 안 든다 — 대상이 종목이 아니라 원주문이다");

        ipc::HeldSellRequests held;
        check(!held.hold(sell_a), "처음 든 종목은 새 자리다");
        check(!held.hold(sell_b), "다른 종목은 따로 든다");

        ipc::OrderRequest sell_a_later = sell_a;
        sell_a_later.sequence          = 5;
        sell_a_later.quantity          = 7;
        check(held.hold(sell_a_later), "같은 종목·계좌의 매도는 가장 최근 것으로 바뀐다");
        check(held.size() == 2, "종목마다 하나라 보유 종목 수를 넘지 않는다");

        const ipc::OrderRequest sell_a_other_account = make_request(6, 41, OrderSide::SELL, "2222");
        check(!held.hold(sell_a_other_account), "같은 종목이라도 계좌가 다르면 따로 든다");
        check(held.size() == 3, "계좌가 다른 매도는 바뀌지 않는다");

        // 큐에 한 자리만 난 상황 — 첫 것만 넣고 멈춘다.
        std::vector<uint64_t> pushed;
        size_t                room = 1;
        const auto            push = [&pushed, &room](const ipc::OrderRequest& request)
        {
            if (room == 0)
            {
                return false;
            }

            --room;
            pushed.push_back(request.sequence);
            return true;
        };

        check(held.drain(push) == 1 && pushed.size() == 1, "자리가 하나면 하나만 넣고 멈춘다");
        check(pushed[0] == 5, "먼저 든 종목부터 넣는다 — 바뀐 것은 원래 자리를 지킨다");
        check(held.size() == 2, "못 넣은 것은 계속 든다");

        room = 10;
        check(held.drain(push) == 2 && held.empty(), "자리가 나면 나머지를 다 넣는다");
        check(pushed[1] == 2 && pushed[2] == 6, "넣는 순서는 든 순서다");

        // 표에 아직 없는 종목은 번호가 없어 코드 글자로 맞춘다.
        ipc::OrderRequest fresh_first = make_request(7, symbol::kNone, OrderSide::SELL, "1111");
        fresh_first.ticker            = "068270";
        ipc::OrderRequest fresh_other = fresh_first;
        fresh_other.sequence          = 8;
        fresh_other.ticker            = "000660";
        ipc::OrderRequest fresh_again = fresh_first;
        fresh_again.sequence          = 9;
        check(!held.hold(fresh_first) && !held.hold(fresh_other), "번호 없는 종목은 코드 글자로 가른다");
        check(held.hold(fresh_again) && held.size() == 2, "번호 없는 같은 종목은 코드 글자로 맞춰 바꾼다");
    }

    std::cout << "test_order_channel: " << g_checks << " checks passed\n";
    return 0;
}
