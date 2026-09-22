// tests/bench_peer_failure.cpp
// 프로세스를 나눴을 때의 고장 쪽 수치를 잰다 — "상대가 죽은 것을 언제 아는가"와 "경계를 넘기는 데 얼마가 드는가".
//  지금까지의 벤치는 전부 잘 돌 때의 속도를 쟀다(bench_market_firehose·bench_feed_ingest·bench_latency_path).
//  전략과 주문·원장을 다른 프로세스로 나누려는 이유는 속도가 아니라 고장 격리다 — 전략이 죽어도 주문 쪽이 남아
//  하던 주문을 마치고 기록하고 내려갈 수 있게 하는 것. 그 설계에 필요한 숫자가 여기 세 개다. [why D-071]
//
// [inv] 측정 범위 — 같은 머신의 두 프로세스다. 브로커 왕복·주문 취소 응답 시간은 들어 있지 않다.
//   재는 것은 "남은 쪽이 상대의 죽음을 알아챌 때까지의 지연"과 "공유메모리로 레코드 한 건을 넘기는 비용"이다.
//
// 재는 것
//   A. 경계 통과 지연 — 자식이 공유메모리에 레코드를 쓰고 부모가 읽기까지. TCP loopback(결과 ③ p50 15.5µs)과 비교용.
//   B. 죽음 감지 지연 — 마지막 생존 신호에서 부모가 알아채기까지. 세 가지 죽는 방식 × 두 가지 감지 수단.
//        crash(abort) · exit(정상 종료) · hang(살아는 있는데 멈춤)
//        OS 종료 알림 — crash·exit은 잡고 hang은 못 잡는다.
//        심장박동 시간초과 — 셋 다 잡지만 문턱만큼 늦다.
//   C. 기록하고 내려가기 — 주문 한 건을 디스크에 확실히 남기는 비용(append + flush). "마무리"의 하한.
//
// 실행: bench_peer_failure            (부모. 자식은 자기 자신을 다시 띄운다)
//       bench_peer_failure --child <모드> <공유메모리이름>   (자식. 손으로 부를 일 없다)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// windows.h의 min/max 매크로가 std::min을 깨뜨린다.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// 두 프로세스가 같이 보는 자리. 원소는 std::atomic_ref로만 만진다 — 공유메모리에 std::atomic 객체를
//  생성하는 것은 표준이 보장하지 않아서다. 64비트 정수 atomic_ref는 두 플랫폼 모두 락 없이 돈다.
struct SharedBlock
{
    uint64_t sequence;     // 자식이 쓴 레코드 번호. 이게 바뀌면 새 레코드다.
    int64_t  written_ns;   // 그 레코드를 쓴 시각(steady_clock)
    int64_t  heartbeat_ns; // 자식이 마지막으로 살아 있다고 찍은 시각
    uint32_t ready;        // 자식이 준비됐다는 표시
    char     payload[84];  // 캡처 레코드 한 건과 같은 크기(체결 80 + 머리 4)
};

static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<int64_t>::is_always_lock_free);

constexpr size_t kSharedSize        = sizeof(SharedBlock);
constexpr int    kLatencySamples    = 100'000;
constexpr int    kHeartbeatSpacingUs = 200;
constexpr int    kAliveMilliseconds = 300;   // 자식이 죽기 전까지 살아 있는 시간
constexpr int64_t kHeartbeatTimeoutNs = 5'000'000; // 5ms — 이 문턱은 설계에서 고를 값이라 결과에 같이 적는다

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t percentile(std::vector<int64_t>& sorted_samples, double fraction)
{
    if (sorted_samples.empty())
    {
        return 0;
    }

    const size_t position = static_cast<size_t>(static_cast<double>(sorted_samples.size()) * fraction);
    return sorted_samples[std::min(sorted_samples.size() - 1, position)];
}

void print_distribution(const char* label, std::vector<int64_t> samples)
{
    if (samples.empty())
    {
        std::printf("| %-28s | %9s | %9s | %9s | %9s |\n", label, "-", "-", "-", "-");
        return;
    }

    std::sort(samples.begin(), samples.end());
    std::printf("| %-28s | %9.2f | %9.2f | %9.2f | %9.2f |\n",
                label,
                static_cast<double>(percentile(samples, 0.50)) / 1000.0,
                static_cast<double>(percentile(samples, 0.99)) / 1000.0,
                static_cast<double>(percentile(samples, 0.999)) / 1000.0,
                static_cast<double>(samples.back()) / 1000.0);
}

// ── 공유메모리 ────────────────────────────────────────────────────────────
struct SharedMapping
{
    SharedBlock* block = nullptr;
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    int  descriptor = -1;
    bool owner      = false;
#endif
    std::string name;
};

bool open_shared(SharedMapping& mapping, const std::string& name, bool create)
{
    mapping.name = name;

#ifdef _WIN32
    if (create)
    {
        mapping.handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            static_cast<DWORD>(kSharedSize), name.c_str());
    }
    else
    {
        mapping.handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    }

    if (mapping.handle == nullptr)
    {
        return false;
    }

    mapping.block = static_cast<SharedBlock*>(MapViewOfFile(mapping.handle, FILE_MAP_ALL_ACCESS, 0, 0, kSharedSize));
    return mapping.block != nullptr;
#else
    const std::string posix_name = "/" + name;
    mapping.owner                = create;
    mapping.descriptor           = shm_open(posix_name.c_str(), create ? (O_CREAT | O_RDWR) : O_RDWR, 0600);

    if (mapping.descriptor < 0)
    {
        return false;
    }

    if (create && ftruncate(mapping.descriptor, static_cast<off_t>(kSharedSize)) != 0)
    {
        return false;
    }

    void* address = mmap(nullptr, kSharedSize, PROT_READ | PROT_WRITE, MAP_SHARED, mapping.descriptor, 0);

    if (address == MAP_FAILED)
    {
        return false;
    }

    mapping.block = static_cast<SharedBlock*>(address);
    return true;
#endif
}

void close_shared(SharedMapping& mapping)
{
#ifdef _WIN32
    if (mapping.block != nullptr)
    {
        UnmapViewOfFile(mapping.block);
    }

    if (mapping.handle != nullptr)
    {
        CloseHandle(mapping.handle);
    }
#else
    if (mapping.block != nullptr)
    {
        munmap(mapping.block, kSharedSize);
    }

    if (mapping.descriptor >= 0)
    {
        close(mapping.descriptor);
    }

    if (mapping.owner)
    {
        shm_unlink(("/" + mapping.name).c_str());
    }
#endif
}

// ── 자식 ──────────────────────────────────────────────────────────────────
int run_child(const std::string& mode, const std::string& mapping_name)
{
    SharedMapping mapping;

    if (!open_shared(mapping, mapping_name, false))
    {
        return 2;
    }

    SharedBlock* block = mapping.block;
    std::atomic_ref<uint32_t>(block->ready).store(1, std::memory_order_release);

    if (mode == "send")
    {
        for (int index = 0; index < kLatencySamples; ++index)
        {
            std::memset(block->payload, static_cast<int>(index & 0xFF), sizeof(block->payload));
            std::atomic_ref<int64_t>(block->written_ns).store(now_ns(), std::memory_order_relaxed);
            std::atomic_ref<uint64_t>(block->sequence)
                .store(static_cast<uint64_t>(index) + 1, std::memory_order_release);

            // 부모가 기다리는 상태에서 한 건씩 받게 띄운다 — 몰아 보내면 큐 대기시간이 섞여 경계 비용이 아니게 된다.
            const int64_t until = now_ns() + 2'000;

            while (now_ns() < until)
            {
            }
        }

        close_shared(mapping);
        return 0;
    }

    // 죽는 방식 셋. 죽기 전까지는 심장박동을 규칙적으로 찍는다.
    const int64_t stop_at = now_ns() + static_cast<int64_t>(kAliveMilliseconds) * 1'000'000;

    while (now_ns() < stop_at)
    {
        std::atomic_ref<int64_t>(block->heartbeat_ns).store(now_ns(), std::memory_order_release);

        const int64_t until = now_ns() + static_cast<int64_t>(kHeartbeatSpacingUs) * 1'000;

        while (now_ns() < until)
        {
        }
    }

    if (mode == "crash")
    {
        // 정리 없이 죽는다 — 전략 코드의 미처리 예외·접근위반과 같은 자리.
        std::abort();
    }

    if (mode == "hang")
    {
        // 살아는 있는데 일을 멈춘 상태. OS는 아무 말도 안 한다.
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    close_shared(mapping);
    return 0; // mode == "exit"
}

// ── 자식 띄우기 ───────────────────────────────────────────────────────────
struct ChildProcess
{
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    pid_t pid = -1;
#endif
};

bool spawn_child(ChildProcess& child, const char* self_path, const std::string& mode, const std::string& mapping_name)
{
#ifdef _WIN32
    std::string command_line = std::string("\"") + self_path + "\" --child " + mode + " " + mapping_name;
    // 첫 멤버가 구조체 크기라 중괄호 초기화로 채운다.
    STARTUPINFOA        startup{sizeof(STARTUPINFOA)};
    PROCESS_INFORMATION information{};

    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &information))
    {
        return false;
    }

    CloseHandle(information.hThread);
    child.handle = information.hProcess;
    return true;
#else
    child.pid = fork();

    if (child.pid < 0)
    {
        return false;
    }

    if (child.pid == 0)
    {
        std::_Exit(run_child(mode, mapping_name));
    }

    return true;
#endif
}

bool child_has_exited(ChildProcess& child)
{
#ifdef _WIN32
    return WaitForSingleObject(child.handle, 0) == WAIT_OBJECT_0;
#else
    int       status = 0;
    const pid_t result = waitpid(child.pid, &status, WNOHANG);
    return result == child.pid;
#endif
}

void kill_child(ChildProcess& child)
{
#ifdef _WIN32
    if (child.handle != nullptr)
    {
        TerminateProcess(child.handle, 1);
        WaitForSingleObject(child.handle, 2000);
        CloseHandle(child.handle);
        child.handle = nullptr;
    }
#else
    if (child.pid > 0)
    {
        kill(child.pid, SIGKILL);
        int status = 0;
        waitpid(child.pid, &status, 0);
        child.pid = -1;
    }
#endif
}

bool wait_for_ready(SharedBlock* block)
{
    const int64_t deadline = now_ns() + 5'000'000'000LL;

    while (now_ns() < deadline)
    {
        if (std::atomic_ref<uint32_t>(block->ready).load(std::memory_order_acquire) == 1)
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return false;
}

std::string make_mapping_name(const char* suffix)
{
#ifdef _WIN32
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = static_cast<unsigned long>(getpid());
#endif
    return "quant_peer_failure_" + std::to_string(process_id) + "_" + suffix;
}

// ── A. 경계 통과 지연 ─────────────────────────────────────────────────────
void measure_boundary_latency(const char* self_path)
{
    SharedMapping mapping;
    const std::string name = make_mapping_name("send");

    if (!open_shared(mapping, name, true))
    {
        std::printf("공유메모리 생성 실패 — A 건너뜀\n");
        return;
    }

    std::memset(mapping.block, 0, kSharedSize);
    ChildProcess child;

    if (!spawn_child(child, self_path, "send", name) || !wait_for_ready(mapping.block))
    {
        std::printf("자식 기동 실패 — A 건너뜀\n");
        close_shared(mapping);
        return;
    }

    std::vector<int64_t> samples;
    samples.reserve(kLatencySamples);
    uint64_t      last_sequence = 0;
    const int64_t deadline      = now_ns() + 10'000'000'000LL;

    while (static_cast<int>(samples.size()) < kLatencySamples && now_ns() < deadline)
    {
        const uint64_t sequence =
            std::atomic_ref<uint64_t>(mapping.block->sequence).load(std::memory_order_acquire);

        if (sequence == last_sequence)
        {
            continue;
        }

        const int64_t written = std::atomic_ref<int64_t>(mapping.block->written_ns).load(std::memory_order_relaxed);
        samples.push_back(now_ns() - written);
        last_sequence = sequence;
    }

    std::printf("\n## A. 경계 통과 지연 — 공유메모리, 레코드 %zu바이트, %zu건\n\n", sizeof(SharedBlock::payload),
                samples.size());
    std::printf("| %-28s | %9s | %9s | %9s | %9s |\n", "구간", "p50(us)", "p99(us)", "p999(us)", "max(us)");
    std::printf("|%s|%s|%s|%s|%s|\n", "------------------------------", "-----------", "-----------", "-----------",
                "-----------");
    print_distribution("자식 쓰기 → 부모 읽기", samples);
    std::printf("\n비교: TCP loopback 같은 구간 p50 15.5us / p99 27.7us / p999 609us (PIPELINE_LATENCY_REPORT 결과 3)\n");

    kill_child(child);
    close_shared(mapping);
}

// ── B. 죽음 감지 지연 ─────────────────────────────────────────────────────
void measure_death_detection(const char* self_path, const std::string& mode)
{
    SharedMapping     mapping;
    const std::string name = make_mapping_name(mode.c_str());

    if (!open_shared(mapping, name, true))
    {
        std::printf("공유메모리 생성 실패 — B(%s) 건너뜀\n", mode.c_str());
        return;
    }

    std::memset(mapping.block, 0, kSharedSize);
    ChildProcess child;

    if (!spawn_child(child, self_path, mode, name) || !wait_for_ready(mapping.block))
    {
        std::printf("자식 기동 실패 — B(%s) 건너뜀\n", mode.c_str());
        close_shared(mapping);
        return;
    }

    int64_t last_heartbeat   = 0;
    int64_t os_detected_at   = 0;
    int64_t timeout_detected = 0;
    const int64_t deadline   = now_ns() + 5'000'000'000LL;

    while (now_ns() < deadline)
    {
        const int64_t heartbeat =
            std::atomic_ref<int64_t>(mapping.block->heartbeat_ns).load(std::memory_order_acquire);

        if (heartbeat > last_heartbeat)
        {
            last_heartbeat = heartbeat;
        }

        if (os_detected_at == 0 && child_has_exited(child))
        {
            os_detected_at = now_ns();
        }

        if (timeout_detected == 0 && last_heartbeat != 0 && now_ns() - last_heartbeat > kHeartbeatTimeoutNs)
        {
            timeout_detected = now_ns();
        }

        if (timeout_detected != 0 && (os_detected_at != 0 || mode == "hang"))
        {
            break;
        }
    }

    const double os_lag =
        os_detected_at == 0 ? -1.0 : static_cast<double>(os_detected_at - last_heartbeat) / 1'000'000.0;
    const double timeout_lag =
        timeout_detected == 0 ? -1.0 : static_cast<double>(timeout_detected - last_heartbeat) / 1'000'000.0;

    char os_text[32];
    char timeout_text[32];
    std::snprintf(os_text, sizeof(os_text), os_lag < 0 ? "못 잡음" : "%.3f", os_lag);
    std::snprintf(timeout_text, sizeof(timeout_text), timeout_lag < 0 ? "못 잡음" : "%.3f", timeout_lag);
    std::printf("| %-22s | %14s | %18s |\n", mode.c_str(), os_text, timeout_text);

    kill_child(child);
    close_shared(mapping);
}

// ── C. 기록하고 내려가기 ──────────────────────────────────────────────────
void measure_durable_append()
{
    const std::string path = std::string("bench_peer_failure_durable.tmp");
    std::FILE*        file = std::fopen(path.c_str(), "wb");

    if (file == nullptr)
    {
        std::printf("임시 파일 생성 실패 — C 건너뜀\n");
        return;
    }

    char                 record[64];
    std::memset(record, 'x', sizeof(record));
    std::vector<int64_t> samples;
    samples.reserve(500);

    for (int index = 0; index < 500; ++index)
    {
        const int64_t started = now_ns();
        std::fwrite(record, 1, sizeof(record), file);
        std::fflush(file);

#ifdef _WIN32
        FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))));
#else
        fsync(fileno(file));
#endif
        samples.push_back(now_ns() - started);
    }

    std::fclose(file);
    std::remove(path.c_str());

    std::printf("\n## C. 주문 한 건을 디스크에 확실히 남기는 비용 — append + flush, 64바이트 x 500회\n\n");
    std::printf("| %-28s | %9s | %9s | %9s | %9s |\n", "구간", "p50(us)", "p99(us)", "p999(us)", "max(us)");
    std::printf("|%s|%s|%s|%s|%s|\n", "------------------------------", "-----------", "-----------", "-----------",
                "-----------");
    print_distribution("write + flush", samples);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "--child")
    {
        return run_child(argv[2], argv[3]);
    }

    std::printf("# 프로세스 경계 고장 실측 (bench_peer_failure)\n");
    std::printf("\n심장박동 문턱 %.1fms, 자식 생존 %dms, 박동 간격 %dus\n",
                static_cast<double>(kHeartbeatTimeoutNs) / 1'000'000.0, kAliveMilliseconds, kHeartbeatSpacingUs);

    measure_boundary_latency(argv[0]);

    std::printf("\n## B. 죽음 감지 지연 — 마지막 생존 신호에서 알아채기까지(ms)\n\n");
    std::printf("| %-22s | %14s | %18s |\n", "죽는 방식", "OS 종료 알림", "심장박동 시간초과");
    std::printf("|%s|%s|%s|\n", "------------------------", "----------------", "--------------------");

    for (const char* mode : {"crash", "exit", "hang"})
    {
        measure_death_detection(argv[0], mode);
    }

    measure_durable_append();

    std::printf("\n판정: 이 표가 답하는 것은 \"전략 프로세스가 죽고 나서 주문 프로세스가 몇 ms 뒤에 마무리를 시작할 수 있는가\"다.\n");
    return 0;
}
