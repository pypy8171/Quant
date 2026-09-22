// 전략 id 테이블 — 전략 이름("DEVSCALE_A"·"MANUAL"·"FORCE_LIQ")을 기동 때 정수 하나로 바꾼다.
//  주문·체결 경로에서 전략을 키로 쓰는 곳(서브원장·중복 신호 키)은 이 번호를 쓴다. 문자열은 로그·CSV에만 남는다.
//  종목 테이블(core/SymbolTable.h)과 같은 규약이다 — 등록은 write_mutex_ 아래 한 번, 이름 조회는 락 없이. [why D-112]
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace strategy_table
{

using StrategyId = uint32_t;

// 0은 "전략 없음" — 신호에 전략을 안 붙인 경로(테스트·수동)가 이 값을 든다. 서브원장은 이 값이면 건너뛴다.
constexpr StrategyId kNone = 0;
constexpr size_t kDefaultCapacity = 1024; // 전략 수 상한 기본값 — 등록은 수십 개 규모

class StrategyTable
{
public:
    explicit StrategyTable(size_t capacity = kDefaultCapacity)
        : capacity_(capacity < 2 ? 2 : capacity), names_(std::make_unique<std::string[]>(capacity_))
    {
    }

    StrategyTable(const StrategyTable&)            = delete;
    StrategyTable& operator=(const StrategyTable&) = delete;

    // 이름을 등록하고 번호를 돌려준다. 이미 있으면 그 번호. 빈 이름·테이블 가득 참은 kNone.
    //  기동·전략 추가 때만 부른다 — 선형 탐색이라 hot path에서 부르지 않는다.
    StrategyId intern(std::string_view name);

    // 등록하지 않고 찾기만. 모르면 kNone.
    [[nodiscard]] StrategyId lookup(std::string_view name) const;

    // 번호 → 이름. 모르는 번호면 빈 문자열. 배열이 고정이라 참조가 테이블 수명 동안 유효하다.
    [[nodiscard]] const std::string& name(StrategyId id) const
    {
        return id < count_.load(std::memory_order_acquire) ? names_[id] : names_[kNone];
    }

    // 등록된 전략 수(id 0 제외).
    [[nodiscard]] size_t size() const
    {
        return count_.load(std::memory_order_acquire) - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    size_t                         capacity_;
    std::unique_ptr<std::string[]> names_; // [0]은 빈 문자열(kNone)
    std::atomic<StrategyId>        count_{1};
    mutable std::mutex             write_mutex_;
};

} // namespace strategy_table
