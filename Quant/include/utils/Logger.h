#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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
//   큐·writer 스레드·파일 핸들은 `Logger::Implementation`(`Quant/src/utils/Logger.cpp`)에 있다. 이 헤더는 많은 파일이 직접 포함해
//   `<fstream>`·`<iostream>`·`<thread>`·`MpscQueue.h`를 여기 두면 헤더 한 줄 수정에 전체 재컴파일 80초가 들었다(T-3).
//   [inv] 로그를 부르는 스레드는 Logger 소멸(정적 소멸) 전에 join돼 있어야 한다. 소멸 뒤 호출은 미정의.
class Logger
{
public:
    static Logger& instance();

    // 싱글톤 — 사본이 생기면 writer 스레드와 큐가 둘이 된다.
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // 실행파일이 놓인 디렉터리(cwd와 무관). 알 수 없으면 cwd.
    static std::filesystem::path executable_directory();

    // 로그·산출물 기준 디렉터리 기본값: QUANT_LOG_DIR 환경변수 > 실행파일 옆 logs/.
    //  cwd 기준이면 테스트 바이너리를 repo 루트에서 돌릴 때 당일 원장(trades_*.csv)에 TEST 행이 섞인다.
    static std::filesystem::path default_base_directory();

    void initialize(const std::filesystem::path& filepath, LogLevel min_level = LogLevel::INFO);

    // 기동 뒤 임계값만 바꾼다(config "log_level"). 봉 닫힘처럼 하루 한 번 켜 보는 DEBUG 줄을 위해 있다.
    void set_min_level(LogLevel min_level);

    // 매크로가 인자 문자열을 만들기 전에 묻는다. DEBUG 줄은 KIS 응답 본문 substr·봉 닫힘 포맷처럼 결합 비용이 있는데
    //  기본 임계값 INFO에서는 그 문자열이 만들어진 뒤 log()에서 버려지고 있었다. [why D-071]
    [[nodiscard]] bool enabled(LogLevel level) const noexcept
    {
        return level >= min_level_.load(std::memory_order_relaxed);
    }

    // 실행 위치(cwd)와 무관하게 로그·산출물을 한 곳에 모으기 위한 기준 디렉터리.
    // main에서 실행파일 기준 절대경로로 한 번 고정한다(미설정 시 QUANT_LOG_DIR, 그것도 없으면 실행 파일 폴더 아래 "logs").
    //  폴더는 이 호출에서 만든다. 읽는 쪽(base_directory·path_for)은 락을 잡지 않는다.
    void set_base_directory(const std::filesystem::path& directory);
    // [inv] 돌려준 참조는 Logger가 사는 동안 유효하다. 뒤에 set_base_directory가 불려도 이전 값을 가리킬 뿐 끊기지 않는다.
    [[nodiscard]] const std::filesystem::path& base_directory() const;

    // 기준 디렉터리 하위 파일의 전체 경로(기준 폴더가 없으면 처음 한 번 만든다).
    [[nodiscard]] std::filesystem::path path_for(const std::string& name) const;

    // hot path: 임계값 아래면 바로 돌아가고, 아니면 시각 스탬프만 찍어 큐에 넘긴다(포맷팅은 writer가 한다).
    void log(LogLevel level, const std::string& message);

    void info(const std::string& message);

    void warn(const std::string& message);

    void error(const std::string& message);

    void debug(const std::string& message);

    // 이 호출 전에 반환된 log()가 모두 파일/콘솔에 반영될 때까지 블로킹(테스트·종료 직전 정합 확인용).
    void flush();

    // 가득 차서 버린 레코드 수(운영 중 관측용).
    [[nodiscard]] uint64_t dropped() const noexcept;

private:
    struct Implementation;

    Logger();
    ~Logger();

    std::atomic<LogLevel> min_level_{LogLevel::INFO}; // log()·enabled()가 매번 읽는다 — Implementation 포인터를 거치지 않게 여기 둔다
    std::unique_ptr<Implementation> implementation_;
};

#define LOG_INFO(message) Logger::instance().info(message)
#define LOG_WARN(message) Logger::instance().warn(message)
#define LOG_ERROR(message) Logger::instance().error(message)
// DEBUG만 가드한다 — 인자는 임계값을 넘을 때만 평가되므로 부작용 있는 식을 넣지 않는다.
#define LOG_DEBUG(message)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (Logger::instance().enabled(LogLevel::DEBUG))                                                               \
        {                                                                                                              \
            Logger::instance().debug(message);                                                                             \
        }                                                                                                              \
    } while (0)
