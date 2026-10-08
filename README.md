# Quant Trading System

한국투자증권(KIS) OpenAPI로 시세를 받아 전략 판단, 리스크 검증, 주문을 처리하는 개인 자동매매 프로젝트입니다. 매매 엔진은 C++, 백테스트·리서치·운영 도구는 Python입니다.

## 현재 상태

- 엔진은 계속 고치고 있습니다.
- 지금 도는 전략은 DeviationScale 하나입니다. 모의계좌에서 돌리고, 9월 하순부터 실계좌에서도 소액으로 돌립니다. 매수·매도 선점을 나누는 수정 동안 하루 멈췄다가(2026-09-25) 9월 26일에 다시 켰습니다.
- 표본이 적어 수익성은 아직 판단하지 않습니다.

## 결과

| 내용 | 링크 |
|---|---|
| 엔진 부하 시험 | [부하 시험](https://claude.ai/artifact/Ahgj1CbDYc7aPfkujiQRQR#load) |
| 모의·실계좌 매매 결과 | [매매 결과](https://claude.ai/artifact/Ahgj1CbDYc7aPfkujiQRQR#journal) |
| 백테스트 | [백테스트](https://claude.ai/artifact/Ahgj1CbDYc7aPfkujiQRQR#backtest) |

## 흐름

```
[수신]        [전략]            [주문]                    [뒷정리]
WS 수신 ×소켓 → 샤드 ×M → 디스패치 → 주문 스레드 1 → 전송 ×4 → KIS
  │ 체결통보                              ↑ 게이트는 여기 하나
  └──────────────→ 체결 스레드 → 포지션 장부 → 장부 사본(100ms) → 전략이 읽음
```

- 엔진(`quant_trader`)은 한 프로세스이고, 스레드 사이는 락 없는 큐로 넘깁니다.
- 수신 스레드는 소켓을 읽어 큐에 넣기만 하고, 전략은 종목 id로 나눈 샤드들이 나눠 계산합니다.
- 주문은 스레드 하나가 게이트·수량 예약·일지 기록을 합니다. KIS 왕복만 전송 스레드 4개가 맡습니다(D-151).
- 체결통보는 체결 스레드가 포지션 장부에 반영합니다. 포지션 장부는 엔진 내부 장부(메모리)이고, 공식 기록은 증권사 계좌 원장입니다. 재기동 복구에는 일지(`ledger_YYYYMMDD.bin`)를 씁니다.

## 리스크 게이트

새 주문은 발주 직전에 `OrderGate::check()`를 지납니다. 전략 신호, 운영단말 수동 주문, 보호 주문, 강제청산이 모두 같은 입구(`OrderRouter::submit`)로 들어옵니다. 판정은 거부 코드(`GateReject`)로 돌려주고, 로그 문장은 따로 만듭니다. 취소·정정과 기동 때 남은 주문 정리는 게이트를 거치지 않습니다.

## 운영

감시견 `scripts/auto_trade_day.ps1`이 트레이더와 보조 프로세스를 띄웁니다. 보조 프로세스는 `PYQuant/tools/ledger_recorder.py`, 대시보드 서버, 체결 알림입니다. 체결 시세·신호·헬스는 엔진이 DB에 직접 넣습니다(D-154). 트레이더가 죽으면 5초 뒤 다시 띄우고, 30분 안에 세 번 죽으면 멈춥니다. 실계좌는 `scripts/auto_trade_live.ps1`이 설정을 확인한 뒤 같은 감시견을 부릅니다. 장이 끝나면 `scripts/market_close_autodoc.py`가 매매일지와 대시보드를 채웁니다. 수동 개입은 MFC 운영단말(`Quant/tools/ops_terminal`, 엔진과 TCP로 연결)로 합니다.

## 저장소 구조

```
Quant/       C++ 엔진(core·ipc·risk·strategy·universe·regime), 테스트, 운영단말
PYQuant/     Python 백테스트, ZMQ 구독·장부 일지 DB 적재, 국면 판정, 대시보드 생성
research/    백테스트 스터디
strategies/  전략 스펙과 매매일지
scripts/     운영 스크립트(감시견·마감 정리·배포)
docs/        설계 결정, 아키텍처, 자동화, 용어
```
