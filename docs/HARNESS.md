# HARNESS.md — 하네스·루프 엔지니어링

이 문서는 이 저장소에서 Claude Code를 감싸는 자동화 층이 실제로 어떻게 동작하고 무슨 효과를 내는지 정리한다. 대상 파일은 대부분 `.claude/`(gitignore 로컬 전용) 아래에 있고, 스크립트·보조 프로세스는 저장소에 있다.

> **다이어그램 페이지**: 아래 구조를 SVG로 그린 공유용 페이지 — [퀀트 하네스 설계도](https://claude.ai/code/artifact/7b76a70f-c3f8-44d0-8d14-efeb3d1fd91d) (5레버 프레임·런타임 파이프라인·루프·효과표, 라이트/다크 대응)

## 두 개념

- **하네스 엔지니어링**은 모델을 감싸는 환경(규칙·도구·피드백·제약)을 만드는 일이다. 모델 자체를 바꾸지 않고 5레버(CLAUDE.md / 스킬 / MCP / 서브에이전트 / 훅)로 무엇을 할 수 있고 무엇을 막을지 정한다.
- **루프 엔지니어링**은 반복되는 사이클을 사람 개입 없이 돌게 만드는 일이다. "작업·검증·반영" 또는 "관측·판단·행동"을 게이트로 묶어 자율적으로 순환시킨다.

하네스가 정적 구조(무대·규칙)라면 루프는 그 위에서 도는 동적 흐름이다.

## 1. 하네스 5레버

### ① CLAUDE.md — 규칙

매 세션 자동 로드된다. 빌드 명령·실행 모드·아키텍처와 함께 토큰 이코노미 표(작업 유형별 사전 체크리스트)와 문체 규약 요약을 담는다. 문체 규약 본문은 매 세션 재전송을 피하려고 [STYLE_GUIDE.md](STYLE_GUIDE.md)로 분리했고, CLAUDE.md에는 요약과 링크만 둔다.

효과: 같은 설명을 반복하지 않아도 동일한 규율이 강제된다. 토큰 이코노미 표가 재읽기 금지·탐색 위임·병렬 호출을 매 작업에 적용시켜 토큰 낭비를 줄인다.

### ② 스킬/커맨드 — 도구

`.claude/commands/`와 `.claude/skills/` 아래에 있다. 자연어나 슬래시로 호출하면 정해진 절차가 로드된다.
개수와 목록은 `py scripts/gen_facts.py --apply`가 아래 표를 채운다.

<!-- gen:harness-counts -->
| 항목 | 개수 | 이름 |
|---|---|---|
| 커맨드 | `14` | `auto-trade-day`, `build`, `comment-diet`, `daily`, `dashboard-sync`, `dev-loop`, `eod-review`, `intraday-start`, `review-apply`, `review-bundle`, `strategy-debate`, `trade-log`, `verify-backtest`, `watch` |
| 에이전트 | `19` | `arch-doc`, `backtest-runner`, `bias-auditor`, `claude-coach`, `committer`, `data-sourcer`, `harness-engineer`, `interviewer`, `intraday-analyst`, `log-reader`, `market-brief`, `perf-optimizer`, `planner`, `pm`, `prep-doc`, `quant-analyst`, `review-recorder`, `reviewer`, `strategist` |
| 스킬 | `1` | `stock-study` |
| 훅 파일 | `8` | `cron-gate.ps1`, `dashboard-refresh.ps1`, `docs-gate.ps1`, `eod-gate.ps1`, `lexicon-gate.ps1`, `output-gate.ps1`, `review-reminder.ps1`, `secret-gate.ps1` |
| settings.json 훅 배선 | `8` | `PreToolUse:secret-gate.ps1`, `PreToolUse:docs-gate.ps1`, `PreToolUse:lexicon-gate.ps1`, `Stop:output-gate.ps1`, `Stop:review-reminder.ps1`, `Stop:dashboard-refresh.ps1`, `SessionStart:eod-gate.ps1`, `SessionStart:cron-gate.ps1` |
<!-- /gen -->

효과: 반복 절차의 재작성·재승인이 사라진다. `/build`는 `settings.local.json`에 15개 넘게 쌓여 있던 vcvars64+cmake 변형을 하나의 절차로 고정했다(한글 임시폴더 경로로 인한 링커 오류 `LNK1104` 회피 포함).

### ③ MCP — 외부 연결

Notion 커넥터가 연결돼 있다. 개발로그와 장전 시황 브리핑을 Notion에 적재한다.

효과: 산출물이 세션 스크롤백에 갇히지 않고 검색·누적 가능한 외부 저장소로 나간다. KIS는 C++ 네이티브로 직접 붙였으므로 MCP로 감싸지 않는다.

### ④ 서브에이전트 — 분업

자연어 위임으로 전문 롤이 격리된 컨텍스트에서 실행되고 결론만 메인으로 돌아온다(파일 덤프가 메인 컨텍스트에 쌓이지 않아 토큰이 절약된다).

설계의 핵심은 "단일 답을 의심하는 구조"다. 에이전트 수를 늘리는 것이 목적이 아니라 서로 의심하게 만들어 여러 명을 한 명의 시니어처럼 굴린다. 백테스트 파이프라인이 대표적이다.

| 단계 | 에이전트 | 책임 |
|---|---|---|
| 엣지 | `@strategist` | 전략 가설 |
| 데이터 | `@data-sourcer` | 합법·무료·정확하게 구할 수 있는가(백테스트 가용성과 라이브 가용성 구분) |
| 재현 | `@backtest-runner` | 같은 입력이면 같은 숫자 + 비용·체결 배선 |
| 감사 | `@bias-auditor` | 미래정보(lookahead)·생존편향·과최적화 반증 |
| 판정 | `@quant-analyst` | 성과 크기·유의성, 표본 못 버티면 결론 보류 |

각 단계는 앞 단계를 의심한다. `@harness-engineer`는 하네스 자체를 감사하는 메타 롤이다. 전체 목록과 호출 예시는 `.claude/AGENTS.md`에 있다.

### ⑤ 훅 — 결정론적 백스톱

사람 판단에 의존하면 언젠가 새는 것을 코드로 막는 층이다. 모두 fail-open이다(파싱 실패·에러 시 exit 0으로 정상 워크플로우를 깨지 않는다).

배선은 아래 표가 `.claude/settings.json`에서 그대로 뽑는다.

<!-- gen:hooks -->
| 이벤트 | matcher | 훅 파일 |
|---|---|---|
| `PreToolUse` | `Bash|PowerShell` | `secret-gate.ps1` |
| `PreToolUse` | `Bash|PowerShell` | `docs-gate.ps1` |
| `PreToolUse` | `Write|Edit|MultiEdit|NotebookEdit|Bash|PowerShell` | `lexicon-gate.ps1` |
| `Stop` | `(전체)` | `output-gate.ps1` |
| `Stop` | `(전체)` | `review-reminder.ps1` |
| `Stop` | `(전체)` | `dashboard-refresh.ps1` |
| `SessionStart` | `(전체)` | `eod-gate.ps1` |
| `SessionStart` | `(전체)` | `cron-gate.ps1` |
<!-- /gen -->

하는 일은 이렇다. `secret-gate.ps1`은 `git commit`/`push`를 가로채 스테이징 diff에서 실거래 키·계좌번호·개인정보
패턴을 스캔해 발견 시 차단한다. `docs-gate.ps1`은 커밋에 `.md`가 있으면 `scripts/check_docs.py`를 돌려
드리프트면 차단한다. `lexicon-gate.ps1`은 쓰려는 본문을 `scripts/check_plain_language.py`로 검사한다.
`output-gate.ps1`·`review-reminder.ps1`·`dashboard-refresh.ps1`은 응답 뒤, `eod-gate.ps1`·`cron-gate.ps1`은
세션 시작 때 각각 점검 결과를 알린다.

효과: 실거래 키 유출과 문서 드리프트가 사람의 주의력이 아니라 기계적으로 차단된다. 실거래 키가 저장소에 있는 1인 운영 환경에서 이 층의 가치가 크다. 배선은 `.claude/settings.json`의 `hooks`에 있다.

## 2. 루프

### A. 개발 루프 — `/dev-loop`

한 작업을 범위확정, 완료기준(DoD) 선언, 작업, 빌드 게이트, 테스트 게이트, `@reviewer` 게이트, 커밋으로 묶어 게이트가 통과할 때까지 자율 반복한다. 실패하면 그 게이트부터 다시 돈다.

결정론적 테스트가 코드로 존재하는 영역에서만 허용한다(OrderGate·OrderRouter·regime 등). 라이브 전략·실주문처럼 FEED 모드로 눈으로 확인해야 하는 영역은 루프를 돌리지 않는다. 검증용 테스트가 없으면 최소 회귀 테스트를 먼저 추가하고 통과시킨다.

### B. 검증 루프 — `/verify-backtest`

백테스트 결과를 결론으로 승격하기 전 재현성 게이트, 편향감사와 성과판정, 종합판결의 파이프라인을 강제한다. ④의 4-에이전트 분업이 여기서 돈다. 과최적화·미래정보·표본부족이 감사를 통과해야만 결론이 살아남는다(유죄 추정, 반증 책임은 백테스트에).

### C. 런타임 자동화 루프 — regime.json 파일 전달

보조 프로세스 `PYQuant/tools/macro_regime_feed.py`가 매크로 국면을 판정해 `regime.json`을 주기 갱신하면, C++ 엔진이 이를 폴링해 `OrderGate::set_entry_halt`를 토글한다. 신규매수만 차단하고 청산은 통과시킨다. 프로세스 간 결합을 파일 하나로 느슨하게 유지하면서 급락 국면에서 신규 진입을 자동 차단한다. 파일이 오래되면(`regime_stale_sec` 초과) stale로 간주해 안전측으로 진입을 막는다.

### D. 스케줄 루프 — 장전 시황 브리핑

클라우드 routine이 평일 08:30 KST(cron `30 23 * * 0-4` UTC)에 자율 세션을 띄워 시황을 웹으로 조회하고, 이 시스템의 유니버스·국면 관점으로 브리핑을 Notion에 적재한다. 매일 아침 손으로 `@market-brief`를 호출하던 것을 무인 반복으로 옮긴 것이다. 이 routine은 `.claude/` 밖의 클라우드 인프라에서 돌며 로컬 파일·로그에 접근하지 못한다.

### E. 상시 알림 루프 — Stop 훅

위 ⑤의 `review-reminder.ps1`이 코드 수정 후 리뷰를 잊지 않게 하는 리마인더로 매 응답 뒤에 돈다.

## 3. 효과 정리 (두 목표 축)

모든 산출물은 목표1(수익 메커니즘)과 목표2(설계 설명력·견고성) 중 어디에 기여하는지로 평가한다.

| 레버·루프 | 목표1 — 수익 메커니즘 | 목표2 — 설계 설명력 |
|---|---|---|
| secret-gate / docs-gate 훅 | 실거래 키 유출·문서 드리프트 차단(운영 안정) | 사람 판단에 의존하지 않는 결정론적 백스톱 설계 |
| dev-loop / verify-backtest | 검증 안 된 코드·엣지가 라이브로 못 감 | 테스트 게이트 없으면 루프 거부하는 규율 |
| 4-에이전트 분업 | 낙관적 백테스트 결론을 걸러냄 | 단일 답을 의심하는 구조 |
| regime.json 브리지 | 급락장 신규진입 자동 차단 | 락-프리 파이프라인과 파일 전달 프로세스 간 통신(IPC) |
| 토큰 이코노미 표 | 개발 효율 | — |

초점은 AI 성능 향상이 아니라, 실수·과신·유출을 줄이고 반복 작업을 자동화하는 데 있다. 실거래 키가 저장소에 있고 1인이 운영하므로, 게이트와 검증 루프의 값이 특히 크다.

## 부록 — 여러 세션을 동시에 띄웠을 때

같은 저장소에 세션을 여럿 띄우면 서로 메시지를 보낼 수 있다. 주소는 탭 제목이 아니라 `quant-XX`
형태의 이름이고, 그 값은 **각 세션이 자기 자신만 안다**. 밖에서 목록을 보면 `quant-85`, `quant-07`
처럼 이름만 나오고 어느 탭인지는 나오지 않는다. 그래서 대상을 못 고르면 전 세션에 뿌리게 되는데,
관계없는 세션도 각자 턴을 한 번씩 돌아 토큰을 쓴다.

대상 세션 탭에서 한 줄 물으면 된다.

```
너 세션 이름 뭐야?
```

`This session is quant-07 [3f6100]` 같은 답이 나온다. 앞의 `quant-07`이 주소다. 세션을 새로 열 때
한 번 물어 적어두면 다음부터 지목해서 보낼 수 있다. `/status`에도 세션 UUID가 나오지만 그건 메시지
주소가 아니다.

세션을 나눠 쓸 때 실제로 부딪힌 것은 빌드다. 여러 세션이 같은 `Quant/build_win`에 각자 링크하면
한쪽이 반쯤 고친 트리를 다른 쪽이 링크해 계좌에 태울 수 있다. 편집은 나눠 하더라도 빌드·링크는
한 세션으로 몰고, 링크 전에 `ninja -n`으로 재컴파일 대상을 확인한다(파일 mtime이 되돌아가 있으면
ninja가 조용히 스킵한다).

## 갱신 참고

- 무엇이 언제 스스로 도는지(예약작업·루틴·마감 파이프라인)는 [docs/AUTOMATION.md](AUTOMATION.md)가 목록으로 소유한다. 여기서는 설계 의도만 다룬다.

- `.claude/`(에이전트·커맨드·훅)는 gitignore 로컬 전용이다. 여기의 변경은 커밋되지 않고 VSCode 재시작 후 적용된다(훅은 즉시 적용). 스크립트·보조 프로세스는 저장소에 있다.
- PowerShell 훅은 UTF-8 BOM으로 저장하고 한글 경로 리터럴을 피한다(`$PSScriptRoot`에서 repo 루트를 유도). PowerShell 5.1이 BOM 없는 UTF-8 한글을 시스템 코드페이지로 오독하기 때문이다.
- 에이전트 개수·목록이 바뀌면 `.claude/AGENTS.md`의 빠른 참조 개수와 목록을 함께 갱신한다.
- 이 문서를 저장소 색인에 넣을 때는 [docs/SYNC_MAP.md](SYNC_MAP.md)의 의존 표에 등록해 `scripts/check_docs.py` 게이트를 통과시킨다.
