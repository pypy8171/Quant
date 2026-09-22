<!-- drift-check: snapshot 2026-09-22 -->

# 주간 유지관리 리포트 — 2026-09-22

빨간 항목: 5

`py ../quant-devtools/maintain.py --weekly`가 만든 스냅샷. `.claude/`는 읽기만 했다. 고칠 항목은 사람이 승인한다.

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

빌드 트리·캐시·가상환경·데이터·스터디 산출물 중 100MB 이상이거나 30일 넘게 손대지 않은 폴더. 전부 gitignore 대상이라 저장소 크기와는 무관하고, 지워도 재빌드·재수집으로 돌아온다.

| 폴더 | 크기 | 마지막 수정 | 비고 |
|---|---|---|---|
| `PYQuant/data/cache` | 699.6 MB | 2026-09-20 | 100MB 이상 |
| `Quant/build_win` | 573.1 MB | 2026-09-22 | 100MB 이상 |
| `PYQuant/.venv` | 515.3 MB | 2026-09-08 | 100MB 이상 |
| `PYQuant/data/minute` | 472.9 MB | 2026-09-21 | 100MB 이상 |
| `PYQuant/.venv-win` | 439.6 MB | 2026-09-22 | 100MB 이상 |
| `out/build/x64-release` | 434.9 MB | 2026-09-22 | 100MB 이상 |
| `research/studies/24_devscale_exit_lines` | 392.1 MB | 2026-09-21 | 100MB 이상 |
| `research/studies/15_impulse_pullback` | 277.6 MB | 2026-09-19 | 100MB 이상 |
| `research/studies/12_base_breakout` | 195.5 MB | 2026-09-19 | 100MB 이상 |
| `research/studies/raw` | 0.3 MB | 2026-08-09 | 43일째 그대로 |
| **합계** | **4000.9 MB** | | 10개 |

## 4. 주석 밀도

게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.

| 파일 | 주석줄 | 전체줄 | 비율 |
|---|---|---|---|
| `Quant/src/strategy/MACrossStrategy.cpp` | 1 | 1 | 100% |
| `Quant/src/strategy/MomentumStrategy.cpp` | 1 | 1 | 100% |
| `Quant/src/strategy/StrategyBase.cpp` | 1 | 1 | 100% |
| `Quant/src/utils/Timer.cpp` | 1 | 1 | 100% |
| `Quant/include/ipc/OrderRouter.h` | 108 | 262 | 41% |
| `Quant/include/modes/Monitors.h` | 9 | 22 | 41% |
| `Quant/include/api/KisErrorCodes.h` | 6 | 15 | 40% |
| `Quant/src/api/WsSocket.h` | 14 | 37 | 38% |
| `Quant/include/api/KisResult.h` | 11 | 33 | 33% |
| `Quant/include/api/KisWebSocket.h` | 51 | 160 | 32% |
| `Quant/include/core/Engine.h` | 210 | 673 | 31% |
| `Quant/include/risk/OrderGate.h` | 202 | 651 | 31% |
| `scripts/log_patterns.py` | 6 | 21 | 29% |
| `Quant/include/api/KisClient.h` | 104 | 367 | 28% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/universe/UniverseScanner.h` | 29 | 109 | 27% |
| `Quant/include/api/IOrderExecutor.h` | 19 | 75 | 25% |
| `Quant/include/core/SignalDispatcher.h` | 28 | 119 | 24% |
| `Quant/include/strategy/StrategyBase.h` | 61 | 265 | 23% |
| `Quant/include/universe/MaAlign.h` | 12 | 53 | 23% |
| `Quant/include/api/KisTypes.h` | 8 | 36 | 22% |
| `Quant/include/core/BarAggregator.h` | 25 | 115 | 22% |
| `Quant/include/core/AppConfig.h` | 16 | 74 | 22% |
| `Quant/include/utils/JsonNode.h` | 5 | 24 | 21% |
| `Quant/include/utils/Logger.h` | 24 | 117 | 21% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 335 | 1658 | 20% |
| `Quant/include/ipc/ZmqBridge.h` | 29 | 149 | 19% |
| `Quant/include/core/Types.h` | 89 | 467 | 19% |
| `Quant/include/api/IMarketDataSource.h` | 8 | 42 | 19% |
| `Quant/include/strategy/TargetBasketPlan.h` | 19 | 102 | 19% |
| `Quant/include/core/DataPoller.h` | 21 | 114 | 18% |
| `Quant/include/core/IFeedSource.h` | 12 | 67 | 18% |
| `Quant/include/core/LedgerReconciler.h` | 36 | 202 | 18% |
| `Quant/include/core/RingBuffer.h` | 26 | 147 | 18% |
| `Quant/include/universe/ScoreWeight.h` | 29 | 173 | 17% |
| `Quant/include/risk/GateReasons.h` | 5 | 30 | 17% |
| `Quant/include/api/KisEndpoints.h` | 5 | 31 | 16% |
| `Quant/include/core/UniverseExit.h` | 15 | 94 | 16% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 34 | 218 | 16% |
| `Quant/include/utils/EtfFilter.h` | 24 | 155 | 15% |
| `Quant/include/core/MpscQueue.h` | 24 | 157 | 15% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 83 | 566 | 15% |
| `Quant/include/core/SessionEndJudge.h` | 12 | 86 | 14% |
| `Quant/src/core/LedgerReconciler.cpp` | 58 | 420 | 14% |
| `Quant/include/strategy/DevScaleRules.h` | 15 | 111 | 14% |
| `Quant/src/api/KisClientInternal.h` | 4 | 30 | 13% |
| `PYQuant/tools/macro_regime_feed.py` | 89 | 675 | 13% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 285 | 13% |
| `Quant/include/ipc/OpsServer.h` | 17 | 135 | 13% |
| `Quant/include/core/TickSize.h` | 6 | 48 | 12% |
| `Quant/include/core/WakeGate.h` | 15 | 121 | 12% |
| `Quant/src/ipc/OrderRouter.cpp` | 237 | 1948 | 12% |
| `Quant/include/utils/Utf8.h` | 8 | 66 | 12% |
| `Quant/src/main.cpp` | 40 | 332 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/include/core/KstTime.h` | 14 | 119 | 12% |
| `Quant/src/api/KisAccount.cpp` | 23 | 196 | 12% |
| `Quant/src/core/OrderRateLimiter.cpp` | 13 | 111 | 12% |
| `Quant/include/api/KisWsDecode.h` | 43 | 368 | 12% |
| `Quant/src/strategy/StrategyFactory.cpp` | 142 | 1243 | 11% |
| `Quant/include/ipc/OpsProtocol.h` | 19 | 168 | 11% |
| `Quant/include/core/OrderRateLimiter.h` | 13 | 115 | 11% |
| `Quant/include/strategy/MACrossStrategy.h` | 16 | 143 | 11% |
| `Quant/src/api/KisTransport.cpp` | 70 | 629 | 11% |
| `Quant/include/utils/ThreadName.h` | 3 | 27 | 11% |
| `Quant/src/core/EngineConfigure.cpp` | 12 | 110 | 11% |
| `Quant/src/api/KisAuth.cpp` | 24 | 221 | 11% |
| `Quant/include/core/MarketSession.h` | 9 | 83 | 11% |
| `Quant/include/core/RegimeFileJudge.h` | 34 | 314 | 11% |
| `Quant/include/core/SymbolTable.h` | 35 | 324 | 11% |
| `Quant/src/universe/UniverseScanner.cpp` | 177 | 1645 | 11% |
| `Quant/include/strategy/MomentumStrategy.h` | 12 | 112 | 11% |
| `Quant/src/risk/OrderGate.cpp` | 201 | 1877 | 11% |
| `Quant/include/strategy/ThemeStrategy.h` | 39 | 367 | 11% |
| `Quant/include/core/ShardMatrix.h` | 17 | 160 | 11% |
| `Quant/src/core/Engine.cpp` | 325 | 3088 | 11% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/include/core/StrategyTable.h` | 11 | 107 | 10% |
| `Quant/include/core/ReconcilePlan.h` | 12 | 120 | 10% |
| `Quant/include/api/KisRestDecode.h` | 35 | 359 | 10% |
| `Quant/include/strategy/TargetBasketStrategy.h` | 9 | 93 | 10% |
| `Quant/include/core/MutexQueue.h` | 8 | 85 | 9% |
| `Quant/src/api/KisUniverse.cpp` | 63 | 675 | 9% |
| `Quant/include/core/ShardRoutes.h` | 13 | 140 | 9% |
| `Quant/src/api/WebSocketClient.cpp` | 87 | 940 | 9% |
| `Quant/include/core/StrategyShard.h` | 17 | 187 | 9% |
| `Quant/include/risk/ReservationJournal.h` | 13 | 144 | 9% |
| `Quant/include/ipc/FillKey.h` | 3 | 34 | 9% |
| `scripts/parse_quant_log.py` | 29 | 331 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 44 | 527 | 8% |
| `PYQuant/dashboard/backfill_live.py` | 15 | 180 | 8% |
| `Quant/include/strategy/SeedPeakStore.h` | 12 | 146 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `Quant/include/core/FeedSupervisor.h` | 7 | 88 | 8% |
| `scripts/market_close_collect.py` | 21 | 268 | 8% |
| `Quant/include/core/StrategyRouter.h` | 9 | 116 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `Quant/src/core/SignalDispatcher.cpp` | 27 | 356 | 8% |
| `PYQuant/tools/universe_feed.py` | 19 | 251 | 8% |
| `Quant/src/api/KisOrder.cpp` | 30 | 398 | 8% |
| `Quant/src/ipc/ZmqBridge.cpp` | 30 | 403 | 7% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 9 | 121 | 7% |
| `Quant/src/modes/Monitors.cpp` | 65 | 889 | 7% |
| `PYQuant/data/datagokr_source.py` | 26 | 360 | 7% |
| `Quant/src/core/AppConfig.cpp` | 17 | 237 | 7% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `PYQuant/backtest/engine.py` | 39 | 562 | 7% |
| `Quant/src/utils/Logger.cpp` | 28 | 407 | 7% |
| `Quant/src/api/KisMarket.cpp` | 47 | 687 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `Quant/include/strategy/PriceTargetStrategy.h` | 20 | 302 | 7% |
| `scripts/market_close_minute_backfill.py` | 5 | 79 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `scripts/check_runtime_health.py` | 34 | 590 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `Quant/src/api/WsSocketPosix.cpp` | 28 | 507 | 6% |
| `Quant/include/core/LatencyTrace.h` | 13 | 240 | 5% |
| `PYQuant/backtest/costs.py` | 8 | 149 | 5% |
| `Quant/include/core/ReplaySource.h` | 13 | 246 | 5% |
| `Quant/include/core/FeedMux.h` | 27 | 512 | 5% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `scripts/dashboard_server.py` | 100 | 1935 | 5% |
| `scripts/market_close_autodoc.py` | 27 | 528 | 5% |
| `PYQuant/core/proc_watch.py` | 16 | 315 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `Quant/src/api/KisIndex.cpp` | 23 | 468 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `PYQuant/tools/month_start_sweep.py` | 5 | 103 | 5% |
| `scripts/analyze_slot_cost.py` | 7 | 147 | 5% |
| `scripts/premarket_routine.py` | 4 | 86 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `PYQuant/main.py` | 33 | 740 | 4% |
| `Quant/include/core/PaperExecutor.h` | 19 | 432 | 4% |
| `PYQuant/backtest/metrics.py` | 8 | 184 | 4% |
| `PYQuant/tests/test_stats.py` | 12 | 278 | 4% |
| `PYQuant/kis/client.py` | 36 | 907 | 4% |
| `Quant/include/core/TickCapture.h` | 18 | 466 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `Quant/src/api/WsSocketWin.cpp` | 13 | 378 | 3% |
| `PYQuant/tests/test_costs_golden.py` | 6 | 176 | 3% |
| `scripts/notify_trades.py` | 21 | 653 | 3% |
| `PYQuant/dashboard/build_dashboard.py` | 55 | 1742 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `scripts/gen_tuning_sheet.py` | 15 | 485 | 3% |
| `PYQuant/tools/check_kis_investor.py` | 2 | 67 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `PYQuant/tests/test_point_in_time.py` | 2 | 71 | 3% |
| `PYQuant/backtest/devscale_replay_rescue.py` | 20 | 760 | 3% |
| `scripts/refresh_dashboard.py` | 5 | 193 | 3% |
| `scripts/build_review_entry.py` | 7 | 274 | 3% |
| `Quant/src/core/BarAggregator.cpp` | 11 | 434 | 3% |
| `PYQuant/tools/bench_market_open.py` | 11 | 448 | 2% |
| `PYQuant/db/client.py` | 18 | 746 | 2% |
| `PYQuant/tools/check_investor_api.py` | 4 | 176 | 2% |
| `PYQuant/backtest/devscale_replay.py` | 16 | 705 | 2% |
| `scripts/extract_swap_what_if.py` | 5 | 227 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 228 | 2% |
| `scripts/exit_ev.py` | 9 | 436 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 585 | 2% |
| `scripts/live_prices_feed.py` | 2 | 100 | 2% |
| `PYQuant/tools/sweep.py` | 2 | 103 | 2% |
| `PYQuant/backtest/stats.py` | 10 | 519 | 2% |
| `Quant/src/core/DataPoller.cpp` | 3 | 161 | 2% |
| `PYQuant/ipc/subscriber.py` | 2 | 108 | 2% |
| `PYQuant/tools/index_intraday_logger.py` | 4 | 216 | 2% |
| `PYQuant/strategy/supply_demand_rank.py` | 1 | 55 | 2% |
| `PYQuant/tools/full_universe_dump.py` | 2 | 111 | 2% |
| `PYQuant/strategy/channel_breakout.py` | 1 | 56 | 2% |
| `PYQuant/tests/test_adjust_splits.py` | 1 | 57 | 2% |
| `PYQuant/features/regime_axes.py` | 11 | 635 | 2% |
| `PYQuant/data/point_in_time.py` | 1 | 58 | 2% |
| `PYQuant/tools/minute_backfill_pairs.py` | 1 | 63 | 2% |
| `PYQuant/live/basket_forward.py` | 5 | 330 | 2% |
| `PYQuant/tools/naver_research_fetch.py` | 9 | 595 | 2% |
| `scripts/trade_costs.py` | 2 | 136 | 1% |
| `Quant/src/ipc/OpsServer.cpp` | 10 | 685 | 1% |
| `PYQuant/tools/check_adjusted.py` | 1 | 69 | 1% |
| `scripts/backfill_fills_db.py` | 2 | 141 | 1% |
| `PYQuant/tools/naver_flow_backfill.py` | 4 | 283 | 1% |
| `PYQuant/tools/investor_flow_logger.py` | 3 | 213 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/check_pykrx_flow.py` | 2 | 150 | 1% |
| `Quant/src/strategy/TargetBasketPlan.cpp` | 4 | 311 | 1% |
| `PYQuant/tools/macro_ingest.py` | 7 | 550 | 1% |
| `scripts/deploy_guard.py` | 1 | 80 | 1% |
| `PYQuant/backtest/ledger.py` | 1 | 82 | 1% |
| `scripts/exit_ev_dashboard.py` | 7 | 587 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
| `PYQuant/tools/naver_bars_backfill.py` | 3 | 280 | 1% |
| `PYQuant/tools/dart_fin_history_fill.py` | 5 | 482 | 1% |
| `Quant/src/strategy/TargetBasketStrategy.cpp` | 4 | 436 | 1% |
| `PYQuant/naver/theme.py` | 1 | 121 | 1% |
| `PYQuant/tests/test_metrics.py` | 1 | 139 | 1% |
| `scripts/_logdir.py` | 1 | 155 | 1% |
| `PYQuant/tools/kind_delisted_fill.py` | 2 | 313 | 1% |
| `PYQuant/tools/dart_shares_history_fill.py` | 2 | 328 | 1% |
| `scripts/summarize_trading_day.py` | 1 | 216 | 0% |
| `scripts/build_study_site.py` | 2 | 483 | 0% |
| `PYQuant/tools/compare_ws_bars.py` | 1 | 243 | 0% |
| `PYQuant/features/fundamental.py` | 1 | 275 | 0% |
| `PYQuant/core/logger.py` | 0 | 51 | 0% |
| `PYQuant/data/asof.py` | 0 | 8 | 0% |
| `PYQuant/data/keys.py` | 0 | 28 | 0% |
| `PYQuant/features/__init__.py` | 0 | 1 | 0% |
| `PYQuant/kis/endpoints.py` | 0 | 20 | 0% |
| `PYQuant/live/trader.py` | 0 | 73 | 0% |
| `PYQuant/strategy/indicators.py` | 0 | 34 | 0% |
| `PYQuant/tests/test_regime_axes.py` | 0 | 94 | 0% |
| `PYQuant/tools/check_datagokr.py` | 0 | 53 | 0% |
| `PYQuant/tools/check_market_flow.py` | 0 | 67 | 0% |
| `PYQuant/tools/check_pykrx.py` | 0 | 28 | 0% |
| `PYQuant/tools/check_sector_index.py` | 0 | 69 | 0% |
| `PYQuant/tools/fetch_naver_themes.py` | 0 | 66 | 0% |
| `PYQuant/tools/fullperiod_validate.py` | 0 | 56 | 0% |
| `PYQuant/tools/nxt_divergence_check.py` | 0 | 138 | 0% |
| `PYQuant/tools/regime_removal_test_2022.py` | 0 | 68 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 227개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/KisClient.h` | 137 | 7 |
| `Quant/include/api/KisClient.h` | 153 | 4 |
| `Quant/include/api/KisClient.h` | 186 | 9 |
| `Quant/include/api/KisClient.h` | 211 | 4 |
| `Quant/include/api/KisClient.h` | 234 | 4 |
| `Quant/include/api/KisClient.h` | 250 | 5 |
| `Quant/include/api/KisClient.h` | 340 | 4 |
| `Quant/include/api/KisClient.h` | 353 | 4 |
| `Quant/include/api/KisEndpoints.h` | 2 | 4 |
| `Quant/include/api/KisWebSocket.h` | 50 | 9 |
| `Quant/include/api/KisWsDecode.h` | 51 | 4 |
| `Quant/include/api/KisWsDecode.h` | 115 | 5 |
| `Quant/include/core/AppConfig.h` | 2 | 4 |
| `Quant/include/core/Engine.h` | 49 | 10 |
| `Quant/include/core/Engine.h` | 88 | 5 |
| `Quant/include/core/Engine.h` | 138 | 5 |
| `Quant/include/core/Engine.h` | 184 | 4 |
| `Quant/include/core/Engine.h` | 261 | 5 |
| `Quant/include/core/Engine.h` | 293 | 8 |
| `Quant/include/core/Engine.h` | 377 | 4 |
| `Quant/include/core/Engine.h` | 423 | 4 |
| `Quant/include/core/Engine.h` | 454 | 4 |
| `Quant/include/core/Engine.h` | 494 | 4 |
| `Quant/include/core/Engine.h` | 650 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 158 | 6 |
| `Quant/include/core/MpscQueue.h` | 10 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/PaperExecutor.h` | 160 | 4 |
| `Quant/include/core/ReconcilePlan.h` | 46 | 5 |
| `Quant/include/core/TickCapture.h` | 26 | 4 |
| `Quant/include/core/Types.h` | 86 | 4 |
| `Quant/include/core/Types.h` | 276 | 5 |
| `Quant/include/core/Types.h` | 386 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 22 | 15 |
| `Quant/include/ipc/OrderRouter.h` | 63 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 72 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 101 | 12 |
| `Quant/include/ipc/OrderRouter.h` | 135 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 158 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 178 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 232 | 4 |
| `Quant/include/modes/Monitors.h` | 7 | 5 |
| `Quant/include/risk/OrderGate.h` | 55 | 4 |
| `Quant/include/risk/OrderGate.h` | 158 | 10 |
| `Quant/include/risk/OrderGate.h` | 170 | 5 |
| `Quant/include/risk/OrderGate.h` | 183 | 4 |
| `Quant/include/risk/OrderGate.h` | 199 | 4 |
| `Quant/include/risk/OrderGate.h` | 206 | 5 |
| `Quant/include/risk/OrderGate.h` | 223 | 4 |
| `Quant/include/risk/OrderGate.h` | 368 | 6 |
| `Quant/include/risk/OrderGate.h` | 387 | 5 |
| `Quant/include/risk/OrderGate.h` | 394 | 6 |
| `Quant/include/risk/OrderGate.h` | 408 | 4 |
| `Quant/include/risk/OrderGate.h` | 425 | 4 |
| `Quant/include/risk/OrderGate.h` | 430 | 4 |
| `Quant/include/risk/OrderGate.h` | 449 | 4 |
| `Quant/include/risk/OrderGate.h` | 583 | 4 |
| `Quant/include/strategy/DevScaleRules.h` | 2 | 7 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 26 | 36 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 74 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 89 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 105 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 489 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 564 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 724 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 734 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 758 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 845 | 11 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 875 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 931 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 945 | 8 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 965 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 971 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1058 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1149 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1158 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1255 | 6 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1415 | 9 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 1483 | 5 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 11 | 23 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 310 | 11 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 432 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 14 | 4 |
| `Quant/include/strategy/MarketMakingStrategy.h` | 10 | 21 |
| `Quant/include/strategy/MomentumStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MomentumStrategy.h` | 52 | 4 |
| `Quant/include/strategy/PriceTargetStrategy.h` | 9 | 10 |
| `Quant/include/strategy/StrategyBase.h` | 41 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 66 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 127 | 6 |
| `Quant/include/strategy/StrategyBase.h` | 168 | 4 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 27 | 18 |
| `Quant/include/strategy/TargetBasketPlan.h` | 2 | 9 |
| `Quant/include/strategy/ThemeStrategy.h` | 14 | 14 |
| `Quant/include/strategy/ThemeStrategy.h` | 54 | 4 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 20 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 4 |
| `Quant/include/universe/ScoreWeight.h` | 62 | 19 |
| `Quant/include/universe/UniverseScanner.h` | 9 | 6 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 23 | 4 |
| `Quant/include/utils/EtfFilter.h` | 45 | 4 |
| `Quant/include/utils/EtfFilter.h` | 106 | 5 |
| `Quant/include/utils/Utf8.h` | 4 | 8 |
| `Quant/src/main.cpp` | 78 | 4 |
| `Quant/src/main.cpp` | 166 | 4 |
| `Quant/src/main.cpp` | 177 | 5 |
| `Quant/src/main.cpp` | 272 | 5 |
| `Quant/src/api/KisAccount.cpp` | 13 | 4 |
| `Quant/src/api/KisAccount.cpp` | 101 | 6 |
| `Quant/src/api/KisMarket.cpp` | 314 | 4 |
| `Quant/src/api/KisMarket.cpp` | 493 | 4 |
| `Quant/src/api/KisMarket.cpp` | 590 | 5 |
| `Quant/src/api/KisOrder.cpp` | 7 | 4 |
| `Quant/src/api/KisOrder.cpp` | 148 | 4 |
| `Quant/src/api/KisOrder.cpp` | 223 | 4 |
| `Quant/src/api/KisOrder.cpp` | 340 | 5 |
| `Quant/src/api/KisTransport.cpp` | 8 | 4 |
| `Quant/src/api/KisTransport.cpp` | 124 | 6 |
| `Quant/src/api/KisTransport.cpp` | 309 | 5 |
| `Quant/src/api/KisTransport.cpp` | 487 | 4 |
| `Quant/src/api/KisUniverse.cpp` | 5 | 6 |
| `Quant/src/api/KisUniverse.cpp` | 90 | 4 |
| `Quant/src/api/KisUniverse.cpp` | 219 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 357 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 531 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 172 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 205 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 474 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 200 | 5 |
| `Quant/src/core/Engine.cpp` | 93 | 5 |
| `Quant/src/core/Engine.cpp` | 795 | 4 |
| `Quant/src/core/Engine.cpp` | 840 | 5 |
| `Quant/src/core/Engine.cpp` | 907 | 4 |
| `Quant/src/core/Engine.cpp` | 986 | 5 |
| `Quant/src/core/Engine.cpp` | 1526 | 5 |
| `Quant/src/core/Engine.cpp` | 1542 | 4 |
| `Quant/src/core/Engine.cpp` | 1580 | 5 |
| `Quant/src/core/Engine.cpp` | 1658 | 4 |
| `Quant/src/core/Engine.cpp` | 1668 | 5 |
| `Quant/src/core/Engine.cpp` | 1837 | 4 |
| `Quant/src/core/Engine.cpp` | 1915 | 5 |
| `Quant/src/core/Engine.cpp` | 2634 | 7 |
| `Quant/src/core/LedgerReconciler.cpp` | 16 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 54 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 96 | 5 |
| `Quant/src/core/LedgerReconciler.cpp` | 197 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 232 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 267 | 6 |
| `Quant/src/core/SignalDispatcher.cpp` | 185 | 4 |
| `Quant/src/core/SignalDispatcher.cpp` | 205 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 49 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 79 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 91 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 120 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 363 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 374 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 556 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 761 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 950 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1019 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1052 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 1384 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 1566 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 1696 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1704 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 1719 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1749 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1813 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1856 | 13 |
| `Quant/src/modes/Monitors.cpp` | 36 | 4 |
| `Quant/src/modes/Monitors.cpp` | 268 | 6 |
| `Quant/src/modes/Monitors.cpp` | 696 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 50 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 64 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 324 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 443 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 503 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 512 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 529 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 548 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 735 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 1187 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 1405 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 35 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 432 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 454 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 573 | 7 |
| `Quant/src/strategy/StrategyFactory.cpp` | 706 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 765 | 4 |
| `Quant/src/strategy/StrategyFactory.cpp` | 797 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 868 | 6 |
| `Quant/src/strategy/StrategyFactory.cpp` | 1016 | 5 |
| `Quant/src/strategy/StrategyFactory.cpp` | 1044 | 5 |
| `Quant/src/universe/UniverseScanner.cpp` | 27 | 11 |
| `Quant/src/universe/UniverseScanner.cpp` | 227 | 8 |
| `Quant/src/universe/UniverseScanner.cpp` | 446 | 4 |
| (이하 27개 생략) | | |

## 5. settings.json 훅 배선

| 이벤트 | 훅 경로 | 실재 |
|---|---|---|
| `PreToolUse` | `.claude/hooks/secret-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/docs-gate.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/lexicon-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/output-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/sync-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/file-index-gate.ps1` | 있음 |
| `Stop` | `.claude/hooks/review-reminder.ps1` | 있음 |
| `Stop` | `.claude/hooks/dashboard-refresh.ps1` | 있음 |
| `Stop` | `.claude/hooks/handoff-due.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/resume-work.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/market-close-gate.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/cron-gate.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/handoff-list.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/session-board-server.ps1` | 있음 |
| `PreCompact` | `.claude/hooks/precompact-handoff.ps1` | 있음 |

| 훅 파일 | 배선 | BOM UTF-8 |
|---|---|---|
| `cron-gate.ps1` | 됨 | 예 |
| `dashboard-refresh.ps1` | 됨 | 예 |
| `docs-gate.ps1` | 됨 | 예 |
| `file-index-gate.ps1` | 됨 | 예 |
| `handoff-due.ps1` | 됨 | 예 |
| `handoff-list.ps1` | 됨 | 예 |
| `lexicon-gate.ps1` | 됨 | 예 |
| `market-close-gate.ps1` | 됨 | 예 |
| `output-gate.ps1` | 됨 | 예 |
| `precompact-handoff.ps1` | 됨 | 예 |
| `resume-work.ps1` | 됨 | 예 |
| `review-reminder.ps1` | 됨 | 예 |
| `secret-gate.ps1` | 됨 | 예 |
| `session-board-server.ps1` | 됨 | 예 |
| `sync-gate.ps1` | 됨 | 예 |

`settings.json` BOM: 아니오

## 6. `.claude/` 변경(해시 매니페스트)

파일 82개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 2026-09-22

- 추가 0개
- 삭제 0개
- 변경 1개: `.claude/sync-gate.state`
