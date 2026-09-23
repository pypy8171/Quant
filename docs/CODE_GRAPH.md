# 코드 의존 그래프 (Code Graph)

> 자동 생성물. 손편집 금지 — 코드가 바뀌면 `py ../quant-devtools/gen_code_graph.py` 로 재생성한다.
> `Quant/include`·`Quant/src` 의 로컬 `#include "..."` 관계에서 뽑았다. 표준/외부 헤더는 제외.

## 모듈 의존 그래프

화살표 A→B 는 "모듈 A가 모듈 B의 헤더를 include 한다". 숫자는 그런 include 파일 쌍의 수(의존 강도).

```mermaid
graph LR
  main[main]
  modes[modes]
  core[core]
  strategy[strategy]
  api[api]
  universe[universe]
  risk[risk]
  ipc[ipc]
  utils[utils]
  api -->|11| core
  api -->|7| utils
  core -->|11| api
  core -->|9| ipc
  core -->|7| risk
  core -->|3| strategy
  core -->|12| utils
  ipc -->|2| api
  ipc -->|13| core
  ipc --> risk
  ipc -->|5| utils
  main -->|4| core
  main --> modes
  main --> strategy
  main --> utils
  modes -->|3| api
  modes -->|3| core
  modes --> ipc
  modes -->|2| utils
  risk -->|6| core
  risk --> ipc
  risk -->|2| utils
  strategy -->|5| api
  strategy -->|23| core
  strategy --> risk
  strategy -->|3| universe
  strategy -->|11| utils
  universe --> api
  universe -->|4| core
  universe -->|3| utils
  utils --> core
```

## 공용 허브 헤더 (재빌드 팬아웃)

유입(누가 나를 include)이 많은 헤더. 한 줄만 바꿔도 아래 개수만큼 번역단위가 재컴파일된다.
증분빌드를 줄이려면 이 헤더를 얇게 유지한다(무거운 include를 전방선언/pimpl로 분리).

| 헤더 | 유입 수 |
|---|---|
| `core/Types.h` | 38 |
| `utils/Logger.h` | 31 |
| `core/KstTime.h` | 17 |
| `core/SymbolTable.h` | 15 |
| `strategy/StrategyBase.h` | 15 |
| `api/KisClient.h` | 13 |
| `core/MarketSession.h` | 9 |
| `core/WakeGate.h` | 9 |

## 파일 단위 상세

각 소스/헤더가 어떤 로컬 헤더를 include 하는지. 유입이 많은 노드(`utils/Logger.h`·`core/Types.h`)가 공용 허브다.

```mermaid
graph LR
  subgraph api
    n_api_IMarketDataSource_h["api/IMarketDataSource.h"]
    n_api_IOrderExecutor_h["api/IOrderExecutor.h"]
    n_api_KisAccount_cpp["api/KisAccount.cpp"]
    n_api_KisClient_cpp["api/KisClient.cpp"]
    n_api_KisClient_h["api/KisClient.h"]
    n_api_KisClientInternal_h["api/KisClientInternal.h"]
    n_api_KisIndex_cpp["api/KisIndex.cpp"]
    n_api_KisMarket_cpp["api/KisMarket.cpp"]
    n_api_KisOrder_cpp["api/KisOrder.cpp"]
    n_api_KisRestDecode_cpp["api/KisRestDecode.cpp"]
    n_api_KisRestDecode_h["api/KisRestDecode.h"]
    n_api_KisTransport_cpp["api/KisTransport.cpp"]
    n_api_KisWebSocket_cpp["api/KisWebSocket.cpp"]
    n_api_KisWebSocket_h["api/KisWebSocket.h"]
    n_api_KisWsDecode_cpp["api/KisWsDecode.cpp"]
    n_api_KisWsDecode_h["api/KisWsDecode.h"]
    n_api_WebSocketClient_cpp["api/WebSocketClient.cpp"]
    n_api_WsSocketPosix_cpp["api/WsSocketPosix.cpp"]
    n_api_WsSocketWin_cpp["api/WsSocketWin.cpp"]
  end
  subgraph core
    n_core_AppConfig_cpp["core/AppConfig.cpp"]
    n_core_AppConfig_h["core/AppConfig.h"]
    n_core_BarAggregator_cpp["core/BarAggregator.cpp"]
    n_core_BarAggregator_h["core/BarAggregator.h"]
    n_core_CommandLine_cpp["core/CommandLine.cpp"]
    n_core_DataPoller_cpp["core/DataPoller.cpp"]
    n_core_DataPoller_h["core/DataPoller.h"]
    n_core_Engine_cpp["core/Engine.cpp"]
    n_core_Engine_h["core/Engine.h"]
    n_core_EngineConfigure_cpp["core/EngineConfigure.cpp"]
    n_core_FeedMux_cpp["core/FeedMux.cpp"]
    n_core_FeedMux_h["core/FeedMux.h"]
    n_core_FeedSupervisor_cpp["core/FeedSupervisor.cpp"]
    n_core_HttpQuoteFeed_cpp["core/HttpQuoteFeed.cpp"]
    n_core_HttpQuoteFeed_h["core/HttpQuoteFeed.h"]
    n_core_IFeedSource_cpp["core/IFeedSource.cpp"]
    n_core_IFeedSource_h["core/IFeedSource.h"]
    n_core_KstTime_cpp["core/KstTime.cpp"]
    n_core_LatencyTrace_cpp["core/LatencyTrace.cpp"]
    n_core_LatencyTrace_h["core/LatencyTrace.h"]
    n_core_LedgerReconciler_cpp["core/LedgerReconciler.cpp"]
    n_core_LedgerReconciler_h["core/LedgerReconciler.h"]
    n_core_MarketSession_cpp["core/MarketSession.cpp"]
    n_core_OrderRateLimiter_cpp["core/OrderRateLimiter.cpp"]
    n_core_OrderRateLimiter_h["core/OrderRateLimiter.h"]
    n_core_PaperExecutor_cpp["core/PaperExecutor.cpp"]
    n_core_PaperExecutor_h["core/PaperExecutor.h"]
    n_core_PrefetchPool_h["core/PrefetchPool.h"]
    n_core_ReconcilePlan_cpp["core/ReconcilePlan.cpp"]
    n_core_ReconcilePlan_h["core/ReconcilePlan.h"]
    n_core_RegimeFileJudge_cpp["core/RegimeFileJudge.cpp"]
    n_core_RegimeFileJudge_h["core/RegimeFileJudge.h"]
    n_core_ReplaySource_cpp["core/ReplaySource.cpp"]
    n_core_ReplaySource_h["core/ReplaySource.h"]
    n_core_SessionEndJudge_cpp["core/SessionEndJudge.cpp"]
    n_core_ShardMatrix_h["core/ShardMatrix.h"]
    n_core_ShardRoutes_cpp["core/ShardRoutes.cpp"]
    n_core_ShardRoutes_h["core/ShardRoutes.h"]
    n_core_SignalDispatcher_cpp["core/SignalDispatcher.cpp"]
    n_core_SignalDispatcher_h["core/SignalDispatcher.h"]
    n_core_StrategyRouter_h["core/StrategyRouter.h"]
    n_core_StrategyShard_cpp["core/StrategyShard.cpp"]
    n_core_StrategyShard_h["core/StrategyShard.h"]
    n_core_StrategyTable_cpp["core/StrategyTable.cpp"]
    n_core_SymbolTable_cpp["core/SymbolTable.cpp"]
    n_core_TickCapture_cpp["core/TickCapture.cpp"]
    n_core_TickCapture_h["core/TickCapture.h"]
    n_core_TickSize_cpp["core/TickSize.cpp"]
    n_core_TickSize_h["core/TickSize.h"]
    n_core_Types_cpp["core/Types.cpp"]
    n_core_Types_h["core/Types.h"]
    n_core_UniverseExit_cpp["core/UniverseExit.cpp"]
    n_core_UniverseExit_h["core/UniverseExit.h"]
    n_core_WakeGate_cpp["core/WakeGate.cpp"]
  end
  subgraph ipc
    n_ipc_ControlChannel_cpp["ipc/ControlChannel.cpp"]
    n_ipc_ControlChannel_h["ipc/ControlChannel.h"]
    n_ipc_FillKey_cpp["ipc/FillKey.cpp"]
    n_ipc_Heartbeat_cpp["ipc/Heartbeat.cpp"]
    n_ipc_LedgerSnapshot_cpp["ipc/LedgerSnapshot.cpp"]
    n_ipc_LedgerSnapshot_h["ipc/LedgerSnapshot.h"]
    n_ipc_MarketFeedChannel_cpp["ipc/MarketFeedChannel.cpp"]
    n_ipc_MarketFeedChannel_h["ipc/MarketFeedChannel.h"]
    n_ipc_OpsProtocol_cpp["ipc/OpsProtocol.cpp"]
    n_ipc_OpsServer_cpp["ipc/OpsServer.cpp"]
    n_ipc_OpsServer_h["ipc/OpsServer.h"]
    n_ipc_OrderChannel_cpp["ipc/OrderChannel.cpp"]
    n_ipc_OrderChannel_h["ipc/OrderChannel.h"]
    n_ipc_OrderRouter_cpp["ipc/OrderRouter.cpp"]
    n_ipc_OrderRouter_h["ipc/OrderRouter.h"]
    n_ipc_SharedLayout_cpp["ipc/SharedLayout.cpp"]
    n_ipc_SharedLayout_h["ipc/SharedLayout.h"]
    n_ipc_SharedRegion_cpp["ipc/SharedRegion.cpp"]
    n_ipc_SharedStrategyDictionary_cpp["ipc/SharedStrategyDictionary.cpp"]
    n_ipc_SharedStrategyDictionary_h["ipc/SharedStrategyDictionary.h"]
    n_ipc_SharedSymbolDictionary_cpp["ipc/SharedSymbolDictionary.cpp"]
    n_ipc_SharedSymbolDictionary_h["ipc/SharedSymbolDictionary.h"]
    n_ipc_SharedWriteLock_cpp["ipc/SharedWriteLock.cpp"]
    n_ipc_ZmqBridge_cpp["ipc/ZmqBridge.cpp"]
    n_ipc_ZmqBridge_h["ipc/ZmqBridge.h"]
  end
  subgraph main
    n_main_cpp["main.cpp"]
  end
  subgraph modes
    n_modes_Monitors_cpp["modes/Monitors.cpp"]
    n_modes_Monitors_h["modes/Monitors.h"]
  end
  subgraph risk
    n_risk_DisplacementDesk_cpp["risk/DisplacementDesk.cpp"]
    n_risk_DisplacementDesk_h["risk/DisplacementDesk.h"]
    n_risk_GateReasons_cpp["risk/GateReasons.cpp"]
    n_risk_LedgerJournal_cpp["risk/LedgerJournal.cpp"]
    n_risk_OrderGate_cpp["risk/OrderGate.cpp"]
    n_risk_OrderGate_h["risk/OrderGate.h"]
    n_risk_ProtectiveOrders_cpp["risk/ProtectiveOrders.cpp"]
    n_risk_ProtectiveOrders_h["risk/ProtectiveOrders.h"]
    n_risk_ProtectiveRule_cpp["risk/ProtectiveRule.cpp"]
    n_risk_ProtectiveRule_h["risk/ProtectiveRule.h"]
  end
  subgraph strategy
    n_strategy_DevScaleRules_cpp["strategy/DevScaleRules.cpp"]
    n_strategy_DevScaleRules_h["strategy/DevScaleRules.h"]
    n_strategy_DeviationScaleStrategy_cpp["strategy/DeviationScaleStrategy.cpp"]
    n_strategy_DeviationScaleStrategy_h["strategy/DeviationScaleStrategy.h"]
    n_strategy_FixedIntervalStrategy_cpp["strategy/FixedIntervalStrategy.cpp"]
    n_strategy_FixedIntervalStrategy_h["strategy/FixedIntervalStrategy.h"]
    n_strategy_IntradayBreakoutStrategy_cpp["strategy/IntradayBreakoutStrategy.cpp"]
    n_strategy_IntradayBreakoutStrategy_h["strategy/IntradayBreakoutStrategy.h"]
    n_strategy_MACrossStrategy_cpp["strategy/MACrossStrategy.cpp"]
    n_strategy_MACrossStrategy_h["strategy/MACrossStrategy.h"]
    n_strategy_MarketMakingStrategy_cpp["strategy/MarketMakingStrategy.cpp"]
    n_strategy_MarketMakingStrategy_h["strategy/MarketMakingStrategy.h"]
    n_strategy_MomentumStrategy_cpp["strategy/MomentumStrategy.cpp"]
    n_strategy_MomentumStrategy_h["strategy/MomentumStrategy.h"]
    n_strategy_PriceTargetStrategy_cpp["strategy/PriceTargetStrategy.cpp"]
    n_strategy_PriceTargetStrategy_h["strategy/PriceTargetStrategy.h"]
    n_strategy_SeedPeakStore_cpp["strategy/SeedPeakStore.cpp"]
    n_strategy_SeedPeakStore_h["strategy/SeedPeakStore.h"]
    n_strategy_StrategyBase_cpp["strategy/StrategyBase.cpp"]
    n_strategy_StrategyBase_h["strategy/StrategyBase.h"]
    n_strategy_StrategyFactory_cpp["strategy/StrategyFactory.cpp"]
    n_strategy_StrategyFactory_h["strategy/StrategyFactory.h"]
    n_strategy_SupplyDemandPullbackStrategy_cpp["strategy/SupplyDemandPullbackStrategy.cpp"]
    n_strategy_SupplyDemandPullbackStrategy_h["strategy/SupplyDemandPullbackStrategy.h"]
    n_strategy_TargetBasketPlan_cpp["strategy/TargetBasketPlan.cpp"]
    n_strategy_TargetBasketPlan_h["strategy/TargetBasketPlan.h"]
    n_strategy_TargetBasketStrategy_cpp["strategy/TargetBasketStrategy.cpp"]
    n_strategy_TargetBasketStrategy_h["strategy/TargetBasketStrategy.h"]
    n_strategy_ThemeStrategy_cpp["strategy/ThemeStrategy.cpp"]
    n_strategy_ThemeStrategy_h["strategy/ThemeStrategy.h"]
    n_strategy_ValueContraryStrategy_cpp["strategy/ValueContraryStrategy.cpp"]
    n_strategy_ValueContraryStrategy_h["strategy/ValueContraryStrategy.h"]
  end
  subgraph universe
    n_universe_MaAlign_cpp["universe/MaAlign.cpp"]
    n_universe_ScoreWeight_cpp["universe/ScoreWeight.cpp"]
    n_universe_ScoreWeight_h["universe/ScoreWeight.h"]
    n_universe_UniverseScanner_cpp["universe/UniverseScanner.cpp"]
    n_universe_UniverseScanner_h["universe/UniverseScanner.h"]
  end
  subgraph utils
    n_utils_EtfFilter_cpp["utils/EtfFilter.cpp"]
    n_utils_JsonNode_cpp["utils/JsonNode.cpp"]
    n_utils_Logger_cpp["utils/Logger.cpp"]
    n_utils_ThreadName_cpp["utils/ThreadName.cpp"]
    n_utils_Utf8_cpp["utils/Utf8.cpp"]
  end
  n_api_IMarketDataSource_h --> n_core_Types_h
  n_api_IOrderExecutor_h --> n_core_Types_h
  n_api_KisAccount_cpp --> n_api_KisRestDecode_h
  n_api_KisClient_cpp --> n_api_KisClient_h
  n_api_KisClient_h --> n_api_IMarketDataSource_h
  n_api_KisClient_h --> n_api_IOrderExecutor_h
  n_api_KisClient_h --> n_api_KisEndpoints_h
  n_api_KisClient_h --> n_api_KisResult_h
  n_api_KisClient_h --> n_api_KisTypes_h
  n_api_KisClient_h --> n_core_Types_h
  n_api_KisClientInternal_h --> n_api_KisClient_h
  n_api_KisClientInternal_h --> n_api_KisErrorCodes_h
  n_api_KisClientInternal_h --> n_utils_EtfFilter_h
  n_api_KisClientInternal_h --> n_utils_Logger_h
  n_api_KisIndex_cpp --> n_api_KisRestDecode_h
  n_api_KisMarket_cpp --> n_api_KisRestDecode_h
  n_api_KisOrder_cpp --> n_core_KstTime_h
  n_api_KisOrder_cpp --> n_utils_JsonNode_h
  n_api_KisRestDecode_cpp --> n_api_KisRestDecode_h
  n_api_KisRestDecode_h --> n_api_KisTypes_h
  n_api_KisRestDecode_h --> n_core_KstTime_h
  n_api_KisRestDecode_h --> n_core_Types_h
  n_api_KisTransport_cpp --> n_api_HttpGet_h
  n_api_KisWebSocket_cpp --> n_api_KisWebSocket_h
  n_api_KisWebSocket_h --> n_api_KisClient_h
  n_api_KisWebSocket_h --> n_api_KisWsDecode_h
  n_api_KisWebSocket_h --> n_core_IFeedSource_h
  n_api_KisWebSocket_h --> n_core_Types_h
  n_api_KisWsDecode_cpp --> n_api_KisWsDecode_h
  n_api_KisWsDecode_h --> n_core_MarketSession_h
  n_api_KisWsDecode_h --> n_core_Types_h
  n_api_WebSocketClient_cpp --> n_api_KisEndpoints_h
  n_api_WebSocketClient_cpp --> n_api_KisWebSocket_h
  n_api_WebSocketClient_cpp --> n_api_KisWsDecode_h
  n_api_WebSocketClient_cpp --> n_core_WakeGate_h
  n_api_WebSocketClient_cpp --> n_utils_Logger_h
  n_api_WebSocketClient_cpp --> n_utils_ThreadName_h
  n_api_WsSocketPosix_cpp --> n_utils_Logger_h
  n_api_WsSocketWin_cpp --> n_utils_Logger_h
  n_core_AppConfig_cpp --> n_core_AppConfig_h
  n_core_AppConfig_cpp --> n_utils_JsonNode_h
  n_core_AppConfig_cpp --> n_utils_Logger_h
  n_core_AppConfig_h --> n_api_KisClient_h
  n_core_AppConfig_h --> n_core_RegimeFileJudge_h
  n_core_AppConfig_h --> n_core_Types_h
  n_core_AppConfig_h --> n_risk_OrderGate_h
  n_core_BarAggregator_cpp --> n_core_BarAggregator_h
  n_core_BarAggregator_cpp --> n_core_KstTime_h
  n_core_BarAggregator_h --> n_core_Types_h
  n_core_CommandLine_cpp --> n_core_CommandLine_h
  n_core_DataPoller_cpp --> n_core_DataPoller_h
  n_core_DataPoller_cpp --> n_core_KstTime_h
  n_core_DataPoller_cpp --> n_utils_Logger_h
  n_core_DataPoller_h --> n_core_Types_h
  n_core_Engine_cpp --> n_core_Engine_h
  n_core_Engine_cpp --> n_core_KstTime_h
  n_core_Engine_cpp --> n_core_LatencyTrace_h
  n_core_Engine_cpp --> n_core_ReconcilePlan_h
  n_core_Engine_cpp --> n_core_UniverseExit_h
  n_core_Engine_cpp --> n_risk_DisplacementDesk_h
  n_core_Engine_cpp --> n_utils_Logger_h
  n_core_Engine_cpp --> n_utils_ThreadName_h
  n_core_Engine_cpp --> n_utils_Utf8_h
  n_core_Engine_h --> n_api_KisClient_h
  n_core_Engine_h --> n_api_KisWebSocket_h
  n_core_Engine_h --> n_core_CommandLine_h
  n_core_Engine_h --> n_core_DataPoller_h
  n_core_Engine_h --> n_core_FeedMux_h
  n_core_Engine_h --> n_core_FeedSupervisor_h
  n_core_Engine_h --> n_core_LatencyTrace_h
  n_core_Engine_h --> n_core_LedgerReconciler_h
  n_core_Engine_h --> n_core_MpscQueue_h
  n_core_Engine_h --> n_core_OrderRateLimiter_h
  n_core_Engine_h --> n_core_PaperExecutor_h
  n_core_Engine_h --> n_core_PrefetchPool_h
  n_core_Engine_h --> n_core_RegimeFileJudge_h
  n_core_Engine_h --> n_core_ReplaySource_h
  n_core_Engine_h --> n_core_RingBuffer_h
  n_core_Engine_h --> n_core_SessionEndJudge_h
  n_core_Engine_h --> n_core_ShardRoutes_h
  n_core_Engine_h --> n_core_SignalDispatcher_h
  n_core_Engine_h --> n_core_StrategyRouter_h
  n_core_Engine_h --> n_core_StrategyShard_h
  n_core_Engine_h --> n_core_SymbolTable_h
  n_core_Engine_h --> n_core_TickCapture_h
  n_core_Engine_h --> n_core_Types_h
  n_core_Engine_h --> n_core_WakeGate_h
  n_core_Engine_h --> n_ipc_ControlChannel_h
  n_core_Engine_h --> n_ipc_Heartbeat_h
  n_core_Engine_h --> n_ipc_LedgerSnapshot_h
  n_core_Engine_h --> n_ipc_OpsServer_h
  n_core_Engine_h --> n_ipc_OrderChannel_h
  n_core_Engine_h --> n_ipc_OrderRouter_h
  n_core_Engine_h --> n_ipc_SharedLayout_h
  n_core_Engine_h --> n_ipc_ZmqBridge_h
  n_core_Engine_h --> n_risk_OrderGate_h
  n_core_Engine_h --> n_risk_ProtectiveOrders_h
  n_core_Engine_h --> n_strategy_StrategyBase_h
  n_core_EngineConfigure_cpp --> n_core_AppConfig_h
  n_core_EngineConfigure_cpp --> n_core_Engine_h
  n_core_EngineConfigure_cpp --> n_utils_Logger_h
  n_core_FeedMux_cpp --> n_core_FeedMux_h
  n_core_FeedMux_h --> n_core_IFeedSource_h
  n_core_FeedMux_h --> n_core_RingBuffer_h
  n_core_FeedMux_h --> n_core_Types_h
  n_core_FeedMux_h --> n_core_WakeGate_h
  n_core_FeedSupervisor_cpp --> n_core_FeedSupervisor_h
  n_core_HttpQuoteFeed_cpp --> n_api_HttpGet_h
  n_core_HttpQuoteFeed_cpp --> n_core_HttpQuoteFeed_h
  n_core_HttpQuoteFeed_cpp --> n_utils_Logger_h
  n_core_HttpQuoteFeed_h --> n_core_Types_h
  n_core_IFeedSource_cpp --> n_core_IFeedSource_h
  n_core_IFeedSource_h --> n_core_Types_h
  n_core_KstTime_cpp --> n_core_KstTime_h
  n_core_LatencyTrace_cpp --> n_core_LatencyTrace_h
  n_core_LatencyTrace_h --> n_core_Types_h
  n_core_LedgerReconciler_cpp --> n_core_LedgerReconciler_h
  n_core_LedgerReconciler_cpp --> n_utils_Logger_h
  n_core_LedgerReconciler_h --> n_api_KisResult_h
  n_core_LedgerReconciler_h --> n_api_KisTypes_h
  n_core_LedgerReconciler_h --> n_core_KstTime_h
  n_core_LedgerReconciler_h --> n_core_ReconcilePlan_h
  n_core_LedgerReconciler_h --> n_risk_OrderGate_h
  n_core_MarketSession_cpp --> n_core_MarketSession_h
  n_core_OrderRateLimiter_cpp --> n_api_KisErrorCodes_h
  n_core_OrderRateLimiter_cpp --> n_core_OrderRateLimiter_h
  n_core_OrderRateLimiter_cpp --> n_risk_GateReasons_h
  n_core_OrderRateLimiter_cpp --> n_utils_Logger_h
  n_core_OrderRateLimiter_h --> n_core_Types_h
  n_core_PaperExecutor_cpp --> n_core_PaperExecutor_h
  n_core_PaperExecutor_h --> n_api_IOrderExecutor_h
  n_core_PaperExecutor_h --> n_api_KisErrorCodes_h
  n_core_PaperExecutor_h --> n_api_KisResult_h
  n_core_PaperExecutor_h --> n_api_KisTypes_h
  n_core_PaperExecutor_h --> n_core_MarketSession_h
  n_core_PaperExecutor_h --> n_core_SymbolTable_h
  n_core_PaperExecutor_h --> n_core_Types_h
  n_core_PrefetchPool_h --> n_core_WakeGate_h
  n_core_PrefetchPool_h --> n_utils_ThreadName_h
  n_core_ReconcilePlan_cpp --> n_core_ReconcilePlan_h
  n_core_ReconcilePlan_h --> n_core_SymbolTable_h
  n_core_RegimeFileJudge_cpp --> n_core_RegimeFileJudge_h
  n_core_RegimeFileJudge_h --> n_core_KstTime_h
  n_core_RegimeFileJudge_h --> n_core_Types_h
  n_core_ReplaySource_cpp --> n_core_ReplaySource_h
  n_core_ReplaySource_h --> n_core_IFeedSource_h
  n_core_ReplaySource_h --> n_core_TickCapture_h
  n_core_SessionEndJudge_cpp --> n_core_SessionEndJudge_h
  n_core_ShardMatrix_h --> n_core_RingBuffer_h
  n_core_ShardMatrix_h --> n_core_SymbolTable_h
  n_core_ShardRoutes_cpp --> n_core_ShardRoutes_h
  n_core_ShardRoutes_h --> n_core_SymbolTable_h
  n_core_SignalDispatcher_cpp --> n_core_LatencyTrace_h
  n_core_SignalDispatcher_cpp --> n_core_SignalDispatcher_h
  n_core_SignalDispatcher_cpp --> n_utils_Logger_h
  n_core_SignalDispatcher_h --> n_core_Types_h
  n_core_SignalDispatcher_h --> n_ipc_LedgerSnapshot_h
  n_core_SignalDispatcher_h --> n_risk_OrderGate_h
  n_core_StrategyRouter_h --> n_core_SymbolTable_h
  n_core_StrategyRouter_h --> n_strategy_StrategyBase_h
  n_core_StrategyShard_cpp --> n_core_StrategyShard_h
  n_core_StrategyShard_h --> n_core_ShardMatrix_h
  n_core_StrategyShard_h --> n_core_StrategyRouter_h
  n_core_StrategyShard_h --> n_core_SymbolTable_h
  n_core_StrategyShard_h --> n_core_Types_h
  n_core_StrategyShard_h --> n_core_WakeGate_h
  n_core_StrategyShard_h --> n_strategy_StrategyBase_h
  n_core_StrategyTable_cpp --> n_core_StrategyTable_h
  n_core_SymbolTable_cpp --> n_core_SymbolTable_h
  n_core_TickCapture_cpp --> n_core_TickCapture_h
  n_core_TickCapture_h --> n_core_MarketSession_h
  n_core_TickCapture_h --> n_core_MpscQueue_h
  n_core_TickCapture_h --> n_core_Types_h
  n_core_TickCapture_h --> n_core_WakeGate_h
  n_core_TickSize_cpp --> n_core_TickSize_h
  n_core_TickSize_h --> n_core_Types_h
  n_core_Types_cpp --> n_core_Types_h
  n_core_Types_h --> n_core_StrategyTable_h
  n_core_Types_h --> n_core_SymbolTable_h
  n_core_UniverseExit_cpp --> n_core_UniverseExit_h
  n_core_UniverseExit_h --> n_core_SymbolTable_h
  n_core_WakeGate_cpp --> n_core_WakeGate_h
  n_ipc_ControlChannel_cpp --> n_ipc_ControlChannel_h
  n_ipc_ControlChannel_h --> n_core_Types_h
  n_ipc_FillKey_cpp --> n_ipc_FillKey_h
  n_ipc_Heartbeat_cpp --> n_ipc_Heartbeat_h
  n_ipc_LedgerSnapshot_cpp --> n_ipc_LedgerSnapshot_h
  n_ipc_LedgerSnapshot_h --> n_core_SymbolTable_h
  n_ipc_MarketFeedChannel_cpp --> n_ipc_MarketFeedChannel_h
  n_ipc_MarketFeedChannel_h --> n_core_Types_h
  n_ipc_MarketFeedChannel_h --> n_ipc_SharedSpscRing_h
  n_ipc_OpsProtocol_cpp --> n_ipc_OpsProtocol_h
  n_ipc_OpsServer_cpp --> n_ipc_OpsServer_h
  n_ipc_OpsServer_cpp --> n_utils_Logger_h
  n_ipc_OpsServer_cpp --> n_utils_ThreadName_h
  n_ipc_OpsServer_h --> n_ipc_OpsProtocol_h
  n_ipc_OrderChannel_cpp --> n_ipc_OrderChannel_h
  n_ipc_OrderChannel_h --> n_core_Types_h
  n_ipc_OrderRouter_cpp --> n_api_KisErrorCodes_h
  n_ipc_OrderRouter_cpp --> n_core_KstTime_h
  n_ipc_OrderRouter_cpp --> n_core_LatencyTrace_h
  n_ipc_OrderRouter_cpp --> n_core_WakeGate_h
  n_ipc_OrderRouter_cpp --> n_ipc_OrderRouter_h
  n_ipc_OrderRouter_cpp --> n_utils_Logger_h
  n_ipc_OrderRouter_h --> n_api_IOrderExecutor_h
  n_ipc_OrderRouter_h --> n_core_ReconcilePlan_h
  n_ipc_OrderRouter_h --> n_core_Types_h
  n_ipc_OrderRouter_h --> n_ipc_FillKey_h
  n_ipc_OrderRouter_h --> n_ipc_ZmqBridge_h
  n_ipc_OrderRouter_h --> n_risk_OrderGate_h
  n_ipc_SharedLayout_cpp --> n_ipc_SharedLayout_h
  n_ipc_SharedLayout_h --> n_ipc_ControlChannel_h
  n_ipc_SharedLayout_h --> n_ipc_Heartbeat_h
  n_ipc_SharedLayout_h --> n_ipc_LedgerSnapshot_h
  n_ipc_SharedLayout_h --> n_ipc_MarketFeedChannel_h
  n_ipc_SharedLayout_h --> n_ipc_OrderChannel_h
  n_ipc_SharedLayout_h --> n_ipc_SharedSpscRing_h
  n_ipc_SharedLayout_h --> n_ipc_SharedStrategyDictionary_h
  n_ipc_SharedLayout_h --> n_ipc_SharedSymbolDictionary_h
  n_ipc_SharedRegion_cpp --> n_ipc_SharedRegion_h
  n_ipc_SharedStrategyDictionary_cpp --> n_ipc_SharedStrategyDictionary_h
  n_ipc_SharedStrategyDictionary_cpp --> n_ipc_SharedWriteLock_h
  n_ipc_SharedStrategyDictionary_h --> n_core_StrategyTable_h
  n_ipc_SharedStrategyDictionary_h --> n_ipc_SharedSpscRing_h
  n_ipc_SharedSymbolDictionary_cpp --> n_ipc_SharedSymbolDictionary_h
  n_ipc_SharedSymbolDictionary_cpp --> n_ipc_SharedWriteLock_h
  n_ipc_SharedSymbolDictionary_h --> n_core_SymbolTable_h
  n_ipc_SharedSymbolDictionary_h --> n_ipc_SharedSpscRing_h
  n_ipc_SharedWriteLock_cpp --> n_ipc_SharedWriteLock_h
  n_ipc_ZmqBridge_cpp --> n_ipc_ZmqBridge_h
  n_ipc_ZmqBridge_cpp --> n_utils_Logger_h
  n_ipc_ZmqBridge_cpp --> n_utils_ThreadName_h
  n_ipc_ZmqBridge_h --> n_core_MpscQueue_h
  n_ipc_ZmqBridge_h --> n_core_Types_h
  n_main_cpp --> n_core_AppConfig_h
  n_main_cpp --> n_core_CommandLine_h
  n_main_cpp --> n_core_Engine_h
  n_main_cpp --> n_core_Types_h
  n_main_cpp --> n_modes_Monitors_h
  n_main_cpp --> n_strategy_StrategyFactory_h
  n_main_cpp --> n_utils_Logger_h
  n_modes_Monitors_cpp --> n_api_KisClient_h
  n_modes_Monitors_cpp --> n_api_KisWebSocket_h
  n_modes_Monitors_cpp --> n_core_KstTime_h
  n_modes_Monitors_cpp --> n_core_MarketSession_h
  n_modes_Monitors_cpp --> n_core_Types_h
  n_modes_Monitors_cpp --> n_ipc_ZmqBridge_h
  n_modes_Monitors_cpp --> n_modes_Monitors_h
  n_modes_Monitors_cpp --> n_utils_Logger_h
  n_modes_Monitors_cpp --> n_utils_Utf8_h
  n_modes_Monitors_h --> n_api_KisClient_h
  n_risk_DisplacementDesk_cpp --> n_risk_DisplacementDesk_h
  n_risk_DisplacementDesk_cpp --> n_utils_Logger_h
  n_risk_DisplacementDesk_h --> n_core_Types_h
  n_risk_DisplacementDesk_h --> n_risk_OrderGate_h
  n_risk_GateReasons_cpp --> n_risk_GateReasons_h
  n_risk_LedgerJournal_cpp --> n_risk_LedgerJournal_h
  n_risk_OrderGate_cpp --> n_core_KstTime_h
  n_risk_OrderGate_cpp --> n_ipc_LedgerSnapshot_h
  n_risk_OrderGate_cpp --> n_risk_GateReasons_h
  n_risk_OrderGate_cpp --> n_risk_OrderGate_h
  n_risk_OrderGate_h --> n_core_StrategyTable_h
  n_risk_OrderGate_h --> n_core_SymbolTable_h
  n_risk_OrderGate_h --> n_core_Types_h
  n_risk_OrderGate_h --> n_risk_LedgerJournal_h
  n_risk_ProtectiveOrders_cpp --> n_risk_ProtectiveOrders_h
  n_risk_ProtectiveOrders_h --> n_risk_OrderGate_h
  n_risk_ProtectiveOrders_h --> n_risk_ProtectiveRule_h
  n_risk_ProtectiveOrders_h --> n_utils_Logger_h
  n_risk_ProtectiveRule_cpp --> n_risk_ProtectiveRule_h
  n_risk_ProtectiveRule_h --> n_core_Types_h
  n_strategy_DevScaleRules_cpp --> n_strategy_DevScaleRules_h
  n_strategy_DevScaleRules_h --> n_core_Types_h
  n_strategy_DeviationScaleStrategy_cpp --> n_strategy_DeviationScaleStrategy_h
  n_strategy_DeviationScaleStrategy_h --> n_api_KisClient_h
  n_strategy_DeviationScaleStrategy_h --> n_core_BarAggregator_h
  n_strategy_DeviationScaleStrategy_h --> n_core_DataPoller_h
  n_strategy_DeviationScaleStrategy_h --> n_core_KstTime_h
  n_strategy_DeviationScaleStrategy_h --> n_core_PrefetchPool_h
  n_strategy_DeviationScaleStrategy_h --> n_core_TickSize_h
  n_strategy_DeviationScaleStrategy_h --> n_core_WakeGate_h
  n_strategy_DeviationScaleStrategy_h --> n_strategy_DevScaleRules_h
  n_strategy_DeviationScaleStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_DeviationScaleStrategy_h --> n_universe_MaAlign_h
  n_strategy_DeviationScaleStrategy_h --> n_utils_Logger_h
  n_strategy_FixedIntervalStrategy_cpp --> n_strategy_FixedIntervalStrategy_h
  n_strategy_FixedIntervalStrategy_h --> n_core_MarketSession_h
  n_strategy_FixedIntervalStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_FixedIntervalStrategy_h --> n_utils_Logger_h
  n_strategy_IntradayBreakoutStrategy_cpp --> n_strategy_IntradayBreakoutStrategy_h
  n_strategy_IntradayBreakoutStrategy_h --> n_strategy_SeedPeakStore_h
  n_strategy_IntradayBreakoutStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_IntradayBreakoutStrategy_h --> n_utils_Logger_h
  n_strategy_MACrossStrategy_cpp --> n_strategy_MACrossStrategy_h
  n_strategy_MACrossStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_MarketMakingStrategy_cpp --> n_strategy_MarketMakingStrategy_h
  n_strategy_MarketMakingStrategy_h --> n_core_TickSize_h
  n_strategy_MarketMakingStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_MomentumStrategy_cpp --> n_strategy_MomentumStrategy_h
  n_strategy_MomentumStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_PriceTargetStrategy_cpp --> n_strategy_PriceTargetStrategy_h
  n_strategy_PriceTargetStrategy_h --> n_core_MarketSession_h
  n_strategy_PriceTargetStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_PriceTargetStrategy_h --> n_utils_Logger_h
  n_strategy_SeedPeakStore_cpp --> n_strategy_SeedPeakStore_h
  n_strategy_SeedPeakStore_h --> n_core_KstTime_h
  n_strategy_SeedPeakStore_h --> n_utils_Logger_h
  n_strategy_StrategyBase_cpp --> n_strategy_StrategyBase_h
  n_strategy_StrategyBase_h --> n_core_StrategyTable_h
  n_strategy_StrategyBase_h --> n_core_Types_h
  n_strategy_StrategyBase_h --> n_risk_ProtectiveRule_h
  n_strategy_StrategyFactory_cpp --> n_core_Engine_h
  n_strategy_StrategyFactory_cpp --> n_core_KstTime_h
  n_strategy_StrategyFactory_cpp --> n_core_Types_h
  n_strategy_StrategyFactory_cpp --> n_core_UniverseExit_h
  n_strategy_StrategyFactory_cpp --> n_strategy_DevScaleRules_h
  n_strategy_StrategyFactory_cpp --> n_strategy_DeviationScaleStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_FixedIntervalStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_IntradayBreakoutStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_MACrossStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_MarketMakingStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_MomentumStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_PriceTargetStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_StrategyFactory_h
  n_strategy_StrategyFactory_cpp --> n_strategy_SupplyDemandPullbackStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_TargetBasketStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_ThemeStrategy_h
  n_strategy_StrategyFactory_cpp --> n_strategy_ValueContraryStrategy_h
  n_strategy_StrategyFactory_cpp --> n_universe_ScoreWeight_h
  n_strategy_StrategyFactory_cpp --> n_universe_UniverseScanner_h
  n_strategy_StrategyFactory_cpp --> n_utils_JsonNode_h
  n_strategy_StrategyFactory_cpp --> n_utils_Logger_h
  n_strategy_StrategyFactory_h --> n_api_KisClient_h
  n_strategy_SupplyDemandPullbackStrategy_cpp --> n_strategy_SupplyDemandPullbackStrategy_h
  n_strategy_SupplyDemandPullbackStrategy_h --> n_api_KisClient_h
  n_strategy_SupplyDemandPullbackStrategy_h --> n_core_KstTime_h
  n_strategy_SupplyDemandPullbackStrategy_h --> n_core_Types_h
  n_strategy_SupplyDemandPullbackStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_SupplyDemandPullbackStrategy_h --> n_utils_Logger_h
  n_strategy_TargetBasketPlan_cpp --> n_strategy_TargetBasketPlan_h
  n_strategy_TargetBasketPlan_h --> n_core_Types_h
  n_strategy_TargetBasketStrategy_cpp --> n_core_KstTime_h
  n_strategy_TargetBasketStrategy_cpp --> n_strategy_TargetBasketStrategy_h
  n_strategy_TargetBasketStrategy_cpp --> n_utils_Logger_h
  n_strategy_TargetBasketStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_TargetBasketStrategy_h --> n_strategy_TargetBasketPlan_h
  n_strategy_ThemeStrategy_cpp --> n_strategy_ThemeStrategy_h
  n_strategy_ThemeStrategy_h --> n_api_KisClient_h
  n_strategy_ThemeStrategy_h --> n_core_MarketSession_h
  n_strategy_ThemeStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_ThemeStrategy_h --> n_utils_Logger_h
  n_strategy_ValueContraryStrategy_cpp --> n_strategy_ValueContraryStrategy_h
  n_strategy_ValueContraryStrategy_h --> n_api_KisClient_h
  n_strategy_ValueContraryStrategy_h --> n_core_MarketSession_h
  n_strategy_ValueContraryStrategy_h --> n_strategy_StrategyBase_h
  n_strategy_ValueContraryStrategy_h --> n_utils_Logger_h
  n_universe_MaAlign_cpp --> n_universe_MaAlign_h
  n_universe_ScoreWeight_cpp --> n_universe_ScoreWeight_h
  n_universe_ScoreWeight_h --> n_core_SymbolTable_h
  n_universe_UniverseScanner_cpp --> n_core_KstTime_h
  n_universe_UniverseScanner_cpp --> n_core_Types_h
  n_universe_UniverseScanner_cpp --> n_universe_MaAlign_h
  n_universe_UniverseScanner_cpp --> n_universe_UniverseScanner_h
  n_universe_UniverseScanner_cpp --> n_utils_EtfFilter_h
  n_universe_UniverseScanner_cpp --> n_utils_JsonNode_h
  n_universe_UniverseScanner_cpp --> n_utils_Logger_h
  n_universe_UniverseScanner_h --> n_api_KisClient_h
  n_universe_UniverseScanner_h --> n_core_SymbolTable_h
  n_universe_UniverseScanner_h --> n_universe_ScoreWeight_h
  n_utils_EtfFilter_cpp --> n_utils_EtfFilter_h
  n_utils_JsonNode_cpp --> n_utils_JsonNode_h
  n_utils_Logger_cpp --> n_core_MpscQueue_h
  n_utils_Logger_cpp --> n_utils_Logger_h
  n_utils_Logger_cpp --> n_utils_ThreadName_h
  n_utils_ThreadName_cpp --> n_utils_ThreadName_h
  n_utils_Utf8_cpp --> n_utils_Utf8_h
```

## Python import 그래프

`PYQuant/**`·`scripts/*.py` 의 내부 import만(표준·서드파티 제외). 화살표는 패키지 단위, 숫자는 파일 쌍 수.

```mermaid
graph LR
  p_PYQuant["PYQuant"]
  p_PYQuant_backtest["PYQuant/backtest"]
  p_PYQuant_core["PYQuant/core"]
  p_PYQuant_dashboard["PYQuant/dashboard"]
  p_PYQuant_data["PYQuant/data"]
  p_PYQuant_db["PYQuant/db"]
  p_PYQuant_exit_ev["PYQuant/exit_ev"]
  p_PYQuant_features["PYQuant/features"]
  p_PYQuant_ipc["PYQuant/ipc"]
  p_PYQuant_kis["PYQuant/kis"]
  p_PYQuant_live["PYQuant/live"]
  p_PYQuant_naver["PYQuant/naver"]
  p_PYQuant_report["PYQuant/report"]
  p_PYQuant_strategy["PYQuant/strategy"]
  p_PYQuant_tests["PYQuant/tests"]
  p_PYQuant_tools["PYQuant/tools"]
  p_scripts["scripts"]
  p_PYQuant -->|2| p_PYQuant_backtest
  p_PYQuant -->|2| p_PYQuant_core
  p_PYQuant -->|4| p_PYQuant_data
  p_PYQuant --> p_PYQuant_db
  p_PYQuant --> p_PYQuant_features
  p_PYQuant -->|2| p_PYQuant_ipc
  p_PYQuant --> p_PYQuant_kis
  p_PYQuant -->|3| p_PYQuant_live
  p_PYQuant --> p_PYQuant_report
  p_PYQuant -->|5| p_PYQuant_strategy
  p_PYQuant_backtest --> p_PYQuant_data
  p_PYQuant_backtest --> p_PYQuant_kis
  p_PYQuant_backtest --> p_PYQuant_strategy
  p_PYQuant_dashboard --> p_PYQuant_backtest
  p_PYQuant_data -->|4| p_PYQuant_kis
  p_PYQuant_db --> p_PYQuant_core
  p_PYQuant_features --> p_PYQuant_data
  p_PYQuant_ipc -->|2| p_PYQuant_core
  p_PYQuant_kis --> p_PYQuant_core
  p_PYQuant_live --> p_PYQuant
  p_PYQuant_live -->|2| p_PYQuant_backtest
  p_PYQuant_live --> p_PYQuant_features
  p_PYQuant_live -->|2| p_PYQuant_kis
  p_PYQuant_live -->|2| p_PYQuant_strategy
  p_PYQuant_strategy -->|6| p_PYQuant_kis
  p_PYQuant_tests -->|6| p_PYQuant_backtest
  p_PYQuant_tests -->|3| p_PYQuant_data
  p_PYQuant_tests --> p_PYQuant_exit_ev
  p_PYQuant_tests --> p_PYQuant_features
  p_PYQuant_tests -->|3| p_PYQuant_kis
  p_PYQuant_tests -->|4| p_PYQuant_strategy
  p_PYQuant_tools -->|5| p_PYQuant
  p_PYQuant_tools --> p_PYQuant_backtest
  p_PYQuant_tools -->|2| p_PYQuant_core
  p_PYQuant_tools -->|7| p_PYQuant_data
  p_PYQuant_tools -->|2| p_PYQuant_db
  p_PYQuant_tools --> p_PYQuant_features
  p_PYQuant_tools -->|10| p_PYQuant_kis
  p_PYQuant_tools --> p_PYQuant_naver
  p_scripts -->|3| p_PYQuant_backtest
  p_scripts --> p_PYQuant_db
  p_scripts -->|2| p_PYQuant_kis
  p_scripts --> p_PYQuant_naver
```

| 파일 | 내부 import |
|---|---|
| `PYQuant/backtest/devscale_replay.py` | `backtest.costs` |
| `PYQuant/backtest/devscale_replay_rescue.py` | `backtest.costs` |
| `PYQuant/backtest/engine.py` | `backtest.costs`, `backtest.ledger`, `data.index_source`, `kis.client`, `strategy.base` |
| `PYQuant/backtest/ledger.py` | `backtest.costs` |
| `PYQuant/backtest/metrics.py` | `backtest.costs` |
| `PYQuant/backtest/report.py` | `backtest.engine` |
| `PYQuant/core/proc_watch.py` | `core.logger` |
| `PYQuant/dashboard/backfill_series_a.py` | `backtest.report` |
| `PYQuant/data/datagokr_source.py` | `kis.client` |
| `PYQuant/data/index_source.py` | `kis.client` |
| `PYQuant/data/krx_source.py` | `kis.client` |
| `PYQuant/data/universe_kospi.py` | `data.krx_source` |
| `PYQuant/data/yfinance_source.py` | `data.universe_kospi`, `kis.client` |
| `PYQuant/db/client.py` | `core.logger` |
| `PYQuant/features/fundamental.py` | `data.point_in_time` |
| `PYQuant/ipc/operator.py` | `core.logger` |
| `PYQuant/ipc/subscriber.py` | `core.logger` |
| `PYQuant/kis/client.py` | `core.logger`, `kis.endpoints` |
| `PYQuant/live/basket_forward.py` | `backtest.engine`, `features`, `strategy.cross_momentum` |
| `PYQuant/live/forward_trader.py` | `backtest.engine`, `kis.client`, `main` |
| `PYQuant/live/trader.py` | `kis.client`, `strategy.base` |
| `PYQuant/main.py` | `backtest.engine`, `backtest.report`, `core`, `core.logger`, `data.datagokr_source`, `data.krx_source`, `data.universe_kospi`, `data.yfinance_source`, `db.client`, `features.fundamental`, `ipc.operator`, `ipc.subscriber`, `kis.client`, `live.basket_forward`, `live.forward_trader`, `live.trader`, `report.account`, `strategy.cross_momentum`, `strategy.mean_reversion`, `strategy.strategy_a`, `strategy.supply_demand_rank`, `strategy.value_contrary` |
| `PYQuant/strategy/base.py` | `kis.client` |
| `PYQuant/strategy/channel_breakout.py` | `strategy.base` |
| `PYQuant/strategy/cross_momentum.py` | `kis.client`, `strategy.base` |
| `PYQuant/strategy/indicators.py` | `kis.client` |
| `PYQuant/strategy/mean_reversion.py` | `strategy.base`, `strategy.indicators` |
| `PYQuant/strategy/strategy_a.py` | `kis.client`, `strategy.base`, `strategy.indicators` |
| `PYQuant/strategy/supply_demand_rank.py` | `kis.client`, `strategy.base` |
| `PYQuant/strategy/value_contrary.py` | `kis.client`, `strategy.base` |
| `PYQuant/tests/test_adjust_splits.py` | `data.datagokr_source` |
| `PYQuant/tests/test_backtest_engine.py` | `backtest.engine`, `data.krx_source`, `kis.client`, `strategy.supply_demand_rank`, `strategy.value_contrary` |
| `PYQuant/tests/test_costs_golden.py` | `backtest.costs`, `backtest.ledger` |
| `PYQuant/tests/test_indicators.py` | `kis.client`, `strategy.indicators` |
| `PYQuant/tests/test_metrics.py` | `backtest.metrics` |
| `PYQuant/tests/test_point_in_time.py` | `data.point_in_time` |
| `PYQuant/tests/test_regime_axes.py` | `features` |
| `PYQuant/tests/test_regime_scorer.py` | `backtest.regime_scorer` |
| `PYQuant/tests/test_stats.py` | `backtest`, `exit_ev` |
| `PYQuant/tests/test_strategy_a.py` | `kis.client`, `strategy.strategy_a` |
| `PYQuant/tools/bench_market_open.py` | `core.logger`, `db.client` |
| `PYQuant/tools/check_adjusted.py` | `data.datagokr_source` |
| `PYQuant/tools/check_datagokr.py` | `data.datagokr_source` |
| `PYQuant/tools/check_investor_api.py` | `kis.client` |
| `PYQuant/tools/check_kis_investor.py` | `kis.client` |
| `PYQuant/tools/check_market_flow.py` | `kis.client` |
| `PYQuant/tools/check_sector_index.py` | `kis.client` |
| `PYQuant/tools/compare_ws_bars.py` | `kis.client` |
| `PYQuant/tools/dart_shares_history_fill.py` | `data.keys` |
| `PYQuant/tools/fetch_naver_themes.py` | `naver.theme` |
| `PYQuant/tools/full_universe_dump.py` | `data.datagokr_source` |
| `PYQuant/tools/fullperiod_validate.py` | `data.datagokr_source`, `main`, `tools.month_start_sweep` |
| `PYQuant/tools/index_intraday_logger.py` | `kis.client` |
| `PYQuant/tools/investor_flow_logger.py` | `kis.client` |
| `PYQuant/tools/ledger_recorder.py` | `core.logger`, `db.client` |
| `PYQuant/tools/minute_backfill.py` | `kis.client` |
| `PYQuant/tools/minute_backfill_pairs.py` | `features.fundamental` |
| `PYQuant/tools/month_start_sweep.py` | `data.datagokr_source`, `main` |
| `PYQuant/tools/nxt_divergence_check.py` | `kis.client` |
| `PYQuant/tools/pit_universe_backfill.py` | `kis.client`, `tools.universe_feed` |
| `PYQuant/tools/regime_removal_test_2022.py` | `main` |
| `PYQuant/tools/sweep.py` | `main` |
| `PYQuant/tools/universe_feed.py` | `data.datagokr_source` |
| `PYQuant/tools/walkforward.py` | `backtest.engine`, `main` |
| `scripts/analyze_slot_cost.py` | `_logdir` |
| `scripts/backfill_fills_db.py` | `_logdir`, `db.client` |
| `scripts/backfill_studies.py` | `backtest.report` |
| `scripts/build_review_entry.py` | `market_close_collect` |
| `scripts/check_runtime_health.py` | `_logdir`, `log_patterns` |
| `scripts/dashboard_server.py` | `_logdir`, `kis.client`, `naver.theme` |
| `scripts/exit_ev.py` | `backtest.costs` |
| `scripts/exit_ev_dashboard.py` | `_logdir`, `exit_ev`, `gen_tuning_sheet` |
| `scripts/extract_swap_what_if.py` | `_logdir` |
| `scripts/market_close_autodoc.py` | `_logdir`, `check_runtime_health`, `log_patterns` |
| `scripts/market_close_collect.py` | `_logdir`, `log_patterns` |
| `scripts/notify_trades.py` | `_logdir`, `dashboard_server`, `kis.client`, `log_patterns` |
| `scripts/parse_quant_log.py` | `_logdir`, `check_runtime_health` |
| `scripts/summarize_trading_day.py` | `_logdir`, `log_patterns` |
| `scripts/trade_costs.py` | `backtest.costs` |

## 프로세스 경계 파일

C++ 엔진·Python 보조 프로세스·스크립트가 파일로 주고받는 지점. 코드의 문자열 리터럴에서 찾았고,
읽기/쓰기는 리터럴 주변 줄의 힌트(ofstream·dump·read_text 등)로 분류했다. 힌트가 없으면 '언급만'.

| 파일 | 쓰는 쪽 | 읽는 쪽 | 언급만 |
|---|---|---|---|
| `regime.json` | `PYQuant/tools/macro_regime_feed.py`, `Quant/src/core/Engine.cpp` | `PYQuant/tools/macro_regime_feed.py`, `Quant/src/core/AppConfig.cpp`, `Quant/src/core/Engine.cpp`, `Quant/src/core/RegimeFileJudge.cpp`, `scripts/dashboard_server.py`, `scripts/notify_trades.py` | `Quant/include/core/AppConfig.h`, `Quant/include/core/Engine.h`, `Quant/include/core/RegimeFileJudge.h`, `Quant/src/core/EngineConfigure.cpp` |
| `prices_live.json` | `scripts/live_prices_feed.py` | `scripts/live_prices_feed.py` |  |
| `trades_*.csv` | `Quant/src/ipc/OrderRouter.cpp`, `scripts/backfill_fills_db.py`, `scripts/exit_ev_dashboard.py` | `PYQuant/dashboard/backfill_live.py`, `PYQuant/tests/test_stats.py`, `Quant/src/ipc/OrderRouter.cpp`, `Quant/src/strategy/StrategyFactory.cpp`, `scripts/backfill_fills_db.py`, `scripts/exit_ev.py`, `scripts/exit_ev_dashboard.py`, `scripts/parse_quant_log.py`, `scripts/trade_costs.py` | `scripts/_logdir.py`, `scripts/notify_trades.py` |
| `universe*.json` |  | `PYQuant/main.py`, `PYQuant/tools/full_universe_dump.py`, `PYQuant/tools/nxt_divergence_check.py`, `PYQuant/tools/universe_feed.py`, `Quant/src/universe/UniverseScanner.cpp`, `scripts/exit_ev_dashboard.py`, `scripts/live_prices_feed.py`, `scripts/market_close_minute_backfill.py`, `scripts/notify_trades.py` | `Quant/include/universe/UniverseScanner.h`, `Quant/src/api/KisUniverse.cpp`, `Quant/src/strategy/StrategyFactory.cpp`, `scripts/dashboard_server.py` |
| `open_orders.txt` | `Quant/src/ipc/OrderRouter.cpp`, `scripts/seed_open_orders.py` | `Quant/src/ipc/OrderRouter.cpp`, `scripts/seed_open_orders.py` |  |
| `quant_trader.log` | `PYQuant/tools/log_report.py`, `scripts/_logdir.py`, `scripts/build_review_entry.py`, `scripts/dashboard_server.py`, `scripts/summarize_trading_day.py` | `PYQuant/tools/compare_ws_bars.py`, `scripts/_logdir.py`, `scripts/check_runtime_health.py`, `scripts/dashboard_server.py`, `scripts/extract_swap_what_if.py`, `scripts/notify_trades.py`, `scripts/parse_quant_log.py`, `scripts/seed_open_orders.py`, `scripts/summarize_trading_day.py` | `Quant/src/main.cpp`, `scripts/exit_ev_dashboard.py` |
| `kis_token_*.json` |  |  | `PYQuant/kis/client.py`, `Quant/src/api/KisAuth.cpp` |

## 영향범위 질의 · 기계 소비

편집·커밋 전 영향범위(재검증/재빌드 대상)를 파일 열지 않고 뽑는다:

```bash
py ../quant-devtools/gen_code_graph.py --impact core/Types.h
py ../quant-devtools/gen_code_graph.py --json   # docs/code_graph.json
```

`docs/code_graph.dot` 도 생성했다. Graphviz가 있으면 SVG로 렌더할 수 있다.

```bash
dot -Tsvg docs/code_graph.dot -o docs/code_graph.svg
```

