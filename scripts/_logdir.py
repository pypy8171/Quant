# -*- coding: utf-8 -*-
"""로그·원장 폴더 해석 — scripts/ 공용.

엔진은 실행파일 기준 logs/ 에 쓰고(QUANT_LOG_DIR로 덮어씀), 테스트 바이너리는 cwd 기준
logs/ 에 쓴다. 그래서 같은 날짜의 trades_YYYYMMDD.csv 가 두 폴더에 생길 수 있고,
소비자마다 고르는 규칙이 다르면 같은 날을 두고 다른 숫자를 낸다(2026-09-08: 561체결 원장을
7체결 시험 원장이 mtime으로 이겼다). 여기 규칙 하나로 통일한다.

  log_dir()          엔진 로그 폴더. QUANT_LOG_DIR > 실행 로그가 가장 최근에 쓰인 후보
                     > 실재하는 첫 후보 > Quant/build_win/logs
  live_logs(dir)     그 폴더의 실행 로그들. D-114로 갈라 띄우면 한 프로세스가 quant_trader.log 를 쓰는 대신
                     주문 쪽이 quant_trader.order.log, 전략 쪽이 quant_trader.strategy.log 를 쓴다
  role_of(path)      그 파일을 쓴 역할("both"·"order"·"strategy") — 판정을 역할별로 가르는 쪽이 쓴다
  find_ledger(date)  그 날짜 원장. 행 수 최대, 동률이면 mtime 최신
  find_log(date)     고른 원장 옆의 quant_trader.log, 없으면 log_dir() 의 것
  log_sources(date, directory)   그 날짜 줄이 들어 있을 수 있는 파일 — archive/quant_trader_<날짜>.log.gz 뒤에
                     라이브 로그. maintain.py --rotate-logs 가 7일 지난 날의 줄을 gz로 옮기므로 옛 날짜는 gz에만 있다
  iter_log_lines(date, directory) 위 파일들을 차례로 열어 줄 단위로 낸다(.gz 도 보통 텍스트처럼)
"""
from __future__ import annotations

import contextlib
import gzip
import heapq
import os
from pathlib import Path
from typing import Iterator

REPO = Path(__file__).resolve().parents[1]
LEDGER_NAME = "trades_{ymd}.csv"
LOG_NAME = "quant_trader.log"
# 역할을 주고 띄운 프로세스가 쓰는 이름(D-114 단계 4). 갈라 띄우면 둘이 한 파일에 섞여 써서 `[큐 고수위]` 줄이
#  어느 쪽 수인지 모르기에 파일을 가른다. 이름은 Quant/src/core/CommandLine.cpp 의 log_file_name 이 정본이다.
ROLE_LOG_NAMES = {"order": "quant_trader.order.log", "strategy": "quant_trader.strategy.log"}
LIVE_LOG_NAMES = (LOG_NAME,) + tuple(ROLE_LOG_NAMES.values())
ARCHIVE_DIR_NAME = "archive"
ARCHIVE_NAME = "quant_trader_{iso}.log.gz"   # maintain.py rotate_engine_log 가 쓰는 이름
ARCHIVE_ROLE_NAME = "quant_trader_{role}_{iso}.log.gz"   # 역할별 회전본
# 줄머리 시각 "2026-09-23 21:33:20.986" 의 길이. 파일 여럿을 시각순으로 합칠 때 쓴다.
TIMESTAMP_WIDTH = 23


def _ymd(date: str) -> str:
    return date.replace("-", "")[:8]


def _iso(date: str) -> str:
    ymd = _ymd(date)
    return f"{ymd[:4]}-{ymd[4:6]}-{ymd[6:]}"


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
        for name in LIVE_LOG_NAMES:   # 갈라 띄운 폴더에는 quant_trader.log 가 아예 없다
            try:
                mt = (c / name).stat().st_mtime
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


def live_logs(directory: Path | None = None) -> list[Path]:
    """그 폴더에 실재하는 실행 로그들 — 한 프로세스면 quant_trader.log 하나, 갈라 띄웠으면 역할별 둘."""
    base = directory if directory is not None else log_dir()
    return [base / name for name in LIVE_LOG_NAMES if (base / name).is_file()]


def role_of(path: Path) -> str:
    """그 파일을 쓴 역할. 회전본(.gz)도 이름으로 가른다."""
    name = path.name
    for role, log_name in ROLE_LOG_NAMES.items():
        if name == log_name or name.startswith("quant_trader_" + role + "_"):
            return role
    return "both"


def find_log(date: str | None = None) -> Path | None:
    """원장을 고른 폴더의 로그를 우선한다 — 그날 실제로 돈 엔진의 로그가 원장 옆에 있다.
    갈라 띄운 폴더에는 quant_trader.log 가 없어 역할별 파일 중 최신을 낸다. 한 파일만 보는 옛 소비자를 위한
    것이고, 둘 다 봐야 하면 live_logs() 나 iter_log_lines() 를 쓴다."""
    if date:
        led = find_ledger(date)
        if led is not None:
            beside = live_logs(led.parent)
            if beside:
                return max(beside, key=lambda path: path.stat().st_mtime)
    here = live_logs(log_dir())
    return max(here, key=lambda path: path.stat().st_mtime) if here else None


def dir_of(log_path: Path) -> Path:
    """로그 파일이 속한 로그 폴더. archive/ 안의 gz 는 한 단계 위가 로그 폴더다."""
    parent = log_path.parent
    return parent.parent if parent.name == ARCHIVE_DIR_NAME else parent


def log_sources(date: str | None = None, directory: Path | None = None) -> list[Path]:
    """date(YYYYMMDD 또는 YYYY-MM-DD) 줄이 들어 있을 수 있는 파일을 옛 줄부터 — 그 날짜 gz, 그다음 라이브 로그.
    date 가 없으면 gz 전부(날짜순) + 라이브 로그. 없는 파일은 뺀다."""
    base = directory if directory is not None else log_dir()
    archive_dir = base / ARCHIVE_DIR_NAME
    sources: list[Path] = []
    if date:
        sources.append(archive_dir / ARCHIVE_NAME.format(iso=_iso(date)))
        sources += [archive_dir / ARCHIVE_ROLE_NAME.format(role=role, iso=_iso(date))
                    for role in ROLE_LOG_NAMES]
    else:
        sources += sorted(archive_dir.glob(ARCHIVE_NAME.format(iso="*")))
        for role in ROLE_LOG_NAMES:
            sources += sorted(archive_dir.glob(ARCHIVE_ROLE_NAME.format(role=role, iso="*")))
    sources += live_logs(base)
    return [path for path in sources if path.is_file()]


def open_log(path: Path):
    """엔진 로그 한 파일을 텍스트로 연다. .gz 는 gzip 으로 — 줄 내용은 같다."""
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return path.open(encoding="utf-8", errors="replace")


def _keyed_lines(handle) -> Iterator[tuple[str, str]]:
    """(정렬키, 줄). 줄머리 시각이 키다. 시각이 없는 줄(여러 줄 출력의 뒷줄)은 앞줄 키를 물려받아
    제자리에 붙어 있는다."""
    previous = ""
    for line in handle:
        head = line[:TIMESTAMP_WIDTH]
        if len(head) == TIMESTAMP_WIDTH and head[4] == "-" and head[10] == " " and head[13] == ":":
            previous = head
        yield previous, line


def iter_log_lines(date: str | None = None, directory: Path | None = None) -> Iterator[str]:
    """log_sources() 의 파일을 열어 줄을 낸다(개행 포함). 소비자는 date 로 줄을 거르는 일을 그대로 한다 —
    gz 에는 그 날짜 줄만 있지만 라이브 로그에는 여러 날이 섞여 있다.

    파일이 둘 이상이면 줄머리 시각으로 합친다. 갈라 띄운 주문·전략 로그는 같은 시각대를 나눠 쓰고 있어,
    이어 붙이면 시각이 되감겨 '마지막 줄'을 보는 판정이 틀린다. [why D-114]"""
    sources = log_sources(date, directory)

    if len(sources) == 1:
        with open_log(sources[0]) as handle:
            yield from handle
        return

    with contextlib.ExitStack() as stack:
        streams = [_keyed_lines(stack.enter_context(open_log(path))) for path in sources]
        for _, line in heapq.merge(*streams, key=lambda pair: pair[0]):
            yield line
