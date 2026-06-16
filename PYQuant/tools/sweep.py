"""
파라미터 강건성 스윕 — 한 번에 한 축만 바꿔 Sharpe/MDD/CAGR/알파를 표로 수집.

과적합 판정: 결과가 고원(plateau, 인접값도 비슷)이면 강건, 단일 첨탑(spike)이면 과적합.
한 축만 변화시키고 나머지는 베이스라인 고정(변수 하나씩 원칙). 데이터소스·유니버스는
1회 생성·재사용(같은 from/to/size면 캐시 적중 → 빠름).

사용 (키 환경변수):
  export DATA_GO_KR_KEY='...'
  python3 PYQuant/tools/sweep.py --axis top_n   --values 5,10,15,20,30
  python3 PYQuant/tools/sweep.py --axis lookback --values 60,90,120,150,250
  python3 PYQuant/tools/sweep.py --axis rebalance --values 10,20,40
  python3 PYQuant/tools/sweep.py --axis regime_ma --values 150,200,250
  python3 PYQuant/tools/sweep.py --axis skip      --values 10,15,20,25

기본 베이스라인(벤치초과 후보 — IS만, OOS 미검증): regime ON, top20, rb20, lb120,
  skip20, regime_ma200, KOSPI200+KOSDAQ100, 2020-01-02~2026-06-11. --base 로 변경 가능.
  ⚠️ 단일구간 1축 스윕은 IS(in-sample)이며 다중비교 미통제 — walk-forward로 OOS 검증 전엔
  "검증된 엣지"가 아니라 후보. (tools/walkforward.py 참조)
"""
import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from main import make_source, select_universe, run_backtest

# 한 번에 한 축만 — 허용 축(전략 파라미터)
AXES = {"top_n", "rebalance", "lookback", "skip", "regime_ma", "regime_thresh"}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--axis", required=True, choices=sorted(AXES),
                    help="스윕할 단일 파라미터 (한 번에 하나만)")
    ap.add_argument("--values", required=True, help="콤마구분 값들 (예: 5,10,15,20,30)")
    ap.add_argument("--from", dest="from_date", default="2020-01-02")
    ap.add_argument("--to",   dest="to_date",   default="2026-06-11")
    ap.add_argument("--universe-size", type=int, default=200)
    ap.add_argument("--kosdaq-size", type=int, default=100)
    ap.add_argument("--market", default="all", choices=["kospi", "kosdaq", "all"])
    ap.add_argument("--no-regime", action="store_true", help="regime 필터 끄기(기본 ON)")
    # 베이스라인(고정값) — 스윕 축이 아닌 것들
    ap.add_argument("--top-n", dest="top_n", type=int, default=20)
    ap.add_argument("--rebalance", type=int, default=20)
    ap.add_argument("--lookback", type=int, default=120)
    ap.add_argument("--skip", type=int, default=20)
    ap.add_argument("--regime-ma", dest="regime_ma", type=int, default=200)
    ap.add_argument("--regime-thresh", dest="regime_thresh", type=float, default=0.5)
    args = ap.parse_args()

    cast = float if args.axis == "regime_thresh" else int   # thresh만 실수
    values = [cast(v) for v in args.values.split(",")]
    base = {"top_n": args.top_n, "rebalance_every": args.rebalance, "lookback": args.lookback,
            "skip": args.skip, "regime_ma": args.regime_ma, "regime_thresh": args.regime_thresh}
    axis_key = {"top_n": "top_n", "rebalance": "rebalance_every", "lookback": "lookback",
                "skip": "skip", "regime_ma": "regime_ma", "regime_thresh": "regime_thresh"}[args.axis]

    src = make_source("datagokr", args.market)
    if not src.authenticate():
        print("DATA_GO_KR_KEY 환경변수 필요. 중단."); return
    universe = select_universe(src, "datagokr", from_date=args.from_date,
                               universe_size=args.universe_size, kosdaq_size=args.kosdaq_size)
    regime = not args.no_regime
    print(f"스윕 축: {args.axis} = {values}  (regime={'ON' if regime else 'OFF'}, "
          f"유니버스 {len(universe)}종목, {args.from_date}~{args.to_date})")
    print(f"베이스라인(고정): { {k:v for k,v in base.items() if k != axis_key} }")
    print("=" * 78)
    hdr = f"{args.axis:>10} | {'수익률%':>8} {'CAGR%':>7} {'샤프':>6} {'MDD%':>7} {'알파%p':>7} {'벤치샤프':>7} {'거래':>5}"
    print(hdr); print("-" * 78)

    rows = []
    for v in values:
        params = dict(base); params[axis_key] = v
        result, _ = run_backtest(
            src, strategy_name="momentum", universe=universe,
            from_date=args.from_date, to_date=args.to_date,
            top_n=params["top_n"], rebalance_every=params["rebalance_every"],
            lookback=params["lookback"], skip=params["skip"],
            regime=regime, regime_ma=params["regime_ma"],
            regime_thresh=params["regime_thresh"], verbose=False)
        n_days = len(result.equity_dates) or 1
        cagr = ((1 + result.total_return/100) ** (252/n_days) - 1) * 100
        print(f"{v:>10} | {result.total_return:>8.1f} {cagr:>7.1f} {result.sharpe:>6.2f} "
              f"{-result.mdd:>7.1f} {result.alpha:>7.1f} {result.bench_sharpe:>7.2f} {result.trade_count:>5}")
        rows.append([v, f"{result.total_return:.2f}", f"{cagr:.2f}", f"{result.sharpe:.3f}",
                     f"{-result.mdd:.2f}", f"{result.alpha:.2f}", f"{result.bench_sharpe:.3f}",
                     result.trade_count])

    out = f"sweep_{args.axis}.csv"
    with open(out, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow([args.axis, "return_pct", "cagr_pct", "sharpe", "mdd_pct",
                    "alpha_pp", "bench_sharpe", "trades"])
        w.writerows(rows)
    print("=" * 78)
    print(f"📄 {out} 저장. 판정: 샤프가 인접값끼리 비슷(고원)이면 강건, 한 값만 튀면 과적합.")


if __name__ == "__main__":
    main()
