// tools/bench_rest_pool.cpp
// REST 커넥션 풀링(P-2) 효과 측정 — 조회 API를 N회 연속 호출해 호출당 지연 분포를 낸다.
//
//   재는 것: KisClient의 HTTP 왕복 1회(요청 조립 → 전송 → 응답 수신·파싱)에 걸린 벽시계 시간.
//   비교 축은 하나뿐이다 — 커넥션 풀링 유무. 같은 바이너리를 환경변수로만 나눠 두 번 돌린다.
//
//     풀링 ON  (기본)             : hSession/hConnect 상주 → TCP+TLS 핸드셰이크를 최초 1회만
//     풀링 OFF (QUANT_HTTP_NOPOOL=1): 매 요청 뒤 연결 파기 → 요청마다 핸드셰이크 재지불
//
//   주문(POST)이 아니라 조회(GET)로 재는 이유: 주문 왕복은 전략 신호에 의존해 통제가 안 되고,
//   실계좌 발주를 동반한다. 풀링이 없애는 비용(핸드셰이크)은 두 경로가 동일하므로 조회로 잰다.
//
//   호출 간격(pace_ms)은 KIS 유통량 제한을 넘지 않으려는 것이고, 측정값 자체(호출당 지연)에는
//   들어가지 않는다. 다만 간격이 너무 길면 서버가 idle 연결을 끊어 풀링 이점이 사라질 수 있다.
//
//   시세 REST는 모의 도메인이 미지원(HTTP500)이라 실전 시세키가 필요하다.
//   config에 quote_kis 블록이 있으면 그걸, 없으면 kis 블록을 쓴다(future_quote_probe와 동일).
//
//   사용법:
//     bench_rest_pool <config> [ticker=005930] [n=40] [pace_ms=60]
//   예)
//     bench_rest_pool config/config_dev_paper.json
//     set QUANT_HTTP_NOPOOL=1 && bench_rest_pool config/config_dev_paper.json

#include "api/KisClient.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// 정렬된 표본에서 백분위수 — 최근접 순위(nearest-rank).
static double pct(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    size_t idx = (size_t)std::ceil(p / 100.0 * (double)sorted.size());
    if (idx == 0)
        idx = 1;
    if (idx > sorted.size())
        idx = sorted.size();
    return sorted[idx - 1];
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2)
    {
        std::cout << "사용법: bench_rest_pool <config> [ticker=005930] [n=40] [pace_ms=60]\n"
                     "  풀링 ON : bench_rest_pool config/config_dev_paper.json\n"
                     "  풀링 OFF: set QUANT_HTTP_NOPOOL=1 && bench_rest_pool config/config_dev_paper.json\n";
        return 1;
    }

    const std::string config_path = argv[1];
    const std::string ticker = (argc > 2) ? argv[2] : "005930";
    const int n = (argc > 3) ? std::atoi(argv[3]) : 40;
    const int pace_ms = (argc > 4) ? std::atoi(argv[4]) : 60;

    std::ifstream f(config_path);
    if (!f)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }
    json cfg = json::parse(f);

    const char* block = cfg.contains("quote_kis") ? "quote_kis" : "kis";
    const json& kb = cfg[block];

    KisConfig kc;
    kc.app_key    = kb.value("app_key", "");
    kc.app_secret = kb.value("app_secret", "");
    kc.is_paper   = kb.value("is_paper", false);

    const char* np = std::getenv("QUANT_HTTP_NOPOOL");
    const bool nopool = np && *np == '1';

    std::cout << "=== REST 커넥션 풀링 벤치 (P-2) ===\n";
    std::cout << "config=" << config_path << "  키블록=" << block
              << "  is_paper=" << (kc.is_paper ? "true" : "false") << "\n";
    std::cout << "ticker=" << ticker << "  n=" << n << "  pace=" << pace_ms << "ms\n";
    std::cout << "커넥션 풀링 = " << (nopool ? "OFF (QUANT_HTTP_NOPOOL=1 — 요청마다 TCP+TLS 재수립)"
                                             : "ON (상주 연결 재사용)")
              << "\n\n";
    if (kc.is_paper)
        std::cout << "[경고] is_paper=true 시세키 — 모의 도메인은 시세 REST 미지원이라 HTTP500이 예상됩니다.\n"
                     "       config에 실전 quote_kis 블록을 두거나 실전 config를 쓰세요.\n\n";

    KisClient kis(kc);
    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }
    std::cout << "[1] 인증 완료 (인증 왕복은 측정에서 제외)\n";

    // 워밍업 — 첫 호출에는 DNS 조회·토큰 경로 초기화가 섞여 분포를 왜곡한다. 측정에서 뺀다.
    constexpr int kWarmup = 3;
    for (int i = 0; i < kWarmup; ++i)
    {
        kis.get_current_price(ticker);
        std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    }
    std::cout << "[2] 워밍업 " << kWarmup << "회 완료\n";

    std::vector<double> ms;
    ms.reserve((size_t)n);
    int fail = 0;
    for (int i = 0; i < n; ++i)
    {
        auto t0 = Clock::now();
        double price = kis.get_current_price(ticker);
        auto t1 = Clock::now();

        double dt = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (price > 0.0)
            ms.push_back(dt);
        else
            ++fail; // 조회 실패는 분포에서 제외(재시도 백오프가 섞여 지연을 왜곡)

        if (pace_ms > 0 && i + 1 < n)
            std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    }
    std::cout << "[3] 측정 " << n << "회 완료 (성공 " << ms.size() << " / 실패 " << fail << ")\n\n";

    if (ms.empty())
    {
        std::cerr << "[중단] 성공 표본 0 — 시세키/도메인/장 상태를 확인하세요.\n";
        return 4;
    }

    std::vector<double> s = ms;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : s)
        sum += v;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "── 호출당 지연 (ms) ──────────────────────\n";
    std::cout << "  표본        " << s.size() << "\n";
    std::cout << "  최소        " << s.front() << "\n";
    std::cout << "  중앙값(p50) " << pct(s, 50) << "\n";
    std::cout << "  p90         " << pct(s, 90) << "\n";
    std::cout << "  p99         " << pct(s, 99) << "\n";
    std::cout << "  최대        " << s.back() << "\n";
    std::cout << "  평균        " << (sum / (double)s.size()) << "\n";
    std::cout << "──────────────────────────────────────────\n";
    std::cout << "풀링 " << (nopool ? "OFF" : "ON") << " 기준값입니다. 반대 조건으로 한 번 더 돌려 비교하세요.\n";
    std::cout << "  같은 회차·같은 네트워크 상태에서 연달아 재야 비교가 성립합니다(외부 회선 변동이 섞임).\n";
    return 0;
}
