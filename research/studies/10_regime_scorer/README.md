# BT-10R · 구조 국면 스코어러 애블레이션 (연구군: index-macro)

> 한 줄 요약: C++ `RegimeController`의 국면 판정(v0 이산)을 연속화·기울기·개장 오버레이로 확장한 4변형을, 지수 long/flat 타이밍 필터로 6개 하락장 창에서 결정론 비교한다. **상태: 미판정(판정 보류 — 편향감사·엣지판정 후속).**

이 스터디는 계열 A(종목레벨 모멘텀/레짐)의 **국면 레버를 스코어러로 분해**한 Track A다. 라이브 C++ `RegimeController::compute_score`/`classify`의 순수 로직을 파이썬(`PYQuant/backtest/regime_scorer.py`, 변형 A)으로 1:1 미러하고, 사용자 문제제기(①단조 ②일봉이라 5분 재평가 무의미 ③이산·과보수)를 겨냥해 연속화(B)·기울기(C)·SOX/VIX 개장 오버레이(D)로 확장했다. `PYQuant/tests/test_regime_scorer.py`가 변형 A의 라이브 패리티를 확인한다.

> **Track B(장중 국면 자동전환)는 여기 없다.** 장중 국면의 후보 지표(시가대비 낙폭·장중 실현변동성·추세지속·가속)는 전부 장중 지수 시계열 파생인데, 장중 지수 PIT 히스토리는 KIS·data.go.kr 어디에도 없어 백테스트 불가다. 그래서 `PYQuant/tools/index_intraday_logger.py`로 **forward 적재만** 가능하며, 검증 궤도는 별도다.

---

## 1. 사전등록 (Pre-registration)
- **가설**: 국면 판정을 이산({-2..+2})에서 연속·기울기로 바꾸면 하락장 진입/청산 타이밍이 개선되어, long/flat 프록시에서 Buy&Hold 대비 MDD를 줄이고 drawdown을 매끄럽게 만든다.
- **사전등록 임계**: 자유파라미터는 가중치 w(기본 1.0)와 classify 임계(`bull_th`/`bear_th`)뿐. tanh 스케일 s는 자유파라미터가 아니라 **지수 20일 실현변동성의 배수로 사전고정**(6개 인과중첩 창에 6연속 파라미터를 맞추는 "식별 불가" 회피).
- **데이터 소스**: `PYQuant/data/index_source.py` `IndexSource().get_historical_ohlcv` — `^KS11`(코스피, 구조축) + `^SOX`(반도체, D 오버레이) + `^VIX`(공포, D 오버레이). 캐시=parquet(`.index_cache`), 워밍 후 오프라인 결정론.
- **유니버스**: index-macro(지수·매크로만). 개별종목·수급·survivorship 미반영.
- **홀드아웃 선언**: train = 2022 제외, **holdout = 2022bear 잠금**.

## 2. 방법 (Method)
- **프록시**: 국면 = 전략 로스터 선택의 대리 → 지수 long/flat. BULL/NEUTRAL = 지수 롱, BEAR = 현금(flat).
- **변형**: A `v0_2axis`(C++ v0 그대로: 종가>200MA ±1 + 정배열/역배열 ±1 → {-2..+2} 이산) · B `continuous`(위치·정배열 스프레드를 tanh 연속화) · C `slope`(B + MA200 기울기 축, V반등 bounce vs 진짜추세 구분) · D `overlay`(개장 SOX/VIX로 classify 임계 shift, compute_score와 분리).
- **룩어헤드 차단(루프 구조)**: 결정=종가(D), 집행=다음봉 시가(D+1 open). 스코어는 `closes[:i+1]`만, 수익은 `open[i+1]→open[i+2]`. 스코어러는 절대 미수정(import만).
- **히스테리시스(dwell) 없음**: 일봉 입력은 하루 상수라 dwell이 무의미. 그 관심사는 폴링 입력을 쓰는 Track B의 것.
- **창 날짜 출처**(발명 아님): `research/studies/06_bear_market/raw/<window>_momentum_on.csv`의 equity 시계열.

## 3. 결과 (Primary) — 6창 × 4변형, 결정론 재현
공개 표는 [`summary.tsv`](summary.tsv). 헤드라인 발췌(총수익% / MDD% / Sharpe / 국면전환수, BH=Buy&Hold 지수):

| 창 | 변형 A(v0) | 변형 B(연속) | 변형 C(기울기) | 변형 D(오버레이) | BH(지수) |
|---|---|---|---|---|---|
| 2011euro | -13.1 / 22.1 | -10.5 / 18.8 | -10.3 / 18.8 | -20.3 / 26.1 | -12.0 / 23.4 |
| 2018semi | -13.9 / 18.6 | -4.1 / 9.4 | -4.1 / 9.4 | -8.3 / 12.4 | -17.7 / 23.3 |
| 2020covid | -0.4 / 34.8 | +19.3 / 16.9 | +24.2 / 14.1 | +21.8 / 15.5 | +28.1 / 34.8 |
| **2022bear**(홀드아웃) | -15.9 / 16.1 | -6.2 / 7.6 | -4.0 / 7.0 | -7.7 / 7.7 | -24.3 / 27.8 |
| 2024blackmon | -1.1 / 13.5 | -4.7 / 13.7 | -4.7 / 13.7 | -7.1 / 15.9 | -8.4 / 17.5 |
| 2026now | +51.5 / 39.1 | +51.5 / 39.1 | +51.5 / 39.1 | +51.5 / 39.1 | +51.5 / 39.1 |

## 4. 진단 (Diagnostics — 참고용, 판정 근거 아님)
- **연속화(B/C)가 이산(A)의 과보수/단조 문제를 완화하는 방향**: 2018semi·2020covid·2022bear 홀드아웃에서 B/C가 A보다 MDD를 크게 낮추고 drawdown을 매끄럽게 만든다(문제 ①③에 부합). 특히 A는 2020covid에서 ~87% 노출을 유지해 타이밍을 놓친다.
- **기울기 축(C)**: 2020covid·2022bear에서 B 대비 소폭 추가 개선(V반등 bounce 구분 가설과 정합), 나머지 창은 B와 사실상 동일.
- **개장 오버레이(D)는 이 배선에서 역효과**: 전 창에서 국면전환수(flips)가 급증(예: 2020covid 20회, 2011euro 27회)하고 수익이 나빠진다 — churn 유발, 유효성 미확인.
- **국면 침묵 구간**: 2026now는 BEAR 판정이 없어 전 변형이 100% 노출 = BH와 동일(강세장에서 필터가 조용함 — 의도된 거동).

## 5. 편향 감사 (bias-auditor)
- **미판정.** 이 하네스는 재현 가능한 숫자·로그만 낸다(ablate.py 책임경계). GUARDRAILS 6대 편향 판정은 후속 `@bias-auditor` 몫. 사전 주의점: 6개 창은 **인과적으로 중첩**되어 창별 우열비교는 신호가 아니다(reviewer 식별불가 경고). long/flat은 전략 로스터의 **대리**일 뿐 라이브 로스터가 아니다.

## 6. 결론 (Verdict)
- **미판정(보류).** 연속화(B/C)가 이산(A)보다 하락장 타이밍이 나아 보이나, 승격 판정은 편향감사(`@bias-auditor`) + 엣지·통계 유의성(`@quant-analyst`) + 2022 홀드아웃 독립확인을 거친 뒤에만 내린다. 개장 오버레이(D)는 현 배선에서 채택 후보 아님(churn).

## 7. 재현 정보
- 스크립트: [`ablate.py`](ablate.py)(하네스) · 스코어러 `PYQuant/backtest/regime_scorer.py`(미수정 import) · 패리티 `PYQuant/tests/test_regime_scorer.py`.
- seed=0 · commit=66bdfaf · source=`IndexSource(.index_cache)` · index=`^KS11`/`^SOX`/`^VIX` · warmup_cal_days=450 · holdout=2022bear · exec=decide-on-close(D)/execute-next-open(D+1).
- 재생성: `PYQuant/`에서 `python -m ...` 없이 스터디 폴더에서 `python ablate.py`(최초 1회 캐시 미스 시 네트워크 필요, 이후 오프라인 결정론).

---

← [studies 색인](../README.md) · [research 허브](../../README.md)
