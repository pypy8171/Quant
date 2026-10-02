"""스터디 29 — 1분봉 백필에서 결정 시각별 종목 패널을 만든다(시장 폭·눌림 후보 바스켓 입력).

입력: PYQuant/data/minute/<종목>/<YYYYMMDD>.parquet (minute_backfill_pairs.py가 전일 기준 시총·거래대금 상위로 고른 날짜별 유니버스)
      PYQuant/data/bars_all_pit.parquet (일봉 종가, ~2026-09-04, 그 뒤는 분봉 마지막 종가로 잇는다)
산출: research/studies/29_regime_score_refit/cache/minute_panel.parquet
      행 = (종목, 날짜, 결정 시각) — t 시점 가격·누적 VWAP·당일 종가·전일 종가·전일 확정 SMA5/10/20/60.

t 시점 가격은 "시작 분 < t"인 마지막 1분봉의 종가(그 봉은 t에 끝난다). 09:00은 첫 봉 시가, VWAP 없음.
SMA는 그 날짜 전 거래일 종가까지만 쓴다(룩어헤드 없음). 300봉 미만 파일(반쪽 날)은 버린다 — devscale_replay.py와 같다.

사용: py -X utf8 research/studies/29_regime_score_refit/build_minute_panel.py   (8프로세스, 약 5~10분)
"""
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

STUDY_DIR = Path(__file__).resolve().parent
REPO_ROOT = STUDY_DIR.parents[2]
MINUTE_DIR = REPO_ROOT / "PYQuant" / "data" / "minute"
DAILY_PARQUET = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit.parquet"
KOSPI_INDEX = REPO_ROOT / "research" / "studies" / "26_regime_threshold_history" / "raw" / "naver_KOSPI.parquet"
OUTPUT = STUDY_DIR / "cache" / "minute_panel.parquet"
DECISION_MINUTES = [540, 600, 660, 720, 780, 840]   # 09:00 10:00 11:00 12:00 13:00 14:00


def ticker_rows(ticker: str) -> list[tuple]:
    rows = []

    for path in sorted((MINUTE_DIR / ticker).glob("*.parquet")):
        bars = pd.read_parquet(path, columns=["time", "open", "high", "low", "close", "volume"])

        if len(bars) < 300:
            continue

        minutes = bars["time"].str[:2].astype(int).to_numpy() * 60 + bars["time"].str[2:4].astype(int).to_numpy()
        typical = ((bars["high"] + bars["low"] + bars["close"]) / 3.0).to_numpy()
        volume = bars["volume"].to_numpy().astype(float)
        cumulative_value = np.cumsum(typical * volume)
        cumulative_volume = np.cumsum(volume)
        closes = bars["close"].to_numpy()
        day_open = float(bars["open"].iloc[0])
        day_close = float(closes[-1])

        for decision in DECISION_MINUTES:
            if decision == 540:
                rows.append((ticker, path.stem, decision, day_open, np.nan, day_open, day_close))
                continue

            ended = np.nonzero(minutes < decision)[0]

            if len(ended) == 0:
                continue

            last = ended[-1]
            vwap = cumulative_value[last] / cumulative_volume[last] if cumulative_volume[last] > 0 else np.nan
            rows.append((ticker, path.stem, decision, float(closes[last]), vwap, day_open, day_close))

    return rows


def main() -> int:
    tickers = sorted(path.name for path in MINUTE_DIR.iterdir() if path.is_dir())

    with ProcessPoolExecutor(max_workers=8) as pool:
        chunks = list(pool.map(ticker_rows, tickers, chunksize=8))

    panel = pd.DataFrame([row for chunk in chunks for row in chunk],
                         columns=["ticker", "day", "minute", "price", "vwap", "open", "close"])
    panel["day"] = pd.to_datetime(panel["day"], format="%Y%m%d")

    # 일봉 종가 = bars_all_pit + 분봉 마지막 종가(같은 날은 분봉 원가격 우선)
    calendar = pd.read_parquet(KOSPI_INDEX).index
    daily = pd.read_parquet(DAILY_PARQUET, columns=["Date", "code", "Close"])
    daily = daily[daily["code"].isin(set(tickers))].rename(columns={"Date": "day", "code": "ticker", "Close": "close"})
    minute_close = panel.drop_duplicates(["ticker", "day"])[["ticker", "day", "close"]]
    closes = pd.concat([daily, minute_close]).drop_duplicates(["ticker", "day"], keep="last")
    wide = closes.pivot(index="day", columns="ticker", values="close").reindex(calendar)
    previous_close = wide.shift(1)
    averages = {window: wide.rolling(window, min_periods=window).mean().shift(1) for window in (5, 10, 20, 60)}

    def lookup(frame: pd.DataFrame, name: str) -> pd.DataFrame:
        stacked = frame.stack().rename(name).reset_index()
        stacked.columns = ["day", "ticker", name]
        return stacked

    extra = lookup(previous_close, "prev_close")

    for window, frame in averages.items():
        extra = extra.merge(lookup(frame, f"sma{window}"), on=["day", "ticker"], how="outer")

    panel = panel.merge(extra, on=["day", "ticker"], how="left")
    OUTPUT.parent.mkdir(exist_ok=True)
    panel.sort_values(["day", "minute", "ticker"]).reset_index(drop=True).to_parquet(OUTPUT)
    print(f"rows {len(panel):,} · days {panel['day'].nunique()} · tickers {panel['ticker'].nunique()} → {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
