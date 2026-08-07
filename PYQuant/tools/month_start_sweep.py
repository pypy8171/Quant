"""월 시작월 민감도 스윕 — 올해(2026) 각 월부터 매매 시작 시 오늘까지 기대 수익률.

오프라인 캐시 전용(DATA_GO_KR_KEY 없음). 유니버스는 as-of 2025-12-30 시총상위
KOSPI를 고정해 '시작 시점' 하나만 변수로 둔다(변수 하나씩 원칙).

사용:
  DATA_GO_KR_KEY=dummy .venv-win/Scripts/python.exe tools/month_start_sweep.py \
      --config baseline
  --config 는 CONFIGS 딕셔너리 키. 결과는 tools/out/<config>_monthly.json 로 저장.
"""
import sys, os, glob, json, argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # PYQuant/
sys.path.insert(0, str(ROOT))

from data.datagokr_source import DataGoKrSource
from main import run_backtest

END = "2026-08-05"     # 캐시 종료일과 정확히 일치해야 캐시 히트(_cache_path는 end ymd 정확매칭)
START_MONTHS = ["2026-01-02", "2026-02-02", "2026-03-02",
                "2026-04-01", "2026-05-02", "2026-06-01",
                "2026-07-01", "2026-08-03"]   # 8월 시작은 2026-08-05까지 ~3거래일뿐(참고용)
UNIV_ASOF = "2025-12-30"     # 연초 직전 point-in-time 유니버스(고정)
UNIV_N = 100

# 실험 구성 — 베이스라인은 실행 #2 채택(regime ON + vol_target 0.15).
# 각 실험은 '변수 하나씩' 원칙에 따라 베이스라인에서 한 축만 바꾼다.
BASE = dict(strategy_name="momentum", top_n=10, rebalance_every=5,
            lookback=120, skip=20, regime=True, regime_ma=200,
            regime_mode="breadth", vol_target=0.15, vol_window=20)

CONFIGS = {
    "baseline":     dict(BASE),
    "no_regime":    {**BASE, "regime": False},
    "vol_adjust":   {**BASE, "vol_adjust": True},       # 점수=수익률/변동성
    "voltgt_010":   {**BASE, "vol_target": 0.10},
    "top5":         {**BASE, "top_n": 5},
    "top20":        {**BASE, "top_n": 20},
    "rb20":         {**BASE, "rebalance_every": 20},
    "daily_regime": {**BASE, "daily_regime": True},
    # C1 — 개별종목 추세필터(듀얼모멘텀): regime이 못 보는 개별 롤오버 컷
    "trend200":     {**BASE, "trend_ma": 200},
    "trend120":     {**BASE, "trend_ma": 120},
    # C1 조합 — vol_adjust 스코어 위에 추세필터
    "trend200_va":  {**BASE, "trend_ma": 200, "vol_adjust": True},
    # 절대모멘텀 게이트(형성구간 수익률>0)
    "absmom":       {**BASE, "abs_mom": True},
}


def build_universe(src) -> list[str]:
    """as-of 2025-12-30 KOSPI 시총상위 N ∩ (2026-08-05까지 OHLCV 캐시 보유)."""
    cached = set()
    for f in glob.glob(str(ROOT / ".datagokr_cache" / "ohlcv_*_20260805.parquet")):
        cached.add(Path(f).name.split("ohlcv_")[1].split("_")[0])
    u = src.universe_top(UNIV_ASOF, UNIV_N)          # 캐시 히트(univ_20251230)
    hit = [c for c in u if c in cached]
    return hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="baseline", choices=list(CONFIGS))
    args = ap.parse_args()
    cfg = CONFIGS[args.config]

    src = DataGoKrSource(market="KOSPI")
    universe = build_universe(src)
    print(f"[sweep] config={args.config}  universe={len(universe)}종목 "
          f"(as-of {UNIV_ASOF})  cfg={cfg}")

    rows = []
    for start in START_MONTHS:
        res, names = run_backtest(src, universe=universe, from_date=start,
                                  to_date=END, verbose=False, **cfg)
        row = dict(start=start,
                   total_return=round(res.total_return, 2),
                   mdd=round(res.mdd, 2),
                   sharpe=round(res.sharpe, 2),
                   win_rate=round(res.win_rate, 1),
                   alpha=round(res.alpha, 2),
                   bench_return=round(res.bench_return, 2),
                   bench_sharpe=round(res.bench_sharpe, 2),
                   trades=res.trade_count,
                   regime_off=getattr(res, "regime_off", 0))
        rows.append(row)
        print(f"  {start[:7]}  ret={row['total_return']:+7.2f}%  "
              f"MDD -{row['mdd']:5.2f}%  Sharpe {row['sharpe']:5.2f}  "
              f"win {row['win_rate']:4.1f}%  α{row['alpha']:+7.2f}%p  "
              f"bench {row['bench_return']:+7.2f}%  N={row['trades']}")

    outdir = ROOT / "tools" / "out"
    outdir.mkdir(exist_ok=True)
    outpath = outdir / f"{args.config}_monthly.json"
    outpath.write_text(json.dumps(dict(config=args.config, cfg=cfg,
                                        universe_n=len(universe), asof=UNIV_ASOF,
                                        end=END, rows=rows), ensure_ascii=False, indent=2))
    print(f"[sweep] saved {outpath}")


if __name__ == "__main__":
    main()
