# strategies/ — 전략별 스펙·실증 기록

전략마다 **확정 스펙**과 **실증 결과(라이브/백테스트)**를 한 폴더에 모은다.
매매 추이를 전략별로 누적 증명하는 용도이며 git으로 추적한다.

## 구조

```
strategies/
  README.md              ← 이 파일(전략 목록·현황 인덱스)
  <전략>/
    SPEC.md              ← 확정 스펙(파라미터·안전장치)
    live/
      YYYY-MM-DD.md      ← 라이브 모의/실거래 실증 일지(하루 1파일)
    backtest/            ← (전략에 백테스트가 있으면) 결과·해석
```

실증 규칙(하루 1파일·검증 가능한 수치만)은 아래 **전략 현황** 표 밑에 정리한다.

## 전략 현황

| 전략 | SPEC | 최근 실증 | 검증 경로 |
|------|------|------|------|
| **DeviationScale** (일봉 정배열+눌림 존 게이트 + 3분봉 이격도 분할매매) | — (3분봉 PIT 재현 불가로 의도적 제외) | [DeviationScale/live/](DeviationScale/live/) (모의계좌 forward 실증 중) | forward 전용 → [BACKTEST_FLOW 트랙 B](../research/BACKTEST_FLOW.md) |
| **ITB** (IntradayBreakout v2, 1분 버킷 채널 돌파 + 당일 시가앵커) | [ITB/SPEC.md](ITB/SPEC.md) | [ITB/live/](ITB/live/) (모의계좌 forward 관찰 중) | forward 전용 → [BACKTEST_FLOW 트랙 B](../research/BACKTEST_FLOW.md) |

- **실증 규칙:** 하루 = 라이브 1파일(`live/YYYY-MM-DD.md`). 같은 날은 같은 파일에 갱신, 날짜가 바뀌면 새 파일. `/trade-log`가 `quant_trader.log`에서 재생성한다. 추측·과장 금지, 로그로 검증 가능한 수치만 — 체결통보가 남은 건만 원 단위 실현손익 확정.

## 경계(다른 폴더)

- **`research/`** — 리서치 프로세스(백테스트 저널 `BACKTEST_LOG.md`, 흐름도 `BACKTEST_FLOW.md`, 협의체 `RESEARCH_COUNCIL.md`, `runs/`). 전략별 결과가 아니라 "무엇을 왜 돌렸나"의 과정 기록.
- **루트(gitignore, 비공개)** — `STRATEGIES.md`(코드화 전략 카탈로그 단일 소스), `REGIME_CONTROLLER_SPEC.md`, `DEV_GUIDE_STRATEGY_A.md`, `DECISIONS.md`, `DAILY_LOG.md`. 개인용이라 커밋 제외.
