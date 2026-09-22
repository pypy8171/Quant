# -*- coding: utf-8 -*-
"""장중 트레이더 exe 교체를 막는다(D-101 결정 1).

재기동 42회 중 29회가 세션이 리팩터·이름·문서를 반영하려고 트레이더를 죽인 것이었고,
재기동마다 보유분이 청산 래퍼로 넘어가 −198만/주가 나갔다. 그래서 매매 창(평일 09:00~15:30,
실계좌면 20:00까지) 안의 exe 교체는 A등급 결함(체결 누락·이중 발주·원장 불일치·주문 불능)일 때만 한다.

막을지는 **지금 도는 프로세스**를 보고 정한다. 트레이더도 감시견도 없으면 바꿔도 깨질 매매가 없어 그냥 통과하고,
돌고 있으면 그 프로세스가 실제로 연 config의 `is_paper`로 창 끝을 잡는다. 2026-09-22에 모의계좌로 돌던 날
고정 경로 `Quant/config/config.json`(실계좌)만 읽어 창 끝을 20:00으로 잡는 바람에, 15:30에 이미 끝난
매매의 마감 뒤 배포를 네 시간 반 막았다.

사용:  py scripts/deploy_guard.py            (막히면 exit 1, 아니면 0)
       py scripts/deploy_guard.py --hotfix-a "<한 줄 사유>"   A등급 결함으로 예외 통과(사유는 로그에 남는다)
       py scripts/deploy_guard.py --config <경로>   도는 프로세스를 보지 않고 이 config로만 판정한다
C:/build_tmp/relink.cmd가 exe를 다시 만들기 전에 이 스크립트를 먼저 부른다.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LOG = REPO / "_private" / "deploy_guard.log"
DEFAULT_CONFIG = REPO / "Quant" / "config" / "config.json"

# 교체하려는 exe를 붙잡고 있는 두 종류. 감시견은 지금 트레이더가 죽어 있어도 곧 다시 띄운다.
TRADER_PATTERN = re.compile(r"quant_trader(\.exe)?(?:[\s\"']|$)", re.IGNORECASE)
WATCHDOG_PATTERN = re.compile(r"auto_trade_day\.(?:ps1|sh)", re.IGNORECASE)
CONFIG_TOKEN_PATTERN = re.compile(r"[^\s\"']*config[^\s\"']*\.json", re.IGNORECASE)

PAPER_WINDOW_END = dt.time(15, 30)   # 모의는 정규장까지
LIVE_WINDOW_END = dt.time(20, 0)     # 실계좌는 애프터마켓까지(D-097)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def command_lines() -> list[str] | None:
    """지금 도는 프로세스의 명령줄을 모은다. 못 읽으면 None — 모르면 막는 쪽으로 판정한다."""
    if sys.platform == "win32":
        # Out-String -Width 4096: 콘솔 폭에서 줄이 접히면 config 경로가 잘려 못 읽는다.
        script = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
                  "Get-CimInstance Win32_Process | ForEach-Object { $_.CommandLine } | "
                  "Where-Object { $_ } | Out-String -Width 4096")
        try:
            done = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                                  capture_output=True, text=True, timeout=60,
                                  encoding="utf-8", errors="replace")
        except (OSError, subprocess.SubprocessError):
            return None

        return done.stdout.splitlines() if done.returncode == 0 else None

    procfs = Path("/proc")
    if not procfs.is_dir():
        return None

    lines: list[str] = []
    for entry in procfs.iterdir():
        if not entry.name.isdigit():
            continue

        try:
            raw = (entry / "cmdline").read_bytes()
        except OSError:   # 조회 사이에 끝난 프로세스는 건너뛴다
            continue

        lines.append(raw.replace(b"\0", b" ").decode("utf-8", "replace").strip())

    return lines


def live_configs(lines: list[str]) -> list[str]:
    """트레이더·감시견이 열고 있는 config 경로. 프로세스는 있는데 경로를 못 뽑으면 빈 문자열을 넣는다."""
    found: list[str] = []
    for line in lines:
        if "deploy_guard" in line:   # 이 검사 자신과 이것을 부른 relink.cmd는 세지 않는다
            continue

        if not (TRADER_PATTERN.search(line) or WATCHDOG_PATTERN.search(line)):
            continue

        match = CONFIG_TOKEN_PATTERN.search(line)
        found.append(match.group(0) if match else "")

    return found


def window_end(config_path: Path) -> dt.time | None:
    """그 config가 모의면 15:30, 실계좌면 20:00. 못 읽으면 None."""
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        is_paper = bool(config.get("kis", {}).get("is_paper", config.get("is_paper", True)))
    except (OSError, ValueError):
        return None

    return PAPER_WINDOW_END if is_paper else LIVE_WINDOW_END


def window_end_of_running(configs: list[str]) -> tuple[dt.time, str]:
    """도는 프로세스들 중 가장 늦게 끝나는 창을 고른다. 판정 근거 한 줄을 같이 돌려준다."""
    latest, reason = PAPER_WINDOW_END, ""
    for token in configs:
        if not token:   # 명령줄에서 config를 못 뽑았다 — 실계좌일 수 있으니 늦은 쪽으로 본다
            return LIVE_WINDOW_END, "도는 프로세스의 config를 명령줄에서 못 읽어 실계좌로 본다"

        path = Path(token)
        resolved = path if path.is_absolute() else REPO / path
        end = window_end(resolved)
        if end is None:
            return LIVE_WINDOW_END, f"{token}을 읽지 못해 실계좌로 본다"

        if end >= latest:
            latest, reason = end, f"도는 프로세스가 연 {token}"

    return latest, reason


def in_trading_window(now: dt.datetime, end: dt.time) -> bool:
    return now.weekday() < 5 and dt.time(9, 0) <= now.time() <= end


def write_log(line: str) -> None:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as log_file:
        log_file.write(line + "\n")


def decide(arguments: argparse.Namespace) -> tuple[dt.time | None, str]:
    """창 끝과 그 근거. 창 끝이 None이면 막을 이유가 없다는 뜻이다."""
    if arguments.config:
        path = Path(arguments.config)
        end = window_end(path if path.is_absolute() else REPO / path)
        if end is None:
            return LIVE_WINDOW_END, f"{arguments.config}를 읽지 못해 실계좌로 본다"

        return end, f"--config {arguments.config}"

    lines = command_lines()
    if lines is None:
        end = window_end(DEFAULT_CONFIG) or LIVE_WINDOW_END
        return end, "도는 프로세스를 못 읽어 Quant/config/config.json으로 본다"

    configs = live_configs(lines)
    if not configs:
        return None, "트레이더도 감시견도 돌고 있지 않다"

    return window_end_of_running(configs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hotfix-a", metavar="사유", help="A등급 결함 예외 — 사유 한 줄")
    parser.add_argument("--config", help="도는 프로세스 대신 이 config로 판정한다")
    arguments = parser.parse_args()

    now = dt.datetime.now()
    stamp = now.strftime("%Y-%m-%d %H:%M:%S")
    end, reason = decide(arguments)

    if end is None:
        print(f"[deploy_guard] 통과 — {reason}. 바꿀 exe를 쓰는 프로세스가 없다.")
        return 0

    if not in_trading_window(now, end):
        print(f"[deploy_guard] 통과 — 매매 창(09:00~{end:%H:%M}) 밖이다. 판정 근거: {reason}.")
        return 0

    if arguments.hotfix_a:
        write_log(f"{stamp} 장중 교체 허용(A등급) — {arguments.hotfix_a}")
        print(f"[deploy_guard] 장중이지만 A등급 결함으로 통과 — 사유: {arguments.hotfix_a}")
        return 0

    write_log(f"{stamp} 장중 교체 차단 — {reason}")
    print(f"[deploy_guard] 매매 창(09:00~{end:%H:%M}) 안이라 exe를 바꾸지 않는다 — "
          "리팩터·이름·문서·성능은 장 마감 뒤에. A등급 결함(체결 누락·이중 발주·원장 불일치·주문 불능)이면 "
          f"--hotfix-a \"사유\"로 지나간다. 판정 근거: {reason}. 정본 CLAUDE.md '장중 운영'.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
