"""
2022 폭락장 격리 ablation — regime 필터의 "위기 헤지" 가치를 정량화.

walk-forward는 OOS(2023~26)가 전부 상승장이라 regime 가치를 발현 못 했다(드래그만 보임).
여기서는 반대로 폭락장(2022, KOSPI ~-25%)만 떼서 regime ON vs OFF를 동일 조건으로 비교 →
"regime이 폭락장 MDD/수익을 정확히 얼마 개선했나"를 숫자 한 줄로.

⚠️ 정직성: 2022는 하락장이라 ON이 이기는 게 당연 — 이건 데이터마이닝이 아니라 **메커니즘 검증**.
   반드시 walk-forward의 "상승장 드래그"와 한 쌍으로 해석할 것:
   "regime은 폭락장 보험이고, 보험료(상승장 드래그)를 2022에서 회수한다."

워밍업: lookback(120)+regime_ma(200) 채우려 2021-07부터 시작(평가는 2022 포함 전구간 동일).
유니버스: 2022-01-01 as-of 1회 생성 후 ON/OFF 공유(유니버스 변수 제거).

사용:  export DATA_GO_KR_KEY='...'
       python3 PYQuant/tools/ablation_2022.py
"""
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from main import make_source, select_universe, run_backtest

FROM, TO = "2021-07-01", "2022-12-31"   # 워밍업 버퍼 포함, 평가구간 2022 폭락장
BASE = dict(strategy_name="momentum", top_n=30, rebalance_every=20,
            lookback=120, skip=20, regime_ma=200)


def main() -> None:
    src = make_source("datagokr", "all")
    if not src.authenticate():
        print("DATA_GO_KR_KEY 환경변수 필요. 중단."); return
    uni = select_universe(src, "datagokr", from_date="2022-01-01",
                          universe_size=200, kosdaq_size=100)
    print(f"2022 폭락장 격리 ablation (유니버스 {len(uni)}종목, {FROM}~{TO})")
    print(f"베이스: top{BASE['top_n']}/rb{BASE['rebalance_every']}/lb{BASE['lookback']}/rgma{BASE['regime_ma']}")
    print("=" * 72)
    print(f"{'설정':<22} {'수익률%':>8} {'MDD%':>8} {'샤프':>7} {'현금화':>6}")
    print("-" * 72)

    out = {}
    for label, regime in [("regime OFF (필터 없음)", False), ("regime ON (breadth 0.5)", True)]:
        r, _ = run_backtest(src, universe=uni, from_date=FROM, to_date=TO,
                            regime=regime, verbose=False, **BASE)
        out[label] = r
        print(f"{label:<22} {r.total_return:>8.1f} {-r.mdd:>8.1f} {r.sharpe:>7.2f} {r.regime_off:>6}")

    off, on = out["regime OFF (필터 없음)"], out["regime ON (breadth 0.5)"]
    print("=" * 72)
    print(f"📊 regime 필터 기여 (2022 폭락장):")
    print(f"   수익률  {on.total_return - off.total_return:+.1f}%p   "
          f"MDD 개선 {off.mdd - on.mdd:+.1f}%p   샤프 {on.sharpe - off.sharpe:+.2f}   "
          f"현금화 {on.regime_off}회")
    print("   ⚠️ walk-forward 상승장 드래그(-8%p)와 한 쌍으로 해석: 폭락장 보험료 회수")

    with open("ablation_2022.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["config", "return_pct", "mdd_pct", "sharpe", "regime_off"])
        for label, r in out.items():
            w.writerow([label, f"{r.total_return:.2f}", f"{-r.mdd:.2f}",
                        f"{r.sharpe:.3f}", r.regime_off])
    print("📄 ablation_2022.csv 저장")


if __name__ == "__main__":
    main()
