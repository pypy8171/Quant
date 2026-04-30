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

`MarketData`(OHLCV + bar_index), `OrderSignal`(side/type/qty/price + strategy_id), `Position`, `OrderBook`(5단계 호가, 채널 `H0STASP0`), `TradeData`(실시간 체결, 채널 `H0STCNT0`).

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

싱글톤 `Logger`가 밀리초 단위 UTC 타임스탬프로 콘솔과 `quant_trader.log`에 기록합니다. 사용 매크로: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`.

## 플랫폼 참고사항

- Windows 빌드 플래그: `/utf-8`, `-D_WIN32_WINNT=0x0A00`(Windows 10+), `-D_CRT_SECURE_NO_WARNINGS`. FEED 화면 출력에는 ANSI 이스케이프 시퀀스와 `SetConsoleOutputCP(CP_UTF8)`를 사용합니다.
- Linux 디버그 빌드는 AddressSanitizer(`-fsanitize=address`)를 활성화합니다.
- `#ifdef _WIN32` 가드로 HTTP 및 WebSocket 플랫폼 코드를 분리합니다. 네트워크 기능 추가 시 이 패턴을 유지하세요.
