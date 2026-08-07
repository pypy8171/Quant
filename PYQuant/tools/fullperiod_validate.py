"""전기간 롤링 검증 — 시작월 스윕(표본<200)의 결론을 통계적으로 재검증.

의장 프로토콜: 시작월 스윕에서 개선 신호가 보인 지표(C1 추세필터)를 2021~2026
전기간에 돌려 표본≥200 거래로 게이트를 판정한다. '승자 재배열(vol_adjust)'인지
'진짜 개선'인지는 여기서 갈린다.

유니버스: as-of 2025-12-30 고정 99종목 → ⚠ survivorship 상방편향(2021~2025 하락·편출
종목 부재). 절대 알파는 낙관적으로 읽어야 하며, 여기서 보는 건 '구성 간 상대 비교'.

사용: DATA_GO_KR_KEY=dummy .venv-win/Scripts/python.exe tools/fullperiod_validate.py
"""
import sys, glob, json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from data.datagokr_source import DataGoKrSource
from main import run_backtest
from tools.month_start_sweep import CONFIGS, build_universe, END

START = "2021-01-04"   # trend200 워밍업(200거래일) 확보 후 유효 시작

VARIANTS = ["baseline", "no_regime", "vol_adjust", "trend200", "trend120",
            "trend200_va", "absmom"]


def main():
    src = DataGoKrSource(market="KOSPI")
    universe = build_universe(src)
    print(f"[fullperiod] {START} ~ {END}  universe={len(universe)}종목 "
          f"(as-of 2025-12-30, survivorship-biased)")
    print(f"{'config':<14} {'ret%':>9} {'MDD%':>7} {'Sharpe':>7} {'win%':>6} "
          f"{'a(ew)%p':>8} {'bench%':>9} {'N':>5}")
    rows = []
    for name in VARIANTS:
        cfg = CONFIGS[name]
        res, _ = run_backtest(src, universe=universe, from_date=START, to_date=END,
                              verbose=False, **cfg)
        row = dict(config=name, total_return=round(res.total_return, 2),
                   mdd=round(res.mdd, 2), sharpe=round(res.sharpe, 2),
                   win_rate=round(res.win_rate, 1), alpha=round(res.alpha, 2),
                   bench_return=round(res.bench_return, 2), trades=res.trade_count)
        rows.append(row)
        print(f"{name:<14} {row['total_return']:>+9.2f} {row['mdd']:>7.2f} "
              f"{row['sharpe']:>7.2f} {row['win_rate']:>6.1f} {row['alpha']:>+8.2f} "
              f"{row['bench_return']:>+9.2f} {row['trades']:>5}")
    outdir = ROOT / "tools" / "out"
    outdir.mkdir(exist_ok=True)
    (outdir / "fullperiod.json").write_text(
        json.dumps(dict(start=START, end=END, universe_n=len(universe), rows=rows),
                   ensure_ascii=False, indent=2))
    print(f"[fullperiod] saved {outdir / 'fullperiod.json'}")


if __name__ == "__main__":
    main()
