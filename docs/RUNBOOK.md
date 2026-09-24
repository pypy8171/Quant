# 운영 런북 (RUNBOOK) — PowerShell 복붙용

이 파일이 운영 명령의 정본이다. `py ../quant-devtools/gen_runbook.py`가 `docs/RUNBOOK.html`(복사 버튼 달린 로컬 페이지, 경로에
사용자명이 들어가 gitignore)로 렌더하고, `gen_facts --apply`(Stop 훅)가 매번 같이 돌린다. 본문의 `{ROOT}`는 렌더 때
저장소 절대경로로 바뀐다. 각 절 머리의 `<!-- sync: -->` 도장은 그 절이 인용하는 스크립트가 바뀌면 낡음으로 잡힌다 —
절을 확인해 고치고 `py ../quant-devtools/sync_impact.py --restamp docs/RUNBOOK.md`. 코드 블록 안의 스크립트 경로가 사라지면
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

<!-- sync: scripts/auto_trade_day.ps1@be03aeb scripts/auto_trade_guard.ps1@6a54b80 -->

감시견 하나가 국면 보조 프로세스·유니버스·대시보드·알림·트레이더를 순서대로 띄우고, 장 마감까지 트레이더가 죽으면 다시
띄운다. 띄우기 전에 그날 장이 열리는지 KIS에 물어(`scripts/check_market_open.py`) 휴장일이면 아무것도 안 띄우고 끝낸다 —
조회가 실패해 개장 여부를 모르는 날은 휴장으로 보지 않고 그대로 진행한다. 마감 뒤 `scripts/market_close_autodoc.py`(일지 사실 구간·리뷰 탭·대시보드)까지 돈다. 부속 창에는 체결 기록기·엔진 자원 표본기와 원장 저널 적재기(`quant-ledger`)가 있는데, 저널 적재기는 엔진이 주문 전에 파일로 적어 둔 원장(D-113)을 DB로 따라 적는다 — 죽어도 되살아나면 안 읽은 구간부터 따라잡는다. 트레이더는 이 감시견이 소유한다 —
손으로 따로 띄우면 엔진이 둘이 된다. 기동 전에 이미 떠 있는 `quant_trader`가 있으면 중단하는데, **발주하는 계좌가 같을 때만**
센다 — 떠 있는 프로세스의 명령줄에서 config를 찾아 `kis.account_no`와 `is_paper`를 열쇠로 만든다. config에 `replay_file`이
있는 프로세스(워크트리의 리플레이 측정)는 증권사에 주문을 내지 않으므로 세지 않고, 열쇠를 읽지 못하면 막는 쪽으로 남긴다.
`-Roles order,strategy,feed`(옛 이름 `-Split`)를 주면 트레이더가 주문·전략·시세 세 프로세스다(D-114 단계 5) —
이날은 `quant_trader`가 셋인 것이 정상이고, 하나가 내려가면 감시견이 나머지도 내려 셋을 같이 다시 띄운다.
셋 중 하나라도 빠진 역할 목록은 뜨기 전에 거절한다 — 실시간 소켓을 쥐는 것이 시세 역할이라, 시세가 없으면
시세도 체결통보도 안 들어오는데 나머지는 멀쩡히 떠 있다. 먼저 나간 쪽이 정상 종료였으면 짝이 스스로 나가기를 20초 기다렸다가 그래도 안 나가면 강제로 내리고, 크래시였으면 기다리지 않는다 — 기다리는 동안 짝이 `stop()`을 돌려 공유 쪽지에 종료 사유를 남긴다(D-114).

모의와 실계좌를 한 기계에서 같이 돌리는 날은 config `instance` 하나가 두 벌을 가른다(D-122). 실계좌 config에
`"instance": "live"`가 있으면 감시견이 상태 파일 `_private/_auto_trade_day_live.json`, 실행 로그 `auto_trade_day_live_*.log`,
엔진 로그 폴더 `Quant/build_win/logs_live`(`QUANT_LOG_DIR`), 마감 표지 `session_done_live_<날짜>`, 창 제목 접미를 쓴다.
마감 정리(`quant_procs.ps1 -KillAll`)도 그 감시견의 자손만 내리므로, 15:35에 마감하는 모의 쪽이 20:00까지 도는 실계좌를
같이 내리지 않는다. 가드도 예약작업 이름이 `QuantAutoTradeGuard_live`로 갈린다.
부속 창도 config를 따라 뜬다 — 대시보드는 `dashboard_port`와 `ledger_journal_dir`를, 체결 기록기는 `zmq_pub_port`와
역할 포트(`zmq_feed_pub_port`·`zmq_strategy_pub_port`, 안 적으면 +2·+3)를 읽는다.
이 셋을 안 넘기면 실계좌 감시견이 띄운 화면에 모의 계좌의 원장·체결이 뜨고 두 감시견이 8787 한 자리를 다툰다(2026-09-23 실측).
config는 `-Encoding UTF8`로 읽는다 — PowerShell 5.1의 기본값이 cp949라 BOM 없는 UTF-8 config의 한글 주석에서 JSON 읽기가 통째로 실패한다.

실계좌 기동은 아래 기본 명령에 `-Config Quant\config\config_live.json -Until 20:05`을 붙이고, 가드는 같은 `-Config`로 `-Install`한다.
그 config는 실계좌 인증 정보라 저장소에 없다(gitignore) — 복붙할 명령 전문은 `_private/LINKS.md`에 있다.

```powershell
cd {ROOT}
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1
```

| 옵션 | 뜻 |
|---|---|
| `-Config Quant\config\config.json` | 실계좌 config로 (기본은 모의 `Quant\config\config_dev_paper.json`) |
| `-Until 15:35` | 이 시각 이후로는 재기동하지 않는다 (기본 15:35) |
| `-DryRun` | 무엇을 띄울지만 출력하고 실제로 띄우지 않는다 |
| `-NoRegimeFeed` / `-NoUniverse` / `-NoDashboard` / `-NoNotify` / `-NoMarketClose` | 해당 단계 건너뛰기 |
| `-NoTrader` | 트레이더를 띄우지 않는다 — 리눅스(WSL)가 띄우는 날. 부속 창·유니버스·마감 정리는 그대로. 아래 1.1절 |

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

### 1.1 트레이더를 리눅스(WSL2)에서 띄우는 날

같은 계좌에 엔진은 하나여야 한다. Windows 쪽은 `-NoTrader`로 띄워 부속 창·유니버스·마감 정리만 맡기고(상태파일에
`trader=external`이 남아 감시자 예약작업도 그날은 Windows 트레이더를 띄우지 않는다), 트레이더는 리눅스 쪽 하루 루프
`scripts/auto_trade_day.sh`가 맡는다 — 기동 전 검사(WSL·Windows 양쪽 중복 프로세스, 모의계좌 확인), `ninja` 증분 재빌드,
미체결 복원(`seed_open_orders.py`), 마감(`--until`, 기본 15:35)까지 죽으면 재기동, 30분 안 3회 종료면 크래시 루프로 멈춤(`exit 3`).
로그는 `QUANT_LOG_DIR`로 Windows 쪽 `Quant\build_win\logs`에 쓰게 해서 `parse_quant_log.py`·`check_runtime_health.py`·
`market_close_autodoc.py`가 평소처럼 읽는다. 엔진은 마감 뒤 스스로 내려간다(D-098). 마감 정리는 Windows 창이 한다.
순서: 08:40까지 Windows 창 → 이어서 cmd 창(wsl). 둘 다 08:45 감시자보다 먼저.

```powershell
cd {ROOT}
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -NoTrader   # 창 1 — 부속 창·유니버스·마감 정리
cmd /k wsl -d Ubuntu-24.04 -u root -e bash scripts/auto_trade_day.sh          # 창 2 — 트레이더(cwd 가 /mnt/c/… 저장소 루트로 넘어간다)
```

리눅스 쪽 상태는 `_private/_auto_trade_linux.json`(`phase`·`pid`·`sessions`·`history`·`error`), 실행 로그는
`logs/auto_trade_linux_YYYYMMDD.log`. `error` 값은 Windows 표와 같고 `duplicate_process_windows`(Windows 트레이더가 떠 있음)·
`not_paper`(모의계좌 아님)·`stale_exe`(`--no-build`인데 소스가 더 새것)·`no_build_dir`가 더 있다. 미리 보기는
`bash scripts/auto_trade_day.sh --dry-run`. 빌드 폴더가 없으면 `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B ~/quant-build -S Quant`
(Ubuntu-24.04·GCC 13, `libcurl4-openssl-dev libssl-dev libzmq3-dev`; 다른 자리면 `QUANT_LINUX_BUILD`). 그날 판정은
`check_runtime_health.py`의 `실행 플랫폼` 행 — 같은 날 Windows·Linux가 섞이면 FAIL(엔진 둘).

`quant-recorder`(ZMQ→TimescaleDB)가 붙는 DB는 WSL2(Ubuntu-22.04) 안의 Docker가 낸다. 감시견이 `quant-wsl-keepalive`
창(`wsl -e sleep infinity`)을 같이 띄워 배포판을 붙잡는다(단발 `wsl -e` 호출은 끝나자마자 배포판이 내려간다, 09-16 실측).
DB가 안 떠도 매매는 돈다 — recorder 적재만 빠진다. 엔진 CPU·메모리·스레드별 CPU·함수별 자기 시간(perf)은 `quant-procwatch`
창(`py PYQuant/main.py procwatch --wsl-distro Ubuntu-24.04`)이 5초마다 `/proc`를 읽어 적재하고 그라파나 "Quant Ops"의 엔진 패널이
그것을 그린다 — 그날 적재 여부는 `check_runtime_health.py`의 `자원 표본 적재` 행. perf는 `sudo apt install linux-tools-generic`
(없으면 함수별 패널만 빈다). 손으로 확인:

```powershell
wsl -l -v                                                            # Ubuntu-22.04가 Running인지
wsl -e docker inspect quant-tsdb --format "{{.State.Health.Status}}"
wsl -e docker ps -a --filter name=quant-tsdb
```

클로드가 감시·수정·마감 문서까지 맡게 하려면 터미널에서 `/auto-trade-day`. 마감 뒤 순서는 [AUTOMATION.md](AUTOMATION.md) 하루 흐름.

## 2. 장중 매매를 창 5개로 손으로 띄우기

<!-- sync: PYQuant/tools/macro_regime_feed.py@712c14b PYQuant/tools/universe_feed.py@543096a scripts/notify_trades.py@41177d1 -->

1절 감시견이 도는 날에는 쓰지 않는다(트레이더가 둘이 된다). 대상은 DevScale 모의계좌 `Quant\config\config_dev_paper.json` —
`Quant\config\config.json`은 실계좌라 장중 시험에 쓰지 않는다. 각 창은 별도 프로세스이고 닫으면 그 부분만 멈춘다.

창 1 — 매크로 국면 보조 프로세스(제일 먼저, 장 끝까지 유지). 죽으면 `regime.json`이 낡아(기본 600초) 국면 게이트가 마지막 값으로 굳는다 — 새 정지·해제·매수 비율이 반영되지 않는다.

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\macro_regime_feed.py --interval 180 --out Quant\config\regime.json
```

창 2 — 유니버스 갱신(장 전 1회, 끝나면 닫아도 된다). 코스피+코스닥 시총 500 ∪ 거래대금 500. 실패해도 KIS 랭킹 폴백으로 매매는 된다.
종목 목록은 T-1 data.go.kr, 시총·거래대금은 실행 시점 네이버 값이다(`--no-live`면 스냅샷 값). `[universe_feed] 기록 완료 → … (N종목, 목록 기준일 …)`이 찍히면 성공.

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
.\PYQuant\.venv-win\Scripts\python.exe scripts\notify_trades.py --config Quant\config\config_dev_paper.json --interval 1800
```

config별 전략: `config_dev_paper.json` DEVIATION_SCALE(일봉 정배열+눌림 게이트, 3분봉 이격 분할 지정가) ·
`config_itb_paper.json` INTRADAY_BREAKOUT(거래대금 스캔 돌파 매수, 물린 보유분 분리 청산) · `config_mm_paper.json` MARKET_MAKING(삼성전자 단일 시장조성).

## 3. 실시간 대시보드

<!-- sync: scripts/dashboard_server.py@5207587 -->

엔진 재빌드 없이 이미 있는 데이터(KIS 잔고·`regime.json`·`universe_scan.json`·로그·체결원장)를 브라우저에 3초마다
표시한다. 종목 행 클릭 → 일/주/5분/3분봉 차트. 종목 뉴스·속보(네이버, 보유 종목 전부 + 유니버스 순환)와 증권사 리서치(매시간 갱신) 카드도 같은 화면에 있다. 라이브 데이터는 이 로컬 서버가 있어야 뜬다(발행 URL 하나로는 안 된다).

한 줄 실행 — 서버를 새 창에서 띄우고 포트(8787)가 열리면 크롬을 연다:

```powershell
cd {ROOT}; $env:PYTHONUTF8="1"; Start-Process py -ArgumentList 'scripts\dashboard_server.py' -WorkingDirectory (Get-Location); while(-not (Test-NetConnection 127.0.0.1 -Port 8787 -InformationLevel Quiet)){Start-Sleep 1}; Start-Process chrome "http://127.0.0.1:8787"
```

다른 모의계좌·포트(포트는 URL도 맞춰 바꾼다):

```powershell
cd {ROOT}; $env:PYTHONUTF8="1"; Start-Process py -ArgumentList 'scripts\dashboard_server.py','--config','Quant\config\config_mm_paper.json','--port','8790' -WorkingDirectory (Get-Location); while(-not (Test-NetConnection 127.0.0.1 -Port 8790 -InformationLevel Quiet)){Start-Sleep 1}; Start-Process chrome "http://127.0.0.1:8790"
```

종료는 서버 창을 닫거나 Ctrl+C. 세션 현황판은 `http://127.0.0.1:8788/sessions`(`../quant-devtools\session_board.py`).

## 4. 로그 감시·요약

<!-- sync: scripts/parse_quant_log.py@6057b64 -->

체결원장(CSV)에 없는 운영 이벤트(원장 저널 기록 실패·ERROR·KIS 거부·게이트 봉쇄·WS 재연결·HTTP 오류·주문·체결·15초 제한 시간 초과·잔고 조회 사이클 걸침)만 뽑는다. 손익은 지어내지
않고 개수·사유만. 주문이 있는 창은 접수 지연 한 줄을 붙인다 — RTT 중앙값·3초 이상 건수·버킷대기 중앙값과 "서버 응답 지연인지 초당한도 버킷인지" 판정(임계는 `scripts/check_runtime_health.py`가 소유). 로그는 실행파일 옆 `Quant\build_win\logs\quant_trader.log`다(cwd가 아니라 exe 기준 — 정본 `scripts/_logdir.py`, 루트 `logs\`는 테스트 바이너리 것).
7일 지난 날의 줄은 `archive\quant_trader_<날짜>.log.gz`로 옮겨져 있는데(`maintain.py --rotate-logs`), `--date`를 주면 그 gz를 이어서 읽는다.

```powershell
cd {ROOT}
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --seek-end     # 감시 시작 시 과거 백로그 건너뜀
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --watch        # 증분 감시(유의미할 때만 출력)
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --full                        # 당일 전체 요약
.\PYQuant\.venv-win\Scripts\python.exe scripts\parse_quant_log.py --full --date 20260903 --json  # 특정일 JSON
```

## 5. 운영단말 (수동 매도)

<!-- sync: docs/guides/OPS_TERMINAL.md@d52678e docs/guides/MFC_TERMINAL.md@75d9070 -->

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

<!-- sync: PYQuant/tools/investor_flow_logger.py@9986905 PYQuant/tools/index_intraday_logger.py@89dbbb5 -->

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
.\PYQuant\.venv-win\Scripts\python.exe ../quant-devtools\check_docs.py               # 문서 드리프트 (0=통과, Stop 훅과 같음)
py ../quant-devtools\gen_facts.py --check                                             # gen 블록 낡음 (--apply 로 치환)
py ../quant-devtools\check_code_conventions.py                                        # 중괄호·캐스트·약어 이름
py ../quant-devtools\check_plain_language.py                                          # 금지 표현 (--fix 로 치환)
.\PYQuant\.venv-win\Scripts\python.exe scripts\check_backtest.py            # 위기 스터디 재현성 (0=PASS)
.\PYQuant\.venv-win\Scripts\python.exe ../quant-devtools\gen_code_graph.py            # 코드 의존 그래프 → docs\CODE_GRAPH.md
.\PYQuant\.venv-win\Scripts\python.exe ../quant-devtools\gen_code_graph.py --impact core/Types.h
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\log_report.py Quant\build_win\logs\quant_trader.log --md report.md --html report.html
```

## 9. 데이터 소스 점검 (일회성 · 조회 전용)

```powershell
cd {ROOT}
$env:PYTHONUTF8 = "1"
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\check_kis_investor.py 005930   # 투자자별 매매동향 깊이
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\check_datagokr.py              # data.go.kr 인증/OHLCV/PIT (DATA_GO_KR_KEY 필요)
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\check_adjusted.py 005930       # 수정주가 vs 원주가 갭
.\PYQuant\.venv-win\Scripts\python.exe PYQuant\tools\full_universe_dump.py          # 전종목 코드 덤프(부하 시험, DATA_GO_KR_KEY 필요)
```

## 10. 부하·지연 벤치 (처리량·꼬리지연 실측 · 주문 없음)

벤치 실행파일도 기본 빌드에 들어간다 — 벤치만 다시 만들 때는 타깃을 지정해 빌드(`Quant\build_win\`에 산출). 인메모리 벤치는 인증이
필요 없고 `feed_latency_measure`만 실제 KIS 조회를 한다.

```powershell
cd {ROOT}
cmake --build Quant\build_win --target bench_market_firehose bench_feed_ingest bench_intake feed_latency_measure
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
`feed_latency_measure` — 실제 KIS 실시간 수신 지연(인증 필요, `--symbols` > `--universe` > 내장 15종목).

```powershell
.\Quant\build_win\bench_intake.exe 8 3 mpsc
.\Quant\build_win\feed_latency_measure.exe --configs Quant\config\config_dev_paper.json --duration 60
.\Quant\build_win\feed_latency_measure.exe --configs Quant\config\config_dev_paper.json --symbols "005930,000660,035720" --trade-only 1
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

<!-- sync: scripts/notify_trades.py@41177d1 -->

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
.\PYQuant\.venv-win\Scripts\python.exe scripts\notify_trades.py --config Quant\config\config_dev_paper.json --test
```

## 13. 다른 세션에 메시지 보내기

세션 주소는 탭 제목이 아니라 `quant-XX`다. 대상 세션 탭에 "너 세션 이름 뭐야?"를 붙여 넣으면 `This session is quant-07 [3f6100]`
형태로 답한다 — 앞의 `quant-07`이 주소다. 이름을 모르면 전 세션에 뿌리게 되고 관계없는 세션도 턴을 돌아 토큰을 쓴다.
(`/status`의 세션 UUID는 주소가 아니다.) 여러 세션이 같은 `build_win`에 각자 링크하면 반쯤 고친 트리가 계좌에 올라간다 —
빌드·링크는 한 세션으로 몬다. 규칙 정본은 [guides/MULTI_SESSION.md](guides/MULTI_SESSION.md).

## 14. 환경·참고

- cwd는 항상 저장소 루트. 한글 깨짐은 `$env:PYTHONUTF8 = "1"`(data.go.kr 계열은 `$env:PYTHONIOENCODING = "utf-8"`).
- `DATA_GO_KR_KEY`: `universe_feed` / `check_datagokr` / `full_universe_dump`에 필요한 환경변수.
- 백그라운드 실행: `Start-Process py -ArgumentList 'scripts\dashboard_server.py' -WindowStyle Hidden`(종료는 11절). 평소엔 전용 창 포그라운드 + Ctrl+C.
- 예약작업(마감 문서·스터디·대시보드 동기화)의 시각·등록·복구 명령은 [AUTOMATION.md](AUTOMATION.md) 1절 — 여기 적지 않는다.
- 마감 후 세션 스킬: `/market-close-review` → `/trade-log` → `/dashboard-sync` → `/stock-study` → `/daily`.
