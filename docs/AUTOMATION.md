# AUTOMATION.md — 스스로 도는 것 목록

이 저장소에서 **사람이 시작하지 않아도 도는 것**을 한 페이지에 모았다. 무엇이 언제 무엇을 만드는지,
그리고 실패하면 어디를 보는지가 목적이다. 왜 그렇게 설계했는지는 [HARNESS.md](HARNESS.md),
문서 정합 규칙은 [SYNC_MAP.md](SYNC_MAP.md)에 있다.

자동화가 늘어나므로 새로 거는 것은 반드시 여기 표에 한 줄 추가한다. 표에 없는 자동화는 잊혀진 자동화다.

## 1. OS 예약작업 (Windows 작업 스케줄러)

세션이 꺼져 있어도 돈다. 대신 **PC가 켜져 있어야 한다.** Claude 크론(`CronCreate`)은 세션 한정이고 7일 만에
만료되므로 지속 자동화에는 쓰지 않는다.

시각은 계좌 모드로 갈린다(아래 블록, 정본 `scripts/market_close_timetable.ps1`). 무엇을 만드는지는 그 아래 표.

<!-- gen:market-close-timetable -->
| 작업 이름 | 모의 (is_paper=true) | 실계좌 (is_paper=false) | 실행 |
|---|---|---|---|
| `QuantAutoTradeGuard` | 08:45~ 5분마다, -Until 15:35, 7h | 08:45~ 5분마다, -Until 20:05, 11.5h | `powershell -File scripts\market_close_timetable.ps1 -Apply` |
| `Quant Basket Targets` | 08:40 | 08:40 | `py PYQuant\main.py basket` |
| `Quant Market Close AutoDoc` | 16:05 | 20:30 | `py scripts\market_close_autodoc.py` |
| `Quant Maintain Daily` | 16:20 | 20:45 | `py scripts\maintain.py --daily` |
| `Quant Minute Backfill` | 16:40 | 21:00 | `py scripts\market_close_minute_backfill.py` |
| `claude_stock_study` | 20:30 | 21:10 | `/stock-study` |
| `claude_dashboard_sync` | 21:10 | 21:40 | `/dashboard-sync` |
| `Quant Maintain Weekly` | 21:20 | 21:50 | `py scripts\maintain.py --weekly` |
<!-- /gen -->

| 작업 이름 | 산출물 |
|---|---|
| `QuantAutoTradeGuard` | 워치독이 없으면 하루 루프 기동 (§4). 5분마다 도는 시간 폭(`-Hours`)이 매매 끝 시각을 정한다 |
| `Quant Basket Targets` | 장 전 08:40, 바스켓 두 슬리브(가치 기울임·모멘텀)의 목표 비중표를 `Quant/config/basket_targets.json`에 쓴다(전일 종가 기준, 주문 없음). 엔진 `TARGET_BASKET`이 14:40~15:00에 원장과의 차이만 낸다(D-109). 파일이 없거나 `as_of`가 오늘이 아니면 엔진은 아무것도 안 낸다 — 판정은 `check_runtime_health.py` "바스켓 파일 당일" 행 |
| `Quant Market Close AutoDoc` | 매매일지 사실 구간 · 리뷰 탭 항목 · `live.json` 백필 · `dashboard.html` · **결정 원장 파생 문서**(`sync_ledgers.py`) |
| `Quant Maintain Daily` | `장 마감 AutoDoc` 뒤. 먼저 로그 정리(`rotate_logs` — 엔진 로그에서 7일 지난 날의 줄을 `logs/archive/quant_trader_<날짜>.log.gz`로 떼어내고, 감시견 로그는 7일 지나면 gz·90일 지나면 삭제. 엔진이 떠 있으면 엔진 로그는 건너뛴다. 원장 `trades_*.csv`는 손대지 않는다. 옮긴 gz는 잃는 게 아니다 — 날짜를 받는 스크립트(`market_close_autodoc`·`market_close_collect`·`parse_quant_log --full`·`summarize_trading_day`·`extract_swap_what_if`)는 `_logdir.log_sources()`로 그 날짜 gz와 라이브 로그를 이어서 읽으니 지난 날 재생성은 그대로 된다), 이어서 생성물 갱신 — `gen_facts` · `gen_code_graph` · `sync_ledgers` · `gen_automation_hub`(`_private/AUTOMATION_HUB.md`) · `gen_tuning_sheet`(`_private/TUNING_SHEET.md`). 대시보드는 부르지 않는다. 손으로는 `py ../quant-devtools/maintain.py --rotate-logs [--dry-run]` |
| `Quant Minute Backfill` | 아침 스캔 `Quant/config/universe_scan.json`을 `PYQuant/data/pit_universe/<오늘>.json`으로 옮기고 그날 유니버스 전체의 1분봉을 `PYQuant/data/minute/`에 쌓는다(`PYQuant/tools/minute_backfill.py --top-n 0`, 약 260종목×4콜). 매매 끝 15분 뒤(모의 15:45·실계좌 20:15) 전엔 돌지 않는다(반쪽 파일이 그날치를 건너뛰게 만든다). 대시보드 차트가 같은 파일을 읽는다 |
| `claude_stock_study` | `claude -p "/stock-study auto"` → `_private/주식_study/{날짜}_재무/` 1종목 · 저널 · 스터디 사이트 |
| `claude_dashboard_sync` | `claude -p "/dashboard-sync"` → 대시보드·스터디 사이트 HTML 재생성. 아티팩트 재발행은 헤드리스 `claude -p`에 Artifact 도구가 없어 못 한다 — 대화 세션에서 `/dashboard-sync`를 불러 같은 URL로 올린다 |
| `Quant Maintain Weekly` | 금요일, `claude_dashboard_sync` 뒤. 미참조 스크립트 · 에이전트 죽은 경로 · 부산물 용량 · 주석 밀도 · 훅 배선 양방향 검사 → `docs/reports/MAINTENANCE_WEEKLY.md` |

**한곳에서 보기:** 시간표·예약작업의 실제 등록 상태(켜짐·다음 실행·마지막 결과)·훅·대시보드 링크를 `_private/AUTOMATION_HUB.md`
한 파일에 모은다. `py ../quant-devtools/gen_automation_hub.py`가 만들고 `Quant Maintain Daily`·`gen_facts --apply`(Stop 훅 `sync-gate.ps1`)가
다시 만든다. 대시보드를 새로 발행하거나 URL이 바뀌면 `_private/dashboards.json`에 적는다 — 허브와 `_private/LINKS.md`의 표는 거기서 생성된다.

**매매 수치·주기 한 장:** 장중에 무엇이 몇 초마다 도는지(REST 속도·유니버스 리스캔·WS 재연결·주문 간격·감시견 간격)와 config 키·코드 상수 값을
`_private/TUNING_SHEET.md`(상세판)와 `_private/TUNING_CYCLE.md`(요약판 — 매매 사이클 10단계, 어떤 데이터가 어디서 몇 초마다) 두 파일로 본다. `py scripts/gen_tuning_sheet.py`가 실행 중 config(`_private/_auto_trade_day.json`)와
`docs/tuning_sheet.toml`(어느 파일의 어느 상수를 볼지)에서 만든다. 값이 바뀌면 Stop 훅 `sync-gate.ps1`이 매 턴 `--check`로 잡아 다시 쓰고,
감시견 기동과 `Quant Maintain Daily`도 부른다. 수치를 조율할 때는 시트의 "어디서" 칸이 가리키는 config 키나 파일:줄을 고친다.

확인·수정:

```powershell
schtasks /query /tn claude_stock_study /v /fo list | Select-String "다음 실행|마지막 결과"
schtasks /change /tn claude_stock_study /st 20:30    # 예시 — 실제 변경은 scripts/market_close_timetable.ps1 -Apply (시각 정본)
```

> **2026-09-14~09-22 정지했다가 다시 켰다.** 토큰 사용량을 줄이려고 클로드를 부르는 셋 — `claude_stock_study`·`claude_dashboard_sync`
> (둘 다 `Disable-ScheduledTask`)과 2절의 장전 시황 브리핑 루틴 — 을 껐었다. 순수 파이썬 작업은 그때도 그대로 돌았다.
> 09-22에 예약작업 둘을 `Enable-ScheduledTask`로 켜고 되살리기용 `*_resume` 작업을 지웠으며, 사라져 있던 브리핑 루틴을
> 같은 프롬프트로 다시 만들었다(`scripts/premarket_routine.py --render` → `/schedule` → `--mark`). 같은 날 등록이 빠져 있던
> `Quant Basket Targets`(평일 08:40)도 다시 걸었다 — 이 파일이 없으면 엔진 `TARGET_BASKET`이 그날 아무것도 내지 않는다.

> **시간표는 계좌 모드로 갈린다 — 정본은 `scripts/market_close_timetable.ps1`.** 감시견 예약작업이 넘기는 config의 `kis.is_paper`를 읽어
> 모의면 매매 끝 15:30(KIS 모의 서버가 15:30 뒤 주문을 거부, T-18 2026-09-18 실측)·마감 루틴 16:00대, 실계좌면 애프터마켓 20:00(D-097)
> 까지 매매·마감 루틴 20:30 시작이다. 인자 없이 돌리면 예정 vs 실제를 표로 보이고 어긋나면 exit 1, `-Apply`는 `schtasks /change`와
> 감시견 재등록(`-Until`·`-Hours`)까지 한다. `.claude/hooks/cron-gate.ps1`과 위 gen 블록·`_private/AUTOMATION_HUB.md`는 이 스크립트의 `-Lines`를 읽으므로 따로 고칠 것이 없다.
>
> 클로드를 부르는 두 작업은 세션 사용량 한도(17시 리셋) 때문에 모의에서도 20:30 뒤다. `scripts/market_close_minute_backfill.py`의
> 장중 실행 거부(모의 15:45·실계좌 20:15)도 같은 config를 읽는다(`--config`, 기본 `config_dev_paper.json`).
> **실계좌 전환 때 확인할 것:** 실계좌 애프터마켓(NXT·KIS 16:00~20:00) 주문 가능은 검색으로 파악한 것이고 실증이 없다 — 전환 뒤
> 첫날 16:00 넘어 체결통보 1건을 눈으로 확인하고, `scripts/market_close_timetable.ps1 -Apply`를 돌린다(`-Config Quant\config\config.json`).
>
> 그 전 20:00·20:40은 원래 16:00·16:20이었다. 2026-09-07에 두 작업이 모두 세션 사용량 한도(17시 리셋)에 걸려
> 실패했다(`LastTaskResult=1`). 한도 리셋 뒤로 옮겼다. 2026-09-08~09-11에는 예약작업이 부르는 npm 전역 CLI가 구버전(2.1.162)이라
> `400 does not support this model`로 실패하고 작업이 Disabled로 남았다. 편집기 확장의 클로드와 npm CLI는 따로 갱신되므로,
> `LastTaskResult=1`이면 `_private/주식_study/_cron_run.log` 끝을 보고 버전이면 `npm i -g @anthropic-ai/claude-code@latest` 뒤
> `Enable-ScheduledTask`로 되살린다. 순수 파이썬인 `Quant Market Close AutoDoc`은 한도와 무관하다.

## 2. 클라우드 루틴 (Claude)

| 루틴 | 시각 | 내용 |
|---|---|---|
| 장전 시황 브리핑 | 평일 08:30 KST | 간밤 시장과 국면 모델(지표 8개) 기준 스탠스를 노션 페이지로 쓴다(발행은 08:42~08:45쯤). 프롬프트 정본은 `docs/premarket/ROUTINE_PROMPT.md`(국면 모델은 gen 블록, 올린 것과 다르면 `check_docs`가 잡는다). md 정본 `docs/premarket/YYYY-MM-DD.md`로 옮기는 것은 아침 세션(`/auto-trade-day` 1단계). 링크는 `_private/LINKS.md` |

PC가 꺼져 있어도 돈다는 점이 OS 예약작업과 다르다. 대신 이 저장소 파일을 만들지는 않는다.

## 3. 훅 — 결정론적 백스톱 (`.claude/hooks/`)

모델 판단에 맡기지 않고 매번 같은 자리에서 걸리는 장치다. `.claude/`는 gitignore 로컬 전용이라
훅 파일 자체는 커밋되지 않는다.

배선(이벤트·matcher·파일 수)의 정본은 [docs/HARNESS.md](HARNESS.md)의 `gen:hooks` 표(`settings.json`에서 생성)다 — 이 표는 훅이 하는 일만 적고, 훅을 더하면 두 곳 다 손본다.

| 훅 | 시점 | 하는 일 |
|---|---|---|
| `secret-gate.ps1` | PreToolUse (Bash·PowerShell) | app_key·app_secret·계좌번호·개인 이름이 커밋 경로로 새는 것을 차단 |
| `docs-gate.ps1` | PreToolUse (Bash·PowerShell) | 커밋 전 `sync_impact.py --diff`로 낡은 도장·gen 블록·재생성 실패를 막고(D-075), 문서가 있으면 `check_docs.py` 정합 확인. 링크·색인에 더해 **결정 원장 파생 문서 드리프트**(`sync_ledgers.py --check`)도 여기서 막힌다 |
| `lexicon-gate.ps1` | PreToolUse (Write·Edit·Bash·PowerShell) | 파일에 쓰려는 본문을 `check_plain_language.py --stdin`으로 검사해 쓰지 않기로 한 말(정본은 `docs/STYLE_GUIDE.md` 대체어 표)이 들어가는 순간 막는다. 금지어를 설명하는 글은 본문에 `lexicon-ok` 표시로 통과 |
| `sync-gate.ps1` | Stop | 턴이 끝날 때 `sync_impact.py --diff --fix` — 낡은 gen 블록은 치환하고, 낡은 도장·재생성 실패·새 힌트가 있으면 턴을 되돌려 그 자리에서 고치게 한다(D-075) |
| `file-index-gate.ps1` | Stop | 턴이 끝날 때 `file_index.py`로 `docs/FILE_INDEX.md`·`_private/FILE_INDEX.md`를 트리와 맞춘다 — 없어진 파일은 빠지고 날짜 파일·로그는 규칙 표가 설명을 채우며, 설명 없는 새 파일은 `(설명 필요)`로 넣고 턴을 되돌려 그 자리에서 채우게 한다. 커밋 쪽은 `docs-gate.ps1`이 `--check --staged`로 스테이징된 추가·삭제와 색인을 대조한다 |
| `review-reminder.ps1` | Stop | 코드 변경 뒤 리뷰 누락을 상기 |
| `push-summary.ps1` | PostToolUse (Bash·PowerShell) | `git push`가 끝나면 이번 푸시에 담긴 내용을 요약해 보여주게 한다 — 푸시 범위·커밋 제목·파일마다 바뀐 줄로 가는 링크와 한 줄 설명·남은 확인. 푸시가 아닌 명령과 거절된 푸시에는 아무것도 하지 않는다 |
| `market-close-gate.ps1` | SessionStart | 사후검토가 밀린 거래일이 있으면 세션 시작에 알림 |
| `session-board-server.ps1` | SessionStart | `../quant-devtools/session_board_server.py`(:8788)를 숨긴 창으로 띄운다 — 트레이더 대시보드가 없어도 세션이 하나라도 열려 있으면 현황판을 보게. 포트가 이미 쓰이면 서버가 스스로 끝나므로 매번 띄운다 |
| `cron-gate.ps1` | SessionStart | 예약작업이 예정 시각을 넘겨 안 돌았거나 `LastTaskResult≠0`이면 작업 이름·실패 시각·복구 커맨드를 알림 |
| `dashboard-refresh.ps1` | Stop | 매매일지·백테스트가 `dashboard.html`보다 새것이면 리뷰 항목과 대시보드를 다시 만든다. 같은 훅이 `session_board.py --quiet`로 세션 현황판도 턴마다 다시 쓴다. 낡았는지는 수정시각으로 보므로 편집 도구·스크립트·다른 세션 어느 경로로 고쳤든 걸린다 |
| `handoff-due.ps1` | Stop | 인계할 때가 되면 exit 2로 턴을 되돌려 `/handoff`를 밟게 한다. 갈래가 둘이다 — ①작업 경계: 이 턴에 HEAD가 바뀌었고(커밋 직후) 문맥이 100K를 넘었다. ②압축 임박: 경계가 아니어도 문맥이 145K를 넘었다(커밋을 하지 않는 세션은 ①이 오지 않아 자동 압축까지 가므로). ②는 압축 구간마다 한 번만 알린다. 판정은 `session_board.py --due`(세션별 직전 HEAD와 알린 이력을 `_private/session_board.state.json`에 둠) |
| `resume-work.ps1` | SessionStart | 압축 직후(`source=compact`)와 무인일 때 인계 파일의 '남은 것'을 가리켜 하던 일을 잇게 한다. 사람이 없으면 `/clear`를 칠 수 없어 자동 압축이 곧 문맥 초기화이므로, 압축 다음 턴이 무엇을 하던 중이었는지 알 길이 이 파일뿐이다. 세션 이름은 짧은 해시라 며칠 전 세션과 겹치므로 **한 시간 안에 갱신된 자기 파일만** 집는다(옛 인계를 이어받아 엉뚱한 일을 하는 것을 막는다) |
| `output-gate.ps1` | Stop | 채팅으로 나가는 문장도 `check_plain_language.py`로 검사한다 — 파일은 lexicon-gate가 막는데 대화에는 게이트가 없어 금지어가 새던 것을 막는다 |
| `handoff-list.ps1` | SessionStart | 아직 보관되지 않은 `_private/HANDOFF_*.md`를 나이·첫 줄과 함께 보여 새 세션이 이어받을 것을 고르게 한다 |
| `precompact-handoff.ps1` | PreCompact | 압축 직전에 `session_board.py --skeleton`으로 `_private/HANDOFF_<세션이름>.md` 뼈대를 만들고 기계적 사실(브랜치·HEAD·미커밋·현황판 줄·문맥·턴·최근 요청)만 채운다. 파일이 있으면 그 절만 갱신 |

**무인 표시 `_private/UNATTENDED.flag`** — 사람이 자는 동안 돌릴 때 만든다. 첫 줄에 만료 시각(ISO, 예 `2026-09-18T09:00:00`)을 적고, 그 시각이 지나면 없는 것으로 친다(빈 파일이면 12시간). 이 표시가 있으면 `handoff-due.ps1`은 '`/clear`를 권하라'가 아니라 '인계 파일을 갱신하고 그대로 이어서 진행하라'로 바뀌고, `resume-work.ps1`은 묻지 말고 진행하라고 알린다. 사람이 없는데 승인을 기다리면 밤새 아무것도 안 되기 때문이다. 판정은 `session_board.py` 의 `unattended()` 한 곳에 있다.

## 4. 하루 무인 루프 — `/auto-trade-day`

장 시작부터 마감 뒤 문서까지 하루치를 한 번에 돈다. 기계적인 부분과 판단이 필요한 부분을 갈라 놓았다.

| 층 | 담당 | 하는 일 |
|---|---|---|
| 감시자 | `scripts/auto_trade_guard.ps1` | 평일 5분 주기 예약작업. 장중인데 워치독이 없으면 기동한다. 남은 트레이더가 남아 있으면 먼저 내린다 |
| 워치독 | `scripts/auto_trade_day.ps1` | 사전 점검(중복 프로세스·계좌 모드), 기동 전 `quant_trader` 재빌드(증분, 실패면 `build_failed`로 중단, `-NoBuild`로 생략), 보조 프로세스·유니버스·대시보드·알림·체결 기록기(`quant-recorder`)·엔진 자원 표본기(`quant-procwatch`, 리눅스 트레이더면 `--wsl-distro Ubuntu-24.04`)·원장 저널 적재기(`quant-ledger`, `PYQuant/tools/ledger_recorder.py --dir <ledger_journal_dir>`, D-113) 기동, KIS 토큰 캐시를 `KIS_TOKEN_CACHE_DIR`로 트레이더와 한 파일로 맞춤, 트레이더를 마감까지 감시·재기동, 마감 뒤 `market_close_autodoc.py` 실행 |
| 워치독(리눅스) | `scripts/auto_trade_day.sh` | 트레이더를 WSL2에서 띄우는 날의 하루 루프 — 사전 점검(WSL·Windows 양쪽 중복 프로세스, 모의계좌), `ninja` 증분 재빌드, 미체결 복원, 마감까지 감시·재기동, 크래시 루프 판정. 부속 창·마감 정리는 Windows 워치독 `-NoTrader`가 맡는다. 상태 `_private/_auto_trade_linux.json`. 절차 `docs/RUNBOOK.md` 1.1절 |
| 감독 | `.claude/commands/auto-trade-day.md` | 국면 판단, 증분 로그 감시, **무발주 감시**, 결함을 코드/상황으로 분류, 코드면 수정·재빌드, **이슈 대장 누적**, 마감 뒤 해석 문서 |

워치독 상태는 `_private/_auto_trade_day.json` 한 파일에 적힌다(`phase`·`sessions`·`history`). 로그 전체를
훑는 대신 이 파일을 읽는다. 최근 30분 안에 세션 종료가 3번이면 `phase=crash_loop`로 스스로 멈춘다(종료 시각은
`exits`에 남는다) — 재기동으로 풀리지 않는 배선 문제를 계좌에 대고 반복하지 않기 위해서다. 예전 기준 "30초 미만 종료
3연속"은 2~3분 살다 죽는 루프를 못 잡았다.

재기동하지 않는 날은 엔진이 표지 파일로 알린다(D-098). 마지막 매매 창이 닫히고 `risk.session_end_grace_sec`(기본 120초)
뒤 주문 큐가 비면 엔진이 `_private/state/session_done_<날짜>`를 쓰고 스스로 내려가고, 운영단말·ZMQ `KILL`은
`_private/state/kill_today_<날짜>`를 쓴다. 워치독은 재기동 직전에 두 파일을 보고 있으면 `phase=closed`로 끝낸다.
`-Until`은 이 길이 막혔을 때의 백업이다. `taskkill`은 파일을 안 쓰므로 장중 exe 교체는 그대로 5초 뒤 재기동된다.
장중 exe 교체 자체는 D-101 결정 1로 A등급 결함(체결 누락·이중 발주·원장 불일치·주문 불능)일 때만이다 —
`C:/build_tmp/relink.cmd`가 먼저 `py scripts/deploy_guard.py`를 부르고, 매매 창 안이면 exit 1로 링크를 막는다(A등급은
`--hotfix-a "사유"`, 사유는 `_private/deploy_guard.log`에 남는다). 리팩터·이름·문서·성능 반영은 장 마감 뒤 감시견의 기동 전 빌드가 한다.
창 끝은 **지금 도는 트레이더·감시견이 실제로 연 config**의 `is_paper`로 잡는다 — 모의면 15:30, 실계좌면 20:00이고,
둘 다 안 돌고 있으면 바꿔도 깨질 매매가 없어 그냥 통과한다(`--config <경로>`로 특정 config를 지정할 수도 있다).
전에는 경로를 `Quant/config/config.json`으로 고정해 읽어, 모의로 돌던 날에도 실계좌 20:00을 적용해 마감 뒤 배포를 막았다.
KILL을 풀고 다시 매매하려면 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/kill_release.ps1` — 표지
파일을 지우고 워치독 상태파일을 옆으로 치워 감시자가 다음 주기(5분 안)에 워치독을 다시 띄운다. 엔진을 손으로 띄우지 않는다.

### 프로세스 수명 — 잡(Job Object)과 감시자

Windows에는 리눅스의 프로세스 그룹 cascade가 없다. 부모가 죽어도 자식은 그대로 남아서, 워치독이
사라지면 부속 창은 유휴 쉘로 떠 있고 트레이더는 아무도 감시하지 않는 채 계속 발주했다. 다음 기동은
그 트레이더 때문에 `duplicate_process`로 막혔다.

워치독은 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 잡을 만들어 보조 프로세스·유니버스·대시보드·알림 창과
트레이더를 모두 여기에 넣는다. 잡 핸들을 쥔 워치독이 사라지는 순간 — 정상 종료든 강제 종료든 —
커널이 나머지를 같이 내린다. 되살리는 쪽은 감시자다. 재기동하면 잔고 재시드가 실제 보유수량을 다시
읽고 청산 관리가 포지션을 다시 잡으므로, 끊긴 자리를 사람이 이을 필요가 없다.

감시자는 사고와 의도적 정지를 구분한다. 오늘 날짜 상태파일의 `phase`가 `crash_loop`·`aborted`·
`done`·`closed`·`past_deadline`이면 손대지 않는다. 크래시 루프를 5분마다 되살리면 계좌만 반복 호출한다.

반대 방향, 즉 부속 창 안의 파이썬만 죽는 경우도 잡는다. 창은 `-NoExit`로 띄우므로 안의 스크립트가
끝나도 빈 창은 남고, 창 목록만 보면 살아 있는 것처럼 보인다. 워치독은 트레이더를 기다리는 동안
60초마다 `python`/`py` 프로세스의 명령줄을 훑어 등록된 스크립트 이름(`macro_regime_feed.py`,
`dashboard_server.py`, `notify_trades.py`, `live_prices_feed.py`, `main.py record`)이 있는지 확인하고, 없으면 남은 창을 내리고 같은 명령으로
다시 띄운다. 기동 직후 45초는 아직 파이썬이 뜨는 중일 수 있어 건너뛴다. 알림 보조 프로세스가 조용히
사라진 것을 사람이 화면을 봐야 아는 상태를 없애기 위한 것이다.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Install     # 예약작업 등록
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -DryRun      # 판단만 보기
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Uninstall
```

### 남은 프로세스 정리 — `scripts/quant_procs.ps1`

잡은 워치독이 정상적으로 사라질 때만 동작한다. 강제 종료, 리부트, 워치독 없이 손으로 띄운 창은
그 경로를 타지 않아서 보조 프로세스나 대시보드가 두 벌씩 남는다. 남은 쪽도 계속 폴링하므로 REST 초당
한도를 같이 갉아먹고, 안의 파이썬만 죽고 `-NoExit` 창만 남으면 화면과 메모리를 차지한 채 아무 일도
하지 않는다. `Get-Process`로는 어느 powershell·python이 매매용인지 구분되지 않는다.

이 스크립트는 명령줄로 역할을 붙이고, 부모-자식을 한 인스턴스로 묶어(`py → python → python`은
하나다) 역할별 개수만 본다. 판정은 넷이다 — `정상` / `중복`(가장 최근에 뜬 것을 남긴다) /
`빈 창`(역할 프로세스가 죽은 `quant-*` 창) / `없음`. <!-- lexicon-ok: 금지어를 예시로 인용하는 줄 -->

```powershell lexicon-ok
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1          # 현황만
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1 -Reap    # 중복·빈 창 정리
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1 -KillAll # 전부 내리고 하루 종료
```

`-Reap`은 중복과 빈 창만 본다. 정상으로 떠 있는 것은 남기므로 하루를 끝낼 때 쓰는 스위치가 아니다.
그 자리는 `-KillAll`이다. 판정과 무관하게 역할 프로세스와 `quant-*` 창을 전부 내리고 — 이때는 어느
트레이더가 진짜인지 가릴 필요가 없어 트레이더도 같이 내린다 — 오늘 상태파일의 `phase`를 `closed`로
적는다. 이 표시가 핵심이다. 감시자는 평일 08:45부터 5분마다 도므로, 표시 없이 프로세스만 죽이면
장중에는 몇 분 안에 다시 떠 있고 재부팅으로도 풀리지 않는다.

워치독이 기동 직전과 종료 직후에 `-Reap -Quiet`으로 이것을 부른다. 트레이더 중복은 정리하지 않고
보고만 한다 — 두 프로세스가 같은 계좌에 발주하면 원장이 깨지는데 어느 쪽이 진짜인지 스크립트가
알 수 없어서, 기존 `duplicate_process` 게이트가 사람 판단으로 처리한다.

### 감독 층에만 있는 두 가지

워치독은 프로세스가 살아 있는지까지만 안다. 아래 둘은 자동화 장치가 아니라 **감독 층의 의무**다.
스크립트로 옮기지 않는 이유는 판단이 필요하기 때문이다 — 무발주가 정상인지 결함인지는 조건식과 로그를
같이 읽어야 갈린다.

| 의무 | 언제 | 하는 일 |
|---|---|---|
| 무발주 감시 | 감시 주기마다 | 장중인데 그 창에 발주가 0이면 신호·게이트 거부·KIS 거부·기준선·유니버스를 순서대로 짚어 "정상 무발주"인지 "결함 무발주"인지 고른다. 같은 종목이 같은 사유로 3회 이상 거부되면 무발주와 같게 취급한다 |
| 이슈 대장 | 기동~마감 상시 | 발견한 이슈를 `_private/_intraday_issues/YYYY-MM-DD.md`에 누적한다. 장중에 고친 건은 같은 줄에 바꾼 파일과 재기동 시각까지 적어 `해결`로, 나머지는 `미해결`로 남긴다 |
| 장중 질문 회수 | 마감 | 사용자가 장중에 던진 질문·지시 중 답만 하고 넘어간 것, 부분만 반영한 것, "장 끝나고"로 미룬 것을 대장에 합류시킨다 |
| 임시 해결 부작용 감사 | 마감 | `해결` 줄마다 참조 변수의 다른 호출부·우회 여부·재기동 이후 로그 증거를 확인한다. 우회로 넘긴 것은 `미해결`로 되돌린다 |
| 마감 회의 | 마감 | 위 셋을 한 표로 합쳐 사용자와 우선순위를 정한다. 방향을 바꾼 결정은 `docs/DECISIONS.md`에 D-NNN |

마감 작업(6절)은 이 대장을 **입력으로 먼저 읽는다.** `해결`은 다시 고치지 않고 사후검토의 장중 조치 절로
옮기고, `미해결`만 개선 목록이 된다. 이 구분이 없으면 마감 뒤에 장중에 이미 고친 것을 다시 고친다.
대장은 `_private/`라 커밋되지 않으므로, 남길 내용은 `docs/market_close/YYYY-MM-DD.md`와 매매일지로 옮겨 적는다.

> 무발주 감시를 의무로 올린 계기는 2026-09-08 오전이다. 분할 매수 기준선 미형성과 명목 한도 전량 거부가
> 겹쳐 한 시간 동안 한 주도 나가지 않았는데, 크래시가 없어 로그도 워치독도 조용했다.

## 5. 장중 상시 루프

| 루프 | 주체 | 주기 | 하는 일 |
|---|---|---|---|
| 매크로 국면 파일 전달 | `PYQuant/tools/macro_regime_feed.py` | 상시 | `regime.json` 갱신 → 엔진이 매수 비율 `entry_scale`·`entry_halt`(신규 매수만 차단)·`force_liquidate`를 옮기고 라벨로 전략 집합을 고른다(D-083·D-084) |
| 제어 스레드 | `Engine::control_thread_fn` | 상시 | 잔고 대조·손익 갱신 감시, 끊기면 보수정지 |
| 증분 로그 감시 | `scripts/parse_quant_log.py --watch` | 15~20분 | 유의미한 창일 때만 출력. 조용하면 토큰 0 |
| 실행 건전성 점검 | `scripts/check_runtime_health.py` | 세션 종료마다(감시견)·마감 뒤 하루 전체 | 유령주문·조기 사망·재기동 투매·회전·초당한도·WS 폴백·주문 접수 지연·잔고 조회 지연을 PASS/WARN/FAIL로 판정. 같은 표를 `market_close_autodoc.py`가 매매일지 4절에 싣는다 — 고친 뒤 "다음 날 확인할 것"은 사람이 아니라 여기 행으로 만든다 |
| 전 종목 시세 파일 전달 | `scripts/live_prices_feed.py` | 20초(`PRICES_PERIOD_SEC`, D-028) | 네이버 벌크 시세를 100종목씩 묶어 받아 `Quant/config/prices_live.json`으로 떨군다. KIS REST 초당 한도와 무관해서 2,700종목을 20초 주기로 훑을 수 있다. `UniverseScanner`가 이 파일을 읽는다 |
| 매매 알림 | `scripts/notify_trades.py` | 체결 즉시 / 요약 30분 | 당일 체결 원장 CSV를 증분으로 읽어 체결을 바로 보내고, 평단·손익 표는 KIS 잔고조회로 주기 발송 |

### 매매 알림 보조 프로세스

포지션 요약은 대시보드 계좌 현황과 같은 항목을 싣는다 — 총평가·총매수금액(원가)·가용현금(D+2)·
예수금·총노출/한도·평가손익·보유 종목수/한도·오늘 익절·손절·실현손익·보유 종목표·국면. 실현손익은
`dashboard_server.read_trades_today()`를 그대로 불러 쓴다. 같은 수치를 두 곳에서 따로 세면 갈린다.

엔진을 건드리지 않는다. 원장 CSV(`logs/trades_YYYYMMDD.csv`)를 따라 읽는 별도 프로세스라
알림 쪽이 죽어도 매매는 그대로 돈다. `auto_trade_day.ps1`이 `quant-notify` 창으로 같이 띄우고,
`-NoNotify`로 끈다.

수신처는 `_private/notify.json`(gitignore)에 적는다. Discord 웹훅은 URL 하나로 끝나고 만료가 없다.
카카오톡 '나에게 보내기'는 access token이 6시간이라 refresh token 갱신을 보조 프로세스가 대신한다.

```json
{
  "discord_webhook": "https://discord.com/api/webhooks/...",
  "discord_webhook_fill": "…",
  "discord_webhook_position": "…",
  "kakao": {"rest_api_key": "...", "refresh_token": "..."}
}
```

둘 다 적으면 둘 다 보낸다. 환경변수 `DISCORD_WEBHOOK_URL`·`KAKAO_REST_API_KEY`+`KAKAO_REFRESH_TOKEN`도 같은 자리에 쓰인다.

체결과 포지션 요약을 다른 채널에서 보려면 웹훅을 둘로 준다. `discord_webhook_fill`은 체결만,
`discord_webhook_position`은 요약만 받고, 빠진 쪽은 `discord_webhook`으로 떨어진다. 기동·테스트
같은 상태 알림은 양쪽에 다 간다 — 한 채널이 죽었을 때 바로 보이게 하려는 것이다. 환경변수는
`DISCORD_WEBHOOK_FILL`·`DISCORD_WEBHOOK_POSITION`.

```bash
py scripts/notify_trades.py --config Quant/config/config_dev_paper.json --test        # 수신처 확인
py scripts/notify_trades.py --config Quant/config/config_dev_paper.json --interval 1800
py scripts/notify_trades.py --config … --events FILL,REJECTED --echo                  # 거절도 함께
```

## 6. 마감 뒤 문서 파이프라인

사실은 스크립트가, 해석은 사람(또는 클로드)이 쓴다. 이 경계를 지키면 자동 실행이 손으로 쓴 문장을 덮지 않는다.

```
scripts/market_close_autodoc.py
  ├─ 매매일지 사실 구간   strategies/<전략>/live/YYYY-MM-DD.md   (수기 일지가 있으면 보존)
  ├─ PYQuant/dashboard/backfill_live.py      → research/dashboard/live.json
  ├─ scripts/build_review_entry.py --date …  → research/dashboard/reviews.json  (사실 키만)
  └─ PYQuant/dashboard/build_dashboard.py    → research/dashboard/dashboard.html
```

예약 실행(`Quant Market Close AutoDoc`, 시각은 1절 표)만이 아니라 장중에도 돈다. 매매일지·백테스트·장전 브리핑(`docs/premarket/`)을 쓰고 나면 Stop 훅이
대시보드와 수정시각을 비교해 낡은 만큼만 다시 만든다(브리핑은 생성기만 다시 돈다). 손으로 돌릴 때는 `py scripts/refresh_dashboard.py --if-stale`.

`scripts/`에는 매매 운영·리서치 산출 스크립트만 남긴다. 문서·규약 게이트, 생성기, 클로드 작업 환경 도구는 저장소 밖 형제 폴더
`../quant-devtools/`(자체 git, D-107)에 있다 — 아래 표에서 `../quant-devtools/…`로 적힌 행이 그것이다. 그 도구들은 저장소 루트를
`__file__`이 아니라 현재 폴더가 속한 git 작업 트리(환경변수 `QUANT_REPO`가 있으면 그것)로 잡으니, 훅·게이트·예약작업은 저장소 루트를
현재 폴더로 놓고 부른다.

| 스크립트 | 역할 |
|---|---|
| `scripts/market_close_collect.py` | 원장·로그에서 사실만 뽑는다(세션·거부 히스토그램·라운드트립·주문 공백) |
| `scripts/build_review_entry.py` | 위 사실을 `quant.review/v1` 항목으로 만들어 리뷰 탭에 넣는다. 기존 항목의 해석 키(`axes`·`improvements`·`gate`)는 건드리지 않고, 항목에 `"locked": [...]`가 있으면 그 키도 제외한다. `incidents`는 목록을 새로 만들되 제목이 같은 항목의 `impact_html`(사람이 쓴 영향)은 옮겨 온다 |
| `scripts/build_study_site.py` | `_private/주식_study/` 전체를 날짜별로 묶어 스터디 사이트 재생성 |
| `scripts/exit_ev.py` · `scripts/exit_ev_dashboard.py` | 모의 원장 청산 체결을 사유별로 묶어 승률·기대값·CI 표(study 17)와 그 근거를 셀마다 펼쳐 보는 화면(`research/studies/17_exit_ev/exit_ev_dashboard.html`)을 만든다. `refresh_dashboard.py`가 매매일 마감 뒤 부르고(마지막 날 = 원장 최신 파일), 발행본은 `/dashboard-sync` |
| `scripts/refresh_dashboard.py` | 위 재생성 순서(라이브 백필·리뷰 항목·생성기)를 소유한다. `--if-stale`은 원천 파일이 산출물보다 새것일 때만 돈다. `market_close_autodoc.py`와 Stop 훅이 모두 이 스크립트를 부르므로 절차가 한쪽만 고쳐져 갈라지지 않는다. 실행 기록은 `logs/refresh_dashboard.log` |
| `../quant-devtools/token_audit.py` | 세션 기록(`~/.claude/projects/<repo>/*.jsonl`)에서 토큰 사용을 절차(탐색·편집·git·빌드·위임·훅 되돌림)·도구 결과·하네스 주입(CLAUDE.md 재주입·압축 요약)·훅 소요별로 집계해 표로 낸다. `--md docs/reports/TOKEN_AUDIT.md`로 보고서 |
| `scripts/trade_costs.py` | 체결 원장 `logs/trades_YYYYMMDD.csv`의 날짜별·종목별 매매 비용(수수료·거래세, 요율은 인자)과 실현손익(`realized_pnl` 열)을 `logs/trade_costs.json`에 누적하고 표로 낸다. 거래 빈도와 손익의 경계를 보는 용도. `py scripts/trade_costs.py --days 7` |
| `../quant-devtools/check_docs.py` | 깨진 내부 링크·색인 누락 검사. exit 0이어야 문서 커밋 |
| `../quant-devtools/sync_impact.py` | 바뀐 파일을 `docs/sync_map.toml`의 규칙과 대조해 봐야 할 문서를 찍고, 문서 안 `<!-- sync: 경로@해시 -->` 도장으로 낡은 문단을 집어낸다. `--fix`는 gen 블록 치환, `--restamp`는 도장 갱신, `--render`는 `docs/SYNC_MAP.md` §2 표 생성. Stop 훅과 커밋 훅이 부른다(D-075) |
| `../quant-devtools/commit_gate.py` | 커밋 직전 게이트 — 스테이징 diff의 보안(시크릿·개인정보·비공개 단어)·문체·문서 드리프트·코드 규약·재현성과 커밋 메시지 형식(`--msg-file`)을 한 번에 본다. 0 통과·1 차단·3 사람 판단 남음. 돌 때마다 규칙별 견본으로 자기 시험을 하고, 통과하면 `.claude/commit-gate.state`에 스테이징 트리 해시를 적어 `secret-gate.ps1` 훅이 게이트를 건너뛴 커밋을 막게 한다. 비공개 단어 목록은 `_private/gate_words.txt`에서 읽는다(없으면 차단) |
| `../quant-devtools/check_code_conventions.py` | 스테이징된 코드 변경의 규약 검사 — 중괄호(`brace_style.py --check`), 없는 D-NNN 참조, 규약에 없는 주석 태그, C스타일 캐스트(값은 `static_cast`·포인터는 `reinterpret_cast`, `(void)x;`는 예외), json 노드 깊은 복사·값 range-for, 약어 이름(`qty`·`it`·한 글자 — 리네임 표 `rename_maps/01_fields.json`·`rename_frags.py`로 판정, `.py`는 `ast`로 그 파일이 정의하는 이름만 보고 `np`·`df` 같은 관례는 예외), 매직 숫자(식 안의 세 자리 이상 맨 숫자 — `constexpr`·멤버 기본값·json 기본값·chrono 리터럴·tests는 예외), 주석·코드 줄 성격 집계. `--comment-only`는 코드 줄이 섞였는지 본다. 밀도는 보지 않는다(정본 `docs/guides/MAINTENANCE_AUTOMATION.md` 4절이 밀도를 게이트로 걸지 말라고 정해 두었다) |
| `scripts/tsan_round.sh` | ThreadSanitizer 회차 — WSL2에서 Debug+TSAN으로 짓고 벤치를 뺀 ctest를 한 판 돌려 스레드 경합을 찾는다. 시각을 정해 두지 않았고, 스레드가 여럿 붙는 코드(`src/core`·`src/risk`·`src/ipc`·`src/feed`)를 고친 워크트리가 머지 직전에 부른다(`docs/guides/MULTI_SESSION.md` 머지 절차). 결과 한 줄은 `_private/state/tsan_last.json`, 원문은 `logs/tsan/`, 판정은 `check_runtime_health.py`의 "TSAN 회차" 행 |
| `../quant-devtools/maintain.py` | 위 검사기를 한 번에 돌리는 진입점. `--check`는 `check_docs` → `check_code_refs --diff-only` → `gen_facts --check` → `gen_code_graph --check` → `sync_impact --stamps` 순으로 묶어 표로 요약한다. `--weekly`는 파일별 주석 밀도와 태그 없는 긴 블록을 `docs/reports/MAINTENANCE_WEEKLY.md`에 남긴다 |
| `../quant-devtools/check_code_refs.py` | 문서가 가리키는 코드 참조가 실재하는지 검사한다 — 경로, `파일::심볼`, 줄번호 참조. 줄번호 참조는 코드가 움직이면 조용히 어긋나므로 새로 추가된 줄에서 막고 `파일::심볼`로 쓰게 한다 |
| `../quant-devtools/gen_facts.py` | 저장소를 세어 `docs/facts.json`을 만들고, 문서의 `<!-- gen:이름 -->` 블록을 그 값으로 채운다. 하네스 개수·훅 배선처럼 손으로 세면 반드시 어긋나는 숫자가 대상이다. KIS 토큰 캐시 파일명은 실 키 앞부분이 들어가므로 가려서 쓴다 |
| `../quant-devtools/gen_code_graph.py` | 헤더 포함 관계로 모듈 그래프를 만들어 `docs/CODE_GRAPH.md`·`code_graph.dot`·`code_graph.json`을 생성한다. `--impact <파일>`은 그 파일을 고쳤을 때 재검증 대상을 파일을 열지 않고 뽑는다 |
| `../quant-devtools/gen_code_flow.py` | `docs/code_flow.toml`(읽는 순서·심볼·볼 것)에서 `docs/CODE_FLOW.md`를 만든다. 줄 번호·시그니처는 소스에서 찾아 채우므로 코드가 옮겨가도 링크가 따라가고, 심볼이 사라지면 `--check`가 exit 1로 막아 명세를 고치게 한다. sync-gate가 `fix_cmd`로 턴 끝마다 재생성한다(D-078) |
| `scripts/gen_tuning_sheet.py` | `docs/tuning_sheet.toml`(config 묶음·단위, 코드 수치(줄번호 참조))과 실행 중 config에서 `_private/TUNING_SHEET.md`(상세판)와 `_private/TUNING_CYCLE.md`(요약판, `[[cycle]]` 문장의 `{이름}` 을 실제 값으로 채움)를 만든다. 값은 소스에서 정규식으로 읽으므로 코드를 고치면 시트가 따라오고, 정규식이 안 잡히면 `--check`가 exit 1로 명세를 고치게 한다. config 는 gitignore 라 git diff 로 못 잡아 sync-gate 가 매 턴 `--check` 를 돈다 |
| `../quant-devtools/claude_backup.ps1` | 메인 트리 `.claude/`를 저장소 밖(`%USERPROFILE%\.claude\backups\Quant\latest`)에 거울로 뜬다. Stop 훅이 매 턴, `maintain --daily`가 하루 한 번, `wt_remove.ps1`이 지우기 전에 부른다. `daily\날짜` 스냅샷은 14일 보관 |
| `../quant-devtools/claude_restore.ps1` | 그 거울에서 `.claude/`를 되돌린다(기본은 빠진 것만, `-Mirror`는 완전 일치, `-From`으로 날짜 스냅샷). 세션이 스스로 부를 수 있게 allow에 열려 있다 |
| `../quant-devtools/wt_add.ps1` | 세션용 워크트리를 만든다 — `git worktree add` + 메인 트리 `.claude`로 정션 + gitignore 로컬 파일(`_private/gate_words.txt`·`research/STRATEGY_LAB.md`·`Quant/config/config_dev_paper.json`·`config_mm_paper.json`·`regime.json`·`universe_scan.json`) 복사. 설치를 손으로 치면 `.claude`를 지우는 줄이 섞여 메인 `.claude/`가 날아간다(09-18·09-19). 제거는 `wt_remove.ps1` |
| `../quant-devtools/gen_runbook.py` | 운영 명령 정본 `docs/RUNBOOK.md`를 복사 버튼 달린 `docs/RUNBOOK.html`(gitignore, 절대경로 치환)로 렌더한다. `gen_facts --apply`가 허브와 같이 부르고, `--check`(check_docs)는 코드 블록의 스크립트 경로가 실재하는지 본다. 인용 스크립트의 인자가 바뀌면 절 머리 도장이 낡음으로 잡힌다 |
| `scripts/premarket_routine.py` | 장전 시황 브리핑 루틴 프롬프트 정본 `docs/premarket/ROUTINE_PROMPT.md`의 본문 출력(`--render`)·올린 해시 기록(`--mark`)·정본과 비교(`--check`, check_docs가 부른다). 루틴 갱신 자체는 세션(`/schedule`)이 한다 |
| `../quant-devtools/brace_style.py` | 중괄호와 블록 앞뒤 빈 줄을 기계적으로 맞춘다(`.clang-format`의 Allman·`InsertBraces`와 같은 규칙). 손으로 맞추지 않는다 |
| `../quant-devtools/check_plain_language.py` | 쓰지 않기로 한 말을 검출·치환한다(`--fix`는 뒤 조사까지 맞춘다). 정본은 `docs/STYLE_GUIDE.md`, 게이트는 `lexicon-gate.ps1`과 `@committer` |
| `../quant-devtools/session_board.py` | 살아 있는 세션(`~/.claude/sessions/*.json`)마다 기록 jsonl의 늘어난 꼬리만 읽어 문맥 K/%·턴(모델 호출 수)·압축 횟수·마지막 사용자 요청을 세고, 현황판 `_private/SESSION_CLAIMS.md` 줄과 인계 파일 유무를 붙여 `_private/session_board.json`·`.html`(30초 자동 새로고침)로 쓴다. 파일은 Stop 훅이 턴마다 다시 쓰고, 서버(`:8788`, 트레이더가 돌 때는 대시보드 `:8787/sessions`도)는 파일이 30초보다 낡았으면 요청 때 한 번 더 만든다(어느 세션도 턴을 안 끝내면 훅만으로는 멈춰 있어서). 문맥 50%↑ 노랑, 80%↑ 빨강, 100K↑면 인계 시점 표시(145K↑는 경계를 안 기다리고 알린다). `--facts`·`--skeleton`·`--due`는 인계 훅이 쓴다 |
| `../quant-devtools/session_board_server.py` | 세션 현황판만 내주는 작은 HTTP 서버(`http://127.0.0.1:8788/sessions`, `/sessions.json`). SessionStart 훅이 세션마다 띄우고 포트가 쓰이면 바로 끝난다. 이 저장소의 세션이 2분 연속 없으면 스스로 내려간다 — 프로젝트를 닫으면 같이 사라진다 |
| `../quant-devtools/session_triage.py` | 코드 세션 여럿이 하루 동안 남긴 상태(미푸시·worktree·브랜치·현황판 `_private/SESSION_CLAIMS.md`·인계 파일·배포 exe 뒤에 쌓인 C++ 커밋)를 한 보고서로 모은다. 되돌릴 수 있는 정리만 옵션으로 한다 — `--prune-branches`(main에 들어간 브랜치 `-d`)·`--archive-handoffs`·`--unowned-patch`. worktree 제거·푸시·exe 교체는 하지 않는다. 절차는 `/triage`(로컬 커맨드), 규칙은 CLAUDE.md 다중 세션 절 |
| `../quant-devtools/unattended_run.ps1` | 사람이 자는 동안 지시서 하나를 여러 사이클에 걸쳐 잇는다. 한 사이클은 `claude -p --permission-mode bypassPermissions` 한 번이고, 끝나면 프로세스가 죽으므로 다음 사이클은 문맥 0에서 시작한다 — 대화 세션에서 불가능한 `/clear`를 이렇게 대신한다. 사이클 사이를 잇는 것은 `_private/HANDOFF_<이름>.md` 하나뿐이라, 매 사이클 지시에 '남은 것'을 파일 경로와 다음 명령까지 적으라는 규칙을 붙인다. `-Name`마다 인계·완료표시·로그가 따로라 여러 개를 동시에 돌려도 섞이지 않는다(단, 같은 파일을 고치는 일을 겹쳐 주지 않는다). 모델이 `_private/DONE_<이름>.flag`를 만들면 남은 사이클을 버리고 끝낸다. 한글 지시는 반드시 `-PromptFile`(UTF-8 BOM)로 준다 — `-Prompt`는 PS 5.1 파이프 인코딩 탓에 물음표로 깨진 적이 있다 |

해석을 채우는 커맨드는 `/market-close-review`(사후검토 문서) → `/trade-log`(매매일지 해석) → `/dashboard-sync`(아티팩트 재발행)
→ `/stock-study`(종목 학습) → `/daily`(DAILY_LOG prepend) 순이다.

## 7. 실패했을 때 어디를 보나

| 증상 | 먼저 볼 것 |
|---|---|
| 대시보드가 어제에 머물러 있다 | `Quant Market Close AutoDoc`의 마지막 결과 → `logs/market_close_autodoc.log` |
| 아티팩트만 낡았다 | 예약작업은 HTML만 다시 만들고 아티팩트는 못 올린다(헤드리스에 Artifact 도구 없음, 09-12 rc=267009). 두 클로드 작업의 액션은 2026-09-19부터 `scripts/run_claude_task.ps1` 래퍼다 — 전에는 stderr 경고 한 줄이 rc=1(거짓 실패)을 만들었고, `claude_stock_study`는 배터리 조건(0x800710E0)으로 안 떴다(조건 해제·한도 PT1H). 대화 세션에서 `/dashboard-sync`로 재발행한다. 다른 실패면 세션 시작 `[CRON]` 알림(`cron-gate.ps1`)과 `_private/_cron_dashboard.log` |
| 스터디가 리포트만 있고 저널이 없다 | 중도 중단. `/stock-study`를 다시 부르면 새 종목을 고르지 않고 빠진 산출물만 채운다 |
| 예약작업이 `LastTaskResult=1` | 세션 사용량 한도를 먼저 의심한다(`_private/_cron_dashboard.log`) |
| 트레이더가 계속 죽는다 | `_private/_auto_trade_day.json`의 `history`에서 종료 코드·지속 시간 |
| 엔진이 내려간 뒤 워치독이 다시 띄우지 않는다 | `_private/state/session_done_<날짜>`·`kill_today_<날짜>`가 있는지(D-098). 마감 자기 종료는 정상, KILL이면 원인을 없앤 뒤 `scripts/kill_release.ps1` |
| 로그가 `[Engine] 종료 요청 — …` 줄 없이 끊겼다 | 요청된 종료가 아니라 죽은 것이다 — 실행파일 옆 `logs/crash_<pid>.dmp`(있으면 예외)·워치독 `history`의 exit 코드 |

## 8. 아직 자동화하지 않은 것

- 예약작업 실패의 **자동 복구** — `cron-gate.ps1`이 알리기까지다. 다시 돌리는 것은 사람이 커맨드를 부른다.
- 유니버스 재스캔은 두 겹이다 — 엔진 안 `Engine::maybe_rescan_universe`(`rescan_interval_sec`, D-077·D-087)와 감시견의 스캔 파일 갱신(10:00 전 3분, 뒤 10분). 둘을 하나로 합치는 것은 미정.
- 커밋·푸시 — 커밋명·파일 목록 승인 게이트를 일부러 유지한다. 검사는 `../quant-devtools/commit_gate.py`가 하고 `git commit`은 승인 뒤 메인 세션이 친다.
