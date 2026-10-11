"""스터디 38 일봉 패널 — 2009-01부터 보통주 일봉을 (종목 × 날짜) 배열로 펴고, 날짜마다 구간 순위를 붙인다.

왜: 손절 시험(daily_stop_grid.py)은 16년 × 수천 종목을 종목별 순서대로 시뮬레이션한다. 판다스 행 단위로는 느려서
배열로 한 번 펴 두고(npz 캐시) 시험기가 numba로 읽는다.

종목 범위: 보통주 전부(features.fundamental 의 is_common_stock 과 같은 판정, 상장폐지 포함). 미리 거르지 않는다.
  대신 날짜마다 전날 값으로 아래 순위를 매겨 두고, 매매를 다 낸 뒤 구간별로 나눠 본다.
  - 주가 10분위, 시가총액 10분위(상장주식수가 2016년부터만 있어 그 전은 −1), 거래대금 평균 10분위(5·10·20·60일)
  - in_1500: 사용자 지시(2026-10-10)의 "의미 있는 1,500종목" — 20일 거래대금·20일 거래량·시가총액 순위 중 가장 좋은
    순위로 줄 세운 1,500개. 결과를 나누는 한 구간으로만 쓴다.
[limit] 일봉 가격은 수정주가, 거래량은 수정 전 값이다. 액면분할 종목은 분할 전 거래대금(수정 종가 × 원 거래량)이
  실제보다 작게 잡힌다. 호가 단위도 수정주가에 매기므로 분할 전 구간은 실제와 다르다.
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "PYQuant"))

from features.fundamental import NON_COMMON_NAME_TOKENS, load_etf_tokens, load_shares  # noqa: E402

BARS_PATH = ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
DATA_DIR = Path(__file__).resolve().parent / "data"
PANEL_PATH = DATA_DIR / "daily_panel.npz"
PANEL_START = "2009-01-01"
TURNOVER_WINDOWS = (5, 10, 20, 60)
SIZE_1500 = 1500
WINDOW_1500 = 20
NO_RANK = np.iinfo(np.int32).max


def rank_within(values: np.ndarray, eligible: np.ndarray) -> np.ndarray:
    """행(날짜)마다 적격 종목 안에서 큰 값이 0위. 부적격·값 없음은 NO_RANK."""
    valid = eligible & np.isfinite(values)
    filled = np.where(valid, values, -np.inf)
    order = np.argsort(-filled, axis=1, kind="stable")
    ranks = np.empty_like(order)
    ranks[np.arange(order.shape[0])[:, None], order] = np.arange(order.shape[1])[None, :]
    return np.where(valid, ranks, NO_RANK).astype(np.int64)


def decile(values: np.ndarray, eligible: np.ndarray) -> np.ndarray:
    """행마다 적격 종목의 10분위(0 = 가장 작음, 9 = 가장 큼). 값 없으면 −1."""
    ranks = rank_within(values, eligible)
    valid = ranks < NO_RANK
    count = valid.sum(axis=1, keepdims=True)
    position = (count - 1 - ranks) / np.maximum(count, 1)  # 0 = 가장 작음
    return np.where(valid, np.minimum((position * 10).astype(np.int64), 9), -1).astype(np.int8)


def rolling_mean(values: np.ndarray, window: int) -> np.ndarray:
    return pd.DataFrame(values).rolling(window, min_periods=window).mean().to_numpy()


def build() -> None:
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "Open", "High", "Low", "Close", "Volume", "code", "name", "market"])
    bars = bars[bars["Date"] >= PANEL_START]
    tokens = tuple(load_etf_tokens()) + NON_COMMON_NAME_TOKENS
    last = bars.sort_values("Date").groupby("code")[["name", "market"]].last()
    common = last.index.str.endswith("0") & ~last["name"].fillna("").apply(lambda text: any(name_token in text for name_token in tokens)).values
    bars = bars[bars["code"].isin(set(last.index[common]))]

    dates = pd.DatetimeIndex(sorted(bars["Date"].unique()))
    tickers = np.array(sorted(bars["code"].unique()))
    wide = {column: bars.pivot(index="Date", columns="code", values=column).reindex(index=dates, columns=tickers)
            .to_numpy(float) for column in ["Open", "High", "Low", "Close", "Volume"]}
    close = wide["Close"]
    volume = np.nan_to_num(wide["Volume"])
    traded = np.isfinite(close)
    halted = traded & ((wide["Open"] <= 0) | (volume <= 0))
    # 거래 없는 날(시가·고가·저가 0)은 전일 종가 한 값으로 둔다 — 그날은 아무 선도 닿지 않는다.
    open_ = np.where(halted, close, wide["Open"])
    high = np.where(halted, close, wide["High"])
    low = np.where(halted, close, wide["Low"])
    big_move = traded & (np.abs(close / pd.DataFrame(close).ffill().shift(1).to_numpy() - 1) > 0.35)

    shares = load_shares()
    shares = shares[shares["ticker"].isin(set(tickers))].sort_values("effective_date")
    shares_wide = shares.pivot_table(index="effective_date", columns="ticker", values="shares_common_issued",
                                     aggfunc="last").reindex(columns=tickers)
    shares_wide = shares_wide.reindex(shares_wide.index.union(dates)).ffill().reindex(dates).to_numpy(float)
    market_cap = shares_wide * close
    turnover = np.where(traded, close * volume, np.nan)
    volume_traded = np.where(traded, volume, np.nan)

    # 순위는 그날 거래된 종목 사이에서 매기고, 날짜 t 에는 t-1 행을 붙인다(그날 아침에 알 수 있던 값).
    columns = {"price_decile": decile(close, traded), "cap_decile": decile(market_cap, traded)}

    for window in TURNOVER_WINDOWS:
        columns[f"turnover{window}_decile"] = decile(rolling_mean(turnover, window), traded)

    best_rank = np.minimum(np.minimum(rank_within(rolling_mean(turnover, WINDOW_1500), traded),
                                      rank_within(rolling_mean(volume_traded, WINDOW_1500), traded)),
                           rank_within(market_cap, traded))
    in_1500 = np.zeros_like(traded)

    for row in range(len(dates)):
        candidates = np.flatnonzero(best_rank[row] < NO_RANK)

        if len(candidates) <= SIZE_1500:
            in_1500[row, candidates] = True
            continue

        cutoff = np.partition(best_rank[row, candidates], SIZE_1500 - 1)[SIZE_1500 - 1]
        in_1500[row, candidates[best_rank[row, candidates] <= cutoff]] = True

    columns["in_1500"] = in_1500

    def shifted(array):
        result = np.full_like(array, -1 if array.dtype == np.int8 else False)
        result[1:] = array[:-1]
        return result.T.copy()

    market_codes = {"KOSPI": 0, "KOSDAQ": 1, "KOSDAQ GLOBAL": 1}
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        PANEL_PATH, dates=dates.strftime("%Y-%m-%d").to_numpy(), tickers=tickers,
        names=last["name"].reindex(tickers).fillna("").to_numpy(),
        market=last["market"].reindex(tickers).map(market_codes).fillna(1).to_numpy(np.int64),
        open=open_.T.copy(), high=high.T.copy(), low=low.T.copy(), close=close.T.copy(), halted=halted.T.copy(),
        big_move=big_move.T.copy(), **{name: shifted(array) for name, array in columns.items()})
    print(f"종목 {len(tickers)}개 · 날짜 {len(dates)}일 · ±35% 넘는 봉 {int(big_move.sum())}개 → {PANEL_PATH}")


if __name__ == "__main__":
    build()
