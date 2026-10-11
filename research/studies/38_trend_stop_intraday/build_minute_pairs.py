"""스터디 38 분봉 백필 대상(날짜 → 종목) — `PYQuant/tools/minute_backfill.py --pairs` 입력을 만든다.

왜: 손절 시험은 장중 신호 순간의 가격으로 사고 팔아야 해서 분봉이 필요하다. 기존 분봉은 820종목이지만 대부분 일부 날짜뿐이라
전 시장을 대표하지 못한다. 날짜 D 마다 D-1 까지의 일봉으로만 고른 종목을 받는다(그날 아침에 알 수 있던 목록, 미래 정보 없음).

선정(사용자 지시 2026-10-10: 거래대금·시총·거래량을 보고 최소 1000종목):
  - 보통주(우선주·ETF·스팩 제외 — features.fundamental.is_common_stock), 상장 20거래일 이상, 최근 20일 거래 없는 날 2일 이하
  - 20일 평균 거래대금 상위 N ∪ 시가총액 상위 N ∪ 20일 평균 거래량 상위 N
  - N 은 하루 합집합이 --min-per-day 이상이 되는 가장 작은 값을 날짜마다 찾는다

사용:
  py research/studies/38_trend_stop_intraday/build_minute_pairs.py --start 2025-10-01 --end 2026-10-08 --out <파일>
"""
import argparse
import json
import sys
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "PYQuant"))

from features.fundamental import FundamentalData  # noqa: E402
from PYQuant.data.point_in_time import as_of_join  # noqa: E402

LISTED_TRADING_DAYS_MIN = 20
ZERO_VOLUME_DAYS_MAX = 2
VOLUME_WINDOW_DAYS = 20


def day_universe(data: FundamentalData, volume_20d: pd.Series, as_of: pd.Timestamp, minimum: int) -> tuple[list[str], int]:
    section = data.bars.cross_section(as_of)
    section = section[data.is_common_stock(section["ticker"], section["name"])]
    listed_rows = data.bars.close.loc[:as_of].notna().sum()
    section = section[section["ticker"].map(listed_rows).fillna(0) >= LISTED_TRADING_DAYS_MIN]
    section = section[section["zero_volume_20d"] <= ZERO_VOLUME_DAYS_MAX].copy()
    section["volume_20d"] = section["ticker"].map(volume_20d)
    section["as_of"] = as_of
    joined = as_of_join(section, data.shares, on="ticker", left_time="as_of", right_time="effective_date")
    joined["market_cap"] = joined["shares_common_issued"].astype(float) * joined["close"]

    by_turnover = joined.sort_values("turnover_20d", ascending=False)["ticker"].tolist()
    by_cap = joined.dropna(subset=["market_cap"]).sort_values("market_cap", ascending=False)["ticker"].tolist()
    by_volume = joined.sort_values("volume_20d", ascending=False)["ticker"].tolist()
    top_n = minimum // 2

    while True:
        chosen = set(by_turnover[:top_n]) | set(by_cap[:top_n]) | set(by_volume[:top_n])

        if len(chosen) >= minimum or top_n >= len(joined):
            return sorted(chosen), top_n

        top_n += 25


def main() -> None:
    parser = argparse.ArgumentParser(description="스터디 38 분봉 백필 대상 생성")
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", required=True)
    parser.add_argument("--min-per-day", type=int, default=1000)
    parser.add_argument("--out", required=True)
    arguments = parser.parse_args()

    data = FundamentalData.load()
    volume_20d = data.bars.volume.rolling(VOLUME_WINDOW_DAYS, min_periods=VOLUME_WINDOW_DAYS).mean()
    calendar = data.bars.trading_days
    days = calendar[(calendar >= pd.Timestamp(arguments.start)) & (calendar <= pd.Timestamp(arguments.end))]
    pairs = {}
    top_sizes = []
    union = set()

    for day in days:
        previous = calendar[calendar < day]
        tickers, top_n = day_universe(data, volume_20d.loc[previous[-1]], previous[-1], arguments.min_per_day)
        pairs[day.strftime("%Y%m%d")] = tickers
        top_sizes.append(top_n)
        union.update(tickers)

    per_day = [len(tickers) for tickers in pairs.values()]
    print(f"거래일 {len(pairs)}일 · 하루 {min(per_day)}–{max(per_day)}종목 · 축별 상위 N {min(top_sizes)}–{max(top_sizes)}"
          f" · 합집합 {len(union)}종목 · (종목,날짜) {sum(per_day):,}쌍")
    Path(arguments.out).write_text(json.dumps(pairs, ensure_ascii=False), encoding="utf-8")
    print(f"→ {arguments.out}")


if __name__ == "__main__":
    main()
