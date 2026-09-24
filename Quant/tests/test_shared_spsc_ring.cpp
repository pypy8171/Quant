// 공유 쪽지 위 한줄 큐(ipc::SharedSpscRing) 경계 시험 — 순서·가득참·되감기, 그리고 건너편이 공유 칸을
//  망가뜨렸을 때 배열 밖을 짚지 않고 수로 남기는지. 마지막 묶음은 진짜 공유메모리(ipc::SharedRegion) 위에서
//  손잡이 둘을 두고 돌린다 — 프로세스가 갈렸을 때와 같은 배치다.
#include "ipc/SharedRegion.h"
#include "ipc/SharedSpscRing.h"
#include "ipc/OrderChannel.h"

#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                     \
    do                                                                                       \
    {                                                                                        \
        ++g_checks;                                                                          \
        if (!(condition))                                                                    \
        {                                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";   \
            return 1;                                                                        \
        }                                                                                    \
    } while (0)

// 시험용 레코드 — 포인터 없는 고정 크기 값. 실제 요청 레코드는 마지막 묶음에서 쓴다.
struct Counted
{
    uint64_t value     = 0;
    uint32_t marker    = 0;
    uint32_t reserved0 = 0;
};

using Ring = ipc::SharedSpscRing<Counted>;

constexpr size_t kCapacity = 8;

// 큐가 놓일 바이트. 캐시라인 경계여야 한다 — 공유 구역에서는 payload()가 그 자리다.
alignas(ipc::kSharedCacheLine) std::byte g_storage[Ring::bytes_for(kCapacity) + 64];

std::string unique_name(const char* suffix)
{
#ifdef _WIN32
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = static_cast<unsigned long>(getpid());
#endif
    return "quant_test_ring_" + std::to_string(process_id) + "_" + suffix;
}
} // namespace

int main()
{
    constexpr size_t storage_bytes = Ring::bytes_for(kCapacity);

    // 1. 놓고 붙는다. 붙는 쪽이 칸 수·레코드 크기를 다르게 알면 붙지 않는다.
    {
        Ring sender;
        CHECK(!sender.is_bound());
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        CHECK(sender.is_bound());
        CHECK(sender.capacity() == kCapacity);
        CHECK(sender.last_error().empty());

        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));
        CHECK(receiver.is_bound());

        ipc::SharedSpscRing<uint64_t> other_record;
        CHECK(!other_record.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));
        CHECK(!other_record.last_error().empty());

        Ring other_capacity;
        CHECK(!other_capacity.attach(g_storage, storage_bytes, kCapacity / 2, ipc::RingEndpoint::kConsumer));

        // 아직 아무도 안 놓은 바이트에는 붙지 않는다(주문 프로세스가 먼저 떠야 한다).
        alignas(ipc::kSharedCacheLine) static std::byte empty_storage[storage_bytes] = {};
        Ring                                            absent;
        CHECK(!absent.attach(empty_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));
    }

    // 2. 잘못된 인자는 놓지 않는다 — 2의 거듭제곱이 아닌 칸 수, 모자란 바이트, 어긋난 경계.
    {
        Ring ring;
        CHECK(!ring.create(g_storage, storage_bytes, 6));
        CHECK(!ring.create(g_storage, storage_bytes, 0));
        CHECK(!ring.create(nullptr, storage_bytes, kCapacity));
        CHECK(!ring.create(g_storage, storage_bytes - 1, kCapacity));
        CHECK(!ring.create(g_storage + 1, storage_bytes, kCapacity)); // 경계가 어긋난 자리
        CHECK(!ring.is_bound());
        CHECK(!ring.last_error().empty());
    }

    // 3. 보낸 순서대로 나온다. 빈 큐에서는 아무것도 안 나온다.
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        Counted taken;
        CHECK(!receiver.pop(taken));

        for (uint64_t value = 1; value <= 5; ++value)
        {
            CHECK(sender.push(Counted{value, 0x1234, 0}));
        }

        CHECK(sender.pending() == 5);

        for (uint64_t value = 1; value <= 5; ++value)
        {
            CHECK(receiver.pop(taken));
            CHECK(taken.value == value);
            CHECK(taken.marker == 0x1234);
        }

        CHECK(!receiver.pop(taken));
        CHECK(sender.sent() == 5);
        CHECK(receiver.received() == 5);
        CHECK(sender.peer_counter_rejected() == 0);
        CHECK(receiver.stamp_out_of_turn() == 0);
    }

    // 4. 가득 차면 더 안 들어간다 — 안 읽은 칸을 덮어쓰지 않는다. 한 칸 비면 한 칸 들어간다.
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        for (uint64_t value = 1; value <= kCapacity; ++value)
        {
            CHECK(sender.push(Counted{value, 0, 0}));
        }

        CHECK(!sender.push(Counted{99, 0, 0}));
        CHECK(sender.pending() == kCapacity);

        Counted taken;
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 1);
        CHECK(sender.push(Counted{99, 0, 0}));
        CHECK(!sender.push(Counted{100, 0, 0}));
    }

    // 5. 칸 수보다 많이 돌려도 순서가 유지된다(첨자가 되감긴다).
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        Counted taken;

        for (uint64_t value = 1; value <= kCapacity * 5; ++value)
        {
            CHECK(sender.push(Counted{value, 0, 0}));
            CHECK(receiver.pop(taken));
            CHECK(taken.value == value);
        }

        CHECK(!receiver.pop(taken));
        CHECK(sender.sent() == kCapacity * 5);
    }

    // 6. 건너편이 "내가 얼마나 읽었나" 칸을 망가뜨리면 보내지 않고 센다 — 안 읽은 칸을 덮는 것보다 미루는 쪽이다.
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        CHECK(sender.push(Counted{1, 0, 0}));

        auto* control = reinterpret_cast<ipc::SharedRingControl*>(g_storage);
        control->published_tail.store(9'999'999, std::memory_order_release);

        CHECK(!sender.push(Counted{2, 0, 0}));
        CHECK(sender.peer_counter_rejected() == 1);
        CHECK(sender.pending() == 0);

        // 칸이 제자리로 돌아오면 다시 보낸다 — 한 번 이상해졌다고 큐가 죽지 않는다.
        control->published_tail.store(1, std::memory_order_release);
        CHECK(sender.push(Counted{2, 0, 0}));
        CHECK(sender.peer_counter_rejected() == 1);
    }

    // 7. 도장이 제 차례보다 앞서 있으면(칸이 덮였거나 순번이 건너뛰었다) 세고 그 자리로 옮겨 이어 읽는다.
    //  여기서 굳으면 요청이 영영 안 나간다.
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        CHECK(sender.push(Counted{1, 0, 0}));
        CHECK(sender.push(Counted{2, 0, 0}));

        // 첫 칸의 도장을 앞으로 밀어 순번 하나를 건너뛴 것처럼 만든다.
        auto* slots = reinterpret_cast<Ring::Slot*>(g_storage + sizeof(ipc::SharedRingControl));
        slots[0].stamp.store(kCapacity + 1, std::memory_order_release);
        slots[0].record.value = 77;

        Counted taken;
        CHECK(!receiver.pop(taken)); // 한 번은 건너뛴 것을 알아채고 자리를 옮긴다
        CHECK(receiver.stamp_out_of_turn() == 1);
        CHECK(receiver.received() == kCapacity);
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 77);
    }

    // 8. 다시 붙을 때 두 순번이 서로 말이 안 되면 안 읽은 칸을 버리고 맨 앞에서 시작한다(주문 쪽 재기동).
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        CHECK(sender.push(Counted{1, 0, 0}));
        CHECK(sender.push(Counted{2, 0, 0}));

        auto* control = reinterpret_cast<ipc::SharedRingControl*>(g_storage);
        control->published_tail.store(123'456, std::memory_order_release);

        Ring rebound;
        CHECK(rebound.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kBoth));
        CHECK(rebound.peer_counter_rejected() == 1);
        CHECK(rebound.received() == 2); // 보낸 자리까지 따라간다 — 안 읽은 두 칸은 버린다
        CHECK(rebound.sent() == 2);

        Counted taken;
        CHECK(!rebound.pop(taken));
        CHECK(rebound.push(Counted{3, 0, 0})); // 같은 손잡이로 이어서 보낼 수 있다
    }

    // 9. 진짜 공유메모리 위에서 요청 레코드를 주고받는다 — 구역 손잡이 둘(만든 쪽·붙은 쪽)이 곧 프로세스 둘이다.
    {
        using RequestRing = ipc::SharedSpscRing<ipc::OrderRequest>;
        constexpr size_t request_capacity = 16;
        const size_t     region_bytes =
            ipc::SharedRegion::header_bytes() + RequestRing::bytes_for(request_capacity);

        const std::string name = unique_name("orders");
        ipc::SharedRegion owner_region;
        CHECK(owner_region.create(name, region_bytes, 1));

        RequestRing order_sender;
        CHECK(order_sender.create(owner_region.payload(), owner_region.payload_bytes(), request_capacity));

        ipc::SharedRegion guest_region;
        CHECK(guest_region.attach(name, region_bytes, 1, ipc::SharedAttachRole::kStrategy));
        RequestRing order_receiver;
        CHECK(order_receiver.attach(guest_region.payload(), guest_region.payload_bytes(), request_capacity,
                                   ipc::RingEndpoint::kConsumer));

        ipc::OrderRequest request;
        request.sequence       = 77;
        request.sent_at_ns     = 1'700'000'000'000'000'000;
        request.symbol_id      = static_cast<symbol::SymbolId>(41);
        request.strategy_index = static_cast<strategy_table::StrategyId>(3);
        request.quantity       = 10;
        request.price          = 71'200.0;
        request.side           = 1;
        request.order_type     = 2;
        request.action         = 1;
        CHECK(order_sender.push(request));

        ipc::OrderRequest taken;
        CHECK(order_receiver.pop(taken));
        CHECK(taken.sequence == 77);
        CHECK(taken.symbol_id == static_cast<symbol::SymbolId>(41));
        CHECK(taken.strategy_index == static_cast<strategy_table::StrategyId>(3));
        CHECK(taken.quantity == 10);
        CHECK(taken.price == 71'200.0);
        CHECK(taken.side == 1);
        CHECK(taken.order_type == 2);
        CHECK(taken.action == 1);
        CHECK(!order_receiver.pop(taken));

        // 응답 레코드도 같은 배치로 돈다 — 사유 칸(고정 길이 문자 배열)이 바이트째 건너간다.
        using ResponseRing = ipc::SharedSpscRing<ipc::OrderResponse>;
        CHECK(ResponseRing::bytes_for(4) < owner_region.payload_bytes());
    }

    // 10. 받는 쪽은 readable() 로 묻는다 — 건너편이 공유 칸에 적은 순번만 보고, 보내는 쪽 값은 안 읽는다.
    //  받는 쪽이 pending() 을 부르면 쌓여 있어도 0으로 보인다는 것까지 같이 고정한다 — 잠드는 조건에
    //  그것을 쓰면 프로세스를 가른 뒤 주문이 만기까지 밀린다.
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        CHECK(receiver.readable() == 0);

        for (uint64_t value = 1; value <= 3; ++value)
        {
            CHECK(sender.push(Counted{value, 0x77, 0}));
        }

        CHECK(sender.pending() == 3);
        CHECK(receiver.readable() == 3);
        CHECK(receiver.pending() == 0); // 붙은 쪽은 보낸 것이 없다 — 이 물음은 받는 쪽 것이 아니다

        Counted taken;
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 1);
        CHECK(receiver.readable() == 2);

        while (receiver.pop(taken))
        {
        }

        CHECK(receiver.readable() == 0);
    }

    // 11. 붙는 쪽이 셋이 된 뒤의 그물 — 읽기만 하려고 붙은 손잡이는 남의 받은 자리를 건드리지 않는다.
    //  시세·전략 둘이 같은 쪽지에 붙으면서, 제 줄이 아닌 줄에도 자리를 잡게 됐다. 그때 그 손잡이가
    //  published_tail 을 적으면 흐르고 있던 줄의 받는 자리가 뒤로 밀려 이미 읽은 것을 다시 읽는다. [why D-114]
    {
        Ring sender;
        CHECK(sender.create(g_storage, storage_bytes, kCapacity));
        Ring receiver;
        CHECK(receiver.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kConsumer));

        for (uint64_t value = 1; value <= 4; ++value)
        {
            CHECK(sender.push(Counted{value, 0x11, 0}));
        }

        Counted taken;
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 1);
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 2);

        auto*          control     = reinterpret_cast<ipc::SharedRingControl*>(g_storage);
        const uint64_t tail_before = control->published_tail.load(std::memory_order_acquire);
        CHECK(tail_before == 2);

        // 제3자가 붙는다 — 이 줄은 제 줄이 아니라 자리만 잡는다.
        Ring onlooker;
        CHECK(onlooker.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kObserver));
        CHECK(control->published_tail.load(std::memory_order_acquire) == tail_before);

        // 자리만 잡은 손잡이가 꺼내거나 넣으려 하면 아무 일도 안 하고 수로 남는다.
        Counted stolen;
        CHECK(!onlooker.pop(stolen));
        CHECK(!onlooker.push(Counted{99, 0, 0}));
        CHECK(onlooker.endpoint_misuse() == 2);
        CHECK(control->published_tail.load(std::memory_order_acquire) == tail_before);

        // 흐르던 줄은 그대로 이어진다 — 3번부터다.
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 3);
        CHECK(receiver.pop(taken));
        CHECK(taken.value == 4);
        CHECK(!receiver.pop(taken));

        // 받는 끝으로 붙은 손잡이는 넣지 못하고, 보내는 끝으로 붙은 손잡이는 꺼내지 못한다.
        CHECK(!receiver.push(Counted{5, 0, 0}));
        CHECK(receiver.endpoint_misuse() == 1);

        Ring feeder;
        CHECK(feeder.attach(g_storage, storage_bytes, kCapacity, ipc::RingEndpoint::kProducer));
        CHECK(!feeder.pop(stolen));
        CHECK(feeder.endpoint_misuse() == 1);
        CHECK(control->published_tail.load(std::memory_order_acquire) == 4);
    }

    std::cout << "test_shared_spsc_ring OK (" << g_checks << " checks)\n";
    return 0;
}
