# PROJECT_OVERVIEW — Quant 저장소 한 장 안내

이 문서 한 장으로 저장소 전체를 따라갈 수 있게 썼다. 전체 파이프라인, 주문 한 건의 여정, C++ 엔진의 스레드·큐·공유메모리 구조,
쓰인 문법과 기법, 그리고 그 설명이 가리키는 코드 줄을 링크로 붙였다. 링크는 `파일#L줄` 형식이라 VS Code·GitHub에서 바로 열린다.
줄 번호는 2026-10-21 소스 기준이다. 코드가 움직이면 줄이 어긋날 수 있으니, 그때는 링크 옆에 적은 함수 이름으로 찾는다.

읽는 순서는 위에서 아래다. 처음이면 1·2·4절만 읽어도 전체 그림이 잡힌다. 더 깊이 들어갈 때 6절(아키텍처)·8절(문법·기법)을 본다.
이 문서는 요약이고, 각 절 끝에 정본 문서를 링크했다. 수치·설정값은 정본과 코드가 우선이다.

---

## 1. 한 장 요약

**무엇을 하는가.** 한국투자증권(KIS) REST·WebSocket으로 시세를 받아 전략이 신호를 만들고, 리스크 게이트를 지나 주문을 내고,
체결 통보로 장부를 맞추는 C++ 자동매매 엔진이다. 실행 파일은 `quant_trader` 하나, 실행 모드는 `TRADE` 하나다(D-130).
평일 09:00–15:30과 애프터마켓 16:00–20:00 KST에 실제 주문을 낸다(D-097). 모의투자는 config의 `"is_paper": true`로 고른다.

**설계 목표.** 지금은 KIS 41종목·계좌 하나지만, 목표는 전 시장 실시간 피드(2,500종목 이상)와 고객 수십만 계좌를 대신
매매하는 구조다(D-071·D-128). 그래서 소켓 하나는 스레드 하나가 맡고, 시세는 종목 id로·원장은 계좌 id로 샤딩하며,
뜨거운 경로(hot path)에 문자열을 두지 않고, 주문 게이트는 하나만 둔다. 원칙 9개는 [CLAUDE.md](../CLAUDE.md) "설계 목표와 원칙"에 있다.

**한 줄 흐름.**

```
KIS WebSocket ──▶ 수신 스레드(얇게) ──▶ FeedMux ──▶ 샤드 행렬(종목 해시) ──▶ 전략 on_trade_batch
                                                                             │ OrderSignal
체결 통보 ◀── KIS ◀── 전송 스레드 ◀── 주문 스레드(게이트·원장 선기록) ◀── SignalDispatcher
   │
   └─▶ 체결 스레드 ──▶ OrderRouter::on_fill ──▶ PositionLedger ──▶ 장부 스냅숏(공유메모리) ──▶ 게이트가 다시 읽음
```

**프로세스.** 기본은 한 프로세스가 모든 역할을 한다(`--role both`). `--role feed|strategy|order`로 셋으로 나누면 같은 배치를
공유메모리에 깔고 각자 자기 면만 쓴다(D-114, 이유는 속도가 아니라 고장 격리).

---

## 2. 저장소 지도

| 경로 | 무엇이 있나 | 처음 볼 파일 |
|---|---|---|
| [Quant/src/](../Quant/src/) · [Quant/include/](../Quant/include/) | C++ 엔진 본체. `api`(KIS)·`core`(엔진·큐·스레드)·`ipc`(공유메모리·주문 라우터·DB·ZMQ)·`risk`(게이트·원장)·`strategy`·`universe`·`regime`·`exchange`(부하시험용 모의 거래소)·`utils` | [main.cpp](../Quant/src/main.cpp#L323) |
| [Quant/tests/](../Quant/tests/) | 단위 테스트 96개 파일(`test_*`)과 벤치 33개 파일(`bench_*`) | [test_engine_order_path.cpp](../Quant/tests/test_engine_order_path.cpp) |
| [Quant/config/](../Quant/config/) | 실행 설정. `config.json`(실계좌, 인증정보 포함)·`config_live.json`·`config_dev_paper.json`(모의), 유니버스·국면·종목명 캐시 | `config_dev_paper.json` |
| [Quant/tools/ops_terminal/](../Quant/tools/ops_terminal/) | MFC 운영단말(TCP로 엔진 `OpsServer`에 붙는다) | [docs/guides/OPS_TERMINAL.md](guides/OPS_TERMINAL.md) |
| [PYQuant/](../PYQuant/) | 파이썬. 백테스트 엔진·데이터 어댑터·KIS 클라이언트·엔진 모니터·대시보드 생성·적재 도구 | [PYQuant/main.py](../PYQuant/main.py#L612) |
| [scripts/](../scripts/) | 매매 운영 스크립트. 감시견·배포·마감 문서·건강 판정·로그 파서 | [auto_trade_day.ps1](../scripts/auto_trade_day.ps1#L556) |
| [research/](../research/) | 리서치 프로세스. `studies/01…45` 스터디, 백테스트 원장·흐름·회의록, 대시보드 | [research/README.md](../research/README.md) |
| [strategies/](../strategies/) | 전략 스펙·실증·매매일지(`DeviationScale`·`ITB`) | [strategies/README.md](../strategies/README.md) |
| [docs/](./) | 설계·운영 문서. 결정 원장 `DECISIONS.md`, 아키텍처, 코드 흐름(생성물), 파일 색인, 가이드 | [docs/FILE_INDEX.md](FILE_INDEX.md) |
| `../quant-devtools/` (저장소 밖) | 게이트·생성기·워크트리 도구(D-107) | — |
| `_private/` (gitignore) | 인계·세션 점유·링크 허브·취업 자료 | — |

파일 하나하나는 [docs/FILE_INDEX.md](FILE_INDEX.md)에 한 줄씩 있다. 모듈 의존 그래프는 [docs/CODE_GRAPH.md](CODE_GRAPH.md).

---

## 3. 빌드·실행·테스트

```bash
cmake --preset x64-release && cmake --build out/build/x64-release     # Windows(VS2022/Ninja). x64-debug도 같다
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant && cmake --build Quant/build   # Linux(libcurl4-openssl-dev 필요)
py ../quant-devtools/check_build_ready.py && ctest --preset x64-release   # 낡은 exe 시험 방지 → 단위 테스트
./Quant/build_win/quant_trader Quant/config/config.json               # 반드시 repo 루트에서
```

- 표준은 C++23([CMakeLists.txt#L4](../Quant/CMakeLists.txt#L4)). 엔진은 정적 라이브러리 `quant_engine`([#L303](../Quant/CMakeLists.txt#L303))로 묶고
  실행 파일 `quant_trader`([#L347](../Quant/CMakeLists.txt#L347))가 그것을 링크한다. 테스트·벤치도 같은 라이브러리를 쓴다.
- 의존: nlohmann_json([#L56](../Quant/CMakeLists.txt#L56), 없으면 FetchContent로 받음), ZeroMQ/cppzmq([#L67](../Quant/CMakeLists.txt#L67)),
  PostgreSQL libpq([#L102](../Quant/CMakeLists.txt#L102)), Linux는 CURL·OpenSSL, Windows는 WinHTTP와 미니덤프용 dbghelp([#L353](../Quant/CMakeLists.txt#L353)).
- 플랫폼 분기: HTTP는 `#ifdef _WIN32`([KisTransport.cpp#L127](../Quant/src/api/KisTransport.cpp#L127)), WebSocket 소켓은 파일 단위
  [WsSocketWin.cpp](../Quant/src/api/WsSocketWin.cpp)·[WsSocketPosix.cpp](../Quant/src/api/WsSocketPosix.cpp)로 나눈다. 네트워크 기능을 더할 때 이 패턴을 지킨다.
- 트레이더 프로세스는 손으로 띄우지 않는다. 감시견 [scripts/auto_trade_day.ps1](../scripts/auto_trade_day.ps1#L556)(모의)·[scripts/auto_trade_live.ps1](../scripts/auto_trade_live.ps1)(실계좌)이 소유한다. 장중 exe 교체는 [scripts/deploy_trader.py](../scripts/deploy_trader.py#L278).

정본: [docs/guides/PROJECT_GUIDE.md](guides/PROJECT_GUIDE.md)(빌드 플래그·테스트 타깃·TSAN), [docs/RUNBOOK.md](RUNBOOK.md)(운영 명령 복붙용).

---

## 4. 전체 파이프라인 — 기동부터 종료까지

각 단계의 첫 줄 링크부터 따라가면 코드 순서 그대로다. 더 잘게 나눈 108걸음은 [docs/CODE_FLOW.md](CODE_FLOW.md)(생성물).

### 4.1 기동

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | 진입. 명령줄을 읽고 `run_trade`로 넘긴다 | [main.cpp#L323 `main`](../Quant/src/main.cpp#L323) → [#L259 `run_trade`](../Quant/src/main.cpp#L259) |
| 2 | 명령줄 파싱(`--role`, `--replay` 등) | [CommandLine.cpp#L88](../Quant/src/core/CommandLine.cpp#L88) |
| 3 | config.json 파싱. `mode`가 TRADE 밖이면 멈춘다 | [AppConfig.cpp#L204 `parse_config`](../Quant/src/core/AppConfig.cpp#L204) |
| 4 | 엔진 설정 주입(리스크·샤드 수·전송 스레드 수·DB·캡처) | [EngineConfigure.cpp#L200 `Engine::configure`](../Quant/src/core/EngineConfigure.cpp#L200) |
| 5 | 배치(layout) 깔기. 역할이 `both`면 힙에, 분리 프로세스면 공유메모리에 | [EngineLayout.cpp#L32 `bind_layout`](../Quant/src/core/EngineLayout.cpp#L32) → [#L138 힙](../Quant/src/core/EngineLayout.cpp#L138) · [#L163 공유메모리](../Quant/src/core/EngineLayout.cpp#L163) |
| 6 | 공유메모리 열기·헤더 쓰기(이름 `quant.engine.{paper\|live}.{계좌}`) | [SharedRegion.cpp#L187 `create`](../Quant/src/ipc/SharedRegion.cpp#L187) · [#L88 OS 매핑](../Quant/src/ipc/SharedRegion.cpp#L88) |
| 7 | 면(plane)을 순서대로 붙인다: 요청→응답→제어→시세제어→체결→박동→국면→시세채널→종목사전→전략사전→원장스냅숏 | [SharedLayout.cpp#L185 `bind`](../Quant/src/ipc/SharedLayout.cpp#L185) |
| 8 | 전략 적재. 타입별 로더가 `engine.add_strategy`를 부른다 | [StrategyFactory.cpp#L578 `load_strategies`](../Quant/src/strategy/StrategyFactory.cpp#L578) · [DevScaleLoader.cpp#L793](../Quant/src/strategy/DevScaleLoader.cpp#L793) |
| 9 | 장부 재시드. 계좌 잔고·미체결을 KIS에서 받아 원장 저널과 대조 | [LedgerReconciler.cpp#L21 `bootstrap`](../Quant/src/core/LedgerReconciler.cpp#L21) · [#L319 `reconcile`](../Quant/src/core/LedgerReconciler.cpp#L319) |
| 10 | 스레드 전부 띄움 | [Engine.cpp#L320 `start`](../Quant/src/core/Engine.cpp#L320) → [#L247 `spawn_threads`](../Quant/src/core/Engine.cpp#L247) |

### 4.2 유니버스 — 오늘 볼 종목 고르기

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | 시장판(MarketBoard) 스레드. 네이버 종목 목록을 받고 5초마다 시세, 60초마다 재랭킹 | [MarketBoard.cpp#L337 `run`](../Quant/src/universe/MarketBoard.cpp#L337) · [#L551 `rerank`](../Quant/src/universe/MarketBoard.cpp#L551) |
| 2 | DevScale 스캔: 지수 게이트 → 후보 → 일봉 필터(이동평균 정렬) → z점수 → 자르기 | [UniverseScanner.cpp#L33 `scan_devscale`](../Quant/src/universe/UniverseScanner.cpp#L33) · [UniverseScoring.cpp#L22](../Quant/src/universe/UniverseScoring.cpp#L22) |
| 3 | 종목을 정수 id로 등록(기동 뒤 뜨거운 경로에는 문자열이 없다) | [EngineSymbols.cpp#L173 `register_symbol`](../Quant/src/core/EngineSymbols.cpp#L173) · [SymbolTable.h#L134](../Quant/include/core/SymbolTable.h#L134) |
| 4 | 전략 등록·샤드 배정 | [EngineStrategyThread.cpp#L50 `add_strategy`](../Quant/src/core/EngineStrategyThread.cpp#L50) |
| 5 | 구독 칸 계획. 세션당 41칸을 보유 → 주문대기 → 점수 순으로 나눈다(D-132) | [WebSocketSlotPlan.h#L61 `plan`](../Quant/include/core/WebSocketSlotPlan.h#L61) · [EngineFeedSlots.cpp#L15](../Quant/src/core/EngineFeedSlots.cpp#L15) |
| 6 | 구독 요청을 수신 쪽에 보냄 | [EngineFeedWatch.cpp#L48 `collect_watch_specifications`](../Quant/src/core/EngineFeedWatch.cpp#L48) → [#L130 `send_watch_request`](../Quant/src/core/EngineFeedWatch.cpp#L130) |
| 7 | 장중 재스캔(보유·대기 종목은 지키고 빈 칸만 바꾼다) | [EngineUniverse.cpp#L80 `maybe_rescan_universe`](../Quant/src/core/EngineUniverse.cpp#L80) → [UniverseRescan.cpp#L172 `run_job`](../Quant/src/core/UniverseRescan.cpp#L172) |

### 4.3 시세 수신 — 소켓에서 샤드까지

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | WebSocket 연결·구독. 소켓 하나는 수신 스레드 하나(`WsRecv`) | [EngineFeed.cpp#L254 `connect_feed`](../Quant/src/core/EngineFeed.cpp#L254) · [WebSocketClient.cpp#L83 `recv_loop`](../Quant/src/api/WebSocketClient.cpp#L83) |
| 2 | 프레임 → 레코드 분기(체결·호가·체결통보) | [KisWebSocketParse.cpp#L86 `parse_message`](../Quant/src/api/KisWebSocketParse.cpp#L86) → [#L284 `dispatch_record`](../Quant/src/api/KisWebSocketParse.cpp#L284) |
| 3 | 순수 디코더(부수효과 없음, 테스트하기 쉽다). 체결 한 건을 `TradeData` 구조체로 | [KisWsDecode.h#L152 `decode_kr_trade`](../Quant/include/api/KisWsDecode.h#L152) · [KisWebSocketParse.cpp#L358](../Quant/src/api/KisWebSocketParse.cpp#L358) |
| 4 | 수신 스레드는 여기까지. 큐에 밀어 넣고 끝 | [EngineFeed.cpp#L43 `push_feed_trade`](../Quant/src/core/EngineFeed.cpp#L43) |
| 5 | FeedMux: 여러 소켓의 큐를 한 스레드가 모아 팬아웃 | [FeedMux.h#L36](../Quant/include/core/FeedMux.h#L36) · [EngineFeed.cpp#L512 `feed_lane_thread_fn`](../Quant/src/core/EngineFeed.cpp#L512) |
| 6 | 팬아웃: 종목 id 해시로 샤드 고르고, 캡처·DB·ZMQ에도 복사 | [EngineFeed.cpp#L108 `fan_out_trade`](../Quant/src/core/EngineFeed.cpp#L108) · [ShardMatrix.h#L81 `consumer_of`](../Quant/include/core/ShardMatrix.h#L81) · [#L87 `push_to`](../Quant/include/core/ShardMatrix.h#L87) |
| 7 | 분리 프로세스면 공유메모리 시세 채널로 건넨다 | [MarketFeedChannel.cpp#L229 `push_trade`](../Quant/src/ipc/MarketFeedChannel.cpp#L229) |
| 8 | 옆길: 틱 캡처(QTCAP v2, 리플레이 입력) · DB 적재(TimescaleDB COPY) · 데이터 스레드(REST 폴링 보조) | [TickCapture.h#L163](../Quant/include/core/TickCapture.h#L163) · [DbManager.cpp#L127](../Quant/src/ipc/DbManager.cpp#L127) → [DbWorkers.cpp#L344 `tick_loop`](../Quant/src/ipc/DbWorkers.cpp#L344) · [EngineDataThread.cpp#L44](../Quant/src/core/EngineDataThread.cpp#L44) · [DataPoller.cpp#L100](../Quant/src/core/DataPoller.cpp#L100) |

시세가 끊기면 제어 스레드가 REST 폴링으로 넘긴다: [EngineControlThread.cpp#L70 `activate_rest_fallback`](../Quant/src/core/EngineControlThread.cpp#L70).

### 4.4 전략 — 신호 계산

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | 샤드 스레드(`Shard N`). 자기 행의 SPSC 큐들을 비우고 전략을 묶음(batch)으로 호출 | [EngineStrategyThread.cpp#L563 `shard_thread_fn`](../Quant/src/core/EngineStrategyThread.cpp#L563) · [StrategyShard.h#L96 `step`](../Quant/include/core/StrategyShard.h#L96) |
| 2 | 전략 인터페이스. `on_start`에서 `symbol_of()`로 id를 받고 `trade.symbol_id`와 정수 비교 | [StrategyBase.h#L38](../Quant/include/strategy/StrategyBase.h#L38) · [#L96 `on_trade_batch`](../Quant/include/strategy/StrategyBase.h#L96) · [#L326 `symbol_of`](../Quant/include/strategy/StrategyBase.h#L326) |
| 3 | 라이브 전략 DeviationScale: 분봉 누적 → 이동평균 이탈 z → 진입·청산 | [DeviationScaleStrategy.cpp#L105 `on_start`](../Quant/src/strategy/DeviationScaleStrategy.cpp#L105) · [#L187 `on_trade_batch`](../Quant/src/strategy/DeviationScaleStrategy.cpp#L187) · [BarAggregator.cpp#L201 `on_tick`](../Quant/src/core/BarAggregator.cpp#L201) |
| 4 | 전략이 낸 `OrderSignal`을 샤드가 모아 `shard_out`(MPSC)으로 올린다 | [StrategyShard.h#L27 `Emitted`](../Quant/include/core/StrategyShard.h#L27) |
| 5 | 디스패처: 순번 도장, 비활성·청산관리 차단, 교체진입, 강제청산 | [SignalDispatcher.cpp#L182 `from_strategy`](../Quant/src/core/SignalDispatcher.cpp#L182) · [#L232 `submit`](../Quant/src/core/SignalDispatcher.cpp#L232) · [#L276 `force_liquidate`](../Quant/src/core/SignalDispatcher.cpp#L276) |
| 6 | 신호 → 요청 레코드(공유메모리에 그대로 복사되는 평범한 구조체) | [OrderChannel.cpp#L129 `to_request`](../Quant/src/ipc/OrderChannel.cpp#L129) |

전략 스레드(`Strategy`)는 샤드 바깥의 느린 일(국면 반영·전략 사전 갱신)을 맡는다: [EngineStrategyThread.cpp#L273](../Quant/src/core/EngineStrategyThread.cpp#L273).

### 4.5 주문 — 게이트·원장 선기록·전송

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | 주문 스레드(`Order`). 요청 면에서 꺼내고 말이 되는지 본다. 1초 넘게 기다린 신규 매수는 버린다(D-127) | [EngineOrderThread.cpp#L391 `order_thread_fn`](../Quant/src/core/EngineOrderThread.cpp#L391) · [#L745 `is_plausible`](../Quant/src/core/EngineOrderThread.cpp#L745) |
| 2 | 운영단말 수동 주문도 같은 입구로 | [EngineOpsServer.cpp#L220 `take_manual_order`](../Quant/src/core/EngineOpsServer.cpp#L220) |
| 3 | 초당 한도 대기·거절 시 후퇴 | [OrderRateLimiter.cpp#L97](../Quant/src/core/OrderRateLimiter.cpp#L97) · [#L103 `on_rejected`](../Quant/src/core/OrderRateLimiter.cpp#L103) |
| 4 | 라우터: 신규 경로 열기(`open_new`) → 보내기 → 닫기, 세 단계로 나뉘어 전송은 다른 스레드가 맡는다(D-151) | [OrderRouterSubmit.cpp#L151 `submit`](../Quant/src/ipc/OrderRouterSubmit.cpp#L151) · [#L164 `new_route`](../Quant/src/ipc/OrderRouterSubmit.cpp#L164) · [OrderRouter.h#L77 `open_new`](../Quant/include/ipc/OrderRouter.h#L77) |
| 5 | **게이트 하나**. 킬스위치 → 방향 → 진입정지 → 매매창 → 수량·명목 → 포지션 → 슬롯 → 총노출 → 일손실 → 손익 낡음 → 중복 → 유량 순으로 본다 | [OrderGate.cpp#L296 `check`](../Quant/src/risk/OrderGate.cpp#L296) · [#L168 `clamp_buy_quantity`](../Quant/src/risk/OrderGate.cpp#L168) · [OrderGateDisplace.cpp#L57 교체진입](../Quant/src/risk/OrderGateDisplace.cpp#L57) |
| 6 | **원장에 먼저 적고 보낸다**(D-113). INTENT 레코드를 일지 파일 `ledger_YYYYMMDD.bin`에 쓴 뒤 전송 | [PositionLedger.cpp#L76 `on_intent`](../Quant/src/risk/PositionLedger.cpp#L76) · [LedgerJournal.h#L78 `Record`](../Quant/include/risk/LedgerJournal.h#L78) |
| 7 | 전송 스레드 풀(`HttpLane N`)이 KIS 주문 API 왕복. 주문번호(ODNO)를 받아 라우터에 돌려준다 | [TransportPool.h#L34](../Quant/include/core/TransportPool.h#L34) · [KisOrder.cpp#L217 `submit_order_acknowledgement`](../Quant/src/api/KisOrder.cpp#L217) |
| 8 | 단계별 시각을 남긴다(지연 추적) | [LatencyTrace.h#L141 `record`](../Quant/include/core/LatencyTrace.h#L141) |
| 9 | 응답 레코드로 전략 쪽에 결과 통보 | [OrderChannel.cpp#L213 `make_response`](../Quant/src/ipc/OrderChannel.cpp#L213) |

### 4.6 체결 — 통보에서 장부까지

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | WebSocket 체결통보(AES 복호화) → 순수 디코더 | [KisWebSocketParse.cpp#L401 `parse_fill_notification`](../Quant/src/api/KisWebSocketParse.cpp#L401) · [KisWsDecode.h#L177 `decode_fill`](../Quant/include/api/KisWsDecode.h#L177) |
| 2 | 체결 큐에 넣는다(분리 프로세스면 공유메모리 체결 면) | [EngineFeed.cpp#L195 `push_fill_notice`](../Quant/src/core/EngineFeed.cpp#L195) · [FillChannel.cpp#L157 `push`](../Quant/src/ipc/FillChannel.cpp#L157) |
| 3 | 체결 스레드(`Fill`)가 꺼내 라우터에 맞춘다(주문번호 매칭) | [EngineFillThread.cpp#L37 `fill_thread_fn`](../Quant/src/core/EngineFillThread.cpp#L37) → [OrderRouterFill.cpp#L113 `on_fill`](../Quant/src/ipc/OrderRouterFill.cpp#L113) |
| 4 | 원장 확정. 보유·현금·실현손익 갱신, FILL 레코드 일지에 추가 | [PositionLedger.cpp#L286 `on_fill_confirmed`](../Quant/src/risk/PositionLedger.cpp#L286) |
| 5 | 장부 스레드(`Ledger`)가 100ms마다 스냅숏을 공유메모리에 발행. 게이트와 전략이 그것을 읽는다 | [EngineLedgerThread.cpp#L24](../Quant/src/core/EngineLedgerThread.cpp#L24) · [OrderGate.cpp#L828 `publish_ledger`](../Quant/src/risk/OrderGate.cpp#L828) · [LedgerSnapshot.h#L154 `read_stable`](../Quant/include/ipc/LedgerSnapshot.h#L154) |
| 6 | 전략이 "팔 수 있는 수량"을 물을 때 스냅숏에서 읽는다 | [Engine.cpp#L569 `ledger_sellable`](../Quant/src/core/Engine.cpp#L569) |

### 4.7 제어·국면·종료

| 걸음 | 하는 일 | 코드 |
|---|---|---|
| 1 | 제어 스레드(`Control`). 박동 감시, 시세 끊김 판정, 수동 정지, 장 마감 판정 | [EngineControlThread.cpp#L109 `control_thread_fn`](../Quant/src/core/EngineControlThread.cpp#L109) · [ControlPlane.cpp#L95 `apply`](../Quant/src/core/ControlPlane.cpp#L95) · [Heartbeat.cpp#L28](../Quant/src/ipc/Heartbeat.cpp#L28) |
| 2 | 국면(regime). 별도 스레드가 8개 지표 투표로 `regime.json`을 쓰고, 엔진이 읽어 전략 켜고 끄기·진입 배율·진입 정지·강제 청산을 적용 | [RegimeFeed.cpp#L177 `run`](../Quant/src/regime/RegimeFeed.cpp#L177) → [EngineRegime.cpp#L225 `poll_regime_file`](../Quant/src/core/EngineRegime.cpp#L225) → [#L51 `apply_regime_selection`](../Quant/src/core/EngineRegime.cpp#L51) · [RegimeFileJudge.h#L97](../Quant/include/core/RegimeFileJudge.h#L97) |
| 3 | 수동 정지(운영단말·킬스위치) | [OrderGateState.cpp#L225 `set_manual_halt`](../Quant/src/risk/OrderGateState.cpp#L225) |
| 4 | 종료. 이유를 공유메모리 헤더에 CAS로 한 번만 적고, 스레드를 순서대로 멈춘다 | [Engine.cpp#L622 `request_shutdown`](../Quant/src/core/Engine.cpp#L622) → [#L653 `stop`](../Quant/src/core/Engine.cpp#L653) · [SharedRegion.cpp#L676 `mark_clean_shutdown`](../Quant/src/ipc/SharedRegion.cpp#L676) |

재기동하면 4.1의 9번(재시드)이 일지 파일과 KIS 잔고를 대조해 이어 간다. 재기동은 보유분을 청산 래퍼(ITB)로 넘기므로 장중 교체는 횟수를 남긴다(`scripts/deploy_guard.py`).

---

## 5. 주문 한 건의 여정 — 시간순으로

위 표를 한 건의 매수 주문으로 다시 읽으면 이렇다. 괄호 안은 그 일을 하는 스레드다.

1. **체결 한 건 도착**(WsRecv): 프레임을 `TradeData`로 풀어 큐에 넣는다. 문자열은 여기서 끝, 뒤로는 정수 id만 간다.
2. **샤드 배정**(FeedMux): 종목 id 해시로 샤드 행을 고른다. 같은 종목은 늘 같은 샤드라 순서가 보장된다.
3. **신호**(Shard N): 전략이 분봉을 갱신하고 진입 조건이 맞으면 `OrderSignal`을 낸다. 수량은 전략이 정하고 게이트가 깎는다.
4. **디스패치**(Shard N → Strategy): 순번 도장, 전략이 꺼져 있거나 청산 관리 중이면 버린다. 살아남으면 요청 면에 복사.
5. **판정**(Order): 1초 넘게 묵은 매수는 폐기. 게이트 12단계 검사. 통과하면 INTENT를 일지 파일에 먼저 쓴다.
6. **전송**(HttpLane N): KIS 주문 API. 주문번호를 받으면 라우터의 관리 주문(ManagedOrder)으로 등록.
7. **체결 통보**(WsRecv → Fill): 주문번호로 맞추고 원장에 FILL 확정. 부분체결은 누적.
8. **스냅숏**(Ledger): 100ms 안에 공유메모리 장부가 갱신되고, 다음 주문부터 게이트가 새 보유·노출로 판정한다.
9. **남는 것**: `logs/trades_YYYYMMDD.csv`, 일지 `ledger_YYYYMMDD.bin`, 지연 추적, ZMQ 발행, DB 체결 테이블(11절).

---

## 6. C++ 아키텍처

### 6.1 스레드

이름은 전부 `thread_name::set_current`([ThreadName.h#L22](../Quant/include/utils/ThreadName.h#L22))로 붙여 디버거·로그에서 보인다. 소스에서 뽑은 이름 목록이다.

| 스레드 | 수 | 하는 일 | 진입 함수 |
|---|---|---|---|
| Main | 1 | 기동·대기·종료 | [main.cpp#L259](../Quant/src/main.cpp#L259) |
| WsRecv | 소켓당 1 | 읽기·최소 디코드·push | [WebSocketClient.cpp#L83](../Quant/src/api/WebSocketClient.cpp#L83) |
| FeedMux · Feed N | 1 (+레인) | 소켓 큐 합치기·팬아웃 | [EngineFeed.cpp#L512](../Quant/src/core/EngineFeed.cpp#L512) |
| Shard N | `strategy_shards`(상한 64) | 전략 호출 | [EngineStrategyThread.cpp#L563](../Quant/src/core/EngineStrategyThread.cpp#L563) |
| Strategy | 1 | 샤드 밖 느린 일·디스패치 | [EngineStrategyThread.cpp#L273](../Quant/src/core/EngineStrategyThread.cpp#L273) |
| Order | 1 | 판정·게이트·원장 선기록 | [EngineOrderThread.cpp#L391](../Quant/src/core/EngineOrderThread.cpp#L391) |
| HttpLane N | `risk.order_transport_threads`(기본 4) | KIS 주문 왕복 | [TransportPool.h#L34](../Quant/include/core/TransportPool.h#L34) |
| Prefetch N | 2–8 | REST 선조회 풀 | [PrefetchPool.h#L33](../Quant/include/core/PrefetchPool.h#L33) |
| Fill · FillRecovery · StaleCancel | 1씩 | 체결 매칭·누락 복구·묵은 주문 취소 | [EngineFillThread.cpp#L37](../Quant/src/core/EngineFillThread.cpp#L37) |
| Ledger | 1 | 스냅숏 발행 | [EngineLedgerThread.cpp#L24](../Quant/src/core/EngineLedgerThread.cpp#L24) |
| Control | 1 | 박동·끊김·정지·마감 | [EngineControlThread.cpp#L109](../Quant/src/core/EngineControlThread.cpp#L109) |
| DataThread · RestPoller · DailyWarm | 1씩 | REST 폴링·일봉 예열 | [EngineDataThread.cpp#L44](../Quant/src/core/EngineDataThread.cpp#L44) |
| MarketBoard · RegimeFeed | 1씩 | 시장판·국면 | [MarketBoard.cpp#L337](../Quant/src/universe/MarketBoard.cpp#L337) · [RegimeFeed.cpp#L177](../Quant/src/regime/RegimeFeed.cpp#L177) |
| DbTick N · DbEvent | 워커 M + 1 | TimescaleDB COPY 적재(D-148·154) | [DbWorkers.cpp#L344](../Quant/src/ipc/DbWorkers.cpp#L344) |
| TickCapture · ZmqBridge · OpsServer · LogWriter | 1씩 | 캡처·발행·운영단말·비동기 로그 | [TickCapture.h#L146](../Quant/include/core/TickCapture.h#L146) · [ZmqBridge.h#L42](../Quant/include/ipc/ZmqBridge.h#L42) · [OpsServer.h#L53](../Quant/include/ipc/OpsServer.h#L53) · [Logger.h#L25](../Quant/include/utils/Logger.h#L25) |
| Reconcile · Replay · ZmqOrderIn N | 상황 따라 | 재시드·리플레이 입력·부하시험 주문 수신 | [LedgerReconciler.cpp#L319](../Quant/src/core/LedgerReconciler.cpp#L319) · [ReplaySource.h#L24](../Quant/include/core/ReplaySource.h#L24) |

스레드는 `std::jthread`로 만들고([Engine.cpp#L260](../Quant/src/core/Engine.cpp#L260)) `stop_token`으로 멈춘다. 스레드 본문은 `thread_guard`([ThreadGuard.h#L12](../Quant/include/utils/ThreadGuard.h#L12))로 감싸 예외가 새면 로그와 함께 종료 요청을 낸다.
잠·깨우기는 `WakeGate`([WakeGate.h#L16](../Quant/include/core/WakeGate.h#L16), D-137)로 한다: 생산자가 `notify`, 소비자는 `atomic::wait`에 들어가며, 바쁜 대기(spin)를 짧게 두고 그 다음 잔다.

### 6.2 큐 — 생산자 수로 고른다

| 큐 | 생산자:소비자 | 어디에 | 핵심 |
|---|---|---|---|
| `RingBuffer<T>` | 1:1 (SPSC) | 소켓→FeedMux, 샤드 행렬의 각 칸 | head·tail을 캐시라인에 따로 두어 거짓 공유를 막는다 — [RingBuffer.h#L27](../Quant/include/core/RingBuffer.h#L27) · [#L146 `alignas`](../Quant/include/core/RingBuffer.h#L146) |
| `MpscQueue<T>` | N:1 (MPSC) | 샤드→디스패처(`shard_out`), 로그, 체결 | Vyukov 식 순번 칸(sequence cell) — [MpscQueue.h#L40](../Quant/include/core/MpscQueue.h#L40) · [#L110 `Cell`](../Quant/include/core/MpscQueue.h#L110) |
| `shard::Matrix` | N×M SPSC 행렬 | 수신 레인 → 샤드 | 생산자 행·소비자 열마다 SPSC 하나. MPSC보다 먼저 검토하는 이유는 경쟁이 없어서다 — [ShardMatrix.h#L30](../Quant/include/core/ShardMatrix.h#L30) · [#L24 `shard_of`](../Quant/include/core/ShardMatrix.h#L24) |
| `SharedSpscRing<Record>` | 1:1, 프로세스 간 | 요청·응답·체결·시세 면 | 레코드는 trivially copyable 강제, 인덱스는 `atomic_ref`로 공유메모리 위에서 — [SharedSpscRing.h#L57](../Quant/include/ipc/SharedSpscRing.h#L57) · [#L60 `static_assert`](../Quant/include/ipc/SharedSpscRing.h#L60) · [#L161 `push`](../Quant/include/ipc/SharedSpscRing.h#L161) |
| `GenerationCopy<C>` | 1:N 읽기 | 전략 사전·구독 표 | 세대 번호 붙인 복사본 교체(읽는 쪽은 잠금 없음) — [GenerationCopy.h#L13](../Quant/include/utils/GenerationCopy.h#L13) |

### 6.3 공유메모리와 프로세스 분리(D-114)

- 영역 하나(`SharedRegion`, [SharedRegion.h#L86](../Quant/include/ipc/SharedRegion.h#L86))에 헤더 128B([#L48](../Quant/include/ipc/SharedRegion.h#L48))를 두고 그 뒤에 면들을 순서대로 깐다. 헤더에는 부팅 세대(boot_generation)와 종료 이유가 있다.
- 면 목록과 순서는 [SharedLayout.h#L120](../Quant/include/ipc/SharedLayout.h#L120)·[SharedLayout.cpp#L185](../Quant/src/ipc/SharedLayout.cpp#L185). 역할이 `both`면 같은 배치를 힙에 깐다. 코드는 한 벌, 메모리 위치만 다르다.
- 장부 스냅숏은 seqlock 식이다. 쓰는 쪽이 version을 홀수로 올리고 쓰고 짝수로 내린다. 읽는 쪽은 version이 같고 짝수일 때만 믿는다 — [LedgerSnapshot.h#L154 `read_stable`](../Quant/include/ipc/LedgerSnapshot.h#L154).
- 박동(`Heartbeat`, [Heartbeat.h#L15](../Quant/include/ipc/Heartbeat.h#L15))으로 상대 프로세스 생존을 본다. 제어 면의 명령은 어느 역할이 받을지 미리 정해 둔다 — [ControlChannel.h#L63 `routes_to_feed`](../Quant/include/ipc/ControlChannel.h#L63).
- 종목 사전·전략 사전도 면이다. 분리 프로세스가 같은 정수 id를 쓰게 하려고 기동 시 한 번 깔아 둔다.

### 6.4 리스크·원장 계층

- **게이트는 하나**: [OrderGate.h#L87](../Quant/include/risk/OrderGate.h#L87). 교체진입(`DisplacementDesk`, [DisplacementDesk.h#L19](../Quant/include/risk/DisplacementDesk.h#L19))과 진입 우선순위([EntryPriority.h#L27](../Quant/include/risk/EntryPriority.h#L27))가 그 아래에 있다.
- **원장**: [PositionLedger.h#L42](../Quant/include/risk/PositionLedger.h#L42). 돈은 `money::Won`(int64 정수 원, [Money.h#L15](../Quant/include/risk/Money.h#L15))으로 다루어 부동소수 오차를 없앴다.
- **일지 파일**: 레코드 192B 고정([LedgerJournal.h#L105](../Quant/include/risk/LedgerJournal.h#L105)), CRC32 표는 컴파일 시점에 만든다([#L141 `make_crc_table`](../Quant/include/risk/LedgerJournal.h#L141)). 사람이 읽는 글자 파일도 옆에 남긴다(커밋 795de31). 정본 [docs/guides/LEDGER_JOURNAL_GUIDE.md](guides/LEDGER_JOURNAL_GUIDE.md).

### 6.5 시세 입력을 바꿔 끼우는 자리

`IFeedSource`([IFeedSource.h#L16](../Quant/include/core/IFeedSource.h#L16))가 입력 인터페이스다. 실전은 KIS WebSocket, 백테스트 리플레이는 `ReplaySource`([ReplaySource.h#L24](../Quant/include/core/ReplaySource.h#L24)), 부하시험은 ZMQ 주문 입력·모의 거래소([ZmqOrderFeed.h#L32](../Quant/include/exchange/ZmqOrderFeed.h#L32) · [MatchingEngine.h#L255](../Quant/include/exchange/MatchingEngine.h#L255)).
주문 실행도 `IOrderExecutor`([IOrderExecutor.h#L72](../Quant/include/api/IOrderExecutor.h#L72))라 모의 실행기 `PaperExecutor`([PaperExecutor.h#L33](../Quant/include/core/PaperExecutor.h#L33))로 바꿀 수 있다.

정본: [docs/ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md)(스레드·큐 표, 3프로세스 그림, 시세 끊김, 국면, 로깅). 결정 배경은 [docs/DECISIONS.md](DECISIONS.md)의 D-071·D-113·D-114·D-127·D-128·D-132·D-151.

---

## 7. 핵심 타입

| 타입 | 뜻 | 선언 |
|---|---|---|
| `symbol::SymbolId` · `symbol::Ticker` | 종목 정수 id, 15자 고정 종목코드. 뜨거운 경로는 id만 쓴다 | [SymbolTable.h#L1](../Quant/include/core/SymbolTable.h#L1) · [#L39](../Quant/include/core/SymbolTable.h#L39) |
| `TradeData` | 체결 한 건. trivially copyable을 `static_assert`로 못 박아 memcpy·공유메모리에 그대로 간다 | [Types.h#L239](../Quant/include/core/Types.h#L239) · [#L258](../Quant/include/core/Types.h#L258) |
| `OrderBook` · `MarketData` | 호가·시세 요약 | [Types.h#L216](../Quant/include/core/Types.h#L216) · [#L39](../Quant/include/core/Types.h#L39) |
| `OrderSide` | 매수/매도. `parse`는 `constexpr`라 컴파일 시점에 풀린다 | [Types.h#L59](../Quant/include/core/Types.h#L59) · [#L86](../Quant/include/core/Types.h#L86) |
| `OrderSignal` | 전략이 내는 신호(종목 id·방향·수량·가격·전략 id·순번) | [Types.h#L149](../Quant/include/core/Types.h#L149) |
| `FillNotification` | 체결 통보 | [Types.h#L275](../Quant/include/core/Types.h#L275) |
| `OrderRequest` · `OrderResponse` | 요청·응답 면 레코드 | [OrderChannel.cpp#L129](../Quant/src/ipc/OrderChannel.cpp#L129) |
| `LedgerJournal::Record` | 일지 레코드 192B | [LedgerJournal.h#L78](../Quant/include/risk/LedgerJournal.h#L78) |
| `KisResult<T>` | KIS 호출 결과(값 또는 오류 코드·메시지) | [KisResult.h](../Quant/include/api/KisResult.h) |
| `money::Won` | 정수 원 | [Money.h#L15](../Quant/include/risk/Money.h#L15) |

---

## 8. 쓰인 문법·기법 — 어디서 어떻게

아래 횟수는 2026-10-21에 `grep -rE`로 `Quant/src`·`Quant/include`의 `.cpp`·`.h`에서 센 줄 수다. 쓰는 자리 하나씩을 링크했다.

### 8.1 C++20/23 표준 기능

| 기능 | 줄 수 | 왜 쓰나 | 예 |
|---|---|---|---|
| `std::jthread` · `std::stop_token` | 37 · 87 | 스레드 수명과 종료 요청을 객체에 묶는다. 소멸자가 join | [WebSocketClient.cpp#L64](../Quant/src/api/WebSocketClient.cpp#L64) · [Engine.cpp#L260](../Quant/src/core/Engine.cpp#L260) |
| `std::atomic_ref` | 20 | 공유메모리 위 일반 정수를 원자적으로 다룬다(객체를 거기 둘 수 없어서) | [SharedSpscRing.h#L91](../Quant/include/ipc/SharedSpscRing.h#L91) · [SharedLayout.cpp#L155](../Quant/src/ipc/SharedLayout.cpp#L155) |
| `atomic::wait` / `notify_one` · `atomic_thread_fence` | 11 · 9 | 조건변수 없이 잠·깨우기 | [WakeGate.cpp#L117](../Quant/src/core/WakeGate.cpp#L117) · [#L112](../Quant/src/core/WakeGate.cpp#L112) |
| `std::span<>` | 1 | 바이트 구간을 복사 없이 디코더에 넘긴다 | [KisWsDecode.h#L31](../Quant/include/api/KisWsDecode.h#L31) |
| `std::expected` | 3 | 예외 대신 값·오류 반환 | [EngineStartupSetup.cpp#L164](../Quant/src/core/EngineStartupSetup.cpp#L164) |
| `std::ranges::` | 5 | 정렬·탐색 | [KisUniverse.cpp#L356](../Quant/src/api/KisUniverse.cpp#L356) |
| `std::from_chars` · `std::to_chars` | 11 · 4 | 할당 없는 숫자 파싱·출력(hot path) | [KisRestDecode.cpp#L89](../Quant/src/api/KisRestDecode.cpp#L89) · [DbCopyFormat.cpp#L18](../Quant/src/ipc/DbCopyFormat.cpp#L18) |
| `std::string_view` | 368 | 복사 없는 문자열 조회(수명은 호출자) | [KisClient.cpp#L19](../Quant/src/api/KisClient.cpp#L19) |
| `std::optional` · `std::variant` | 177 · 7 | 있을 수도 없을 수도 있는 값·두 갈래 결과 | [KisAccount.cpp#L118](../Quant/src/api/KisAccount.cpp#L118) · [EngineOrderThread.cpp#L226](../Quant/src/core/EngineOrderThread.cpp#L226) |
| `std::byte` | 53 | 배치 바이트 계산 | [EngineLayout.cpp#L142](../Quant/src/core/EngineLayout.cpp#L142) |
| `std::chrono` | 561 | 모든 시각·간격. 정수 ms를 손으로 들고 다니지 않는다 | [KisAuth.cpp#L64](../Quant/src/api/KisAuth.cpp#L64) |
| `std::filesystem` | 136 | 일지·캡처·설정 경로 | [EngineControlThread.cpp#L455](../Quant/src/core/EngineControlThread.cpp#L455) |
| 지정 초기화자(`.field = `) | 1,074 | 구조체 필드를 이름으로 채워 순서 실수를 막는다 | [KisAuth.cpp#L136](../Quant/src/api/KisAuth.cpp#L136) |
| `[[nodiscard]]` · `noexcept` | 457 · 465 | 반환값 무시 금지, 던지지 않음 표시(큐·디코더) | [EngineOrderThread.cpp#L327](../Quant/src/core/EngineOrderThread.cpp#L327) · [KisClient.cpp#L9](../Quant/src/api/KisClient.cpp#L9) |
| `constexpr` · `static_assert` | 516 · 55 | 컴파일 시점 상수·표(CRC)·크기 검증 | [LedgerJournal.h#L141](../Quant/include/risk/LedgerJournal.h#L141) · [Types.h#L258](../Quant/include/core/Types.h#L258) |
| `enum class` · `override` · `final` | 48 · 164 · 10 | 범위 있는 열거·가상 함수 표시·상속 금지 | [Types.h#L132](../Quant/include/core/Types.h#L132) · [WsSocketPosix.cpp#L356](../Quant/src/api/WsSocketPosix.cpp#L356) |
| `alignas(kCacheLine)` | 16 | 큐 인덱스 거짓 공유 방지 | [MpscQueue.h#L156](../Quant/include/core/MpscQueue.h#L156) · [RingBuffer.h#L146](../Quant/include/core/RingBuffer.h#L146) |
| `memory_order_*` | 495 | 락프리 큐의 획득·해제 순서 | [KisWebSocket.cpp#L6](../Quant/src/api/KisWebSocket.cpp#L6) |
| 템플릿 | 50 | 큐·풀·세대 복사 같은 그릇 | [TransportPool.h#L34](../Quant/include/core/TransportPool.h#L34) · [ThreadGuard.h#L19](../Quant/include/utils/ThreadGuard.h#L19) |

안 쓰는 것(0줄): `std::format`, concepts/`requires`, `[[likely]]`, `operator<=>`, `consteval`, `std::bit_cast`, `std::to_underlying`, `using enum`, `std::latch`/`barrier`.
문법 설명 자체는 [docs/guides/CPP20_23_GUIDE.md](guides/CPP20_23_GUIDE.md)(17절)에 있다.

### 8.2 설계 기법

| 기법 | 어디에 | 보기 |
|---|---|---|
| 락프리 SPSC/MPSC 큐 | 시세·신호·로그 | [RingBuffer.h#L27](../Quant/include/core/RingBuffer.h#L27) · [MpscQueue.h#L40](../Quant/include/core/MpscQueue.h#L40) |
| seqlock 식 스냅숏 | 장부 | [LedgerSnapshot.h#L154](../Quant/include/ipc/LedgerSnapshot.h#L154) |
| 세대 복사(RCU 비슷) | 전략 사전·구독 표 | [GenerationCopy.h#L13](../Quant/include/utils/GenerationCopy.h#L13) |
| 순수 디코더(입력 바이트 → 구조체, 부수효과 없음) | WS·REST 응답 | [KisWsDecode.h#L152](../Quant/include/api/KisWsDecode.h#L152) · [KisRestDecode.h#L70](../Quant/include/api/KisRestDecode.h#L70) |
| 인터페이스로 바꿔 끼우기 | 시세 입력·주문 실행·시세 조회 | [IFeedSource.h#L16](../Quant/include/core/IFeedSource.h#L16) · [IOrderExecutor.h#L72](../Quant/include/api/IOrderExecutor.h#L72) · [IMarketDataSource.h#L22](../Quant/include/api/IMarketDataSource.h#L22) |
| pimpl(구현 숨김) | 로거 | [Logger.h#L82 `Implementation`](../Quant/include/utils/Logger.h#L82) |
| 비동기 로깅(MPSC 65,536 + writer 스레드) | 전역 | [Logger.h#L25](../Quant/include/utils/Logger.h#L25) · [#L91 `LOG_INFO`](../Quant/include/utils/Logger.h#L91) |
| 작업 풀 템플릿 | 전송·선조회 | [TransportPool.h#L34](../Quant/include/core/TransportPool.h#L34) · [PrefetchPool.h#L33](../Quant/include/core/PrefetchPool.h#L33) |
| 정수 원 | 원장·게이트 | [Money.h#L15](../Quant/include/risk/Money.h#L15) |
| 선기록 후전송(write-ahead) + CRC | 일지 | [PositionLedger.cpp#L76](../Quant/src/risk/PositionLedger.cpp#L76) · [LedgerJournal.cpp#L95](../Quant/src/risk/LedgerJournal.cpp#L95) |
| 초당 한도 버킷 공유(D-138·162) | KIS 호출 | [KisRateBucket.h#L8](../Quant/include/api/KisRateBucket.h#L8) |
| OS 공유메모리 | Windows `CreateFileMapping`·POSIX `shm_open` 한 파일 안 분기 | [SharedRegion.cpp#L88](../Quant/src/ipc/SharedRegion.cpp#L88) |
| 대칭키 복호화(AES, 체결통보) | WS | [KisWebSocketParse.cpp#L3](../Quant/src/api/KisWebSocketParse.cpp#L3) |
| 지연 추적(단계별 시각) | 주문 경로 | [LatencyTrace.h#L141](../Quant/include/core/LatencyTrace.h#L141) |
| 미니덤프(dbghelp) | 비정상 종료 | [CMakeLists.txt#L353](../Quant/CMakeLists.txt#L353) |

코드 규약(Allman 중괄호, C 캐스트 금지, 약어 금지, 헤더엔 선언만)은 [docs/guides/CODE_CONVENTIONS.md](guides/CODE_CONVENTIONS.md).

---

## 9. 전략 계층

| 전략 | 상태 | 선언 |
|---|---|---|
| DeviationScale(이동평균 이탈 눌림 매수) | **라이브 유일**(모의·실계좌). 청산은 SMA60 종가 이탈 다음 날 09:00(D-158) | [DeviationScaleStrategy.h#L205](../Quant/include/strategy/DeviationScaleStrategy.h#L205) |
| IntradayBreakout(ITB) | 신호 전략 아님. 승계 보유분 청산 전용 래퍼 | [IntradayBreakoutStrategy.h#L38](../Quant/include/strategy/IntradayBreakoutStrategy.h#L38) |
| TargetBasket | 가치 코어 바스켓(모의, D-109) | [TargetBasketStrategy.h#L21](../Quant/include/strategy/TargetBasketStrategy.h#L21) |
| SwingTrend · SurgeHold · VwapPullback · MACross · ValueContrary · MarketMaking · FixedInterval | 실험·시험용 | [SwingTrendStrategy.h#L32](../Quant/include/strategy/SwingTrendStrategy.h#L32) · [SurgeHoldStrategy.h#L30](../Quant/include/strategy/SurgeHoldStrategy.h#L30) · [VwapPullbackStrategy.h#L25](../Quant/include/strategy/VwapPullbackStrategy.h#L25) · [MACrossStrategy.h#L15](../Quant/include/strategy/MACrossStrategy.h#L15) · [ValueContraryStrategy.h#L39](../Quant/include/strategy/ValueContraryStrategy.h#L39) · [MarketMakingStrategy.h#L33](../Quant/include/strategy/MarketMakingStrategy.h#L33) · [FixedIntervalStrategy.h#L18](../Quant/include/strategy/FixedIntervalStrategy.h#L18) |

**전략을 하나 더하려면**
1. `StrategyBase`([StrategyBase.h#L38](../Quant/include/strategy/StrategyBase.h#L38))를 상속한다. `on_start`([#L112](../Quant/include/strategy/StrategyBase.h#L112))에서 `symbol_of()`로 id를 받아 두고 `on_trade_batch`([#L96](../Quant/include/strategy/StrategyBase.h#L96))에서 `trade.symbol_id`와 정수 비교한다.
2. [StrategyFactory.cpp#L578](../Quant/src/strategy/StrategyFactory.cpp#L578)의 타입별 로더에 `engine.add_strategy(...)` 등록을 더한다.
3. config의 `strategies` 배열에 타입 이름을 넣는다. 국면별 켜고 끄기는 `regime_strategies` 맵.
4. 테스트는 `Quant/tests/test_<이름>.cpp`를 만들고 `Quant/CMakeLists.txt`에 타깃을 더한다.

전략 스펙·실증·매매일지는 [strategies/README.md](../strategies/README.md). 라이브 상태 요약은 저장소 루트 `STRATEGIES.md`(로컬 전용, gitignore).

---

## 10. KIS 계층

- `KisClient`([KisClient.h#L65](../Quant/include/api/KisClient.h#L65))는 7개 파일로 나뉜다: Transport(WinHTTP/libcurl, [KisTransport.cpp](../Quant/src/api/KisTransport.cpp)) · Auth([KisAuth.cpp#L104 `authenticate`](../Quant/src/api/KisAuth.cpp#L104)) · Market · Index · Order(KRX/NXT/SOR 시장 선택, D-096, [KisOrder.cpp#L217](../Quant/src/api/KisOrder.cpp#L217)) · Account · Universe.
- WebSocket은 `KisWebSocket`([KisWebSocket.h#L34](../Quant/include/api/KisWebSocket.h#L34)) 위에 소켓 추상 `WsSocket`([WsSocket.h#L9](../Quant/src/api/WsSocket.h#L9)), 구현은 Win/Posix 파일.
- 엔드포인트·TR ID·필드표는 기억보다 MCP `kis-code-assistant`가 최신이다(D-120). 확인한 값은 쓰는 자리에 확인일과 함께 남긴다.
- 모의와 실계좌는 앱키가 다르고, 앱키는 계좌당 하나, 구독 칸은 세션당 41이다. 칸은 늘릴 수 없어 우선순위로 나눈다(D-132).

---

## 11. 남기는 것 — 로그·일지·캡처·발행·DB

| 무엇 | 어디에 | 쓰는 코드 | 읽는 도구 |
|---|---|---|---|
| 실행 로그 | `logs/quant_trader.log`(기준 폴더는 `QUANT_LOG_DIR` 또는 실행파일 옆 `logs/`) | [Logger.h#L25](../Quant/include/utils/Logger.h#L25) | [scripts/parse_quant_log.py](../scripts/parse_quant_log.py#L325)(`--watch`로 장중 판정) |
| 체결 CSV | `logs/trades_YYYYMMDD.csv` | [OrderJournal.cpp#L350](../Quant/src/ipc/OrderJournal.cpp#L350) | [scripts/summarize_trading_day.py](../scripts/summarize_trading_day.py) |
| 원장 일지 | `ledger_journal_dir/ledger_YYYYMMDD.bin` + 글자 파일 | [LedgerJournal.h#L180](../Quant/include/risk/LedgerJournal.h#L180) | [PYQuant/tools/ledger_dump.py](../PYQuant/tools/ledger_dump.py) |
| 틱 캡처(QTCAP v2) | `capture_dir/` | [TickCapture.h#L146](../Quant/include/core/TickCapture.h#L146) | 리플레이 입력 [ReplaySource.h#L24](../Quant/include/core/ReplaySource.h#L24) · [scripts/capture_stats.py](../scripts/capture_stats.py) |
| ZMQ 발행(4포트 블록, 모의 5555–5558·실 5565–5568) | TCP | [ZmqBridge.h#L42](../Quant/include/ipc/ZmqBridge.h#L42) | [PYQuant/ipc/subscriber.py#L38](../PYQuant/ipc/subscriber.py#L38) · [PYQuant/tools/ledger_recorder.py#L102](../PYQuant/tools/ledger_recorder.py#L102) |
| TimescaleDB 적재(틱·체결, COPY) | 도커 `Ubuntu-24.04` | [DbManager.h#L111](../Quant/include/ipc/DbManager.h#L111) · [DbConnection.cpp#L42](../Quant/src/ipc/DbConnection.cpp#L42) | [PYQuant/db/client.py](../PYQuant/db/client.py) · Grafana |
| 지연 추적 | 로그·벤치 | [LatencyTrace.h#L141](../Quant/include/core/LatencyTrace.h#L141) | [scripts/startup_to_order_timeline.py](../scripts/startup_to_order_timeline.py) |
| 운영단말 TCP | 포트는 config `ops` | [OpsServer.h#L53](../Quant/include/ipc/OpsServer.h#L53) | [Quant/tools/ops_terminal/OpsTerminalDlg.cpp](../Quant/tools/ops_terminal/OpsTerminalDlg.cpp) |

데이터 출처(네이버·KIS·data.go.kr·FinanceDataReader가 각각 무엇을 얼마나 주는지)는 [docs/DATA_SOURCES.md](DATA_SOURCES.md).

---

## 12. 운영 자동화 — 하루가 어떻게 도는가

1. **장 전** `scripts/premarket_routine.py` → 유니버스·국면 파일 준비.
2. **기동** 감시견 [auto_trade_day.ps1#L556 `Start-TraderProcess`](../scripts/auto_trade_day.ps1#L556)(모의)·[auto_trade_live.ps1](../scripts/auto_trade_live.ps1)(실계좌)이 Job Object로 트레이더를 띄우고 죽으면 다시 띄운다. 손으로 띄우면 엔진이 둘이 된다.
3. **장중** `parse_quant_log.py --watch`가 판정을 내고, 고친 코드는 시각을 가리지 않고 [deploy_trader.py](../scripts/deploy_trader.py#L278)로 교체한다(교체 횟수는 `_private/deploy_guard.log`).
4. **마감** [market_close_autodoc.py#L509](../scripts/market_close_autodoc.py#L509)가 매매일지(`strategies/<전략>/live/YYYY-MM-DD.md`)·마감 기록·대시보드를 만들고, [check_runtime_health.py#L4719](../scripts/check_runtime_health.py#L4719)의 판정 행이 매매일지 4절에 실린다. 사람이 열어 봐야 하는 확인 항목은 두지 않는다.
5. **대시보드** [scripts/dashboard_server.py#L1938](../scripts/dashboard_server.py#L1938)(:8787)과 [PYQuant/dashboard/build_dashboard.py#L1885](../PYQuant/dashboard/build_dashboard.py#L1885).

예약작업 시각표·훅·실패 시 볼 곳은 [docs/AUTOMATION.md](AUTOMATION.md). 운영 명령 복붙은 [docs/RUNBOOK.md](RUNBOOK.md).

---

## 13. Python — 백테스트·데이터·도구

- 진입 [PYQuant/main.py#L612](../PYQuant/main.py#L612): `backtest` · `live` · `monitor` · `record` · `operate`.
- 백테스트 엔진 [PYQuant/backtest/engine.py#L119 `BacktestEngine`](../PYQuant/backtest/engine.py#L119). 비용 모델(수수료·세금·슬리피지)은 [costs.py#L33](../PYQuant/backtest/costs.py#L33), 성과 지표 `metrics.py`, 통계 검정 `stats.py`, 국면 점수 `regime_scorer.py`, DevScale 리플레이 `devscale_replay.py`.
- 데이터: 시점 고정(`asof.py`·`point_in_time.py`), KRX·data.go.kr·FinanceDataReader 어댑터(`data/`). 규칙은 백테스트를 2010년부터 현재까지 한 번에 돌리고 청산은 익절·손절·MAE/MFE부터 본다.
- KIS 파이썬 클라이언트 `kis/client.py`·`endpoints.py`, 엔진 모니터 `ipc/subscriber.py`, 운영 명령 `ipc/operator.py`.
- 도구 `tools/`: 원장 기록기·부하 주입([load_injector.py#L598](../PYQuant/tools/load_injector.py#L598))·분봉 백필·워크포워드([walkforward.py#L50](../PYQuant/tools/walkforward.py#L50))·DART 재무 적재.
- 테스트 `PYQuant/tests/`(pytest).

백테스트 절차 정본은 [research/BACKTEST_FLOW.md](../research/BACKTEST_FLOW.md), 결과 원장은 [research/BACKTESTS.md](../research/BACKTESTS.md)·[research/BACKTEST_LOG.md](../research/BACKTEST_LOG.md). 스터디는 `research/studies/NN_*/README.md`(현재 45번까지), 리서치 회의는 [research/RESEARCH_COUNCIL.md](../research/RESEARCH_COUNCIL.md).

---

## 14. 테스트·벤치

- 단위 테스트 96개 파일은 `Quant/tests/test_*.cpp`, 벤치 33개는 `bench_*.cpp`. 타깃은 [Quant/CMakeLists.txt](../Quant/CMakeLists.txt#L399)부터 쭉 등록돼 있다.
- 주문 경로 전체를 한 번에 보는 테스트: [test_engine_order_path.cpp](../Quant/tests/test_engine_order_path.cpp). 역할 분리는 [test_engine_roles.cpp](../Quant/tests/test_engine_roles.cpp), 파이프라인은 [test_engine_pipeline.cpp](../Quant/tests/test_engine_pipeline.cpp).
- 성능은 바꾸기 전에 먼저 잰다(원칙 7). 수신 [bench_feed_ingest.cpp](../Quant/tests/bench_feed_ingest.cpp), 주문 경로 [bench_latency_path.cpp](../Quant/tests/bench_latency_path.cpp), 게이트 경쟁 [bench_gate_contention.cpp](../Quant/tests/bench_gate_contention.cpp), 장부 발행 [bench_ledger_publish.cpp](../Quant/tests/bench_ledger_publish.cpp), 잠·깨우기 [bench_wake_gate.cpp](../Quant/tests/bench_wake_gate.cpp).
- 부하시험(모의 거래소 + ZMQ 주문 주입)은 [docs/guides/LOAD_TEST_GUIDE.md](guides/LOAD_TEST_GUIDE.md). 데이터 경합 검사(TSAN)는 `scripts/tsan_round.sh`.

---

## 15. 더 읽을 문서

| 알고 싶은 것 | 문서 |
|---|---|
| 왜 그렇게 정했나(결정 번호 D-NNN) | [docs/DECISIONS.md](DECISIONS.md) |
| 코드 줄 단위 읽는 순서(생성물) | [docs/CODE_FLOW.md](CODE_FLOW.md) |
| 스레드·큐·공유 면 정본 | [docs/ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md) |
| 옛 단일 큐 시절 서사(역사) | [docs/guides/PIPELINE_A_to_Z.md](guides/PIPELINE_A_to_Z.md) · 루트 `ARCHITECTURE.md`(로컬 전용, gitignore) |
| 문법 설명 | [docs/guides/CPP20_23_GUIDE.md](guides/CPP20_23_GUIDE.md) |
| 코드 규약·주석 태그 | [docs/guides/CODE_CONVENTIONS.md](guides/CODE_CONVENTIONS.md) |
| 용어·약어 | [docs/GLOSSARY.md](GLOSSARY.md) |
| 운영 명령·자동화 | [docs/RUNBOOK.md](RUNBOOK.md) · [docs/AUTOMATION.md](AUTOMATION.md) |
| 여러 세션이 같이 일하는 법 | [docs/guides/MULTI_SESSION.md](guides/MULTI_SESSION.md) |
| 문서 동기화 규칙 | [docs/SYNC_MAP.md](SYNC_MAP.md) |
| 미룬 이슈 | [docs/DEFERRED_ISSUES.md](DEFERRED_ISSUES.md) |
