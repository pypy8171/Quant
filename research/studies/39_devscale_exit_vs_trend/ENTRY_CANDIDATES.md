# 스터디 39 — 실계좌 DeviationScale 진입 재설계 후보 (2026-10-11)

시험은 다른 에이전트가 4회차 하네스(`research/studies/39_devscale_exit_vs_trend/run_round4.py`)로 돌린다. 이 문서는
① 왜 지금 진입이 지수를 못 이기는지에 대한 가설과 그 근거, ② 일봉으로 시험할 후보 5개(사전 등록 격자 19칸),
③ 미리 정한 판정 기준, ④ `run_round3.evaluate`에 넣는 방법을 적는다. 성과 숫자는 아직 없다. 아래 진단 숫자는 성과 판정이 아니라
가설의 근거로만 쓴다.

자료와 범위: 스터디 38 Panel(`PYQuant/data/bars_all_pit_v2.parquet` 보통주 3,190 종목, 상장폐지 포함) 2010-01-04 – 2026-10-08을
한 번에 쓴다. 전 종목에서 4회차와 같은 두 판(U2 = 전날 거래대금 10억 이상 ∧ 주문 ≤ 20일 평균의 0.5%, U0 = 주문 상한만)으로 나눈다.
분봉 백필 도구의 `--all-market`(`PYQuant/tools/minute_fullmarket_backfill.py`)은 쓰지 않는다. config·엔진 코드·커밋은 건드리지 않았다.

---

## 1. 진단 — 왜 지수를 못 이기는가

### 1-1. 이미 측정된 것 (4회차, `results/round4/`)

| 항목 | 값 | 출처 |
|---|---|---|
| 라이브 칸(a) 매매당 순수익 | −0.65% (117,332 건, U2) | `results/round4/first_table_a.csv` |
| 지수 대비 연환산 초과 / t | −36.0%p / −12.2 (기준선 2.72) | 같은 파일 |
| 63칸 중 지수 대비 기준선 넘은 칸 | 0 | `results/round4/cell_table.csv` |
| 손절 없는 칸(익절 없음·손절 3–15) 지수 대비 t | −0.25 – −0.97 | 같은 파일 35–40행 |
| 무작위 대비 t(라이브 칸) | +18.9 — 무작위 쪽이 매매당 −1.06%로 더 나빴다 | 같은 파일 |
| 라이브 칸 보유 일수 | 0일 17,809 · 1–5일 71,963 · 6–20일 27,481 · 21일 이상 79 건 | `results/round4/trades.parquet` 칸 41 |
| 슬롯 판(4칸·200만) | 연 −16.6%, MDD −95% | `results/round4/first_table_a.csv` |

읽는 법: 손절을 빼고 오래 들수록 지수와 거의 같아진다(t −0.3 근처). 종목을 고른 효과는 0에 가깝고, 짧게 사고파는 동안
비용과 아래 1-2의 장중 손실이 쌓여 크게 진다. "무작위보다 낫다"는 무작위 쪽이 시가에 사서 장중 하락을 통째로 먹기 때문에 생긴 몫이 크다(1-2 진단 1).

### 1-2. 이번에 잰 진단 3개 (성과 판정 아님)

명령: 부록 B 스크립트, 103초. 라이브 근사는 4회차와 같은 신호 수를 다시 냈다(U2 322,435 · U0 790,796 — 마스크 코드 검증).

**진단 1 — 장중은 지고 밤은 번다.** U2 풀(라이브 후보 조건과 같은 바탕) 종목-일 평균:

| | 전 기간 | 연도별 범위(2010–2026, 17개 해) |
|---|---|---|
| 시가 → 종가 | **−0.180%** | −0.108 (2023) – −0.378 (2011), 17개 해 모두 음 |
| 전날 종가 → 시가 | **+0.334%** | +0.201 (2021) – +0.905 (2026), 17개 해 모두 양 |

엔진은 장중에 사고(존 가격), 손절·익절도 장중에 낸다. 보유가 짧을수록 매매 수익이 장중 구간에 쏠리고, 그 구간 평균이 하루 −0.18%다.
이것은 진입 신호와 무관한 체결 시점 문제라서 신호를 아무리 바꿔도 남는다 — 5절에 따로 축으로 둔다.

**진단 2 — "눌림"이 아니라 단기 승자를 산다.** 전날 기준 횡단면 백분위(0 = 가장 낮음):

| | 60일 수익 백분위 평균 | 5일 수익 백분위 평균 | 5일 수익 하위 20% 비중 | 20일 변동성 백분위 평균 |
|---|---|---|---|---|
| 라이브 근사 신호(U2) | 0.563 | **0.657** | **3.1%** | 0.413 |
| 같은 날 U2 풀 | 0.577 | 0.537 | 22.7% | 0.628 |

60일로 보면 더 오른 종목을 사지 않는다(0.563 대 0.577) — "다 오른 종목을 늦게 산다"는 60일 축에서는 근거가 없다.
대신 직전 5일에 오른 종목을 산다. 국내 시장의 단기(1주) 반전 경향과 반대 방향이다.

**진단 3 — 진입가가 SMA20 위다.** 라이브 근사 진입가 ÷ 전날 SMA20 − 1의 분위는 10% +0.97 · 25% +2.19 · 50% +3.76 · 75% +5.16 · 90% +5.48(%),
SMA20 위 비중 97%. 존 하단 −8%는 거의 쓰이지 않는다. 시가 이격 ≥ −1% 필터(`entry_open_dev_min_pct`)가 시가를 SMA20 근처 위로 묶고,
접은 SMA20(오늘 가격 포함)이 기준이라 장중 하락이 이격을 덜 깎기 때문이다. 시가가 존 위에서 내려와 상단(+5%)에 산 비중이 24%다.

### 1-3. 가설

| 번호 | 가설 | 근거 | 시험하는 후보 |
|---|---|---|---|
| H-A | 존이 위쪽으로 넓어 "SMA20 위 +1 – +5%"를 산다. 눌림을 사려면 상단을 내리고 시가 이격 필터를 풀어야 한다 | 진단 3 | C1 |
| H-B | 단기 승자를 사서 1주 반전에 진다. 장기 추세 위의 단기 패자를 사야 한다 | 진단 2 | C2 |
| H-C | 가격·추세 조건에는 종목 선택 정보가 없다(D-013 "정배열 단독은 유의하게 짐", 4회차 손절 없는 칸 t≈0). 가격 밖 정보(수급)가 필요하다 | 4회차 1-1, D-013·D-014(30일 표본만 봄) | C3 |
| H-D | 무작위 대비 이긴 몫은 저변동 쪽으로 기운 덕이다(신호 변동성 백분위 0.41 대 풀 0.63, ATR ≤ 4.5% 필터). 그 축만 떼면 더 깨끗하다 | 진단 2 | C4 |
| H-E | 정배열은 늦은 추세 신호다. 추세를 따라가려면 신고가 근처(투자자가 고가를 기준점으로 삼는 효과)를 사고 오래 들어야 한다 | 38번 "수익은 21일 넘게 든 매매에서", D-064 | C5 |
| H-F | 대형·유동 종목이 섞여 효율적이다 | 38번 2–5억 바구니. 단 4회차에서 U0(중앙 7.3억)와 U2(29.7억)가 똑같이 졌다(t −11.7 대 −12.2) — 이 진입에서는 근거 약함 | 후보가 아니라 판(U2·U0) 축으로만 본다 |
| H-G | 체결 시점(장중 매수·장중 매도)이 하루 −0.18%를 깎는다 | 진단 1 | 5절 X1(이번 19칸 밖) |

기각된 가설 기록: "60일 기준 다 오른 종목을 늦게 산다"는 진단 2에서 근거가 없었다.

---

## 2. 후보 5개 (사전 등록 19칸)

공통 규칙
- 신호 값은 전부 t−1 행(전날 종가까지)으로 계산해 t 행으로 민다. 전날 행이 바로 앞 시장일이 아니면(정지 뒤 첫날) 신호 없음.
  t 행에서 쓰는 것은 그날 시가(가격 범위 2,000–100,000원, 진입가)와 그날 코스피 시가 갭(> −1%, 엔진 `risk_off_index_pct`와 같음)뿐이다.
- 횡단면 백분위는 그날 거래된 전 종목(코스피·코스닥, 이후 폐지 종목 포함) 안에서 낸다.
- C1은 엔진처럼 장중 존 가격에 산다(4회차 근사 E 그대로). C2–C5는 t 시가 + 1호가에 산다(`run_round3.OpenEntry`, 무작위 대조군과 같은 체결).
- C2–C5는 보드 안에서 시장일마다 점수 상위 **30 종목**까지만 신호로 둔다(**내가 정함** — 메모리 한도와 슬롯 4칸 현실. 슬롯 판은 그 안에서 다시 전날 거래대금 큰 순).
- 손으로 정한 값은 모두 **내가 정함**이고 격자를 둔다. 격자 칸은 판정 때 서로의 옆 칸이다.

### C1. 존 상단 낮춤 — 현행 진입의 제거실험 (H-A)

- 식: 4회차 근사 E에서 존 상단 `u`와 시가 이격 하한만 바꾼다.
  존 = 접은 SMA5 > 접은 SMA10 > 접은 SMA20 ∧ −8% ≤ p / ((S19 + p)/20) − 1 ≤ u, S19 = 전날까지 19개 종가 합.
  그날 [저가, 고가]가 존 가격 구간에 닿으면 산다. ATR14/전날 SMA20 ≤ 4.5%, 진입가 2,000–100,000원.
- 격자(6칸): u ∈ {+5%(라이브), 0%, −3%} **내가 정함** × 시가 이격 하한 ∈ {−1%(라이브), 끔}. (+5, −1)은 라이브 재현이자 기준.
- 엣지 근거: 진단 3. 같은 정배열 종목에서 SMA20 아래로 실제로 눌린 날만 산다.
- 엔진 난이도: **낮음** — config 값만. `entry_upper_pct`는 음수도 그대로 계산된다(`Quant/src/strategy/DevScaleRules.cpp` `zone_band`, `Quant/src/strategy/DeviationScaleZone.cpp` 46–55행). 시가 이격 끔은 `entry_open_dev_min_pct`를 꺼짐 센티널(`Params::kOpenDeviationOff`)로.
  배선: 기존 DEVIATION_SCALE 로더(config 값 변경). 국면 축: 없음.
- 붙일 청산: **칸 55 (d) E4j5**(종가 < SMA20 5일 연속, 진입 다음 날부터). 4회차 보유 중앙 17일 — 슬롯 200건 판정이 되는 가장 긴 추세 청산 쪽.
- 신호 수(U2/U0): +5·−1 322,435/790,796 · +5·끔 330,194/808,992 · 0·−1 49,072/137,079 · 0·끔 55,941/152,645 · −3·−1 **3,722**/7,690 · −3·끔 **5,899**/11,579.
  −3% 칸은 U2에서 하루 1–2건이라 슬롯 200건 미만이면 "판정 불가"가 정상이다.

### C2. 장기 추세 위 단기 과매도 (H-B)

- 식(t 행): `rank5[t−1] ≤ q` ∧ `close[t−1] > SMA_L[t−1]`.
  rank5 = 5행 수익(close[t−1]/close[t−6] − 1)의 그날 횡단면 백분위. 점수 = −rank5(가장 많이 빠진 순 30개).
- 격자(4칸): q ∈ {10%, 20%} × L ∈ {60, 120} **내가 정함**.
- 엣지 근거: 국내 1주 반전(유동성 공급 보상)과, 장기 추세 위라는 조건으로 부실·급락 종목을 줄인다. 현행이 진단 2에서 반대쪽(5일 승자)을 사고 있었다.
- 위험: 신호 변동성 백분위 평균 0.86 — 고변동 종목으로 쏠린다. 반전 프리미엄이 변동성 보상일 수 있어 C4와 함께 봐야 한다. 매매 회전이 빠르다.
- 엔진 난이도: **중간** — 횡단면 순위에 전 종목 전날 종가가 필요하다. Python 장전 잡이 대상 파일을 쓰고 TARGET_BASKET 로더(`Quant/src/strategy/StrategyFactory.cpp` `load_target_basket`)가 시가에 사는 경로. 배선: 신규 장전 잡 + 기존 로더(TARGET_BASKET이 종목별 손익절을 받는지는 확인 필요). 국면 축: 없음.
- 붙일 청산: **칸 19 가격 익절8·손절15**(반전은 가격 목표가 자연스럽다. 4회차 보유 중앙 14일). 옆 칸 12·18·20·26.
- 신호 수(U2/U0): 10%·60 94,216/105,166 · 10%·120 101,539/110,312 · 20%·60 108,936/113,012 · 20%·120 112,539/114,666.

### C3. 수급 괴리 — 외국인·기관이 사는데 가격은 빠진 종목 (H-C)

- 식(t 행): `rankF[t−1] ≥ 1 − q` ∧ `ret_k[t−1] ≤ 0`.
  F = Σ(최근 20행, t−1 포함) (외국인 + 기관 순매수 수량) × 무수정 종가 ÷ Σ(같은 20행) 거래대금. 20행이 다 있을 때만. rankF = 그날 횡단면 백분위. 점수 = rankF.
- 격자(4칸): q ∈ {10%, 20%} × k ∈ {5, 20} **내가 정함**.
- 자료: `PYQuant/data/investor_flow_pit_2010.parquet`(네이버 trend, 2010-01-04 – 2026-10-01, 3,081 종목). 신호 기간 행의 95.3%를 덮는다. 빈 날은 신호 없음.
  날짜 d 값은 d 장 마감 뒤 나오므로 t−1까지만 쓴다. 시점 등급 **B**(사후에 받은 값, 정정 여부 확인 안 됨). 2026-10-02 – 10-08 신호 없음.
- 엣지 근거: 가격 밖 정보. 정보가 많은 쪽의 지속 매수가 단기 가격 약세와 어긋날 때 그 차이가 메워진다. D-014의 "수급 무효"는 30일 표본이었고 STRATEGY_LAB H12가 2009년 이후 재검정을 걸어 뒀다 — 이 칸이 그 재검정이다(재검정 1회를 `trials_prior`에 더한다).
- 엔진 난이도: **중간** — 장전에 전 종목 20일 수급이 필요하다. KIS 수급 TR은 종목당 30일·분당 20건이라 2,500 종목이면 2시간이 넘는다. 네이버 trend를 저녁에 받는 Python 잡(`PYQuant/tools/naver_flow_backfill.py` 확장) → 대상 파일 → TARGET_BASKET. 배선: 신규 장전 잡. 국면 축: 없음.
- 붙일 청산: **칸 61 (e) 보유 20일**(수급 신호의 시계가 20일). 옆 칸 62.
- 신호 수(U2/U0): 10%·5일 83,689/111,900 · 10%·20일 45,524/95,772 · 20%·5일 109,555/114,710 · 20%·20일 87,078/112,135.

### C4. 저변동 종목의 얕은 눌림 (H-D)

- 식(t 행): `rankV[t−1] ≤ q` (∧ 눌림이면 `SMA60[t−1] < close[t−1] < SMA20[t−1]`).
  V = 최근 20행 일간 수익 표준편차(t−1 포함). rankV = 그날 횡단면 백분위. 점수 = −rankV.
- 격자(3칸): (q 20%, 눌림) · (q 30%, 눌림) · (q 10%, 눌림 없음) **내가 정함**. 눌림 없는 칸은 하루 상위 30개 한도 때문에 q를 바꿔도 같은 종목이 나와 하나만 둔다(U0에서 20%와 30%가 115,890건으로 같았다).
- 엣지 근거: 저변동 이상 현상(복권형 종목 회피). 현행 진입이 이미 저변동 쪽으로 기울어 무작위를 이겼다(진단 2). STRATEGY_LAB H10의 진입판.
- 위험: U0 눌림 없는 칸은 20일 평균 거래대금 중앙 2.2억 — 거래가 적어 변동성이 낮은 종목이 섞인다. U2를 본판으로 보는 이유다.
- 엔진 난이도: **중간–낮음** — UniverseScanner가 일봉을 이미 들고 있어 20일 변동성은 계산되지만 횡단면 순위는 장전 잡이 낫다. 배선: 장전 잡 → TARGET_BASKET, 또는 DEVIATION_SCALE 진입 필터에 외부 점수 항. 국면 축: 없음.
- 붙일 청산: **칸 61 (e) 보유 20일**(월 단위 저변동 바구니와 같은 시계). 옆 칸 62.
- 신호 수(U2/U0): 20%·눌림 14,841/60,701 · 30%·눌림 28,705/83,626 · 10%·없음 62,528/115,890.

### C5. 250일 고가 근접 (H-E)

- 식(t 행): `close[t−1] / max(high[t−250 .. t−1]) ≥ h`. 250행이 다 있을 때만. 점수 = 그 비율.
- 격자(2칸): h ∈ {90%, 95%} **내가 정함**.
- 엣지 근거: 신고가 근처에서 투자자가 고가를 기준점으로 삼아 좋은 소식에 덜 반응한다. 정배열보다 덜 늦은 추세 신호이고, 수익이 21일 넘는 보유에서 나왔다는 38번 결과와 맞물린다.
  국내 근거는 엇갈린다 — 그래서 격자를 2칸으로만 둔다.
- 엔진 난이도: **낮음** — `Quant/src/strategy/DeviationScaleZone.cpp` 148–176행이 250봉 고가를 이미 계산해 로그에만 쓴다. 필터 값 하나 추가(DEVIATION_SCALE 확장) 또는 장전 잡. 단 시가 매수라 존 진입과 체결이 다르다. 국면 축: 없음.
- 붙일 청산: **칸 60 (d) E4j10**(4회차 보유 중앙 28일, 추세가 꺾일 때까지 든다). 옆 칸 59.
- 신호 수(U2/U0): 90% 110,597/112,132 · 95% 94,606/102,683.

### 기존 전략과의 겹침

실계좌는 DeviationScale만 돈다. C1은 그 자체의 변형이라 상관이 가장 높다. C2(단기 반전)는 C5(신고가 추세)와 방향이 반대라 둘 다 붙으면 조합 분산이 크다.
C4는 모의 계좌 가치 코어 바스켓과 같은 방어 계열이다. 후보 둘 이상이 통과하면 일별 초과수익 상관을 먼저 낸다.

---

## 3. 판정 기준 (실행 전에 고정)

판정 단위는 후보 격자 19칸 각각의 **본 청산 칸**(위에 적은 칸 하나), U2 판, 종가 평가다. 63칸 전부와 U0 판은 같이 돌려 표에 싣는다.

| 번호 | 기준 | 값 |
|---|---|---|
| 1 | 지수 대비(코스피 종목은 코스피, 코스닥 종목은 코스닥) `index_t_min` | ≥ max(그 실행의 `bar_index`, **3.0**) |
| 2 | 무작위 진입(같은 날·같은 판 풀·같은 청산) 대비 `random_t_min` | ≥ max(`bar_random`, **3.0**) |
| 3 | 옆 칸 같은 부호 | 청산 옆 칸(`neighbours_same_sign`)과 같은 후보 안 격자 옆 칸(본 청산 칸의 `index_annual_pct` 부호) 모두 |
| 4 | 양(+)인 해 비율 `share_positive_years` | ≥ 0.7 (2010–2026 17개 해, 해 빼기 없음) |
| 5 | 슬롯 판(4칸·200만·건당 50만) | 슬롯 매매 ≥ 200, 연환산 > 코스피 보유(8.47%) **그리고** > 코스닥 보유(3.18%) |
| 6 | 미청산 평가 민감도 | 종가·−10%·−20% 셋 다 통과(`passes_all_valuations`) |
| 7 | U0 판 | 본 청산 칸 `index_annual_pct` 부호가 U2와 같다(t 요구 없음) |
| 8 | 현행 진입 대비 | 같은 청산 칸의 4회차 U2 일별 포트폴리오 수익(`results/round4/daily_returns.parquet`)과의 날짜별 차이 평균이 양(+). t는 표에 싣는다 |

- 3.0은 헌장 "20칸 t 3.0"이다(19칸). 뒤집기 기준선(4회차 2.72)이 더 높게 나오면 그것을 쓴다.
- 존 이탈 칸(41·42)은 C2–C5에서 뜻이 없다(정배열 존이 아닌 진입). 기준선 계산에는 4회차처럼 63칸 그대로 넣는다 — 기준선이 오를 뿐이라 보수 쪽이다.
- 청산은 가격 격자 42칸 표를 먼저 싣고, 본 청산 칸의 이긴 매매 MAE(고점 전 최대 하락)·MFE(최고 상승) 분포를 `run_exit_test.excursions`로 같이 낸다(창 60일).
- 통과가 0칸이면 "이 다섯 축의 일봉 진입에는 지수 대비 엣지가 없다"로 적고, 현행 진입은 계속 지수에 지는 상태임을 같이 적는다. 기각된 진입은 라이브에서 내린다(헌장).
- 연도별 표(`year_table.csv`)는 안정성 확인용으로 같이 낸다. 특정 해를 빼거나 기간을 잘라 판정하지 않는다.

사전 등록 파일: 실행 전에 `results/round5/choices_log.csv`에 위 8개 기준·19칸 격자·하루 30개 한도·본 청산 칸·시드(`common.SEED`, `run_round3.RANDOM_SEED`)를 적는다.

---

## 4. `run_round3.evaluate`에 넣는 방법

부록 A의 모듈을 `research/studies/39_devscale_exit_vs_trend/entry_candidates.py`로 두면, 각 칸은 `build(context) -> (entries, approach, view)`를 낸다.

- `entries`: 진입 행 번호(int64, 오름차순). 그 행 날짜가 매수일.
- `approach`: C1은 4회차와 같은 0 시가가 존 안 · 1 아래에서 올라와 삼 · 2 위에서 내려와 삼. C2–C5는 전부 0(시가 매수, 그날 저가·고가가 매수 뒤라 손절·익절 판정에 쓴다).
- `view`: `entry_mid`·`entry_raw`·`entry_tick`·`aligned_close`·`aligned_previous`·`folded_open_aligned`를 가진 객체. C1은 `ZoneView`, C2–C5는 `run_round3.OpenEntry`.

`run_round4.main`의 판 반복을 아래로 바꾸면 된다(나머지 지수·슬롯·표 코드는 그대로).

```python
import run_round4 as r4
import entry_candidates as ec

features = ec.Features(data)                         # 약 100초(수급 병합 포함)

for board, extra in r3.BOARDS:                        # U2, U0
    context = ec.Context(data, engine, flags, features, extra)

    for name, label, build in ec.VARIANTS:            # 19칸
        entries, approach, view = build(context)
        strategy = r3.evaluate(data, view, rules, entries, approach, runner=r4.run_exits_next_day)
        control_rows, _ = r3.random_entries(data, flags, extra, entries)     # 같은 날·같은 판 풀, 시가 매수
        control = r3.evaluate(data, context.open_view, rules, control_rows,
                              np.zeros(len(control_rows), dtype=np.int64), runner=r4.run_exits_next_day)
        strategy["index_id"] = market_of_stock[strategy["stock"]]
        control["index_id"] = market_of_stock[control["stock"]]

        for valuation, multiplier in r3.VALUATIONS:
            table, year_table, bars, equity, daily = r3.board_tables(
                data, rules, strategy, control, multiplier, index_open, index_close, groups,
                (signs_month, signs_year), first_day, last_day, years, kospi_curve)
            table = r4.with_kosdaq_hold(table, kosdaq_annual, kosdaq_mdd)
            primary = ec.PRIMARY_CELL[name]
            # 기준 1·2: max(bars[...]["main_bar"], 3.0) 로 다시 판정. live_* 열(같은 실행의 칸 41 대비)은 C2–C5 에서 무시.
            # 기준 8: daily["portfolio"][primary] − round4 daily_returns(cell == primary).portfolio_pct 를 날짜로 맞춰
            #         r3.family_t(diff[None, :], month_of_day, year_of_day, month_count, year_count, signs_month, signs_year,
            #                     np.array([True])) 로 t 를 낸다.
        del strategy, control
```

- 메모리: 가장 큰 칸은 C1 U0 +5·끔 808,992건으로 4회차 U0(790,796)와 같은 크기다. 나머지는 하루 30개 한도로 12만 건 이하.
- 시간 예상(★예상치): 4회차가 두 판 94초였다. 19칸 × 2판이면 30분 안팎.
- 라이브 재현 점검: C1 (+5, −1) U2 결과가 `results/round4/cell_table.csv`와 같아야 한다(같은 마스크·같은 커널). 다르면 나머지를 돌리지 않는다.

---

## 5. 이번 19칸 밖에 두는 다음 수 — X1 체결 시점(H-G)

진단 1은 신호와 무관하게 장중 구간이 하루 −0.18%를 깎는다는 뜻이다. 어떤 후보든 "종가 매수(장 마감 동시호가)"로 바꾸면
그 구간을 피하고 밤 구간(+0.33%)을 처음부터 가진다. 하네스로는 커널에 approach 3("진입일 저가·고가 모두 종가로 봄")을 넣으면 된다 —
`run_round4.exit_rule_next_day` 82–83행의 `low_after`·`high_after`를 `approach == 1 or approach == 3`, `approach == 2 or approach == 3`으로,
진입가는 `data.close[t−1]`(t−1 행 진입)로. 19칸 결과를 보고 통과 후보(없으면 C1 기준 칸)에 시가 대 종가 두 판으로 붙인다.
엔진 쪽은 DEVIATION_SCALE이 15:20 이후 동시호가에 내는 주문 경로가 있어야 해 난이도가 높다(신규 주문 시점 처리).

---

## 6. 편향 점검

- 미래 정보: 모든 신호 배열은 `previous_known`(같은 종목 한 행 밀기 + 정지 뒤 첫날 제외)을 거친다. 횡단면 순위도 t−1 날짜 안에서 낸 뒤 민다.
  수정주가 수준값은 쓰지 않고 비율만 쓴다(가격 범위는 무수정 시가).
- 생존 편향: 패널이 상장폐지 종목을 포함하고 폐지 배수(round11)를 그대로 쓴다.
- 다중 비교: 19칸 → t 3.0 하한, 뒤집기 기준선이 더 높으면 그것. 63 청산칸은 기술 통계로만 싣는다.
- 자료 시점 등급: OHLCV A. 수급 B(사후 수집). 재무(`PYQuant/data/fin/fin_point_in_time.parquet`)는 2015-06부터라 2010 – 2026 전 기간 조건을 못 맞춰 이번 후보에서 뺐다 —
  가치(저PBR×수익성)는 2015 – 2026 판으로 따로 돌리는 것이 우회로다.

---

## 7. 이번 턴에 만든 파일

- `research/studies/39_devscale_exit_vs_trend/ENTRY_CANDIDATES.md` — 이 문서. 부록 A가 마스크 모듈 전문, 부록 B가 점검 스크립트 전문이다.
  저장소에 `.py`로 두지 않은 것은 요청이 "한 파일"이어서다. 시험 에이전트는 부록 A를 `research/studies/39_devscale_exit_vs_trend/entry_candidates.py`로 옮겨 쓰면 된다.
- 재실행(점검): 부록 A·B를 같은 폴더에 두고 `py -X utf8 check_candidates.py` — 103초, 위 신호 수와 진단 1–3을 다시 낸다.

---

## 부록 A. `entry_candidates.py`

```python
"""스터디 39 진입 후보 C1–C5 마스크(ENTRY_CANDIDATES.md 부록 A 와 같은 코드).

모든 신호 값은 t−1 행(전날 종가까지)에서 계산해 t 행으로 민다. t 행에서 쓰는 것은 그날 시가(가격 범위·진입가)와
그날 코스피 시가 갭(엔진 risk_off 와 같음)뿐이다. C1 만 엔진처럼 장중 존 가격에 사고, C2–C5 는 t 시가 + 1 호가에 산다.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

import run_round3 as r3
import common
import round4_test

r2, r1 = r3.r2, r3.r1
FLOW = r1.REPOSITORY / "PYQuant" / "data" / "investor_flow_pit_2010.parquet"
DAILY_CAP = 30          # 내가 정함: C2–C5 는 그날 점수 상위 30 종목까지만 신호로 둔다(보드 안에서)


# ── 보조 ────────────────────────────────────────────────────────────────────

def shift_rows(values: np.ndarray, data, rows: int) -> np.ndarray:
    """같은 종목 안에서 rows 행 앞 값. 앞 행이 모자라면 NaN."""
    panel = data.panel
    out = np.full(values.shape[0], np.nan)
    out[rows:] = values[:-rows]
    offset = np.arange(panel.row_count) - panel.starts[panel.stock_of_row]
    out[offset < rows] = np.nan
    return out


def previous_known(values: np.ndarray, data) -> np.ndarray:
    """t−1 행 값을 t 행에 둔다. 전날 행이 바로 앞 시장일이 아니면(정지 뒤 첫날) NaN."""
    out = round4_test.shift_within_stock(values.astype(np.float64), data.panel.starts)
    out[~data.previous_traded] = np.nan
    return out


def day_rank(values: np.ndarray, data) -> np.ndarray:
    """그 행 날짜에 거래된 전 종목 안 백분위(0 에 가까울수록 작음). NaN 은 NaN."""
    frame = pd.DataFrame({"day": data.panel.market_position, "value": values})
    return frame.groupby("day")["value"].rank(pct=True).to_numpy()


def daily_top(mask: np.ndarray, score: np.ndarray, data, cap: int = DAILY_CAP) -> np.ndarray:
    """mask 안에서 시장일마다 score 큰 순 cap 개만 남긴다."""
    rows = np.flatnonzero(mask)
    frame = pd.DataFrame({"row": rows, "day": data.panel.market_position[rows], "score": score[rows]})
    order = frame.groupby("day")["score"].rank(ascending=False, method="first").to_numpy()
    out = np.zeros_like(mask)
    out[rows[order <= cap]] = True
    return out


def board_pool(data, flags: dict, extra: tuple) -> np.ndarray:
    """run_round3.random_entries 의 대조군 풀과 같은 바탕. 대조군이 같은 날 같은 풀에서 뽑히게 한다."""
    pool = flags["signal_period"] & flags["kospi_gap_gt_-1"] & np.isfinite(data.open)

    with np.errstate(invalid="ignore"):
        pool &= (data.raw_open >= r1.MIN_PRICE) & (data.raw_open <= r1.MAX_PRICE)

    for name in extra:
        pool &= flags[name]

    return pool


# ── 피처(한 번 계산) ────────────────────────────────────────────────────────

class Features:
    """C2–C5 가 쓰는 t−1 값. 이름 끝 _prev 는 전날 종가까지 확정된 값을 t 행에 둔 것."""

    def __init__(self, data, with_flow: bool = True):
        panel = data.panel
        starts = panel.starts
        close = data.close

        with np.errstate(invalid="ignore", divide="ignore"):
            r1d = close / shift_rows(close, data, 1) - 1.0
            r5 = close / shift_rows(close, data, 5) - 1.0
            r20 = close / shift_rows(close, data, 20) - 1.0
            r60 = close / shift_rows(close, data, 60) - 1.0

        sma120 = common.rolling_mean_known(close, starts, 120, True)
        filled = np.nan_to_num(r1d)
        mean = common.rolling_mean_known(filled, starts, 20, True)
        square = common.rolling_mean_known(filled * filled, starts, 20, True)
        offset = np.arange(panel.row_count) - starts[panel.stock_of_row]
        rv20 = np.sqrt(np.maximum(square - mean * mean, 0.0) * 20.0 / 19.0)
        rv20[offset < 21] = np.nan
        high250 = (pd.Series(data.high).groupby(panel.stock_of_row).rolling(250, min_periods=250).max()
                   .reset_index(level=0, drop=True).sort_index().to_numpy())

        self.close_prev = previous_known(close, data)
        self.sma20_prev = previous_known(data.sma[20], data)
        self.sma60_prev = previous_known(data.sma[60], data)
        self.sma120_prev = previous_known(sma120, data)
        self.r5_prev = previous_known(r5, data)
        self.r20_prev = previous_known(r20, data)
        self.r60_prev = previous_known(r60, data)
        self.rv20_prev = previous_known(rv20, data)
        self.high250_prev = previous_known(high250, data)
        self.r5_rank_prev = previous_known(day_rank(r5, data), data)
        self.r60_rank_prev = previous_known(day_rank(r60, data), data)
        self.rv20_rank_prev = previous_known(day_rank(rv20, data), data)
        self.flow20_prev = np.full(panel.row_count, np.nan)
        self.flow20_rank_prev = np.full(panel.row_count, np.nan)

        if with_flow:
            flow = pd.read_parquet(FLOW, columns=["date", "ticker", "foreign_net_quantity", "institution_net_quantity"])
            flow["date"] = flow["date"].dt.strftime("%Y%m%d").astype(np.int64)
            flow["net"] = (flow["foreign_net_quantity"] + flow["institution_net_quantity"]).astype(np.float64)
            rows = pd.DataFrame({"ticker": np.array(panel.codes)[panel.stock_of_row], "date": panel.dates})
            net = rows.merge(flow[["ticker", "date", "net"]], on=["ticker", "date"], how="left")["net"].to_numpy()
            value = net * panel.raw_close                          # 순매수 금액(무수정 주식 수 × 무수정 종가)
            known = np.isfinite(value).astype(np.float64)
            flow_mean = common.rolling_mean_known(np.nan_to_num(value), starts, 20, True)
            known20 = common.rolling_mean_known(known, starts, 20, True)
            turnover_mean = common.rolling_mean_known(np.nan_to_num(panel.day_turnover), starts, 20, True)

            with np.errstate(invalid="ignore", divide="ignore"):
                flow20 = np.where((known20 >= 1.0) & (turnover_mean > 0), flow_mean / turnover_mean, np.nan)

            self.flow20_prev = previous_known(flow20, data)
            self.flow20_rank_prev = previous_known(day_rank(flow20, data), data)
            self.flow_coverage = float(np.isfinite(value[panel.signal_ok]).mean())


# ── C1: 현행 존의 제거실험(엔진처럼 장중 존 가격에 산다) ─────────────────────

class ZoneView:
    """r2.Engine 과 같은 식, 존 상단·하단만 바꾼다. evaluate 에 view 로 넘긴다."""

    def __init__(self, data, engine, pullback: float, upper: float):
        s19 = data.before[20]
        low_ratio, high_ratio = 1.0 - pullback, 1.0 + upper
        band_low = low_ratio * s19 / (20.0 - low_ratio)
        band_high = high_ratio * s19 / (20.0 - high_ratio)
        floor = np.where(engine.align_floor > 0, engine.align_floor * (1.0 + r2.EPSILON), engine.align_floor + r2.EPSILON)
        self.lower = np.maximum(floor, band_low)
        self.upper = band_high

        with np.errstate(invalid="ignore"):
            self.interval = np.isfinite(s19) & (self.lower <= self.upper) & (self.upper > 0)
            self.touched = self.interval & (data.low <= self.upper) & (data.high >= self.lower)

        inside = (data.open >= self.lower) & (data.open <= self.upper)
        self.entry_mid = np.where(inside, data.open, np.where(data.open > self.upper, self.upper, self.lower))
        self.entry_raw = self.entry_mid * data.unadjust_clean
        self.entry_tick = r1.ticks(self.entry_raw, data.panel.dates)
        self.aligned_close = engine.aligned_close
        self.aligned_previous = engine.aligned_previous
        self.folded_open_aligned = engine.folded_open_aligned


def c1_zone(data, engine, flags, extra, upper: float, open_dev_min: float | None):
    """upper: 존 상단(+0.05 라이브, 0, −0.03). open_dev_min: −0.01 라이브, None 이면 끔."""
    view = ZoneView(data, engine, r1.PULLBACK, upper)

    with np.errstate(invalid="ignore"):
        mask = (flags["signal_period"] & view.interval & view.touched & flags["atr14_le_4.5"] & flags["kospi_gap_gt_-1"]
                & (view.entry_raw >= r1.MIN_PRICE) & (view.entry_raw <= r1.MAX_PRICE))

        if open_dev_min is not None:
            mask &= data.open_deviation >= open_dev_min

    for name in extra:
        mask &= flags[name]

    entries = np.flatnonzero(mask).astype(np.int64)
    open_price = data.open[entries]
    approach = np.where(open_price > view.upper[entries], 2, np.where(open_price < view.lower[entries], 1, 0)).astype(np.int64)
    return entries, approach, view


# ── C2–C5: t 시가에 산다 ────────────────────────────────────────────────────

def open_entries(mask: np.ndarray, score: np.ndarray, data, open_view):
    mask = daily_top(mask & np.isfinite(score), score, data)
    entries = np.flatnonzero(mask).astype(np.int64)
    return entries, np.zeros(len(entries), dtype=np.int64), open_view


def c2_reversal(data, features, pool, open_view, bottom: float, trend_window: int):
    """장기 추세 위 단기 과매도: 전날 5일 수익 하위 bottom AND 전날 종가 > 전날 SMA{trend_window}. 점수 = −5일 수익 백분위."""
    trend = features.sma60_prev if trend_window == 60 else features.sma120_prev

    with np.errstate(invalid="ignore"):
        mask = pool & (features.r5_rank_prev <= bottom) & (features.close_prev > trend)

    return open_entries(mask, -features.r5_rank_prev, data, open_view)


def c3_flow(data, features, pool, open_view, top: float, lookback: int):
    """수급 괴리: 전날 20일 (외국인+기관) 순매수 ÷ 20일 거래대금 상위 top AND 전날 lookback 일 수익 ≤ 0. 점수 = 순매수 백분위."""
    price = features.r5_prev if lookback == 5 else features.r20_prev

    with np.errstate(invalid="ignore"):
        mask = pool & (features.flow20_rank_prev >= 1.0 - top) & (price <= 0.0)

    return open_entries(mask, features.flow20_rank_prev, data, open_view)


def c4_low_vol(data, features, pool, open_view, bottom: float, pullback: bool):
    """저변동: 전날 20일 실현변동성 하위 bottom. pullback 이면 SMA60 < 전날 종가 < SMA20 도. 점수 = −변동성 백분위."""
    with np.errstate(invalid="ignore"):
        mask = pool & (features.rv20_rank_prev <= bottom)

        if pullback:
            mask &= (features.close_prev < features.sma20_prev) & (features.close_prev > features.sma60_prev)

    return open_entries(mask, -features.rv20_rank_prev, data, open_view)


def c5_high(data, features, pool, open_view, near: float):
    """250 일 고가 근접: 전날 종가 ÷ 전날까지 250 행 최고가 ≥ near. 점수 = 그 비율."""
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = features.close_prev / features.high250_prev
        mask = pool & (ratio >= near)

    return open_entries(mask, ratio, data, open_view)


# ── 사전 등록 격자(19 칸) ───────────────────────────────────────────────────

VARIANTS = (
    ("C1", "존 상단+5·시가이격≥−1 (라이브 재현)", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.05, -0.01)),
    ("C1", "존 상단+5·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.05, None)),
    ("C1", "존 상단0·시가이격≥−1", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.0, -0.01)),
    ("C1", "존 상단0·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.0, None)),
    ("C1", "존 상단−3·시가이격≥−1", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, -0.03, -0.01)),
    ("C1", "존 상단−3·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, -0.03, None)),
    ("C2", "5일 하위10%·SMA60 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.10, 60)),
    ("C2", "5일 하위10%·SMA120 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.10, 120)),
    ("C2", "5일 하위20%·SMA60 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.20, 60)),
    ("C2", "5일 하위20%·SMA120 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.20, 120)),
    ("C3", "순매수 상위10%·5일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.10, 5)),
    ("C3", "순매수 상위10%·20일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.10, 20)),
    ("C3", "순매수 상위20%·5일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.20, 5)),
    ("C3", "순매수 상위20%·20일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.20, 20)),
    ("C4", "변동성 하위20%·눌림", lambda c: c4_low_vol(c.data, c.features, c.pool, c.open_view, 0.20, True)),
    ("C4", "변동성 하위30%·눌림", lambda c: c4_low_vol(c.data, c.features, c.pool, c.open_view, 0.30, True)),
    ("C4", "변동성 하위10%·눌림 없음", lambda c: c4_low_vol(c.data, c.features, c.pool, c.open_view, 0.10, False)),
    ("C5", "250일 고가 90% 이상", lambda c: c5_high(c.data, c.features, c.pool, c.open_view, 0.90)),
    ("C5", "250일 고가 95% 이상", lambda c: c5_high(c.data, c.features, c.pool, c.open_view, 0.95)),
)
PRIMARY_CELL = {"C1": 55, "C2": 19, "C3": 61, "C4": 61, "C5": 60}   # 4회차 칸 번호(r1.build_rules 순서)


class Context:
    """VARIANTS 람다가 읽는 묶음."""

    def __init__(self, data, engine, flags, features, extra):
        self.data, self.engine, self.flags, self.features, self.extra = data, engine, flags, features, extra
        self.pool = board_pool(data, flags, extra)
        self.open_view = r3.OpenEntry(data, engine)
```

## 부록 B. `check_candidates.py` (신호 수·진단, 성과 판정 아님)

```python
"""후보 마스크 점검: 신호 수·라이브 재현·진단 3 개(성과 판정 아님)."""
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_round3 as r3     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
import entry_candidates as ec     # noqa: E402

r2, r1 = r3.r2, r3.r1
started = time.time()
data = r1.Data()
panel = data.panel
engine = r2.Engine(data)
flags = r2.flags_of(data, engine)
features = ec.Features(data)
print(f"준비 {time.time() - started:.0f}초 · 수급 덮는 비율(신호 기간 행) {features.flow_coverage:.3f}", flush=True)
rows = []

for board, extra in r3.BOARDS:
    context = ec.Context(data, engine, flags, features, extra)

    for name, label, build in ec.VARIANTS:
        entries, approach, view = build(context)
        days = panel.market_position[entries]
        per_day = np.bincount(days, minlength=panel.market_count)
        active = per_day[per_day > 0]
        rows.append({"board": board, "candidate": name, "variant": label, "signals": len(entries),
                     "days_with_signal": int((per_day > 0).sum()), "median_per_day": float(np.median(active)) if len(active) else 0.0,
                     "stocks": int(len(np.unique(panel.stock_of_row[entries]))),
                     "inside_below_above": np.round(np.bincount(approach, minlength=3) / max(len(approach), 1), 3).tolist(),
                     "r60_rank_mean": float(np.nanmean(features.r60_rank_prev[entries])),
                     "r5_rank_mean": float(np.nanmean(features.r5_rank_prev[entries])),
                     "rv20_rank_mean": float(np.nanmean(features.rv20_rank_prev[entries])),
                     "turnover20_median_억": float(np.nanmedian(data.previous_20d[entries]) / 1e8)})
    print(f"{board} 끝 {time.time() - started:.0f}초", flush=True)

table = pd.DataFrame(rows)
pd.set_option("display.width", 250)
print(table.to_string(index=False))

# 진단 1: 장중(시가→종가)과 밤(전날 종가→시가) 평균 수익, U2 풀 동일가중, 연도별
pool = ec.board_pool(data, flags, r3.BOARDS[0][1]) & data.previous_traded
previous_close = ec.previous_known(data.close, data)

with np.errstate(invalid="ignore", divide="ignore"):
    intraday = data.close / data.open - 1.0
    overnight = data.open / previous_close - 1.0

frame = pd.DataFrame({"year": panel.years[pool], "intraday": intraday[pool], "overnight": overnight[pool]})
by_year = frame.groupby("year")[["intraday", "overnight"]].mean() * 100.0
print("진단1 U2 풀 행 평균(%) 전체", (frame[["intraday", "overnight"]].mean() * 100.0).round(4).to_dict())
print(by_year.round(3).to_string())

# 진단 2: 라이브 재현 신호의 전날 60일·5일 수익 백분위 분포(대조 풀과 비교)
context = ec.Context(data, engine, flags, features, r3.BOARDS[0][1])
live_entries, _, _ = ec.VARIANTS[0][2](context)
for label, rows_ in (("라이브 신호", live_entries), ("U2 풀", np.flatnonzero(context.pool))):
    r60 = features.r60_rank_prev[rows_]
    r5 = features.r5_rank_prev[rows_]
    print(f"진단2 {label}: 60일 백분위 평균 {np.nanmean(r60):.3f} · 상위30% 비중 {np.nanmean(r60 >= 0.7):.3f} · "
          f"5일 백분위 평균 {np.nanmean(r5):.3f} · 하위20% 비중 {np.nanmean(r5 <= 0.2):.3f} · 변동성 백분위 평균 "
          f"{np.nanmean(features.rv20_rank_prev[rows_]):.3f}")

# 진단 3: 라이브 재현 신호의 진입가 이격(전날 SMA20 대비)
with np.errstate(invalid="ignore", divide="ignore"):
    view = ec.ZoneView(data, engine, r1.PULLBACK, 0.05)
    deviation = view.entry_mid[live_entries] / data.sma20_previous[live_entries] - 1.0
print("진단3 진입가 ÷ 전날 SMA20 − 1 분위(%)", np.round(np.nanpercentile(deviation, [10, 25, 50, 75, 90]) * 100.0, 2).tolist(),
      "· 0 위 비중", round(float(np.nanmean(deviation > 0)), 3))
table.to_csv(Path(__file__).resolve().parent / "candidate_counts.csv", index=False, encoding="utf-8-sig")
print(f"끝 {time.time() - started:.0f}초")
```
