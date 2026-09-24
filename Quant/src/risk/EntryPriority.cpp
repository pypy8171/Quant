#include "risk/EntryPriority.h"

#include <algorithm>
#include <format>

// ─── 우선순위 표 ─────────────────────────────────────────────────────────────
void EntryPriority::set(const std::vector<Entry>& entries, int total, size_t capacity)
{
    auto table   = std::make_shared<Table>();
    table->total = total;

    // id 배열은 종목 테이블 용량만큼 — id가 용량을 넘지 않으므로 경계 검사가 index < size() 하나로 끝난다.
    size_t extent = capacity;

    for (const Entry& entry : entries)
    {
        extent = std::max<size_t>(extent, static_cast<size_t>(entry.symbol) + 1);
    }

    table->rank_by_symbol.assign(extent, 0);
    table->below_by_symbol.assign(extent, 0);
    table->z_by_symbol.assign(extent, 0.0);
    std::vector<symbol::SymbolId> symbols_by_rank; // 랭크 오름차순 id — below_by_symbol을 만들 때만 쓴다
    symbols_by_rank.reserve(entries.size());

    for (const Entry& entry : entries)
    {
        if (entry.symbol == symbol::kNone || entry.rank <= 0)
        {
            continue;
        }

        if (table->rank_by_symbol[entry.symbol] == 0)
        {
            symbols_by_rank.push_back(entry.symbol);
        }

        table->rank_by_symbol[entry.symbol] = entry.rank;
        table->z_by_symbol[entry.symbol]    = entry.z_score;
    }

    std::sort(symbols_by_rank.begin(), symbols_by_rank.end(),
              [&](symbol::SymbolId left, symbol::SymbolId right) {
                  return table->rank_by_symbol[left] < table->rank_by_symbol[right];
              });

    // 같은 랭크가 여럿이면 그 묶음의 첫 위치가 "나보다 위" 수다.
    for (size_t index = 0; index < symbols_by_rank.size(); ++index)
    {
        const symbol::SymbolId symbol = symbols_by_rank[index];
        int32_t                below  = static_cast<int32_t>(index);

        while (below > 0 && table->rank_by_symbol[symbols_by_rank[static_cast<size_t>(below) - 1]] ==
                                table->rank_by_symbol[symbol])
        {
            --below;
        }

        table->below_by_symbol[symbol] = below;
    }

    std::lock_guard<std::mutex> lock(priority_mutex_);
    table_ = std::move(table);
}

std::shared_ptr<const EntryPriority::Table> EntryPriority::snapshot() const
{
    std::lock_guard<std::mutex> lock(priority_mutex_);
    return table_;
}

bool EntryPriority::z_of(const Table& table, symbol::SymbolId symbol, double& z_score) noexcept
{
    if (rank_of(table, symbol) == 0)
    {
        return false;
    }

    z_score = table.z_by_symbol[symbol];
    return true;
}

// ─── 교체 기록 ───────────────────────────────────────────────────────────────
bool EntryPriority::append_decline(symbol::SymbolId symbol, std::string& text) const
{
    std::lock_guard<std::mutex> lock(displace_mutex_);
    const auto iterator = decline_.find(symbol);

    if (iterator == decline_.end())
    {
        return false;
    }

    text += " — 교체 보류: ";
    text += iterator->second;
    return true;
}

void EntryPriority::note_decline(symbol::SymbolId symbol, std::string why) const
{
    std::lock_guard<std::mutex> lock(displace_mutex_);
    decline_[symbol] = std::move(why);
}

void EntryPriority::clear_decline(symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(displace_mutex_);
    decline_.erase(symbol);
}

//  쿨다운: 방금 밀려난 종목이 곧장 되돌아오면 교체 비용만 왕복으로 나간다.
//  슬롯 예약: 교체로 비운 자리를 수혜 종목이 아닌 다른 종목이 가로채면 매도 비용만 치르고 사려던 종목은 또 못 산다.
EntryPriority::Admission EntryPriority::admit(symbol::SymbolId symbol, TimePoint now, symbol::SymbolId& reserved_for)
{
    std::lock_guard<std::mutex> lock(displace_mutex_);

    if (symbol < cooldown_until_.size())
    {
        TimePoint& cooldown_until = cooldown_until_[symbol];

        if (cooldown_until != TimePoint{})
        {
            if (now < cooldown_until)
            {
                return Admission::Cooling;
            }

            cooldown_until = TimePoint{};
        }
    }

    if (slot_reserved_for_ == symbol::kNone)
    {
        return Admission::Open;
    }

    if (now >= slot_reserved_until_)
    {
        slot_reserved_for_ = symbol::kNone;
    }
    else if (slot_reserved_for_ == symbol)
    {
        slot_reserved_for_ = symbol::kNone; // 수혜 종목이 자리를 가져갔다
    }
    else
    {
        reserved_for = slot_reserved_for_;
        return Admission::SlotReserved;
    }

    return Admission::Open;
}

std::string EntryPriority::refusal(symbol::SymbolId new_symbol, TimePoint now, int max_per_day,
                                   const symbol::SymbolTable& symbols) const
{
    std::lock_guard<std::mutex> lock(displace_mutex_);

    if (max_per_day > 0 && displace_count_ >= max_per_day)
    {
        return std::format("당일 교체 횟수 {}/{} 소진", displace_count_, max_per_day);
    }

    if (slot_reserved_for_ != symbol::kNone && now < slot_reserved_until_)
    {
        // 직전 교체로 비운 자리가 아직 안 찼다
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(slot_reserved_until_ - now).count();
        return std::format("비운 자리를 {}가 쓰는 중({}초 남음)", symbols.name(slot_reserved_for_).view(), left);
    }

    if (new_symbol < cooldown_until_.size())
    {
        const TimePoint cooldown_until = cooldown_until_[new_symbol];

        if (cooldown_until != TimePoint{} && now < cooldown_until)
        {
            // 방금 밀려난 종목이 곧장 되돌아오는 핑퐁 차단
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(cooldown_until - now).count();
            return std::format("밀려난 종목 재진입 대기({}초 남음)", left);
        }
    }

    return std::string();
}

void EntryPriority::note_displacement(symbol::SymbolId victim, symbol::SymbolId beneficiary, TimePoint now,
                                      int cooldown_sec, int slot_hold_sec, size_t capacity)
{
    std::lock_guard<std::mutex> lock(displace_mutex_);
    ++displace_count_;

    if (cooldown_sec > 0 && victim != symbol::kNone)
    {
        // 종목 테이블 용량만큼 한 번만 늘린다 — id는 용량을 넘지 않으므로 그 뒤로는 인덱스 대입뿐이다.
        if (victim >= cooldown_until_.size())
        {
            cooldown_until_.resize(std::max<size_t>(capacity, victim + 1));
        }

        cooldown_until_[victim] = now + std::chrono::seconds(cooldown_sec);
    }

    // 비운 슬롯을 수혜 종목에 예약한다. 예약이 없으면 매도 체결 직후 다른 종목이 가로채고,
    //  그러면 교체 비용만 치르고 정작 사려던 종목은 또 못 산다.
    slot_reserved_for_   = beneficiary;
    slot_reserved_until_ = now + std::chrono::seconds(slot_hold_sec > 0 ? slot_hold_sec : kDefaultSlotHoldSec);
}

void EntryPriority::reset_daily()
{
    std::lock_guard<std::mutex> lock(displace_mutex_);
    std::fill(cooldown_until_.begin(), cooldown_until_.end(), TimePoint{});
    slot_reserved_for_   = symbol::kNone;
    slot_reserved_until_ = TimePoint{};
    displace_count_      = 0;
}
