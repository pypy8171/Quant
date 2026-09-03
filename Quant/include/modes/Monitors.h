#pragma once
#include "api/KisClient.h" // KisConfig
#include <atomic>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 관찰용 모니터 모드 — 트레이딩 엔진과 무관한 시세 표시 툴.
//  main()에서 mode 값에 따라 호출되며, 각자 자기 루프를 돌다 running=false에 종료한다.
//  반환값은 프로세스 종료 코드(0=정상).
// ─────────────────────────────────────────────────────────────────────────────

// FEED — KIS WebSocket 실시간 호가/체결을 1초 주기로 콘솔 표시.
// futures: 국내 선물 종목코드(H0IFCNT0/H0IFASP0, 실계좌 도메인 전용). 비어 있으면 현물만.
int run_feed(const KisConfig& kis_cfg, const std::vector<std::string>& tickers,
             const std::vector<std::string>& futures, const std::atomic<bool>& running);

// KR_TEST — KOSPI 시총 상위 20 + 관심종목 fundamentals(REST) + 체결(WS) 1초 표시.
int run_kr_test(const KisConfig& kis_cfg, const std::atomic<bool>& running);

// US_TEST — M7 미국주식 REST 시세(장 외에도 동작) 500ms 표시.
int run_us_test(const KisConfig& kis_cfg, const std::atomic<bool>& running);
