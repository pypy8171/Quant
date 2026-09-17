# 엔진 아키텍처 요약

`CLAUDE.md`에서 옮겨 온 스레드 모델·핵심 타입·국면·KIS·WebSocket·로깅 요약이다(2026-09-14, 매 호출의 문맥 바닥을 줄이려고).
헤더의 공개 역할이 바뀌면 이 문단을 고치고 `py scripts/sync_impact.py --restamp docs/ENGINE_ARCHITECTURE.md`로 도장을 갱신한다
(규칙은 `docs/sync_map.toml`). 읽는 순서로 따라가는 코드 흐름은 [CODE_FLOW.md](CODE_FLOW.md), 결정 이력은 [DECISIONS.md](DECISIONS.md).

## 아키텍처

### 스레드 모델

<!-- sync: Quant/include/core/Engine.h@5345885 Quant/src/core/Engine.cpp@dd37ff7 Quant/include/core/DataPoller.h@01ca109 Quant/include/core/SignalDispatcher.h@1603d88 Quant/include/core/OrderPacer.h@9a50ea6 Quant/include/core/LedgerReconciler.h@34df50d Quant/include/core/WakeGate.h@aa7b2a2 Quant/include/core/BarAggregator.h@67b2ee8 Quant/include/core/LatencyTrace.h@49b1f85 Quant/include/core/ReconcilePlan.h@66ee435 -->
엔진은 락-프리 파이프라인(데이터→전략 샤드→디스패치→주문)에 체결 소비 스레드와 제어 스레드를 더해 다섯 개 + 샤드 M개의 스레드를 실행합니다(config `strategy_shards`, 기본 1):

```
[데이터 스레드]  →  pipeline_.bars_mx·pipeline_.td_mx (링 행렬 행)  →  [샤드 스레드 m]  →  pipeline_.shard_out (MpscQueue)  →  [전략(디스패치) 스레드]  →  pipeline_.order_queue  →  [주문 스레드]
  KIS REST                                           열 m의 전략들                                   SignalDispatcher                              KIS 주문 API
  OHLCV 봉·대체 틱   [WS 수신] → pipeline_.ob_mx·pipeline_.td_mx  → Emitted 봉투                     → OrderSignal                                 send_order()
```

- `RingBuffer<T>`는 명시적 메모리 순서를 가진 `std::atomic`을 사용하는 SPSC(단일 생산자/단일 소비자) 락-프리 큐입니다.
- 데이터 스레드는 `fetch_interval_sec`초마다 KIS REST를 폴링하며, 장 외 시간에는 건너뜁니다. REST 현재가 폴링(폴링 모드 유니버스·WS 구독 상한 넘침 대체·틱 끊긴 보유 보충)은 `Quant/include/core/DataPoller.h`의 `DataPoller`가 맡고, 폴러의 틱은 체결 행렬 `pipeline_.td_mx`의 데이터 스레드 행으로 갑니다(WS 콜백 행과 생산자를 나눈다, D-062, `test_data_poller`). KST 시각 변환은 `Quant/include/core/KstTime.h`. 유니버스 재스캔(`rescan_interval_sec`)도 이 스레드가 돌린다 — 소유 종목이 스캔에서 연속으로 빠지면 `rescan_block_after_sec`에 신규매수를 막고 `rescan_drop_after_sec`에 전략을 떼며, 판정은 `Quant/include/core/UniverseExit.h`의 순수 함수다(D-077, `test_signal_dispatcher`). 틱·호가의 시각은 정수 HHMMSS(`hhmmss`, 093001 → 93001)다 — 디코더가 `krx::parse_hhmmss`(`Quant/include/core/MarketSession.h`)로 한 번 읽고 폴러는 `kst::hhmmss_int`로 만들며, 문자열은 화면·캡처 파일에서만 `krx::hhmmss_str`로 되돌린다(D-071). 수신 시각 `recv_ns`(steady_clock ns)는 호가·체결 모두 소켓 읽기 스레드가 디코드 직후 찍는다 — 채널이 달라도 같은 시계라 도착 순서를 되돌릴 수 있고, 구간 지연 CSV의 출발점이다(REST 대체 틱은 0, 리플레이는 내보낼 때 다시 찍는다, D-071).
- 틱은 수신 N×전략 샤드 M SPSC 링 행렬(`Quant/include/core/ShardMatrix.h`의 `shard::Matrix`, 종목 해시로 열을 고른다, 행은 WS 소켓(레인)마다 하나에 데이터 스레드 행을 더한 것 — `pipeline_.ws_lanes`·`pipeline_.data_row`, `test_shard_matrix`)을 지나 샤드 스레드(`shard_thread_fn`, `Quant/include/core/StrategyShard.h`의 `strat::Shard`, `test_strategy_shard`)가 자기 열을 비웁니다. 샤드는 틱의 종목 id로 그 종목을 보는 전략만 방문하며(`Quant/include/core/StrategyRouter.h`의 `strat::Router`, 구독 종목을 안 밝힌 전략은 전부 받는다, D-071, `test_strategy_router`), `NONE`이 아닌 신호를 봉투 `strat::Emitted`로 `pipeline_.shard_out`(MpscQueue)에 넣고 전략(디스패치) 스레드가 이를 주문 큐로 넘깁니다. 샤드 수는 config `strategy_shards`(기본 1)이고 전략은 자기 구독 종목의 열 하나가 소유합니다(`strat::owner_shard` — 종목이 여러 열에 걸치는 전략이 있으면 `start()`가 경고하고 1로 돌립니다). 전략은 자기 종목·후보를 `on_start`에서 `symbol_of()`(Engine이 `set_symbol_resolver`로 `SymbolTable::intern`을 넣는다)로 id로 받아 두고 틱에서는 `trade.symbol_id`와 정수로만 비교합니다(`same_symbol`, 원칙 6) — 집계기도 id 키입니다. 신호가 큐에 가기 전의 판단 — 순번 stamp, 비활성 전략·청산 관리 보유 종목의 신규 차단, 슬롯이 찬 상태의 교체 진입(최약체 매도 뒤 매수 보류), 강제청산 재발주 스로틀, 기동 뒤 한도 초과분 정리 — 는 `Quant/include/core/SignalDispatcher.h`의 `SignalDispatcher`가 맡습니다(전략 스레드의 지역 객체, D-063, `test_signal_dispatcher`). DeviationScale의 3분봉은 config `bar_source`로 고른다 — `"ws"`(기본)는 전략 스레드가 체결 틱을 `Quant/include/core/BarAggregator.h`의 `bars::BarAggregator`로 1분봉에 모으고 판단 직전 `bars::resample`로 `interval_min` 봉을 만든다(REST 1분봉은 시드·폴백, REST 대체 틱이 오면 REST 봉으로 되돌아간다, 판단은 언제나 interval_min 봉, 틱이 없어도 판단 직전 `close_stale`이 지난 분 봉을 시계로 닫는다, D-068·D-069·D-072·D-074, `test_bar_aggregator`), `"rest"`는 REST 3분봉을 그대로 쓴다.
- 체결 소비 스레드(`fill_thread_fn`, D-056)는 WS 수신 스레드가 `pipeline_.fill_queue`(SPSC)에 넣은 체결통보를 받아 `OrderRouter::on_fill`(원장 반영·CSV)과 운영단말 방송을 합니다. 수신 스레드는 push만 하므로 체결 처리 동안 틱이 서지 않습니다. 큐가 비면 condvar에서 자고 생산자가 깨웁니다(Logger와 같은 방식). 이 깨우기는 `Quant/include/core/WakeGate.h`의 `sync::WakeGate` 한 조각이고 전략·주문 스레드의 유휴도 같은 조각을 씁니다 — 전략은 200us yield 뒤 잠들고, 주문은 재시도 만기까지 `wait_until`합니다(D-071, `test_wake_gate`). `Quant/src/main.cpp`가 `timeBeginPeriod(1)`을 잡아 sleep 격자를 15.6ms에서 2ms로 내리며, `RingBuffer::high_water()`를 제어 스레드가 1분마다 `[큐 고수위]`로 남깁니다. 신호 하나의 구간 지연(틱 수신→신호→pop→라우터 반환)은 주문 스레드가 `Quant/include/core/LatencyTrace.h`로 `logs/latency_trace.csv`에 한 줄씩 남깁니다(`test_latency_trace`).
- 주문 스레드는 큐에서 꺼낸 신호를 `OrderRouter`에 넘깁니다. 직전 KIS 호출 뒤 최소 간격 대기, 거부의 재시도 분류(유량 한도는 action 불문, 청산 SELL은 40240000 제외, BUY 제외), 재시도 버퍼의 만기·청산 완료 폐기는 `Quant/include/core/OrderPacer.h`의 `OrderPacer`가 맡습니다(주문 스레드의 지역 객체, D-065, `test_order_pacer`). 게이트가 만들고 조절기가 읽는 유량 한도 거부 문장은 `Quant/include/risk/GateReasons.h` 한 곳이 정의합니다(D-067).
- 제어 스레드(`control_thread_fn`)는 파이프라인 밖에서 잔고 대조·손익(daily_pnl) 갱신 상태 감시 등 주기 운영 작업을 담당합니다(갱신이 끊기면 OrderGate 보수정지 토글). KIS 토큰도 이 스레드가 5분마다 만료 30분 전에 미리 갱신합니다 — 발급 HTTP 왕복이 파이프라인 스레드에 걸리지 않게 하고, 전략 스레드는 `pipeline_.order_queue`가 차면 기다리지 않고 신호를 버리고 셉니다(`pipeline_.order_dropped`, D-073). WS 시세가 끊겼을 때의 판정 — 장 외 무시, 재연결 백오프(실패 n회째 30×n초, 상한 300초), 연속 3회 실패에 한 번 REST 폴백 요구 — 는 `Quant/include/core/FeedSupervisor.h`의 `feed::Supervisor`가 맡고, 제어 스레드는 소켓 재연결(`IFeedSource::reconnect_stale` — 소켓이 여럿이면 멈춘 것만 자기 종목으로 다시 잇는다)과 폴백 적용(불가면 kill switch)만 합니다(D-071, `test_feed_supervisor`).
- 잔고 → 원장 대조(기동 시드·주기 대조·당일 손익 기준선 파일·잔고조회 서킷브레이커)는 `Quant/include/core/LedgerReconciler.h`의 `LedgerReconciler`가 맡습니다. 브로커 호출·대조 행 기록·종목명 등록을 `std::function`으로 받아 `Engine`은 배선만 하고, 테스트는 KIS 없이 `OrderGate`만 링크합니다(D-061, `test_ledger_reconciler`). 체결 직후 5초는 대조를 미루고 30초마다 한 번은 돕니다(`note_fill`, D-074).
- 대사 단계 귀속(D-038): 전략 스레드가 신호마다 `OrderSignal.seq`를 단조 stamp하고 라우터가 원장 CSV 전 행에 같은 번호를 남깁니다. 잔고 대조는 덮어쓰기·정리 전 원장 값으로 `core/ReconcilePlan.h`(순수 함수)가 어긋난 종목만 골라 `RECONCILE` 행(`OVERWRITE|PRUNE|KEEP`)을 씁니다.

### 핵심 타입 (`Quant/include/core/Types.h`)

<!-- sync: Quant/include/core/Types.h@b8ee715 -->
`MarketData`(OHLCV + bar_index), `OrderSignal`(side/type/qty/price/**ref_price** + strategy_id, 종목 id `symbol_id`는 전략 스레드가 큐에 넣기 전에 찍는다), `Position`, `OrderBook`(5단계 호가, 채널 `H0STASP0`/선물 `H0IFASP0`), `TradeData`(실시간 체결, 채널 `H0STCNT0`/선물 `H0IFCNT0`; 호가·체결 모두 종목 id `symbol_id`와 정수 시각 `hhmmss`를 들고, 봉·호가·체결의 `ticker`는 `symbol::Ticker` 15자 고정 배열이라 세 구조체는 trivially copyable이다 — 문자열은 `.str()`, D-071), `WatchSpec`(FEED 구독 종목 명세 — `is_future` 플래그로 현·선 채널 선택), `Regime`(enum: BULL/NEUTRAL/BEAR/UNKNOWN), `RegimeSnapshot`(장 시작 국면 판정 결과 — score·200MA·정배열/역배열·지수 이평 분해).

> `OrderSignal.ref_price`는 시장가(price=0) 주문의 명목 한도 평가 기준가다. 지정가는 `price`로 명목을 재지만 시장가는 `price`가 0이라 이 값이 없으면 명목 백스톱이 우회된다(특히 급락장 강제청산의 시장가 전량매도). 발주 측이 직전 현재가/평단을 stamp한다.

### 국면(Regime) 대응

<!-- sync: Quant/include/core/RegimeFileBridge.h@763c37f -->
전략 집합 선택·매수 비율·신규매수 정지·강제청산은 전부 한 입력, 매크로 보조 프로세스(`PYQuant/tools/macro_regime_feed.py`)가 3분마다 쓰는 `regime.json`(config `regime_file`·`regime_stale_sec`)에서 나온다(D-084). 코스피·코스닥·나스닥·S&P 선물·10년물·VIX·환율·WTI 8개 투표가 점수가 되고, 파일의 `regime` 라벨(RISK_ON/NEUTRAL/RISK_OFF, 판정 보류면 UNKNOWN)·`entry_scale`(매수 명목 비율 0~1, D-083)·`entry_halt`(비율 0과 같은 뜻)·`force_liquidate`가 각각 다음으로 간다 — 라벨은 RISK_ON→BULL·NEUTRAL·RISK_OFF→BEAR로 옮겨 **라벨이 바뀐 회차에만** `Engine::apply_regime_selection`이 config `"regime_strategies": {"RISK_ON":[id…],"NEUTRAL":[…],"RISK_OFF":[…]}`(종전 `BULL`·`BEAR` 키도 같은 뜻, `'*'` 접두 매칭) 기준으로 전략의 `active_`를 켜고 끈다(맵이 없으면 전략별 `active_regimes`로 하위호환, 맵이 등록 전략과 하나도 안 맞으면 WARN). `entry_scale`은 `OrderGate::set_entry_scale`로 넘어가 `DeviationScaleStrategy`가 베이스·물타기 명목에 곱하고, `entry_halt`는 `OrderGate::set_entry_halt`(신규매수만 차단, 청산은 통과)를 토글하고, `force_liquidate`는 여기에 더해 strategy_thread가 보유 전량에 대해 `FORCE_LIQ` 시장가 매도를 2초 간격으로 재발주하게 한다. 파일의 갱신 지연(stale)·무효(valid=false)·모르는 라벨은 이전 값을 유지하며, 개장 후 만료(시간 상자, 기본 0=끔)·전이·1회 로그 판정은 `Quant/include/core/RegimeFileBridge.h`의 상태기계가 맡고 `Engine::poll_regime_file`은 파일 읽기와 적용만 한다(D-060, `test_regime_bridge`). 드릴 절차는 [docs/guides/REGIME_DRILL_GUIDE.md](guides/REGIME_DRILL_GUIDE.md).

코스피 하나의 200일선·정배열로 BULL/NEUTRAL/BEAR를 따로 내던 `RegimeController`는 전략 on/off 말고 하는 일이 없어 09-14에 지웠다(D-085). `[RegimeSelect] 국면=RISK_ON …` 줄의 국면도 `regime.json` 라벨이다.

### 전략 추가하기

1. `StrategyBase`(`Quant/include/strategy/StrategyBase.h`)를 상속합니다.
2. `id()`, `on_data(const MarketData&)`, `describe()`를 구현합니다. 종목 비교는 문자열이 아니라 `on_start`에서 `symbol_of(ticker)`로 받은 id와 `trade.symbol_id`로 합니다(`same_symbol` 헬퍼). 신호에는 `signal.symbol_id`를 찍습니다.
3. `main.cpp`에서 `engine.add_strategy(std::make_unique<YourStrategy>(...))` 로 등록합니다.
4. 필요하면 `"strategies"` 아래에 설정 항목을 추가하고 `main.cpp`의 전략 로딩 블록에서 파싱합니다.

### KIS API 클라이언트 (`Quant/include/api/KisClient.h`, 구현은 `Quant/src/api/Kis*.cpp` 7파일)

<!-- sync: Quant/include/api/KisClient.h@d022527 Quant/include/api/KisResult.h@654719e Quant/include/api/KisTypes.h@e627dfe Quant/include/api/KisRestDecode.h@2042c76 Quant/include/api/IOrderExecutor.h@fc84061 Quant/include/api/IMarketDataSource.h@8c8d745 -->
클래스는 하나고 구현이 도메인별로 나뉩니다(D-048): `KisTransport.cpp`(플랫폼별 HTTP — Windows는 WinHTTP, Linux는 libcurl — 재시도·초당 한도·공용 인증 헤더 `auth_headers()`), `KisAuth.cpp`(OAuth2 토큰 발급·캐시), `KisMarket.cpp`(주식 시세 — 분봉 페이지 병합·집계는 순수 함수 헤더 `Quant/include/api/KisRestDecode.h`, D-051), `KisIndex.cpp`(지수·수급·선물), `KisOrder.cpp`(주문), `KisAccount.cpp`(잔고·미체결), `KisUniverse.cpp`(순위·유니버스). 구현끼리만 쓰는 include·상수는 `Quant/src/api/KisClientInternal.h`. 주요 메서드: `authenticate()`, `get_ohlcv()`, `get_current_price()`, `send_order()`, 국내 선물 시세 `get_future_price()`(단일 시세)·`get_future_board()`(전광판, 그릭스 포함). 새 REST 호출은 인증 헤더 네 줄을 손으로 쓰지 말고 `auth_headers(tr_id, {추가 항목})`을 씁니다. 공개 헤더는 `nlohmann::json`을 내보내지 않습니다 — 잔고 `get_balance()`·전광판 `get_future_board()`는 `KisResult<T>`(`Quant/include/api/KisResult.h`, 실패 코드 동반) 봉투에 값 타입(`Quant/include/api/KisTypes.h`)을 담아 돌려주고, 응답 필드 해석은 `Quant/include/api/KisRestDecode.h`의 순수 함수가 맡습니다(D-059). 인터페이스는 둘을 구현합니다 — 주문 `IOrderExecutor`(`Quant/include/api/IOrderExecutor.h`, D-039)와 읽기 전용 시세·봉 `IMarketDataSource`(`Quant/include/api/IMarketDataSource.h`, D-066 — 현재가·일봉·분봉·지수 일봉·지수 현재값·해외 일봉). 순위·수급·잔고는 인터페이스 밖입니다.

### WebSocket 클라이언트 (`Quant/include/api/KisWebSocket.h`, 구현은 `Quant/src/api/WebSocketClient.cpp` + `WsSocketWin.cpp`/`WsSocketPosix.cpp`)

<!-- sync: Quant/include/api/KisWebSocket.h@8d792d9 Quant/src/api/WebSocketClient.cpp@4fe0972 Quant/src/api/WsSocket.h@48372a1 -->
FEED 모드에서 사용합니다. REST로 approval key를 발급받고, `ops.koreainvestment.com:31000`(모의) 또는 `:21000`(실거래)에 연결한 뒤 구독한 채널의 파싱된 구조체를 등록된 콜백으로 전달합니다. 구독 채널은 종목당 `WatchSpec`으로 정하며, 국내 현물 호가 `H0STASP0`·체결 `H0STCNT0`, 국내 선물 호가 `H0IFASP0`·체결 `H0IFCNT0`(`WatchSpec.is_future=true`로 선택), 미국 체결 `HDFSCNT0`을 지원합니다. 선물 체결에는 매수/매도 방향 코드가 없어 `direction`을 0으로 둡니다. 최초 연결·재연결 경로에 흩어져 있던 구독 하드코딩은 `subscribe_all()` 한 곳으로 통합되어, 재연결 시 선물 채널이 누락되던 불일치를 없앴습니다. 국내 선물 실시간은 실계좌 WS 도메인 전용이라 모의(`is_paper=true`)에서는 지원되지 않습니다. 소켓 계층은 `Quant/src/api/WsSocket.h`의 `WsSocket` 인터페이스 뒤에 있고(D-049) 플랫폼당 한 파일만 링크되므로, 연결·재연결·백오프·구독은 `WebSocketClient.cpp`에 플랫폼 코드 없이 한 벌입니다. 공개 헤더는 `<windows.h>`를 끌어오지 않습니다. 엔진은 소켓을 `feed::IFeedSource`(`Quant/include/core/IFeedSource.h`)로만 보며 — `Engine::set_feed_source`로 소스를 직접 주거나 config `replay_file`로 캡처 파일을 틀면 KIS 없이 기동해(인증·계좌·유니버스 스캔 없음, 종목은 config `tickers`) 주문·잔고를 모의 체결기가 받는다(`test_engine`이 시험용 시세로 레인 1×샤드 1과 2×2를, 캡처 파일로 리플레이를 한 바퀴씩 돈다, D-071) — config `feed_keys`로 세션 키를 더 주면 `Quant/include/core/FeedMux.h`의 `feed::FeedMux`가 소켓 여럿을 한 소스로 묶어(종목은 한 소켓에만, 체결통보는 첫 소켓만) 구독 상한이 소켓 수만큼 늡니다. 소켓 하나가 멈추면 그 소켓만 자기 배정 종목으로 다시 잇고 나머지 소켓의 틱은 그 사이에도 흐릅니다(`reconnect_stale`, D-071, `test_feed_mux`). 엔진은 레인 모드로 받는다 — 소켓 i의 수신 스레드가 레인 i를 달고 콜백을 직접 불러 행렬의 행 i에 넣고(`IFeedSource::lanes()`·`set_lane_callbacks`, mux 스레드 없음), 틱 캡처 큐는 그래서 `MpscQueue`다(D-071, `test_feed_mux`).

### 로깅

<!-- sync: Quant/include/utils/Logger.h@3fa097b -->
싱글톤 `Logger`가 밀리초 단위 UTC 타임스탬프로 콘솔과 `logs/quant_trader.log`(cwd 하위 `logs/` 폴더에 고정, 부모 폴더는 자동 생성)에 기록합니다. 과거 로그는 `logs/archive/`에 보관합니다. 사용 매크로: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`. 기본 임계값은 INFO이고 config `"log_level": "DEBUG"`가 봉 닫힘(D-069)·KIS 응답 본문 같은 DEBUG 줄을 연다 — 비교표를 뽑는 날만 켠다(`PYQuant/tools/compare_ws_bars.py`).

**비동기 구조**: 전략·주문 hot path는 레코드를 큐에 push만 하고 즉시 반환하며, 타임스탬프 포맷팅과 파일/콘솔 I/O는 전용 writer 스레드가 담당합니다(저지연은 평균 지연보다 최악 지연(tail latency)이 중요하다는 설계 의도로 디스크 플러시를 hot path에서 분리). 큐는 락 없는 `MpscQueue<Record>`(65,536슬롯)이고 writer는 큐가 비면 condvar에서 자며 생산자는 writer가 "잔다"고 표시한 때만 깨웁니다(D-045). 밀림 처리: 큐가 가득 차면 새 레코드를 드롭하고 `dropped()`로 셉니다(hot path 블로킹 방지). 종료·테스트 직전 정합 확인용 `flush()`를 제공합니다.
