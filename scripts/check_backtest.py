#!/usr/bin/env python3
"""백테스트 재현성 게이트 — 위기 스터디(07·08·09)의 *결정론*만 검사한다(비파괴).

이 게이트는 트리를 변경하지 않으며 결정론만 검사한다. git 상태와 완전히 독립이다.
  - `git checkout`·`git reset`·`git stash` 등 워킹트리를 버리는 명령을 절대 쓰지 않는다.
  - 실행 전후로 `git status --porcelain` 가 반드시 동일하다(스냅샷에서 원복하므로).

무엇을 검사하나(결정론 = 같은 입력→같은 출력):
  1) 감시 산출물(07 summary.tsv·README, 08 README, 09 README)의 현재 워킹트리 내용을
     스크래치패드 임시파일로 스냅샷.
  2) `INDEX_OFFLINE=1 BACKTEST_AS_OF=2026-08-15` 로 07·08·09 재실행(산출물 덮어씀).
  3) 재생성본을 스냅샷과 **바이트 비교**. 전부 동일 → 결정론 PASS. 하나라도 다르면 FAIL.
  4) **성패 무관하게 스냅샷에서 원복** → 게이트는 디스크를 건드리지 않은 셈이 된다.

왜 결정론이 되나:
  08/09 는 원래 벤치마크 종료일로 date.today() 를 써서 매일 캐시키·숫자가 흔들렸다.
  data.asof.as_of() 로 그 상한을 BACKTEST_AS_OF 로 고정해 **AS_OF 고정으로 결정론화**했다.
  INDEX_OFFLINE=1 은 캐시 미스 시 네트워크 재다운로드를 차단(RuntimeError)해,
  커밋된 캐시에서만 도는 폐쇄 재현을 보장한다.

사용법:  python scripts/check_backtest.py
종료코드:
  0 = 결정론 PASS(재실행이 현재 산출물을 비트단위 재현) 또는 SKIP(캐시 없음 — 로컬 전용).
  1 = 비결정(재실행 실패 또는 산출물 불일치). 두 경우 모두 워킹트리는 원상복구된다.
"""
from __future__ import annotations

import filecmp
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "PYQuant" / ".index_cache"
# 결정론 앵커 날짜 — 환경변수로 오버라이드 가능(미지정 시 커밋된 산출물 생성일 고정).
AS_OF = os.environ.get("BACKTEST_AS_OF", "2026-08-15")
# 스냅샷 임시경로 — 세션 고정 절대경로(죽은 UUID) 대신 QUANT_SCRATCH_DIR env → 없으면 OS 임시폴더.
SCRATCH = Path(os.environ.get("QUANT_SCRATCH_DIR") or tempfile.mkdtemp(prefix="quant_bt_"))

STUDIES = [
    ("07", "research/studies/07_crisis_regimes/analyze_crisis_regimes.py"),
    ("08", "research/studies/08_crisis_response/backtest_crisis_response.py"),
    ("09", "research/studies/09_crisis_strategies/backtest_crisis_strategies.py"),
]
# 감시 산출물(재실행이 덮어쓰는 결정론 대상). 스터디별 README + 07 summary.tsv.
WATCHED = [
    "research/studies/07_crisis_regimes/summary.tsv",
    "research/studies/07_crisis_regimes/README.md",
    "research/studies/08_crisis_response/README.md",
    "research/studies/09_crisis_strategies/README.md",
]

# 콘솔 코드페이지와 무관하게 한글 출력이 깨지지 않도록 utf-8 고정.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def snapshot(snap_dir: Path) -> dict[str, Path | None]:
    """감시 산출물의 현재 내용을 snap_dir 로 복사. 미존재 파일은 None 으로 기록."""
    saved: dict[str, Path | None] = {}
    for i, rel in enumerate(WATCHED):
        src = REPO / rel
        if src.exists():
            dst = snap_dir / f"{i:02d}_{Path(rel).name}"
            shutil.copy2(src, dst)
            saved[rel] = dst
        else:
            saved[rel] = None
    return saved


def restore(saved: dict[str, Path | None]) -> None:
    """스냅샷에서 원복. 스냅샷에 없던(재실행이 새로 만든) 파일은 삭제."""
    for rel, snap in saved.items():
        dst = REPO / rel
        if snap is None:
            if dst.exists():
                dst.unlink()
        else:
            shutil.copy2(snap, dst)


def main() -> int:
    if not CACHE.exists() or not any(CACHE.glob("*.parquet")):
        print("SKIP: 캐시 없음(오프라인 재현 불가, 로컬 전용 게이트)")
        return 0

    SCRATCH.mkdir(parents=True, exist_ok=True)
    snap_dir = Path(tempfile.mkdtemp(prefix="bt_gate_", dir=str(SCRATCH)))
    saved = snapshot(snap_dir)

    env = dict(os.environ)
    env["INDEX_OFFLINE"] = "1"
    env["BACKTEST_AS_OF"] = AS_OF
    env["PYTHONIOENCODING"] = "utf-8"

    try:
        for tag, script in STUDIES:
            r = subprocess.run(
                [sys.executable, script],
                cwd=str(REPO), env=env,
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            if r.returncode != 0:
                print(f"비결정: 스터디 {tag} 재실행 실패(exit {r.returncode})")
                for ln in (r.stderr or r.stdout or "").strip().splitlines()[-15:]:
                    print(f"  | {ln}")
                return 1

        mismatches = []
        for rel, snap in saved.items():
            cur = REPO / rel
            if snap is None:
                if cur.exists():
                    mismatches.append(f"{rel} (재실행이 새로 생성 — 기존엔 없던 파일)")
                continue
            if not cur.exists():
                mismatches.append(f"{rel} (재실행이 생성하지 않음 — 소실)")
            elif not filecmp.cmp(snap, cur, shallow=False):
                mismatches.append(f"{rel} (내용 불일치 — 재실행이 다른 바이트 산출)")

        if mismatches:
            print(f"비결정: 재생성 산출물이 현재 워킹트리와 불일치 (AS_OF={AS_OF}).")
            for m in mismatches:
                print(f"  - {m}")
            print("워킹트리는 원복됩니다. (감시 산출물을 최신 AS_OF 로 재생성해 커밋 후 다시 실행)")
            return 1

        print(f"재현성 게이트 통과 — 재실행이 현재 산출물을 비트단위 재현 "
              f"(AS_OF={AS_OF}, INDEX_OFFLINE, 결정론).")
        return 0
    finally:
        restore(saved)
        shutil.rmtree(snap_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
