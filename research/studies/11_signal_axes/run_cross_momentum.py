#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C1 횡단면 12-1 모멘텀 — 재현 가능 하네스 (단일 사전등록 config, 스윕 금지).

재현 정보(각인):
  전략   : CrossMomentumStrategy(top_n=10, rebalance_every=21, lookback=252, skip=20)
  ablation: vol_adjust False(동일가중 랭킹) vs True(1/σ 위험조정 랭킹) — 사이징은 둘 다 동일가중
  유니버스: datagokr universe_top(as-of from_date) KOSPI 상위 100 (PIT·survivorship-free)
  데이터  : PYQuant/data/datagokr_source.py, FETCH_FLOOR=20200101, 수정주가 ON
  체결    : 신호=종가 t / 체결=다음봉 시가 / 수수료0.015%+세금0.18%+슬리피지5bp (엔진 CostModel)
  seed    : 난수 미사용(정렬 결정론) — set 순회는 sorted()로 고정
  기간    : 2021-01-01 ~ 2024-12-31 (2022는 동일 equity 곡선에서 슬라이스 = 홀드아웃 라벨)

실행: py research/studies/11_signal_axes/run_cross_momentum.py   (cwd=repo root, DATA_GO_KR_KEY 필요)
"""
import sys, os, types, csv, json, subprocess
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "PYQuant"))
OUT = Path(__file__).resolve().parent

from data.datagokr_source import DataGoKrSource
from backtest.engine import BacktestEngine, CostModel
from backtest.report import _derived_metrics, _turnover_proxy
from strategy.cross_momentum import CrossMomentumStrategy

FROM, TO = "2021-01-01", "2024-12-31"
TOP_N, RB, LB, SKIP = 10, 21, 252, 20
UNIV_MKT, UNIV_SIZE = "KOSPI", 100
WARMUP = int((LB + SKIP) * 2.1) + 20   # main.py momentum 워밍업 공식과 동일


class InstrEngine(BacktestEngine):
    """터미널 강제청산(engine.py:358-362 버그) 계측용 — 로직 불변, 카운트만."""
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.forced_liq = []   # (ticker, date, qty, last_close)
    def _execute(self, sig, date, all_bars):
        bars = all_bars.get(sig.ticker, [])
        if sig.side == "SELL" and not any(b.date > date for b in bars):
            past = [b for b in bars if b.date <= date]
            if past:
                self.forced_liq.append((sig.ticker, date, sig.quantity, past[-1].close))
        return super()._execute(sig, date, all_bars)


def cost(rt_pct):
    """round-trip 목표%로 CostModel. commission0.015%×2 + tax0.18% 고정, 나머지는 slippage로."""
    fixed = 2 * 0.00015 + 0.0018            # 0.21%
    slip = max(0.0, (rt_pct / 100.0 - fixed) / 2.0) * 10_000
    return CostModel(commission_rate=0.00015, tax_rate=0.0018, slippage_bps=slip)


def slice_metrics(res, y0, y1):
    """equity 곡선을 [y0,y1] 연도로 슬라이스해 파생지표 재계산(홀드아웃 분리)."""
    ds, eq = res.equity_dates, res.equity_curve
    idx = [i for i, d in enumerate(ds) if y0 <= d[:4] <= y1]
    if not idx:
        return None
    sub = eq[idx[0]:idx[-1] + 1]
    d = _derived_metrics(sub)
    _, mdd, sharpe = BacktestEngine._curve_stats(sub)
    tr = (sub[-1] - sub[0]) / sub[0] * 100 if sub[0] > 0 else 0.0
    # 벤치(동일가중 BH) 동일 슬라이스
    bench = res.bench_curve
    btr = None
    if bench:
        bsub = bench[idx[0]:idx[-1] + 1]
        btr = (bsub[-1] - bsub[0]) / bsub[0] * 100 if bsub and bsub[0] > 0 else None
    return dict(start=ds[idx[0]], end=ds[idx[-1]], ret=tr, cagr=d["cagr"],
                sharpe=sharpe, mdd=mdd, calmar=d["calmar"], bench=btr)


def rebal_count(res):
    """리밸런싱 발생일수 = 신규편입/청산이 기록된 날 근사. BUY 발생 고유일수로 카운트."""
    return len({t.date for t in res.trades if t.side == "BUY"})


def signal_start(res):
    buys = [t.date for t in res.trades if t.side == "BUY"]
    return min(buys) if buys else "N/A"


def run_one(src, universe, cost_model, vol_adjust, tag):
    strat = CrossMomentumStrategy(top_n=TOP_N, rebalance_every=RB, lookback=LB,
                                  skip=SKIP, vol_adjust=vol_adjust)
    eng = InstrEngine(src, strat, initial_cash=100_000_000, cost_model=cost_model,
                      target_positions=TOP_N, warmup_days=WARMUP)
    res = eng.run(universe, start_date=FROM, end_date=TO, verbose=False)
    full = dict(
        tag=tag, ret=res.total_return, cagr=_derived_metrics(res.equity_curve)["cagr"],
        sharpe=res.sharpe, mdd=res.mdd,
        calmar=_derived_metrics(res.equity_curve)["calmar"],
        win=res.win_rate, turnover=_turnover_proxy(res),
        rebals=rebal_count(res), sigstart=signal_start(res),
        ntr=res.trade_count, bench=res.bench_return, bench_mdd=res.bench_mdd,
        bench_sharpe=res.bench_sharpe, kodex=res.kodex_return, alpha=res.alpha,
        forced=len(eng.forced_liq), forced_detail=eng.forced_liq,
        start=res.start_date, end=res.end_date,
    )
    return res, full


def export(res, base):
    ds, eq = res.equity_dates, res.equity_curve
    bench = res.bench_curve or [None] * len(ds)
    peak = eq[0] if eq else 0
    with open(OUT / f"{base}_equity.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "equity", "return_pct", "drawdown_pct", "bench_equity", "cash", "npos"])
        init = eq[0] if eq else 0
        for i, d in enumerate(ds):
            e = eq[i]; peak = max(peak, e)
            w.writerow([d, f"{e:.0f}", f"{(e-init)/init*100:.2f}", f"-{(peak-e)/peak*100:.2f}",
                        f"{bench[i]:.0f}" if bench[i] is not None else "",
                        f"{res.daily_cash[i]:.0f}", res.daily_npos[i]])
    with open(OUT / f"{base}_trades.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "ticker", "side", "price", "qty", "pnl"])
        for t in res.trades:
            w.writerow([t.date, t.ticker, t.side, f"{t.price:.0f}", t.quantity,
                        f"{t.pnl:.0f}" if t.side == "SELL" else ""])
    with open(OUT / f"{base}_holdings.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "ticker", "qty", "value"])
        for i, d in enumerate(res.equity_dates):
            for (t, q, v) in (res.daily_holdings[i] or []):
                w.writerow([d, t, q, f"{v:.0f}"])


def main():
    src = DataGoKrSource(market=UNIV_MKT)
    if not src.authenticate():
        print("FATAL: datagokr 인증 실패 — DATA_GO_KR_KEY 확인"); return 2
    universe = src.universe_top(FROM, UNIV_SIZE, sizes={UNIV_MKT: UNIV_SIZE})
    if not universe:
        print("FATAL: universe_top 빈 결과"); return 2
    print(f"[universe] {UNIV_MKT} top{UNIV_SIZE} as-of {FROM}: {len(universe)}종목")
    try:
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                         cwd=str(REPO)).decode().strip()
    except Exception:
        commit = "unknown"

    results = {}
    # 1) 동일가중 (headline) + 2) 역가중(1/σ) ablation — 기본 비용(slip5bp≈0.31% round trip)
    for va, tag in [(False, "EW"), (True, "VA")]:
        res, full = run_one(src, universe, cost(0.31), va, tag)
        export(res, "cross_momentum_" + {"EW": "equalweight", "VA": "voladj"}[tag])
        results[tag] = (res, full)
        h = slice_metrics(res, "2022", "2022")
        full["holdout2022"] = h

    # 3) 비용 감도 (동일가중 기준)
    cost_sens = {}
    for rt in [0.21, 0.31, 0.5, 1.0]:
        _, full = run_one(src, universe, cost(rt), False, f"EW@{rt}")
        cost_sens[rt] = full

    # ── 출력 ──
    print("\n" + "=" * 78)
    print("C1 12-1 횡단면 모멘텀 — 재현 하네스 결과")
    print(f"commit={commit}  기간={FROM}~{TO}  universe=KOSPI top{UNIV_SIZE}  rebal={RB}d(월)")
    print(f"lookback={LB} skip={SKIP} top_n={TOP_N}  warmup={WARMUP}일  비용=엔진기본(slip5bp)")
    print("=" * 78)
    hdr = f"{'metric':<16}{'EW(동일가중)':>18}{'VA(1/σ위험조정)':>20}"
    print(hdr)
    print("-" * 78)
    ew, va = results["EW"][1], results["VA"][1]
    def row(lbl, k, fmt="{:+.2f}"):
        print(f"{lbl:<16}{fmt.format(ew[k]):>18}{fmt.format(va[k]):>20}")
    print(f"{'실질신호시작':<16}{ew['sigstart']:>18}{va['sigstart']:>20}")
    print(f"{'평가기간':<16}{ew['start']+'~'+ew['end']:>18}{'':>20}")
    row("총수익률%", "ret"); row("CAGR%", "cagr"); row("Sharpe", "sharpe", "{:.2f}")
    row("MDD%", "mdd", "-{:.2f}"); row("Calmar", "calmar", "{:.2f}")
    row("승률%", "win", "{:.1f}"); row("turnover(연,proxy)", "turnover", "{:.2f}")
    print(f"{'리밸실행횟수':<16}{ew['rebals']:>18}{va['rebals']:>20}")
    print(f"{'매도거래수':<16}{ew['ntr']:>18}{va['ntr']:>20}")
    print(f"{'강제청산(상폐)':<16}{ew['forced']:>18}{va['forced']:>20}")
    print("-" * 78)
    print(f"[벤치 동일가중BH] 수익률 {ew['bench']:+.2f}%  MDD -{ew['bench_mdd']:.2f}%  Sharpe {ew['bench_sharpe']:.2f}")
    print(f"[KODEX200 BH] {ew['kodex']:+.2f}%" if ew['kodex'] is not None else "[KODEX200] 데이터없음")
    print(f"[초과수익 α] EW {ew['alpha']:+.2f}%p  |  VA {va['alpha']:+.2f}%p  (vs 동일가중BH)")

    print("\n── 2022 홀드아웃 (동일 곡선 슬라이스, 튜닝 미접촉) ──")
    for tag in ["EW", "VA"]:
        h = results[tag][1]["holdout2022"]
        if h:
            bench_s = f"  benchBH {h['bench']:+.2f}%" if h['bench'] is not None else ""
            print(f"  {tag}: {h['start']}~{h['end']}  수익 {h['ret']:+.2f}%  "
                  f"CAGR {h['cagr']:+.2f}%  Sharpe {h['sharpe']:.2f}  MDD -{h['mdd']:.2f}%"
                  f"  Calmar {h['calmar']:.2f}{bench_s}")

    print("\n── 비용 감도 (동일가중, round-trip%) ──")
    print(f"  {'round-trip':>10}{'총수익%':>12}{'CAGR%':>10}{'Sharpe':>10}{'MDD%':>10}{'α vs BH':>10}")
    for rt in [0.21, 0.31, 0.5, 1.0]:
        c = cost_sens[rt]
        print(f"  {rt:>9.2f}%{c['ret']:>11.2f}{c['cagr']:>10.2f}{c['sharpe']:>10.2f}"
              f"{-c['mdd']:>10.2f}{c['alpha']:>10.2f}")

    if ew["forced_detail"]:
        print("\n── 강제청산(상폐/데이터종료) 상세 ──")
        for (t, d, q, px) in ew["forced_detail"][:20]:
            print(f"  {t}  {d}  qty={q}  last_close={px:,.0f}")

    # 재현 정보 JSON
    with open(OUT / "cross_momentum_run_meta.json", "w", encoding="utf-8") as f:
        json.dump(dict(commit=commit, window=f"{FROM}~{TO}", universe=f"KOSPI top{UNIV_SIZE}",
                       params=dict(top_n=TOP_N, rebalance_every=RB, lookback=LB, skip=SKIP,
                                   warmup=WARMUP),
                       ablation="EW(vol_adjust=False) vs VA(vol_adjust=True)",
                       cost="engine default slip5bp ~0.31% round-trip",
                       ew={k: v for k, v in ew.items() if k != "forced_detail"},
                       va={k: v for k, v in va.items() if k != "forced_detail"}),
                  f, ensure_ascii=False, indent=2, default=str)
    print(f"\n[산출물] {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
