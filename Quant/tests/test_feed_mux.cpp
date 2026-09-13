// FeedMux 단위 테스트 — 종목 배정, 소스별 수신 스레드 → mux 스레드 하나로 콜백, 소스 안 순서 보존, 증분 구독·상한·
//  넘침 회수, 체결통보는 첫 소스만, 연결 실패 시 전부 끊기, 링 넘침 카운트.
// 빌드: cmake --build <dir> --target test_feed_mux
#include "core/FeedMux.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

WatchSpec spec(const std::string& t, bool fut = false)
{
    WatchSpec s;
    s.ticker    = t;
    s.is_future = fut;
    return s;
}

// 가짜 소스 — 구독 목록·상한·연결 상태를 흉내 내고, emit_*는 호출 스레드에서 콜백을 부른다.
struct FakeSource final : feed::IFeedSource
{
    explicit FakeSource(int cap, bool connect_ok = true) : cap_(cap), connect_ok_(connect_ok) {}

    void set_callbacks(OrderBookCb ob, TradeCb td) override
    {
        on_ob_ = std::move(ob);
        on_td_ = std::move(td);
    }

    void set_fill_callback(FillCb cb) override
    {
        on_fill_ = std::move(cb);
    }

    bool connect(const std::vector<WatchSpec>& specs) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = connect_ok_;
        specs_.clear();
        overflow_.clear();

        for (const auto& s : specs)
        {
            if (static_cast<int>(specs_.size()) >= cap_)
            {
                overflow_.push_back(s);
            }
            else
            {
                specs_.insert(s.ticker);
            }
        }

        return connect_ok_;
    }

    void disconnect() override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = false;
        ++disconnects;
    }

    bool subscribe_incremental(const WatchSpec& s) override
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (specs_.count(s.ticker))
        {
            return false;
        }

        if (static_cast<int>(specs_.size()) >= cap_)
        {
            return false;
        }

        specs_.insert(s.ticker);
        return true;
    }

    bool has_spec(const WatchSpec& s) const override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return specs_.count(s.ticker) > 0;
    }

    std::vector<WatchSpec> take_overflow_specs() override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto                        out = std::move(overflow_);
        overflow_.clear();
        return out;
    }

    bool is_connected() const override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return connected_;
    }

    bool is_stale(int) const override
    {
        return stale;
    }

    void emit_trade(const std::string& ticker, double px)
    {
        TradeData td;
        td.ticker = ticker;
        td.price  = px;
        on_td_(td);
    }

    void emit_book(const std::string& ticker)
    {
        OrderBook ob;
        ob.ticker = ticker;
        on_ob_(ob);
    }

    bool emit_fill(const std::string& odno)
    {
        if (!on_fill_)
        {
            return false;
        }

        FillNotification fn;
        fn.odno = odno;
        on_fill_(fn);
        return true;
    }

    size_t spec_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return specs_.size();
    }

    std::atomic<bool> stale{false};
    int               disconnects = 0;

private:
    mutable std::mutex     mtx_;
    int                    cap_;
    bool                   connect_ok_;
    bool                   connected_ = false;
    std::set<std::string>  specs_;
    std::vector<WatchSpec> overflow_;
    OrderBookCb            on_ob_;
    TradeCb                on_td_;
    FillCb                 on_fill_;
};

struct Sink
{
    std::mutex                   mtx;
    std::vector<TradeData>       trades;
    std::vector<OrderBook>       books;
    std::vector<FillNotification> fills;
    std::set<std::thread::id>    threads;

    size_t total()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return trades.size() + books.size() + fills.size();
    }

    bool wait_total(size_t n)
    {
        for (int i = 0; i < 500; ++i)
        {
            if (total() >= n)
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        return false;
    }
};

void attach(feed::FeedMux& mux, Sink& sink)
{
    mux.set_callbacks(
        [&sink](const OrderBook& ob)
        {
            std::lock_guard<std::mutex> lk(sink.mtx);
            sink.books.push_back(ob);
            sink.threads.insert(std::this_thread::get_id());
        },
        [&sink](const TradeData& td)
        {
            std::lock_guard<std::mutex> lk(sink.mtx);
            sink.trades.push_back(td);
            sink.threads.insert(std::this_thread::get_id());
        });
    mux.set_fill_callback(
        [&sink](const FillNotification& fn)
        {
            std::lock_guard<std::mutex> lk(sink.mtx);
            sink.fills.push_back(fn);
            sink.threads.insert(std::this_thread::get_id());
        });
}
} // namespace

int main()
{
    // 1. 배정과 전달: 5종목을 두 소스에 3/2로 나누고, 소스마다 다른 스레드가 쏜 이벤트가 mux 스레드 하나로 온다.
    //    같은 소스 안 순서는 그대로다.
    {
        auto a  = std::make_unique<FakeSource>(40);
        auto b  = std::make_unique<FakeSource>(40);
        auto* pa = a.get();
        auto* pb = b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 1024);
        Sink          sink;
        attach(mux, sink);

        const std::vector<WatchSpec> specs = {spec("A"), spec("B"), spec("C"), spec("D"), spec("E")};
        CHECK(mux.connect(specs));
        CHECK(mux.is_connected() && !mux.is_stale(10));
        CHECK(pa->spec_count() == 3 && pb->spec_count() == 2);
        CHECK(mux.source_of(spec("A")) == 0 && mux.source_of(spec("B")) == 1 && mux.source_of(spec("E")) == 0);
        CHECK(mux.has_spec(spec("C")) && !mux.has_spec(spec("Z")));

        std::thread ta([pa] { for (int i = 0; i < 1000; ++i) { pa->emit_trade("A", 1000 + i); } });
        std::thread tb([pb] { for (int i = 0; i < 1000; ++i) { pb->emit_trade("B", 2000 + i); } pb->emit_book("B"); });
        ta.join();
        tb.join();
        CHECK(sink.wait_total(2001));
        CHECK(sink.threads.size() == 1 && sink.threads.count(std::this_thread::get_id()) == 0);
        CHECK(sink.books.size() == 1 && sink.trades.size() == 2000);

        double last_a = 0, last_b = 0;
        bool   ordered = true;

        for (const auto& td : sink.trades)
        {
            double& last = td.ticker == "A" ? last_a : last_b;
            ordered      = ordered && td.price > last;
            last         = td.price;
        }

        CHECK(ordered);
        CHECK(mux.dropped() == 0 && mux.high_water(0) > 0);

        // 체결통보는 첫 소스만 등록된다.
        CHECK(pa->emit_fill("X1"));
        CHECK(!pb->emit_fill("X2"));
        CHECK(sink.wait_total(2002) && sink.fills.size() == 1 && sink.fills[0].odno == "X1");

        // 재연결: 배정은 유지된다.
        mux.disconnect();
        CHECK(!mux.is_connected() && mux.is_stale(10));
        CHECK(mux.connect({spec("E"), spec("A"), spec("B")}));
        CHECK(mux.source_of(spec("E")) == 0 && mux.source_of(spec("B")) == 1);
    }

    // 2. 증분 구독은 배정이 적은 소스로, 상한에 걸리면 배정을 지운다. 넘침 회수는 소스 전부를 합친다.
    {
        auto a  = std::make_unique<FakeSource>(2);
        auto b  = std::make_unique<FakeSource>(2);
        auto* pa = a.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 64);
        Sink          sink;
        attach(mux, sink);

        CHECK(mux.connect({spec("A"), spec("B"), spec("C")}));
        CHECK(mux.source_of(spec("C")) == 0);
        CHECK(mux.subscribe_incremental(spec("D")));
        CHECK(mux.source_of(spec("D")) == 1);
        CHECK(!mux.subscribe_incremental(spec("D")) && mux.has_spec(spec("D"))); // 중복
        CHECK(!mux.subscribe_incremental(spec("E")) && !mux.has_spec(spec("E")));  // 상한
        CHECK(!mux.source_of(spec("E")).has_value());

        // 선물은 같은 코드라도 다른 키.
        CHECK(!mux.has_spec(spec("A", true)));

        // 넘침: 소스 a에 3개 넣어 1개 넘치게 하고 회수하면 배정에서 빠진다.
        CHECK(mux.connect({spec("A"), spec("C"), spec("F"), spec("G"), spec("H")}));
        const auto over = mux.take_overflow_specs();
        CHECK(!over.empty());

        for (const auto& s : over)
        {
            CHECK(!mux.source_of(s).has_value());
        }

        CHECK(pa->spec_count() == 2);
    }

    // 3. 소스 하나가 연결에 실패하면 전부 끊고 false. stale은 하나라도 멈추면 true.
    {
        auto a  = std::make_unique<FakeSource>(40);
        auto b  = std::make_unique<FakeSource>(40, /*connect_ok=*/false);
        auto* pa = a.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 64);
        Sink          sink;
        attach(mux, sink);
        CHECK(!mux.connect({spec("A"), spec("B")}));
        CHECK(pa->disconnects == 1 && !mux.is_connected());
    }

    {
        auto a  = std::make_unique<FakeSource>(40);
        auto b  = std::make_unique<FakeSource>(40);
        auto* pb = b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 64);
        Sink          sink;
        attach(mux, sink);
        CHECK(mux.connect({spec("A"), spec("B")}));
        CHECK(!mux.is_stale(10));
        pb->stale = true;
        CHECK(mux.is_stale(10));
    }

    // 3b. 멈춘 소스만 다시 잇는다 — 살아 있는 소스는 끊지 않고 종목도 그대로, 멈춘 소스는 자기 배정 종목으로 다시 붙는다.
    //     배정이 없던 새 종목은 다시 잇는 소스에 붙는다. 전부 멈췄으면 전부 다시 잇는다.
    {
        auto  a  = std::make_unique<FakeSource>(40);
        auto  b  = std::make_unique<FakeSource>(40);
        auto* pa = a.get();
        auto* pb = b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 64);
        Sink          sink;
        attach(mux, sink);
        const std::vector<WatchSpec> four{spec("A"), spec("B"), spec("C"), spec("D")};
        CHECK(mux.connect(four));
        CHECK(pa->spec_count() == 2 && pb->spec_count() == 2);
        CHECK(mux.source_of(spec("B")) == std::optional<size_t>(1) && mux.source_of(spec("D")) == std::optional<size_t>(1));

        pb->stale = true;
        std::vector<WatchSpec> five = four;
        five.push_back(spec("E")); // 재스캔이 더한 새 종목 — 배정이 없다
        CHECK(mux.reconnect_stale(five, 10));
        CHECK(pa->disconnects == 0 && pa->spec_count() == 2);
        CHECK(pb->disconnects == 1 && pb->spec_count() == 3);
        CHECK(pb->has_spec(spec("B")) && pb->has_spec(spec("D")) && pb->has_spec(spec("E")));
        CHECK(mux.source_of(spec("E")) == std::optional<size_t>(1));
        pb->stale = false;
        CHECK(!mux.is_stale(10));

        pa->stale = true;
        pb->stale = true;
        CHECK(mux.reconnect_stale(five, 10));
        CHECK(pa->disconnects == 1 && pb->disconnects == 2);
        CHECK(pa->spec_count() + pb->spec_count() == 5);
    }

    // 4. 링이 차면 버리고 센다 — 콜백이 막혀 있는 동안 링 용량보다 많이 쏜다.
    {
        auto a  = std::make_unique<FakeSource>(40);
        auto* pa = a.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        feed::FeedMux     mux(std::move(srcs), 16);
        std::atomic<bool> gate{true};
        std::atomic<int>  got{0};
        mux.set_callbacks([](const OrderBook&) {},
                          [&](const TradeData&)
                          {
                              while (gate.load())
                              {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                              }

                              ++got;
                          });
        CHECK(mux.connect({spec("A")}));

        for (int i = 0; i < 100; ++i)
        {
            pa->emit_trade("A", i);
        }

        gate = false;

        for (int i = 0; i < 500 && got.load() + static_cast<int>(mux.dropped()) < 100; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        CHECK(mux.dropped() > 0 && got.load() + static_cast<int>(mux.dropped()) == 100);
        CHECK(mux.high_water(0) == 16);
    }

    // 5. 소스가 없으면 connect는 false.
    {
        feed::FeedMux mux({}, 64);
        CHECK(!mux.connect({spec("A")}) && mux.source_count() == 0);
    }

    // 6. 레인 모드: 소스 i가 쏜 이벤트는 그 스레드에서 레인 i를 달고 바로 온다 — mux 스레드도 링도 안 거친다.
    //    체결통보는 첫 소스만, 역시 그 스레드에서. 소스 하나짜리 기본 구현은 lanes()=1, 레인 0.
    {
        auto  a  = std::make_unique<FakeSource>(40);
        auto  b  = std::make_unique<FakeSource>(40);
        auto* pa = a.get();
        auto* pb = b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
        srcs.push_back(std::move(a));
        srcs.push_back(std::move(b));
        feed::FeedMux mux(std::move(srcs), 64);
        CHECK(mux.lanes() == 2);

        std::mutex                        mtx;
        std::vector<std::pair<uint32_t, std::thread::id>> seen; // (레인, 부른 스레드)
        std::vector<std::string>          tickers;
        std::atomic<int>                  fills{0};
        std::thread::id                   fill_thread;
        mux.set_lane_callbacks(
            [&](uint32_t lane, const OrderBook& ob)
            {
                std::lock_guard<std::mutex> lk(mtx);
                seen.emplace_back(lane, std::this_thread::get_id());
                tickers.push_back(ob.ticker.str());
            },
            [&](uint32_t lane, const TradeData& td)
            {
                std::lock_guard<std::mutex> lk(mtx);
                seen.emplace_back(lane, std::this_thread::get_id());
                tickers.push_back(td.ticker.str());
            });
        mux.set_fill_callback(
            [&](const FillNotification&)
            {
                fill_thread = std::this_thread::get_id();
                fills.fetch_add(1);
            });
        CHECK(mux.connect({spec("A"), spec("B")}));

        std::thread ta([pa] { for (int i = 0; i < 300; ++i) { pa->emit_trade("A", 1 + i); } pa->emit_book("A"); });
        std::thread tb([pb] { for (int i = 0; i < 300; ++i) { pb->emit_trade("B", 1 + i); } });
        const auto id_a = ta.get_id();
        const auto id_b = tb.get_id();
        ta.join();
        tb.join();

        // join 뒤라 더 올 게 없다 — 링이 없으니 기다릴 것도 없다.
        bool lanes_ok = seen.size() == 601;

        for (size_t i = 0; i < seen.size() && lanes_ok; ++i)
        {
            const bool from_a = seen[i].second == id_a;
            lanes_ok          = (from_a || seen[i].second == id_b) && seen[i].first == (from_a ? 0u : 1u) &&
                       tickers[i] == (from_a ? "A" : "B");
        }

        CHECK(lanes_ok);
        CHECK(mux.dropped() == 0 && mux.high_water(0) == 0 && mux.high_water(1) == 0);

        bool        fill_sent = false;
        std::thread tf([pa, &fill_sent] { fill_sent = pa->emit_fill("F1"); });
        const auto id_f = tf.get_id();
        tf.join();
        CHECK(fill_sent && fills.load() == 1 && fill_thread == id_f);
        CHECK(!pb->emit_fill("F2") && fills.load() == 1);

        FakeSource one(4);
        uint32_t   got_lane = 9;
        CHECK(one.lanes() == 1);
        one.set_lane_callbacks([&](uint32_t lane, const OrderBook&) { got_lane = lane; },
                               [&](uint32_t lane, const TradeData&) { got_lane = lane; });
        one.emit_trade("Z", 1.0);
        CHECK(got_lane == 0);
    }

    std::cout << "test_feed_mux: " << g_checks << " checks passed\n";
    return 0;
}
