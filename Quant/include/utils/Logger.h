#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string& filepath, LogLevel min_level = LogLevel::INFO) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.open(filepath, std::ios::app);
        min_level_ = min_level;
    }

    // 화면 표시 모드일 때 콘솔 출력을 끄고 파일에만 기록
    void set_console_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        console_enabled_ = enabled;
    }

    void log(LogLevel level, const std::string& msg) {
        if (level < min_level_) return;

        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;

        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count()
           << " [" << level_str(level) << "] " << msg;

        std::lock_guard<std::mutex> lock(mutex_);
        if (console_enabled_) std::cout << ss.str() << '\n';
        if (file_.is_open()) file_ << ss.str() << '\n';
    }

    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void error(const std::string& m) { log(LogLevel::ERROR, m); }
    void debug(const std::string& m) { log(LogLevel::DEBUG, m); }

private:
    Logger() = default;
    static const char* level_str(LogLevel l) {
        switch(l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "?????";
    }

    std::mutex    mutex_;
    std::ofstream file_;
    LogLevel      min_level_      = LogLevel::INFO;
    bool          console_enabled_ = true;
};

#define LOG_INFO(msg)  Logger::instance().info(msg)
#define LOG_WARN(msg)  Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
