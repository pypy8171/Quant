"""스터디 40 결과를 스터디 38 대시보드의 '매수 시험' 탭 데이터(dashboard/data/entry40.txt)로 쓴다.

캔들 차트용 일봉 묶음·종목 이름(meta.txt)은 스터디 38 대시보드 것을 그대로 쓴다. 그 묶음에 없는 종목은 탭에서
"차트 자료 없음"으로 보인다.
사용: py research/studies/40_devscale_entry_touch/build_dashboard.py
"""
import sys
from pathlib import Path

import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402

DATA_DIR = HERE / "data"


def main() -> None:
    curve = pd.read_csv(DATA_DIR / "curve.csv", parse_dates=["date"])
    curve = curve.groupby(curve["date"].dt.to_period("M")).last().reset_index(drop=True)
    curve["date"] = curve["date"].dt.strftime("%Y-%m-%d")
    trades = {}

    for path in sorted(DATA_DIR.glob("trades_*.parquet")):
        frame = pd.read_parquet(path)
        trades[path.stem[len("trades_"):]] = board.clean(
            frame[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price", "reason", "net"]]
            .rename(columns={"ticker_index": "ticker"}))

    payload = {"summary": board.clean(pd.read_csv(DATA_DIR / "summary.csv")),
               "years": board.clean(pd.read_csv(DATA_DIR / "years.csv")),
               "candidates": board.clean(pd.read_csv(DATA_DIR / "candidates.csv")),
               "curve": board.clean(curve), "benchmark": board.clean(board.benchmark_curve(curve["date"])),
               "trades": trades}
    print(f"→ entry40.txt {board.write_json('entry40.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
