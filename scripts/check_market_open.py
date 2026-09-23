"""오늘 주식시장이 열리는지 KIS에 물어 본다 — 감시견이 휴장일에 엔진을 띄우지 않게 하는 관문.

여태 scripts/auto_trade_day.ps1 에는 주말 판정조차 없어서 토·일·공휴일에도 트레이더가 떴다.
시세가 안 오고 주문도 안 나가니 손해는 없지만 토큰을 새로 받고 WS 재접속을 되풀이한다.

국내휴장일조회(CTCA0903R)는 기준일부터 24일치를 한 번에 주므로 받아 두면 그 기간에는 다시 안 부른다.
공식 안내가 "원장 서비스와 연관되어 있어 가급적 1일 1회 호출"이라 캐시가 선택이 아니라 요건이다. [why D-120]

  py scripts/check_market_open.py            # 오늘. 종료코드 0=개장 1=휴장 2=모름
  py scripts/check_market_open.py --date 20260924
  py scripts/check_market_open.py --show     # 받아 둔 달력을 그대로 보여준다

판정을 못 했을 때(조회 실패·달력에 없는 날) 2를 돌려준다. 부르는 쪽은 2를 휴장으로 보지 않는다 —
조회 한 번 실패한 날 매매를 통째로 건너뛰는 쪽이 휴장일에 헛도는 것보다 비싸다.
"""
from __future__ import annotations

import argparse
import datetime as datetime_module
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CACHE_PATH = REPO_ROOT / "logs" / "holiday_calendar.json"
DEFAULT_CONFIG = REPO_ROOT / "Quant" / "config" / "config.json"

EXIT_OPEN = 0
EXIT_CLOSED = 1
EXIT_UNKNOWN = 2

# wday_dvsn_cd — 01 이 일요일이다(2026-09-23 실측: 수=04, 목=05, 금=06, 토=07, 일=01, 월=02).
WEEKDAY_NAMES = {"01": "일", "02": "월", "03": "화", "04": "수", "05": "목", "06": "금", "07": "토"}


def load_cache() -> list:
    try:
        cached = json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []

    rows = cached.get("rows", [])
    return rows if isinstance(rows, list) else []


def save_cache(rows: list) -> None:
    CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "fetched_at": datetime_module.datetime.now().isoformat(timespec="seconds"),
        "rows": rows,
    }
    CACHE_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def find_row(rows: list, date: str) -> dict:
    for row in rows:
        if str(row.get("bass_dt", "")) == date:
            return row

    return {}


def fetch_calendar(config_path: Path, date: str) -> list:
    """KIS에 물어 달력을 받는다. 실패하면 빈 목록."""
    sys.path.insert(0, str(REPO_ROOT / "PYQuant"))

    try:
        from kis.client import from_config
    except ImportError as error:
        print(f"[오류] KIS 클라이언트를 못 불러왔다 ({error})", file=sys.stderr)
        return []

    # 어떤 예외도 밖으로 내보내지 않는다 — 파이썬이 예외로 죽으면 종료코드가 1이고,
    # 부르는 쪽은 1을 '휴장'으로 읽어 개장일 매매를 통째로 건너뛴다.
    # 설정 파일 없음·인증 실패·연결 끊김이 모두 이 자리로 온다. [inv] 실패는 빈 목록 = 모름(2)
    try:
        client = from_config(str(config_path))
        return client.get_holiday_calendar(date)
    except Exception as error:
        print(f"[오류] 휴장일 조회 실패 ({type(error).__name__}: {error})", file=sys.stderr)
        return []


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    parser = argparse.ArgumentParser(description="오늘 장이 열리는지 KIS에 물어 본다")
    parser.add_argument("--date", default="", help="YYYYMMDD. 비우면 오늘")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--show", action="store_true", help="받아 둔 달력을 보여주고 끝낸다")
    parser.add_argument("--refresh", action="store_true", help="캐시를 무시하고 다시 받는다")
    arguments = parser.parse_args()

    date = arguments.date or datetime_module.date.today().strftime("%Y%m%d")
    rows = [] if arguments.refresh else load_cache()

    if arguments.show:
        if not rows:
            print("받아 둔 달력이 없다 — 먼저 인자 없이 한 번 돌린다")
            return EXIT_UNKNOWN

        for row in rows:
            weekday = WEEKDAY_NAMES.get(str(row.get("wday_dvsn_cd", "")), "?")
            state = "개장" if str(row.get("opnd_yn", "")).upper() == "Y" else "휴장"
            print(f"  {row.get('bass_dt', '?')} ({weekday}) {state}")

        return EXIT_OPEN

    row = find_row(rows, date)

    if not row:
        rows = fetch_calendar(Path(arguments.config), date)

        if rows:
            save_cache(rows)

        row = find_row(rows, date)

    if not row:
        print(f"[모름] {date} 개장 여부를 확인하지 못했다 — 휴장으로 단정하지 않는다")
        return EXIT_UNKNOWN

    weekday = WEEKDAY_NAMES.get(str(row.get("wday_dvsn_cd", "")), "?")
    is_open = str(row.get("opnd_yn", "")).upper() == "Y"

    if is_open:
        print(f"[개장] {date} ({weekday}) — 주문을 낼 수 있는 날이다")
        return EXIT_OPEN

    # 다음 개장일을 같이 알려 주면 감시견 로그만 보고도 언제 다시 도는지 안다.
    later = [later_row for later_row in rows
             if str(later_row.get("bass_dt", "")) > date
             and str(later_row.get("opnd_yn", "")).upper() == "Y"]
    next_open = later[0].get("bass_dt", "?") if later else "달력 범위 밖"
    print(f"[휴장] {date} ({weekday}) — 장이 열리지 않는다. 다음 개장일 {next_open}")
    return EXIT_CLOSED


if __name__ == "__main__":
    sys.exit(main())
