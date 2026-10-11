"""후보 마스크 점검: 신호 수·라이브 재현·진단 3 개(성과 판정 아님)."""
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_round3 as round3     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
import entry_candidates as entry_candidates     # noqa: E402

round2, exit_test = round3.round2, round3.exit_test
started = time.time()
data = exit_test.Data()
panel = data.panel
engine = round2.Engine(data)
flags = round2.flags_of(data, engine)
features = entry_candidates.Features(data)
print(f"준비 {time.time() - started:.0f}초 · 수급 덮는 비율(신호 기간 행) {features.flow_coverage:.3f}", flush=True)
rows = []

for board, extra in round3.BOARDS:
    context = entry_candidates.Context(data, engine, flags, features, extra)

    for name, label, build in entry_candidates.VARIANTS:
        entries, approach, view = build(context)
        days = panel.market_position[entries]
        per_day = np.bincount(days, minlength=panel.market_count)
        active = per_day[per_day > 0]
        rows.append({"board": board, "candidate": name, "variant": label, "signals": len(entries),
                     "days_with_signal": int((per_day > 0).sum()), "median_per_day": float(np.median(active)) if len(active) else 0.0,
                     "stocks": int(len(np.unique(panel.stock_of_row[entries]))),
                     "inside_below_above": np.round(np.bincount(approach, minlength=3) / max(len(approach), 1), 3).tolist(),
                     "r60_rank_mean": float(np.nanmean(features.r60_rank_prev[entries])),
                     "r5_rank_mean": float(np.nanmean(features.r5_rank_prev[entries])),
                     "rv20_rank_mean": float(np.nanmean(features.rv20_rank_prev[entries])),
                     "turnover20_median_억": float(np.nanmedian(data.previous_20d[entries]) / 1e8)})
    print(f"{board} 끝 {time.time() - started:.0f}초", flush=True)

table = pd.DataFrame(rows)
pd.set_option("display.width", 250)
print(table.to_string(index=False))

# 진단 1: 장중(시가→종가)과 밤(전날 종가→시가) 평균 수익, U2 풀 동일가중, 연도별
pool = entry_candidates.board_pool(data, flags, round3.BOARDS[0][1]) & data.previous_traded
previous_close = entry_candidates.previous_known(data.close, data)

with np.errstate(invalid="ignore", divide="ignore"):
    intraday = data.close / data.open - 1.0
    overnight = data.open / previous_close - 1.0

frame = pd.DataFrame({"year": panel.years[pool], "intraday": intraday[pool], "overnight": overnight[pool]})
by_year = frame.groupby("year")[["intraday", "overnight"]].mean() * 100.0
print("진단1 U2 풀 행 평균(%) 전체", (frame[["intraday", "overnight"]].mean() * 100.0).round(4).to_dict())
print(by_year.round(3).to_string())

# 진단 2: 라이브 재현 신호의 전날 60일·5일 수익 백분위 분포(대조 풀과 비교)
context = entry_candidates.Context(data, engine, flags, features, round3.BOARDS[0][1])
live_entries, _, _ = entry_candidates.VARIANTS[0][2](context)
for label, rows_ in (("라이브 신호", live_entries), ("U2 풀", np.flatnonzero(context.pool))):
    r60 = features.r60_rank_prev[rows_]
    round5 = features.r5_rank_prev[rows_]
    print(f"진단2 {label}: 60일 백분위 평균 {np.nanmean(r60):.3f} · 상위30% 비중 {np.nanmean(r60 >= 0.7):.3f} · "
          f"5일 백분위 평균 {np.nanmean(round5):.3f} · 하위20% 비중 {np.nanmean(round5 <= 0.2):.3f} · 변동성 백분위 평균 "
          f"{np.nanmean(features.rv20_rank_prev[rows_]):.3f}")

# 진단 3: 라이브 재현 신호의 진입가 이격(전날 SMA20 대비)
with np.errstate(invalid="ignore", divide="ignore"):
    view = entry_candidates.ZoneView(data, engine, exit_test.PULLBACK, 0.05)
    deviation = view.entry_mid[live_entries] / data.sma20_previous[live_entries] - 1.0
print("진단3 진입가 ÷ 전날 SMA20 − 1 분위(%)", np.round(np.nanpercentile(deviation, [10, 25, 50, 75, 90]) * 100.0, 2).tolist(),
      "· 0 위 비중", round(float(np.nanmean(deviation > 0)), 3))
table.to_csv(Path(__file__).resolve().parent / "candidate_counts.csv", index=False, encoding="utf-8-sig")
print(f"끝 {time.time() - started:.0f}초")
