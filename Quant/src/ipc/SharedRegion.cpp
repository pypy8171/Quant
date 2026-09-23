// SharedRegion.h 구현 — 이름 붙은 공유메모리를 만들고 붙고 닫는다. 플랫폼 갈래는 HTTP 전송(KisTransport.cpp)과
//  같은 방식으로 이 파일 안 #ifdef 하나로 둔다(WebSocket처럼 파일을 가를 만큼 길지 않다).
#include "ipc/SharedRegion.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ipc
{
namespace
{
int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 머리에 적힌 주인을 표 한 장으로 되돌린다 — 번호와 기동 시각을 같이 봐야 번호 재사용에 안 속는다.
ProcessIdentity creator_of(const SharedRegionHeader& header)
{
    ProcessIdentity identity;
    identity.process_id = header.creator_process_id;
    identity.start_time = header.creator_start_time;
    return identity;
}

// 머리가 성한 구역만 쓴다 — 이름이 남과 부딪혔거나, 판이 바뀐 채 남아 있는 것을 여기서 거른다.
bool header_matches(const SharedRegionHeader& header, size_t bytes, uint32_t layout_version)
{
    return header.magic == kSharedRegionMagic && header.layout_version == layout_version &&
           header.bytes == bytes;
}

#ifndef _WIN32
// 구역 파일 권한 — 만든 사용자만 읽고 쓴다. 트레이더와 전략은 같은 계정으로 돈다(감시견이 띄운다).
constexpr int kRegionPermissions = 0600;

// 남아 있는 구역의 머리만 떠 온다. 성한 머리를 못 읽으면 거짓이고, out은 건드리지 않는다.
bool read_posix_header(const std::string& posix_name, SharedRegionHeader& out)
{
    const int descriptor = shm_open(posix_name.c_str(), O_RDONLY, kRegionPermissions);

    if (descriptor < 0)
    {
        return false;
    }

    void* address = mmap(nullptr, sizeof(SharedRegionHeader), PROT_READ, MAP_SHARED, descriptor, 0);
    ::close(descriptor);

    if (address == MAP_FAILED)
    {
        return false;
    }

    const auto* header = static_cast<const SharedRegionHeader*>(address);
    const bool  usable = header->magic == kSharedRegionMagic;

    if (usable)
    {
        std::memcpy(&out, header, sizeof(SharedRegionHeader));
    }

    munmap(address, sizeof(SharedRegionHeader));
    return usable;
}
#endif
} // namespace

SharedRegion::~SharedRegion()
{
    close();
}

SharedRegion::SharedRegion(SharedRegion&& other) noexcept
    : address_(other.address_)
    , bytes_(other.bytes_)
    , owner_(other.owner_)
    , took_over_stale_(other.took_over_stale_)
    , name_(std::move(other.name_))
    , last_error_(std::move(other.last_error_))
#ifdef _WIN32
    , mapping_handle_(other.mapping_handle_)
#else
    , descriptor_(other.descriptor_)
#endif
{
    other.address_         = nullptr;
    other.bytes_           = 0;
    other.owner_           = false;
    other.took_over_stale_ = false;
#ifdef _WIN32
    other.mapping_handle_ = nullptr;
#else
    other.descriptor_ = -1;
#endif
}

SharedRegion& SharedRegion::operator=(SharedRegion&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    close();

    address_         = other.address_;
    bytes_           = other.bytes_;
    owner_           = other.owner_;
    took_over_stale_ = other.took_over_stale_;
    name_            = std::move(other.name_);
    last_error_      = std::move(other.last_error_);
#ifdef _WIN32
    mapping_handle_       = other.mapping_handle_;
    other.mapping_handle_ = nullptr;
#else
    descriptor_       = other.descriptor_;
    other.descriptor_ = -1;
#endif
    other.address_         = nullptr;
    other.bytes_           = 0;
    other.owner_           = false;
    other.took_over_stale_ = false;
    return *this;
}

bool SharedRegion::create(std::string_view name, size_t bytes, uint32_t layout_version)
{
    close();
    last_error_.clear();

    if (name.empty() || bytes < header_bytes())
    {
        last_error_ = "이름이 비었거나 크기가 머리보다 작다";
        return false;
    }

    name_            = std::string(name);
    bytes_           = bytes;
    owner_           = true;
    took_over_stale_ = false;

    // 물려받은 판이 있으면 그 번호 다음을 쓴다 — 붙어 있던 쪽이 "내 판이 갈렸다"를 이 값으로 안다.
    uint64_t previous_generation = 0;

#ifdef _WIN32
    const HANDLE handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                             static_cast<DWORD>(bytes), name_.c_str());

    if (handle == nullptr)
    {
        last_error_ = "CreateFileMapping 실패 code=" + std::to_string(GetLastError());
        close();
        return false;
    }

    // 윈도우 이름 있는 쪽지는 **마지막 핸들이 닫힐 때** 사라진다. 그래서 "이미 있다"는 두 가지다 —
    //  ① 엔진이 정말 둘 떠 있거나, ② 주인은 죽었는데 짝이 핸들을 쥐고 있어 죽기 직전 값 그대로 남은 것이거나.
    //  둘을 안 가르면 ②에서 재기동이 통째로 막히고, 안 가른 채 그냥 쓰면 옛 큐 자리에 붙는다. 주인에게 묻는다.
    //  [why D-114]
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        SharedRegionHeader existing{};
        void*              peek = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(SharedRegionHeader));

        if (peek != nullptr)
        {
            std::memcpy(&existing, peek, sizeof(SharedRegionHeader));
            UnmapViewOfFile(peek);
        }

        if (existing.magic != kSharedRegionMagic || process_is_alive(creator_of(existing)))
        {
            CloseHandle(handle);
            last_error_ = "같은 이름의 공유 구역을 이미 누가 쥐고 있다 — 엔진이 둘 떠 있는지 본다";
            close();
            return false;
        }

        previous_generation = existing.boot_generation;
        took_over_stale_    = existing.shutdown_reason == 0;
    }

    mapping_handle_ = handle;
    address_        = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, bytes);

    if (address_ == nullptr)
    {
        last_error_ = "MapViewOfFile 실패 code=" + std::to_string(GetLastError());
        close();
        return false;
    }
#else
    const std::string posix_name = "/" + name_;
    descriptor_                  = shm_open(posix_name.c_str(), O_CREAT | O_EXCL | O_RDWR, kRegionPermissions);

    // 리눅스는 쥔 프로세스가 없어도 이름이 /dev/shm에 남는다. 남아 있으면 만든 쪽이 아직 사는지 묻고,
    //  살아 있으면 실패(엔진 둘), 죽은 판이면 치우고 새로 만든다. 윈도우 갈래와 묻는 것이 같아졌다 —
    //  거기서는 "이름이 남아 있다"가 "짝이 핸들을 쥐고 있다"는 뜻이라 주인에게 따로 물어야 했다.
    if (descriptor_ < 0 && errno == EEXIST)
    {
        SharedRegionHeader existing{};
        const bool         readable = read_posix_header(posix_name, existing);

        if (readable && process_is_alive(creator_of(existing)))
        {
            last_error_ = "같은 이름의 공유 구역을 이미 누가 쥐고 있다 — 엔진이 둘 떠 있는지 본다";
            owner_      = false; // 내가 만든 판이 아니다 — 닫으면서 남의 이름을 지우면 안 된다
            close();
            return false;
        }

        if (readable)
        {
            previous_generation = existing.boot_generation;
            took_over_stale_    = existing.shutdown_reason == 0;
        }

        shm_unlink(posix_name.c_str());
        descriptor_ = shm_open(posix_name.c_str(), O_CREAT | O_EXCL | O_RDWR, kRegionPermissions);
    }

    if (descriptor_ < 0)
    {
        last_error_ = "shm_open 실패 errno=" + std::to_string(errno);
        close();
        return false;
    }

    if (ftruncate(descriptor_, static_cast<off_t>(bytes)) != 0)
    {
        last_error_ = "ftruncate 실패 errno=" + std::to_string(errno);
        close();
        return false;
    }

    void* address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);

    if (address == MAP_FAILED)
    {
        last_error_ = "mmap 실패 errno=" + std::to_string(errno);
        close();
        return false;
    }

    address_ = address;
#endif

    // 머리를 먼저 지우고 쓴다 — 붙는 쪽은 magic을 보고 들어오므로 magic이 마지막이어야 반쪽 머리를 안 본다.
    std::memset(address_, 0, bytes);
    const ProcessIdentity identity = current_process_identity();
    auto*                 header   = static_cast<SharedRegionHeader*>(address_);
    header->layout_version         = layout_version;
    header->bytes                  = bytes;
    header->created_at_ns          = now_ns();
    header->creator_process_id     = identity.process_id;
    header->creator_start_time     = identity.start_time;
    header->boot_generation        = previous_generation + 1;
    header->shutdown_reason        = static_cast<uint32_t>(SharedShutdownReason::kNone);
    header->magic                  = kSharedRegionMagic;
    return true;
}

bool SharedRegion::attach(std::string_view name, size_t bytes, uint32_t layout_version)
{
    close();
    last_error_.clear();

    if (name.empty() || bytes < header_bytes())
    {
        last_error_ = "이름이 비었거나 크기가 머리보다 작다";
        return false;
    }

    name_  = std::string(name);
    bytes_ = bytes;
    owner_ = false;

#ifdef _WIN32
    mapping_handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());

    if (mapping_handle_ == nullptr)
    {
        last_error_ = "OpenFileMapping 실패 code=" + std::to_string(GetLastError());
        close();
        return false;
    }

    address_ = MapViewOfFile(mapping_handle_, FILE_MAP_ALL_ACCESS, 0, 0, bytes);

    if (address_ == nullptr)
    {
        last_error_ = "MapViewOfFile 실패 code=" + std::to_string(GetLastError());
        close();
        return false;
    }
#else
    const std::string posix_name = "/" + name_;
    descriptor_                  = shm_open(posix_name.c_str(), O_RDWR, kRegionPermissions);

    if (descriptor_ < 0)
    {
        last_error_ = "shm_open 실패 errno=" + std::to_string(errno);
        close();
        return false;
    }

    void* address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);

    if (address == MAP_FAILED)
    {
        last_error_ = "mmap 실패 errno=" + std::to_string(errno);
        close();
        return false;
    }

    address_ = address;
#endif

    const auto* header = static_cast<const SharedRegionHeader*>(address_);

    if (!header_matches(*header, bytes, layout_version))
    {
        last_error_ = "구역 머리가 다르다 — magic=" + std::to_string(header->magic) +
                      " 판=" + std::to_string(header->layout_version) +
                      " 크기=" + std::to_string(header->bytes);
        close();
        return false;
    }

    return true;
}

void SharedRegion::close() noexcept
{
#ifdef _WIN32
    if (address_ != nullptr)
    {
        UnmapViewOfFile(address_);
    }
#else
    if (address_ != nullptr)
    {
        munmap(address_, bytes_);
    }
#endif

    release_platform_handles();
    address_ = nullptr;
    bytes_   = 0;
    owner_   = false;
    name_.clear();
}

void SharedRegion::release_platform_handles() noexcept
{
#ifdef _WIN32
    if (mapping_handle_ != nullptr)
    {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
#else
    if (descriptor_ >= 0)
    {
        ::close(descriptor_);
        descriptor_ = -1;
    }

    // 만든 쪽만 이름을 치운다. 붙은 쪽이 치우면 아직 쓰는 쪽이 있는 판을 지운다.
    if (owner_ && !name_.empty())
    {
        shm_unlink(("/" + name_).c_str());
    }
#endif
}

const SharedRegionHeader* SharedRegion::header() const noexcept
{
    return static_cast<const SharedRegionHeader*>(address_);
}

SharedRegionHeader* SharedRegion::mutable_header() noexcept
{
    return static_cast<SharedRegionHeader*>(address_);
}

ProcessIdentity SharedRegion::creator_identity() const noexcept
{
    const SharedRegionHeader* head = header();

    if (head == nullptr)
    {
        return ProcessIdentity{};
    }

    return creator_of(*head);
}

bool SharedRegion::creator_is_alive() const noexcept
{
    return process_is_alive(creator_identity());
}

// 아래 둘은 건너편이 도는 중에 읽고 쓰는 칸이다 — 주인이 옛 판을 물려받아 번호를 올리는 순간에도 짝은 붙어
//  있다. 그래서 머리 안에 원자 타입을 박지 않고(머리는 고정 크기 정수만 두는 자리다) 읽고 쓰는 자리에서
//  atomic_ref로 감싼다. [inv] boot_generation·shutdown_reason은 이 네 함수 밖에서 직접 건드리지 않는다.
uint64_t SharedRegion::boot_generation() const noexcept
{
    const SharedRegionHeader* head = header();

    if (head == nullptr)
    {
        return 0;
    }

    // const_cast — 공유 쪽지 위의 칸이라 이 객체가 const여도 건너편은 쓰고 있다. 여기서는 읽기만 한다.
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(head->boot_generation)).load(std::memory_order_acquire);
}

SharedShutdownReason SharedRegion::shutdown_reason() const noexcept
{
    const SharedRegionHeader* head = header();

    if (head == nullptr)
    {
        return SharedShutdownReason::kNone;
    }

    return static_cast<SharedShutdownReason>(
        std::atomic_ref<uint32_t>(const_cast<uint32_t&>(head->shutdown_reason)).load(std::memory_order_acquire));
}

void SharedRegion::mark_clean_shutdown(SharedShutdownReason reason) noexcept
{
    SharedRegionHeader* head = mutable_header();

    if (head == nullptr || !owner_ || reason == SharedShutdownReason::kNone)
    {
        return;
    }

    // 먼저 적은 사유가 남는다 — stop() 은 두 번 불릴 수 있고(소멸자가 또 부른다) 뒤엣것은 왜 내려갔는지를
    //  모른다. 덮어쓰면 마감 자기 종료가 기동 실패로 바뀐다.
    uint32_t expected = static_cast<uint32_t>(SharedShutdownReason::kNone);
    std::atomic_ref<uint32_t>(head->shutdown_reason)
        .compare_exchange_strong(expected, static_cast<uint32_t>(reason), std::memory_order_release,
                                 std::memory_order_relaxed);
}

std::byte* SharedRegion::payload() noexcept
{
    if (address_ == nullptr)
    {
        return nullptr;
    }

    return static_cast<std::byte*>(address_) + header_bytes();
}

const std::byte* SharedRegion::payload() const noexcept
{
    if (address_ == nullptr)
    {
        return nullptr;
    }

    return static_cast<const std::byte*>(address_) + header_bytes();
}

size_t SharedRegion::payload_bytes() const noexcept
{
    if (address_ == nullptr)
    {
        return 0;
    }

    return bytes_ - header_bytes();
}

} // namespace ipc
