# 리셋 2라운드 종합 — 2026-09-19 밤 ~ 09-20 새벽 (D-103)

> 이 폴더는 리셋 회의 2라운드의 정본이다. 1라운드 결론은 `research/RESET_2026-09-19.md`(D-101), 이 문서는 그 뒤 하룻밤 사이 한 일을
> 한 장에 모은 색인이다(세션 quant-88). 숫자는 전부 09-20 새벽에 파일에서 다시 센 값이고, 각 칸의 경로가 근거다.
> 읽는 순서: §0 → §1 표 → 필요한 갈래의 절 → 정본 한 장(§6) → 스터디 README.

## 0. 한 줄 결론

오너 지시(09-19 밤) "발급받은 키(DART·FRED·ECOS·관세청)를 실제로 써서 재무·거시·수급·기술 데이터 전부로 백테스트 근거 전략을 만들어
모의 매매에 올려라. 규칙이 방해하면 규칙을 바꿔라"에 따라 **데이터 4소스 적재·공용 통계 모듈·스터디 2건·규칙 개정**을 하룻밤에 끝냈다.
스터디 결과는 둘 다 **미달·기각** — 재무(19)는 방향은 맞으나 t 1.14, 거시(20)는 배수 갱신 비용이 초과수익을 먹었다.
살아남은 것은 데이터 마트·도구·판정 양식이고, 다음 실행 조건은 §3에 적었다. 모의 매매에 올릴 전략은 아직 없다.

## 1. 결과 한눈에

| 갈래 | 한 것 | 상태 | 정본 |
|---|---|---|---|
| 데이터 적재 | DART 재무·주식수, FRED 14, ECOS 11, 관세청 6, 네이버 리서치 600건, 파생 1 | 끝(제약은 §2-3) | §2, `PYQuant/data/{fin,macro,research}/` |
| 공용 코드 | 통계 표준 `stats.py`, 시점 고정 조인 `point_in_time.py`, 팩터 `features/`, 키 로더 `keys.py` | 끝, 테스트 통과 | §4 |
| 스터디 19 재무 | 저PBR×고ROE 상위 30, 격자 18, walk-forward 5창 | **미달**(3층 중 1층 통과) | §3-1, `research/studies/19_fundamental_factors/` |
| 스터디 20 거시 | 네 축 국면 배수 × 코스피, 1999~2025, 격자 27 | **기각**(7항목 중 2 통과) | §3-2, `research/studies/20_macro_overlay/` |
| 규칙·하네스 | D-103: 회의는 파일로 끝남, 리서치 에이전트 쓰기 권한, 파이썬 적재는 메인 트리 | 저장소 문서 끝, `.claude/` 오너 실행 끝 | §5, `docs/DECISIONS.md` D-103 |
| 회의 보고 | 7장(전략·리스크·재무·거시·편향·판정·하네스) | 끝 | §6 |
| 오너 결정 | 6건 대기 + A등급 후보 1건 | **대기** | §7 |

## 2. 데이터 적재 현황

### 2-1. 소스별 표

| 소스 | 어댑터 | 저장 위치 | 기간 | 규모 | 등급 | 재실행 |
|---|---|---|---|---|---|---|
| DART 재무제표(주요계정) | `PYQuant/tools/dart_fin_history_fill.py` | `PYQuant/data/fin/dart_fs_raw.parquet` → `fin_point_in_time.parquet` | 2015~2026 | 원본 2,472,394행 → 시점 고정 96,869행, 회사 3,990 | B | `--from 2015`, 1,920호출 1,002초 |
| DART 상장주식수 | `PYQuant/tools/dart_shares_history_fill.py` | `PYQuant/data/fin/shares_point_in_time.parquet` | 2015~2026 | 216,755행(2020~은 data.go.kr 월말 A, 2015~2019 사업보고서 B) | A/B | 같은 파일 |
| FRED(ALFRED 판본) | `PYQuant/tools/macro_ingest.py --source fred` | `PYQuant/data/macro/fred_<id>.parquet` | 2005~ | 14시리즈(DGS2·DGS10·T10Y2Y·FEDFUNDS·CPIAUCSL·PPIACO·GDPC1·UNRATE·ICSA·UMCSENT·VIXCLS·DEXKOUS·DTWEXBGS·BAMLH0A0HYM2) | A | `--since 2005-01-01` |
| ECOS(한국은행) | `--source ecos` | `ecos_<이름>.parquet` | 2005~ | 11시리즈(기준금리·국고 3·10년·원달러·CPI·M2·수출·수입·선행지수·선행/동행 순환) | B(발표일 근사) | 같음 |
| 관세청 수출입 10일 잠정치 | `--source tradedata` | `tradedata_{export,import}_{10d,20d,month}.parquet` | 2016-01~ | 6시리즈 128~129행 | B | 10년 창 분할 호출(09-20 수정) |
| 네이버 리서치(증권사 보고서) | `PYQuant/tools/naver_research_fetch.py` | `PYQuant/data/research/{list,pdf,text}/`, `latest.json` | 최근 | PDF·본문 600건, 컨센서스 `PYQuant/data/consensus/` | — | 카테고리 6개, `--estimates` |
| 파생: 외국인 순매수/거래대금 | `research/studies/20_macro_overlay/build_axes.py` | `PYQuant/data/macro/derived_foreign_flow_kospi.parquet` | 2019~ | 1,876행 | A | 스터디 20 재실행에 포함 |

키는 `_private/keys.json`에서 `PYQuant/data/keys.py::load_key(이름)`로만 읽는다. 값은 어디에도 적지 않는다. 관세청 활용신청은 09-20 새벽 승인됐다.

### 2-2. 열 규약 (모든 parquet 공통)

`observation_date`(관측 기준일) · `published_at`(알 수 있게 된 시각) · `value` · `source` · `series_id` · `fetched_at` · `grade`.
백테스트는 `published_at ≤ 결정 시각` 행만 본다(`PYQuant/data/point_in_time.py`). 등급 A = 발표 시각을 그대로 안다, B = 규칙으로 근사했다, C = 현재 값뿐이다.

### 2-3. 제약·근사 (결과를 읽을 때 같이 봐야 할 것)

| 항목 | 내용 | 영향 |
|---|---|---|
| 관세청 시작 시점 | API가 2016-01부터만 준다(요청은 2005부터) | 수출 축은 2016 전 ECOS 월간 수출로 대체 |
| ECOS 발표일 | 월간 통계에 발표 시각이 없어 "기준월 말 + N일"로 근사(예: 수출 +46일) | 등급 B, 스터디 20 §1에 명시 |
| 빠진 거시 시리즈 | 스펙 27개 중 7개 미적재(`research/studies/20_macro_overlay/README.md` "빠진 시리즈") | 축 점수는 남은 시리즈로 계산 |
| DART 회사 목록 | corpCode 현재 목록 기반이라 옛 상폐사 일부가 빠짐 | 스터디 19 등급 B, 생존 편향 상한 미측정 |
| 스터디 19 미실행 항목 | 시총가중 분위·크기 분해·용량(20일 거래대금 1%) | 다음 실행 |

## 3. 스터디 결과

### 3-1. 스터디 19 — 저PBR × 고ROE 상위 30 동일가중 (미달)

| 층 | 기준 | 값 | 판정 |
|---|---|---|---|
| 1 초과수익 t | ≥ 2.0 | 1.14(뉴이-웨스트 1.31) | 미달 |
| 2 walk-forward | 5창 중 ≥ 3 양수 | 4 / 5 | 통과 |
| 3 ±1 이웃 | 이웃 전부 > 0, 중심 샤프 ≥ 격자 최대의 70% | 양수 4/5, 샤프 비 0.39 | 미달 |

- 판정 구간 2021-01~2025-12, 비용 MID: 순초과수익 연 +6.99%, 샤프 0.27, 최대 낙폭 38.2%, 편도 회전 월 23.3%, 비용 연 1.65%. 종목-월 80,181, 유니버스 중앙값 994.
- 보조 지표는 강하다: 순위 IC 0.104(t 7.88), 5분위 단조 1.00, Q5−Q1 연 +26.8%(t 4.32). 팩터 자체보다 **상위 30 동일가중이라는 포트폴리오 구성**이 약하다.
- 격자에서는 가치 비중 0.7·분기 리밸 칸이 일관되게 높다(N=40·w=0.7·3개월 t 2.83) — 사전등록 중심 셀이 아니므로 **결론이 아니라 다음 사전등록의 후보**다.
- 다음 실행 조건: 새 번호 스터디로 (1) 중심 셀을 w=0.7·분기 리밸로 사전등록, (2) 시총가중·크기 분해·용량 추가, (3) 상폐사 포함 회사 목록으로 등급 A 재시도.

### 3-2. 스터디 20 — 거시 네 축 국면 배수 × 코스피 (기각)

- 1999-01~2025-12 6,652일, 격자 27: 연복리 7.67% → 6.32%(−1.35%p), 최대 낙폭 −56.0% → −55.3%, 확장−수축 t −0.39, 배수 전환 연 2.35회.
- 원인 진단: 일간 배수(0.05 단위)가 노출 변경 비용을 27년 17.8%나 냈다. 국면 신호가 나빠서가 아니라 **배수 갱신 주기를 스펙이 정하지 않아** 비용이 초과수익을 먹었다.
- 통과한 것: 룩어헤드 차단(`published_at` 단조 배열, 테스트 `PYQuant/tests/test_regime_axes.py`)과 백테스트·라이브가 같은 함수 `PYQuant/features/regime_axes.py::score(as_of, cell)`를 쓰는 구조.
- 다음 실행 조건: `research/RESET_2026-09-19_R2/macro-quant.md`에 **배수 갱신 주기(월 1회 또는 축 부호 변경 시)**를 추가 사전등록한 뒤 재실행(약 8분).

### 3-3. 두 스터디가 같이 말하는 것

1. 새 데이터 두 갈래 모두 **신호는 있으나 구성·비용에서 진다** — 팩터를 더 찾기보다 포트폴리오 구성(비중·리밸 주기)을 사전등록 축으로 올려야 한다.
2. 판정 양식(`PYQuant/backtest/stats.py`: 뉴이-웨스트 t → walk-forward → 이웃 → BH-FDR → 블록 부트스트랩 → DSR)이 두 스터디에서 같은 표를 냈다. 이 양식이 이제 정본이다(`research/RESET_2026-09-19_R2/quant-analyst.md` §2).
3. 오너 지시의 "모의 매매에 올린다"는 이번 밤에 못 갔다. 올릴 후보가 생기는 시점은 3-1·3-2의 다음 실행 뒤다.

## 4. 공용 코드 신설

| 파일 | 역할 | 테스트 |
|---|---|---|
| `PYQuant/backtest/stats.py`(520줄) | 뉴이-웨스트 t, walk-forward 창(2015~ 9창/2019~ 5창), BH-FDR, 블록 부트스트랩, DSR — 스터디 공통 판정 | `PYQuant/tests/test_stats.py` 14건 |
| `PYQuant/data/point_in_time.py` | `published_at ≤ 결정 시각` 조인, 발효일 = DART `rcept_no` 앞 8자리 | `PYQuant/tests/test_point_in_time.py` |
| `PYQuant/features/fundamental.py` | 재무 팩터(PBR·ROE 등) 시점 고정 계산 | 스터디 19 실행이 검증 |
| `PYQuant/features/regime_axes.py`(582줄) | 거시 네 축 점수 `score(as_of, cell)`, 백테스트·라이브 공용 | `PYQuant/tests/test_regime_axes.py` 4건 |
| `PYQuant/data/keys.py` | `_private/keys.json` 읽기, 값은 반환만 | — |
| `research/studies/13_trendx_gate/stats_util.py` | 116줄 → 21줄, `stats.py`를 부르는 얇은 층 | 기존 테스트 |

pytest 전체 96건 통과(09-20 새벽, §10 첫 명령).

## 5. 규칙·하네스 개정 (D-103)

| 결정 | 내용 | 적용 위치 | 상태 |
|---|---|---|---|
| 1 | 회의는 코드·parquet·metrics.json 파일로 끝난다(표·문단만은 미완) | `research/COUNCIL_CHARTER.md` §4·§5, `research/RESEARCH_COUNCIL.md` 절차 7.5 | 끝 |
| 2 | 리서치 에이전트 쓰기 범위 `PYQuant/`·`research/`·`scripts/`·`strategies/` | `docs/DECISIONS.md` D-103 | 끝 |
| 3 | 엔진 빌드와 무관한 파이썬 적재·리서치 코드는 메인 트리에서 바로 만든다 | `CLAUDE.md` 다중 세션 절 한 줄 | 끝 |
| 4 | 키 읽기는 `PYQuant/data/keys.py::load_key` 한 곳 | 어댑터 4개 | 끝 |
| 5 | 에이전트 정의: data-sourcer에 Write·Edit·WebFetch, fundamental-quant·macro-quant·strategist·risk-behavior에 Write·Edit, quant-analyst·market-brief에 Write, 15개에 시니어 절 | `.claude/agents/*.md`(로컬), `.claude/AGENTS.md` 4줄 | 오너가 스크립트 실행, 끝 |

- `.claude/`는 자동 모드 분류기가 세션의 편집을 막아 스크립트(`scratchpad/apply_d103.py`, `--undo` 있음)를 오너가 직접 돌렸다. `.claude/agents/*.md.bak` 14개가 로컬에 남아 있다(원복용, 커밋 대상 아님).
- 실수 기록: 스크립트 시험용 사본의 경로 치환이 실패해 실제 `.claude/` 15파일이 한 번 수정됐고 즉시 `--undo`로 되돌렸다. 시험은 반드시 경로를 인자로 받는 방식으로 한다.
- 새 에이전트 정의 권한은 VSCode 재시작 뒤 적용된다.

## 6. 회의 보고 7장 색인

| 파일 | 저자 | 한 줄 결론 | 산출로 이어진 것 |
|---|---|---|---|
| `strategist.md`(115줄) | strategist | 포트폴리오 세 슬리브(장중·재무·저변동) 배분 56/10/34 제안, 정배열 축은 기저를 못 이김 | 오너 결정 1~3(§7) |
| `risk-behavior.md`(170줄) | risk-behavior | 게이트가 못 막는 5가지(평가손·주간 낙폭·재진입·손절 뒤 복귀·승계분); 손절 뒤 당일 재매수 72% | A등급 후보 047050(§7), 규칙 코드화(§8) |
| `fundamental-quant.md`(164줄) | fundamental-quant | 재무 슬리브 사전등록 스펙 3후보, 공통 규약(발효일·폴백·등급) | 스터디 19 |
| `macro-quant.md`(145줄) | macro-quant | 네 축 → 국면 → 배수 스펙, 시리즈 27개와 등급, 지금 국면 파일은 "간밤 등락표" | 스터디 20, 오버레이 함수 |
| `bias-auditor.md`(142줄) | bias-auditor | 기존 백테스트 숫자 전부 결론 승격 불가(2022 홀드아웃 다중 사용·비용 상수 4갈래·등급 B) | 판정 양식 통일, `costs.py` 한 벌 |
| `quant-analyst.md`(306줄) | quant-analyst | 스터디 17개 중 엣지 주장 생존 0, 라이브 9거래일은 부호 판정 불가(약 65거래일 필요), 채택 표준 한 양식 | `PYQuant/backtest/stats.py` |
| `harness-engineer.md`(114줄) | harness-engineer | 회의가 코드를 못 낸 이유는 소유·권한·형식; 새 에이전트 대신 기존 7개에 쓰기 권한 | D-103, `.claude/` 개정 |

## 7. 오너 결정 대기

| 번호 | 결정 | 출처 | 미룰 때 |
|---|---|---|---|
| 1 | 자본 배분 56(장중)/10(재무)/34(저변동) | `strategist.md` §3 | 슬리브 추가 불가, 현행 장중 100% 유지 |
| 2 | 바스켓 손절(슬리브 단위 −N% 정지) | `strategist.md` §3 | 게이트는 종목 단위만 |
| 3 | S5(저변동 바스켓) 4슬롯 | `strategist.md` §3 | 스터디 18(H10)부터 시작 못 함 |
| 4 | 재무 슬리브 별도 계좌 여부 | `fundamental-quant.md` §4 | 전략별 손익 귀속 수정(3-c)과 묶임 |
| 5 | 실계좌 마감 청산 15:20 → 15:15 정정 여부 | `_private/HANDOFF_quant-88.md` 남은 것 2 | 모의(15:15)와 실계좌(15:20) 불일치 지속 |
| 6 | 스터디 20 배수 갱신 주기(월 1회 / 부호 변경 시) | §3-2 | 재실행 못 함 |
| A등급 후보 | 047050 재기동마다 `이전 세션 주문 체결(ODNO 미매핑)` 재기록 — 원장 중복 가능 | `risk-behavior.md` | 월요일 장 전 `on_fill_confirmed` ODNO 중복 차단 확인 |

## 8. 남은 일 (순서대로)

| 순서 | 일 | 조건 | 비고 |
|---|---|---|---|
| 1 | 이번 밤 산출물 커밋(어댑터 4·공용 코드·스펙 7장·규칙 개정·스터디 19·20·README) | 게이트 `py scripts/commit_gate.py` → 오너 승인 | 남의 미커밋(`docs/HARNESS.md`·`docs/facts.json`·`research/dashboard/*`·스터디 08·09·11·17) 분리 |
| 2 | 스터디 20 재실행 | 결정 6 | 약 8분 |
| 3 | 스터디 19 후속(새 번호) | §3-1 다음 실행 조건 | 상폐사 포함 회사 목록 먼저 |
| 4 | 관세청·ECOS 월간 발표일 실측(등급 B → A) | 관세청 보도자료 일정 표 | `macro_ingest.py` `published_at` 규칙 교체 |
| 5 | 빠진 거시 시리즈 7개 적재 | 소스 확인 | 스터디 20 README 목록 |
| 6 | `research/studies/render_studies.py`·`build_study_site.py`가 새 폴더를 자동 인식하도록 | — | 지금은 손으로 README 행 추가 |
| 7 | 네이버 리서치를 장중 대시보드에 싣기(오너 요청) | `latest.json` 읽는 패널 | `scripts/dashboard_server.py` |
| 8 | 리스크 규칙 코드화(재진입 차단·승계분 보호·주간 낙폭) | 결정 1~2 | 워크트리, 장 마감 뒤 배포 |
| 9 | R-11 이름 정리(`bars_all_pit` 등 옛 이름) | R-12 뒤 | 어댑터가 옛 이름을 참조 |

## 9. 부수 발견·알려진 문제

- `scripts/check_backtest.py` exit 1 — 기존 스터디 08·09 README 드리프트(이번 밤 산출과 무관).
- `research/studies/17_exit_ev/`에 `metrics.json`이 없다(기술통계 스터디라 판정 표가 없음).
- `PYQuant/features/__init__.py` docstring을 스터디 19 에이전트가 덮어썼다(기능 영향 없음, 정리 대상).
- 관세청 API는 한 호출 10년 제한(resultCode 99) — 9년 창으로 나눠 부르도록 `fetch_tradedata`를 고쳤다.
- 파이썬 콘솔 출력은 `PYTHONIOENCODING=utf-8` 없이는 cp949로 깨진다. Bash 히어독은 백슬래시를 지우므로 정규식이 든 파일은 Write로 만든다.
- 예약작업 `claude_stock_study`·`claude_dashboard_sync` 09-19 20:45 실패(rc 2147946720·1), `Quant Market Close AutoDoc` 09-18분 미실행 — 이 세션 범위 밖, `docs/AUTOMATION.md` 복구 절.

## 10. 명령 모음

```bash
# 테스트 전체
PYTHONIOENCODING=utf-8 py -m pytest PYQuant/tests -q
# 적재 재실행(append-only, 중복은 한 행)
PYTHONIOENCODING=utf-8 py PYQuant/tools/macro_ingest.py --source fred --since 2005-01-01
PYTHONIOENCODING=utf-8 py PYQuant/tools/macro_ingest.py --source ecos --since 2005-01-01
PYTHONIOENCODING=utf-8 py PYQuant/tools/macro_ingest.py --source tradedata --since 2005-01-01
PYTHONIOENCODING=utf-8 py PYQuant/tools/dart_fin_history_fill.py --from 2015
PYTHONIOENCODING=utf-8 py PYQuant/tools/naver_research_fetch.py
# 스터디 19
py research/studies/19_fundamental_factors/run_pbr_roe.py --cost mid
# 스터디 20 (약 8분)
py research/studies/20_macro_overlay/build_axes.py --cell market_only && py research/studies/20_macro_overlay/build_axes.py --cell four_axis
py research/studies/20_macro_overlay/overlay_backtest.py --cell market_only && py research/studies/20_macro_overlay/overlay_backtest.py --cell four_axis
# 문서·낱말 검사
py scripts/check_docs.py && py scripts/check_plain_language.py
```
