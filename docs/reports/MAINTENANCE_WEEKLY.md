<!-- drift-check: snapshot 2026-09-08 -->

# 주간 유지관리 리포트 — 2026-09-08
빨간 항목: 3


`py scripts/maintain.py --weekly`가 만든 스냅샷. `.claude/`는 읽기만 했다. 고칠 항목은 사람이 승인한다.

## 1. 미참조 스크립트

`scripts/*.py`·`PYQuant/tools/*.py` 중 다른 파일·`.claude/`·`docs/`·예약작업 어디에서도 이름이 안 나오는 것.

- 없음

## 2. 에이전트·커맨드의 죽은 경로

- [!] `.claude/agents/committer.md:31` → `Quant/kis_token_`
- [!] `.claude/agents/harness-engineer.md:21` → `scripts/check_`
- [!] `.claude/agents/intraday-analyst.md:56` → `Quant/config/config_`

## 3. 부산물 용량

| 폴더 | 크기 |
|---|---|
| `logs` | 1.8 MB |
| `research/dashboard` | 0.6 MB |
| `Quant/build_g1` | 42.4 MB |
| `Quant/build_g2` | 37.8 MB |
| `Quant/build_g2_dbg` | 18.9 MB |
| `Quant/build_g3` | 36.9 MB |
| `Quant/build_win` | 110.4 MB |

## 4. 주석 밀도

게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.

| 파일 | 주석줄 | 전체줄 | 비율 |
|---|---|---|---|
| `Quant/src/strategy/MACrossStrategy.cpp` | 1 | 1 | 100% |
| `Quant/src/strategy/MomentumStrategy.cpp` | 1 | 1 | 100% |
| `Quant/src/strategy/StrategyBase.cpp` | 1 | 1 | 100% |
| `Quant/src/utils/Config.cpp` | 1 | 1 | 100% |
| `Quant/src/utils/Logger.cpp` | 1 | 1 | 100% |
| `Quant/src/utils/Timer.cpp` | 1 | 1 | 100% |
| `Quant/include/universe/UniverseScanner.h` | 64 | 130 | 49% |
| `Quant/include/modes/Monitors.h` | 9 | 22 | 41% |
| `Quant/include/ipc/OrderRouter.h` | 57 | 153 | 37% |
| `Quant/include/api/KisErrorCodes.h` | 4 | 11 | 36% |
| `Quant/include/risk/OrderGate.h` | 122 | 337 | 36% |
| `Quant/include/core/Engine.h` | 104 | 319 | 33% |
| `Quant/include/strategy/StrategyBase.h` | 36 | 125 | 29% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/api/KisClient.h` | 83 | 306 | 27% |
| `Quant/include/api/IOrderExecutor.h` | 20 | 76 | 26% |
| `Quant/include/core/RegimeController.h` | 19 | 76 | 25% |
| `Quant/include/core/Types.h` | 52 | 240 | 22% |
| `Quant/include/api/KisWebSocket.h` | 39 | 181 | 22% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 175 | 842 | 21% |
| `Quant/include/ipc/ZmqBridge.h` | 17 | 82 | 21% |
| `Quant/include/utils/EtfFilter.h` | 24 | 118 | 20% |
| `Quant/include/universe/ScoreWeight.h` | 24 | 123 | 20% |
| `scripts/log_patterns.py` | 3 | 16 | 19% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 34 | 191 | 18% |
| `Quant/src/universe/UniverseScanner.cpp` | 111 | 684 | 16% |
| `Quant/src/main.cpp` | 52 | 321 | 16% |
| `Quant/include/core/MpscQueue.h` | 24 | 149 | 16% |
| `Quant/src/core/Engine.cpp` | 259 | 1670 | 16% |
| `Quant/src/risk/OrderGate.cpp` | 139 | 921 | 15% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 252 | 15% |
| `Quant/include/utils/Utf8.h` | 8 | 55 | 15% |
| `Quant/src/ipc/OrderRouter.cpp` | 137 | 985 | 14% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 54 | 407 | 13% |
| `Quant/include/strategy/ThemeStrategy.h` | 38 | 306 | 12% |
| `Quant/include/strategy/MACrossStrategy.h` | 16 | 129 | 12% |
| `Quant/include/core/RingBuffer.h` | 12 | 98 | 12% |
| `Quant/src/strategy/StrategyFactory.cpp` | 88 | 738 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/include/core/MarketSession.h` | 4 | 35 | 11% |
| `Quant/include/strategy/MomentumStrategy.h` | 12 | 106 | 11% |
| `Quant/include/core/MutexQueue.h` | 8 | 73 | 11% |
| `Quant/include/core/TickSize.h` | 3 | 28 | 11% |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 39 | 372 | 10% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/src/api/WebSocketClient.cpp` | 144 | 1564 | 9% |
| `Quant/include/utils/Logger.h` | 26 | 286 | 9% |
| `PYQuant/tools/macro_regime_feed.py` | 23 | 256 | 9% |
| `PYQuant/tools/universe_feed.py` | 14 | 156 | 9% |
| `scripts/parse_quant_log.py` | 25 | 279 | 9% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 10 | 114 | 9% |
| `Quant/include/strategy/PriceTargetStrategy.h` | 20 | 231 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/src/api/KisClient.cpp` | 232 | 2828 | 8% |
| `scripts/check_plain_language.py` | 38 | 465 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `Quant/src/modes/Monitors.cpp` | 63 | 784 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `PYQuant/backtest/engine.py` | 35 | 529 | 7% |
| `Quant/src/ipc/ZmqBridge.cpp` | 18 | 273 | 7% |
| `PYQuant/dashboard/backfill_live.py` | 10 | 156 | 6% |
| `scripts/dashboard_server.py` | 67 | 1058 | 6% |
| `Quant/src/core/RegimeController.cpp` | 10 | 162 | 6% |
| `PYQuant/data/datagokr_source.py` | 21 | 345 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `PYQuant/tools/month_start_sweep.py` | 5 | 103 | 5% |
| `scripts/analyze_slot_cost.py` | 7 | 147 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `PYQuant/backtest/metrics.py` | 8 | 183 | 4% |
| `scripts/check_runtime_health.py` | 7 | 162 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `PYQuant/dashboard/build_dashboard.py` | 40 | 1126 | 4% |
| `PYQuant/kis/client.py` | 26 | 747 | 3% |
| `scripts/check_code_refs.py` | 8 | 230 | 3% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `scripts/eod_autodoc.py` | 15 | 438 | 3% |
| `scripts/eod_collect.py` | 8 | 235 | 3% |
| `scripts/check_docs.py` | 5 | 150 | 3% |
| `PYQuant/main.py` | 19 | 585 | 3% |
| `scripts/notify_sidecar.py` | 21 | 653 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `PYQuant/tools/probe_kis_investor.py` | 2 | 67 | 3% |
| `PYQuant/db/client.py` | 12 | 409 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `scripts/extract_swap_counterfactual.py` | 5 | 219 | 2% |
| `PYQuant/tools/check_investor_api.py` | 4 | 176 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 226 | 2% |
| `scripts/refresh_dashboard.py` | 4 | 186 | 2% |
| `scripts/gen_code_graph.py` | 12 | 562 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 585 | 2% |
| `scripts/live_prices_feed.py` | 2 | 100 | 2% |
| `scripts/brace_style.py` | 7 | 354 | 2% |
| `scripts/gen_facts.py` | 9 | 461 | 2% |
| `PYQuant/tools/sweep.py` | 2 | 103 | 2% |
| `scripts/build_review_entry.py` | 5 | 259 | 2% |
| `PYQuant/ipc/subscriber.py` | 2 | 108 | 2% |
| `PYQuant/tools/index_intraday_logger.py` | 4 | 216 | 2% |
| `PYQuant/strategy/supply_demand_rank.py` | 1 | 55 | 2% |
| `PYQuant/tools/full_universe_dump.py` | 2 | 111 | 2% |
| `PYQuant/strategy/donchian_breakout.py` | 1 | 56 | 2% |
| `PYQuant/tests/test_adjust_splits.py` | 1 | 57 | 2% |
| `PYQuant/tools/probe_adjusted.py` | 1 | 69 | 1% |
| `PYQuant/tools/investor_flow_logger.py` | 3 | 213 | 1% |
| `scripts/sync_ledgers.py` | 2 | 146 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/check_pykrx_flow.py` | 2 | 150 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
| `scripts/maintain.py` | 4 | 422 | 1% |
| `scripts/_logdir.py` | 1 | 108 | 1% |
| `PYQuant/tests/test_metrics.py` | 1 | 139 | 1% |
| `scripts/summarize_trading_day.py` | 1 | 214 | 0% |
| `scripts/build_study_site.py` | 2 | 483 | 0% |
| `PYQuant/core/logger.py` | 0 | 16 | 0% |
| `PYQuant/data/asof.py` | 0 | 8 | 0% |
| `PYQuant/live/trader.py` | 0 | 73 | 0% |
| `PYQuant/strategy/indicators.py` | 0 | 34 | 0% |
| `PYQuant/tools/ablation_2022.py` | 0 | 68 | 0% |
| `PYQuant/tools/check_pykrx.py` | 0 | 28 | 0% |
| `PYQuant/tools/fullperiod_validate.py` | 0 | 56 | 0% |
| `PYQuant/tools/probe_datagokr.py` | 0 | 53 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 204개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/KisClient.h` | 91 | 7 |
| `Quant/include/api/KisClient.h` | 103 | 4 |
| `Quant/include/api/KisClient.h` | 143 | 9 |
| `Quant/include/api/KisClient.h` | 190 | 4 |
| `Quant/include/api/KisClient.h` | 206 | 5 |
| `Quant/include/api/KisClient.h` | 278 | 4 |
| `Quant/include/api/KisClient.h` | 291 | 4 |
| `Quant/include/api/KisWebSocket.h` | 25 | 13 |
| `Quant/include/api/KisWebSocket.h` | 53 | 7 |
| `Quant/include/api/KisWebSocket.h` | 62 | 4 |
| `Quant/include/core/Engine.h` | 25 | 9 |
| `Quant/include/core/Engine.h` | 46 | 4 |
| `Quant/include/core/Engine.h` | 51 | 4 |
| `Quant/include/core/Engine.h` | 56 | 5 |
| `Quant/include/core/Engine.h` | 66 | 4 |
| `Quant/include/core/Engine.h` | 108 | 6 |
| `Quant/include/core/Engine.h` | 128 | 5 |
| `Quant/include/core/Engine.h` | 153 | 6 |
| `Quant/include/core/Engine.h` | 182 | 5 |
| `Quant/include/core/Engine.h` | 219 | 4 |
| `Quant/include/core/Engine.h` | 231 | 4 |
| `Quant/include/core/MpscQueue.h` | 9 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/RegimeController.h` | 10 | 12 |
| `Quant/include/core/RingBuffer.h` | 8 | 5 |
| `Quant/include/core/Types.h` | 58 | 4 |
| `Quant/include/core/Types.h` | 180 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 17 | 15 |
| `Quant/include/ipc/OrderRouter.h` | 73 | 12 |
| `Quant/include/ipc/OrderRouter.h` | 101 | 4 |
| `Quant/include/ipc/ZmqBridge.h` | 13 | 11 |
| `Quant/include/modes/Monitors.h` | 7 | 5 |
| `Quant/include/risk/OrderGate.h` | 11 | 21 |
| `Quant/include/risk/OrderGate.h` | 41 | 7 |
| `Quant/include/risk/OrderGate.h` | 49 | 5 |
| `Quant/include/risk/OrderGate.h` | 88 | 10 |
| `Quant/include/risk/OrderGate.h` | 100 | 5 |
| `Quant/include/risk/OrderGate.h` | 112 | 4 |
| `Quant/include/risk/OrderGate.h` | 128 | 4 |
| `Quant/include/risk/OrderGate.h` | 135 | 5 |
| `Quant/include/risk/OrderGate.h` | 151 | 4 |
| `Quant/include/risk/OrderGate.h` | 204 | 4 |
| `Quant/include/risk/OrderGate.h` | 239 | 6 |
| `Quant/include/risk/OrderGate.h` | 257 | 5 |
| `Quant/include/risk/OrderGate.h` | 274 | 4 |
| `Quant/include/risk/OrderGate.h` | 321 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 18 | 25 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 55 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 66 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 79 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 310 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 326 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 365 | 8 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 385 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 393 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 401 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 410 | 6 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 425 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 431 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 488 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 497 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 528 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 686 | 8 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 734 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 746 | 4 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 10 | 23 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 280 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 14 | 4 |
| `Quant/include/strategy/MarketMakingStrategy.h` | 10 | 21 |
| `Quant/include/strategy/MomentumStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MomentumStrategy.h` | 51 | 4 |
| `Quant/include/strategy/PriceTargetStrategy.h` | 9 | 10 |
| `Quant/include/strategy/StrategyBase.h` | 25 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 50 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 89 | 6 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 24 | 18 |
| `Quant/include/strategy/ThemeStrategy.h` | 13 | 14 |
| `Quant/include/strategy/ThemeStrategy.h` | 53 | 4 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 19 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 36 | 4 |
| `Quant/include/universe/ScoreWeight.h` | 12 | 19 |
| `Quant/include/universe/UniverseScanner.h` | 7 | 5 |
| `Quant/include/universe/UniverseScanner.h` | 50 | 5 |
| `Quant/include/universe/UniverseScanner.h` | 57 | 4 |
| `Quant/include/universe/UniverseScanner.h` | 66 | 4 |
| `Quant/include/universe/UniverseScanner.h` | 71 | 4 |
| `Quant/include/universe/UniverseScanner.h` | 85 | 7 |
| `Quant/include/universe/UniverseScanner.h` | 95 | 7 |
| `Quant/include/universe/UniverseScanner.h` | 110 | 4 |
| `Quant/include/universe/UniverseScanner.h` | 121 | 5 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 23 | 4 |
| `Quant/include/utils/EtfFilter.h` | 37 | 4 |
| `Quant/include/utils/EtfFilter.h` | 88 | 5 |
| `Quant/include/utils/Logger.h` | 24 | 8 |
| `Quant/include/utils/Utf8.h` | 4 | 8 |
| `Quant/src/main.cpp` | 43 | 5 |
| `Quant/src/main.cpp` | 62 | 5 |
| `Quant/src/main.cpp` | 175 | 6 |
| `Quant/src/api/KisClient.cpp` | 31 | 6 |
| `Quant/src/api/KisClient.cpp` | 149 | 6 |
| `Quant/src/api/KisClient.cpp` | 318 | 5 |
| `Quant/src/api/KisClient.cpp` | 927 | 4 |
| `Quant/src/api/KisClient.cpp` | 1231 | 4 |
| `Quant/src/api/KisClient.cpp` | 1301 | 4 |
| `Quant/src/api/KisClient.cpp` | 1417 | 4 |
| `Quant/src/api/KisClient.cpp` | 1477 | 6 |
| `Quant/src/api/KisClient.cpp` | 1563 | 4 |
| `Quant/src/api/KisClient.cpp` | 1698 | 4 |
| `Quant/src/api/KisClient.cpp` | 1819 | 5 |
| `Quant/src/api/KisClient.cpp` | 1944 | 5 |
| `Quant/src/api/KisClient.cpp` | 2087 | 4 |
| `Quant/src/api/KisClient.cpp` | 2164 | 5 |
| `Quant/src/api/KisClient.cpp` | 2250 | 5 |
| `Quant/src/api/KisClient.cpp` | 2298 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 381 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 679 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 1086 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 1299 | 8 |
| `Quant/src/api/WebSocketClient.cpp` | 1424 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 1462 | 6 |
| `Quant/src/api/WebSocketClient.cpp` | 1509 | 6 |
| `Quant/src/core/Engine.cpp` | 65 | 5 |
| `Quant/src/core/Engine.cpp` | 97 | 11 |
| `Quant/src/core/Engine.cpp` | 265 | 4 |
| `Quant/src/core/Engine.cpp` | 291 | 5 |
| `Quant/src/core/Engine.cpp` | 309 | 4 |
| `Quant/src/core/Engine.cpp` | 365 | 5 |
| `Quant/src/core/Engine.cpp` | 433 | 5 |
| `Quant/src/core/Engine.cpp` | 457 | 4 |
| `Quant/src/core/Engine.cpp` | 480 | 8 |
| `Quant/src/core/Engine.cpp` | 507 | 5 |
| `Quant/src/core/Engine.cpp` | 646 | 4 |
| `Quant/src/core/Engine.cpp` | 790 | 5 |
| `Quant/src/core/Engine.cpp` | 845 | 4 |
| `Quant/src/core/Engine.cpp` | 854 | 5 |
| `Quant/src/core/Engine.cpp` | 892 | 5 |
| `Quant/src/core/Engine.cpp` | 999 | 4 |
| `Quant/src/core/Engine.cpp` | 1030 | 5 |
| `Quant/src/core/Engine.cpp` | 1060 | 7 |
| `Quant/src/core/Engine.cpp` | 1167 | 4 |
| `Quant/src/core/Engine.cpp` | 1222 | 7 |
| `Quant/src/core/Engine.cpp` | 1253 | 4 |
| `Quant/src/core/Engine.cpp` | 1273 | 5 |
| `Quant/src/core/Engine.cpp` | 1478 | 4 |
| `Quant/src/core/Engine.cpp` | 1493 | 5 |
| `Quant/src/core/Engine.cpp` | 1570 | 7 |
| `Quant/src/core/RegimeController.cpp` | 59 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 52 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 62 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 192 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 199 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 289 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 440 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 583 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 685 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 798 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 814 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 865 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 902 | 13 |
| `Quant/src/modes/Monitors.cpp` | 231 | 6 |
| `Quant/src/modes/Monitors.cpp` | 606 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 38 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 52 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 279 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 324 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 342 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 363 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 421 | 6 |
| `Quant/src/risk/OrderGate.cpp` | 503 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 652 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 889 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 28 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 343 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 417 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 463 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 488 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 510 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 553 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 640 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 23 | 11 |
| `Quant/src/universe/UniverseScanner.cpp` | 49 | 5 |
| `Quant/src/universe/UniverseScanner.cpp` | 122 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 150 | 6 |
| `Quant/src/universe/UniverseScanner.cpp` | 209 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 451 | 7 |
| `Quant/src/universe/UniverseScanner.cpp` | 464 | 6 |
| `Quant/src/universe/UniverseScanner.cpp` | 527 | 5 |
| `Quant/src/universe/UniverseScanner.cpp` | 567 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 603 | 4 |
| `PYQuant/data/datagokr_source.py` | 212 | 4 |
| `PYQuant/kis/client.py` | 544 | 4 |
| `PYQuant/live/forward_trader.py` | 193 | 5 |
| `PYQuant/live/forward_trader.py` | 219 | 4 |
| `PYQuant/tools/log_report.py` | 44 | 4 |
| `PYQuant/tools/macro_regime_feed.py` | 49 | 6 |
| `PYQuant/tools/macro_regime_feed.py` | 73 | 6 |
| (이하 4개 생략) | | |

## 5. settings.json 훅 배선

| 이벤트 | 훅 경로 | 실재 |
|---|---|---|
| `PreToolUse` | `.claude/hooks/secret-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/docs-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/lexicon-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/review-reminder.ps1` | 있음 |
| `Stop` | `.claude/hooks/dashboard-refresh.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/eod-gate.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/cron-gate.ps1` | 있음 |

| 훅 파일 | 배선 | BOM UTF-8 |
|---|---|---|
| `cron-gate.ps1` | 됨 | 예 |
| `dashboard-refresh.ps1` | 됨 | 예 |
| `docs-gate.ps1` | 됨 | 예 |
| `eod-gate.ps1` | 됨 | 예 |
| `lexicon-gate.ps1` | 됨 | 예 |
| `review-reminder.ps1` | 됨 | 예 |
| `secret-gate.ps1` | 됨 | 예 |

`settings.json` BOM: 아니오

## 6. `.claude/` 변경(해시 매니페스트)

파일 49개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 없음(첫 실행)

- 추가 49개: `.claude/AGENTS.md`, `.claude/COACH_LOG.md`, `.claude/PROJECT_FACTS.md`, `.claude/UPGRADE_2026-09-07.diff`, `.claude/agents/arch-doc.md`, `.claude/agents/backtest-runner.md`, `.claude/agents/bias-auditor.md`, `.claude/agents/claude-coach.md`, `.claude/agents/committer.md`, `.claude/agents/data-sourcer.md`, `.claude/agents/harness-engineer.md`, `.claude/agents/interviewer.md`, `.claude/agents/intraday-analyst.md`, `.claude/agents/log-reader.md`, `.claude/agents/market-brief.md`, `.claude/agents/perf-optimizer.md`, `.claude/agents/planner.md`, `.claude/agents/pm.md`, `.claude/agents/prep-doc.md`, `.claude/agents/quant-analyst.md`, `.claude/agents/review-recorder.md`, `.claude/agents/reviewer.md`, `.claude/agents/strategist.md`, `.claude/commands/auto-trade-day.md`, `.claude/commands/build.md`, `.claude/commands/daily.md`, `.claude/commands/dashboard-sync.md`, `.claude/commands/dev-loop.md`, `.claude/commands/eod-review.md`, `.claude/commands/intraday-start.md`, `.claude/commands/review-apply.md`, `.claude/commands/review-bundle.md`, `.claude/commands/strategy-debate.md`, `.claude/commands/trade-log.md`, `.claude/commands/verify-backtest.md`, `.claude/commands/watch.md`, `.claude/hooks/cron-gate.ps1`, `.claude/hooks/dashboard-refresh.ps1`, `.claude/hooks/docs-gate.ps1`, `.claude/hooks/eod-gate.ps1`
- 삭제 0개
- 변경 0개
