<!-- drift-check: snapshot 2026-10-16 -->

# 주간 유지관리 리포트 — 2026-10-16

빨간 항목: 20

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
| `research/studies/38_trend_support_realtime` | 2507.2 MB | 2026-10-11 | 100MB 이상 |
| `PYQuant/data/cache` | 1737.3 MB | 2026-10-16 | 100MB 이상 |
| `research/studies/38_trend_stop_intraday` | 1685.0 MB | 2026-10-16 | 100MB 이상 |
| `PYQuant/data/ticks_raw` | 1633.1 MB | 2026-10-16 | 100MB 이상 |
| `.vs` | 1561.2 MB | 2026-10-14 | 100MB 이상 |
| `PYQuant/data/ticks_raw_live` | 1355.0 MB | 2026-10-16 | 100MB 이상 |
| `Quant/build_win` | 883.8 MB | 2026-10-16 | 100MB 이상 |
| `PYQuant/.venv-win` | 732.8 MB | 2026-10-10 | 100MB 이상 |
| `out/build/x64-release` | 724.5 MB | 2026-10-15 | 100MB 이상 |
| `PYQuant/data/minute` | 529.0 MB | 2026-10-16 | 100MB 이상 |
| `PYQuant/.venv` | 515.3 MB | 2026-09-08 | 100MB 이상 · 38일째 그대로 |
| `research/studies/24_devscale_exit_lines` | 392.1 MB | 2026-09-25 | 100MB 이상 |
| `research/studies/35_surge_box_breakout` | 349.3 MB | 2026-10-16 | 100MB 이상 |
| `research/studies/39_devscale_exit_vs_trend` | 301.3 MB | 2026-10-11 | 100MB 이상 |
| `research/studies/15_impulse_pullback` | 277.6 MB | 2026-09-19 | 100MB 이상 |
| `research/studies/12_base_breakout` | 195.4 MB | 2026-09-26 | 100MB 이상 |
| `research/studies/41_universe_basket` | 157.9 MB | 2026-10-11 | 100MB 이상 |
| `research/studies/33_post_surge_pullback` | 143.7 MB | 2026-10-12 | 100MB 이상 |
| `research/studies/30_strong_stock_strategies` | 126.9 MB | 2026-10-12 | 100MB 이상 |
| `PYQuant/data/themes` | 124.5 MB | 2026-10-16 | 100MB 이상 |
| `PYQuant/.index_cache` | 5.7 MB | 2026-08-25 | 52일째 그대로 |
| `research/studies/06_bear_market` | 3.5 MB | 2026-09-06 | 39일째 그대로 |
| `research/studies/raw` | 0.3 MB | 2026-08-09 | 67일째 그대로 |
| `PYQuant/data/index_intraday` | 0.0 MB | 2026-08-25 | 52일째 그대로 |
| **합계** | **15942.6 MB** | | 24개 |

## 4. 주석 밀도

게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.

| 파일 | 주석줄 | 전체줄 | 비율 |
|---|---|---|---|
| `Quant/src/utils/Timer.cpp` | 1 | 1 | 100% |
| `Quant/include/api/KisErrorCodes.h` | 21 | 35 | 60% |
| `Quant/include/ipc/OrderJournal.h` | 68 | 148 | 46% |
| `Quant/include/strategy/DevScaleRules.h` | 55 | 121 | 45% |
| `Quant/include/universe/ScoreWeight.h` | 35 | 77 | 45% |
| `Quant/include/api/KisRestDecode.h` | 58 | 129 | 45% |
| `Quant/include/ipc/OrderRouter.h` | 233 | 527 | 44% |
| `Quant/src/api/WsSocket.h` | 16 | 39 | 41% |
| `Quant/include/core/EngineOrderAge.h` | 18 | 44 | 41% |
| `Quant/include/utils/EtfFilter.h` | 20 | 50 | 40% |
| `Quant/include/api/KisClient.h` | 154 | 387 | 40% |
| `Quant/include/utils/Utf8.h` | 8 | 21 | 38% |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 182 | 478 | 38% |
| `Quant/include/api/KisWebSocket.h` | 63 | 169 | 37% |
| `Quant/include/core/EnginePipeline.h` | 57 | 157 | 36% |
| `Quant/include/utils/JsonNode.h` | 5 | 14 | 36% |
| `Quant/include/api/KisResult.h` | 12 | 34 | 35% |
| `Quant/include/core/Engine.h` | 372 | 1101 | 34% |
| `Quant/include/risk/PositionLedger.h` | 193 | 573 | 34% |
| `Quant/include/api/IOrderExecutor.h` | 46 | 137 | 34% |
| `Quant/include/api/KisWsDecode.h` | 60 | 179 | 34% |
| `Quant/include/strategy/SwingTrendRules.h` | 23 | 69 | 33% |
| `Quant/include/universe/MaAlign.h` | 11 | 34 | 32% |
| `Quant/include/core/CommandLine.h` | 29 | 91 | 32% |
| `Quant/include/strategy/SeedPeakStore.h` | 18 | 57 | 32% |
| `Quant/src/strategy/StrategyLoadPass.h` | 36 | 114 | 32% |
| `Quant/src/universe/detail/Pipeline.h` | 83 | 263 | 32% |
| `Quant/include/ipc/RegimeCell.h` | 10 | 32 | 31% |
| `Quant/include/universe/UniverseScanner.h` | 44 | 142 | 31% |
| `Quant/include/strategy/ValueContraryStrategy.h` | 30 | 99 | 30% |
| `Quant/include/ipc/LedgerSnapshot.h` | 70 | 233 | 30% |
| `Quant/include/core/MarketSession.h` | 16 | 54 | 30% |
| `Quant/include/risk/EntryPriority.h` | 31 | 105 | 30% |
| `Quant/include/risk/OrderGate.h` | 129 | 440 | 29% |
| `Quant/include/core/EngineOpsChannel.h` | 14 | 48 | 29% |
| `Quant/include/core/AppConfig.h` | 32 | 110 | 29% |
| `Quant/include/ipc/OrderChannel.h` | 79 | 276 | 29% |
| `scripts/log_patterns.py` | 6 | 21 | 29% |
| `Quant/include/ipc/FillChannel.h` | 47 | 165 | 28% |
| `Quant/include/ipc/MarketFeedChannel.h` | 43 | 151 | 28% |
| `Quant/include/core/EngineFeedState.h` | 25 | 88 | 28% |
| `Quant/src/core/EngineLedgerThread.cpp` | 17 | 60 | 28% |
| `Quant/include/ipc/OrderHistory.h` | 25 | 89 | 28% |
| `Quant/include/api/KisEndpoints.h` | 10 | 36 | 28% |
| `Quant/include/strategy/StrategyFactory.h` | 6 | 22 | 27% |
| `Quant/include/core/WebSocketSlotPlan.h` | 17 | 63 | 27% |
| `Quant/include/ipc/ZmqBridge.h` | 59 | 219 | 27% |
| `Quant/include/api/HttpGet.h` | 4 | 15 | 27% |
| `Quant/include/core/IFeedSource.h` | 22 | 84 | 26% |
| `Quant/include/risk/Money.h` | 18 | 69 | 26% |
| `Quant/include/ipc/ControlChannel.h` | 42 | 162 | 26% |
| `Quant/include/core/Types.h` | 112 | 438 | 26% |
| `Quant/include/strategy/MarketMakingStrategy.h` | 26 | 102 | 25% |
| `Quant/include/utils/Logger.h` | 26 | 102 | 25% |
| `Quant/include/ipc/DbManager.h` | 39 | 154 | 25% |
| `Quant/include/ipc/SharedRegion.h` | 54 | 214 | 25% |
| `Quant/include/ipc/ProcessIdentity.h` | 10 | 40 | 25% |
| `Quant/include/utils/AtomicFile.h` | 3 | 12 | 25% |
| `Quant/include/core/StrategyTable.h` | 34 | 138 | 25% |
| `Quant/include/api/KisRateBucket.h` | 23 | 94 | 24% |
| `Quant/include/exchange/OrderWire.h` | 23 | 94 | 24% |
| `Quant/include/core/DataPoller.h` | 43 | 178 | 24% |
| `Quant/include/core/SignalDispatcher.h` | 43 | 179 | 24% |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 52 | 217 | 24% |
| `Quant/include/api/IMarketDataSource.h` | 10 | 42 | 24% |
| `Quant/include/utils/ThreadName.h` | 10 | 42 | 24% |
| `Quant/include/exchange/ZmqOrderFeed.h` | 42 | 177 | 24% |
| `Quant/include/core/KstTime.h` | 21 | 89 | 24% |
| `Quant/include/core/SessionEndJudge.h` | 12 | 51 | 24% |
| `Quant/include/ipc/OrderRouterRecoveryState.h` | 16 | 68 | 24% |
| `Quant/include/risk/ProtectiveRule.h` | 14 | 60 | 23% |
| `Quant/include/universe/MarketBoard.h` | 44 | 189 | 23% |
| `Quant/include/ipc/Heartbeat.h` | 28 | 121 | 23% |
| `Quant/include/ipc/SharedLayout.h` | 54 | 234 | 23% |
| `Quant/include/strategy/StrategyBase.h` | 90 | 390 | 23% |
| `Quant/include/core/UniverseRescan.h` | 27 | 118 | 23% |
| `Quant/include/ipc/SharedSymbolDictionary.h` | 26 | 114 | 23% |
| `Quant/include/risk/PositionLedgerTypes.h` | 32 | 144 | 22% |
| `Quant/include/utils/GenerationCopy.h` | 9 | 41 | 22% |
| `Quant/include/core/SymbolTable.h` | 40 | 185 | 22% |
| `Quant/include/ipc/SharedStrategyDictionary.h` | 24 | 111 | 22% |
| `Quant/include/core/EngineStrategyRegistry.h` | 9 | 42 | 21% |
| `Quant/include/api/KisTypes.h` | 20 | 94 | 21% |
| `Quant/include/core/UniverseExit.h` | 15 | 72 | 21% |
| `Quant/include/ipc/SideLabel.h` | 5 | 24 | 21% |
| `Quant/include/exchange/MatchingEngine.h` | 68 | 327 | 21% |
| `Quant/include/core/ReconcilePlan.h` | 11 | 53 | 21% |
| `Quant/include/core/ControlPlane.h` | 28 | 135 | 21% |
| `Quant/include/ipc/OpsProtocol.h` | 19 | 92 | 21% |
| `Quant/include/core/TickSize.h` | 8 | 39 | 21% |
| `Quant/src/api/KisClientInternal.h` | 4 | 20 | 20% |
| `Quant/include/core/FeedMux.h` | 32 | 165 | 19% |
| `Quant/include/strategy/TargetBasketPlan.h` | 19 | 102 | 19% |
| `Quant/include/core/LedgerReconciler.h` | 37 | 200 | 18% |
| `Quant/src/risk/OrderGate.cpp` | 155 | 849 | 18% |
| `Quant/include/risk/GateReasons.h` | 6 | 33 | 18% |
| `Quant/include/core/RingBuffer.h` | 27 | 149 | 18% |
| `Quant/include/core/BarAggregator.h` | 25 | 138 | 18% |
| `Quant/include/core/HttpQuoteFeed.h` | 17 | 94 | 18% |
| `Quant/src/core/EngineControlThread.cpp` | 84 | 469 | 18% |
| `Quant/include/core/ShardRoutes.h` | 22 | 123 | 18% |
| `Quant/include/core/PaperExecutor.h` | 27 | 151 | 18% |
| `Quant/include/core/RegimeFileJudge.h` | 27 | 152 | 18% |
| `Quant/src/core/EngineLayout.cpp` | 40 | 226 | 18% |
| `Quant/include/ipc/FixedText.h` | 11 | 63 | 17% |
| `Quant/include/regime/RegimeFeed.h` | 41 | 235 | 17% |
| `Quant/include/risk/LedgerJournal.h` | 43 | 248 | 17% |
| `Quant/src/core/EngineFillThread.cpp` | 32 | 185 | 17% |
| `Quant/include/ipc/SharedSpscRing.h` | 72 | 418 | 17% |
| `Quant/src/core/EngineStrategyThread.cpp` | 126 | 734 | 17% |
| `Quant/src/api/KisOrder.cpp` | 86 | 503 | 17% |
| `Quant/src/api/KisUniverse.cpp` | 114 | 669 | 17% |
| `Quant/include/core/WakeGate.h` | 22 | 130 | 17% |
| `Quant/include/core/LatencyTrace.h` | 27 | 160 | 17% |
| `Quant/src/core/EngineDataThread.cpp` | 133 | 795 | 17% |
| `Quant/include/core/HealthSnapshot.h` | 10 | 60 | 17% |
| `Quant/include/strategy/VwapPullbackRules.h` | 32 | 194 | 16% |
| `Quant/include/strategy/SurgeHoldPlan.h` | 18 | 111 | 16% |
| `Quant/src/core/EngineFeed.cpp` | 95 | 591 | 16% |
| `Quant/src/api/KisAccount.cpp` | 46 | 287 | 16% |
| `Quant/include/risk/LedgerKeys.h` | 30 | 188 | 16% |
| `Quant/src/universe/UniverseItb.cpp` | 20 | 126 | 16% |
| `Quant/include/strategy/MACrossStrategy.h` | 11 | 70 | 16% |
| `Quant/src/core/OrderRateLimiter.cpp` | 22 | 140 | 16% |
| `Quant/src/risk/OrderGateDisplace.cpp` | 43 | 275 | 16% |
| `Quant/src/ipc/OrderRouter.cpp` | 29 | 188 | 15% |
| `Quant/src/core/EngineConfigure.cpp` | 33 | 214 | 15% |
| `Quant/src/core/EngineOrderThread.cpp` | 147 | 956 | 15% |
| `Quant/src/ipc/ZmqBridge.cpp` | 97 | 635 | 15% |
| `Quant/include/core/MpscQueue.h` | 24 | 158 | 15% |
| `Quant/include/utils/ThreadGuard.h` | 9 | 60 | 15% |
| `Quant/src/core/Engine.cpp` | 106 | 707 | 15% |
| `Quant/include/ipc/OrderRouterSubmitState.h` | 11 | 74 | 15% |
| `Quant/include/core/PrefetchPool.h` | 27 | 182 | 15% |
| `Quant/include/ipc/FillKey.h` | 4 | 27 | 15% |
| `Quant/src/main.cpp` | 55 | 372 | 15% |
| `Quant/src/strategy/DeviationScaleOrders.cpp` | 68 | 468 | 15% |
| `Quant/include/risk/DisplacementDesk.h` | 9 | 62 | 15% |
| `Quant/include/ipc/OrderRouterTypes.h` | 5 | 35 | 14% |
| `Quant/src/ipc/OrderRouterFill.cpp` | 109 | 774 | 14% |
| `Quant/src/core/EngineRegime.cpp` | 43 | 313 | 14% |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 90 | 657 | 14% |
| `Quant/src/core/EngineSymbols.cpp` | 42 | 312 | 13% |
| `Quant/src/core/EngineStartupSetup.cpp` | 35 | 261 | 13% |
| `Quant/src/api/KisTransport.cpp` | 110 | 822 | 13% |
| `Quant/src/regime/detail/RegimeTables.h` | 9 | 68 | 13% |
| `Quant/include/core/TransportPool.h` | 24 | 182 | 13% |
| `Quant/src/api/KisAuth.cpp` | 37 | 282 | 13% |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 8 | 62 | 13% |
| `Quant/include/core/OrderRateLimiter.h` | 14 | 109 | 13% |
| `Quant/src/strategy/DeviationScaleZone.cpp` | 34 | 267 | 13% |
| `Quant/src/core/LedgerReconciler.cpp` | 65 | 513 | 13% |
| `Quant/include/ipc/OpsServer.h` | 24 | 190 | 13% |
| `Quant/src/api/KisWebSocketParse.cpp` | 58 | 463 | 13% |
| `Quant/src/api/WebSocketClient.cpp` | 63 | 503 | 13% |
| `Quant/include/ipc/DbWorkerState.h` | 24 | 192 | 12% |
| `Quant/src/ipc/OrderRouterBlockedSell.cpp` | 35 | 281 | 12% |
| `Quant/src/strategy/DeviationScaleBars.cpp` | 48 | 389 | 12% |
| `Quant/include/strategy/DeviationScaleState.h` | 9 | 73 | 12% |
| `Quant/src/core/EngineControlPlane.cpp` | 28 | 229 | 12% |
| `Quant/include/core/TickCapture.h` | 30 | 249 | 12% |
| `Quant/include/strategy/SwingTrendStrategy.h` | 28 | 233 | 12% |
| `Quant/include/ipc/SharedWriteLock.h` | 3 | 25 | 12% |
| `PYQuant/data/universe_kospi.py` | 18 | 151 | 12% |
| `PYQuant/tests/test_indicators.py` | 14 | 118 | 12% |
| `Quant/include/core/ShardMatrix.h` | 19 | 162 | 12% |
| `Quant/src/strategy/DevScaleLoader.cpp` | 97 | 830 | 12% |
| `Quant/src/ipc/OrderRouterModify.cpp` | 46 | 396 | 12% |
| `Quant/include/strategy/SurgeHoldStrategy.h` | 20 | 173 | 12% |
| `Quant/src/core/EngineUniverse.cpp` | 23 | 212 | 11% |
| `Quant/src/core/EngineOpsServer.cpp` | 34 | 318 | 11% |
| `Quant/include/ipc/OrderRouterSendSteps.h` | 5 | 47 | 11% |
| `Quant/include/strategy/VwapPullbackStrategy.h` | 15 | 143 | 10% |
| `PYQuant/ipc/subscriber.py` | 17 | 163 | 10% |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 87 | 838 | 10% |
| `PYQuant/live/forward_trader.py` | 30 | 289 | 10% |
| `Quant/src/core/PrefetchPool.cpp` | 9 | 89 | 10% |
| `Quant/src/universe/UniverseRiskGate.cpp` | 15 | 149 | 10% |
| `scripts/parse_quant_log.py` | 34 | 348 | 10% |
| `Quant/src/universe/MarketBoardParse.cpp` | 21 | 216 | 10% |
| `Quant/src/universe/UniverseQuotes.cpp` | 10 | 103 | 10% |
| `Quant/include/core/StrategyShard.h` | 20 | 208 | 10% |
| `Quant/src/core/AppConfig.cpp` | 34 | 358 | 9% |
| `Quant/src/strategy/IntradayBreakoutStrategy.cpp` | 43 | 455 | 9% |
| `Quant/include/core/MutexQueue.h` | 8 | 85 | 9% |
| `Quant/src/risk/PositionLedger.cpp` | 50 | 537 | 9% |
| `Quant/include/risk/ProtectiveOrders.h` | 12 | 129 | 9% |
| `Quant/include/core/FeedSupervisor.h` | 7 | 76 | 9% |
| `Quant/src/ipc/LedgerSnapshot.cpp` | 24 | 262 | 9% |
| `Quant/src/risk/PositionLedgerQuery.cpp` | 26 | 284 | 9% |
| `Quant/src/strategy/DeviationScaleExit.cpp` | 35 | 385 | 9% |
| `Quant/src/core/ControlPlane.cpp` | 27 | 298 | 9% |
| `Quant/src/core/EngineFeedWatch.cpp` | 21 | 235 | 9% |
| `Quant/src/risk/LedgerKeys.cpp` | 2 | 23 | 9% |
| `Quant/src/strategy/DeviationScaleStrategy.cpp` | 39 | 453 | 9% |
| `PYQuant/tests/test_regime_scorer.py` | 11 | 128 | 9% |
| `Quant/include/core/StrategyRouter.h` | 10 | 117 | 9% |
| `Quant/src/api/KisMarket.cpp` | 51 | 598 | 9% |
| `Quant/src/strategy/MarketMakingStrategy.cpp` | 11 | 129 | 9% |
| `Quant/include/strategy/TargetBasketStrategy.h` | 9 | 108 | 8% |
| `PYQuant/tools/load_highwater_reader.py` | 22 | 265 | 8% |
| `Quant/src/core/SignalDispatcher.cpp` | 27 | 329 | 8% |
| `PYQuant/dashboard/backfill_live.py` | 15 | 183 | 8% |
| `Quant/src/strategy/StrategyFactory.cpp` | 55 | 676 | 8% |
| `PYQuant/tools/universe_feed.py` | 20 | 247 | 8% |
| `PYQuant/strategy/cross_momentum.py` | 7 | 87 | 8% |
| `Quant/src/utils/Logger.cpp` | 38 | 474 | 8% |
| `PYQuant/strategy/mean_reversion.py` | 6 | 78 | 8% |
| `PYQuant/tests/test_strategy_a.py` | 16 | 208 | 8% |
| `scripts/make_load_test_config.py` | 12 | 157 | 8% |
| `scripts/market_close_autodoc.py` | 42 | 560 | 8% |
| `PYQuant/data/datagokr_source.py` | 26 | 360 | 7% |
| `scripts/market_close_collect.py` | 19 | 264 | 7% |
| `Quant/include/core/ReplaySource.h` | 10 | 140 | 7% |
| `Quant/src/ipc/DbWorkers.cpp` | 31 | 436 | 7% |
| `Quant/src/universe/UniverseScanner.cpp` | 9 | 128 | 7% |
| `PYQuant/strategy/strategy_a.py` | 11 | 157 | 7% |
| `Quant/src/ipc/OrderJournal.cpp` | 35 | 500 | 7% |
| `PYQuant/backtest/engine.py` | 39 | 562 | 7% |
| `Quant/src/core/HttpQuoteFeed.cpp` | 25 | 361 | 7% |
| `Quant/src/ipc/SharedRegion.cpp` | 51 | 737 | 7% |
| `Quant/src/api/WsSocketPosix.cpp` | 36 | 522 | 7% |
| `PYQuant/strategy/value_contrary.py` | 6 | 88 | 7% |
| `Quant/src/strategy/ValueContraryStrategy.cpp` | 15 | 231 | 6% |
| `Quant/src/core/EngineFeedSlots.cpp` | 19 | 296 | 6% |
| `Quant/src/risk/DisplacementDesk.cpp` | 10 | 156 | 6% |
| `scripts/market_close_minute_backfill.py` | 5 | 79 | 6% |
| `Quant/src/universe/UniverseFeatures.cpp` | 46 | 731 | 6% |
| `Quant/src/core/SymbolTable.cpp` | 16 | 256 | 6% |
| `Quant/src/ipc/ProcessIdentity.cpp` | 10 | 162 | 6% |
| `Quant/src/universe/UniverseCandidates.cpp` | 37 | 601 | 6% |
| `PYQuant/main.py` | 49 | 803 | 6% |
| `PYQuant/data/index_source.py` | 6 | 100 | 6% |
| `scripts/check_runtime_health.py` | 286 | 4776 | 6% |
| `Quant/src/ipc/SharedLayout.cpp` | 23 | 385 | 6% |
| `Quant/src/core/WakeGate.cpp` | 7 | 121 | 6% |
| `scripts/seed_open_orders.py` | 7 | 123 | 6% |
| `Quant/src/strategy/SwingTrendLoader.cpp` | 8 | 142 | 6% |
| `PYQuant/tools/walkforward.py` | 9 | 160 | 6% |
| `scripts/_logdir.py` | 20 | 358 | 6% |
| `Quant/src/ipc/ControlChannel.cpp` | 7 | 126 | 6% |
| `scripts/dashboard_server.py` | 109 | 2012 | 5% |
| `Quant/src/ipc/Heartbeat.cpp` | 6 | 111 | 5% |
| `Quant/src/ipc/OrderChannel.cpp` | 20 | 371 | 5% |
| `Quant/src/strategy/MACrossStrategy.cpp` | 5 | 93 | 5% |
| `Quant/src/core/UniverseRescan.cpp` | 23 | 428 | 5% |
| `PYQuant/backtest/costs.py` | 8 | 149 | 5% |
| `PYQuant/db/client.py` | 63 | 1202 | 5% |
| `Quant/src/core/RegimeFileJudge.cpp` | 12 | 230 | 5% |
| `scripts/backfill_studies.py` | 9 | 173 | 5% |
| `Quant/src/universe/UniverseScoring.cpp` | 10 | 197 | 5% |
| `PYQuant/data/krx_source.py` | 7 | 138 | 5% |
| `scripts/redact_publish.py` | 8 | 159 | 5% |
| `Quant/src/risk/EntryPriority.cpp` | 11 | 223 | 5% |
| `PYQuant/data/yfinance_source.py` | 7 | 142 | 5% |
| `Quant/src/ipc/DbManager.cpp` | 16 | 325 | 5% |
| `PYQuant/report/account.py` | 8 | 163 | 5% |
| `Quant/src/strategy/SwingTrendOrders.cpp` | 24 | 490 | 5% |
| `PYQuant/tests/test_backtest_engine.py` | 6 | 123 | 5% |
| `PYQuant/core/proc_watch.py` | 21 | 431 | 5% |
| `Quant/src/risk/OrderGateState.cpp` | 12 | 247 | 5% |
| `PYQuant/kis/client.py` | 47 | 992 | 5% |
| `Quant/src/strategy/SurgeHoldExits.cpp` | 9 | 190 | 5% |
| `Quant/src/api/WsSocketWin.cpp` | 18 | 385 | 5% |
| `scripts/premarket_routine.py` | 4 | 86 | 5% |
| `Quant/src/core/KstTime.cpp` | 5 | 108 | 5% |
| `PYQuant/dashboard/backfill_series_a.py` | 9 | 197 | 5% |
| `Quant/src/api/KisClient.cpp` | 3 | 68 | 4% |
| `PYQuant/backtest/metrics.py` | 8 | 184 | 4% |
| `Quant/src/utils/AtomicFile.cpp` | 2 | 46 | 4% |
| `PYQuant/tests/test_stats.py` | 12 | 278 | 4% |
| `Quant/src/strategy/SurgeHoldBuys.cpp` | 12 | 284 | 4% |
| `Quant/src/core/CommandLine.cpp` | 7 | 172 | 4% |
| `Quant/src/core/ReplaySource.cpp` | 6 | 150 | 4% |
| `Quant/src/ipc/SharedWriteLock.cpp` | 1 | 25 | 4% |
| `Quant/src/utils/ThreadGuard.cpp` | 1 | 25 | 4% |
| `Quant/src/risk/PositionLedgerReconcile.cpp` | 14 | 351 | 4% |
| `Quant/src/regime/RegimeFeedScore.cpp` | 26 | 665 | 4% |
| `Quant/src/regime/RegimeFeed.cpp` | 17 | 436 | 4% |
| `Quant/src/api/KisIndex.cpp` | 11 | 285 | 4% |
| `Quant/src/risk/LedgerJournal.cpp` | 13 | 340 | 4% |
| `PYQuant/backtest/regime_scorer.py` | 6 | 157 | 4% |
| `Quant/src/regime/RegimeFeedParse.cpp` | 13 | 346 | 4% |
| `PYQuant/tools/ledger_recorder.py` | 6 | 162 | 4% |
| `Quant/src/core/WebSocketSlotPlan.cpp` | 5 | 137 | 4% |
| `Quant/src/universe/MarketBoard.cpp` | 23 | 637 | 4% |
| `Quant/src/core/StrategyTable.cpp` | 5 | 142 | 4% |
| `scripts/refresh_dashboard.py` | 7 | 199 | 4% |
| `Quant/src/ipc/OrderHistory.cpp` | 4 | 114 | 4% |
| `Quant/src/strategy/SurgeHoldStrategy.cpp` | 11 | 315 | 3% |
| `scripts/check_backtest.py` | 5 | 144 | 3% |
| `scripts/check_market_open.py` | 5 | 144 | 3% |
| `Quant/src/strategy/SwingTrendStrategy.cpp` | 18 | 520 | 3% |
| `scripts/notify_trades.py` | 24 | 701 | 3% |
| `PYQuant/tests/test_costs_golden.py` | 6 | 176 | 3% |
| `PYQuant/dashboard/build_dashboard.py` | 65 | 1915 | 3% |
| `Quant/src/ipc/FillChannel.cpp` | 7 | 211 | 3% |
| `scripts/stresstest_flow_profile.py` | 5 | 153 | 3% |
| `Quant/src/core/EngineSetters.cpp` | 10 | 308 | 3% |
| `Quant/src/ipc/DbConnection.cpp` | 11 | 339 | 3% |
| `Quant/src/ipc/OpsServer.cpp` | 24 | 770 | 3% |
| `PYQuant/backtest/report.py` | 9 | 289 | 3% |
| `scripts/gen_tuning_sheet.py` | 15 | 485 | 3% |
| `Quant/src/utils/EtfFilter.cpp` | 4 | 130 | 3% |
| `PYQuant/tools/load_injector.py` | 22 | 727 | 3% |
| `Quant/src/core/DataPoller.cpp` | 13 | 430 | 3% |
| `Quant/src/exchange/ZmqOrderFeed.cpp` | 21 | 701 | 3% |
| `Quant/src/ipc/SharedStrategyDictionary.cpp` | 4 | 136 | 3% |
| `PYQuant/tools/ledger_dump.py` | 13 | 455 | 3% |
| `PYQuant/tools/pit_universe_backfill.py` | 3 | 105 | 3% |
| `Quant/src/ipc/SharedSymbolDictionary.cpp` | 4 | 141 | 3% |
| `Quant/src/strategy/VwapPullbackLoader.cpp` | 18 | 635 | 3% |
| `PYQuant/tests/test_point_in_time.py` | 2 | 71 | 3% |
| `Quant/src/exchange/MatchingEngine.cpp` | 23 | 826 | 3% |
| `Quant/src/strategy/SwingTrendState.cpp` | 12 | 439 | 3% |
| `Quant/src/ipc/MarketFeedChannel.cpp` | 10 | 369 | 3% |
| `PYQuant/backtest/devscale_replay_rescue.py` | 20 | 760 | 3% |
| `scripts/build_plain_dashboard.py` | 5 | 194 | 3% |
| `scripts/build_review_entry.py` | 7 | 274 | 3% |
| `scripts/deploy_trader.py` | 8 | 316 | 3% |
| `Quant/src/core/LatencyTrace.cpp` | 7 | 278 | 3% |
| `Quant/src/core/BarAggregator.cpp` | 11 | 439 | 3% |
| `scripts/daily_trade_report.py` | 15 | 602 | 2% |
| `PYQuant/tools/bench_market_open.py` | 11 | 448 | 2% |
| `PYQuant/backtest/devscale_replay.py` | 18 | 736 | 2% |
| `Quant/src/strategy/FixedIntervalStrategy.cpp` | 2 | 83 | 2% |
| `Quant/src/strategy/SurgeHoldState.cpp` | 10 | 415 | 2% |
| `Quant/src/api/KisWsDecode.cpp` | 6 | 255 | 2% |
| `Quant/src/exchange/OrderWire.cpp` | 2 | 87 | 2% |
| `Quant/src/universe/ScoreWeight.cpp` | 3 | 133 | 2% |
| `scripts/deploy_guard.py` | 5 | 223 | 2% |
| `scripts/extract_swap_what_if.py` | 5 | 227 | 2% |
| `PYQuant/tools/minute_backfill.py` | 5 | 228 | 2% |
| `Quant/src/risk/PositionLedgerJournal.cpp` | 6 | 284 | 2% |
| `scripts/exit_ev.py` | 9 | 438 | 2% |
| `PYQuant/tools/log_report.py` | 12 | 586 | 2% |
| `Quant/src/strategy/TargetBasketStrategy.cpp` | 9 | 444 | 2% |
| `Quant/src/strategy/VwapPullbackStrategy.cpp` | 14 | 718 | 2% |
| `PYQuant/tools/sweep.py` | 2 | 103 | 2% |
| `PYQuant/backtest/stats.py` | 10 | 519 | 2% |
| `Quant/src/strategy/VwapPullbackRules.cpp` | 8 | 423 | 2% |
| `Quant/src/strategy/SeedPeakStore.cpp` | 5 | 270 | 2% |
| `Quant/src/ipc/DbCopyFormat.cpp` | 5 | 271 | 2% |
| `PYQuant/tools/full_universe_dump.py` | 2 | 109 | 2% |
| `PYQuant/strategy/supply_demand_rank.py` | 1 | 55 | 2% |
| `PYQuant/strategy/channel_breakout.py` | 1 | 56 | 2% |
| `PYQuant/tests/test_adjust_splits.py` | 1 | 57 | 2% |
| `PYQuant/features/regime_axes.py` | 11 | 635 | 2% |
| `Quant/src/api/KisRestDecode.cpp` | 9 | 521 | 2% |
| `PYQuant/data/point_in_time.py` | 1 | 58 | 2% |
| `PYQuant/tools/minute_backfill_pairs.py` | 1 | 63 | 2% |
| `PYQuant/live/basket_forward.py` | 5 | 330 | 2% |
| `PYQuant/tools/naver_research_fetch.py` | 9 | 595 | 2% |
| `scripts/capture_stats.py` | 3 | 199 | 2% |
| `scripts/trade_costs.py` | 2 | 136 | 1% |
| `Quant/src/core/ShardRoutes.cpp` | 1 | 69 | 1% |
| `scripts/backfill_fills_db.py` | 2 | 141 | 1% |
| `PYQuant/tools/naver_flow_backfill.py` | 4 | 286 | 1% |
| `PYQuant/tools/bench_recorder.py` | 3 | 219 | 1% |
| `Quant/src/core/FeedMux.cpp` | 7 | 518 | 1% |
| `PYQuant/strategy/base.py` | 1 | 75 | 1% |
| `PYQuant/tools/minute_strong_open_fetch.py` | 4 | 301 | 1% |
| `Quant/src/core/ReconcilePlan.cpp` | 1 | 76 | 1% |
| `PYQuant/tools/macro_ingest.py` | 7 | 550 | 1% |
| `Quant/src/strategy/TargetBasketPlan.cpp` | 4 | 320 | 1% |
| `PYQuant/backtest/ledger.py` | 1 | 82 | 1% |
| `Quant/src/utils/ThreadName.cpp` | 1 | 82 | 1% |
| `Quant/src/strategy/SwingTrendRules.cpp` | 3 | 253 | 1% |
| `PYQuant/ipc/operator.py` | 1 | 87 | 1% |
| `PYQuant/tools/load_latency_reader.py` | 3 | 263 | 1% |
| `scripts/exit_ev_dashboard.py` | 6 | 569 | 1% |
| `PYQuant/tools/dart_fin_history_fill.py` | 5 | 482 | 1% |
| `Quant/src/core/TickCapture.cpp` | 4 | 413 | 1% |
| `scripts/startup_to_order_timeline.py` | 2 | 218 | 1% |
| `PYQuant/tools/naver_bars_backfill.py` | 3 | 343 | 1% |
| `Quant/src/risk/ProtectiveOrders.cpp` | 2 | 237 | 1% |
| `PYQuant/naver/theme.py` | 1 | 121 | 1% |
| `Quant/src/strategy/DevScaleRules.cpp` | 2 | 251 | 1% |
| `scripts/restart_verify.py` | 2 | 260 | 1% |
| `PYQuant/tests/test_metrics.py` | 1 | 139 | 1% |
| `PYQuant/tools/minute_fullmarket_backfill.py` | 2 | 304 | 1% |
| `scripts/deploy_lock.py` | 1 | 152 | 1% |
| `PYQuant/tools/kind_delisted_fill.py` | 2 | 313 | 1% |
| `scripts/build_study_site.py` | 3 | 487 | 1% |
| `PYQuant/tools/dart_shares_history_fill.py` | 2 | 328 | 1% |
| `Quant/src/core/PaperExecutor.cpp` | 2 | 354 | 1% |
| `scripts/summarize_trading_day.py` | 1 | 216 | 0% |
| `PYQuant/tools/compare_ws_bars.py` | 1 | 243 | 0% |
| `scripts/kis_limit_check.py` | 1 | 246 | 0% |
| `PYQuant/features/fundamental.py` | 1 | 275 | 0% |
| `Quant/src/strategy/SurgeHoldPlan.cpp` | 1 | 317 | 0% |
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
| `Quant/src/core/MarketSession.cpp` | 0 | 90 | 0% |
| `Quant/src/core/SessionEndJudge.cpp` | 0 | 45 | 0% |
| `Quant/src/core/StrategyShard.cpp` | 0 | 12 | 0% |
| `Quant/src/core/TickSize.cpp` | 0 | 20 | 0% |
| `Quant/src/core/Types.cpp` | 0 | 79 | 0% |
| `Quant/src/core/UniverseExit.cpp` | 0 | 30 | 0% |
| `Quant/src/ipc/FillKey.cpp` | 0 | 15 | 0% |
| `Quant/src/ipc/OpsProtocol.cpp` | 0 | 119 | 0% |
| `Quant/src/risk/GateReasons.cpp` | 0 | 13 | 0% |
| `Quant/src/risk/ProtectiveRule.cpp` | 0 | 22 | 0% |
| `Quant/src/strategy/StrategyBase.cpp` | 0 | 122 | 0% |
| `Quant/src/universe/MaAlign.cpp` | 0 | 30 | 0% |
| `Quant/src/utils/JsonNode.cpp` | 0 | 19 | 0% |
| `Quant/src/utils/Utf8.cpp` | 0 | 100 | 0% |
| `scripts/stresstest_join_procwatch.py` | 0 | 97 | 0% |

태그 없는 연속 주석 블록(4줄 이상): 259개

| 파일 | 시작줄 | 길이 |
|---|---|---|
| `Quant/include/api/IOrderExecutor.h` | 31 | 5 |
| `Quant/include/api/KisClient.h` | 248 | 6 |
| `Quant/include/api/KisClient.h` | 323 | 5 |
| `Quant/include/api/KisEndpoints.h` | 2 | 6 |
| `Quant/include/api/KisErrorCodes.h` | 12 | 4 |
| `Quant/include/api/KisWebSocket.h` | 48 | 9 |
| `Quant/include/api/KisWsDecode.h` | 37 | 4 |
| `Quant/include/api/KisWsDecode.h` | 91 | 5 |
| `Quant/include/core/AppConfig.h` | 2 | 4 |
| `Quant/include/core/Engine.h` | 69 | 10 |
| `Quant/include/core/Engine.h` | 123 | 4 |
| `Quant/include/core/Engine.h` | 239 | 6 |
| `Quant/include/core/Engine.h` | 311 | 5 |
| `Quant/include/core/Engine.h` | 384 | 4 |
| `Quant/include/core/Engine.h` | 468 | 5 |
| `Quant/include/core/Engine.h` | 508 | 8 |
| `Quant/include/core/Engine.h` | 806 | 4 |
| `Quant/include/core/Engine.h` | 851 | 4 |
| `Quant/include/core/EngineFeedState.h` | 16 | 5 |
| `Quant/include/core/EngineFeedState.h` | 30 | 4 |
| `Quant/include/core/EngineFeedState.h` | 77 | 4 |
| `Quant/include/core/EngineOpsChannel.h` | 10 | 5 |
| `Quant/include/core/EngineOrderAge.h` | 5 | 5 |
| `Quant/include/core/EnginePipeline.h` | 21 | 5 |
| `Quant/include/core/EngineStrategyRegistry.h` | 12 | 5 |
| `Quant/include/core/LedgerReconciler.h` | 49 | 4 |
| `Quant/include/core/LedgerReconciler.h` | 140 | 6 |
| `Quant/include/core/MpscQueue.h` | 11 | 13 |
| `Quant/include/core/MutexQueue.h` | 8 | 8 |
| `Quant/include/core/PaperExecutor.h` | 75 | 4 |
| `Quant/include/core/ReconcilePlan.h` | 45 | 5 |
| `Quant/include/core/StrategyShard.h` | 91 | 4 |
| `Quant/include/core/StrategyTable.h` | 24 | 6 |
| `Quant/include/core/TickSize.h` | 4 | 4 |
| `Quant/include/core/Types.h` | 138 | 4 |
| `Quant/include/core/Types.h` | 429 | 4 |
| `Quant/include/core/WebSocketSlotPlan.h` | 8 | 10 |
| `Quant/include/exchange/OrderWire.h` | 1 | 8 |
| `Quant/include/ipc/ControlChannel.h` | 27 | 6 |
| `Quant/include/ipc/DbWorkerState.h` | 27 | 5 |
| `Quant/include/ipc/Heartbeat.h` | 20 | 5 |
| `Quant/include/ipc/Heartbeat.h` | 69 | 5 |
| `Quant/include/ipc/LedgerSnapshot.h` | 125 | 4 |
| `Quant/include/ipc/OrderRouter.h` | 107 | 6 |
| `Quant/include/ipc/OrderRouter.h` | 484 | 4 |
| `Quant/include/ipc/OrderRouterRecoveryState.h` | 25 | 6 |
| `Quant/include/ipc/SharedLayout.h` | 28 | 12 |
| `Quant/include/ipc/SharedLayout.h` | 50 | 4 |
| `Quant/include/ipc/ZmqBridge.h` | 71 | 4 |
| `Quant/include/risk/Money.h` | 5 | 8 |
| `Quant/include/risk/OrderGate.h` | 115 | 4 |
| `Quant/include/risk/OrderGate.h` | 191 | 10 |
| `Quant/include/risk/OrderGate.h` | 249 | 4 |
| `Quant/include/risk/OrderGate.h` | 309 | 6 |
| `Quant/include/risk/PositionLedger.h` | 244 | 4 |
| `Quant/include/risk/PositionLedger.h` | 259 | 4 |
| `Quant/include/risk/PositionLedger.h` | 281 | 6 |
| `Quant/include/risk/PositionLedger.h` | 293 | 7 |
| `Quant/include/risk/PositionLedger.h` | 308 | 4 |
| `Quant/include/risk/PositionLedger.h` | 331 | 5 |
| `Quant/include/risk/PositionLedger.h` | 338 | 6 |
| `Quant/include/risk/PositionLedger.h` | 352 | 4 |
| `Quant/include/risk/PositionLedger.h` | 360 | 4 |
| `Quant/include/risk/PositionLedger.h` | 365 | 4 |
| `Quant/include/risk/PositionLedgerTypes.h` | 68 | 4 |
| `Quant/include/risk/PositionLedgerTypes.h` | 82 | 4 |
| `Quant/include/risk/ProtectiveRule.h` | 12 | 4 |
| `Quant/include/strategy/DevScaleRules.h` | 2 | 15 |
| `Quant/include/strategy/DevScaleRules.h` | 37 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 30 | 35 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 77 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 92 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 108 | 5 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 163 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 317 | 4 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 386 | 11 |
| `Quant/include/strategy/DeviationScaleStrategy.h` | 405 | 4 |
| `Quant/include/strategy/FixedIntervalStrategy.h` | 12 | 6 |
| `Quant/include/strategy/IntradayBreakoutStrategy.h` | 122 | 4 |
| `Quant/include/strategy/MACrossStrategy.h` | 10 | 5 |
| `Quant/include/strategy/MACrossStrategy.h` | 18 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 54 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 91 | 5 |
| `Quant/include/strategy/StrategyBase.h` | 126 | 4 |
| `Quant/include/strategy/StrategyBase.h` | 222 | 6 |
| `Quant/include/strategy/StrategyBase.h` | 255 | 4 |
| `Quant/include/strategy/StrategyFactory.h` | 7 | 5 |
| `Quant/include/strategy/SurgeHoldPlan.h` | 2 | 9 |
| `Quant/include/strategy/TargetBasketPlan.h` | 2 | 9 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 25 | 14 |
| `Quant/include/strategy/ValueContraryStrategy.h` | 42 | 5 |
| `Quant/include/strategy/VwapPullbackRules.h` | 2 | 10 |
| `Quant/include/strategy/VwapPullbackStrategy.h` | 2 | 10 |
| `Quant/include/universe/ScoreWeight.h` | 37 | 23 |
| `Quant/include/universe/UniverseScanner.h` | 9 | 7 |
| `Quant/include/utils/EtfFilter.h` | 8 | 5 |
| `Quant/include/utils/EtfFilter.h` | 21 | 4 |
| `Quant/include/utils/EtfFilter.h` | 42 | 5 |
| `Quant/include/utils/GenerationCopy.h` | 2 | 5 |
| `Quant/include/utils/ThreadGuard.h` | 2 | 5 |
| `Quant/src/main.cpp` | 76 | 4 |
| `Quant/src/main.cpp` | 148 | 4 |
| `Quant/src/main.cpp` | 204 | 5 |
| `Quant/src/main.cpp` | 318 | 5 |
| `Quant/src/api/KisAccount.cpp` | 20 | 4 |
| `Quant/src/api/KisAuth.cpp` | 231 | 4 |
| `Quant/src/api/KisMarket.cpp` | 72 | 4 |
| `Quant/src/api/KisMarket.cpp` | 530 | 6 |
| `Quant/src/api/KisOrder.cpp` | 16 | 4 |
| `Quant/src/api/KisOrder.cpp` | 76 | 7 |
| `Quant/src/api/KisTransport.cpp` | 25 | 4 |
| `Quant/src/api/KisTransport.cpp` | 30 | 4 |
| `Quant/src/api/KisTransport.cpp` | 92 | 6 |
| `Quant/src/api/KisTransport.cpp` | 184 | 6 |
| `Quant/src/api/KisTransport.cpp` | 512 | 6 |
| `Quant/src/api/KisTransport.cpp` | 575 | 11 |
| `Quant/src/api/KisUniverse.cpp` | 17 | 6 |
| `Quant/src/api/KisUniverse.cpp` | 333 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 385 | 9 |
| `Quant/src/api/KisUniverse.cpp` | 418 | 5 |
| `Quant/src/api/KisUniverse.cpp` | 628 | 5 |
| `Quant/src/api/KisWebSocketParse.cpp` | 136 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 112 | 4 |
| `Quant/src/api/WebSocketClient.cpp` | 121 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 147 | 5 |
| `Quant/src/api/WebSocketClient.cpp` | 286 | 4 |
| `Quant/src/api/WsSocket.h` | 22 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 63 | 4 |
| `Quant/src/api/WsSocketPosix.cpp` | 197 | 5 |
| `Quant/src/api/WsSocketPosix.cpp` | 213 | 5 |
| `Quant/src/api/WsSocketWin.cpp` | 297 | 4 |
| `Quant/src/core/ControlPlane.cpp` | 1 | 9 |
| `Quant/src/core/ControlPlane.cpp` | 176 | 4 |
| `Quant/src/core/EngineControlThread.cpp` | 1 | 11 |
| `Quant/src/core/EngineControlThread.cpp` | 63 | 7 |
| `Quant/src/core/EngineControlThread.cpp` | 77 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 1 | 7 |
| `Quant/src/core/EngineDataThread.cpp` | 63 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 284 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 484 | 5 |
| `Quant/src/core/EngineDataThread.cpp` | 647 | 4 |
| `Quant/src/core/EngineDataThread.cpp` | 653 | 4 |
| `Quant/src/core/EngineFillThread.cpp` | 1 | 6 |
| `Quant/src/core/EngineLayout.cpp` | 1 | 7 |
| `Quant/src/core/EngineLedgerThread.cpp` | 1 | 5 |
| `Quant/src/core/EngineOrderThread.cpp` | 1 | 8 |
| `Quant/src/core/EngineOrderThread.cpp` | 454 | 4 |
| `Quant/src/core/EngineRegime.cpp` | 1 | 8 |
| `Quant/src/core/EngineStartupSetup.cpp` | 40 | 5 |
| `Quant/src/core/EngineStartupSetup.cpp` | 92 | 5 |
| `Quant/src/core/EngineStrategyThread.cpp` | 1 | 9 |
| `Quant/src/core/EngineStrategyThread.cpp` | 154 | 5 |
| `Quant/src/core/EngineStrategyThread.cpp` | 183 | 5 |
| `Quant/src/core/EngineSymbols.cpp` | 1 | 9 |
| `Quant/src/core/LedgerReconciler.cpp` | 17 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 57 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 102 | 5 |
| `Quant/src/core/LedgerReconciler.cpp` | 218 | 4 |
| `Quant/src/core/LedgerReconciler.cpp` | 253 | 4 |
| `Quant/src/core/OrderRateLimiter.cpp` | 36 | 4 |
| `Quant/src/core/RegimeFileJudge.cpp` | 101 | 4 |
| `Quant/src/core/SignalDispatcher.cpp` | 214 | 4 |
| `Quant/src/ipc/DbConnection.cpp` | 113 | 4 |
| `Quant/src/ipc/DbManager.cpp` | 78 | 4 |
| `Quant/src/ipc/DbWorkers.cpp` | 353 | 6 |
| `Quant/src/ipc/OrderRouterBlockedSell.cpp` | 21 | 7 |
| `Quant/src/ipc/OrderRouterBlockedSell.cpp` | 32 | 7 |
| `Quant/src/ipc/OrderRouterBlockedSell.cpp` | 189 | 4 |
| `Quant/src/ipc/OrderRouterBlockedSell.cpp` | 238 | 5 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 126 | 6 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 136 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 153 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 236 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 319 | 6 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 344 | 16 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 482 | 4 |
| `Quant/src/ipc/OrderRouterFill.cpp` | 722 | 4 |
| `Quant/src/ipc/OrderRouterModify.cpp` | 330 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 211 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 280 | 6 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 318 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 560 | 4 |
| `Quant/src/ipc/OrderRouterReconcile.cpp` | 773 | 7 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 20 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 249 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 261 | 4 |
| `Quant/src/ipc/OrderRouterSubmit.cpp` | 282 | 4 |
| `Quant/src/ipc/ZmqBridge.cpp` | 191 | 4 |
| `Quant/src/risk/DisplacementDesk.cpp` | 46 | 6 |
| `Quant/src/risk/OrderGate.cpp` | 29 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 68 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 102 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 158 | 10 |
| `Quant/src/risk/OrderGate.cpp` | 203 | 8 |
| `Quant/src/risk/OrderGate.cpp` | 352 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 595 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 654 | 5 |
| `Quant/src/risk/OrderGate.cpp` | 665 | 7 |
| `Quant/src/risk/OrderGate.cpp` | 685 | 4 |
| `Quant/src/risk/OrderGate.cpp` | 824 | 4 |
| (이하 59개 생략) | | |

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
| `cite-gate.ps1` | [!] 안 됨 | 예 |
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

파일 88개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: 2026-10-09

- 추가 2개: `.claude/hooks/cite-gate.ps1`, `.claude/hooks/cite_gate.py`
- 삭제 0개
- 변경 5개: `.claude/PROJECT_FACTS.md`, `.claude/commit-gate.state`, `.claude/hooks/stop-gates.ps1`, `.claude/settings.json`, `.claude/sync-gate.state`
