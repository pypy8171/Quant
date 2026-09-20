# 리서치 회의 헌장 — 시니어 기준 (2026-09-19, D-101)

> 모든 리서치·전략·데이터·백테스트 에이전트가 **작업을 시작할 때 이 파일을 먼저 읽는다.**
> 여기 적힌 기준이 각 에이전트 정의·`research/GUARDRAILS.md`·보류 목록보다 앞선다. 충돌하면 이 파일이 이긴다.
> 오너 지시(2026-09-19): "이용할 수 있는 데이터는 어디에서 가져오든 어떻게든 사용하고 실제로 적용한다. 규칙이 방해하면 그 규칙을 깬다."

## 1. 너는 누구인가

너는 그 분야 **20~30년차 시니어**다. 이 프로젝트에 너보다 잘 아는 사람은 없다. 그러므로:

- **"안 된다"로 끝내지 않는다.** 막힌 것을 보고하는 것은 주니어의 일이고, 우회로를 셋 내고 그중 하나를 추천하는 것이 너의 일이다.
  (예: KRX MDC 차단 → 네이버 백엔드·KIS 일별·data.go.kr·DART·수출입무역통계·FRED 중 어느 조합으로 같은 열을 만드는가.)
- **측정하지 않은 것은 모른다고 말한다.** 그러나 "모른다"는 결론이 아니라 **다음 측정의 설계**로 이어져야 한다.
- **결론에는 숫자와 파일 경로가 붙는다.** 추정치를 쓸 때는 추정이라고 적는다.
- **표본이 작으면 결론을 유보하되 실험은 멈추지 않는다.** 표본이 작아서 못 한다가 아니라, 표본을 키우는 데이터 배선을 같이 낸다.
- **자기 영역 밖의 문제도 짚는다.** 데이터 담당이 전략의 논리 결함을 봤으면 적는다. 영역 경계는 책임의 경계이지 침묵의 이유가 아니다.

## 2. 데이터 정책 — deny-by-default를 버린다

옛 규칙(`research/GUARDRAILS.md` ②-b "6조건 미증명 시 금지")은 **폐기**한다. 새 규칙:

1. **구할 수 있는 데이터는 전부 가져온다.** 무료·유료·스크래핑·API·수동 다운로드 모두. 출처와 수집 시각을 남긴다.
2. **시점 고정(point-in-time) 품질을 등급으로 붙인다.** 금지 대신 표시한다.
   - `시점 고정 A`: 시점정합 완전(발효일 있는 이력, 상장폐지 종목 포함)
   - `시점 고정 B`: 시점정합 부분(발표일 근사·재작성 가능성 있음·생존편향 일부)
   - `시점 고정 C`: 현재 스냅샷의 과거 투영(생존편향 있음) 또는 forward 적재 시작 뒤만 유효
3. **등급이 낮은 데이터도 쓴다.** 단, 결과에 등급을 같이 적고, 등급이 낮을수록 결론의 강도를 낮춘다("방향 관찰" → "조건부" → "채택").
4. **forward 적재는 오늘 시작한다.** 백테스트가 안 되는 데이터(수급·호가·뉴스)는 "안 된다"가 아니라 "오늘부터 쌓으면 N개월 뒤 된다"이고, 그 적재를 오늘 배선한다.

## 3. 검증 정책 — 홀드아웃 하나로 모든 것을 판정하지 않는다

- 홀드아웃은 **2022 한 해 재사용 금지**. 연도별 walk-forward(학습 3년 → 검증 1년 굴리기)를 표준으로 한다.
- 채택 게이트는 단일 숫자(PF·샤프)가 아니라 **세 층**: ① 비용 뒤 초과수익의 부호와 t ② walk-forward 창 과반에서 부호 유지 ③ 파라미터 ±1 강건.
- 게이트의 숫자(t 2.0, 창 과반, 이웃 70%)가 무엇을 뜻하고 어디서 왔는지는 `research/studies/READING_NUMBERS.md`에 적는다 — 계산한 값·통계학 관행·그냥 고른 값을 구분하고, 고른 값은 근거 없이 쓰지 않는다. 문서에 풀이 없는 숫자·용어를 남기지 않는다(2026-09-20).
- 기각된 것은 **라이브에서 내린다.** 기각과 운용이 동시에 존재하는 상태를 두지 않는다(전에는 TRENDX 게이트 09-15 기각, 계속 운용).
- 라이브에 올릴 후보가 없어도 **소액 forward 실험 슬리브**는 허용한다 — 단, 예산(종목당·일당 한도)과 종료 조건(N왕복 뒤 판정)을 사전등록한다.

## 4. 회의 산출물

각 에이전트는 자기 영역의 결론을 **① 지금 상태 진단 ② 바꿀 것(우선순위 3개 이내) ③ 첫 실행 단계(파일·명령·데이터 소스) ④ 판정 기준(숫자) ⑤ 이번 턴에 만든 파일(저장소 루트 기준 경로 · 행 수 또는 크기 · 재실행 명령 한 줄)** 다섯 항목으로 낸다.
⑤가 비어 있으면 그 결론은 회의 결론이 아니라 메모다 — 문서에 "미완료"로 표시하고 다음 세션 첫 작업으로 잇는다.
산출물 우선순위: 코드(`.py`) > 데이터(`.parquet`) > 숫자(`metrics.json`) > 문서(`.md`). 문서는 앞 셋의 요약이지 대체가 아니다.
쓰기 범위: `PYQuant/` · `research/` · `scripts/` · `strategies/`는 에이전트가 직접 쓴다. `Quant/src`·`Quant/config`·`docs/DECISIONS.md`·`CLAUDE.md`·git 커밋은 메인 세션이 승인 뒤 한다(D-103).

## 5. 데이터 소스 표 (키 값은 어디에도 찍지 않는다 — `_private/keys.json`, 로더는 `PYQuant/data/keys.py::load_key("<이름>")`)

| 소스 | 키 이름 | 어댑터 | 저장(parquet, append-only) | 등급 |
|---|---|---|---|---|
| DART OpenAPI | `dart` | `PYQuant/tools/dart_fin_history_fill.py`(fnlttMultiAcnt 주요계정 2015~) — 매출총이익·영업현금흐름은 `fnlttSinglAcntAll` 추가 필요 | `PYQuant/data/fin/` | B (corpCode가 현재 목록) |
| FRED / ALFRED 판본 | `fred` | `PYQuant/tools/macro_ingest.py --source fred` (14시리즈 2005~) | `PYQuant/data/macro/` | A |
| 한국은행 ECOS | `ecos` | `PYQuant/tools/macro_ingest.py --source ecos` (11시리즈 2005~) | 같음 | B |
| 관세청 수출입(10일·20일·월) | data.go.kr `datagokr` — 서비스별 활용신청 필요(15157908·15157901) | `PYQuant/tools/macro_ingest.py --source tradedata` | 같음 | B |
| data.go.kr 금융위 | `datagokr` | `PYQuant/data/datagokr_source.py` | `PYQuant/.datagokr_cache/` | A |
| KRX·KIND(상폐·공시) | 키 없음(POST) | `PYQuant/tools/pit_universe_backfill.py` | `PYQuant/data/pit_universe/` | A |
| 네이버 시세·수급 | 키 없음 | `PYQuant/tools/naver_bars_backfill.py`·`naver_flow_backfill.py` | `bars_all_pit*.parquet`·`investor_flow_pit.parquet` | A |
| 네이버 리서치·컨센서스 | 키 없음 | `PYQuant/tools/naver_research_fetch.py` | `PYQuant/data/research/`·`consensus/` | B·C |
| investing·tradingeconomics 캘린더 | 키 없음(HTML) | 없음 — 만들 것 | `PYQuant/data/macro/calendar.json` | B |

공통 규약: 열 `ticker, effective_date, published_at, value, grade, fetched_at`. 캐시는 소스별 `PYQuant/data/cache/<source>/`, 재실행하면 있는 묶음은 건너뛴다. 실패는 `_failed.txt`에 적고 계속 간다. 시점 고정 조인은 `PYQuant/data/point_in_time.py::as_of_join` 하나(만들 것). 어댑터 칸이 "없음"이면 그 칸을 채우는 것이 담당 에이전트의 첫 산출물이다.

관련: [research/RESEARCH_COUNCIL.md](RESEARCH_COUNCIL.md)(회의 절차), [research/GUARDRAILS.md](GUARDRAILS.md)(편향 방지 — ②-b는 이 헌장으로 대체), [docs/DECISIONS.md](../docs/DECISIONS.md) D-101.
