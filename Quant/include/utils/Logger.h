#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
//   설계 의도: 호출 스레드(hot path: 전략·주문 스레드)는 레코드를 큐에 넣기만 하고 즉시 반환한다.
//   타임스탬프 포맷팅과 파일/콘솔 I/O(디스크 플러시가 tail latency를 만드는 지점)는 전용 writer
//   스레드가 담당한다. 동기 로깅은 평균은 멀쩡해도 디스크가 튀는 순간 최악지연을 오염시키기 때문에,
//   "저지연은 평균이 아니라 최악을 다루는 문제"라는 원칙에 맞춰 I/O를 hot path에서 분리했다.
//
//   백프레셔: 큐가 상한(kMaxQueue)을 넘으면 가장 오래된 레코드를 버리고 드롭 수를 센다.
//   → 디스크가 오래 멈춰도 로깅이 메모리를 무한정 먹거나 hot path를 블로킹하지 않는다(운영 안전).
class Logger
{
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    void init(const std::filesystem::path& filepath, LogLevel min_level = LogLevel::INFO)
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        // 부모 디렉터리를 먼저 만든다. main에서 실행파일 기준 절대경로가 넘어오므로
        // cwd 위치와 무관하게 로그가 한 폴더에 모인다. (Windows 한글 경로 대비 path로 open)
        std::error_code ec;
        auto parent = filepath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
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
            return;

        // hot path: 시각 스탬프만 찍고 큐에 넘긴다(포맷팅은 writer가 수행).
        Record rec{level, std::chrono::system_clock::now(), msg};

        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            if (!running_)
            {
                // 종료 중(writer 정지)에는 유실 방지를 위해 동기 폴백으로 기록.
                write_locked(format(rec));
                return;
            }
            if (queue_.size() >= kMaxQueue)
            {
                queue_.pop_front(); // 가장 오래된 것 드롭 — 무한 증가·블로킹 방지
                ++dropped_;
            }
            queue_.push_back(std::move(rec));
        }
        q_cv_.notify_one();
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

    // 큐에 쌓인 레코드가 모두 파일/콘솔에 반영될 때까지 블로킹(테스트·종료 직전 정합 확인용).
    void flush()
    {
        std::unique_lock<std::mutex> lock(q_mutex_);
        drained_cv_.wait(lock, [this] { return queue_.empty() || !running_; });
        if (file_.is_open())
            file_.flush();
    }

private:
    struct Record
    {
        LogLevel level;
        std::chrono::system_clock::time_point ts;
        std::string msg;
    };

    Logger()
    {
        running_ = true;
        writer_ = std::thread(&Logger::writer_loop, this);
    }

    ~Logger()
    {
        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            running_ = false;
        }
        q_cv_.notify_all();
        if (writer_.joinable())
            writer_.join();
        // writer 정지 후 남은 레코드를 마지막으로 비운다(스레드 join으로 경쟁 없음).
        for (auto& rec : queue_)
            write_locked(format(rec));
        if (dropped_ > 0 && file_.is_open())
            file_ << "[Logger] 종료 시점 드롭된 로그 " << dropped_ << "건\n";
        if (file_.is_open())
            file_.flush();
    }

    void writer_loop()
    {
        for (;;)
        {
            std::deque<Record> batch;
            {
                std::unique_lock<std::mutex> lock(q_mutex_);
                q_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
                if (!running_ && queue_.empty())
                    break;
                batch.swap(queue_); // 한 번에 스왑 → 락 보유시간 최소화
            }
            // I/O는 락 밖에서(hot path의 enqueue를 막지 않음).
            for (auto& rec : batch)
                write_unlocked(format(rec));
            if (file_.is_open())
                file_.flush();
            drained_cv_.notify_all();
        }
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
            std::cout << line << '\n';
        if (file_.is_open())
            file_ << line << '\n';
    }
    // 종료 경로의 동기 폴백에서 사용(q_mutex_ 보유 상태로 호출됨).
    void write_locked(const std::string& line)
    {
        write_unlocked(line);
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
    std::filesystem::path base_dir_{"logs"}; // set_base_dir 전 기본값(하위호환)

    std::atomic<LogLevel> min_level_{LogLevel::INFO};
    std::atomic<bool> console_enabled_{true};

    static constexpr size_t kMaxQueue = 100000; // 백프레셔 상한(초과 시 최오래 드롭)
    std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::condition_variable drained_cv_;
    std::deque<Record> queue_;
    bool running_ = false;
    size_t dropped_ = 0;
    std::thread writer_;
};

#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARN(msg) Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
