// tests/test_fill_channel.cpp
// 시세 → 주문 체결통보 통로 검증 (D-114 단계 5) — 체결 한 건이 경계를 넘어도 순서와 내용이 그대로인가.
//
//   ① 체결통보 → 레코드 → 체결통보 왕복에서 값이 안 바뀌는가(글자 칸·시각·수량·가격)
//   ② 보낸 차례대로 나오는가 — 체결 순서가 바뀌면 예약 수량이 엉뚱한 주문에서 풀린다
//   ③ 칸을 넘는 글자를 글자 경계에서 자르고 잘렸다고 알리는가
//   ④ 말이 안 되는 레코드를 원장에 넣지 않고 세는가(건너편이 망가졌을 때)
//   ⑤ 큐가 차면 버리고 세는가 — 보내는 쪽은 안 기다린다
//   ⑥ 붙은 손잡이가 제 끝만 맡는가(보내는 끝은 못 꺼내고 받는 끝은 못 넣는다)
//
//   사용법: test_fill_channel

#include "ipc/FillChannel.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

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

constexpr size_t kCapacity     = 8;
constexpr size_t kStorageBytes = 64 * 1024;

alignas(ipc::kSharedCacheLine) std::byte g_storage[kStorageBytes];

FillNotification fill_of(const std::string& order_number, int quantity, double price)
{
    FillNotification fill;
    fill.kis_order_no      = order_number;
    fill.original_order_no = "0000000000";
    fill.ticker            = "005930";
    fill.side              = OrderSide::BUY;
    fill.filled_quantity   = quantity;
    fill.filled_price      = price;
    fill.fill_time         = "093001";
    fill.order_quantity    = quantity * 2;
    fill.session_generation = 3;
    fill.exchange          = "KRX";
    fill.timestamp         = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
    return fill;
}

void test_round_trip()
{
    ipc::FillChannel sender;
    check(!sender.is_bound(), "놓기 전에는 안 붙은 상태");
    check(sender.create(g_storage, kStorageBytes, kCapacity), "체결 통로를 놓는다");
    check(sender.is_bound(), "놓은 뒤에는 붙은 상태");

    ipc::FillChannel receiver;
    check(receiver.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity),
          "받는 끝으로 붙는다");

    const FillNotification original = fill_of("0000012345", 7, 71'200.0);
    bool                   truncated = true;
    const ipc::FillNotice  notice    = ipc::to_notice(original, 1, 999, &truncated);
    check(!truncated, "칸에 드는 글자는 안 잘린다");
    check(ipc::is_plausible(notice, ipc::FillLimits{}), "옮긴 레코드는 말이 된다");
    check(sender.push(notice), "체결통보를 넣는다");

    ipc::FillNotice taken;
    check(receiver.pop(ipc::FillLimits{}, taken), "체결통보가 나온다");

    const FillNotification restored = ipc::to_fill(taken);
    check(restored.kis_order_no == original.kis_order_no, "주문번호가 그대로");
    check(restored.original_order_no == original.original_order_no, "원주문번호가 그대로");
    check(restored.ticker == original.ticker, "종목 코드가 그대로");
    check(restored.side == original.side, "방향이 그대로");
    check(restored.filled_quantity == original.filled_quantity, "체결수량이 그대로");
    check(restored.filled_price == original.filled_price, "체결단가가 그대로");
    check(restored.fill_time == original.fill_time, "체결시각이 그대로");
    check(restored.session_generation == original.session_generation, "세션 번호가 그대로");
    check(restored.order_quantity == original.order_quantity, "주문수량이 그대로");
    check(restored.exchange == original.exchange, "거래소 구분이 그대로");
    check(restored.timestamp == original.timestamp, "받은 시각이 그대로");

    // 넘어간 건수는 링 순번을 그대로 읽고, 그 순번은 공유 칸에 있다 — 어느 손잡이로 물어도 같은 답이다.
    //  한쪽 프로세스만 보고도 통로가 도는지 알 수 있다. [why D-114 단계 5]
    check(sender.sent() == 1, "보낸 건수를 센다");
    check(receiver.received() == 1, "받은 건수를 센다");
    check(sender.received() == 1, "보낸 쪽에서도 받은 건수가 보인다");
    check(receiver.sent() == 1, "받은 쪽에서도 보낸 건수가 보인다");
    check(sender.overflow() == 0 && receiver.discarded() == 0, "버린 것이 없다");
}

void test_order_is_kept()
{
    ipc::FillChannel sender;
    check(sender.create(g_storage, kStorageBytes, kCapacity), "놓기(차례 시험)");

    ipc::FillChannel receiver;
    check(receiver.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity), "붙기(차례 시험)");

    for (int index = 1; index <= 5; ++index)
    {
        const FillNotification fill = fill_of("000001234" + std::to_string(index), index, 70'000.0 + index);
        check(sender.push(ipc::to_notice(fill, static_cast<uint64_t>(index), index)), "차례로 넣기");
    }

    for (int index = 1; index <= 5; ++index)
    {
        ipc::FillNotice taken;
        check(receiver.pop(ipc::FillLimits{}, taken), "차례로 꺼내기");
        check(taken.sequence == static_cast<uint64_t>(index), "순번이 보낸 차례 그대로");
        check(taken.filled_quantity == index, "수량이 보낸 차례 그대로");
        check(ipc::to_fill(taken).kis_order_no == "000001234" + std::to_string(index), "주문번호가 보낸 차례 그대로");
    }

    ipc::FillNotice empty;
    check(!receiver.pop(ipc::FillLimits{}, empty), "다 꺼내면 더 안 나온다");
    check(receiver.received() == 5, "받은 건수가 다섯");
}

void test_truncation()
{
    FillNotification fill = fill_of("0123456789012345678901234567890", 3, 70'000.0);
    fill.ticker           = "0059300059300059300";

    bool                  truncated = false;
    const ipc::FillNotice notice    = ipc::to_notice(fill, 1, 1, &truncated);

    check(truncated, "칸을 넘으면 잘렸다고 알린다");
    check(std::strlen(notice.kis_order_no) == ipc::kFillOrderNumberMax - 1, "주문번호가 칸 안에서 끝난다");
    check(std::strlen(notice.ticker) == ipc::kFillTickerMax - 1, "종목 코드가 칸 안에서 끝난다");
    check(ipc::is_plausible(notice, ipc::FillLimits{}), "잘려도 말은 되는 레코드다");

    // 여러 바이트 글자가 칸 경계에 걸리면 글자 한가운데서 자르지 않는다.
    FillNotification wide = fill_of("0000012345", 3, 70'000.0);
    wide.exchange         = "가나다라마바사";
    const ipc::FillNotice wide_notice = ipc::to_notice(wide, 1, 1, nullptr);
    check(std::strlen(wide_notice.exchange) % 3 == 0, "세 바이트 글자 경계에서 자른다");
    check(std::strlen(wide_notice.exchange) < ipc::kFillExchangeMax, "잘린 글자가 칸 안에서 끝난다");
}

void test_implausible_is_dropped()
{
    const ipc::FillLimits limits;
    const ipc::FillNotice good = ipc::to_notice(fill_of("0000012345", 3, 70'000.0), 1, 1);
    check(ipc::is_plausible(good, limits), "온전한 레코드는 통과한다");

    ipc::FillNotice no_sequence = good;
    no_sequence.sequence        = 0;
    check(!ipc::is_plausible(no_sequence, limits), "순번 0은 안 받는다");

    ipc::FillNotice no_time = good;
    no_time.sent_at_ns      = 0;
    check(!ipc::is_plausible(no_time, limits), "보낸 시각 0은 안 받는다");

    ipc::FillNotice bad_side = good;
    bad_side.side            = 9;
    check(!ipc::is_plausible(bad_side, limits), "표 밖 방향은 안 받는다");

    ipc::FillNotice no_quantity = good;
    no_quantity.filled_quantity = 0;
    check(!ipc::is_plausible(no_quantity, limits), "체결수량 0은 체결이 아니다");

    ipc::FillNotice huge_quantity = good;
    huge_quantity.filled_quantity = limits.quantity_max + 1;
    check(!ipc::is_plausible(huge_quantity, limits), "상한을 넘는 수량은 안 받는다");

    ipc::FillNotice negative_order = good;
    negative_order.order_quantity  = -1;
    check(!ipc::is_plausible(negative_order, limits), "음수 주문수량은 안 받는다");

    ipc::FillNotice zero_order = good;
    zero_order.order_quantity  = 0;
    check(ipc::is_plausible(zero_order, limits), "주문수량 0은 '모른다'라 받는다");

    ipc::FillNotice no_price = good;
    no_price.filled_price    = 0.0;
    check(!ipc::is_plausible(no_price, limits), "가격 0은 안 받는다");

    ipc::FillNotice huge_price = good;
    huge_price.filled_price    = limits.price_max * 2;
    check(!ipc::is_plausible(huge_price, limits), "상한을 넘는 가격은 안 받는다");

    ipc::FillNotice no_ticker = good;
    no_ticker.ticker[0]       = '\0';
    check(!ipc::is_plausible(no_ticker, limits), "종목 코드가 비면 안 받는다");

    // 칸이 0으로 안 끝나면 읽는 쪽이 칸 밖을 짚는다 — 여기서 먼저 걸러야 한다.
    ipc::FillNotice unterminated = good;
    std::memset(unterminated.ticker, 'A', ipc::kFillTickerMax);
    check(!ipc::is_plausible(unterminated, limits), "0으로 안 끝나는 칸은 안 받는다");

    // 통로가 그것을 대신 세는가 — 꺼내는 자리가 여기 하나뿐이라 부르는 쪽이 검사를 잊을 수 없다.
    ipc::FillChannel sender;
    check(sender.create(g_storage, kStorageBytes, kCapacity), "놓기(망가진 칸 시험)");

    ipc::FillChannel receiver;
    check(receiver.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity),
          "붙기(망가진 칸 시험)");

    ipc::FillNotice later = ipc::to_notice(fill_of("0000099999", 5, 70'500.0), 2, 2);

    check(sender.push(no_quantity), "말 안 되는 레코드를 넣는다");
    check(sender.push(later), "온전한 레코드를 이어 넣는다");

    ipc::FillNotice taken;
    check(receiver.pop(limits, taken), "다음 온전한 레코드가 나온다");
    check(taken.sequence == 2, "버린 자리에서 멈추지 않고 다음 칸으로 간다");
    check(receiver.discarded() == 1, "버린 건수를 센다");
}

void test_overflow()
{
    ipc::FillChannel sender;
    check(sender.create(g_storage, kStorageBytes, kCapacity), "놓기(넘침 시험)");

    for (size_t index = 0; index < kCapacity; ++index)
    {
        check(sender.push(ipc::to_notice(fill_of("0000012345", 1, 70'000.0), index + 1, 1)), "칸을 채운다");
    }

    check(!sender.push(ipc::to_notice(fill_of("0000012345", 1, 70'000.0), kCapacity + 1, 1)), "가득 차면 못 넣는다");
    check(sender.overflow() == 1, "버린 건수를 센다");
    check(sender.pending() == kCapacity, "대기 칸 수가 칸 수와 같다");
}

void test_endpoints()
{
    ipc::FillChannel owner;
    check(owner.create(g_storage, kStorageBytes, kCapacity), "놓기(끝 시험)");

    ipc::FillChannel feed_side;
    check(feed_side.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kProducer, kCapacity),
          "시세가 보내는 끝으로 붙는다");

    ipc::FillChannel onlooker;
    check(onlooker.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kObserver, kCapacity),
          "전략은 자리만 잡는다");

    const ipc::FillNotice notice = ipc::to_notice(fill_of("0000012345", 4, 70'000.0), 1, 1);
    check(feed_side.push(notice), "보내는 끝은 넣는다");

    ipc::FillNotice taken;
    check(!feed_side.pop(ipc::FillLimits{}, taken), "보내는 끝은 못 꺼낸다");
    check(!onlooker.pop(ipc::FillLimits{}, taken), "자리만 잡은 손잡이는 못 꺼낸다");
    check(!onlooker.push(notice), "자리만 잡은 손잡이는 못 넣는다");

    check(owner.pop(ipc::FillLimits{}, taken), "만든 쪽이 받는 끝을 겸한다");
    check(taken.sequence == 1, "끼어든 손잡이가 자리를 밀지 않았다");
    check(owner.readable() == 0, "다 읽으면 읽을 칸이 없다");
}

void test_refusals()
{
    ipc::FillChannel layout;
    check(layout.create(g_storage, kStorageBytes, kCapacity), "놓기(거절 시험)");

    ipc::FillChannel other;
    check(!other.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity * 2),
          "칸 수가 다르면 안 붙는다");
    check(!other.last_error().empty(), "거절 사유가 남는다");
    check(!other.attach(nullptr, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity),
          "자리가 없으면 안 붙는다");
    check(!other.attach(g_storage + 8, kStorageBytes - 8, ipc::RingEndpoint::kConsumer, kCapacity),
          "경계가 어긋나면 안 붙는다");
    check(!other.attach(g_storage, 64, ipc::RingEndpoint::kConsumer, kCapacity), "구역이 작으면 안 붙는다");
    check(!other.create(g_storage, kStorageBytes, 6), "2의 거듭제곱이 아닌 칸 수는 못 놓는다");
    check(other.attach(g_storage, kStorageBytes, ipc::RingEndpoint::kConsumer, kCapacity), "같은 값이면 붙는다");

    other.unbind();
    check(!other.is_bound(), "뗀 뒤에는 안 붙은 상태");
}

} // namespace

int main()
{
    std::cout << "=== 시세 → 주문 체결통보 통로 (D-114 단계 5) ===\n";
    test_round_trip();
    test_order_is_kept();
    test_truncation();
    test_implausible_is_dropped();
    test_overflow();
    test_endpoints();
    test_refusals();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
