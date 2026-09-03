// tests/bench_market_firehose.cpp
// 전종목 규모 시세 파이프라인 부하테스트 ("market firehose")
//
// 목적: "최악지연(tail latency)을 다룬다"는 주장을 숫자로 바운드한다.
//   KRX 상장 보통주 전종목(~2,600) 규모로 호가/체결을 팬아웃시켜, 내부 3-stage
//   처리 파이프라인의 전 구간(E2E) 지연 분포(중앙값(p50)·상위 1%(p99)·상위 0.1%(p999))와 최대 지속가능 처리량(throughput)
//   ("용량 천장")을 실측한다.
//
//   ⚠ 정직 경계 — 본 하네스는 내부 처리단만 측정한다. 실제 KIS REST/WS 네트워크 지연은
//     빠져 있다. 프로덕션 end-to-end 지연은 무료 API 폴링 주기(초 단위)가 좌우하며, 그것이
//     진짜 병목이다. 여기서 보려는 건 "처리단은 전종목 규모에서도 µs로 여유가 있다 →
//     병목은 링버퍼가 아니라 피드다"라는 점이다.
//
// 토폴로지 (Engine.cpp와 동일한 3-stage 락프리 파이프):
//   ws_producer → [ob_q, td_q] → strategy_thread → order_q → order_thread
//
// 현실성:
//   - 종목별 메시지 rate를 Zipf(멱법칙)로 배분 — 소수 대형주가 총 호가 팬아웃의
//     대부분을 차지하는 실제 시장 구조 근사(균등 분포는 비현실적).
//   - 티커는 실제 상장 전종목 코드(universe_full.json)를 로드, 없으면 합성 6자리 폴백.
//
// 측정 관례 (bench_intake / test_pipeline_stress 계승):
//   - pacing은 sleep 금지(Windows 부정확) → busy-wait.
//   - 지연 샘플은 소비자 단독 스레드에서만 수집(스레드별 독립 vector = 안전).
//   - reserve로 미리 잡아 측정 중 재할당(소비자 스톨) 차단.
//   - 종료 후 드레인 구간 항목은 분위수(percentile) 오염원 → 카운트만, 샘플 제외.
//   - release 빌드로만 유의미(debug/ASan은 무시). 빌드타입을 배너에 병기.
//
// 사용법:
//   bench_market_firehose load  [--universe path] [--tickers N] [--rate MSGS_PER_SEC]
//                               [--duration SEC] [--ob-ratio R] [--zipf S]
//   bench_market_firehose sweep [--universe path] [--tickers N]
//                               [--start R] [--step R] [--max R] [--dwell SEC] [--zipf S]
//
//   기본값: load  --tickers 2600 --rate 200000 --duration 20 --ob-ratio 0.7 --zipf 1.0
//           sweep --tickers 2600 --start 50000 --step 100000 --max 2000000 --dwell 4

#include "core/RingBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using clk = std::chrono::steady_clock;

static inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
}

static inline void busy_wait_until_ns(int64_t deadline_ns)
{
    while (now_ns() < deadline_ns)
    {
        /* pure spin — Windows scheduler 회피 */
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock 메시지 (실제 Types.h OrderBook/TradeData/OrderSignal와 동일 페이로드 크기)
//   send_ts_ns: 생산자 인테이크 시각 (E2E 기준점)
// ─────────────────────────────────────────────────────────────────────────────
struct MockOrderBook
{
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t seq;
    double   ask_price[5];
    int64_t  ask_qty[5];
    double   bid_price[5];
    int64_t  bid_qty[5];
};

struct MockTradeData
{
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t seq;
    double   price;
    int64_t  quantity;
    int      direction;
};

struct MockOrderSignal
{
    char     ticker[8];
    int64_t  send_ts_ns;      // 원본 시세 인테이크 시각 (E2E)
    int64_t  strat_ts_ns;     // strategy가 신호를 push한 시각 (strategy→order 분해용)
    uint64_t origin_seq;
    int      side;
    int      quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// 티커 로드: universe_full.json(코드 배열) → 없으면 합성 6자리
//   JSON 파서 미도입 — "codes":[ ... ] 안의 6자리 숫자열만 정규식 없이 스캔 추출.
//   (universe_full.json은 full_universe_dump.py가 생성; 스키마는 codes 배열 포함)
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> load_universe(const std::string& path, int fallback_n)
{
    std::vector<std::string> out;
    if (!path.empty())
    {
        std::ifstream f(path);
        if (f)
        {
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // "codes" 배열 이후만 스캔(ticker 이름 등 다른 숫자 오염 방지). 없으면 전체 스캔.
            size_t start = body.find("\"codes\"");
            std::string scan = (start != std::string::npos) ? body.substr(start) : body;
            // 따옴표로 감싼 연속 6자리 숫자 토큰만 코드로 취급.
            for (size_t i = 0; i + 1 < scan.size(); ++i)
            {
                if (scan[i] != '"')
                    continue;
                size_t j = i + 1, digits = 0;
                while (j < scan.size() && scan[j] >= '0' && scan[j] <= '9')
                {
                    ++j;
                    ++digits;
                }
                if (digits == 6 && j < scan.size() && scan[j] == '"')
                {
                    out.emplace_back(scan.substr(i + 1, 6));
                    i = j;
                }
            }
        }
    }
    if (out.empty())
    {
        out.reserve(fallback_n);
        for (int i = 1; i <= fallback_n; ++i)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%06d", i);
            out.emplace_back(buf);
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zipf 가중 종목 선택기 — rank r의 확률 ∝ 1/r^s.
//   누적분포를 미리 만들어 uniform → binary search로 O(log N) 샘플.
//   s=0 → 균등, s=1 → 고전적 Zipf(상위 종목이 메시지 대부분 차지).
// ─────────────────────────────────────────────────────────────────────────────
struct ZipfPicker
{
    std::vector<double> cdf;
    explicit ZipfPicker(size_t n, double s)
    {
        cdf.resize(n);
        double acc = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            acc += 1.0 / std::pow(static_cast<double>(i + 1), s);
            cdf[i] = acc;
        }
        const double total = acc;
        for (auto& c : cdf)
            c /= total;
    }
    // u ∈ [0,1) → 종목 인덱스
    size_t pick(double u) const
    {
        auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
        size_t idx = static_cast<size_t>(it - cdf.begin());
        return (idx < cdf.size()) ? idx : cdf.size() - 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 공유 통계
// ─────────────────────────────────────────────────────────────────────────────
struct Stats
{
    std::atomic<uint64_t> ob_produced{0};
    std::atomic<uint64_t> td_produced{0};
    std::atomic<uint64_t> ob_consumed{0};
    std::atomic<uint64_t> td_consumed{0};
    std::atomic<uint64_t> signals{0};
    std::atomic<uint64_t> orders{0};
    std::atomic<uint64_t> ob_drops{0};
    std::atomic<uint64_t> td_drops{0};
    std::atomic<uint64_t> order_drops{0};
    std::atomic<uint64_t> ob_hwm{0};   // 큐 high-water mark (근사)
    std::atomic<uint64_t> td_hwm{0};
    std::atomic<uint64_t> order_hwm{0};

    // 스레드별 독립 수집 (합산은 join 후)
    std::vector<int64_t> intake_to_strat_ns; // strategy 스레드가 채움
    std::vector<int64_t> strat_to_order_ns;  // order 스레드가 채움
    std::vector<int64_t> e2e_ns;             // order 스레드가 채움
};

static inline void bump_hwm(std::atomic<uint64_t>& hwm, uint64_t v)
{
    uint64_t cur = hwm.load(std::memory_order_relaxed);
    while (v > cur && !hwm.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    {
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WS 시뮬레이션 Producer — 총 offered rate를 균등 간격 busy-wait pacing으로 방출,
//   종목은 Zipf로 선택, OB/TD는 ob_ratio로 분기.
// ─────────────────────────────────────────────────────────────────────────────
static void producer_fn(RingBuffer<MockOrderBook>& ob_q,
                        RingBuffer<MockTradeData>& td_q,
                        const std::vector<std::string>& tickers,
                        const ZipfPicker& zipf,
                        Stats& st,
                        std::atomic<bool>& stop,
                        int64_t total_rate,
                        double ob_ratio,
                        int64_t duration_ns)
{
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    const int64_t interval_ns = (total_rate > 0) ? static_cast<int64_t>(1e9 / total_rate) : 0;
    const int64_t t0 = now_ns();
    const int64_t t_end = t0 + duration_ns;
    int64_t next_emit = t0;
    uint64_t seq = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        const int64_t t = now_ns();
        if (t >= t_end)
            break;
        if (interval_ns > 0)
        {
            if (t < next_emit)
                busy_wait_until_ns(next_emit);
            next_emit += interval_ns;
        }

        const size_t tk = zipf.pick(u01(rng));
        const int64_t ts = now_ns();

        if (u01(rng) < ob_ratio)
        {
            MockOrderBook ob{};
            std::memcpy(ob.ticker, tickers[tk].c_str(), 7);
            ob.send_ts_ns = ts;
            ob.seq = seq++;
            for (int i = 0; i < 5; ++i)
            {
                ob.ask_price[i] = 70000.0 + i * 10;
                ob.ask_qty[i] = 100 * (i + 1);
                ob.bid_price[i] = 69990.0 - i * 10;
                ob.bid_qty[i] = 100 * (i + 1);
            }
            if (ob_q.push(ob))
            {
                st.ob_produced.fetch_add(1, std::memory_order_relaxed);
                bump_hwm(st.ob_hwm, ob_q.size());
            }
            else
                st.ob_drops.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            MockTradeData td{};
            std::memcpy(td.ticker, tickers[tk].c_str(), 7);
            td.send_ts_ns = ts;
            td.seq = seq++;
            td.price = 70000.0 + (seq % 100);
            td.quantity = 10 + (seq % 50);
            td.direction = (seq % 2) ? 1 : 5;
            if (td_q.push(td))
            {
                st.td_produced.fetch_add(1, std::memory_order_relaxed);
                bump_hwm(st.td_hwm, td_q.size());
            }
            else
                st.td_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy Thread — ob_q/td_q 소비, intake→strategy 지연 수집(단독 스레드),
//   OB 100건당 1 / TD 200건당 1 신호 생성.
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& ob_q,
                        RingBuffer<MockTradeData>& td_q,
                        RingBuffer<MockOrderSignal>& order_q,
                        Stats& st,
                        std::atomic<bool>& stop,
                        size_t lat_cap)
{
    st.intake_to_strat_ns.reserve(lat_cap);
    uint64_t obc = 0, tdc = 0;

    while (!stop.load(std::memory_order_relaxed) || !ob_q.empty() || !td_q.empty())
    {
        while (auto opt = ob_q.pop())
        {
            const int64_t t = now_ns();
            if (st.intake_to_strat_ns.size() < lat_cap)
                st.intake_to_strat_ns.push_back(t - opt->send_ts_ns);
            st.ob_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;
            for (int i = 0; i < 5; ++i)
                sink += opt->ask_price[i] - opt->bid_price[i];
            (void)sink;
            if (++obc % 100 == 0)
            {
                MockOrderSignal sig{};
                std::memcpy(sig.ticker, opt->ticker, 7);
                sig.send_ts_ns = opt->send_ts_ns;
                sig.strat_ts_ns = now_ns();
                sig.origin_seq = opt->seq;
                sig.side = 0;
                sig.quantity = 10;
                if (order_q.push(sig))
                {
                    st.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_hwm(st.order_hwm, order_q.size());
                }
                else
                    st.order_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        while (auto opt = td_q.pop())
        {
            const int64_t t = now_ns();
            if (st.intake_to_strat_ns.size() < lat_cap)
                st.intake_to_strat_ns.push_back(t - opt->send_ts_ns);
            st.td_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = opt->price * opt->quantity;
            (void)sink;
            if (++tdc % 200 == 0)
            {
                MockOrderSignal sig{};
                std::memcpy(sig.ticker, opt->ticker, 7);
                sig.send_ts_ns = opt->send_ts_ns;
                sig.strat_ts_ns = now_ns();
                sig.origin_seq = opt->seq;
                sig.side = 1;
                sig.quantity = 5;
                if (order_q.push(sig))
                {
                    st.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_hwm(st.order_hwm, order_q.size());
                }
                else
                    st.order_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order Thread — order_q 소비, strategy→order & E2E 지연 수집(단독 스레드).
//   외부 REST 지연 시뮬레이션 없음 — 내부 파이프라인만.
// ─────────────────────────────────────────────────────────────────────────────
static void order_fn(RingBuffer<MockOrderSignal>& order_q,
                     Stats& st,
                     std::atomic<bool>& stop,
                     size_t lat_cap,
                     std::atomic<bool>& measuring)
{
    st.strat_to_order_ns.reserve(lat_cap);
    st.e2e_ns.reserve(lat_cap);

    while (!stop.load(std::memory_order_relaxed) || !order_q.empty())
    {
        auto opt = order_q.pop();
        if (!opt)
            continue;
        const int64_t t = now_ns();
        // 측정창이 닫힌 뒤(드레인) 항목은 percentile 오염원 → 카운트만.
        if (measuring.load(std::memory_order_relaxed))
        {
            if (st.strat_to_order_ns.size() < lat_cap)
                st.strat_to_order_ns.push_back(t - opt->strat_ts_ns);
            if (st.e2e_ns.size() < lat_cap)
                st.e2e_ns.push_back(t - opt->send_ts_ns);
        }
        st.orders.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Percentile 유틸
// ─────────────────────────────────────────────────────────────────────────────
struct Pctl
{
    int64_t p50 = 0, p99 = 0, p999 = 0, mx = 0;
    size_t  n = 0;
};

static Pctl percentiles(std::vector<int64_t>& v)
{
    Pctl r;
    r.n = v.size();
    if (v.empty())
        return r;
    std::sort(v.begin(), v.end());
    auto at = [&](double p) {
        size_t i = static_cast<size_t>(p * (v.size() - 1));
        return v[i];
    };
    r.p50 = at(0.50);
    r.p99 = at(0.99);
    r.p999 = at(0.999);
    r.mx = v.back();
    return r;
}

static std::string fmt_ns(int64_t n)
{
    char b[32];
    if (n < 1000)
        std::snprintf(b, sizeof(b), "%lld ns", (long long)n);
    else if (n < 1'000'000)
        std::snprintf(b, sizeof(b), "%.2f us", n / 1000.0);
    else
        std::snprintf(b, sizeof(b), "%.2f ms", n / 1'000'000.0);
    return std::string(b);
}

static const char* build_type()
{
#ifdef NDEBUG
    return "Release (NDEBUG)";
#else
    return "Debug (⚠ 측정 무의미 — Release로 재빌드)";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 한 회 실행 (한 rate로 duration 동안). 결과 Pctl들과 드롭 여부를 채운다.
// ─────────────────────────────────────────────────────────────────────────────
struct RunResult
{
    Pctl e2e, i2s, s2o;
    uint64_t ob_prod = 0, td_prod = 0, ob_cons = 0, td_cons = 0;
    uint64_t signals = 0, orders = 0;
    uint64_t drops = 0;
    uint64_t ob_hwm = 0, td_hwm = 0, order_hwm = 0;
    bool lossless = false;
    double elapsed_sec = 0;
};

static RunResult run_once(const std::vector<std::string>& tickers,
                          const ZipfPicker& zipf,
                          int64_t total_rate,
                          double ob_ratio,
                          int duration_sec,
                          size_t ob_cap,
                          size_t td_cap,
                          size_t order_cap,
                          size_t lat_cap)
{
    RingBuffer<MockOrderBook>   ob_q(ob_cap);
    RingBuffer<MockTradeData>   td_q(td_cap);
    RingBuffer<MockOrderSignal> order_q(order_cap);

    Stats st;
    std::atomic<bool> stop{false};
    std::atomic<bool> measuring{true};

    const int64_t t0 = now_ns();
    std::thread t_ord(order_fn, std::ref(order_q), std::ref(st), std::ref(stop), lat_cap, std::ref(measuring));
    std::thread t_str(strategy_fn, std::ref(ob_q), std::ref(td_q), std::ref(order_q), std::ref(st),
                      std::ref(stop), lat_cap);
    std::thread t_ws(producer_fn, std::ref(ob_q), std::ref(td_q), std::cref(tickers), std::cref(zipf),
                     std::ref(st), std::ref(stop), total_rate, ob_ratio,
                     (int64_t)duration_sec * 1'000'000'000LL);

    t_ws.join();                                    // 방출 종료
    measuring.store(false, std::memory_order_relaxed); // 이후 소비분은 드레인 → 샘플 제외
    stop.store(true, std::memory_order_relaxed);
    t_str.join();
    t_ord.join();

    RunResult r;
    r.elapsed_sec = (now_ns() - t0) / 1e9;
    r.e2e = percentiles(st.e2e_ns);
    r.i2s = percentiles(st.intake_to_strat_ns);
    r.s2o = percentiles(st.strat_to_order_ns);
    r.ob_prod = st.ob_produced.load();
    r.td_prod = st.td_produced.load();
    r.ob_cons = st.ob_consumed.load();
    r.td_cons = st.td_consumed.load();
    r.signals = st.signals.load();
    r.orders = st.orders.load();
    r.drops = st.ob_drops.load() + st.td_drops.load() + st.order_drops.load();
    r.ob_hwm = st.ob_hwm.load();
    r.td_hwm = st.td_hwm.load();
    r.order_hwm = st.order_hwm.load();
    r.lossless = (r.drops == 0) && (r.ob_prod == r.ob_cons) && (r.td_prod == r.td_cons) &&
                 (r.signals == r.orders);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// 인자 파서 (--key value)
// ─────────────────────────────────────────────────────────────────────────────
static std::string arg_str(int argc, char** argv, const char* key, const std::string& def)
{
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0)
            return argv[i + 1];
    return def;
}
static int64_t arg_i64(int argc, char** argv, const char* key, int64_t def)
{
    std::string s = arg_str(argc, argv, key, "");
    return s.empty() ? def : std::atoll(s.c_str());
}
static double arg_dbl(int argc, char** argv, const char* key, double def)
{
    std::string s = arg_str(argc, argv, key, "");
    return s.empty() ? def : std::atof(s.c_str());
}

static void print_banner(const char* mode, const std::vector<std::string>& tickers,
                         const std::string& uni_path, double ob_ratio, double zipf_s)
{
    std::printf("=== Market Firehose Bench — mode=%s ===\n", mode);
    std::printf("build           : %s\n", build_type());
    std::printf("tickers         : %zu  (source: %s)\n", tickers.size(),
                uni_path.empty() ? "synthetic" : uni_path.c_str());
    std::printf("ob:td ratio     : %.2f : %.2f\n", ob_ratio, 1.0 - ob_ratio);
    std::printf("zipf s          : %.2f  (0=uniform, 1=대형주 편중)\n", zipf_s);
    std::printf("hw concurrency  : %u\n", std::thread::hardware_concurrency());
    std::printf("NOTE: 내부 처리단만 측정. 네트워크/REST 피드 지연 제외(실제 지배적 병목).\n\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string mode = (argc > 1) ? argv[1] : "load";
    const std::string uni_path = arg_str(argc, argv, "--universe", "");
    const int fallback_n = (int)arg_i64(argc, argv, "--tickers", 2600);
    const double zipf_s = arg_dbl(argc, argv, "--zipf", 1.0);
    const double ob_ratio = arg_dbl(argc, argv, "--ob-ratio", 0.7);

    std::vector<std::string> tickers = load_universe(uni_path, fallback_n);
    ZipfPicker zipf(tickers.size(), zipf_s);

    // 큐 용량: 스로틀 없는 순간 버스트를 흡수하되, 백로그가 tail을 지배하지 않게.
    const size_t OB_CAP = 1u << 16;    // 65,536
    const size_t TD_CAP = 1u << 16;
    const size_t ORDER_CAP = 1u << 12; // 4,096
    const size_t LAT_CAP = 1u << 23;   // 8.4M 샘플 상한

    if (mode == "sweep")
    {
        const int64_t start = arg_i64(argc, argv, "--start", 50000);
        const int64_t step = arg_i64(argc, argv, "--step", 100000);
        const int64_t maxr = arg_i64(argc, argv, "--max", 2'000'000);
        const int dwell = (int)arg_i64(argc, argv, "--dwell", 4);

        print_banner("sweep", tickers, uni_path, ob_ratio, zipf_s);
        std::printf("sweep: rate %lld → %lld step %lld, dwell %ds/step\n\n",
                    (long long)start, (long long)maxr, (long long)step, dwell);
        std::printf("%-12s %-10s %-10s %-10s %-10s %-8s %-9s\n",
                    "offered/s", "e2e_p50", "e2e_p99", "e2e_p999", "e2e_max", "drops", "lossless");
        std::printf("%s\n", std::string(72, '-').c_str());

        int64_t ceiling = 0;
        for (int64_t rate = start; rate <= maxr; rate += step)
        {
            RunResult r = run_once(tickers, zipf, rate, ob_ratio, dwell, OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);
            std::printf("%-12lld %-10s %-10s %-10s %-10s %-8llu %-9s\n",
                        (long long)rate, fmt_ns(r.e2e.p50).c_str(), fmt_ns(r.e2e.p99).c_str(),
                        fmt_ns(r.e2e.p999).c_str(), fmt_ns(r.e2e.mx).c_str(),
                        (unsigned long long)r.drops, r.lossless ? "yes" : "NO");
            // CSV: rate,p50ns,p99ns,p999ns,maxns,drops,lossless,ob_prod,td_prod
            std::printf("CSV,%lld,%lld,%lld,%lld,%lld,%llu,%d,%llu,%llu\n",
                        (long long)rate, (long long)r.e2e.p50, (long long)r.e2e.p99,
                        (long long)r.e2e.p999, (long long)r.e2e.mx, (unsigned long long)r.drops,
                        r.lossless ? 1 : 0, (unsigned long long)r.ob_prod,
                        (unsigned long long)r.td_prod);
            if (r.lossless)
                ceiling = rate;
            else
                break; // 첫 드롭 발생 → 용량 천장 확정
        }
        std::printf("\n용량 천장 (최대 무손실 offered rate): %lld msg/sec\n", (long long)ceiling);
        return 0;
    }

    // ── load 모드 ──
    const int64_t rate = arg_i64(argc, argv, "--rate", 200000);
    const int duration = (int)arg_i64(argc, argv, "--duration", 20);

    print_banner("load", tickers, uni_path, ob_ratio, zipf_s);
    std::printf("offered rate    : %lld msg/sec (총)\n", (long long)rate);
    std::printf("duration        : %d sec\n\n", duration);

    RunResult r = run_once(tickers, zipf, rate, ob_ratio, duration, OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);

    std::printf("=== Throughput ===\n");
    std::printf("elapsed         : %.2f sec\n", r.elapsed_sec);
    std::printf("OB  produced/consumed/drop : %llu / %llu / (hwm %llu)\n",
                (unsigned long long)r.ob_prod, (unsigned long long)r.ob_cons,
                (unsigned long long)r.ob_hwm);
    std::printf("TD  produced/consumed/drop : %llu / %llu / (hwm %llu)\n",
                (unsigned long long)r.td_prod, (unsigned long long)r.td_cons,
                (unsigned long long)r.td_hwm);
    std::printf("signals/orders  : %llu / %llu (order_q hwm %llu)\n",
                (unsigned long long)r.signals, (unsigned long long)r.orders,
                (unsigned long long)r.order_hwm);
    std::printf("total in rate   : %.0f msg/sec (실측)\n",
                (r.ob_prod + r.td_prod) / (r.elapsed_sec > 0 ? r.elapsed_sec : 1));
    std::printf("total drops     : %llu\n\n", (unsigned long long)r.drops);

    std::printf("=== Latency (내부 처리단, 네트워크 제외) ===\n");
    auto line = [](const char* lbl, const Pctl& p) {
        std::printf("%-26s n=%-9zu p50=%-10s p99=%-10s p999=%-10s max=%s\n", lbl, p.n,
                    fmt_ns(p.p50).c_str(), fmt_ns(p.p99).c_str(), fmt_ns(p.p999).c_str(),
                    fmt_ns(p.mx).c_str());
    };
    line("intake -> strategy", r.i2s);
    line("strategy -> order", r.s2o);
    line("E2E (intake -> order)", r.e2e);

    std::printf("\n[%s] no drops, no backlog (전종목 규모 무손실 판정)\n",
                r.lossless ? "PASS" : "FAIL");
    return r.lossless ? 0 : 1;
}
