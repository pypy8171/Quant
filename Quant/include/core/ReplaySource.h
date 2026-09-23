// 캡처 파일(TickCapture v1·v2)을 읽어 WS와 같은 콜백으로 되돌려 주는 피드 소스 — 리플레이 백테스트의 입력.
// 스레드: connect()가 재생 스레드 하나를 띄우고 그 스레드가 콜백을 부른다(WS 수신 스레드 자리). [why D-071]
#pragma once
#include "core/IFeedSource.h"
#include "core/TickCapture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <functional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace feed
{

class ReplaySource final : public IFeedSource
{
public:
    // speed: 1.0이면 캡처 때 간격(received_ns 차이)대로, 2.0이면 두 배 빠르게, 0이면 쉬지 않고 최대 속도.
    explicit ReplaySource(std::filesystem::path file, double speed = 0.0)
        : file_(std::move(file)), speed_(speed)
    {
    }

    ~ReplaySource() override;

    ReplaySource(const ReplaySource&)            = delete;
    ReplaySource& operator=(const ReplaySource&) = delete;

    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override
    {
        on_order_book_    = std::move(on_order_book);
        on_trade_ = std::move(on_trade);
    }

    // specs가 비어 있으면 파일의 전 종목을 재생한다. 파일을 못 열면 false.
    bool connect(const std::vector<WatchSpec>& specifications) override;

    void disconnect() override;

    // 목록에 넣기만 한다. 파일에 없는 종목이면 아무것도 안 나오는데, 그건 캡처 쪽 문제라 여기서 판정하지 않는다.
    bool subscribe_incremental(const WatchSpec& specification) override
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        return filter_.insert(specification.ticker).second;
    }

    bool has_specification(const WatchSpec& specification) const override
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        return filter_.empty() || filter_.count(specification.ticker) > 0;
    }

    std::vector<WatchSpec> take_overflow_specifications() override
    {
        return {};
    }

    bool is_connected() const override
    {
        return connected_.load(std::memory_order_acquire);
    }

    // 리플레이는 재연결 경로를 타지 않는다 — 파일이 끝나도 stale이 아니라 finished()다.
    bool is_stale(int) const override
    {
        return false;
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return finished_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t played() const noexcept
    {
        return played_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t skipped() const noexcept
    {
        return skipped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return file_;
    }

private:
    void run(std::stop_token stop_token, TickReader& reader);

    bool pass(std::string_view ticker) const
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        return filter_.empty() || filter_.count(ticker) > 0;
    }

    // 문자열 집합을 string_view로 찾게 하는 해시 — 레코드마다 std::string을 만들지 않는다.
    struct TransparentStringHash
    {
        using is_transparent = void;

        size_t operator()(std::string_view text) const noexcept
        {
            return std::hash<std::string_view>{}(text);
        }
    };

    // 정지 요청에 50ms 안에 응답하도록 잘라 잔다. 시계 격자(2ms)보다 짧은 간격은 sleep 없이 지나간다.
    static void pace(const std::stop_token& stop_token, int64_t wait_ns);

    std::filesystem::path file_;
    double                speed_;
    static int64_t now_ns();

    OrderBookCb           on_order_book_;
    TradeCb               on_trade_;

    mutable std::mutex                                                       filter_mutex_;
    // 문자열인 이유: 캡처 파일 레코드의 티커가 문자열이고 소스 계층은 종목 테이블 앞이다(라이브 소켓과 같은 자리).
    //  string_view로 찾아 레코드마다 복사는 없다.
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> filter_; // 비어 있으면 전 종목

    std::jthread          thread_;
    std::atomic<bool>     connected_{false};
    std::atomic<bool>     finished_{false};
    std::atomic<uint64_t> played_{0};
    std::atomic<uint64_t> skipped_{0};
};

} // namespace feed
