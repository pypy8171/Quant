// 프로세스 둘이 같이 보는 공유 쪽지 한 장 — 만들고, 붙고, 같은 판인지 본다. 큐도 레코드도 여기 없다.
//  주문·전략·시세가 다른 프로세스로 갈리면(D-114 단계 4·5) 그 사이를 오가는 면(큐·박동·장부 사본 등)이
//  전부 이 구역 위에 얹힌다 — 자리표는 Quant/include/ipc/SharedLayout.h 하나가 정한다. 운반 수단만 바뀌고
//  레코드는 한 프로세스일 때 쓰던 것 그대로다.
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

// 쪽지에 붙는 쪽이 누구인가. 만드는 쪽(주문)은 여기 없다 — 주인 칸은 머리에 따로 있다.
//  [inv] 값은 머리의 배열 첨자로 쓴다. 끝에만 더하고 더할 때 kSharedAttachRoleCount 를 같이 올린다.
enum class SharedAttachRole : uint8_t
{
    kStrategy = 0, // 전략 프로세스
    kFeed     = 1, // 시세 프로세스 — 소켓 하나를 쥐고 체결통보까지 받는다 [why D-114]
};

constexpr size_t kSharedAttachRoleCount = 2;

// 로그에 싣는 역할 이름. [inv] 돌려주는 조각은 정적 글자라 수명이 끝나지 않는다.
[[nodiscard]] std::string_view role_name(SharedAttachRole role) noexcept;

// 붙은 쪽 한 자리. 머리 안에 박히므로 고정 크기 정수만 둔다.
//  번호와 기동 시각을 같이 둔다 — 번호만 보면 죽은 쪽의 번호를 물려받은 남이 산 것으로 읽힌다.
struct SharedParticipant
{
    uint32_t process_id      = 0; // 0 = 아무도 안 붙었다
    uint32_t shutdown_reason = 0; // SharedShutdownReason. 0 = 안 적혔다 = 크래시
    uint64_t start_time      = 0;
    uint64_t reserved0       = 0; // 붙은 시각을 적던 칸. 읽는 곳이 없어 비웠다 — 크기는 그대로 둔다
    uint64_t reserved1       = 0;
};

static_assert(sizeof(SharedParticipant) == 32, "붙은 쪽 한 자리는 32바이트여야 머리가 캐시라인 두 줄로 떨어진다");

// 구역 머리. 붙는 쪽이 "내가 아는 판인가"를 이것만 보고 정한다 — 안 맞으면 붙지 않는다.
//  주문 쪽이 다시 떠 옛 쪽지를 물려받으면 boot_generation이 오르므로, 붙은 쪽은 자기가 붙었던 판이 갈린 것을 안다.
//  [inv] 고정 크기 정수만 둔다. 칸을 더할 때는 reserved를 쓰고 layout_version을 올린다.
struct SharedRegionHeader
{
    uint32_t magic          = 0; // kSharedRegionMagic. 남의 이름과 부딪힌 것을 먼저 거른다
    uint32_t layout_version = 0; // 구역 안 배치가 바뀌면 올린다. 다르면 안 붙는다
    uint64_t bytes          = 0; // 머리를 포함한 구역 전체 크기
    int64_t  created_at_ns  = 0; // 만든 시각(system_clock). 기록용이다 — 재기동 구분은 boot_generation이 한다
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
    // 붙은 쪽 자리 — 역할마다 하나다. 붙는 쪽이 둘(전략·시세)이 되면서 주인 칸 하나로는 누가 곱게 내려갔고
    //  누가 죽었는지를 못 가린다. 남은 쪽은 제 자리가 아닌 칸은 읽기만 한다. [why D-114]
    SharedParticipant attached[kSharedAttachRoleCount];
};

// shutdown_reason 에 들어가는 값. 0은 "안 적혔다"라서 뜻을 주지 않는다.
enum class SharedShutdownReason : uint32_t
{
    kNone        = 0, // 아직 안 내려갔다 — 남은 쪽은 크래시로 읽는다
    kSessionEnd  = 1, // 장 마감 자기 종료
    kOperator    = 2, // 사람·감시견이 내렸다(배포 재기동 포함)
    kStartupFail = 3, // 기동 중 접고 내려갔다
};

// 머리는 캐시라인 두 줄이다 — 뒤에 놓이는 큐의 제어 칸이 캐시라인 경계에서 시작해야 두 프로세스가
//  머리와 큐 칸을 두고 싸우지 않는다(payload()는 페이지 머리 + 128바이트라 64로 나뉜다).
static_assert(sizeof(SharedRegionHeader) == 2 * 64, "구역 머리는 캐시라인 두 줄이어야 한다");

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

    // 새로 만든다(주문 프로세스가 부른다). 같은 이름이 이미 있으면 머리에 적힌 주인이 아직 사는지 묻는다 —
    //  살아 있으면 실패한다(엔진 둘이 뜬 것을 여기서 잡는다). 주인이 이미 내려간 판이면 물려받아 기동 번호를 하나
    //  올린다(리눅스는 지우고 새로 만들고, 윈도우는 짝이 쥔 같은 매핑을 다시 쓴다). bytes는 머리를 포함한 크기다.
    bool create(std::string_view name, size_t bytes, uint32_t layout_version);

    // 이미 있는 구역에 붙는다(전략·시세 프로세스가 부른다). 머리의 magic·layout_version·bytes가 다르면
    //  붙지 않는다. role 은 제 자리를 고르는 값이다 — 붙자마자 그 칸에 제 번호와 기동 시각을 적는다.
    bool attach(std::string_view name, size_t bytes, uint32_t layout_version, SharedAttachRole role);

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

    // 주인이 아직 사는지 운영체제에 묻는다. 박동은 "멎었다"까지만 말하고, 멎은 것이 죽어서인지 굳어서인지는
    //  이것이 가른다. 지금 엔진은 부르지 않고 시험(test_shared_region)만 부른다.
    [[nodiscard]] bool creator_is_alive() const noexcept;

    // 이 이름으로 몇 번째 기동인가. 붙을 때 받은 값과 달라졌으면 주인이 다시 떠 이 쪽지를 물려받아 새로 민
    //  것이다 — 그때 붙은 쪽이 들고 있는 큐 자리는 전부 옛 것이라 그대로 쓰면 안 된다.
    [[nodiscard]] uint64_t boot_generation() const noexcept;

    // 0이면 아직 안 적혔다 — 주인이 죽었는데 이 값이 0이면 크래시다.
    [[nodiscard]] SharedShutdownReason shutdown_reason() const noexcept;

    // 이 손잡이가 붙은 역할. 주인이면 뜻이 없다(is_owner()를 먼저 본다).
    [[nodiscard]] SharedAttachRole attached_role() const noexcept
    {
        return role_;
    }

    // 그 역할이 이 판에 붙은 적이 있는가(번호가 적혔는가).
    [[nodiscard]] bool participant_attached(SharedAttachRole role) const noexcept;

    // 그 역할의 표(번호 + 기동 시각). 안 붙었으면 빈 표다.
    [[nodiscard]] ProcessIdentity participant_identity(SharedAttachRole role) const noexcept;

    // 그 역할이 아직 사는지 운영체제에 묻는다. 안 붙었으면 거짓이다.
    [[nodiscard]] bool participant_is_alive(SharedAttachRole role) const noexcept;

    // 그 역할의 종료 사유. kNone 인데 살아 있지 않으면 크래시다.
    [[nodiscard]] SharedShutdownReason participant_shutdown_reason(SharedAttachRole role) const noexcept;

    // 정상 종료 자리에서 마지막에 부른다. 주인이면 주인 칸에, 붙은 쪽이면 제 역할 칸에 적는다 —
    //  이 줄 뒤에 죽으면 남은 쪽이 크래시로 세지 않는다.
    void mark_clean_shutdown(SharedShutdownReason reason) noexcept;

    // create()가 물려받은 옛 쪽지에 종료 사유가 안 적혀 있었는가. 참이면 앞선 기동이 크래시로 끝났다는 뜻이라
    //  로그에 남긴다. 종료 사유가 적힌 옛 쪽지를 물려받았으면 거짓이고 기동 번호만 오른다.
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

    // 머리를 고쳐 쓰는 자리. 주인은 주인 칸을, 붙은 쪽은 제 역할 자리만 쓴다. const 판은 header()다.
    [[nodiscard]] SharedRegionHeader* mutable_header() noexcept;

    // 붙은 쪽 한 자리. 열려 있지 않거나 역할 값이 표 밖이면 nullptr이다.
    [[nodiscard]] const SharedParticipant* participant_of(SharedAttachRole role) const noexcept;

    void*            address_         = nullptr; // 매핑된 첫 바이트(= 머리)
    size_t           bytes_           = 0;
    bool             owner_           = false;
    bool             took_over_stale_ = false;
    SharedAttachRole role_            = SharedAttachRole::kStrategy; // 주인이면 뜻이 없다
    std::string      name_;
    std::string      last_error_;

#ifdef _WIN32
    void* mapping_handle_ = nullptr; // HANDLE. windows.h를 헤더로 끌어오지 않으려고 void*로 둔다
#else
    int descriptor_ = -1;
#endif
};

} // namespace ipc
