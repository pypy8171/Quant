# CLAUDE.md

이 파일은 이 저장소에서 작업할 때 Claude Code(claude.ai/code)에게 제공하는 가이드다. 매 턴 지켜야 하는 규칙만 적고,
절차·표·목록은 정본 문서로 링크한다(매 턴 재전송되는 토큰이라 길이가 곧 비용이다).

## 빌드·테스트

```bash
cmake --preset x64-release && cmake --build out/build/x64-release     # Windows(VS2022/Ninja). x64-debug도 같다
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant && cmake --build Quant/build   # Linux(libcurl4-openssl-dev 필요)
ctest --preset x64-release          # 단위 테스트 34개. 수동 Ninja 레이아웃은 ctest --test-dir Quant/build_win
./Quant/build_win/quant_trader Quant/config/config.json               # 반드시 repo 루트에서
```

테스트 타깃 목록·이름 풀이·TSAN은 [docs/guides/PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md) "단위 테스트".
헤드리스 MSVC 빌드가 LNK1104로 실패하면 메모리 `build_win_temp_workaround`.

## 실행 모드

`Quant/config/config.json`의 `"mode"`: **`FEED`**는 KIS WebSocket 실시간 시세만 표시(주문 없음, 연결·인증 검증용,
`"futures"` 배열을 주면 선물도 구독), **`TRADE`**는 5-스레드 엔진이 장 중(평일 09:00–15:30 KST) 실제 주문을 낸다.
config에는 **실계좌 인증 정보**가 있다. 모의투자는 `"is_paper": true`.

## 설계 목표와 원칙 (D-071)

목표는 **전 시장 실시간 피드(2,500+종목·초당 수십만 건)를 받을 수 있는 구조**다. KIS 41종목은 그 1×1 특수 케이스일 뿐이고
현재 구조 유지는 목표가 아니다. 설계·리뷰·리팩터 판단 기준(단계와 버린 대안은 [docs/DECISIONS.md](docs/DECISIONS.md) D-071):

1. 한 소켓은 한 스레드가 처리한다(N:1). 스레드 수는 코어·부하로 정한다.
2. 순서 보장 단위는 종목이다. 종목 해시로 샤딩한다.
3. 수신 스레드는 얇게 — 읽기·최소 디코드·push까지.
4. 리스크·주문은 단일 시퀀서. `OrderGate`·원장은 샤딩하지 않는다.
5. 큐는 생산자 수로 고른다 — 하나면 `RingBuffer`, 여럿이면 `MpscQueue`. N×M SPSC 행렬을 MPSC보다 먼저 검토한다.
6. hot path에 문자열 없음 — 종목은 기동 시 정수 id, 틱·호가·디스패치는 id 배열 인덱스.
7. 측정 없이 손대지 않는다.
8. 틱은 캡처한다(raw append-only, 리플레이 백테스트 입력).

## 아키텍처

요약은 [docs/ENGINE_ARCHITECTURE.md](docs/ENGINE_ARCHITECTURE.md), 읽는 순서는 [docs/CODE_FLOW.md](docs/CODE_FLOW.md).
전략 추가는 `StrategyBase` 상속 → `on_start`에서 `symbol_of()`로 id를 받아 `trade.symbol_id`와 정수 비교 → `main.cpp`의
`engine.add_strategy(...)` 등록. 플랫폼 코드는 HTTP `#ifdef _WIN32`(`Quant/src/api/KisTransport.cpp`), WebSocket은
파일 단위(`WsSocketWin.cpp`/`WsSocketPosix.cpp`) — 네트워크 기능 추가 시 이 패턴 유지. Windows 빌드 플래그·콘솔 UTF-8은
[docs/guides/PROJECT_GUIDE.md](docs/guides/PROJECT_GUIDE.md).

## 작업 규약

### 문서 동기화

색인·요약·도장이 트리와 어긋나는 것을 훅이 막는다 — 정본 [docs/SYNC_MAP.md](docs/SYNC_MAP.md), 규칙 `docs/sync_map.toml`.
파일을 찾을 때는 [docs/FILE_INDEX.md](docs/FILE_INDEX.md)·`_private/FILE_INDEX.md`(새 파일은 `(설명 필요)` 줄만 채운다).
낡은 문서는 기억으로 찾지 않는다(D-075): Stop 훅 `sync-gate.ps1`이 낡은 도장을 잡으면 그 자리에서 문단을 고치고
`py scripts/sync_impact.py --restamp <문서>`. 새 요약 문단은 **gen 블록 → 도장 → 링크만** 순으로 고르고, 동기화 대상은 늘리지 않는다.
검사는 `python scripts/check_docs.py`(exit 1 = 드리프트). MFC 단말을 고쳤으면 [docs/guides/MFC_TERMINAL.md](docs/guides/MFC_TERMINAL.md)를
같은 커밋에서, 실행 인자·접속 방법이 바뀌면 `_private/LINKS.md`도.

### 커밋 절차

메인 세션이 직접 한다(`@committer`는 히스토리 세탁·force-push·헝크 분할만). ① 스테이징 후
`py scripts/commit_gate.py --msg-file <메시지 파일>`(`[차단]`은 고치기 전 커밋 금지, `[확인]`은 판정을 적는다) → ② 응답에 커밋명과
파일 목록을 보여주고 승인("커밋" 한마디가 이 단계 시작) → ③ `git commit`. 푸시는 사용자가 말할 때만. 스테이징을 바꿨으면 게이트 재실행.
커밋 제목은 `type(범위): 한국어 제목`, 쉬운 말로(정본 [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md)).

### 파일 지칭 규약

같은 이름 파일이 많으니 **저장소 루트 기준 전체 경로**로 지칭한다 — 응답·문서·커밋 메시지·산출물(csv·로그) 전부.
`README.md`·`config.json`·"그 리드미"처럼 이름만 쓰지 않는다.

### 링크 허브

아티팩트 발행·재발행, 복붙용 PowerShell 가이드 변경 시 `_private/LINKS.md`를 말하지 않아도 같이 고친다(gitignore).
저장소에 남는 자동화 목록은 [docs/AUTOMATION.md](docs/AUTOMATION.md).

### 문서 문체 규약

담백·겸손하게. 정본 [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md)(과장·설교조 회피, 금지 표현 표). 적용 범위는 설계 문서·매매일지·
스터디 리포트·대시보드 `*_html`·커밋 메시지·**코드 주석·로그 문구**까지. 게이트는 쓰는 순간 `lexicon-gate.ps1`, 커밋 직전
`scripts/commit_gate.py`. 검출·치환은 `python scripts/check_plain_language.py [--fix]`.

### 코드 작업 규약

주석 위치별 내용·태그 5개(`[inv]`·`[lock-order]`·`[wire]`·`[why D-NNN]`·`[formula]`)·주석 삭제 3단계의 정본은
[docs/guides/MAINTENANCE_AUTOMATION.md](docs/guides/MAINTENANCE_AUTOMATION.md) 4절 — 코드를 고치기 전에 읽는다.
중괄호는 Allman, 한 줄 본문에도 붙이고 `}` 뒤·제어문 앞에 빈 줄 하나. C스타일 캐스트(`(int)x`)는 금지 — 값은
`static_cast<T>(x)`, 포인터는 `reinterpret_cast<T>(x)`, `(void)x;`만 예외. 안 해도 되는 복사는 만들지 않는다 — 조회 결과는
`const&`나 `std::string_view`(수명은 `[inv]`), json 노드는 `value(k, json::array())` 대신 `find()` 참조, range-for는
`const auto&`, 값 전달은 `std::move`로 받는 sink만. **이름에 약어를 쓰지 않는다** — `qty`·`cfg`·`it`·`i` 대신 `quantity`·`config`·
`iterator`·`index`. 읽는 사람이 용어를 먼저 익히는 것이 우선이라 그렇다(예외는 전문 필드·지표명·단위 접미사·표준 멤버, 정본 4절
"이름 표기"). 정리는 `py scripts/brace_style.py <자기 파일만>`,
검사는 `py scripts/check_code_conventions.py [--comment-only]`.

## 장중 운영 — 판단이 서면 실행한다

장중이라도 재빌드·재기동은 허가돼 있다. 결함을 찾았고 수정이 명확하면 묻지 말고 수정 → 테스트 → 재빌드 → 재기동
(되묻는 사이 체결이 원장에서 새는 것을 이미 겪었다). 재기동은 **repo 루트에서 원래 인자 그대로**, 트레이더는 감시견
`scripts/auto_trade_day.ps1` 소유(exe 교체 → Stop-Process → 감시견 대기, 손으로 띄우면 엔진 둘). LNK1104는 실행 중 프로세스가
exe를 잠근 것. 재기동 직후 잔고 재시드·`OrderRouter (FEP) 초기화 완료`·체결통보 매칭 1건 확인. 절차 정본 [docs/AUTOMATION.md](docs/AUTOMATION.md).
**예외 — 물어본다**: 리스크 한도·계좌 전환(모의↔실계좌), 보유분 강제청산, git 커밋·푸시.

## 다중 세션 — 세션당 git worktree

정본 [docs/guides/MULTI_SESSION.md](docs/guides/MULTI_SESSION.md). 코드를 바꾸는 세션은 자기 worktree
(`git worktree add ../Quant-wt-<주제> -b wt/<주제>`), 메인 트리 `Quant/`는 트레이더 배포 세션만. 세션 시작·새 단계마다
`_private/SESSION_CLAIMS.md`에 자기 줄을 적고 남의 줄은 안 건드린다. 남이 잡은 파일은 먼저 묻고, 공용 파일(`Engine.cpp/.h`·
`CLAUDE.md`·`docs/DECISIONS.md`·`Quant/CMakeLists.txt`)은 줄 단위 최소 편집. 머지는 큐 순서대로 `rebase main` → ctest → `--ff-only`,
통보는 `ListAgents`로 이름을 확인해 지목(브로드캐스트 금지). 교통정리(`/triage`)는 사용자가 시킬 때만.

## 토큰 이코노미 (매 작업 적용)

**같은 결과면 최소 토큰.** 상세는 메모리 `feedback_token_economy`.
편집 직후 재-Read 금지, 읽은 파일 재조회 금지, Edit 밖 경로로 바뀐 파일은 `git diff -U2`·`grep -n`으로 바뀐 줄만.
여러 파일 탐색은 서브에이전트에 위임해 결론만. 대형 파일은 Grep으로 위치를 잡고 `offset`/`limit`. git은 `--porcelain`/`-s`,
로그는 `head`/`tail`, 독립 조회는 병렬, 폴링·sleep 금지. 응답은 결론부터 짧게, 내린 결정 재설명·안 할 옵션 나열 금지.
작업 단위가 바뀌면 `/clear`.
**문맥 145K를 넘기면** 자동 압축을 기다리지 말고 `/handoff`로 인계 파일을 쓴 뒤 `/clear`를 권한다(압축은 무엇을 남길지 고를 수 없어 같은 일을 두 번 읽게 된다). Stop 훅이 그 시점을 알린다.
