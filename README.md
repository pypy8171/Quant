# Quant Trading System

한국투자증권(KIS) OpenAPI로 시세를 받아 전략 판단, 리스크 검증, 주문을 처리하는 개인 자동매매 프로젝트입니다. 매매 엔진은 C++, 백테스트·리서치·운영 도구는 Python입니다.

## 현재 상태

- 엔진은 계속 고치고 있습니다. 구조를 바꾼 이유는 [docs/DECISIONS.md](docs/DECISIONS.md)에 남깁니다.
- 지금 도는 전략은 DeviationScale 하나입니다. 모의계좌에서 돌리고, 9월 하순부터 실계좌에서도 소액으로 돌립니다. 매수·매도 선점을 나누는 수정 동안 하루 멈췄다가(2026-09-25) 9월 26일에 다시 켰습니다.
- 표본이 적어 수익성은 아직 판단하지 않습니다.
- 파이썬이 하던 일을 엔진 안으로 옮기고 있습니다. 종목 목록·시세 수집과 체결 시세 DB 적재는 옮겼고, 국면 판정은 엔진 쪽 결과를 파이썬 쪽과 나란히 비교하고 있습니다. 구조의 기준은 지금 규모(종목 수십 개·계좌 하나)가 아니라 전 시장 실시간 시세와 여러 계좌 대행 매매입니다(D-071·D-128).

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
KIS OpenAPI  WebSocket: 시세·체결통보 / REST: 봉·잔고·주문
네이버       전 종목 목록·현재가·거래대금
    │
C++ 엔진 quant_trader — 기본은 한 프로세스, --role feed|strategy|order 로 셋
    ┌ 시세 ───────────┐   ┌ 전략 ─────────────┐   ┌ 주문 ─────────────────┐
    │ 수신 ×N         │ → │ 샤드 ×M (전략)    │ → │ OrderGate → KIS 주문  │
    │ 체결통보 디코드 │ ─────────────────────────→ │ 체결 반영 → 원장      │
    │ DB 적재 워커 ×K │   │ 시세판·유니버스   │ ← │ 원장 스냅숏           │
    │ (설정으로 켬)   │   │ 국면 판정         │   │ 원장 저널 파일        │
    └─────────────────┘   └───────────────────┘   └───────────────────────┘
    │                       │ ZMQ PUB                        │
    │ COPY → ticks          PYQuant/main.py record           PYQuant/tools/ledger_recorder.py
    │                       → signals·health (·ticks)        → ledger_events·orders·fills·positions
    └───────────────────────┴───────────────┬────────────────┘
                                        TimescaleDB
```

- 한 프로세스로 띄우면 수신·샤드·주문·체결 스레드가 락 없는 큐로 넘깁니다. 전략은 등록 순서대로 샤드에 돌아가며 배정되고, 한 종목의 체결은 그 종목을 보는 샤드마다 들어온 순서대로 갑니다(D-110).
- `--role`로 셋으로 띄우면 주문 프로세스가 공유 메모리(`Quant/include/ipc/SharedLayout.h`)를 만들고 나머지가 붙습니다. 통로는 시세, 체결통보, 주문 요청·응답, 제어, 박동, 국면, 원장 스냅숏입니다. 전략 쪽이 죽어도 주문·원장이 남게 하려는 분리입니다(D-114).
- 체결통보는 시세 쪽 소켓으로 들어옵니다. 시세 쪽은 풀어서 넘기기만 하고 원장은 주문 쪽이 고칩니다. 전략 쪽은 원장 스냅숏으로 보유·매도 가능 수량을 읽습니다.
- 유니버스: 전략 쪽 시세판 스레드(`MarketBoard`)가 네이버에서 5초마다 현재가·거래대금을 받고, 1분마다 거래대금 상위 100종목을 다시 고릅니다. 그 뒤 KIS 일봉으로 걸러 점수를 매깁니다(`Quant/src/universe/`). `market_board`를 끄면 전처럼 `PYQuant/tools/universe_feed.py`가 쓴 파일을 읽습니다.
- 국면 판정: 매매는 `Quant/config/regime.json`을 읽어 신규 진입 정지와 전략 선택에 씁니다. 이 파일은 전략 프로세스 안 스레드(`Quant/src/regime/RegimeFeed.cpp`)가 3분마다 씁니다. 이력은 `logs/regime_history.jsonl`, 장초 기준점은 `logs/regime_open_ref.json`에 남깁니다. 경로·주기·정지선·청산선은 설정의 `regime_feed` 항목입니다(D-147). 엔진 없이 한 번만 판정하려면 `Quant/build_win/regime_feed_once.exe`를 씁니다.
- DB 적재는 세 갈래입니다.
  - 체결 시세: 엔진의 `Quant/src/ipc/DbManager.cpp`가 넣습니다(D-148). 수신 스레드는 체결 한 건을 큐에 넣고 바로 돌아옵니다. 적재 워커 K개(기본 2)가 종목 id로 나눠 받아 `ticks` 표에 COPY로 넣습니다(5,000행 또는 200ms마다). 큐가 차면 버리고 세고, DB가 끊기면 1초부터 30초까지 간격을 늘리며 다시 붙습니다. 100만 행 측정에서 워커 2개가 초당 약 37만 행을 넣었습니다(파이썬 기록기는 약 10만 행). `database.enabled`, libpq 빌드, 환경변수 `TSDB_PASSWORD`가 모두 있어야 켜집니다. 운영 설정은 켜 두었고, 그 설정이면 감시견이 파이썬 기록기를 `--record-ticks` 없이 띄워 같은 체결이 두 번 들어가지 않게 합니다. 끄면 전처럼 파이썬 기록기가 ZMQ로 받아 넣습니다.
  - 신호·상태: `PYQuant/main.py record`가 ZMQ로 받아 `signals`·`health`에 넣습니다.
  - 원장: 주문 프로세스가 KIS에 주문을 보내기 전에 원장 저널 파일(`ledger_YYYYMMDD.bin`)에 먼저 적습니다. `PYQuant/tools/ledger_recorder.py`가 그 파일을 2초마다 따라 읽어 `ledger_events`·`orders`·`fills`·`positions`에 넣습니다. DB가 내려가 있어도 매매는 계속됩니다(D-113).
- 주문·전략 스레드는 DB를 부르지 않습니다.
- 코드는 역할별 파일로 나눴습니다. `Quant/src/core/Engine.cpp`는 약 4,900줄에서 960줄이 됐고, 스레드마다 `Engine*Thread.cpp`, 시세 입력은 `EngineFeed.cpp`, 제어 요청은 `ControlPlane`으로 뗐습니다(D-135). `OrderGate`에서는 보유·선점·평단을 `PositionLedger`로, 진입 우선순위를 `EntryPriority`로 뗐습니다.
- 스레드·큐 상세는 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md).

## 리스크 게이트

새 주문은 발주 직전에 `OrderGate::check()`를 지납니다. 전략 신호, 운영단말 수동 주문, 보호 주문, 강제청산이 모두 같은 입구(`OrderRouter::submit`)로 들어옵니다. 판정은 거부 코드(`GateReject`)로 돌려주고, 로그 문장은 따로 만듭니다. 취소·정정과 기동 때 남은 주문 정리는 게이트를 거치지 않습니다.

<!-- gen:ordergate-rejects -->
`Quant/include/risk/OrderGate.h`의 `GateReject` 거부 코드: `19`개
<!-- /gen:ordergate-rejects -->

- 매수·매도 모두: 전체 주문 정지, 방향·수량 오류, 거래 시간, 한 주문 수량 상한, 중복 주문, 주문 속도(초·분).
- 매수만: 신규 진입 정지, 한 주문 금액 상한, 종목당 수량·금액, 총노출, 일일 손실, 손익 갱신 지연, 보유 슬롯·진입 우선순위·교체 대기.

손실·노출 한도는 매도에 걸지 않습니다. 보유분을 못 파는 쪽이 더 위험하다고 봐서입니다. 코드 [Quant/src/risk/OrderGate.cpp](Quant/src/risk/OrderGate.cpp), 테스트 [Quant/tests/test_order_gate.cpp](Quant/tests/test_order_gate.cpp).

## 운영

감시견 `scripts/auto_trade_day.ps1`이 트레이더와 보조 프로세스를 띄웁니다. 보조 프로세스는 `PYQuant/main.py record`, `PYQuant/tools/ledger_recorder.py`, 대시보드 서버, 체결 알림입니다. 트레이더가 죽으면 5초 뒤 다시 띄우고, 30분 안에 세 번 죽으면 멈춥니다. 실계좌는 `scripts/auto_trade_live.ps1`이 설정을 확인한 뒤 같은 감시견을 부릅니다. 장이 끝나면 `scripts/market_close_autodoc.py`가 매매일지와 대시보드를 채웁니다. 수동 개입은 MFC 운영단말(`Quant/tools/ops_terminal`, 엔진과 TCP로 연결)로 합니다. 상세는 [docs/AUTOMATION.md](docs/AUTOMATION.md).

## 저장소 구조

```
Quant/       C++ 엔진(core·ipc·risk·strategy·universe·regime), 테스트, 운영단말
PYQuant/     Python 백테스트, ZMQ 구독·원장 DB 적재, 국면 판정, 대시보드 생성
research/    백테스트 스터디
strategies/  전략 스펙과 매매일지
scripts/     운영 스크립트(감시견·마감 정리·배포)
docs/        설계 결정, 아키텍처, 자동화, 용어
```

## 빌드와 실행

```bash
# Windows (VS2022, Ninja, vcpkg)
cmake --preset x64-release && cmake --build out/build/x64-release
ctest --preset x64-release

# Linux (g++-14, libcurl4-openssl-dev, libssl-dev)
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -B Quant/build -S Quant && cmake --build Quant/build
```

ZeroMQ와 libpq는 있으면 붙습니다. 없으면 ZMQ 발행과 엔진 DB 적재가 빠진 채로 빌드됩니다.

저장소 루트에서 `quant_trader <config.json>`으로 실행합니다. 실행하면 주문을 냅니다(실행 모드는 TRADE 하나, D-130). 모의계좌는 `"is_paper": true`. 설정 뼈대는 `Quant/config/config.json.example`, 실행 절차는 [docs/RUNBOOK.md](docs/RUNBOOK.md).

## 기술 스택

C++23(MSVC, GCC 14), CMake·Ninja, WinHTTP·libcurl, nlohmann/json, ZeroMQ, libpq, MFC, Python 3.11, TimescaleDB, Docker
