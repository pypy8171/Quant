// FeedMux 단위 테스트 — 종목 배정, 소스별 수신 스레드 → multiplexer 스레드 하나로 콜백, 소스 안 순서 보존, 증분 구독·상한·
//  넘침 회수, 체결통보는 맡은 소스 하나만(자리가 아니라 owns_fill_notice로 고른다), 연결 실패 시 전부 끊기, 링 넘침 카운트.
// 빌드: cmake --build <directory> --target test_feed_mux
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

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

WatchSpec specification(const std::string& ticker)
{
    WatchSpec specification;
    specification.ticker = ticker;
    return specification;
}

// 가짜 소스 — 구독 목록·상한·연결 상태를 흉내 내고, emit_*는 호출 스레드에서 콜백을 부른다.
struct FakeSource final : feed::IFeedSource
{
    explicit FakeSource(int capture, bool connect_ok = true) : capacity_(capture), connect_ok_(connect_ok) {}

    void set_callbacks(OrderBookCb order_book, TradeCb trade) override
    {
        on_order_book_ = std::move(order_book);
        on_trade_ = std::move(trade);
    }

    void set_fill_callback(FillCb callback) override
    {
        on_fill_ = std::move(callback);
    }

    bool connect(const std::vector<WatchSpec>& specifications) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = connect_ok_;
        specifications_.clear();
        overflow_.clear();

        for (const auto& specification : specifications)
        {
            if (static_cast<int>(specifications_.size()) >= capacity_)
            {
                overflow_.push_back(specification);
            }
            else
            {
                specifications_.insert(specification.ticker);
            }
        }

        return connect_ok_;
    }

    void disconnect() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        ++disconnects;
    }

    bool subscribe_incremental(const WatchSpec& specification) override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (specifications_.count(specification.ticker))
        {
            return false;
        }

        if (static_cast<int>(specifications_.size()) >= capacity_)
        {
            return false;
        }

        specifications_.insert(specification.ticker);
        return true;
    }

    bool has_specification(const WatchSpec& specification) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return specifications_.count(specification.ticker) > 0;
    }

    std::vector<WatchSpec> take_overflow_specifications() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        out = std::move(overflow_);
        overflow_.clear();
        return out;
    }

    bool is_connected() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool is_stale(int) const override
    {
        return stale;
    }

    void emit_trade(const std::string& ticker, double price)
    {
        TradeData trade;
        trade.ticker = ticker;
        trade.price  = price;
        on_trade_(trade);
    }

    void emit_book(const std::string& ticker)
    {
        OrderBook order_book;
        order_book.ticker = ticker;
        on_order_book_(order_book);
    }

    bool emit_fill(const std::string& kis_order_no)
    {
        if (!on_fill_)
        {
            return false;
        }

        FillNotification fill_notification;
        fill_notification.kis_order_no = kis_order_no;
        on_fill_(fill_notification);
        return true;
    }

    size_t specification_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return specifications_.size();
    }

    bool owns_fill_notice() const override
    {
        return fill_notice;
    }

    std::atomic<bool> stale{false};
    int               disconnects = 0;
    bool              fill_notice = true; // 이 소스가 체결통보를 맡나(설정의 hts_id 자리)

private:
    mutable std::mutex     mutex_;
    int                    capacity_;
    bool                   connect_ok_;
    bool                   connected_ = false;
    std::set<std::string>  specifications_;
    std::vector<WatchSpec> overflow_;
    OrderBookCb            on_order_book_;
    TradeCb                on_trade_;
    FillCb                 on_fill_;
};

struct Sink
{
    std::mutex                   mutex;
    std::vector<TradeData>       trades;
    std::vector<OrderBook>       books;
    std::vector<FillNotification> fills;
    std::set<std::thread::id>    threads;

    size_t total()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return trades.size() + books.size() + fills.size();
    }

    bool wait_total(size_t count)
    {
        for (int index = 0; index < 500; ++index)
        {
            if (total() >= count)
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        return false;
    }
};

void attach(feed::FeedMux& multiplexer, Sink& sink)
{
    multiplexer.set_callbacks(
        [&sink](const OrderBook& order_book)
        {
            std::lock_guard<std::mutex> lock(sink.mutex);
            sink.books.push_back(order_book);
            sink.threads.insert(std::this_thread::get_id());
        },
        [&sink](const TradeData& trade)
        {
            std::lock_guard<std::mutex> lock(sink.mutex);
            sink.trades.push_back(trade);
            sink.threads.insert(std::this_thread::get_id());
        });
    multiplexer.set_fill_callback(
        [&sink](const FillNotification& fill_notification)
        {
            std::lock_guard<std::mutex> lock(sink.mutex);
            sink.fills.push_back(fill_notification);
            sink.threads.insert(std::this_thread::get_id());
        });
}
} // namespace

int main()
{
    // 1. 배정과 전달: 5종목을 두 소스에 3/2로 나누고, 소스마다 다른 스레드가 쏜 이벤트가 multiplexer 스레드 하나로 온다.
    //    같은 소스 안 순서는 그대로다.
    {
        auto fake_source_a  = std::make_unique<FakeSource>(40);
        auto fake_source_b  = std::make_unique<FakeSource>(40);
        auto* source_a = fake_source_a.get();
        auto* source_b = fake_source_b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 1024);
        Sink          sink;
        attach(multiplexer, sink);

        const std::vector<WatchSpec> specifications = {specification("A"), specification("B"), specification("C"), specification("D"), specification("E")};
        CHECK(multiplexer.connect(specifications));
        CHECK(multiplexer.is_connected() && !multiplexer.is_stale(10));
        CHECK(source_a->specification_count() == 3 && source_b->specification_count() == 2);
        CHECK(multiplexer.source_of(specification("A")) == 0 && multiplexer.source_of(specification("B")) == 1 && multiplexer.source_of(specification("E")) == 0);
        CHECK(multiplexer.has_specification(specification("C")) && !multiplexer.has_specification(specification("Z")));

        std::thread thread_a([source_a]
        {
            for (int index = 0; index < 1000; ++index)
            {
                source_a->emit_trade("A", 1000 + index);
            }
        });
        std::thread thread_b([source_b]
        {
            for (int index = 0; index < 1000; ++index)
            {
                source_b->emit_trade("B", 2000 + index);
            }

            source_b->emit_book("B");
        });
        thread_a.join();
        thread_b.join();
        CHECK(sink.wait_total(2001));
        CHECK(sink.threads.size() == 1 && sink.threads.count(std::this_thread::get_id()) == 0);
        CHECK(sink.books.size() == 1 && sink.trades.size() == 2000);

        double last_a = 0, last_b = 0;
        bool   ordered = true;

        for (const auto& trade : sink.trades)
        {
            double& last = trade.ticker == "A" ? last_a : last_b;
            ordered      = ordered && trade.price > last;
            last         = trade.price;
        }

        CHECK(ordered);
        CHECK(multiplexer.dropped() == 0 && multiplexer.high_water(0) > 0);

        // 체결통보는 첫 소스만 등록된다.
        CHECK(source_a->emit_fill("X1"));
        CHECK(!source_b->emit_fill("X2"));
        CHECK(sink.wait_total(2002) && sink.fills.size() == 1 && sink.fills[0].kis_order_no == "X1");

        // 재연결: 배정은 유지된다.
        multiplexer.disconnect();
        CHECK(!multiplexer.is_connected() && multiplexer.is_stale(10));
        CHECK(multiplexer.connect({specification("E"), specification("A"), specification("B")}));
        CHECK(multiplexer.source_of(specification("E")) == 0 && multiplexer.source_of(specification("B")) == 1);
    }

    // 2. 증분 구독은 배정이 적은 소스로, 상한에 걸리면 배정을 지운다. 넘침 회수는 소스 전부를 합친다.
    {
        auto fake_source_a  = std::make_unique<FakeSource>(2);
        auto fake_source_b  = std::make_unique<FakeSource>(2);
        auto* source_a = fake_source_a.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);
        Sink          sink;
        attach(multiplexer, sink);

        CHECK(multiplexer.connect({specification("A"), specification("B"), specification("C")}));
        CHECK(multiplexer.source_of(specification("C")) == 0);
        CHECK(multiplexer.subscribe_incremental(specification("D")));
        CHECK(multiplexer.source_of(specification("D")) == 1);
        CHECK(!multiplexer.subscribe_incremental(specification("D")) && multiplexer.has_specification(specification("D"))); // 중복
        CHECK(!multiplexer.subscribe_incremental(specification("E")) && !multiplexer.has_specification(specification("E")));  // 상한
        CHECK(!multiplexer.source_of(specification("E")).has_value());

        // 넘침: 소스 a에 3개 넣어 1개 넘치게 하고 회수하면 배정에서 빠진다.
        CHECK(multiplexer.connect({specification("A"), specification("C"), specification("F"), specification("G"), specification("H")}));
        const auto over = multiplexer.take_overflow_specifications();
        CHECK(!over.empty());

        for (const auto& overflow_entry : over)
        {
            CHECK(!multiplexer.source_of(overflow_entry).has_value());
        }

        CHECK(source_a->specification_count() == 2);
    }

    // 3. 소스 하나가 연결에 실패하면 전부 끊고 false. stale은 하나라도 멈추면 true.
    {
        auto fake_source_a  = std::make_unique<FakeSource>(40);
        auto fake_source_b  = std::make_unique<FakeSource>(40, /*connect_ok=*/false);
        auto* source_a = fake_source_a.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);
        Sink          sink;
        attach(multiplexer, sink);
        CHECK(!multiplexer.connect({specification("A"), specification("B")}));
        CHECK(source_a->disconnects == 1 && !multiplexer.is_connected());
    }

    {
        auto fake_source_a  = std::make_unique<FakeSource>(40);
        auto fake_source_b  = std::make_unique<FakeSource>(40);
        auto* source_b = fake_source_b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);
        Sink          sink;
        attach(multiplexer, sink);
        CHECK(multiplexer.connect({specification("A"), specification("B")}));
        CHECK(!multiplexer.is_stale(10));
        source_b->stale = true;
        CHECK(multiplexer.is_stale(10));
    }

    // 3b. 멈춘 소스만 다시 잇는다 — 살아 있는 소스는 끊지 않고 종목도 그대로, 멈춘 소스는 자기 배정 종목으로 다시 붙는다.
    //     배정이 없던 새 종목은 다시 잇는 소스에 붙는다. 전부 멈췄으면 전부 다시 잇는다.
    {
        auto  fake_source_a  = std::make_unique<FakeSource>(40);
        auto  fake_source_b  = std::make_unique<FakeSource>(40);
        auto* source_a = fake_source_a.get();
        auto* source_b = fake_source_b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);
        Sink          sink;
        attach(multiplexer, sink);
        const std::vector<WatchSpec> four{specification("A"), specification("B"), specification("C"), specification("D")};
        CHECK(multiplexer.connect(four));
        CHECK(source_a->specification_count() == 2 && source_b->specification_count() == 2);
        CHECK(multiplexer.source_of(specification("B")) == std::optional<size_t>(1) && multiplexer.source_of(specification("D")) == std::optional<size_t>(1));

        source_b->stale = true;
        std::vector<WatchSpec> five = four;
        five.push_back(specification("E")); // 재스캔이 더한 새 종목 — 배정이 없다
        CHECK(multiplexer.reconnect_stale(five, 10));
        CHECK(source_a->disconnects == 0 && source_a->specification_count() == 2);
        CHECK(source_b->disconnects == 1 && source_b->specification_count() == 3);
        CHECK(source_b->has_specification(specification("B")) && source_b->has_specification(specification("D")) && source_b->has_specification(specification("E")));
        CHECK(multiplexer.source_of(specification("E")) == std::optional<size_t>(1));
        source_b->stale = false;
        CHECK(!multiplexer.is_stale(10));

        source_a->stale = true;
        source_b->stale = true;
        CHECK(multiplexer.reconnect_stale(five, 10));
        CHECK(source_a->disconnects == 1 && source_b->disconnects == 2);
        CHECK(source_a->specification_count() + source_b->specification_count() == 5);
    }

    // 4. 링이 차면 버리고 센다 — 콜백이 막혀 있는 동안 링 용량보다 많이 쏜다.
    {
        auto fake_source  = std::make_unique<FakeSource>(40);
        auto* source_a = fake_source.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source));
        feed::FeedMux     multiplexer(std::move(sources), 16);
        std::atomic<bool> gate{true};
        std::atomic<int>  received{0};
        multiplexer.set_callbacks([](const OrderBook&) {},
                          [&](const TradeData&)
                          {
                              while (gate.load())
                              {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                              }

                              ++received;
                          });
        CHECK(multiplexer.connect({specification("A")}));

        for (int index = 0; index < 100; ++index)
        {
            source_a->emit_trade("A", index);
        }

        gate = false;

        for (int index = 0; index < 500 && received.load() + static_cast<int>(multiplexer.dropped()) < 100; ++index)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        CHECK(multiplexer.dropped() > 0 && received.load() + static_cast<int>(multiplexer.dropped()) == 100);
        CHECK(multiplexer.high_water(0) == 16);
    }

    // 5. 소스가 없으면 connect는 false.
    {
        feed::FeedMux multiplexer({}, 64);
        CHECK(!multiplexer.connect({specification("A")}) && multiplexer.source_count() == 0);
    }

    // 6. 직접 호출 모드: 소스 i가 쏜 이벤트는 그 스레드에서 번호 i를 달고 바로 온다 — multiplexer 스레드도 링도 안 거친다.
    //    체결통보는 첫 소스만, 역시 그 스레드에서. 소스 하나짜리 기본 구현은 lanes()=1, 수신 스레드 0.
    {
        auto  fake_source_a  = std::make_unique<FakeSource>(40);
        auto  fake_source_b  = std::make_unique<FakeSource>(40);
        auto* source_a = fake_source_a.get();
        auto* source_b = fake_source_b.get();
        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);
        CHECK(multiplexer.lanes() == 2);

        std::mutex                        mutex;
        std::vector<std::pair<uint32_t, std::thread::id>> seen; // (수신 스레드 번호, 부른 스레드)
        std::vector<std::string>          tickers;
        std::atomic<int>                  fills{0};
        std::thread::id                   fill_thread;
        multiplexer.set_lane_callbacks(
            [&](uint32_t lane, const OrderBook& order_book)
            {
                std::lock_guard<std::mutex> lock(mutex);
                seen.emplace_back(lane, std::this_thread::get_id());
                tickers.push_back(order_book.ticker.string());
            },
            [&](uint32_t lane, const TradeData& trade)
            {
                std::lock_guard<std::mutex> lock(mutex);
                seen.emplace_back(lane, std::this_thread::get_id());
                tickers.push_back(trade.ticker.string());
            });
        multiplexer.set_fill_callback(
            [&](const FillNotification&)
            {
                fill_thread = std::this_thread::get_id();
                fills.fetch_add(1);
            });
        CHECK(multiplexer.connect({specification("A"), specification("B")}));

        std::thread thread_a([source_a]
        {
            for (int index = 0; index < 300; ++index)
            {
                source_a->emit_trade("A", 1 + index);
            }

            source_a->emit_book("A");
        });
        std::thread thread_b([source_b]
        {
            for (int index = 0; index < 300; ++index)
            {
                source_b->emit_trade("B", 1 + index);
            }
        });
        const auto id_a = thread_a.get_id();
        const auto id_b = thread_b.get_id();
        thread_a.join();
        thread_b.join();

        // join 뒤라 더 올 게 없다 — 링이 없으니 기다릴 것도 없다.
        bool lanes_ok = seen.size() == 601;

        for (size_t seen_index = 0; seen_index < seen.size() && lanes_ok; ++seen_index)
        {
            const bool from_a = seen[seen_index].second == id_a;
            lanes_ok          = (from_a || seen[seen_index].second == id_b) && seen[seen_index].first == (from_a ? 0u : 1u) &&
                       tickers[seen_index] == (from_a ? "A" : "B");
        }

        CHECK(lanes_ok);
        CHECK(multiplexer.dropped() == 0 && multiplexer.high_water(0) == 0 && multiplexer.high_water(1) == 0);

        bool        fill_sent = false;
        std::thread tf([source_a, &fill_sent]
        {
            fill_sent = source_a->emit_fill("F1");
        });
        const auto id_f = tf.get_id();
        tf.join();
        CHECK(fill_sent && fills.load() == 1 && fill_thread == id_f);
        CHECK(!source_b->emit_fill("F2") && fills.load() == 1);

        FakeSource one(4);
        uint32_t   received_lane = 9;
        CHECK(one.lanes() == 1);
        one.set_lane_callbacks([&](uint32_t lane, const OrderBook&)
        {
            received_lane = lane;
        },
                               [&](uint32_t lane, const TradeData&)
                               {
                                   received_lane = lane;
                               });
        one.emit_trade("Z", 1.0);
        CHECK(received_lane == 0);
    }

    // 7. 체결통보는 자리(0번)가 아니라 맡은 소스로 간다 — 맡는 자리를 옮겨도 한 소스만 받는다(원장 이중 계상 방지).
    {
        auto fake_source_a         = std::make_unique<FakeSource>(4);
        auto fake_source_b         = std::make_unique<FakeSource>(4);
        fake_source_a->fill_notice = false; // 0번은 맡지 않는다
        auto* source_a             = fake_source_a.get();
        auto* source_b             = fake_source_b.get();

        std::vector<std::unique_ptr<feed::IFeedSource>> sources;
        sources.push_back(std::move(fake_source_a));
        sources.push_back(std::move(fake_source_b));
        feed::FeedMux multiplexer(std::move(sources), 64);

        CHECK(multiplexer.fill_notice_sources() == std::vector<size_t>{1});
        CHECK(multiplexer.owns_fill_notice());

        std::atomic<int> fills{0};
        multiplexer.set_lane_callbacks([](uint32_t, const OrderBook&) {}, [](uint32_t, const TradeData&) {});
        multiplexer.set_fill_callback([&](const FillNotification&)
        {
            fills.fetch_add(1);
        });

        CHECK(!source_a->emit_fill("N1")); // 맡지 않은 소스에는 콜백이 걸리지 않는다
        CHECK(source_b->emit_fill("Y1") && fills.load() == 1);

        // 아무도 맡지 않으면 아무 데도 걸지 않는다.
        auto fake_source_c         = std::make_unique<FakeSource>(4);
        fake_source_c->fill_notice = false;
        auto* source_c             = fake_source_c.get();

        std::vector<std::unique_ptr<feed::IFeedSource>> none;
        none.push_back(std::move(fake_source_c));
        feed::FeedMux no_owner(std::move(none), 64);
        no_owner.set_lane_callbacks([](uint32_t, const OrderBook&) {}, [](uint32_t, const TradeData&) {});
        no_owner.set_fill_callback([&](const FillNotification&)
        {
            fills.fetch_add(1);
        });
        CHECK(no_owner.fill_notice_sources().empty() && !no_owner.owns_fill_notice());
        CHECK(!source_c->emit_fill("N2") && fills.load() == 1);
    }

    std::cout << "test_feed_mux: " << g_checks << " checks passed\n";
    return 0;
}
