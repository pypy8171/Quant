"""스터디 25 요약 — 개장 이격 구제 격자의 변형별 한 줄 표 + 구제가 실제로 몇 번 열렸는지 진단.

`summarize_grid.py`(스터디 24)와 같은 열에, 이 스터디에서만 필요한 두 가지를 더 센다.
  · 이격 조건이 **후보 종목일**(정배열 + ATR≤4 통과) 중 몇 개를 막는가 — 구제의 최대 사정거리.
  · 구제가 몇 종목일을 열었고 그중 몇 건이 실제 매수로 이어졌는가.

사용: py -X utf8 research/studies/25_devscale_open_dev_rescue/summarize_rescue.py \
          research/studies/25_devscale_open_dev_rescue/rescue_grid.tsv
산출: 같은 폴더의 rescue_grid_summary.txt(표)와 metrics.json(숫자).
"""
import json
import sys
from pathlib import Path

import pandas as pd

BASE = "base_0903"
ATR_MAX = 4.0
DEVIATION_MIN = -1.0


def main() -> int:
    path = Path(sys.argv[1])
    days = pd.read_csv(path.with_name(path.stem + "_days.tsv"), sep="\t", low_memory=False)
    days["month"] = days.ymd.astype(str).str[:6]

    rows = []
    for variant, group in days.groupby("variant", sort=False):
        buys = int(group.buys.sum())
        turnover = float(group.buy_notional.sum())
        net = float(group.net_pnl.sum())
        by_month = group.groupby("month").agg(net=("net_pnl", "sum"), deployed=("max_deployed", "sum"))
        rows.append({"variant": variant, "buys": buys,
                     "gross_pct": float(group.gross_pnl.sum()) / turnover * 100.0 if turnover else 0.0,
                     "net_pct": net / turnover * 100.0 if turnover else 0.0,
                     "net_per_buy": net / buys if buys else 0.0,
                     "months_pos": int((by_month.net > 0).sum()), "months": int(len(by_month)),
                     "net_total_man": net / 10_000.0,
                     "worst_month_man": float(by_month.net.min()) / 10_000.0,
                     "rescued_days": int(group.rescued.fillna(False).astype(bool).sum())})
    table = pd.DataFrame(rows).sort_values("variant")
    pd.set_option("display.width", 220)
    text = table.to_string(index=False, float_format=lambda x: f"{x:.3f}")
    print(text)
    path.with_name(path.stem + "_summary.txt").write_text(text + "\n", encoding="utf-8")

    base = days[days.variant == BASE]
    candidate = base[(base.aligned_prev == True) & (base.atr14_pct <= ATR_MAX)]   # noqa: E712 (pandas 마스크)
    blocked = candidate[candidate.open_dev_pct < DEVIATION_MIN]
    metrics = {"ticker_days": int(len(base)),
               "candidate_days_aligned_atr_ok": int(len(candidate)),
               "blocked_by_open_deviation": int(len(blocked)),
               "blocked_share_of_candidates": round(len(blocked) / max(len(candidate), 1), 4),
               "variants": {row["variant"]: {key: row[key] for key in
                            ("buys", "net_pct", "net_per_buy", "months_pos", "net_total_man", "worst_month_man", "rescued_days")}
                            for row in rows}}
    for variant in sorted(days.variant.unique()):
        if variant == BASE:
            continue
        group = days[days.variant == variant]
        rescued = group[group.rescued.fillna(False).astype(bool)]
        metrics.setdefault("rescue_detail", {})[variant] = {
            "rescued_days": int(len(rescued)), "rescued_days_aligned": int(rescued.aligned_prev.sum()),
            "rescued_days_with_buy": int((rescued.buys > 0).sum()),
            "buys_delta_vs_base": int(group.buys.sum() - base.buys.sum()),
            "net_delta_won": round(float(group.net_pnl.sum() - base.net_pnl.sum()))}
    path.with_name("metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n" + json.dumps(metrics["rescue_detail"], ensure_ascii=False, indent=2))
    print(f"후보 종목일 {metrics['candidate_days_aligned_atr_ok']}개 중 이격으로 막힌 날 "
          f"{metrics['blocked_by_open_deviation']}개({metrics['blocked_share_of_candidates'] * 100:.1f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
