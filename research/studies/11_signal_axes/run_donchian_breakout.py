#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C3 20일 채널 돌파(종목별 절대 추세추종) — 재현 하네스.

재프레이밍: 편향감사 지시대로 "위기 6창 부분검증" 주장 폐기 → datagokr top100에 상시 멀티이어
per-stock 신호로 2021~2024 전 구간 검정. period=20 단일 사전등록(스윕 금지).

재현 정보(각인):
  전략   : DonchianBreakoutStrategy(period=20)  진입 close≥직전20일최고 / 청산 close≤직전20일최저
  유니버스: datagokr universe_top(as-of FROM) KOSPI 상위 100 (PIT, cross_momentum/mean_reversion 동일 풀)
  데이터  : PYQuant/data/datagokr_source.py, FETCH_FLOOR=20200101, 수정주가 ON
  체결    : 신호=종가 t / 체결=다음봉 시가 / 수수료0.015%+세금0.18%+슬리피지5bp
  look-ahead: 20일 최고/최저는 bars[-(period+1):-1](신호봉 t 제외) → 자기참조 누출 없음
  seed    : 난수 미사용(sorted 결정론) / 기간 2021-01-01~2024-12-31 (2022는 동일 곡선 슬라이스)

실행: py research/studies/11_signal_axes/run_donchian_breakout.py   (cwd=repo root, DATA_GO_KR_KEY 필요)
"""
import sys, csv, json, subprocess, statistics
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
from strategy.donchian_breakout import DonchianBreakoutStrategy

FROM, TO = "2021-01-01", "2024-12-31"
PERIOD = 20
UNIV_MKT, UNIV_SIZE = "KOSPI", 100
WARMUP = PERIOD * 3 + 30


class InstrEngine(BacktestEngine):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.forced_liq = []
    def _execute(self, sig, date, all_bars):
        bars = all_bars.get(sig.ticker, [])
        if sig.side == "SELL" and not any(b.date > date for b in bars):
            if self._positions.get(sig.ticker, 0) > 0:
                self.forced_liq.append((sig.ticker, date))
        return super()._execute(sig, date, all_bars)


def cost(rt_pct):
    fixed = 2 * 0.00015 + 0.0018
    slip = max(0.0, (rt_pct / 100.0 - fixed) / 2.0) * 10_000
    return CostModel(commission_rate=0.00015, tax_rate=0.0018, slippage_bps=slip)


def slice_metrics(res, y0, y1):
    ds, eq = res.equity_dates, res.equity_curve
    idx = [i for i, d in enumerate(ds) if y0 <= d[:4] <= y1]
    if not idx:
        return None
    sub = eq[idx[0]:idx[-1] + 1]
    d = _derived_metrics(sub)
    _, mdd, sharpe = BacktestEngine._curve_stats(sub)
    tr = (sub[-1] - sub[0]) / sub[0] * 100 if sub[0] > 0 else 0.0
    btr = None
    if res.bench_curve:
        bsub = res.bench_curve[idx[0]:idx[-1] + 1]
        btr = (bsub[-1] - bsub[0]) / bsub[0] * 100 if bsub and bsub[0] > 0 else None
    return dict(start=ds[idx[0]], end=ds[idx[-1]], ret=tr, cagr=d["cagr"],
                sharpe=sharpe, mdd=mdd, calmar=d["calmar"], bench=btr)


def summ(res, forced=None):
    d = _derived_metrics(res.equity_curve)
    npos = res.daily_npos or []
    return dict(ret=res.total_return, cagr=d["cagr"], sharpe=res.sharpe, mdd=res.mdd,
                calmar=d["calmar"], win=res.win_rate, turnover=_turnover_proxy(res),
                rebals=len({t.date for t in res.trades if t.side == "BUY"}),
                sigstart=min([t.date for t in res.trades if t.side == "BUY"], default="N/A"),
                ntr=res.trade_count, bench=res.bench_return, bench_mdd=res.bench_mdd,
                bench_sharpe=res.bench_sharpe, alpha=res.alpha,
                forced=(len(forced) if forced is not None else None),
                npos_avg=(sum(npos) / len(npos) if npos else 0), npos_max=(max(npos) if npos else 0),
                start=res.start_date, end=res.end_date)


def export(res, base):
    ds, eq = res.equity_dates, res.equity_curve
    bench = res.bench_curve or [None] * len(ds)
    init = eq[0] if eq else 0
    peak = eq[0] if eq else 0
    with open(OUT / f"{base}_equity.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "equity", "return_pct", "drawdown_pct", "bench_equity", "cash", "npos"])
        for i, dd in enumerate(ds):
            e = eq[i]; peak = max(peak, e)
            w.writerow([dd, f"{e:.0f}", f"{(e-init)/init*100:.2f}", f"-{(peak-e)/peak*100:.2f}",
                        f"{bench[i]:.0f}" if bench[i] is not None else "",
                        f"{res.daily_cash[i]:.0f}", res.daily_npos[i]])
    with open(OUT / f"{base}_trades.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "ticker", "side", "price", "qty", "pnl"])
        for t in res.trades:
            w.writerow([t.date, t.ticker, t.side, f"{t.price:.0f}", t.quantity,
                        f"{t.pnl:.0f}" if t.side == "SELL" else ""])
    with open(OUT / f"{base}_holdings.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f); w.writerow(["date", "ticker", "qty", "value"])
        for i, dd in enumerate(ds):
            for (t, q, v) in (res.daily_holdings[i] or []):
                w.writerow([dd, t, q, f"{v:.0f}"])


def load_equity(path):
    """equity CSV → {date: equity}. 상관계수용."""
    out = {}
    p = Path(path)
    if not p.exists():
        return out
    with open(p, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            try:
                out[r["date"]] = float(r["equity"])
            except (KeyError, ValueError):
                pass
    return out


def daily_ret_corr(a: dict, b: dict):
    """두 equity 시계열의 공통일 일간수익률 Pearson 상관. (corr, n)."""
    days = sorted(set(a) & set(b))
    ra, rb = [], []
    for i in range(1, len(days)):
        d0, d1 = days[i-1], days[i]
        if a[d0] > 0 and b[d0] > 0:
            ra.append(a[d1] / a[d0] - 1.0)
            rb.append(b[d1] / b[d0] - 1.0)
    if len(ra) < 3:
        return None, len(ra)
    try:
        return statistics.correlation(ra, rb), len(ra)
    except statistics.StatisticsError:
        return None, len(ra)


def main():
    src = DataGoKrSource(market=UNIV_MKT)
    if not src.authenticate():
        print("FATAL: datagokr 인증 실패"); return 2
    universe = src.universe_top(FROM, UNIV_SIZE, sizes={UNIV_MKT: UNIV_SIZE})
    if not universe:
        print("FATAL: universe_top 빈 결과"); return 2
    print(f"[universe] {UNIV_MKT} top{UNIV_SIZE} as-of {FROM}: {len(universe)}종목")
    try:
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=str(REPO)).decode().strip()
    except Exception:
        commit = "unknown"

    # 상폐/데이터종료 census (건수만)
    last_bar, market_last = {}, ""
    for t in universe:
        bars = src.get_historical_ohlcv(t, FROM, TO)
        if bars:
            last_bar[t] = bars[-1].date
            market_last = max(market_last, bars[-1].date)
    dead = {t: d for t, d in last_bar.items() if d < market_last}
    print(f"[census] 시장 마지막거래일={market_last}  데이터종료 종목={len(dead)}건")

    eng = InstrEngine(src, DonchianBreakoutStrategy(period=PERIOD), initial_cash=100_000_000,
                      cost_model=cost(0.31), target_positions=1, warmup_days=WARMUP)
    res = eng.run(universe, start_date=FROM, end_date=TO, verbose=False)
    export(res, "donchian_breakout")
    S = summ(res, eng.forced_liq)
    hold = slice_metrics(res, "2022", "2022")

    cost_sens = {}
    for rt in [0.21, 0.31, 0.5, 1.0]:
        e = InstrEngine(src, DonchianBreakoutStrategy(period=PERIOD), initial_cash=100_000_000,
                        cost_model=cost(rt), target_positions=1, warmup_days=WARMUP)
        cost_sens[rt] = summ(e.run(universe, start_date=FROM, end_date=TO, verbose=False))

    # 상관계수 — donchian vs cross_momentum(EW), donchian vs mean_reversion(B필터), cross_momentum vs mean_reversion
    dc_eq = load_equity(OUT / "donchian_breakout_equity.csv")
    cm_eq = load_equity(OUT / "cross_momentum_equalweight_equity.csv")
    mr_eq = load_equity(OUT / "mean_reversion_filter_equity.csv")

    print("\n" + "=" * 78)
    print("C3 20일 채널 돌파(시계열, period=20) — 재현 하네스 결과")
    print(f"commit={commit}  기간={FROM}~{TO}  universe=KOSPI top{UNIV_SIZE}  상시 per-stock")
    print("=" * 78)
    print(f"  실질신호시작 : {S['sigstart']}   평가기간 {S['start']}~{S['end']}")
    print(f"  총수익률 {S['ret']:+.2f}%   CAGR {S['cagr']:+.2f}%   Sharpe {S['sharpe']:.2f}")
    print(f"  MDD -{S['mdd']:.2f}%   Calmar {S['calmar']:.2f}   승률 {S['win']:.1f}%")
    print(f"  turnover(연,proxy) {S['turnover']:.2f}   매수일수 {S['rebals']}   매도거래수 {S['ntr']}")
    print(f"  보유종목수 avg {S['npos_avg']:.1f} / max {S['npos_max']}   강제청산(상폐) {S['forced']}건")
    print(f"  [벤치 동일가중BH] {S['bench']:+.2f}%  MDD -{S['bench_mdd']:.2f}%  Sharpe {S['bench_sharpe']:.2f}")
    print(f"  [초과수익 α] {S['alpha']:+.2f}%p (vs 동일가중BH)")

    print("\n── 2022 홀드아웃 (동일 곡선 슬라이스, 튜닝 미접촉) ──")
    if hold:
        bs = f"  benchBH {hold['bench']:+.2f}%" if hold['bench'] is not None else ""
        print(f"  {hold['start']}~{hold['end']}  수익 {hold['ret']:+.2f}%  CAGR {hold['cagr']:+.2f}%  "
              f"Sharpe {hold['sharpe']:.2f}  MDD -{hold['mdd']:.2f}%  Calmar {hold['calmar']:.2f}{bs}")

    print("\n── 비용 감도 (round-trip%) ──")
    print(f"  {'round-trip':>10}{'총수익%':>12}{'CAGR%':>10}{'Sharpe':>10}{'MDD%':>10}{'α vs BH':>10}")
    for rt in [0.21, 0.31, 0.5, 1.0]:
        c = cost_sens[rt]
        print(f"  {rt:>9.2f}%{c['ret']:>11.2f}{c['cagr']:>10.2f}{c['sharpe']:>10.2f}{-c['mdd']:>10.2f}{c['alpha']:>10.2f}")

    print("\n── 전략간 일간수익률 상관 (equity 기반, 공통 거래일) ──")
    for lbl, a, b in [("채널 돌파(시계열추세) vs 횡단면모멘텀", dc_eq, cm_eq),
                      ("채널 돌파(시계열추세) vs 단기역추세", dc_eq, mr_eq),
                      ("횡단면모멘텀 vs 단기역추세", cm_eq, mr_eq)]:
        corr, n = daily_ret_corr(a, b)
        print(f"  {lbl:<34} corr={corr:+.3f}  (n={n})" if corr is not None
              else f"  {lbl:<34} corr=N/A  (n={n}, CSV 부재?)")

    with open(OUT / "donchian_breakout_run_meta.json", "w", encoding="utf-8") as f:
        json.dump(dict(commit=commit, window=f"{FROM}~{TO}", universe=f"KOSPI top{UNIV_SIZE}",
                       params=dict(period=PERIOD, warmup=WARMUP), market_last=market_last,
                       dead_census=len(dead), main=S, holdout2022=hold,
                       corr_donchian_cross_momentum=daily_ret_corr(dc_eq, cm_eq)[0],
                       corr_donchian_mean_reversion=daily_ret_corr(dc_eq, mr_eq)[0],
                       corr_cross_momentum_mean_reversion=daily_ret_corr(cm_eq, mr_eq)[0]),
                  f, ensure_ascii=False, indent=2, default=str)
    print(f"\n[산출물] {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
