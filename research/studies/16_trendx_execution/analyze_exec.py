#!/usr/bin/env python3
"""검증 1·2의 3분봉 리플레이 결과를 기저(현행 라이브)와 짝지어 비교한다.

입력  research/studies/16_trendx_execution/exec_replay_days.tsv
      (PYQuant/backtest/devscale_replay.py --set exec 가 만든 (변형,종목,날짜) 행)
출력  research/studies/16_trendx_execution/exec_paired.tsv

단위는 (종목,날짜) 하루 r. 기저 e0_base_stop2.5(=config_dev_paper TRENDX: buy_split_steps=0·stop 2.5%)에서
한 번에 한 가지만 바꾼 변형과 짝지어 차이를 낸다. 진입이 없던 날도 r=0으로 남겨야 "진입을 미뤄
안 산 날"이 이득/손실로 제대로 세어진다.

검정 둘을 같이 낸다(어느 쪽으로 봐도 같은 결론인지 보려고).
  t_month  날짜 평균 → 월 평균 → 월 시계열 1표본 t (13_trendx_gate excess_r와 같은 집계)
  t_day    날짜 평균 시계열에 1표본 t (표본 월이 9개뿐이라 병기)

    py research/studies/16_trendx_execution/analyze_exec.py
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

import pandas as pd

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parents[2]
sys.path.insert(0, str(_ROOT / "research" / "studies" / "13_trendx_gate"))
from stats_util import one_sample_t  # noqa: E402

BASE = "e0_base_stop2.5"
DAYS = _HERE / "exec_replay_days.tsv"
R_DENOM_PCT = 6.0
# 리플레이는 engine.CostModel(왕복 0.265% = 매수 0.02 + 매도 0.245)을 쓰는데 13번 일봉 스터디의
# 정본 왕복비용은 0.31%다(D-064). 차액 0.045%p를 매도 명목에 얹어 비용 감도를 같이 낸다.
COST_GAP = 0.00045
# 분봉 백필이 2025-09-11부터라 2022 홀드아웃은 못 돈다. 있는 구간을 앞뒤로 갈라 최소한의 구간 분리만 한다.
WINDOWS = {
    "full":    ("20250911", "20991231"),
    "2025H2":  ("20250911", "20251229"),
    "2026now": ("20251230", "20991231"),
}


def exit_rates(s: pd.Series) -> dict[str, float]:
    cnt: Counter = Counter()
    for v in s:
        if isinstance(v, str) and v.strip():
            cnt.update(json.loads(v))
    tot = sum(cnt.values())
    return {f"exit_{k}": v / tot for k, v in cnt.items()} if tot else {}


def main() -> int:
    d = pd.read_csv(DAYS, sep="\t", dtype={"ticker": str, "ymd": str})
    dep = d["max_deployed"].where(d["max_deployed"] > 0)
    d["net_adj"] = d["net_pnl"] - COST_GAP * d["sell_notional"]
    d["r_adj"] = (d["net_adj"] / dep * 100.0).fillna(0.0) / R_DENOM_PCT
    d["day_pct"] = (d["net_pnl"] / dep * 100.0).fillna(0.0)
    base = d[d["variant"] == BASE].set_index(["ticker", "ymd"])
    rows = []
    for name, g in d.groupby("variant", sort=True):
        g = g.set_index(["ticker", "ymd"])
        diff = (g["r"] - base["r"].reindex(g.index))
        for win, (lo, hi) in WINDOWS.items():
            ymd = g.index.get_level_values("ymd")
            m = (ymd >= lo) & (ymd <= hi)
            gg, dd = g[m], diff[m]
            if gg.empty:
                continue
            day_key = gg.index.get_level_values("ymd")
            daily_diff = dd.groupby(day_key).mean()
            month_diff = daily_diff.groupby(daily_diff.index.str[:6]).mean()
            dmean, dt, dp, dn = one_sample_t(month_diff.to_numpy())
            _, dt_day, dp_day, dn_day = one_sample_t(daily_diff.to_numpy())
            adj = gg["r_adj"] - base["r_adj"].reindex(gg.index)
            adj_daily = adj.groupby(day_key).mean()
            adj_month = adj_daily.groupby(adj_daily.index.str[:6]).mean()
            amean, at, ap, _ = one_sample_t(adj_month.to_numpy())
            traded = gg[gg["buys"] > 0]
            wins = traded.loc[traded["day_pct"] > 0, "day_pct"]
            loss = traded.loc[traded["day_pct"] < 0, "day_pct"]
            avg_win = float(wins.mean()) if len(wins) else float("nan")
            avg_loss = float(-loss.mean()) if len(loss) else float("nan")
            rows.append({
                "variant": name, "window": win, "n_pairs": int(len(gg)),
                "n_days": int(pd.Series(day_key).nunique()), "n_months": dn,
                "traded_days": int(len(traded)), "entry_rate": float((gg["buys"] > 0).mean()),
                "mean_r": float(gg["r"].mean()), "mean_r_traded": float(traded["r"].mean()) if len(traded) else 0.0,
                "win_rate": float((traded["net_pnl"] > 0).mean()) if len(traded) else float("nan"),
                "net_sum": float(gg["net_pnl"].sum()), "deployed_sum": float(gg["max_deployed"].sum()),
                "avg_win_pct": avg_win, "avg_loss_pct": avg_loss,
                "breakeven_wr": avg_loss / (avg_win + avg_loss) if avg_win + avg_loss > 0 else float("nan"),
                "paired_dr": dmean, "t_month": dt, "p_month": dp,
                "paired_dr_cost031": amean, "t_month_cost031": at, "p_month_cost031": ap,
                "t_day": dt_day, "p_day": dp_day, "n_days_test": dn_day,
                "beat_months": float((month_diff > 0).mean()) if dn else float("nan"),
                "mae_med": float(traded["mae_pct"].median()) if len(traded) else float("nan"),
                "buy_fills": int(gg["buys"].sum()), "sell_fills": int(gg["sells"].sum()),
                **exit_rates(gg["exits"]),
            })
    res = pd.DataFrame(rows)
    res["net_over_deployed_pct"] = res["net_sum"] / res["deployed_sum"] * 100.0
    res.to_csv(_HERE / "exec_paired.tsv", sep="\t", index=False, float_format="%.5f")
    pd.set_option("display.width", 280)
    pd.set_option("display.max_rows", 200)
    cols = ["variant", "window", "n_pairs", "n_days", "n_months", "entry_rate", "mean_r", "win_rate",
            "breakeven_wr", "net_over_deployed_pct", "paired_dr", "t_month", "p_month", "t_day", "p_day",
            "beat_months", "mae_med", "exit_stop", "exit_tp", "exit_market_close", "exit_zone_exit"]
    cols = [c for c in cols if c in res.columns]
    print(res[cols].to_string(index=False, float_format=lambda x: f"{x:.4f}"))
    print(f"[done] {_HERE / 'exec_paired.tsv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
