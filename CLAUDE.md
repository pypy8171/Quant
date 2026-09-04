# CLAUDE.md

이 파일은 이 저장소에서 작업할 때 Claude Code(claude.ai/code)에게 제공하는 가이드입니다.

## 빌드 명령어

**Windows (CMake Presets — Visual Studio 2022 / Ninja)**:
```bash
cmake --preset x64-debug   # 디버그 구성
cmake --build out/build/x64-debug

cmake --preset x64-release
cmake --build out/build/x64-release
```

**Windows (수동 Ninja 빌드, 현재 `build_win/` 레이아웃 기준)**:
```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B Quant/build_win -S Quant
cmake --build Quant/build_win
./Quant/build_win/quant_trader Quant/config/config.json
```

**Linux**:
```bash
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant
cmake --build Quant/build
./Quant/build/quant_trader Quant/config/config.json
```

Linux는 `libcurl4-openssl-dev`가 필요합니다 (`sudo apt install libcurl4-openssl-dev`). Windows는 네이티브 WinHTTP를 사용하므로 nlohmann/json(CMake FetchContent로 자동 다운로드) 외에 추가 의존성이 없습니다.

테스트 스위트는 없습니다. 검증은 **FEED** 모드로 실행하여 실시간 출력을 확인하는 방식으로 합니다.

## 실행 모드

`Quant/config/config.json`의 `"mode"` 값으로 제어합니다:

- **`"FEED"`** — KIS WebSocket에 연결하여 실시간 호가·체결 데이터를 1초마다 콘솔에 표시합니다. 주문 없이 연결 상태와 인증 정보를 검증할 때 사용합니다. 최상위 config `"tickers"`(국내 현물)와 함께 `"futures"`(국내 선물 코드 배열)를 주면 선물 실시간도 같이 구독·표시합니다. 선물은 실계좌 WS 도메인 전용이라 `is_paper=true`면 경고만 내고 건너뜁니다.
- **`"TRADE"`** — 4-스레드 엔진(3-스레드 파이프라인 + 제어 스레드)을 실행하고 장 중(평일 09:00–15:30 KST)에 실제 주문을 냅니다.

`config.json`에는 현재 **실거래 인증 정보**(`app_key`, `app_secret`, 실계좌 번호)가 저장되어 있습니다. 모의투자 엔드포인트(`openapivts.koreainvestment.com:29443`)로 전환하려면 `"is_paper": true`로 설정하세요.

## 아키텍처

### 스레드 모델

엔진은 락-프리 파이프라인 3-스레드(데이터→전략→주문)에 제어 스레드 하나를 더해 총 네 개의 스레드를 실행합니다:

```
[데이터 스레드]  →  market_queue_ (RingBuffer)  →  [전략 스레드]  →  order_queue_ (RingBuffer)  →  [주문 스레드]
  KIS REST                                          등록된 전략들                                    KIS 주문 API
  OHLCV 봉                                          → OrderSignal                                   send_order()
```

- `RingBuffer<T>`는 명시적 메모리 순서를 가진 `std::atomic`을 사용하는 SPSC(단일 생산자/단일 소비자) 락-프리 큐입니다.
- 데이터 스레드는 `fetch_interval_sec`초마다 KIS REST를 폴링하며, 장 외 시간에는 건너뜁니다.
- 전략 스레드는 등록된 전략 전체를 순회하며, `NONE`이 아닌 신호는 주문 큐에 push합니다.
- 제어 스레드(`control_thread_fn`)는 파이프라인 밖에서 잔고 리컨사일·손익(daily_pnl) 갱신 상태 감시 등 주기 운영 작업을 담당합니다(갱신이 끊기면 OrderGate 보수정지 토글).

### 핵심 타입 (`Quant/include/core/Types.h`)

`MarketData`(OHLCV + bar_index), `OrderSignal`(side/type/qty/price/**ref_price** + strategy_id), `Position`, `OrderBook`(5단계 호가, 채널 `H0STASP0`/선물 `H0IFASP0`), `TradeData`(실시간 체결, 채널 `H0STCNT0`/선물 `H0IFCNT0`), `WatchSpec`(FEED 구독 종목 명세 — `is_future` 플래그로 현·선 채널 선택), `Regime`(enum: BULL/NEUTRAL/BEAR/UNKNOWN), `RegimeSnapshot`(장 시작 국면 판정 결과 — score·200MA·정배열/역배열·지수 이평 분해).

> `OrderSignal.ref_price`는 시장가(price=0) 주문의 명목 한도 평가 기준가다. 지정가는 `price`로 명목을 재지만 시장가는 `price`가 0이라 이 값이 없으면 명목 백스톱이 우회된다(특히 급락장 강제청산의 시장가 전량매도). 발주 측이 직전 현재가/평단을 stamp한다.

### 국면(Regime) 대응

`RegimeController`(`Quant/include/core/RegimeController.h`)가 장 시작 1회 지수 종가>200MA(±1)와 정배열/역배열(ma20·ma60·ma120, ±1)로 `score∈{-2..+2}`를 매겨 BULL/NEUTRAL/BEAR/UNKNOWN을 판정한다. config `"regime_strategies": {"BULL":[id…],"NEUTRAL":[…],"BEAR":[…]}`를 주면 국면이 전략 집합을 자동 선택하고(재평가 주기 `regime_reeval_sec`, 기본 300초), 지정하지 않으면 전략별 `active_regimes` 방식으로 하위호환한다. BEAR 등에서는 보유 전량을 시장가로 청산하는 `FORCE_LIQ` 신호를 낸다. 이와 별개로 매크로 사이드카(`macro_regime_feed.py`)가 쓰는 `regime.json` 파일브리지가 `OrderGate::set_entry_halt`(신규매수만 차단, 청산은 통과)를 토글한다(config `regime_file`·`regime_stale_sec`).

### 전략 추가하기

1. `StrategyBase`(`Quant/include/strategy/StrategyBase.h`)를 상속합니다.
2. `id()`, `on_data(const MarketData&)`, `describe()`를 구현합니다.
3. `main.cpp`에서 `engine.add_strategy(std::make_unique<YourStrategy>(...))` 로 등록합니다.
4. 필요하면 `"strategies"` 아래에 설정 항목을 추가하고 `main.cpp`의 전략 로딩 블록에서 파싱합니다.

### KIS API 클라이언트 (`Quant/src/api/KisClient.cpp`)

플랫폼별 분기: Windows는 WinHTTP, Linux는 libcurl. OAuth2 토큰 발급과 bearer 토큰 캐싱을 처리합니다. 주요 메서드: `authenticate()`, `get_ohlcv()`, `get_current_price()`, `send_order()`, 국내 선물 시세 `get_future_price()`(단일 시세)·`get_future_board()`(전광판, 그릭스 포함).

### WebSocket 클라이언트 (`Quant/include/api/KisWebSocket.h`)

FEED 모드에서 사용합니다. REST로 approval key를 발급받고, `ops.koreainvestment.com:31000`(모의) 또는 `:21000`(실거래)에 연결한 뒤 구독한 채널의 파싱된 구조체를 등록된 콜백으로 전달합니다. 구독 채널은 종목당 `WatchSpec`으로 정하며, 국내 현물 호가 `H0STASP0`·체결 `H0STCNT0`, 국내 선물 호가 `H0IFASP0`·체결 `H0IFCNT0`(`WatchSpec.is_future=true`로 선택), 미국 체결 `HDFSCNT0`을 지원합니다. 선물 체결에는 매수/매도 방향 코드가 없어 `direction`을 0으로 둡니다. 최초 연결·재연결 경로에 흩어져 있던 구독 하드코딩은 `subscribe_all()` 한 곳으로 통합되어, 재연결 시 선물 채널이 누락되던 불일치를 없앴습니다. 국내 선물 실시간은 실계좌 WS 도메인 전용이라 모의(`is_paper=true`)에서는 지원되지 않습니다.

### 로깅

싱글톤 `Logger`가 밀리초 단위 UTC 타임스탬프로 콘솔과 `logs/quant_trader.log`(cwd 하위 `logs/` 폴더에 고정, 부모 폴더는 자동 생성)에 기록합니다. 과거 로그는 `logs/archive/`에 보관합니다. 사용 매크로: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`.

**비동기 구조**: 전략·주문 hot path는 레코드를 큐에 push만 하고 즉시 반환하며, 타임스탬프 포맷팅과 파일/콘솔 I/O는 전용 writer 스레드가 담당합니다(저지연은 평균 지연보다 최악 지연(tail latency)이 중요하다는 설계 의도로 디스크 플러시를 hot path에서 분리). 백프레셔: 큐가 상한(`kMaxQueue`)을 넘으면 가장 오래된 레코드를 드롭하고 드롭 수를 셉니다(운영 중 무한 증가·블로킹 방지). 종료·테스트 직전 정합 확인용 `flush()`를 제공합니다.

### 문서 동기화 (드리프트 방지)

색인·요약·링크가 실제 트리와 어긋나는 것을 막는다. 대표/색인 파일이 무엇을 요약하는지의 의존 표와 커밋 전 체크리스트는 [docs/SYNC_MAP.md](docs/SYNC_MAP.md)에 있다. 문서를 옮기거나 새 스터디·전략을 추가한 뒤에는 다음으로 링크·색인 정합을 검사한다(깨진 내부 링크·색인 미등재를 기계적으로 잡음, `@committer`가 문서 커밋 전 자동 실행):

```bash
python scripts/check_docs.py   # exit 0 = 통과, 1 = 드리프트
```

### 문서 문체 규약 (AI 문체 회피)

담백·겸손하게 쓴다. 수치·표·코드·링크·다이어그램은 절대 불변, 아래는 피한다:

- 과장·단정·설교조("~는 허구다/증명한다/입증한다/구조적으로 막는다") → 담백한 서술
- "X가 아니라 Y" 대조는 문단당 최대 1회
- 부풀린 동사(표방한다·지배당한다·강제한다·보장한다·권위적으로) → 담백한 동사(목표로 한다·좌우한다·막는다·확인한다)
- "유일·오직·결정적" 절대화 남용 완화(사실이면 유지)
- 메타 표지 헤더("왜 이걸 했나/정직 경계/정직성 배너") → 평범한 명사형 헤더
- 화살표(→)·`=`로 인과를 잇는 문장 → 산문화(단 다이어그램·표·파라미터·비율 표기의 기호는 유지)
- 볼드는 문단당 1개(표 헤더·정의목록 라벨은 예외)
- 멀쩡한 한국어 단어를 영어로 치환하지 말 것(예: "저지연"→"tail latency" 금지). 아포리즘·대조 스타일만 걷어내고 단어는 한국어로 유지, 전문용어는 괄호 주석으로(저지연은 최악 지연(tail latency)…)
- **지표 약어 병기**: 처음 보는 독자가 못 알아보는 지표 약어(p50·p99·p999·E2E·MDD·OOS·CAGR·turnover…)는 처음 등장 시 **쉬운말(약어)** 로 병기한다(예: `중앙값(p50)`·`최대낙폭(MDD)`·`전 구간(E2E)`). 표 헤더도 병기, 반복 등장은 약어만 가능. 코드는 **주석에만** 병기하고 식별자·필드명·데이터 키는 그대로. 병기 표준어 정본은 [docs/GLOSSARY.md](docs/GLOSSARY.md)(성능·지연 / 백테스트 섹션).

기존 톤 규율과 같은 기조다: 독자무시 뉘앙스 금지, 보고서 톤 겸손. `07/08/09_crisis` README처럼 스크립트 재생성 산출물은 손편집 대신 생성기/템플릿 단에서 고친다(재현성 게이트 `check_backtest.py`).

**적용 범위**: 설계 문서뿐 아니라 정리·기록 문서 전부에 적용한다. 새로 쓸 때와 기존 문서를 고칠 때 모두 해당한다.

- 장중매매 일지 `strategies/<전략>/live/YYYY-MM-DD.md`
- 사후검토 `strategies/<전략>/reviews/*.md`
- 백테스트 일지 `research/BACKTEST_LOG.md`·`research/BACKTESTS.md`·`research/runs/*.md`
- 스터디 리포트 `research/studies/**/README.md`(생성물은 생성기에서)
- 대시보드 데이터 `research/dashboard/reviews.json`의 `*_html` 필드
- 개발 로그 `DAILY_LOG.md`와 `docs/` 산문

**반복 적발된 표현과 대체어**. 왼쪽은 쓰지 않고, 오른쪽에서 문맥에 맞는 것을 고른다.

| 쓰지 않는다 | 대신 |
|---|---|
| 신선도, 신선도 상실/복구 | 갱신 상태, 갱신 끊김, 갱신 재개, 최신 여부 |
| 스테일, stale | 갱신 지연, 갱신이 멈춤 |
| 캐비엇 | 단서, 주의 |
| 양날의 검 | 실제 트레이드오프를 풀어쓴다("낙폭은 줄지만 반등 참여를 놓친다") |
| 사망, 죽는, 소멸 | 종료, 중단, 사라짐, 해소 |
| 폭풍, 스톰, 폭주 | 다발, 연속 발생, 반복 발생 |
| 좌초 | 미완료, 끝나지 않음 |
| 출혈 | 손실 확대 |
| 위장 | "~로 보임" |
| 봉쇄, 원천 차단 | 차단 |
| 잠식 | 차지, 밀어냄 |
| 재앙 | 큰 낙폭 |
| 독(毒) 은유 | 역효과 |
| 정직성 배너, "(정직성)" 표지 헤더 | 데이터·방법의 한계 |
| 두 마디 문학적 헤드라인("~한 날 — ~", "…남았다") | 평이한 요약 서술문 |

전문용어를 한국어로 옮기기 애매하면 한국어(영어) 병기로 쓴다: 취소·재주문 반복(churn), 잦은 반전(whipsaw), 대체 경로(fallback). 코드 식별자·설정키·필드명(`daily_pnl`, `regime_stale_sec`, `sellable_qty()`)은 산문 규칙과 무관하게 그대로 둔다.

문체를 고칠 때 수치·표·코드·링크·다이어그램·결론은 바꾸지 않는다. 톤만 바꾸고 판정은 그대로 남긴다.

## 플랫폼 참고사항

- Windows 빌드 플래그: `/utf-8`, `-D_WIN32_WINNT=0x0A00`(Windows 10+), `-D_CRT_SECURE_NO_WARNINGS`. FEED 화면 출력에는 ANSI 이스케이프 시퀀스와 `SetConsoleOutputCP(CP_UTF8)`를 사용합니다.
- Linux 디버그 빌드는 AddressSanitizer(`-fsanitize=address`)를 활성화합니다.
- `#ifdef _WIN32` 가드로 HTTP 및 WebSocket 플랫폼 코드를 분리합니다. 네트워크 기능 추가 시 이 패턴을 유지하세요.

## 토큰 이코노미 (매 작업 적용)

**원칙: 같은 결과가 나온다면 최소 토큰으로.** 작업을 시작하기 전에 해당 유형의 체크 항목을 적용한다.

| 작업 유형 | 시작 전 적용 |
|---|---|
| **파일 편집** | Edit/Write **직후 재-Read 금지**(하네스가 파일 상태 추적, 실패 시 에러). 이미 읽은 파일 재조회 금지 |
| **코드·파일 탐색** | 여러 파일/디렉터리를 훑어야 하면 `Explore`/서브에이전트 위임 → **결론만** 수신(파일 덤프를 메인 컨텍스트에 쌓지 않음). 파일·심볼·값이 이미 특정된 단일 사실은 직접 조회 |
| **명령 실행** | git은 `--porcelain`/`-s`, 로그·grep은 `head`/`tail`·범위 제한. 큰 diff·파일·트리 통째 덤프 금지(필요한 줄만) |
| **다중 조회** | 서로 독립인 조회는 **한 메시지에 병렬 tool 호출**로 묶어 왕복 최소화 |
| **서브에이전트 위임** | 예상 실패·예외를 **첫 호출에 포함**해 재질의 왕복을 줄인다(예: 원격 main 세탁 갈라짐 → `git rebase --onto origin/main <parent> HEAD`) |
| **백그라운드 작업** | 폴링·sleep 루프 금지 — 완료 알림으로 재호출됨 |
| **응답 작성** | 결론부터, 짧게. 이미 내린 결정 재설명·안 할 옵션 나열·중복 요약 금지 |
| **검증** | 바뀐 범위만 재검증. 같은 확인 두 번 금지 |

> 상세·근거는 개인 메모리 `feedback_token_economy`. 이 표는 프로젝트 개발 시 매 작업의 사전 체크리스트로 참조한다.
