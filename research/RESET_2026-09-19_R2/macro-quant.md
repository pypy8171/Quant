# 거시 오버레이 — 2라운드 사전등록 스펙 (macro-quant, 2026-09-20)

> 1라운드 결론 `research/RESET_2026-09-19.md` §D-4("거시는 종목 선택이 아니라 사이징 오버레이")를 숫자와 시리즈 id로 옮긴 문서다.
> 오너 지시(09-19 밤): "FRED·ECOS 키를 발급받게 해 놓고 왜 쓴 게 없나. 금리·인플레이션·환율·PPI·GDP·CPI·수출·실업률을 전략에 넣어라."
> 적재는 `PYQuant/tools/macro_ingest.py`(다른 에이전트 진행 중) → `PYQuant/data/macro/<source>_<series>.parquet`
> (`observation_date, published_at, value, grade`). 이 문서는 그 parquet를 **읽는 쪽**의 규칙이다. 키 값은 어디에도 적지 않는다.

## 1. 진단

1. **지금 국면 파일은 거시가 아니라 "간밤 등락표"다.** `PYQuant/tools/macro_regime_feed.py`의 8지표(코스피·코스닥·나스닥·S&P·10Y·VIX·원달러·WTI)는 전부 하루 등락률의 ±1·±2 표결이고, 임계(`HALT_SCORE −7`·`LIQ_SCORE −11`·`warn/strong`)는 코드 주석대로 "관측 4거래일·토글 6회" 근거의 잠정값이다. `logs/regime_history.jsonl`은 09-11~09-18 **6거래일 1,085행**, 그 안에서 국면 라벨이 17번 바뀌었고 `entry_halt` 9행 — 검증 표본이 아니라 관측 로그다. 금리·물가·수출·실업률·GDP는 어느 항에도 없다.
2. **시점 고정이 아닌 것**: (a) 원/달러는 한국 장과 같은 세션이라 09:00 게이트로 새어 든다(코드 주석 D-033이 스스로 적음). (b) FRED 일별 시리즈를 FDR로 받으면 현재 수정본 하나뿐이라 과거 재구성이 안 된다. (c) 스터디 07의 `KRW=X`는 2007-08~2023-06만 있어 1997·2000·2008 IMF·닷컴·금융위기 셀이 NA다(`research/studies/07_crisis_regimes/README.md` 커버리지 표). 셋 다 `published_at` 열이 있는 parquet로 바꾸면 사라진다.
3. **검증된 것이 없다.** 스터디 10(구조 국면 v0)은 "미판정"이고 후속 A-1 순열검정은 BEAR 라벨 뒤 20일 수익이 오히려 +0.27%p, p=0.61(검출 한계 1.6%p) — 지수 위치 축 하나로는 분리력이 없다. 스터디 08의 코스피 단일 곡선(1996-12~2026-08, 7,305봉)에서 200일선 게이트(M1)는 MDD 20.2%p를 줄이는 대신 **연 3.54%p를 반납**(방어효율 5.7), 변동성 타게팅(M5)도 연 3.13%p 반납, VIX 게이트(M3)는 낙폭 축소 0. 즉 "가격 파생 지표만으로 켜는 방어"는 낙폭은 줄여도 수익을 되돌려준다는 것이 이미 세 번 나왔다. 성장·물가·유동성처럼 가격보다 **느리고 독립된 축**을 넣어야 방어효율 분모(반납)가 줄어드는지가 이번 검정의 핵심 질문이다.

## 2. 사전등록 스펙 — 네 축 → 국면 → 배수

### 2-1. 공통 규칙

- **결정 시각** `t` = 한국 거래일 D 08:50 KST. 시리즈 값은 `published_at ≤ t`인 행 중 `observation_date`가 가장 늦은 것 하나만 쓴다. `observation_date`로 정렬하지 않는다. 미국 지표(08:30 ET 발표 = 21:30/22:30 KST, 시장 종가 16:00 ET = 05:00/06:00 KST)는 이 규칙만으로 다음 한국 세션에 들어온다(GUARDRAILS ③ 별도 처리 불필요). `published_at`은 UTC 저장.
- **변환** 월간 시리즈: `yoy = x_t/x_{t−12m} − 1`, `mom3a = (x_t/x_{t−3m})^4 − 1`, `chg6 = x_t − x_{t−6m}`. 일간: `ret60 = x_t/x_{t−60d} − 1`, `chg60 = x_t − x_{t−60d}`. 시차의 기준도 `published_at ≤ t`인 행이다(월간 `t−12m`은 12행 앞이 아니라 관측월 12개월 앞).
- **표준화** `z = clip((f − median_W) / (1.4826·MAD_W), −3, +3)`. W = 월간 120관측(10년)·일간 2,520관측. 최소 36개월/250일 미만이면 NaN. 창은 `t` 이전 값만으로 확장(expanding→rolling).
- **축 점수** `A = Σ w_s·sign_s·z_s / Σ abs(w_s)` — NaN인 시리즈는 분모에서도 뺀다. 유효 시리즈 2개 미만이면 축 NaN → 그 축은 0(중립)으로 두고 `axis_missing` 플래그를 남긴다.
- **등급 처리** A·B는 점수에 쓴다. C(현재값을 과거에 투영)는 백테스트 점수에 넣지 않고 표시만.

### 2-2. 지표 표

| 지표 | 소스·시점 고정 등급 | 발표 주기·지연 | 축(부호) | 점수화 규칙(변환·가중) |
|---|---|---|---|---|
| 미국 산업생산 `INDPRO` | ALFRED vintage(**A**) | 월, 익월 중순 09:15 ET | 성장(+) | yoy z, w=1 |
| 미국 실업률 `UNRATE` | ALFRED(**A**) | 월, 첫 금요일 08:30 ET | 성장(−) | `u3 = 3개월 평균`, `f = u3 − min(u3, 12m)`(샴 룰 변형) z, w=1 |
| 신규실업수당 `ICSA` | FRED(**A**, 수정 미미) | 주, 목 08:30 ET | 성장(−) | 4주 평균 yoy z, w=1 |
| 10Y−2Y `T10Y2Y` | FRED(**A**) | 일 | 성장(+) | 수준 z, w=0.5 |
| 한국 수출 1·10·20일 잠정치(총액·반도체) | 관세청 보도자료 HTML(**B**, 잠정→확정 수정) | 11·21·익월 1일 09:00 KST | 성장(+) | 누적일수 같은 전년 구간 대비 yoy z, 총액 w=2·반도체 w=1. 확정치가 오면 잠정행은 그대로 두고 확정행을 추가(`grade` 열로 구분) |
| 한국 수출 월간 확정 | ECOS `403Y001` 수출총액 / data.go.kr 관세청 품목별(**B**) | 월, 익월 15일경 | 성장(+) | 잠정치와 같은 열의 확정행. 점수는 잠정치가 먼저 쓴다 |
| 반도체 지수 `^SOX` | `PYQuant/.index_cache/idx__SOX_*.parquet`(**A**, 가격) | 일 | 성장(+) | ret60 z, w=1 |
| 한국 GDP 분기 실질 | ECOS `200Y002`(코드는 `StatisticItemList`로 확정, **B**) | 분기, 속보 +25일 08:00 KST | 성장 표시만 | 점수 미투입(지연 4주+, 분기 4점/년으로 z 불가). 대시보드 값 |
| 미국 CPI `CPIAUCSL` | ALFRED(**A**) | 월, 익월 10~15일 08:30 ET | 물가(+) | `mom3a − yoy`(가속) z, w=1.5 |
| 미국 PPI `PPIACO` | ALFRED(**A**) | 월, CPI 다음 날 08:30 ET | 물가(+) | yoy z, w=1 |
| 10Y 기대인플레 `T10YIE` | FRED(**A**) | 일 | 물가(+) | chg60 z, w=1 |
| WTI `DCOILWTICO` | FRED(**A**) | 일(1~2일 지연) | 물가(+) | ret60 z, w=0.5 |
| 한국 CPI | ECOS `901Y009` 총지수(**B**, 통계청 익월 초 08:00 KST) | 월 | 물가(+) | `yoy − 0.02` z, w=1 |
| 한국 PPI | ECOS `404Y014` 총지수(**B**, 익월 20일경) | 월 | 물가(+) | yoy z, w=0.5 |
| 연방기금금리 `FEDFUNDS` | FRED(**A**) | 월 | 유동성(−) | chg6 z, w=1 |
| 미국 2Y `DGS2` | FRED(**A**) | 일, H.15 익일 | 유동성(−) | chg60 z, w=1 |
| 미국 M2 `M2SL` | ALFRED(**A**) | 월, 익월 넷째 화 13:00 ET | 유동성(+) | yoy z, w=1 |
| 달러지수 `DTWEXBGS` | FRED(**A**, 2006~) | 일, 1주 지연 | 유동성(−) | ret60 z, w=1. 2006 이전은 `DX-Y.NYB` 캐시(**B**)로 잇고 `grade`에 표시 |
| 한은 기준금리 | ECOS `722Y001` 항목 `0101000`(**A**, 금통위 당일 10:00 KST) | 연 8회 | 유동성(−) | chg6 z, w=1 |
| 국고채 3년 | ECOS `817Y002` 항목 `010200000`(**A**, 당일 16:30 KST) | 일 | 유동성(−) | chg60 z, w=1 |
| 원/달러 | ECOS `731Y001` 항목 `0000001` 매매기준율(**A**) | 일 | 유동성(−) | ret60 z, w=1. 매매기준율은 **전일 거래 가중평균을 당일 08:30에 고시**하므로 `published_at = observation_date 08:30 KST`, 결정 시각 08:50에서 쓸 수 있는 값은 사실상 전일 시세 |
| 한국 M2 | ECOS `101Y003` 평잔 계절조정(**B**, 익익월 중순) | 월 | 유동성(+) | yoy z, w=0.5 |
| VIX `VIXCLS` | FRED(**A**) | 일 | 위험선호(−) | 수준 z, w=1 |
| HY 스프레드 `BAMLH0A0HYM2` | FRED(**A**, 1996-12~) | 일, 익일 | 위험선호(−) | 수준 z, w=1 |
| 코스피 200일선 위치 | `idx__KS11` 캐시(**A**) | 일 | 위험선호(+) | `close/MA200 − 1` z, w=1 — 스터디 10 v0의 축1을 여기로 흡수 |
| 외국인 순매수 20일 | `PYQuant/data/investor_flow_pit.parquet` `foreign_net_quantity×close` 코스피 합(**A**, 2019~) | 일 | 위험선호(+) | 20일 합 / 20일 거래대금 합 z, w=1. 2019 이전은 NaN |
| 기존 8지표 등락 점수 | `logs/regime_history.jsonl` `risk_score/18`(**C**, 09-11~) | 3분 | 위험선호(+) | 라이브 표시 전용, 백테스트 미투입 |

미국 지표를 한국 시장에 쓰는 근거는 두 가지다. ① 수출의 최대 변수는 미국 소비·투자 사이클이고 코스피 수익률 분포의 꼬리는 스터디 07의 17개 위기 중 14개가 미국발 또는 글로벌 위험선호 충격이다(`^GSPC` 대표 사건에 코스피가 동행). ② 달러 유동성(연준 금리·달러지수·HY 스프레드)은 외국인 수급의 선행 변수이고, 외국인 순매수와 원/달러의 동행은 라이브 등락표가 이미 원달러를 −1표로 쓰는 이유다. 한국 지표는 성장 축에 수출 잠정치(가장 빠른 월중 지표, 가중 2), 유동성 축에 기준금리·국고채 3년·원달러, 물가 축에 한국 CPI·PPI를 넣는다. 한국 GDP·실업률은 지연이 커서 표시만 한다.

### 2-3. 국면 판정과 배수

- 축 상태 `s_G, s_P ∈ {+, −}`는 데드밴드·이력 규칙으로만 바뀐다: `+`에서 `−`로는 `G < −δ`, `−`에서 `+`로는 `G > +δ`(δ = 0.25). 초기값은 부호. 물가도 같다.
- 국면(월간, G·P를 갱신하는 발표가 들어온 다음 거래일 08:50에 재판정):

| `s_G` | `s_P` | 국면 | 기본 배수 `base` |
|---|---|---|---|
| + | − | 확장 | 1.00 |
| + | + | 과열 | 0.75 |
| − | + | 수축 | 0.50 |
| − | − | 회복 | 0.75 |

- 일간 조정(위험선호 R·유동성 L, 매일 08:50):
  `m_R = 1.0 (R ≥ −1) / 0.75 (−2 ≤ R < −1) / 0.5 (R < −2)`, `entry_halt = (R < −2.5)`.
  `m_L = clip(1 + 0.1·min(L, 0), 0.8, 1.0)` — 유동성은 조이기만 한다.
  **최종** `macro_scale = round(clip(base · m_R · m_L, 0, 1), 2)`(0.05 단위).
- `force_liquidate`는 이 오버레이가 **내지 않는다**. 강제 청산은 `LossLadder`(§E)만 소유한다. 현 등락표의 `LIQ_SCORE −11`은 폐기.
- 배선: `regime.json`에 `macro_regime`(확장/과열/수축/회복)·`macro_axes {G,P,L,R}`·`macro_scale`·`macro_apply(bool)`를 추가. 판정 전(`macro_apply=false`)엔 엔진이 `entry_scale`에 곱하지 않고 로그 `[매크로]`에만 적는다. 판정 통과 뒤 `entry_scale = min(기존 등락표 scale, macro_scale)` → `OrderGate::set_entry_scale`. 전략별 슬롯 수는 `slots × macro_scale` 내림(수축 20→10).

### 2-4. 검증 설계와 합격 숫자

- **데이터**: 코스피 `PYQuant/.index_cache/idx__KS11_1996-01-01_2026-08-15_adj.parquet`(1996-12~, 7,305봉) 단일 곡선 + 2019~는 `PYQuant/data/bars_all_pit_v2.parquet` 동일가중 유니버스(거래대금 ≥10억)와 §D-1 저변동성 바스켓 둘 다. 노출 `e_D = macro_scale(D−1 08:50)`, D 시가 체결, 노출 변경분에 비용 0.23%.
- **조건부 분포**: 월수익을 국면별로 나눠 확장−수축 차이의 t(Newey-West 3), 국면별 MDD·하위 5% 분위, 전환 횟수/년, 스터디 07 이벤트별 고점→첫 수축·과열 라벨 지연(일)·저점→첫 회복·확장 라벨 지연(일) 중앙값.
- **walk-forward**: 1999~2025 연도 창 27개(각 창은 그 앞 3년을 학습 창으로 두되 **중심 파라미터는 고정**: δ 0.25, W 120, 수축 0.5). 격자는 강건성 공개용 — δ {0.15, 0.25, 0.35} × W {96, 120, 144} × 수축 {0.4, 0.5, 0.6} 27셀 전량 공개, `trials_prior = 27`.
- **합격(§3 "국면 배수" 행 + 스터디 08 교훈)**: ① 확장−수축 월수익 t ≥ 2.0 ② 오버레이 적용 후 매수 후 보유 대비 연도 창 27개 중 ≥16에서 비용 뒤 Calmar 개선 ③ 중심의 ±1 이웃 6셀 전부 같은 부호 ④ MDD 상대 감소 ≥20%(코스피 −64.7% → −51.8% 이하) ⑤ **비용 뒤 CAGR 반납 ≤ 1.0%p/년**(스터디 08 M1 3.54·M5 3.13이 실패선) ⑥ 전환 ≤6회/년 ⑦ 비용 뒤 월 초과수익 t ≥ 1.5(Deflated Sharpe p < 0.05는 27셀 기준). ①~⑦ 전부 통과 시 `macro_apply=true`. ④⑥은 통과하고 ⑤만 미달이면 사전등록된 축소판 하나(수축 0.5만 적용, 나머지 1.0)를 한 번 더 판정하고 그것도 미달이면 표시만.
- 2019~ 바스켓 창(7년)은 표본이 짧아 "방향 관찰"까지만 적고 채택 근거로 쓰지 않는다.

## 3. 발표 이벤트 정지(`event_halt`)

한국 정규장(09:00~15:30)·실계좌 애프터마켓(16:00~20:00) 안에 떨어지는 발표만 시간 정지한다. 미국 지표는 전부 장 밖(08:30 ET = 21:30/22:30 KST, FOMC 14:00 ET = 03:00/04:00 KST)이라 시간 정지가 아니라 **서프라이즈 조건 정지**로 다룬다.

| 이벤트 | 발표 시각(KST) | 정지 창 | 캘린더 소스(엔드포인트) |
|---|---|---|---|
| 한은 금통위 기준금리 결정 | 연 8회, 약 09:50~10:00(기자회견 11:10) | 09:45~10:30 | `https://www.bok.or.kr/portal/singl/crncyPolicyDrcMtg/listYear.do?menuNo=200755&mtgSe=A`(HTML, 연간 일정) — ECOS OpenAPI에는 공표일정 서비스가 없다(`StatisticTableList·StatisticItemList·StatisticSearch·KeyStatisticList·StatisticMeta·StatisticWord`뿐) |
| 산업부·관세청 월간 수출입동향 | 매월 1일 09:00(휴일이면 다음 영업일) | 08:45~09:30 | 규칙 생성 + `https://www.customs.go.kr/kcs/na/ntt/selectNttList.do?bbsId=1362&mi=2891` 보도자료 목록 제목 "수출입 현황"으로 확인 |
| 관세청 1~10일·1~20일 잠정치 | 11일·21일 09:00(휴일이면 다음 영업일) | 08:45~09:30 | 같음 |
| 중국 국가통계국 PMI | 월말 10:30 | 10:15~11:00 | `https://tradingeconomics.com/china/calendar`(HTML 200 확인, RESET §1-4) |
| 중국 산업생산·소매판매 | 월중 11:00 | 10:45~11:30 | 같음 |
| Caixin 제조업 PMI | 1영업일 10:45 | 10:30~11:15 | 같음 |
| 통계청 CPI·한국 GDP 속보 | 08:00 | 정지 없음(개장 전) — 서프라이즈 표시만 | `https://tradingeconomics.com/south-korea/calendar` |
| 일본은행 결정 | 정오 전후 불규칙 | 정지 없음, 당일 `m_R` 한 단계 하향 플래그만 | 같음(japan) |
| 미국 CPI·고용·PPI·GDP·FOMC | 장 밖 | 서프라이즈 `abs(actual − consensus) / sd(최근 24회 서프라이즈) ≥ 2` 또는 FOMC 결정이 컨센서스와 다르면 **다음 세션 09:00~09:30 정지** | 날짜: `https://api.stlouisfed.org/fred/releases/dates?file_type=json&include_release_dates_with_no_data=true&realtime_start=<오늘>`(release_id 10 CPI·50 고용·46 PPI·53 GDP·13 산업생산·21 M2, 시각은 규칙 08:30/09:15/13:00 ET). FOMC: `https://www.federalreserve.gov/monetarypolicy/fomccalendars.htm`. 컨센서스·실제치: tradingeconomics `united-states/calendar` |

- 정지 창은 `PYQuant/data/macro/calendar.json`에 `{event, at_utc, halt_from_utc, halt_to_utc, consensus, previous, actual, surprise_z}`로 야간 생성(`scripts/event_feed.py`, RESET §C). 엔진은 `regime.json`의 `event_halt_until`(ISO 시각) 한 필드만 읽어 `OrderGate::set_entry_halt`와 같은 경로로 켠다. 보유분 청산·손절은 정지 창에도 그대로 나간다(신규 진입만 멈춤).
- 옛 값 보관: 발표 뒤 `actual`을 채운 행은 덮어쓰지 않고 `published_at`을 붙여 추가한다(캘린더 자체도 시점 고정).

## 4. 첫 실행 단계 — `research/studies/20_macro_overlay/`

순서: `PREREG.md`(이 문서 §2·§3을 그대로 옮기고 격자·`trials_prior=27` 명시) 커밋 → 스크립트 → 산출물. `scripts/check_backtest.py`가 해시 시각 < 산출물 mtime을 검사한다.

| 스크립트 | 입력 | 출력 |
|---|---|---|
| `build_axes.py` | `PYQuant/data/macro/*.parquet`, `PYQuant/.index_cache/idx__KS11_1996-*.parquet`·`idx__SOX_1985-*`·`idx__VIX_2000-*`, `PYQuant/data/investor_flow_pit.parquet` | `out/axes.parquet`(`decision_ts, G, P, L, R, s_G, s_P, regime, base, m_R, m_L, macro_scale, n_series_G..R, axis_missing`) — `decision_ts`마다 `published_at ≤ decision_ts` 필터를 다시 건다(`PYQuant/data/pit.py::as_of_join`) |
| `overlay_backtest.py` | `out/axes.parquet`, 바스켓 수익(코스피 / 동일가중 / 저변동성) | `metrics.json`: `basket, cell, cagr_bh, cagr_ov, mdd_bh, mdd_ov, mdd_rel_reduction, sharpe_bh, sharpe_ov, calmar_bh, calmar_ov, excess_month_t, exp_minus_con_t, windows_pos_of_27, transitions_per_year, cagr_giveback_pp, lag_peak_days_median, lag_trough_days_median, grid[27]{delta,W,base_con,calmar_ov}, neighbors_same_sign, dsr_p, cost_bp, trials_prior, data_grades{series:grade}, input_sha256` |
| `regime_history_backfill.py` | `out/axes.parquet` | `out/regime_band_1996_2026.csv`(대시보드 국면 띠) |

- 적재 명령(키는 환경변수 `FRED_API_KEY`·`ECOS_API_KEY`·`TRADEDATA_API_KEY`, 값은 문서에 안 적는다):
  `py PYQuant/tools/macro_ingest.py --source fred --vintage --series CPIAUCSL,PPIACO,UNRATE,INDPRO,M2SL`(ALFRED `fred/series/observations?output_type=2&realtime_start=1990-01-01`, 열마다 vintage 날짜 → `published_at`)
  `py PYQuant/tools/macro_ingest.py --source fred --series ICSA,T10Y2Y,T10YIE,DCOILWTICO,FEDFUNDS,DGS2,DTWEXBGS,VIXCLS,BAMLH0A0HYM2`(수정 없는 시리즈, `published_at = observation_date 익영업일 16:15 ET`)
  `py PYQuant/tools/macro_ingest.py --source ecos --series 722Y001:0101000:D,817Y002:010200000:D,731Y001:0000001:D,901Y009:0:M,404Y014:*AA:M,101Y003:BBHS00:M,403Y001:*:M`(`StatisticSearch/{key}/json/kr/1/10000/{표}/{주기}/{시작}/{끝}/{항목}`; 항목 코드는 첫 실행에서 `StatisticItemList`로 대조해 `out/ecos_items.json`에 남김)
  `py PYQuant/tools/macro_ingest.py --source tradedata --from 2015-01`(관세청 보도자료 HTML 표 → 잠정치 총액·반도체, `published_at` = 게시 시각)
- **지금 있는 데이터만으로 오늘 돌릴 수 있는 셀** `cell=market_only`: R 축(VIX·코스피 200일선·2019~ 외국인)·L 축 일부(`DX-Y.NYB` 캐시·`KRW=X` 캐시, 등급 B)·G 축의 `^SOX` 만으로 `build_axes.py --cell market_only` → 코스피 1996~ 단일 곡선 `overlay_backtest.py --basket kospi`. 이 셀의 결과는 "가격 파생 축만"이라는 이름으로 적고 채택 근거로 안 쓴다 — 스터디 08이 이미 그 계열의 한계를 보였다. 네 축 셀은 parquet가 5영업일 연속 적재 성공한 뒤 돈다(RESET §C 승격 규칙).
- 실행 시간 목표: 축 계산 1996~ 월간 ≤5초, 27셀 격자 ≤60초(§3 하네스 행).

## 5. 대시보드 거시 패널 (`scripts/dashboard_server.py` `/api/state` `macro` 키, 읽기 전용)

데이터는 `PYQuant/data/macro/state.json`(적재 스크립트가 06:30·17:00에 발행)과 `regime.json`(3분)만 읽는다. 위젯 다섯.

| 패널 | 시리즈 | 갱신 | 판정 색 |
|---|---|---|---|
| 국면 판 | 네 축 점수 막대(−3~+3), 국면 라벨, `macro_scale`, `macro_apply`(표시만/적용) | G·P는 발표마다, L·R은 3분 | 확장 초록·과열 노랑·회복 파랑·수축 빨강. 축 막대는 abs(z)<1 회색, 1~2 노랑, ≥2 방향색(성장·유동성·위험선호는 + 초록/− 빨강, 물가는 반대) |
| 다음 발표 카운트다운 | 앞으로 5건: 이름·KST 시각·컨센서스·전월·정지 창 여부 | 1분 | 정지 창 안이면 빨간 테두리 + "신규 진입 정지 mm:ss" |
| 서프라이즈 | 최근 10건 actual/consensus/`surprise_z` | 발표마다 | abs(z) ≥2 빨강, 1~2 노랑 |
| 한국 수출 | 1·10·20일 잠정치 yoy, 반도체 yoy, 다음 잠정치 D-day | 11·21·1일 09:00 | yoy 부호색 |
| 시리즈 표·추이 | 지표 · 값 · `observation_date` · `published_at` · 등급 · z, 12개월 축 점수 스파크라인, 1996~ 코스피 위 국면 띠 | 표는 06:30·17:00, 띠는 주 1회 | 등급 A 초록·B 노랑·C 회색 |

기존 8지표 등락 패널은 위험선호 축 아래로 내려 보조 행으로 둔다(라벨 RISK_ON/OFF는 유지, 엔진 로그 호환).

## 6. 자기 영역 밖에서 본 문제

1. **`force_liquidate`를 등락표가 소유한다.** `PYQuant/tools/macro_regime_feed.py` `LIQ_SCORE −11`이 참이면 C++가 보유 전량을 시장가로 판다(코드 주석 자인). 하루 등락률 8개의 합으로 전량 청산을 결정하는 구조는 §E `LossLadder`와 이중 소유다. 오버레이 판정과 무관하게 이 필드를 등락표에서 먼저 뺀다.
2. **저변동성 바스켓(§D-1)과 이 오버레이는 같은 방향의 방어를 두 번 센다.** 저변동성은 수축·과열에서 앞서고 회복에서 뒤진다. 오버레이 검정은 동일가중 유니버스에서도 같이 돌려야 MDD 감소가 어느 쪽 몫인지 갈린다(§2-4에 넣었다).
3. **원/달러 매매기준율은 "당일" 값이 전일 시세다.** 08:30 고시를 당일 종가처럼 쓰면 유동성 축이 하루 앞서 보인다. `published_at` 규칙으로 막았지만 대시보드 "현재 환율"은 KIS·네이버 실시간으로 따로 붙여야 한다.
4. **Track B(장중 국면) forward 적재가 08-25 하루로 끝났다.** `PYQuant/data/index_intraday/index_2026-08-25.jsonl` 한 파일뿐. 장중 국면 검증은 이 적재 없이는 시작도 못 한다 — `scripts/eod_data_nightly.py`에 같이 태우거나 지운다.
5. **`PYQuant/data/bars_all_pit.parquet` v1의 `name` 열이 깨져 있다**(cp949 → 표시 불가 문자). v2 검수 항목에 인코딩을 넣는다.
6. **RESET §D에서 유일하게 "통과"로 표시된 변동성 타게팅은 재판정 대상이다.** 스터디 08 코스피에서 M5는 연 3.13%p 반납·회복 참여율 중앙값 48%다. 헌장 3층 게이트(비용 뒤 t·창 과반·±1)로 다시 세지 않으면 통과 표시를 유지할 근거가 없다.
7. **파생상품 만기(둘째 목요일, 분기 만기 15:20 동시호가)** 는 거시 캘린더는 아니지만 같은 `event_halt` 경로로 14:30~15:30 신규 진입 정지를 걸 수 있다. 운영(마감 청산 15:15) 담당과 겹치는 창이라 거기서 정한다.
8. **D-014 "수급 무효"는 국면 조건부로 다시 봐야 한다.** 외국인 순매수의 정보량은 원화 약세·달러 강세 국면에서 다르다는 것이 상식적 가설이고, 이번 축 점수가 생기면 수급 팩터 스터디에 `regime` 열을 조건 변수로 붙일 수 있다.
