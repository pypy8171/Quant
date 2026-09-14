# CLAUDE.md

이 파일은 이 저장소에서 작업할 때 Claude Code(claude.ai/code)에게 제공하는 가이드입니다.

## 빌드 명령어

**Windows (CMake Presets — Visual Studio 2022 / Ninja)**:
```bash
cmake --preset x64-debug   # 디버그 구성
cmake --build out/build/x64-debug

cmake --preset x64-release
cmake --build out/build/x64-release
```

**Windows (수동 Ninja 빌드, 현재 `build_win/` 레이아웃 기준)**:
```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B Quant/build_win -S Quant
cmake --build Quant/build_win
./Quant/build_win/quant_trader Quant/config/config.json
```

**Linux**:
```bash
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant
cmake --build Quant/build
./Quant/build/quant_trader Quant/config/config.json
```

Linux는 `libcurl4-openssl-dev`가 필요합니다 (`sudo apt install libcurl4-openssl-dev`). Windows는 네이티브 WinHTTP를 사용하므로 nlohmann/json(CMake FetchContent로 자동 다운로드) 외에 추가 의존성이 없습니다.

<!-- gen:test-targets -->
단위 테스트는 `Quant/tests/`에 있고 ctest에 등록돼 있습니다 — 실행 타깃 `34`개.

```bash
cmake --build out/build/x64-release --target test_order_gate test_order_router test_ws_frame test_ws_decode test_kis_decode test_ops_server test_ops_protocol test_market_session test_reconcile_plan test_ledger_reconciler test_data_poller test_signal_dispatcher test_bar_aggregator test_order_pacer test_regime_bridge test_regime test_ringbuffer test_ringbuffer_stress test_pipeline_stress test_wake_gate test_symbol_table test_tick_capture test_replay_source test_paper_executor test_feed_mux test_engine test_feed_supervisor test_shard_matrix test_strategy_shard test_strategy_router test_latency_trace test_mpsc test_account_ledger test_logger
```
<!-- /gen -->
테스트 이름은 각각 원장·게이트·라우터·큐·WS 디코더·REST 분봉 디코더·정규장 시각·잔고 대조 계산·잔고 대조기·REST 현재가 폴러·신호 디스패처·발주 조절기·운영단말 프로토콜/서버·비동기 로거·매크로 국면 파일 판정기·N분봉 집계기·소비자 깨우기 조각·구간 지연 CSV·종목 id 테이블·틱 캡처·캡처 리플레이 소스·모의 체결기·피드 소스 mux·수신 N×샤드 M 링 행렬·전략 샤드·종목 id 전략 라우터·WS 피드 감독기·시험용 시세로 도는 Engine 한 바퀴(레인 1×샤드 1, 2×2, 캡처 리플레이)를 가리킨다.

```bash
ctest --preset x64-release          # 저장소 루트에서. 스트레스 2종은 3초로 줄여 돈다
ctest --test-dir Quant/build_win    # 수동 Ninja 레이아웃일 때
```
Linux에서는 `-DQUANT_TSAN=ON`으로 Debug를 ThreadSanitizer로 만들 수 있습니다(ASAN과 배타). 실계좌 연결 검증은 **FEED** 모드로 실행하여 실시간 출력을 확인합니다.

## 실행 모드

`Quant/config/config.json`의 `"mode"` 값으로 제어합니다:

- **`"FEED"`** — KIS WebSocket에 연결하여 실시간 호가·체결 데이터를 1초마다 콘솔에 표시합니다. 주문 없이 연결 상태와 인증 정보를 검증할 때 사용합니다. 최상위 config `"tickers"`(국내 현물)와 함께 `"futures"`(국내 선물 코드 배열)를 주면 선물 실시간도 같이 구독·표시합니다. 선물은 실계좌 WS 도메인 전용이라 `is_paper=true`면 경고만 내고 건너뜁니다.
- **`"TRADE"`** — 5-스레드 엔진(3-스레드 파이프라인 + 체결 소비 + 제어 스레드)을 실행하고 장 중(평일 09:00–15:30 KST)에 실제 주문을 냅니다.

`config.json`에는 현재 **실거래 인증 정보**(`app_key`, `app_secret`, 실계좌 번호)가 저장되어 있습니다. 모의투자 엔드포인트(`openapivts.koreainvestment.com:29443`)로 전환하려면 `"is_paper": true`로 설정하세요.

## 설계 목표와 원칙 (D-071)

목표는 **전 시장 실시간 피드(코스콤급, 2,500+종목·초당 수십만 건)를 받을 수 있는 구조**다. KIS 41종목은 그 구조의
1×1 특수 케이스일 뿐이고, 현재 구조 유지는 목표가 아니다 — 바꿔서 나아지면 바꾼다. 설계·리뷰·리팩터 판단은 아래 원칙으로 한다.
단계와 버린 대안은 [docs/DECISIONS.md](docs/DECISIONS.md) D-071.

1. **한 소켓은 한 스레드가 처리한다(N:1).** 소켓 여럿이 스레드 하나에 고정 배정되는 것이 정상이고, 한 소켓을 스레드 둘이 나눠 읽는 것(워커 풀이 준비된 소켓을 아무나 집어가는 구조)만 금지한다. 스레드 수는 소켓 수가 아니라 코어·부하로 정한다.
2. **순서 보장 단위는 종목이다.** 채널 간 순서는 지키지 않고, 종목 해시로 샤딩한다.
3. **수신 스레드는 얇게.** 읽기·최소 디코드·push까지. 파싱이 무거워지면 소비자로 옮긴다.
4. **리스크·주문은 단일 시퀀서.** `OrderGate`·원장은 샤딩하지 않는다. 앞단(수신 N·전략 샤드 M)만 늘린다.
5. **큐는 생산자 수로 고른다.** 생산자 하나면 `RingBuffer`, 여럿이면 `MpscQueue`. N×M SPSC 행렬을 MPSC 하나보다 먼저 검토한다.
6. **hot path에 문자열 없음.** 종목은 기동 시 정수 id를 받고 틱·호가·디스패치·현재가 캐시는 id 배열 인덱스로 간다.
7. **측정 없이 손대지 않는다.** 큐 고수위·`seq` 구간 시각·타이머 해상도 실측이 최적화의 전제다.
8. **틱은 캡처한다.** raw 틱 append-only 캡처가 리플레이 백테스트의 입력이다.

## 아키텍처

스레드 모델·링 행렬·샤드·디스패처·체결 소비·제어 스레드·잔고 대조·핵심 타입·국면 두 축·KIS 클라이언트 7파일·WebSocket·로깅의 요약은
[docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md)에 있다(sync 도장이 헤더 변경을 잡는다). 코드를 읽는 순서는
[docs/CODE_FLOW.md](docs/CODE_FLOW.md), 전략 추가는 `StrategyBase` 상속 → `on_start`에서 `symbol_of()`로 id를 받아 `td.sym`과 정수 비교 →
`main.cpp`의 `engine.add_strategy(...)` 등록 순서다(자세한 절차는 같은 문서).

## 작업 규약

### 문서 동기화 (드리프트 방지)

색인·요약·링크가 실제 트리와 어긋나는 것을 막는다. 대표/색인 파일이 무엇을 요약하는지의 의존 표와 커밋 전 체크리스트는 [docs/SYNC_MAP.md](docs/SYNC_MAP.md)에 있다. 문서를 옮기거나 새 스터디·전략을 추가한 뒤에는 다음으로 링크·색인 정합을 검사한다(깨진 내부 링크·색인 미등재를 기계적으로 잡음, `@committer`가 문서 커밋 전 자동 실행):

```bash
python scripts/check_docs.py   # exit 0 = 통과, 1 = 드리프트
```

파일을 찾을 때는 색인 [docs/FILE_INDEX.md](docs/FILE_INDEX.md)(추적 파일 전부, 한 줄 설명)와 `_private/FILE_INDEX.md`를 본다.
`py scripts/file_index.py`가 트리와 맞춰 다시 쓰고 Stop 훅 `file-index-gate.ps1`이 턴마다 돌린다 — 새 파일은 `(설명 필요)`로
들어오니 그 줄만 채우면 되고, 커밋은 `docs-gate.ps1`이 스테이징된 추가·삭제와 색인을 대조해 막는다.

소스가 바뀌어 낡은 문서는 기억으로 찾지 않는다(D-075). 정본 `docs/sync_map.toml`의 규칙과 문서 안 도장
`<!-- sync: 경로@해시 -->`를 `scripts/sync_impact.py`가 대조해, 턴이 끝날 때 Stop 훅 `sync-gate.ps1`이 낡은 gen 블록은 치환하고
낡은 도장·힌트가 있으면 턴을 되돌린다 — 그 자리에서 문단을 고치고 `py scripts/sync_impact.py --restamp <문서>`로 도장을 갱신한다.
커밋 직전 `docs-gate.ps1`이 같은 검사로 막는다. 새 요약 문단을 쓸 때는 순서대로 묻는다: **생성할 수 있나(`<!-- gen: -->`) →
없으면 도장을 찍나 → 둘 다 아니면 링크만 하나.** 동기화 대상은 늘리지 않는 쪽이 맞다.

MFC 단말(`Quant/tools/ops_terminal/`)을 고쳤으면 [docs/guides/MFC_TERMINAL.md](docs/guides/MFC_TERMINAL.md)를
같은 커밋에서 갱신한다(8절 체크리스트). 실행 인자·산출물 경로·접속 방법이 바뀌면 `_private/LINKS.md` 운영단말 행도 고친다.

### 파일 지칭 규약 (전체 경로)

`README.md`·`config.json`·`main.cpp`처럼 저장소에 같은 이름이 여럿인 파일이 많다. 파일을 지칭할 때는
**저장소 루트 기준 전체 경로**를 쓴다. 대화 응답·문서·커밋 메시지 전부 해당한다.

- 쓴다: `research/studies/12_base_breakout/README.md`, `Quant/config/universe_scan.json`
- 쓰지 않는다: "README", "config 파일", "그 스터디 리드미"

산출물(csv·parquet·로그)도 같다. 폴더만 말하고 파일명을 생략하지 않는다.

### 링크 허브 자동 갱신

아티팩트를 새로 발행·재발행했거나, 복붙용 PowerShell 가이드(실행 절차·예약작업 시각)를 바꿨으면
`_private/LINKS.md`를 **말하지 않아도** 같이 고친다. 링크와 절차가 흩어지면 다음에 찾는 비용이 커진다.
`_private/`는 gitignore라 커밋 대상이 아니다. 저장소에 남기는 자동화 목록은 [docs/AUTOMATION.md](docs/AUTOMATION.md)가 소유한다.

### 문서 문체 규약 (AI 문체 회피)

담백·겸손하게 쓴다. 수치·표·코드·링크·다이어그램은 바꾸지 않는다. 적용 범위는 설계 문서만이 아니라
매매일지·사후검토·백테스트 일지·스터디 리포트·`research/dashboard/reviews.json`의 `*_html`·`DAILY_LOG.md`·
`docs/` 산문과 커밋 메시지 전부다. 문서를 쓰거나 고치기 전에 정본을 읽는다: [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md)
(과장·설교조 회피, 금지 표현과 대체어 표, 지표 약어 병기). `@committer` (d-2) 문체 스캔이 이 정본을 게이트로 쓴다.

적용 범위에는 코드 주석과 로그 문구도 들어간다 — 화면과 로그에 그대로 나오기 때문이다. 게이트는 두 지점이다: 쓰는 순간 `.claude/hooks/lexicon-gate.ps1`(Write·Edit 본문 검사, 차단), 커밋 직전 `@committer` (d-2) 문체 스캔.

```bash
python scripts/check_plain_language.py          # 검출
python scripts/check_plain_language.py --fix    # 치환(뒤 조사까지 맞춤)
```

### 코드 작업 규약 (주석·중괄호·커밋 분리)

코드를 고치거나 주석을 쓰기 전에 정본을 읽는다: [docs/guides/MAINTENANCE_AUTOMATION.md](docs/guides/MAINTENANCE_AUTOMATION.md) 4절.
주석은 위치마다 담을 것이 정해져 있다.

| 위치 | 쓸 것 | 쓰지 않을 것 |
|---|---|---|
| 파일 머리 | 목적 한 줄, 스레드 소유권, 관련 D-NNN | 항목 목록·개수 — 정본 위치를 가리킨다 |
| 함수 위 | 왜 따로 있는지, 호출 제약, 실패 시 동작 | 절차 서술 |
| 멤버 옆 | 단위와 불변식 | 경위(D-NNN으로 보낸다) |
| 블록 안 | 함정과 비자명한 결정 | 세 줄 넘는 태그 없는 설명 |

태그는 다섯 개다: `// [inv]` 불변식 · `// [lock-order]` 락 순서·memory_order 근거 ·
`// [wire]` 외부 프로토콜 필드·에러코드 · `// [why D-NNN]` 결정 참조 · `// [formula]` 수식·임계값 유도.

주석을 지울 때는 세 단계를 지킨다.

1. 지우기 전에 그 주장이 지금 코드와 맞는지 확인한다. 틀린 주석을 D-NNN으로 옮기면 오류가 정본이 된다.
2. 삭제 줄의 숫자·식별자·ID가 각각 어디에 남는지 목록으로 보고한다. 남을 곳이 없으면 지우지 않는다.
3. 코드 줄 diff는 0이어야 하고, 주석 정리 커밋은 기능 수정 커밋과 분리한다.

중괄호는 Allman이고 한 줄 본문에도 붙인다(`.clang-format`의 `InsertBraces`). `}` 뒤와 제어문 앞에는 빈 줄을 하나 둔다.
정리는 손으로 하지 말고 스크립트로 한다.

```bash
py scripts/brace_style.py                            # 중괄호·빈 줄 정리(인자 없으면 include/src/tests 전체)
py scripts/check_code_conventions.py                 # 스테이징 변경의 중괄호·D-NNN·태그 검사
py scripts/check_code_conventions.py --comment-only  # 주석 전용 커밋인지 검증(코드 줄 0)
```

주석 밀도는 게이트로 걸지 않는다. 파일별 밀도와 태그 없는 4줄 이상 블록은
`py scripts/maintain.py --weekly`가 `docs/reports/MAINTENANCE_WEEKLY.md`에 표로 남긴다.

## 플랫폼 참고사항

- Windows 빌드 플래그: `/utf-8`, `-D_WIN32_WINNT=0x0A00`(Windows 10+), `-D_CRT_SECURE_NO_WARNINGS`. FEED 화면 출력에는 ANSI 이스케이프 시퀀스와 `SetConsoleOutputCP(CP_UTF8)`를 사용합니다.
- Linux 디버그 빌드는 AddressSanitizer(`-fsanitize=address`)를 활성화합니다.
- HTTP는 `#ifdef _WIN32` 가드(`Quant/src/api/KisTransport.cpp`), WebSocket 소켓은 파일 단위(`WsSocketWin.cpp`/`WsSocketPosix.cpp`, CMake `if(WIN32)`)로 플랫폼 코드를 분리합니다. 네트워크 기능 추가 시 이 패턴을 유지하세요.

## 장중 운영 — 판단이 서면 실행한다

**장중이라도 재빌드·재기동은 허가돼 있다.** 결함을 찾았고 수정이 명확하면 물어보지 말고 진행한다.
매번 "재시작해도 될까요"를 되묻는 것이 더 큰 손해다(그 사이 체결이 원장에서 새는 것을 이미 겪었다).

| 상황 | 행동 |
|---|---|
| 원장·포지션 정합을 깨는 버그 발견 | 즉시 수정 → 테스트 빌드·실행 → `quant_trader` 재빌드 → 재기동 |
| `LNK1104 quant_trader.exe` (링크 실패) | 실행 중 프로세스가 exe를 잠근 것. 프로세스를 내리고 링크·재기동 |
| 재기동 | 반드시 **repo 루트**에서 원래 인자 그대로. cwd가 다르면 유니버스가 붕괴한다 |
| 재기동 직후 확인 | 잔고 재시드 수량·평단, `OrderRouter (FEP) 초기화 완료`, 체결통보 매칭 1건 |

재기동은 미체결 예약주문에 대한 라우터 기억(`history_`)을 지운다. 그 주문이 나중에 체결되면
ODNO 미매핑 체결로 들어오는데, 지금은 미연결 체결 경로가 원장·포지션에 반영한다(`OrderRouter::on_fill`).
잔고 재시드가 실제 보유수량을 다시 읽으므로 재기동 자체가 정합을 복구하는 방향이다.

예외 — 이건 그대로 물어본다: config의 리스크 한도·계좌 전환(모의↔실계좌), 보유분 강제청산,
git 커밋·푸시(`@committer` 승인 게이트).

## 다중 세션 — 세션당 git worktree

세션 여럿이 같은 작업 트리를 만지면 한쪽의 미완성 편집이 다른 쪽 빌드·테스트·커밋에 섞인다(09-11 실측:
동시 세션 3개가 `Quant/src/core/Engine.cpp`·`docs/DECISIONS.md`를 같이 건드려 diff 소유가 불분명해짐).
규칙은 하나다 — **코드를 바꾸는 세션은 자기 worktree에서 일한다.**

```bash
git worktree add ../Quant-wt-<주제> -b wt/<주제>      # 세션 시작 시 1회
git worktree list                                      # 누가 어디를 잡고 있는지
git worktree remove ../Quant-wt-<주제>                 # 머지 뒤 정리
```

- **메인 트리(`Quant/`)는 트레이더 배포 세션 하나만** 쓴다. `Quant/build_win/quant_trader.exe` 교체·감시견 재기동·
  `Quant/config/*.json` 수정은 이 세션만 한다. 다른 세션은 worktree에서 빌드해 ctest까지만 돌리고, exe 교체는 메인 세션에
  넘긴다(교체 절차는 메모리 `project_trader_watchdog_owner`).
- 문서만 고치는 세션은 메인 트리도 가능하되, 같은 파일을 두 세션이 열지 않는다(`git status --porcelain`으로 먼저 본다).
- 예약 작업(`_private/_cron/*_task.md`)은 메인 트리에서 돌고 `research/`·`_private/`만 쓴다. 코드 세션은 그 시각에
  `research/STRATEGY_LAB.md`를 건드리지 않는다.
- worktree는 `Quant/build_win/`을 공유하지 않는다 — 빌드 산출물은 worktree마다 새로 만든다(`$env:TEMP=C:uild_tmp` 회피는 동일).

### 세션끼리 순서·충돌을 알아서 정리한다 (상시)

코드 세션이 둘 이상이면 사용자에게 묻지 않고 세션끼리 순서를 정하고 충돌을 피한다. 작업은 한 단계로 끝나지 않고 이어지므로
이 절차도 상시다. 상태는 현황판 `_private/SESSION_CLAIMS.md`(gitignore, 메인 트리)에 두고, 통보는 `ListAgents`로 이름을 확인해
지목해서 보낸다(브로드캐스트 금지).

1. 세션 시작·새 단계 시작 때 현황판을 읽고 자기 줄(세션 이름·브랜치·D-NNN·파일 목록)을 적는다. 없으면 만든다. 남의 줄은 고치지 않는다.
2. 머지 큐 순서대로만 main에 넣는다. `git rebase main` → 전체 ctest → `git merge --ff-only`를 한 세션씩. "머지 시작"·"머지 완료 <sha>"를
   나머지 코드 세션에 보낸다. 순서를 바꾸려면 앞뒤 세션에 먼저 말한다. 푸시는 사용자가 말할 때만.
3. 남이 잡은 파일을 만져야 하면 그 세션에 먼저 묻는다. 공용 파일(`Quant/src/core/Engine.cpp`·`Quant/include/core/Engine.h`·
   `CLAUDE.md`·`docs/DECISIONS.md`·`Quant/CMakeLists.txt`)은 줄 단위 최소 편집 — `docs/DECISIONS.md`는 꼬리에 자기 절만,
   `CLAUDE.md`는 자기 줄만 고치고 행 번호를 알린다.
4. `py scripts/brace_style.py`는 인자 없이 돌리지 않는다(전체가 바뀌어 남의 diff에 섞인다). 자기 파일만 지정한다.
5. D-NNN은 현황판에 먼저 적고 쓴다.
6. 한 단계가 머지되면 다음 단계를 큐 끝에 붙이고 이어간다. 사용자 승인은 커밋(`@committer` 게이트)만 받는다.

### 교통정리 — 하루 끝에 한 세션이 취합한다 (주기)

세션이 여럿이면 완료 행·인계 파일·머지된 브랜치·주인 없는 worktree가 쌓이고, 어느 세션도 남의 것을 치우지 않으니 아무도 치우지 않는다
(09-13 실측: 머지 큐 22행 중 완료 20, 인계 파일 6개, 09-11 detached worktree 하나가 이틀 남음). 그래서 **교통정리는 코드 작업과
별개의 역할**이고, 하루 끝(또는 머지 큐 완료 행이 8개를 넘으면) 세션 하나가 `/triage`로 맡는다. 판정은 `scripts/session_triage.py`가
하고(main 미푸시·worktree 앞뒤·머지된 브랜치·현황판 완료/진행·죽은 세션·인계 파일 나이·배포 exe 뒤처짐), 절차는
`.claude/commands/triage.md`. 교통정리 세션만 남의 완료 행을 `_private/archive/`로 옮길 수 있다 — 옮기기 전에 진행 중인 세션에
"현황판 동결"을 지목해서 알리고, 끝나면 "압축 완료"를 보낸다. 되돌릴 수 있는 것(머지된 브랜치 삭제·인계 파일 보관·주인 없는 트리 패치 보관)은
스크립트가 하고, 되돌리기 어려운 것(worktree 제거·푸시·exe 교체·주인 없는 브랜치 처분)은 보고서에 적어 사용자에게 넘긴다.
하루 요약은 `DAILY_LOG.md` 머리에 D-NNN별로 붙이고, 세션 이름이 든 상세는 `_private/TRIAGE_<날짜>.md`에만 둔다.

## 토큰 이코노미 (매 작업 적용)

**원칙: 같은 결과가 나온다면 최소 토큰으로.** 작업을 시작하기 전에 해당 유형의 체크 항목을 적용한다.

| 작업 유형 | 시작 전 적용 |
|---|---|
| **파일 편집** | Edit/Write **직후 재-Read 금지**(하네스가 파일 상태 추적, 실패 시 에러). 이미 읽은 파일 재조회 금지. 같은 파일을 세 번째 읽게 되면 그 자리에서 필요한 범위를 넓혀 한 번에 읽는다. 단 Edit이 아닌 경로(빌드·생성기 산출물, `sed`/스크립트 편집, git 체크아웃·rebase, 서브에이전트·사용자 편집)로 바뀐 파일은 상태 추적이 안 되므로 확인한다 — 이때도 전체를 다시 읽지 말고 `git diff -U2 <파일>`·`grep -n`으로 바뀐 줄만 본다 |
| **코드·파일 탐색** | 여러 파일/디렉터리를 훑어야 하면 `Explore`/서브에이전트 위임 → **결론만** 수신(파일 덤프를 메인 컨텍스트에 쌓지 않음). 파일·심볼·값이 이미 특정된 단일 사실은 직접 조회. 심볼 위치는 Grep으로 먼저 특정하고 그 범위만 Read — 대형 파일(Engine.cpp·main.cpp·dashboard_server.py급)은 통째로 읽지 말고 `offset`/`limit` 지정 |
| **명령 실행** | git은 `--porcelain`/`-s`, 로그·grep은 `head`/`tail`·범위 제한. 큰 diff·파일·트리 통째 덤프 금지(필요한 줄만) |
| **다중 조회** | 서로 독립인 조회는 **한 메시지에 병렬 tool 호출**로 묶어 왕복 최소화 |
| **서브에이전트 위임** | 예상 실패·예외를 **첫 호출에 포함**해 재질의 왕복을 줄인다(예: 원격 main 세탁 갈라짐 → `git rebase --onto origin/main <parent> HEAD`) |
| **백그라운드 작업** | 폴링·sleep 루프 금지 — 완료 알림으로 재호출됨 |
| **응답 작성** | 결론부터, 짧게. 이미 내린 결정 재설명·안 할 옵션 나열·중복 요약 금지 |
| **검증** | 바뀐 범위만 재검증. 같은 확인 두 번 금지 |
| **세션 운영** | 작업 단위가 바뀌면 세션을 분리한다(`/clear`). 초반에 쌓인 토큰은 남은 턴 수만큼 재전송되므로, 긴 단일 세션이 가장 큰 낭비 요인이다(68세션 실측: 세션당 평균 216턴·요청당 약 102K 토큰) |

> 상세·근거는 개인 메모리 `feedback_token_economy`. 이 표는 프로젝트 개발 시 매 작업의 사전 체크리스트로 참조한다.
