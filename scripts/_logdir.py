# -*- coding: utf-8 -*-
"""로그·원장 폴더 해석 — scripts/ 공용.

엔진은 실행파일 기준 logs/ 에 쓰고(QUANT_LOG_DIR로 덮어씀), 테스트 바이너리는 cwd 기준
logs/ 에 쓴다. 그래서 같은 날짜의 trades_YYYYMMDD.csv 가 두 폴더에 생길 수 있고,
소비자마다 고르는 규칙이 다르면 같은 날을 두고 다른 숫자를 낸다(2026-09-08: 561체결 원장을
7체결 시험 원장이 mtime으로 이겼다). 여기 규칙 하나로 통일한다.

  log_dir()          엔진 로그 폴더. QUANT_LOG_DIR > quant_trader.log 가 가장 최근에 쓰인 후보
                     > 실재하는 첫 후보 > Quant/build_win/logs
  find_ledger(date)  그 날짜 원장. 행 수 최대, 동률이면 mtime 최신
  find_log(date)     고른 원장 옆의 quant_trader.log, 없으면 log_dir() 의 것
"""
from __future__ import annotations

import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LEDGER_NAME = "trades_{ymd}.csv"
LOG_NAME = "quant_trader.log"


def _ymd(date: str) -> str:
    return date.replace("-", "")[:8]


def candidate_dirs() -> list[Path]:
    """탐색 순서. 환경변수가 있으면 맨 앞. 중복 경로는 한 번만."""
    cands: list[Path] = []
    env = os.environ.get("QUANT_LOG_DIR")
    if env:
        cands.append(Path(env))
    cands += [
        REPO / "Quant" / "build_win" / "logs",   # 실행파일 옆 — 평소 운영 경로
        REPO / "logs",                           # cwd 기준으로 뜬 바이너리·테스트
        REPO / "Quant" / "logs",
        Path.cwd() / "logs",
    ]
    seen, out = set(), []
    for c in cands:
        try:
            key = c.resolve()
        except OSError:
            key = c
        if key not in seen:
            seen.add(key)
            out.append(c)
    return out


def log_dir() -> Path:
    env = os.environ.get("QUANT_LOG_DIR")
    if env:
        return Path(env)
    best, best_mt = None, -1.0
    for c in candidate_dirs():
        try:
            mt = (c / LOG_NAME).stat().st_mtime
        except OSError:
            continue
        if mt > best_mt:
            best, best_mt = c, mt
    if best is not None:
        return best
    for c in candidate_dirs():
        if c.is_dir():
            return c
    return REPO / "Quant" / "build_win" / "logs"


def _count_rows(p: Path) -> int:
    n = 0
    with p.open("rb") as f:
        for line in f:
            if line.strip():
                n += 1
    return max(0, n - 1)  # 헤더 제외


def ledger_candidates(date: str) -> list[tuple[Path, int, float]]:
    """(경로, 데이터 행 수, mtime) — 좋은 것부터. 행 수 최대, 동률이면 mtime 최신."""
    name = LEDGER_NAME.format(ymd=_ymd(date))
    out = []
    for d in candidate_dirs():
        p = d / name
        try:
            st = p.stat()
        except OSError:
            continue
        out.append((p, _count_rows(p), st.st_mtime))
    out.sort(key=lambda t: (t[1], t[2]), reverse=True)
    return out


def find_ledger(date: str) -> Path | None:
    c = ledger_candidates(date)
    return c[0][0] if c else None


def find_log(date: str | None = None) -> Path | None:
    """원장을 고른 폴더의 로그를 우선한다 — 그날 실제로 돈 엔진의 로그가 원장 옆에 있다."""
    if date:
        led = find_ledger(date)
        if led is not None and (led.parent / LOG_NAME).exists():
            return led.parent / LOG_NAME
    p = log_dir() / LOG_NAME
    return p if p.exists() else None
