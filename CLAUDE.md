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

단위 테스트는 `Quant/tests/`에 있고 ctest에 등록돼 있습니다(원장·게이트·라우터·큐·WS 디코더·REST 분봉 디코더·정규장 시각·잔고 대조 계산·운영단말 프로토콜/서버·비동기 로거 등 16개).
```bash
cmake --build out/build/x64-release --target test_order_gate test_order_router test_ws_frame test_ws_decode test_kis_decode test_market_session test_reconcile_plan test_regime test_ringbuffer test_ringbuffer_stress test_pipeline_stress test_mpsc test_account_ledger test_ops_protocol test_ops_server test_logger
ctest --preset x64-release          # 저장소 루트에서. 스트레스 2종은 3초로 줄여 돈다
ctest --test-dir Quant/build_win    # 수동 Ninja 레이아웃일 때
```
Linux에서는 `-DQUANT_TSAN=ON`으로 Debug를 ThreadSanitizer로 만들 수 있습니다(ASAN과 배타). 실계좌 연결 검증은 **FEED** 모드로 실행하여 실시간 출력을 확인합니다.

## 실행 모드

`Quant/config/config.json`의 `"mode"` 값으로 제어합니다:

- **`"FEED"`** — KIS WebSocket에 연결하여 실시간 호가·체결 데이터를 1초마다 콘솔에 표시합니다. 주문 없이 연결 상태와 인증 정보를 검증할 때 사용합니다. 최상위 config `"tickers"`(국내 현물)와 함께 `"futures"`(국내 선물 코드 배열)를 주면 선물 실시간도 같이 구독·표시합니다. 선물은 실계좌 WS 도메인 전용이라 `is_paper=true`면 경고만 내고 건너뜁니다.
- **`"TRADE"`** — 5-스레드 엔진(3-스레드 파이프라인 + 체결 소비 + 제어 스레드)을 실행하고 장 중(평일 09:00–15:30 KST)에 실제 주문을 냅니다.

`config.json`에는 현재 **실거래 인증 정보**(`app_key`, `app_secret`, 실계좌 번호)가 저장되어 있습니다. 모의투자 엔드포인트(`openapivts.koreainvestment.com:29443`)로 전환하려면 `"is_paper": true`로 설정하세요.

## 아키텍처

### 스레드 모델

엔진은 락-프리 파이프라인 3-스레드(데이터→전략→주문)에 체결 소비 스레드와 제어 스레드를 더해 총 다섯 개의 스레드를 실행합니다:

```
[데이터 스레드]  →  market_queue_ (RingBuffer)  →  [전략 스레드]  →  order_queue_ (RingBuffer)  →  [주문 스레드]
  KIS REST                                          등록된 전략들                                    KIS 주문 API
  OHLCV 봉                                          → OrderSignal                                   send_order()
```

- `RingBuffer<T>`는 명시적 메모리 순서를 가진 `std::atomic`을 사용하는 SPSC(단일 생산자/단일 소비자) 락-프리 큐입니다.
- 데이터 스레드는 `fetch_interval_sec`초마다 KIS REST를 폴링하며, 장 외 시간에는 건너뜁니다.
- 전략 스레드는 등록된 전략 전체를 순회하며, `NONE`이 아닌 신호는 주문 큐에 push합니다.
- 체결 소비 스레드(`fill_thread_fn`, D-056)는 WS 수신 스레드가 `fill_queue_`(SPSC)에 넣은 체결통보를 받아 `OrderRouter::on_fill`(원장 반영·CSV)과 운영단말 방송을 합니다. 수신 스레드는 push만 하므로 체결 처리 동안 틱이 서지 않습니다. 큐가 비면 condvar에서 자고 생산자가 깨웁니다(Logger와 같은 방식).
- 제어 스레드(`control_thread_fn`)는 파이프라인 밖에서 잔고 대조·손익(daily_pnl) 갱신 상태 감시 등 주기 운영 작업을 담당합니다(갱신이 끊기면 OrderGate 보수정지 토글).
- 대사 단계 귀속(D-038): 전략 스레드가 신호마다 `OrderSignal.seq`를 단조 stamp하고 라우터가 원장 CSV 전 행에 같은 번호를 남깁니다. 잔고 대조는 덮어쓰기·정리 전 원장 값으로 `core/ReconcilePlan.h`(순수 함수)가 어긋난 종목만 골라 `RECONCILE` 행(`OVERWRITE|PRUNE|KEEP`)을 씁니다.

### 핵심 타입 (`Quant/include/core/Types.h`)

`MarketData`(OHLCV + bar_index), `OrderSignal`(side/type/qty/price/**ref_price** + strategy_id), `Position`, `OrderBook`(5단계 호가, 채널 `H0STASP0`/선물 `H0IFASP0`), `TradeData`(실시간 체결, 채널 `H0STCNT0`/선물 `H0IFCNT0`), `WatchSpec`(FEED 구독 종목 명세 — `is_future` 플래그로 현·선 채널 선택), `Regime`(enum: BULL/NEUTRAL/BEAR/UNKNOWN), `RegimeSnapshot`(장 시작 국면 판정 결과 — score·200MA·정배열/역배열·지수 이평 분해).

> `OrderSignal.ref_price`는 시장가(price=0) 주문의 명목 한도 평가 기준가다. 지정가는 `price`로 명목을 재지만 시장가는 `price`가 0이라 이 값이 없으면 명목 백스톱이 우회된다(특히 급락장 강제청산의 시장가 전량매도). 발주 측이 직전 현재가/평단을 stamp한다.

### 국면(Regime) 대응

`RegimeController`(`Quant/include/core/RegimeController.h`)가 장 시작 1회 지수 종가>200MA(±1)와 정배열/역배열(ma20·ma60·ma120, ±1)로 `score∈{-2..+2}`를 매겨 BULL/NEUTRAL/BEAR/UNKNOWN을 판정한다. config `"regime_strategies": {"BULL":[id…],"NEUTRAL":[…],"BEAR":[…]}`를 주면 국면이 전략 집합을 자동 선택하고(재평가 주기 `regime_reeval_sec`, 기본 300초), 지정하지 않으면 전략별 `active_regimes` 방식으로 하위호환한다. 판정 파라미터(지수코드·이평기간·점수 임계값)는 config `"regime_tuning"`으로 덮어쓸 수 있고, 임계값 오버라이드는 실계좌에서 무시된다.

국면 축은 둘이고 하는 일이 다르다. **`RegimeController`는 전략 집합만 고른다 — 청산은 하지 않는다.** 보유 전량을 시장가로 청산하는 `FORCE_LIQ`는 다른 축, 즉 매크로 보조 프로세스(`macro_regime_feed.py`)가 쓰는 `regime.json` 파일 전달이 낸다(config `regime_file`·`regime_stale_sec`). 이 파일의 `entry_halt`는 `OrderGate::set_entry_halt`(신규매수만 차단, 청산은 통과)를 토글하고, `force_liquidate`는 여기에 더해 strategy_thread가 보유 전량에 대해 `FORCE_LIQ` 시장가 매도를 2초 간격으로 재발주하게 한다. 드릴 절차는 [docs/guides/REGIME_DRILL_GUIDE.md](docs/guides/REGIME_DRILL_GUIDE.md).

### 전략 추가하기

1. `StrategyBase`(`Quant/include/strategy/StrategyBase.h`)를 상속합니다.
2. `id()`, `on_data(const MarketData&)`, `describe()`를 구현합니다.
3. `main.cpp`에서 `engine.add_strategy(std::make_unique<YourStrategy>(...))` 로 등록합니다.
4. 필요하면 `"strategies"` 아래에 설정 항목을 추가하고 `main.cpp`의 전략 로딩 블록에서 파싱합니다.

### KIS API 클라이언트 (`Quant/include/api/KisClient.h`, 구현은 `Quant/src/api/Kis*.cpp` 7파일)

클래스는 하나고 구현이 도메인별로 나뉩니다(D-048): `KisTransport.cpp`(플랫폼별 HTTP — Windows는 WinHTTP, Linux는 libcurl — 재시도·초당 한도·공용 인증 헤더 `auth_headers()`), `KisAuth.cpp`(OAuth2 토큰 발급·캐시), `KisMarket.cpp`(주식 시세 — 분봉 페이지 병합·집계는 순수 함수 헤더 `Quant/include/api/KisRestDecode.h`, D-051), `KisIndex.cpp`(지수·수급·선물), `KisOrder.cpp`(주문), `KisAccount.cpp`(잔고·미체결), `KisUniverse.cpp`(순위·유니버스). 구현끼리만 쓰는 include·상수는 `Quant/src/api/KisClientInternal.h`. 주요 메서드: `authenticate()`, `get_ohlcv()`, `get_current_price()`, `send_order()`, 국내 선물 시세 `get_future_price()`(단일 시세)·`get_future_board()`(전광판, 그릭스 포함). 새 REST 호출은 인증 헤더 네 줄을 손으로 쓰지 말고 `auth_headers(tr_id, {추가 항목})`을 씁니다. 공개 헤더는 `nlohmann::json`을 내보내지 않습니다 — 잔고 `get_balance()`·전광판 `get_future_board()`는 `KisResult<T>`(`Quant/include/api/KisResult.h`, 실패 코드 동반) 봉투에 값 타입(`Quant/include/api/KisTypes.h`)을 담아 돌려주고, 응답 필드 해석은 `Quant/include/api/KisRestDecode.h`의 순수 함수가 맡습니다(D-059).

### WebSocket 클라이언트 (`Quant/include/api/KisWebSocket.h`, 구현은 `Quant/src/api/WebSocketClient.cpp` + `WsSocketWin.cpp`/`WsSocketPosix.cpp`)

FEED 모드에서 사용합니다. REST로 approval key를 발급받고, `ops.koreainvestment.com:31000`(모의) 또는 `:21000`(실거래)에 연결한 뒤 구독한 채널의 파싱된 구조체를 등록된 콜백으로 전달합니다. 구독 채널은 종목당 `WatchSpec`으로 정하며, 국내 현물 호가 `H0STASP0`·체결 `H0STCNT0`, 국내 선물 호가 `H0IFASP0`·체결 `H0IFCNT0`(`WatchSpec.is_future=true`로 선택), 미국 체결 `HDFSCNT0`을 지원합니다. 선물 체결에는 매수/매도 방향 코드가 없어 `direction`을 0으로 둡니다. 최초 연결·재연결 경로에 흩어져 있던 구독 하드코딩은 `subscribe_all()` 한 곳으로 통합되어, 재연결 시 선물 채널이 누락되던 불일치를 없앴습니다. 국내 선물 실시간은 실계좌 WS 도메인 전용이라 모의(`is_paper=true`)에서는 지원되지 않습니다. 소켓 계층은 `Quant/src/api/WsSocket.h`의 `WsSocket` 인터페이스 뒤에 있고(D-049) 플랫폼당 한 파일만 링크되므로, 연결·재연결·백오프·구독은 `WebSocketClient.cpp`에 플랫폼 코드 없이 한 벌입니다. 공개 헤더는 `<windows.h>`를 끌어오지 않습니다.

### 로깅

싱글톤 `Logger`가 밀리초 단위 UTC 타임스탬프로 콘솔과 `logs/quant_trader.log`(cwd 하위 `logs/` 폴더에 고정, 부모 폴더는 자동 생성)에 기록합니다. 과거 로그는 `logs/archive/`에 보관합니다. 사용 매크로: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`.

**비동기 구조**: 전략·주문 hot path는 레코드를 큐에 push만 하고 즉시 반환하며, 타임스탬프 포맷팅과 파일/콘솔 I/O는 전용 writer 스레드가 담당합니다(저지연은 평균 지연보다 최악 지연(tail latency)이 중요하다는 설계 의도로 디스크 플러시를 hot path에서 분리). 큐는 락 없는 `MpscQueue<Record>`(65,536슬롯)이고 writer는 큐가 비면 condvar에서 자며 생산자는 writer가 "잔다"고 표시한 때만 깨웁니다(D-045). 밀림 처리: 큐가 가득 차면 새 레코드를 드롭하고 `dropped()`로 셉니다(hot path 블로킹 방지). 종료·테스트 직전 정합 확인용 `flush()`를 제공합니다.

### 문서 동기화 (드리프트 방지)

색인·요약·링크가 실제 트리와 어긋나는 것을 막는다. 대표/색인 파일이 무엇을 요약하는지의 의존 표와 커밋 전 체크리스트는 [docs/SYNC_MAP.md](docs/SYNC_MAP.md)에 있다. 문서를 옮기거나 새 스터디·전략을 추가한 뒤에는 다음으로 링크·색인 정합을 검사한다(깨진 내부 링크·색인 미등재를 기계적으로 잡음, `@committer`가 문서 커밋 전 자동 실행):

```bash
python scripts/check_docs.py   # exit 0 = 통과, 1 = 드리프트
```

MFC 단말(`Quant/tools/ops_terminal/`)을 고쳤으면 [docs/guides/MFC_TERMINAL.md](docs/guides/MFC_TERMINAL.md)를
같은 커밋에서 갱신한다(8절 체크리스트). 실행 인자·산출물 경로·접속 방법이 바뀌면 `_private/LINKS.md` 운영단말 행도 고친다.

### 파일 지칭 규약 (전체 경로)

`README.md`·`config.json`·`main.cpp`처럼 저장소에 같은 이름이 여럿인 파일이 많다. 파일을 지칭할 때는
**저장소 루트 기준 전체 경로**를 쓴다. 대화 응답·문서·커밋 메시지 전부 해당한다.

- 쓴다: `research/studies/12_base_breakout/README.md`, `Quant/config/universe_scan.json`
- 쓰지 않는다: "README", "config 파일", "그 스터디 리드미"

산출물(csv·parquet·로그)도 같다. 폴더만 말하고 파일명을 생략하지 않는다.

### 링크 허브 자동 갱신

아티팩트를 새로 발행·재발행했거나, 복붙용 PowerShell 가이드(실행 절차·예약작업 시각)를 바꿨으면
`_private/LINKS.md`를 **말하지 않아도** 같이 고친다. 링크와 절차가 흩어지면 다음에 찾는 비용이 커진다.
`_private/`는 gitignore라 커밋 대상이 아니다. 저장소에 남기는 자동화 목록은 [docs/AUTOMATION.md](docs/AUTOMATION.md)가 소유한다.

### 문서 문체 규약 (AI 문체 회피)

담백·겸손하게 쓴다. 수치·표·코드·링크·다이어그램은 바꾸지 않는다. 적용 범위는 설계 문서만이 아니라
매매일지·사후검토·백테스트 일지·스터디 리포트·`research/dashboard/reviews.json`의 `*_html`·`DAILY_LOG.md`·
`docs/` 산문과 커밋 메시지 전부다. 문서를 쓰거나 고치기 전에 정본을 읽는다: [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md)
(과장·설교조 회피, 금지 표현과 대체어 표, 지표 약어 병기). `@committer` (d-2) 문체 스캔이 이 정본을 게이트로 쓴다.

적용 범위에는 코드 주석과 로그 문구도 들어간다 — 화면과 로그에 그대로 나오기 때문이다. 게이트는 두 지점이다: 쓰는 순간 `.claude/hooks/lexicon-gate.ps1`(Write·Edit 본문 검사, 차단), 커밋 직전 `@committer` (d-2) 문체 스캔.

```bash
python scripts/check_plain_language.py          # 검출
python scripts/check_plain_language.py --fix    # 치환(뒤 조사까지 맞춤)
```

### 코드 작업 규약 (주석·중괄호·커밋 분리)

코드를 고치거나 주석을 쓰기 전에 정본을 읽는다: [docs/guides/MAINTENANCE_AUTOMATION.md](docs/guides/MAINTENANCE_AUTOMATION.md) 4절.
주석은 위치마다 담을 것이 정해져 있다.

| 위치 | 쓸 것 | 쓰지 않을 것 |
|---|---|---|
| 파일 머리 | 목적 한 줄, 스레드 소유권, 관련 D-NNN | 항목 목록·개수 — 정본 위치를 가리킨다 |
| 함수 위 | 왜 따로 있는지, 호출 제약, 실패 시 동작 | 절차 서술 |
| 멤버 옆 | 단위와 불변식 | 경위(D-NNN으로 보낸다) |
| 블록 안 | 함정과 비자명한 결정 | 세 줄 넘는 태그 없는 설명 |

태그는 다섯 개다: `// [inv]` 불변식 · `// [lock-order]` 락 순서·memory_order 근거 ·
`// [wire]` 외부 프로토콜 필드·에러코드 · `// [why D-NNN]` 결정 참조 · `// [formula]` 수식·임계값 유도.

주석을 지울 때는 세 단계를 지킨다.

1. 지우기 전에 그 주장이 지금 코드와 맞는지 확인한다. 틀린 주석을 D-NNN으로 옮기면 오류가 정본이 된다.
2. 삭제 줄의 숫자·식별자·ID가 각각 어디에 남는지 목록으로 보고한다. 남을 곳이 없으면 지우지 않는다.
3. 코드 줄 diff는 0이어야 하고, 주석 정리 커밋은 기능 수정 커밋과 분리한다.

중괄호는 Allman이고 한 줄 본문에도 붙인다(`.clang-format`의 `InsertBraces`). `}` 뒤와 제어문 앞에는 빈 줄을 하나 둔다.
정리는 손으로 하지 말고 스크립트로 한다.

```bash
py scripts/brace_style.py                            # 중괄호·빈 줄 정리(인자 없으면 include/src/tests 전체)
py scripts/check_code_conventions.py                 # 스테이징 변경의 중괄호·D-NNN·태그 검사
py scripts/check_code_conventions.py --comment-only  # 주석 전용 커밋인지 검증(코드 줄 0)
```

주석 밀도는 게이트로 걸지 않는다. 파일별 밀도와 태그 없는 4줄 이상 블록은
`py scripts/maintain.py --weekly`가 `docs/reports/MAINTENANCE_WEEKLY.md`에 표로 남긴다.

## 플랫폼 참고사항

- Windows 빌드 플래그: `/utf-8`, `-D_WIN32_WINNT=0x0A00`(Windows 10+), `-D_CRT_SECURE_NO_WARNINGS`. FEED 화면 출력에는 ANSI 이스케이프 시퀀스와 `SetConsoleOutputCP(CP_UTF8)`를 사용합니다.
- Linux 디버그 빌드는 AddressSanitizer(`-fsanitize=address`)를 활성화합니다.
- HTTP는 `#ifdef _WIN32` 가드(`Quant/src/api/KisTransport.cpp`), WebSocket 소켓은 파일 단위(`WsSocketWin.cpp`/`WsSocketPosix.cpp`, CMake `if(WIN32)`)로 플랫폼 코드를 분리합니다. 네트워크 기능 추가 시 이 패턴을 유지하세요.

## 장중 운영 — 판단이 서면 실행한다

**장중이라도 재빌드·재기동은 허가돼 있다.** 결함을 찾았고 수정이 명확하면 물어보지 말고 진행한다.
매번 "재시작해도 될까요"를 되묻는 것이 더 큰 손해다(그 사이 체결이 원장에서 새는 것을 이미 겪었다).

| 상황 | 행동 |
|---|---|
| 원장·포지션 정합을 깨는 버그 발견 | 즉시 수정 → 테스트 빌드·실행 → `quant_trader` 재빌드 → 재기동 |
| `LNK1104 quant_trader.exe` (링크 실패) | 실행 중 프로세스가 exe를 잠근 것. 프로세스를 내리고 링크·재기동 |
| 재기동 | 반드시 **repo 루트**에서 원래 인자 그대로. cwd가 다르면 유니버스가 붕괴한다 |
| 재기동 직후 확인 | 잔고 재시드 수량·평단, `OrderRouter (FEP) 초기화 완료`, 체결통보 매칭 1건 |

재기동은 미체결 예약주문에 대한 라우터 기억(`history_`)을 지운다. 그 주문이 나중에 체결되면
ODNO 미매핑 체결로 들어오는데, 지금은 미연결 체결 경로가 원장·포지션에 반영한다(`OrderRouter::on_fill`).
잔고 재시드가 실제 보유수량을 다시 읽으므로 재기동 자체가 정합을 복구하는 방향이다.

예외 — 이건 그대로 물어본다: config의 리스크 한도·계좌 전환(모의↔실계좌), 보유분 강제청산,
git 커밋·푸시(`@committer` 승인 게이트).

## 다중 세션 — 세션당 git worktree

세션 여럿이 같은 작업 트리를 만지면 한쪽의 미완성 편집이 다른 쪽 빌드·테스트·커밋에 섞인다(09-11 실측:
동시 세션 3개가 `Quant/src/core/Engine.cpp`·`docs/DECISIONS.md`를 같이 건드려 diff 소유가 불분명해짐).
규칙은 하나다 — **코드를 바꾸는 세션은 자기 worktree에서 일한다.**

```bash
git worktree add ../Quant-wt-<주제> -b wt/<주제>      # 세션 시작 시 1회
git worktree list                                      # 누가 어디를 잡고 있는지
git worktree remove ../Quant-wt-<주제>                 # 머지 뒤 정리
```

- **메인 트리(`Quant/`)는 트레이더 배포 세션 하나만** 쓴다. `Quant/build_win/quant_trader.exe` 교체·감시견 재기동·
  `Quant/config/*.json` 수정은 이 세션만 한다. 다른 세션은 worktree에서 빌드해 ctest까지만 돌리고, exe 교체는 메인 세션에
  넘긴다(교체 절차는 메모리 `project_trader_watchdog_owner`).
- 문서만 고치는 세션은 메인 트리도 가능하되, 같은 파일을 두 세션이 열지 않는다(`git status --porcelain`으로 먼저 본다).
- 예약 작업(`_private/_cron/*_task.md`)은 메인 트리에서 돌고 `research/`·`_private/`만 쓴다. 코드 세션은 그 시각에
  `research/STRATEGY_LAB.md`를 건드리지 않는다.
- worktree는 `Quant/build_win/`을 공유하지 않는다 — 빌드 산출물은 worktree마다 새로 만든다(`$env:TEMP=C:uild_tmp` 회피는 동일).

## 토큰 이코노미 (매 작업 적용)

**원칙: 같은 결과가 나온다면 최소 토큰으로.** 작업을 시작하기 전에 해당 유형의 체크 항목을 적용한다.

| 작업 유형 | 시작 전 적용 |
|---|---|
| **파일 편집** | Edit/Write **직후 재-Read 금지**(하네스가 파일 상태 추적, 실패 시 에러). 이미 읽은 파일 재조회 금지. 같은 파일을 세 번째 읽게 되면 그 자리에서 필요한 범위를 넓혀 한 번에 읽는다. 단 Edit이 아닌 경로(빌드·생성기 산출물, `sed`/스크립트 편집, git 체크아웃·rebase, 서브에이전트·사용자 편집)로 바뀐 파일은 상태 추적이 안 되므로 확인한다 — 이때도 전체를 다시 읽지 말고 `git diff -U2 <파일>`·`grep -n`으로 바뀐 줄만 본다 |
| **코드·파일 탐색** | 여러 파일/디렉터리를 훑어야 하면 `Explore`/서브에이전트 위임 → **결론만** 수신(파일 덤프를 메인 컨텍스트에 쌓지 않음). 파일·심볼·값이 이미 특정된 단일 사실은 직접 조회. 심볼 위치는 Grep으로 먼저 특정하고 그 범위만 Read — 대형 파일(Engine.cpp·main.cpp·dashboard_server.py급)은 통째로 읽지 말고 `offset`/`limit` 지정 |
| **명령 실행** | git은 `--porcelain`/`-s`, 로그·grep은 `head`/`tail`·범위 제한. 큰 diff·파일·트리 통째 덤프 금지(필요한 줄만) |
| **다중 조회** | 서로 독립인 조회는 **한 메시지에 병렬 tool 호출**로 묶어 왕복 최소화 |
| **서브에이전트 위임** | 예상 실패·예외를 **첫 호출에 포함**해 재질의 왕복을 줄인다(예: 원격 main 세탁 갈라짐 → `git rebase --onto origin/main <parent> HEAD`) |
| **백그라운드 작업** | 폴링·sleep 루프 금지 — 완료 알림으로 재호출됨 |
| **응답 작성** | 결론부터, 짧게. 이미 내린 결정 재설명·안 할 옵션 나열·중복 요약 금지 |
| **검증** | 바뀐 범위만 재검증. 같은 확인 두 번 금지 |
| **세션 운영** | 작업 단위가 바뀌면 세션을 분리한다(`/clear`). 초반에 쌓인 토큰은 남은 턴 수만큼 재전송되므로, 긴 단일 세션이 가장 큰 낭비 요인이다(68세션 실측: 세션당 평균 216턴·요청당 약 102K 토큰) |

> 상세·근거는 개인 메모리 `feedback_token_economy`. 이 표는 프로젝트 개발 시 매 작업의 사전 체크리스트로 참조한다.
