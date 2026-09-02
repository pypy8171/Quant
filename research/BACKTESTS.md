# 백테스트 카탈로그 (BACKTESTS) — 계열 A

> **계열 A(종목레벨 모멘텀/레짐)** 백테스트를 한 줄씩 카탈로그로 정리한다. 각 행 = _무엇을 물었나 · 발견 · 판정 · 상세 링크_.
> 상세 해석·규칙변경 원문은 [BACKTEST_LOG.md](BACKTEST_LOG.md)(실행 저널), 방향 진화 요약은 [research 허브](README.md).
> 위기대응 계열(07~09)은 성격이 다른 독립 계열이라 여기 넣지 않는다 — 맨 아래 링크 참조.

## 한눈에 보기

| ID | 날짜 | 무엇을 물었나 | 기간·유니버스 | 핵심 발견 | 판정 |
|---|---|---|---|---|---|
| BT-01 | 2026-08-05 | 모멘텀+국면필터가 비용 물고 벤치를 이기나 (1~5년 롤링) | 최근 1~5년 · KOSPI 시총100(PIT) | α 최근 12개월 집중, 3년창 엣지 소멸, **전 구간 MDD −22~−36% 게이트 불합격** | ⚠️ 방향만 |
| BT-02 | 2026-08-05 | 변동성 타게팅으로 MDD를 게이트(≤15%)까지 낮추나 | 최근 1~3년 · 동일 | MDD 8~12%p 개선, 샤프 유지, **1년 −11.6% 첫 통과**. α는 풀노출 벤치 대비 음전 | ✅ 채택 |
| BT-03 | 2026-08-05 | 모멘텀 엣지가 약세장(OOS)에서 살아남나 + regime 순기여 | 2022 약세장 · 동일 | 모멘텀 단독 −35%(벤치 −20% 미달, 모멘텀 크래시). **regime ON=100% 현금으로 재앙 회피** | ✅ regime 확정 |
| BT-04 | 2026-08-07 | 올해 어느 달부터 시작했으면 얼마? + C1 개별추세필터 | 2026 월별 8개 시작 · 동일 | **진입 시점이 지배**(1·2·4월 +30~43% vs 6·7월 −). C1은 강세장 inert. N=10~89 소표본 | ⚠️ 재검증 필요 |
| BT-05 | 2026-08-07 | 지표 4종 중 표본≥200에서 baseline을 견고히 이기나 | 2021~2026 전기간(N≥750) · 동일 | **채택 게이트 통과 0개**. trend200 미세우위·MDD악화, absmom inert, vol_adjust 악화. 유효 레버=regime | ❌ 지표 튜닝 한계 |
| BT-06 | 2026-08-09 | 과거 하락장 유사구간에서 어떤 전략이 유효한가 | 6구간(2011~2026) · KOSPI 대형 | **momentum 우세하나 2018 박스장은 mean_reversion 승**. regime은 순수하락 방어/V반등 독 | 📊 장세 의존 |

> 판정 범례: ✅ 채택 · ⚠️ 방향은 맞으나 미완 · ❌ 기각 · 📊 조건부(장세 의존).
> 공통 전략 규칙(6-1 모멘텀·동일가중·next-bar-open·비용)은 [BACKTEST_LOG의 "매수·매도 판단 근거"](BACKTEST_LOG.md) 참조.

## 상세 링크 (본문 복제 금지 — 원문은 저널/스터디)

| ID | 실행 원문 | 종목별 원장 / 원자료 | export |
|---|---|---|---|
| BT-01 | [BACKTEST_LOG 실행 #1](BACKTEST_LOG.md) | [studies/01_momentum_regime](studies/01_momentum_regime/README.md) | `bt_1y.json`~`bt_5y.json` |
| BT-02 | [BACKTEST_LOG 실행 #2](BACKTEST_LOG.md) | [studies/02_vol_target](studies/02_vol_target/README.md) | `bt2_1y.json`~`bt2_5y.json` |
| BT-03 | [BACKTEST_LOG 실행 #3](BACKTEST_LOG.md) | [studies/03_2022_ablation](studies/03_2022_ablation/README.md) | `bt_2022bear.json`(ON)·`bt_2022bear_noregime.json`(OFF) |
| BT-04 | [BACKTEST_LOG 실행 #4](BACKTEST_LOG.md) | [runs/2026-08-07](runs/2026-08-07_month-start-sweep.md) | `baseline_monthly.json` 외 |
| BT-05 | [BACKTEST_LOG 실행 #5](BACKTEST_LOG.md) | [runs/2026-08-07](runs/2026-08-07_month-start-sweep.md) | `tools/out/fullperiod.json` |
| BT-06 | [studies/06_bear_market](studies/06_bear_market/README.md) | [이벤트별 매매 원장](studies/06_bear_market/events/README.md) | `studies/06_bear_market/summary_*.tsv` |

---

## 데이터 소스 요약 (백테스트 커버리지)
| 소스 | 커버 | 유니버스 | 시점정합(PIT)? | 쓴 항목 |
|---|---|---|---|---|
| datagokr(금융위) | 2020~현재 | 시총상위 스냅샷 | ✅ | BT-01~05, BT-06(2022/2024/2026) |
| yfinance(무키) | 2005~현재 | 정적 대형주 131 | ❌(생존편향) | BT-06(2011/2018/2020) |

> ⚠️ 공통: datagokr PIT 유니버스도 "캐시 보유 ∩ 시총상위"라 완전한 편출·폐지 포함은 아님. 절대 초과수익(α)보다 **구성 간 상대비교**로 읽을 것 — 전 항목 공통 주의.

---

> **위기대응 계열(07~09)** 은 지수레벨·엔진 무관 독립 계열이라 이 카탈로그에 넣지 않는다 → [studies/README — 계열 B](studies/README.md#계열-b--지수레벨-위기대응) 참조.

← [research 허브](README.md)
