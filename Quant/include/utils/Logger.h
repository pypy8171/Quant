#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR
};

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
        std::lock_guard<std::mutex> lock(mutex_);
        // 부모 디렉터리를 먼저 만든다. main에서 실행파일 기준 절대경로가 넘어오므로
        // cwd 위치와 무관하게 로그가 한 폴더에 모인다. (Windows 한글 경로 대비 path로 open)
        std::error_code ec;
        auto parent = filepath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        file_.open(filepath, std::ios::app);
        min_level_ = min_level;
    }

    // 실행 위치(cwd)와 무관하게 로그·산출물을 한 곳에 모으기 위한 기준 디렉터리.
    // main에서 실행파일 기준 절대경로로 한 번 고정한다(미설정 시 cwd 하위 "logs").
    void set_base_dir(const std::filesystem::path& dir)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        base_dir_ = dir;
    }
    std::filesystem::path base_dir()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return base_dir_;
    }
    // 기준 디렉터리 하위 파일의 전체 경로(부모 폴더가 없으면 생성).
    std::filesystem::path path_for(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::error_code ec;
        std::filesystem::create_directories(base_dir_, ec);
        return base_dir_ / name;
    }

    // 화면 표시 모드일 때 콘솔 출력을 끄고 파일에만 기록
    void set_console_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        console_enabled_ = enabled;
    }

    void log(LogLevel level, const std::string& msg)
    {
        if (level < min_level_)
            return;

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << ms.count() << " [" << level_str(level) << "] " << msg;

        std::lock_guard<std::mutex> lock(mutex_);
        if (console_enabled_)
            std::cout << ss.str() << '\n';
        if (file_.is_open())
            file_ << ss.str() << '\n';
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

private:
    Logger() = default;
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

    std::mutex mutex_;
    std::ofstream file_;
    LogLevel min_level_ = LogLevel::INFO;
    bool console_enabled_ = true;
    std::filesystem::path base_dir_{"logs"}; // set_base_dir 전 기본값(하위호환)
};

#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARN(msg) Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
