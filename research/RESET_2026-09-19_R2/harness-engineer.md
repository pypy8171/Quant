# 하네스 엔지니어 보고 — 리셋 2라운드 (2026-09-20)

> 작성: harness-engineer(읽기 전용)가 낸 보고를 메인 세션이 파일로 옮기고, §3의 수정안은 같은 밤에 적용했다(적용 여부는 §3 각 표의 "적용" 칸).

## 결론 5줄

1. 회의가 코드를 못 낸 이유는 인원이 아니라 **소유·권한·형식**이다: 리서치 에이전트 8명 중 Write가 있는 것은 backtest-runner 하나, 헌장 §4와 에이전트 "출력 형식"이 전부 표·문단이며, 회의 절차 8단계에 "구현 실행" 단계가 없다.
2. 실측(00:30): `_private/keys.json`에 `dart·datagokr·ecos·fred` 4개(관세청 서비스 활용신청은 미완). DART 어댑터는 00:01에야 생겼고 FRED·ECOS 어댑터는 0개, `PYQuant/data/macro/`는 빈 폴더였다. (같은 밤 01:00 기준 셋 다 생겼다 — `dart_fin_history_fill.py`·`macro_ingest.py`·`naver_research_fetch.py`.)
3. 새 에이전트는 만들지 않는다 — data-sourcer를 "소싱·적재 엔지니어"로 넓히고 fundamental-quant·macro-quant·quant-analyst·strategist·market-brief·risk-behavior에 Write/Edit를 준다.
4. 규칙 충돌 둘을 D-103으로 푼다: "제안만·메인 승인 하에 반영" 문구(에이전트 정의 14곳)와 CLAUDE.md 다중 세션 규칙(코드는 worktree에서만)이 파이썬 적재 코드까지 막고 있었다.
5. 결정론 백스톱으로 `scripts/check_council_output.py`(회의 문서의 각 절에 실존하는 산출 파일 줄이 있는가, fail-open Stop 훅) 하나를 제안한다.

## 0. 실측 (2026-09-20 00:30, main dc53ee9)

`.claude/PROJECT_FACTS.md` 대조: OrderGate reject_reason 19·add_executable 50·에이전트 20·커맨드 16·훅 15/15·스터디 01~17(04·05 없음) 전부 일치. 도장만 낡음(06d3f87 → dc53ee9) → `py scripts/gen_facts.py --apply`.

데이터 어댑터 실측(오너가 말한 실패의 실체):

| 소스 | 키 | 어댑터(00:30) | 어댑터(01:00) |
|---|---|---|---|
| DART | `dart` 있음 | `PYQuant/tools/dart_fin_history_fill.py` 미추적·검증 300행뿐 | 전량 1,920호출 적재 실행 |
| FRED/ALFRED | `fred` 있음 | 없음 | `macro_ingest.py` 14시리즈 59,555행 |
| ECOS | `ecos` 있음 | 없음 | 11시리즈 25,938행 |
| 관세청 수출입 | `datagokr`로 호출, 서비스 활용신청 미완(403) | 없음 | 파서 준비, 승인 뒤 동작 |
| 네이버 수급 | 키 불필요 | `naver_flow_backfill.py` 69MB — 됐다 | — |
| 리셋 §C가 적은 `eod_data_nightly.py`·`event_feed.py`·`panel.py`·`point_in_time.py`·`PYQuant/features/` | — | 전부 없음 | 아직 없음 |

## 1. 진단 — 에이전트별 채점 (베테랑 기준, 깊이 1~5)

| 에이전트 | 깊이 | 산출물 형태 | 도구가 일에 맞나 | 부족한 점 / 문서로 끝내는 문구 |
|---|---|---|---|---|
| fundamental-quant | 5 | 표·문단 | 아니오(Read만) | `:40` "배선을 제안한다". 자기 1순위 소스 어댑터·키 위치를 모른다. `fnlttMultiAcnt` 14계정에 매출총이익·영업현금흐름이 없어 GP/A·발생액은 `fnlttSinglAcntAll`이 있어야 한다 |
| macro-quant | 5 | 표·문단 | 아니오 | `:37` "yfinance/FDR"(yfinance 전 심볼 실패). 키 위치·저장 규약 없음 |
| data-sourcer | 4 | 판정 문단 + "뼈대 제시" | 아니오(Read만, WebFetch 없음) | `:48` "갱신을 제안", `:61` "뼈대를 제시", `:87` "실행은 사용자·메인". 확보 지도가 2026-06 상태(수급 ❌ — 09-19에 A등급 69MB 적재됨). **오늘 실패의 직접 원인** |
| backtest-runner | 4 | 파일(유일) | 예 | `:33` 스터디 "01~11"(실제 17), `:75` "오래 걸리면 승인 하에". 어댑터는 자기 일이 아니다 — data-sourcer와 사이에 구멍 |
| quant-analyst | 4 | 문단 | 부분 | `:64` "제안(실행은 runner)" — metrics.json을 못 남김 |
| bias-auditor | 5 | 판정표 | 예 | 없음. 유지 |
| strategist | 4 | 문단 6절 | 아니오 | `:59`·`:65`·`:169` "제안/승인 하에 기록". `:119` 거래세 0.18%(2026 0.20%) |
| risk-behavior | 5 | 문단 | 부분 | 판정 행(`check_runtime_health.py`)을 직접 쓰게 하면 CLAUDE.md "확인은 사용자 몫이 아니다"와 맞물린다 |
| market-brief | 3 | 브리핑 | 아니오(Bash·Write 없음) | 리셋 §C 리서치·컨센서스 행을 맡을 사람이 없었다 |
| intraday-analyst | 4 | 문단 | 예 | `:56` "yfinance 피드" 낡음 |
| pm | 4 | 문단 | 예 | `:60` `@arch-doc`(비활성) 라우팅 잔존. 라우팅 프롬프트에 "만들 파일"이 없다 |
| planner | 3 | Phase 문단 | 예 | `:23` "4스레드"(5스레드). 리셋 §C 파일 8개를 계획으로 냈는데 실존 2개 |
| reviewer | 4 | 표 | 예 | `:30`·`:70` "테스트 8개·ctest 미배선" — 35개 등록됨 |
| perf-optimizer | 4 | 표 | 예 | `:73` "벤치 4개" — 9개 |
| log-reader·committer·harness-engineer·claude-coach·interviewer·prep-doc | 4 | 각자 맞음 | 예 | 이번 안건 밖 |

문서로 끝내는 문구 합계 14곳. 헌장 §4 네 항목이 전부 텍스트라 "잘 지킨 결과"가 곧 문서다.

## 2. 왜 회의는 했는데 코드가 안 나왔나 — 구조 원인 3개

1. **쓰기 권한** — 리서치 8명 중 Write 0명. 어댑터는 data-sourcer 영역인데 못 쓰고, backtest-runner는 "하네스" 영역이라 안 쓴다. → 6명에 Write/Edit(§3-2). 감사 역할(bias-auditor·reviewer·pm·harness-engineer)은 읽기 유지.
2. **산출물 형식 지시** — 헌장 §4 ①~④ 전부 서술, 회의 절차 산출물 절이 `BACKTEST_LOG.md`·`STRATEGY_LAB.md` 행뿐. 모두가 지시를 잘 따라서 문서 16장이 나왔다. → 헌장 §4에 ⑤ "이번 턴에 만든 파일" 추가, 비면 메모로 격하.
3. **메인 세션의 순서와 규칙** — 회의 절차 8단계에 "구현 실행"이 없다. CLAUDE.md 다중 세션 절이 파이썬 적재 코드까지 worktree 대상으로 읽혔다. 위임 프롬프트에 "파일을 만들어라"가 없으면 결론은 텍스트다. "키는 오너 발급 뒤"가 어댑터 착수를 키 뒤로 미뤘다 — 키 없이도 엔드포인트·캐시·조인은 짤 수 있었다. 16명 결과를 메인이 207줄로 취합하는 사이 문맥이 소진돼 코드 차례가 안 왔다. → D-103, 절차 7.5 "구현 실행(같은 턴·병렬)".

## 3. 수정안과 적용

### 3-1. 헌장 `research/COUNCIL_CHARTER.md` — 적용 완료
§4 다섯 항목(⑤ 만든 파일, 산출물 우선순위 코드>parquet>metrics>문서, 쓰기 범위) + §5 데이터 소스·키·어댑터 표 신설 + "PIT" → "시점 고정".

### 3-2. 도구 권한 변경 (프런트매터 `tools:`)

| 에이전트 | 추가 | 쓰기 범위(본문에 적는다 — `tools:`는 경로 제한이 안 된다) |
|---|---|---|
| data-sourcer | Write, Edit, WebFetch | `PYQuant/tools/`·`PYQuant/data/` |
| fundamental-quant | Write, Edit | `PYQuant/tools/dart_*`·`PYQuant/features/`·`research/studies/` |
| macro-quant | Write, Edit | `PYQuant/tools/macro_*`·`PYQuant/data/macro/`·`PYQuant/features/regime_*` |
| quant-analyst | Write | `research/studies/*/metrics.json` |
| strategist | Write, Edit | `PYQuant/features/signals_*.py`·`strategies/<전략>/`·`research/STRATEGY_LAB.md` |
| market-brief | Bash, Write | `PYQuant/data/research/`·`PYQuant/data/macro/brief_<날짜>.json` |
| risk-behavior | Write, Edit | `scripts/check_runtime_health.py` 판정 행·`research/studies/*/risk_*.py`. `Quant/config/*.json` 제외 |
| bias-auditor·reviewer·pm·planner·perf-optimizer·harness-engineer·log-reader | 유지(읽기) | 의심·감사 역할은 쓰지 않는다 |

### 3-3. 에이전트별 바꿀 문단 (요지)
- 8명 공통 "시니어 기준" 절: 헌장 요지 5줄(우회로 셋·시점 고정 등급·⑤ 만든 파일·산출물 우선순위·쓰기 범위)로 교체.
- fundamental-quant `:40` → "적재를 먼저 끝낸다(`dart_fin_history_fill.py`) … 팩터는 `PYQuant/features/fundamental.py::compute(as_of)` 한 함수".
- macro-quant `:37` yfinance → FinanceDataReader+ALFRED, `:38~39` → `macro_ingest.py` 적재 먼저·`regime_axes.py::score(as_of)`.
- data-sourcer description·`:16~19`·`:48`·`:61`·`:87`·출력 형식 → "판정만 하지 않는다, 없으면 네가 만든다, 1콜을 네가 찍는다, [적재 결과] 경로·행 수·기간·등급·재실행 명령".
- strategist `:49`·`:65`·`:169` → 원장 행을 직접 쓴다, `:119` 매도세 0.20%(왕복 0.23%), 출력에 `[신호 함수] PYQuant/features/signals_<이름>.py`.
- quant-analyst `:64` → `metrics.json`을 직접 쓴다(키: sample_n, period, holdout, sharpe, mdd, t_stat, walk_forward_sign_ratio).
- risk-behavior 출력 ③ → (a) `check_runtime_health.py` 판정 행 코드는 직접, (b) config diff는 메인.
- market-brief 추가 업무 3 "리서치·컨센서스를 파일로 남긴다"(`PYQuant/tools/naver_research_fetch.py` 실행).
- backtest-runner `:33` 01~17, `:75` "10분 넘으면 백그라운드, 승인은 파괴적 명령에만".
- pm `:60` `@arch-doc` 삭제 → `@data-sourcer 적재 / @fundamental-quant 재무 / @macro-quant 거시`, "프롬프트에 만들 파일 경로를 넣는다".
- planner `:23` 5스레드, 추가 업무 "새 파일 이름마다 누가 이번 턴에 만드는가".
- reviewer `:30`·`:70` ctest 35개 등록, perf-optimizer `:73` 벤치 9개, intraday-analyst `:56` FinanceDataReader.
- `.claude/AGENTS.md` 권한 칸 7곳 "읽기 전용" → 쓰기 범위 명시, 뒤에 `py scripts/sync_impact.py --restamp .claude/AGENTS.md`.

### 3-4. 새 에이전트 — 만들지 않는다
data-engineer 신설(B안)은 data-sourcer와 소스 지식이 겹치고 라우팅만 흐려진다. research-reader(C안)는 야간 잡이라 커맨드가 맞다. B안 정의 초안은 harness-engineer 원 보고(세션 기록)에 있다 — 오너가 "판정과 구현을 분리하고 싶다"고 할 때만 꺼낸다.

### 3-5. 하네스 보강 (우선순위순)

| # | 무엇 | 왜 | 상태 |
|---|---|---|---|
| 1 | `PYQuant/data/keys.py::load_key` | 어댑터마다 키 읽기를 따로 짜면 하나는 값을 찍는다 | 적용(01:1x) |
| 2 | `scripts/check_council_output.py` + Stop 훅 `council-gate.ps1` | 회의 문서 절마다 `산출 파일:` 줄과 실존 경로 검사, fail-open | 제안 |
| 3 | `.claude/commands/council.md` | 8단계 + 7.5 구현 실행을 커맨드로, 위임 프롬프트에 "만들 파일" 칸 강제 | 제안 |
| 4 | `.claude/commands/research-digest.md` | 네이버 리서치 PDF 요약 야간 잡 | 제안 |
| 5 | PROJECT_FACTS 도장 갱신 | 수치는 맞고 도장만 낡음 | `py scripts/gen_facts.py --apply` |

## 4. 절차 개정 — 적용 완료
`research/RESEARCH_COUNCIL.md`: 멤버표에 backtest-runner·bias-auditor·quant-analyst 3행, data-sourcer·fundamental·macro 산출 칸을 코드·parquet로, 진행 순서 7.5 "구현 실행", 산출물 절 머리에 "파일로 생겨야 끝난다", 기록 규칙 마지막 줄을 쓰기 범위로. `docs/DECISIONS.md` D-103은 메인이 적는다.

## 5. 자기 영역 밖에서 본 문제
1. `PYQuant/tools/dart_fin_history_fill.py`·`macro_ingest.py`·`naver_research_fetch.py`·`PYQuant/data/keys.py`가 git 미추적 — 이번 커밋에 넣는다(데이터는 gitignore).
2. `fnlttMultiAcnt` 14계정에 매출총이익·영업활동현금흐름이 없다 — GP/A·발생액은 `fnlttSinglAcntAll` 없이는 못 만든다(strategist·fundamental-quant도 같은 지적).
3. 관세청 서비스 활용신청(15157908·15157901)은 오너 10분.
4. 리셋 §C 표 파일 이름 8개 중 실존은 이제 5개 — 표를 실제 이름으로 고친다(`dart_fin_backfill.py` → `dart_fin_history_fill.py` 등).
5. `RESEARCH_COUNCIL.md` 멤버표에 결과를 만드는 셋이 없었다 — 넣었다.
6. 헌장·에이전트 시니어 절의 "PIT"은 "시점 고정"으로 — 헌장은 고쳤고 에이전트 정의는 3-3과 같이.
7. `.claude/agents/reviewer.md` "ctest 미배선"은 틀렸다 — 35개 등록.

산출 파일: `research/COUNCIL_CHARTER.md`(§4·§5 개정), `research/RESEARCH_COUNCIL.md`(7.5·멤버 3행), `PYQuant/data/keys.py`(30줄), 이 보고. 에이전트 정의 적용은 `.claude/agents/*.md`(로컬 전용, VSCode 재시작 뒤 반영).
