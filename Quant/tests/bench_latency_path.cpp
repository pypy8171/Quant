// tests/bench_latency_path.cpp
// 09-13 hot path 변경(D-071 Phase 2·3)의 항목별 전후 비교 — 옛 방식을 벤치 안에 최소 복제해 같은 입력으로 잰다.
//  결과는 `docs/reports/PIPELINE_LATENCY_REPORT.md` 결과 ⑥, 절차는 `docs/guides/LOAD_TEST_GUIDE.md` §6.
//  스레드: 항목 1~4는 단일 스레드, 5(캡처)·6(multiplexer)·7(연쇄)은 생산자 1·소비자 1. release 빌드로만 잰다. [why D-071]
//
// [inv] 측정 범위 — 실 KIS WS 프레임 수신·복호화·네트워크는 빠져 있다. 재는 것은 디코더 입구(`^` 페이로드)부터
//   전략 on_trade 반환까지의 in-process 비용이다. 옛 방식 복제는 지운 코드의 핵심 연산(문자열 키 맵·mutex·
//   substr+stoi·전 전략 방문)만 옮긴 것이라 절대값은 참고, 비교는 같은 입력에서의 상대값으로 읽는다.
//
// 측정 본문은 항목별 bench_latency_path_<주제>.cpp, 공용 시계·분위수·출력은 bench_latency_path_support.h/.cpp에 있다.

#include "bench_latency_path_support.h"

using namespace latency_path_bench;

int main()
{
#ifdef _WIN32
    std::system("chcp 65001 > nul");
#endif
    const auto tickers = make_tickers();
    std::printf("bench_latency_path — 09-13 hot path 변경 항목별 전후 (release, 종목 %zu개)\n", kSymbols);
    bench_decode();
    bench_last_price(tickers);
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
