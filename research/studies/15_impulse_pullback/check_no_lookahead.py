# -*- coding: utf-8 -*-
"""하네스 자체 점검 — 룩어헤드 구조적 차단과 사건 정의의 손계산 일치.

1) 절단 동치: 패널을 날짜 T에서 잘라도 sig_date <= T 인 후보가 전 구간 실행과
   완전히 같아야 한다. 같지 않으면 신호가 미래 봉을 본 것이다.
2) 손계산: 합성 시계열 하나로 swing_high/low·move·거래량확인·retrace를 대조한다.
3) 쿨다운: 20거래일 규칙이 실제로 걸리는지 확인한다.

py research/studies/15_impulse_pullback/check_no_lookahead.py
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_impulse_pullback import (  # noqa: E402
    COOLDOWN, apply_band_and_cooldown, load_inputs, pit_membership, scan_candidates,
)

fails = []


def check(name, cond, detail=""):
    print(("  OK  " if cond else "  실패 ") + name + ("" if cond else f" — {detail}"))
    if not cond:
        fails.append(name)


# ── 1) 절단 동치 ────────────────────────────────────────────────────────────
px, univ, idx = load_inputs()
snaps, by_snap = pit_membership(univ)
codes = sorted(px["code"].unique())[::12][:150]     # 결정적 부분표본
sub = px[px["code"].isin(codes)]
CUT = pd.Timestamp("2024-06-28")

full = scan_candidates(sub, snaps, by_snap)
trunc = scan_candidates(sub[sub["date"] <= CUT], snaps, by_snap)
key = ["code", "sig_date", "retrace", "move_pct", "impulse_days", "days_since_high"]
a = full[full["sig_date"] <= CUT - pd.Timedelta(days=5)][key].sort_values(["code", "sig_date"]).reset_index(drop=True)
b = trunc[trunc["sig_date"] <= CUT - pd.Timedelta(days=5)][key].sort_values(["code", "sig_date"]).reset_index(drop=True)
print(f"[1] 절단 동치 — 전구간 {len(a):,}건 vs 절단 {len(b):,}건 (부분표본 {len(codes)}종목)")
check("절단 전후 후보 건수·값 동일", a.shape == b.shape and np.allclose(
    a.select_dtypes("number").to_numpy(), b.select_dtypes("number").to_numpy(), equal_nan=True)
    and (a["code"].tolist() == b["code"].tolist()),
    f"{a.shape} vs {b.shape}")

# ── 2) 손계산 (합성 시계열) ─────────────────────────────────────────────────
n = 200
close = np.full(n, 10000.0)
close[:100] = 10000.0
close[100:120] = np.linspace(10000, 14000, 20)     # 임펄스 +40%
close[120:] = np.linspace(14000, 12800, n - 120)   # 되돌림
high = close * 1.0
vol = np.full(n, 100000.0)
vol[110] = 500000.0                                 # 거래량 확인일 (vol_ma20 shift1 대비 5배)
d = pd.DataFrame({
    "date": pd.bdate_range("2023-01-02", periods=n),
    "code": "TEST01", "open": close, "high": high, "low": close * 0.99,
    "close": close, "volume": vol,
})
snaps2 = np.array([np.datetime64("2023-01-02")], dtype="datetime64[ns]")
by2 = {snaps2[0]: {"TEST01": "KOSPI"}}
c2 = scan_candidates(d, snaps2, by2)
t = 130
row = c2[c2["date" if "date" in c2 else "sig_date"] == d["date"].iloc[t]]
w0 = t - 39
hd = w0 + int(np.argmax(high[w0:t + 1]))
ld = w0 + int(np.argmin(close[w0:hd]))
exp_move = (high[hd] - close[ld]) / close[ld]
exp_ret = (high[hd] - close[t]) / (high[hd] - close[ld])
print(f"\n[2] 합성 손계산 t={d['date'].iloc[t].date()} 기대 move={exp_move:.4f} retrace={exp_ret:.4f}")
check("합성 시계열에서 move·retrace 일치", len(row) == 1
      and abs(float(row["move_pct"].iloc[0]) - exp_move) < 1e-9
      and abs(float(row["retrace"].iloc[0]) - exp_ret) < 1e-9,
      f"행 {len(row)}개")

# 거래량 확인 조건을 없애면 후보가 사라져야 한다
d2 = d.copy()
d2["volume"] = 100000.0
check("거래량 2.5배 확인이 없으면 사건 없음", len(scan_candidates(d2, snaps2, by2)) == 0)

# ── 3) 쿨다운 ───────────────────────────────────────────────────────────────
sel = apply_band_and_cooldown(full, 0.30, 0.55)
gap_ok = True
for code, g in sel.groupby("code"):
    idxs = np.sort(g["sig_idx"].to_numpy())
    if len(idxs) > 1 and np.min(np.diff(idxs)) < COOLDOWN:
        gap_ok = False
        break
print(f"\n[3] 쿨다운 — 밴드 30~55% 사건 {len(sel):,}건")
check(f"같은 종목 재신호 간격 >= {COOLDOWN}거래일", gap_ok)

print("\n실패 " + (", ".join(fails) if fails else "없음"))
sys.exit(1 if fails else 0)
