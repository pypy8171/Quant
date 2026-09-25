// 공유 쪽지 위에 얹는 한줄 큐 — 보내는 쪽 하나, 받는 쪽 하나(SPSC). 프로세스가 갈려도 형태는 RingBuffer와
//  같고, 다른 것은 셋이다. ① 칸을 미리 잡아 둔 바이트 위에 놓는다(프로세스마다 주소가 달라 포인터를 못 쓴다).
//  ② 레코드는 포인터 없는 고정 크기 값뿐이다. ③ **건너편을 믿지 않는다** — 내 자리(순번)는 내 프로세스 안에만
//  두고 공유 칸에는 건너편이 보라고 적어만 둔다. 건너편이 그 칸을 망가뜨려도 배열 밖을 짚지 않고, 이상한 값은
//  세어서 남긴다(건강 판정이 그 수를 본다). 요청·응답·제어 줄과 체결·시세 통로가 전부 이 큐 위에 서 있어서
//  여기서 한 번 막으면 모든 면이 같이 막힌다.
//  [why D-114]
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace ipc
{

// 캐시라인 한 줄. RingBuffer의 것과 같은 값이지만, 그 헤더(벡터·optional)를 끌어오지 않으려고 따로 둔다.
constexpr size_t kSharedCacheLine = 64;

// 제어 칸 머리. 붙는 쪽이 "같은 큐인가"를 이것만 보고 정한다.
constexpr uint32_t kSharedRingMagic = 0x51'52'4e'47; // 'QRNG'

// 이 인스턴스가 줄의 어느 끝을 맡는가. 붙는 쪽이 둘(전략·시세)이 되면서 "읽기만 하려고 붙는" 자리가 생겼다 —
//  그 자리가 남의 소비자 칸을 건드리지 않게 목적을 붙을 때 받는다. [why D-114]
enum class RingEndpoint : uint8_t
{
    kBoth     = 0, // 한 인스턴스가 양쪽 끝을 맡는다 — 한 프로세스로 돌 때와 자리를 놓는 쪽
    kProducer = 1, // 넣기만 한다. published_tail 은 읽기만 한다
    kConsumer = 2, // 꺼내기만 한다. published_tail 을 적는 쪽은 여기뿐이다
    kObserver = 3, // 자리만 잡는다. 공유 칸에 아무것도 안 적는다
};

// 큐 하나의 제어 칸. 순번 둘과 예비 칸 하나는 각각 제 캐시라인을 차지한다 — 보내는 쪽과 받는 쪽이 같은 줄을 두고 싸우면
//  프로세스 사이에서는 스레드 사이보다 더 비싸다.
//  [inv] 여기 있는 값은 전부 건너편이 고칠 수 있다고 보고 읽는다. 배열 첨자는 이 값으로 만들지 않는다.
struct SharedRingControl
{
    uint32_t magic        = 0;
    uint32_t record_bytes = 0;
    uint64_t capacity     = 0; // 칸 수, 2의 거듭제곱

    alignas(kSharedCacheLine) std::atomic<uint64_t> published_head{0}; // 보내는 쪽이 적는다
    alignas(kSharedCacheLine) std::atomic<uint64_t> published_tail{0}; // 받는 쪽이 적는다
    alignas(kSharedCacheLine) std::atomic<uint64_t> reserved{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "프로세스 둘이 같이 보는 값이라 잠금 없는 원자여야 한다 — 잠금이 끼면 프로세스 안 뮤텍스가 된다");

// 한줄 큐 한쪽 끝. attach()로 붙은 인스턴스는 한쪽(보내는 쪽·받는 쪽·구경)만 맡는다 — 양쪽을 다 맡는 것은
//  create()로 놓은 인스턴스(kBoth)뿐이다.
//  Record는 포인터 없는 고정 크기 값이어야 한다(바이트째 복사한다).
template <typename Record> class SharedSpscRing
{
public:
    static_assert(std::is_trivially_copyable_v<Record>,
                  "레코드는 바이트째 복사된다 — 포인터·문자열·가상 함수를 담을 수 없다");

    // 칸 하나. 순번 도장을 먼저 보고 레코드를 읽는다 — 도장이 제 차례가 아니면 아직 안 쓰인 칸이다.
    struct Slot
    {
        alignas(8) std::atomic<uint64_t> stamp{0};
        Record record{};
    };

    // capacity칸짜리 큐가 차지하는 바이트. 구역 크기를 셈할 때 쓴다.
    [[nodiscard]] static constexpr size_t bytes_for(size_t capacity) noexcept
    {
        return sizeof(SharedRingControl) + capacity * sizeof(Slot);
    }

    // 큐를 새로 놓는다(구역을 만든 쪽이 한 번 부른다). base는 캐시라인 경계여야 하고 capacity는 2의 거듭제곱이다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, size_t capacity) noexcept
    {
        if (!check_arguments(base, bytes, capacity))
        {
            return false;
        }

        // 칸까지 통째로 지우고 머리를 마지막에 적는다 — 붙는 쪽은 magic을 보고 들어온다.
        std::memset(base, 0, bytes_for(capacity));
        auto* control         = reinterpret_cast<SharedRingControl*>(base);
        control->record_bytes = static_cast<uint32_t>(sizeof(Record));
        control->capacity     = capacity;
        control->published_head.store(0, std::memory_order_relaxed);
        control->published_tail.store(0, std::memory_order_relaxed);
        control->magic = kSharedRingMagic;

        bind(base, capacity);

        // 자리를 놓는 쪽은 이 줄을 한 프로세스가 다 쓸 때도 있어(Both) 양쪽 끝을 다 맡는다.
        endpoint_ = RingEndpoint::kBoth;
        return true;
    }

    // 이미 놓인 큐에 붙는다(건너편 프로세스가 부른다). 머리가 다르면 붙지 않는다.
    //  endpoint 는 이 손잡이가 줄의 어느 끝인가다 — 기본값을 안 두는 것은 붙는 쪽이 둘(전략·시세)이고 줄마다 맡는 끝이 달라
    //  "그냥 붙기"가 남의 커서를 덮는 일이 실제로 생기기 때문이다. [why D-114]
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, size_t capacity, RingEndpoint endpoint) noexcept
    {
        if (!check_arguments(base, bytes, capacity))
        {
            return false;
        }

        const auto* control = reinterpret_cast<const SharedRingControl*>(base);

        if (control->magic != kSharedRingMagic || control->record_bytes != sizeof(Record) ||
            control->capacity != capacity)
        {
            last_error_ = "큐 머리가 다르다 — 옛 exe가 새 배치에 붙었는지 본다";
            return false;
        }

        bind(base, capacity);
        endpoint_ = endpoint;

        // 붙는 쪽은 자기 자리를 이미 돌던 큐에서 이어받는다 — 재기동 전 칸을 처음부터 다시 읽지 않는다.
        //  다만 두 순번이 서로 말이 안 되면(뒤가 앞을 넘거나 차이가 칸 수보다 크면) 그대로 쓰지 않는다.
        //  그때는 안 읽은 칸을 버리고 맨 앞에서 시작한다 — 엉뚱한 자리를 기다리며 굳는 것보다 낫다.
        const uint64_t head = control->published_head.load(std::memory_order_acquire);
        uint64_t       tail = control->published_tail.load(std::memory_order_acquire);

        if (head - tail > capacity_)
        {
            tail = head;
            ++peer_counter_rejected_;

            // 고친 자리를 공유 칸에 적는 것은 **받는 쪽으로 붙은 손잡이뿐**이다. 안 적으면 보내는 쪽이 계속
            //  "가득 참"으로 보고 큐가 굳으니 받는 쪽은 적어야 하고, 반대로 보내는 쪽·구경하는 쪽이 적으면
            //  이미 잘 돌던 줄에서 남의 받은 자리를 통째로 앞으로 밀어 안 읽은 칸을 버리게 된다. [why D-114]
            if (writes_consumer_cursor())
            {
                control_->published_tail.store(tail, std::memory_order_release);
            }
        }

        next_to_send_    = head;
        next_to_receive_ = tail;
        return true;
    }

    void unbind() noexcept
    {
        control_ = nullptr;
        slots_   = nullptr;
    }

    [[nodiscard]] bool is_bound() const noexcept
    {
        return control_ != nullptr;
    }

    // 보내는 쪽이 부른다. false는 "가득 참" — 건너편이 안 읽고 있다는 뜻이라 호출자가 센다.
    [[nodiscard]] bool push(const Record& record) noexcept
    {
        if (control_ == nullptr)
        {
            return false;
        }

        // 보내는 끝이 아닌 손잡이로 넣으면 건너편 보내는 쪽과 같은 칸을 두고 다툰다(SPSC 가 깨진다).
        //  막고 센다 — 배선이 어긋난 것이라 소리 없이 지나가면 안 된다.
        if (endpoint_ != RingEndpoint::kBoth && endpoint_ != RingEndpoint::kProducer)
        {
            ++endpoint_misuse_;
            return false;
        }

        // 내 자리는 내 프로세스 안 값이다. 건너편이 적어 둔 칸은 "얼마나 읽었나"를 볼 때만 쓴다.
        const uint64_t peer_tail = control_->published_tail.load(std::memory_order_acquire);
        const uint64_t used      = next_to_send_ - peer_tail;

        // 건너편이 나보다 앞서 읽었다고 적혀 있으면(부호 없는 뺄셈이라 아주 큰 수로 보인다) 그 값은 못 믿는다.
        //  그때는 보내지 않고 센다 — 안 읽은 칸을 덮어쓰는 쪽보다 한 건을 미루는 쪽이 낫다.
        if (used > capacity_)
        {
            ++peer_counter_rejected_;
            return false;
        }

        if (used == capacity_)
        {
            return false;
        }

        Slot& slot = slots_[next_to_send_ & index_mask_];
        slot.record = record;

        // 도장을 마지막에 찍는다(release) — 받는 쪽은 도장을 먼저 보므로(acquire) 반쪽 레코드를 읽지 않는다.
        slot.stamp.store(next_to_send_ + 1, std::memory_order_release);
        ++next_to_send_;
        control_->published_head.store(next_to_send_, std::memory_order_release);
        return true;
    }

    // 받는 쪽이 부른다. false는 "빈 큐"다. 건너편이 적은 순번은 첨자로 쓰지 않는다 — 내 자리의 도장만 본다.
    [[nodiscard]] bool pop(Record& out) noexcept
    {
        if (control_ == nullptr)
        {
            return false;
        }

        // 꺼내는 쪽은 published_tail 을 적는다. 받는 끝이 아닌 손잡이가 꺼내면 남의 받은 자리를 덮는다.
        if (!writes_consumer_cursor())
        {
            ++endpoint_misuse_;
            return false;
        }

        Slot&          slot  = slots_[next_to_receive_ & index_mask_];
        const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

        if (stamp != next_to_receive_ + 1)
        {
            // 도장이 제 차례보다 뒤면 아직 안 쓰인 칸이다 — 빈 큐다.
            //  앞서 있으면 순번이 건너뛴 것이다. 세어 두고 그 자리로 옮겨 다음 부름에서 읽는다
            //  — 여기서 굳으면 요청이 영영 안 나간다(A등급). 건너뛴 사실은 수로 남아 건강 판정에 실린다.
            if (stamp > next_to_receive_ + 1)
            {
                ++stamp_out_of_turn_;
                next_to_receive_ = stamp - 1;
                control_->published_tail.store(next_to_receive_, std::memory_order_release);
            }

            return false;
        }

        out = slot.record;
        ++next_to_receive_;
        control_->published_tail.store(next_to_receive_, std::memory_order_release);
        return true;
    }

    // 보내는 쪽이 보는 대기 칸 수(어림값 — 건너편이 그 사이 더 읽었을 수 있다).
    //  [inv] 이것은 보내는 스레드만 부른다. next_to_send_ 는 그 스레드의 값이라 남이 읽으면 경합이다.
    //  받는 쪽은 readable() 을 부른다.
    [[nodiscard]] size_t pending() const noexcept
    {
        if (control_ == nullptr)
        {
            return 0;
        }

        const uint64_t peer_tail = control_->published_tail.load(std::memory_order_acquire);
        const uint64_t used      = next_to_send_ - peer_tail;
        return used > capacity_ ? 0 : static_cast<size_t>(used);
    }

    // 받는 쪽이 보는 읽을 칸 수(어림값 — 건너편이 그 사이 더 보냈을 수 있다).
    //  건너편이 공유 칸에 적어 둔 순번과 내 자리의 차다. 보내는 쪽 값(next_to_send_)은 건드리지 않는다
    //  — 그것은 남의 프로세스·남의 스레드 것이라 읽으면 경합이고, 프로세스를 가르면 0이라 뜻도 없다.
    //  받는 쪽이 pending() 을 부르면 큐에 쌓여 있어도 "비었다"로 읽어 잠든다. [why D-114]
    [[nodiscard]] size_t readable() const noexcept
    {
        if (control_ == nullptr)
        {
            return 0;
        }

        const uint64_t peer_head = control_->published_head.load(std::memory_order_acquire);
        const uint64_t ready     = peer_head - next_to_receive_;

        // 건너편 순번이 내 자리보다 뒤면(부호 없는 뺄셈이라 아주 큰 수로 보인다) 그 값은 못 믿는다.
        //  0을 주면 깨울 때까지·만기까지 잠들 뿐이고, 진짜 남은 칸은 pop() 의 도장이 가린다.
        return ready > capacity_ ? 0 : static_cast<size_t>(ready);
    }

    // 보낸 쪽도 받는 쪽도 아닌 스레드가 "큐에 남았나"를 물을 때 쓴다 — 공유 칸 둘만 읽고 두 끝의 제 자리
    //  값(next_to_send_·next_to_receive_)은 건드리지 않는다. readable() 은 받는 쪽 제 자리를 읽으므로
    //  남이 부르면 꺼내는 스레드와 겹친다. 두 순번을 따로 읽어 그 사이 건너편이 움직일 수 있다 — 어림값이다.
    //  [inv] 아무 스레드나 불러도 된다. [why D-114 단계 5]
    [[nodiscard]] size_t in_flight() const noexcept
    {
        if (control_ == nullptr)
        {
            return 0;
        }

        const uint64_t published = control_->published_head.load(std::memory_order_acquire);
        const uint64_t consumed  = control_->published_tail.load(std::memory_order_acquire);
        const uint64_t used      = published - consumed;

        return used > capacity_ ? 0 : static_cast<size_t>(used);
    }

    // 지금까지 보낸 수·받은 수. 둘 다 공유 칸에서 읽는다 — 제 자리 값(next_to_send_·next_to_receive_)을
    //  읽으면 건너편 끝이나 제3의 스레드(제어 스레드가 1분마다 찍는 고수위 로그)가 부를 때 그 끝과 겹친다.
    //  공유 칸은 같은 값을 release 로 적어 둔 것이라 숫자는 그대로고, 어느 끝에서 물어도 같은 답이 온다.
    //  [why D-114 단계 5]
    [[nodiscard]] uint64_t sent() const noexcept
    {
        return control_ == nullptr ? 0 : control_->published_head.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t received() const noexcept
    {
        return control_ == nullptr ? 0 : control_->published_tail.load(std::memory_order_acquire);
    }

    // 건너편이 적어 둔 순번이 말이 안 돼 보내기를 미룬 횟수. 0이 아니면 건너편 프로세스를 의심한다.
    [[nodiscard]] uint64_t peer_counter_rejected() const noexcept
    {
        return peer_counter_rejected_;
    }

    // 도장이 제 차례보다 앞서 있던 횟수. 0이 아니면 칸이 덮였거나 순번이 건너뛰었다는 뜻이다.
    [[nodiscard]] uint64_t stamp_out_of_turn() const noexcept
    {
        return stamp_out_of_turn_;
    }

    // 제 끝이 아닌 일(보내는 쪽이 꺼내거나 그 반대)을 청한 횟수. 0이 아니면 배선이 어긋난 것이다.
    [[nodiscard]] uint64_t endpoint_misuse() const noexcept
    {
        return endpoint_misuse_;
    }

    [[nodiscard]] RingEndpoint endpoint() const noexcept
    {
        return endpoint_;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return static_cast<size_t>(capacity_);
    }

    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

private:
    // published_tail 을 적어도 되는 끝인가. 받는 쪽과 한 프로세스가 양쪽을 다 맡는 경우뿐이다.
    [[nodiscard]] bool writes_consumer_cursor() const noexcept
    {
        return endpoint_ == RingEndpoint::kBoth || endpoint_ == RingEndpoint::kConsumer;
    }

    [[nodiscard]] bool check_arguments(std::byte* base, size_t bytes, size_t capacity) noexcept
    {
        last_error_.clear();

        if (base == nullptr || capacity == 0 || (capacity & (capacity - 1)) != 0)
        {
            last_error_ = "칸 수가 0이거나 2의 거듭제곱이 아니다";
            return false;
        }

        if (bytes < bytes_for(capacity))
        {
            last_error_ = "구역이 큐보다 작다 — 필요=" + std::to_string(bytes_for(capacity)) +
                          " 받은 것=" + std::to_string(bytes);
            return false;
        }

        if ((reinterpret_cast<uintptr_t>(base) % alignof(SharedRingControl)) != 0)
        {
            last_error_ = "큐가 캐시라인 경계에서 시작하지 않는다";
            return false;
        }

        return true;
    }

    void bind(std::byte* base, size_t capacity) noexcept
    {
        control_    = reinterpret_cast<SharedRingControl*>(base);
        slots_      = reinterpret_cast<Slot*>(base + sizeof(SharedRingControl));
        capacity_   = capacity;
        index_mask_ = capacity - 1;
    }

    SharedRingControl* control_ = nullptr;
    Slot*              slots_   = nullptr;
    uint64_t           capacity_   = 0;
    uint64_t           index_mask_ = 0;

    // 내 자리. 공유 칸이 아니라 내 프로세스 안에 있다 — 건너편이 못 고친다.
    uint64_t next_to_send_    = 0;
    uint64_t next_to_receive_ = 0;

    // 이 손잡이가 맡은 끝. create() 는 kBoth, attach() 는 부르는 쪽이 정한다.
    RingEndpoint endpoint_ = RingEndpoint::kBoth;

    uint64_t    peer_counter_rejected_ = 0;
    uint64_t    stamp_out_of_turn_     = 0;
    uint64_t    endpoint_misuse_       = 0;
    std::string last_error_;
};

} // namespace ipc
