# -*- coding: utf-8 -*-
"""장중 트레이더 exe 교체를 막는다(D-101 결정 1).

재기동 42회 중 29회가 세션이 리팩터·이름·문서를 반영하려고 트레이더를 죽인 것이었고,
재기동마다 보유분이 청산 래퍼로 넘어가 −198만/주가 나갔다. 그래서 매매 창(평일 09:00~15:30,
실계좌면 20:00까지) 안의 exe 교체는 A등급 결함(체결 누락·이중 발주·원장 불일치·주문 불능)일 때만 한다.

사용:  py scripts/deploy_guard.py            (막히면 exit 1, 아니면 0)
       py scripts/deploy_guard.py --hotfix-a "<한 줄 사유>"   A등급 결함으로 예외 통과(사유는 로그에 남는다)
C:/build_tmp/relink.cmd·scripts/auto_trade_day.ps1이 exe를 다시 만들기 전에 이 스크립트를 먼저 부른다.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LOG = REPO / "_private" / "deploy_guard.log"

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def trading_window_end(config_path: Path) -> dt.time | None:
    """모의는 정규장 15:30까지, 실계좌는 애프터마켓 20:00(D-097)까지 매매 창으로 본다. 못 읽으면 None."""
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        is_paper = bool(config.get("kis", {}).get("is_paper", config.get("is_paper", True)))
    except (OSError, ValueError):
        return None

    return dt.time(15, 30) if is_paper else dt.time(20, 0)


def in_trading_window(now: dt.datetime, window_end: dt.time) -> bool:
    return now.weekday() < 5 and dt.time(9, 0) <= now.time() <= window_end


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hotfix-a", metavar="사유", help="A등급 결함 예외 — 사유 한 줄")
    parser.add_argument("--config", default=str(REPO / "Quant" / "config" / "config.json"))
    arguments = parser.parse_args()

    now = dt.datetime.now()
    window_end = trading_window_end(Path(arguments.config))
    stamp = now.strftime("%Y-%m-%d %H:%M:%S")

    if window_end is None:
        print(f"[deploy_guard] {arguments.config}를 읽지 못해 모의/실계좌를 가릴 수 없다 — 경로를 확인한다(--config).",
              file=sys.stderr)
        return 1

    if not in_trading_window(now, window_end):
        return 0

    if arguments.hotfix_a:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with LOG.open("a", encoding="utf-8") as log_file:
            log_file.write(f"{stamp} 장중 교체 허용(A등급) — {arguments.hotfix_a}\n")
        print(f"[deploy_guard] 장중이지만 A등급 결함으로 통과 — 사유: {arguments.hotfix_a}")
        return 0

    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as log_file:
        log_file.write(f"{stamp} 장중 교체 차단\n")
    print(f"[deploy_guard] 매매 창(09:00~{window_end:%H:%M}) 안이라 exe를 바꾸지 않는다 — "
          "리팩터·이름·문서·성능은 장 마감 뒤에. A등급 결함(체결 누락·이중 발주·원장 불일치·주문 불능)이면 "
          "--hotfix-a \"사유\"로 지나간다. 정본 CLAUDE.md '장중 운영'.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
