// REST 현재가 폴러(core/DataPoller.h) 단위 테스트. 브로커·큐·WS 재구독을 std::function으로 대신해 KIS 없이
//  유니버스 폴링(KR만·실패 건너뜀·종료 플래그), 넘침 목록의 중복·재구독 복귀·REST 대체와 1회 로그, 틱 끊긴
//  보유 선택, KST 시각 변환을 고정한다. Logger만 링크한다. 관련 결정: D-053(대체 틱 큐), D-062(분리).
// 빌드: cmake --build <dir> --target test_data_poller
#include "core/DataPoller.h"
#include "core/KstTime.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <iostream>
#include <map>
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

// 2027-01-15 08:00:00 UTC = KST 17:00:00.
constexpr std::time_t kT0 = 1800000000;

WatchSpec spec(const char* t, Market m = Market::KR, bool fut = false)
{
    WatchSpec s;
    s.ticker    = t;
    s.market    = m;
    s.is_future = fut;
    return s;
}

int test_kst()
{
    CHECK(kst::ymd(kT0) == "20270115");
    CHECK(kst::hhmmss(kT0) == "170000");
    CHECK(kst::hhmmss(kT0 + 7 * 3600 + 61) == "000101"); // UTC 15:01:01 = KST 다음날 00:01:01
    CHECK(kst::ymd(kT0 + 7 * 3600 + 61) == "20270116");
    return 0;
}

int test_pure()
{
    CHECK(poller::same_spec(spec("A"), spec("A")));
    CHECK(!poller::same_spec(spec("A"), spec("A", Market::US)));
    CHECK(!poller::same_spec(spec("A"), spec("A", Market::KR, true)));

    const auto ts = std::chrono::system_clock::now();
    const auto td = poller::make_tick("005930", 71000.0, "093001", ts);
    CHECK(td.ticker == "005930" && td.price == 71000.0 && td.time == "093001" && td.quantity == 0 &&
          td.direction == 0 && td.market == Market::KR && td.timestamp == ts && td.strength == 0.0);

    // 틱 없음·오래됨은 고르고, 신선한 것은 남긴다.
    const auto now    = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::seconds(60);
    std::map<std::string, std::chrono::steady_clock::time_point> seen{{"FRESH", now - std::chrono::seconds(5)},
                                                                     {"OLD", now - std::chrono::seconds(120)}};
    const auto stale = poller::select_stale({"FRESH", "OLD", "NONE"},
                                            [&](const std::string& t) -> std::optional<std::chrono::steady_clock::time_point>
                                            {
                                                auto it = seen.find(t);

                                                if (it == seen.end())
                                                {
                                                    return std::nullopt;
                                                }

                                                return it->second;
                                            },
                                            cutoff);
    CHECK(stale.size() == 2 && stale[0] == "OLD" && stale[1] == "NONE");
    return 0;
}

int test_universe()
{
    std::vector<std::string> asked;
    std::vector<TradeData>   out;
    std::map<std::string, double> px{{"A", 100.0}, {"B", 0.0}, {"C", 300.0}};
    DataPoller p(
        [&](const std::string& t)
        {
            asked.push_back(t);
            return px.count(t) ? px[t] : 0.0;
        },
        [&](const TradeData& td) { out.push_back(td); });
    p.set_universe_pacing(std::chrono::milliseconds(0));

    // US spec은 건너뛰고, 현재가 0은 틱을 안 흘린다. 시각은 KST HHMMSS.
    const int n = p.poll_universe({spec("A"), spec("US1", Market::US), spec("B"), spec("C")}, kT0);
    CHECK(n == 2 && asked.size() == 3 && out.size() == 2);
    CHECK(out[0].ticker == "A" && out[0].price == 100.0 && out[0].time == "170000");
    CHECK(out[1].ticker == "C" && out[1].price == 300.0);

    // 종료 플래그가 내려가면 첫 종목 전에 끊는다.
    asked.clear();
    out.clear();
    p.set_keep_going([] { return false; });
    CHECK(p.poll_universe({spec("A")}, kT0) == 0 && asked.empty());
    return 0;
}

int test_overflow()
{
    std::vector<std::string> asked;
    std::vector<TradeData>   out;
    double                   px_b = 0.0;
    DataPoller p(
        [&](const std::string& t)
        {
            asked.push_back(t);
            return t == "A" ? 100.0 : px_b;
        },
        [&](const TradeData& td) { out.push_back(td); });
    p.set_universe_pacing(std::chrono::milliseconds(0));

    // 등록: 같은 채널은 한 번만, 선물은 다른 채널.
    CHECK(p.add_overflow(spec("A")) && !p.add_overflow(spec("A")) && p.add_overflow(spec("A", Market::KR, true)));
    CHECK(p.overflow_count() == 2);

    // WS에서 온 넘침도 합친다(중복은 무시). 재구독 전부 실패 → KR 현물만 REST, B는 0이라 틱 없음.
    int resub_calls = 0;
    const auto never = [&](const WatchSpec&)
    {
        ++resub_calls;
        return false;
    };
    int n = p.poll_overflow({spec("A"), spec("B")}, never, kT0); // A·A(선물)은 100, B는 0
    CHECK(p.overflow_count() == 3 && resub_calls == 3);
    CHECK(n == 2 && out.size() == 2 && out[0].ticker == "A" && out[0].time == "170000" && out[1].ticker == "A");
    CHECK(asked.size() == 3); // A·A(선물)·B — 선물도 market은 KR이라 REST를 물어본다(종전과 같다)

    // A만 재구독 성공 → 목록에서 빠지고 REST도 안 물어본다. B가 살아나면 틱이 나온다.
    asked.clear();
    out.clear();
    px_b = 50.0;
    n = p.poll_overflow({}, [](const WatchSpec& s) { return s.ticker == "A" && !s.is_future; }, kT0);
    CHECK(p.overflow_count() == 2 && n == 2);
    CHECK(out.size() == 2 && out[0].ticker == "A" && out[0].price == 100.0 && out[1].ticker == "B" &&
          out[1].price == 50.0);
    CHECK(asked.size() == 2 && asked[0] == "A" && asked[1] == "B");

    // 목록이 비면 아무것도 안 한다.
    DataPoller empty([](const std::string&) { return 1.0; }, [](const TradeData&) {});
    CHECK(empty.poll_overflow({}, never, kT0) == 0);
    return 0;
}

int test_top_up()
{
    std::vector<std::pair<std::string, double>> got;
    DataPoller p([](const std::string& t) { return t == "A" ? 100.0 : 0.0; }, [](const TradeData&) {});
    p.set_top_up_pacing(std::chrono::milliseconds(0));

    // 실패(0)도 그대로 넘긴다 — 0을 버릴지는 받는 쪽(set_last_px)이 정한다. 틱은 흘리지 않는다.
    const int n = p.top_up({"A", "B"}, [&](const std::string& t, double px) { got.emplace_back(t, px); });
    CHECK(n == 2 && got.size() == 2 && got[0].first == "A" && got[0].second == 100.0 && got[1].second == 0.0);

    p.set_keep_going([] { return false; });
    got.clear();
    CHECK(p.top_up({"A"}, [&](const std::string& t, double px) { got.emplace_back(t, px); }) == 0 && got.empty());
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* env = std::getenv("QUANT_LOG_DIR"); !env || !*env)
    {
        Logger::instance().set_base_dir(Logger::executable_dir() / "logs_test");
    }

    if (test_kst() || test_pure() || test_universe() || test_overflow() || test_top_up())
    {
        return 1;
    }

    std::cout << "test_data_poller: " << g_checks << " checks passed\n";
    return 0;
}
