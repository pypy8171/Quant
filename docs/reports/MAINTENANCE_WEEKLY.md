<!-- drift-check: snapshot 2026-09-25 -->

# 주간 유지관리 리포트 — 2026-09-25

빨간 항목: 17

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
- [!] `.claude/commands/doc-audit.md:13` → `Quant/src/core/Engine.cpp/.h`
- [!] `.claude/commands/doc-audit.md:60` → `docs/archive/`
- [!] `.claude/commands/doc-audit.md:80` → `Quant/build`
- [!] `.claude/commands/doc-audit.md:80` → `Quant/.claude`

## 3. 부산물 용량

빌드 트리·캐시·가상환경·데이터·스터디 산출물 중 100MB 이상이거나 30일 넘게 손대지 않은 폴더. 전부 gitignore 대상이라 저장소 크기와는 무관하고, 지워도 재빌드·재수집으로 돌아온다.

| 폴더 | 크기 | 마지막 수정 | 비고 |
|---|---|---|---|
| `.vs` | 5085.2 MB | 2026-09-25 | 100MB 이상 |
| `out/build/x64-debug` | 2462.9 MB | 2026-09-25 | 100MB 이상 |
| `PYQuant/data/cache` | 699.6 MB | 2026-09-20 | 100MB 이상 |
| `Quant/build_win` | 649.5 MB | 2026-09-25 | 100MB 이상 |
| `PYQuant/.venv` | 515.3 MB | 2026-09-08 | 100MB 이상 |
| `out/build/x64-release` | 490.9 MB | 2026-09-25 | 100MB 이상 |
| `PYQuant/.venv-win` | 481.5 MB | 2026-09-23 | 100MB 이상 |
| `PYQuant/data/minute` | 479.2 MB | 2026-09-23 | 100MB 이상 |
| `research/studies/24_devscale_exit_lines` | 392.1 MB | 2026-09-25 | 100MB 이상 |
| `research/studies/15_impulse_pullback` | 277.6 MB | 2026-09-19 | 100MB 이상 |
| `research/studies/12_base_breakout` | 195.5 MB | 2026-09-19 | 100MB 이상 |
| `PYQuant/data/ticks_raw` | 144.4 MB | 2026-09-24 | 100MB 이상 |
| `PYQuant/.index_cache` | 5.7 MB | 2026-08-25 | 31일째 그대로 |
| `research/studies/raw` | 0.3 MB | 2026-08-09 | 46일째 그대로 |
| `PYQuant/data/index_intraday` | 0.0 MB | 2026-08-25 | 31일째 그대로 |
| **합계** | **11879.7 MB** | | 15개 |

## 4. 주석 밀도

게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.

| 파일 | 주석줄 | 전체줄 | 비율 |
|---|---|---|---|
| `Quant/src/utils/Timer.cpp` | 1 | 1 | 100% |
| `Quant/include/universe/ScoreWeight.h` | 35 | 77 | 45% |
| `Quant/include/api/KisErrorCodes.h` | 8 | 18 | 44% |
| `Quant/include/utils/Utf8.h` | 16 | 36 | 44% |
| `Quant/include/ipc/OrderRouter.h` | 161 | 390 | 41% |
| `Quant/include/utils/EtfFilter.h` | 20 | 50 | 40% |
| `Quant/src/api/WsSocket.h` | 14 | 37 | 38% |
| `Quant/include/api/KisRestDecode.h` | 31 | 82 | 38% |
| `Quant/include/core/Engine.h` | 377 | 1026 | 37% |
| `Quant/include/strategy/DevScaleRules.h` | 15 | 42 | 36% |
| `Quant/include/utils/JsonNode.h` | 5 | 14 | 36% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 159 | 449 | 35% |
| `Quant/include/api/KisWebSocket.h` | 59 | 167 | 35% |
| `Quant/src/universe/detail/Pipeline.h` | 82 | 236 | 35% |
| `Quant/include/ipc/LedgerSnapshot.h` | 60 | 173 | 35% |
| `Quant/include/risk/PositionLedger.h` | 173 | 499 | 35% |
| `Quant/include/api/KisClient.h` | 116 | 344 | 34% |
| `Quant/include/api/KisResult.h` | 11 | 33 | 33% |
| `Quant/include/ipc/RegimeCell.h` | 8 | 24 | 33% |
| `Quant/include/universe/MaAlign.h` | 12 | 36 | 33% |
| `Quant/include/core/CommandLine.h` | 29 | 91 | 32% |
| `Quant/include/api/KisWsDecode.h` | 53 | 167 | 32% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 29 | 94 | 31% |
| `Quant/include/risk/OrderGate.h` | 107 | 347 | 31% |
| `Quant/include/risk/EntryPriority.h` | 31 | 105 | 30% |
| `Quant/include/ipc/OrderChannel.h` | 62 | 214 | 29% |
| `Quant/include/ipc/FillChannel.h` | 47 | 164 | 29% |
| `scripts/log_patterns.py` | 6 | 21 | 29% |
| `Quant/include/core/AppConfig.h` | 26 | 95 | 27% |
| `Quant/include/universe/UniverseScanner.h` | 35 | 128 | 27% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/ipc/ZmqBridge.h` | 56 | 208 | 27% |
| `Quant/include/api/HttpGet.h` | 4 | 15 | 27% |
| `Quant/include/strategy/SeedPeakStore.h` | 13 | 49 | 27% |
| `Quant/include/api/IOrderExecutor.h` | 20 | 76 | 26% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 25 | 95 | 26% |
| `Quant/include/ipc/MarketFeedChannel.h` | 41 | 156 | 26% |
| `Quant/include/core/SignalDispatcher.h` | 33 | 126 | 26% |
| `Quant/include/core/WebSocketSlotPlan.h` | 16 | 62 | 26% |
| `Quant/include/core/MarketSession.h` | 9 | 35 | 26% |
| `Quant/include/core/Types.h` | 100 | 390 | 26% |
| `Quant/include/core/DataPoller.h` | 24 | 94 | 26% |
| `Quant/include/strategy/StrategyBase.h` | 73 | 286 | 26% |
| `Quant/include/exchange/OrderWire.h` | 23 | 91 | 25% |
| `Quant/include/core/SessionEndJudge.h` | 12 | 48 | 25% |
| `Quant/include/ipc/ProcessIdentity.h` | 10 | 40 | 25% |
| `Quant/include/utils/Logger.h` | 26 | 104 | 25% |
| `Quant/include/ipc/SharedRegion.h` | 53 | 213 | 25% |
| `Quant/include/strategy/ThemeStrategy.h` | 26 | 106 | 25% |
| `Quant/include/core/LedgerReconciler.h` | 37 | 151 | 25% |
| `Quant/include/ipc/ControlChannel.h` | 36 | 152 | 24% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 40 | 169 | 24% |
| `Quant/include/core/IFeedSource.h` | 19 | 81 | 23% |
| `Quant/include/ipc/SharedSymbolDictionary.h` | 26 | 114 | 23% |
| `Quant/include/risk/ProtectiveRule.h` | 14 | 62 | 23% |
| `Quant/include/core/ControlPlane.h` | 27 | 120 | 22% |
| `Quant/include/exchange/ZmqOrderFeed.h` | 38 | 169 | 22% |
| `Quant/include/api/KisTypes.h` | 8 | 36 | 22% |
| `Quant/include/ipc/OpsProtocol.h` | 19 | 86 | 22% |
| `Quant/include/core/BarAggregator.h` | 25 | 115 | 22% |
| `Quant/include/core/SymbolTable.h` | 40 | 185 | 22% |
| `Quant/include/ipc/SharedStrategyDictionary.h` | 24 | 111 | 22% |
| `Quant/include/api/KisRateBucket.h` | 14 | 65 | 22% |
| `Quant/include/core/StrategyTable.h` | 28 | 132 | 21% |
| `Quant/include/ipc/SharedLayout.h` | 48 | 228 | 21% |
| `Quant/include/core/KstTime.h` | 18 | 86 | 21% |
| `Quant/include/ipc/Heartbeat.h` | 20 | 97 | 21% |
| `Quant/include/core/UniverseExit.h` | 15 | 73 | 21% |
| `Quant/include/core/RegimeFileJudge.h` | 27 | 132 | 20% |
| `Quant/include/core/ReconcilePlan.h` | 11 | 54 | 20% |
| `Quant/include/core/UniverseRescan.h` | 21 | 104 | 20% |
| `Quant/include/core/FeedMux.h` | 32 | 165 | 19% |
| `Quant/include/exchange/MatchingEngine.h` | 46 | 238 | 19% |
| `Quant/include/risk/GateReasons.h` | 5 | 26 | 19% |
| `Quant/include/api/IMarketDataSource.h` | 8 | 42 | 19% |
| `Quant/src/core/EngineStrategyThread.cpp` | 97 | 511 | 19% |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 29 | 153 | 19% |
| `Quant/src/core/EngineControlThread.cpp` | 73 | 389 | 19% |
| `Quant/include/strategy/TargetBasketPlan.h` | 19 | 102 | 19% |
| `Quant/include/strategy/MACrossStrategy.h` | 11 | 60 | 18% |
| `Quant/src/core/EngineLayout.cpp` | 39 | 213 | 18% |
| `Quant/include/core/RingBuffer.h` | 27 | 148 | 18% |
| `Quant/src/core/EngineOrderThread.cpp` | 85 | 471 | 18% |
| `Quant/src/core/EngineFillThread.cpp` | 18 | 101 | 18% |
| `Quant/include/core/HttpQuoteFeed.h` | 16 | 90 | 18% |
| `Quant/include/strategy/PriceTargetStrategy.h` | 14 | 82 | 17% |
| `Quant/include/risk/LedgerKeys.h` | 30 | 177 | 17% |
| `Quant/include/core/ShardRoutes.h` | 20 | 119 | 17% |
| `Quant/src/core/EngineDataThread.cpp` | 106 | 639 | 17% |
| `Quant/include/risk/LedgerJournal.h` | 36 | 222 | 16% |
| `Quant/include/ipc/SharedSpscRing.h` | 64 | 396 | 16% |
| `Quant/include/api/KisEndpoints.h` | 5 | 31 | 16% |
| `Quant/include/core/PaperExecutor.h` | 22 | 139 | 16% |
| `Quant/include/core/TickSize.h` | 6 | 38 | 16% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 9 | 57 | 16% |
| `Quant/include/utils/ThreadName.h` | 3 | 19 | 16% |
| `Quant/src/core/EngineRegime.cpp` | 41 | 266 | 15% |
| `Quant/include/core/MpscQueue.h` | 24 | 157 | 15% |
| `Quant/include/core/LatencyTrace.h` | 23 | 151 | 15% |
| `Quant/include/ipc/FillKey.h` | 4 | 27 | 15% |
| `Quant/include/risk/DisplacementDesk.h` | 9 | 61 | 15% |
| `Quant/src/core/EngineConfigure.cpp` | 21 | 146 | 14% |
| `Quant/src/main.cpp` | 50 | 351 | 14% |
| `Quant/include/ipc/OpsServer.h` | 19 | 140 | 14% |
| `Quant/src/api/KisAccount.cpp` | 31 | 229 | 14% |
| `Quant/src/risk/OrderGate.cpp` | 156 | 1164 | 13% |
| `Quant/include/core/WakeGate.h` | 15 | 112 | 13% |
| `Quant/src/api/KisClientInternal.h` | 4 | 30 | 13% |
| `Quant/src/core/Engine.cpp` | 119 | 899 | 13% |
| `PYQuant/tools/macro_regime_feed.py` | 89 | 675 | 13% |
| `Quant/include/core/PrefetchPool.h` | 32 | 244 | 13% |
| `Quant/src/core/OrderRateLimiter.cpp` | 17 | 130 | 13% |
| `Quant/src/universe/UniverseItb.cpp` | 17 | 131 | 13% |
| `Quant/include/core/OrderRateLimiter.h` | 14 | 108 | 13% |
| `Quant/src/core/EngineSymbols.cpp` | 34 | 263 | 13% |
| `Quant/src/api/KisUniverse.cpp` | 103 | 798 | 13% |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 200 | 1567 | 13% |
| `Quant/src/core/EngineControlPlane.cpp` | 23 | 182 | 13% |
| `Quant/src/api/KisTransport.cpp` | 91 | 740 | 12% |
| `Quant/src/strategy/StrategyFactory.cpp` | 159 | 1297 | 12% |
| `Quant/src/api/KisAuth.cpp` | 32 | 262 | 12% |
| `Quant/include/core/TickCapture.h` | 30 | 250 | 12% |
| `Quant/include/ipc/SharedWriteLock.h` | 3 | 25 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `Quant/src/core/LedgerReconciler.cpp` | 59 | 495 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/src/core/EngineFeed.cpp` | 107 | 904 | 12% |
| `Quant/include/core/ShardMatrix.h` | 19 | 162 | 12% |
| `Quant/src/ipc/OrderRouter.cpp` | 322 | 2746 | 12% |
| `Quant/src/strategy/MomentumStrategy.cpp` | 8 | 69 | 12% |
| `Quant/src/api/WebSocketClient.cpp` | 58 | 504 | 12% |
| `Quant/src/ipc/ZmqBridge.cpp` | 59 | 522 | 11% |
| `Quant/include/core/FeedSupervisor.h` | 7 | 62 | 11% |
| `Quant/src/core/EngineUniverse.cpp` | 22 | 201 | 11% |
| `Quant/src/core/EngineOpsServer.cpp` | 32 | 297 | 11% |
| `Quant/src/strategy/IntradayBreakoutStrategy.cpp` | 43 | 412 | 10% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/src/api/KisOrder.cpp` | 56 | 556 | 10% |
| `Quant/src/risk/LedgerKeys.cpp` | 2 | 20 | 10% |
| `Quant/src/core/AppConfig.cpp` | 29 | 292 | 10% |
| `Quant/include/core/StrategyShard.h` | 18 | 183 | 10% |
| `Quant/include/strategy/TargetBasketStrategy.h` | 9 | 93 | 10% |
| `Quant/include/strategy/MomentumStrategy.h` | 5 | 52 | 10% |
| `Quant/include/core/MutexQueue.h` | 8 | 85 | 9% |
| `Quant/include/risk/ProtectiveOrders.h` | 12 | 128 | 9% |
| `scripts/check_runtime_health.py` | 197 | 2102 | 9% |
| `Quant/src/core/SignalDispatcher.cpp` | 27 | 293 | 9% |
| `scripts/parse_quant_log.py` | 31 | 339 | 9% |
| `Quant/src/universe/UniverseRiskGate.cpp` | 14 | 157 | 9% |
| `Quant/src/ipc/LedgerSnapshot.cpp` | 17 | 192 | 9% |
| `Quant/src/universe/UniverseQuotes.cpp` | 15 | 170 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/src/api/KisWebSocketParse.cpp` | 42 | 499 | 8% |
| `PYQuant/dashboard/backfill_live.py` | 15 | 180 | 8% |
| `Quant/src/utils/Logger.cpp` | 37 | 458 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `scripts/market_close_collect.py` | 21 | 268 | 8% |
| `scripts/make_load_test_config.py` | 11 | 141 | 8% |
| `Quant/include/core/StrategyRouter.h` | 9 | 116 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `Quant/src/core/ControlPlane.cpp` | 21 | 277 | 8% |
| `PYQuant/tools/universe_feed.py` | 19 | 251 | 8% |
| `Quant/src/universe/UniverseCandidates.cpp` | 33 | 436 | 8% |
| `Quant/src/risk/DisplacementDesk.cpp` | 11 | 150 | 7% |
| `PYQuant/data/datagokr_source.py` | 26 | 360 | 7% |
| `Quant/include/core/ReplaySource.h` | 10 | 139 | 7% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `PYQuant/backtest/engine.py` | 39 | 562 | 7% |
| `Quant/src/api/KisMarket.cpp` | 47 | 687 | 7% |
| `Quant/src/universe/UniverseScanner.cpp` | 8 | 117 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `Quant/src/strategy/MarketMakingStrategy.cpp` | 9 | 136 | 7% |
| `Quant/src/ipc/OrderChannel.cpp` | 24 | 377 | 6% |
| `scripts/market_close_minute_backfill.py` | 5 | 79 | 6% |
| `PYQuant/tools/load_highwater_reader.py` | 16 | 253 | 6% |
| `Quant/src/core/SymbolTable.cpp` | 16 | 256 | 6% |
| `Quant/src/risk/PositionLedger.cpp` | 82 | 1313 | 6% |
| `Quant/src/ipc/ProcessIdentity.cpp` | 10 | 162 | 6% |
| `Quant/src/strategy/MACrossStrategy.cpp` | 6 | 98 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `scripts/market_close_autodoc.py` | 32 | 541 | 6% |
| `Quant/src/universe/UniverseFeatures.cpp` | 30 | 510 | 6% |
| `Quant/src/ipc/SharedLayout.cpp` | 22 | 382 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `Quant/src/ipc/ControlChannel.cpp` | 7 | 126 | 6% |
| `Quant/src/api/WsSocketPosix.cpp` | 28 | 507 | 6% |
| `Quant/src/core/UniverseRescan.cpp` | 22 | 402 | 5% |
| `Quant/src/core/HttpQuoteFeed.cpp` | 18 | 333 | 5% |
| `scripts/dashboard_server.py` | 108 | 2010 | 5% |
| `PYQuant/backtest/costs.py` | 8 | 149 | 5% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `Quant/src/universe/UniverseScoring.cpp` | 10 | 194 | 5% |
| `Quant/src/risk/EntryPriority.cpp` | 11 | 216 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `PYQuant/core/proc_watch.py` | 16 | 318 | 5% |
| `PYQuant/main.py` | 38 | 762 | 5% |
| `Quant/src/ipc/SharedRegion.cpp` | 31 | 623 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `Quant/src/api/KisIndex.cpp` | 23 | 468 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `PYQuant/tools/month_start_sweep.py` | 5 | 103 | 5% |
| `Quant/src/api/KisClient.cpp` | 3 | 62 | 5% |
| `Quant/src/core/KstTime.cpp` | 5 | 105 | 5% |
| `Quant/src/strategy/ValueContraryStrategy.cpp` | 11 | 231 | 5% |
| `scripts/analyze_slot_cost.py` | 7 | 147 | 5% |
| `PYQuant/kis/client.py` | 47 | 992 | 5% |
| `scripts/premarket_routine.py` | 4 | 86 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `Quant/src/ipc/Heartbeat.cpp` | 4 | 90 | 4% |
| `PYQuant/backtest/metrics.py` | 8 | 184 | 4% |
| `PYQuant/tests/test_stats.py` | 12 | 278 | 4% |
| `Quant/src/ipc/FillChannel.cpp` | 10 | 232 | 4% |
| `Quant/src/strategy/ThemeStrategy.cpp` | 11 | 269 | 4% |
| `Quant/src/core/CommandLine.cpp` | 7 | 172 | 4% |
| `Quant/src/ipc/SharedWriteLock.cpp` | 1 | 25 | 4% |
| `Quant/src/core/RegimeFileJudge.cpp` | 8 | 209 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `Quant/src/core/WebSocketSlotPlan.cpp` | 5 | 132 | 4% |
| `Quant/src/api/WsSocketWin.cpp` | 14 | 379 | 4% |
| `Quant/src/strategy/SupplyDemandPullbackStrategy.cpp` | 15 | 412 | 4% |
| `PYQuant/db/client.py` | 34 | 952 | 4% |
| `Quant/src/core/StrategyTable.cpp` | 5 | 142 | 4% |
| `PYQuant/tools/ledger_recorder.py` | 6 | 171 | 4% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `scripts/check_market_open.py` | 5 | 144 | 3% |
| `scripts/notify_trades.py` | 24 | 701 | 3% |
| `PYQuant/tests/test_costs_golden.py` | 6 | 176 | 3% |
| `PYQuant/dashboard/build_dashboard.py` | 61 | 1821 | 3% |
| `PYQuant/tools/load_injector.py` | 21 | 627 | 3% |
| `PYQuant/ipc/subscriber.py` | 4 | 121 | 3% |
| `scripts/stresstest_flow_profile.py` | 5 | 153 | 3% |
| `Quant/src/utils/EtfFilter.cpp` | 4 | 125 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `scripts/gen_tuning_sheet.py` | 15 | 485 | 3% |
| `Quant/src/core/ReplaySource.cpp` | 4 | 131 | 3% |
| `PYQuant/tools/check_kis_investor.py` | 2 | 67 | 3% |
| `Quant/src/ipc/SharedStrategyDictionary.cpp` | 4 | 134 | 3% |
| `Quant/src/ipc/SharedSymbolDictionary.cpp` | 4 | 138 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `PYQuant/tests/test_point_in_time.py` | 2 | 71 | 3% |
| `Quant/src/ipc/MarketFeedChannel.cpp` | 10 | 368 | 3% |
| `PYQuant/backtest/devscale_replay_rescue.py` | 20 | 760 | 3% |
| `Quant/src/core/LatencyTrace.cpp` | 7 | 266 | 3% |
| `Quant/src/exchange/MatchingEngine.cpp` | 22 | 843 | 3% |
| `scripts/refresh_dashboard.py` | 5 | 193 | 3% |
| `scripts/build_review_entry.py` | 7 | 274 | 3% |
| `Quant/src/core/BarAggregator.cpp` | 11 | 434 | 3% |
| `Quant/src/strategy/PriceTargetStrategy.cpp` | 6 | 237 | 3% |
| `PYQuant/tools/bench_market_open.py` | 11 | 448 | 2% |
| `Quant/src/exchange/OrderWire.cpp` | 2 | 86 | 2% |
| `Quant/src/universe/ScoreWeight.cpp` | 3 | 129 | 2% |
| `PYQuant/tools/check_investor_api.py` | 4 | 176 | 2% |
| `PYQuant/backtest/devscale_replay.py` | 16 | 705 | 2% |
| `Quant/src/exchange/ZmqOrderFeed.cpp` | 14 | 622 | 2% |
| `scripts/deploy_guard.py` | 5 | 223 | 2% |
| `scripts/deploy_trader.py` | 7 | 315 | 2% |
| `scripts/extract_swap_what_if.py` | 5 | 227 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 228 | 2% |
| `scripts/exit_ev.py` | 9 | 436 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 585 | 2% |
| `PYQuant/tools/ledger_dump.py` | 7 | 343 | 2% |
| `scripts/live_prices_feed.py` | 2 | 100 | 2% |
| `PYQuant/tools/sweep.py` | 2 | 103 | 2% |
| `PYQuant/backtest/stats.py` | 10 | 519 | 2% |
| `scripts/_logdir.py` | 4 | 215 | 2% |
| `PYQuant/tools/index_intraday_logger.py` | 4 | 216 | 2% |
| `PYQuant/strategy/supply_demand_rank.py` | 1 | 55 | 2% |
| `PYQuant/tools/full_universe_dump.py` | 2 | 111 | 2% |
| `PYQuant/strategy/channel_breakout.py` | 1 | 56 | 2% |
| `PYQuant/tests/test_adjust_splits.py` | 1 | 57 | 2% |
| `Quant/src/core/ShardRoutes.cpp` | 1 | 57 | 2% |
| `Quant/src/core/DataPoller.cpp` | 4 | 229 | 2% |
| `PYQuant/features/regime_axes.py` | 11 | 635 | 2% |
| `PYQuant/data/point_in_time.py` | 1 | 58 | 2% |
| `Quant/src/ipc/OpsServer.cpp` | 12 | 716 | 2% |
| `Quant/src/api/KisRestDecode.cpp` | 5 | 309 | 2% |
| `PYQuant/tools/minute_backfill_pairs.py` | 1 | 63 | 2% |
| `Quant/src/strategy/StrategyBase.cpp` | 1 | 64 | 2% |
| `PYQuant/live/basket_forward.py` | 5 | 330 | 2% |
| `PYQuant/tools/naver_research_fetch.py` | 9 | 595 | 2% |
| `Quant/src/api/KisWsDecode.cpp` | 4 | 270 | 1% |
| `scripts/trade_costs.py` | 2 | 136 | 1% |
| `PYQuant/tools/check_adjusted.py` | 1 | 69 | 1% |
| `PYQuant/tools/load_status_sampler.py` | 4 | 277 | 1% |
| `scripts/backfill_fills_db.py` | 2 | 141 | 1% |
| `PYQuant/tools/naver_flow_backfill.py` | 4 | 283 | 1% |
| `PYQuant/tools/investor_flow_logger.py` | 3 | 213 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/check_pykrx_flow.py` | 2 | 150 | 1% |
| `Quant/src/core/ReconcilePlan.cpp` | 1 | 76 | 1% |
| `Quant/src/risk/LedgerJournal.cpp` | 3 | 228 | 1% |
| `Quant/src/strategy/TargetBasketPlan.cpp` | 4 | 311 | 1% |
| `PYQuant/tools/macro_ingest.py` | 7 | 550 | 1% |
| `PYQuant/backtest/ledger.py` | 1 | 82 | 1% |
| `scripts/exit_ev_dashboard.py` | 7 | 587 | 1% |
| `PYQuant/tools/load_latency_reader.py` | 3 | 258 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
| `PYQuant/tools/naver_bars_backfill.py` | 3 | 280 | 1% |
| `PYQuant/tools/dart_fin_history_fill.py` | 5 | 482 | 1% |
| `Quant/src/core/TickCapture.cpp` | 4 | 403 | 1% |
| `Quant/src/strategy/TargetBasketStrategy.cpp` | 4 | 436 | 1% |
| `Quant/src/risk/ProtectiveOrders.cpp` | 2 | 226 | 1% |
| `Quant/src/core/FeedMux.cpp` | 4 | 477 | 1% |
| `PYQuant/naver/theme.py` | 1 | 121 | 1% |
| `scripts/restart_verify.py` | 2 | 256 | 1% |
| `PYQuant/tests/test_metrics.py` | 1 | 139 | 1% |
| `scripts/deploy_lock.py` | 1 | 152 | 1% |
| `PYQuant/tools/kind_delisted_fill.py` | 2 | 313 | 1% |
| `PYQuant/tools/dart_shares_history_fill.py` | 2 | 328 | 1% |
| `Quant/src/core/PaperExecutor.cpp` | 2 | 340 | 1% |
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
| `Quant/src/api/KisWebSocket.cpp` | 0 | 18 | 0% |
| `Quant/src/core/FeedSupervisor.cpp` | 0 | 35 | 0% |
| `Quant/src/core/IFeedSource.cpp` | 0 | 17 | 0% |
| `Quant/src/core/MarketSession.cpp` | 0 | 59 | 0% |
| `Quant/src/core/SessionEndJudge.cpp` | 0 | 45 | 0% |
| `Quant/src/core/StrategyShard.cpp` | 0 | 12 | 0% |
| `Quant/src/core/TickSize.cpp` | 0 | 18 | 0% |
| `Quant/src/core/Types.cpp` | 0 | 81 | 0% |
| `Quant/src/core/UniverseExit.cpp` | 0 | 30 | 0% |
| `Quant/src/core/WakeGate.cpp` | 0 | 16 | 0% |
| `Quant/src/ipc/FillKey.cpp` | 0 | 15 | 0% |
| `Quant/src/ipc/OpsProtocol.cpp` | 0 | 119 | 0% |
| `Quant/src/risk/GateReasons.cpp` | 0 | 11 | 0% |
| `Quant/src/risk/ProtectiveRule.cpp` | 0 | 35 | 0% |
| `Quant/src/strategy/DevScaleRules.cpp` | 0 | 83 | 0% |
| `Quant/src/strategy/FixedIntervalStrategy.cpp` | 0 | 69 | 0% |
| `Quant/src/strategy/SeedPeakStore.cpp` | 0 | 111 | 0% |
| `Quant/src/universe/MaAlign.cpp` | 0 | 32 | 0% |
| `Quant/src/utils/JsonNode.cpp` | 0 | 19 | 0% |
| `Quant/src/utils/ThreadName.cpp` | 0 | 15 | 0% |
| `Quant/src/utils/Utf8.cpp` | 0 | 169 | 0% |
| `scripts/stresstest_join_procwatch.py` | 0 | 97 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 250개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/KisClient.h` | 95 | 7 |
| `Quant/include/api/KisClient.h` | 111 | 4 |
| `Quant/include/api/KisClient.h` | 145 | 9 |
| `Quant/include/api/KisClient.h` | 170 | 4 |
| `Quant/include/api/KisClient.h` | 199 | 7 |
| `Quant/include/api/KisClient.h` | 218 | 5 |
| `Quant/include/api/KisClient.h` | 309 | 4 |
| `Quant/include/api/KisClient.h` | 322 | 4 |
| `Quant/include/api/KisEndpoints.h` | 2 | 4 |
| `Quant/include/api/KisWebSocket.h` | 50 | 9 |
| `Quant/include/api/KisWsDecode.h` | 37 | 4 |
| `Quant/include/api/KisWsDecode.h` | 79 | 5 |
| `Quant/include/core/AppConfig.h` | 2 | 4 |
| `Quant/include/core/Engine.h` | 62 | 10 |
| `Quant/include/core/Engine.h` | 129 | 4 |
| `Quant/include/core/Engine.h` | 230 | 5 |
| `Quant/include/core/Engine.h` | 276 | 5 |
| `Quant/include/core/Engine.h` | 340 | 4 |
| `Quant/include/core/Engine.h` | 399 | 5 |
| `Quant/include/core/Engine.h` | 456 | 8 |
| `Quant/include/core/Engine.h` | 615 | 4 |
| `Quant/include/core/Engine.h` | 658 | 4 |
| `Quant/include/core/Engine.h` | 689 | 4 |
| `Quant/include/core/Engine.h` | 981 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 49 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 107 | 6 |
| `Quant/include/core/MpscQueue.h` | 10 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/PaperExecutor.h` | 67 | 4 |
| `Quant/include/core/ReconcilePlan.h` | 46 | 5 |
| `Quant/include/core/StrategyShard.h` | 91 | 4 |
| `Quant/include/core/Types.h` | 89 | 4 |
| `Quant/include/core/Types.h` | 288 | 5 |
| `Quant/include/core/Types.h` | 381 | 4 |
| `Quant/include/core/WebSocketSlotPlan.h` | 8 | 9 |
| `Quant/include/exchange/OrderWire.h` | 1 | 8 |
| `Quant/include/ipc/Heartbeat.h` | 18 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 23 | 15 |
| `Quant/include/ipc/OrderRouter.h` | 64 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 90 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 125 | 12 |
| `Quant/include/ipc/OrderRouter.h` | 205 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 254 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 330 | 4 |
| `Quant/include/ipc/SharedLayout.h` | 28 | 8 |
| `Quant/include/ipc/ZmqBridge.h` | 54 | 4 |
| `Quant/include/risk/OrderGate.h` | 108 | 4 |
| `Quant/include/risk/OrderGate.h` | 170 | 10 |
| `Quant/include/risk/OrderGate.h` | 275 | 6 |
| `Quant/include/risk/PositionLedger.h` | 212 | 4 |
| `Quant/include/risk/PositionLedger.h` | 224 | 4 |
| `Quant/include/risk/PositionLedger.h` | 231 | 6 |
| `Quant/include/risk/PositionLedger.h` | 243 | 7 |
| `Quant/include/risk/PositionLedger.h` | 291 | 5 |
| `Quant/include/risk/PositionLedger.h` | 298 | 6 |
| `Quant/include/risk/PositionLedger.h` | 312 | 4 |
| `Quant/include/risk/PositionLedger.h` | 329 | 4 |
| `Quant/include/risk/PositionLedger.h` | 334 | 4 |
| `Quant/include/risk/PositionLedger.h` | 359 | 4 |
| `Quant/include/risk/PositionLedger.h` | 463 | 4 |
| `Quant/include/risk/ProtectiveRule.h` | 12 | 4 |
| `Quant/include/strategy/DevScaleRules.h` | 2 | 7 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 29 | 36 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 77 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 92 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 108 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 274 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 302 | 9 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 336 | 9 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 349 | 4 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 11 | 23 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 101 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 14 | 4 |
| `Quant/include/strategy/MarketMakingStrategy.h` | 10 | 21 |
| `Quant/include/strategy/MomentumStrategy.h` | 6 | 5 |
| `Quant/include/strategy/PriceTargetStrategy.h` | 9 | 10 |
| `Quant/include/strategy/StrategyBase.h` | 47 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 72 | 5 |
| `Quant/include/strategy/StrategyBase.h` | 143 | 6 |
| `Quant/include/strategy/StrategyBase.h` | 176 | 4 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | 27 | 18 |
| `Quant/include/strategy/TargetBasketPlan.h` | 2 | 9 |
| `Quant/include/strategy/ThemeStrategy.h` | 14 | 14 |
| `Quant/include/strategy/ThemeStrategy.h` | 52 | 4 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 20 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 5 |
| `Quant/include/universe/ScoreWeight.h` | 37 | 23 |
| `Quant/include/universe/UniverseScanner.h` | 9 | 7 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 21 | 4 |
| `Quant/include/utils/EtfFilter.h` | 42 | 5 |
| `Quant/include/utils/Utf8.h` | 7 | 8 |
| `Quant/src/main.cpp` | 76 | 4 |
| `Quant/src/main.cpp` | 148 | 4 |
| `Quant/src/main.cpp` | 204 | 5 |
| `Quant/src/main.cpp` | 299 | 5 |
| `Quant/src/api/KisAccount.cpp` | 15 | 4 |
| `Quant/src/api/KisAccount.cpp` | 87 | 7 |
| `Quant/src/api/KisMarket.cpp` | 314 | 4 |
| `Quant/src/api/KisMarket.cpp` | 493 | 4 |
| `Quant/src/api/KisMarket.cpp` | 590 | 5 |
| `Quant/src/api/KisOrder.cpp` | 10 | 4 |
| `Quant/src/api/KisOrder.cpp` | 84 | 5 |
| `Quant/src/api/KisOrder.cpp` | 254 | 4 |
| `Quant/src/api/KisOrder.cpp` | 363 | 4 |
| `Quant/src/api/KisOrder.cpp` | 498 | 5 |
| `Quant/src/api/KisTransport.cpp` | 12 | 4 |
| `Quant/src/api/KisTransport.cpp` | 66 | 4 |
| `Quant/src/api/KisTransport.cpp` | 137 | 6 |
| `Quant/src/api/KisTransport.cpp` | 322 | 5 |
| `Quant/src/api/KisTransport.cpp` | 554 | 7 |
| `Quant/src/api/KisUniverse.cpp` | 5 | 6 |
| `Quant/src/api/KisUniverse.cpp` | 219 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 287 | 8 |
| `Quant/src/api/KisUniverse.cpp` | 409 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 462 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 654 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 109 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 142 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 200 | 5 |
| `Quant/src/api/WsSocketWin.cpp` | 292 | 4 |
| `Quant/src/core/ControlPlane.cpp` | 1 | 8 |
| `Quant/src/core/Engine.cpp` | 244 | 4 |
| `Quant/src/core/Engine.cpp` | 289 | 5 |
| `Quant/src/core/EngineControlThread.cpp` | 1 | 11 |
| `Quant/src/core/EngineControlThread.cpp` | 60 | 7 |
| `Quant/src/core/EngineDataThread.cpp` | 1 | 7 |
| `Quant/src/core/EngineDataThread.cpp` | 180 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 258 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 268 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 432 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 529 | 5 |
| `Quant/src/core/EngineFillThread.cpp` | 1 | 6 |
| `Quant/src/core/EngineLayout.cpp` | 1 | 6 |
| `Quant/src/core/EngineOrderThread.cpp` | 1 | 8 |
| `Quant/src/core/EngineOrderThread.cpp` | 239 | 4 |
| `Quant/src/core/EngineRegime.cpp` | 1 | 8 |
| `Quant/src/core/EngineStrategyThread.cpp` | 1 | 9 |
| `Quant/src/core/EngineStrategyThread.cpp` | 89 | 5 |
| `Quant/src/core/EngineStrategyThread.cpp` | 118 | 4 |
| `Quant/src/core/EngineSymbols.cpp` | 1 | 9 |
| `Quant/src/core/EngineUniverse.cpp` | 80 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 16 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 54 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 98 | 5 |
| `Quant/src/core/LedgerReconciler.cpp` | 208 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 243 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 278 | 6 |
| `Quant/src/core/OrderRateLimiter.cpp` | 33 | 4 |
| `Quant/src/core/SignalDispatcher.cpp` | 190 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 52 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 101 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 113 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 142 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 495 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 508 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 741 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1604 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1682 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 1715 | 7 |
| `Quant/src/ipc/OrderRouter.cpp` | 2039 | 5 |
| `Quant/src/ipc/OrderRouter.cpp` | 2225 | 6 |
| `Quant/src/ipc/OrderRouter.cpp` | 2383 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2392 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2406 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2443 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2468 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2516 | 4 |
| `Quant/src/ipc/OrderRouter.cpp` | 2685 | 4 |
| `Quant/src/risk/DisplacementDesk.cpp` | 40 | 6 |
| `Quant/src/risk/OrderGate.cpp` | 23 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 75 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 89 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 460 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 555 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 576 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 586 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 603 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 625 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 770 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 930 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 1105 | 4 |
| `Quant/src/risk/PositionLedger.cpp` | 132 | 7 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 253 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 290 | 5 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 296 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 482 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 508 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 591 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 746 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 777 | 5 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 868 | 12 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 909 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 967 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 981 | 8 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 1165 | 4 |
| `Quant/src/strategy/IntradayBreakoutStrategy.cpp` | 221 | 11 |
| (이하 50개 생략) | | |

## 5. settings.json 훅 배선

| 이벤트 | 훅 경로 | 실재 |
|---|---|---|
| `PreToolUse` | `.claude/hooks/pre-gates.ps1` | 있음 |
| `PreToolUse` | `.claude/hooks/lexicon-gate.ps1` | 있음 |
| `PostToolUse` | `.claude/hooks/push-summary.ps1` | 있음 |
| `Stop` | `.claude/hooks/stop-gates.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/resume-work.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/market-close-gate.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/cron-gate.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/handoff-list.ps1` | 있음 |
| `SessionStart` | `.claude/hooks/session-board-server.ps1` | 있음 |
| `PreCompact` | `.claude/hooks/precompact-handoff.ps1` | 있음 |

| 훅 파일 | 배선 | BOM UTF-8 |
|---|---|---|
| `cron-gate.ps1` | 됨 | 예 |
| `dashboard-refresh.ps1` | [!] 안 됨 | 예 |
| `docs-gate.ps1` | [!] 안 됨 | 예 |
| `file-index-gate.ps1` | [!] 안 됨 | 예 |
| `handoff-due.ps1` | [!] 안 됨 | 예 |
| `handoff-list.ps1` | 됨 | 예 |
| `lexicon-gate.ps1` | 됨 | 예 |
| `market-close-gate.ps1` | 됨 | 예 |
| `output-gate.ps1` | [!] 안 됨 | 예 |
| `pre-gates.ps1` | 됨 | 예 |
| `precompact-handoff.ps1` | 됨 | 예 |
| `push-summary.ps1` | 됨 | 예 |
| `resume-work.ps1` | 됨 | 예 |
| `review-reminder.ps1` | [!] 안 됨 | 예 |
| `secret-gate.ps1` | [!] 안 됨 | 예 |
| `session-board-server.ps1` | 됨 | 예 |
| `stop-gates.ps1` | 됨 | 예 |
| `sync-gate.ps1` | [!] 안 됨 | 예 |

`settings.json` BOM: 아니오

## 6. `.claude/` 변경(해시 매니페스트)

파일 86개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 2026-09-22

- 추가 5개: `.claude/commands/doc-audit.md`, `.claude/hooks/pre-gates.ps1`, `.claude/hooks/push-summary.ps1`, `.claude/hooks/stop-gates.ps1`, `.claude/settings.json.bak-0922-1936`
- 삭제 1개: `.claude/scheduled_tasks.lock`
- 변경 14개: `.claude/PROJECT_FACTS.md`, `.claude/commands/build.md`, `.claude/commands/handoff.md`, `.claude/commit-gate.state`, `.claude/hooks/docs-gate.ps1`, `.claude/hooks/file-index-gate.ps1`, `.claude/hooks/handoff-due.ps1`, `.claude/hooks/handoff-list.ps1`, `.claude/hooks/lexicon-gate.ps1`, `.claude/hooks/output-gate.ps1`, `.claude/hooks/secret-gate.ps1`, `.claude/hooks/sync-gate.ps1`, `.claude/settings.json`, `.claude/sync-gate.state`
