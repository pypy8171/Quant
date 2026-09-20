"""격자 종목일 표(*_days.tsv, devscale_replay.py 산출)를 변형 하나당 한 줄로 줄인다. 인자는 월 표 경로(짝 _days.tsv를 읽는다).
월 표는 매수 없는 날(넘긴 보유를 판 날)을 빼므로 넘김 변형에서는 종목일 표로 세야 맞다.

열: 매수 수, 비용 전 손익(매수 금액 대비 %), 세후 손익(%), 세후/건(원), 플러스 달/전체 달, 세후 총액(만), 평균 투입(만).
사용: py -X utf8 research/studies/24_devscale_exit_lines/summarize_grid.py <tsv> [--top N] [--sort net_pct]
"""
import argparse
from pathlib import Path

import pandas as pd


def summarize(path: Path) -> pd.DataFrame:
    days = pd.read_csv(path.with_name(path.stem + "_days.tsv"), sep="\t")
    days["month"] = days.ymd.astype(str).str[:6]
    rows = []
    for variant, group in days.groupby("variant", sort=False):
        buys = int(group.buys.sum())
        turnover = float(group.buy_notional.sum())
        gross, net = float(group.gross_pnl.sum()), float(group.net_pnl.sum())
        by_month = group.groupby("month").agg(net=("net_pnl", "sum"), deployed=("max_deployed", "sum"))
        rows.append({"variant": variant, "buys": buys,
                     "gross_pct": gross / turnover * 100.0 if turnover else 0.0,
                     "net_pct": net / turnover * 100.0 if turnover else 0.0,
                     "net_per_buy": net / buys if buys else 0.0,
                     "months_pos": int((by_month.net > 0).sum()), "months": int(len(by_month)),
                     "net_total_man": net / 10_000.0,
                     "deployed_avg_man": float(by_month.deployed.mean()) / 10_000.0 / 21.0})   # 월 합계를 거래일 21로 나눠 하루 평균
    return pd.DataFrame(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv")
    parser.add_argument("--top", type=int, default=0)
    parser.add_argument("--sort", default="net_pct")
    arguments = parser.parse_args()
    table = summarize(Path(arguments.tsv)).sort_values(arguments.sort, ascending=False)
    if arguments.top:
        table = table.head(arguments.top)
    pd.set_option("display.width", 200)
    print(table.to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
