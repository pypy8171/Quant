# 유지관리 자동화 가이드 — 문서·그래프·주석이 코드를 따라오게 하기

작성: 2026-09-08. 상태: 에이전트 회의(pm·committer·arch-doc·harness-engineer·reviewer) 반영본.
근거: 같은 날 전수 리뷰(2026-09-08, 리포트는 gitignore라 링크하지 않는다)에서 문서 14개 61건,
하네스 13건의 드리프트가 나왔다. 원인을 세어 보면 다섯 가지로 모인다.

| 원인 | 사례 | 건수 비중 |
|---|---|---|
| 줄번호 참조 | `main.cpp:184`, `Engine.cpp:349` — 편집 한 번에 전부 어긋남. 282건 전부 문서에 있고 227건이 `PIPELINE_A_to_Z.md` 한 곳 | 절반 이상 | <!-- drift-check: ok -->
| 손으로 센 개수 | 검사 11개→18, 커맨드 8→13, 에이전트 18→19, 훅 3→7, 테스트 5→11, 전략 13→16 | 다수 |
| 손으로 그린 트리 | PROJECT_GUIDE 디렉터리 트리에 `modes/`·`universe/`·전략 8종 누락 | 소수·규모 큼 |
| 생성기 미배선 | `gen_code_graph.py`는 사람이 기억해야 돌았다 | 1 |
| 평이화 스크럽이 기계 문자열까지 치환 | `"BH"`·로그 정규식 5개가 바뀌어 파서·대시보드가 깨짐 | 1(치명) | <!-- lexicon-ok: 금지어를 예시로 인용하는 행 -->

주석도 같은 병을 앓는다. OrderGate.h 머리의 검사 목록은 9개를 세는데 실제 거부 지점은 18곳이고,
뮤텍스 순서표에는 선언된 뮤텍스 두 개가 빠져 있으며, `sellable_` 행말주석은 `average_prices_` 것을 복사해 놓았다.
주석 정리는 삭제가 아니라 사실 검증부터다.

## 1. 원칙 다섯 가지

1. **셀 수 있는 것은 쓰지 않고 생성한다.** 개수·목록·트리·의존 그래프·config 키는 생성기가 만들고,
   문서에는 표식 블록(`<!-- gen:이름 -->` … `<!-- /gen -->`)으로 들어간다. 생성기는 블록 범위만 치환한다(파일 재작성 금지 —
   `PROJECT_FACTS.md`에는 `sync_ledgers.py`가 소유한 `AUTO:negative-results` 블록이 이미 있다). 검사기는 블록이 낡으면 실패한다.
   생성값은 백틱으로 감싸 평이화 스크럽에서 제외한다. `CLAUDE.md`에는 생성 블록을 두지 않는다(하네스 정본은 손으로 유지).
2. **줄번호가 아니라 심볼로 가리킨다.** 문서와 주석은 `Engine.cpp::control_thread_fn`처럼 파일과 심볼을 쓴다.
   검사기는 경로가 존재하는지(없으면 실패), 심볼이 그 파일 또는 짝 파일(`X.h`↔`X.cpp`)에 있는지(없으면 경고만) grep으로 확인한다.
   `파일:숫자` 형태의 새 줄번호 참조는 게이트에서 막되, 스냅샷 문서와 코드 펜스 안은 제외한다.
   줄번호를 나쁜 예시로 인용해야 하는 줄은 줄 끝에 `<!-- drift-check: ok -->`를 달아 그 줄만 뺀다.
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
| 장 마감 | `eod_autodoc.py`(대시보드 포함) 뒤에 `maintain.py --daily` — 시각은 계좌 모드로 갈린다(`scripts/eod_timetable.ps1`, 표는 `docs/AUTOMATION.md` 1절) | gen_facts → gen_code_graph(C++·Python) → sync_ledgers → gen_automation_hub(`_private/AUTOMATION_HUB.md`). 대시보드는 부르지 않는다(`refresh_dashboard.py`가 소유, 호출자를 늘리면 갈라진다). 결과는 `logs/maintenance.log` | 분 |
| 주간 | 금요일 예약 `maintain.py --weekly`(`claude_dashboard_sync` 뒤, 시각은 `scripts/eod_timetable.ps1`, `StartWhenAvailable` 켬. 일요일은 이 PC가 켜져 있다는 증거가 없고 cron-gate가 주말을 건너뛰어 미실행 감지도 안 된다) | 미참조 스크립트·에이전트 죽은 경로·부산물 용량·주석 밀도·`settings.json` 훅 배선 양방향 검사(훅 파일 전부 배선됐는가, 배선 경로 전부 실재하는가, BOM UTF-8인가)·`.claude/` 해시 매니페스트 → `docs/reports/MAINTENANCE_WEEKLY.md` | 제한 없음 |
| 세션 시작 | SessionStart 훅(cron-gate 4번째 항목) | 주간 리포트가 N일 낡았거나 빨간 항목이 있으면 한 줄 알림만. 세션 시작에 검사를 돌리지 않는다 | 밀리초 |

`maintain.py --check`는 보고만 한다. 자동 적용·자동 스테이징을 하지 않으며 검사기별로 exit code를 따로 낸다.
검사 범위는 diff에 걸린 파일만이다. 경로 기반 면제: `docs/reports/**`, `strategies/*/reviews/**`, `docs/eod/**`, `DAILY_LOG.md`.
`--weekly`는 `.claude/`를 읽기만 하고 보고한다. 훅·에이전트 수정은 사람이 승인한다(`.claude/`는 gitignore라 되돌릴 히스토리가 없다).

## 3. 생성기와 검사기

| 스크립트 | 역할 | 소비자 |
|---|---|---|
| `scripts/gen_facts.py` | 저장소를 세어 `docs/facts.json`을 만들고 표식 블록을 블록 범위로 치환한다. 항목: C++ 전략·전략 로더·Python 전략, OrderGate 거부 지점(`reject_reason =` 개수), 빌드 타깃·ctest 배선 여부, 커맨드·에이전트·스킬·훅(settings.json)·config 파일, 디렉터리 트리, config 키, 80줄 초과 함수. `_private/`·`logs/`·`out/`·`build_*/`는 제외하고 경로는 저장소 상대로만 찍는다 | PROJECT_FACTS.md, HARNESS.md, PROJECT_GUIDE.md, GLOSSARY.md, README.md, CODE_GRAPH_GUIDE.md |
| `scripts/gen_code_graph.py` | C++ include 그래프(현행) + Python import 그래프 + 프로세스 경계 파일(regime.json·prices_live.json·trades_*.csv) | CODE_GRAPH.md, code_graph.json |
| `scripts/check_code_refs.py` | 문서의 경로·`파일::심볼` 실재 검사, `파일:숫자` 줄번호 참조 신규 금지. 심볼 매칭은 `\b심볼\b`만 보고 시그니처·오버로드는 보지 않는다. `auto 이름 = [`(람다)·`#define 이름`도 정의로 인정 | docs-gate |
| `scripts/check_plain_language.py` | 기존 + 코드 모드에서 `re.*(` 인자·dict 키·비교식 우변·키워드 인자 값 보호, `--fix`는 보호 줄을 건너뛰고 경고 | lexicon-gate, committer(승인 후) |
| `scripts/log_patterns.py` | C++ 로그 문구 정규식의 단일 소유자. 구·신 문구 양쪽 허용 | eod_autodoc, eod_collect, notify_sidecar, summarize_trading_day, check_runtime_health |
| `scripts/_logdir.py` | 로그·원장 폴더 해석 한 곳(`QUANT_LOG_DIR` 최우선, 원장은 행 수 최대 → 동률 mtime) | eod_autodoc, summarize, dashboard_server, parse_quant_log, analyze_slot_cost |
| `scripts/maintain.py` | 위를 순서대로 부르는 진입점(`--daily`, `--weekly`, `--check`). 단계마다 `subprocess.run`으로 격리하고 rc는 로그에 남긴다(`eod_autodoc.py`와 같은 패턴) | 예약작업, committer |
| `docs/sync_map.json` | 소스 glob → 봐야 할 문서의 역인덱스. `review-reminder.ps1`이 이미 부르는 `git diff HEAD --name-only` 결과에 PS 네이티브(`ConvertFrom-Json`)로 매칭해 문서 이름을 지목한다. `py` 위임 금지(응답마다 0.8초) | Stop 훅 |

스냅샷 문서(그날의 리뷰·리포트·매매일지)는 머리에 `<!-- drift-check: snapshot 2026-09-08 -->`를 달아 검사에서 뺀다.
표식이 붙은 문서는 현행 근거로 인용하지 않는다. 지금의 `REALTIME_READINESS_REVIEW.md`·`OPTIMIZATION_REVIEW.md`·
`PIPELINE_A_to_Z.md`(줄번호 227건)가 여기 해당한다. 계속 갱신할 문서만 검사 대상이다.

## 4. 주석·표기 규약 (코드) — 정본

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

캐스트 표기 — C스타일 캐스트(`(int)x`)는 쓰지 않는다. 값은 `static_cast<T>(x)`, 포인터는 `reinterpret_cast<T>(x)`,
const 제거는 `const_cast<T>(x)`다. 미사용 인자 관용구 `(void)x;`만 예외다. 이유는 두 가지다 — C스타일은 한 문법이
static·reinterpret·const 세 가지를 겸해 무엇을 의도했는지 코드만 보고는 알 수 없고, `(int)`처럼 짧아 grep으로 훑을 수도
없다. 값이 잘리거나 부호가 뒤집히는 것을 막아주지는 않으니(동작은 `static_cast`도 같다), 범위가 걱정되면 캐스트 말고
호출측 가드나 `std::cmp_less` 같은 안전 비교를 쓴다. 검사는 `scripts/check_code_conventions.py` 5번 규칙이 추가된 줄만 본다.

복사 표기 — 안 해도 되는 복사는 만들지 않는다. 측정해서 느린 곳을 고치라는 원칙 7과 충돌하지 않는다. 여기서 말하는 것은
성능이 아니라 문법이다. 복사가 필요 없는 자리에 복사를 쓰면 읽는 사람이 "왜 여기서 값을 떠 가나"를 매번 다시 판단해야 한다.

- 조회 결과를 값으로 돌려주지 않는다. 멤버를 그대로 주면 `const std::string&`, 리터럴 폴백이 섞이면 `std::string_view`다
  (`std::string_view`를 돌려줄 때는 어느 컨테이너에 수명이 묶이는지 `// [inv]`로 적는다 — 재해시가 뷰를 끊는다).
- json 노드는 `j.value("k", json::array())`로 받지 않는다. 기본값을 만들려고 노드 전체를 베낀다 — 원소가 수천이면
  수천을 베낀다. `find()`로 참조를 잡고 없을 때만 함수 지역 `static const`의 빈 노드를 가리킨다.
- range-for는 `const auto&`가 기본이다. 값으로 받는 것은 원소가 정수·포인터일 때만이다.
- 값 전달 파라미터는 sink일 때만 쓴다 — 받은 값을 `std::move`로 멤버에 넣는 자리다. 읽기만 하면 `const&`다.
- 복사가 의도한 것이면(락 안에서 뜬 스냅샷, 호출자가 나중에 고칠 사본) 왜 복사인지 주석으로 남긴다. 그래야 다음 사람이
  지우지 않는다. `KisClient::token()`이 그 예다.

검사는 `scripts/check_code_conventions.py` 5번 규칙이 추가된 줄만 본다. json 쪽은 오류, 값 range-for는 원소 타입을 알 수
없어 경고다. 나머지 셋은 기계로 가릴 수 없으니 리뷰에서 본다.

이름 표기 — 약어를 쓰지 않는다. 변수·인자·멤버·함수·타입 모두 풀어 쓴다(`qty`→`quantity`, `cfg`→`config`, `it`→`iterator`,
`ev`→`event`, `sym`→`symbol_id`). 한 글자 이름도 같다(`i`·`n`·`x` 금지, 템플릿 인자 `T`·`U`만 예외). 이유는 성능이나 취향이
아니라 순서다 — 이 코드를 읽고 고치는 사람은 아직 도메인 용어(호가·체결·원장·유니버스·레인)를 익히는 중이고, 용어를 코드에서
매번 온전한 말로 만나야 몸에 붙는다. `qty`는 아는 사람에게만 `quantity`다. 익숙해진 뒤에 줄이는 것은 언제든 할 수 있지만,
약어로 시작하면 익숙해질 기회가 없다. 경위와 전수 변환 규칙은 D-092.

- 예외는 이름이지 약어가 아닌 것뿐이다: KIS 전문 필드(`tr_id`·`odno`·`hhmmss`, `[wire]` 줄), 지표 이름(`pnl`·`atr`·`pbr`·`per`·
  `p50`·`p99`), 단위 접미사(`_ns`·`_ms`·`_us`·`_sec`·`_min`), 표준 라이브러리·OS 멤버(`std::`·`zmq::` 한정 이름, `.str()`·`.ec`·
  `tm_min`·`sin_addr`), `argc`·`argv`·`ok`·`now`, 네임스페이스 별칭 `fs`.
- 길어지는 것은 감수한다. `simple_moving_average_20`이 `sma20`보다 길지만, 읽는 사람이 이동평균이라는 말을 한 번 더 본다.
- 검사는 `scripts/check_code_conventions.py` 7번 규칙이 추가된 C++ 코드 줄만 본다(오류). 판정 표는 리네임에 쓴
  `scripts/rename_maps/01_fields.json`·`scripts/rename_frags.py`를 그대로 쓰므로, 예외를 늘리려면 그 표(SKIP·WIRE)를 고친다.

초기화 위치 — 초기화는 세 목록에만 쓴다. 프로세스 수준(콘솔·로거·인자·설정·크래시 핸들러·모드 분기)은
`Quant/src/main.cpp`의 `main()` 호출 목록, 설정값을 엔진 세터에 옮기는 배선은 `Engine::configure(const AppConfig&)`
(`Quant/src/core/EngineConfigure.cpp`)의 호출 목록, 엔진 수준(샤드·인증·주문 라우터·원장·전략·피드·스레드)은
`Engine::start()`의 호출 목록이다. 새 초기화는 이름 있는 함수 하나로 만들고 그 목록에 한 줄을 더한다 — 함수 몸통
안에 섞어 넣거나 주기 블록·콜백에서 처음 불릴 때 만들지 않는다. 순서를 읽는 사람은 그 두 목록만 보면 되게 한다.
config.json을 읽는 곳은 `Quant/src/core/AppConfig.cpp`의 `parse_config()` 하나다(전략별 파라미터는
`strategy/StrategyFactory.cpp`가 예외) — Engine 세터와 모드 함수는 typed 값만 받고, 키 누락·값 오류는 네트워크를
건드리기 전에 그 자리에서 던진다. 함수 안 `static` 지역 변수는 `constexpr`(또는 상수 초기화되는 정수·bool·mutex)만
쓴다 — 람다·생성자로 채우는 매직 스태틱은 호출마다 초기화 가드를 거치고 첫 호출이 hot path에 걸리면 그때 비용을
낸다. 값이 컴파일 타임에 정해지면 함수 밖 `constexpr` 표로 빼고(`WebSocketClient.cpp`의 base64 역표가 그 예),
런타임 입력이 필요하면 위 두 목록으로 올린다.

주석을 줄이는 작업(에이전트 포함)의 확인 절차:
1. 지우기 전에 그 주장이 지금 코드와 맞는지 확인한다. 틀린 주석을 D-NNN으로 옮기면 오류를 정본에 승격시킨다.
2. 삭제 줄에서 숫자·식별자·ID(`\d+%`, 날짜, `[A-Z]-?\d+`, `§\d`)를 뽑아 각각이 남은 주석·DECISIONS·대상 문서 중 한 곳에 있는지 목록으로 보고한다. 없으면 삭제하지 않는다.
3. 코드 라인 diff는 0이어야 한다(`git diff -U0`에서 `//`·`*`가 없는 삭제·추가 줄 0). 주석 정리 커밋은 기능 수정 커밋과 분리한다.

## 5. 에이전트 쪽 요구

- 모든 에이전트는 개수·경로를 `docs/facts.json`이나 `PROJECT_FACTS.md`의 생성 블록에서 읽는다. 스스로 세지 않고 프롬프트에 박지 않는다.
  현재 거부 지점 수를 프롬프트에 복제한 곳(backtest-runner·planner·reviewer·log-reader·harness-engineer)은 정본 참조로 바꾼다.
- 문서를 쓰는 에이전트(arch-doc·prep-doc·review-recorder·eod 계열)는 줄번호 참조를 쓰지 않는다.
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

0. 자본이 걸린 결함부터(리뷰 리포트 §치명): OrderGate 미등록 종목 손익, DevScale 시장가 매도 `reference_price` 누락, ITB 미연결 포지션, WS 다중 레코드.
1. 스크럽 파괴 복구와 보호(원칙 5) — 지금 깨져 있다. `log_patterns.py` 신설.
2. `_logdir.py`와 로거 경로 고정 — 원장 오염 경로 차단.
3. `check_code_refs.py` + docs-gate 분기(shadow 1주). 이 문서 자신이 첫 검사 대상이다.
4. `gen_facts.py` + 표식 블록 6개 문서 — 개수 드리프트 종결. 에이전트 프롬프트의 복제 수치 제거.
5. `gen_code_graph.py` Python 확장 + `maintain.py --daily` 배선(`eod_autodoc` 뒤).
6. 주간 예약(금 20:50) + `sync_map.json` + review-reminder 지목 + cron-gate 4번째 항목.
