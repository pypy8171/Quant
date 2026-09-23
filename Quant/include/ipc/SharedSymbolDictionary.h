// 프로세스 둘이 같이 보는 종목 표 — 공유 쪽지 위에 놓인 버킷 배열·이름 배열 한 벌.
//  전략과 주문이 갈리면 같은 종목에 같은 번호가 붙어야 한다. 주문 요청이 종목을 정수 번호로 나르기
//  때문이다(D-071 원칙 6) — 양쪽이 각자 번호를 찍으면 전략 쪽 3번과 주문 쪽 3번이 다른 종목이 되고,
//  그건 엉뚱한 종목에 주문이 나가는 것이다. [why D-114]
//  해시·탐사·발행 순서는 symbol::SymbolTable과 같은 한 벌을 쓴다(symbol::table_lookup·table_insert).
//  알고리즘이 두 벌이 되면 한쪽만 고쳐지고 그 순간 양쪽 번호가 갈린다.
//  [inv] 넣는 쪽은 주문 프로세스 하나뿐이고, 전략 프로세스는 읽기만 한다. 그래서 쓰기 자물쇠를 건너편이
//  쥔 채 죽어도 읽는 쪽은 멈추지 않는다 — 읽기 경로는 자물쇠를 건드리지 않는다.
#pragma once

#include "core/SymbolTable.h"
#include "ipc/SharedSpscRing.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ipc
{

// 표 머리. 붙는 쪽이 "같은 표인가"를 이것만 보고 정한다.
constexpr uint32_t kSharedDictionaryMagic = 0x51'44'43'54; // 'QDCT'

// 표 하나의 제어 칸. 다음 번호와 쓰기 자물쇠는 각각 제 캐시라인을 차지한다 — 번호를 읽는 쪽과 넣는 쪽이
//  같은 줄을 두고 싸우면 프로세스 사이에서는 스레드 사이보다 비싸다.
//  [inv] 고정 크기 정수만 둔다. 포인터는 없다 — 주소는 프로세스마다 다르다.
struct SharedDictionaryControl
{
    uint32_t magic        = 0;
    uint32_t record_bytes = 0; // sizeof(symbol::Ticker). 배치가 다른 exe를 기동 자리에서 거른다
    uint64_t capacity     = 0; // 번호 상한(0 제외)
    uint64_t bucket_count = 0; // 2의 거듭제곱
    uint64_t reserved     = 0;

    alignas(kSharedCacheLine) std::atomic<symbol::SymbolId> count{1}; // 다음에 줄 번호
    alignas(kSharedCacheLine) std::atomic<uint32_t> write_lock{0};    // 0 = 빈 자리, 1 = 누가 넣는 중
};

// 공유 쪽지 한 구획 위에 표를 놓거나, 이미 놓인 표에 붙는다.
//  [inv] 복사하지 않는다 — 손잡이 하나에 구획 하나다.
class SharedSymbolDictionary
{
public:
    SharedSymbolDictionary()                                         = default;
    SharedSymbolDictionary(const SharedSymbolDictionary&)            = delete;
    SharedSymbolDictionary& operator=(const SharedSymbolDictionary&) = delete;

    // capacity개 종목이 드는 표가 차지하는 바이트. 구역 크기를 셈할 때 쓴다.
    [[nodiscard]] static size_t bytes_for(size_t capacity);

    // 표를 새로 놓는다(구역을 만든 주문 쪽이 한 번 부른다). base는 캐시라인 경계여야 한다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, size_t capacity);

    // 이미 놓인 표에 붙는다(전략 쪽이 부른다). 머리가 다르면 붙지 않는다.
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, size_t capacity);

    void unbind() noexcept;

    [[nodiscard]] bool is_bound() const noexcept
    {
        return control_ != nullptr;
    }

    // 있으면 그 번호, 없으면 새 번호. 가득 차거나 안 붙었으면 kNone.
    //  [inv] 주문 프로세스만 부른다. 그 안에서는 스레드가 여럿이라 자물쇠로 직렬화한다.
    symbol::SymbolId intern(std::string_view ticker);

    // 읽기 — 자물쇠를 잡지 않는다. 전략 프로세스가 쓰는 길이다.
    [[nodiscard]] symbol::SymbolId lookup(std::string_view ticker) const
    {
        return symbol::table_lookup(slots_, ticker);
    }

    [[nodiscard]] symbol::Ticker name(symbol::SymbolId id) const
    {
        return symbol::table_name(slots_, id);
    }

    // 등록된 종목 수(번호 0 제외).
    [[nodiscard]] size_t size() const;

    [[nodiscard]] size_t capacity() const noexcept
    {
        return slots_.capacity;
    }

    // 마지막 실패 사유 — 로그에 그대로 싣는다. 성공하면 빈 문자열이다.
    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

private:
    // base·bytes·capacity가 표를 놓을 만한지 본다. 아니면 last_error_에 사유를 남긴다.
    [[nodiscard]] bool check_arguments(const std::byte* base, size_t bytes, size_t capacity);

    // 제어 칸 뒤에 버킷·이름 배열 자리를 잡아 slots_에 채운다.
    void bind(std::byte* base, size_t capacity);

    SharedDictionaryControl* control_ = nullptr;
    symbol::TableSlots       slots_;
    std::string              last_error_;
};

} // namespace ipc
