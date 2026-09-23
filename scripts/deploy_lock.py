# -*- coding: utf-8 -*-
"""트레이더 배포를 한 번에 하나만 돌게 하는 잠금.

세션 여럿이 각자 장중 배포를 하면 빌드·exe 옮기기·프로세스 내리기가 서로 겹친다. 한 세션이 exe 를 옆으로
옮긴 사이 다른 세션이 링크하거나, 둘이 같은 트레이더를 연달아 내려 감시견의 "30분 안 세 번" 멈춤에 걸린다.

잠금은 운영체제의 파일 잠금이다 — Windows 는 msvcrt.locking, 그 밖은 fcntl.flock. 상태 값을 읽고 나서
쓰는 방식은 둘이 같은 순간에 "비어 있음"을 읽으면 둘 다 들어간다. 파일 잠금은 잡는 동작 하나가 곧 판정이라
그 틈이 없고, 잡은 프로세스가 죽으면 운영체제가 풀어 주므로 오래된 잠금을 치우는 규칙도 필요 없다.

뒤에 온 세션은 거절되지 않고 기다린다. 기다리는 동안 누가 잡고 있는지(누구·PID·시작 시각·하는 일)를
`_private/state/deploy.lock.owner.json` 에서 읽어 보여 준다.

사용:  py scripts/deploy_lock.py            지금 누가 잡고 있는지
       with DeployLock(who="quant-c9", purpose="배포", wait_seconds=600): ...
"""
from __future__ import annotations

import datetime as dt
import json
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STATE_DIR = REPO / "_private" / "state"
LOCK_PATH = STATE_DIR / "deploy.lock"
OWNER_PATH = STATE_DIR / "deploy.lock.owner.json"
POLL_SECONDS = 2.0

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


class LockTimeout(RuntimeError):
    """기다릴 시간 안에 잠금을 못 얻었다."""


def _try_lock(handle) -> bool:
    """잡으면 True, 남이 잡고 있으면 False. 기다리지 않는다."""
    if sys.platform == "win32":
        import msvcrt

        try:
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            return False

        return True

    import fcntl

    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False

    return True


def _unlock(handle) -> None:
    if sys.platform == "win32":
        import msvcrt

        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        return

    import fcntl

    fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def read_owner() -> dict | None:
    """지금 잡고 있는 쪽의 기록. 잠금이 풀려 있으면 None."""
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    with open(LOCK_PATH, "a+b") as handle:
        if _try_lock(handle):
            _unlock(handle)
            return None

    try:
        return json.loads(OWNER_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"who": "(기록 없음)", "pid": 0, "started": "", "purpose": ""}


def describe(owner: dict) -> str:
    return "{} (pid {}, {} 부터) — {}".format(owner.get("who", "?"), owner.get("pid", "?"),
                                          owner.get("started", "?"), owner.get("purpose", ""))


class DeployLock:
    """with 블록 동안 배포 잠금을 잡는다. wait_seconds 안에 못 잡으면 LockTimeout."""

    def __init__(self, who: str, purpose: str, wait_seconds: float = 600.0):
        self.who = who
        self.purpose = purpose
        self.wait_seconds = wait_seconds
        self._handle = None

    def __enter__(self) -> "DeployLock":
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        handle = open(LOCK_PATH, "a+b")
        deadline = time.monotonic() + self.wait_seconds
        announced = ""

        while not _try_lock(handle):
            if time.monotonic() >= deadline:
                handle.close()
                owner = read_owner()
                raise LockTimeout("배포 잠금을 {:.0f}초 안에 못 얻었다 — 잡은 쪽: {}".format(
                    self.wait_seconds, describe(owner) if owner else "(방금 풀림)"))

            owner = read_owner()
            line = describe(owner) if owner else ""
            if line and line != announced:
                print("[잠금] 다른 배포가 진행 중이라 기다린다 — " + line, flush=True)
                announced = line

            time.sleep(POLL_SECONDS)

        self._handle = handle
        owner = {"who": self.who, "pid": os.getpid(), "purpose": self.purpose,
                 "started": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
        OWNER_PATH.write_text(json.dumps(owner, ensure_ascii=False), encoding="utf-8")
        return self

    def __exit__(self, *exception_info) -> None:
        try:
            OWNER_PATH.unlink(missing_ok=True)
        except OSError:
            pass

        _unlock(self._handle)
        self._handle.close()
        self._handle = None


def main() -> int:
    owner = read_owner()
    print("배포 잠금: " + ("비어 있다" if owner is None else "잡혀 있다 — " + describe(owner)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
