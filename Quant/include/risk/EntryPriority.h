#pragma once
#include "core/SymbolTable.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// EntryPriority — 진입 우선순위 표와 교체 진입 기록
//
//  우선순위 표는 유니버스 스캔이 낸 종목별 랭크·z를 종목 id 배열로 굳힌 불변 스냅샷이다. 재스캔이 새 표를 만들어
//  통째로 바꿔 끼우고, 읽는 쪽(check·plan_displacement)은 포인터만 복사해 락 밖에서 읽는다. [why D-112]
//  교체 기록은 교체 진입(D-019)이 남기는 것 — 밀려난 종목의 재진입 쿨다운, 비운 슬롯의 수혜 종목 예약,
//  종목별 직전 교체 거절 사유, 당일 교체 횟수. OrderGate에서 떼어 냈다 — 원장과 섞여 있으면 교체 규칙을
//  고칠 때 원장 코드를 같이 읽어야 했다. 원장과 표를 함께 보는 판정(check·plan_displacement)은 OrderGate에 남는다.
//
// 쓰는 스레드는 둘이다 — 표는 스캔을 받는 Engine 스레드(set), 교체 기록은 주문 스레드(check·plan·note).
//   표와 교체 기록은 락을 따로 든다(priority_mutex_·displace_mutex_). 둘 다 잎 잠금이다 — 이 클래스는 쥔 채 다른
//   락을 잡지 않는다.
// [lock-order] OrderGate::check()는 원장 락(PositionLedger::Reader가 쥔 positions_mutex_)을 쥔 채
//   여기의 snapshot·append_decline·admit을 부른다
//   (positions → {priority, displace}). 반대로 여기 락을 쥔 채 원장 락을 잡는 경로는 없다.
// ─────────────────────────────────────────────────────────────────────────────
class EntryPriority
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 슬롯 예약 기본 시간 — config의 displace_slot_hold_sec가 0 이하일 때 쓴다.
    static constexpr int kDefaultSlotHoldSec = 120;

    struct Entry
    {
        symbol::SymbolId symbol  = symbol::kNone;
        int              rank    = 0;   // 1=최고
        double           z_score = 0.0; // 종합점수 표준화값
    };

    struct Table
    {
        std::vector<int32_t> rank_by_symbol;  // id → 랭크(0=없음)
        std::vector<int32_t> below_by_symbol; // id → 나보다 랭크가 낮은(점수 높은) 표 항목 수
        std::vector<double>  z_by_symbol;     // id → 종합점수 z
        int                  total = 0;       // 랭크 모집단 크기(등록 종목 수)
    };

    // 3c-1 판정 결과 — 교체 쿨다운 중, 다른 종목에 예약된 슬롯, 통과.
    enum class Admission
    {
        Open,
        Cooling,
        SlotReserved,
    };

    EntryPriority()                                = default;
    EntryPriority(const EntryPriority&)            = delete;
    EntryPriority& operator=(const EntryPriority&) = delete;

    // ── 우선순위 표 ──────────────────────────────────────────────────────────
    // 표를 새로 굳혀 바꿔 끼운다. capacity는 종목 테이블 용량 — id 배열을 그만큼 잡아 경계 검사가 index < size() 하나로 끝난다.
    void set(const std::vector<Entry>& entries, int total, size_t capacity);
    [[nodiscard]] std::shared_ptr<const Table> snapshot() const;
    // 모르는 종목이면 0. check()의 원장 순회가 항목마다 부른다.
    [[nodiscard]] static int rank_of(const Table& table, symbol::SymbolId symbol) noexcept
    {
        return symbol < table.rank_by_symbol.size() ? table.rank_by_symbol[symbol] : 0;
    }

    // 표에 있으면 z를 채우고 참.
    [[nodiscard]] static bool z_of(const Table& table, symbol::SymbolId symbol, double& z_score) noexcept;

    // ── 교체 기록 ────────────────────────────────────────────────────────────
    // 직전 교체 거절 사유가 있으면 " — 교체 보류: <사유>"를 text 뒤에 붙이고 참.
    bool append_decline(symbol::SymbolId symbol, std::string& text) const;
    // 거절 사유 기록은 계획만 세우는 plan_displacement(const)가 남기므로 const다 — 사유 맵은 판정 상태가 아니라 문구 캐시다.
    void note_decline(symbol::SymbolId symbol, std::string why) const;
    void clear_decline(symbol::SymbolId symbol) const;
    // 3c-1 — 교체 쿨다운·비운 슬롯 예약. 지난 쿨다운·만료되거나 수혜 종목이 가져간 예약은 여기서 지운다.
    //  SlotReserved면 reserved_for에 예약을 든 종목을 적는다.
    [[nodiscard]] Admission admit(symbol::SymbolId symbol, TimePoint now, symbol::SymbolId& reserved_for);
    // plan_displacement (2) — 당일 교체 횟수·슬롯 예약·재진입 쿨다운 중 막는 것이 있으면 그 사유, 없으면 빈 문자열.
    [[nodiscard]] std::string refusal(symbol::SymbolId new_symbol, TimePoint now, int max_per_day,
                                      const symbol::SymbolTable& symbols) const;
    // 교체 한 건을 적는다 — 당일 횟수 +1, 밀려난 종목 쿨다운(cooldown_sec > 0일 때), 비운 슬롯을 수혜 종목에 예약.
    //  slot_hold_sec가 0 이하면 kDefaultSlotHoldSec. capacity는 쿨다운 배열을 처음 늘릴 때의 크기.
    void note_displacement(symbol::SymbolId victim, symbol::SymbolId beneficiary, TimePoint now, int cooldown_sec,
                           int slot_hold_sec, size_t capacity);
    // 장 시작 — 쿨다운·예약·당일 횟수를 지운다. 표와 거절 사유는 그대로 둔다.
    void reset_daily();

private:
    mutable std::mutex           priority_mutex_;
    std::shared_ptr<const Table> table_; // nullptr이면 표 없음(우선순위 바 미동작)

    mutable std::mutex     displace_mutex_;
    std::vector<TimePoint> cooldown_until_;                 // 밀려난 종목 id → 재진입 허용 시각(기본값 = 없음). 종목 테이블 용량만큼
    symbol::SymbolId       slot_reserved_for_ = symbol::kNone; // 비운 슬롯을 쓸 종목(다른 종목이 가로채지 못하게)
    TimePoint              slot_reserved_until_{};          // 예약 만료 시각
    mutable std::unordered_map<symbol::SymbolId, std::string> decline_; // 신규 종목 id → 직전 교체 거절 사유(거부 문구용)
    int                    displace_count_ = 0;             // 당일 교체 횟수(reset_daily에서 0으로)
};
