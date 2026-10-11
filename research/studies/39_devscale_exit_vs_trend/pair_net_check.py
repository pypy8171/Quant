"""스터디 39 사후 진단(사전 등록 밖) — 같은 진입마다 (칸 순수익 − (a) 순수익)을 대조 없이 잰다.

판정 값(순수익 − 대조)의 짝 차이는 보유가 긴 칸일수록 대조(같은 구간 전 종목, 시가 매수)가 더 깎여 커진다.
대조를 빼지 않은 순수익 짝 차이와, 종목 묶음 t 를 같이 낸다. 입력은 run_exit_test.py 가 쓴 results/trades.parquet.
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/pair_net_check.py
"""
from __future__ import annotations

import sys
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY.parent / "38_trend_support_realtime"))

import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402

import round6_test     # noqa: E402

RESULTS = STUDY / "results"


def main() -> None:
    trades = pd.read_parquet(RESULTS / "trades.parquet")
    rules = pd.read_csv(RESULTS / "rules.csv")
    live_cell = int(rules.loc[rules["rule"].str.startswith("(a) "), "cell"].iloc[0])
    cells = int(trades["cell"].max()) + 1
    net = trades["net_pct"].to_numpy(np.float64).reshape(-1, cells)
    value = trades["value_pct"].to_numpy(np.float64).reshape(-1, cells)
    first = trades[trades["cell"] == 0]
    month = ((first["entry_date"].to_numpy() // 10000 - 2010) * 12 + (first["entry_date"].to_numpy() // 100) % 100 - 1)
    stock = pd.factorize(first["code"])[0]
    rows = []

    for cell in range(cells):
        if cell == live_cell:
            continue

        net_pair = net[:, cell] - net[:, live_cell]
        value_pair = value[:, cell] - value[:, live_cell]
        rows.append({"cell": cell, "rule": rules.at[cell, "rule"],
                     "net_pair_mean": float(net_pair.mean()), "net_pair_median": float(np.median(net_pair)),
                     "net_pair_t_month": round6_test.month_cluster_t(net_pair, month),
                     "net_pair_t_stock": round6_test.month_cluster_t(net_pair, stock),
                     "value_pair_mean": float(value_pair.mean()),
                     "value_pair_t_month": round6_test.month_cluster_t(value_pair, month),
                     "value_pair_t_stock": round6_test.month_cluster_t(value_pair, stock),
                     "share_entries_cell_better": float(np.mean(net_pair > 0))})

    common.write_csv(RESULTS / "pair_net_check.csv", pd.DataFrame(rows))


if __name__ == "__main__":
    main()
