// 전략 id 테이블 — 전략 이름("DEVSCALE_005930"·"MANUAL"·"FORCE_LIQ")을 기동 때 정수 하나로 바꾼다.
//  주문·체결 경로에서 전략을 키로 쓰는 곳(서브원장·중복 신호 키)은 이 번호를 쓴다. 문자열은 로그·CSV에만 남는다.
//  종목 테이블(core/SymbolTable.h)과 같은 규약이다 — 등록은 write_mutex_ 아래 한 번, 이름 조회는 락 없이. [why D-112]
//  알맹이(이름 배열·다음 번호)는 TableSlots로 떼어 두었다. 힙에 두면 이 표이고, 공유 쪽지에 두면
//  ipc::SharedStrategyDictionary다 — 프로세스를 갈라도 같은 전략에 같은 번호가 붙어야 해서다. [why D-114]
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace strategy_table
{

using StrategyId = uint32_t;

// 0은 "전략 없음" — 신호에 전략을 안 붙인 경로(테스트·수동)가 이 값을 든다. 서브원장은 이 값이면 건너뛴다.
constexpr StrategyId kNone = 0;
constexpr size_t kDefaultCapacity = 1024; // 전략 수 상한 기본값 — 등록은 수십~수백 개 규모(재스캔이 종목마다 하나)

// 표가 드는 전략 이름 — std::string 대신 고정 배열이다. 공유 쪽지에는 포인터를 올릴 수 없고(주소는
//  프로세스마다 다르다) 이름 배열이 통째로 그 위에 놓이기 때문이다. [why D-114]
struct StrategyName
{
    static constexpr size_t kMax = 31; // "DEVSCALE_005930"이 15자다 — 설정 접두가 길어질 자리를 둔다

    char    data[kMax] = {};
    uint8_t length     = 0;

    constexpr StrategyName() = default;

    // 넘치면 잘린다 — 넣는 쪽이 fits()로 먼저 거르므로 잘린 이름이 표에 들어가지는 않는다.
    void assign(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const noexcept
    {
        return {data, length};
    }

    [[nodiscard]] std::string string() const
    {
        return {data, length};
    }
};

static_assert(sizeof(StrategyName) == StrategyName::kMax + 1, "이름 칸에 빈틈이 생기면 공유 쪽지 배치가 어긋난다");

// 이름이 칸에 드는가. 안 들면 등록을 거절한다 — 잘라서 넣으면 접두가 같은 전략 둘이 한 번호를 쓰고
//  서브원장 귀속이 섞인다. 종목 코드와 달리 전략 이름은 설정이 정하므로 길이 상한이 없다.
[[nodiscard]] bool fits(std::string_view name) noexcept;

// 표의 알맹이 — 이름 배열과 다음 번호. 힙이든 공유 쪽지든 이 셋만 가리키면 아래 함수들이 그대로 돈다.
//  [inv] 배열은 만든 뒤 재할당하지 않는다. 그래서 읽기가 락 없이 간다.
struct TableSlots
{
    StrategyName*            names    = nullptr;
    std::atomic<StrategyId>* count    = nullptr;
    size_t                   capacity = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return names == nullptr;
    }
};

// 찾기만 한다. 모르면 kNone. 자물쇠를 잡지 않는다.
[[nodiscard]] StrategyId table_lookup(const TableSlots& slots, std::string_view name);

// 있으면 그 번호, 없으면 새 번호. 빈 이름·칸을 넘는 이름·가득 찬 표는 kNone.
//  [inv] 부르는 쪽이 쓰기를 직렬화한다 — 힙 표는 뮤텍스, 공유 표는 쪽지 위 자물쇠다.
[[nodiscard]] StrategyId table_insert(const TableSlots& slots, std::string_view name);

// 번호 → 이름. 모르는 번호면 빈 이름. 값으로 돌려준다 — 공유 표는 건너편이 떼면 배열이 사라진다.
[[nodiscard]] StrategyName table_name(const TableSlots& slots, StrategyId id);

class StrategyTable
{
public:
    explicit StrategyTable(size_t capacity = kDefaultCapacity);

    StrategyTable(const StrategyTable&)            = delete;
    StrategyTable& operator=(const StrategyTable&) = delete;

    // 이름을 등록하고 번호를 돌려준다. 이미 있으면 그 번호. 빈 이름·너무 긴 이름·테이블 가득 참은 kNone.
    //  기동·재스캔 때만 부른다 — 선형 탐색이라 hot path에서 부르지 않는다.
    StrategyId intern(std::string_view name);

    // 표 알맹이를 남이 놓은 것으로 바꾼다 — 공유 쪽지 위 표(ipc::SharedStrategyDictionary)를 엔진이 꽂는다.
    //  종목 표(core/SymbolTable.h)의 adopt와 같은 규약이다. [why D-114]
    //  [inv] 스레드가 뜨기 전에만 부른다. 힙 배열은 여기서 놓는다.
    void adopt(const TableSlots& slots, std::function<StrategyId(std::string_view)> register_hook);

    // 등록하지 않고 찾기만. 모르면 kNone.
    [[nodiscard]] StrategyId lookup(std::string_view name) const
    {
        return table_lookup(slots_, name);
    }

    // 번호 → 이름. 모르는 번호면 빈 이름.
    [[nodiscard]] StrategyName name(StrategyId id) const
    {
        return table_name(slots_, id);
    }

    // 등록된 전략 수(id 0 제외). 세는 칸은 slots_가 가리키는 것이다 — adopt로 공유 표를 꽂았으면
    //  쪽지 위 칸이고, 그러지 않았으면 아래 count_다.
    [[nodiscard]] size_t size() const
    {
        return slots_.count->load(std::memory_order_acquire) - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return slots_.capacity;
    }

private:
    std::unique_ptr<StrategyName[]> names_; // [0]은 빈 이름(kNone)
    std::atomic<StrategyId>         count_{1};
    TableSlots                      slots_;
    mutable std::mutex              write_mutex_;
    // 비어 있지 않으면 표가 남의 것이다 — 넣기를 이쪽에 넘긴다(adopt).
    std::function<StrategyId(std::string_view)> register_hook_;
};

} // namespace strategy_table
