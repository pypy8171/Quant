# AUTOMATION.md — 스스로 도는 것 목록

이 저장소에서 **사람이 시작하지 않아도 도는 것**을 한 페이지에 모았다. 무엇이 언제 무엇을 만드는지,
그리고 실패하면 어디를 보는지가 목적이다. 왜 그렇게 설계했는지는 [HARNESS.md](HARNESS.md),
문서 정합 규칙은 [SYNC_MAP.md](SYNC_MAP.md)에 있다.

자동화가 늘어나므로 새로 거는 것은 반드시 여기 표에 한 줄 추가한다. 표에 없는 자동화는 잊혀진 자동화다.

## 1. OS 예약작업 (Windows 작업 스케줄러)

세션이 꺼져 있어도 돈다. 대신 **PC가 켜져 있어야 한다.** Claude 크론(`CronCreate`)은 세션 한정이고 7일 만에
만료되므로 지속 자동화에는 쓰지 않는다.

| 작업 이름 | 시각 | 실행 | 산출물 |
|---|---|---|---|
| `QuantAutoTradeGuard` | 평일 08:45부터 5분마다 7시간 | `powershell -File scripts/auto_trade_guard.ps1` | 워치독이 없으면 하루 루프 기동 (§4) |
| `Quant EOD AutoDoc` | 평일 16:05 | `python scripts/eod_autodoc.py` | 매매일지 사실 구간 · 리뷰 탭 항목 · `live.json` 백필 · `dashboard.html` · **결정 원장 파생 문서**(`sync_ledgers.py`) |
| `claude_stock_study` | 평일 20:00 | `claude -p "/stock-study auto"` | `_private/주식_study/{날짜}_재무/` 7종목 · 저널 · 스터디 사이트 |
| `claude_dashboard_sync` | 평일 20:40 | `claude -p "/dashboard-sync"` | 매매·스터디 아티팩트 재발행(같은 URL) |
| `Quant Maintain Daily` | 평일 16:20 | `python scripts/maintain.py --daily` | `EOD AutoDoc`(16:05) 뒤. 생성물 갱신 — `gen_facts` · `gen_code_graph` · `sync_ledgers`. 대시보드는 부르지 않는다 |
| `Quant Maintain Weekly` | 금요일 20:50 | `python scripts/maintain.py --weekly` | `claude_dashboard_sync`(20:40) 뒤. 미참조 스크립트 · 에이전트 죽은 경로 · 부산물 용량 · 주석 밀도 · 훅 배선 양방향 검사 → `docs/reports/MAINTENANCE_WEEKLY.md` |

확인·수정:

```powershell
schtasks /query /tn claude_stock_study /v /fo list | Select-String "다음 실행|마지막 결과"
schtasks /change /tn claude_stock_study /st 20:00
```

> 20:00·20:40은 원래 16:00·16:20이었다. 2026-09-07에 두 작업이 모두 세션 사용량 한도(17시 리셋)에 걸려
> 실패했다(`LastTaskResult=1`). 한도 리셋 뒤로 옮겼다. 순수 파이썬인 `Quant EOD AutoDoc`은 한도와 무관해
> 16:05에 그대로 둔다.

## 2. 클라우드 루틴 (Claude)

| 루틴 | 시각 | 내용 |
|---|---|---|
| 장전 시황 브리핑 | 평일 08:30 KST | `market-brief` 결과를 세션으로 전달. 링크는 `_private/LINKS.md` |

PC가 꺼져 있어도 돈다는 점이 OS 예약작업과 다르다. 대신 이 저장소 파일을 만들지는 않는다.

## 3. 훅 — 결정론적 백스톱 (`.claude/hooks/`)

모델 판단에 맡기지 않고 매번 같은 자리에서 걸리는 장치다. `.claude/`는 gitignore 로컬 전용이라
훅 파일 자체는 커밋되지 않는다.

| 훅 | 시점 | 하는 일 |
|---|---|---|
| `secret-gate.ps1` | PreToolUse (Bash·PowerShell) | app_key·app_secret·계좌번호·개인 이름이 커밋 경로로 새는 것을 차단 |
| `docs-gate.ps1` | PreToolUse (Bash·PowerShell) | 문서 커밋 전 `check_docs.py` 정합 확인. 링크·색인에 더해 **결정 원장 파생 문서 드리프트**(`sync_ledgers.py --check`)도 여기서 막힌다 |
| `lexicon-gate.ps1` | PreToolUse (Write·Edit) | 파일에 쓰려는 본문을 `check_plain_language.py --stdin`으로 검사해 쓰지 않기로 한 말(`고아`·`프로브`·`사다리`·`가디언` 등)이 들어가는 순간 막는다. 금지어를 설명하는 글은 본문에 `lexicon-ok` 표시로 통과 | <!-- lexicon-ok: 금지어를 예시로 인용하는 줄 -->
| `review-reminder.ps1` | Stop | 코드 변경 뒤 리뷰 누락을 상기 |
| `eod-gate.ps1` | SessionStart | 사후검토가 밀린 거래일이 있으면 세션 시작에 알림 |
| `cron-gate.ps1` | SessionStart | 예약작업이 예정 시각을 넘겨 안 돌았거나 `LastTaskResult≠0`이면 작업 이름·실패 시각·복구 커맨드를 알림 |
| `dashboard-refresh.ps1` | Stop | 매매일지·백테스트가 `dashboard.html`보다 새것이면 리뷰 항목과 대시보드를 다시 만든다. 낡았는지는 수정시각으로 보므로 편집 도구·스크립트·다른 세션 어느 경로로 고쳤든 걸린다 |

## 4. 하루 무인 루프 — `/auto-trade-day`

장 시작부터 마감 뒤 문서까지 하루치를 한 번에 돈다. 기계적인 부분과 판단이 필요한 부분을 갈라 놓았다.

| 층 | 담당 | 하는 일 |
|---|---|---|
| 감시자 | `scripts/auto_trade_guard.ps1` | 평일 5분 주기 예약작업. 장중인데 워치독이 없으면 기동한다. 남은 트레이더가 남아 있으면 먼저 내린다 |
| 워치독 | `scripts/auto_trade_day.ps1` | 사전 점검(중복 프로세스·exe 갱신 여부·계좌 모드), 보조 프로세스·유니버스·대시보드·알림 기동, 트레이더를 마감까지 감시·재기동, 마감 뒤 `eod_autodoc.py` 실행 |
| 감독 | `.claude/commands/auto-trade-day.md` | 국면 판단, 증분 로그 감시, **무발주 감시**, 결함을 코드/상황으로 분류, 코드면 수정·재빌드, **이슈 대장 누적**, 마감 뒤 해석 문서 |

워치독 상태는 `_private/_auto_trade_day.json` 한 파일에 적힌다(`phase`·`sessions`·`history`). 로그 전체를
훑는 대신 이 파일을 읽는다. 30초 미만 종료가 3연속이면 `phase=crash_loop`로 스스로 멈춘다 — 재기동으로
풀리지 않는 배선 문제를 계좌에 대고 반복하지 않기 위해서다.

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
`dashboard_server.py`, `notify_sidecar.py`, `live_prices_feed.py`)이 있는지 확인하고, 없으면 남은 창을 내리고 같은 명령으로
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
`껍데기`(역할 프로세스가 죽은 `quant-*` 창) / `없음`. <!-- lexicon-ok: 금지어를 예시로 인용하는 줄 -->

```powershell lexicon-ok
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1          # 현황만
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1 -Reap    # 중복·껍데기 정리
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
대장은 `_private/`라 커밋되지 않으므로, 남길 내용은 `docs/eod/YYYY-MM-DD.md`와 매매일지로 옮겨 적는다.

> 무발주 감시를 의무로 올린 계기는 2026-09-08 오전이다. 분할 매수 기준선 미형성과 명목 한도 전량 거부가
> 겹쳐 한 시간 동안 한 주도 나가지 않았는데, 크래시가 없어 로그도 워치독도 조용했다.

## 5. 장중 상시 루프

| 루프 | 주체 | 주기 | 하는 일 |
|---|---|---|---|
| 매크로 국면 파일 전달 | `PYQuant/tools/macro_regime_feed.py` | 상시 | `regime.json` 갱신 → 엔진이 `OrderGate::set_entry_halt` 토글(신규 매수만 차단, 청산은 통과) |
| 엔진 내부 국면 | `RegimeController` | `regime_reeval_sec`(기본 300초) | 국면별 전략 집합 자동 선택, BEAR에서 `FORCE_LIQ` |
| 제어 스레드 | `Engine::control_thread_fn` | 상시 | 잔고 대조·손익 갱신 감시, 끊기면 보수정지 |
| 증분 로그 감시 | `scripts/parse_quant_log.py --watch` | 15~20분 | 유의미한 창일 때만 출력. 조용하면 토큰 0 |
| 전 종목 시세 파일 전달 | `scripts/live_prices_feed.py` | 20초(`PRICES_PERIOD_SEC`, D-028) | 네이버 벌크 시세를 100종목씩 묶어 받아 `Quant/config/prices_live.json`으로 떨군다. KIS REST 초당 한도와 무관해서 2,700종목을 20초 주기로 훑을 수 있다. `UniverseScanner`가 이 파일을 읽는다 |
| 매매 알림 | `scripts/notify_sidecar.py` | 체결 즉시 / 요약 30분 | 당일 체결 원장 CSV를 증분으로 읽어 체결을 바로 보내고, 평단·손익 표는 KIS 잔고조회로 주기 발송 |

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
py scripts/notify_sidecar.py --config Quant/config/config_dev_paper.json --test        # 수신처 확인
py scripts/notify_sidecar.py --config Quant/config/config_dev_paper.json --interval 1800
py scripts/notify_sidecar.py --config … --events FILL,REJECTED --echo                  # 거절도 함께
```

## 6. 마감 뒤 문서 파이프라인

사실은 스크립트가, 해석은 사람(또는 클로드)이 쓴다. 이 경계를 지키면 자동 실행이 손으로 쓴 문장을 덮지 않는다.

```
scripts/eod_autodoc.py
  ├─ 매매일지 사실 구간   strategies/<전략>/live/YYYY-MM-DD.md   (수기 일지가 있으면 보존)
  ├─ PYQuant/dashboard/backfill_live.py      → research/dashboard/live.json
  ├─ scripts/build_review_entry.py --date …  → research/dashboard/reviews.json  (사실 키만)
  └─ PYQuant/dashboard/build_dashboard.py    → research/dashboard/dashboard.html
```

16:05 예약 실행만이 아니라 장중에도 돈다. 매매일지나 백테스트를 쓰고 나면 Stop 훅이 대시보드와
수정시각을 비교해 낡은 만큼만 다시 만든다. 손으로 돌릴 때는 `py scripts/refresh_dashboard.py --if-stale`.

| 스크립트 | 역할 |
|---|---|
| `scripts/eod_collect.py` | 원장·로그에서 사실만 뽑는다(세션·거부 히스토그램·라운드트립·주문 공백) |
| `scripts/build_review_entry.py` | 위 사실을 `quant.review/v1` 항목으로 만들어 리뷰 탭에 넣는다. 기존 항목의 해석 키(`axes`·`improvements`·`gate`)는 건드리지 않고, 항목에 `"locked": [...]`가 있으면 그 키도 제외한다. `incidents`는 목록을 새로 만들되 제목이 같은 항목의 `impact_html`(사람이 쓴 영향)은 옮겨 온다 |
| `scripts/build_study_site.py` | `_private/주식_study/` 전체를 날짜별로 묶어 스터디 사이트 재생성 |
| `scripts/refresh_dashboard.py` | 위 재생성 순서(라이브 백필·리뷰 항목·생성기)를 소유한다. `--if-stale`은 원천 파일이 산출물보다 새것일 때만 돈다. `eod_autodoc.py`와 Stop 훅이 모두 이 스크립트를 부르므로 절차가 한쪽만 고쳐져 갈라지지 않는다. 실행 기록은 `logs/refresh_dashboard.log` |
| `scripts/check_docs.py` | 깨진 내부 링크·색인 누락 검사. exit 0이어야 문서 커밋 |
| `scripts/check_code_conventions.py` | 스테이징된 코드 변경의 규약 검사 — 중괄호(`brace_style.py --check`), 없는 D-NNN 참조, 규약에 없는 주석 태그, 주석·코드 줄 성격 집계. `--comment-only`는 코드 줄이 섞였는지 본다. 밀도는 보지 않는다(정본 `docs/guides/MAINTENANCE_AUTOMATION.md` 4절이 밀도를 게이트로 걸지 말라고 정해 두었다) |
| `scripts/maintain.py` | 위 검사기를 한 번에 돌리는 진입점. `--check`는 `check_docs` → `check_code_refs --diff-only` → `gen_facts --check` → `gen_code_graph --check` 순으로 묶어 표로 요약한다. `--weekly`는 파일별 주석 밀도와 태그 없는 긴 블록을 `docs/reports/MAINTENANCE_WEEKLY.md`에 남긴다 |
| `scripts/check_code_refs.py` | 문서가 가리키는 코드 참조가 실재하는지 검사한다 — 경로, `파일::심볼`, 줄번호 참조. 줄번호 참조는 코드가 움직이면 조용히 어긋나므로 새로 추가된 줄에서 막고 `파일::심볼`로 쓰게 한다 |
| `scripts/gen_facts.py` | 저장소를 세어 `docs/facts.json`을 만들고, 문서의 `<!-- gen:이름 -->` 블록을 그 값으로 채운다. 하네스 개수·훅 배선처럼 손으로 세면 반드시 어긋나는 숫자가 대상이다. KIS 토큰 캐시 파일명은 실 키 앞부분이 들어가므로 가려서 쓴다 |
| `scripts/gen_code_graph.py` | 헤더 포함 관계로 모듈 그래프를 만들어 `docs/CODE_GRAPH.md`·`code_graph.dot`·`code_graph.json`을 생성한다. `--impact <파일>`은 그 파일을 고쳤을 때 재검증 대상을 파일을 열지 않고 뽑는다 |
| `scripts/brace_style.py` | 중괄호와 블록 앞뒤 빈 줄을 기계적으로 맞춘다(`.clang-format`의 Allman·`InsertBraces`와 같은 규칙). 손으로 맞추지 않는다 |
| `scripts/check_plain_language.py` | 쓰지 않기로 한 말을 검출·치환한다(`--fix`는 뒤 조사까지 맞춘다). 정본은 `docs/STYLE_GUIDE.md`, 게이트는 `lexicon-gate.ps1`과 `@committer` |

해석을 채우는 커맨드는 `/eod-review`(사후검토 문서) → `/trade-log`(매매일지 해석) → `/dashboard-sync`(아티팩트 재발행)
→ `/stock-study`(종목 학습) → `/daily`(DAILY_LOG prepend) 순이다.

## 7. 실패했을 때 어디를 보나

| 증상 | 먼저 볼 것 |
|---|---|
| 대시보드가 어제에 머물러 있다 | `Quant EOD AutoDoc`의 마지막 결과 → `logs/eod_autodoc.log` |
| 아티팩트만 낡았다 | 세션 시작 `[CRON]` 알림을 먼저 본다(`cron-gate.ps1`). 원인 문자열은 `_private/_cron_dashboard.log` |
| 스터디가 리포트만 있고 저널이 없다 | 중도 중단. `/stock-study`를 다시 부르면 새 종목을 고르지 않고 빠진 산출물만 채운다 |
| 예약작업이 `LastTaskResult=1` | 세션 사용량 한도를 먼저 의심한다(`_private/_cron_dashboard.log`) |
| 트레이더가 계속 죽는다 | `_private/_auto_trade_day.json`의 `history`에서 종료 코드·지속 시간 |

## 8. 아직 자동화하지 않은 것

- 예약작업 실패의 **자동 복구** — `cron-gate.ps1`이 알리기까지다. 다시 돌리는 것은 사람이 커맨드를 부른다.
- 유니버스 장중 주기 재스캔 — 기동 시 1회만 돈다.
- 커밋·푸시 — `@committer` 승인 게이트를 일부러 유지한다. 자동화 대상이 아니다.
