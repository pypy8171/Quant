// 프로세스 둘이 같이 보는 공유 쪽지 한 장 — 만들고, 붙고, 같은 판인지 본다. 큐도 레코드도 여기 없다.
//  전략과 주문·원장이 다른 프로세스로 갈리면(D-114 단계 4) 둘 사이를 오가는 것은 요청 큐·응답 큐·박동
//  셋뿐이고, 그 셋이 이 구역 위에 얹힌다. 운반 수단만 바뀌고 레코드는 한 프로세스일 때 쓰던 것 그대로다.
//  [inv] 구역에 올리는 것은 포인터 없는 고정 크기 값뿐이다 — 주소는 프로세스마다 다르다.
//  [why D-114]
#pragma once

#include "ipc/ProcessIdentity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ipc
{

// 구역 머리. 붙는 쪽이 "내가 아는 판인가"를 이것만 보고 정한다 — 안 맞으면 붙지 않는다.
//  주문 쪽이 재기동하면 created_at_ns가 바뀌므로, 전략 쪽은 자기가 붙었던 판이 갈린 것을 안다.
//  [inv] 고정 크기 정수만 둔다. 칸을 더할 때는 reserved를 쓰고 layout_version을 올린다.
struct SharedRegionHeader
{
    uint32_t magic          = 0; // kSharedRegionMagic. 남의 이름과 부딪힌 것을 먼저 거른다
    uint32_t layout_version = 0; // 구역 안 배치가 바뀌면 올린다. 다르면 안 붙는다
    uint64_t bytes          = 0; // 머리를 포함한 구역 전체 크기
    int64_t  created_at_ns  = 0; // 만든 시각(system_clock). 재기동을 구분하는 값이다
    uint32_t creator_process_id = 0;
    // 정상 종료가 마지막에 적는다(0 = 아직 안 적혔다 = 크래시). 남은 쪽이 이 칸 하나로 둘을 가른다.
    //  감시견의 session_done 표지는 날짜 단위라 같은 날 두 번째 기동의 죽음을 가리지 못한다 — 그래서 쪽지에 둔다.
    uint32_t shutdown_reason = 0; // SharedShutdownReason
    // creator_process_id 의 짝. 번호 재사용을 가린다 — 번호만 보면 죽은 주인의 번호를 물려받은 남이 주인으로 읽힌다.
    uint64_t creator_start_time = 0;
    // 이 이름으로 몇 번째 기동인가. 옛 판을 물려받으면 하나 오른다. 붙은 쪽은 기억해 둔 값과 달라지면 제 판이 갈린 것이다.
    uint64_t boot_generation = 0;
    uint64_t reserved3       = 0;
    uint64_t reserved4       = 0;
};

// shutdown_reason 에 들어가는 값. 0은 "안 적혔다"라서 뜻을 주지 않는다.
enum class SharedShutdownReason : uint32_t
{
    kNone        = 0, // 아직 안 내려갔다 — 남은 쪽은 크래시로 읽는다
    kSessionEnd  = 1, // 장 마감 자기 종료
    kOperator    = 2, // 사람·감시견이 내렸다(배포 재기동 포함)
    kStartupFail = 3, // 기동 중 접고 내려갔다
};

// 머리는 캐시라인 한 줄이다 — 뒤에 놓이는 큐의 제어 칸이 캐시라인 경계에서 시작해야 두 프로세스가
//  머리와 큐 칸을 두고 싸우지 않는다(payload()는 페이지 머리 + 64바이트라 64로 나뉜다).
static_assert(sizeof(SharedRegionHeader) == 64, "구역 머리는 캐시라인 한 줄이어야 한다");

constexpr uint32_t kSharedRegionMagic = 0x51'54'52'47; // 'QTRG'

// 이름 붙은 공유 구역 하나. 소유(만든 쪽)면 닫을 때 치우고, 붙은 쪽이면 지도만 접는다.
//  [inv] 복사하지 않는다 — 손잡이 하나에 매핑 하나다.
class SharedRegion
{
public:
    SharedRegion() = default;
    ~SharedRegion();

    SharedRegion(const SharedRegion&)            = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    SharedRegion(SharedRegion&& other) noexcept;
    SharedRegion& operator=(SharedRegion&& other) noexcept;

    // 새로 만든다(주문 프로세스가 부른다). 리눅스는 앞서 죽은 판이 /dev/shm에 남으므로 먼저 지우고 만든다.
    //  윈도우는 쥔 프로세스가 없으면 이름이 저절로 사라지므로, 이미 있다는 것은 아직 누가 쥐고 있다는 뜻
    //  이다 — 그때는 실패한다(엔진 둘이 뜬 것을 여기서 잡는다). bytes는 머리를 포함한 크기다.
    bool create(std::string_view name, size_t bytes, uint32_t layout_version);

    // 이미 있는 구역에 붙는다(전략 프로세스가 부른다). 머리의 magic·layout_version·bytes가 다르면 붙지 않는다.
    bool attach(std::string_view name, size_t bytes, uint32_t layout_version);

    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept
    {
        return address_ != nullptr;
    }

    // 만든 쪽인가(닫을 때 치우는 쪽). 붙은 쪽이면 거짓이다.
    [[nodiscard]] bool is_owner() const noexcept
    {
        return owner_;
    }

    // 구역 머리. 열려 있지 않으면 nullptr이다. [inv] 수명은 close()까지다.
    [[nodiscard]] const SharedRegionHeader* header() const noexcept;

    // 머리에 적힌 주인의 표(번호 + 기동 시각). 열려 있지 않으면 빈 표다.
    [[nodiscard]] ProcessIdentity creator_identity() const noexcept;

    // 주인이 아직 사는지 운영체제에 묻는다. 붙은 쪽이 주기로 불러 짝의 죽음을 안다 —
    //  박동은 "멎었다"까지만 말하고, 멎은 것이 죽어서인지 굳어서인지는 이것이 가른다.
    [[nodiscard]] bool creator_is_alive() const noexcept;

    // 이 이름으로 몇 번째 기동인가. 붙을 때 받은 값과 달라졌으면 주인이 다시 떠 쪽지를 새로 놓은 것이다 —
    //  그때 붙은 쪽이 들고 있는 큐 자리는 전부 옛 것이라 그대로 쓰면 안 된다.
    [[nodiscard]] uint64_t boot_generation() const noexcept;

    // 0이면 아직 안 적혔다 — 주인이 죽었는데 이 값이 0이면 크래시다.
    [[nodiscard]] SharedShutdownReason shutdown_reason() const noexcept;

    // 정상 종료 자리에서 마지막에 부른다(주인만). 이 줄 뒤에 죽으면 남은 쪽이 크래시로 세지 않는다.
    void mark_clean_shutdown(SharedShutdownReason reason) noexcept;

    // create()가 주인 없이 남아 있던 옛 쪽지를 물려받았는가. 참이면 앞선 기동이 크래시로 끝났다는 뜻이라
    //  로그에 남긴다(정상 종료였으면 쪽지가 남지 않거나 shutdown_reason이 적혀 있다).
    [[nodiscard]] bool took_over_stale() const noexcept
    {
        return took_over_stale_;
    }

    // 머리 뒤 첫 바이트 — 큐·박동이 여기부터 자리를 잡는다. 열려 있지 않으면 nullptr이다.
    [[nodiscard]] std::byte* payload() noexcept;
    [[nodiscard]] const std::byte* payload() const noexcept;

    // payload가 쓸 수 있는 바이트 수(구역 크기 − 머리).
    [[nodiscard]] size_t payload_bytes() const noexcept;

    [[nodiscard]] std::string_view name() const noexcept
    {
        return name_;
    }

    // 마지막 실패 사유 — 로그에 그대로 싣는다. 성공하면 빈 문자열이다.
    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

    // 머리가 차지하는 바이트. 구역 크기를 셈할 때 쓴다.
    [[nodiscard]] static constexpr size_t header_bytes()
    {
        return sizeof(SharedRegionHeader);
    }

private:
    // 플랫폼 손잡이를 닫는다. address_·handle_은 건드리지 않는다 — close()와 이동 대입이 같이 쓴다.
    void release_platform_handles() noexcept;

    // 머리를 고쳐 쓰는 자리(주인만). const 판은 header()다.
    [[nodiscard]] SharedRegionHeader* mutable_header() noexcept;

    void*       address_          = nullptr; // 매핑된 첫 바이트(= 머리)
    size_t      bytes_            = 0;
    bool        owner_            = false;
    bool        took_over_stale_  = false;
    std::string name_;
    std::string last_error_;

#ifdef _WIN32
    void* mapping_handle_ = nullptr; // HANDLE. windows.h를 헤더로 끌어오지 않으려고 void*로 둔다
#else
    int descriptor_ = -1;
#endif
};

} // namespace ipc
