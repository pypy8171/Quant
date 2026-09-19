# 운영 런북 (RUNBOOK) — PowerShell 복붙용

이 파일이 운영 명령의 정본이다. `py scripts/gen_runbook.py`가 `docs/RUNBOOK.html`(복사 버튼 달린 로컬 페이지, 경로에
사용자명이 들어가 gitignore)로 렌더하고, `gen_facts --apply`(Stop 훅)가 매번 같이 돌린다. 본문의 `{ROOT}`는 렌더 때
저장소 절대경로로 바뀐다. 각 절 머리의 `<!-- sync: -->` 도장은 그 절이 인용하는 스크립트가 바뀌면 낡음으로 잡힌다 —
절을 확인해 고치고 `py scripts/sync_impact.py --restamp docs/RUNBOOK.md`. 코드 블록 안의 스크립트 경로가 사라지면
`gen_runbook.py --check`(check_docs)가 잡는다.

절차 문서와의 역할 나눔: 왜·언제는 [AUTOMATION.md](AUTOMATION.md)(예약작업·감시견·하루 흐름), 세션이 따라가는
단계는 `.claude/commands/*.md`, 사람이 창에 붙여 넣는 명령은 여기.

## 0. 공통 준비

새 PowerShell 창을 열면 매번 먼저. Python은 항상 `py` 런처(이 머신의 `python`은 스토어 스텁)이고, 저장소 스크립트는
`.venv-win`의 파이썬으로 돌린다(`py`는 활성 venv를 안 본다).

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
```

## 1. 자동매매 하루 루프 (한 창으로 끝내기)

<!-- sync: scripts/auto_trade_day.ps1@691b256 scripts/auto_trade_guard.ps1@1a3da84 -->

감시견 하나가 국면 보조 프로세스·유니버스·대시보드·알림·트레이더를 순서대로 띄우고, 장 마감까지 트레이더가 죽으면 다시
띄운다. 마감 뒤 `scripts/eod_autodoc.py`(일지 사실 구간·리뷰 탭·대시보드)까지 돈다. 트레이더는 이 감시견이 소유한다 —
손으로 따로 띄우면 엔진이 둘이 된다.

```powershell
cd {ROOT}
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1
```

| 옵션 | 뜻 |
|---|---|
| `-Config Quant\config\config.json` | 실계좌 config로 (기본은 모의 `Quant\config\config_dev_paper.json`) |
| `-Until 15:35` | 이 시각 이후로는 재기동하지 않는다 (기본 15:35) |
| `-DryRun` | 무엇을 띄울지만 출력하고 실제로 띄우지 않는다 |
| `-NoSidecar` / `-NoUniverse` / `-NoDashboard` / `-NoNotify` / `-NoEod` | 해당 단계 건너뛰기 |

진행 상태는 `_private\_auto_trade_day.json`(`phase`·`sessions`·`history`), 실행 로그는 `logs\auto_trade_day_YYYYMMDD.log`.
감시견이 죽으면 잡(Job Object)이 부속 창과 트레이더를 같이 내리고, 감시자 예약작업(평일 08:45부터 5분마다)이 장중이면
다시 띄운다. 최초 1회 등록:

```powershell
cd {ROOT}
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Install
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -DryRun     # 지금 뭘 할지만 보기
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Uninstall  # 해제
```

오늘 `phase`가 멈춤 사유(`crash_loop`·`aborted`·`done`·`closed`·`past_deadline`)면 감시자는 되살리지 않는다 — 원인을 없앤 뒤
손으로 한 번 띄우면 그다음부터 다시 감시자가 맡는다. 사유별 뜻과 감시견 로그 정리는 [AUTOMATION.md](AUTOMATION.md) 4절.
감시견 로그에 `tail -f`를 걸지 않는다(파일 잠금으로 감시견이 죽는다) — `Get-Content -Tail`로 본다.

`quant-recorder`(ZMQ→TimescaleDB)가 붙는 DB는 WSL2(Ubuntu-22.04) 안의 Docker가 낸다. 감시견이 `quant-wsl-keepalive`
창(`wsl -e sleep infinity`)을 같이 띄워 배포판을 붙잡는다(단발 `wsl -e` 호출은 끝나자마자 배포판이 내려간다, 09-16 실측).
DB가 안 떠도 매매는 돈다 — recorder 적재만 빠진다. 손으로 확인:

```powershell
wsl -l -v                                                            # Ubuntu-22.04가 Running인지
wsl -e docker inspect quant-tsdb --format "{{.State.Health.Status}}"
wsl -e docker ps -a --filter name=quant-tsdb
```

클로드가 감시·수정·마감 문서까지 맡게 하려면 터미널에서 `/auto-trade-day`. 마감 뒤 순서는 [AUTOMATION.md](AUTOMATION.md) 하루 흐름.

## 2. 장중 매매를 창 5개로 손으로 띄우기

<!-- sync: PYQuant/tools/macro_regime_feed.py@2201093 PYQuant/tools/universe_feed.py@543096a scripts/notify_sidecar.py@02bb045 -->

1절 감시견이 도는 날에는 쓰지 않는다(트레이더가 둘이 된다). 대상은 DevScale 모의계좌 `Quant\config\config_dev_paper.json` —
`Quant\config\config.json`은 실계좌라 장중 시험에 쓰지 않는다. 각 창은 별도 프로세스이고 닫으면 그 부분만 멈춘다.

창 1 — 매크로 국면 보조 프로세스(제일 먼저, 장 끝까지 유지). 죽으면 `regime.json`이 낡아 신규 매수가 막힌다.

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\macro_regime_feed.py --interval 180 --out Quant\config\regime.json
```

창 2 — 유니버스 갱신(장 전 1회, 끝나면 닫아도 된다). 코스피+코스닥 시총 500 ∪ 거래대금 500. 실패해도 KIS 랭킹 폴백으로 매매는 된다.
`--date`는 기본 T-1이라 그냥 돌리면 전일 종가 기준. `count=NNN, basDt=(어제)`가 찍히면 성공.

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
$env:DATA_GO_KR_KEY = "<발급키>"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\universe_feed.py --market ALL --n-mktcap 500 --n-turnover 500 --out Quant\config\universe_scan.json
```

창 3 — 트레이더 엔진(09:00 직전). 항상 저장소 루트에서 띄운다 — `build_win\` 안에서 띄우면 config 상대경로가 어긋나
유니버스가 1종목으로 무너진다. 마감 청산까지 완주시킨다(장중 Ctrl+C 금지).

```powershell
cd {ROOT}
.\Quant\build_win\quant_trader.exe Quant\config\config_dev_paper.json
```

창 4 — 대시보드(3절), 창 5 — 매매 알림(체결은 즉시, 평단·손익 표는 30분마다. 닫아도 매매에는 영향 없음).

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe scripts\notify_sidecar.py --config Quant\config\config_dev_paper.json --interval 1800
```

config별 전략: `config_dev_paper.json` DEVIATION_SCALE(일봉 정배열+눌림 게이트, 3분봉 이격 분할 지정가) ·
`config_itb_paper.json` INTRADAY_BREAKOUT(거래대금 스캔 돌파 매수, 물린 보유분 분리 청산) · `config_mm_paper.json` MARKET_MAKING(삼성전자 단일 시장조성).

## 3. 실시간 대시보드

<!-- sync: scripts/dashboard_server.py@a24c552 -->

엔진 재빌드 없이 이미 있는 데이터(KIS 잔고·`regime.json`·`universe_scan.json`·로그·체결원장)를 브라우저에 3초마다
표시한다. 종목 행 클릭 → 일/주/5분/3분봉 차트. 라이브 데이터는 이 로컬 서버가 있어야 뜬다(발행 URL 하나로는 안 된다).

한 줄 실행 — 서버를 새 창에서 띄우고 포트(8787)가 열리면 크롬을 연다:

```powershell
cd {ROOT}; $env:PYTHONUTF8="1"; Start-Process py -ArgumentList 'scripts\dashboard_server.py' -WorkingDirectory (Get-Location); while(-not (Test-NetConnection 127.0.0.1 -Port 8787 -InformationLevel Quiet)){Start-Sleep 1}; Start-Process chrome "http://127.0.0.1:8787"
```

다른 모의계좌·포트(포트는 URL도 맞춰 바꾼다):

```powershell
cd {ROOT}; $env:PYTHONUTF8="1"; Start-Process py -ArgumentList 'scripts\dashboard_server.py','--config','Quant\config\config_mm_paper.json','--port','8790' -WorkingDirectory (Get-Location); while(-not (Test-NetConnection 127.0.0.1 -Port 8790 -InformationLevel Quiet)){Start-Sleep 1}; Start-Process chrome "http://127.0.0.1:8790"
```

종료는 서버 창을 닫거나 Ctrl+C. 세션 현황판은 `http://127.0.0.1:8788/sessions`(`scripts\session_board.py`).

## 4. 로그 감시·요약

<!-- sync: scripts/parse_quant_log.py@6a71be6 -->

체결원장(CSV)에 없는 운영 이벤트(ERROR·KIS 거부·게이트 봉쇄·WS 재연결·HTTP 오류·주문·체결)만 뽑는다. 손익은 지어내지
않고 개수·사유만. 로그는 실행파일 옆 `Quant\build_win\logs\quant_trader.log`다(cwd가 아니라 exe 기준 — 정본 `scripts/_logdir.py`, 루트 `logs\`는 테스트 바이너리 것).
7일 지난 날의 줄은 `archive\quant_trader_<날짜>.log.gz`로 옮겨져 있는데(`maintain.py --rotate-logs`), `--date`를 주면 그 gz를 이어서 읽는다.

```powershell
cd {ROOT}
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --seek-end     # 감시 시작 시 과거 백로그 건너뜀
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --watch        # 증분 감시(유의미할 때만 출력)
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --full                        # 당일 전체 요약
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --full --date 20260903 --json  # 특정일 JSON
```

## 5. 운영단말 (수동 매도)

<!-- sync: docs/guides/OPS_TERMINAL.md@e53b5bb docs/guides/MFC_TERMINAL.md@7c29946 -->

토큰은 `Quant\config\config_dev_paper.json`의 `ops_token`. 가이드 [guides/OPS_TERMINAL.md](guides/OPS_TERMINAL.md).

```powershell
cd {ROOT}
.\Quant\build_win\ops_client.exe --token <ops_token> positions
.\Quant\build_win\ops_client.exe --token <ops_token> sell <종목코드> <수량>     # 시장가
```

MFC 창은 바탕화면 `운영단말.lnk` 더블클릭(토큰은 사용자 환경변수 `QUANT_OPS_TOKEN`, 엔진은 감시견 것에 붙는다). 토큰을
바꾸면 `[Environment]::SetEnvironmentVariable('QUANT_OPS_TOKEN', '<새 토큰>', 'User')`. 손으로 띄우기·빌드는
[guides/MFC_TERMINAL.md](guides/MFC_TERMINAL.md).

## 6. 빌드

CMake preset(VS2022/Ninja):

```powershell
cd {ROOT}
cmake --preset x64-release
cmake --build out\build\x64-release
ctest --preset x64-release
```

수동 Ninja 레이아웃(`Quant\build_win\`, 감시견이 띄우는 exe가 여기 있다):

```powershell
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B Quant\build_win -S Quant
cmake --build Quant\build_win
ctest --test-dir Quant\build_win
```

헤드리스 PowerShell에서 LNK1104(한글 TEMP 경로)가 나면 임시 폴더를 ASCII 경로로. 실행 중인 트레이더가 exe를 잠근
LNK1104는 다른 문제다 — 교체 절차는 [AUTOMATION.md](AUTOMATION.md).

```powershell
$env:TEMP = "C:\build_tmp"; $env:TMP = "C:\build_tmp"
```

## 7. forward 데이터 적재 (조회 전용 · 주문 없음)

<!-- sync: PYQuant/tools/investor_flow_logger.py@41e5901 PYQuant/tools/index_intraday_logger.py@34fde54 -->

외국인·기관 확정 수급(장 마감 후 18:10 KST 이후):

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\investor_flow_logger.py --top 50      # 시총 상위 50
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\investor_flow_logger.py --volume-rank
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\investor_flow_logger.py 005930 000660  # 지정 종목
```

장중 지수 스냅샷(코스피/코스닥/코스피200) 30초 주기:

```powershell
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\index_intraday_logger.py --interval 30 --codes 0001 1001 2001
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\index_intraday_logger.py --once       # 점검용 1회
```

## 8. 리포트·게이트 (커밋 전 점검)

```powershell
cd {ROOT}
.\PYQuant\.venv-win\Scripts\python.exe scripts\check_docs.py               # 문서 드리프트 (0=통과, Stop 훅과 같음)
py scripts\gen_facts.py --check                                             # gen 블록 낡음 (--apply 로 치환)
py scripts\check_code_conventions.py                                        # 중괄호·캐스트·약어 이름
py scripts\check_plain_language.py                                          # 금지 표현 (--fix 로 치환)
.\PYQuant\.venv-win\Scripts\python.exe scripts\check_backtest.py            # 위기 스터디 재현성 (0=PASS)
.\PYQuant\.venv-win\Scripts\python.exe scripts\gen_code_graph.py            # 코드 의존 그래프 → docs\CODE_GRAPH.md
.\PYQuant\.venv-win\Scripts\python.exe scripts\gen_code_graph.py --impact core/Types.h
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\log_report.py Quant\build_win\logs\quant_trader.log --md report.md --html report.html
```

## 9. 데이터 소스 점검 (일회성 · 조회 전용)

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\probe_kis_investor.py 005930   # 투자자별 매매동향 깊이
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\probe_datagokr.py              # data.go.kr 인증/OHLCV/PIT (DATA_GO_KR_KEY 필요)
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\probe_adjusted.py 005930       # 수정주가 vs 원주가 갭
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\full_universe_dump.py          # 전종목 코드 덤프(부하 시험, DATA_GO_KR_KEY 필요)
```

## 10. 부하·지연 벤치 (처리량·꼬리지연 실측 · 주문 없음)

벤치 실행파일은 기본 빌드에 안 들어간다 — 타깃을 지정해 먼저 빌드(`Quant\build_win\`에 산출). 인메모리 벤치는 인증이
필요 없고 `feed_latency_probe`만 실제 KIS 조회를 한다.

```powershell
cd {ROOT}
cmake --build Quant\build_win --target bench_market_firehose bench_feed_ingest bench_intake feed_latency_probe
```

`bench_market_firehose` — 전종목 시세를 인프로세스로 쏴 파이프라인 전 구간 지연·처리량(네트워크 없음). `load`=고정 부하, `sweep`=속도 계단 상승.

```powershell
.\Quant\build_win\bench_market_firehose.exe load --tickers 2600 --rate 200000 --duration 20
.\Quant\build_win\bench_market_firehose.exe sweep --start 50000 --step 100000 --max 2000000 --dwell 4
```

`bench_feed_ingest` — TCP 수신부터 전략·주문까지(소켓 경유). `self`=한 프로세스 자가 부하, 두 머신이면 `serve`+`send`.

```powershell
.\Quant\build_win\bench_feed_ingest.exe self --tickers 2600 --rate 200000 --duration 20 --port 47001
.\Quant\build_win\bench_feed_ingest.exe serve --port 47001 --tickers 2600                          # 수신 머신
.\Quant\build_win\bench_feed_ingest.exe send  --host 127.0.0.1 --port 47001 --rate 200000 --duration 20   # 송신 머신
```

`bench_intake` — 큐 인테이크 마이크로벤치(위치인자 N duration qtype delay_ns cap rate, 기본 8 3.0 mpsc 0 65536 0).
`feed_latency_probe` — 실제 KIS 실시간 수신 지연(인증 필요, `--symbols` > `--universe` > 내장 15종목).

```powershell
.\Quant\build_win\bench_intake.exe 8 3 mpsc
.\Quant\build_win\feed_latency_probe.exe --configs Quant\config\config_dev_paper.json --duration 60
.\Quant\build_win\feed_latency_probe.exe --configs Quant\config\config_dev_paper.json --symbols "005930,000660,035720" --trade-only 1
```

스트레스 시험 — 락프리 큐 정합·경합(인자 없음, PASS/FAIL):

```powershell
cmake --build Quant\build_win --target test_ringbuffer_stress test_pipeline_stress test_mpsc
.\Quant\build_win\test_ringbuffer_stress.exe
.\Quant\build_win\test_pipeline_stress.exe
.\Quant\build_win\test_mpsc.exe
```

측정 결과는 [reports/PIPELINE_LATENCY_REPORT.md](reports/PIPELINE_LATENCY_REPORT.md).

## 11. 프로세스 확인·정리

`Get-Process`는 커맨드라인을 안 준다 — 어떤 스크립트가 떠 있는지는 `Get-CimInstance Win32_Process`로 본다.
python이 PID 쌍으로 보이는 것은 py 런처/venv 래퍼가 자식을 낳는 구조라 정상이다.

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'Quant|quant_trader|macro_regime|dashboard_server|auto_trade' } | Select-Object ProcessId, ParentProcessId, Name, CommandLine | Format-List
Get-Process | Where-Object ProcessName -match 'powershell|python|py|quant'    # 가볍게
Stop-Process -Id <PID> -Force
```

## 12. 매매 알림 수신처 설정 (최초 1회)

<!-- sync: scripts/notify_sidecar.py@02bb045 -->

Discord — 서버 → 채널 설정 → 연동 → 웹후크 → 새 웹후크 → URL 복사. 폰 Discord 앱에서 그 채널 알림을 켜면 푸시가 온다.
`_private\notify.json`(gitignore)에 적는다. 체결과 포지션 요약을 다른 채널로 나누려면 웹후크를 둘 발급해 두 번째 형태로.
카카오톡은 developers.kakao.com 앱 → 카카오 로그인 → 동의항목 `talk_message` → REST API 키·refresh token을
`"kakao": {"rest_api_key": "...", "refresh_token": "..."}`로 넣는다. 둘 다 있으면 두 곳 모두 간다.

```json
{ "discord_webhook": "여기에_복사한_URL" }
```

```json
{ "discord_webhook_fill": "체결_채널_URL", "discord_webhook_position": "포지션_채널_URL" }
```

확인:

```powershell
cd {ROOT}
.\PYQuant\.venv-win\Scripts\python.exe scripts\notify_sidecar.py --config Quant\config\config_dev_paper.json --test
```

## 13. 다른 세션에 메시지 보내기

세션 주소는 탭 제목이 아니라 `quant-XX`다. 대상 세션 탭에 "너 세션 이름 뭐야?"를 붙여 넣으면 `This session is quant-07 [3f6100]`
형태로 답한다 — 앞의 `quant-07`이 주소다. 이름을 모르면 전 세션에 뿌리게 되고 관계없는 세션도 턴을 돌아 토큰을 쓴다.
(`/status`의 세션 UUID는 주소가 아니다.) 여러 세션이 같은 `build_win`에 각자 링크하면 반쯤 고친 트리가 계좌에 올라간다 —
빌드·링크는 한 세션으로 몬다. 규칙 정본은 [guides/MULTI_SESSION.md](guides/MULTI_SESSION.md).

## 14. 환경·참고

- cwd는 항상 저장소 루트. 한글 깨짐은 `$env:PYTHONUTF8 = "1"`(data.go.kr 계열은 `$env:PYTHONIOENCODING = "utf-8"`).
- `DATA_GO_KR_KEY`: `universe_feed` / `probe_datagokr` / `full_universe_dump`에 필요한 환경변수.
- 백그라운드 실행: `Start-Process py -ArgumentList 'scripts\dashboard_server.py' -WindowStyle Hidden`(종료는 11절). 평소엔 전용 창 포그라운드 + Ctrl+C.
- 예약작업(마감 문서·스터디·대시보드 동기화)의 시각·등록·복구 명령은 [AUTOMATION.md](AUTOMATION.md) 1절 — 여기 적지 않는다.
- 마감 후 세션 스킬: `/eod-review` → `/trade-log` → `/dashboard-sync` → `/stock-study` → `/daily`.
