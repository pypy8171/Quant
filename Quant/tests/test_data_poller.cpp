// REST 현재가 폴러(core/DataPoller.h) 단위 테스트. 브로커·큐·WS 재구독을 std::function으로 대신해 KIS 없이
//  유니버스 폴링(KR만·실패 건너뜀·종료 플래그), 넘침 목록의 중복·재구독 복귀·REST 대체와 1회 로그, 틱 끊긴
//  보유 선택, KST 시각 변환, 폴러 자기 스레드의 바퀴·폴백 전환·멈춤을 고정한다. Logger만 링크한다. 관련 결정: D-053(대체 틱 큐), D-062(분리).
// 빌드: cmake --build <directory> --target test_data_poller
#include "core/DataPoller.h"
#include "core/KstTime.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
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

// 2027-01-15 08:00:00 UTC = KST 17:00:00.
constexpr std::time_t kT0 = 1800000000;

WatchSpec specification(const char* ticker, Market market = Market::KR)
{
    WatchSpec specification;
    specification.ticker = ticker;
    specification.market = market;
    return specification;
}

int test_kst()
{
    CHECK(kst::date_yyyymmdd(kT0) == "20270115");
    CHECK(kst::hhmmss(kT0) == "170000");
    CHECK(kst::hhmmss(kT0 + 7 * 3600 + 61) == "000101"); // UTC 15:01:01 = KST 다음날 00:01:01
    CHECK(kst::date_yyyymmdd(kT0 + 7 * 3600 + 61) == "20270116");
    return 0;
}

int test_pure()
{
    CHECK(poller::same_specification(specification("A"), specification("A")));
    CHECK(!poller::same_specification(specification("A"), specification("A", Market::US)));
    CHECK(!poller::same_specification(specification("A"), specification("C")));

    const auto timestamp = std::chrono::system_clock::now();
    const auto trade = poller::make_tick("005930", 71000.0, 93001, timestamp);
    CHECK(trade.ticker == "005930" && trade.price == 71000.0 && trade.hhmmss == 93001 && trade.quantity == 0 &&
          trade.direction == 0 && trade.market == Market::KR && trade.timestamp == timestamp && trade.strength == 0.0);

    // 틱 없음·오래됨은 고르고, 신선한 것은 남긴다.
    const auto now    = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::seconds(60);
    std::map<std::string, std::chrono::steady_clock::time_point> seen{{"FRESH", now - std::chrono::seconds(5)},
                                                                     {"OLD", now - std::chrono::seconds(120)}};
    const auto stale = poller::select_stale({"FRESH", "OLD", "NONE"},
                                            [&](const std::string& ticker) -> std::optional<std::chrono::steady_clock::time_point>
                                            {
                                                auto iterator = seen.find(ticker);

                                                if (iterator == seen.end())
                                                {
                                                    return std::nullopt;
                                                }

                                                return iterator->second;
                                            },
                                            cutoff);
    CHECK(stale.size() == 2 && stale[0] == "OLD" && stale[1] == "NONE");
    return 0;
}

int test_universe()
{
    std::vector<std::string> asked;
    std::vector<TradeData>   out;
    std::map<std::string, double> price{{"A", 100.0}, {"B", 0.0}, {"C", 300.0}};
    DataPoller data_poller(
        [&](const std::string& ticker)
        {
            asked.push_back(ticker);
            return price.count(ticker) ? price[ticker] : 0.0;
        },
        [&](const TradeData& trade) { out.push_back(trade); });
    data_poller.set_universe_call_interval(std::chrono::milliseconds(0));

    // US spec은 건너뛰고, 현재가 0은 틱을 안 흘린다. 시각은 KST HHMMSS.
    const int count = data_poller.poll_universe({specification("A"), specification("US1", Market::US), specification("B"), specification("C")}, kT0);
    CHECK(count == 2 && asked.size() == 3 && out.size() == 2);
    CHECK(out[0].ticker == "A" && out[0].price == 100.0 && out[0].hhmmss == 170000);
    CHECK(out[1].ticker == "C" && out[1].price == 300.0);

    // 종료 플래그가 내려가면 첫 종목 전에 끊는다.
    asked.clear();
    out.clear();
    data_poller.set_keep_going([] { return false; });
    CHECK(data_poller.poll_universe({specification("A")}, kT0) == 0 && asked.empty());
    return 0;
}

int test_overflow()
{
    std::vector<std::string> asked;
    std::vector<TradeData>   out;
    double                   price_b = 0.0;
    DataPoller data_poller(
        [&](const std::string& ticker)
        {
            asked.push_back(ticker);
            return ticker == "A" || ticker == "C" ? 100.0 : price_b;
        },
        [&](const TradeData& trade) { out.push_back(trade); });
    data_poller.set_universe_call_interval(std::chrono::milliseconds(0));

    // 등록: 같은 채널은 한 번만, 다른 종목은 다른 채널.
    CHECK(data_poller.add_overflow(specification("A")) && !data_poller.add_overflow(specification("A")) && data_poller.add_overflow(specification("C")));
    CHECK(data_poller.overflow_count() == 2);

    // 빼기: 있던 것만 빠지고, 없는 것은 거짓. 다시 넣어 아래 흐름을 그대로 둔다.
    CHECK(data_poller.remove_overflow(specification("A")) && !data_poller.remove_overflow(specification("A")));
    CHECK(data_poller.overflow_count() == 1 && data_poller.add_overflow(specification("A")));

    // WS에서 온 넘침도 합친다(중복은 무시). 재구독 전부 실패 → KR 현물만 REST, B는 0이라 틱 없음.
    int resub_calls = 0;
    const auto never = [&](const WatchSpec&)
    {
        ++resub_calls;
        return false;
    };
    int count = data_poller.poll_overflow({specification("A"), specification("B")}, never, kT0); // A·C는 100, B는 0
    CHECK(data_poller.overflow_count() == 3 && resub_calls == 3);
    CHECK(count == 2 && out.size() == 2 && out[0].ticker == "C" && out[0].hhmmss == 170000 && out[1].ticker == "A");
    CHECK(asked.size() == 3); // C·A·B

    // A만 재구독 성공 → 목록에서 빠지고 REST도 안 물어본다. B가 살아나면 틱이 나온다.
    asked.clear();
    out.clear();
    price_b = 50.0;
    count = data_poller.poll_overflow({}, [](const WatchSpec& specification) { return specification.ticker == "A"; }, kT0);
    CHECK(data_poller.overflow_count() == 2 && count == 2);
    CHECK(out.size() == 2 && out[0].ticker == "C" && out[0].price == 100.0 && out[1].ticker == "B" &&
          out[1].price == 50.0);
    CHECK(asked.size() == 2 && asked[0] == "C" && asked[1] == "B");

    // 목록이 비면 아무것도 안 한다.
    DataPoller empty([](const std::string&) { return 1.0; }, [](const TradeData&) {});
    CHECK(empty.poll_overflow({}, never, kT0) == 0);
    return 0;
}

int test_top_up()
{
    std::vector<std::pair<std::string, double>> received;
    DataPoller data_poller([](const std::string& ticker) { return ticker == "A" ? 100.0 : 0.0; }, [](const TradeData&) {});
    data_poller.set_top_up_call_interval(std::chrono::milliseconds(0));

    // 실패(0)도 그대로 넘긴다 — 0을 버릴지는 받는 쪽(set_last_price)이 정한다. 틱은 흘리지 않는다.
    const int count = data_poller.top_up({"A", "B"}, [&](const std::string& ticker, double price) { received.emplace_back(ticker, price); });
    CHECK(count == 2 && received.size() == 2 && received[0].first == "A" && received[0].second == 100.0 && received[1].second == 0.0);

    data_poller.set_keep_going([] { return false; });
    received.clear();
    CHECK(data_poller.top_up({"A"}, [&](const std::string& ticker, double price) { received.emplace_back(ticker, price); }) == 0 && received.empty());
    return 0;
}

// 폴러 자기 스레드 — 넘침 목록을 매 바퀴 조회하고, REST 폴백으로 바뀌면 유니버스를 돈다. 멈춤 요청은
//  바퀴 사이 대기를 바로 깨운다(바퀴 주기가 길어도 종료가 늦지 않다). [why D-138]
bool wait_until(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return true;
}

int test_loop()
{
    std::mutex               asked_mutex;
    std::vector<std::string> asked;
    std::atomic<int>         sunk{0};
    std::atomic<int>         counted{0};
    std::atomic<bool>        rest_mode{false};
    std::atomic<int>         websocket_calls{0};
    DataPoller data_poller(
        [&](const std::string& ticker)
        {
            std::lock_guard<std::mutex> lock(asked_mutex);
            asked.push_back(ticker);
            return 100.0;
        },
        [&](const TradeData&) { ++sunk; });
    data_poller.set_universe_call_interval(std::chrono::milliseconds(0));

    DataPoller::LoopSources sources;
    sources.rest_mode      = [&] { return rest_mode.load(); };
    sources.universe       = [] { return std::vector<WatchSpec>{specification("U")}; };
    sources.from_websocket = [&]
    {
        // 첫 바퀴에만 넘침 종목을 준다 — 그 뒤로는 폴러 목록에 남아 매 바퀴 조회돼야 한다.
        return websocket_calls++ == 0 ? std::vector<WatchSpec>{specification("A")} : std::vector<WatchSpec>{};
    };
    sources.on_ticks = [&](int ticks) { counted += ticks; };
    data_poller.start(std::move(sources), std::chrono::milliseconds(5));

    CHECK(wait_until([&] { return counted.load() >= 3; })); // 세 바퀴 이상 A를 조회했다
    CHECK(sunk.load() >= 3 && data_poller.overflow_count() == 1);

    {
        std::lock_guard<std::mutex> lock(asked_mutex);
        asked.clear();
    }

    rest_mode = true;
    CHECK(wait_until([&]
    {
        std::lock_guard<std::mutex> lock(asked_mutex);
        return std::find(asked.begin(), asked.end(), "U") != asked.end();
    }));

    data_poller.request_stop();
    data_poller.join();

    // 바퀴 주기가 1분이어도 멈춤 요청이 대기를 깨워 곧바로 회수된다.
    DataPoller slow([](const std::string&) { return 1.0; }, [](const TradeData&) {});
    DataPoller::LoopSources idle;
    std::atomic<int> rounds{0};
    idle.from_websocket = [&]
    {
        ++rounds;
        return std::vector<WatchSpec>{};
    };
    slow.start(std::move(idle), std::chrono::minutes(1));
    CHECK(wait_until([&] { return rounds.load() >= 1; }));
    const auto stop_begin = std::chrono::steady_clock::now();
    slow.request_stop();
    slow.join();
    CHECK(std::chrono::steady_clock::now() - stop_begin < std::chrono::seconds(2) && rounds.load() == 1);
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    if (test_kst() || test_pure() || test_universe() || test_overflow() || test_top_up() || test_loop())
    {
        return 1;
    }

    std::cout << "test_data_poller: " << g_checks << " checks passed\n";
    return 0;
}
