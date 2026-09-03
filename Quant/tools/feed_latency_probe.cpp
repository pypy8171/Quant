// tools/feed_latency_probe.cpp
// 실 KIS 실시간 시세 수신·처리 지연 프로브 — 멀티세션 샤딩(300+종목 확장)
//
// 목적: bench_feed_ingest(합성 부하, TCP loopback)의 자매 실증. 여기서는 **실제
//   KIS WebSocket 실시간 시세**를 다수 구독해, "실데이터가 실제로 우리 파이프라인을
//   통과한다"는 사실과, 수신 콜백에서 주문 결정까지의 내부 처리 지연을 라이브로 잰다.
//
//   왜 멀티세션인가: KIS 실시간 WS는 app_key당 1세션·약 41건 등록 상한이다. 호가+체결
//     동시 구독이면 ~20종목, 체결전용(H0STCNT0만)이면 ~40종목이 세션 한계다. 300+종목을
//     라이브로 받으려면 app_key를 여러 개 발급해 세션 N개를 병렬로 돌리고(샤딩) 결과를
//     하나의 측정으로 합쳐야 한다. 300종목 체결전용이면 ~8세션.
//
//   RingBuffer는 SPSC(단일 생산자/소비자)다. WS 세션마다 수신 스레드가 하나이므로
//     세션당 큐·소비자를 1:1로 두면 각 큐는 SPSC를 지킨다. 지연 표본은 세션별로 모아
//     종료 후 하나로 병합한다.
//
//   ⚠ 정직 경계 — 무료 OpenAPI 실시간 시세에는 µs 해상도의 거래소 원천 타임스탬프가
//     없다(체결시각은 초/HHMMSS 단위). 따라서 "거래소→KIS→우리" 물리 wire 지연은
//     이 경로로 측정 불가다. 이 프로브가 재는 것은 (1) 수신 콜백 진입 → 주문 결정까지의
//     내부 처리 지연(우리가 통제하는 구간)과 (2) 다수 구독에서 관측되는 실제 메시지
//     rate·집계 처리량이다. wire 지연 측정은 bench_feed_ingest가 담당(합성).
//
// 실행 (반드시 장 중 09:00–15:30 KST — 장외에는 틱이 없어 샘플 0):
//   feed_latency_probe --sessions creds.json --universe universe_full.json --count 300
//   feed_latency_probe --configs a.json,b.json --count 80 --trade-only 1
//   feed_latency_probe [config.json] --symbols "005930,000660,..."   (단일세션 하위호환)
//
//   자격증명(세션) 소스 — 아래 중 하나:
//     --sessions <path>  : JSON 배열 [{app_key,app_secret,is_paper?,hts_id?}, ...]
//                          또는 {"sessions":[...]}
//     --configs a,b,...  : 각 파일의 "kis" 블록을 세션 하나로(쉼표 구분)
//     <config.json>      : 위치 인자 하나 = 단일 세션(기존 동작)
//
//   종목 소스 — 우선순위: --symbols > --universe > 내장 기본 15종목
//     --symbols "A,B,.." : 명시 리스트
//     --universe <path>  : universe_full.json 의 codes 배열 로드
//   수량 조절:
//     --count N          : 실제 구독할 종목 수 상한(0=전부). 세션 용량을 넘으면 잘라내고 경고.
//     --per-session K    : 세션당 종목 상한(기본: 체결전용 40 / 호가+체결 20)
//     --trade-only 0|1   : 1(기본)=체결만(H0STCNT0, 밀도↑), 0=호가+체결
//     --duration SEC     : 수신 시간(기본 60)

#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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

// 수신 콜백 → 소비자로 넘기는 경량 레코드 (recv 시각만 필요).
struct ProbeMsg
{
    int64_t recv_ts_ns;
    char    type; // 'O'=orderbook, 'T'=trade
};

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
    auto at = [&](double p) { return v[(size_t)(p * (v.size() - 1))]; };
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

static std::string arg_str(int argc, char** argv, const char* key, const std::string& def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0)
            return argv[i + 1];
    return def;
}
static int64_t arg_i64(int argc, char** argv, const char* key, int64_t def)
{
    std::string s = arg_str(argc, argv, key, "");
    return s.empty() ? def : std::atoll(s.c_str());
}
static std::vector<std::string> split_csv(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        // 앞뒤 공백 제거
        size_t a = tok.find_first_not_of(" \t\r\n");
        size_t b = tok.find_last_not_of(" \t\r\n");
        if (a != std::string::npos)
            out.push_back(tok.substr(a, b - a + 1));
    }
    return out;
}

// kis 블록(app_key 등)을 담은 JSON 오브젝트 → KisConfig
static KisConfig kc_from_kis_obj(const nlohmann::json& k)
{
    KisConfig kc;
    kc.app_key      = k.at("app_key").get<std::string>();
    kc.app_secret   = k.at("app_secret").get<std::string>();
    kc.account_no   = k.value("account_no", "");
    kc.account_type = k.value("account_type", "01");
    kc.hts_id       = k.value("hts_id", "");
    kc.is_paper     = k.value("is_paper", true);
    return kc;
}

// 세션 하나: 자격증명 + 담당 종목 + 큐 + WS + 소비자 스레드 + 표본.
struct Session
{
    KisConfig                          kc;
    std::vector<std::string>           symbols;
    std::unique_ptr<RingBuffer<ProbeMsg>> q;
    std::unique_ptr<KisWebSocket>      ws;
    std::thread                        consumer;
    std::vector<int64_t>               lat;      // recv→decision (세션 전용, race 없음)
    std::atomic<uint64_t>              ob{0}, td{0}, dropped{0}, decided{0};
    bool                               connected = false;
};

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const int  duration    = (int)arg_i64(argc, argv, "--duration", 60);
    const bool trade_only  = arg_i64(argc, argv, "--trade-only", 1) != 0;
    const int  count_cap   = (int)arg_i64(argc, argv, "--count", 0); // 0 = 전부
    const int  per_default = trade_only ? 40 : 20;
    const int  per_session = (int)arg_i64(argc, argv, "--per-session", per_default);
    const std::string sym_csv   = arg_str(argc, argv, "--symbols", "");
    const std::string universe  = arg_str(argc, argv, "--universe", "");
    const std::string sessions_path = arg_str(argc, argv, "--sessions", "");
    const std::string configs_csv   = arg_str(argc, argv, "--configs", "");

    // ---- 1) 자격증명(세션) 로드 ----
    std::vector<KisConfig> creds;
    try
    {
        if (!sessions_path.empty())
        {
            std::ifstream f(sessions_path);
            if (!f) { std::printf("[probe] --sessions 열기 실패: %s\n", sessions_path.c_str()); return 1; }
            nlohmann::json j; f >> j;
            const nlohmann::json& arr = j.is_array() ? j : j.at("sessions");
            for (const auto& s : arr)
                creds.push_back(kc_from_kis_obj(s));
        }
        else if (!configs_csv.empty())
        {
            for (const auto& path : split_csv(configs_csv))
            {
                std::ifstream f(path);
                if (!f) { std::printf("[probe] --configs 항목 열기 실패: %s\n", path.c_str()); return 1; }
                nlohmann::json j; f >> j;
                creds.push_back(kc_from_kis_obj(j.at("kis")));
            }
        }
        else
        {
            // 위치 인자 하나 = 단일 config(기존 동작)
            std::string cfg_path = "config/config_dev_paper.json";
            for (int i = 1; i < argc; ++i)
                if (argv[i][0] != '-') { cfg_path = argv[i]; break; }
            std::ifstream f(cfg_path);
            if (!f) { std::printf("[probe] config 열기 실패: %s\n", cfg_path.c_str()); return 1; }
            nlohmann::json j; f >> j;
            creds.push_back(kc_from_kis_obj(j.at("kis")));
        }
    }
    catch (const std::exception& e)
    {
        std::printf("[probe] 자격증명 파싱 실패: %s\n", e.what());
        return 1;
    }
    if (creds.empty()) { std::printf("[probe] 세션 자격증명이 없다.\n"); return 1; }

    // ---- 2) 종목 리스트 로드 ----
    std::vector<std::string> all_symbols = split_csv(sym_csv);
    if (all_symbols.empty() && !universe.empty())
    {
        try
        {
            std::ifstream f(universe);
            if (!f) { std::printf("[probe] --universe 열기 실패: %s\n", universe.c_str()); return 1; }
            nlohmann::json j; f >> j;
            if (j.contains("universe") && j.at("universe").is_array())
            {
                // universe_scan.json 스키마: [{"ticker":"005930",...}, ...]
                for (const auto& e : j.at("universe"))
                    all_symbols.push_back(e.at("ticker").get<std::string>());
            }
            else
            {
                // {"codes":[...]} 또는 바로 배열
                const nlohmann::json& codes = j.contains("codes") ? j.at("codes") : j;
                for (const auto& c : codes)
                    all_symbols.push_back(c.get<std::string>());
            }
        }
        catch (const std::exception& e)
        {
            std::printf("[probe] universe 파싱 실패: %s\n", e.what());
            return 1;
        }
    }
    if (all_symbols.empty())
        all_symbols = {"005930", "000660", "373220", "207940", "005380", "000270",
                       "005490", "035420", "051910", "006400", "035720", "105560",
                       "055550", "012330", "028260"};

    // ---- 3) 수량 조절 + 세션 용량 대조 ----
    const int sessions_avail = (int)creds.size();
    const int capacity       = sessions_avail * per_session; // 라이브로 받을 수 있는 상한
    int want = (int)all_symbols.size();
    if (count_cap > 0)
        want = std::min(want, count_cap);
    int use = std::min(want, capacity);
    if (use < want)
    {
        int need_sessions = (want + per_session - 1) / per_session;
        std::printf("[probe] ⚠ 요청 %d종목 > 세션 용량 %d (%d세션 × %d/세션). %d종목만 구독.\n",
                    want, capacity, sessions_avail, per_session, use);
        std::printf("        %d종목을 라이브로 받으려면 app_key %d개(현재 %d개)가 필요하다.\n",
                    want, need_sessions, sessions_avail);
    }
    all_symbols.resize(use);

    const int used_sessions = (use + per_session - 1) / per_session;
    const int used_sessions_clamped = std::max(1, std::min(used_sessions, sessions_avail));

    std::printf("=== KIS 실시간 시세 멀티세션 프로브 ===\n");
    std::printf("세션(app_key)   : %d개 발견, %d개 사용\n", sessions_avail, used_sessions_clamped);
    std::printf("구독 밀도       : %s (%d등록/종목, 세션당 최대 %d종목)\n",
                trade_only ? "체결전용 H0STCNT0" : "호가+체결", trade_only ? 1 : 2, per_session);
    std::printf("종목            : %d개 구독 (요청 %d)\n", use, want);
    std::printf("duration        : %d sec\n", duration);
    std::printf("NOTE: 내부(수신콜백→주문결정) 지연 + 관측 실 msg rate만 측정. 거래소 wire\n");
    std::printf("      지연은 무료 API에 µs 원천 ts가 없어 측정 불가(정직 경계).\n");
    std::printf("      장외(09:00–15:30 KST 밖)에는 틱이 없어 샘플 0.\n\n");

    // ---- 4) 종목을 세션에 분배(연속 청크) + 세션 구성 ----
    std::vector<std::unique_ptr<Session>> sess;
    for (int i = 0; i < used_sessions_clamped; ++i)
    {
        auto s = std::make_unique<Session>();
        s->kc = creds[i];
        s->q  = std::make_unique<RingBuffer<ProbeMsg>>(1u << 16);
        int begin = i * per_session;
        int end   = std::min((int)all_symbols.size(), begin + per_session);
        for (int k = begin; k < end; ++k)
            s->symbols.push_back(all_symbols[k]);
        sess.push_back(std::move(s));
    }

    std::atomic<bool> stop{false};

    // 세션별 소비자 스레드 — 각 큐는 SPSC(WS 수신 스레드 1 : 소비자 1).
    for (auto& sp : sess)
    {
        Session* s = sp.get();
        s->lat.reserve(1u << 21);
        s->consumer = std::thread([s, &stop] {
            while (!stop.load(std::memory_order_relaxed) || !s->q->empty())
            {
                auto opt = s->q->pop();
                if (!opt)
                    continue;
                // 트레이딩 결정 대리 연산(전략 hot path 근사).
                volatile int64_t sink = opt->recv_ts_ns ^ 0x5a5a;
                (void)sink;
                const int64_t t = now_ns();
                if (s->lat.size() < s->lat.capacity())
                    s->lat.push_back(t - opt->recv_ts_ns);
                s->decided.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // ---- 5) 세션별 WS 연결 ----
    for (auto& sp : sess)
    {
        Session* s = sp.get();
        s->ws = std::make_unique<KisWebSocket>(s->kc);
        s->ws->set_callbacks(
            [s](const OrderBook&) {
                ProbeMsg m{now_ns(), 'O'};
                if (s->q->push(m)) s->ob.fetch_add(1, std::memory_order_relaxed);
                else               s->dropped.fetch_add(1, std::memory_order_relaxed);
            },
            [s](const TradeData&) {
                ProbeMsg m{now_ns(), 'T'};
                if (s->q->push(m)) s->td.fetch_add(1, std::memory_order_relaxed);
                else               s->dropped.fetch_add(1, std::memory_order_relaxed);
            });
    }

    const int64_t t0 = now_ns();
    int connected_sessions = 0;
    for (size_t i = 0; i < sess.size(); ++i)
    {
        Session* s = sess[i].get();
        std::vector<WatchSpec> specs;
        for (const auto& t : s->symbols)
        {
            WatchSpec w; w.ticker = t; w.market = Market::KR; w.trade_only = trade_only;
            specs.push_back(w);
        }
        std::printf("[probe] 세션 %zu/%zu 연결 시도 (%zu종목)...\n",
                    i + 1, sess.size(), s->symbols.size());
        if (s->ws->connect(specs))
        {
            s->connected = true;
            ++connected_sessions;
        }
        else
        {
            std::printf("[probe] 세션 %zu 연결 실패 (approval key/세션/상한 확인).\n", i + 1);
        }
    }
    if (connected_sessions == 0)
    {
        std::printf("[probe] 전 세션 연결 실패.\n");
        stop.store(true);
        for (auto& sp : sess) if (sp->consumer.joinable()) sp->consumer.join();
        return 1;
    }
    std::printf("[probe] %d/%zu 세션 연결. %d초 수신...\n\n", connected_sessions, sess.size(), duration);

    for (int i = 0; i < duration; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    for (auto& sp : sess)
        if (sp->connected) sp->ws->disconnect();
    stop.store(true, std::memory_order_relaxed);
    for (auto& sp : sess)
        if (sp->consumer.joinable()) sp->consumer.join();
    const double elapsed = (now_ns() - t0) / 1e9;

    // ---- 6) 집계 + 표본 병합 ----
    uint64_t ob_total = 0, td_total = 0, drop_total = 0, dec_total = 0;
    std::vector<int64_t> lat;
    for (auto& sp : sess)
    {
        ob_total   += sp->ob.load();
        td_total   += sp->td.load();
        drop_total += sp->dropped.load();
        dec_total  += sp->decided.load();
        lat.insert(lat.end(), sp->lat.begin(), sp->lat.end());
    }
    const uint64_t total = ob_total + td_total;

    std::printf("=== 수신 결과 (전 세션 집계) ===\n");
    std::printf("elapsed         : %.1f sec\n", elapsed);
    std::printf("세션 연결       : %d/%zu\n", connected_sessions, sess.size());
    std::printf("orderbook/trade : %llu / %llu (drop %llu)\n",
                (unsigned long long)ob_total, (unsigned long long)td_total,
                (unsigned long long)drop_total);
    std::printf("decided         : %llu\n", (unsigned long long)dec_total);
    std::printf("관측 msg rate   : %.1f msg/sec (실 라이브, %d종목/%d세션)\n",
                total / (elapsed > 0 ? elapsed : 1), use, connected_sessions);

    if (lat.empty())
    {
        std::printf("\n[주의] 샘플 0 — 장외이거나 틱 미수신. 장 중(09:00–15:30 KST)에 재실행.\n");
        return 0;
    }
    Pctl p = percentiles(lat);
    std::printf("\n=== 내부 지연 (수신콜백 → 주문결정, 실데이터, 전 세션 병합) ===\n");
    std::printf("n=%zu  p50=%s  p99=%s  p999=%s  max=%s\n", p.n, fmt_ns(p.p50).c_str(),
                fmt_ns(p.p99).c_str(), fmt_ns(p.p999).c_str(), fmt_ns(p.mx).c_str());
    std::printf("\n비교: bench_feed_ingest(합성)의 proc(recv→order)와 같은 구간 — 실데이터로 재확인.\n");
    return 0;
}
