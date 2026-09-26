# -*- coding: utf-8 -*-
"""트레이더 재기동이 제대로 끝났는지 엔진 로그로 판정한다.

프로세스가 떠 있는 것만으로는 성공이 아니다. 인증·잔고 조회·원장 시드에서 멈추거나, 스레드를 다 띄운 직후
죽는 경우가 있다(09-21 08:20 낡은 오브젝트로 기동 직후 죽기를 되풀이). 그래서 로그에 기동 단계 표지가
차례로 찍히는지를 본다.

  1. 재기동 직전에 엔진 로그 파일마다 끝 위치를 적어 둔다 — 그 뒤에 새로 쓰인 부분만 읽어서, 이전 기동의
     줄과 섞이지 않게 한다. 파일이 줄었으면(회전) 처음부터 읽는다.
  2. 새 부분에서 `퀀트 엔진 시작` 을 찾고, 역할별 필수 표지가 다 나오면 성공 후보다.
       both·order      : OrderRouter (FEP) 초기화 완료 → 모든 스레드 시작 완료
       strategy·feed   : 모든 스레드 시작 완료
     원장 시드(`원장 부트스트랩 완료`)는 config 가 끌 수 있어 필수로 두지 않고, 없으면 성공에 메모만 붙인다.
  3. 성공 후보가 된 뒤에도 settle 초 동안 기다려 그 사이 죽지 않았는지 본다.
  4. 실패 문구(비정상 종료·기동 중단·인증 실패·설정 로드 실패)가 나오면 실패, 제한 시간을 넘기면 판정 불가.

결과는 `_private/state/restart_verify.jsonl` 에 한 줄씩 쌓인다. check_runtime_health.py 의 "재기동 기동 판정"
행이 그날 줄을 읽어 판정한다.

사용:  py scripts/restart_verify.py --watch      지금 위치를 적고 다음 재기동을 기다려 판정한다(손으로 내렸을 때)
       deploy_trader.py 는 snapshot() → 프로세스 내림 → verify() 로 부른다.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _logdir import LIVE_LOG_NAMES, REPO, candidate_dirs, role_of  # noqa: E402

RESULT_PATH = REPO / "_private" / "state" / "restart_verify.jsonl"
START_MARK = "[Engine] ── 퀀트 엔진 시작"
READY_MARK = "[Engine] 모든 스레드 시작 완료"
REQUIRED_MARKS = {
    "both": ("[Engine] OrderRouter (FEP) 초기화 완료", READY_MARK),
    "order": ("[Engine] OrderRouter (FEP) 초기화 완료", READY_MARK),
    "strategy": (READY_MARK,),
    # 시세 쪽은 주문도 원장도 안 든다 — WebSocket 소켓과 디코드만 맡는다. 스레드 줄 하나로 본다.
    "feed": (READY_MARK,),
}
LEDGER_MARK = "[Engine] 원장 부트스트랩 완료"
FILL_SESSION_MARK = "[Engine] 체결통보 세션:"
FAIL_MARKS = (
    "[Main] 비정상 종료",
    "[Main] 설정 로드 실패",
    "[Main] 실행 인자 오류",
    "[Engine] KIS 인증 실패",
    "기동 중단",
    "[Engine] 원장 부트스트랩 예외",
)

SUCCESS, FAIL, UNKNOWN = "성공", "실패", "판정불가"

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


@dataclass
class LogVerdict:
    path: str
    role: str
    result: str = UNKNOWN
    reached: list[str] = field(default_factory=list)
    detail: str = ""


def log_files() -> list[Path]:
    """엔진이 쓸 수 있는 실행 로그 전부 — 계좌별 폴더(logs_<instance>)까지. 아직 없는 파일도 넣는다(새로 생길 수 있다)."""
    directories = list(candidate_dirs())
    build_directory = REPO / "Quant" / "build_win"
    if build_directory.is_dir():
        directories += sorted(path for path in build_directory.glob("logs_*") if path.is_dir())

    files, seen = [], set()
    for directory in directories:
        for name in LIVE_LOG_NAMES:
            path = directory / name
            key = str(path.resolve()) if path.parent.exists() else str(path)
            if key not in seen:
                seen.add(key)
                files.append(path)

    return files


def snapshot() -> dict[str, int]:
    """파일마다 지금 끝 위치. 없는 파일은 0 — 새로 생기면 처음부터 읽는다."""
    offsets = {}
    for path in log_files():
        try:
            offsets[str(path)] = path.stat().st_size
        except OSError:
            offsets[str(path)] = 0

    return offsets


def _new_text(path: Path, offset: int) -> str:
    try:
        size = path.stat().st_size
    except OSError:
        return ""

    start = offset if size >= offset else 0   # 줄었으면 회전된 것 — 처음부터
    with open(path, "rb") as handle:
        handle.seek(start)
        return handle.read().decode("utf-8", "replace")


def _judge_text(path: Path, text: str) -> LogVerdict | None:
    """새로 쓰인 부분 하나를 판정한다. 엔진 시작 줄이 없으면 None(이 파일에서는 재기동이 없었다)."""
    begin = text.rfind(START_MARK)
    if begin < 0:
        return None

    role = role_of(path)
    body = text[begin:]
    verdict = LogVerdict(path=str(path.relative_to(REPO)) if path.is_relative_to(REPO) else str(path), role=role)

    for line in body.splitlines():
        failed = next((mark for mark in FAIL_MARKS if mark in line), None)
        if failed:
            verdict.result = FAIL
            detail = line[24:]
            if detail.startswith("{"):  # 새 형식의 {스레드} 칸은 건너뛴다
                detail = detail.partition("} ")[2]
            verdict.detail = detail.strip()[:200]
            return verdict

    required = REQUIRED_MARKS.get(role, REQUIRED_MARKS["both"])
    verdict.reached = [mark.split("] ", 1)[-1] for mark in required if mark in body]
    if len(verdict.reached) == len(required):
        verdict.result = SUCCESS
        notes = []
        if role in ("both", "order") and LEDGER_MARK not in body:
            notes.append("원장 시드 줄 없음")

        fill_line = next((line for line in body.splitlines() if FILL_SESSION_MARK in line), "")
        if fill_line:
            notes.append(fill_line.split(FILL_SESSION_MARK, 1)[1].strip())

        verdict.detail = " · ".join(notes)

    return verdict


def verify(offsets: dict[str, int], expected: int, alive, timeout_seconds: float = 90.0,
           settle_seconds: float = 20.0) -> list[LogVerdict]:
    """expected 개의 로그가 새 기동을 찍을 때까지 기다려 판정한다.

    alive(): 새로 뜬 트레이더 수를 돌려주는 함수. 성공 후보가 된 뒤 settle 동안 이 수가 expected 밑으로
    내려가면 스레드를 띄운 직후 죽은 것으로 보고 실패로 바꾼다.
    """
    deadline = time.monotonic() + timeout_seconds
    verdicts: dict[str, LogVerdict] = {}

    while time.monotonic() < deadline:
        for name, offset in offsets.items():
            path = Path(name)
            judged = _judge_text(path, _new_text(path, offset))
            if judged is not None:
                verdicts[name] = judged

        finished = [verdict for verdict in verdicts.values() if verdict.result != UNKNOWN]
        if any(verdict.result == FAIL for verdict in finished):
            break

        if len(finished) >= expected:
            break

        time.sleep(1.0)

    ordered = list(verdicts.values())
    if len(ordered) < expected:
        ordered.append(LogVerdict(path="(로그 없음)", role="?",
                                  detail="{}초 안에 새 기동 줄이 {}개뿐(기대 {}개)".format(
                                      int(timeout_seconds), len(ordered), expected)))

    if all(verdict.result == SUCCESS for verdict in ordered):
        settle_deadline = time.monotonic() + settle_seconds
        while time.monotonic() < settle_deadline:
            if alive() < expected:
                for verdict in ordered:
                    verdict.result = FAIL
                    verdict.detail = "스레드를 다 띄운 뒤 {}초 안에 프로세스가 내려갔다".format(int(settle_seconds))
                break

            time.sleep(1.0)

    for verdict in ordered:
        if verdict.result == UNKNOWN and not verdict.detail:
            verdict.detail = "{}초 안에 못 끝냄 — 닿은 단계: {}".format(
                int(timeout_seconds), ", ".join(verdict.reached) or "엔진 시작만")

    return ordered


def overall(verdicts: list[LogVerdict]) -> str:
    results = {verdict.result for verdict in verdicts}
    if FAIL in results:
        return FAIL

    if results == {SUCCESS}:
        return SUCCESS

    return UNKNOWN


def record(verdicts: list[LogVerdict], who: str, reason: str) -> dict:
    """판정 한 건을 jsonl 에 쌓고 그 줄을 돌려준다."""
    row = {"time": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), "who": who, "reason": reason,
           "result": overall(verdicts), "logs": [asdict(verdict) for verdict in verdicts]}
    RESULT_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(RESULT_PATH, "a", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    return row


def summary_text(row: dict) -> str:
    lines = ["[재기동 판정] {} — {} ({})".format(row["result"], row["reason"], row["who"])]
    for log in row["logs"]:
        lines.append("  {} [{}] {}{}".format(log["path"], log["role"], log["result"],
                                            " — " + log["detail"] if log["detail"] else ""))

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="다음 트레이더 재기동을 기다려 기동 성공을 판정한다")
    parser.add_argument("--watch", action="store_true", help="지금 위치를 적고 다음 재기동을 기다린다")
    parser.add_argument("--expected", type=int, default=1, help="새로 뜰 트레이더 수(갈라 띄우면 3 — 주문·전략·시세)")
    parser.add_argument("--timeout", type=float, default=600.0, help="--watch 에서 기다릴 최대 초")
    parser.add_argument("--who", default="손 재기동")
    arguments = parser.parse_args()

    if not arguments.watch:
        parser.print_help()
        return 2

    offsets = snapshot()
    print("로그 {}개 위치를 적었다 — 재기동을 기다린다(최대 {:.0f}초)".format(len(offsets), arguments.timeout), flush=True)
    verdicts = verify(offsets, arguments.expected, alive=lambda: arguments.expected,
                      timeout_seconds=arguments.timeout, settle_seconds=0.0)
    row = record(verdicts, arguments.who, "--watch")
    print(summary_text(row))
    return 0 if row["result"] == SUCCESS else 1


if __name__ == "__main__":
    sys.exit(main())
