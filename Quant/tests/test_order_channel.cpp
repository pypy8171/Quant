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
//
//   사용법: test_order_channel

#include "ipc/OrderChannel.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
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

    std::cout << "test_order_channel: " << g_checks << " checks passed\n";
    return 0;
}
