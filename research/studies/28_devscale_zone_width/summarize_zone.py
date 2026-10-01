"""존 폭 격자(devscale_replay.py --set zone_width, 스터디 28) 종목일 표를 변형 한 줄로 줄이고, 기간 앞/뒤 반으로 나눠 순위가 유지되는지 본다.

열: 매수 건수, 건당 세후 %(매수 금액 대비), 1년 세후 총액(만), 플러스 달/전체 달, 최악 달(만), 청산 사유 비중(존 이탈·손절·익절),
    하루 동시 보유 종목 수(평균·90분위 — 라이브 동시 4종목 상한이 얼마나 걸리는지; 하네스는 상한을 모델링하지 않는다).
앞/뒤 반: 2025-09-19~2026-03-19 / 2026-03-20~2026-09-18(달력 가운데). 넘긴 보유의 손익은 판 날에 붙는다.

    py -X utf8 research/studies/28_devscale_zone_width/summarize_zone.py research/studies/28_devscale_zone_width/zone_grid.tsv
산출: 같은 폴더 zone_grid_summary.tsv·zone_grid_halves.tsv·metrics.json, 콘솔에 표.
"""
import argparse
import json
from pathlib import Path

import pandas as pd

SPLIT_YMD = "20260320"


def _exit_counts(group: pd.DataFrame) -> dict[str, int]:
    counts: dict[str, int] = {}
    for text in group.exits:
        for tag, value in json.loads(text).items():
            counts[tag] = counts.get(tag, 0) + int(value)
    return counts


def summarize(days: pd.DataFrame) -> pd.DataFrame:
    rows = []
    for variant, group in days.groupby("variant", sort=False):
        buys = int(group.buys.sum())
        turnover = float(group.buy_notional.sum())
        net = float(group.net_pnl.sum())
        gross = float(group.gross_pnl.sum())
        by_month = group.groupby("month").net_pnl.sum()
        exits = _exit_counts(group)
        sells = sum(exits.values())
        held = group[group.max_deployed > 0].groupby("ymd").ticker.nunique()
        rows.append({"variant": variant, "buys": buys,
                     "net_pct": net / turnover * 100.0 if turnover else 0.0,
                     "gross_pct": gross / turnover * 100.0 if turnover else 0.0,
                     "net_total_man": net / 10_000.0,
                     "months_pos": int((by_month > 0).sum()), "months": int(len(by_month)),
                     "worst_month_man": float(by_month.min()) / 10_000.0 if len(by_month) else 0.0,
                     "zone_exit_share": exits.get("zone_exit", 0) / sells if sells else 0.0,
                     "stop_share": exits.get("stop", 0) / sells if sells else 0.0,
                     "tp_share": exits.get("tp", 0) / sells if sells else 0.0,
                     "held_mean": float(held.mean()) if len(held) else 0.0,
                     "held_p90": float(held.quantile(0.9)) if len(held) else 0.0})
    return pd.DataFrame(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv")
    arguments = parser.parse_args()
    path = Path(arguments.tsv)
    days = pd.read_csv(path.with_name(path.stem + "_days.tsv"), sep="\t", dtype={"ticker": str, "ymd": str})
    days["month"] = days.ymd.str[:6]
    full = summarize(days)
    front = summarize(days[days.ymd < SPLIT_YMD]).set_index("variant")
    back = summarize(days[days.ymd >= SPLIT_YMD]).set_index("variant")
    halves = pd.DataFrame({"front_buys": front.buys, "front_net_pct": front.net_pct, "front_net_man": front.net_total_man,
                           "back_buys": back.buys, "back_net_pct": back.net_pct, "back_net_man": back.net_total_man})
    grid = halves[halves.index.str.startswith("z_")].copy()
    for column in ("front_net_pct", "back_net_pct", "front_net_man", "back_net_man"):
        grid[column.replace("net", "rank")] = grid[column].rank(ascending=False).astype(int)
    rank_correlation_percent = float(grid.front_net_pct.rank().corr(grid.back_net_pct.rank()))   # 순위의 피어슨 = 스피어만(scipy 없이)
    rank_correlation_man = float(grid.front_net_man.rank().corr(grid.back_net_man.rank()))

    full.to_csv(path.with_name(path.stem + "_summary.tsv"), sep="\t", index=False, float_format="%.4f")
    grid.reset_index().rename(columns={"index": "variant"}).to_csv(path.with_name(path.stem + "_halves.tsv"), sep="\t",
                                                                   index=False, float_format="%.4f")
    metrics = {"ticker_days": int(days[days.variant == full.variant.iloc[0]].shape[0]), "split_ymd": SPLIT_YMD,
               "spearman_front_back_net_pct": rank_correlation_percent, "spearman_front_back_net_man": rank_correlation_man,
               "variants": full.set_index("variant").to_dict(orient="index")}
    path.with_name("metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=1), encoding="utf-8")
    pd.set_option("display.width", 240)
    print(full.sort_values("net_pct", ascending=False).to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    print()
    print(grid.sort_values("front_rank_pct").to_string(float_format=lambda x: f"{x:.3f}"))
    print(f"\nSpearman 앞/뒤 순위 상관: 건당 % {rank_correlation_percent:.3f} · 총액 {rank_correlation_man:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
