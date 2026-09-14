# 다중 세션 운영 — worktree·현황판·머지 큐·교통정리

세션 여럿이 같은 저장소에서 코드를 바꿀 때의 절차 정본. `CLAUDE.md`의 "다중 세션" 절은 이 문서의 요약이고,
판정 스크립트는 `scripts/session_triage.py`, 교통정리 절차는 `.claude/commands/triage.md`(로컬), 세션 현황판은
`scripts/session_board.py`(웹 `/sessions`), 인계 절차는 `.claude/commands/handoff.md`(로컬)다.

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
6. 한 단계가 머지되면 다음 단계를 큐 끝에 붙이고 이어간다. 사용자 승인은 커밋(커밋명 승인 게이트)만 받는다.

### 교통정리 — 하루 끝에 한 세션이 취합한다 (주기)

세션이 여럿이면 완료 행·인계 파일·머지된 브랜치·주인 없는 worktree가 쌓이고, 어느 세션도 남의 것을 치우지 않으니 아무도 치우지 않는다
(09-13 실측: 머지 큐 22행 중 완료 20, 인계 파일 6개, 09-11 detached worktree 하나가 이틀 남음). 그래서 **교통정리는 코드 작업과
별개의 역할**이고, 하루 끝(또는 머지 큐 완료 행이 8개를 넘으면) 세션 하나가 `/triage`로 맡는다. 판정은 `scripts/session_triage.py`가
하고(main 미푸시·worktree 앞뒤·머지된 브랜치·현황판 완료/진행·죽은 세션·인계 파일 나이·배포 exe 뒤처짐), 절차는
`.claude/commands/triage.md`. 교통정리 세션만 남의 완료 행을 `_private/archive/`로 옮길 수 있다 — 옮기기 전에 진행 중인 세션에
"현황판 동결"을 지목해서 알리고, 끝나면 "압축 완료"를 보낸다. 되돌릴 수 있는 것(머지된 브랜치 삭제·인계 파일 보관·주인 없는 트리 패치 보관)은
스크립트가 하고, 되돌리기 어려운 것(worktree 제거·푸시·exe 교체·주인 없는 브랜치 처분)은 보고서에 적어 사용자에게 넘긴다.
하루 요약은 `DAILY_LOG.md` 머리에 D-NNN별로 붙이고, 세션 이름이 든 상세는 `_private/TRIAGE_<날짜>.md`에만 둔다.
