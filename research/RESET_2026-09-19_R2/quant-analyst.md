# 리셋 2라운드 — 성과 판정 기준 (quant-analyst, 2026-09-20)

> 읽은 것: `research/COUNCIL_CHARTER.md`, `research/RESET_2026-09-19.md` §1-2·§3·§D, `research/studies/README.md`,
> `research/studies/render_studies.py`, `research/studies/11_signal_axes/metrics.json`, `14_hold_axis`·`15_impulse_pullback`·`17_exit_ev` README,
> `research/studies/13_trendx_gate/stats_util.py`, `PYQuant/backtest/costs.py`·`ledger.py`·`metrics.py`.
> 라이브 원장 `Quant/build_win/logs/trades_20260908.csv`~`trades_20260918.csv`(9거래일)는 이 문서를 쓰며 다시 계산했다
> (임시 스크립트, FIFO 왕복·개시 전략 귀속·일 단위 블록 부트스트랩. 숫자는 §1-2).
> 오너 지시(09-19 밤): "지금 기준이 맞다고 생각하지 않는다. 다 바꿔도 된다." — 그래서 기준을 한 벌로 다시 쓴다.

## 0. 결론 다섯 줄

1. 스터디 17개 중 **"엣지가 있다"는 주장으로 살아남는 것은 0개**. 살아남는 것은 **기각 결론 7개**(09·10·12·13 게이트·14·15·16)와 기술통계 4개(06·07·08·17)뿐이다. 01~03·04·05·11은 표본 구조(겹치는 창·생존편향 유니버스·2022 한 해 재사용)가 결론을 못 버틴다.
2. 라이브 9거래일 2,311왕복은 **어느 부호도 말하지 못한다** — 왕복 단위 t=−1.76이지만 일 단위 t=−0.93, 손실의 92%가 09-14 하루(운영 이월 갭)에서 났다. 부호를 굳히려면 일 단위로 **약 65거래일**, 왕복 단위로 TRENDX 3,200왕복이 필요하다.
3. 채택 표준은 **한 양식**(§2)으로 통일한다: 같은 유니버스 동일가중 매수 후 보유 대비 비용 뒤 초과수익 → 뉴이-웨스트 t → walk-forward 창 과반 → ±1 격자 → BH-FDR q → 일·월 블록 부트스트랩 신뢰구간. 합격·보류·기각 문턱은 숫자로 §2-4, `metrics.json` v2 열 이름은 §2-5.
4. 후보 비교는 같은 표에서 하되, 단독 합격 뒤 **월 초과수익 상관 ≤0.5 ∧ 슬리브 합산 샤프 +0.10 이상**일 때만 더한다(§3).
5. 승격은 백테스트 합격 → 소액 forward 슬리브(종목 ≤50만·일 ≤−10만·총 ≤−100만, 100왕복 또는 60거래일) → 본 슬리브. 강등은 20거래일 굴림 CI 상한 <0, 실현 비용이 사전등록 1.5배, 낙폭이 사전등록 MDD 1.5배 중 하나면 즉시(§4).

---

## 1. 진단

### 1-1. 스터디 17개 — 지금 기준으로 살아남는 결론

판정 기준은 헌장 §3 세 층(① 비용 뒤 초과수익의 부호와 t ② walk-forward 창 과반 ③ 파라미터 ±1)과 "2022 한 해 홀드아웃 재사용 금지"다.
"유효"는 그 결론이 지금 기준으로도 서는 것, "무효"는 결론 자체가 아니라 **표본 구조가 결론을 못 버티는 것**을 뜻한다.

| 스터디 | 주장 | 표본 | 판정 | 왜 |
|---|---|---|---|---|
| 01 모멘텀×국면 | 최근 1년에 알파 집중, 3년 창에서 사라짐 | 창 5개(1~5년, 끝 날짜 같음), 시총 상위 100 스냅샷 | **무효** | 창 5개가 전부 같은 끝점을 공유해 독립 표본이 1개다. 유니버스가 현재 스냅샷(생존편향). t·walk-forward 없음 |
| 02 변동성 타게팅 | MDD 8~12%p 개선·샤프 유지, 채택 | 01과 같은 창 5개 | **조건부 유효(위험 오버레이로만)** | 노출을 줄이면 MDD가 주는 것은 기계적이다. 비교 대상이 풀노출 벤치라 "위험조정 우세"는 아직 안 쟀다. 같은 변동성으로 맞춘 벤치와 walk-forward 재검정 뒤 오버레이로만 |
| 03 2022 제거 | 모멘텀 단독 −35.4% < 벤치 −20.5%, 국면 ON은 현금 | 1년 1점 | **무효(엣지)·유효(사실)** | "모멘텀 크래시가 있다"는 사실은 남고, 국면 필터의 기여는 n=1이라 못 잰다 |
| 04·05 월별 시작시점 스윕 | 요약 JSON만 | 원장 없음 | **무효** | 재현 불가. 결론에 쓰지 않는다 |
| 06 하락장 6구간 | 6구간×4전략 거동 | 사건 6 | **기술통계** | 우열 비교 불가(n=6). 구간 정의는 14·16이 물려받아 "이 구간에서" 라는 단서로만 |
| 07 위기 17건 | speed×shape 그리드 | n≈17, 셀당 1~3 | **기술통계** | 스터디가 스스로 "신호 아님"이라 적었다. 그대로 |
| 08 위기 대응 M1~M5 | 종가-종가라 몸통은 못 막고 꼬리만 | 지수 1개, 위기 구간 | **기술통계·유효(한계 서술)** | 인과 스트레스테스트로서 "당일 급락은 t−1 정보로 못 막는다"는 구조 사실. 엣지 주장 없음 |
| 09 위기 전략 10종 | 매수 후 보유 대비 결정적 우위 0 | 지수 1개, 2022 홀드아웃 | **유효(기각)** | 10종 전부 뒤짐. 부정 결론은 표본이 작아도 선다(양의 증거 부재) |
| 10 국면 스코어러 | 순열검정 p=0.61 → 분리력 없음 | 06의 6창 | **유효(기각)** | 순열검정이 있는 유일한 국면 스터디. 연속화 변형 B는 미판정 그대로 |
| 11 신호 3축 | 세 축 모두 α<0, 눌림 필터 C4가 2022에 +4.77%p | 2021~2024, 시총 상위 100 스냅샷, C4 37거래 | **무효** | 생존편향 유니버스(README 단서 그대로), 2022 재사용, C4는 37거래 1점. "방어 단서"는 결론이 아니라 가설 |
| 12 급락 후 회복 진입 | 엣지 없음 | 임계 2회 시도 | **유효(기각)** | bias-auditor REJECT. 시도 수 고지됨. 재시도 금지 |
| 13 TRENDX 게이트 | 게이트 월초과 −0.015R(t=−0.96, 46개월) / 점수 IC t=9.8~12.4(89개월) | 시점 고정 풀 500, 46·89개월 | **유효(게이트 기각) + 재검정 후보(점수 IC)** | 월 단위 t·홀드아웃·IC까지 갖춘 유일한 스터디. IC 유의는 살아 있으나 유동성 항 포함 점수는 홀드아웃 마진 0 → §2 팩터 양식으로 다시 잰다(IC·ICIR·5분위·회전율) |
| 14 유지 게이트 4변형 | V2 차이 없음(p=0.55), V3 하락장 열위(p=0.004) | 짝 4,885, 일 정합 t | **유효(기각)·조건부** | 짝지은 설계·일 정합 t는 좋다. 일봉 근사(⑥)라 V3·V4 절대 수준은 못 믿는다 — V1↔V2 결론만 |
| 15 임펄스 눌림 | 108셀 전부 음, 대조군 빼면 0 근처 | 사건 25,588, 날짜 클러스터 t | **유효(기각)** | 가장 잘 설계된 부정 결과. 단, **벤치를 잘못 골랐다**(시가총액가중 지수 vs 동일가중 바스켓) — 이 교훈이 §2 벤치 규칙이 된다 |
| 16 TRENDX 실행 | ATR 손절 비유의, 진입 지연 t=1.01 | 일봉 4구간·3분봉 리플레이 | **유효(기각)** | 3분봉 "유의"는 손절 제거 효과라 스터디가 스스로 걸렀다 |
| 17 청산 기대값 | 판정 통과 셀 0 | 9거래일, 716레그 | **기술통계·유효(무결정)** | 사전등록·일 블록 부트스트랩·판정 제외 규칙이 이미 §2 양식의 원형이다. 결정을 안 낸 것이 맞다 |

집계: 엣지 주장 생존 **0**, 기각 결론 생존 7, 기술통계 4, 무효 6(01·03·04·05·11 + 02의 "채택" 부분).
재검정 후보 3개: 13 점수 IC(팩터 양식), 02 변동성 타게팅(오버레이 양식), 11 C4 눌림 필터(가설 등록만).

무효의 공통 원인은 셋이다 — ① 창이 겹치거나 끝점을 공유해 독립 표본이 1개(01·02) ② 유니버스가 현재 스냅샷(01·02·11) ③ 2022 한 해를 03·09·11·12·14·15·16 일곱 스터디가 재사용해 홀드아웃이 아니게 됨. 이 셋은 §2 양식이 절차로 막는다.

### 1-2. 라이브 9거래일 원장 — 통계적으로 말할 수 있는 것과 없는 것

FIFO로 매도 레그를 매수 로트에 짝지어 **개시 전략**에 귀속했다(09-08 이전 로트가 없어 못 짝지은 매도 299레그, 손익 0인 13행 제외).
손익은 원장 `realized_pnl`(매도 수수료·세금 차감값)에서 매수 수수료 0.015%를 더 뺀 값. 비용 감도는 0.21% 한 값(원장 실측)만 — 3단 감도는 백테스트 쪽 규칙이고 라이브는 실측이 정본이다.

| | 왕복 | 거래일 | 합계(원) | 왕복당(원) | 왕복 t | **일 단위 t** | 승률 | 손익비 | 가중 수익률 | 일 블록 부트스트랩 95% CI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 전체 | 2,311 | 9 | −1,581,702 | −684 | −1.76 | **−0.93** | 36.4% | 1.48 | −0.149% | [−0.38, +0.15] |
| TRENDX 개시 | 1,457 | 9 | −1,077,964 | −740 | −1.69 | −0.98 | 37.4% | 1.40 | −0.186% | [−0.47, +0.23] |
| DEVSCALE 개시 | 693 | 9 | +92,775 | +134 | +0.16 | +0.09 | 38.4% | 1.66 | +0.024% | [−0.40, +0.60] |
| 미연결 로트 개시 | 145 | 9 | −565,723 | −3,902 | −2.09 | −2.30 | 17.2% | 1.66 | −0.656% | [−1.39, −0.29] |

말할 수 있는 것:
- **전략 신호의 부호는 미확정.** 왕복 단위 t가 −1.76·−5.49(수익률)로 커 보이지만 같은 날 왕복이 지수 베타를 공유해 독립이 아니다. 일 단위로 묶으면 t=−0.93(전체)·+0.54(TRENDX 수익률)로 부호조차 안 선다. 유효 표본은 왕복 수가 아니라 **거래일 9**다.
- **손실은 집중돼 있다.** 09-14 하루 −1,462,799원이 순손실의 92%. 09-11 마감 청산 정지 → 19종목 갭 이월(운영)이다. 이 하루를 빼면 전체가 +에 가깝지만, 빼는 것 자체가 사후 선택이라 판정에는 쓰지 않는다.
- **청산 경로별로는 부호가 보인다** — 개시 전략과 무관하게 ITB(청산 관리)로 나간 왕복이 TRENDX 개시 127건 −478K, DEVSCALE 개시 174건 −1,261K. 미연결 로트 개시 145건은 일 단위 t=−2.30으로 유일하게 유의한 음수인데, 이것은 전략이 아니라 재기동 뒤 평단을 잃은 승계 보유분이다. 즉 원장이 유의하게 말하는 유일한 것은 "운영 경로가 돈을 잃는다"이고, 이것은 RESET §1-1 log-reader 표와 같다.

말할 수 없는 것과 필요한 표본:
- 관측된 μ/σ로 t=2.5를 얻으려면 왕복 단위 TRENDX 3,205왕복·DEVSCALE 159,190왕복(사실상 부호 없음). 일 단위로는 일 손익 평균 −176K·표준편차 567K에서 **(2.5·567/176)² ≈ 65거래일**. RESET §1-2의 "≈2,300왕복·80거래일"과 같은 자리다. 결론: 라이브 원장은 **3개월치**가 쌓이기 전에는 전략 판정 근거가 아니고, 그때까지는 **운영 지표(이월 0·이관 0·비용/비용전)** 만 판정한다.
- 원장이 못 주는 것: 이관 시점의 미실현(전략 몫)과 그 뒤 청산(운영 몫)의 분리. RESET §E 원장 4열(`session_id·origin_strategy·handover_mark·signal_price`)이 들어와야 개시 전략 귀속이 FIFO 추정이 아니라 사실이 된다.

---

## 2. 채택 표준 — 사전등록 양식 한 벌

모든 후보(일 단위 팩터 바스켓·장중 전략·오버레이)가 같은 양식을 쓴다. 다른 것은 **표본 단위**(월/일/왕복)와 **문턱 숫자**뿐이다.
`research/studies/<NN>/PREREG.md`에 이 양식 항목을 산출물보다 먼저 커밋한다(RESET §F).

### 2-1. 벤치마크 — 둘을 항상 같이 낸다

| 벤치 | 무엇 | 왜 |
|---|---|---|
| **1차: 같은 유니버스 동일가중 매수 후 보유** | 후보가 고르는 풀(예: 시점 고정 거래대금 상위 500)을 같은 리밸런스 주기로 동일가중 보유, **같은 비용** 부과 | 15번 스터디가 보여준 대로 동일가중 바스켓은 시가총액가중 지수 대비 −0.29%/5일 드리프트가 있다. 이걸 안 빼면 종목 선택 효과와 가중 효과가 섞인다 |
| 2차: KOSPI(또는 KOSDAQ) 지수 | 표시용. 합격 판정에는 쓰지 않는다 | 운용 성과를 바깥 사람이 읽는 기준 |

초과수익 = 후보 비용 뒤 수익 − 1차 벤치 비용 뒤 수익. 벤치에 비용을 안 물리던 관행(RESET §1-3 "벤치 비용 0")은 여기서 끝낸다.

### 2-2. 지표 — 표 맨 위 고정 3줄 + 본 지표

표 상단 고정: **표본 수(왕복/월/사건) · 기간 · walk-forward 창 수와 봉인 창**. 표본이 문턱 미만이면 지표를 내되 "표본 부족 — 결론 보류"를 같은 줄에 박는다.

| 묶음 | 지표 | 정의(약어 없이) |
|---|---|---|
| 초과수익 | `excess_return_after_cost_annual` | 연율화 초과수익, 비용 뒤 |
| 유의성 | `newey_west_t` | 초과수익 시계열(월 또는 일)의 1표본 t, 뉴이-웨스트 분산(시차 = 월 6 / 일 20 / 왕복은 일 블록) |
| 위험 | `sharpe`, `sortino`, `max_drawdown_percent`, `drawdown_recovery_days`, `calmar` | 연율화(월 12의 제곱근, 일 252의 제곱근). 회복일은 고점 회복까지 거래일 |
| 거래 | `turnover_one_way_monthly_percent`, `round_trip_count`, `win_rate_percent`, `payoff_ratio`, `expected_value_per_round_trip_bp`, `cost_to_gross_ratio` | 손익비 = 평균 이익/평균 손실. 비용/비용전 비율 |
| 분리검증 | `walk_forward_windows_total`, `walk_forward_windows_positive`, `sealed_window_excess` | 창 정의 §2-3 |
| 강건성 | `grid_neighbors_positive_count`, `grid_neighbors_total`, `center_sharpe_over_best_ratio` | ±1 격자 |
| 다중검정 | `trials_prior`, `benjamini_hochberg_q`, `deflated_sharpe_p` | 같은 배치에서 시도한 셀 수, BH-FDR q, 스윕이면 Deflated Sharpe |
| 신뢰구간 | `bootstrap_ci_low`, `bootstrap_ci_high`, `bootstrap_block_days`, `bootstrap_draws` | 블록 부트스트랩(§2-3) |
| 팩터 전용 | `rank_ic_mean`, `rank_ic_information_ratio`, `quintile_spread_after_cost_annual`, `quintile_monotonic_spearman`, `capacity_adv20_1pct_krw` | 월 리밸런스 |
| 비용 감도 | `excess_at_cost_021`, `excess_at_cost_050`, `excess_at_cost_100` | 시계열·장중 전략 3단 |

### 2-3. 분리검증·통계 절차

**walk-forward 창.** 학습 3년 → 검증 1년, 1년씩 굴린다.
- 데이터 2015-01~2026-09(네이버 일봉 1990~ + KIND 상폐 목록으로 시점 고정 유니버스를 2015부터 만들면): 검증 창 2018·2019·2020·2021·2022·2023·2024·2025·2026(9개월) = **9창(8 완전 + 1 부분)**. 과반 = **5/9 이상**.
- 지금 `bars_all_pit.parquet`(2019~)만으로는 검증 2022·2023·2024·2025·2026 = **5창**, 과반 3/5 이상. 팩터 스터디는 2015 확장 전까지 5창으로 시작하되 합격은 9창에서만 확정.
- **봉인 창**: 2026-01-01부터는 `research/HOLDOUT_LEDGER.md`에 스터디당 1회 개봉. 2022는 봉인 창이 아니라 그냥 walk-forward 창 하나다.
- 학습 창에서 격자 최적 셀을 고르고 검증 창에 그대로 적용. 검증 창 초과수익을 이어 붙인 시계열이 t·샤프·MDD의 계산 대상이다(학습 창 성과는 표시만).

**뉴이-웨스트 t.** 월 시계열 시차 6, 일 시계열 시차 20. 왕복 단위는 t를 내지 않고 **일 합산 뒤** 일 시계열로 낸다(17번 방식). 사전등록 단일 가설이면 문턱 2.0, 격자 20셀 이상이면 3.0 또는 Deflated Sharpe p<0.05.

**다중검정.** 한 배치(같은 PREREG 안)에서 시도한 셀 수 `trials_prior`를 먼저 적는다. 팩터 여러 개를 한 번에 스캔하면 BH-FDR **q 0.10 이하**. 파라미터 스윕은 Deflated Sharpe. 시도 수 미기재 = 자동 보류.

**블록 부트스트랩.** 월 시계열은 블록 6개월, 일 시계열은 블록 20거래일, 왕복은 **거래일 복원추출**(17번과 같음). 2,000회, seed 고정(20260919). 95% 구간.

**±1 격자.** 핵심 파라미터 각각 ±1단(창 길이 ±20%, 분위 5→3/10, 임계 ±1틱 단위)의 이웃 전부에서 초과수익 부호 유지, 중심 샤프가 격자 최댓값의 70% 이상.

**비용.** 정본 `PYQuant/backtest/costs.py` `CostSpec` 한 벌. 시계열·장중은 왕복 0.21/0.50/1.00% 3단(헌장). 팩터는 편도 회전 × 0.21%로 월 비용을 만들고 슬리피지 상단 0.50%로 한 번 더.

### 2-4. 합격 · 보류 · 기각 문턱

| 층 | 팩터 바스켓(월) | 시계열 전략(일) | 장중 전략(왕복) | 오버레이(국면 배수) |
|---|---|---|---|---|
| 표본 최소 | 검증 월 36 이상, 종목-월 3,000 이상 | 검증 일 750 이상, 왕복 300 이상 | 거래일 40 이상, 왕복은 (2.5·sd/μ)² 사전 계산값 이상 | 검증 월 36 이상, 국면 전환 6회 이상 |
| ① 부호·t | Q5−Q1 비용 뒤 >0 ∧ newey_west_t ≥2.0 ∧ rank IC ≥0.03 ∧ ICIR ≥0.5 ∧ 5분위 단조 Spearman ≥0.8 | 초과수익 비용 뒤 >0 ∧ t ≥2.0(단일)/3.0(스윕) | 왕복 기대값 ≥+10bp ∧ 일 시계열 t ≥2.0 | 확장−수축 월수익 차 t ≥2.0 |
| ② walk-forward | 창 과반(5/9 또는 3/5 이상) 스프레드 >0 ∧ 봉인 창 >0 | 같음 | 월 창 과반 >0(리플레이) | 창 과반 ∧ MDD 20% 이상 감소 |
| ③ 강건 | ±1 이웃 전부 >0 ∧ 중심 샤프 ≥70% | 같음 ∧ 비용 0.50%까지 >0 | 같음 ∧ 비용/비용전 ≤0.5 ∧ 비신호 청산 ≤30% | ±1 ∧ 전환 ≤6회/년 |
| 다중검정·CI | BH-FDR q ≤0.10 ∧ 부트스트랩 CI 하한 >0 | DSR p<0.05(스윕) ∧ CI 하한 >0 | 일 블록 CI 하한 >0 | CI 하한 >0 |
| 회전·용량 | 편도 회전 월 ≤30%(가치·퀄리티)/≤60%(수급·PEAD) ∧ ADV20 1% ≥100억 | MDD ≤ 벤치 MDD+5%p | 실현 슬리피지 ≤ 사전등록 | — |

- **합격**: 위 다섯 행 전부 통과.
- **보류**: ①은 통과했으나 ②·③·CI 중 하나가 미달, 또는 표본이 최소 미만. 보류는 "무엇이 더 필요한가"(필요 N·필요 기간·필요 유니버스)를 숫자로 적어야 닫힌다. 보류 상태로 30일 지나면 자동 기각.
- **기각**: ① 미달, 또는 walk-forward 창 과반 미달, 또는 시도 수 미기재. 기각은 `docs/DECISIONS.md` D-번호로 남기고 **라이브에서 내린다**(헌장).

### 2-5. `metrics.json` v2 스키마

`"schema": "quant.metrics/v2"`. v1 열은 이름을 유지하고 아래를 더한다(없는 값은 `null`이 아니라 `"not_computed"` 문자열 — null은 "계산했더니 없음"과 구별이 안 된다).

```json
{
  "schema": "quant.metrics/v2",
  "study_id": "18_factor_harness", "strategy": "low_volatility_q1", "family": "factor_monthly",
  "prereg_path": "research/studies/18_factor_harness/PREREG.md", "prereg_sha256": "(해시)", "run_manifest_path": "(경로)",
  "universe": "pit_turnover_top500", "universe_grade": "A", "period_start": "2015-01-05", "period_end": "2026-09-19",
  "sample_unit": "month", "sample_count": 104, "round_trip_count": 1240,
  "benchmark_primary": "same_universe_equal_weight_buy_and_hold", "benchmark_secondary": "KOSPI",
  "cost_spec": {"commission_percent": 0.015, "sell_tax_percent": 0.20, "slippage_percent": 0.0, "round_trip_percent": 0.23},
  "excess_return_after_cost_annual": 0.0, "newey_west_t": 0.0, "newey_west_lag": 6,
  "excess_at_cost_021": 0.0, "excess_at_cost_050": 0.0, "excess_at_cost_100": 0.0,
  "sharpe": 0.0, "sortino": 0.0, "max_drawdown_percent": 0.0, "drawdown_recovery_days": 0, "calmar": 0.0,
  "benchmark_sharpe": 0.0, "benchmark_max_drawdown_percent": 0.0,
  "turnover_one_way_monthly_percent": 0.0, "win_rate_percent": 0.0, "payoff_ratio": 0.0,
  "expected_value_per_round_trip_bp": 0.0, "cost_to_gross_ratio": 0.0,
  "walk_forward_windows_total": 9, "walk_forward_windows_positive": 0, "walk_forward_train_years": 3, "walk_forward_test_years": 1,
  "sealed_window": "2026-01-01~", "sealed_window_excess": "not_computed", "sealed_window_opened_at": "not_opened",
  "grid_neighbors_total": 4, "grid_neighbors_positive_count": 0, "center_sharpe_over_best_ratio": 0.0,
  "trials_prior": 12, "benjamini_hochberg_q": 0.0, "deflated_sharpe_p": "not_computed",
  "bootstrap_ci_low": 0.0, "bootstrap_ci_high": 0.0, "bootstrap_block_days": 20, "bootstrap_draws": 2000, "bootstrap_seed": 20260919,
  "rank_ic_mean": 0.0, "rank_ic_information_ratio": 0.0, "quintile_spread_after_cost_annual": 0.0, "quintile_monotonic_spearman": 0.0,
  "capacity_adv20_1pct_krw": 0,
  "verdict": "pass / hold / reject 중 하나", "verdict_reason": "(사유)", "hold_needs": {"sample_count": 0, "days": 0, "universe": ""},
  "bias_audit": "pass / conditional / reject / not_done 중 하나",
  "equity_csv_path": "(경로)", "trades_csv_path": "(경로)", "source": "(경로)"
}
```

`scripts/check_backtest.py`가 v2 필수 열 누락·`trials_prior` 누락·`prereg_sha256` 시각이 산출물 mtime보다 늦은 것을 잡는다.
대시보드(`scripts/exit_ev_dashboard.py` 백테스트 탭)는 v1·v2를 같이 읽되 v1 행에는 "구 양식 — 판정 미적용" 표식.

---

## 3. 전략 비교 틀

### 3-1. 한 표에 놓는 규칙

같은 표에 놓으려면 **기간·비용·유니버스·벤치**가 같아야 한다. 다르면 랭킹하지 않고 따로 표시한다.
후보 다섯(재무 팩터·저변동성·수급 지속·거시 오버레이·장중 DEVIATION_SCALE)은 표본 단위가 달라(월/월/월/월/왕복) 그대로는 못 놓는다.
공통 단위는 **일 단위 슬리브 손익 시계열**이다 — 팩터는 일 종가 평가, 장중은 일 실현손익, 오버레이는 적용 전후 차이. 이 일 시계열을 월로 합쳐 표를 만든다.

| 열 | 뜻 |
|---|---|
| 단독 합격 여부 | §2-4 판정. 합격 아닌 후보는 이 표에 오르되 조합 계산에서 뺀다 |
| 월 초과수익·newey_west_t·샤프·MDD·회전 | §2-2 |
| 상관 행렬 | 후보 쌍의 **월 초과수익 상관**(검증 창만). 0.5 넘는 쌍은 같은 슬리브로 본다 |
| 한계 기여 | 기존 조합에 이 후보를 위험 동일가중(역변동성)으로 더했을 때 조합 샤프 변화 `marginal_sharpe` 와 MDD 변화 |
| 용량 | ADV20 1% 기준 수용 자본 |

### 3-2. 조합(슬리브 합산) 규칙

1. 슬리브 가중은 **역변동성(위험 동일가중)** 으로 고정한다. 최적화 가중은 표본이 이 정도로는 과적합이라 쓰지 않는다.
2. 후보를 더하는 조건: 단독 합격 ∧ 기존 슬리브와 상관 0.5 이하 ∧ 조합 샤프 **+0.10 이상** ∧ 조합 MDD 악화 없음(walk-forward 검증 창 이어 붙인 시계열 기준, 블록 부트스트랩 CI로 샤프 차이의 하한 >0이면 확정, 아니면 보류).
3. 오버레이(국면 배수)는 슬리브가 아니라 **곱**이다: 합격 슬리브 조합에 곱했을 때 MDD 20% 이상 감소 ∧ 샤프 유지(−0.05 이내) ∧ 전환 연 6회 이하. 슬리브 합격이 0인 동안은 표시만(RESET §D-4).
4. 순서: 저변동성(지금 데이터) → 13번 점수 IC 재검정 → 수급 지속(과거분 채우기 뒤) → 재무(DART 뒤) → 장중 DEVIATION_SCALE(리플레이·틱 캡처 뒤). 각각 단독 표가 먼저, 조합 표는 합격 2개부터.

---

## 4. 승격·강등 규칙

### 4-1. 승격 — 세 단

| 단 | 조건 | 예산·기간 | 종료·판정 |
|---|---|---|---|
| ① 백테스트 합격 | §2-4 합격 + bias-auditor PASS + PREREG 해시 | — | `metrics.json` `verdict: pass` |
| ② 소액 forward 슬리브 | ①. `strategies/<전략>/sleeves/SLEEVE_<id>.md`에 예산·종료·판정을 적고 커밋 | 종목 50만원 이하, 일 손실 −10만원, 누적 −100만원(즉시 중지). 팩터 바스켓은 슬리브 총액 1,000만원 이하 | 장중: 100왕복 또는 60거래일 중 먼저. 팩터: 리밸런스 3회(종목-월 60 이상). 판정: 비용 뒤 왕복(또는 종목-월) 기대값 >0 ∧ 일 가중 t ≥1.5 ∧ 실현 비용/왕복이 사전등록의 1.5배 이하 ∧ 체결률 90% 이상(지정가면). 미달은 ①로 되돌리되 같은 PREREG로 재도전 1회만 |
| ③ 본 슬리브 | ② 통과 | 슬리브당 자본의 20% 이하, 조합 §3-2 | 상시 강등 감시(§4-2) |

실계좌 문턱은 RESET §5-6 그대로(합격 전략 1개 이상 ∧ forward 10거래일 비용 뒤 0 이상)인데, 여기에 **②를 통과한 슬리브만**을 더한다.

### 4-2. 강등 규칙 — 라이브에서 내리는 숫자

`scripts/check_runtime_health.py`에 판정 행으로 넣어 자동으로 낸다(사람이 열어 보는 항목을 만들지 않는다).

| 지표 | 창 | 내리는 조건 | 조치 |
|---|---|---|---|
| 일 블록 부트스트랩 CI 상한 | 굴림 20거래일 | 상한 <0 | 즉시 `entry_halt`, ②로 강등 |
| 실현 비용/왕복 | 굴림 20왕복 | 사전등록 비용의 1.5배 초과 | 진입 중지, 슬리피지 원인 확인 뒤 재개 |
| 낙폭 | 슬리브 개시부터 | 사전등록 MDD의 1.5배 | 즉시 청산·강등 |
| 승률·손익비 | 누적 100왕복 | 사전등록 값의 부트스트랩 95% 구간 밖 | 보류(진입 절반) |
| 비신호 청산 비중 | 굴림 20거래일 | 30% 초과 | 전략이 아니라 운영 문제 — 슬리브는 유지, 운영 판정 행 |
| 파라미터 변경 | — | config 전략 키 diff에 `PARAM_LEDGER.md` 행 없음 | 새 슬리브로 취급(①부터) |

강등된 슬리브는 같은 PREREG로 재승격 1회만. 두 번째 강등은 D-번호 기각.

---

## 5. 첫 실행 단계 — `PYQuant/backtest/stats.py`

`research/studies/13_trendx_gate/stats_util.py`(betainc·one_sample_t·benjamini_hochberg_qvalues·spearman)를 여기로 옮기고 아래를 더한다. 스터디 13·14·16·17은 여기서 import하고 기존 out 파일 소수 4자리 재현이 골든이다. 의존은 numpy·pandas만(scipy 없이 — 13이 이미 그렇게 돼 있다).

```python
# PYQuant/backtest/stats.py
from dataclasses import dataclass
import numpy as np, pandas as pd

@dataclass(frozen=True)
class TestResult:
    mean: float; t_statistic: float; p_value: float; sample_count: int; standard_error: float

def one_sample_t(values) -> TestResult: ...
    # 13 stats_util 이관. n<2면 nan.

def newey_west_t(series: pd.Series, lag: int) -> TestResult: ...
    # 평균 0 검정. 분산 = gamma_0 + 2 * sum_{k=1..lag} (1 - k/(lag+1)) * gamma_k. lag=0이면 one_sample_t와 같아야 한다.

def block_bootstrap_ci(values: pd.Series, block_length: int, draws: int = 2000, seed: int = 20260919,
                       statistic=np.mean, alpha: float = 0.05) -> tuple[float, float]: ...
    # 원형 블록 부트스트랩. 왕복 원장은 거래일을 복원추출하는 아래 함수를 쓴다.

def day_block_bootstrap_ci(frame: pd.DataFrame, day_column: str, value_column: str, weight_column: str | None,
                           draws: int = 2000, seed: int = 20260919, alpha: float = 0.05) -> tuple[float, float]: ...
    # 17번 방식. weight_column이 있으면 sum(value)/sum(weight)(가중 수익률), 없으면 평균.

def benjamini_hochberg_q(p_values: list[float]) -> list[float]: ...
    # 13 이관.

def deflated_sharpe_p(observed_sharpe: float, trials: int, sample_count: int,
                      skewness: float, kurtosis: float, sharpe_variance_across_trials: float) -> float: ...
    # Bailey & Lopez de Prado(2014). trials=1이면 보통 샤프 검정과 같다.

def rank_ic(scores: pd.DataFrame, forward_returns: pd.DataFrame) -> pd.Series: ...
    # Date x code 두 와이드 패널 -> 날짜별 Spearman. 같은 날 단면 안에서만.

def ic_information_ratio(ic_series: pd.Series) -> float: ...
    # mean/std, 연율화 없음(월이면 월 ICIR).

def quintile_spread(scores: pd.DataFrame, forward_returns: pd.DataFrame, quantiles: int = 5) -> pd.DataFrame: ...
    # 열: q1..q5 수익, spread(q5-q1), monotonic_spearman(분위 순위 vs 평균수익).

def walk_forward_windows(start: str, end: str, train_years: int = 3, test_years: int = 1) -> list[tuple[str, str, str, str]]: ...
    # [(train_start, train_end, test_start, test_end)]. 2015-01~2026-09 -> 9개, 2019-01~2026-09 -> 5개.

def walk_forward_verdict(test_excess_by_window: list[float]) -> tuple[int, int, bool]: ...
    # (양의 창 수, 전체 창 수, 과반 여부). 과반 = positive > total // 2.

def grid_robustness(grid: dict[tuple, float], center: tuple) -> tuple[int, int, float]: ...
    # (±1 이웃 중 >0 개수, 이웃 수, 중심/최댓값 비율). 이웃은 각 축 ±1 한 칸.

def required_sample_count(mean: float, standard_deviation: float, target_t: float = 2.5) -> int: ...
    # ceil((target_t * sd / mean)^2). mean=0이면 -1.

def performance_summary(daily_excess: pd.Series, periods_per_year: int = 252) -> dict: ...
    # sharpe·sortino·max_drawdown_percent·drawdown_recovery_days·calmar·annual_return. metrics.json v2 열 이름으로 반환.
```

**테스트 3개** (`PYQuant/tests/test_stats.py`):

1. `test_newey_west_matches_one_sample_t_when_lag_zero_and_shrinks_under_autocorrelation` — 백색잡음 1,000개에서 `newey_west_t(lag=0)`과 `one_sample_t`가 1e-9 안에서 같고, AR(1) φ=0.5 시계열(seed 고정)에서 `newey_west_t(lag=6)`의 |t|가 `one_sample_t`의 |t|보다 작다(자기상관을 보정하면 t가 줄어야 한다).
2. `test_day_block_bootstrap_reproduces_study_17_cell` — `research/studies/17_exit_ev/exit_ev_all.tsv`의 TRENDX 묶음(수익률 0.07%, CI [−0.19, 0.46])을 같은 seed·2,000회로 재계산해 소수 2자리 일치. 골든이 스터디 산출물이라 이관이 계산을 바꾸지 않았음을 확인한다.
3. `test_walk_forward_windows_count_and_no_overlap` — `walk_forward_windows("2015-01-01","2026-09-19")`가 9개, `("2019-01-01","2026-09-19")`가 5개를 내고, 모든 창에서 test_start = train_end 다음 날, 검증 창끼리 겹치지 않으며, 마지막 창 test_end가 입력 end로 잘린다. 덤으로 무작위 점수 검정: 무작위 점수 1,000회 `rank_ic` 평균이 0±0.01, `benjamini_hochberg_q`로 q<0.1 비율 5% 이하(RESET §3 하네스 행).

실행: `py -m pytest PYQuant/tests/test_stats.py -q`. 골든 재현: `py research/studies/13_trendx_gate/run_trendx_gate.py`가 import를 바꾼 뒤에도 `results.tsv` 소수 4자리 일치.

---

## 6. 자기 영역 밖에서 본 문제

1. **`research/studies/11_signal_axes/metrics.json` 데이터 오류** — "단기 역추세(무필터)" 행의 `equity_csv_path`·`trades_csv_path`가 `cross_momentum_voladj_*.csv`를 가리킨다(자기 파일이 아니다). 대시보드 백테스트 탭이 이 행을 그리면 다른 전략의 곡선이 나온다. 생성기(`11_signal_axes/*_run_meta.json` → metrics) 쪽에서 고쳐야 한다.
2. **`render_studies.py`는 01~03만 안다** — `STUDIES` 딕셔너리가 세 개뿐이라 06 이후 폴더의 README는 손으로 쓴 것이다. §2-5 v2 스키마를 넣을 때 렌더러가 `metrics.json`을 읽어 README 상단 고정 3줄(표본·기간·창)을 자동으로 찍게 하지 않으면 도장이 또 어긋난다.
3. **원장의 개시 전략 귀속은 지금 추정이다** — FIFO로 짝지어도 09-08 이전 로트가 없는 매도 299레그, `ORPHAN` 레이블로 매수된 로트 145왕복이 남는다. RESET §E 원장 4열이 들어오기 전까지는 §1-2 표의 전략별 행을 "추정"이라고 적어야 한다. 매수 로트에 전략이 붙어 있어도 재기동 뒤 `ORPHAN`으로 다시 매수되는 경로가 있는지 엔진 쪽 확인이 필요하다.
4. **청산 관리(ITB) 경로가 두 전략 개시 왕복에서 −1.74M** — 이것은 전략 판정 밖의 운영 손실인데 원장에는 전략 손익으로 섞여 들어간다. 강등 규칙 §4-2 "비신호 청산 비중"이 운영 판정으로 분기되는 이유다.
5. **비용 정의 시점 불일치** — 원장은 매도세 0.18%(2024 값)로 계산돼 있고 `costs.py LIVE`는 0.20%다. §2 표준의 라이브 비교는 원장 행을 `LEDGER_UNTIL_2026_09_21` 규칙으로 되짚은 뒤에만 유효하다(RESET §B-1).
6. **17번의 사전등록 전제(회의에서 기술통계 일부를 먼저 봄)** 는 이번 표준에서 `bias_audit: conditional`로 남긴다. 이후 스터디는 PREREG 커밋 시각이 첫 계산보다 앞서야 하고, 그 순서를 `check_backtest.py`가 잡는다.
7. **표본을 키우는 배선이 우선이다** — 위 §1-2가 말하는 대로 라이브는 65거래일, 팩터는 2015년부터 시점 고정 유니버스가 있어야 9창이 된다. 네이버 일봉 1990~ + KIND 상폐 목록 과거분 채우기가 없으면 §2-3의 "9창 합격"은 2027년까지 못 낸다.
