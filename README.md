# Quant Trading System

한국투자증권(KIS) OpenAPI로 시세를 받아 전략 판단, 리스크 검증, 주문을 처리하는 개인 자동매매 프로젝트입니다. 매매 엔진은 C++, 백테스트·리서치·운영 도구는 Python입니다.

## 현재 상태

- 엔진은 계속 고치고 있습니다. 구조를 바꾼 이유는 [docs/DECISIONS.md](docs/DECISIONS.md)에 남깁니다.
- 지금 도는 전략은 DeviationScale 하나입니다. 모의계좌에서 돌리고, 9월 하순부터 실계좌에서도 소액으로 돌립니다. 매수·매도 선점을 나누는 수정 동안 하루 멈췄다가(2026-09-25) 9월 26일에 다시 켰습니다.
- 표본이 적어 수익성은 아직 판단하지 않습니다.
- 최근에는 엔진을 시세·전략·주문 세 프로세스로 나눌 수 있게 하고, 큰 파일을 역할별로 쪼갰습니다. 파이썬이 하던 종목 목록·시세 수집과 국면 판정, 체결 DB 적재도 엔진 안으로 옮기는 중입니다. 구조의 기준은 지금 규모(종목 수십 개·계좌 하나)가 아니라 전 시장 실시간 시세와 여러 계좌 대행 매매입니다(D-071·D-128).

## 결과

<!-- gen:readme-results -->
| 내용 | 위치 |
|---|---|
| 백테스트 스터디와 모의계좌 매매 결과 | [대시보드](https://claude.ai/artifact/CVr332PFqRQbkoadjCfShP) |
| 스터디별 코드와 표 | [research/studies/](research/studies/), 색인 [research/README.md](research/README.md) |
| 날짜별 매매일지 | [strategies/DeviationScale/live/](strategies/DeviationScale/live/) |
| 엔진 부하 시험 | [부하 시험 결과](https://claude.ai/artifact/73wNFxyr7xK6hR7ZxcLN5i) |
<!-- /gen:readme-results -->

## 흐름

```
KIS OpenAPI (WebSocket 시세·체결통보, REST 봉·잔고·주문), 네이버 (전 종목 목록·현재가·거래대금)
    │
C++ 엔진 (quant_trader) — 한 프로세스, 또는 --role 로 셋
    ┌ 시세 프로세스 ──┐   ┌ 전략 프로세스 ─────┐   ┌ 주문 프로세스 ─────────┐
    │ 수신 ×N         │ → │ 유니버스 스캔      │   │                        │
    │ (소켓 보유)     │   │ 샤드 ×M·전략       │ → │ OrderGate 검증 → 주문  │
    │ 체결통보 디코드 │ → │                    │ ← │ 체결 반영 → 원장       │
    │ (DB 적재, 꺼짐) │   │ 국면 판정(병행)    │   │ → 원장 스냅숏          │
    └─────────────────┘   └────────────────────┘   └────────────────────────┘
         └──────── 공유 메모리 (시세·체결·요청·응답·제어·박동·원장 스냅숏) ────┘
    │                                        │
    │ ZMQ PUB (역할마다 포트 하나)           │ 원장 저널 파일
Python SUB (PYQuant/main.py record)     Python 적재기 (PYQuant/tools/ledger_recorder.py)
    └──────────────┬────────────────────┘
               TimescaleDB
```

- 기본은 한 프로세스에서 수신·전략·주문 스레드를 나눠 돌립니다. 전략은 등록 순서대로 샤드 스레드에 하나씩 배정되고, 한 종목의 체결은 그 종목을 보는 샤드마다 한 줄로 들어가 순서가 지켜집니다(D-110).
- `--role feed`·`--role strategy`·`--role order`로 띄우면 세 프로세스가 공유 메모리(`Quant/include/ipc/SharedLayout.h`)로 이어집니다. 전략 쪽이 죽어도 주문·원장은 남게 하려는 분리이고, 모의계좌와 부하 시험에서 확인하고 있습니다(D-114). 한 프로세스일 때도 같은 통로 코드를 쓰고 메모리 자리만 다릅니다.
- 체결통보는 시세 프로세스의 소켓으로 들어오므로, 시세 쪽은 풀어서 공유 메모리에 넣기만 하고 원장은 주문 프로세스가 고칩니다. 전략 쪽은 주문 프로세스가 내놓는 원장 스냅숏으로 보유·매도 가능 수량을 읽습니다.
- 유니버스는 전략 프로세스의 `MarketBoard` 스레드가 네이버에서 전 종목 목록과 현재가·거래대금을 받아(5초마다) 만들고, KIS 일봉으로 걸러 점수를 매깁니다(`Quant/src/universe/`). 전에는 파이썬(`PYQuant/tools/universe_feed.py`, data.go.kr 목록)이 파일로 넘겼고, 지금 그 경로는 `market_board`를 끈 경우에만 씁니다.
- 국면 판정도 엔진 안 스레드(`Quant/src/regime/RegimeFeed.cpp`)로 옮겼습니다. 다만 아직은 파이썬 판정과 결과를 나란히 남겨 비교하는 단계이고, 매매 판단은 파이썬이 쓰는 `regime.json`을 읽습니다.
- 체결 시세를 엔진이 TimescaleDB에 바로 넣는 경로(`Quant/src/ipc/DbManager.cpp`, `COPY`)도 만들었지만 설정의 `database` 블록이 있을 때만 켜지고, 지금 운영에서는 꺼 두고 파이썬 기록기가 적재합니다.
- 원장은 주문을 보내기 전에 파일에 먼저 적고, 적재기가 그 파일을 따라 읽어 DB에 넣습니다. DB가 내려가 있어도 매매는 멈추지 않습니다(D-113).
- 코드는 역할별 파일로 나눴습니다. `Quant/src/core/Engine.cpp`(약 4,900줄 → 900줄)에서 스레드마다 `Engine*Thread.cpp`, 시세 입력은 `EngineFeed.cpp`, 제어 요청은 `ControlPlane`으로 뗐고(D-135), `OrderGate`에서 보유·선점·평단을 `PositionLedger`로, 진입 우선순위를 `EntryPriority`로 뗐습니다. 유니버스 스캔도 단계별 파일(`Quant/src/universe/`)로 나눴습니다.
- 스레드·큐 상세는 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md).

## 리스크 게이트

모든 주문은 발주 직전에 `OrderGate::check()`를 지납니다. 판정은 거부 코드(`GateReject`)로 돌려주고, 로그 문장은 따로 만듭니다.

<!-- gen:ordergate-rejects -->
`Quant/include/risk/OrderGate.h`의 `GateReject` 거부 코드: `19`개
<!-- /gen:ordergate-rejects -->

전체 주문 정지, 신규매수 정지, 거래 시간, 한 주문 수량·금액 상한, 종목당·총노출·일일 손실 한도, 손익 갱신 지연, 보유 슬롯과 진입 우선순위·교체 대기, 주문 속도(초·분)·중복을 봅니다. 매도는 막지 않습니다 — 막았다가 보유분을 못 판 일이 있었습니다. 코드 [Quant/src/risk/OrderGate.cpp](Quant/src/risk/OrderGate.cpp), 테스트 [Quant/tests/test_order_gate.cpp](Quant/tests/test_order_gate.cpp).

## 운영

감시견 `scripts/auto_trade_day.ps1`(모의)·`scripts/auto_trade_live.ps1`(실계좌)이 보조 프로세스와 트레이더를 띄우고, 트레이더가 죽으면 다시 띄웁니다. 장이 끝나면 `scripts/market_close_autodoc.py`가 매매일지와 대시보드를 채웁니다. 수동 개입은 MFC 운영단말(`Quant/tools/ops_terminal`)로 합니다. 상세는 [docs/AUTOMATION.md](docs/AUTOMATION.md).

## 저장소 구조

```
Quant/       C++ 엔진(core·ipc·risk·strategy·universe), 전략, 운영단말
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

저장소 루트에서 `quant_trader <config.json>`으로 실행합니다. 실행하면 주문을 냅니다(실행 모드는 TRADE 하나, D-130). 모의계좌는 `"is_paper": true`. 설정 뼈대는 `Quant/config/config.json.example`, 실행 절차는 [docs/RUNBOOK.md](docs/RUNBOOK.md).

## 기술 스택

C++23(MSVC, GCC 14), CMake·Ninja, WinHTTP·libcurl, nlohmann/json, ZeroMQ, MFC, Python 3.11, TimescaleDB, Docker
