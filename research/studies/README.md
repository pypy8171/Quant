# studies/ — 폴더형 백테스트 스터디

> [BACKTESTS.md](../BACKTESTS.md) 목록 항목 중 **trade CSV가 남아 종목별 매매 원장까지 드릴다운되는** 스터디를 폴더로 승격한 곳.
> 각 폴더의 `README.md`는 질문·설정·발견 요약 + 런별 실현손익 표, `{run}.md`는 **어떤 종목을 언제 사고팔아 얼마 손익**이었는지 왕복 원장(FIFO 매칭).
> 스터디 문서의 t·창·문턱 숫자가 무엇을 뜻하고 어디서 왔는지는 [READING_NUMBERS.md](READING_NUMBERS.md)(확률 표는 `py research/studies/threshold_check.py`가 만든다).

## 계열 A — 종목레벨 모멘텀/레짐

| 스터디 | 무엇 | 런 | 원장 |
|---|---|---|---|
| [01_momentum_regime/](01_momentum_regime/README.md) | 모멘텀×국면필터 기준선 | 최근 1~5년 | 5런 |
| [02_vol_target/](02_vol_target/README.md) | 변동성 타게팅 사이징 | 최근 1~5년 | 5런 |
| [03_2022_removal_test/](03_2022_removal_test/README.md) | 2022 약세장 regime ON/OFF | ON·OFF | 2런 |
| [06_bear_market/](06_bear_market/README.md) | 하락장 6구간 이벤트 스터디 | 6구간×4전략 | [events/](06_bear_market/events/README.md) |

> 지표 전체(수익률·샤프·MDD·α)는 [BACKTEST_LOG](../BACKTEST_LOG.md) 실행 #1~#3(폴더 01~03), 6구간은 각 이벤트 README.
> 월별 시작시점 스윕·05는 trade CSV 없이 summary JSON만이라 폴더 미승격 — [목록](../BACKTESTS.md) 카드로만(의도적 공백).

### 계열 A 확장 — 구조 국면 스코어러 (Track A)

계열 A의 **유일 레버(국면필터)** 를 라이브 C++ `RegimeController`에서 분해해, 이산 판정을 연속화·기울기·오버레이로 제거실험한 스터디.

| 스터디 | 무엇 | 한 줄 요지 |
|---|---|---|
| [10_regime_scorer/](10_regime_scorer/README.md) | 구조 국면 스코어러 4변형(A/B/C/D) 제거실험 | C++ `compute_score`/`classify`를 미러(A)하고 연속(B)·기울기(C)·SOX/VIX 오버레이(D)로 확장 → 06 하락장 6창에서 지수 long/flat 프록시로 결정론 비교. `test_regime_scorer.py`가 라이브 패리티 강제. **미판정**(연속화가 나아 보이나 편향감사·엣지판정 후속). 장중 국면 Track B(`index_intraday_logger.py`)는 지수 PIT 부재로 forward 적재만. |

### 계열 A 확장 — 신호 3축 나란히 비교 (신호 3축 나란히 비교)

성격이 다른 세 신호축을 같은 유니버스·기간·비용에서 나란히 검정한 스터디.

| 스터디 | 무엇 | 한 줄 요지 |
|---|---|---|
| [11_signal_axes/](11_signal_axes/README.md) | 횡단면추세(C1)·시계열추세(C3)·단기역추세(C4) 3축 나란히 비교 | KOSPI 상위 100·2021~2024에서 비용 물고 동일가중 매수 후 보유 초과 여부. 세 축 모두 벤치 못 이김(α<0) — **벤치 못 이김**(전천후 엣지 아님). C4 눌림 필터본만 무필터 대비 α·낙폭 개선하고 2022 홀드아웃에서 매수 후 보유 +4.77%p 상회 — 하락장 국한 방어 단서. 지표는 [metrics.json](11_signal_axes/metrics.json), 규칙원문은 [BACKTEST_LOG 실행 #6](../BACKTEST_LOG.md). |

### 계열 A 확장 — 급락 후 바닥권 진입

급락 국면에서 되돌림을 잡는 진입 규칙이 지수 대비 초과수익을 내는지 검정한 스터디.

| 스터디 | 무엇 | 한 줄 요지 |
|---|---|---|
| [12_base_breakout/](12_base_breakout/README.md) | 급락 후 20MA 회복일 진입 · 보유 60거래일 | 250일 고점 대비 임계(KOSDAQ −25% / KOSPI −20%) 이하에서 20MA를 회복한 날 익일 시가 진입, 같은 구간 지수 대비 초과수익으로 판정. **엣지 없음 · 라이브 배선 금지** — bias-auditor REJECT, 승격 게이트 미통과. 임계를 두 번 시도한 다중검정과 사후 선택 위험을 리포트에 고지했다. |
| [13_trendx_gate/](13_trendx_gate/README.md) | TRENDX 게이트(정배열 ∧ 이격 5~35) PIT 풀 기저 비교(46개월, 일 정합) · 점수 순위상관(IC, 89개월) | 선정 층만 잰다. 현행 셀 월초과 −0.015R(t=−0.96, p=0.34), 홀드아웃 2022 −0.046(t=−1.33) — 유의한 플러스도 D-012 수준 마이너스도 배제. 점수 IC는 유의(t=9.8~12.4)하고 선택 마진도 8셀 전부 양(+)이지만, 그 마진을 적격군 자체의 마이너스가 상쇄하고 라이브가 쓰는 유동성 항 포함 점수로는 홀드아웃 마진이 0. STRATEGIES.md #1 기각·#2 보류(2026-09-15). |
| [14_hold_axis/](14_hold_axis/README.md) | DevScale `hold_zone`(유지 게이트) 4변형 브래킷 없이 짝비교 | D-088 배경 조사. 전일 정배열(aligned_hold)만 쓰는 현행(V1)은 당일 막 정배열이 시작된 종목을 유지 게이트가 막아 진입 자체를 못 한다. 여기에 당일 정배열(aligned)을 OR로 더한 V2는 홀드아웃(2022bear)에서 현행 대비 유의한 차이 없음(p=0.55, 결과 같은 거래 96%) — 손실 안 늘어남만 확인. 이격만으로 유지 판정하는 V3는 유의하게 열위(p=0.004). |
| [15_impulse_pullback/](15_impulse_pullback/README.md) | 임펄스 상승 후 30~55% 눌림 + 중기추세 유지 진입 · 보유 1/3/5/10일 | PIT 유니버스 2020~2026-09-01에서 사건 25,588건. 지수 대비 초과수익이 네 보유기간·세 밴드·세 비용 전부 음수(H=5 −0.30%, t=−5.40), 2022 홀드아웃도 −0.24%로 같은 부호. 같은 날 유니버스 평균을 빼면 사건 알파는 0 근처라 음수의 대부분은 동일가중 바스켓의 지수 대비 드리프트다. **bias-auditor 감사 전 수치.** |
| [16_trendx_execution/](16_trendx_execution/README.md) | TRENDX 실행 층: ATR14×배수 손절 vs 고정 2.5% · 진입 2봉 확인 지연 | 손절: 일봉 4구간은 배수 전부 비유의(t≤1.86), 2022bear 홀드아웃은 배수 셋 다 음수. 3분봉 리플레이는 유의(+0.083R, t>7)하지만 발동률 0.1~1.1%라 "배수 효과"가 아니라 "손절 사실상 제거" 효과. STRATEGIES.md #7 보류(2026-09-15). 진입 지연: ΔR +0.0128(t=1.01, p=0.34), 표본 앞뒤로 부호 뒤집힘 — STRATEGIES.md #8 기각(2026-09-15). |
| [17_exit_ev/](17_exit_ev/README.md) | 모의 원장 청산 사유별 조건부 승률·기대값 + 일 단위 블록 부트스트랩 CI (2026-09-08~18, 9거래일, 716레그) | 사전등록한 판정 규칙(두 구간 수익률 CI가 같은 쪽)을 통과한 청산 규칙 **없음** — config 변경 없음. TRENDX 묶음 +0.07% CI [−0.19, 0.46], 익절 +4.80M을 손절 −4.16M이 거의 지운다. bias-auditor 1차 기각(order_id 재기동 충돌로 레그 10.6% 섞임, 손익이 이미 매도 비용 차감값) 뒤 고쳐 재계산. |
| [19_fundamental_factors/](19_fundamental_factors/README.md) | 저PBR×고ROE 복합(상위 30 동일가중 월 리밸), DART 주요계정 시점 고정 B, walk-forward 5창 | **미달** — 비용 MID 초과수익 연 +6.99%(t 1.14, 뉴이-웨스트 1.31), walk-forward 4/5 양수, 이웃 4/5·샤프 비 0.39. 신호(IC t 7.88)는 있으나 상위 30 동일가중 구성이 약함 |
| [20_macro_overlay/](20_macro_overlay/README.md) | 거시 네 축(성장·물가·위험선호·유동성) 국면 점수 → 코스피 노출 배수 오버레이, 1999~2025 walk-forward, 격자 27셀 사전등록 | `macro_apply=false` — 7항목 중 이웃 부호·전환 횟수만 통과, MDD −56.0%→−55.3%(방어 없음)·CAGR 반납 1.35%p. 일간 배수가 회전을 만들어(노출 변경 비용 27년 17.8%) 재실행 전 배수 갱신 주기를 스펙에 정해야 한다. |
| [21_macro_overlay_hold/](21_macro_overlay_hold/README.md) | 스터디 20 재실행 — 위험선호·유동성 배수를 축 구간이 바뀐 날에만 갱신(오너 결정 6, 2026-09-20), 나머지 설계·합격 숫자 동일 | `macro_apply=false` — 비용 12.2%→5.7%로 절반 아래인데 MDD −56.0%→−54.0%(3.7%)·CAGR 반납 1.03%p·월 초과수익 t −1.54, 20과 같은 2항목만 통과. 비용이 원인이 아니라 국면 라벨 지연(수축 달의 월수익이 +1.40%로 가장 높음)이 원인. 파라미터 더 안 뒤짐 |
| [22_pbr_roe_value_tilt/](22_pbr_roe_value_tilt/README.md) | 스터디 19 후속 — 중심을 가치 비중 0.7·분기 리밸로 미리 못 박고(격자 27칸) 시총가중·크기 3분위·용량·상폐 보유 기록을 더함. 일봉은 KIND 상폐사 보강 A, 재무 B | **채택 후보** — 비용 MID 초과수익 연 +15.70%(t 2.41, 뉴이-웨스트 2.76), walk-forward 4/5 양수, 이웃 6/6·샤프 비 0.77. 시총가중 +10.02%(비 0.64), 크기 분위 고름, 용량 10% 한도 32억. 판정 구간을 19에서 이미 봤으므로 소액 forward 검증으로 넘김 |
| [23_value_tilt_liquidity_floor/](23_value_tilt_liquidity_floor/README.md) | 스터디 22 후속 — 거래대금 하한을 30억·50억으로 올리고 walk-forward를 7창(2019~2025)·층 ② 6/7로 조여 다시 잼. 데이터·중심 칸·격자는 22와 같음 | **채택 후보** — 하한 30억: 비용 MID 초과수익 연 +13.48%(t 2.82, 뉴이-웨스트 3.28), 6/7창(2020만 음수), 이웃 6/6·샤프 비 0.86, 시총가중 +7.72%, 용량 96억. 하한 50억: +10.76%(t 2.20), 6/7, 6/6·0.81, 용량 158억. 22의 초과수익은 거래 얇은 종목 의존이 아님. 중심 칸을 19·22 보고 골랐으니 확정은 모의 소액 forward에서 |

## 계열 B — 지수레벨 위기대응

> 계열 A와 **엔진·데이터가 다른 독립 계열**이다. 엔진 무관 단독 실행 스크립트로 yfinance 무키 **대표 지수**를 굴려 위기 국면의 익스포저 토글을 검증한다(개별종목·수급·survivorship 미반영, 룩어헤드 차단). 엣지 발견이 아니라 **인과적 스트레스테스트**로 읽는다.

| 스터디 | 무엇 | 한 줄 요지 |
|---|---|---|
| [07_crisis_regimes/](07_crisis_regimes/README.md) | 위기 17건 지수레벨 특성화 | speed(fast/slow)×shape(V/U/L) 거동 그리드로 분류 — 표본 n≈17·셀당 1~3개라 셀단위 우열비교는 무의미, hindsight 라벨은 신호 아님(기술통계만). |
| [08_crisis_response/](08_crisis_response/README.md) | 위기 인과적 대응 5종(M1~M5) | t-1 정보로만 익스포저 산출 → 종가-종가 구조상 **당일 급락 몸통은 못 막고 꼬리만** 자름. 낙폭이 아니라 전 구간(위기+정상+회복) 순효과로 평가. |
| [09_crisis_strategies/](09_crisis_strategies/README.md) | 위기 전략 10종(방어5+공세5) | 동일 규율로 확장·홀드아웃(2022) 잠금. 매수 후 보유 대비 결정적 우위는 없고 진짜 유효 신호는 소수(O3 SOX 선행 등) — 순위 요약은 [SUMMARY_RANKING.md](09_crisis_strategies/SUMMARY_RANKING.md). |

> 04/05 공백은 계열 A와 동일하게 **의도적**(summary만 남아 폴더 미승격).

## 재생성
```bash
# 01~03 (raw/bt_*.csv → 각 스터디 폴더)
python3 render_studies.py
# 06_bear_market (raw/*_trades.csv → events/ 트리)
cd 06_bear_market && python3 render_ledger.py
```
`raw/`(원본 trade CSV)는 gitignore.

← [research 허브](../README.md) · [백테스트 목록](../BACKTESTS.md)
