# 재무·회계 팩터 슬리브 — 사전등록 스펙 (리셋 회의 2라운드, 2026-09-19 밤, 작성 09-20 00시)

> 작성: 재무·회계 팩터 리서처(fundamental-quant). 읽은 것: `research/COUNCIL_CHARTER.md`, `research/RESET_2026-09-19.md` §1-2·§C·§D·§3.
> 이 문서는 회의록이 아니라 `research/studies/19_fundamental_value/`를 짤 때 그대로 옮기는 사전등록 스펙이다.
> 숫자는 결과를 보기 전에 정했고, 결과가 나온 뒤에는 이 파일을 고치지 않는다(고치면 새 번호 스터디로 간다).
> 약어 대신 풀어 쓴다: 시점 고정(과거 어느 날에도 그날 알 수 있던 값만 쓰는 것), 과거분 채우기(옛 데이터를 한 번에 받아 두는 것).

## 1. 지금 상태 진단

- 재무 데이터가 저장소에 한 행도 없다(`PYQuant/data/fin/`은 09-19 23:57 빈 폴더). 일봉은 1990년부터 상폐 포함 1,310만 행(`PYQuant/data/bars_all_pit_v2.parquet`), 수급은 2009년부터(`PYQuant/data/investor_flow_pit.parquet`)가 있는데, 스터디 01~17 전부가 가격·거래량·지수만 썼다. 저PBR·자본수익률·발생액·실적 발표 뒤 표류처럼 40년 문헌이 있는 축을 한 번도 재지 않았다.
- 유니버스 파일에 재무 열이 없고(`PYQuant/tools/universe_feed.py` schema 1: 시총·거래대금뿐), 시가총액 이력·상장주식수 이력·업종 표도 없다. 그래서 장부가/시가(PBR)조차 과거 어느 날 값으로도 못 만든다.
- 시점 고정 조인 함수(`PYQuant/data/pit.py::as_of_join`, §C가 정본으로 지정)가 아직 없고 `PYQuant/data/asof.py`에는 기준일 하나만 있다. 재무는 기준일(3/31)이 아니라 공시일(5월 중순)부터 쓸 수 있으므로 이 함수 없이는 45~90일 미래 참조가 그대로 들어간다.

## 2. 공통 규약 (세 전략이 같이 쓴다)

### 2-1. 입력 파일과 열 이름

다른 에이전트가 `PYQuant/tools/dart_fin_history_fill.py`로 적재 중인 `PYQuant/data/fin/fin_point_in_time.parquet`는 아직 파일이 없어 열 이름을 확인하지 못했다. 아래는 §C 규약(`ticker, effective_date, published_at, value, grade`, 긴 형식, 추가만)에 DART 다중회사 주요계정(`fnlttMultiAcnt`) 응답 필드를 얹은 기대 스키마다. 적재 스크립트의 이름이 다르면 `PYQuant/features/fundamental.py::load_fin()` 한 곳에서만 바꿔 읽고, 스터디 코드는 아래 이름만 쓴다.

| 열 | 형 | 뜻 · DART 원 필드 |
|---|---|---|
| `ticker` | 문자 6자리 | 종목코드(`stock_code`) |
| `corp_code` | 문자 8자리 | DART 고유번호 |
| `fiscal_year` | 정수 | `bsns_year` |
| `report_code` | 문자 | `reprt_code`: 11013 1분기 / 11012 반기 / 11014 3분기 / 11011 사업보고서 |
| `period_end` | 날짜 | 보고 기간 말일(분기말) |
| `fs_div` | 문자 | CFS 연결 / OFS 별도 |
| `statement` | 문자 | `sj_div`: BS 재무상태표 / IS 손익계산서 |
| `account_name` | 문자 | `account_nm`: 자산총계·부채총계·자본총계·자본금·이익잉여금·유동자산·유동부채·매출액·영업이익·법인세차감전 순이익·당기순이익 |
| `amount` | int64 | `thstrm_amount`: 재무상태표는 기말 잔액, 손익은 당기 3개월(사업보고서는 연간) |
| `amount_cumulative` | int64 | `thstrm_add_amount`: 손익 누적(반기·3분기), 없으면 결측 |
| `published_at` | 날짜 | `rcept_dt` 접수일 |
| `effective_date` | 날짜 | `published_at` 다음 거래일(일봉 달력 기준). 접수 시각을 모르므로 당일은 쓰지 않는다 |
| `rcept_no` | 문자 | 접수번호. 정정 공시는 새 `rcept_no`·새 `published_at`으로 새 행, 옛 행은 지우지 않는다 |
| `grade` | 문자 | "A" |

같이 만들어야 하는 파일 둘(§4에 스크립트):

| 파일 | 열 | 소스 · 시점 고정 등급 |
|---|---|---|
| `PYQuant/data/fin/shares_point_in_time.parquet` | `ticker, published_at, effective_date, shares_common_issued, shares_preferred_issued, treasury_common, rcept_no` | DART 주식의 총수 현황(`stockTotqySttus`), 보고서마다 한 행. 등급 A |
| `PYQuant/data/fin/company_master.parquet` | `ticker, corp_code, corp_name, industry_code, fiscal_month, listed_market` | DART 기업개황(`company`)의 `induty_code`·`acc_mt`. 업종·결산월은 거의 안 바뀌므로 현재 스냅샷을 쓴다. 등급 C, 결과에 표기 |

시가총액: `market_cap = Close × shares_common_issued`(둘 다 그날 발효분). 2020년부터는 data.go.kr `mrktTotAmt`(`PYQuant/data/datagokr_source.py` 스냅샷)가 있으므로 2020년 한 해 겹치는 구간에서 두 값의 비가 0.98~1.02 안에 드는 종목이 95% 넘어야 2015~2019 구간을 등급 B로 쓴다. 넘지 못하면 일봉이 분할 수정 주가인지부터 확인한다(`PYQuant/tools/check_adjusted.py`).

일봉 열은 그대로: `Date, Open, High, Low, Close, Volume, code, name, market, delisted`. 거래대금은 `Close × Volume`으로 만든다(원 데이터에 거래대금 열이 없다).

### 2-2. 분기 값 만들기 [formula]

- 손익 분기 3개월값: 1분기 = 11013 `amount`; 2분기 = 11012 `amount`(단, `amount == amount_cumulative`면 누적으로 보고 1분기를 뺀다); 3분기 = 11014 같은 규칙; 4분기 = 11011 `amount` − 11014 `amount_cumulative`.
- 검산: 네 분기 합이 사업보고서 연간과 1% 넘게 어긋나는 회사-연도는 그 해 손익 항목을 결측으로 둔다(결측률을 `metrics.json`에 적는다).
- 최근 4분기 합(TTM): 같은 `fs_div`의 연속 4분기가 다 있을 때만. 연결이 있으면 연결, 없으면 별도. 한 종목이 어느 날부터 연결을 내기 시작하면 그 발효일부터 연결로 바꾸고, 바뀐 뒤 4분기가 찰 때까지 TTM 항목은 결측.
- 재무상태표 항목은 발효일 기준 최신 보고서의 기말 잔액. `자본총계 ≤ 0`은 어느 지표에도 넣지 않는다.
- 횡단면 정규화: 매 리밸 시점 유니버스 안에서 1%·99% 절단(윈저라이즈) 뒤 z-점수. 결측은 0으로 채우지 않고 그 종목을 그 달 복합에서 뺀다.

### 2-3. 유니버스 (리밸 시점마다 다시 계산)

| 규칙 | 값 | 근거 |
|---|---|---|
| 보통주만 | 코드 끝자리 `0`; 이름 토큰 `Quant/config/etf_name_tokens.json` + `스팩`·`리츠` 제외 | 우선주·ETF·스팩은 장부가 지표 의미가 다르다 |
| 상장 경과 | 첫 일봉부터 12개월 이상 | 4분기 재무가 차야 TTM이 나온다 |
| 유동성 | 20일 평균 거래대금 ≥ 10억원 | 저PBR 상위 분위가 거래대금 하위와 겹치는 체결 불가 알파를 걸러 낸다 |
| 크기 | 시가총액 ≥ 500억원 | 같은 이유. 크기별(시총 상위 200 / 나머지) 분해는 결과에 따로 적는다 |
| 거래정지 대리 | 최근 20거래일 중 거래량 0인 날 3일 이상이면 제외 | KIND 관리·정지 이력이 아직 없다(§6) |
| 업종 | 가치·퀄리티는 금융업(`industry_code` 앞 두 자리 64·65·66) 제외, 표류(PEAD)는 포함 | 은행·보험의 자본·이익 정의가 다르다 |
| 결산월 | `fiscal_month ≠ 12`는 1차 스터디에서 제외(약 2%) | 분기 차분 규칙이 어긋난다 |
| 상장폐지 | `delisted` 종목 포함. 마지막 거래일 종가로 그 달 청산(수익률 반영). 정리매매 구간은 거래량 조건으로 자연 탈락 | 생존 편향 제거 |

예상 크기: 월 1,100~1,400 종목(추정, 2026-09 기준 보통주 약 2,400 중 유동성·크기 통과분).

### 2-4. 리밸·수익률·비용

- 매월 첫 거래일 종가에 체결. 신호 기준일 `as_of` = 전월 마지막 거래일, 재무는 `effective_date ≤ as_of`인 최신 행(`as_of_join`).
- 월 수익률 = 종가 대 종가, 배당 제외(일봉에 배당이 없다. 고배당 가치주가 불리하게 잡히므로 결과에 "배당 제외, 보수적" 표기).
- 비용은 `PYQuant/backtest/costs.py`에 세 벌을 추가해 쓰고 스터디 안에 숫자를 두지 않는다: `RESEARCH_LOW`(수수료 0.015·매도세 0.20·슬리피지 0틱 = 왕복 0.23%), `RESEARCH_MID`(슬리피지 1틱 + 충격 0.05% 양쪽 ≈ 왕복 0.35%), `RESEARCH_HIGH`(≈ 0.55%). 판정은 MID, 세 벌 결과를 모두 적는다.
- 걸어 나가기(walk-forward): 검증 5창 = 2021·2022·2023·2024·2025 각 1년, 학습 = 직전 3년(복합 가중·종목 수만 학습창에서 고른다). 2016-07~2020은 첫 학습창, 2026-01~08은 관찰창(판정 밖). 2022 한 해 홀드아웃 재사용 금지.

## 3. 전략 후보 (우선순위 순)

### 후보 1 — 저PBR × 고ROE 복합 (가치·퀄리티)

| 항목 | 내용 |
|---|---|
| 가설 | 장부가 대비 싼 주식 가운데 자본수익률이 높은 것이 유니버스 동일가중을 비용 뒤에도 이긴다. 근거: Fama·French(1992) 가치, Asness·Frazzini·Pedersen(2019) 퀄리티, Novy-Marx(2013) 싸면서 수익성 높은 것의 결합. 한국은 2017~2020 저PBR 단독 부진과 2024 기업가치 제고 정책 뒤 회복이 알려져 있어 국면 의존을 5창에서 그대로 드러낸다(한국 실증은 이 스터디가 직접 잰다) |
| 유니버스 | §2-3, 금융업 제외 |
| 신호 [formula] | `bp = 자본총계 / market_cap`(자본총계는 최신 재무상태표 기말, 연결 우선). `roe = 당기순이익_TTM / 평균(자본총계_현재, 자본총계_4분기전)`. `composite_vq = 0.5·z(ln bp) + 0.5·z(roe)`. 둘 중 하나 결측이면 제외 |
| 포트폴리오 | 팩터 검정: `composite_vq` 5분위, 동일가중·시총가중 둘 다. 슬리브: 상위 30 동일가중, 월 리밸, 보유 종목은 상위 20%(약 60등) 안이면 유지(회전 버퍼) |
| 필요 열 | `fin_point_in_time`: `account_name ∈ {자본총계, 당기순이익}`, `amount, amount_cumulative, report_code, fs_div, effective_date`. `shares_point_in_time`: `shares_common_issued, effective_date`. 일봉: `Date, Close, Volume, code, delisted`. `company_master`: `industry_code, fiscal_month` |
| 합격(비용 MID 뒤, 사전등록) | ① 순위 IC(신호 대 다음 달 수익) 평균 ≥ 0.03, IC t ≥ 2.5(약 120개월), ICIR ≥ 0.5, 5분위 단조(스피어만 ≥ 0.8), Q5−Q1 동일가중 연율 > 0 이고 t ≥ 2.0, 시총가중 Q5−Q1도 > 0(동일가중만 통과면 "조건부"). 상위 30 슬리브 대 유니버스 동일가중 월 초과수익 t ≥ 2.0 ② 5창 중 ≥ 3창에서 Q5−Q1 > 0 ③ 종목 수 {20, 30, 40} × 가중 {0.3/0.7, 0.5/0.5, 0.7/0.3} × 보유 {1, 2, 3개월} 이웃 전부 > 0, 중심 샤프의 70% 이상. 편도 회전 월 ≤ 30%. 용량: 보유 종목 20일 거래대금 1% 합계 중앙값을 적는다(계좌 규모의 100배 이상이면 통과, §6) |
| 예상 회전 | 편도 8~15%/월(버퍼 적용, 추정) |
| 예상 표본 | 약 1,200 종목 × 122개월(2016-07~2026-08) ≈ 146,000 종목-월. 판정 창 60개월 |

### 후보 2 — 실적 발표 뒤 표류 (표준화 이익 서프라이즈, PEAD)

| 항목 | 내용 |
|---|---|
| 가설 | 분기 영업이익이 전년 동기 대비 크게 좋아진 종목은 공시 뒤 2~3개월 더 오른다. 근거: Bernard·Thomas(1989), Foster·Olsen·Shevlin(1984). 한국은 대형주가 잠정실적을 먼저 내서 정기보고서 시점엔 표류가 줄어 있을 수 있으므로, 크기별 분해가 핵심 관찰 |
| 유니버스 | §2-3, 금융업 포함. 추가로 "최근 63거래일 안에 `effective_date`가 있는 종목"만 그 달 후보 |
| 신호 [formula] | `oi_q` = 분기 3개월 영업이익(§2-2). `sue = (oi_q − oi_{q−4}) / 표준편차(oi_j − oi_{j−4}, j = q−1 … q−8)`. 8분기 이력 필요(2017-3분기부터 신호). 표준편차 결측·0이면 제외. `매출액_q ≤ 0`이면 제외 |
| 포트폴리오 | (가) 사건 연구: `effective_date` 종가에서 +63거래일까지 누적 초과수익(유니버스 동일가중 차감), SUE 5분위. (나) 월 포트폴리오: 후보 중 SUE 상위 20% 동일가중, 월 리밸, 보고서가 63일 넘으면 자동 탈락 |
| 필요 열 | `fin_point_in_time`: `account_name ∈ {영업이익, 매출액}`, `amount, amount_cumulative, report_code, fs_div, period_end, effective_date`. 일봉 같음. `shares`는 크기 분해에만 |
| 합격(비용 MID 뒤) | ① (가) Q5−Q1 63일 누적 초과수익 > 0, t ≥ 2.0. (나) 월 IC ≥ 0.03, ICIR ≥ 0.5, 상위 20% 대 유니버스 동일가중 월 초과 t ≥ 2.0 ② 5창 중 ≥ 3창 ③ 표준편차 창 {6, 8, 12}분기 × 보유 {42, 63, 84}일 × 분위 {15, 20, 25}% 이웃 전부 > 0. 편도 회전 월 ≤ 60%(월 50% 회전이면 연 비용 ≈ 2.1%, 이것을 넘는 초과수익이 있어야 한다) |
| 시점 등급 | 신호 시점 B: 정기보고서 접수일은 분기말 뒤 30~45일이라 잠정실적을 낸 대형주는 이미 반영돼 있다(보수적 편향, 표류를 덜 잡는 쪽). 2단계로 DART 공시목록(`list`, 공정공시 "영업(잠정)실적") 적재 뒤 같은 표본으로 재검정 |
| 예상 회전 | 편도 40~55%/월(추정) |
| 예상 표본 | 약 1,000 종목 × 4분기 × 9년 ≈ 36,000 사건, 월 후보 300~600 종목 |

### 후보 3 — 총이익/자산(GP/A) × 발생액 (전체 재무제표 적재 뒤)

| 항목 | 내용 |
|---|---|
| 가설 | 매출총이익 대비 자산이 큰 회사가 비싸지 않은 값에 거래되며, 이익 가운데 현금이 아닌 부분(발생액)이 큰 회사는 뒤에 뒤처진다. 근거: Novy-Marx(2013), Sloan(1996). 한국 발생액 이상현상은 이 스터디가 잰다 |
| 데이터 조건 | 주요계정 API에는 매출원가·영업활동현금흐름이 없다. 단일회사 전체 재무제표(`fnlttSinglAcntAll`)가 필요하다. 회사 2,800 × 사업보고서만 10년 = 약 28,000 호출(하루 20,000 한도, 2일). 분기까지는 4배라 연간부터 한다(Sloan도 연간) |
| 유니버스 | §2-3, 금융업 제외 |
| 신호 [formula] | `gpa = (매출액 − 매출원가) / 자산총계`(연간, 사업보고서 `effective_date`부터 12개월 유효). `accruals = (당기순이익 − 영업활동현금흐름) / 평균(자산총계_당기, 자산총계_전기)`. `composite_qa = z(gpa) − z(accruals)` |
| 포트폴리오 | 후보 1과 같다(5분위 + 상위 30 동일가중, 월 리밸, 버퍼). 신호는 연 1회 갱신이라 회전이 낮다 |
| 필요 열 | 새 파일 `PYQuant/data/fin/fin_full_point_in_time.parquet`(같은 긴 형식, `account_id` 열 추가: `ifrs-full_CostOfSales`, `ifrs-full_CashFlowsFromUsedInOperatingActivities`, `ifrs-full_Assets`, `ifrs-full_Revenue`, `ifrs-full_ProfitLoss`; 표준 계정 ID가 없는 회사는 `account_name` 문자열 매칭 뒤 등급 B) |
| 합격 | 후보 1과 같은 숫자. 편도 회전 월 ≤ 15% |
| 예상 회전 | 편도 5~10%/월(추정) |
| 예상 표본 | 약 1,200 종목 × 110개월(2017-04~) ≈ 132,000 종목-월 |

## 4. 첫 실행 단계 — `research/studies/19_fundamental_value/`

스터디 폴더 하나에 세 후보를 `--factor vq|pead|qa`로 둔다(같은 유니버스·같은 조인·같은 비용을 쓰므로 나누면 세 벌이 어긋난다). 파일과 역할:

| 파일 | 하는 일 |
|---|---|
| `PYQuant/data/pit.py` (신규) | `as_of_join(left, right, key="ticker", left_date="as_of", right_date="effective_date")`: 종목별로 `effective_date ≤ as_of`인 최신 행 하나를 붙인다. 정정 공시는 `published_at`이 늦은 행이 이긴다. 단위 테스트 `PYQuant/tests/test_pit_join.py`: 기준일 정렬 오류(3/31 값을 4월에 쓰면 실패)·정정 이중행·상폐 뒤 조인 없음 |
| `PYQuant/features/fundamental.py` (신규) | `load_fin()`(열 이름 매핑 한 곳), `quarterly_flows()`(§2-2), `ttm()`, `universe(as_of)`(§2-3), `compute_factors(as_of) -> DataFrame[ticker, bp, roe, sue, gpa, accruals, composite_vq, composite_qa, fin_effective_date, factor_as_of, grade]`. 백테스트와 08:20 장전 잡이 같은 함수를 부른다(§D-5). `--check`로 품질 게이트 5항목을 돌려 하나라도 미달이면 exit 1 |
| `PYQuant/tools/dart_shares_history_fill.py` (신규) | `stockTotqySttus` 2015~ 과거분 채우기, 출력 `shares_point_in_time.parquet`. 상폐 종목은 `corpCode.xml`에 종목코드가 비어 있으므로 KIND 상폐 목록의 회사명으로 `corp_code`를 찾는다(못 찾은 수를 로그에 남기고 등급 B) |
| `PYQuant/tools/dart_company_master_fill.py` (신규) | `company` API 2,800회, 출력 `company_master.parquet` |
| `PYQuant/backtest/costs.py` | `RESEARCH_LOW/MID/HIGH` 세 벌 추가(골든 테스트는 `LIVE`만 대조하므로 영향 없음) |
| `research/studies/19_fundamental_value/preregistration.md` | 이 문서 §2·§3의 숫자를 그대로 복사. 결과보다 먼저 커밋해 해시를 남긴다 |
| `research/studies/19_fundamental_value/run.py` | `--factor vq|pead|qa --cost low|mid|high --rebalance monthly --windows 2021-2025`. 출력: `metrics.json`, `quintile_curves.csv`(월 × 5분위 × 동일/시총가중), `walkforward.csv`, `holdings_YYYY-MM.csv`(라이브 비교용), `README.md`(`research/studies/_TEMPLATE.md` 8절) |

`metrics.json` 열(판정에 쓰는 것만, 세 후보 공통):
`factor, cost_spec, as_of, data_hash, months, universe_size_median, fin_coverage_pct, effective_date_missing, amend_duplicate_rows, ttm_mismatch_pct, ic_mean, ic_t, icir, quintile_annual_ew[5], quintile_annual_vw[5], spearman_monotonic, q5_q1_annual_ew, q5_q1_t_ew, q5_q1_annual_vw, top30_excess_monthly_mean, top30_excess_t, top30_sharpe, top30_max_drawdown, turnover_oneway_monthly, cost_drag_annual, capacity_krw_median, size_split{large,small}, walkforward[{validation_year, train_years, q5_q1_net, top30_excess_t, sign}], robustness{cell, value}[27], verdict{layer1, layer2, layer3, overall}, grade`.

품질 게이트(돌리기 전, §3 데이터 행): 유니버스-월의 재무 커버 ≥ 90%, `effective_date` 결측 0, 같은 `(ticker, fiscal_year, report_code, fs_div, account_name)`에 `rcept_no`가 같은 이중행 0, 네 분기 합 대 연간 어긋남 ≤ 1%인 회사-연도 ≥ 95%, 2020년 시가총액 대조 ≥ 95%.

실행 순서(명령은 저장소 루트에서):

```
py PYQuant/tools/dart_fin_history_fill.py --from 2015          # 진행 중(다른 에이전트)
py PYQuant/tools/dart_company_master_fill.py
py PYQuant/tools/dart_shares_history_fill.py --from 2015
py PYQuant/features/fundamental.py --check                     # 품질 게이트, exit 0이어야 다음
py research/studies/19_fundamental_value/run.py --factor vq --cost mid
py research/studies/19_fundamental_value/run.py --factor pead --cost mid
```

속도 목표는 §3 하네스 행 그대로: 전 종목 10년 단일 팩터 ≤ 10초, 27셀 × 5창 ≤ 60초(월 122개 횡단면이라 일 단위 전수 스캔을 하지 않으면 된다). 첫 결과 목표일: 후보 1·2는 재무 적재 끝난 다음 날, 후보 3은 전체 재무제표 2일 적재 뒤.

## 5. 라이브 배선 — 별도 프로세스·별도 계좌를 추천한다

추천은 하나다: 월 리밸 슬리브는 `PYQuant/live/factor_sleeve.py`(파이썬, KIS REST, 월 첫 거래일 08:40 주문 산출·09:00 시장가 30건, 자기 원장 `PYQuant/data/sleeve/ledger.csv`)로 따로 돌리고, 계좌도 장중 엔진과 나눈다. 이유 셋. 첫째, 5-스레드 엔진은 15:15 마감 청산과 이월 0을 목표로 돌고(`research/RESET_2026-09-19.md` §A, `market_close_exit_hhmm` 1515) 승계 보유분은 ITB 청산 관리가 팔아 버리므로, 같은 계좌에 한 달 들고 갈 종목을 두면 엔진이 그날 청산한다. 둘째, 월 30건 주문에 초당 한도·샤딩·틱 캡처가 필요 없고, 리스크 규칙(일 손실 한도·슬롯)도 일 단위 회전용이라 슬리브의 낙폭 규칙(12개월 유니버스 대비 −10%p면 정지)과 다르다. 셋째, 메모리의 다계좌 원칙(계좌당 프로세스, 엔진 안 다계좌 금지)과 같다. §D-5의 유니버스 파일 점수 열은 그대로 한다. 같은 `compute_factors(as_of)`가 08:20에 `composite_vq`·`sue`를 schema 2 열로 얹고 `Features::external`·`score_w_external`(판정 전 0)로 장중 엔진의 진입 우선순위에만 기울인다. 이것은 부산물이지 슬리브의 실행 경로가 아니다. 슬리브 예산은 계좌의 30% 상한·종목 30 동일가중·종료 조건은 12개월 또는 스터디 기각 중 먼저 오는 것으로 사전등록한다.

## 6. 자기 영역 밖에서 본 문제

1. `research/RESET_2026-09-19.md` §3 횡단면 팩터 행의 용량 조건 "20일 거래대금 1% ≥ 100억"은 보유 종목 합계로 읽어도 평균 거래대금 200억 종목 50개가 필요해 코스닥 대부분이 빠진다. 계좌 규모(수천만 원대)의 100배를 용량 기준으로 바꾸는 것을 제안한다. 위 §3에는 그렇게 적었다.
2. `docs/DATA_SOURCES.md` 53행은 아직 DART 키 미발급으로 적혀 있다. 키가 나왔으니 §C 표와 같이 고쳐야 훅이 낡은 문단을 잡지 않는다.
3. `PYQuant/data/pit_universe/`에 §C가 말한 `delisted.csv`가 없고 2025-09 이후 일별 json만 있다. 일봉 v2의 `delisted` 참/거짓만으로는 상폐 사유(합병·자진·부실)를 못 가르므로, KIND 상폐 목록 적재(`PYQuant/tools/pit_universe_backfill.py` 확장)가 후보 1·3의 생존 편향 제거에 먼저 필요하다.
4. `PYQuant/backtest/costs.py::LIVE`는 슬리피지 0틱이다. 라이브 원장과 맞추는 데는 맞지만 백테스트가 이것을 그대로 쓰면 비용을 낮게 잡는다. §2-4의 세 벌을 같은 파일에 두고 스터디가 `LIVE`를 직접 쓰지 못하게 하는 검사 한 줄을 `scripts/check_backtest.py`에 넣기를 제안한다.
5. 수급 데이터(`investor_flow_pit.parquet`, 2009~)와 재무를 같은 `as_of_join`으로 붙이면 "외국인 순매수 지속 × 저PBR" 교차표가 바로 나온다. D-014 수급 무효 판정은 이 표본으로 다시 재는 것이 §D-3 계획과 맞다.
6. 월요일 config 판정(§3 "이월 0")과 여기 슬리브(한 달 보유)는 서로 다른 계좌라야 한 판정 표에 같이 들어갈 수 있다. 계좌 분리 여부는 오너 결정 항목이다.
