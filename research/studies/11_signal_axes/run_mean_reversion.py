#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C4 단기 역추세(평균회귀, 코드/파일 stem: mean_reversion) — 이격 과매도 + 반등확인 + 거래량 필터 재현 하네스.

재현 정보(각인):
  전략   : MeanReversionContraryStrategy(top_n=10, rebalance_every=5, sma_period=20, dev_max=-8.0)
  ablation: A(무필터=순수 이격 과매도) vs B(반등양봉+거래량 vol_mult=1.5×SMA20vol)
  유니버스: datagokr universe_top(as-of FROM) KOSPI 상위 100 (PIT·survivorship-free)
  데이터  : PYQuant/data/datagokr_source.py, FETCH_FLOOR=20200101, 수정주가 ON
  체결    : 신호=종가 t / 체결=다음봉 시가 / 수수료0.015%+세금0.18%+슬리피지5bp (엔진 CostModel)
  청산    : dev가 -8% 위로 회복(SMA20 재이탈 근사)하면 후보 이탈→매도 / rb5일 = 시간청산 근사
  look-ahead: 반등양봉(close_t>open_t)·거래량(vol_t vs SMA)은 신호봉 t만 사용(엔진이 t<=date로 슬라이스)
  seed    : 난수 미사용(정렬 결정론) / 기간: 2021-01-01~2024-12-31 (2022는 동일 곡선 슬라이스)

실행: py research/studies/11_signal_axes/run_mean_reversion.py   (cwd=repo root, DATA_GO_KR_KEY 필요)
"""
import sys, csv, json, subprocess
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
from backtest.engine import BacktestEngine, CostModel, Trade
from backtest.report import _derived_metrics, _turnover_proxy
from strategy.mean_reversion import MeanReversionContraryStrategy

FROM, TO = "2021-01-01", "2024-12-31"
TOP_N, RB, SMA, DEV = 10, 5, 20, -8.0
VOL_MULT, VOL_WIN = 1.5, 20
UNIV_MKT, UNIV_SIZE = "KOSPI", 100
WARMUP = max(SMA, VOL_WIN) * 2 + 30


class InstrEngine(BacktestEngine):
    """터미널 강제청산(engine.py:358-362) 계측 — 로직 불변(마지막 종가 청산 유지)."""
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.forced_liq = []
    def _execute(self, sig, date, all_bars):
        bars = all_bars.get(sig.ticker, [])
        if sig.side == "SELL" and not any(b.date > date for b in bars):
            past = [b for b in bars if b.date <= date]
            if past and self._positions.get(sig.ticker, 0) > 0:
                self.forced_liq.append((sig.ticker, date, self._positions[sig.ticker], past[-1].close))
        return super()._execute(sig, date, all_bars)


class HaircutEngine(BacktestEngine):
    """헤어컷 시나리오(ii): 미래봉 없는 매도(상폐/데이터종료)를 회수가치 0으로 청산."""
    def _execute(self, sig, date, all_bars):
        bars = all_bars.get(sig.ticker, [])
        if sig.side == "SELL" and not any(b.date > date for b in bars):
            held = self._positions.get(sig.ticker, 0)
            sell_qty = min(sig.quantity, held)
            if sell_qty > 0:
                buy = next((t for t in reversed(self._trades)
                            if t.ticker == sig.ticker and t.side == "BUY"), None)
                entry = buy.price if buy else 0.0
                self._positions[sig.ticker] = held - sell_qty      # 회수 0: cash 증가 없음
                if self._positions[sig.ticker] == 0:
                    del self._positions[sig.ticker]
                self._trades.append(Trade(sig.ticker, "SELL", date, 0.0, sell_qty, -entry * sell_qty))
            return
        return super()._execute(sig, date, all_bars)


def cost(rt_pct):
    fixed = 2 * 0.00015 + 0.0018
    slip = max(0.0, (rt_pct / 100.0 - fixed) / 2.0) * 10_000
    return CostModel(commission_rate=0.00015, tax_rate=0.0018, slippage_bps=slip)


def strat(filtered):
    return MeanReversionContraryStrategy(
        top_n=TOP_N, rebalance_every=RB, sma_period=SMA, dev_max=DEV,
        rebound=filtered, vol_mult=(VOL_MULT if filtered else 0.0), vol_win=VOL_WIN)


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


def summ(res, tag, forced=None):
    d = _derived_metrics(res.equity_curve)
    return dict(tag=tag, ret=res.total_return, cagr=d["cagr"], sharpe=res.sharpe,
                mdd=res.mdd, calmar=d["calmar"], win=res.win_rate,
                turnover=_turnover_proxy(res),
                rebals=len({t.date for t in res.trades if t.side == "BUY"}),
                sigstart=min([t.date for t in res.trades if t.side == "BUY"], default="N/A"),
                ntr=res.trade_count, bench=res.bench_return, bench_mdd=res.bench_mdd,
                bench_sharpe=res.bench_sharpe, alpha=res.alpha,
                forced=(len(forced) if forced is not None else None),
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


def main():
    src = DataGoKrSource(market=UNIV_MKT)
    if not src.authenticate():
        print("FATAL: datagokr 인증 실패 — DATA_GO_KR_KEY 확인"); return 2
    universe = src.universe_top(FROM, UNIV_SIZE, sizes={UNIV_MKT: UNIV_SIZE})
    if not universe:
        print("FATAL: universe_top 빈 결과"); return 2
    print(f"[universe] {UNIV_MKT} top{UNIV_SIZE} as-of {FROM}: {len(universe)}종목")
    try:
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=str(REPO)).decode().strip()
    except Exception:
        commit = "unknown"

    # ── 상폐/데이터종료 census (독립 — 종목 마지막봉 < 시장 마지막거래일) ──
    last_bar = {}
    market_last = ""
    for t in universe:
        bars = src.get_historical_ohlcv(t, FROM, TO)
        if bars:
            last_bar[t] = bars[-1].date
            market_last = max(market_last, bars[-1].date)
    dead = {t: d for t, d in last_bar.items() if d < market_last}
    print(f"[census] 시장 마지막거래일={market_last}  데이터종료(<마지막) 종목={len(dead)}건")

    results = {}
    for filt, tag in [(False, "A_nofilter"), (True, "B_filter")]:
        eng = InstrEngine(src, strat(filt), initial_cash=100_000_000, cost_model=cost(0.31),
                          target_positions=TOP_N, warmup_days=WARMUP)
        res = eng.run(universe, start_date=FROM, end_date=TO, verbose=False)
        export(res, "mean_reversion_" + ("filter" if filt else "nofilter"))
        # 헤어컷 시나리오(ii)
        eng2 = HaircutEngine(src, strat(filt), initial_cash=100_000_000, cost_model=cost(0.31),
                             target_positions=TOP_N, warmup_days=WARMUP)
        res2 = eng2.run(universe, start_date=FROM, end_date=TO, verbose=False)
        # 이 전략이 실제 담았던 dead 종목(=강제청산 유발) 집합
        held_dead = sorted({t.ticker for t in res.trades if t.ticker in dead
                            and any(x.side == "BUY" and x.ticker == t.ticker for x in res.trades)})
        results[tag] = dict(full=summ(res, tag, eng.forced_liq),
                            haircut=summ(res2, tag + "_hc"),
                            holdout=slice_metrics(res, "2022", "2022"),
                            forced=eng.forced_liq, held_dead=held_dead)

    # 비용 감도 (필터 B 기준 — 기대 후보)
    cost_sens = {}
    for rt in [0.21, 0.31, 0.5, 1.0]:
        eng = InstrEngine(src, strat(True), initial_cash=100_000_000, cost_model=cost(rt),
                          target_positions=TOP_N, warmup_days=WARMUP)
        r = eng.run(universe, start_date=FROM, end_date=TO, verbose=False)
        cost_sens[rt] = summ(r, f"B@{rt}")

    # ── 출력 ──
    A, B = results["A_nofilter"]["full"], results["B_filter"]["full"]
    print("\n" + "=" * 82)
    print("C4 단기 역추세(평균회귀, 이격 과매도 + 반등/거래량 필터) — 재현 하네스 결과")
    print(f"commit={commit}  기간={FROM}~{TO}  universe=KOSPI top{UNIV_SIZE}  rb={RB}d")
    print(f"sma{SMA} dev<{DEV}% top_n={TOP_N}  필터B: 반등양봉+거래량{VOL_MULT}x(win{VOL_WIN})  비용=slip5bp")
    print("=" * 82)
    print(f"{'metric':<18}{'A(무필터)':>16}{'B(반등+거래량)':>20}")
    print("-" * 82)
    def row(lbl, k, fmt="{:+.2f}"):
        print(f"{lbl:<18}{fmt.format(A[k]):>16}{fmt.format(B[k]):>20}")
    print(f"{'실질신호시작':<18}{A['sigstart']:>16}{B['sigstart']:>20}")
    print(f"{'평가기간':<18}{A['start']+'~'+A['end']:>16}{'':>20}")
    row("총수익률%", "ret"); row("CAGR%", "cagr"); row("Sharpe", "sharpe", "{:.2f}")
    row("MDD%", "mdd", "-{:.2f}"); row("Calmar", "calmar", "{:.2f}")
    row("승률%", "win", "{:.1f}"); row("turnover(연,proxy)", "turnover", "{:.2f}")
    print(f"{'리밸실행횟수':<18}{A['rebals']:>16}{B['rebals']:>20}")
    print(f"{'매도거래수':<18}{A['ntr']:>16}{B['ntr']:>20}")
    print(f"{'강제청산(상폐)':<18}{A['forced']:>16}{B['forced']:>20}")
    print("-" * 82)
    print(f"[벤치 등가중BH] 수익률 {A['bench']:+.2f}%  MDD -{A['bench_mdd']:.2f}%  Sharpe {A['bench_sharpe']:.2f}")
    print(f"[초과수익 α] A {A['alpha']:+.2f}%p  |  B {B['alpha']:+.2f}%p  (vs 등가중BH)")

    print("\n── 상폐/데이터종료 헤어컷 (i)마지막종가청산 vs (ii)회수0 ──")
    print(f"  {'':<12}{'강제청산건':>10}{'(i)총수익%':>12}{'(i)CAGR':>10}{'(i)MDD':>10}"
          f"{'(ii)총수익%':>12}{'(ii)CAGR':>10}{'(ii)MDD':>10}")
    for tag in ["A_nofilter", "B_filter"]:
        r = results[tag]; f_, h = r["full"], r["haircut"]
        print(f"  {tag:<12}{f_['forced']:>10}{f_['ret']:>12.2f}{f_['cagr']:>10.2f}{-f_['mdd']:>10.2f}"
              f"{h['ret']:>12.2f}{h['cagr']:>10.2f}{-h['mdd']:>10.2f}")
    print(f"  담았던 데이터종료 종목: A={results['A_nofilter']['held_dead']}  B={results['B_filter']['held_dead']}")

    print("\n── 2022 홀드아웃 (동일 곡선 슬라이스, 튜닝 미접촉) ──")
    for tag in ["A_nofilter", "B_filter"]:
        h = results[tag]["holdout"]
        if h:
            bs = f"  benchBH {h['bench']:+.2f}%" if h['bench'] is not None else ""
            print(f"  {tag}: {h['start']}~{h['end']}  수익 {h['ret']:+.2f}%  CAGR {h['cagr']:+.2f}%  "
                  f"Sharpe {h['sharpe']:.2f}  MDD -{h['mdd']:.2f}%  Calmar {h['calmar']:.2f}{bs}")

    print("\n── 비용 감도 (필터 B, round-trip%) ──")
    print(f"  {'round-trip':>10}{'총수익%':>12}{'CAGR%':>10}{'Sharpe':>10}{'MDD%':>10}{'α vs BH':>10}")
    for rt in [0.21, 0.31, 0.5, 1.0]:
        c = cost_sens[rt]
        print(f"  {rt:>9.2f}%{c['ret']:>11.2f}{c['cagr']:>10.2f}{c['sharpe']:>10.2f}{-c['mdd']:>10.2f}{c['alpha']:>10.2f}")

    with open(OUT / "mean_reversion_run_meta.json", "w", encoding="utf-8") as f:
        json.dump(dict(commit=commit, window=f"{FROM}~{TO}", universe=f"KOSPI top{UNIV_SIZE}",
                       params=dict(top_n=TOP_N, rebalance_every=RB, sma_period=SMA, dev_max=DEV,
                                   vol_mult=VOL_MULT, vol_win=VOL_WIN, warmup=WARMUP),
                       market_last=market_last, dead_census=len(dead),
                       A=results["A_nofilter"]["full"], B=results["B_filter"]["full"],
                       A_haircut=results["A_nofilter"]["haircut"],
                       B_haircut=results["B_filter"]["haircut"]),
                  f, ensure_ascii=False, indent=2, default=str)
    print(f"\n[산출물] {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
