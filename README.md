# Quant Trading System

한국투자증권(KIS) OpenAPI로 시세를 받아 전략 판단, 리스크 검증, 주문을 처리하는 개인 자동매매 프로젝트입니다. 매매 엔진은 C++, 백테스트·리서치·운영 도구는 Python입니다.

## 현재 상태

- 엔진은 계속 고치고 있습니다. 구조를 바꾼 이유는 [docs/DECISIONS.md](docs/DECISIONS.md)에 남깁니다.
- 모의계좌에서 돌려 본 전략은 세 개(DeviationScale, ITB, TRENDX)이고, 지금 기본 설정에서 도는 것은 DeviationScale 하나입니다. 실계좌에서 전략으로 매매한 적은 아직 없습니다.
- 표본이 적어 수익성은 아직 판단하지 않습니다.

## 결과

| 내용 | 위치 |
|---|---|
| 백테스트 스터디와 모의계좌 매매 결과 | [대시보드](https://claude.ai/artifact/CVr332PFqRQbkoadjCfShP) |
| 스터디별 코드와 표 | [research/studies/](research/studies/), 색인 [research/README.md](research/README.md) |
| 날짜별 매매일지 | [strategies/DeviationScale/live/](strategies/DeviationScale/live/) |
| 엔진 부하 시험 | [부하 시험 결과](https://claude.ai/artifact/2HR69XdJRKU4puWt3aoMz7) |

## 흐름

```
KIS OpenAPI (WebSocket 시세·체결통보, REST 봉·잔고·주문)
    │
C++ 엔진 (quant_trader)
    수신 → 전략 → OrderGate 검증 → 주문 → 원장
    │                                   │
    │ ZMQ PUB (TRADE·SIGNAL·ORDER·FILL·HEALTH)
    │                                   │ 원장 저널 파일
Python SUB (PYQuant/main.py record)     Python 적재기 (PYQuant/tools/ledger_recorder.py)
    └──────────────┬────────────────────┘
               TimescaleDB
```

- 엔진은 한 프로세스에서 수신·전략·주문 스레드를 나눠 돌리고, 종목 id 해시로 전략 스레드를 골라 한 종목의 순서를 지킵니다. 전략과 주문을 두 프로세스로 나누는 작업을 모의계좌에서 시험하고 있습니다(D-114).
- 원장은 주문을 보내기 전에 파일에 먼저 적고, 적재기가 그 파일을 따라 읽어 DB에 넣습니다. DB가 내려가 있어도 매매는 멈추지 않습니다(D-113).
- 스레드·큐 상세는 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md).

## 리스크 게이트

모든 주문은 발주 직전에 `OrderGate::check()`를 지납니다.

<!-- gen:ordergate-rejects -->
`Quant/src/risk/OrderGate.cpp`의 `reject_reason =` 지점: `19`개
<!-- /gen:ordergate-rejects -->

전체 주문 정지, 신규매수 정지, 거래 시간, 종목당·총노출·일일 손실 한도, 보유 슬롯, 주문 속도·중복, 한 주문 상한을 봅니다. 매도는 막지 않습니다 — 막았다가 보유분을 못 판 일이 있었습니다. 코드 [Quant/src/risk/OrderGate.cpp](Quant/src/risk/OrderGate.cpp), 테스트 [Quant/tests/test_order_gate.cpp](Quant/tests/test_order_gate.cpp).

## 운영

감시견 `scripts/auto_trade_day.ps1`이 보조 프로세스와 트레이더를 띄우고, 트레이더가 죽으면 다시 띄웁니다. 장이 끝나면 `scripts/market_close_autodoc.py`가 매매일지와 대시보드를 채웁니다. 수동 개입은 MFC 운영단말(`Quant/tools/ops_terminal`)로 합니다. 상세는 [docs/AUTOMATION.md](docs/AUTOMATION.md).

## 저장소 구조

```
Quant/       C++ 엔진, 전략, 운영단말
PYQuant/     Python 백테스트, ZMQ 구독·DB 적재, 대시보드 생성
research/    백테스트 스터디
strategies/  전략 스펙과 매매일지
scripts/     운영 스크립트
docs/        설계 결정, 아키텍처, 자동화, 용어
```

## 빌드와 실행

```bash
# Windows (VS2022, Ninja)
cmake --preset x64-release && cmake --build out/build/x64-release
ctest --preset x64-release

# Linux (g++-14, libcurl4-openssl-dev)
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -B Quant/build -S Quant && cmake --build Quant/build
```

저장소 루트에서 `quant_trader <config.json>`으로 실행합니다. `"mode"`가 `FEED`면 시세만 받고, `TRADE`면 주문을 냅니다. 모의계좌는 `"is_paper": true`. 설정 뼈대는 `Quant/config/config.json.example`, 실행 절차는 [docs/RUNBOOK.md](docs/RUNBOOK.md).

## 기술 스택

C++23(MSVC, GCC 14), CMake·Ninja, WinHTTP·libcurl, nlohmann/json, ZeroMQ, MFC, Python 3.11, TimescaleDB, Docker
