<!-- drift-check: snapshot 2026-10-02 -->

# 주간 유지관리 리포트 — 2026-10-02

빨간 항목: 19

`py ../quant-devtools/maintain.py --weekly`가 만든 스냅샷. `.claude/`는 읽기만 했다. 고칠 항목은 사람이 승인한다.

## 1. 미참조 스크립트

`scripts/*.py`·`PYQuant/tools/*.py` 중 다른 파일·`.claude/`·`docs/`·예약작업 어디에서도 이름이 안 나오는 것.

- 없음

## 2. 에이전트·커맨드의 죽은 경로

- [!] `.claude/agents/macro-quant.md:36` → `PYQuant/tools/macro_regime_feed.py`
- [!] `.claude/agents/prep-doc.md:8` → `_private/…`
- [!] `.claude/agents/prep-doc.md:12` → `_private/…`
- [!] `.claude/agents/prep-doc.md:19` → `_private/…`
- [!] `.claude/agents/prep-doc.md:69` → `_private/…`
- [!] `.claude/agents/prep-doc.md:69` → `_private/…`
- [!] `.claude/commands/auto-trade-day.md:13` → `_private/…`
- [!] `.claude/commands/doc-audit.md:13` → `Quant/src/core/Engine.cpp/.h`
- [!] `.claude/commands/doc-audit.md:60` → `docs/archive/`
- [!] `.claude/commands/doc-audit.md:80` → `Quant/build`
- [!] `.claude/commands/doc-audit.md:80` → `Quant/.claude`

## 3. 부산물 용량

빌드 트리·캐시·가상환경·데이터·스터디 산출물 중 100MB 이상이거나 30일 넘게 손대지 않은 폴더. 전부 gitignore 대상이라 저장소 크기와는 무관하고, 지워도 재빌드·재수집으로 돌아온다.

| 폴더 | 크기 | 마지막 수정 | 비고 |
|---|---|---|---|
| `PYQuant/data/cache` | 1309.3 MB | 2026-10-02 | 100MB 이상 |
| `Quant/build_win` | 1013.2 MB | 2026-10-02 | 100MB 이상 |
| `out/build/x64-release` | 576.4 MB | 2026-10-01 | 100MB 이상 |
| `PYQuant/data/ticks_raw` | 567.7 MB | 2026-10-02 | 100MB 이상 |
| `PYQuant/.venv` | 515.3 MB | 2026-09-08 | 100MB 이상 |
| `PYQuant/data/minute` | 491.4 MB | 2026-10-02 | 100MB 이상 |
| `PYQuant/.venv-win` | 481.5 MB | 2026-09-23 | 100MB 이상 |
| `research/studies/24_devscale_exit_lines` | 392.1 MB | 2026-09-25 | 100MB 이상 |
| `research/studies/35_surge_box_breakout` | 342.2 MB | 2026-10-02 | 100MB 이상 |
| `PYQuant/data/ticks_raw_live` | 315.1 MB | 2026-10-02 | 100MB 이상 |
| `research/studies/15_impulse_pullback` | 277.6 MB | 2026-09-19 | 100MB 이상 |
| `research/studies/12_base_breakout` | 195.5 MB | 2026-09-26 | 100MB 이상 |
| `research/studies/33_post_surge_pullback` | 143.7 MB | 2026-10-02 | 100MB 이상 |
| `research/studies/30_strong_stock_strategies` | 127.0 MB | 2026-10-02 | 100MB 이상 |
| `PYQuant/.index_cache` | 5.7 MB | 2026-08-25 | 38일째 그대로 |
| `research/studies/raw` | 0.3 MB | 2026-08-09 | 53일째 그대로 |
| `PYQuant/data/index_intraday` | 0.0 MB | 2026-08-25 | 38일째 그대로 |
| **합계** | **6754.0 MB** | | 17개 |

## 4. 주석 밀도

게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.

| 파일 | 주석줄 | 전체줄 | 비율 |
|---|---|---|---|
| `Quant/src/utils/Timer.cpp` | 1 | 1 | 100% |
| `Quant/include/api/KisErrorCodes.h` | 15 | 26 | 58% |
| `Quant/include/ipc/OrderJournal.h` | 68 | 148 | 46% |
| `Quant/include/universe/ScoreWeight.h` | 35 | 77 | 45% |
| `Quant/include/strategy/DevScaleRules.h` | 33 | 75 | 44% |
| `Quant/include/api/KisRestDecode.h` | 53 | 126 | 42% |
| `Quant/src/api/WsSocket.h` | 16 | 39 | 41% |
| `Quant/include/utils/EtfFilter.h` | 20 | 50 | 40% |
| `Quant/include/ipc/OrderRouter.h` | 219 | 555 | 39% |
| `Quant/include/utils/Utf8.h` | 8 | 21 | 38% |
| `Quant/include/api/KisWebSocket.h` | 62 | 168 | 37% |
| `Quant/include/api/KisClient.h` | 138 | 377 | 37% |
| `Quant/include/utils/JsonNode.h` | 5 | 14 | 36% |
| `Quant/include/api/KisResult.h` | 12 | 34 | 35% |
| `Quant/include/ipc/LedgerSnapshot.h` | 60 | 174 | 34% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 159 | 463 | 34% |
| `Quant/include/api/IOrderExecutor.h` | 46 | 136 | 34% |
| `Quant/include/core/Engine.h` | 433 | 1285 | 34% |
| `Quant/include/ipc/RegimeCell.h` | 8 | 24 | 33% |
| `Quant/include/api/KisWsDecode.h` | 58 | 177 | 33% |
| `Quant/include/universe/MaAlign.h` | 11 | 34 | 32% |
| `Quant/src/universe/detail/Pipeline.h` | 77 | 241 | 32% |
| `Quant/include/core/CommandLine.h` | 29 | 91 | 32% |
| `Quant/include/core/MarketSession.h` | 12 | 38 | 32% |
| `Quant/include/risk/PositionLedger.h` | 194 | 619 | 31% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 31 | 100 | 31% |
| `Quant/include/api/KisTypes.h` | 13 | 42 | 31% |
| `Quant/include/risk/OrderGate.h` | 115 | 376 | 31% |
| `Quant/src/strategy/StrategyLoadPass.h` | 24 | 79 | 30% |
| `Quant/include/universe/UniverseScanner.h` | 41 | 136 | 30% |
| `Quant/include/risk/EntryPriority.h` | 31 | 105 | 30% |
| `Quant/include/core/AppConfig.h` | 31 | 107 | 29% |
| `Quant/include/ipc/FillChannel.h` | 47 | 164 | 29% |
| `Quant/include/ipc/OrderChannel.h` | 75 | 262 | 29% |
| `scripts/log_patterns.py` | 6 | 21 | 29% |
| `Quant/include/ipc/MarketFeedChannel.h` | 43 | 151 | 28% |
| `Quant/src/core/EngineLedgerThread.cpp` | 17 | 60 | 28% |
| `Quant/include/ipc/OrderHistory.h` | 25 | 89 | 28% |
| `Quant/include/api/KisEndpoints.h` | 10 | 36 | 28% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/core/WebSocketSlotPlan.h` | 17 | 63 | 27% |
| `Quant/include/core/Types.h` | 101 | 375 | 27% |
| `Quant/include/api/HttpGet.h` | 4 | 15 | 27% |
| `Quant/include/strategy/SeedPeakStore.h` | 13 | 49 | 27% |
| `Quant/include/ipc/ControlChannel.h` | 40 | 155 | 26% |
| `Quant/include/exchange/OrderWire.h` | 23 | 91 | 25% |
| `Quant/include/ipc/SharedRegion.h` | 54 | 214 | 25% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 27 | 107 | 25% |
| `Quant/include/ipc/ProcessIdentity.h` | 10 | 40 | 25% |
| `Quant/include/utils/AtomicFile.h` | 3 | 12 | 25% |
| `Quant/include/utils/ThreadName.h` | 7 | 28 | 25% |
| `Quant/include/core/DataPoller.h` | 41 | 165 | 25% |
| `Quant/include/utils/Logger.h` | 25 | 101 | 25% |
| `Quant/include/ipc/DbManager.h` | 23 | 93 | 25% |
| `Quant/include/core/StrategyTable.h` | 34 | 138 | 25% |
| `Quant/include/regime/RegimeFeed.h` | 32 | 133 | 24% |
| `Quant/include/ipc/ZmqBridge.h` | 60 | 250 | 24% |
| `Quant/include/api/KisRateBucket.h` | 16 | 67 | 24% |
| `Quant/include/api/IMarketDataSource.h` | 10 | 42 | 24% |
| `Quant/include/exchange/ZmqOrderFeed.h` | 42 | 177 | 24% |
| `Quant/include/core/KstTime.h` | 21 | 89 | 24% |
| `Quant/include/core/SessionEndJudge.h` | 12 | 51 | 24% |
| `Quant/include/core/IFeedSource.h` | 19 | 81 | 23% |
| `Quant/include/risk/ProtectiveRule.h` | 14 | 60 | 23% |
| `Quant/include/ipc/Heartbeat.h` | 26 | 113 | 23% |
| `Quant/include/strategy/StrategyBase.h` | 76 | 332 | 23% |
| `Quant/include/core/UniverseRescan.h` | 27 | 118 | 23% |
| `Quant/include/ipc/SharedSymbolDictionary.h` | 26 | 114 | 23% |
| `Quant/include/ipc/SharedLayout.h` | 53 | 233 | 23% |
| `Quant/include/core/SymbolTable.h` | 40 | 185 | 22% |
| `Quant/include/ipc/SharedStrategyDictionary.h` | 24 | 111 | 22% |
| `Quant/include/universe/MarketBoard.h` | 40 | 186 | 22% |
| `Quant/include/exchange/MatchingEngine.h` | 55 | 256 | 21% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 41 | 192 | 21% |
| `Quant/include/core/SignalDispatcher.h` | 33 | 155 | 21% |
| `Quant/include/core/ControlPlane.h` | 28 | 135 | 21% |
| `Quant/include/core/UniverseExit.h` | 15 | 73 | 21% |
| `Quant/include/ipc/OpsProtocol.h` | 19 | 93 | 20% |
| `Quant/include/core/ReconcilePlan.h` | 11 | 54 | 20% |
| `Quant/include/core/TickSize.h` | 8 | 40 | 20% |
| `Quant/include/core/FeedMux.h` | 32 | 165 | 19% |
| `Quant/include/strategy/TargetBasketPlan.h` | 19 | 102 | 19% |
| `Quant/src/core/EngineControlThread.cpp` | 78 | 421 | 19% |
| `Quant/include/core/LedgerReconciler.h` | 37 | 200 | 18% |
| `Quant/include/core/BarAggregator.h` | 25 | 136 | 18% |
| `Quant/include/core/RingBuffer.h` | 27 | 148 | 18% |
| `Quant/include/risk/GateReasons.h` | 6 | 33 | 18% |
| `Quant/include/core/HttpQuoteFeed.h` | 17 | 94 | 18% |
| `Quant/include/core/RegimeFileJudge.h` | 27 | 151 | 18% |
| `Quant/src/core/EngineStrategyThread.cpp` | 110 | 616 | 18% |
| `Quant/src/api/KisAccount.cpp` | 40 | 225 | 18% |
| `Quant/src/core/EngineLayout.cpp` | 40 | 226 | 18% |
| `Quant/include/core/PaperExecutor.h` | 27 | 153 | 18% |
| `Quant/src/core/EngineFillThread.cpp` | 26 | 149 | 17% |
| `Quant/include/risk/LedgerJournal.h` | 43 | 248 | 17% |
| `Quant/include/ipc/SharedSpscRing.h` | 72 | 418 | 17% |
| `Quant/include/core/WakeGate.h` | 22 | 130 | 17% |
| `Quant/include/core/LatencyTrace.h` | 27 | 160 | 17% |
| `Quant/include/core/ShardRoutes.h` | 20 | 119 | 17% |
| `Quant/src/core/EngineDataThread.cpp` | 114 | 680 | 17% |
| `Quant/include/strategy/MACrossStrategy.h` | 11 | 66 | 17% |
| `Quant/src/core/EngineOrderThread.cpp` | 137 | 856 | 16% |
| `Quant/include/risk/LedgerKeys.h` | 30 | 188 | 16% |
| `Quant/src/core/OrderRateLimiter.cpp` | 22 | 140 | 16% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 10 | 64 | 16% |
| `Quant/src/core/EngineRegime.cpp` | 42 | 269 | 16% |
| `Quant/src/core/EngineConfigure.cpp` | 33 | 214 | 15% |
| `Quant/include/core/MpscQueue.h` | 24 | 157 | 15% |
| `Quant/src/ipc/OrderRouter.cpp` | 25 | 166 | 15% |
| `Quant/src/ipc/ZmqBridge.cpp` | 93 | 618 | 15% |
| `Quant/src/universe/UniverseItb.cpp` | 20 | 134 | 15% |
| `Quant/include/ipc/FillKey.h` | 4 | 27 | 15% |
| `Quant/include/risk/DisplacementDesk.h` | 9 | 61 | 15% |
| `Quant/src/risk/OrderGate.cpp` | 162 | 1156 | 14% |
| `Quant/src/api/KisUniverse.cpp` | 105 | 750 | 14% |
| `Quant/src/main.cpp` | 51 | 367 | 14% |
| `Quant/src/api/KisOrder.cpp` | 63 | 459 | 14% |
| `Quant/include/core/PrefetchPool.h` | 32 | 237 | 14% |
| `Quant/src/core/EngineSymbols.cpp` | 42 | 312 | 13% |
| `Quant/src/api/KisAuth.cpp` | 36 | 270 | 13% |
| `Quant/src/api/KisClientInternal.h` | 4 | 30 | 13% |
| `Quant/src/core/EngineControlPlane.cpp` | 25 | 190 | 13% |
| `Quant/src/core/Engine.cpp` | 138 | 1059 | 13% |
| `Quant/include/core/OrderRateLimiter.h` | 14 | 108 | 13% |
| `Quant/src/ipc/OrderRouterFill.cpp` | 93 | 724 | 13% |
| `Quant/src/api/KisWebSocketParse.cpp` | 59 | 464 | 13% |
| `Quant/src/api/KisTransport.cpp` | 98 | 775 | 13% |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 154 | 1228 | 13% |
| `Quant/src/api/WebSocketClient.cpp` | 63 | 503 | 13% |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 190 | 1561 | 12% |
| `Quant/src/core/LedgerReconciler.cpp` | 62 | 510 | 12% |
| `Quant/include/core/TransportPool.h` | 21 | 174 | 12% |
| `Quant/include/core/TickCapture.h` | 30 | 250 | 12% |
| `Quant/include/ipc/SharedWriteLock.h` | 3 | 25 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/src/strategy/DevScaleLoader.cpp` | 95 | 802 | 12% |
| `Quant/include/core/ShardMatrix.h` | 19 | 162 | 12% |
| `Quant/src/core/EngineFeed.cpp` | 121 | 1042 | 12% |
| `Quant/src/strategy/IntradayBreakoutStrategy.cpp` | 43 | 409 | 11% |
| `Quant/include/ipc/OpsServer.h` | 19 | 181 | 10% |
| `PYQuant/ipc/subscriber.py` | 17 | 163 | 10% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/src/core/EngineOpsServer.cpp` | 32 | 309 | 10% |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 71 | 694 | 10% |
| `Quant/src/core/EngineUniverse.cpp` | 21 | 209 | 10% |
| `Quant/src/risk/LedgerKeys.cpp` | 2 | 20 | 10% |
| `Quant/include/core/StrategyShard.h` | 18 | 183 | 10% |
| `Quant/src/universe/UniverseQuotes.cpp` | 10 | 104 | 10% |
| `Quant/src/universe/UniverseRiskGate.cpp` | 15 | 158 | 9% |
| `Quant/src/core/AppConfig.cpp` | 31 | 327 | 9% |
| `Quant/include/risk/ProtectiveOrders.h` | 12 | 127 | 9% |
| `Quant/include/core/MutexQueue.h` | 8 | 85 | 9% |
| `Quant/src/core/SignalDispatcher.cpp` | 27 | 293 | 9% |
| `Quant/include/core/FeedSupervisor.h` | 7 | 76 | 9% |
| `scripts/parse_quant_log.py` | 31 | 339 | 9% |
| `Quant/src/core/ControlPlane.cpp` | 26 | 295 | 9% |
| `Quant/src/ipc/LedgerSnapshot.cpp` | 17 | 195 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/src/api/KisMarket.cpp` | 49 | 582 | 8% |
| `PYQuant/tools/load_highwater_reader.py` | 22 | 265 | 8% |
| `Quant/src/utils/Logger.cpp` | 38 | 461 | 8% |
| `PYQuant/dashboard/backfill_live.py` | 15 | 183 | 8% |
| `PYQuant/tools/universe_feed.py` | 20 | 247 | 8% |
| `Quant/src/strategy/StrategyFactory.cpp` | 46 | 571 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `Quant/include/strategy/TargetBasketStrategy.h` | 9 | 112 | 8% |
| `Quant/include/core/StrategyRouter.h` | 9 | 116 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `scripts/make_load_test_config.py` | 12 | 157 | 8% |
| `Quant/src/risk/DisplacementDesk.cpp` | 11 | 150 | 7% |
| `PYQuant/data/datagokr_source.py` | 26 | 360 | 7% |
| `scripts/market_close_collect.py` | 19 | 264 | 7% |
| `Quant/include/core/ReplaySource.h` | 10 | 139 | 7% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `PYQuant/backtest/engine.py` | 39 | 562 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `scripts/check_runtime_health.py` | 252 | 3732 | 7% |
| `scripts/market_close_autodoc.py` | 37 | 549 | 7% |
| `Quant/src/ipc/OrderJournal.cpp` | 33 | 491 | 7% |
| `Quant/src/universe/UniverseScanner.cpp` | 9 | 135 | 7% |
| `Quant/src/api/WsSocketPosix.cpp` | 34 | 513 | 7% |
| `Quant/src/strategy/MarketMakingStrategy.cpp` | 9 | 136 | 7% |
| `Quant/src/universe/UniverseCandidates.cpp` | 36 | 563 | 6% |
| `Quant/src/strategy/ValueContraryStrategy.cpp` | 15 | 235 | 6% |
| `Quant/src/core/HttpQuoteFeed.cpp` | 22 | 346 | 6% |
| `scripts/market_close_minute_backfill.py` | 5 | 79 | 6% |
| `Quant/src/ipc/OrderChannel.cpp` | 26 | 413 | 6% |
| `Quant/src/core/SymbolTable.cpp` | 16 | 256 | 6% |
| `Quant/src/ipc/ProcessIdentity.cpp` | 10 | 162 | 6% |
| `PYQuant/main.py` | 49 | 799 | 6% |
| `Quant/src/strategy/MACrossStrategy.cpp` | 6 | 98 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `Quant/src/ipc/SharedLayout.cpp` | 23 | 385 | 6% |
| `Quant/src/ipc/SharedRegion.cpp` | 40 | 673 | 6% |
| `Quant/src/universe/UniverseFeatures.cpp` | 40 | 679 | 6% |
| `Quant/src/core/WakeGate.cpp` | 7 | 120 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `Quant/src/risk/PositionLedger.cpp` | 79 | 1394 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `Quant/src/ipc/ControlChannel.cpp` | 7 | 125 | 6% |
| `PYQuant/core/proc_watch.py` | 19 | 341 | 6% |
| `scripts/dashboard_server.py` | 109 | 2012 | 5% |
| `Quant/src/ipc/Heartbeat.cpp` | 6 | 111 | 5% |
| `Quant/src/core/UniverseRescan.cpp` | 23 | 427 | 5% |
| `PYQuant/backtest/costs.py` | 8 | 149 | 5% |
| `PYQuant/db/client.py` | 63 | 1202 | 5% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `scripts/_logdir.py` | 18 | 351 | 5% |
| `Quant/src/risk/EntryPriority.cpp` | 11 | 216 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `Quant/src/universe/UniverseScoring.cpp` | 10 | 200 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `Quant/src/api/KisClient.cpp` | 3 | 62 | 5% |
| `Quant/src/ipc/FillChannel.cpp` | 12 | 251 | 5% |
| `Quant/src/core/KstTime.cpp` | 5 | 105 | 5% |
| `PYQuant/kis/client.py` | 47 | 992 | 5% |
| `Quant/src/ipc/DbManager.cpp` | 39 | 837 | 5% |
| `scripts/premarket_routine.py` | 4 | 86 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `Quant/src/utils/AtomicFile.cpp` | 2 | 44 | 5% |
| `Quant/src/universe/MarketBoard.cpp` | 36 | 803 | 4% |
| `Quant/src/api/WsSocketWin.cpp` | 17 | 382 | 4% |
| `PYQuant/backtest/metrics.py` | 8 | 184 | 4% |
| `PYQuant/tests/test_stats.py` | 12 | 278 | 4% |
| `Quant/src/core/CommandLine.cpp` | 7 | 172 | 4% |
| `Quant/src/ipc/SharedWriteLock.cpp` | 1 | 25 | 4% |
| `Quant/src/core/RegimeFileJudge.cpp` | 8 | 209 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `Quant/src/core/WebSocketSlotPlan.cpp` | 5 | 138 | 4% |
| `Quant/src/api/KisIndex.cpp` | 10 | 278 | 4% |
| `Quant/src/core/StrategyTable.cpp` | 5 | 142 | 4% |
| `Quant/src/ipc/OrderHistory.cpp` | 4 | 114 | 4% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `scripts/check_market_open.py` | 5 | 144 | 3% |
| `scripts/notify_trades.py` | 24 | 701 | 3% |
| `PYQuant/tests/test_costs_golden.py` | 6 | 176 | 3% |
| `PYQuant/dashboard/build_dashboard.py` | 62 | 1844 | 3% |
| `scripts/stresstest_flow_profile.py` | 5 | 153 | 3% |
| `Quant/src/utils/EtfFilter.cpp` | 4 | 125 | 3% |
| `PYQuant/tools/ledger_recorder.py` | 5 | 160 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `Quant/src/exchange/MatchingEngine.cpp` | 25 | 805 | 3% |
| `scripts/gen_tuning_sheet.py` | 15 | 485 | 3% |
| `PYQuant/tools/load_injector.py` | 22 | 727 | 3% |
| `Quant/src/regime/RegimeFeed.cpp` | 39 | 1297 | 3% |
| `Quant/src/core/ReplaySource.cpp` | 4 | 136 | 3% |
| `Quant/src/ipc/SharedStrategyDictionary.cpp` | 4 | 136 | 3% |
| `PYQuant/tools/ledger_dump.py` | 13 | 455 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `Quant/src/exchange/ZmqOrderFeed.cpp` | 20 | 701 | 3% |
| `Quant/src/ipc/SharedSymbolDictionary.cpp` | 4 | 141 | 3% |
| `PYQuant/tests/test_point_in_time.py` | 2 | 71 | 3% |
| `Quant/src/ipc/MarketFeedChannel.cpp` | 10 | 369 | 3% |
| `PYQuant/backtest/devscale_replay_rescue.py` | 20 | 760 | 3% |
| `scripts/refresh_dashboard.py` | 5 | 193 | 3% |
| `scripts/build_review_entry.py` | 7 | 274 | 3% |
| `Quant/src/core/LatencyTrace.cpp` | 7 | 276 | 3% |
| `scripts/deploy_trader.py` | 8 | 316 | 3% |
| `Quant/src/core/BarAggregator.cpp` | 11 | 437 | 3% |
| `PYQuant/tools/bench_market_open.py` | 11 | 448 | 2% |
| `PYQuant/backtest/devscale_replay.py` | 18 | 736 | 2% |
| `Quant/src/core/DataPoller.cpp` | 10 | 412 | 2% |
| `Quant/src/exchange/OrderWire.cpp` | 2 | 86 | 2% |
| `Quant/src/universe/ScoreWeight.cpp` | 3 | 129 | 2% |
| `scripts/deploy_guard.py` | 5 | 223 | 2% |
| `scripts/extract_swap_what_if.py` | 5 | 227 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 228 | 2% |
| `Quant/src/api/KisWsDecode.cpp` | 5 | 241 | 2% |
| `scripts/exit_ev.py` | 9 | 438 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 586 | 2% |
| `Quant/src/ipc/OpsServer.cpp` | 15 | 740 | 2% |
| `PYQuant/tools/sweep.py` | 2 | 103 | 2% |
| `PYQuant/backtest/stats.py` | 10 | 519 | 2% |
| `PYQuant/tools/full_universe_dump.py` | 2 | 109 | 2% |
| `Quant/src/api/KisRestDecode.cpp` | 9 | 493 | 2% |
| `PYQuant/strategy/supply_demand_rank.py` | 1 | 55 | 2% |
| `PYQuant/strategy/channel_breakout.py` | 1 | 56 | 2% |
| `PYQuant/tests/test_adjust_splits.py` | 1 | 57 | 2% |
| `Quant/src/core/ShardRoutes.cpp` | 1 | 57 | 2% |
| `PYQuant/features/regime_axes.py` | 11 | 635 | 2% |
| `PYQuant/data/point_in_time.py` | 1 | 58 | 2% |
| `PYQuant/tools/minute_backfill_pairs.py` | 1 | 63 | 2% |
| `Quant/src/strategy/TargetBasketStrategy.cpp` | 7 | 445 | 2% |
| `Quant/src/strategy/StrategyBase.cpp` | 1 | 64 | 2% |
| `PYQuant/live/basket_forward.py` | 5 | 330 | 2% |
| `PYQuant/tools/naver_research_fetch.py` | 9 | 595 | 2% |
| `scripts/capture_stats.py` | 3 | 199 | 2% |
| `scripts/trade_costs.py` | 2 | 136 | 1% |
| `scripts/backfill_fills_db.py` | 2 | 141 | 1% |
| `PYQuant/tools/naver_flow_backfill.py` | 4 | 286 | 1% |
| `PYQuant/tools/bench_recorder.py` | 3 | 219 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/minute_strong_open_fetch.py` | 4 | 301 | 1% |
| `Quant/src/core/ReconcilePlan.cpp` | 1 | 76 | 1% |
| `Quant/src/strategy/TargetBasketPlan.cpp` | 4 | 311 | 1% |
| `Quant/src/risk/LedgerJournal.cpp` | 3 | 234 | 1% |
| `PYQuant/tools/macro_ingest.py` | 7 | 550 | 1% |
| `PYQuant/backtest/ledger.py` | 1 | 82 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
| `PYQuant/tools/load_latency_reader.py` | 3 | 263 | 1% |
| `scripts/exit_ev_dashboard.py` | 6 | 569 | 1% |
| `PYQuant/tools/dart_fin_history_fill.py` | 5 | 482 | 1% |
| `Quant/src/core/TickCapture.cpp` | 4 | 409 | 1% |
| `scripts/startup_to_order_timeline.py` | 2 | 218 | 1% |
| `Quant/src/risk/ProtectiveOrders.cpp` | 2 | 226 | 1% |
| `PYQuant/tools/naver_bars_backfill.py` | 3 | 343 | 1% |
| `PYQuant/naver/theme.py` | 1 | 121 | 1% |
| `Quant/src/core/FeedMux.cpp` | 4 | 496 | 1% |
| `scripts/restart_verify.py` | 2 | 260 | 1% |
| `Quant/src/strategy/DevScaleRules.cpp` | 1 | 137 | 1% |
| `PYQuant/tests/test_metrics.py` | 1 | 139 | 1% |
| `scripts/deploy_lock.py` | 1 | 152 | 1% |
| `PYQuant/tools/kind_delisted_fill.py` | 2 | 313 | 1% |
| `PYQuant/tools/dart_shares_history_fill.py` | 2 | 328 | 1% |
| `Quant/src/core/PaperExecutor.cpp` | 2 | 353 | 1% |
| `scripts/summarize_trading_day.py` | 1 | 216 | 0% |
| `scripts/build_study_site.py` | 2 | 483 | 0% |
| `PYQuant/tools/compare_ws_bars.py` | 1 | 243 | 0% |
| `scripts/kis_limit_check.py` | 1 | 246 | 0% |
| `PYQuant/features/fundamental.py` | 1 | 275 | 0% |
| `PYQuant/core/logger.py` | 0 | 51 | 0% |
| `PYQuant/data/asof.py` | 0 | 8 | 0% |
| `PYQuant/data/keys.py` | 0 | 28 | 0% |
| `PYQuant/features/__init__.py` | 0 | 1 | 0% |
| `PYQuant/kis/endpoints.py` | 0 | 20 | 0% |
| `PYQuant/live/trader.py` | 0 | 73 | 0% |
| `PYQuant/strategy/indicators.py` | 0 | 33 | 0% |
| `PYQuant/tests/test_db_client.py` | 0 | 94 | 0% |
| `PYQuant/tests/test_regime_axes.py` | 0 | 94 | 0% |
| `PYQuant/tools/fetch_naver_themes.py` | 0 | 66 | 0% |
| `Quant/src/api/KisWebSocket.cpp` | 0 | 18 | 0% |
| `Quant/src/core/FeedSupervisor.cpp` | 0 | 35 | 0% |
| `Quant/src/core/IFeedSource.cpp` | 0 | 23 | 0% |
| `Quant/src/core/MarketSession.cpp` | 0 | 59 | 0% |
| `Quant/src/core/PrefetchPool.cpp` | 0 | 24 | 0% |
| `Quant/src/core/SessionEndJudge.cpp` | 0 | 45 | 0% |
| `Quant/src/core/StrategyShard.cpp` | 0 | 12 | 0% |
| `Quant/src/core/TickSize.cpp` | 0 | 18 | 0% |
| `Quant/src/core/Types.cpp` | 0 | 76 | 0% |
| `Quant/src/core/UniverseExit.cpp` | 0 | 30 | 0% |
| `Quant/src/ipc/FillKey.cpp` | 0 | 15 | 0% |
| `Quant/src/ipc/OpsProtocol.cpp` | 0 | 119 | 0% |
| `Quant/src/risk/GateReasons.cpp` | 0 | 11 | 0% |
| `Quant/src/risk/ProtectiveRule.cpp` | 0 | 20 | 0% |
| `Quant/src/strategy/FixedIntervalStrategy.cpp` | 0 | 69 | 0% |
| `Quant/src/strategy/SeedPeakStore.cpp` | 0 | 111 | 0% |
| `Quant/src/universe/MaAlign.cpp` | 0 | 30 | 0% |
| `Quant/src/utils/JsonNode.cpp` | 0 | 19 | 0% |
| `Quant/src/utils/ThreadName.cpp` | 0 | 61 | 0% |
| `Quant/src/utils/Utf8.cpp` | 0 | 86 | 0% |
| `scripts/stresstest_join_procwatch.py` | 0 | 97 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 235개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/IOrderExecutor.h` | 30 | 5 |
| `Quant/include/api/KisClient.h` | 236 | 6 |
| `Quant/include/api/KisClient.h` | 323 | 5 |
| `Quant/include/api/KisEndpoints.h` | 2 | 6 |
| `Quant/include/api/KisErrorCodes.h` | 12 | 4 |
| `Quant/include/api/KisWebSocket.h` | 49 | 9 |
| `Quant/include/api/KisWsDecode.h` | 37 | 4 |
| `Quant/include/api/KisWsDecode.h` | 92 | 5 |
| `Quant/include/core/AppConfig.h` | 2 | 4 |
| `Quant/include/core/Engine.h` | 64 | 10 |
| `Quant/include/core/Engine.h` | 152 | 4 |
| `Quant/include/core/Engine.h` | 268 | 6 |
| `Quant/include/core/Engine.h` | 372 | 5 |
| `Quant/include/core/Engine.h` | 467 | 4 |
| `Quant/include/core/Engine.h` | 554 | 5 |
| `Quant/include/core/Engine.h` | 620 | 8 |
| `Quant/include/core/Engine.h` | 810 | 4 |
| `Quant/include/core/Engine.h` | 853 | 4 |
| `Quant/include/core/Engine.h` | 888 | 4 |
| `Quant/include/core/Engine.h` | 1220 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 49 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 140 | 6 |
| `Quant/include/core/MpscQueue.h` | 10 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/PaperExecutor.h` | 77 | 4 |
| `Quant/include/core/ReconcilePlan.h` | 46 | 5 |
| `Quant/include/core/StrategyShard.h` | 91 | 4 |
| `Quant/include/core/StrategyTable.h` | 24 | 6 |
| `Quant/include/core/TickSize.h` | 5 | 4 |
| `Quant/include/core/Types.h` | 90 | 4 |
| `Quant/include/core/Types.h` | 366 | 4 |
| `Quant/include/core/WebSocketSlotPlan.h` | 8 | 10 |
| `Quant/include/exchange/OrderWire.h` | 1 | 8 |
| `Quant/include/ipc/ControlChannel.h` | 26 | 6 |
| `Quant/include/ipc/Heartbeat.h` | 19 | 5 |
| `Quant/include/ipc/Heartbeat.h` | 61 | 5 |
| `Quant/include/ipc/OrderRouter.h` | 108 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 392 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 516 | 4 |
| `Quant/include/ipc/SharedLayout.h` | 28 | 11 |
| `Quant/include/ipc/SharedLayout.h` | 49 | 4 |
| `Quant/include/ipc/ZmqBridge.h` | 65 | 4 |
| `Quant/include/risk/OrderGate.h` | 114 | 4 |
| `Quant/include/risk/OrderGate.h` | 190 | 10 |
| `Quant/include/risk/OrderGate.h` | 302 | 6 |
| `Quant/include/risk/PositionLedger.h` | 287 | 4 |
| `Quant/include/risk/PositionLedger.h` | 302 | 4 |
| `Quant/include/risk/PositionLedger.h` | 317 | 6 |
| `Quant/include/risk/PositionLedger.h` | 329 | 7 |
| `Quant/include/risk/PositionLedger.h` | 350 | 4 |
| `Quant/include/risk/PositionLedger.h` | 381 | 5 |
| `Quant/include/risk/PositionLedger.h` | 388 | 6 |
| `Quant/include/risk/PositionLedger.h` | 402 | 4 |
| `Quant/include/risk/PositionLedger.h` | 419 | 4 |
| `Quant/include/risk/PositionLedger.h` | 424 | 4 |
| `Quant/include/risk/PositionLedger.h` | 464 | 4 |
| `Quant/include/risk/PositionLedger.h` | 583 | 4 |
| `Quant/include/risk/ProtectiveRule.h` | 12 | 4 |
| `Quant/include/strategy/DevScaleRules.h` | 2 | 11 |
| `Quant/include/strategy/DevScaleRules.h` | 33 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 29 | 35 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 76 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 91 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 107 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 150 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 275 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 346 | 11 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 361 | 4 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 122 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 6 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 14 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 47 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 77 | 5 |
| `Quant/include/strategy/StrategyBase.h` | 188 | 6 |
| `Quant/include/strategy/StrategyBase.h` | 221 | 4 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/TargetBasketPlan.h` | 2 | 9 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 20 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 37 | 5 |
| `Quant/include/universe/ScoreWeight.h` | 37 | 23 |
| `Quant/include/universe/UniverseScanner.h` | 9 | 7 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 21 | 4 |
| `Quant/include/utils/EtfFilter.h` | 42 | 5 |
| `Quant/src/main.cpp` | 78 | 4 |
| `Quant/src/main.cpp` | 150 | 4 |
| `Quant/src/main.cpp` | 206 | 5 |
| `Quant/src/main.cpp` | 313 | 5 |
| `Quant/src/api/KisAccount.cpp` | 17 | 4 |
| `Quant/src/api/KisAuth.cpp` | 219 | 4 |
| `Quant/src/api/KisMarket.cpp` | 57 | 4 |
| `Quant/src/api/KisMarket.cpp` | 514 | 6 |
| `Quant/src/api/KisOrder.cpp` | 10 | 4 |
| `Quant/src/api/KisOrder.cpp` | 82 | 7 |
| `Quant/src/api/KisOrder.cpp` | 235 | 4 |
| `Quant/src/api/KisTransport.cpp` | 12 | 4 |
| `Quant/src/api/KisTransport.cpp` | 74 | 6 |
| `Quant/src/api/KisTransport.cpp` | 147 | 6 |
| `Quant/src/api/KisTransport.cpp` | 336 | 6 |
| `Quant/src/api/KisTransport.cpp` | 573 | 9 |
| `Quant/src/api/KisUniverse.cpp` | 5 | 6 |
| `Quant/src/api/KisUniverse.cpp` | 235 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 306 | 9 |
| `Quant/src/api/KisUniverse.cpp` | 444 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 708 | 5 |
| `Quant/src/api/KisWebSocketParse.cpp` | 136 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 112 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 121 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 147 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 286 | 4 |
| `Quant/src/api/WsSocket.h` | 22 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 54 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 188 | 5 |
| `Quant/src/api/WsSocketPosix.cpp` | 204 | 5 |
| `Quant/src/api/WsSocketWin.cpp` | 294 | 4 |
| `Quant/src/core/ControlPlane.cpp` | 1 | 9 |
| `Quant/src/core/ControlPlane.cpp` | 173 | 4 |
| `Quant/src/core/Engine.cpp` | 244 | 5 |
| `Quant/src/core/Engine.cpp` | 304 | 5 |
| `Quant/src/core/EngineControlThread.cpp` | 1 | 11 |
| `Quant/src/core/EngineControlThread.cpp` | 62 | 7 |
| `Quant/src/core/EngineControlThread.cpp` | 76 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 1 | 7 |
| `Quant/src/core/EngineDataThread.cpp` | 45 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 135 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 364 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 526 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 532 | 4 |
| `Quant/src/core/EngineFillThread.cpp` | 1 | 6 |
| `Quant/src/core/EngineLayout.cpp` | 1 | 7 |
| `Quant/src/core/EngineLedgerThread.cpp` | 1 | 5 |
| `Quant/src/core/EngineOrderThread.cpp` | 1 | 8 |
| `Quant/src/core/EngineOrderThread.cpp` | 297 | 4 |
| `Quant/src/core/EngineRegime.cpp` | 1 | 8 |
| `Quant/src/core/EngineStrategyThread.cpp` | 1 | 9 |
| `Quant/src/core/EngineStrategyThread.cpp` | 123 | 5 |
| `Quant/src/core/EngineStrategyThread.cpp` | 152 | 5 |
| `Quant/src/core/EngineSymbols.cpp` | 1 | 9 |
| `Quant/src/core/LedgerReconciler.cpp` | 17 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 57 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 102 | 5 |
| `Quant/src/core/LedgerReconciler.cpp` | 216 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 251 | 4 |
| `Quant/src/core/OrderRateLimiter.cpp` | 36 | 4 |
| `Quant/src/core/SignalDispatcher.cpp` | 184 | 4 |
| `Quant/src/ipc/DbManager.cpp` | 45 | 5 |
| `Quant/src/ipc/DbManager.cpp` | 172 | 4 |
| `Quant/src/ipc/DbManager.cpp` | 718 | 6 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 107 | 6 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 118 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 221 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 255 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 282 | 6 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 307 | 16 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 429 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 672 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 214 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 293 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 447 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 632 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 13 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 236 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 248 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 269 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 621 | 7 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 632 | 7 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 1171 | 4 |
| `Quant/src/ipc/ZmqBridge.cpp` | 174 | 4 |
| `Quant/src/risk/DisplacementDesk.cpp` | 40 | 6 |
| `Quant/src/risk/OrderGate.cpp` | 23 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 113 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 127 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 476 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 573 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 594 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 604 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 621 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 643 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 766 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 905 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 1091 | 4 |
| `Quant/src/risk/PositionLedger.cpp` | 141 | 7 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 1 | 4 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 34 | 9 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 79 | 4 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 196 | 4 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 556 | 7 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 632 | 4 |
| `Quant/src/strategy/DevScaleLoader.cpp` | 734 | 5 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 256 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 306 | 5 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 499 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 600 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 738 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 856 | 9 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 893 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 953 | 4 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 967 | 8 |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 1150 | 5 |
| (이하 35개 생략) | | |

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

파일 1020개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 2026-09-25

- 추가 934개: `.claude/worktrees/agent-aba2f9a71597c9508/.clang-format`, `.claude/worktrees/agent-aba2f9a71597c9508/.dockerignore`, `.claude/worktrees/agent-aba2f9a71597c9508/.env.example`, `.claude/worktrees/agent-aba2f9a71597c9508/.git`, `.claude/worktrees/agent-aba2f9a71597c9508/.gitattributes`, `.claude/worktrees/agent-aba2f9a71597c9508/.gitignore`, `.claude/worktrees/agent-aba2f9a71597c9508/.mcp.json`, `.claude/worktrees/agent-aba2f9a71597c9508/CLAUDE.md`, `.claude/worktrees/agent-aba2f9a71597c9508/CMakeLists.txt`, `.claude/worktrees/agent-aba2f9a71597c9508/CMakePresets.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/Dockerfile`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/__init__.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/costs.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/devscale_replay.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/devscale_replay_rescue.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/engine.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/ledger.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/metrics.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/regime_scorer.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/report.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/backtest/stats.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/config/bench_market_open.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/config/bench_market_open_stress.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/config/default_universe.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/config/strategy_a.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/core/__init__.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/core/logger.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/core/proc_watch.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/dashboard/backfill_live.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/dashboard/backfill_series_a.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/dashboard/build_dashboard.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/__init__.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/asof.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/datagokr_source.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/index_source.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/keys.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/krx_source.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/point_in_time.py`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/trend7_codes.json`, `.claude/worktrees/agent-aba2f9a71597c9508/PYQuant/data/universe_kospi.py`
- 삭제 0개
- 변경 15개: `.claude/AGENTS.md`, `.claude/PROJECT_FACTS.md`, `.claude/agents/backtest-runner.md`, `.claude/agents/bias-auditor.md`, `.claude/agents/data-sourcer.md`, `.claude/agents/fundamental-quant.md`, `.claude/agents/interviewer.md`, `.claude/agents/macro-quant.md`, `.claude/agents/quant-analyst.md`, `.claude/agents/risk-behavior.md`, `.claude/agents/strategist.md`, `.claude/commands/auto-trade-day.md`, `.claude/commands/doc-audit.md`, `.claude/commit-gate.state`, `.claude/sync-gate.state`
