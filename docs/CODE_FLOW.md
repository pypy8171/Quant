# 실시간 매매 코드 흐름 (CODE_FLOW)

<!-- drift-check: snapshot — 줄 번호는 생성 시점 소스 기준, ../quant-devtools/gen_code_flow.py가 다시 만든다 -->
> 자동 생성물. 손편집 금지 — 읽는 순서는 `docs/code_flow.toml`에 적고 `py ../quant-devtools/gen_code_flow.py`로 다시 만든다.
> 줄 번호·시그니처는 생성 시점의 소스에서 찍었다. 심볼이 사라지면 `--check`가 막는다(D-078).

TRADE 모드에서 틱 하나가 들어와 주문이 나가고 체결이 원장에 닿기까지를 **읽는 순서대로** 나열한 문서다.
단계마다 심볼 링크(`파일#L줄`)·시그니처·볼 것 한 줄·덮는 테스트가 있다. 링크는 VS Code 미리보기·GitHub에서 그 줄로 열린다.

스레드 모델·설계 원칙은 [CLAUDE.md](../CLAUDE.md)의 "설계 목표와 원칙"·"아키텍처" 절이 정본이고, 모듈 간 include 관계는
[CODE_GRAPH.md](CODE_GRAPH.md), 바꾼 이유는 [DECISIONS.md](DECISIONS.md)다. 이 문서는 그 셋을 대신하지 않고 **입구**만 맡는다.

## 한 장 그림

```mermaid
flowchart LR
  subgraph recv[1. 수신 — 소켓 스레드 N]
    WS[KisWebSocket::recv_loop] --> DEC[kis_websocket::decode_*] --> CB[Engine WS 콜백]
  end
  subgraph mx[SPSC 링 행렬]
    CB --> TD[pipeline_.trade_matrix / order_book_matrix (행=수신 스레드, 열=샤드)]
    DP[DataPoller REST 대체 틱] --> TD
  end
  subgraph shard[2. 샤드 스레드 M]
    TD --> ST[strategy::Shard::step] --> RT[strategy::Router::for_each] --> S[StrategyBase::on_trade_batch]
    S --> EM[strategy::Emitted]
  end
  EM --> SO[pipeline_.shard_out MpscQueue]
  subgraph disp[3. 디스패치 스레드]
    SO --> SD[SignalDispatcher::from_strategy] --> OQ[pipeline_.order_queue SPSC]
  end
  subgraph ord[4. 주문 스레드]
    OQ --> PC[OrderRateLimiter] --> OR[OrderRouter::new_route] --> OG[OrderGate::check] --> KIS[KisClient::submit_order_acknowledgement]
  end
  subgraph fill[5. 체결 스레드]
    FN[parse_fill_notification] --> FQ[pipeline_.fill_queue SPSC] --> OF[OrderRouter::on_fill] --> OGF[OrderGate::on_fill_confirmed]
  end
  KIS -. ODNO .-> OR
  WS -. H0STCNI9 .-> FN
```

## 읽는 순서

- [0. 기동 — config에서 스레드가 서기까지](#0-기동-config에서-스레드가-서기까지) — 10걸음
- [1. 수신 — 소켓 바이트에서 링 행렬까지](#1-수신-소켓-바이트에서-링-행렬까지) — 11걸음
- [2. 샤드 — 틱이 전략을 만나 신호가 되기까지](#2-샤드-틱이-전략을-만나-신호가-되기까지) — 10걸음
- [3. 디스패치 — 신호가 주문 큐에 들어가기 전 판단](#3-디스패치-신호가-주문-큐에-들어가기-전-판단) — 5걸음
- [4. 주문 — 큐에서 KIS까지, 거부와 재시도](#4-주문-큐에서-kis까지-거부와-재시도) — 11걸음
- [5. 체결 — 체결통보가 원장에 닿기까지](#5-체결-체결통보가-원장에-닿기까지) — 5걸음
- [6. 제어·주기 — 파이프라인 밖에서 돌아가는 것](#6-제어주기-파이프라인-밖에서-돌아가는-것) — 7걸음
- [7. 종료 — 큐를 비우고 스레드를 내린다](#7-종료-큐를-비우고-스레드를-내린다) — 2걸음
- [부록. 실계좌 없이 같은 경로를 돌리는 것](#부록-실계좌-없이-같은-경로를-돌리는-것) — 3걸음

## 0. 기동 — config에서 스레드가 서기까지

config를 `AppConfig`로 읽고 `Engine::configure`가 세터에 옮기고 전략을 만든 뒤, `Engine::start()`가 행렬·샤드·원장·소켓 콜백·스레드 다섯을 세운다. 이 절만 읽으면 누가 무엇을 소유하는지 보인다.

1. [`main`](../Quant/src/main.cpp#L303) — 진입. 번호 주석이 초기화 순서다 — 콘솔·로거 → 인자 → `parse_config` → 로그 임계값 → 크래시 핸들러 → 모드 분기(FEED·KR_TEST·US_TEST는 modes/, TRADE는 `run_trade`)  
   `Quant/src/main.cpp:303` · `int main(int argc, char* argv[])`
2. [`parse_config`](../Quant/src/core/AppConfig.cpp#L195) — json → `AppConfig`. 키 누락·값 오류는 여기서 멈춘다(네트워크 전). 모드 오버라이드(인자)도 여기서 반영  
   `Quant/src/core/AppConfig.cpp:195` · `AppConfig parse_config(const json& document, const std::string& mode_override)`
3. [`run_trade`](../Quant/src/main.cpp#L273) — `Engine engine(...)` → `engine.configure(app)` → `load_strategies` → `engine.start()` → `is_running` 대기 → `engine.stop()`. join은 여기 한 곳  
   `Quant/src/main.cpp:273` · `static int run_trade(const AppConfig& app)`
4. [`Engine::configure`](../Quant/src/core/EngineConfigure.cpp#L106) — AppConfig 값을 엔진 세터로 — 채널(ZMQ·운영단말)·국면별 전략 집합·시세 전용 KIS·리스크(게이트 한도·매매 창) 네 묶음  
   `Quant/src/core/EngineConfigure.cpp:106` · `void Engine::configure(const AppConfig& app)`
5. [`load_strategies`](../Quant/src/strategy/StrategyFactory.cpp#L1170) — config `strategies[]`를 전략 객체로. 새 전략을 붙이는 자리(docs/ENGINE_ARCHITECTURE.md '전략 추가하기')  
   `Quant/src/strategy/StrategyFactory.cpp:1170` · `void load_strategies(StrategyLoadCtx& context, const json& strategies)`
6. [`Engine::add_strategy`](../Quant/src/core/Engine.cpp#L93) — 전략 등록. 심볼 해석기(`set_symbol_resolver` → `SymbolTable::intern`)가 여기서 주입된다  
   `Quant/src/core/Engine.cpp:93` · `void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)`
7. [`Engine::start`](../Quant/src/core/Engine.cpp#L1400) — 도우미 호출 목록이 기동 순서다 — `setup_shards`(행렬 `reshape`, 행=수신 스레드+폴러, 열=샤드) → ZMQ → 모의 체결기 → 라우터·대조기·폴러 → `try_bootstrap_ledger`(원장 시드) → `start_strategies` → 구독 목록 → `connect_feed` → `spawn_threads`  
   `Quant/src/core/Engine.cpp:1400` · `void Engine::start()`
8. [`Engine::connect_feed (WS 콜백 설치)`](../Quant/src/core/Engine.cpp#L1224) — 소켓 수신 스레드 i의 호가·체결 콜백. 종목 id로 열을 고르고(`consumer_of`) `pipeline_.trade_matrix`·`order_book_matrix`의 자기 행에 `push_to` — 가득 차면 버리고 센다(블로킹 금지)  
   `Quant/src/core/Engine.cpp:1224` · `feed_.websocket->set_lane_callbacks([this] (uint32_t lane, const OrderBook& in) …`
9. [`Engine::spawn_threads`](../Quant/src/core/Engine.cpp#L1378) — data·strategy·order·fill·control 다섯 jthread + 샤드 M. stop_token이 첫 인자라 람다로 감싼다  
   `Quant/src/core/Engine.cpp:1378` · `data_thread_ = std::jthread([this] (std::stop_token stop_token) { data_thread_fn(stop_token); });`
10. [`LedgerReconciler::bootstrap`](../Quant/src/core/LedgerReconciler.cpp#L20) — 기동 잔고 시드 — 브로커 잔고를 원장·게이트 포지션으로. 실패 재시도 횟수와 실패 시 기동 중단 여부  
   `Quant/src/core/LedgerReconciler.cpp:20` · `bool LedgerReconciler::bootstrap(int attempts, std::chrono::milliseconds retry_delay)` · 시험 [test_ledger_reconciler](../Quant/tests/test_ledger_reconciler.cpp)

리뷰할 때 볼 것:

- 스레드 소유권 — 각 큐의 생산자·소비자가 하나씩인지(`RingBuffer`는 SPSC, 생산자 둘이면 `MpscQueue`, 원칙 5)
- config 키가 `parse_config`에서 검증되고 `Engine::configure`가 세터로 옮기는지 — 빠진 키의 기본값이 실계좌에 안전한 쪽인지
- 기동 순서 — 원장 시드(`bootstrap`)가 소켓 콜백 설치보다 앞인지(체결이 먼저 오면 원장이 비어 있다)

## 1. 수신 — 소켓 바이트에서 링 행렬까지

소켓 읽기 스레드는 얇다(원칙 3): 프레임 → 필드 분리 → 구조체 → `received_ns` 스탬프 → 행렬 push. 문자열은 여기서 끝나고 종목은 정수 id가 된다(원칙 6).

11. [`KisWebSocket::recv_loop`](../Quant/src/api/WebSocketClient.cpp#L143) — 소켓 하나 = 스레드 하나(원칙 1). 프레임 읽기 → `parse_message`. 끊김 감지와 재연결 신호  
   `Quant/src/api/WebSocketClient.cpp:143` · `void KisWebSocket::recv_loop(std::stop_token stop_token)` · 시험 [test_ws_frame](../Quant/tests/test_ws_frame.cpp)
12. [`KisWebSocket::parse_message`](../Quant/src/api/WebSocketClient.cpp#L542) — `|`로 헤더 분리 → 암호화 여부(체결통보는 AES) → `dispatch_record`. PINGPONG·구독 응답 처리도 여기  
   `Quant/src/api/WebSocketClient.cpp:542` · `void KisWebSocket::parse_message(const std::string& message)`
13. [`KisWebSocket::dispatch_record`](../Quant/src/api/WebSocketClient.cpp#L730) — tr_id로 채널 분기 — H0STCNT0 체결·H0STASP0 호가(KRX), H0UNCNT0/H0UNASP0(KRX+NXT 통합, D-096), H0IFCNT0/H0IFASP0 선물, H0STCNI0/H0STCNI9 체결통보(실/모의)  
   `Quant/src/api/WebSocketClient.cpp:730` · `void KisWebSocket::dispatch_record(std::string_view transaction_id, kis_websocket::Fields fields)`
14. [`KisWebSocket::parse_kr_trade`](../Quant/src/api/WebSocketClient.cpp#L811) — `decode_kr_trade` → `trade.symbol_id`(SymbolTable) → `received_ns` 스탬프 → `on_trade_` 콜백. 호가는 `parse_orderbook`이 같은 모양  
   `Quant/src/api/WebSocketClient.cpp:811` · `void KisWebSocket::parse_kr_trade(kis_websocket::Fields fields)`
15. [`kis_websocket::decode_kr_trade`](../Quant/include/api/KisWsDecode.h#L131) — 순수 함수. 필드 인덱스 → `TradeData`(가격·수량·`hhmmss` 정수·방향). 필드 번호가 [wire] 정본  
   `Quant/include/api/KisWsDecode.h:131` · `Decode decode_kr_trade(Fields fields, TradeData& trade);` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
16. [`kis_websocket::decode_orderbook`](../Quant/include/api/KisWsDecode.h#L126) — 5단계 호가 → `OrderBook`. 매도·매수 가격/잔량 필드 위치  
   `Quant/include/api/KisWsDecode.h:126` · `Decode decode_orderbook(Fields fields, OrderBook& order_book);` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
17. [`shard::Matrix::push_to`](../Quant/include/core/ShardMatrix.h#L85) — 행(생산자)×열(소비자) SPSC 셀에 push. `consumer_of(sym)`이 종목 해시로 열을 고른다(원칙 2)  
   `Quant/include/core/ShardMatrix.h:85` · `[[nodiscard]] bool push_to(uint32_t producer, uint32_t consumer, const T& value)` · 시험 [test_shard_matrix](../Quant/tests/test_shard_matrix.cpp)
18. [`feed::FeedMux`](../Quant/include/core/FeedMux.h#L36) — 소켓 여럿을 한 `IFeedSource`로. 직접 호출 모드면 소켓 i 스레드가 행 i로 직접 push(mux 스레드 없음). 체결통보는 맡은 소켓 하나만(`owns_fill_notice`)  
   `Quant/include/core/FeedMux.h:36` · `class FeedMux final : public IFeedSource` · 시험 [test_feed_mux](../Quant/tests/test_feed_mux.cpp)
19. [`Engine::data_thread_fn`](../Quant/src/core/Engine.cpp#L1850) — REST 축 — 봉 폴링·`poll_regime_file`(국면)·잔고 대조·유니버스 재스캔·하루 경계(`new_trading_day`·`reset_daily`). 폴러의 대체 틱은 `pipeline_.trade_matrix`의 자기 행(`data_row`)으로 간다  
   `Quant/src/core/Engine.cpp:1850` · `void Engine::data_thread_fn(std::stop_token stop_token)`
20. [`DataPoller::poll_universe`](../Quant/src/core/DataPoller.cpp#L11) — REST 현재가 → 대체 `TradeData`(`received_ns`=0). 구독 상한 넘침·틱 끊긴 보유 보충(`top_up`)도 이 클래스  
   `Quant/src/core/DataPoller.cpp:11` · `int DataPoller::poll_universe(const std::vector<WatchSpec>& specifications, std::time_t now_utc)` · 시험 [test_data_poller](../Quant/tests/test_data_poller.cpp)
21. [`feed::TickCapture::on_trade`](../Quant/include/core/TickCapture.h#L164) — raw 틱 append-only 캡처(원칙 8). 리플레이(`ReplaySource`)의 입력  
   `Quant/include/core/TickCapture.h:164` · `void on_trade(const TradeData& trade) noexcept;` · 시험 [test_tick_capture](../Quant/tests/test_tick_capture.cpp)

리뷰할 때 볼 것:

- 수신 스레드에 힙 할당·문자열 생성·락이 있는지 — 있으면 소비자로 옮길 후보
- `received_ns`가 디코드 직후 한 번만 찍히는지(호가·체결 같은 시계, D-071)
- 가득 찬 링에 블로킹하지 않는지 — 버리고 세는지(`dropped` 카운터가 로그에 나오는지)

## 2. 샤드 — 틱이 전략을 만나 신호가 되기까지

샤드 m은 자기 열만 비운다. 틱의 종목 id로 그 종목을 보는 전략만 방문하고(`Router`), `NONE`이 아닌 신호를 봉투에 싸서 `pipeline_.shard_out`에 넣는다. 전략 코드 리뷰는 이 절에서 시작한다.

22. [`Engine::shard_thread_fn`](../Quant/src/core/Engine.cpp#L3140) — 전략 집합 버전이 바뀌면 `rebuild`, 아니면 `step`. 비면 `WakeGate`로 잠든다. `emit`은 `pipeline_.shard_out` push + 디스패치 스레드 깨우기  
   `Quant/src/core/Engine.cpp:3140` · `void Engine::shard_thread_fn(std::stop_token stop_token, uint32_t row)`
23. [`strategy::Shard::step`](../Quant/include/core/StrategyShard.h#L95) — 열의 호가·체결·봉 셀을 순서대로 비우고 전략 배치 훅을 부른다. `on_price`로 현재가 캐시 갱신  
   `Quant/include/core/StrategyShard.h:95` · `bool step(Emit&& emit, OnPrice&& on_price, SymbolIdOf&& symbol_id_of)` · 시험 [test_strategy_shard](../Quant/tests/test_strategy_shard.cpp)
24. [`strategy::Router::for_each`](../Quant/include/core/StrategyRouter.h#L78) — 종목 id → 그 종목을 구독한 전략 목록. 구독을 안 밝힌 전략은 전부 받는다  
   `Quant/include/core/StrategyRouter.h:78` · `void for_each(symbol::SymbolId id, Fn&& callback) const` · 시험 [test_strategy_router](../Quant/tests/test_strategy_router.cpp)
25. [`strategy::Emitted`](../Quant/include/core/StrategyShard.h#L27) — 샤드 → 디스패치 봉투: 신호 + 전략 id + 활성 여부. 이 구조체가 두 스레드의 계약이다  
   `Quant/include/core/StrategyShard.h:27` · `struct Emitted`
26. [`StrategyBase::on_trade_batch`](../Quant/include/strategy/StrategyBase.h#L76) — 전략 훅의 계약(가상 함수 다섯). 기본 구현은 `on_trade` 하나를 out에 담는다. `symbol_of`·`same_symbol`도 이 헤더  
   `Quant/include/strategy/StrategyBase.h:76` · `virtual void on_trade_batch(const TradeData&, std::vector<OrderSignal>& /*out*/)`
27. [`DeviationScaleStrategy::on_start`](../Quant/include/strategy/DeviationScaleStrategy.h#L204) — 종목 id 받기·REST 봉 시드·프리페치 스레드. 전략 하나를 끝까지 따라가는 예로 이 전략을 쓴다  
   `Quant/include/strategy/DeviationScaleStrategy.h:204` · `void on_start() override;`
28. [`DeviationScaleStrategy::on_trade_batch`](../Quant/include/strategy/DeviationScaleStrategy.h#L208) — 틱 → `aggregator_.on_tick` → 판단 직전 `close_stale`·`bars::resample` → 진입/청산 판단 → out. 매매 로직의 본체. 스탑·트레일 뒤 `stop_cooldown_sec`, 전량 청산 뒤 `reentry_cooldown_sec` 동안은 새 베이스를 깔지 않는다  
   `Quant/include/strategy/DeviationScaleStrategy.h:208` · `void on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;`
29. [`bars::BarAggregator::on_tick`](../Quant/src/core/BarAggregator.cpp#L199) — 체결 틱을 1분봉으로. `close_stale`은 틱이 없어도 시계로 지난 분을 닫는다(D-074)  
   `Quant/src/core/BarAggregator.cpp:199` · `bool BarAggregator::on_tick(const TradeData& trade)` · 시험 [test_bar_aggregator](../Quant/tests/test_bar_aggregator.cpp)
30. [`bars::resample`](../Quant/include/core/BarAggregator.h#L44) — 1분봉 → `interval_min` 봉. 판단은 언제나 이 봉으로(D-072)  
   `Quant/include/core/BarAggregator.h:44` · `std::vector<MarketData> resample(const std::vector<MarketData>& bars_1m, int interval_min, int max_count = 0);` · 시험 [test_bar_aggregator](../Quant/tests/test_bar_aggregator.cpp)
31. [`DeviationScaleStrategy::emit_liquidation`](../Quant/include/strategy/DeviationScaleStrategy.h#L282) — 청산 신호 조립 — 시장가면 `reference_price` 스탬프, 매도 가능 수량은 원장 접근자(`sellable_quantity`, 동기 잔고조회 금지)  
   `Quant/include/strategy/DeviationScaleStrategy.h:282` · `bool emit_liquidation(std::vector<OrderSignal>& out, int position, …`

리뷰할 때 볼 것:

- 전략이 문자열로 종목을 비교하는 곳이 있는지 — `same_symbol`/`trade.symbol_id` 정수 비교여야 한다(원칙 6)
- 전략 `on_*`이 블로킹 I/O(REST)를 직접 부르는지 — DeviationScale은 프리페치 스레드로 뺐다
- `reference_price`를 시장가 신호에 찍는지 — 없으면 명목 백스톱이 우회된다(Types.h 주석)
- 전략 객체를 만지는 곳이 자기 샤드 스레드뿐인지 — 종목 틱은 `RouteTable` 마스크로 그 샤드에 온다(D-110)

## 3. 디스패치 — 신호가 주문 큐에 들어가기 전 판단

디스패치 스레드는 `pipeline_.shard_out`의 단일 소비자이자 `pipeline_.order_queue`의 단일 생산자다. 순번 stamp·비활성 차단·청산 관리 종목 차단·교체 진입·강제청산 재발주·한도 초과 정리를 한 객체(`SignalDispatcher`)가 맡고, 싱크는 큐 push와 ZMQ 신호 발행을 같이 한다.

32. [`Engine::strategy_thread_fn`](../Quant/src/core/Engine.cpp#L2999) — 루프 한 바퀴: `flush_held` → `drain_manual_inbox` → 강제청산(2초 재발주) → `trim_excess_once`(기동 20초 뒤 1회) → `pipeline_.shard_out` 비우기(`from_strategy`) → 잠(상한 10ms). 국면 파일은 데이터 스레드가 읽는다(6절)  
   `Quant/src/core/Engine.cpp:2999` · `void Engine::strategy_thread_fn(std::stop_token stop_token)`
33. [`dispatch::SignalDispatcher::from_strategy`](../Quant/src/core/SignalDispatcher.cpp#L158) — 비활성 전략의 신규 매수·청산 관리(청산 관리) 종목의 신규 매수 차단 → `submit`. 판정 함수는 `set_exit_managed_check`으로 엔진이 준다  
   `Quant/src/core/SignalDispatcher.cpp:158` · `void SignalDispatcher::from_strategy(bool active, bool exit_manager, const OrderSignal& signal)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
34. [`dispatch::SignalDispatcher::submit`](../Quant/src/core/SignalDispatcher.cpp#L208) — `seq` stamp(D-038) → 슬롯이 찼으면 교체 계획(최약체 매도 뒤 매수 보류) → 싱크(= `order_queue_` push)  
   `Quant/src/core/SignalDispatcher.cpp:208` · `void SignalDispatcher::submit(OrderSignal signal)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
35. [`dispatch::SignalDispatcher::force_liquidate`](../Quant/src/core/SignalDispatcher.cpp#L252) — 보유 전량 시장가 매도를 2초 간격 재발주. `reference_price`가 여기서 찍히는지 본다  
   `Quant/src/core/SignalDispatcher.cpp:252` · `void SignalDispatcher::force_liquidate(Clock::time_point now)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
36. [`Engine::drain_manual_inbox`](../Quant/src/core/Engine.cpp#L4027) — 운영단말 수동 주문(`ops_.manual_inbox`)이 같은 싱크로 들어온다 — 생산자를 늘리지 않기 위해 이 스레드가 꺼낸다  
   `Quant/src/core/Engine.cpp:4027` · `void Engine::drain_manual_inbox(const std::function<void(const OrderSignal&)>& emit)`

리뷰할 때 볼 것:

- `pipeline_.order_queue.push`가 이 스레드에서만 일어나는지(SPSC 불변식) — 수동 주문(`ops_.manual_inbox`)도 여기서 합류하는지
- 큐가 차면 기다리지 않고 버리고 세는지(`order_dropped`, D-073)
- 교체 진입(displace) — 최약체 매도가 나간 뒤에만 매수가 풀리는지(`flush_held`)

## 4. 주문 — 큐에서 KIS까지, 거부와 재시도

주문 스레드가 유일한 시퀀서다(원칙 4). 조절기(`OrderRateLimiter`)가 간격·재시도를 정하고, 라우터가 게이트 검사 뒤 KIS에 보내고 ODNO를 기억하며, 구간 지연을 CSV에 남긴다.

37. [`Engine::order_thread_fn`](../Quant/src/core/Engine.cpp#L3269) — `take_due_retry` 우선 → 큐 pop → `wait_before_send` → `router->submit` → 성공이면 `note_sent`, 거부면 `on_rejected` → `LatencyTrace::record`. 비면 재시도 만기까지 `wait_until`  
   `Quant/src/core/Engine.cpp:3269` · `void Engine::order_thread_fn(std::stop_token stop_token)`
38. [`order_rate::OrderRateLimiter::wait_before_send`](../Quant/src/core/OrderRateLimiter.cpp#L78) — 직전 KIS 호출 뒤 최소 간격. `take_due_retry`·`on_rejected`(재시도 분류)·만기 폐기가 같은 파일  
   `Quant/src/core/OrderRateLimiter.cpp:78` · `OrderRateLimiter::Clock::duration OrderRateLimiter::wait_before_send(Clock::time_point now) const` · 시험 [test_order_rate_limiter](../Quant/tests/test_order_rate_limiter.cpp)
39. [`order_rate::OrderRateLimiter::on_rejected`](../Quant/src/core/OrderRateLimiter.cpp#L84) — 거부 → 재시도 여부·다음 시각. 유량 한도 문장은 `GateReasons.h`와 맞춰 본다  
   `Quant/src/core/OrderRateLimiter.cpp:84` · `bool OrderRateLimiter::on_rejected(Pending pending, OrderStatus status, const std::string& reject_reason, …` · 시험 [test_order_rate_limiter](../Quant/tests/test_order_rate_limiter.cpp)
40. [`OrderRouter::submit`](../Quant/src/ipc/OrderRouter.cpp#L65) — `action`으로 분기만 — NEW는 `new_route`, CANCEL·REPLACE는 `cancel_route`·`replace_route`  
   `Quant/src/ipc/OrderRouter.cpp:65` · `ManagedOrder OrderRouter::submit(const OrderSignal& signal)` · 시험 [test_order_router](../Quant/tests/test_order_router.cpp)
41. [`OrderRouter::new_route`](../Quant/src/ipc/OrderRouter.cpp#L77) — `clamp_buy_quantity` → 매도가능수량 0이면 `reconcile_blocked_sell` → 시장가 매도 중복 가드(`history_`) → `gate_.check` → `take_intent`(원장 INTENT 선기록·선점, 실패면 전송 안 함) → `kis_.submit_order_acknowledgement`(ODNO) → `gate_.on_accepted` → `record`·`write_trade_row`(`seq` 동반). 실패 경로마다 무엇이 되돌려지는지  
   `Quant/src/ipc/OrderRouter.cpp:77` · `ManagedOrder OrderRouter::new_route(const OrderSignal& in_signal)` · 시험 [test_order_router](../Quant/tests/test_order_router.cpp)
42. [`OrderGate::check`](../Quant/src/risk/OrderGate.cpp#L232) — 거부 검사 사슬 — 킬스위치 → 방향 → entry_halt(국면 자동 + 운영단말 수동, OR, D-091) → 매매 창(정규장+애프터마켓, D-097) → 1주문 수량·명목 → 종목당 포지션·명목 → 슬롯(교체·쿨다운·점수 우선) → 총노출 → 일손실 → PNL_STALE → 중복 → 유량. 순서가 곧 우선순위다. 매도가능수량은 라우터가 본다  
   `Quant/src/risk/OrderGate.cpp:232` · `bool OrderGate::check(const OrderSignal& signal, std::string& reject_reason)` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
43. [`OrderGate::clamp_buy_quantity`](../Quant/src/risk/OrderGate.cpp#L63) — 매수 수량을 현금·명목 한도로 깎는다. 0이 되면 거부  
   `Quant/src/risk/OrderGate.cpp:63` · `int OrderGate::clamp_buy_quantity(const OrderSignal& signal)` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
44. [`OrderGate::plan_displacement`](../Quant/src/risk/OrderGate.cpp#L1603) — 슬롯이 찼을 때 어느 보유를 내보낼지. 디스패처의 교체 진입이 이 계획을 쓴다  
   `Quant/src/risk/OrderGate.cpp:1603` · `OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account, symbol::SymbolId new_symbol) const` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
45. [`OrderGate::on_intent`](../Quant/src/risk/OrderGate.cpp#L718) — 전송 **전에** 원장 저널에 INTENT를 적고 `reserved_`를 선점한다(슬롯·현금). 기록에 실패하면 선점을 되돌리고 거짓을 준다 — 그 주문은 나가지 않는다(D-113). 접수 뒤 짝은 `on_accepted`, 되돌리는 짝은 `on_fill_confirmed`·`on_cancel`·`on_reject`  
   `Quant/src/risk/OrderGate.cpp:718` · `bool OrderGate::on_intent(const std::string& account, const std::string& ticker, OrderSide side, int quantity, …` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
46. [`KisClient::submit_order_acknowledgement`](../Quant/src/api/KisOrder.cpp#L152) — 현금 주문 REST. tr_id(실/모의)·`EXCG_ID_DVSN_CD`(KRX/NXT/SOR, D-096)·`authentication_headers`·응답에서 ODNO. 여기서만 KIS에 주문이 닿는다  
   `Quant/src/api/KisOrder.cpp:152` · `OrderAck KisClient::submit_order_acknowledgement(const OrderSignal& signal)`
47. [`trace::LatencyTrace::record`](../Quant/include/core/LatencyTrace.h#L128) — 틱 수신→신호→pop→라우터 반환 네 시각을 `logs/latency_trace.csv` 한 줄로  
   `Quant/include/core/LatencyTrace.h:128` · `void record(const OrderSignal& signal, const Marks& marks, bool kis_called, bool accepted);` · 시험 [test_latency_trace](../Quant/tests/test_latency_trace.cpp)

리뷰할 때 볼 것:

- 게이트 거부 사유 문장이 `GateReasons.h` 한 곳에서 오는지(D-067) — 조절기가 문장으로 분류하므로 흩어지면 깨진다
- 청산 SELL의 재시도가 BUY와 다르게 분류되는지(유량 한도는 action 불문, 40240000 제외)
- `on_intent`가 KIS 전송 **앞**에 불려 원장에 적히고 선점되는지(D-113) — 못 적으면 주문이 나가면 안 된다
- 시장가 명목 검사가 `reference_price`로 되는지(`price`=0)

## 5. 체결 — 체결통보가 원장에 닿기까지

체결통보(H0STCNI0/H0STCNI9)는 수신 스레드가 복호화·디코드만 하고 `pipeline_.fill_queue`에 push한다. 체결 스레드가 라우터의 `on_fill`로 원장·게이트를 갱신하고 운영단말에 방송한다.

48. [`KisWebSocket::parse_fill_notification`](../Quant/src/api/WebSocketClient.cpp#L897) — AES 복호화 → `decode_fill` → `on_fill_` 콜백(Engine이 `pipeline_.fill_queue.push`)  
   `Quant/src/api/WebSocketClient.cpp:897` · `void KisWebSocket::parse_fill_notification(kis_websocket::Fields fields)`
49. [`kis_websocket::decode_fill`](../Quant/include/api/KisWsDecode.h#L150) — 체결통보 필드 → `FillNotification`(ODNO·체결/거부·수량·가격). 거부 통보도 같은 채널  
   `Quant/include/api/KisWsDecode.h:150` · `Decode decode_fill(Fields fields, FillNotification& fill_notification);` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
50. [`Engine::fill_thread_fn`](../Quant/src/core/Engine.cpp#L3503) — `pipeline_.fill_queue` pop → `on_fill` → `ledger_->note_fill`(대조 5초 유예, D-074) → 운영단말 `broadcast`(FILL). 비면 `WakeGate`  
   `Quant/src/core/Engine.cpp:3503` · `void Engine::fill_thread_fn(std::stop_token stop_token)`
51. [`OrderRouter::on_fill`](../Quant/src/ipc/OrderRouter.cpp#L1885) — ODNO로 주문 찾기 → 상태 갱신 → `gate_.on_fill_confirmed` → 원장 CSV. 못 찾으면 미연결 체결 경로  
   `Quant/src/ipc/OrderRouter.cpp:1885` · `void OrderRouter::on_fill(const FillNotification& fill_notification)` · 시험 [test_order_router](../Quant/tests/test_order_router.cpp)
52. [`OrderGate::on_fill_confirmed`](../Quant/src/risk/OrderGate.cpp#L1358) — 포지션·평단·실현손익 갱신, `reserved_` 해제. `FillResult`가 실현 PnL을 돌려준다  
   `Quant/src/risk/OrderGate.cpp:1358` · `OrderGate::FillResult OrderGate::on_fill_confirmed( …` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)

리뷰할 때 볼 것:

- ODNO가 라우터 기억(`history_`)에 없는 체결(재기동 뒤 미체결)도 원장·포지션에 반영되는지(미연결 체결 경로)
- 부분 체결이 여러 번 와도 `reserved_`가 정확히 남은 수량만큼 풀리는지
- 체결 처리 중 수신 스레드가 서지 않는지 — 수신은 push만(D-056)

## 6. 제어·주기 — 파이프라인 밖에서 돌아가는 것

국면(전략 집합 선택)·잔고 대조·토큰 갱신·큐 고수위·WS 끊김 복구는 파이프라인 스레드에 걸리지 않게 데이터·제어 스레드가 돈다.

53. [`Engine::control_thread_fn`](../Quant/src/core/Engine.cpp#L3661) — 큐 고수위 1분 로그 → 토큰 선갱신(5분, 만료 30분 전) → 손익 갱신 감시(끊기면 `set_kill_switch` 보수정지) → WS stale·재연결·REST 폴백(`feed::Supervisor` 판정)  
   `Quant/src/core/Engine.cpp:3661` · `void Engine::control_thread_fn(std::stop_token stop_token)`
54. [`Engine::poll_regime_file`](../Quant/src/core/Engine.cpp#L2824) — 데이터 스레드가 부른다. `regime.json` 축 — `entry_halt`(신규매수 차단)·`entry_scale`(매수비율)·`force_liquidate`, 그리고 라벨 전이 때 `apply_regime_selection`(전략 집합 선택, D-084). 상태기계는 `RegimeFileJudge.h`  
   `Quant/src/core/Engine.cpp:2824` · `void Engine::poll_regime_file()` · 시험 [test_regime_file_judge](../Quant/tests/test_regime_file_judge.cpp)
55. [`OrderGate::set_manual_halt`](../Quant/include/risk/OrderGate.h#L331) — 운영단말 HALT_REQ의 수동 정지 — 신규 매수·전략 매도를 따로 끈다. 국면의 `entry_halt_`와는 다른 플래그고 `is_entry_halted`에서만 OR로 합친다(D-091)  
   `Quant/include/risk/OrderGate.h:331` · `void set_manual_halt(OrderSide side, bool on);` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
56. [`Engine::apply_regime_selection`](../Quant/src/core/Engine.cpp#L221) — 국면 → `regime_strategies` 집합으로 전략 활성/비활성. 청산은 하지 않는다  
   `Quant/src/core/Engine.cpp:221` · `void Engine::apply_regime_selection(Regime regime, bool force_log)`
57. [`Engine::maybe_rescan_universe`](../Quant/src/core/Engine.cpp#L428) — 유니버스 재스캔 — 빠진 보유 종목은 40초에 신규매수 차단, 600초에 전략 해제(`UniverseExit.h`, D-077). 청산 관리 보유(`exit_managed_tickers`)는 스캔 신규매수에서 뺀다  
   `Quant/src/core/Engine.cpp:428` · `void Engine::maybe_rescan_universe()` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
58. [`LedgerReconciler::reconcile`](../Quant/src/core/LedgerReconciler.cpp#L297) — 브로커 잔고 ↔ 원장. 어긋난 종목만 `RECONCILE` 행(`ReconcilePlan.h` 순수 함수). 잔고조회 서킷브레이커  
   `Quant/src/core/LedgerReconciler.cpp:297` · `void LedgerReconciler::reconcile(bool resync_positions, std::time_t now_utc)` · 시험 [test_ledger_reconciler](../Quant/tests/test_ledger_reconciler.cpp)
59. [`Engine::activate_rest_fallback`](../Quant/src/core/Engine.cpp#L3623) — WS가 stale이면 REST 현재가 폴링으로 대체 틱(`received_ns`=0). 복귀는 `deactivate_rest_fallback`  
   `Quant/src/core/Engine.cpp:3623` · `bool Engine::activate_rest_fallback(const std::string& reason)`

리뷰할 때 볼 것:

- 제어 작업이 파이프라인 락을 잡는지 — 잡으면 최악 지연이 여기서 난다
- 전략 집합 선택·비율·정지·청산이 전부 `regime.json` 라벨에서 나오는지 — 코스피 200MA 축은 지웠다(D-085)
- 대조가 체결 직후 5초를 미루되 30초마다 한 번은 도는지(`defer_after_fill`)

## 7. 종료 — 큐를 비우고 스레드를 내린다

SIGINT·운영단말 종료 → `request_shutdown` → `stop`. 체결 큐는 비울 때까지 돌고 로거는 `flush`한다.

60. [`Engine::request_shutdown`](../Quant/src/core/Engine.cpp#L1752) — 시그널 핸들러에서 불려도 되는 최소 동작(플래그·깨우기)만  
   `Quant/src/core/Engine.cpp:1752` · `void Engine::request_shutdown(std::string_view reason)`
61. [`Engine::stop`](../Quant/src/core/Engine.cpp#L1771) — stop_token 요청 → join 순서(control → order → 샤드 → strategy → data → WS 끊기 → fill 마지막, 큐를 비우고 끝난다) → ZMQ·운영단말 서버 정지 → 전략 `on_stop` → 통계 출력. 미체결 예약주문 기억은 여기서 사라진다(재기동 규칙, CLAUDE.md '장중 운영')  
   `Quant/src/core/Engine.cpp:1771` · `void Engine::stop()`

## 부록. 실계좌 없이 같은 경로를 돌리는 것

리플레이 소스와 모의 체결기가 소켓·KIS 자리에 들어간다. 위 흐름을 캡처 파일로 다시 밟을 때 본다.

62. [`feed::ReplaySource`](../Quant/include/core/ReplaySource.h#L23) — 캡처 파일 → `IFeedSource`. 내보낼 때 `received_ns`를 다시 찍는다. config `replay_file`·`replay_speed`  
   `Quant/include/core/ReplaySource.h:23` · `class ReplaySource final : public IFeedSource` · 시험 [test_replay_source](../Quant/tests/test_replay_source.cpp)
63. [`feed::PaperExecutor`](../Quant/include/core/PaperExecutor.h#L32) — `IOrderExecutor` 모의 체결기 — 주문 즉시 체결통보를 만들어 같은 `pipeline_.fill_queue` 경로로 넣는다  
   `Quant/include/core/PaperExecutor.h:32` · `class PaperExecutor final : public IOrderExecutor` · 시험 [test_paper_executor](../Quant/tests/test_paper_executor.cpp)
64. [`sync::WakeGate`](../Quant/include/core/WakeGate.h#L16) — 소비자 잠·깨우기 한 조각. 전략·주문·체결·샤드 유휴가 전부 이걸 쓴다  
   `Quant/include/core/WakeGate.h:16` · `class WakeGate` · 시험 [test_wake_gate](../Quant/tests/test_wake_gate.cpp)

## 파일별 — 한 파일을 열었을 때 이 문서의 어느 걸음인지

| 파일 | 걸음 |
|---|---|
| [Quant/include/api/KisWsDecode.h](../Quant/include/api/KisWsDecode.h) | 15 `decode_kr_trade`, 16 `decode_orderbook`, 49 `decode_fill` |
| [Quant/include/core/BarAggregator.h](../Quant/include/core/BarAggregator.h) | 30 `resample` |
| [Quant/include/core/FeedMux.h](../Quant/include/core/FeedMux.h) | 18 `FeedMux` |
| [Quant/include/core/LatencyTrace.h](../Quant/include/core/LatencyTrace.h) | 47 `record` |
| [Quant/include/core/PaperExecutor.h](../Quant/include/core/PaperExecutor.h) | 63 `PaperExecutor` |
| [Quant/include/core/ReplaySource.h](../Quant/include/core/ReplaySource.h) | 62 `ReplaySource` |
| [Quant/include/core/ShardMatrix.h](../Quant/include/core/ShardMatrix.h) | 17 `push_to` |
| [Quant/include/core/StrategyRouter.h](../Quant/include/core/StrategyRouter.h) | 24 `for_each` |
| [Quant/include/core/StrategyShard.h](../Quant/include/core/StrategyShard.h) | 23 `step`, 25 `Emitted` |
| [Quant/include/core/TickCapture.h](../Quant/include/core/TickCapture.h) | 21 `on_trade` |
| [Quant/include/core/WakeGate.h](../Quant/include/core/WakeGate.h) | 64 `WakeGate` |
| [Quant/include/risk/OrderGate.h](../Quant/include/risk/OrderGate.h) | 55 `set_manual_halt` |
| [Quant/include/strategy/DeviationScaleStrategy.h](../Quant/include/strategy/DeviationScaleStrategy.h) | 27 `on_start`, 28 `on_trade_batch`, 31 `emit_liquidation` |
| [Quant/include/strategy/StrategyBase.h](../Quant/include/strategy/StrategyBase.h) | 26 `on_trade_batch` |
| [Quant/src/api/KisOrder.cpp](../Quant/src/api/KisOrder.cpp) | 46 `submit_order_acknowledgement` |
| [Quant/src/api/WebSocketClient.cpp](../Quant/src/api/WebSocketClient.cpp) | 11 `recv_loop`, 12 `parse_message`, 13 `dispatch_record`, 14 `parse_kr_trade`, 48 `parse_fill_notification` |
| [Quant/src/core/AppConfig.cpp](../Quant/src/core/AppConfig.cpp) | 2 `parse_config` |
| [Quant/src/core/BarAggregator.cpp](../Quant/src/core/BarAggregator.cpp) | 29 `on_tick` |
| [Quant/src/core/DataPoller.cpp](../Quant/src/core/DataPoller.cpp) | 20 `poll_universe` |
| [Quant/src/core/Engine.cpp](../Quant/src/core/Engine.cpp) | 6 `add_strategy`, 7 `start`, 8 `connect_feed (WS 콜백 설치)`, 9 `spawn_threads`, 19 `data_thread_fn`, 22 `shard_thread_fn`, 32 `strategy_thread_fn`, 36 `drain_manual_inbox`, 37 `order_thread_fn`, 50 `fill_thread_fn`, 53 `control_thread_fn`, 54 `poll_regime_file`, 56 `apply_regime_selection`, 57 `maybe_rescan_universe`, 59 `activate_rest_fallback`, 60 `request_shutdown`, 61 `stop` |
| [Quant/src/core/EngineConfigure.cpp](../Quant/src/core/EngineConfigure.cpp) | 4 `configure` |
| [Quant/src/core/LedgerReconciler.cpp](../Quant/src/core/LedgerReconciler.cpp) | 10 `bootstrap`, 58 `reconcile` |
| [Quant/src/core/OrderRateLimiter.cpp](../Quant/src/core/OrderRateLimiter.cpp) | 38 `wait_before_send`, 39 `on_rejected` |
| [Quant/src/core/SignalDispatcher.cpp](../Quant/src/core/SignalDispatcher.cpp) | 33 `from_strategy`, 34 `submit`, 35 `force_liquidate` |
| [Quant/src/ipc/OrderRouter.cpp](../Quant/src/ipc/OrderRouter.cpp) | 40 `submit`, 41 `new_route`, 51 `on_fill` |
| [Quant/src/main.cpp](../Quant/src/main.cpp) | 1 `main`, 3 `run_trade` |
| [Quant/src/risk/OrderGate.cpp](../Quant/src/risk/OrderGate.cpp) | 42 `check`, 43 `clamp_buy_quantity`, 44 `plan_displacement`, 45 `on_intent`, 52 `on_fill_confirmed` |
| [Quant/src/strategy/StrategyFactory.cpp](../Quant/src/strategy/StrategyFactory.cpp) | 5 `load_strategies` |

## 이 문서를 고치는 법

- 걸음을 더하거나 순서를 바꾼다: `docs/code_flow.toml`의 `[[stage.step]]`. 심볼과 파일만 적으면 줄은 생성기가 찾는다.
- 함수 이름이 바뀌어 `[missing]`이 나면: 명세의 `sym`을 새 이름으로. 자리가 없어졌으면 걸음을 지운다.
- 이름 없는 자리(람다·블록)는 `match` 정규식으로 가리킨다.
- 그림(`diagram`)과 요약(`summary`)은 사람이 쓴다 — 스레드·큐가 바뀌면 같은 커밋에서 고친다.

