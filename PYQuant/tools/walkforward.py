"""
Walk-forward (OOS) 검증 — in-sample 고원이 out-of-sample에서도 유지되는지 채점.

스윕(sweep.py)은 전부 IS(in-sample)라 과적합을 못 가린다. 여기서는 anchored walk-forward로
"한 번도 안 본 미래 구간(OOS)"에서만 성과를 측정한다:
  - 각 폴드: IS(시작~분할점)에서 파라미터 선택(--grid 주면 그리드 최적, 아니면 고정값) →
    그 파라미터로 OOS(다음 1년)를 1회 평가. OOS는 절대 파라미터 선택에 안 쓴다.
  - 폴드별 OOS 수익곡선을 이어붙여 단일 OOS 누적곡선 생성 → 진짜 out-of-sample 성과.
  - 유니버스는 각 구간 시작일 as-of로 선정(세그먼트별 point-in-time).

판정: OOS 샤프가 IS 샤프의 ~60%+ 유지 & OOS가 벤치 초과면 엣지 후보가 OOS 통과.
  IS-OOS 격차가 크면 = 과적합 세금. 단일구간 스윕 숫자가 무너지면 그게 진짜 답이다.

사용 (키 환경변수):
  export DATA_GO_KR_KEY='...'
  # 고정 파라미터 OOS 검증(권장 첫 실행): 선택된 설정이 OOS에서 사나?
  python3 PYQuant/tools/walkforward.py
  # 파라미터 선택이 과적합인지: IS에서 lookback 그리드 최적 → OOS 채점
  python3 PYQuant/tools/walkforward.py --grid lookback=60,120,250
"""
import argparse
import csv
import sys
from itertools import product
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from main import make_source, select_universe, run_backtest
from backtest.engine import BacktestEngine

# anchored 폴드: (IS시작, IS끝, OOS시작, OOS끝). IS는 확장창, OOS는 다음 1년.
FOLDS = [
    ("2020-01-02", "2022-12-31", "2023-01-01", "2023-12-31"),
    ("2020-01-02", "2023-12-31", "2024-01-01", "2024-12-31"),
    ("2020-01-02", "2024-12-31", "2025-01-01", "2025-12-31"),
    ("2020-01-02", "2025-12-31", "2026-01-01", "2026-06-11"),
]


def _stitch(prev_curve, seg_curve):
    """직전 곡선 끝값에 이어붙이기(세그먼트 시작을 직전 끝으로 스케일)."""
    if not seg_curve:
        return prev_curve
    base = prev_curve[-1] if prev_curve else seg_curve[0]
    scale = base / seg_curve[0] if seg_curve[0] else 1.0
    return prev_curve + [v * scale for v in seg_curve]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--grid", default=None,
                    help="IS 최적화할 파라미터 (예: 'lookback=60,120,250'). 없으면 고정값 OOS검증")
    ap.add_argument("--market", default="all", choices=["kospi", "kosdaq", "all"])
    ap.add_argument("--universe-size", type=int, default=200)
    ap.add_argument("--kosdaq-size", type=int, default=100)
    ap.add_argument("--no-regime", action="store_true")
    # 고정 베이스라인(강건 후보 설정)
    ap.add_argument("--top-n", dest="top_n", type=int, default=30)
    ap.add_argument("--rebalance", type=int, default=20)
    ap.add_argument("--lookback", type=int, default=120)
    ap.add_argument("--skip", type=int, default=20)
    ap.add_argument("--regime-ma", dest="regime_ma", type=int, default=200)
    args = ap.parse_args()

    base = {"top_n": args.top_n, "rebalance_every": args.rebalance, "lookback": args.lookback,
            "skip": args.skip, "regime_ma": args.regime_ma}
    regime = not args.no_regime

    # 그리드 파싱: "lookback=60,120,250" → [{'lookback':60},{'lookback':120},...]
    grid_combos = [{}]
    if args.grid:
        key, vals = args.grid.split("=")
        key = {"rebalance": "rebalance_every"}.get(key, key)
        grid_combos = [{key: int(v)} for v in vals.split(",")]

    src = make_source("datagokr", args.market)
    if not src.authenticate():
        print("DATA_GO_KR_KEY 환경변수 필요. 중단."); return

    mode = f"IS그리드최적({args.grid})" if args.grid else "고정파라미터"
    print(f"Walk-forward ({mode}, regime={'ON' if regime else 'OFF'}, "
          f"top{args.top_n}/rb{args.rebalance}/lb{args.lookback}/skip{args.skip}/rgma{args.regime_ma})")
    print("=" * 92)
    print(f"{'OOS구간':<12} {'선택파라미터':<18} {'IS샤프':>7} {'OOS샤프':>7} {'OOS수익%':>8} "
          f"{'OOS MDD%':>8} {'OOS알파%p':>9} {'현금화':>5}")
    print("-" * 92)

    rows = []
    oos_curve, bench_curve = [], []
    for is_from, is_to, oos_from, oos_to in FOLDS:
        # IS 유니버스 as-of
        is_uni = select_universe(src, "datagokr", from_date=is_from,
                                 universe_size=args.universe_size, kosdaq_size=args.kosdaq_size)
        # IS에서 파라미터 선택
        best, best_sharpe = dict(base), -1e9
        for combo in grid_combos:
            params = dict(base); params.update(combo)
            r, _ = run_backtest(src, strategy_name="momentum", universe=is_uni,
                                from_date=is_from, to_date=is_to, regime=regime, verbose=False, **params)
            if r.sharpe > best_sharpe:
                best_sharpe, best = r.sharpe, params
        # OOS 평가 (선택된 파라미터 고정, OOS 유니버스 as-of)
        oos_uni = select_universe(src, "datagokr", from_date=oos_from,
                                  universe_size=args.universe_size, kosdaq_size=args.kosdaq_size)
        r_oos, _ = run_backtest(src, strategy_name="momentum", universe=oos_uni,
                                from_date=oos_from, to_date=oos_to, regime=regime, verbose=False, **best)
        if args.grid:
            gk = list(grid_combos[0].keys())[0]
            pick = f"{gk}={best[gk]}"
        else:
            pick = "고정"
        print(f"{oos_from[:7]:<12} {pick:<18} {best_sharpe:>7.2f} {r_oos.sharpe:>7.2f} "
              f"{r_oos.total_return:>8.1f} {-r_oos.mdd:>8.1f} {r_oos.alpha:>9.1f} {r_oos.regime_off:>5}")
        rows.append([oos_from, oos_to, pick, f"{best_sharpe:.3f}", f"{r_oos.sharpe:.3f}",
                     f"{r_oos.total_return:.2f}", f"{-r_oos.mdd:.2f}", f"{r_oos.alpha:.2f}",
                     r_oos.regime_off])
        oos_curve = _stitch(oos_curve, r_oos.equity_curve or [])
        bench_curve = _stitch(bench_curve, r_oos.bench_curve or [])

    # 이어붙인 OOS 누적곡선 전체 통계
    s_ret, s_mdd, s_sharpe = BacktestEngine._curve_stats(oos_curve)
    b_ret, b_mdd, b_sharpe = BacktestEngine._curve_stats(bench_curve)
    print("=" * 92)
    print(f"📊 통합 OOS(이어붙임): 전략 수익 {s_ret:+.1f}% 샤프 {s_sharpe:.2f} MDD -{s_mdd:.1f}%")
    print(f"               벤치(등가중) 수익 {b_ret:+.1f}% 샤프 {b_sharpe:.2f} MDD -{b_mdd:.1f}%")
    verdict = "✅ OOS도 벤치 초과(위험조정)" if s_sharpe > b_sharpe else "❌ OOS에서 벤치 미달 — 과적합 의심"
    print(f"   판정: {verdict}  (알파 {s_ret-b_ret:+.1f}%p)")

    with open("walkforward_picks.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["oos_from", "oos_to", "pick", "is_sharpe", "oos_sharpe",
                    "oos_return_pct", "oos_mdd_pct", "oos_alpha_pp", "regime_off"])
        w.writerows(rows)
    with open("walkforward_oos_curve.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["idx", "strategy_equity", "bench_equity"])
        for i in range(len(oos_curve)):
            be = bench_curve[i] if i < len(bench_curve) else ""
            w.writerow([i, f"{oos_curve[i]:.0f}", f"{be:.0f}" if be != "" else ""])
    print("📄 walkforward_picks.csv / walkforward_oos_curve.csv 저장")


if __name__ == "__main__":
    main()
