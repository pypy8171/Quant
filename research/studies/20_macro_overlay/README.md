# 거시 국면 오버레이 — 네 축 국면 배수를 코스피에 곱한다 (연구군: index-macro)

> 한 줄 요약: 성장·물가·유동성·위험선호 네 축 국면 배수를 코스피 매수 후 보유에 곱했더니 1999~2025 전 구간에서 낙폭은 거의 그대로(−56.0% → −55.3%)이고 연 1.35%p를 반납했다. 사전등록 7항목 중 2개만 통과. 상태: **초과수익 없음(`macro_apply=false`)** — 표시만 한다.
> 첫 실행(2026-09-20). 정본 스펙 [research/RESET_2026-09-19_R2/macro-quant.md](../../RESET_2026-09-19_R2/macro-quant.md), 사전등록 [PREREG.md](PREREG.md), 숫자 [metrics.json](metrics.json).

---

## 1. 사전등록

[PREREG.md](PREREG.md)에 결과 보기 전에 적었다. 합격 7항목·격자 27셀·`trials_prior=27`·스펙과 다르게 한 곳(첫 발표치 규약, ECOS 발표일 근사, ALFRED 판본 이전 구간 근사, WTI 캐시 대체, 평일 결정 달력) 전부 거기 있다.

## 2. 방법

- **점수 함수** `PYQuant/features/regime_axes.py::score(as_of, cell)` — 백테스트와 라이브가 같은 함수. 시리즈 행은 `published_at < D`인 것만 보이고 관측치마다 첫 발표치 하나만 쓴다. 테스트 `PYQuant/tests/test_regime_axes.py`가 as_of 뒤 발표 행을 7배로 바꾸거나 지워도 점수가 같은지 확인한다(두 셀 모두 통과).
- **노출** `e_D = macro_scale(D−1 08:50)`, D 시가 체결, 수익 = `e_D × (open_{D+1}/open_D − 1)`, 노출 변경분에 0.23%. 매수 후 보유는 비용 0.
- **룩어헤드 차단**: 결정일 D는 D 이전 발표만 본다(하네스가 `visible_from` 단조 배열로 막음) · 노출은 전 평일 결정값 · 거시는 신호 전용(수익곡선은 코스피 시가만) · 정규화는 창 W 안의 과거 값만(중앙값·MAD, 전표본 통계 없음).
- **결측**: 축의 유효 시리즈가 2개 미만이면 축 NaN → 중립(성장 +, 물가 −, 배수 1.0). `four_axis`는 2009-01-07부터 성장·물가 축이 켜진다(ALFRED·ECOS 적재가 2005부터라 12개월 yoy + 36개월 z 예열). 위험선호 축은 2000-12(VIX 250일 예열)부터, 그 전은 배수 1.0.

### 쓴 시리즈(20개)와 등급

| 축 | 시리즈(등급) |
|---|---|
| 성장 | UNRATE 샴 변형(A) · ICSA 4주 yoy(B, 2009 전 근사) · T10Y2Y 수준(B) · ECOS 수출 yoy(B, 발표일 +46일 근사) · SOX ret60(A) |
| 물가 | CPIAUCSL 가속(A) · PPIACO yoy(A) · CL=F ret60(B, WTI 대체) · ECOS CPI yoy−2%(B) |
| 유동성 | FEDFUNDS chg6(A) · DGS2 chg60(B) · 달러지수 ret60(B, 2006 전 DX-Y.NYB 비율 연결) · 한은 기준금리 6개월 변화(B) · 국고채 3년 chg60(B) · 원/달러 ret60(B) · ECOS M2 yoy(B) |
| 위험선호 | VIX 캐시 수준(A) · HY 스프레드(A, 2023-09~) · 코스피 200일선 괴리(A) · 외국인 20일 순매수/거래대금(A, 2019~, `PYQuant/data/macro/derived_foreign_flow_kospi.parquet`) |

`market_only` 셀은 SOX·달러지수 캐시·원달러 캐시·VIX·HY·200일선·외국인만(성장·물가 축 없음, 기본 배수 1.0 고정).

### 빠진 시리즈(스펙 27개 중 7개)

| 시리즈 | 축 | 이유 |
|---|---|---|
| INDPRO | 성장 | ALFRED 미적재 |
| 한국 수출 1·10·20일 잠정치 | 성장 | 관세청 data.go.kr 활용신청 대기 |
| T10YIE | 물가 | FRED 미적재 |
| DCOILWTICO | 물가 | FRED 미적재 — CL=F 캐시로 대신 |
| 한국 PPI 404Y014 | 물가 | ECOS 미적재 |
| M2SL | 유동성 | ALFRED 미적재 |
| 기존 8지표 등락 점수 | 위험선호 | 등급 C — 스펙대로 백테스트 미투입 |

## 3. 1차 결과 (1999-01-04~2025-12-30, 6,652일, 코스피, 비용 뒤)

| | 매수 후 보유 | `four_axis` 오버레이 | `market_only` 오버레이 |
|---|---:|---:|---:|
| CAGR | 7.67% | 6.32% | 6.66% |
| MDD | −56.0% | −55.3% | −55.3% |
| Calmar | 0.137 | 0.114 | 0.120 |
| 샤프(일간) | 0.43 | 0.41 | 0.41 |
| 평균 노출 | 1.00 | 0.80 | 0.93 |
| 누적 비용 | 0 | 12.2% | 17.8% |

사전등록 판정(`four_axis`):

| 번호 | 항목 | 기준 | 값 | 판정 |
|---|---|---|---:|---|
| ① | 확장 − 수축 월수익 t(NW3) | ≥ 2.0 | −0.39 | 미달 |
| ② | 연도 창 Calmar 개선 | ≥ 16/27 | 7/27 | 미달 |
| ③ | ±1 이웃 6셀 같은 부호 | 전부 | 전부 음(−) | 통과(개선 아님이 강건) |
| ④ | MDD 상대 감소 | ≥ 20% | 1.2% | 미달 |
| ⑤ | CAGR 반납 | ≤ 1.0%p/년 | 1.35%p | 미달 |
| ⑥ | 국면 전환 | ≤ 6회/년 | 2.35회 | 통과 |
| ⑦ | 월 초과수익 t · DSR p | ≥ 1.5 · < 0.05 | −1.77 · 0.98 | 미달 |

`market_only`도 같은 방향(⑤ 1.01%p 미달, ② 5/27, ④ 1.2%). 축소판(수축 배수만)은 "⑤만 미달" 조건이 아니라 돌리지 않았다.

## 4. 전체 격자 (27셀 × 2셀 = 54런, 전부 [metrics.json](metrics.json) `cells.<cell>.grid`)

δ {0.15, 0.25, 0.35} × W {96, 120, 144} × 수축 {0.4, 0.5, 0.6}. `four_axis` Calmar 0.095~0.116, `market_only` 0.115~0.120 — 54셀 전부 매수 후 보유 0.137 아래. 우연으로 이기는 셀조차 없다.

## 5. 진단 (판정 근거 아님)

- 국면별 월수익 평균(매수 후 보유, 2009~2025): 수축 +1.40% · 확장 +0.97% · 회복 +0.96% · 과열 −0.12%. 라벨이 수축일 때 수익이 가장 높다 — 월간 거시 발표의 지연이 저점 뒤에야 "수축"을 찍는다(2020-08·2022-11·2025-01이 그렇다). 전환 표는 `out/axes_four_axis.parquet`.
- 축이 켜진 2009-01-07 이후만 보면 MDD −43.1% → −26.0%, CAGR 7.70% → 5.59%, Calmar 0.179 → 0.215. 낙폭 방어는 있으나 연 2.1%p 반납 — 스터디 08 M1(3.54)·M5(3.13)보다 작을 뿐 합격선(1.0)은 못 넘는다.
- 전 구간 MDD가 안 줄어든 이유: 오버레이 최대 낙폭은 1999-07→2001-09(−55.3%)인데 그때는 위험선호 축이 예열 전(배수 1.0)이라 매수 후 보유와 같다. 2008년 창은 −51.6% → −36.9%.
- 비용의 몫: 일간 배수 `m_R·m_L`이 0.05 단위로 매일 흔들려 `market_only`에서 노출 변경 비용만 27년 누적 17.8%(연 0.66%p). 반납의 절반 이상이 회전 비용이다.

## 6. 편향 감사 (bias-auditor 대상)

① Look-ahead: 하네스가 구조로 막음 + 테스트 통과. 다만 ECOS 월간 발표일은 근사(B)라 ±며칠 오차는 남는다. ② Survivorship: 지수 단일 곡선, 해당 없음. ③ Hindsight: 파라미터·합격선은 스펙에서 왔고 결과 뒤 안 바꿈. ④ 과최적화: 격자 54셀 전부 공개, 전부 미달. ⑤ 표본: 네 축이 유효한 구간은 2009~2025(17년, 국면 전환 40회) — 수축 30개월. ⑥ 체결: 시가 체결·0.23%, 슬리피지 별도 없음(지수 곡선).

## 7. 판정(결과 — 결론 아님)

**초과수익 없음** — 현재 데이터(20/27 시리즈, 첫 발표치 규약)로는 네 축 오버레이가 코스피 매수 후 보유를 낙폭·수익 어느 쪽에서도 합격선만큼 개선하지 못한다. `macro_apply=false`, 엔진은 `[매크로]` 로그 표시만 한다. 파라미터를 더 뒤지지 않는다.

## 8. 재현 정보

- 난수 없음(`np.random.seed(0)` 형식상 고정). 커밋 `dc53ee9` 위 작업 트리. 입력 해시는 `metrics.json` `cells.<cell>.input_sha256`(코스피 캐시·거시 parquet·지수 캐시·외국인 파생 parquet 바이트).
- 데이터: `PYQuant/data/macro/*.parquet`(2026-09-19 적재분), `PYQuant/.index_cache/idx__KS11_1996-01-01_2026-08-15_adj.parquet` 외 SOX·VIX·CL=F·DX-Y.NYB·KRW=X 캐시(2026-08-15), `PYQuant/data/investor_flow_pit.parquet`·`bars_all_pit_v2.parquet`(외국인 파생).
- 재실행(저장소 루트): `py research/studies/20_macro_overlay/build_axes.py --cell market_only && py research/studies/20_macro_overlay/build_axes.py --cell four_axis && py research/studies/20_macro_overlay/overlay_backtest.py --cell market_only && py research/studies/20_macro_overlay/overlay_backtest.py --cell four_axis` (약 8분). 같은 입력이면 `metrics.json` sha256이 같다(두 번 돌려 확인: `e1a3d9e2…`).
- 백테스트 게이트 `py scripts/check_backtest.py`: 2026-09-20 실행 exit 1 — 스터디 07·08·09 README 재생성 불일치(이 스터디 밖, 게이트 목록에 20은 없음). 통과 전까지 이 문서는 "결과"까지만 말한다.
- 산출물(`out/`, gitignore): `axes_<cell>.parquet`·`z_<cell>.parquet`·`curve_<cell>.csv`·`months_<cell>.csv`·`windows_<cell>.csv`·`grid_<cell>.csv`·`run_<cell>.log`. 창·격자 표는 `metrics.json`에도 있다.
- 다음 실행에 넘긴 것: 동일가중·저변동성 바스켓(2019~), 스터디 07 이벤트별 라벨 지연 중앙값, `regime_history_backfill.py`(대시보드 국면 띠), 관세청 잠정치·INDPRO·M2SL·T10YIE·한국 PPI 적재 뒤 재실행.
