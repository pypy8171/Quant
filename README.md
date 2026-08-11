# Quant Trading System

한국투자증권(KIS) OpenAPI에 연결해 **시세 수신 → 전략 판단 → 리스크 검증 → 주문**을 하나의 파이프라인으로 처리하는 개인 자동매매 시스템입니다. C++로 실매매 엔진을, Python으로 백테스트·리서치를 담당합니다.

> **현재 단계.** 전략을 여러 가지 시험하며 찾는 중이라, 모의계좌로 매매하며 표본을 모으고 있습니다. 실행 파이프라인·체결통보 처리·리스크 게이트를 검증하는 단계이며, 전략 수익성은 아직 실증 표본이 부족합니다. 아래 백테스트 수치는 **구성 간 상대비교용**이며 절대 성과가 아닙니다(생존편향 주의, [research](research/) 참조).

---

## 아키텍처

```
        KIS OpenAPI (REST 시세·주문  ·  WebSocket 체결·호가)
                              │
   ┌──────────────────────────┴───────────────────────────┐
   │                    C++ Engine (락-프리 파이프라인)      │
   │                                                       │
   │  [Data]  ──ring──▶  [Strategy]  ──ring──▶  [Order]     │
   │  REST 폴링          등록 전략 순회         OrderRouter  │
   │  OHLCV 봉           → OrderSignal          → OrderGate  │
   │                                            → KIS 주문   │
   │  [WebSocket] 실시간 체결/호가 → Strategy                │
   │  [Control]   WS stale 감지 → kill switch               │
   └───────────────────────────────────────────────────────┘
```

- **RingBuffer** — 명시적 메모리 순서를 쓰는 SPSC 락-프리 큐(스레드 간 배압).
- **OrderGate** — 주문이 나가기 전 통과해야 하는 위험 검증 게이트(아래).
- 확장 구성(ZMQ IPC · TimescaleDB 적재 · Python 오퍼레이터)은 [PROJECT_GUIDE.md](PROJECT_GUIDE.md) 참조.

---

## 리스크 게이트 (OrderGate)

주문은 전부 `send_order` 직전에 게이트를 통과한 신호만 실행됩니다. 한 번의 실수로 손실이 커지지 않게 하는 층입니다.

| 검증 | 동작 |
|---|---|
| Kill switch | 전방향 하드스톱 — BUY·SELL 모두 차단 |
| Entry halt | 신규 진입(BUY)만 정지, 보유분 청산(SELL)은 통과 — 지수 급락 국면용 |
| 종목당 보유 한도 | 실체결 + 미체결 선점 합산으로 종목당 최대 수량 제한 |
| 일일 손실 한도 | 당일 손실이 한도 초과 시 **신규 매수만** 거부(강제청산 아님) |
| Rate limit | 초당·분당 주문 수 상한(KIS 한도 준수) |
| 중복 신호 | 동일 전략+종목 1초 내 중복 거부 |
| Fat-finger 백스톱 | 1주문 최대 수량·최대 명목 초과 시 거부 |

관련: [OrderGate.h](Quant/include/risk/OrderGate.h) · [OrderGate.cpp](Quant/src/risk/OrderGate.cpp) · 단위 테스트 [test_order_gate.cpp](Quant/tests/test_order_gate.cpp)

---

## 리서치 · 백테스트

전략 채택은 수익률이 아니라 **게이트(MDD·위험조정)** 로 판정합니다. 무엇이 진짜 엣지고 무엇이 착시(생존편향·과적합·소표본)인지 걸러내는 과정을 저널로 남깁니다.

- 지금까지 검증된 견고한 레버는 **국면필터(regime)** 하나 — 약세장에서 노출을 걷어 하방을 막는 효과.
- 지표 튜닝(추세필터·절대모멘텀·변동성조절)은 전기간 재검증에서 **채택 게이트 통과 0개**.
- 카탈로그: [research/BACKTESTS.md](research/BACKTESTS.md) · 실행 저널: [research/BACKTEST_LOG.md](research/BACKTEST_LOG.md)

> 유니버스가 "오늘 살아남은 종목" 위주라 벤치마크가 부풀려집니다. α 절대값은 신뢰하지 말고 구성 간 상대비교로만 읽으십시오 — 전 항목 공통 주의.

---

## 구조

```
Quant/          C++ 실매매 엔진 (Engine · OrderGate · OrderRouter · KIS 클라이언트 · 전략)
PYQuant/        Python 백테스트·리서치·라이브 트레이더
research/       백테스트 저널 · 카탈로그 · 하락장 이벤트 스터디
strategies/     전략별 스펙(SPEC) · 실증 로그 · 백테스트 결과
scripts/        운영 보조 스크립트
```

세부 디렉토리·기술스택·Docker 운영은 [PROJECT_GUIDE.md](PROJECT_GUIDE.md)에 정리돼 있습니다.

---

## 빌드 · 실행

**Windows (Ninja)**
```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B Quant/build_win -S Quant
cmake --build Quant/build_win
./Quant/build_win/quant_trader Quant/config/config.json
```

**Linux** (`libcurl4-openssl-dev` 필요)
```bash
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant
cmake --build Quant/build
./Quant/build/quant_trader Quant/config/config.json
```

실행 모드는 `config.json`의 `"mode"`로 제어합니다 — `FEED`(시세 검증, 주문 없음) / `TRADE`(전략 엔진 + 실주문). 인증정보가 담긴 `config.json`은 저장소에 포함되지 않습니다(`config.json.example` 참고).

---

## 기술 스택

C++17 · CMake/Ninja · WinHTTP(Windows)/libcurl(Linux) · nlohmann/json · 자체 SPSC RingBuffer · ZeroMQ(선택) · Python 3.11 · TimescaleDB(선택) · Docker
