"""스터디 40-4 체결 건(씨앗 0)에 손실 원인 후보를 붙여 대시보드 '문제 종목' 탭 데이터로 쓴다.

문제 건 = 합친 순수익이 0 미만인 체결 건 전부. 원인 후보는 체결 건마다 참·거짓으로 매긴다(TAGS). 탭은 문제 건에서 각 후보가 전체 건보다 얼마나 자주 나오는지 보여 준다.
산출: research/studies/38_trend_stop_intraday/dashboard/data/problems40.txt(원인 후보·통계), problems40/<규칙>.txt(문제 건)
사용: py research/studies/40_devscale_entry_touch/build_problem_dashboard.py  (build_split_dashboard.py 다음에)
"""
import base64
import gzip
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402
import daily_stop_grid as grid  # noqa: E402
from turn_points import market_index  # noqa: E402

# 비트 순서 = 탭의 원인 후보 순서. 문턱값은 손실 건을 사람이 나눠 보기 위한 구분선이고 최적화한 값이 아니다.
TAGS = [
    ("rise20", "매수 전 20일에 +20% 넘게 오른 뒤 삼"),
    ("high_vs_low20", "직전 20일 저점보다 +15% 넘게 높은 가격에 삼"),
    ("market_down", "시장지수가 60일 평균 아래일 때 삼"),
    ("quick5", "매수 당일·다음 날 바로 5일선 아래로 마감"),
    ("never_up", "매수 뒤 고가가 한 번도 +3%에 못 감"),
    ("gave_back", "+10% 넘게 올랐다가 반납하고 손실"),
    ("gap_down", "보유 중 시가가 전일 종가보다 −10% 넘게 빠진 날 있음"),
    ("not_aligned60", "매수 전일 5>10>20>60 정배열 아님"),
]
KEEP = ["ticker", "turn_t", "entry_t", "entry_price", "exit5_t", "exit5_price", "exit5_net", "exit10_t", "exit10_price",
        "exit10_net", "exit20_t", "exit20_price", "exit20_net", "exit_t", "net", "hold", "rise20", "high_vs_low20", "best",
        "worst", "tags"]


def read_split() -> dict:
    return json.loads(gzip.decompress(base64.b64decode((board.OUT / "split40.txt").read_bytes())))


def features(trades: pd.DataFrame, panel: grid.Panel, close_filled: np.ndarray, market_below: np.ndarray) -> pd.DataFrame:
    """체결 건마다 원인 후보 값과 비트를 단다."""
    rows = []

    for ticker, entry_t, entry_price, exit5_t, exit_t, net, aligned60 in trades[
            ["ticker", "entry_t", "entry_price", "exit5_t", "exit_t", "net", "aligned60"]].itertuples(index=False):
        before = close_filled[ticker, max(entry_t - 21, 0)]
        rise20 = close_filled[ticker, entry_t - 1] / before - 1 if before > 0 else np.nan
        low20 = np.nanmin(panel.low[ticker, max(entry_t - 20, 0):entry_t]) if entry_t > 0 else np.nan
        high_vs_low20 = entry_price / low20 - 1 if low20 > 0 else np.nan
        held = slice(entry_t, exit_t + 1)
        best = np.nanmax(panel.high[ticker, held]) / entry_price - 1
        worst = np.nanmin(panel.low[ticker, held]) / entry_price - 1
        opens = panel.open[ticker, entry_t + 1:exit_t + 1]
        previous = close_filled[ticker, entry_t:exit_t]
        gaps = opens / previous - 1 if len(opens) else np.array([0.0])
        worst_gap = np.nanmin(gaps) if np.isfinite(gaps).any() else 0.0
        flags = [rise20 > 0.20, high_vs_low20 > 0.15, bool(market_below[entry_t]), exit5_t - entry_t <= 1,
                 best < 0.03, best > 0.10 and net < 0, worst_gap < -0.10, aligned60 != 1]
        rows.append({"rise20": rise20 * 100, "high_vs_low20": high_vs_low20 * 100, "best": best * 100,
                     "worst": worst * 100, "worst_gap": worst_gap * 100,
                     "tags": sum(1 << bit for bit, flag in enumerate(flags) if flag)})

    return pd.DataFrame(rows, index=trades.index)


def tag_statistics(name: str, trades: pd.DataFrame, losers: pd.Series) -> list[dict]:
    """원인 후보마다: 전체 건 중 비율, 문제 건 중 비율, 해당 건·아닌 건의 건당 순수익."""
    rows = []

    for bit, (key, _) in enumerate(TAGS):
        has = (trades["tags"].to_numpy() & (1 << bit)) > 0
        rows.append({"rule": name, "tag": key, "share_all": has.mean() * 100,
                     "share_losers": has[losers.to_numpy()].mean() * 100,
                     "net_with": trades["net"][has].mean() if has.any() else np.nan,
                     "net_without": trades["net"][~has].mean() if (~has).any() else np.nan,
                     "loss_sum_with": trades["net"][has & losers.to_numpy()].sum(),
                     "count": int(has.sum())})

    return rows


def main() -> None:
    panel = grid.Panel()
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    market = pd.Series(market_index(panel))
    market_below = (market < market.rolling(60).mean()).shift(1, fill_value=False).to_numpy()
    split = read_split()
    out, statistics_rows = {}, []

    for name, columns in split["trades"].items():
        trades = pd.DataFrame(columns)
        trades = pd.concat([trades, features(trades, panel, close_filled, market_below)], axis=1)
        losers = trades["net"] < 0
        statistics_rows += tag_statistics(name, trades, losers)
        out[name] = int(losers.sum())
        board.write_json(f"problems40/{name}.txt", board.clean(trades.loc[losers, KEEP].sort_values("net")), compress=True)
        print(f"  {name} 체결 {len(trades)}건 중 문제 건 {losers.sum()}건", flush=True)

    payload = {"tags": [{"key": key, "label": label} for key, label in TAGS], "stats": board.clean(pd.DataFrame(statistics_rows)),
               "loser_count": out}
    print(f"→ problems40.txt {board.write_json('problems40.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
