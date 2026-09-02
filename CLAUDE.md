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

- **`"FEED"`** — KIS WebSocket에 연결하여 실시간 호가·체결 데이터를 1초마다 콘솔에 표시합니다. 주문 없이 연결 상태와 인증 정보를 검증할 때 사용합니다.
- **`"TRADE"`** — 3-스레드 전략 엔진을 실행하고 장 중(평일 09:00–15:30 KST)에 실제 주문을 냅니다.

`config.json`에는 현재 **실거래 인증 정보**(`app_key`, `app_secret`, 실계좌 번호)가 저장되어 있습니다. 모의투자 엔드포인트(`openapivts.koreainvestment.com:29443`)로 전환하려면 `"is_paper": true`로 설정하세요.

## 아키텍처

### 스레드 모델

엔진은 락-프리 파이프라인으로 세 개의 스레드를 실행합니다:

```
[데이터 스레드]  →  market_queue_ (RingBuffer)  →  [전략 스레드]  →  order_queue_ (RingBuffer)  →  [주문 스레드]
  KIS REST                                          등록된 전략들                                    KIS 주문 API
  OHLCV 봉                                          → OrderSignal                                   send_order()
```

- `RingBuffer<T>`는 명시적 메모리 순서를 가진 `std::atomic`을 사용하는 SPSC(단일 생산자/단일 소비자) 락-프리 큐입니다.
- 데이터 스레드는 `fetch_interval_sec`초마다 KIS REST를 폴링하며, 장 외 시간에는 건너뜁니다.
- 전략 스레드는 등록된 전략 전체를 순회하며, `NONE`이 아닌 신호는 주문 큐에 push합니다.

### 핵심 타입 (`Quant/include/core/Types.h`)

`MarketData`(OHLCV + bar_index), `OrderSignal`(side/type/qty/price/**ref_price** + strategy_id), `Position`, `OrderBook`(5단계 호가, 채널 `H0STASP0`), `TradeData`(실시간 체결, 채널 `H0STCNT0`), `Regime`(enum: BULL/NEUTRAL/BEAR/UNKNOWN), `RegimeSnapshot`(장 시작 국면 판정 결과 — score·200MA·정배열/역배열·지수 이평 분해).

> `OrderSignal.ref_price`는 시장가(price=0) 주문의 명목 한도 평가 기준가다. 지정가는 `price`로 명목을 재지만 시장가는 `price`가 0이라 이 값이 없으면 명목 백스톱이 우회된다(특히 급락장 강제청산의 시장가 전량매도). 발주 측이 직전 현재가/평단을 stamp한다.

### 국면(Regime) 대응

`RegimeController`(`Quant/include/core/RegimeController.h`)가 장 시작 1회 지수 종가>200MA(±1)와 정배열/역배열(ma20·ma60·ma120, ±1)로 `score∈{-2..+2}`를 매겨 BULL/NEUTRAL/BEAR/UNKNOWN을 판정한다. config `"regime_strategies": {"BULL":[id…],"NEUTRAL":[…],"BEAR":[…]}`를 주면 국면이 전략 집합을 자동 선택하고(재평가 주기 `regime_reeval_sec`, 기본 300초), 지정하지 않으면 전략별 `active_regimes` 방식으로 하위호환한다. BEAR 등에서는 보유 전량을 시장가로 청산하는 `FORCE_LIQ` 신호를 낸다. 이와 별개로 매크로 사이드카(`macro_regime_feed.py`)가 쓰는 `regime.json` 파일브리지가 `OrderGate::set_entry_halt`(신규매수만 차단, 청산은 통과)를 토글한다(config `regime_file`·`regime_stale_sec`).

### 전략 추가하기

1. `StrategyBase`(`Quant/include/strategy/StrategyBase.h`)를 상속합니다.
2. `id()`, `on_data(const MarketData&)`, `describe()`를 구현합니다.
3. `main.cpp`에서 `engine.add_strategy(std::make_unique<YourStrategy>(...))` 로 등록합니다.
4. 필요하면 `"strategies"` 아래에 설정 항목을 추가하고 `main.cpp`의 전략 로딩 블록에서 파싱합니다.

### KIS API 클라이언트 (`Quant/src/api/KisClient.cpp`)

플랫폼별 분기: Windows는 WinHTTP, Linux는 libcurl. OAuth2 토큰 발급과 bearer 토큰 캐싱을 처리합니다. 주요 메서드: `authenticate()`, `get_ohlcv()`, `get_current_price()`, `send_order()`.

### WebSocket 클라이언트 (`Quant/include/api/KisWebSocket.h`)

FEED 모드에서 사용합니다. REST로 approval key를 발급받고, `ops.koreainvestment.com:31000`(모의) 또는 `:21000`(실거래)에 연결한 뒤 `H0STASP0`과 `H0STCNT0` 채널을 구독하여 파싱된 구조체를 등록된 콜백으로 전달합니다.

### 로깅

싱글톤 `Logger`가 밀리초 단위 UTC 타임스탬프로 콘솔과 `logs/quant_trader.log`(cwd 하위 `logs/` 폴더에 고정, 부모 폴더는 자동 생성)에 기록합니다. 과거 로그는 `logs/archive/`에 보관합니다. 사용 매크로: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`.

**비동기 구조**: 전략·주문 hot path는 레코드를 큐에 push만 하고 즉시 반환하며, 타임스탬프 포맷팅과 파일/콘솔 I/O는 전용 writer 스레드가 담당합니다("저지연은 평균이 아니라 최악(tail latency)을 다루는 문제"라는 설계 의도로 디스크 플러시를 hot path에서 분리). 백프레셔: 큐가 상한(`kMaxQueue`)을 넘으면 가장 오래된 레코드를 드롭하고 드롭 수를 셉니다(운영 중 무한 증가·블로킹 방지). 종료·테스트 직전 정합 확인용 `flush()`를 제공합니다.

### 문서 동기화 (드리프트 방지)

색인·요약·링크가 실제 트리와 어긋나는 것을 막는다. 대표/색인 파일이 무엇을 요약하는지의 의존 표와 커밋 전 체크리스트는 [docs/SYNC_MAP.md](docs/SYNC_MAP.md)에 있다. 문서를 옮기거나 새 스터디·전략을 추가한 뒤에는 다음으로 링크·색인 정합을 검사한다(깨진 내부 링크·색인 미등재를 기계적으로 잡음, `@committer`가 문서 커밋 전 자동 실행):

```bash
python scripts/check_docs.py   # exit 0 = 통과, 1 = 드리프트
```

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
