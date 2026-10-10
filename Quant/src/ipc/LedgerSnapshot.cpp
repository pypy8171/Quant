#include "ipc/LedgerSnapshot.h"

#include <cstring>

namespace ipc
{

namespace
{

// 구조체 하나를 단어 배열로 옮겨 원자 칸에 relaxed로 싣는다. 순서는 판 번호 쪽 울타리가 맞춘다.
template <typename Value, size_t kWords>
void store_words(std::atomic<uint64_t> (&cells)[kWords], const Value& value) noexcept
{
    static_assert(sizeof(Value) == kWords * sizeof(uint64_t), "단어 수가 구조체 크기와 맞아야 한다");
    uint64_t words[kWords];
    std::memcpy(words, &value, sizeof(Value));

    for (size_t cell_index = 0; cell_index < kWords; ++cell_index)
    {
        cells[cell_index].store(words[cell_index], std::memory_order_relaxed);
    }
}

// store_words의 반대 — 원자 칸을 relaxed로 읽어 구조체로 옮긴다.
template <typename Value, size_t kWords>
Value load_words(const std::atomic<uint64_t> (&cells)[kWords]) noexcept
{
    static_assert(sizeof(Value) == kWords * sizeof(uint64_t), "단어 수가 구조체 크기와 맞아야 한다");
    uint64_t words[kWords];

    for (size_t cell_index = 0; cell_index < kWords; ++cell_index)
    {
        words[cell_index] = cells[cell_index].load(std::memory_order_relaxed);
    }

    Value value;
    std::memcpy(&value, words, sizeof(Value));
    return value;
}

// 판이 안정되지 않았을 때 돌려주는 전역값 — 새 진입도 전략 매도도 막고 자리도 없다고 본다.
//  틀린 판으로 주문을 내는 것보다 한 바퀴 쉬는 쪽이 낫다는 판단이다.
LedgerGlobals halted_globals() noexcept
{
    LedgerGlobals globals;
    globals.entry_halted       = 1;
    globals.manual_sell_halted = 1;
    globals.capacity_full      = 1;
    return globals;
}

} // namespace

void LedgerSnapshot::begin_publish() noexcept
{
    // 판 번호를 홀수로 올려 "쓰는 중"을 알린다.
    version_.fetch_add(1, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_relaxed);
    written_count_.store(0, std::memory_order_relaxed); // 실은 종목 목록은 판마다 새로 쌓는다

    // 이 뒤의 값 쓰기가 판 번호 홀수화보다 위로 올라가면 안 된다. release 연산은 "앞에 있던 쓰기"만
    //  묶으므로 뒤엣것을 잡으려면 울타리가 따로 있어야 한다. 없으면 읽는 쪽이 짝수 판 번호로
    //  반쪽 값을 읽는다.
    std::atomic_thread_fence(std::memory_order_release);
}

void LedgerSnapshot::end_publish() noexcept
{
    // 쓰는 쪽 칸에 채운 값을 판 번호가 아직 홀수인 동안 공개 칸으로 옮긴다. 이번 판에 손댄 줄만 옮긴다 —
    //  안 옮긴 줄은 지난 판의 stamp를 달고 있어 읽는 쪽이 빈 줄로 본다.
    store_words(published_globals_, staging_globals_);

    const uint32_t written = written_count_.load(std::memory_order_relaxed);

    for (uint32_t slot = 0; slot < written && slot < kMaxSymbols; ++slot)
    {
        const symbol::SymbolId id = written_ids_[slot].load(std::memory_order_relaxed);
        store_words(published_rows_[id], staging_rows_[id]);
    }

    // 값 쓰기가 전부 끝난 뒤에 판 번호가 짝수로 보여야 한다 — 여기는 "앞에 있던 쓰기"를 묶는
    //  것이 맞으므로 release 연산 하나로 족하다.
    version_.fetch_add(1, std::memory_order_release);
}

LedgerRow& LedgerSnapshot::row_for_write(symbol::SymbolId id) noexcept
{
    if (id == symbol::kNone || static_cast<size_t>(id) >= kMaxSymbols)
    {
        return discard_;
    }

    LedgerRow&     row = staging_rows_[id];
    const uint64_t now = generation_.load(std::memory_order_relaxed);

    if (row.stamp != now)
    {
        // 이번 판에 처음 손대는 줄이다. 지난 판 값이 남아 있으므로 0으로 되돌린다.
        row       = LedgerRow{};
        row.stamp = now;

        const uint32_t written = written_count_.load(std::memory_order_relaxed);

        if (written < kMaxSymbols)
        {
            written_ids_[written].store(id, std::memory_order_relaxed);
            written_count_.store(written + 1, std::memory_order_relaxed);
        }
    }

    return row;
}

LedgerRow LedgerSnapshot::load_published_row(size_t index) const noexcept
{
    return load_words<LedgerRow>(published_rows_[index]);
}

LedgerRow LedgerSnapshot::row(symbol::SymbolId id) const noexcept
{
    if (id == symbol::kNone || static_cast<size_t>(id) >= kMaxSymbols)
    {
        return LedgerRow{};
    }

    const std::optional<LedgerRow> stable = read_stable([this, id]
    {
        const LedgerRow row = load_published_row(id);

        // 이번 판에 안 채워진 줄은 값이 없는 것이다 — 지난 판 값을 돌려주면 그게 곧 낡은 사본이다.
        return row.stamp == generation_.load(std::memory_order_relaxed) ? row : LedgerRow{};
    });

    // 판을 못 읽었으면 빈 줄을 돌려준다 — 보유 0으로 보여 이 종목의 매도 판단을 한 바퀴 쉬게 된다.
    return stable.value_or(LedgerRow{});
}

LedgerGlobals LedgerSnapshot::globals() const noexcept
{
    const std::optional<LedgerGlobals> stable = read_stable([this]
    {
        return load_words<LedgerGlobals>(published_globals_);
    });

    if (!stable)
    {
        return halted_globals();
    }

    return *stable;
}

EntryView LedgerSnapshot::entry(symbol::SymbolId id) const noexcept
{
    // 보유·선점·여력을 한 판에서 함께 읽는다. 따로 읽으면 그 사이에 판이 바뀌어 낡은 조합을 본다. [why D-086]
    const std::optional<EntryView> stable = read_stable([this, id]
    {
        EntryView view;
        view.capacity_full = load_words<LedgerGlobals>(published_globals_).capacity_full != 0;

        if (id != symbol::kNone && static_cast<size_t>(id) < kMaxSymbols)
        {
            const LedgerRow row = load_published_row(id);

            if (row.stamp == generation_.load(std::memory_order_relaxed))
            {
                view.position = row.position;
                view.reserved = row.reserved;
            }
        }

        return view;
    });

    if (!stable)
    {
        // 판을 못 읽었으면 자리가 없다고 본다 — 새 진입을 한 바퀴 미룬다.
        EntryView view;
        view.capacity_full = true;
        return view;
    }

    return *stable;
}

size_t LedgerSnapshot::collect_rows(symbol::SymbolId* out_ids, LedgerRow* out_rows, size_t capacity) const noexcept
{
    // 목록 전체를 한 판 안에서 읽는다 — 줄마다 따로 읽으면 앞줄과 뒷줄이 다른 판의 것이 되어,
    //  "이미 판 종목이 아직 보유로 잡히는" 조합을 본다.
    const std::optional<size_t> stable = read_stable([this, out_ids, out_rows, capacity]() -> size_t
    {
        const uint64_t now     = generation_.load(std::memory_order_relaxed);
        const uint32_t written = written_count_.load(std::memory_order_relaxed);
        size_t         taken   = 0;

        for (uint32_t slot = 0; slot < written && slot < kMaxSymbols; ++slot)
        {
            const symbol::SymbolId id = written_ids_[slot].load(std::memory_order_relaxed);

            if (id == symbol::kNone || static_cast<size_t>(id) >= kMaxSymbols)
            {
                continue;
            }

            const LedgerRow row = load_published_row(id);

            if (row.stamp != now)
            {
                continue;
            }

            if (taken < capacity)
            {
                out_ids[taken]  = id;
                out_rows[taken] = row;
            }

            ++taken;
        }

        return taken;
    });

    // 판을 못 읽었으면 0줄 — 보유 전체를 훑는 정리 주문이 이번 바퀴를 건너뛴다.
    return stable.value_or(0);
}

uint64_t LedgerSnapshot::generation() const noexcept
{
    return generation_.load(std::memory_order_acquire);
}

void collect_all_rows(const LedgerSnapshot& snapshot, std::vector<symbol::SymbolId>& ids, std::vector<LedgerRow>& rows)
{
    // 한 판에 실리는 줄은 "보유 + 미체결"이라 슬롯 상한 언저리의 수십 줄이다. 그 크기로 시작해서
    //  모자랄 때만 키운다 — 호출부마다 295KB를 들고 있지 않으려고 이렇게 한다.
    constexpr size_t kFirstGuess = 64;

    if (ids.size() < kFirstGuess)
    {
        ids.resize(kFirstGuess);
        rows.resize(kFirstGuess);
    }

    size_t total = snapshot.collect_rows(ids.data(), rows.data(), ids.size());

    if (total > ids.size())
    {
        // 모자랐다. 실제 수만큼 키워 한 번 더 읽는다. 그 사이 판이 바뀌어 또 모자랄 수 있으므로
        //  담긴 만큼으로 줄여 끝낸다 — 못 담은 줄이 남은 채로 "전부"라고 말하지 않는다.
        ids.resize(total);
        rows.resize(total);
        total = snapshot.collect_rows(ids.data(), rows.data(), ids.size());
    }

    const size_t taken = (total < ids.size()) ? total : ids.size();
    ids.resize(taken);
    rows.resize(taken);
}

} // namespace ipc
