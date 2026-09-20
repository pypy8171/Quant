# strategies/ — 전략별 스펙·실증 기록

전략마다 **확정 스펙**과 **실증 결과(라이브/백테스트)**를 한 폴더에 모은다.
매매 추이를 전략별로 누적 기록하는 용도이며 git으로 추적한다.

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
| **ITB** (IntradayBreakout v2, 1분 버킷 채널 돌파 + 당일 시가 기준점) | [ITB/SPEC.md](ITB/SPEC.md) | [ITB/live/](ITB/live/) (모의계좌 forward 관찰 중) | forward 전용 → [BACKTEST_FLOW 트랙 B](../research/BACKTEST_FLOW.md) |
| **TargetBasket** (가치 기울임 스터디 22·23 + 모멘텀 02·03 두 슬리브, 파이썬이 08:40 비중표를 쓰고 엔진이 14:40 집행, D-109) | `docs/DECISIONS.md` D-109 | 2026-09-21 첫 집행 예정 | 백테스트는 스터디 22·23·02·03, forward는 아래 검증 가정 V1~V5 |

- **TargetBasket 검증 가정(forward 10일):** 보는 것은 수익이 아니라 실행 충실도다. 판정은 `scripts/check_runtime_health.py`의 "바스켓" 다섯 행이 매일 자동으로 낸다(매매일지 4절).
  - V1 파일 — 08:40 작성기가 쓴 `Quant/config/basket_targets.json`의 `as_of`가 그날이고 엔진이 그것을 읽는다(행 "바스켓 파일 당일").
  - V2 청산관리 격리 — 바스켓 종목에 ITB 청산관리가 붙지 않는다(행 "바스켓 청산관리 부착", 기대 0).
  - V3 소유권 — 바스켓 종목을 다른 전략(교체·15:15 마감 청산·한도 초과분 정리)이 팔지 않는다(행 "바스켓 타전략 매도", 기대 0).
  - V4 재기동 — 하루 안에 같은 종목·같은 방향 주문이 두 번 나가지 않는다(행 "바스켓 재기동 중복", 기대 0).
  - V5 시각 — 매도 레그 → 매수 레그가 15:05 안에 끝나고 15:00 창 종료로 이월된 건이 없다(행 "바스켓 매수 레그 시각").
- **실증 규칙:** 하루 = 라이브 1파일(`live/YYYY-MM-DD.md`). 같은 날은 같은 파일에 갱신, 날짜가 바뀌면 새 파일. `/trade-log`가 `quant_trader.log`에서 재생성한다. 추측·과장 금지, 로그로 검증 가능한 수치만 — 체결통보가 남은 건만 원 단위 실현손익 확정.

## 경계(다른 폴더)

- **`research/`** — 리서치 프로세스(백테스트 저널 `BACKTEST_LOG.md`, 흐름도 `BACKTEST_FLOW.md`, 협의체 `RESEARCH_COUNCIL.md`, `runs/`). 전략별 결과가 아니라 "무엇을 왜 돌렸나"의 과정 기록.
- **루트(gitignore, 비공개)** — `STRATEGIES.md`(코드화 전략 목록 단일 소스), `REGIME_CONTROLLER_SPEC.md`, `DEV_GUIDE_STRATEGY_A.md`, `DECISIONS.md`, `DAILY_LOG.md`. 개인용이라 커밋 제외.
