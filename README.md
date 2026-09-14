# Quant Trading System

한국투자증권(KIS) OpenAPI에 연결해 시세 수신·전략 판단·리스크 검증·주문을 하나의 파이프라인으로 처리하는 개인 자동매매 시스템입니다. C++로 실매매 엔진을, Python으로 백테스트·리서치·운영 보조를 담당합니다.

> **현재 단계.** 전략을 여러 가지 시험하며 찾는 중이라, 모의계좌로 매매하며 표본을 모으고 있습니다. 지금 실증 중인 전략은 **DeviationScale**(일봉 눌림 존 + 3분봉 이격도 분할매매)과 **ITB**(1분 버킷 채널 돌파) 둘이고, 매일의 발주·체결·거부를 하루 1파일 일지로 남깁니다. 전략 수익성은 아직 실증 표본이 부족하며, 아래 백테스트 결론은 **구성 간 상대비교용**입니다(생존편향 주의, [research](research/) 참조).

---

## 아키텍처

```
      KIS OpenAPI (WebSocket 체결·호가·체결통보  ·  REST 봉·현재가·잔고·주문)
                                   │
   ┌───────────────────────────────┴──────────────────────────────────┐
   │              C++ Engine — 5 스레드 + 전략 샤드 M개 (락-프리)          │
   │                                                                   │
   │  [WS 수신 i] ─┐                                                    │
   │  [데이터]   ──┴─▶ 링 행렬 (수신 N × 샤드 M, 종목 해시로 열 선택)          │
   │  REST 시드·폴백·대조         │                                        │
   │                      [샤드 m] 자기 열의 전략만 방문 → 신호 봉투         │
   │                              │  MpscQueue                          │
   │                      [디스패치] 순번·교체·강제청산 스로틀 → OrderSignal │
   │                              │  SPSC                               │
   │                      [주문]  OrderGate 검증 → OrderRouter → KIS 발주 │
   │                                                                   │
   │  [체결]  WS 체결통보 → 원장 반영·CSV (수신 스레드는 push만)            │
   │  [제어]  잔고 대조·손익 갱신·토큰 선갱신·시세 끊김 판정→재연결/폴백     │
   └───────────────────────────────────────────────────────────────────┘
```

- **시세 원천** — 주 시세는 WebSocket 체결 틱이고, 전략 스레드가 틱을 1분봉으로 모아 판단 봉(예: 3분봉)으로 다시 묶습니다. REST 봉은 시드와 폴백입니다. 소켓은 여럿일 수 있고 각 소켓의 수신 스레드가 행렬의 자기 행에 바로 넣습니다.
- **큐** — 생산자가 하나면 SPSC `RingBuffer`, 여럿이면 `MpscQueue`. 순서 보장 단위는 종목이라 종목 해시로 샤드를 고르고, 전략은 자기 종목의 열 하나가 소유합니다. hot path에는 문자열이 없습니다 — 종목은 기동 시 정수 id로 바꿔 두고 틱·호가·디스패치는 id로만 비교합니다.
- **리스크·주문은 단일 시퀀서** — `OrderGate`와 원장은 샤딩하지 않습니다. 신호마다 순번을 찍어 원장 CSV 전 행에 남기므로 나중에 어떤 신호가 어떤 체결이 됐는지 따라갈 수 있습니다.
- **시세 끊김** — `feed::Supervisor`가 장 외 무시·재연결 백오프·연속 실패 시 REST 폴백을 판정하고, 제어 스레드는 멈춘 소켓만 다시 잇습니다. 폴백도 안 될 때만 kill switch가 켜집니다.
- **국면(Regime) 축 둘** — `RegimeController`는 장 시작 지수 국면(BULL/NEUTRAL/BEAR)으로 **전략 집합만 고릅니다**(장중 전환은 2회 연속 확인). 보유 전량 시장가 청산(`FORCE_LIQ`)과 신규매수 정지(entry halt)는 다른 축, 매크로 보조 프로세스가 쓰는 `regime.json`이 냅니다.
- **실계좌 없이 도는 경로** — 캡처한 틱 파일 리플레이, 모의 체결기 `PaperExecutor`, KIS를 링크하지 않는 단위 테스트. 엔진 분해 결과(`DataPoller`·`SignalDispatcher`·`OrderPacer`·`LedgerReconciler`·`RegimeFileBridge`)가 각각 테스트를 가집니다.

설계 목표는 KIS 41종목 하나의 소켓이 아니라 **전 시장 실시간 피드(2,500+종목)를 받을 수 있는 구조**이고, 지금 구조는 그 1×1 특수 케이스입니다. 8원칙과 단계는 [docs/DECISIONS.md](docs/DECISIONS.md) D-071, 스레드·큐·타입 요약은 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 코드 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md).

---

## 리스크 게이트 (OrderGate)

주문은 전부 `send_order` 직전에 게이트를 통과한 신호만 실행됩니다. 한 번의 실수로 손실이 커지지 않게 하는 층이고, 거부 사유는 18가지입니다. 주요 항목:

| 검증 | 동작 |
|---|---|
| Kill switch | 전방향 하드스톱 — BUY·SELL 모두 차단 |
| Entry halt | 신규 진입(BUY)만 정지, 보유분 청산(SELL)은 통과 — 지수 급락 국면용 |
| 손익 갱신 끊김 보수정지 | 당일 손익 계산이 멈추면 신규 매수 정지 |
| 종목당 보유·명목 한도 | 실체결 + 미체결 선점 합산으로 종목당 수량·금액 제한. 한도를 넘는 분할매수는 거부 대신 한도 안으로 줄인다 |
| 총노출 한도 | 자본 × 배수를 넘는 신규 매수 거부 |
| 일일 손실 한도 | 당일 손실이 한도 초과 시 **신규 매수만** 거부(강제청산 아님) |
| 슬롯·교체 | 보유 슬롯이 차면 점수가 더 높은 종목만 최약체를 팔고 들어오며, 교체 직후 쿨다운 |
| Rate limit | 초당·분당 주문 수 상한(KIS 한도 준수) |
| 중복 신호 | 동일 전략+종목 1초 내 중복 거부 |
| Fat-finger 백스톱 | 1주문 최대 수량·최대 명목 초과 시 거부(시장가 주문은 `ref_price`로 명목 평가 — 시장가의 백스톱 우회 차단, `FORCE_LIQ` 매도는 평단을 stamp) |

청산(SELL)은 게이트가 막지 않습니다 — 막으면 KIS가 주문을 통째로 거부해 한 주도 못 빠져나온 일이 있었습니다.

관련: [OrderGate.h](Quant/include/risk/OrderGate.h) · [OrderGate.cpp](Quant/src/risk/OrderGate.cpp) · 단위 테스트 [test_order_gate.cpp](Quant/tests/test_order_gate.cpp)

---

## 전략

C++ 엔진 전략 10종(`Quant/include/strategy/`), Python 백테스트 전략 6종(`PYQuant/strategy/`). 라이브에 붙어 있는 것은 DeviationScale과 ITB 둘이고, 나머지는 백테스트나 모의 시험 단계입니다. 전략 추가는 `StrategyBase` 상속 → `on_start`에서 종목 id를 받아 두기 → `main.cpp` 등록, 절차는 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md) "전략 추가하기".

---

## 두 개의 문 — 리서치와 실증

이 저장소의 깊은 내용은 두 허브로 갈립니다. 목적에 맞는 문으로 들어가세요.

### 📊 리서치·백테스트 → [research/README.md](research/README.md)
전략 채택을 수익률이 아니라 게이트(최대낙폭(MDD)·위험조정)로 판정하고, 무엇이 진짜 엣지고 무엇이 착시(생존편향·과적합·소표본)인지 저널로 걸러냅니다. 종목레벨(계열 A)에서 견고한 레버는 **국면필터(regime) 하나**, 지표 튜닝은 전기간 재검증에서 채택 게이트 통과 0개입니다. 지수레벨 위기대응(계열 B, 위기 17건)은 매수 후 보유 대비 결정적 우위가 없었고, 종가 기준이라 **당일 급락 몸통은 못 막고 꼬리만** 자릅니다. 상세는 허브에서 이어집니다.

### 🧾 전략·실증(매매일지) → [strategies/README.md](strategies/README.md)
분봉 단위처럼 과거 시점정합 재현이 불가한 전략은 **모의계좌 forward 실증**이 유일한 검증 경로입니다. 매일의 존 판정·발주·체결·거부를 `strategies/<전략>/live/YYYY-MM-DD.md` 한 파일로 남겨 백테스트 가정이 실제 체결과 어디서 어긋나는지 대조합니다. 전략 현황·실증 폴더는 허브 표에서 진입하세요.

> 초과수익(α) 절대값은 신뢰하지 말고 구성 간 상대비교로만 읽으십시오(유니버스 생존편향) — 전 항목 공통 주의.

---

## 운영 자동화

장중 매매는 사람이 창을 여는 대신 감시견 스크립트가 맡습니다. `scripts/auto_trade_day.ps1`이 보조 프로세스(매크로 국면·유니버스 스캔)·대시보드·트레이더를 순서대로 띄우고 장 마감까지 트레이더가 죽으면 다시 띄우며, 마감 뒤 `scripts/eod_autodoc.py`가 그날 일지의 사실 구간(손익·세션·종목별 사유)과 대시보드를 채웁니다. Windows 예약작업이 이 감시견을 5분마다 확인합니다.

운영 중 손으로 개입할 때는 MFC 운영단말(`Quant/tools/ops_terminal`, 포지션 표·수동 매매·kill switch)이나 콘솔 `ops_client`를 씁니다. 예약작업·훅·마감 파이프라인 전체 목록은 [docs/AUTOMATION.md](docs/AUTOMATION.md), 단말은 [docs/guides/MFC_TERMINAL.md](docs/guides/MFC_TERMINAL.md).

---

## 구조

```
Quant/          C++ 실매매 엔진 (Engine · OrderGate · OrderRouter · KIS 클라이언트 · 전략 · tools/ops_terminal)
PYQuant/        Python 백테스트·리서치·대시보드 생성기
research/       백테스트 저널 · 목록 · 하락장 이벤트 스터디 · 대시보드 산출물
strategies/     전략별 스펙(SPEC) · 실증 일지 · 백테스트 결과
scripts/        운영 보조 스크립트 (감시견 · 마감 자동 문서 · 문서 게이트)
docs/           설계 결정(DECISIONS) · 아키텍처 · 코드 흐름 · 자동화 · 용어 사전
tools/          존 판정 점검 등 독립 도구
```

세부 디렉토리·기술스택·Docker 운영은 [docs/guides/PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md)에 정리돼 있습니다. 코드·로그에 나오는 약어(`DevScale`·`ITB`·`dev_buy`·`reconcile` 등)의 뜻은 [용어 사전 GLOSSARY.md](docs/GLOSSARY.md)에서 찾을 수 있습니다.

---

## 빌드 · 실행

**Windows (VS2022 / Ninja, 프리셋)**
```bash
cmake --preset x64-release
cmake --build out/build/x64-release
ctest --preset x64-release                       # 단위 테스트 34개
./out/build/x64-release/quant_trader Quant/config/config.json   # 반드시 저장소 루트에서
```

**Linux** (`g++-14`·`libcurl4-openssl-dev` 필요)
```bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -B Quant/build -S Quant
cmake --build Quant/build
./Quant/build/quant_trader Quant/config/config.json
```

실행 모드는 `config.json`의 `"mode"`로 정하고 두 번째 인자로 덮어쓸 수 있습니다 — `FEED`(WebSocket 시세만 표시, 주문 없음) / `TRADE`(전략 엔진 + 실주문, 평일 09:00–15:30 KST) / `KR_TEST`·`US_TEST`(관찰용). 모의계좌는 `"is_paper": true`. 인증정보가 담긴 `config.json`은 저장소에 포함되지 않습니다(`Quant/config/config.json.example` 참고).

---

## 문서 지도

| 알고 싶은 것 | 문서 |
|---|---|
| 왜 이렇게 바꿨나(버린 대안 포함) | [docs/DECISIONS.md](docs/DECISIONS.md) |
| 스레드·큐·타입·국면·KIS 클라이언트 요약 | [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md) |
| 코드를 읽는 순서 | [docs/CODE_FLOW.md](docs/CODE_FLOW.md) |
| 빌드 상세·Docker·Linux | [docs/guides/PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md) |
| 예약작업·감시견·훅 | [docs/AUTOMATION.md](docs/AUTOMATION.md) |
| Claude Code 하네스(에이전트·스킬) | [docs/HARNESS.md](docs/HARNESS.md) |
| 용어 | [docs/GLOSSARY.md](docs/GLOSSARY.md) |

---

## 기술 스택

C++23(MSVC 14.44 / GCC 14, D-070) · CMake/Ninja · WinHTTP(Windows)/libcurl(Linux) · nlohmann/json · 자체 SPSC RingBuffer·MPSC 큐 · MFC 운영단말(Windows 선택) · ZeroMQ(선택) · Python 3.11 · TimescaleDB(선택) · Docker
