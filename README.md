# Quant Trading System

한국투자증권(KIS) OpenAPI에 연결해 시세 수신·전략 판단·리스크 검증·주문을 하나의 파이프라인으로 처리하는 개인 자동매매 시스템입니다. C++로 실매매 엔진을, Python으로 백테스트·리서치를 담당합니다.

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
- **국면(Regime) 자동전환** — `RegimeController`가 장 시작 지수 국면(BULL/NEUTRAL/BEAR)을 판정해 전략 집합을 자동 선택하고, 약세장에서 `FORCE_LIQ`로 강제청산한다(config `regime_strategies`). 매크로 사이드카(`regime.json`)는 별도로 신규매수만 막는 entry halt를 토글한다.
- 확장 구성(ZMQ IPC · TimescaleDB 적재 · Python 오퍼레이터)은 [PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md) 참조.

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
| Fat-finger 백스톱 | 1주문 최대 수량·최대 명목 초과 시 거부(시장가 주문은 `ref_price`로 명목 평가 — 시장가의 백스톱 우회 차단, `FORCE_LIQ` 매도는 평단을 stamp) |

관련: [OrderGate.h](Quant/include/risk/OrderGate.h) · [OrderGate.cpp](Quant/src/risk/OrderGate.cpp) · 단위 테스트 [test_order_gate.cpp](Quant/tests/test_order_gate.cpp)

---

## 두 개의 문 — 리서치와 실증

이 저장소의 깊은 내용은 두 허브로 갈립니다. 목적에 맞는 문으로 들어가세요.

### 📊 리서치·백테스트 → [research/README.md](research/README.md)
전략 채택을 수익률이 아니라 게이트(MDD·위험조정)로 판정하고, 무엇이 진짜 엣지고 무엇이 착시(생존편향·과적합·소표본)인지 저널로 걸러냅니다. 지금까지 견고한 레버는 **국면필터(regime) 하나**, 지표 튜닝은 전기간 재검증에서 채택 게이트 통과 0개입니다. 상세 결론·카탈로그·위기대응 계열은 허브에서 이어집니다.

### 🧾 전략·실증(매매일지) → [strategies/README.md](strategies/README.md)
분봉 단위처럼 과거 시점정합 재현이 불가한 전략은 **모의계좌 forward 실증**이 유일한 검증 경로입니다. 매일의 존 판정·발주·체결·거부를 하루 1파일로 남겨 백테스트 가정이 실제 체결과 어디서 어긋나는지 대조합니다. 전략 현황·실증 폴더는 허브 표에서 진입하세요.

> α 절대값은 신뢰하지 말고 구성 간 상대비교로만 읽으십시오(유니버스 생존편향) — 전 항목 공통 주의.

---

## 구조

```
Quant/          C++ 실매매 엔진 (Engine · OrderGate · OrderRouter · KIS 클라이언트 · 전략)
PYQuant/        Python 백테스트·리서치·라이브 트레이더
research/       백테스트 저널 · 카탈로그 · 하락장 이벤트 스터디
strategies/     전략별 스펙(SPEC) · 실증 로그 · 백테스트 결과
scripts/        운영 보조 스크립트
```

세부 디렉토리·기술스택·Docker 운영은 [PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md)에 정리돼 있습니다. 코드·로그에 나오는 약어(`DevScale`·`ITB`·`dev_buy`·`reconcile` 등)의 뜻은 [용어 사전 GLOSSARY.md](docs/GLOSSARY.md)에서 찾을 수 있습니다.

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

실행 모드는 `config.json`의 `"mode"`로 제어합니다 — `FEED`(시세 검증, 주문 없음) / `TRADE`(전략 엔진 + 실주문). 인증정보가 담긴 `config.json`은 저장소에 포함되지 않습니다(`Quant/config/config.json.example` 참고).

---

## 기술 스택

C++17 · CMake/Ninja · WinHTTP(Windows)/libcurl(Linux) · nlohmann/json · 자체 SPSC RingBuffer · ZeroMQ(선택) · Python 3.11 · TimescaleDB(선택) · Docker
