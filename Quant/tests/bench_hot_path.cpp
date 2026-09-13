// tests/bench_hot_path.cpp
// 09-13 hot path 변경(D-071 Phase 2·3)의 항목별 전후 비교 — 옛 방식을 벤치 안에 최소 복제해 같은 입력으로 잰다.
//  결과는 `docs/reports/PIPELINE_LATENCY_REPORT.md` 결과 ⑥, 절차는 `docs/guides/LOAD_TEST_GUIDE.md` §6.
//  스레드: 항목 1~4는 단일 스레드, 5(캡처)·6(mux)·7(연쇄)은 생산자 1·소비자 1. release 빌드로만 잰다. [why D-071]
//
// [inv] 측정 범위 — 실 KIS WS 프레임 수신·복호화·네트워크는 빠져 있다. 재는 것은 디코더 입구(`^` 페이로드)부터
//   전략 on_trade 반환까지의 in-process 비용이다. 옛 방식 복제는 지운 코드의 핵심 연산(문자열 키 맵·mutex·
//   substr+stoi·전 전략 방문)만 옮긴 것이라 절대값은 참고, 비교는 같은 입력에서의 상대값으로 읽는다.

#include "api/KisWsDecode.h"
#include "core/FeedMux.h"
#include "core/MarketSession.h"
#include "core/RingBuffer.h"
#include "core/StrategyRouter.h"
#include "core/SymbolTable.h"
#include "core/TickCapture.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

volatile int64_t g_sink = 0;

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// t(steady ns)까지 바쁘게 기다린다 — 생산자 투입률 맞춤용. sleep은 2ms 격자라 못 쓴다.
void spin_until(int64_t t)
{
    while (now_ns() < t)
    {
    }
}

// f를 iters번 돌린 평균 ns/op. 첫 1/16은 예열로 버린다.
template <class F>
double ns_per_op(size_t iters, F&& f)
{
    const size_t warm = iters / 16;

    for (size_t i = 0; i < warm; ++i)
    {
        f(i);
    }

    const int64_t t0 = now_ns();

    for (size_t i = 0; i < iters; ++i)
    {
        f(i);
    }

    return static_cast<double>(now_ns() - t0) / static_cast<double>(iters);
}

struct Pct
{
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

Pct percentiles(std::vector<double>& v)
{
    Pct p;

    if (v.empty())
    {
        return p;
    }

    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
    p.p50  = at(0.50);
    p.p99  = at(0.99);
    p.p999 = at(0.999);
    p.max  = v.back();
    return p;
}

void row(const char* name, double ns_old, double ns_new)
{
    const double ratio = ns_new > 0 ? ns_old / ns_new : 0.0;
    std::printf("  %-44s %9.1f  %9.1f  %6.2fx\n", name, ns_old, ns_new, ratio);
}

void row1(const char* name, double ns)
{
    std::printf("  %-44s %9.1f\n", name, ns);
}

void head(const char* title)
{
    std::printf("\n== %s\n", title);
}

// 종목 코드 2,600개 — 여섯 자리 숫자열. 실 코스피·코스닥 규모.
constexpr size_t kSymbols = 2600;

std::vector<std::string> make_tickers()
{
    std::vector<std::string> v;
    v.reserve(kSymbols);

    for (size_t i = 0; i < kSymbols; ++i)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%06zu", 5930 + i * 7);
        v.emplace_back(buf);
    }

    return v;
}

// 종목 접근 순서 — 선형이면 캐시가 다 먹으므로 LCG로 섞는다.
size_t next_idx(size_t& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (s >> 33) % kSymbols;
}

// [wire] H0STCNT0 페이로드 — 46필드를 '^'로 잇는다. 디코더가 읽는 자리(0·1·2·12·13·18·21)만 실값, 나머지는 0.
std::string make_kr_trade_payload(const std::string& ticker, int px)
{
    std::string s;

    for (int i = 0; i < 46; ++i)
    {
        if (i > 0)
        {
            s += '^';
        }

        switch (i)
        {
            case 0: s += ticker; break;
            case 1: s += "093001"; break;
            case 2: s += std::to_string(px); break;
            case 12: s += "150"; break;
            case 13: s += "1234567"; break;
            case 18: s += "120.55"; break;
            case 21: s += "1"; break;
            default: s += '0'; break;
        }
    }

    return s;
}

// ─── 1. 디코더 — 시각 문자열 → 정수 hhmmss (9d838e0) ─────────────────────────────
// 옛: td.time = f[1]로 문자열을 싣고 소비자마다 다시 파싱(IntradayBreakout은 stoi(substr), 나머지는 parse_hhmm).
// 새: 디코더가 parse_hhmmss 한 번, 소비자는 정수 나눗셈.
void bench_decode()
{
    head("1. 디코더 시각 — 문자열 실어 소비자 N회 파싱 vs 정수 한 번");
    const std::string payload = make_kr_trade_payload("005930", 71200);
    std::vector<std::string_view> f;
    f.reserve(64);
    constexpr size_t kIters     = 2'000'000;
    constexpr int    kConsumers = 4; // 봉 집계기 세션 판정 + 전략 셋이 같은 틱을 본다

    const double split_ns = ns_per_op(kIters, [&](size_t)
    {
        kis_ws::split_fields(payload, '^', f);
        g_sink += static_cast<int64_t>(f.size());
    });

    kis_ws::split_fields(payload, '^', f);
    TradeData td;

    const double decode_ns = ns_per_op(kIters, [&](size_t)
    {
        (void)kis_ws::decode_kr_trade(f, td);
        g_sink += td.hhmmss;
    });

    const double old_ns = ns_per_op(kIters, [&](size_t)
    {
        std::string time(f[1]); // 옛 td.time
        int v = std::stoi(time.substr(0, 6)); // 옛 IntradayBreakoutStrategy::parse_hhmmss

        for (int c = 1; c < kConsumers; ++c)
        {
            v += krx::parse_hhmm(time);
        }

        g_sink += v;
    });

    const double new_ns = ns_per_op(kIters, [&](size_t)
    {
        const int32_t h = krx::parse_hhmmss(f[1]);
        int           v = h;

        for (int c = 1; c < kConsumers; ++c)
        {
            v += h / 100;
        }

        g_sink += v;
    });

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    row1("(참고) split_fields 46필드", split_ns);
    row1("(참고) decode_kr_trade 전체(새 방식 포함)", decode_ns);
    row("시각: 문자열+소비자 4회 파싱 vs 정수 1회", old_ns, new_ns);
}

// ─── 2. 현재가 캐시 — mutex+map<string> vs id 배열 (f9026d5) ──────────────────────
struct OldLastPx
{
    double                   px = 0;
    Clock::time_point        at;
};

void bench_last_px(const std::vector<std::string>& tickers)
{
    head("2. 현재가 캐시 — mutex + unordered_map<string> vs atomic 배열[id]");
    constexpr size_t kIters = 2'000'000;

    std::unordered_map<std::string, OldLastPx> old_map;
    std::mutex                                 old_mu;

    for (const auto& t : tickers)
    {
        old_map[t] = {};
    }

    sym::SymbolTable tab(kSymbols + 1);
    std::vector<sym::SymbolId> ids;

    for (const auto& t : tickers)
    {
        ids.push_back(tab.intern(t));
    }

    auto arr_px = std::make_unique<std::atomic<double>[]>(kSymbols + 1);
    auto arr_at = std::make_unique<std::atomic<int64_t>[]>(kSymbols + 1);

    size_t s = 1;
    const double old_set = ns_per_op(kIters, [&](size_t)
    {
        const auto& t = tickers[next_idx(s)];
        std::lock_guard<std::mutex> lk(old_mu);
        old_map[t] = OldLastPx{71200.0, Clock::now()};
    });

    s = 1;
    const double new_set = ns_per_op(kIters, [&](size_t)
    {
        const sym::SymbolId id = ids[next_idx(s)];
        arr_px[id].store(71200.0, std::memory_order_relaxed);
        arr_at[id].store(now_ns(), std::memory_order_relaxed);
    });

    s = 1;
    const double old_get = ns_per_op(kIters, [&](size_t)
    {
        const auto& t = tickers[next_idx(s)];
        std::lock_guard<std::mutex> lk(old_mu);
        auto it = old_map.find(t);
        g_sink += static_cast<int64_t>(it == old_map.end() ? 0.0 : it->second.px);
    });

    s = 1;
    const double new_get = ns_per_op(kIters, [&](size_t)
    {
        const sym::SymbolId id = ids[next_idx(s)];
        g_sink += static_cast<int64_t>(arr_px[id].load(std::memory_order_relaxed));
    });

    // 수신 콜백이 아직 틱마다 부르는 문자열 → id 조회. 디코더가 id를 직접 찍는 후속이 없앨 비용.
    s = 1;
    const double intern_ns = ns_per_op(kIters, [&](size_t)
    {
        g_sink += tab.intern(tickers[next_idx(s)]);
    });

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    row("set_last_px(틱마다)", old_set, new_set);
    row("last_px(운영단말·발주 기준가)", old_get, new_get);
    row1("(잔여) SymbolTable::intern(문자열) 틱마다", intern_ns);
}

// ─── 3. 라우터 — 전 전략 방문 vs 종목 id 디스패치 (780597a) ───────────────────────
// 구독 종목 하나를 밝히는 가짜 전략. on_trade는 오늘 전략들처럼 sym이 다르면 바로 돌아간다.
class OneSymStrategy final : public StrategyBase
{
public:
    explicit OneSymStrategy(std::string ticker) : ticker_(std::move(ticker)) {}

    void bind(sym::SymbolId id)
    {
        sym_ = id;
    }

    std::string id() const override
    {
        return "bench_" + ticker_;
    }

    std::string describe() const override
    {
        return "bench";
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.sym != sym_)
        {
            return std::nullopt;
        }

        ++hits;
        return std::nullopt;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        WatchSpec w;
        w.ticker = ticker_;
        return {w};
    }

    int64_t hits = 0;

private:
    std::string   ticker_;
    sym::SymbolId sym_ = sym::kNone;
};

void bench_router_n(const std::vector<std::string>& tickers, size_t n_strats)
{
    constexpr size_t kIters = 1'000'000;
    sym::SymbolTable tab(kSymbols + 1);
    std::vector<std::unique_ptr<OneSymStrategy>> owned;
    std::vector<StrategyBase*>                   ptrs;

    for (size_t i = 0; i < n_strats; ++i)
    {
        owned.push_back(std::make_unique<OneSymStrategy>(tickers[i]));
        owned.back()->bind(tab.intern(tickers[i]));
        ptrs.push_back(owned.back().get());
    }

    strat::Router r;
    r.rebuild(ptrs, [&tab](const std::string& t) { return tab.intern(t); });

    std::vector<TradeData> ticks(n_strats);

    for (size_t i = 0; i < n_strats; ++i)
    {
        ticks[i].ticker = tickers[i];
        ticks[i].sym    = tab.intern(tickers[i]);
        ticks[i].price  = 1000.0 + static_cast<double>(i);
    }

    size_t s = 7;
    const double old_ns = ns_per_op(kIters, [&](size_t)
    {
        const auto& td = ticks[next_idx(s) % n_strats];

        for (auto* p : ptrs)
        {
            (void)p->on_trade(td);
        }
    });

    s = 7;
    const double new_ns = ns_per_op(kIters, [&](size_t)
    {
        const auto& td = ticks[next_idx(s) % n_strats];
        r.for_each(td.sym, [&td](StrategyBase* p) { (void)p->on_trade(td); });
    });

    char name[64];
    std::snprintf(name, sizeof(name), "전략 %zu개(종목 1개씩) 틱 1건 디스패치", n_strats);
    row(name, old_ns, new_ns);
}

void bench_router(const std::vector<std::string>& tickers)
{
    head("3. 라우터 — 전 전략 방문(sym 비교 후 반환) vs Router::for_each");
    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    bench_router_n(tickers, 4);
    bench_router_n(tickers, 40);
    bench_router_n(tickers, 400);
}

// ─── 4. 전략 상태 키 — string vs SymbolId (0039468) ──────────────────────────────
struct State
{
    double  ma = 0;
    int64_t n  = 0;
};

void bench_state_key(const std::vector<std::string>& tickers)
{
    head("4. 전략 상태 — unordered_map<string> vs unordered_map<SymbolId> vs vector[id], 2,600키");
    constexpr size_t kIters = 2'000'000;

    std::unordered_map<std::string, State>   by_str;
    std::unordered_map<sym::SymbolId, State> by_id;
    std::vector<State>                       by_idx(kSymbols + 1);
    sym::SymbolTable                         tab(kSymbols + 1);
    std::vector<sym::SymbolId>               ids;

    for (const auto& t : tickers)
    {
        by_str[t] = {};
        const auto id = tab.intern(t);
        by_id[id]     = {};
        ids.push_back(id);
    }

    size_t s = 3;
    const double str_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& st = by_str[tickers[next_idx(s)]];
        st.ma += 1.0;
        ++st.n;
        g_sink += st.n;
    });

    s = 3;
    const double id_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& st = by_id[ids[next_idx(s)]];
        st.ma += 1.0;
        ++st.n;
        g_sink += st.n;
    });

    s = 3;
    const double idx_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& st = by_idx[ids[next_idx(s)]];
        st.ma += 1.0;
        ++st.n;
        g_sink += st.n;
    });

    // 전략이 틱마다 하던 "내 종목인가" 비교 — 여섯 자리 문자열 vs 정수.
    const std::string mine = tickers[10];
    const sym::SymbolId mine_id = ids[10];
    s = 5;
    const double cmp_str = ns_per_op(kIters, [&](size_t)
    {
        g_sink += (tickers[next_idx(s)] != mine) ? 1 : 0;
    });

    s = 5;
    const double cmp_id = ns_per_op(kIters, [&](size_t)
    {
        g_sink += (ids[next_idx(s)] != mine_id) ? 1 : 0;
    });

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    row("상태 맵 조회+갱신: string 키 vs SymbolId 키", str_ns, id_ns);
    row("상태 맵 조회+갱신: string 키 vs vector[id]", str_ns, idx_ns);
    row("내 종목인가: 문자열 != vs 정수 !=", cmp_str, cmp_id);
}

// ─── 5. 틱 캡처 — 수신 콜백에 더해진 push 비용 (13559a2) ──────────────────────────
// rate_per_s가 0이면 쉬지 않고 밀어 넣는다(링·기록 스레드 상한), 아니면 그 투입률에 맞춘다(실 부하 자리).
void bench_capture_at(const std::vector<TradeData>& ticks, double rate_per_s)
{
    constexpr size_t kTicks = 400'000;
    const auto file = std::filesystem::temp_directory_path() / "bench_hot_path_capture.bin";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    const int64_t gap = rate_per_s > 0 ? static_cast<int64_t>(1e9 / rate_per_s) : 0;

    double   push_ns = 0;
    uint64_t dropped = 0;
    int64_t  total   = 0;
    {
        feed::TickCapture cap(file);

        if (!cap.ok())
        {
            std::printf("  캡처 파일을 못 열었다: %s\n", file.string().c_str());
            return;
        }

        size_t        s    = 11;
        int64_t       next = now_ns();
        const int64_t t0   = next;
        int64_t       busy = 0;

        for (size_t i = 0; i < kTicks; ++i)
        {
            if (gap > 0)
            {
                spin_until(next);
                next += gap;
            }

            const int64_t a = now_ns();
            cap.on_trade(ticks[next_idx(s)]);
            busy += now_ns() - a;
        }

        cap.flush();
        total   = now_ns() - t0;
        push_ns = static_cast<double>(busy) / static_cast<double>(kTicks);
        dropped = cap.dropped();
    }

    const auto bytes = std::filesystem::file_size(file, ec);
    std::filesystem::remove(file, ec);
    const double written = static_cast<double>(kTicks - dropped);
    std::printf("  %-12s on_trade %6.1f ns/틱 | 드롭 %6llu / %zu | 기록 %.0f 건/s (%.1f MB, flush까지 %.0f ms)\n",
                gap > 0 ? "투입률 맞춤" : "최대속도", push_ns, static_cast<unsigned long long>(dropped), kTicks,
                written / (static_cast<double>(total) / 1e9), static_cast<double>(bytes) / 1e6,
                static_cast<double>(total) / 1e6);
}

void bench_capture(const std::vector<std::string>& tickers)
{
    head("5. 틱 캡처 — 수신 콜백의 capture_->on_trade(td) 비용과 기록 스레드 상한(링 65,536)");
    std::vector<TradeData> ticks(kSymbols);

    for (size_t i = 0; i < kSymbols; ++i)
    {
        ticks[i].ticker   = tickers[i];
        ticks[i].sym      = static_cast<sym::SymbolId>(i + 1);
        ticks[i].hhmmss   = 93001;
        ticks[i].price    = 71200.0;
        ticks[i].quantity = 150;
        ticks[i].recv_ns  = now_ns();
    }

    std::printf("  전 시장 100k/s(투입률 맞춤)와 상한(최대속도) 두 줄. 드롭은 기록 스레드가 못 따라온 양.\n");
    bench_capture_at(ticks, 100'000.0);
    bench_capture_at(ticks, 0.0);
}

// ─── 6. FeedMux 홉 — 소켓 수신 스레드 → 링 → mux 스레드 → 콜백 (824408f) ──────────
// 구독·연결만 흉내 내는 소스. emit은 호출 스레드에서 콜백을 부른다(수신 스레드 자리).
struct SpinSource final : feed::IFeedSource
{
    TradeCb on_td;
    bool    connected = false;

    void set_callbacks(OrderBookCb, TradeCb td) override
    {
        on_td = std::move(td);
    }

    bool connect(const std::vector<WatchSpec>&) override
    {
        connected = true;
        return true;
    }

    void disconnect() override
    {
        connected = false;
    }

    bool subscribe_incremental(const WatchSpec&) override
    {
        return true;
    }

    bool has_spec(const WatchSpec&) const override
    {
        return true;
    }

    std::vector<WatchSpec> take_overflow_specs() override
    {
        return {};
    }

    bool is_connected() const override
    {
        return connected;
    }

    bool is_stale(int) const override
    {
        return false;
    }
};

void bench_mux(const std::vector<std::string>& tickers, size_t n_sources, double total_rate_per_s)
{
    constexpr size_t kTicks = 200'000;
    const int64_t    gap    = total_rate_per_s > 0 ? static_cast<int64_t>(1e9 * static_cast<double>(n_sources) / total_rate_per_s) : 0;
    std::vector<std::unique_ptr<feed::IFeedSource>> srcs;
    std::vector<SpinSource*>                        raw;

    for (size_t i = 0; i < n_sources; ++i)
    {
        auto s = std::make_unique<SpinSource>();
        raw.push_back(s.get());
        srcs.push_back(std::move(s));
    }

    feed::FeedMux mux(std::move(srcs));
    std::vector<double> lat;
    lat.reserve(kTicks * n_sources);
    std::atomic<size_t> got{0};

    mux.set_callbacks([](const OrderBook&) {},
                      [&](const TradeData& td)
                      {
                          lat.push_back(static_cast<double>(now_ns() - td.recv_ns));
                          got.fetch_add(1, std::memory_order_release);
                      });

    std::vector<WatchSpec> specs(n_sources);
    (void)mux.connect(specs);

    std::vector<std::thread> producers;
    const int64_t t0 = now_ns();

    for (size_t p = 0; p < n_sources; ++p)
    {
        producers.emplace_back([&, p]
        {
            TradeData td;
            td.ticker = tickers[p];
            td.sym    = static_cast<sym::SymbolId>(p + 1);
            td.price  = 1000.0;

            int64_t next = now_ns();

            for (size_t i = 0; i < kTicks; ++i)
            {
                if (gap > 0)
                {
                    spin_until(next);
                    next += gap;
                }

                td.recv_ns = now_ns();
                raw[p]->on_td(td);
            }
        });
    }

    for (auto& t : producers)
    {
        t.join();
    }

    while (got.load(std::memory_order_acquire) + mux.dropped() < kTicks * n_sources)
    {
        std::this_thread::yield();
    }

    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const Pct    p    = percentiles(lat);
    std::printf("  소스 %zu개: 홉 지연 p50 %.0f ns  p99 %.0f ns  p999 %.0f ns  max %.0f ns  |  %.0f 건/s, 드롭 %llu\n",
                n_sources, p.p50, p.p99, p.p999, p.max, static_cast<double>(lat.size()) / secs,
                static_cast<unsigned long long>(mux.dropped()));
}

void bench_feed_mux(const std::vector<std::string>& tickers)
{
    head("6. FeedMux 홉 — 소스 수신 스레드 emit → mux 스레드 콜백까지(직결이면 0). 투입률 맞춤은 합계 100k/s");
    std::printf("  생산자는 바쁘게 기다리며 코어를 하나씩 잡는다(코어 %u개) — 소스 수가 코어에 가까우면 p99는 스케줄러 몫.\n",
                std::thread::hardware_concurrency());
    bench_mux(tickers, 1, 100'000.0);
    bench_mux(tickers, 2, 100'000.0);
    bench_mux(tickers, 4, 100'000.0);
    bench_mux(tickers, 1, 0.0);
    bench_mux(tickers, 4, 0.0);
}

// ─── 7. 연쇄 — 디코드 → intern → SPSC → 전략 스레드(캐시·라우터·on_trade) ──────────
// 09-13 조각을 실제 순서로 잇는다. 생산자는 WS 수신 콜백 자리, 소비자는 전략 스레드 자리.
void bench_chain_at(const std::vector<std::string>& tickers, double rate_per_s)
{
    const int64_t gap = rate_per_s > 0 ? static_cast<int64_t>(1e9 / rate_per_s) : 0;
    const size_t kTicks  = gap > 0 ? 300'000 : 1'000'000;
    constexpr size_t kStrats = 40;

    std::vector<std::string> payloads;
    payloads.reserve(kSymbols);

    for (size_t i = 0; i < kSymbols; ++i)
    {
        payloads.push_back(make_kr_trade_payload(tickers[i], 70000 + static_cast<int>(i)));
    }

    sym::SymbolTable tab(kSymbols + 1);
    std::vector<std::unique_ptr<OneSymStrategy>> owned;
    std::vector<StrategyBase*>                   ptrs;

    for (size_t i = 0; i < kStrats; ++i)
    {
        owned.push_back(std::make_unique<OneSymStrategy>(tickers[i]));
        owned.back()->bind(tab.intern(tickers[i]));
        ptrs.push_back(owned.back().get());
    }

    strat::Router r;
    r.rebuild(ptrs, [&tab](const std::string& t) { return tab.intern(t); });
    auto arr_px = std::make_unique<std::atomic<double>[]>(kSymbols + 1);

    RingBuffer<TradeData> q(1u << 16);
    std::vector<double>   lat;
    lat.reserve(kTicks);
    std::atomic<bool> done{false};

    std::thread consumer([&]
    {
        size_t n = 0;

        while (n < kTicks)
        {
            auto opt = q.pop();

            if (!opt)
            {
                if (done.load(std::memory_order_acquire) && q.empty())
                {
                    break;
                }

                continue;
            }

            const sym::SymbolId id = opt->sym;
            arr_px[id].store(opt->price, std::memory_order_relaxed);
            r.for_each(id, [&](StrategyBase* s) { (void)s->on_trade(*opt); });
            lat.push_back(static_cast<double>(now_ns() - opt->recv_ns));
            ++n;
        }
    });

    std::vector<std::string_view> f;
    f.reserve(64);
    size_t        s    = 13;
    const int64_t t0   = now_ns();
    int64_t       next = t0;

    for (size_t i = 0; i < kTicks; ++i)
    {
        if (gap > 0)
        {
            spin_until(next);
            next += gap;
        }

        const std::string& payload = payloads[next_idx(s)];
        kis_ws::split_fields(payload, '^', f);
        TradeData td;
        (void)kis_ws::decode_kr_trade(f, td);
        td.recv_ns = now_ns();
        td.sym     = tab.intern(td.ticker);

        while (!q.push(std::move(td)))
        {
            std::this_thread::yield();
        }
    }

    done.store(true, std::memory_order_release);
    consumer.join();
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const Pct    p    = percentiles(lat);
    int64_t      hits = 0;

    for (const auto& o : owned)
    {
        hits += o->hits;
    }

    if (gap > 0)
    {
        std::printf("  투입률 맞춤 %.0f 틱/s: 틱 %zu건, 전략 적중 %lld\n", static_cast<double>(kTicks) / secs, kTicks,
                    static_cast<long long>(hits));
    }
    else
    {
        std::printf("  최대속도: 틱 %zu건 %.2f초 → %.0f 틱/s (틱당 %.0f ns), 전략 적중 %lld\n", kTicks, secs,
                    static_cast<double>(kTicks) / secs, secs * 1e9 / static_cast<double>(kTicks),
                    static_cast<long long>(hits));
    }

    std::printf("  수신 시각 → on_trade 반환: p50 %.0f ns  p99 %.0f ns  p999 %.0f ns  max %.0f ns\n", p.p50, p.p99,
                p.p999, p.max);
}

} // namespace

int main()
{
#ifdef _WIN32
    std::system("chcp 65001 > nul");
#endif
    const auto tickers = make_tickers();
    std::printf("bench_hot_path — 09-13 hot path 변경 항목별 전후 (release, 종목 %zu개)\n", kSymbols);
    bench_decode();
    bench_last_px(tickers);
    bench_router(tickers);
    bench_state_key(tickers);
    bench_capture(tickers);
    bench_feed_mux(tickers);
    head("7. 연쇄 — split+decode → intern → td_queue push → pop → set_last_px → Router → on_trade(전략 40개)");
    std::printf("  소비자는 바쁘게 기다린다(엔진 전략 스레드의 200us yield 뒤 잠들기는 빠져 있다). 코어 %u개.\n",
                std::thread::hardware_concurrency());
    bench_chain_at(tickers, 100'000.0);
    bench_chain_at(tickers, 0.0);
    std::printf("\nsink=%lld\n", static_cast<long long>(g_sink));
    return 0;
}
