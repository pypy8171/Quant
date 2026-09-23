# SYNC_MAP — 문서 동기화 맵 (드리프트 방지)

> 목적: **자식(소스) 파일을 고쳤을 때 같이 갱신해야 할 대표(색인) 파일**을 즉시 특정한다.
> 색인·요약·링크가 실제 트리와 어긋나는 드리프트(과거 07/08/09 스터디가 상위 색인에서 누락됐던 사고)를 막는다.

두 층으로 검사한다:
1. **결정론 검사(자동)** — `../quant-devtools/check_docs.py`. 깨진 내부 링크 + 색인 커버리지를 기계적으로 잡는다. `@committer`가 커밋 전 실행한다.
2. **의미적 체크리스트(사람)** — 스크립트가 못 잡는 "요약 문구가 최신인가·결론이 바뀌었나"를 아래 표로 확인한다.

---

## 1. 소유권 원칙 — 1서사 = 1소유자

같은 내용을 여러 곳에 복제하지 않는다. 각 서사는 **소유자 1곳**만 풀텍스트로 쓰고, 나머지는 링크한다.

| 서사 | 소유자(풀텍스트) | 링크만(복제 금지) |
|---|---|---|
| 백테스트 실행 상세(규칙변경·결과표·해석) | `research/BACKTEST_LOG.md` | `research/BACKTESTS.md` 카드, `research/README.md` 타임라인 |
| 스터디 내용(방법·결과·종목원장) | `research/studies/<NN>/README.md` | `research/studies/README.md` 색인, `BACKTESTS.md` 카드 |
| 전략 스펙·실증 | `strategies/<전략>/`(SPEC·live/) | `strategies/README.md` 표, 루트 `README.md` |
| 장 종료 리뷰(코드·매매·주문 문제와 조치) | `docs/market_close/YYYY-MM-DD.md` | `docs/market_close/README.md` 목록, `docs/DEFERRED_ISSUES.md`·`docs/DECISIONS.md` 항목 |
| 장전 시황 브리핑(아침 매크로·스탠스) | `docs/premarket/YYYY-MM-DD.md` | `docs/premarket/README.md` 목록, 대시보드 장전 브리핑 탭(생성) |
| 백테스트 규율 | `research/GUARDRAILS.md` | 각 study README, `bias-auditor` |

---

## 2. 의존 표 — 자식이 바뀌면 이 대표 파일을 갱신하라

정본은 [`docs/sync_map.toml`](sync_map.toml)이고 아래 표는 `py ../quant-devtools/sync_impact.py --render`가 만든다(마커 안 손편집 금지).
"검사" 열이 누가 낡음을 확정하는지다 — **자동**은 기계가 재생성하고, **도장**은 문서 안 `<!-- sync: 경로@해시 -->`가
소스 버전을 기억해 바뀐 문단만 집어내며, **힌트**는 매핑만 있어 같은 소스 버전에 한 번 알려준다.

<!-- sync-map:rules -->
| 소스(바뀌면) | 대표 문서(봐라) | 검사 | 맞출 것 |
|---|---|---|---|
| `scripts/market_close_timetable.ps1` | `docs/AUTOMATION.md#1. OS 예약작업` | 자동(gen 블록) | 마감 시간표(모의/실계좌)는 gen:market-close-timetable 블록. _private/AUTOMATION_HUB.md 는 gen_facts --apply 가 같이 다시 쓴다 |
| `scripts/market_close_timetable.ps1` | `docs/guides/MAINTENANCE_AUTOMATION.md#자동화 층`, `.claude/commands/dashboard-sync.md`, `.claude/commands/auto-trade-day.md`, `.claude/skills/stock-study/SKILL.md` | 힌트 | 예약작업 시각을 글로 적은 곳. 시각 숫자는 넣지 말고 허브·시간표 스크립트를 가리키게 한다 |
| `PYQuant/tools/macro_regime_feed.py` | `docs/premarket/ROUTINE_PROMPT.md#이 시스템의 국면 모델` | 자동(gen 블록) | 루틴 프롬프트 안의 국면 모델(지표 8개·임계·정지/청산선)은 gen:regime-model 블록. 블록이 바뀌면 check_docs 가 '올린 것과 다르다'고 잡는다 → /schedule 로 다시 올리고 premarket_routine.py --mark |
| `Quant/CMakeLists.txt`, `Quant/tests/test_*.cpp` | `docs/guides/PROJECT_GUIDE.md#단위 테스트` | 자동(gen 블록) | 테스트 타깃 목록·개수는 gen:test-targets 블록이 채운다 |
| `Quant/include/strategy/*.h`, `Quant/src/strategy/StrategyFactory.cpp`, `PYQuant/strategy/*.py` | `README.md`, `docs/guides/PROJECT_GUIDE.md`, `.claude/PROJECT_FACTS.md` | 자동(gen 블록) | 전략 클래스·로더 표는 gen:cpp-strategies / gen:py-strategies 블록 |
| `.claude/commands/*.md`, `.claude/agents/*.md`, `.claude/skills/**`, `.claude/hooks/*.ps1`, `.claude/settings.json` | `docs/HARNESS.md`, `docs/AUTOMATION.md#훅` | 자동(gen 블록) | 개수·훅 배선표는 gen:harness-counts / gen:hooks 블록. 훅이 하는 일 설명 문단은 stamp |
| `Quant/include/**/*.h`, `Quant/src/**/*.cpp` | `docs/CODE_GRAPH.md` | 자동(명령) `py ../quant-devtools/gen_code_graph.py --check` | #include 그래프는 손편집 금지, 재생성 |
| `Quant/include/**/*.h`, `Quant/src/**/*.cpp`, `docs/code_flow.toml` | `docs/CODE_FLOW.md` | 자동(명령) `py ../quant-devtools/gen_code_flow.py --check` | 코드 흐름 문서의 줄 링크·시그니처는 재생성(fix_cmd). 심볼이 사라지면 code_flow.toml의 sym을 고친다 |
| `Quant/include/**/*.h`, `Quant/src/**/*.cpp`, `scripts/*.py`, `scripts/*.ps1`, `PYQuant/tools/*.py`, `PYQuant/data/*.py`, `docs/tuning_sheet.toml`, `scripts/gen_tuning_sheet.py` | `_private/TUNING_SHEET.md`, `_private/TUNING_CYCLE.md` | 자동(명령) `py scripts/gen_tuning_sheet.py --check` | 장중 매매 수치·주기 시트는 재생성(fix_cmd). config 변경은 git diff 에 안 잡히므로 sync-gate 가 매 턴 --check 를 따로 돈다. code_value 정규식이 안 잡히면 tuning_sheet.toml 을 고친다 |
| `docs/DECISIONS.md`, `Quant/config/config_dev_paper.json` | `research/STRATEGY_LAB.md`, `.claude/PROJECT_FACTS.md` | 자동(명령) `py ../quant-devtools/sync_ledgers.py --check` | **원장**:·**음성결과**: 줄에서 마커 구간 생성 |
| `Quant/include/core/*.h`, `Quant/src/core/*.cpp`, `Quant/src/main.cpp` | `docs/ENGINE_ARCHITECTURE.md#아키텍처`, `docs/guides/PROJECT_GUIDE.md` | 도장 | 스레드 모델·모듈 책임 요약 문단. 헤더의 공개 역할이 바뀌면 문단을 고치고 --restamp |
| `Quant/include/api/*.h`, `Quant/src/api/*.cpp` | `docs/ENGINE_ARCHITECTURE.md#KIS API 클라이언트`, `docs/ENGINE_ARCHITECTURE.md#WebSocket 클라이언트` | 도장 | 파일 분할·인터페이스·채널 목록 요약 |
| `Quant/include/risk/*.h`, `Quant/src/risk/*.cpp` | `README.md`, `docs/GLOSSARY.md` | 도장 | OrderGate 거부 사유·한도 설명. 거부 지점 개수는 gen:ordergate-rejects |
| `Quant/tools/ops_terminal/**` | `docs/guides/MFC_TERMINAL.md`, `_private/LINKS.md` | 도장 | 화면·스레드 모델·빌드 조건 절 + 이력 한 줄. 실행 방법이 바뀌면 LINKS.md 운영단말 행 |
| `scripts/dashboard_server.py`, `PYQuant/dashboard/**` | `docs/design/DASHBOARD_SPEC.md` | 도장 | API·데이터 계약 |
| `scripts/*.py`, `scripts/*.ps1` | `docs/AUTOMATION.md#스크립트`, `docs/SYNC_MAP.md` | 힌트 | 새 스크립트는 AUTOMATION.md 표에 행. 검사기·생성기면 SYNC_MAP.md에도 |
| `research/studies/*/README.md` | `research/studies/README.md`, `research/BACKTESTS.md`, `research/README.md` | 자동(명령) `py ../quant-devtools/check_docs.py` | 색인 등재는 check_docs가 잡는다. 요지 1줄·타임라인 서사는 사람 |
| `strategies/*/SPEC.md`, `strategies/*/live/*.md` | `strategies/README.md`, `README.md` | 힌트 | 표에 SPEC·실증·검증경로 행 |
| `docs/STYLE_GUIDE.md` | `CLAUDE.md#문서 문체 규약`, `.claude/hooks/lexicon-gate.ps1` | 힌트 | 금지 표현을 추가했으면 게이트 두 곳의 사전도 |
| `docs/guides/CODE_CONVENTIONS.md` | `CLAUDE.md#코드 작업 규약` | 힌트 | 규약을 더하거나 예외를 늘리면 검사기 규칙과 판정 표도. 약어 예외는 rename_frags.py의 SKIP·WIRE가 정본 |
| `docs/sync_map.toml` | `docs/SYNC_MAP.md`, `docs/AUTOMATION.md#스크립트`, `docs/HARNESS.md` | 자동(명령) `py ../quant-devtools/sync_impact.py --render --check` | SYNC_MAP.md §2 표는 이 파일에서 생성 |
| `docs/RUNBOOK.md` | `docs/RUNBOOK.html` | 자동(명령) `py ../quant-devtools/gen_runbook.py --check` | 운영 런북 HTML 은 RUNBOOK.md 에서 통째로 렌더(gitignore). 코드 블록의 스크립트 경로가 없으면 --check 가 잡는다 |
| `scripts/auto_trade_day.ps1`, `scripts/auto_trade_guard.ps1`, `scripts/dashboard_server.py`, `scripts/parse_quant_log.py`, `scripts/notify_trades.py`, `PYQuant/tools/macro_regime_feed.py`, `PYQuant/tools/universe_feed.py`, `PYQuant/tools/investor_flow_logger.py`, `PYQuant/tools/index_intraday_logger.py`, `docs/guides/OPS_TERMINAL.md`, `docs/guides/MFC_TERMINAL.md` | `docs/RUNBOOK.md` | 도장 | 런북이 인용하는 스크립트 인자·옵션. 절 머리 도장이 낡으면 그 절의 명령을 맞추고 --restamp docs/RUNBOOK.md |
| `research/COUNCIL_CHARTER.md`, `research/GUARDRAILS.md` | `research/RESEARCH_COUNCIL.md#멤버와 역할`, `.claude/agents/bias-auditor.md`, `.claude/AGENTS.md` | 힌트 | 헌장·규율(D-101)이 바뀌면 회의 멤버 표, 편향 감사관의 대조 기준, 에이전트 색인을 같이 본다 |
<!-- /sync-map:rules -->

> `research/BACKTEST_LOG.md`는 **소스(소유자)**라 위 표의 "대표"가 아니다 — 다른 문서가 이걸 링크한다.
> `STRATEGY_LAB.md`·`ARCHITECTURE.md` 등 gitignore 개인문서는 GitHub에 없으므로 색인에서 **하드링크하지 말 것**(텍스트+"로컬전용" 표기만).

---

## 2-b. 도장(stamp) — 요약 문단이 어느 소스 버전을 보고 쓴 것인지

요약 문단 위에 한 줄을 둔다.

```md
<!-- sync: Quant/include/core/OrderRateLimiter.h@3f2a91c Quant/tests/test_order_rate_limiter.cpp@ab12cd3 -->
주문 스레드는 … `OrderRateLimiter`가 맡습니다(D-065).
```

해시는 그 파일의 git blob 해시 앞 7자다. 소스가 바뀌면 `py ../quant-devtools/sync_impact.py --diff`가 그 도장을 `[stale]`로 찍고
Stop 훅(`sync-gate.ps1`)이 턴을 되돌린다. 문단을 고친 뒤 `--restamp 문서.md`로 도장을 갱신한다. 해시를 비워 두면
(`<!-- sync: 경로 -->`) 첫 검사에서 낡음으로 잡히고 `--restamp`가 채운다.

새 요약 문단을 쓸 때는 이 순서로 묻는다 — **생성할 수 있나(gen 블록) → 없으면 도장을 찍나 → 둘 다 아니면 링크만 하나.**
동기화 대상은 늘리지 않는 쪽이 맞다.

## 3. 커밋 전 체크리스트 (스크립트가 못 잡는 의미적 최신성)

`../quant-devtools/check_docs.py`는 **링크 깨짐·색인 등재 여부**만 본다. 아래는 사람이 확인한다:

- [ ] **새 백테스트 실행** → `BACKTEST_LOG`에 원문 prepend + `BACKTESTS` 목록 한 줄 + `research/README` 타임라인 서사 1줄(수치는 링크).
- [ ] **새 스터디 폴더** → `studies/README`의 **올바른 계열**(A 종목레벨 / B 지수레벨위기) 섹션에 요지 1줄로 등재. (등재 누락 자체는 스크립트가 잡음)
- [ ] **새 전략 폴더** → `strategies/README` 표에 SPEC·실증·검증경로 행 추가.
- [ ] **결론이 바뀜**(예: regime 유효성 재판정, 지표 채택) → 루트 `README`·`research/README`의 요약 문구를 **함께** 고쳤나.
- [ ] **파일 이동/리네임** → `python ../quant-devtools/check_docs.py` 통과 확인(깨진 상대링크 0).
- [ ] **헤더 추가/이동·`#include` 변경** → `py ../quant-devtools/gen_code_graph.py` 재생성으로 `docs/CODE_GRAPH.md`·`code_graph.dot`(+ 필요시 `--json`) 최신화.
- [ ] **도장 낡음 `[stale]`** → 문단을 고치고 `--restamp`. 낡은 도장이 남아 있으면 커밋이 막힌다.

---

## 4. 사용

```bash
py ../quant-devtools/sync_impact.py --diff --fix   # 바뀐 파일 → 낡은 gen 블록 치환·도장·힌트 (Stop 훅이 이걸 돌린다)
py ../quant-devtools/sync_impact.py --restamp CLAUDE.md
python ../quant-devtools/check_docs.py             # exit 0 = 통과, 1 = 드리프트(항목별 출력)
```

두 지점에서 돈다. 턴이 끝날 때 `.claude/hooks/sync-gate.ps1`(Stop)이 `--diff --fix`를 돌려 낡은 것이 있으면 되돌리고,
커밋 직전 `.claude/hooks/docs-gate.ps1`(PreToolUse)이 코드·문서 어느 쪽이 스테이징돼도 같은 검사로 막는다.
`../quant-devtools/commit_gate.py`도 커밋 전 `check_docs`를 실행하고, 실패 시 커밋을 멈추고 보고한다.
