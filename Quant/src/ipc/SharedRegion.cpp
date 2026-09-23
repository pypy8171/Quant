// SharedRegion.h 구현 — 이름 붙은 공유메모리를 만들고 붙고 닫는다. 플랫폼 갈래는 HTTP 전송(KisTransport.cpp)과
//  같은 방식으로 이 파일 안 #ifdef 하나로 둔다(WebSocket처럼 파일을 가를 만큼 길지 않다).
#include "ipc/SharedRegion.h"

#include <chrono>
#include <cstring>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
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

uint32_t current_process_id()
{
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
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

// 남아 있는 구역의 머리를 읽어 만든 쪽이 아직 사는지 본다. 성한 머리가 아니거나 그 프로세스가 없으면 거짓이다.
//  [inv] 번호 재사용은 감수한다 — 죽은 판을 살아 있다고 잘못 보면 기동이 실패하고 사람이 /dev/shm을 치우면 된다.
//   반대(산 판을 죽었다고 보고 덮어쓰기)보다 이쪽이 안전하다.
bool creator_is_alive(const std::string& posix_name)
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
    const bool  alive  = header->magic == kSharedRegionMagic && header->creator_process_id != 0 &&
                       kill(static_cast<pid_t>(header->creator_process_id), 0) == 0;
    munmap(address, sizeof(SharedRegionHeader));
    return alive;
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
    , name_(std::move(other.name_))
    , last_error_(std::move(other.last_error_))
#ifdef _WIN32
    , mapping_handle_(other.mapping_handle_)
#else
    , descriptor_(other.descriptor_)
#endif
{
    other.address_ = nullptr;
    other.bytes_   = 0;
    other.owner_   = false;
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

    address_    = other.address_;
    bytes_      = other.bytes_;
    owner_      = other.owner_;
    name_       = std::move(other.name_);
    last_error_ = std::move(other.last_error_);
#ifdef _WIN32
    mapping_handle_       = other.mapping_handle_;
    other.mapping_handle_ = nullptr;
#else
    descriptor_       = other.descriptor_;
    other.descriptor_ = -1;
#endif
    other.address_ = nullptr;
    other.bytes_   = 0;
    other.owner_   = false;
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

    name_  = std::string(name);
    bytes_ = bytes;
    owner_ = true;

#ifdef _WIN32
    const HANDLE handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                             static_cast<DWORD>(bytes), name_.c_str());

    if (handle == nullptr)
    {
        last_error_ = "CreateFileMapping 실패 code=" + std::to_string(GetLastError());
        close();
        return false;
    }

    // 이미 있다는 것은 아직 쥔 프로세스가 있다는 뜻이다 — 쥔 곳이 없으면 이름째 사라지는 것이 윈도우다.
    //  여기서 실패시켜 엔진이 둘 뜨는 것을 기동 자리에서 잡는다.
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(handle);
        last_error_ = "같은 이름의 공유 구역을 이미 누가 쥐고 있다 — 엔진이 둘 떠 있는지 본다";
        close();
        return false;
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

    // 리눅스는 쥔 프로세스가 없어도 이름이 /dev/shm에 남는다. 남아 있으면 만든 쪽이 아직 사는지 보고,
    //  살아 있으면 실패(엔진 둘), 죽은 판이면 치우고 새로 만든다. 윈도우는 쥔 곳이 없으면 이름이
    //  저절로 사라지므로 ERROR_ALREADY_EXISTS 자체가 "살아 있는 주인" — 양쪽 뜻이 같아진다.
    if (descriptor_ < 0 && errno == EEXIST)
    {
        if (creator_is_alive(posix_name))
        {
            last_error_ = "같은 이름의 공유 구역을 이미 누가 쥐고 있다 — 엔진이 둘 떠 있는지 본다";
            owner_      = false; // 내가 만든 판이 아니다 — 닫으면서 남의 이름을 지우면 안 된다
            close();
            return false;
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
    auto* header               = static_cast<SharedRegionHeader*>(address_);
    header->layout_version     = layout_version;
    header->bytes              = bytes;
    header->created_at_ns      = now_ns();
    header->creator_process_id = current_process_id();
    header->magic              = kSharedRegionMagic;
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
