// 피드 소스 여러 개를 한 IFeedSource로 묶는다 — Engine은 소켓이 몇 개든 하나만 본다. 소켓마다 수신 스레드가 따로
//  돌고(원칙 1), 종목은 한 소스에만 배정된다(원칙 2). 콜백 전달은 두 모드다 — 레인 모드(set_lane_callbacks)는 소스 i의
//  수신 스레드가 레인 i를 달고 Engine 콜백을 직접 부른다(Engine의 N행 행렬이 소비자라 생산자를 모을 이유가 없다, 원칙 5).
//  mux 모드(set_callbacks)는 소스당 SPSC 링 하나를 mux 스레드 하나가 돌아가며 비워 콜백을 부른다 — 소비자가 하나여야
//  하는 쪽(시험·단일 큐)만 쓴다. 소켓이 하나면 끼우지 않는다.
// 스레드: 레인 모드는 소스의 수신 스레드가 콜백까지. mux 모드는 수신 스레드가 push, mux 스레드가 pop·콜백.
//  connect/subscribe는 Engine의 제어·데이터 스레드. 체결통보는 첫 소스만 넘긴다 — KIS는 세션마다 같은 통보를
//  보내므로 둘 이상 받으면 원장이 두 번 센다. 레인 모드에서도 첫 소스 스레드 하나만 부르므로 통보 큐는 SPSC로 남는다. [why D-071]
#pragma once
#include "core/IFeedSource.h"
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "core/WakeGate.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace feed
{

class FeedMux final : public IFeedSource
{
public:
    using Event = std::variant<OrderBook, TradeData, FillNotification>;

    // ring_capacity는 소스당 링 슬롯 수(2의 거듭제곱으로 올림). 가득 차면 수신 스레드는 버리고 dropped()로 센다 —
    //  막히면 소켓 뒤로 밀려 그 소스의 모든 종목이 늦어진다(원칙 3).
    explicit FeedMux(std::vector<std::unique_ptr<IFeedSource>> sources, size_t ring_capacity = 1u << 16)
        : sources_(std::move(sources))
    {
        lanes_.reserve(sources_.size());

        for (size_t i = 0; i < sources_.size(); ++i)
        {
            lanes_.push_back(std::make_unique<Lane>(ring_capacity));
        }
    }

    ~FeedMux() override
    {
        mux_thread_ = {}; // request_stop + join. 소스 소멸(수신 스레드 정지)보다 먼저 — 링을 비우는 쪽이 먼저 선다.
    }

    FeedMux(const FeedMux&)            = delete;
    FeedMux& operator=(const FeedMux&) = delete;

    uint32_t lanes() const override
    {
        return static_cast<uint32_t>(sources_.size());
    }

    // 레인 모드 — 소스 i의 수신 스레드가 레인 i를 달고 Engine 콜백을 직접 부른다. 링·mux 스레드를 거치지 않는다.
    //  set_callbacks와 같이 쓰지 않는다(나중에 부른 쪽이 소스 콜백을 덮는다).
    void set_lane_callbacks(LaneOrderBookCb on_ob, LaneTradeCb on_trade) override
    {
        lane_ob_    = std::move(on_ob);
        lane_trade_ = std::move(on_trade);
        lane_mode_  = true;

        for (size_t i = 0; i < sources_.size(); ++i)
        {
            const uint32_t lane = static_cast<uint32_t>(i);
            sources_[i]->set_callbacks([this, lane](const OrderBook& ob) { lane_ob_(lane, ob); },
                                       [this, lane](const TradeData& td) { lane_trade_(lane, td); });
        }
    }

    // mux 모드 — 소스마다 "자기 링에 push" 콜백을 등록하고 mux 스레드를 띄운다. Engine 콜백은 mux 스레드에서만 불린다.
    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade) override
    {
        on_ob_     = std::move(on_ob);
        on_trade_  = std::move(on_trade);
        lane_mode_ = false;

        for (size_t i = 0; i < sources_.size(); ++i)
        {
            Lane* lane = lanes_[i].get();
            sources_[i]->set_callbacks([this, lane](const OrderBook& ob) { enqueue(*lane, Event{ob}); },
                                       [this, lane](const TradeData& td) { enqueue(*lane, Event{td}); });
        }

        if (!mux_thread_.joinable())
        {
            mux_thread_ = std::jthread([this](std::stop_token st) { mux_loop(st); });
        }
    }

    void set_fill_callback(FillCb cb) override
    {
        on_fill_ = std::move(cb);

        if (!sources_.empty())
        {
            // 모드는 부르는 시점에 본다 — set_callbacks/set_lane_callbacks와 등록 순서에 매이지 않게.
            Lane* lane = lanes_[0].get();
            sources_[0]->set_fill_callback(
                [this, lane](const FillNotification& fn)
                {
                    if (lane_mode_)
                    {
                        on_fill_(fn);
                        return;
                    }

                    enqueue(*lane, Event{fn});
                });
        }
    }

    // specs를 소스에 고르게 나눠(i % N) 각각 connect한다. 이미 배정된 종목은 그 소스를 지킨다(재연결).
    //  하나라도 실패하면 전부 끊고 false — Engine의 재연결 판단이 소스 단위가 아니라 하나이기 때문이다.
    bool connect(const std::vector<WatchSpec>& specs) override
    {
        if (sources_.empty())
        {
            return false;
        }

        std::vector<std::vector<WatchSpec>> per_source(sources_.size());
        {
            std::lock_guard<std::mutex> lk(assign_mtx_);
            size_t                      next = 0;

            for (const auto& s : specs)
            {
                const auto it = assign_.find(key(s));
                size_t     idx;

                if (it != assign_.end())
                {
                    idx = it->second;
                }
                else
                {
                    idx = next++ % sources_.size();
                    assign_.emplace(key(s), idx);
                }

                per_source[idx].push_back(s);
            }
        }

        for (size_t i = 0; i < sources_.size(); ++i)
        {
            if (!sources_[i]->connect(per_source[i]))
            {
                for (size_t j = 0; j < i; ++j)
                {
                    sources_[j]->disconnect();
                }

                return false;
            }
        }

        return true;
    }

    void disconnect() override
    {
        for (auto& s : sources_)
        {
            s->disconnect();
        }
    }

    // 새 종목은 배정 수가 가장 적은 소스로. 상한에 걸려 그 소스 목록에서도 빠지면 배정을 지워 has_spec이 false가 된다.
    bool subscribe_incremental(const WatchSpec& spec) override
    {
        size_t idx;
        {
            std::lock_guard<std::mutex> lk(assign_mtx_);
            const auto                  it = assign_.find(key(spec));

            if (it != assign_.end())
            {
                idx = it->second;
            }
            else
            {
                idx = least_loaded_locked();
                assign_.emplace(key(spec), idx);
            }
        }

        const bool sent = sources_[idx]->subscribe_incremental(spec);

        if (!sent && !sources_[idx]->has_spec(spec))
        {
            std::lock_guard<std::mutex> lk(assign_mtx_);
            assign_.erase(key(spec));
        }

        return sent;
    }

    bool has_spec(const WatchSpec& spec) const override
    {
        std::optional<size_t> idx;
        {
            std::lock_guard<std::mutex> lk(assign_mtx_);
            const auto                  it = assign_.find(key(spec));

            if (it != assign_.end())
            {
                idx = it->second;
            }
        }

        return idx && sources_[*idx]->has_spec(spec);
    }

    std::vector<WatchSpec> take_overflow_specs() override
    {
        std::vector<WatchSpec> out;

        for (auto& s : sources_)
        {
            auto part = s->take_overflow_specs();
            out.insert(out.end(), part.begin(), part.end());
        }

        if (!out.empty())
        {
            std::lock_guard<std::mutex> lk(assign_mtx_);

            for (const auto& s : out)
            {
                assign_.erase(key(s));
            }
        }

        return out;
    }

    bool is_connected() const override
    {
        for (const auto& s : sources_)
        {
            if (s->is_connected())
            {
                return true;
            }
        }

        return false;
    }

    // 소스 하나라도 끊겼거나 멈췄으면 stale — Engine이 전체를 다시 잇는다. 소스 단위 재연결은 다음 조각.
    bool is_stale(int threshold_sec) const override
    {
        for (const auto& s : sources_)
        {
            if (!s->is_connected() || s->is_stale(threshold_sec))
            {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] size_t source_count() const noexcept
    {
        return sources_.size();
    }

    // 측정용(원칙 7). 링이 찼을 때 버린 이벤트 수와 소스별 고수위.
    [[nodiscard]] uint64_t dropped() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t high_water(size_t source_idx) const noexcept
    {
        return source_idx < lanes_.size() ? lanes_[source_idx]->ring.high_water() : 0;
    }

    // 종목이 어느 소스에 배정됐는지. 없으면 nullopt. 테스트·진단용.
    [[nodiscard]] std::optional<size_t> source_of(const WatchSpec& spec) const
    {
        std::lock_guard<std::mutex> lk(assign_mtx_);
        const auto                  it = assign_.find(key(spec));
        return it == assign_.end() ? std::nullopt : std::optional<size_t>(it->second);
    }

private:
    struct Lane
    {
        explicit Lane(size_t cap) : ring(cap) {}

        RingBuffer<Event> ring;
    };

    static std::string key(const WatchSpec& s)
    {
        return s.is_future ? s.ticker + "/F" : s.ticker;
    }

    // 수신 스레드에서. 링이 차면 버린다 — 여기서 기다리면 그 소켓의 전 종목이 밀린다.
    void enqueue(Lane& lane, Event&& ev)
    {
        if (!lane.ring.push(std::move(ev)))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        wake_.notify();
    }

    size_t least_loaded_locked() const
    {
        std::vector<size_t> load(sources_.size(), 0);

        for (const auto& [k, idx] : assign_)
        {
            ++load[idx];
        }

        size_t best = 0;

        for (size_t i = 1; i < load.size(); ++i)
        {
            if (load[i] < load[best])
            {
                best = i;
            }
        }

        return best;
    }

    bool all_empty() const
    {
        for (const auto& l : lanes_)
        {
            if (!l->ring.empty())
            {
                return false;
            }
        }

        return true;
    }

    // 링을 돌아가며 비운다. 한 링당 한 바퀴에 kBurst개까지만 — 한 소켓이 바쁘다고 다른 소켓 종목이 굶지 않게.
    void mux_loop(std::stop_token st)
    {
        static constexpr size_t kBurst = 256;

        while (!st.stop_requested())
        {
            bool any = false;

            for (auto& l : lanes_)
            {
                for (size_t n = 0; n < kBurst; ++n)
                {
                    auto ev = l->ring.pop();

                    if (!ev)
                    {
                        break;
                    }

                    any = true;
                    dispatch(*ev);
                }
            }

            if (!any)
            {
                wake_.wait_for(std::chrono::milliseconds(5), st, [this] { return all_empty(); });
            }
        }
    }

    void dispatch(Event& ev)
    {
        if (auto* td = std::get_if<TradeData>(&ev))
        {
            if (on_trade_)
            {
                on_trade_(*td);
            }
        }
        else if (auto* ob = std::get_if<OrderBook>(&ev))
        {
            if (on_ob_)
            {
                on_ob_(*ob);
            }
        }
        else if (auto* fn = std::get_if<FillNotification>(&ev))
        {
            if (on_fill_)
            {
                on_fill_(*fn);
            }
        }
    }

    std::vector<std::unique_ptr<IFeedSource>> sources_;
    std::vector<std::unique_ptr<Lane>>        lanes_; // sources_와 같은 index

    OrderBookCb     on_ob_;    // mux 모드
    TradeCb         on_trade_; // mux 모드
    LaneOrderBookCb lane_ob_;    // 레인 모드
    LaneTradeCb     lane_trade_; // 레인 모드
    FillCb          on_fill_;
    bool            lane_mode_ = false; // 수신 스레드가 돌기 전(connect 전)에 정해진다

    mutable std::mutex                      assign_mtx_;
    std::unordered_map<std::string, size_t> assign_; // key(spec) → 소스 index. 한 종목은 한 소스에만

    sync::WakeGate        wake_;
    std::atomic<uint64_t> dropped_{0};
    std::jthread          mux_thread_; // 멤버 선언 순서상 마지막 — 소멸자가 먼저 세운다
};

} // namespace feed
