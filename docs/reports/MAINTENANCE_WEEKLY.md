<!-- drift-check: snapshot 2026-09-09 -->

# 주간 유지관리 리포트 — 2026-09-09

빨간 항목: 5

`py scripts/maintain.py --weekly`가 만든 스냅샷. `.claude/`는 읽기만 했다. 고칠 항목은 사람이 승인한다.

## 1. 미참조 스크립트

`scripts/*.py`·`PYQuant/tools/*.py` 중 다른 파일·`.claude/`·`docs/`·예약작업 어디에서도 이름이 안 나오는 것.

- 없음

## 2. 에이전트·커맨드의 죽은 경로

- [!] `.claude/agents/prep-doc.md:8` → `_private/…`
- [!] `.claude/agents/prep-doc.md:12` → `_private/…`
- [!] `.claude/agents/prep-doc.md:19` → `_private/…`
- [!] `.claude/agents/prep-doc.md:69` → `_private/…`
- [!] `.claude/agents/prep-doc.md:69` → `_private/…`

## 3. 부산물 용량

| 폴더 | 크기 |
|---|---|
| `logs` | 1.9 MB |
| `research/dashboard` | 0.6 MB |
| `Quant/build_g1` | 42.4 MB |
| `Quant/build_g2` | 37.8 MB |
| `Quant/build_g2_dbg` | 18.9 MB |
| `Quant/build_g3` | 36.9 MB |
| `Quant/build_win` | 116.6 MB |

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
| `Quant/include/modes/Monitors.h` | 9 | 22 | 41% |
| `Quant/include/ipc/OrderRouter.h` | 68 | 180 | 38% |
| `Quant/include/api/KisErrorCodes.h` | 4 | 11 | 36% |
| `Quant/include/risk/OrderGate.h` | 109 | 331 | 33% |
| `Quant/include/core/Engine.h` | 104 | 342 | 30% |
| `Quant/include/strategy/StrategyBase.h` | 36 | 127 | 28% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/api/KisClient.h` | 83 | 314 | 26% |
| `Quant/include/api/IOrderExecutor.h` | 20 | 76 | 26% |
| `Quant/include/universe/UniverseScanner.h` | 21 | 89 | 24% |
| `Quant/include/universe/MaAlign.h` | 12 | 53 | 23% |
| `Quant/include/core/Types.h` | 52 | 240 | 22% |
| `Quant/include/ipc/ZmqBridge.h` | 17 | 82 | 21% |
| `Quant/include/api/KisWebSocket.h` | 39 | 190 | 21% |
| `Quant/include/core/RegimeController.h` | 19 | 93 | 20% |
| `scripts/log_patterns.py` | 3 | 16 | 19% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 182 | 1045 | 17% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 34 | 205 | 17% |
| `Quant/include/utils/EtfFilter.h` | 24 | 150 | 16% |
| `Quant/include/core/MpscQueue.h` | 24 | 165 | 15% |
| `Quant/src/main.cpp` | 55 | 392 | 14% |
| `Quant/include/universe/ScoreWeight.h` | 24 | 172 | 14% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 282 | 13% |
| `Quant/src/core/Engine.cpp` | 264 | 2088 | 13% |
| `Quant/include/utils/Utf8.h` | 8 | 66 | 12% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 54 | 448 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/src/risk/OrderGate.cpp` | 142 | 1207 | 12% |
| `Quant/include/core/RingBuffer.h` | 12 | 104 | 12% |
| `Quant/include/strategy/MACrossStrategy.h` | 16 | 141 | 11% |
| `Quant/src/ipc/OrderRouter.cpp` | 152 | 1373 | 11% |
| `Quant/include/strategy/MomentumStrategy.h` | 12 | 110 | 11% |
| `Quant/include/strategy/ThemeStrategy.h` | 38 | 354 | 11% |
| `Quant/src/strategy/StrategyFactory.cpp` | 98 | 937 | 10% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/include/core/MarketSession.h` | 4 | 39 | 10% |
| `Quant/src/universe/UniverseScanner.cpp` | 133 | 1342 | 10% |
| `Quant/include/core/MutexQueue.h` | 8 | 82 | 10% |
| `scripts/check_plain_language.py` | 47 | 508 | 9% |
| `PYQuant/tools/macro_regime_feed.py` | 23 | 256 | 9% |
| `PYQuant/tools/universe_feed.py` | 14 | 156 | 9% |
| `scripts/parse_quant_log.py` | 25 | 279 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 39 | 475 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `Quant/src/api/WebSocketClient.cpp` | 144 | 1842 | 8% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 10 | 129 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `Quant/include/utils/Logger.h` | 26 | 346 | 8% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `Quant/src/modes/Monitors.cpp` | 63 | 913 | 7% |
| `Quant/include/strategy/PriceTargetStrategy.h` | 20 | 293 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `Quant/src/api/KisClient.cpp` | 232 | 3428 | 7% |
| `PYQuant/backtest/engine.py` | 35 | 529 | 7% |
| `PYQuant/dashboard/backfill_live.py` | 10 | 156 | 6% |
| `scripts/dashboard_server.py` | 67 | 1058 | 6% |
| `PYQuant/data/datagokr_source.py` | 21 | 345 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `Quant/src/ipc/ZmqBridge.cpp` | 18 | 305 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `Quant/include/core/TickSize.h` | 3 | 56 | 5% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `Quant/src/core/RegimeController.cpp` | 10 | 197 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `PYQuant/tools/month_start_sweep.py` | 5 | 103 | 5% |
| `scripts/analyze_slot_cost.py` | 7 | 147 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `PYQuant/backtest/metrics.py` | 8 | 183 | 4% |
| `scripts/check_runtime_health.py` | 7 | 162 | 4% |
| `scripts/check_code_refs.py` | 9 | 232 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `PYQuant/dashboard/build_dashboard.py` | 40 | 1126 | 4% |
| `PYQuant/kis/client.py` | 26 | 747 | 3% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `scripts/eod_autodoc.py` | 15 | 438 | 3% |
| `scripts/eod_collect.py` | 8 | 235 | 3% |
| `scripts/check_docs.py` | 5 | 150 | 3% |
| `PYQuant/main.py` | 19 | 585 | 3% |
| `scripts/notify_sidecar.py` | 21 | 653 | 3% |
| `scripts/check_code_conventions.py` | 6 | 190 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `PYQuant/tools/probe_kis_investor.py` | 2 | 67 | 3% |
| `PYQuant/db/client.py` | 12 | 409 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `scripts/gen_facts.py` | 12 | 473 | 3% |
| `scripts/extract_swap_counterfactual.py` | 5 | 219 | 2% |
| `PYQuant/tools/check_investor_api.py` | 4 | 176 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 226 | 2% |
| `scripts/refresh_dashboard.py` | 4 | 186 | 2% |
| `scripts/gen_code_graph.py` | 12 | 566 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 585 | 2% |
| `scripts/live_prices_feed.py` | 2 | 100 | 2% |
| `scripts/brace_style.py` | 7 | 354 | 2% |
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
| `scripts/maintain.py` | 6 | 430 | 1% |
| `scripts/sync_ledgers.py` | 2 | 146 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/check_pykrx_flow.py` | 2 | 150 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
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
| `PYQuant/tools/nxt_divergence_probe.py` | 0 | 138 | 0% |
| `PYQuant/tools/probe_datagokr.py` | 0 | 53 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 197개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/KisClient.h` | 99 | 7 |
| `Quant/include/api/KisClient.h` | 111 | 4 |
| `Quant/include/api/KisClient.h` | 151 | 9 |
| `Quant/include/api/KisClient.h` | 198 | 4 |
| `Quant/include/api/KisClient.h` | 214 | 5 |
| `Quant/include/api/KisClient.h` | 286 | 4 |
| `Quant/include/api/KisClient.h` | 299 | 4 |
| `Quant/include/api/KisWebSocket.h` | 25 | 13 |
| `Quant/include/api/KisWebSocket.h` | 53 | 7 |
| `Quant/include/api/KisWebSocket.h` | 62 | 4 |
| `Quant/include/core/Engine.h` | 25 | 9 |
| `Quant/include/core/Engine.h` | 46 | 4 |
| `Quant/include/core/Engine.h` | 51 | 4 |
| `Quant/include/core/Engine.h` | 56 | 5 |
| `Quant/include/core/Engine.h` | 71 | 4 |
| `Quant/include/core/Engine.h` | 120 | 6 |
| `Quant/include/core/Engine.h` | 141 | 5 |
| `Quant/include/core/Engine.h` | 176 | 6 |
| `Quant/include/core/Engine.h` | 205 | 5 |
| `Quant/include/core/Engine.h` | 242 | 4 |
| `Quant/include/core/Engine.h` | 254 | 4 |
| `Quant/include/core/MpscQueue.h` | 9 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/RegimeController.h` | 10 | 12 |
| `Quant/include/core/RingBuffer.h` | 8 | 5 |
| `Quant/include/core/Types.h` | 58 | 4 |
| `Quant/include/core/Types.h` | 180 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 17 | 15 |
| `Quant/include/ipc/OrderRouter.h` | 73 | 12 |
| `Quant/include/ipc/OrderRouter.h` | 101 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 121 | 6 |
| `Quant/include/ipc/ZmqBridge.h` | 13 | 11 |
| `Quant/include/modes/Monitors.h` | 7 | 5 |
| `Quant/include/risk/OrderGate.h` | 44 | 4 |
| `Quant/include/risk/OrderGate.h` | 77 | 10 |
| `Quant/include/risk/OrderGate.h` | 89 | 5 |
| `Quant/include/risk/OrderGate.h` | 102 | 4 |
| `Quant/include/risk/OrderGate.h` | 118 | 4 |
| `Quant/include/risk/OrderGate.h` | 125 | 5 |
| `Quant/include/risk/OrderGate.h` | 142 | 4 |
| `Quant/include/risk/OrderGate.h` | 197 | 4 |
| `Quant/include/risk/OrderGate.h` | 232 | 6 |
| `Quant/include/risk/OrderGate.h` | 251 | 5 |
| `Quant/include/risk/OrderGate.h` | 268 | 4 |
| `Quant/include/risk/OrderGate.h` | 315 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 19 | 25 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 56 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 67 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 83 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 268 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 367 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 383 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 435 | 8 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 462 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 474 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 483 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 497 | 6 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 515 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 521 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 605 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 614 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 649 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 848 | 8 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 914 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 930 | 4 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 10 | 23 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 310 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 14 | 4 |
| `Quant/include/strategy/MarketMakingStrategy.h` | 10 | 21 |
| `Quant/include/strategy/MomentumStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MomentumStrategy.h` | 53 | 4 |
| `Quant/include/strategy/PriceTargetStrategy.h` | 9 | 10 |
| `Quant/include/strategy/StrategyBase.h` | 25 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 50 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 90 | 6 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 24 | 18 |
| `Quant/include/strategy/ThemeStrategy.h` | 13 | 14 |
| `Quant/include/strategy/ThemeStrategy.h` | 53 | 4 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 19 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 36 | 4 |
| `Quant/include/universe/ScoreWeight.h` | 12 | 19 |
| `Quant/include/universe/UniverseScanner.h` | 7 | 6 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 23 | 4 |
| `Quant/include/utils/EtfFilter.h` | 45 | 4 |
| `Quant/include/utils/EtfFilter.h` | 106 | 5 |
| `Quant/include/utils/Logger.h` | 24 | 8 |
| `Quant/include/utils/Utf8.h` | 4 | 8 |
| `Quant/src/main.cpp` | 50 | 5 |
| `Quant/src/main.cpp` | 69 | 5 |
| `Quant/src/main.cpp` | 223 | 6 |
| `Quant/src/api/KisClient.cpp` | 31 | 6 |
| `Quant/src/api/KisClient.cpp` | 169 | 6 |
| `Quant/src/api/KisClient.cpp` | 360 | 5 |
| `Quant/src/api/KisClient.cpp` | 1091 | 4 |
| `Quant/src/api/KisClient.cpp` | 1460 | 4 |
| `Quant/src/api/KisClient.cpp` | 1537 | 4 |
| `Quant/src/api/KisClient.cpp` | 1665 | 4 |
| `Quant/src/api/KisClient.cpp` | 1748 | 6 |
| `Quant/src/api/KisClient.cpp` | 1852 | 4 |
| `Quant/src/api/KisClient.cpp` | 1992 | 4 |
| `Quant/src/api/KisClient.cpp` | 2141 | 5 |
| `Quant/src/api/KisClient.cpp` | 2299 | 5 |
| `Quant/src/api/KisClient.cpp` | 2479 | 4 |
| `Quant/src/api/KisClient.cpp` | 2577 | 5 |
| `Quant/src/api/KisClient.cpp` | 2679 | 5 |
| `Quant/src/api/KisClient.cpp` | 2740 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 450 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 799 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 1277 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 1518 | 8 |
| `Quant/src/api/WebSocketClient.cpp` | 1672 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 1721 | 6 |
| `Quant/src/api/WebSocketClient.cpp` | 1775 | 6 |
| `Quant/src/core/Engine.cpp` | 71 | 5 |
| `Quant/src/core/Engine.cpp` | 115 | 11 |
| `Quant/src/core/Engine.cpp` | 330 | 4 |
| `Quant/src/core/Engine.cpp` | 357 | 5 |
| `Quant/src/core/Engine.cpp` | 377 | 4 |
| `Quant/src/core/Engine.cpp` | 444 | 5 |
| `Quant/src/core/Engine.cpp` | 532 | 5 |
| `Quant/src/core/Engine.cpp` | 560 | 4 |
| `Quant/src/core/Engine.cpp` | 588 | 8 |
| `Quant/src/core/Engine.cpp` | 617 | 5 |
| `Quant/src/core/Engine.cpp` | 797 | 4 |
| `Quant/src/core/Engine.cpp` | 993 | 5 |
| `Quant/src/core/Engine.cpp` | 1064 | 4 |
| `Quant/src/core/Engine.cpp` | 1074 | 5 |
| `Quant/src/core/Engine.cpp` | 1119 | 5 |
| `Quant/src/core/Engine.cpp` | 1252 | 4 |
| `Quant/src/core/Engine.cpp` | 1296 | 5 |
| `Quant/src/core/Engine.cpp` | 1343 | 7 |
| `Quant/src/core/Engine.cpp` | 1473 | 4 |
| `Quant/src/core/Engine.cpp` | 1531 | 7 |
| `Quant/src/core/Engine.cpp` | 1569 | 4 |
| `Quant/src/core/Engine.cpp` | 1594 | 5 |
| `Quant/src/core/Engine.cpp` | 1818 | 5 |
| `Quant/src/core/Engine.cpp` | 1865 | 4 |
| `Quant/src/core/Engine.cpp` | 1880 | 5 |
| `Quant/src/core/Engine.cpp` | 1972 | 7 |
| `Quant/src/core/RegimeController.cpp` | 59 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 69 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 80 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 226 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 233 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 446 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 658 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 846 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 965 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 1099 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1115 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 1130 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1168 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1238 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1280 | 13 |
| `Quant/src/modes/Monitors.cpp` | 264 | 6 |
| `Quant/src/modes/Monitors.cpp` | 706 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 47 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 61 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 365 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 420 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 443 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 465 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 537 | 6 |
| `Quant/src/risk/OrderGate.cpp` | 633 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 813 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 1161 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 28 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 399 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 484 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 495 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 576 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 613 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 639 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 693 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 806 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 24 | 11 |
| `Quant/src/universe/UniverseScanner.cpp` | 48 | 5 |
| `Quant/src/universe/UniverseScanner.cpp` | 198 | 7 |
| `Quant/src/universe/UniverseScanner.cpp` | 530 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 672 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 926 | 4 |
| `Quant/src/universe/UniverseScanner.cpp` | 1258 | 4 |
| `PYQuant/data/datagokr_source.py` | 212 | 4 |
| `PYQuant/kis/client.py` | 544 | 4 |
| `PYQuant/live/forward_trader.py` | 193 | 5 |
| `PYQuant/live/forward_trader.py` | 219 | 4 |
| `PYQuant/tools/log_report.py` | 44 | 4 |
| `PYQuant/tools/macro_regime_feed.py` | 49 | 6 |
| `PYQuant/tools/macro_regime_feed.py` | 73 | 6 |
| `PYQuant/tools/universe_feed.py` | 115 | 4 |
| `scripts/dashboard_server.py` | 48 | 6 |
| `scripts/dashboard_server.py` | 351 | 6 |
| `scripts/dashboard_server.py` | 507 | 6 |

## 5. settings.json 훅 배선

| 이벤트 | 훅 경로 | 실재 |
|---|---|---|
| `PreToolUse` | `.claude/hooks/secret-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/docs-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/lexicon-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/output-gate.ps1` | 있음 |
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
| `output-gate.ps1` | 됨 | 예 |
| `review-reminder.ps1` | 됨 | 예 |
| `secret-gate.ps1` | 됨 | 예 |

`settings.json` BOM: 아니오

## 6. `.claude/` 변경(해시 매니페스트)

파일 51개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 2026-09-09

- 추가 0개
- 삭제 0개
- 변경 0개
