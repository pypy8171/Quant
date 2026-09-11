#pragma once
#include "core/MpscQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR
};

// 비동기 로거.
//   설계 의도: 호출 스레드(hot path: 전략·주문 스레드)는 레코드를 큐에 넣고 즉시 반환한다.
//   타임스탬프 포맷팅과 파일/콘솔 I/O(디스크 플러시가 꼬리 지연(tail latency)을 만드는 지점)는
//   전용 writer 스레드가 담당한다. 동기 로깅은 디스크가 튀는 순간 최악 지연을 오염시키므로,
//   최악 지연을 낮추려고 I/O를 hot path에서 분리했다.
//
//   큐는 락 없는 MPSC(`MpscQueue`, Vyukov)다. 호출 스레드는 CAS 한 번으로 칸을 예약하고 레코드를
//   옮겨 놓는다 — 뮤텍스를 잡지 않는다. writer는 큐가 비면 yield 몇 번 뒤 condvar에서 잔다. 깨우기는
//   writer가 "잔다"고 표시해 둔 때만 notify를 부르므로, 연속 기록 중 hot path는 원자 load 하나다. [why D-045]
//
//   밀림 처리: 큐(kQueueCapacity 슬롯)가 가득 차면 새 레코드를 버리고 드롭 수를 센다.
//   디스크가 오래 멈춰도 로깅이 메모리를 무한정 먹거나 hot path를 블로킹하지 않는다(운영 안전).
//   [inv] 로그를 부르는 스레드는 Logger 소멸(정적 소멸) 전에 join돼 있어야 한다. 소멸 뒤 호출은 미정의.
#ifdef _WIN32
// windows.h를 이 헤더에 넣으면 ERROR 매크로가 LogLevel::ERROR와 부딪힌다. SDK 선언과 같은 형으로 직접 선언.
struct HINSTANCE__;
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(HINSTANCE__*, wchar_t*, unsigned long);
#endif

class Logger
{
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    // 싱글톤 — 사본이 생기면 writer 스레드와 큐가 둘이 된다.
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // 실행파일이 놓인 디렉터리(cwd와 무관). 알 수 없으면 cwd.
    static std::filesystem::path executable_dir()
    {
#ifdef _WIN32
        wchar_t buf[4096];
        unsigned long n = GetModuleFileNameW(nullptr, buf, 4096);

        if (n == 0 || n >= 4096)
        {
            return std::filesystem::current_path();
        }

        return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
        std::error_code ec;
        auto p = std::filesystem::read_symlink("/proc/self/exe", ec);

        if (ec)
        {
            return std::filesystem::current_path();
        }

        return p.parent_path();
#endif
    }

    // 로그·산출물 기준 디렉터리 기본값: QUANT_LOG_DIR 환경변수 > 실행파일 옆 logs/.
    //  cwd 기준이면 테스트 바이너리를 repo 루트에서 돌릴 때 당일 원장(trades_*.csv)에 TEST 행이 섞인다.
    static std::filesystem::path default_base_dir()
    {
        if (const char* env = std::getenv("QUANT_LOG_DIR"); env && *env)
        {
            return std::filesystem::path(env);
        }

        return executable_dir() / "logs";
    }

    void init(const std::filesystem::path& filepath, LogLevel min_level = LogLevel::INFO)
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        // 부모 디렉터리를 먼저 만든다. main에서 실행파일 기준 절대경로가 넘어오므로
        // cwd 위치와 무관하게 로그가 한 폴더에 모인다. (Windows 한글 경로 대비 path로 open)
        std::error_code ec;
        auto parent = filepath.parent_path();

        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
        }

        file_.open(filepath, std::ios::app);
        min_level_.store(min_level, std::memory_order_relaxed);
    }

    // 실행 위치(cwd)와 무관하게 로그·산출물을 한 곳에 모으기 위한 기준 디렉터리.
    // main에서 실행파일 기준 절대경로로 한 번 고정한다(미설정 시 cwd 하위 "logs").
    void set_base_dir(const std::filesystem::path& dir)
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        base_dir_ = dir;
    }

    std::filesystem::path base_dir()
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        return base_dir_;
    }

    // 기준 디렉터리 하위 파일의 전체 경로(부모 폴더가 없으면 생성).
    std::filesystem::path path_for(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        std::error_code ec;
        std::filesystem::create_directories(base_dir_, ec);
        return base_dir_ / name;
    }

    // 화면 표시 모드일 때 콘솔 출력을 끄고 파일에만 기록
    void set_console_enabled(bool enabled)
    {
        console_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void log(LogLevel level, const std::string& msg)
    {
        if (level < min_level_.load(std::memory_order_relaxed))
        {
            return;
        }

        // hot path: 시각 스탬프만 찍고 큐에 넘긴다(포맷팅은 writer가 수행). 락 없음.
        Record rec{level, std::chrono::system_clock::now(), msg, nullptr};

        if (!running_.load(std::memory_order_acquire))
        {
            // 종료 중(writer 정지)에는 동기 폴백. 늦은 호출자끼리의 file_ 경쟁만 cfg_mutex_로 막는다.
            std::lock_guard<std::mutex> lock(cfg_mutex_);
            write_unlocked(format(rec));
            return;
        }

        if (!enqueue(std::move(rec)))
        {
            // 가득 참 — 새 레코드를 버린다. MPSC는 생산자 쪽에서 가장 오래된 칸을 뺄 수 없다.
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void info(const std::string& m)
    {
        log(LogLevel::INFO, m);
    }

    void warn(const std::string& m)
    {
        log(LogLevel::WARN, m);
    }

    void error(const std::string& m)
    {
        log(LogLevel::ERROR, m);
    }

    void debug(const std::string& m)
    {
        log(LogLevel::DEBUG, m);
    }

    // 이 호출 전에 반환된 log()가 모두 파일/콘솔에 반영될 때까지 블로킹(테스트·종료 직전 정합 확인용).
    // 표식 레코드를 큐에 넣고 writer가 그 칸에 닿기를 기다린다. 큐는 티켓 순으로 소비되므로 표식보다
    // 앞 티켓(= 먼저 반환된 log)은 표식 처리 시점에 전부 기록돼 있다.
    void flush()
    {
        if (!running_.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(cfg_mutex_);

            if (file_.is_open())
            {
                file_.flush();
            }

            return;
        }

        std::atomic<bool> done{false};

        while (!enqueue(Record{LogLevel::DEBUG, {}, {}, &done}))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 가득 참 — writer가 비울 때까지
        }

        // [inv] 표식은 writer 또는 소멸자 배수(drain)가 반드시 처리하므로 기다림은 끝난다.
        while (!done.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }

    // 가득 차서 버린 레코드 수(운영 중 관측용).
    [[nodiscard]] uint64_t dropped() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    struct Record
    {
        LogLevel level;
        std::chrono::system_clock::time_point ts;
        std::string msg;
        std::atomic<bool>* flush_mark; // flush()의 표식. 아니면 nullptr
    };

    Logger()
    {
        running_.store(true, std::memory_order_release);
        writer_ = std::thread(&Logger::writer_loop, this);
    }

    // 큐에 넣고, writer가 자고 있으면 깨운다. 실패(가득 참)면 false.
    // [lock-order] push(release) → seq_cst fence → sleeping 읽기. writer 쪽은 sleeping 쓰기 → fence → 큐 확인.
    //  양쪽 다 store-fence-load라 둘 중 하나는 상대 store를 본다 — 넣었는데 아무도 안 깨우는 경우가 없다.
    //  notify는 wake_mtx_ 없이 부른다. writer가 잠들기 직전이면 신호가 새지만 wait_for 상한이 받는다.
    bool enqueue(Record&& rec)
    {
        if (!queue_.push(std::move(rec)))
        {
            return false;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (writer_sleeping_.load(std::memory_order_relaxed))
        {
            wake_cv_.notify_one();
        }

        return true;
    }

    ~Logger()
    {
        running_.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        wake_cv_.notify_one();

        if (writer_.joinable())
        {
            writer_.join();
        }

        // writer가 마지막 pop 뒤에 들어온 레코드를 비운다. join 뒤라 이 스레드가 유일한 소비자다.
        while (auto rec = queue_.pop())
        {
            consume(*rec);
        }

        const uint64_t dropped = dropped_.load(std::memory_order_relaxed);

        if (dropped > 0 && file_.is_open())
        {
            file_ << "[Logger] 종료 시점 드롭된 로그 " << dropped << "건\n";
        }

        if (file_.is_open())
        {
            file_.flush();
        }
    }

    // writer 스레드 단독. 큐가 비면 yield 몇 번 뒤 condvar에서 잔다 — 로그는 지연보다 hot path 비간섭이 우선이다.
    void writer_loop()
    {
        int idle = 0;
        size_t since_flush = 0;

        while (true)
        {
            auto rec = queue_.pop();

            if (!rec)
            {
                if (!running_.load(std::memory_order_acquire))
                {
                    break; // 이 뒤에 들어온 건 소멸자가 비운다
                }

                if (since_flush > 0 && file_.is_open())
                {
                    file_.flush(); // 한산해진 순간 한 번
                    since_flush = 0;
                }

                if (++idle < kIdleSpins)
                {
                    std::this_thread::yield();
                }
                else
                {
                    sleep_until_work();
                }

                continue;
            }

            idle = 0;
            consume(*rec);

            if (++since_flush >= kFlushEvery && file_.is_open())
            {
                file_.flush(); // 폭주 중에도 tail -f가 볼 수 있게
                since_flush = 0;
            }
        }
    }

    // "잔다"를 먼저 알리고 큐를 다시 본 뒤 잔다(enqueue의 fence 짝). 신호가 새는 경우를 대비해 상한을 둔다.
    void sleep_until_work()
    {
        std::unique_lock<std::mutex> lock(wake_mtx_);
        writer_sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (queue_.empty() && running_.load(std::memory_order_acquire))
        {
            wake_cv_.wait_for(lock, kSleepCap);
        }

        writer_sleeping_.store(false, std::memory_order_relaxed);
    }

    // 레코드 하나 처리 — 표식이면 파일을 비우고 신호, 아니면 기록. 소비자 스레드(writer 또는 소멸자)만 부른다.
    void consume(const Record& rec)
    {
        if (rec.flush_mark != nullptr)
        {
            if (file_.is_open())
            {
                file_.flush();
            }

            rec.flush_mark->store(true, std::memory_order_release);
            return;
        }

        write_unlocked(format(rec));
    }

    std::string format(const Record& rec) const
    {
        auto t = std::chrono::system_clock::to_time_t(rec.ts);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(rec.ts.time_since_epoch()) % 1000;
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << ms.count() << " [" << level_str(rec.level) << "] " << rec.msg;
        return ss.str();
    }

    // 콘솔·파일 실제 기록. writer 스레드에서 호출(락 불필요 — file_/console은 writer 단독 소유).
    void write_unlocked(const std::string& line)
    {
        if (console_enabled_.load(std::memory_order_relaxed))
        {
            std::cout << line << '\n';
        }

        if (file_.is_open())
        {
            file_ << line << '\n';
        }
    }

    static const char* level_str(LogLevel l)
    {
        switch (l)
        {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO ";
        case LogLevel::WARN:
            return "WARN ";
        case LogLevel::ERROR:
            return "ERROR";
        }

        return "?????";
    }

    // 설정(파일 핸들·디렉터리)용 뮤텍스와 큐용 뮤텍스를 분리 — 설정 변경이 hot path 큐잉과 경쟁하지 않게.
    std::mutex cfg_mutex_;
    std::ofstream file_;
    std::filesystem::path base_dir_{default_base_dir()}; // set_base_dir 전에도 실행파일 기준

    std::atomic<LogLevel> min_level_{LogLevel::INFO};
    std::atomic<bool> console_enabled_{true};

    static constexpr size_t kQueueCapacity = 1u << 16; // 슬롯 수(2의 거듭제곱). 가득 차면 새 레코드 드롭
    static constexpr int kIdleSpins = 64;              // 빈 큐에서 yield 횟수, 넘으면 condvar 잠
    static constexpr std::chrono::milliseconds kSleepCap{10}; // 신호가 샜을 때 최대 잠
    static constexpr size_t kFlushEvery = 256;         // 연속 기록 중 파일 flush 간격(레코드 수)
    MpscQueue<Record> queue_{kQueueCapacity};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> writer_sleeping_{false}; // writer가 wake_cv_에서 자는 중(생산자가 notify 여부 결정)
    std::mutex wake_mtx_;                      // writer만 잡는다. 생산자는 notify만 부른다
    std::condition_variable wake_cv_;
    std::thread writer_;
};

#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARN(msg) Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
