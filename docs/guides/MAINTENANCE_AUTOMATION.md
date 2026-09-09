# 유지관리 자동화 가이드 — 문서·그래프·주석이 코드를 따라오게 하기

작성: 2026-09-08. 상태: 에이전트 회의(pm·committer·arch-doc·harness-engineer·reviewer) 반영본.
근거: 같은 날 전수 리뷰(2026-09-08, 리포트는 gitignore라 링크하지 않는다)에서 문서 14개 61건,
하네스 13건의 드리프트가 나왔다. 원인을 세어 보면 다섯 가지로 모인다.

| 원인 | 사례 | 건수 비중 |
|---|---|---|
| 줄번호 앵커 | `main.cpp:184`, `Engine.cpp:349` — 편집 한 번에 전부 어긋남. 282건 전부 문서에 있고 227건이 `PIPELINE_A_to_Z.md` 한 곳 | 절반 이상 | <!-- drift-check: ok -->
| 손으로 센 개수 | 검사 11개→18, 커맨드 8→13, 에이전트 18→19, 훅 3→7, 테스트 5→11, 전략 13→16 | 다수 |
| 손으로 그린 트리 | PROJECT_GUIDE 디렉터리 트리에 `modes/`·`universe/`·전략 8종 누락 | 소수·규모 큼 |
| 생성기 미배선 | `gen_code_graph.py`는 사람이 기억해야 돌았다 | 1 |
| 평이화 스크럽이 기계 문자열까지 치환 | `"BH"`·로그 정규식 5개가 바뀌어 파서·대시보드가 깨짐 | 1(치명) | <!-- lexicon-ok: 금지어를 예시로 인용하는 행 -->

주석도 같은 병을 앓는다. OrderGate.h 머리의 검사 목록은 9개를 세는데 실제 거부 지점은 18곳이고,
뮤텍스 순서표에는 선언된 뮤텍스 두 개가 빠져 있으며, `sellable_` 행말주석은 `avg_prices_` 것을 복사해 놓았다.
주석 정리는 삭제가 아니라 사실 검증부터다.

## 1. 원칙 다섯 가지

1. **셀 수 있는 것은 쓰지 않고 생성한다.** 개수·목록·트리·의존 그래프·config 키는 생성기가 만들고,
   문서에는 표식 블록(`<!-- gen:이름 -->` … `<!-- /gen -->`)으로 들어간다. 생성기는 블록 범위만 치환한다(파일 재작성 금지 —
   `PROJECT_FACTS.md`에는 `sync_ledgers.py`가 소유한 `AUTO:negative-results` 블록이 이미 있다). 검사기는 블록이 낡으면 실패한다.
   생성값은 백틱으로 감싸 평이화 스크럽에서 제외한다. `CLAUDE.md`에는 생성 블록을 두지 않는다(하네스 정본은 손으로 유지).
2. **줄번호가 아니라 심볼로 가리킨다.** 문서와 주석은 `Engine.cpp::control_thread_fn`처럼 파일과 심볼을 쓴다.
   검사기는 경로가 존재하는지(없으면 실패), 심볼이 그 파일 또는 짝 파일(`X.h`↔`X.cpp`)에 있는지(없으면 경고만) grep으로 확인한다.
   `파일:숫자` 형태의 새 앵커는 게이트에서 막되, 스냅샷 문서와 코드 펜스 안은 제외한다.
   앵커를 나쁜 예시로 인용해야 하는 줄은 줄 끝에 `<!-- drift-check: ok -->`를 달아 그 줄만 뺀다.
3. **주석은 왜·불변식·함정만 남긴다.** 무엇을 하는지는 코드가, 경위는 `docs/DECISIONS.md`의 D-NNN이 말한다.
   목록·개수를 주석에 적지 않고 정본 위치를 가리킨다. 규약은 이 문서 §4가 정본이다.
4. **검사는 비용에 맞는 자리에 둔다.** 편집 훅은 밀리초, 커밋 게이트는 초, 장 마감 예약은 분, 주간은 제한 없음.
   지금 편집·Bash 훅이 호출당 약 2초씩 먹고 있으므로(PowerShell 기동) 훅 파일을 늘리지 않고 기존 훅 안에서 분기한다.
5. **기계가 읽는 문자열은 산문이 아니다.** 정규식·dict 키·센티넬·로그 매칭 리터럴·키워드 인자 값은 평이화 스크럽 대상에서 제외한다.
   Python 파서가 읽는 C++ 로그 문구는 `scripts/log_patterns.py` 한 곳이 정규식을 소유한다.
   스크럽의 `--fix`는 무인 경로(committer 자동 정정)에서 뺀다. 치환 diff를 보이고 승인받은 뒤 적용하며,
   대상 줄에 `re.`·dict 키·비교식이 있으면 치환하지 않고 경고한다.

## 2. 층별 배치

| 층 | 트리거 | 하는 일 | 예산 |
|---|---|---|---|
| 편집 | Write/Edit 훅(lexicon-gate) | 문체 검사만. 코드 리터럴·정규식은 건너뛴다 | 현행 약 2초, 목표 300ms |
| 커밋 전 | `docs-gate.ps1` 하나. 스테이징 목록을 `.md`와 `.h/.cpp/.py`로 분기(별도 code-gate 파일을 만들지 않는다 — Bash matcher에 훅을 더 얹으면 모든 Bash 호출이 2초씩 더 느려진다) | `.md`: check_docs + check_code_refs. 코드: check_code_refs + gen_facts --check + code_graph 최신 여부. 코드 분기는 도입 1주는 경고만(shadow) | < 5초 |
| 장 마감 | 16:05 `eod_autodoc.py`(대시보드 포함) 뒤에 `maintain.py --daily` | gen_facts → gen_code_graph(C++·Python) → sync_ledgers. 대시보드는 부르지 않는다(`refresh_dashboard.py`가 소유, 호출자를 늘리면 갈라진다). 결과는 `logs/maintenance.log` | 분 |
| 주간 | 금요일 20:50 예약 `maintain.py --weekly`(`claude_dashboard_sync` 20:40 뒤, `StartWhenAvailable` 켬. 일요일은 이 PC가 켜져 있다는 증거가 없고 cron-gate가 주말을 건너뛰어 미실행 감지도 안 된다) | 미참조 스크립트·에이전트 죽은 경로·부산물 용량·주석 밀도·`settings.json` 훅 배선 양방향 검사(훅 파일 전부 배선됐는가, 배선 경로 전부 실재하는가, BOM UTF-8인가)·`.claude/` 해시 매니페스트 → `docs/reports/MAINTENANCE_WEEKLY.md` | 제한 없음 |
| 세션 시작 | SessionStart 훅(cron-gate 4번째 항목) | 주간 리포트가 N일 낡았거나 빨간 항목이 있으면 한 줄 알림만. 세션 시작에 검사를 돌리지 않는다 | 밀리초 |

`maintain.py --check`는 보고만 한다. 자동 적용·자동 스테이징을 하지 않으며 검사기별로 exit code를 따로 낸다.
검사 범위는 diff에 걸린 파일만이다. 경로 기반 면제: `docs/reports/**`, `strategies/*/reviews/**`, `docs/eod/**`, `DAILY_LOG.md`.
`--weekly`는 `.claude/`를 읽기만 하고 보고한다. 훅·에이전트 수정은 사람이 승인한다(`.claude/`는 gitignore라 되돌릴 히스토리가 없다).

## 3. 생성기와 검사기

| 스크립트 | 역할 | 소비자 |
|---|---|---|
| `scripts/gen_facts.py` | 저장소를 세어 `docs/facts.json`을 만들고 표식 블록을 블록 범위로 치환한다. 항목: C++ 전략·전략 로더·Python 전략, OrderGate 거부 지점(`reject_reason =` 개수), 빌드 타깃·ctest 배선 여부, 커맨드·에이전트·스킬·훅(settings.json)·config 파일, 디렉터리 트리, config 키, 80줄 초과 함수. `_private/`·`logs/`·`out/`·`build_*/`는 제외하고 경로는 저장소 상대로만 찍는다 | PROJECT_FACTS.md, HARNESS.md, PROJECT_GUIDE.md, GLOSSARY.md, README.md, CODE_GRAPH_GUIDE.md |
| `scripts/gen_code_graph.py` | C++ include 그래프(현행) + Python import 그래프 + 프로세스 경계 파일(regime.json·prices_live.json·trades_*.csv) | CODE_GRAPH.md, code_graph.json |
| `scripts/check_code_refs.py` | 문서의 경로·`파일::심볼` 실재 검사, `파일:숫자` 앵커 신규 금지. 심볼 매칭은 `\b심볼\b`만 보고 시그니처·오버로드는 보지 않는다. `auto 이름 = [`(람다)·`#define 이름`도 정의로 인정 | docs-gate |
| `scripts/check_plain_language.py` | 기존 + 코드 모드에서 `re.*(` 인자·dict 키·비교식 우변·키워드 인자 값 보호, `--fix`는 보호 줄을 건너뛰고 경고 | lexicon-gate, committer(승인 후) |
| `scripts/log_patterns.py` | C++ 로그 문구 정규식의 단일 소유자. 구·신 문구 양쪽 허용 | eod_autodoc, eod_collect, notify_sidecar, summarize_trading_day, check_runtime_health |
| `scripts/_logdir.py` | 로그·원장 폴더 해석 한 곳(`QUANT_LOG_DIR` 최우선, 원장은 행 수 최대 → 동률 mtime) | eod_autodoc, summarize, dashboard_server, parse_quant_log, analyze_slot_cost |
| `scripts/maintain.py` | 위를 순서대로 부르는 진입점(`--daily`, `--weekly`, `--check`). 단계마다 `subprocess.run`으로 격리하고 rc는 로그에 남긴다(`eod_autodoc.py`와 같은 패턴) | 예약작업, committer |
| `docs/sync_map.json` | 소스 glob → 봐야 할 문서의 역인덱스. `review-reminder.ps1`이 이미 부르는 `git diff HEAD --name-only` 결과에 PS 네이티브(`ConvertFrom-Json`)로 매칭해 문서 이름을 지목한다. `py` 위임 금지(응답마다 0.8초) | Stop 훅 |

스냅샷 문서(그날의 리뷰·리포트·매매일지)는 머리에 `<!-- drift-check: snapshot 2026-09-08 -->`를 달아 검사에서 뺀다.
표식이 붙은 문서는 현행 근거로 인용하지 않는다. 지금의 `REALTIME_READINESS_REVIEW.md`·`OPTIMIZATION_REVIEW.md`·
`PIPELINE_A_to_Z.md`(줄번호 227건)가 여기 해당한다. 계속 갱신할 문서만 검사 대상이다.

## 4. 주석 규약 (코드) — 정본

- 파일 머리: 목적 한 줄, 스레드 소유권(어느 메서드를 어느 스레드가 부르는지), 관련 결정 D-NNN. 목록·개수는 적지 않고
  정본 위치를 가리킨다(예: "검사 항목과 순서의 정본은 `OrderGate.cpp::check` 하나다").
- 함수 위: 왜 이 함수가 따로 있는지, 호출 제약(어느 스레드·락 상태), 실패 시 동작. 절차 서술 금지.
- 멤버 옆: 단위와 불변식만(`// 원, 장중 갱신`), 경위는 D-NNN.
- 블록 안: 함정과 비자명한 결정만. 태그 없는 블록이 세 줄을 넘으면 함수로 뽑거나 D-NNN으로 보낸다.
- 줄 수 제한의 예외는 접두 태그로 표시한다. 함수로 뽑을 수 없는 지식은 코드 옆에 둔다.
  `// [inv]` 불변식 · `// [lock-order]` 락 순서·memory_order 근거 · `// [wire]` 외부 프로토콜 필드 인덱스·에러코드 ·
  `// [why D-NNN]` 결정 참조 · `// [formula]` 수식·임계값 유도.
- 경위를 D-NNN으로 옮길 때: 코드 주석의 별칭(`MM-1`, `G1`, `C-2`, `B2`, `H-1` 등)과 D-NNN의 매핑표를 먼저 한 번 만들고,
  이관은 파일별로 그 파일을 고칠 때 같이 한다. 대응 항목이 없으면 신규 발번하고 `**상태**: 사후 기록(코드 주석에서 이관)`을 단다.
- 밀도는 게이트로 걸지 않는다. 주간 리포트에 파일별 밀도와 태그 없는 4줄 이상 연속 주석 블록만 표로 남긴다.

주석을 줄이는 작업(에이전트 포함)의 확인 절차:
1. 지우기 전에 그 주장이 지금 코드와 맞는지 확인한다. 틀린 주석을 D-NNN으로 옮기면 오류를 정본에 승격시킨다.
2. 삭제 줄에서 숫자·식별자·ID(`\d+%`, 날짜, `[A-Z]-?\d+`, `§\d`)를 뽑아 각각이 남은 주석·DECISIONS·대상 문서 중 한 곳에 있는지 목록으로 보고한다. 없으면 삭제하지 않는다.
3. 코드 라인 diff는 0이어야 한다(`git diff -U0`에서 `//`·`*`가 없는 삭제·추가 줄 0). 주석 정리 커밋은 기능 수정 커밋과 분리한다.

## 5. 에이전트 쪽 요구

- 모든 에이전트는 개수·경로를 `docs/facts.json`이나 `PROJECT_FACTS.md`의 생성 블록에서 읽는다. 스스로 세지 않고 프롬프트에 박지 않는다.
  현재 거부 지점 수를 프롬프트에 복제한 곳(backtest-runner·planner·reviewer·log-reader·harness-engineer)은 앵커 참조로 바꾼다.
- 문서를 쓰는 에이전트(arch-doc·prep-doc·review-recorder·eod 계열)는 줄번호 앵커를 쓰지 않는다.
- committer는 커밋 전 `maintain.py --check`를 부른다(check_docs를 포함하므로 호출 하나로 대체). 스크럽 `--fix`는 자동으로 돌리지 않는다.
- 테스트를 실행하는 에이전트(reviewer·dev-loop)는 반드시 임시 폴더에서 실행한다. 로거는 `QUANT_LOG_DIR` 미설정 시
  실행파일 기준 폴더에 쓰도록 바꿔 cwd 사고를 차단한다.
- `.claude/`는 커밋되지 않으므로 커밋 게이트가 못 잡는다. 주간 `maintain.py --weekly`가 에이전트·커맨드의 죽은 경로와 `settings.json` 배선을 검사한다.

## 6. 하지 않는 것

- 주석 밀도 임계값 게이트: 우회만 늘어난다.
- Stop 훅에 재생성 추가: 응답마다 도는 자리다. mtime 비교로 건너뛰는 현행 `--if-stale`만 둔다.
- git hook: 게이트가 Claude 훅과 예약작업 둘로 이미 충분하고, 다른 클론에서 실행 환경(py 런처·PowerShell)을 보장할 수 없다.
- `maintain.py`의 자동 스테이징·자동 수정: 검사기는 보고만 한다.
- 훅 파일 추가: 기존 훅 안에서 분기한다.

## 7. 도입 순서

0. 자본이 걸린 결함부터(리뷰 리포트 §치명): OrderGate 미등록 종목 손익, DevScale 시장가 매도 `ref_price` 누락, ITB 미연결 포지션, WS 다중 레코드.
1. 스크럽 파괴 복구와 보호(원칙 5) — 지금 깨져 있다. `log_patterns.py` 신설.
2. `_logdir.py`와 로거 앵커 — 원장 오염 경로 차단.
3. `check_code_refs.py` + docs-gate 분기(shadow 1주). 이 문서 자신이 첫 검사 대상이다.
4. `gen_facts.py` + 표식 블록 6개 문서 — 개수 드리프트 종결. 에이전트 프롬프트의 복제 수치 제거.
5. `gen_code_graph.py` Python 확장 + `maintain.py --daily` 16:05 배선(`eod_autodoc` 뒤).
6. 주간 예약(금 20:50) + `sync_map.json` + review-reminder 지목 + cron-gate 4번째 항목.
