#!/usr/bin/env python3
"""장 마감 뒤 당일 1분봉을 로컬에 쌓는다 — 예약작업 `Quant Minute Backfill`(평일 16:40)이 부른다.
왜 따로 있나: `PYQuant/tools/minute_backfill.py`는 날짜별 시점 유니버스(PIT, 그날 아침에 알 수 있던
종목 목록) 파일을 요구하는데 그 파일은 백필 도구로만 만들어져 왔다. 아침 스캔 산출물
`Quant/config/universe_scan.json`이 같은 스키마이므로 그것을 오늘 날짜로 옮겨 두고 백필을 돌린다.
이렇게 하루씩 쌓이면 3분봉 이력이 KIS 250일 롤링 한도(docs/DECISIONS.md D-009)를 넘겨 남는다.
대시보드 차트(`scripts/dashboard_server.py::_local_minute_today`)도 같은 파일을 읽으므로 스키마를 바꾸면 둘 다 본다.
"""
import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

# 예약작업 콘솔은 cp949라 em dash 같은 글자에서 죽는다(scripts/eod_autodoc.py와 같은 처치).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

REPO = Path(__file__).resolve().parents[1]
KST = timezone(timedelta(hours=9))
SCAN = REPO / "Quant" / "config" / "universe_scan.json"
PIT_DIR = REPO / "PYQuant" / "data" / "pit_universe"


def main() -> int:
    ap = argparse.ArgumentParser(description="장 마감 뒤 당일 1분봉 백필(예약작업용)")
    ap.add_argument("--force", action="store_true",
                    help="15:45 전이라도 돈다. 장중에 돌리면 반쪽 파일이 남아 그날치가 건너뛰어진다")
    args = ap.parse_args()
    now = datetime.now(KST)
    if now.weekday() >= 5:
        print("[eod_minute_backfill] 주말 — 건너뜀")
        return 0
    # 장중에 돌면 마감 전까지의 봉만 담긴 파일이 남고, 백필 도구는 그 파일이 있다는 이유로 그날을
    #  건너뛴다(2026-09-11 실수). 마감 뒤에만 돌린다.
    if not args.force and (now.hour, now.minute) < (15, 45):
        print("[eod_minute_backfill] 15:45 전 — 마감 뒤에 돌린다(--force로 강제)")
        return 2
    ymd = now.strftime("%Y%m%d")
    pit = PIT_DIR / f"{ymd}.json"
    if not pit.exists():
        if not SCAN.exists():
            print(f"[eod_minute_backfill] {SCAN} 없음 — 유니버스 스캔이 안 돌았다")
            return 1
        doc = json.loads(SCAN.read_text(encoding="utf-8"))
        # 아침 스캔의 basDt는 전 거래일이어야 오늘의 시점 유니버스다. 며칠 묵은 파일이면 쓰지 않는다.
        bas = str(doc.get("basDt", ""))
        if not bas or (now.date() - datetime.strptime(bas, "%Y%m%d").date()).days > 4:
            print(f"[eod_minute_backfill] universe_scan.json basDt={bas} — 오늘 것으로 볼 수 없어 건너뜀")
            return 1
        PIT_DIR.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SCAN, pit)
        print(f"[eod_minute_backfill] PIT 유니버스 저장 {pit.name} (basDt={bas}, {doc.get('count')}종목)")
    day = now.strftime("%Y-%m-%d")
    cmd = [sys.executable, str(REPO / "PYQuant" / "tools" / "minute_backfill.py"),
           "--start", day, "--end", day, "--top-n", "0"]
    print("[eod_minute_backfill]", " ".join(cmd))
    return subprocess.call(cmd, cwd=str(REPO))


if __name__ == "__main__":
    sys.exit(main())
