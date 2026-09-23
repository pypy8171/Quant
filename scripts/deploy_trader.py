# -*- coding: utf-8 -*-
"""트레이더 장중 배포 한 번 — 잠금 → 빌드 → 교체 → 재기동 → 기동 판정 → 기록·알림.

손으로 하던 순서(relink.cmd 빌드 → exe 옆으로 옮기기 → taskkill → 감시견 재기동 기다리기 → 로그 열어 보기)를
한 명령으로 묶는다. 두 가지를 더한다.

  배포 잠금(scripts/deploy_lock.py)  세션 여럿이 동시에 배포해도 한 번에 하나만 돈다. 뒤에 온 세션은 기다린다.
  기동 판정(scripts/restart_verify.py) 감시견이 다시 띄운 트레이더가 로그에 기동 단계를 다 찍었는지 보고
                                      성공·실패·판정불가를 낸다. 결과는 _private/deploy_guard.log 와
                                      _private/state/restart_verify.jsonl 에 남고, 알림 수신처가 있으면 보낸다.

트레이더를 내리는 것은 감시견이 떠 있을 때만 한다 — 감시견 없이 내리면 다시 띄울 쪽이 없다. 내리기 전에
`_private/state/planned_restart_<pid>` 표지를 남겨, 감시견이 이 재기동을 "30분 안 세 번" 크래시 계산에 넣지 않게 한다.

사용:  py scripts/deploy_trader.py --who quant-c9                빌드하고 바뀌었으면 재기동·판정
       py scripts/deploy_trader.py --who quant-c9 --no-restart   빌드만(잠금은 잡는다)
       py scripts/deploy_trader.py --who quant-c9 --restart-only 빌드 없이 재기동·판정만
종료 코드: 0 성공(또는 재기동할 것 없음) · 1 빌드 실패 · 2 기동 실패 · 3 판정불가 · 4 잠금 대기 초과 · 5 감시견 없음
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import restart_verify  # noqa: E402
from deploy_lock import DeployLock, LockTimeout  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
BUILD_DIRECTORY = REPO / "Quant" / "build_win"
TARGET_EXE = BUILD_DIRECTORY / "quant_trader.exe"
GUARD_LOG = REPO / "_private" / "deploy_guard.log"
STATE_DIR = REPO / "_private" / "state"
RELAUNCH_WAIT_SECONDS = 60.0   # 감시견은 내려간 뒤 5초 쉬고 띄운다. 여유를 크게 둔다

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def say(message: str) -> None:
    print("[{}] {}".format(dt.datetime.now().strftime("%H:%M:%S"), message), flush=True)


def processes() -> list[dict]:
    """이 트리의 트레이더와 감시견. [{"pid", "name", "path", "command"}]"""
    script = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
              "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'quant_trader*' -or "
              "$_.CommandLine -like '*auto_trade_day*' } | "
              "Select-Object ProcessId, Name, ExecutablePath, CommandLine | ConvertTo-Json -Compress")
    done = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=60)
    if done.returncode != 0 or not done.stdout.strip():
        return []

    rows = json.loads(done.stdout)
    rows = rows if isinstance(rows, list) else [rows]
    return [{"pid": row["ProcessId"], "name": row["Name"] or "", "path": row["ExecutablePath"] or "",
             "command": row["CommandLine"] or ""} for row in rows]


def traders(rows: list[dict]) -> list[dict]:
    """이 트리 build_win 에서 뜬 트레이더. 옆으로 옮긴 quant_trader_old_*.exe 로 도는 것도 넣는다."""
    root = str(BUILD_DIRECTORY).lower()
    return [row for row in rows
            if row["name"].lower().startswith("quant_trader") and row["path"].lower().startswith(root)]


def watchdogs(rows: list[dict]) -> list[dict]:
    # 감시견은 PowerShell 이 auto_trade_day.ps1 을 파일로 돌리는 것뿐이다. 명령줄에 그 이름이 글자로만 들어 있는
    #  셸(이 조회 자신, 다른 세션의 bash)은 뺀다.
    return [row for row in rows if row["name"].lower() in ("powershell.exe", "pwsh.exe")
            and re.search(r"-File\s+\S*auto_trade_day\.ps1", row["command"], re.IGNORECASE)]


def guard_log(line: str) -> None:
    GUARD_LOG.parent.mkdir(parents=True, exist_ok=True)
    with open(GUARD_LOG, "a", encoding="utf-8", newline="\n") as handle:
        handle.write("{} {}\n".format(dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), line))


def notify(text: str) -> None:
    """알림 수신처(_private/notify.json)가 있으면 보낸다. 실패해도 배포는 끝난 것이라 삼킨다."""
    try:
        from notify_trades import build_notifiers
    except Exception:
        return

    for notifier in build_notifiers(quiet=True):
        kinds = getattr(notifier, "kinds", None)
        if kinds and "system" not in kinds:
            continue

        try:
            notifier.send(text)
        except Exception as exception:
            say("알림 실패({}): {}".format(notifier.name, exception))


def build() -> int:
    """scripts/build_trader.ps1 을 부른다. exe 가 잠겨 있으면(3) 옆으로 옮기고 한 번 더."""
    command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(REPO / "scripts" / "build_trader.ps1")]
    code = subprocess.run(command, cwd=REPO).returncode
    if code == 3:
        moved = BUILD_DIRECTORY / "quant_trader_old_{}.exe".format(dt.datetime.now().strftime("%H%M%S"))
        TARGET_EXE.rename(moved)
        say("돌고 있는 exe 를 {} 로 옮기고 다시 링크한다".format(moved.name))
        code = subprocess.run(command, cwd=REPO).returncode

        # 다시 링크가 깨졌으면 옮긴 exe 를 제자리로 — 그 사이 트레이더가 죽으면 감시견이 띄울 exe 가 없다.
        if code != 0 and not TARGET_EXE.exists():
            moved.rename(TARGET_EXE)
            say("링크 실패 — 옮겼던 exe 를 제자리로 돌렸다")

    return code


def restart_and_verify(who: str, reason: str) -> int:
    rows = processes()
    running = traders(rows)
    if not running:
        say("도는 트레이더가 없다 — 재기동할 것이 없다")
        return 0

    if not watchdogs(rows):
        say("트레이더는 도는데 감시견이 없다 — 내리면 다시 띄울 쪽이 없어 재기동하지 않는다")
        guard_log("배포 재기동 보류 — 감시견 없음 ({})".format(who))
        return 5

    old_pids = {row["pid"] for row in running}
    offsets = restart_verify.snapshot()
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    for pid in old_pids:
        (STATE_DIR / "planned_restart_{}".format(pid)).write_text(who, encoding="utf-8")

    say("트레이더 {}개를 내린다(pid {}) — 감시견이 새 exe 로 다시 띄운다".format(len(old_pids), ", ".join(map(str, sorted(old_pids)))))
    for pid in old_pids:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)

    def fresh() -> int:
        return len([row for row in traders(processes()) if row["pid"] not in old_pids])

    deadline = time.monotonic() + RELAUNCH_WAIT_SECONDS
    while fresh() < len(old_pids) and time.monotonic() < deadline:
        time.sleep(2.0)

    verdicts = restart_verify.verify(offsets, expected=len(old_pids), alive=fresh)
    row = restart_verify.record(verdicts, who, reason)
    text = restart_verify.summary_text(row)
    print(text, flush=True)
    guard_log("배포 재기동 {} — {} ({})".format(row["result"], reason, who))
    notify(text)

    return {restart_verify.SUCCESS: 0, restart_verify.FAIL: 2}.get(row["result"], 3)


def main() -> int:
    parser = argparse.ArgumentParser(description="트레이더 장중 배포 — 잠금·빌드·교체·재기동·기동 판정")
    parser.add_argument("--who", required=True, help="배포하는 세션 이름(잠금 소유자로 보인다)")
    parser.add_argument("--reason", default="장중 배포", help="판정 기록에 남길 한 줄")
    parser.add_argument("--no-restart", action="store_true", help="빌드만 한다")
    parser.add_argument("--restart-only", action="store_true", help="빌드 없이 재기동·판정만")
    parser.add_argument("--wait", type=float, default=600.0, help="배포 잠금을 기다릴 최대 초")
    arguments = parser.parse_args()

    try:
        with DeployLock(who=arguments.who, purpose=arguments.reason, wait_seconds=arguments.wait):
            subprocess.run([sys.executable, str(REPO / "scripts" / "deploy_guard.py")], cwd=REPO)

            if not arguments.restart_only:
                before = TARGET_EXE.stat().st_mtime if TARGET_EXE.exists() else 0.0
                code = build()
                if code != 0:
                    say("빌드가 끝나지 않았다(종료 코드 {}) — 재기동하지 않는다".format(code))
                    return 1

                if arguments.no_restart:
                    return 0

                # exe 가 그대로이고 도는 트레이더도 그 exe 로 돌고 있으면 바꿀 것이 없다.
                unchanged = TARGET_EXE.stat().st_mtime == before
                target = str(TARGET_EXE).lower()
                if unchanged and all(row["path"].lower() == target for row in traders(processes())):
                    say("exe 가 그대로이고 트레이더도 그 exe 로 돈다 — 재기동하지 않는다")
                    return 0

            return restart_and_verify(arguments.who, arguments.reason)
    except LockTimeout as exception:
        say(str(exception))
        return 4


if __name__ == "__main__":
    sys.exit(main())
