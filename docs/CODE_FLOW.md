# 실시간 매매 코드 흐름 (CODE_FLOW)

<!-- drift-check: snapshot — 줄 번호는 생성 시점 소스 기준, scripts/gen_code_flow.py가 다시 만든다 -->
> 자동 생성물. 손편집 금지 — 읽는 순서는 `docs/code_flow.toml`에 적고 `py scripts/gen_code_flow.py`로 다시 만든다.
> 줄 번호·시그니처는 생성 시점의 소스에서 찍었다. 심볼이 사라지면 `--check`가 막는다(D-078).

TRADE 모드에서 틱 하나가 들어와 주문이 나가고 체결이 원장에 닿기까지를 **읽는 순서대로** 나열한 문서다.
단계마다 심볼 링크(`파일#L줄`)·시그니처·볼 것 한 줄·덮는 테스트가 있다. 링크는 VS Code 미리보기·GitHub에서 그 줄로 열린다.

스레드 모델·설계 원칙은 [CLAUDE.md](../CLAUDE.md)의 "설계 목표와 원칙"·"아키텍처" 절이 정본이고, 모듈 간 include 관계는
[CODE_GRAPH.md](CODE_GRAPH.md), 바꾼 이유는 [DECISIONS.md](DECISIONS.md)다. 이 문서는 그 셋을 대신하지 않고 **입구**만 맡는다.

## 한 장 그림

```mermaid
flowchart LR
  subgraph recv[1. 수신 — 소켓 스레드 N]
    WS[KisWebSocket::recv_loop] --> DEC[kis_ws::decode_*] --> CB[Engine WS 콜백]
  end
  subgraph mx[SPSC 링 행렬]
    CB --> TD[td_mx_ / ob_mx_ (행=수신 레인, 열=샤드)]
    DP[DataPoller REST 대체 틱] --> TD
  end
  subgraph shard[2. 샤드 스레드 M]
    TD --> ST[strat::Shard::step] --> RT[strat::Router::for_each] --> S[StrategyBase::on_trade_batch]
    S --> EM[strat::Emitted]
  end
  EM --> SO[shard_out_ MpscQueue]
  subgraph disp[3. 디스패치 스레드]
    SO --> SD[SignalDispatcher::from_strategy] --> OQ[order_queue_ SPSC]
  end
  subgraph ord[4. 주문 스레드]
    OQ --> PC[OrderPacer] --> OR[OrderRouter::submit] --> OG[OrderGate::check] --> KIS[KisClient::submit_order_ack]
  end
  subgraph fill[5. 체결 스레드]
    FN[parse_fill_notification] --> FQ[fill_queue_ SPSC] --> OF[OrderRouter::on_fill] --> OGF[OrderGate::on_fill_confirmed]
  end
  KIS -. ODNO .-> OR
  WS -. H0STCNI9 .-> FN
```

## 읽는 순서

- [0. 기동 — config에서 스레드가 서기까지](#0-기동-config에서-스레드가-서기까지) — 7걸음
- [1. 수신 — 소켓 바이트에서 링 행렬까지](#1-수신-소켓-바이트에서-링-행렬까지) — 11걸음
- [2. 샤드 — 틱이 전략을 만나 신호가 되기까지](#2-샤드-틱이-전략을-만나-신호가-되기까지) — 10걸음
- [3. 디스패치 — 신호가 주문 큐에 들어가기 전 판단](#3-디스패치-신호가-주문-큐에-들어가기-전-판단) — 6걸음
- [4. 주문 — 큐에서 KIS까지, 거부와 재시도](#4-주문-큐에서-kis까지-거부와-재시도) — 10걸음
- [5. 체결 — 체결통보가 원장에 닿기까지](#5-체결-체결통보가-원장에-닿기까지) — 5걸음
- [6. 제어·주기 — 파이프라인 밖에서 돌아가는 것](#6-제어주기-파이프라인-밖에서-돌아가는-것) — 5걸음
- [7. 종료 — 큐를 비우고 스레드를 내린다](#7-종료-큐를-비우고-스레드를-내린다) — 2걸음
- [부록. 실계좌 없이 같은 경로를 돌리는 것](#부록-실계좌-없이-같은-경로를-돌리는-것) — 3걸음

## 0. 기동 — config에서 스레드가 서기까지

config를 읽고 전략을 만들고 `Engine::start()`가 행렬·샤드·원장·소켓 콜백·스레드 다섯을 세운다. 이 절만 읽으면 누가 무엇을 소유하는지 보인다.

1. [`main`](../Quant/src/main.cpp#L389) — 진입. `load_config` → `Engine engine(...)` → `set_*` 배선 → `load_strategies` → `engine.start()` 순서를 훑는다  
   `Quant/src/main.cpp:389` · `int main(int argc, char* argv[])`
2. [`load_strategies`](../Quant/src/strategy/StrategyFactory.cpp#L988) — config `strategies[]`를 전략 객체로. 새 전략을 붙이는 자리(CLAUDE.md '전략 추가하기')  
   `Quant/src/strategy/StrategyFactory.cpp:988` · `void load_strategies(StrategyLoadCtx& ctx, const json& strategies)`
3. [`Engine::add_strategy`](../Quant/src/core/Engine.cpp#L33) — 전략 등록. 심볼 해석기(`set_symbol_resolver` → `SymbolTable::intern`)가 여기서 주입된다  
   `Quant/src/core/Engine.cpp:33` · `void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)`
4. [`Engine::start`](../Quant/src/core/Engine.cpp#L1077) — 행렬 `reshape`(행=수신 레인+폴러, 열=샤드) → 샤드 생성 → 원장 시드 → WS 연결·콜백 → jthread 다섯. 아래 두 걸음은 이 함수 안이다  
   `Quant/src/core/Engine.cpp:1077` · `void Engine::start()`
5. [`Engine::start (WS 콜백 설치)`](../Quant/src/core/Engine.cpp#L946) — 소켓 레인 i의 호가·체결 콜백. 종목 id로 열을 고르고(`consumer_of`) 자기 행에 `push_to` — 가득 차면 버리고 센다(블로킹 금지)  
   `Quant/src/core/Engine.cpp:946` · `feed_.ws->set_lane_callbacks([this] (uint32_t lane, const OrderBook& in) …`
6. [`Engine::start (스레드 기동)`](../Quant/src/core/Engine.cpp#L1064) — data·strategy·order·fill·control 다섯 jthread + 샤드 M. stop_token이 첫 인자라 람다로 감싼다  
   `Quant/src/core/Engine.cpp:1064` · `data_thread_     = std::jthread([this] (std::stop_token st) { data_thread_fn(st); });`
7. [`LedgerReconciler::bootstrap`](../Quant/src/core/LedgerReconciler.cpp#L20) — 기동 잔고 시드 — 브로커 잔고를 원장·게이트 포지션으로. 실패 재시도 횟수와 실패 시 기동 중단 여부  
   `Quant/src/core/LedgerReconciler.cpp:20` · `bool LedgerReconciler::bootstrap(int attempts, std::chrono::milliseconds retry_delay)` · 시험 [test_ledger_reconciler](../Quant/tests/test_ledger_reconciler.cpp)

리뷰할 때 볼 것:

- 스레드 소유권 — 각 큐의 생산자·소비자가 하나씩인지(`RingBuffer`는 SPSC, 생산자 둘이면 `MpscQueue`, 원칙 5)
- config 키가 `set_*`로 하나씩 들어가는지 — 빠진 키의 기본값이 실계좌에 안전한 쪽인지
- 기동 순서 — 원장 시드(`bootstrap`)가 소켓 콜백 설치보다 앞인지(체결이 먼저 오면 원장이 비어 있다)

## 1. 수신 — 소켓 바이트에서 링 행렬까지

소켓 읽기 스레드는 얇다(원칙 3): 프레임 → 필드 분리 → 구조체 → `recv_ns` 스탬프 → 행렬 push. 문자열은 여기서 끝나고 종목은 정수 id가 된다(원칙 6).

8. [`KisWebSocket::recv_loop`](../Quant/src/api/WebSocketClient.cpp#L134) — 소켓 하나 = 스레드 하나(원칙 1). 프레임 읽기 → `parse_message`. 끊김 감지와 재연결 신호  
   `Quant/src/api/WebSocketClient.cpp:134` · `void KisWebSocket::recv_loop(std::stop_token st)` · 시험 [test_ws_frame](../Quant/tests/test_ws_frame.cpp)
9. [`KisWebSocket::parse_message`](../Quant/src/api/WebSocketClient.cpp#L527) — `|`로 헤더 분리 → 암호화 여부(체결통보는 AES) → `dispatch_record`. PINGPONG·구독 응답 처리도 여기  
   `Quant/src/api/WebSocketClient.cpp:527` · `void KisWebSocket::parse_message(const std::string& msg)`
10. [`KisWebSocket::dispatch_record`](../Quant/src/api/WebSocketClient.cpp#L706) — tr_id로 채널 분기 — H0STCNT0 체결·H0STASP0 호가·H0IFCNT0/H0IFASP0 선물·H0STCNI9 체결통보  
   `Quant/src/api/WebSocketClient.cpp:706` · `void KisWebSocket::dispatch_record(std::string_view tr_id, kis_ws::Fields f)`
11. [`KisWebSocket::parse_kr_trade`](../Quant/src/api/WebSocketClient.cpp#L787) — `decode_kr_trade` → `td.sym`(SymbolTable) → `recv_ns` 스탬프 → `on_trade_` 콜백. 호가는 `parse_orderbook`이 같은 모양  
   `Quant/src/api/WebSocketClient.cpp:787` · `void KisWebSocket::parse_kr_trade(kis_ws::Fields f)`
12. [`kis_ws::decode_kr_trade`](../Quant/include/api/KisWsDecode.h#L242) — 순수 함수. 필드 인덱스 → `TradeData`(가격·수량·`hhmmss` 정수·방향). 필드 번호가 [wire] 정본  
   `Quant/include/api/KisWsDecode.h:242` · `inline Decode decode_kr_trade(Fields f, TradeData& td)` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
13. [`kis_ws::decode_orderbook`](../Quant/include/api/KisWsDecode.h#L226) — 5단계 호가 → `OrderBook`. 매도·매수 가격/잔량 필드 위치  
   `Quant/include/api/KisWsDecode.h:226` · `inline Decode decode_orderbook(Fields f, OrderBook& ob)` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
14. [`shard::Matrix::push_to`](../Quant/include/core/ShardMatrix.h#L85) — 행(생산자)×열(소비자) SPSC 셀에 push. `consumer_of(sym)`이 종목 해시로 열을 고른다(원칙 2)  
   `Quant/include/core/ShardMatrix.h:85` · `[[nodiscard]] bool push_to(uint32_t producer, uint32_t consumer, const T& v)` · 시험 [test_shard_matrix](../Quant/tests/test_shard_matrix.cpp)
15. [`feed::FeedMux`](../Quant/include/core/FeedMux.h#L33) — 소켓 여럿을 한 `IFeedSource`로. 레인 모드면 소켓 i 스레드가 행 i로 직접 push(mux 스레드 없음). 체결통보는 첫 소켓만  
   `Quant/include/core/FeedMux.h:33` · `class FeedMux final : public IFeedSource` · 시험 [test_feed_mux](../Quant/tests/test_feed_mux.cpp)
16. [`Engine::data_thread_fn`](../Quant/src/core/Engine.cpp#L1300) — REST 축 — 봉 폴링·국면 재평가·잔고 대조·유니버스 재스캔. 폴러의 대체 틱은 `td_mx_`의 자기 행(`data_row_`)으로 간다  
   `Quant/src/core/Engine.cpp:1300` · `void Engine::data_thread_fn(std::stop_token st)`
17. [`poller::DataPoller::poll_universe`](../Quant/src/core/DataPoller.cpp#L11) — REST 현재가 → 대체 `TradeData`(`recv_ns`=0). 구독 상한 넘침·틱 끊긴 보유 보충(`top_up`)도 이 클래스  
   `Quant/src/core/DataPoller.cpp:11` · `int DataPoller::poll_universe(const std::vector<WatchSpec>& specs, std::time_t now_utc)` · 시험 [test_data_poller](../Quant/tests/test_data_poller.cpp)
18. [`feed::TickCapture::on_trade`](../Quant/include/core/TickCapture.h#L223) — raw 틱 append-only 캡처(원칙 8). 리플레이(`ReplaySource`)의 입력  
   `Quant/include/core/TickCapture.h:223` · `void on_trade(const TradeData& td) noexcept` · 시험 [test_tick_capture](../Quant/tests/test_tick_capture.cpp)

리뷰할 때 볼 것:

- 수신 스레드에 힙 할당·문자열 생성·락이 있는지 — 있으면 소비자로 옮길 후보
- `recv_ns`가 디코드 직후 한 번만 찍히는지(호가·체결 같은 시계, D-071)
- 가득 찬 링에 블로킹하지 않는지 — 버리고 세는지(`dropped` 카운터가 로그에 나오는지)

## 2. 샤드 — 틱이 전략을 만나 신호가 되기까지

샤드 m은 자기 열만 비운다. 틱의 종목 id로 그 종목을 보는 전략만 방문하고(`Router`), `NONE`이 아닌 신호를 봉투에 싸서 `shard_out_`에 넣는다. 전략 코드 리뷰는 이 절에서 시작한다.

19. [`Engine::shard_thread_fn`](../Quant/src/core/Engine.cpp#L2164) — 전략 집합 버전이 바뀌면 `rebuild`, 아니면 `step`. 비면 `WakeGate`로 잠든다. `emit`은 `shard_out_` push + 디스패치 스레드 깨우기  
   `Quant/src/core/Engine.cpp:2164` · `void Engine::shard_thread_fn(std::stop_token st, uint32_t m)`
20. [`strat::Shard::step`](../Quant/include/core/StrategyShard.h#L141) — 열의 호가·체결·봉 셀을 순서대로 비우고 전략 배치 훅을 부른다. `on_price`로 현재가 캐시 갱신  
   `Quant/include/core/StrategyShard.h:141` · `bool step(Emit&& emit, OnPrice&& on_price, SymOf&& sym_of)` · 시험 [test_strategy_shard](../Quant/tests/test_strategy_shard.cpp)
21. [`strat::Router::for_each`](../Quant/include/core/StrategyRouter.h#L78) — 종목 id → 그 종목을 구독한 전략 목록. 구독을 안 밝힌 전략은 전부 받는다  
   `Quant/include/core/StrategyRouter.h:78` · `void for_each(sym::SymbolId id, Fn&& fn) const` · 시험 [test_strategy_router](../Quant/tests/test_strategy_router.cpp)
22. [`strat::Emitted`](../Quant/include/core/StrategyShard.h#L68) — 샤드 → 디스패치 봉투: 신호 + 전략 id + 활성 여부. 이 구조체가 두 스레드의 계약이다  
   `Quant/include/core/StrategyShard.h:68` · `struct Emitted`
23. [`StrategyBase::on_trade_batch`](../Quant/include/strategy/StrategyBase.h#L55) — 전략 훅의 계약(가상 함수 다섯). 기본 구현은 `on_trade` 하나를 out에 담는다. `symbol_of`·`same_symbol`도 이 헤더  
   `Quant/include/strategy/StrategyBase.h:55` · `virtual void on_trade_batch(const TradeData&, std::vector<OrderSignal>& /*out*/)`
24. [`DeviationScaleStrategy::on_start`](../Quant/include/strategy/DeviationScaleStrategy.h#L234) — 종목 id 받기·REST 봉 시드·프리페치 스레드. 전략 하나를 끝까지 따라가는 예로 이 전략을 쓴다  
   `Quant/include/strategy/DeviationScaleStrategy.h:234` · `void on_start() override`
25. [`DeviationScaleStrategy::on_trade_batch`](../Quant/include/strategy/DeviationScaleStrategy.h#L294) — 틱 → `agg_.on_tick` → 판단 직전 `close_stale`·`bars::resample` → 진입/청산 판단 → out. 매매 로직의 본체  
   `Quant/include/strategy/DeviationScaleStrategy.h:294` · `void on_trade_batch(const TradeData& td, std::vector<OrderSignal>& out) override`
26. [`bars::BarAggregator::on_tick`](../Quant/src/core/BarAggregator.cpp#L194) — 체결 틱을 1분봉으로. `close_stale`은 틱이 없어도 시계로 지난 분을 닫는다(D-074)  
   `Quant/src/core/BarAggregator.cpp:194` · `bool BarAggregator::on_tick(const TradeData& td)` · 시험 [test_bar_aggregator](../Quant/tests/test_bar_aggregator.cpp)
27. [`bars::resample`](../Quant/include/core/BarAggregator.h#L44) — 1분봉 → `interval_min` 봉. 판단은 언제나 이 봉으로(D-072)  
   `Quant/include/core/BarAggregator.h:44` · `std::vector<MarketData> resample(const std::vector<MarketData>& bars_1m, int interval_min, int max_count = 0);` · 시험 [test_bar_aggregator](../Quant/tests/test_bar_aggregator.cpp)
28. [`DeviationScaleStrategy::emit_liquidation`](../Quant/include/strategy/DeviationScaleStrategy.h#L1304) — 청산 신호 조립 — 시장가면 `ref_price` 스탬프, 매도 가능 수량은 원장 접근자(`sellable_qty`, 동기 잔고조회 금지)  
   `Quant/include/strategy/DeviationScaleStrategy.h:1304` · `bool emit_liquidation(std::vector<OrderSignal>& out, int pos, …`

리뷰할 때 볼 것:

- 전략이 문자열로 종목을 비교하는 곳이 있는지 — `same_symbol`/`td.sym` 정수 비교여야 한다(원칙 6)
- 전략 `on_*`이 블로킹 I/O(REST)를 직접 부르는지 — DeviationScale은 프리페치 스레드로 뺐다
- `ref_price`를 시장가 신호에 찍는지 — 없으면 명목 백스톱이 우회된다(Types.h 주석)
- 한 전략의 구독 종목이 열 둘에 걸치지 않는지(`owner_shard` 경고)

## 3. 디스패치 — 신호가 주문 큐에 들어가기 전 판단

디스패치 스레드는 `shard_out_`의 단일 소비자이자 `order_queue_`의 단일 생산자다. 순번 stamp·비활성 차단·교체 진입·강제청산 재발주·한도 초과 정리를 한 객체(`SignalDispatcher`)가 맡는다.

29. [`Engine::strategy_thread_fn`](../Quant/src/core/Engine.cpp#L1957) — 루프 한 바퀴: `poll_regime_file` → `flush_held` → 강제청산 → `trim_excess_once` → `shard_out_` 비우기(`from_strategy`) → 수동 주문 → 잠  
   `Quant/src/core/Engine.cpp:1957` · `void Engine::strategy_thread_fn(std::stop_token st)`
30. [`Engine::poll_regime_file`](../Quant/src/core/Engine.cpp#L1875) — `regime.json` 축 — `entry_halt`(신규매수 차단)·`entry_scale`(매수비율)·`force_liquidate`, 그리고 라벨 전이 때 `apply_regime_selection`(전략 집합 선택, D-084). 상태기계는 `RegimeFileBridge.h`  
   `Quant/src/core/Engine.cpp:1875` · `void Engine::poll_regime_file()` · 시험 [test_regime_bridge](../Quant/tests/test_regime_bridge.cpp)
31. [`dispatch::SignalDispatcher::from_strategy`](../Quant/src/core/SignalDispatcher.cpp#L120) — 비활성 전략·청산 관리 중 종목의 신규 차단 → `submit`  
   `Quant/src/core/SignalDispatcher.cpp:120` · `void SignalDispatcher::from_strategy(bool active, const std::string& strategy_id, const OrderSignal& sig)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
32. [`dispatch::SignalDispatcher::submit`](../Quant/src/core/SignalDispatcher.cpp#L147) — `seq` stamp(D-038) → 슬롯이 찼으면 교체 계획(최약체 매도 뒤 매수 보류) → 싱크(= `order_queue_` push)  
   `Quant/src/core/SignalDispatcher.cpp:147` · `void SignalDispatcher::submit(const OrderSignal& sig)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
33. [`dispatch::SignalDispatcher::force_liquidate`](../Quant/src/core/SignalDispatcher.cpp#L245) — 보유 전량 시장가 매도를 2초 간격 재발주. `ref_price`가 여기서 찍히는지 본다  
   `Quant/src/core/SignalDispatcher.cpp:245` · `void SignalDispatcher::force_liquidate(Clock::time_point now)` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
34. [`Engine::drain_manual_inbox`](../Quant/src/core/Engine.cpp#L2766) — 운영단말 수동 주문이 같은 싱크로 들어온다 — 생산자를 늘리지 않기 위해 이 스레드가 꺼낸다  
   `Quant/src/core/Engine.cpp:2766` · `void Engine::drain_manual_inbox(const std::function<void(const OrderSignal&)>& emit)`

리뷰할 때 볼 것:

- `order_queue_.push`가 이 스레드에서만 일어나는지(SPSC 불변식) — 수동 주문(`manual_inbox_`)도 여기서 합류하는지
- 큐가 차면 기다리지 않고 버리고 세는지(`order_dropped`, D-073)
- 교체 진입(displace) — 최약체 매도가 나간 뒤에만 매수가 풀리는지(`flush_held`)

## 4. 주문 — 큐에서 KIS까지, 거부와 재시도

주문 스레드가 유일한 시퀀서다(원칙 4). 조절기(`OrderPacer`)가 간격·재시도를 정하고, 라우터가 게이트 검사 뒤 KIS에 보내고 ODNO를 기억하며, 구간 지연을 CSV에 남긴다.

35. [`Engine::order_thread_fn`](../Quant/src/core/Engine.cpp#L2292) — `take_due_retry` 우선 → 큐 pop → `wait_before_send` → `router->submit` → 성공이면 `note_sent`, 거부면 `on_rejected` → `LatencyTrace::record`. 비면 재시도 만기까지 `wait_until`  
   `Quant/src/core/Engine.cpp:2292` · `void Engine::order_thread_fn(std::stop_token st)`
36. [`pacing::OrderPacer::wait_before_send`](../Quant/src/core/OrderPacer.cpp#L75) — 직전 KIS 호출 뒤 최소 간격. `take_due_retry`·`on_rejected`(재시도 분류)·만기 폐기가 같은 파일  
   `Quant/src/core/OrderPacer.cpp:75` · `OrderPacer::Clock::duration OrderPacer::wait_before_send(Clock::time_point now) const` · 시험 [test_order_pacer](../Quant/tests/test_order_pacer.cpp)
37. [`pacing::OrderPacer::on_rejected`](../Quant/src/core/OrderPacer.cpp#L81) — 거부 → 재시도 여부·다음 시각. 유량 한도 문장은 `GateReasons.h`와 맞춰 본다  
   `Quant/src/core/OrderPacer.cpp:81` · `bool OrderPacer::on_rejected(const Pending& p, OrderStatus status, const std::string& reject_reason, …` · 시험 [test_order_pacer](../Quant/tests/test_order_pacer.cpp)
38. [`OrderRouter::submit`](../Quant/src/ipc/OrderRouter.cpp#L52) — `gate_.check` → `kis_.submit_order_ack`(ODNO·KRX 조직번호) → `gate_.on_accept` → 원장 CSV 행(`seq` 동반). 실패 경로마다 무엇이 되돌려지는지  
   `Quant/src/ipc/OrderRouter.cpp:52` · `ManagedOrder OrderRouter::submit(const OrderSignal& sig)` · 시험 [test_order_router](../Quant/tests/test_order_router.cpp)
39. [`OrderGate::check`](../Quant/src/risk/OrderGate.cpp#L222) — 거부 검사 사슬 — 보수정지·entry_halt·일손실·명목·슬롯·유량·매도 가능 수량. 순서가 곧 우선순위다  
   `Quant/src/risk/OrderGate.cpp:222` · `bool OrderGate::check(const OrderSignal& sig, std::string& reject_reason)` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
40. [`OrderGate::clamp_buy_qty`](../Quant/src/risk/OrderGate.cpp#L53) — 매수 수량을 현금·명목 한도로 깎는다. 0이 되면 거부  
   `Quant/src/risk/OrderGate.cpp:53` · `int OrderGate::clamp_buy_qty(const OrderSignal& sig)` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
41. [`OrderGate::plan_displacement`](../Quant/src/risk/OrderGate.cpp#L1196) — 슬롯이 찼을 때 어느 보유를 내보낼지. 디스패처의 교체 진입이 이 계획을 쓴다  
   `Quant/src/risk/OrderGate.cpp:1196` · `OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account, …` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
42. [`OrderGate::on_accept`](../Quant/src/risk/OrderGate.cpp#L662) — `reserved_` 선점(슬롯·현금). 체결·취소에서 되돌리는 짝은 `on_fill_confirmed`·`on_cancel`  
   `Quant/src/risk/OrderGate.cpp:662` · `void OrderGate::on_accept(const std::string& account, const std::string& ticker, …` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)
43. [`KisClient::submit_order_ack`](../Quant/src/api/KisOrder.cpp#L138) — 현금 주문 REST. tr_id(실/모의)·`auth_headers`·응답에서 ODNO. 여기서만 KIS에 주문이 닿는다  
   `Quant/src/api/KisOrder.cpp:138` · `OrderAck KisClient::submit_order_ack(const OrderSignal& signal)`
44. [`trace::LatencyTrace::record`](../Quant/include/core/LatencyTrace.h#L81) — 틱 수신→신호→pop→라우터 반환 네 시각을 `logs/latency_trace.csv` 한 줄로  
   `Quant/include/core/LatencyTrace.h:81` · `void record(const OrderSignal& sig, const Marks& m, bool kis_called, bool accepted)` · 시험 [test_latency_trace](../Quant/tests/test_latency_trace.cpp)

리뷰할 때 볼 것:

- 게이트 거부 사유 문장이 `GateReasons.h` 한 곳에서 오는지(D-067) — 조절기가 문장으로 분류하므로 흩어지면 깨진다
- 청산 SELL의 재시도가 BUY와 다르게 분류되는지(유량 한도는 action 불문, 40240000 제외)
- `on_accept`가 KIS ack 뒤에만 불려 `reserved_`가 선점되는지 — 거부인데 선점되면 슬롯이 샌다
- 시장가 명목 검사가 `ref_price`로 되는지(`price`=0)

## 5. 체결 — 체결통보가 원장에 닿기까지

체결통보(H0STCNI9)는 수신 스레드가 복호화·디코드만 하고 `fill_queue_`에 push한다. 체결 스레드가 라우터의 `on_fill`로 원장·게이트를 갱신하고 운영단말에 방송한다.

45. [`KisWebSocket::parse_fill_notification`](../Quant/src/api/WebSocketClient.cpp#L873) — AES 복호화 → `decode_fill` → `on_fill_` 콜백(Engine이 `fill_queue_.push`)  
   `Quant/src/api/WebSocketClient.cpp:873` · `void KisWebSocket::parse_fill_notification(kis_ws::Fields f)`
46. [`kis_ws::decode_fill`](../Quant/include/api/KisWsDecode.h#L334) — 체결통보 필드 → `FillNotification`(ODNO·체결/거부·수량·가격). 거부 통보도 같은 채널  
   `Quant/include/api/KisWsDecode.h:334` · `inline Decode decode_fill(Fields f, FillNotification& fn)` · 시험 [test_ws_decode](../Quant/tests/test_ws_decode.cpp)
47. [`Engine::fill_thread_fn`](../Quant/src/core/Engine.cpp#L2398) — `fill_queue_` pop → `on_fill` → `ledger_->note_fill`(대조 5초 유예, D-074) → 운영단말 `broadcast`. 비면 `WakeGate`  
   `Quant/src/core/Engine.cpp:2398` · `void Engine::fill_thread_fn(std::stop_token st)`
48. [`OrderRouter::on_fill`](../Quant/src/ipc/OrderRouter.cpp#L1578) — ODNO로 주문 찾기 → 상태 갱신 → `gate_.on_fill_confirmed` → 원장 CSV. 못 찾으면 미연결 체결 경로  
   `Quant/src/ipc/OrderRouter.cpp:1578` · `void OrderRouter::on_fill(const FillNotification& fn)` · 시험 [test_order_router](../Quant/tests/test_order_router.cpp)
49. [`OrderGate::on_fill_confirmed`](../Quant/src/risk/OrderGate.cpp#L963) — 포지션·평단·실현손익 갱신, `reserved_` 해제. `FillResult`가 실현 PnL을 돌려준다  
   `Quant/src/risk/OrderGate.cpp:963` · `OrderGate::FillResult OrderGate::on_fill_confirmed( …` · 시험 [test_order_gate](../Quant/tests/test_order_gate.cpp)

리뷰할 때 볼 것:

- ODNO가 라우터 기억(`history_`)에 없는 체결(재기동 뒤 미체결)도 원장·포지션에 반영되는지(미연결 체결 경로)
- 부분 체결이 여러 번 와도 `reserved_`가 정확히 남은 수량만큼 풀리는지
- 체결 처리 중 수신 스레드가 서지 않는지 — 수신은 push만(D-056)

## 6. 제어·주기 — 파이프라인 밖에서 돌아가는 것

국면(전략 집합 선택)·잔고 대조·토큰 갱신·큐 고수위·WS 끊김 복구는 파이프라인 스레드에 걸리지 않게 데이터·제어 스레드가 돈다.

50. [`Engine::control_thread_fn`](../Quant/src/core/Engine.cpp#L2551) — 큐 고수위 1분 로그 → 토큰 선갱신(5분, 만료 30분 전) → 손익 갱신 감시(끊기면 보수정지) → WS stale·재연결·REST 폴백  
   `Quant/src/core/Engine.cpp:2551` · `void Engine::control_thread_fn(std::stop_token st)`
51. [`Engine::apply_regime_selection`](../Quant/src/core/Engine.cpp#L148) — 국면 → `regime_strategies` 집합으로 전략 활성/비활성. 청산은 하지 않는다  
   `Quant/src/core/Engine.cpp:148` · `void Engine::apply_regime_selection(Regime r, bool force_log)`
52. [`Engine::maybe_rescan_universe`](../Quant/src/core/Engine.cpp#L265) — 유니버스 재스캔 — 빠진 보유 종목은 40초에 신규매수 차단, 600초에 전략 해제(`UniverseExit.h`, D-077)  
   `Quant/src/core/Engine.cpp:265` · `void Engine::maybe_rescan_universe()` · 시험 [test_signal_dispatcher](../Quant/tests/test_signal_dispatcher.cpp)
53. [`ledger::LedgerReconciler::reconcile`](../Quant/src/core/LedgerReconciler.cpp#L291) — 브로커 잔고 ↔ 원장. 어긋난 종목만 `RECONCILE` 행(`ReconcilePlan.h` 순수 함수). 잔고조회 서킷브레이커  
   `Quant/src/core/LedgerReconciler.cpp:291` · `void LedgerReconciler::reconcile(bool resync_positions, std::time_t now_utc)` · 시험 [test_ledger_reconciler](../Quant/tests/test_ledger_reconciler.cpp)
54. [`Engine::activate_rest_fallback`](../Quant/src/core/Engine.cpp#L2513) — WS가 stale이면 REST 현재가 폴링으로 대체 틱(`recv_ns`=0). 복귀는 `deactivate_rest_fallback`  
   `Quant/src/core/Engine.cpp:2513` · `bool Engine::activate_rest_fallback(const std::string& reason)`

리뷰할 때 볼 것:

- 제어 작업이 파이프라인 락을 잡는지 — 잡으면 최악 지연이 여기서 난다
- 전략 집합 선택·비율·정지·청산이 전부 `regime.json` 라벨에서 나오는지 — 코스피 200MA 축은 지웠다(D-085)
- 대조가 체결 직후 5초를 미루되 30초마다 한 번은 도는지(`defer_after_fill`)

## 7. 종료 — 큐를 비우고 스레드를 내린다

SIGINT·운영단말 종료 → `request_shutdown` → `stop`. 체결 큐는 비울 때까지 돌고 로거는 `flush`한다.

55. [`Engine::request_shutdown`](../Quant/src/core/Engine.cpp#L1207) — 시그널 핸들러에서 불려도 되는 최소 동작(플래그·깨우기)만  
   `Quant/src/core/Engine.cpp:1207` · `void Engine::request_shutdown()`
56. [`Engine::stop`](../Quant/src/core/Engine.cpp#L1222) — stop_token 요청 → join 순서(수신 먼저, 체결 마지막) → 통계 출력. 미체결 예약주문 기억은 여기서 사라진다(재기동 규칙, CLAUDE.md '장중 운영')  
   `Quant/src/core/Engine.cpp:1222` · `void Engine::stop()`

## 부록. 실계좌 없이 같은 경로를 돌리는 것

리플레이 소스와 모의 체결기가 소켓·KIS 자리에 들어간다. 위 흐름을 캡처 파일로 다시 밟을 때 본다.

57. [`ReplaySource`](../Quant/include/core/ReplaySource.h#L21) — 캡처 파일 → `IFeedSource`. 내보낼 때 `recv_ns`를 다시 찍는다. config `replay_file`·`replay_speed`  
   `Quant/include/core/ReplaySource.h:21` · `class ReplaySource final : public IFeedSource` · 시험 [test_replay_source](../Quant/tests/test_replay_source.cpp)
58. [`PaperExecutor`](../Quant/include/core/PaperExecutor.h#L27) — `IOrderExecutor` 모의 체결기 — 주문 즉시 체결통보를 만들어 같은 `fill_queue_` 경로로 넣는다  
   `Quant/include/core/PaperExecutor.h:27` · `class PaperExecutor final : public IOrderExecutor` · 시험 [test_paper_executor](../Quant/tests/test_paper_executor.cpp)
59. [`sync::WakeGate`](../Quant/include/core/WakeGate.h#L16) — 소비자 잠·깨우기 한 조각. 전략·주문·체결·샤드 유휴가 전부 이걸 쓴다  
   `Quant/include/core/WakeGate.h:16` · `class WakeGate` · 시험 [test_wake_gate](../Quant/tests/test_wake_gate.cpp)

## 파일별 — 한 파일을 열었을 때 이 문서의 어느 걸음인지

| 파일 | 걸음 |
|---|---|
| [Quant/include/api/KisWsDecode.h](../Quant/include/api/KisWsDecode.h) | 12 `decode_kr_trade`, 13 `decode_orderbook`, 46 `decode_fill` |
| [Quant/include/core/BarAggregator.h](../Quant/include/core/BarAggregator.h) | 27 `resample` |
| [Quant/include/core/FeedMux.h](../Quant/include/core/FeedMux.h) | 15 `FeedMux` |
| [Quant/include/core/LatencyTrace.h](../Quant/include/core/LatencyTrace.h) | 44 `record` |
| [Quant/include/core/PaperExecutor.h](../Quant/include/core/PaperExecutor.h) | 58 `PaperExecutor` |
| [Quant/include/core/ReplaySource.h](../Quant/include/core/ReplaySource.h) | 57 `ReplaySource` |
| [Quant/include/core/ShardMatrix.h](../Quant/include/core/ShardMatrix.h) | 14 `push_to` |
| [Quant/include/core/StrategyRouter.h](../Quant/include/core/StrategyRouter.h) | 21 `for_each` |
| [Quant/include/core/StrategyShard.h](../Quant/include/core/StrategyShard.h) | 20 `step`, 22 `Emitted` |
| [Quant/include/core/TickCapture.h](../Quant/include/core/TickCapture.h) | 18 `on_trade` |
| [Quant/include/core/WakeGate.h](../Quant/include/core/WakeGate.h) | 59 `WakeGate` |
| [Quant/include/strategy/DeviationScaleStrategy.h](../Quant/include/strategy/DeviationScaleStrategy.h) | 24 `on_start`, 25 `on_trade_batch`, 28 `emit_liquidation` |
| [Quant/include/strategy/StrategyBase.h](../Quant/include/strategy/StrategyBase.h) | 23 `on_trade_batch` |
| [Quant/src/api/KisOrder.cpp](../Quant/src/api/KisOrder.cpp) | 43 `submit_order_ack` |
| [Quant/src/api/WebSocketClient.cpp](../Quant/src/api/WebSocketClient.cpp) | 8 `recv_loop`, 9 `parse_message`, 10 `dispatch_record`, 11 `parse_kr_trade`, 45 `parse_fill_notification` |
| [Quant/src/core/BarAggregator.cpp](../Quant/src/core/BarAggregator.cpp) | 26 `on_tick` |
| [Quant/src/core/DataPoller.cpp](../Quant/src/core/DataPoller.cpp) | 17 `poll_universe` |
| [Quant/src/core/Engine.cpp](../Quant/src/core/Engine.cpp) | 3 `add_strategy`, 4 `start`, 5 `start (WS 콜백 설치)`, 6 `start (스레드 기동)`, 16 `data_thread_fn`, 19 `shard_thread_fn`, 29 `strategy_thread_fn`, 30 `poll_regime_file`, 34 `drain_manual_inbox`, 35 `order_thread_fn`, 47 `fill_thread_fn`, 50 `control_thread_fn`, 51 `apply_regime_selection`, 52 `maybe_rescan_universe`, 54 `activate_rest_fallback`, 55 `request_shutdown`, 56 `stop` |
| [Quant/src/core/LedgerReconciler.cpp](../Quant/src/core/LedgerReconciler.cpp) | 7 `bootstrap`, 53 `reconcile` |
| [Quant/src/core/OrderPacer.cpp](../Quant/src/core/OrderPacer.cpp) | 36 `wait_before_send`, 37 `on_rejected` |
| [Quant/src/core/SignalDispatcher.cpp](../Quant/src/core/SignalDispatcher.cpp) | 31 `from_strategy`, 32 `submit`, 33 `force_liquidate` |
| [Quant/src/ipc/OrderRouter.cpp](../Quant/src/ipc/OrderRouter.cpp) | 38 `submit`, 48 `on_fill` |
| [Quant/src/main.cpp](../Quant/src/main.cpp) | 1 `main` |
| [Quant/src/risk/OrderGate.cpp](../Quant/src/risk/OrderGate.cpp) | 39 `check`, 40 `clamp_buy_qty`, 41 `plan_displacement`, 42 `on_accept`, 49 `on_fill_confirmed` |
| [Quant/src/strategy/StrategyFactory.cpp](../Quant/src/strategy/StrategyFactory.cpp) | 2 `load_strategies` |

## 이 문서를 고치는 법

- 걸음을 더하거나 순서를 바꾼다: `docs/code_flow.toml`의 `[[stage.step]]`. 심볼과 파일만 적으면 줄은 생성기가 찾는다.
- 함수 이름이 바뀌어 `[missing]`이 나면: 명세의 `sym`을 새 이름으로. 자리가 없어졌으면 걸음을 지운다.
- 이름 없는 자리(람다·블록)는 `match` 정규식으로 가리킨다.
- 그림(`diagram`)과 요약(`summary`)은 사람이 쓴다 — 스레드·큐가 바뀌면 같은 커밋에서 고친다.

