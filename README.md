# Quant Trading System

한국투자증권(KIS) OpenAPI로 시세를 받아 전략 판단, 리스크 검증, 주문까지 한 프로세스 안에서 처리하는 개인 자동매매 시스템입니다. 실매매 엔진은 C++로, 백테스트와 리서치, 운영 보조 도구는 Python으로 썼습니다.

## 결과 보기

저장소를 받지 않아도 아래에서 결과를 볼 수 있습니다.

| 보고 싶은 것 | 위치 |
|---|---|
| 백테스트 스터디 22건의 질문, 방법, 데이터, 결과, 판정. 모의계좌 매매의 날짜별 주문·체결·실현손익 | [퀀트 백테스트 스터디](https://claude.ai/artifact/LuFGpPCgBhrNgDVNc3obrq) |
| 스터디별 코드와 숫자 표 | [research/studies/](research/studies/), 색인은 [research/README.md](research/README.md) |
| 날짜별 매매일지(무엇을 왜 사고팔았고 무엇이 거부됐는지) | [strategies/DeviationScale/live/](strategies/DeviationScale/live/), 전략 색인은 [strategies/README.md](strategies/README.md) |
| 설계를 바꾼 이유와 버린 대안 | [docs/DECISIONS.md](docs/DECISIONS.md) |

## 지금 어디까지 왔나

모의계좌로 매매하며 표본을 모으는 단계입니다. 기본 설정에서 도는 전략은 DeviationScale(일봉에서 눌림 구간을 고르고 3분봉 이격도로 나눠 사는 전략) 하나이고, ITB(1분 단위 채널 돌파)는 별도 모의 설정으로 관찰하고 있습니다.

수익성은 아직 판단하지 않습니다. 실증 표본이 적고, 백테스트 결론은 유니버스 생존편향 때문에 구성끼리 비교하는 용도로만 읽습니다. 매일의 발주·체결·거부는 하루 한 파일의 일지로 남기고, 장이 끝나면 스크립트가 손익과 비용, 종목별 사유를 채웁니다.

## 엔진 구성

```
      KIS OpenAPI (WebSocket: 체결·호가·체결통보   REST: 봉·현재가·잔고·주문)
                                   │
   ┌───────────────────────────────┴──────────────────────────────────┐
   │              C++ 엔진 (스레드 5개 + 전략 스레드 M개)                  │
   │                                                                   │
   │  [수신 i] ────┐                                                    │
   │  [데이터] ────┴─▶ 링 버퍼 행렬 (수신 N × 전략 M, 종목 해시로 열 선택)     │
   │  REST 초기값·대체 경로·대조   │                                       │
   │                      [전략 m]  자기 열의 전략만 실행 → 신호            │
   │                              │  다중 생산자 큐                        │
   │                      [디스패치] 순번 부여·종목 교체·강제청산 속도 조절    │
   │                              │  단일 생산자 큐                        │
   │                      [주문]   OrderGate 검증 → OrderRouter → KIS 발주 │
   │                                                                   │
   │  [체결]   WebSocket 체결통보 → 원장 반영, CSV 기록                    │
   │  [제어]   잔고 대조, 손익 갱신, 토큰 갱신, 시세 끊김 판정과 재연결       │
   └───────────────────────────────────────────────────────────────────┘
```

주 시세는 WebSocket 체결 틱입니다. 전략 스레드가 틱을 1분봉으로 모으고 다시 판단용 봉(예: 3분봉)으로 묶습니다. REST 봉은 기동 시 초기값과 소켓이 끊겼을 때의 대체 경로로만 씁니다.

순서를 지켜야 하는 단위는 종목입니다. 종목 코드를 해시해 전략 스레드를 고르므로 한 종목의 틱은 항상 같은 스레드가 같은 순서로 처리합니다. 큐는 생산자가 하나면 단일 생산자 링 버퍼(`RingBuffer`), 여럿이면 다중 생산자 큐(`MpscQueue`)를 씁니다. 틱을 처리하는 경로에는 문자열이 없습니다. 종목은 기동 시 정수 id로 바꿔 두고 배열 인덱스로만 다룹니다.

리스크 검증과 원장은 한 스레드가 순서대로 처리하고 나누지 않습니다. 신호마다 순번을 찍어 원장 CSV까지 남기므로 어느 신호가 어느 체결이 됐는지 나중에 따라갈 수 있습니다.

시세가 끊기면 `feed::Supervisor`가 장 외 시간 무시, 재연결 대기 시간 증가, 연속 실패 시 REST 대체 경로 전환을 판정합니다. 대체 경로마저 안 될 때만 전체 주문 정지(kill switch)를 켭니다. 시장 국면은 보조 프로세스가 쓰는 파일 `regime.json` 하나로 들어옵니다. 라벨(RISK_ON, NEUTRAL, RISK_OFF)이 그 국면에서 돌릴 전략 집합을 고르고, 점수가 매수 비율, 신규매수 정지, 보유분 전량 청산을 정합니다.

실계좌 없이도 돌릴 수 있습니다. 캡처한 틱 파일 리플레이, 모의 체결기 `PaperExecutor`, KIS를 링크하지 않는 단위 테스트 35개가 있습니다.

지금 구조는 KIS 소켓 하나에 41종목이지만, 목표는 전 시장 실시간 피드(2,500종목 이상)를 같은 구조로 받는 것입니다. 원칙과 단계는 [docs/DECISIONS.md](docs/DECISIONS.md)의 D-071, 스레드·큐·타입 설명은 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 코드를 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md)에 있습니다.

## 리스크 게이트

모든 주문은 발주 직전에 `OrderGate::check()`를 지납니다. 한 번의 실수로 손실이 커지지 않게 하는 층입니다. 거부 지점 수는 아래 한 줄을 스크립트가 코드에서 세어 넣습니다.

<!-- gen:ordergate-rejects -->
`Quant/src/risk/OrderGate.cpp`의 `reject_reason =` 지점: `19`개
<!-- /gen:ordergate-rejects -->

| 검증 | 동작 |
|---|---|
| 전체 주문 정지 | 매수와 매도를 모두 막는다 |
| 신규매수 정지 | 매수만 막고 보유분 매도는 통과시킨다. 지수가 급락한 국면에 쓴다 |
| 거래 시간 | 정규장 09:00~15:30과 애프터마켓 16:00~20:00(KST) 밖의 신규 주문을 거부한다. 모의계좌는 15:30까지 |
| 종목당 보유·금액 한도 | 체결분과 미체결 주문을 합산한다. 한도를 넘는 분할매수는 거부하지 않고 한도 안으로 줄인다 |
| 총노출·일일 손실 한도 | 자본의 배수를 넘거나 당일 손실이 한도를 넘으면 신규 매수만 거부한다 |
| 슬롯과 교체 | 보유 슬롯이 차면 점수가 더 높은 종목만 가장 약한 종목을 팔고 들어온다. 교체 직후에는 잠시 쉰다 |
| 주문 속도와 중복 | 초당·분당 주문 수 상한(KIS 한도)을 지키고, 같은 전략·종목의 1초 안 중복 신호를 거부한다 |
| 한 주문 상한 | 최대 수량과 최대 금액을 넘는 주문을 거부한다. 시장가도 참조가로 금액을 계산한다 |

매도는 게이트가 막지 않습니다. 막았더니 KIS가 주문을 통째로 거부해 한 주도 빠져나오지 못한 일이 있었습니다. 코드는 [OrderGate.h](Quant/include/risk/OrderGate.h)와 [OrderGate.cpp](Quant/src/risk/OrderGate.cpp), 테스트는 [test_order_gate.cpp](Quant/tests/test_order_gate.cpp)입니다.

## 전략

C++ 엔진 전략 10종은 `Quant/include/strategy/`에, Python 백테스트 전략 6종은 `PYQuant/strategy/`에 있습니다. 새 전략은 `StrategyBase`를 상속하고, `on_start`에서 종목 id를 받아 두고, `Quant/src/strategy/StrategyFactory.cpp`에 등록하면 됩니다. 절차는 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md)의 "전략 추가하기"에 있습니다.

## 백테스트와 실증

백테스트([research/](research/))에서는 전략 채택을 수익률이 아니라 최대낙폭과 위험조정 수익 같은 기준으로 판정하고, 스터디마다 무엇이 진짜 우위이고 무엇이 착시(생존편향, 과적합, 표본 부족)였는지 적습니다. 지금까지 종목 단위에서 견고했던 것은 시장 국면 필터 하나였습니다. 지표 튜닝은 전 기간 재검증에서 채택 기준을 통과한 것이 없었고, 지수 단위의 위기 대응(위기 17건)은 사서 들고 있는 것보다 낫지 않았습니다.

분봉 전략은 과거 시점을 그대로 재현할 수 없어서 모의계좌로 앞으로 돌려 보는 실증이 유일한 검증 경로입니다. 매일의 판정·발주·체결·거부를 `strategies/<전략>/live/YYYY-MM-DD.md`에 남기고, 백테스트의 가정이 실제 체결과 어디서 어긋나는지 대조합니다.

## 운영 자동화

장중 매매는 감시견 스크립트 `scripts/auto_trade_day.ps1`이 맡습니다. 보조 프로세스(시장 국면, 유니버스 스캔, 전 종목 시세 파일, 체결 알림, 체결 기록기), 대시보드, 트레이더를 순서대로 띄우고 장이 끝날 때까지 트레이더가 죽으면 다시 띄웁니다. Windows 예약작업이 이 감시견을 5분마다 확인합니다. 장이 끝나면 `scripts/market_close_autodoc.py`가 그날 일지의 손익, 비용, 종목별 사유와 대시보드를 채웁니다.

사람이 개입할 때는 MFC 운영단말(`Quant/tools/ops_terminal`: 포지션 표, 수동 매매, 전체 주문 정지)이나 콘솔 `ops_client`를 씁니다. 예약작업과 훅, 마감 절차는 [docs/AUTOMATION.md](docs/AUTOMATION.md), 단말은 [docs/guides/MFC_TERMINAL.md](docs/guides/MFC_TERMINAL.md)에 있습니다.

## 저장소 구조

```
Quant/          C++ 실매매 엔진 (Engine, OrderGate, OrderRouter, KIS 클라이언트, 전략, tools/ops_terminal)
PYQuant/        Python 백테스트, 리서치, 대시보드 생성기
research/       백테스트 스터디와 색인, 하락장 이벤트 스터디
strategies/     전략별 스펙(SPEC), 실증 일지, 백테스트 결과
scripts/        운영 스크립트 (감시견, 마감 자동 문서, 문서 검사)
docs/           설계 결정, 아키텍처, 코드 흐름, 자동화, 용어 사전
tools/          존 판정 점검 같은 독립 도구
```

대시보드 HTML은 생성물이라 저장소에 두지 않습니다. `PYQuant/dashboard/build_dashboard.py`가 만들어 위 링크로 발행합니다. 코드와 로그에 나오는 약어는 [docs/GLOSSARY.md](docs/GLOSSARY.md)에서 찾을 수 있습니다.

## 빌드와 실행

Windows(VS2022, Ninja):

```bash
cmake --preset x64-release && cmake --build out/build/x64-release
ctest --preset x64-release
```

Linux(`g++-14`, `libcurl4-openssl-dev`):

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -B Quant/build -S Quant && cmake --build Quant/build
```

실행은 저장소 루트에서 `quant_trader <config.json>`입니다. 평소에는 감시견이 실행하고, 감시견은 `Quant/build_win/` 트리의 실행 파일을 씁니다([docs/RUNBOOK.md](docs/RUNBOOK.md)). 실행 모드는 config의 `"mode"`로 정합니다. `FEED`는 시세만 표시하고 주문하지 않으며, `TRADE`는 전략 엔진이 실제 주문을 냅니다. `KR_TEST`와 `US_TEST`는 관찰용입니다. 모의계좌는 `"is_paper": true`입니다. 인증 정보가 든 config는 저장소에 없고, 뼈대는 `Quant/config/config.json.example`입니다.

## 문서 지도

| 알고 싶은 것 | 문서 |
|---|---|
| 왜 이렇게 바꿨나, 버린 대안은 무엇인가 | [docs/DECISIONS.md](docs/DECISIONS.md) |
| 스레드, 큐, 타입, 국면, KIS 클라이언트 | [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md) |
| 코드를 읽는 순서 | [docs/CODE_FLOW.md](docs/CODE_FLOW.md) |
| 빌드 상세, Docker, Linux | [docs/guides/PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md) |
| 예약작업, 감시견, 훅 | [docs/AUTOMATION.md](docs/AUTOMATION.md) |
| Claude Code 하네스(에이전트, 스킬) | [docs/HARNESS.md](docs/HARNESS.md) |
| 용어 | [docs/GLOSSARY.md](docs/GLOSSARY.md) |

## 기술 스택

C++23(MSVC 14.44, GCC 14), CMake와 Ninja, WinHTTP(Windows)와 libcurl(Linux), nlohmann/json, 자체 SPSC 링 버퍼와 MPSC 큐, MFC 운영단말(Windows), ZeroMQ(설치돼 있으면 자동 사용), Python 3.11, TimescaleDB, Docker
