#include "ipc/LedgerSnapshot.h"

namespace ipc
{

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

    LedgerRow&     row = rows_[id];
    const uint64_t now = generation_.load(std::memory_order_relaxed);

    if (row.stamp != now)
    {
        // 이번 판에 처음 손대는 줄이다. 지난 판 값이 남아 있으므로 0으로 되돌린다.
        row       = LedgerRow{};
        row.stamp = now;

        const uint32_t written = written_count_.load(std::memory_order_relaxed);

        if (written < kMaxSymbols)
        {
            written_ids_[written] = id;
            written_count_.store(written + 1, std::memory_order_relaxed);
        }
    }

    return row;
}

LedgerRow LedgerSnapshot::row(symbol::SymbolId id) const noexcept
{
    if (id == symbol::kNone || static_cast<size_t>(id) >= kMaxSymbols)
    {
        return LedgerRow{};
    }

    return read_stable([this, id]
    {
        const LedgerRow& row = rows_[id];

        // 이번 판에 안 채워진 줄은 값이 없는 것이다 — 지난 판 값을 돌려주면 그게 곧 낡은 사본이다.
        return row.stamp == generation_.load(std::memory_order_relaxed) ? row : LedgerRow{};
    });
}

LedgerGlobals LedgerSnapshot::globals() const noexcept
{
    return read_stable([this] { return globals_; });
}

EntryView LedgerSnapshot::entry(symbol::SymbolId id) const noexcept
{
    // 보유·선점·여력을 한 판에서 함께 읽는다. 따로 읽으면 그 사이에 판이 바뀌어 낡은 조합을 본다. [why D-086]
    return read_stable([this, id]
    {
        EntryView view;
        view.capacity_full = globals_.capacity_full != 0;

        if (id != symbol::kNone && static_cast<size_t>(id) < kMaxSymbols)
        {
            const LedgerRow& row = rows_[id];

            if (row.stamp == generation_.load(std::memory_order_relaxed))
            {
                view.position = row.position;
                view.reserved = row.reserved;
            }
        }

        return view;
    });
}

size_t LedgerSnapshot::collect_rows(symbol::SymbolId* out_ids, LedgerRow* out_rows, size_t capacity) const noexcept
{
    // 목록 전체를 한 판 안에서 읽는다 — 줄마다 따로 읽으면 앞줄과 뒷줄이 다른 판의 것이 되어,
    //  "이미 판 종목이 아직 보유로 잡히는" 조합을 본다.
    return read_stable([this, out_ids, out_rows, capacity]() -> size_t
    {
        const uint64_t now     = generation_.load(std::memory_order_relaxed);
        const uint32_t written = written_count_.load(std::memory_order_relaxed);
        size_t         taken   = 0;

        for (uint32_t slot = 0; slot < written && slot < kMaxSymbols; ++slot)
        {
            const symbol::SymbolId id = written_ids_[slot];

            if (id == symbol::kNone || static_cast<size_t>(id) >= kMaxSymbols || rows_[id].stamp != now)
            {
                continue;
            }

            if (taken < capacity)
            {
                out_ids[taken]  = id;
                out_rows[taken] = rows_[id];
            }

            ++taken;
        }

        return taken;
    });
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
