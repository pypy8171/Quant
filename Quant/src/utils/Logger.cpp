#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include "core/MpscQueue.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <thread>
#include <vector>

#ifdef _WIN32
// windows.h를 넣으면 ERROR 매크로가 LogLevel::ERROR와 부딪힌다. SDK 선언과 같은 형으로 직접 선언.
struct HINSTANCE__;
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(HINSTANCE__*, wchar_t*, unsigned long);
#endif

// 큐는 락 없는 MPSC(`MpscQueue`, Vyukov)다. 호출 스레드는 CAS 한 번으로 칸을 예약하고 레코드를
//  옮겨 놓는다 — 뮤텍스를 잡지 않는다. writer는 큐가 비면 yield 몇 번 뒤 condvar에서 잔다. 깨우기는
//  writer가 "잔다"고 표시해 둔 때만 notify를 부르므로, 연속 기록 중 hot path는 원자 load 하나다. [why D-045]
//
//  밀림 처리: 큐(kQueueCapacity 슬롯)가 가득 차면 새 레코드를 버리고 드롭 수를 센다.
//  디스크가 오래 멈춰도 로깅이 메모리를 무한정 먹거나 hot path를 블로킹하지 않는다(운영 안전).
struct Logger::Implementation
{
    struct Record
    {
        LogLevel level;
        std::chrono::system_clock::time_point timestamp;
        std::string message;
        std::atomic<bool>* flush_mark; // flush()의 표식. 아니면 nullptr
        // 찍은 스레드 이름. 포인터로 들고 가면 그 스레드가 먼저 끝날 때 끊기므로 16바이트를 복사한다.
        std::array<char, thread_name::kMaxLength> thread_label{};
    };

    Implementation()
    {
        // set_base_directory 전에도 쓸 수 있게 기본값(QUANT_LOG_DIR 또는 실행파일 옆 logs)을 첫 판으로 둔다.
        //  폴더는 path_for가 처음 불릴 때 만든다 — 산출물을 쓰지 않는 실행에 빈 폴더를 남기지 않는다.
        base_directories_.push_back(std::make_unique<BaseDirectory>(Logger::default_base_directory()));
        current_base_directory_.store(base_directories_.back().get(), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        writer_ = std::jthread([this](std::stop_token stop_token) { writer_loop(stop_token); });
    }

    ~Implementation()
    {
        running_.store(false, std::memory_order_release); // 이 뒤의 enqueue는 거절
        writer_.request_stop();                            // 자고 있으면 stop_token 대기가 여기서 깬다

        if (writer_.joinable())
        {
            writer_.join();
        }

        // writer가 마지막 pop 뒤에 들어온 레코드를 비운다. join 뒤라 이 스레드가 유일한 소비자다.
        while (auto record = queue_.pop())
        {
            consume(*record);
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

    // hot path: 시각 스탬프만 찍고 큐에 넘긴다(포맷팅은 writer가 수행). 락 없음.
    void submit(LogLevel level, const std::string& message)
    {
        Record record{level, std::chrono::system_clock::now(), message, nullptr};
        const char* thread_label = thread_name::current(); // 끝 NUL 포함 kMaxLength 이하가 보장된다
        std::memcpy(record.thread_label.data(), thread_label, std::strlen(thread_label));

        if (!running_.load(std::memory_order_acquire))
        {
            // 종료 중(writer 정지)에는 동기 폴백. 늦은 호출자끼리의 file_ 경쟁만 config_mutex_로 막는다.
            std::lock_guard<std::mutex> lock(config_mutex_);
            write_unlocked(format(record));
            return;
        }

        if (!enqueue(std::move(record)))
        {
            // 가득 참 — 새 레코드를 버린다. MPSC는 생산자 쪽에서 가장 오래된 칸을 뺄 수 없다.
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 표식 레코드를 큐에 넣고 writer가 그 칸에 닿기를 기다린다. 큐는 티켓 순으로 소비되므로 표식보다
    // 앞 티켓(= 먼저 반환된 log)은 표식 처리 시점에 전부 기록돼 있다.
    void flush()
    {
        if (!running_.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(config_mutex_);

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

    // 큐에 넣고, writer가 자고 있으면 깨운다. 실패(가득 참)면 false.
    // [lock-order] push(release) → seq_cst fence → sleeping 읽기. writer 쪽은 sleeping 쓰기 → fence → 큐 확인.
    //  양쪽 다 store-fence-load라 둘 중 하나는 상대 store를 본다 — 넣었는데 아무도 안 깨우는 경우가 없다.
    //  notify는 wake_mutex_ 없이 부른다. writer가 잠들기 직전이면 신호가 새지만 wait_for 상한이 받는다.
    bool enqueue(Record&& record)
    {
        if (!queue_.push(std::move(record)))
        {
            return false;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (writer_sleeping_.load(std::memory_order_relaxed))
        {
            wake_condition_variable_.notify_one();
        }

        return true;
    }

    // writer 스레드 단독. 큐가 비면 yield 몇 번 뒤 condvar에서 잔다 — 로그는 지연보다 hot path 비간섭이 우선이다.
    void writer_loop(std::stop_token stop_token)
    {
        thread_name::set_current("LogWriter");
        int idle = 0;
        size_t since_flush = 0;

        while (true)
        {
            auto record = queue_.pop();

            if (!record)
            {
                if (stop_token.stop_requested())
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
                    sleep_until_work(stop_token);
                }

                continue;
            }

            idle = 0;
            consume(*record);

            if (++since_flush >= kFlushEvery && file_.is_open())
            {
                file_.flush(); // 폭주 중에도 tail -f가 볼 수 있게
                since_flush = 0;
            }
        }
    }

    // "잔다"를 먼저 알리고 큐를 다시 본 뒤 잔다(enqueue의 fence 짝). 신호가 새는 경우를 대비해 상한을 둔다.
    void sleep_until_work(std::stop_token stop_token)
    {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        writer_sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (queue_.empty() && !stop_token.stop_requested())
        {
            wake_condition_variable_.wait_for(lock, stop_token, kSleepCap, [this] { return !queue_.empty(); });
        }

        writer_sleeping_.store(false, std::memory_order_relaxed);
    }

    // 레코드 하나 처리 — 표식이면 파일을 비우고 신호, 아니면 기록. 소비자 스레드(writer 또는 소멸자)만 부른다.
    void consume(const Record& record)
    {
        if (record.flush_mark != nullptr)
        {
            if (file_.is_open())
            {
                file_.flush();
            }

            record.flush_mark->store(true, std::memory_order_release);
            return;
        }

        write_unlocked(format(record));
    }

    std::string format(const Record& record) const
    {
        auto time_value = std::chrono::system_clock::to_time_t(record.timestamp);
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch()) % 1000;
        std::tm time_buffer{};
#ifdef _WIN32
        localtime_s(&time_buffer, &time_value);
#else
        localtime_r(&time_value, &time_buffer);
#endif
        std::ostringstream stream;
        stream << std::put_time(&time_buffer, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count() << " {" << record.thread_label.data() << " thread} [" << level_string(record.level) << "] "
           << record.message;
        return stream.str();
    }

    // 콘솔·파일 실제 기록. 평소에는 writer 스레드가 쓴다. writer가 없을 때 submit 대체 경로(config_mutex_ 아래)와
    //  소멸자 drain도 부르고, file_은 initialize()가 메인 스레드에서 연다.
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

    static const char* level_string(LogLevel log_level)
    {
        switch (log_level)
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

    // 기준 디렉터리 한 판. 만든 뒤로 path는 바뀌지 않고, 폴더 생성은 판마다 한 번(created)이다.
    struct BaseDirectory
    {
        explicit BaseDirectory(std::filesystem::path directory) : path(std::move(directory)) {}

        const std::filesystem::path path;
        std::once_flag created;
    };

    // 폴더를 한 번만 만든다. 이미 만든 판이면 call_once는 원자 load 하나로 돌아온다.
    static void ensure_created(BaseDirectory& base_directory)
    {
        std::call_once(base_directory.created, [&base_directory]
        {
            std::error_code error_code;
            std::filesystem::create_directories(base_directory.path, error_code);
        });
    }

    // 설정(파일 핸들·기준 디렉터리 판 목록)용 뮤텍스. 큐는 락 없는 MPSC 큐다. wake_mutex_는 writer가 잠들고 깨는 데만 쓴다.
    std::mutex config_mutex_;
    std::ofstream file_;

    // [inv] 판은 Implementation이 끝날 때까지 지우지 않는다 — base_directory()가 참조를 내주고, 읽는 쪽은 락 없이
    //  current_base_directory_만 본다. 목록에 넣는 것은 set_base_directory 하나다(config_mutex_ 아래).
    std::vector<std::unique_ptr<BaseDirectory>> base_directories_;
    // [lock-order] set_base_directory가 판을 다 만든 뒤 release로 공개하고, 읽는 쪽은 acquire로 받아 path를 본다.
    std::atomic<BaseDirectory*> current_base_directory_{nullptr};

    std::atomic<bool> console_enabled_{true};

    static constexpr size_t kQueueCapacity = 1u << 16; // 슬롯 수(2의 거듭제곱). 가득 차면 새 레코드 드롭
    static constexpr int kIdleSpins = 64;              // 빈 큐에서 yield 횟수, 넘으면 condvar 잠
    static constexpr std::chrono::milliseconds kSleepCap{10}; // 신호가 샜을 때 최대 잠
    static constexpr size_t kFlushEvery = 256;         // 연속 기록 중 파일 flush 간격(레코드 수)
    MpscQueue<Record> queue_{kQueueCapacity};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> writer_sleeping_{false}; // writer가 wake_condition_variable_에서 자는 중(생산자가 notify 여부 결정)
    std::mutex wake_mutex_;                      // writer만 잡는다. 생산자는 notify만 부른다
    std::condition_variable_any wake_condition_variable_;      // stop_token 대기는 _any에만 있다
    std::jthread writer_;
};

Logger::Logger() : implementation_(std::make_unique<Implementation>()) {}

Logger::~Logger() = default;

Logger& Logger::instance()
{
    static Logger institution;
    return institution;
}

std::filesystem::path Logger::executable_directory()
{
#ifdef _WIN32
    wchar_t buffer[4096];
    unsigned long length = GetModuleFileNameW(nullptr, buffer, 4096);

    if (length == 0 || length >= 4096)
    {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#else
    std::error_code error_code;
    auto symlink = std::filesystem::read_symlink("/proc/self/exe", error_code);

    if (error_code)
    {
        return std::filesystem::current_path();
    }

    return symlink.parent_path();
#endif
}

std::filesystem::path Logger::default_base_directory()
{
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); environment && *environment)
    {
        return std::filesystem::path(environment);
    }

    return executable_directory() / "logs";
}

void Logger::initialize(const std::filesystem::path& filepath, LogLevel min_level)
{
    std::lock_guard<std::mutex> lock(implementation_->config_mutex_);
    // 부모 디렉터리를 먼저 만든다. main에서 실행파일 기준 절대경로가 넘어오므로
    // cwd 위치와 무관하게 로그가 한 폴더에 모인다. (Windows 한글 경로 대비 path로 open)
    std::error_code error_code;
    auto parent = filepath.parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error_code);
    }

    implementation_->file_.open(filepath, std::ios::app);
    min_level_.store(min_level, std::memory_order_relaxed);
}

// 새 판을 만들고 폴더까지 만든 뒤 공개한다. 이전 판은 지우지 않는다(참조를 쥔 호출자가 있을 수 있다).
//  운영에서는 main이 스레드를 띄우기 전 한 번 부르고, 테스트는 바이너리마다 한 번 부른다.
void Logger::set_base_directory(const std::filesystem::path& directory)
{
    auto base_directory = std::make_unique<Implementation::BaseDirectory>(directory);
    Implementation::ensure_created(*base_directory);

    std::lock_guard<std::mutex> lock(implementation_->config_mutex_);
    implementation_->base_directories_.push_back(std::move(base_directory));
    implementation_->current_base_directory_.store(implementation_->base_directories_.back().get(),
                                                    std::memory_order_release);
}

// 락 없이 현재 판을 읽는다. [inv] 참조는 Logger가 사는 동안 유효하다(판을 지우지 않는다).
const std::filesystem::path& Logger::base_directory() const
{
    return implementation_->current_base_directory_.load(std::memory_order_acquire)->path;
}

// 주문마다(D-094) 불리는 호출자가 많아 락을 잡지 않는다. 폴더 생성은 판마다 처음 한 번뿐이다. [why D-094]
std::filesystem::path Logger::path_for(const std::string& name) const
{
    Implementation::BaseDirectory& base_directory = *implementation_->current_base_directory_.load(std::memory_order_acquire);
    Implementation::ensure_created(base_directory);
    return base_directory.path / name;
}

void Logger::set_console_enabled(bool enabled)
{
    implementation_->console_enabled_.store(enabled, std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const std::string& message)
{
    if (level < min_level_.load(std::memory_order_relaxed))
    {
        return;
    }

    implementation_->submit(level, message);
}

void Logger::flush()
{
    implementation_->flush();
}

uint64_t Logger::dropped() const noexcept
{
    return implementation_->dropped_.load(std::memory_order_relaxed);
}

void Logger::set_min_level(LogLevel min_level)
{
    min_level_.store(min_level, std::memory_order_relaxed);
}

void Logger::info(const std::string& message)
{
    log(LogLevel::INFO, message);
}

void Logger::warn(const std::string& message)
{
    log(LogLevel::WARN, message);
}

void Logger::error(const std::string& message)
{
    log(LogLevel::ERROR, message);
}

void Logger::debug(const std::string& message)
{
    log(LogLevel::DEBUG, message);
}
