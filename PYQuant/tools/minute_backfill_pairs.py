#!/usr/bin/env python3
"""분봉 백필 대상(날짜 → 종목) 파일을 네이버 일봉 패널에서 만든다 — `minute_backfill.py --pairs` 입력.

왜 필요한가: DevScale 리플레이는 "그날 아침에 알 수 있던 유니버스"의 분봉이 있어야 한다. `Quant/config/universe_scan.json`은
당일분만 남고 시세 소스가 하루 늦어(data.go.kr) 과거 날짜엔 못 쓴다. 그래서 네이버 일봉 패널(`features.fundamental.FundamentalData`)로
전 거래일 기준 시총 상위 N ∪ 20일 거래대금 상위 N 을 날짜마다 다시 뽑는다. D의 목록은 D-1 종가까지만 본다(룩어헤드 없음).

사용:
  py PYQuant/tools/minute_backfill_pairs.py --start 2025-09-01 --end 2026-09-18 --top-n 150 --out C:/build_tmp/minute_pairs.json
  py PYQuant/tools/minute_backfill.py --start 2025-09-01 --end 2026-09-18 --pairs C:/build_tmp/minute_pairs.json
"""
import argparse
import json
import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from features.fundamental import FundamentalData  # noqa: E402


def build_pairs(data: FundamentalData, start: pd.Timestamp, end: pd.Timestamp, top_n: int) -> dict:
    calendar = data.bars.trading_days
    days = calendar[(calendar >= start) & (calendar <= end)]
    pairs = {}
    union = set()

    for day in days:
        previous = calendar[calendar < day]

        if previous.empty:
            continue

        section = data.candidates(previous[-1])
        by_cap = section.nlargest(top_n, "market_cap")["ticker"]
        by_turnover = section.nlargest(top_n, "turnover_20d")["ticker"]
        tickers = sorted(set(by_cap) | set(by_turnover))
        pairs[day.strftime("%Y%m%d")] = tickers
        union.update(tickers)

    print(f"거래일 {len(pairs)}일 · 하루 평균 {sum(len(tickers) for tickers in pairs.values()) / max(len(pairs), 1):.0f}종목 · 합집합 {len(union)}종목"
          f" · KIS 콜 약 {sum(len(tickers) for tickers in pairs.values()) * 4:,}회")
    return pairs


def main():
    parser = argparse.ArgumentParser(description="분봉 백필 대상(날짜→종목) 파일 생성")
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", required=True)
    parser.add_argument("--top-n", type=int, default=150, help="시총 상위 N ∪ 거래대금 상위 N (기본 150)")
    parser.add_argument("--out", required=True)
    arguments = parser.parse_args()

    data = FundamentalData.load()
    pairs = build_pairs(data, pd.Timestamp(arguments.start), pd.Timestamp(arguments.end), arguments.top_n)
    Path(arguments.out).write_text(json.dumps(pairs, ensure_ascii=False), encoding="utf-8")
    print(f"→ {arguments.out}")


if __name__ == "__main__":
    main()
