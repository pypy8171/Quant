// 캡처 파일(TickCapture v1)을 읽어 WS와 같은 콜백으로 되돌려 주는 피드 소스 — 리플레이 백테스트의 입력.
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
#include <thread>
#include <unordered_set>
#include <vector>

namespace feed
{

class ReplaySource final : public IFeedSource
{
public:
    // speed: 1.0이면 캡처 때 간격(recv_ns 차이)대로, 2.0이면 두 배 빠르게, 0이면 쉬지 않고 최대 속도.
    explicit ReplaySource(std::filesystem::path file, double speed = 0.0)
        : file_(std::move(file)), speed_(speed)
    {
    }

    ~ReplaySource() override
    {
        disconnect();
    }

    ReplaySource(const ReplaySource&)            = delete;
    ReplaySource& operator=(const ReplaySource&) = delete;

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade) override
    {
        on_ob_    = std::move(on_ob);
        on_trade_ = std::move(on_trade);
    }

    // specs가 비어 있으면 파일의 전 종목을 재생한다. 파일을 못 열면 false.
    bool connect(const std::vector<WatchSpec>& specs) override
    {
        disconnect();

        {
            std::lock_guard<std::mutex> lk(filter_mtx_);

            for (const auto& s : specs)
            {
                filter_.insert(s.ticker);
            }
        }

        auto reader = std::make_unique<TickReader>(file_);

        if (!reader->ok())
        {
            return false;
        }

        finished_.store(false, std::memory_order_relaxed);
        connected_.store(true, std::memory_order_release);
        thread_ = std::jthread([this, rd = std::move(reader)](std::stop_token st) mutable { run(st, *rd); });
        return true;
    }

    void disconnect() override
    {
        if (thread_.joinable())
        {
            thread_.request_stop();
            thread_.join();
        }

        connected_.store(false, std::memory_order_release);
    }

    // 목록에 넣기만 한다. 파일에 없는 종목이면 아무것도 안 나오는데, 그건 캡처 쪽 문제라 여기서 판정하지 않는다.
    bool subscribe_incremental(const WatchSpec& spec) override
    {
        std::lock_guard<std::mutex> lk(filter_mtx_);
        return filter_.insert(spec.ticker).second;
    }

    bool has_spec(const WatchSpec& spec) const override
    {
        std::lock_guard<std::mutex> lk(filter_mtx_);
        return filter_.empty() || filter_.count(spec.ticker) > 0;
    }

    std::vector<WatchSpec> take_overflow_specs() override
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
    void run(std::stop_token st, TickReader& rd)
    {
        Record  r;
        int64_t prev_ns = 0;

        while (!st.stop_requested() && rd.next(r))
        {
            const Common& c = r.kind == kKindTrade ? r.trade.c : r.book.c;

            if (!pass(c.ticker))
            {
                skipped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (speed_ > 0.0 && prev_ns != 0 && c.recv_ns > prev_ns)
            {
                pace(st, static_cast<int64_t>(static_cast<double>(c.recv_ns - prev_ns) / speed_));
            }

            prev_ns = c.recv_ns;

            if (st.stop_requested())
            {
                break;
            }

            // 캡처 때 recv_ns는 위 간격 계산에만 쓰고, 내보내는 틱에는 이 프로세스 시계를 찍는다 — 구간 지연 CSV가
            //  옛 시계와 지금 시계를 빼는 일이 없게. [why D-071]
            if (r.kind == kKindTrade)
            {
                if (on_trade_)
                {
                    TradeData td = to_trade(r.trade);
                    td.recv_ns   = now_ns();
                    on_trade_(td);
                }
            }
            else if (on_ob_)
            {
                OrderBook ob = to_book(r.book);
                ob.recv_ns   = now_ns();
                on_ob_(ob);
            }

            played_.fetch_add(1, std::memory_order_relaxed);
        }

        // 정지 요청으로 나온 것은 "끝까지 재생"이 아니다.
        finished_.store(!st.stop_requested(), std::memory_order_release);
        connected_.store(false, std::memory_order_release);
    }

    bool pass(const char* ticker) const
    {
        std::lock_guard<std::mutex> lk(filter_mtx_);
        return filter_.empty() || filter_.count(ticker) > 0;
    }

    // 정지 요청에 50ms 안에 응답하도록 잘라 잔다. 시계 격자(2ms)보다 짧은 간격은 sleep 없이 지나간다.
    static void pace(const std::stop_token& st, int64_t wait_ns)
    {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + nanoseconds(wait_ns);

        while (!st.stop_requested())
        {
            const auto left = deadline - steady_clock::now();

            if (left <= milliseconds(2))
            {
                break;
            }

            std::this_thread::sleep_for(left > milliseconds(50) ? milliseconds(50) : duration_cast<milliseconds>(left));
        }
    }

    std::filesystem::path file_;
    double                speed_;
    static int64_t now_ns()
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }

    OrderBookCb           on_ob_;
    TradeCb               on_trade_;

    mutable std::mutex              filter_mtx_;
    std::unordered_set<std::string> filter_; // 비어 있으면 전 종목

    std::jthread          thread_;
    std::atomic<bool>     connected_{false};
    std::atomic<bool>     finished_{false};
    std::atomic<uint64_t> played_{0};
    std::atomic<uint64_t> skipped_{0};
};

} // namespace feed
