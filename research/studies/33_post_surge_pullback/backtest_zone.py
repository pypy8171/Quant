"""스터디 33 후속 — 초안 ④ 매수만 바꾼 칸: 재상승 확인을 기다리지 않고 급등일 몸통 40–60% 되돌림 구간에 닿으면 산다.

본 시험(backtest.py)은 눌림 뒤 "종가 > 전일 고가"를 기다려 다음 날 시가에 샀다. 이 규칙은 되돌림 자리보다 위에서
사게 만들었다(에이치브이엠 2026-09 건: 눌림 종가 58,300, 매수 62,200). 사용자가 뜻한 것은 되돌림 자리에서 사기였으므로
매수 한 줄만 바꾸고 나머지(급등일 정의·지켜보기 5일·급등일 시가 이탈 폐기·익절 급등일 고가·보유 10일·비용)는 그대로 둔다.

규칙(2026-10-01, 결과 보기 전 고정):
  P40 = 급등일 종가 − 0.4 × (종가 − 시가), P60 = 급등일 종가 − 0.6 × (종가 − 시가)
  매수: 지켜보는 날(D0+1 ~ D0+5) 중 처음으로 저가 ≤ P40 인 날.
        시가가 P40 이하(P60 이상)면 시가 + 1호가, 아니면 P40 지정가(호가 단위 내림) 체결.
        시가가 이미 P60 아래면 구간을 건너뛴 것으로 보고 버린다. 저가가 급등일 시가 아래면 버린다(본 시험과 같음).
  거래량: 장중 지정가는 그날 거래량을 모르므로 두 칸을 낸다.
        none   — 거래량 조건 없음
        volume — 전날 거래량 ≤ 급등일 거래량 × 0.5 인 날만 산다
  손절: 급등일 시가 × 0.99(호가 단위 내림). 매수 당일에도 저가가 닿으면 손절로 본다(장중 순서를 몰라 보수적으로).
  익절: 급등일 고가. 매수 당일에는 보지 않는다(그날 고가는 매수 전에 찍혔을 수 있다).
  시간 청산: 매수일 뒤 9거래일째 종가(매수일 포함 10거래일).

입력: PYQuant/data/bars_all_pit_v2.parquet 외 backtest.py load_inputs 와 같음.
산출: trades_zone.tsv(build_replay.py --rule zone 입력), metrics_zone.tsv(구간·칸별 건수·건당·승률·t).
재실행(저장소 루트): py -X utf8 research/studies/33_post_surge_pullback/backtest_zone.py
"""
from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
_specification = importlib.util.spec_from_file_location("study33_backtest", STUDY / "backtest.py")
study = importlib.util.module_from_spec(_specification)
sys.modules["study33_backtest"] = study
_specification.loader.exec_module(study)

sys.stdout.reconfigure(encoding="utf-8")

GAIN = 10.0
WINDOW = 5
HOLD = 10
ZONE_UPPER = 0.4          # 몸통의 40% 되돌림 — 지정가 자리
ZONE_LOWER = 0.6          # 몸통의 60% 되돌림 — 시가가 이보다 아래면 버림
VOLUME_RATIO = 0.5
LAST_DATE = 20260929
PERIODS = [("development", 20100101, 20221231), ("validation", 20230101, 20250930), ("recent_1y", 20251001, LAST_DATE)]


def period_of(date_int: int) -> str | None:
    for name, first, last in PERIODS:
        if first <= date_int <= last:
            return name

    return None


def find_buy(series, rows: int, surge_row: int, flags: np.ndarray, zone_top: float, zone_bottom: float,
             volume_rule: str) -> int:
    surge_open = series.open_price[surge_row]
    surge_volume = series.volume[surge_row]

    for row in range(surge_row + 1, min(rows, surge_row + WINDOW + 1)):
        if flags[row] or series.open_price[row] < zone_bottom:
            return -1

        volume_ok = volume_rule == "none" or series.volume[row - 1] <= VOLUME_RATIO * surge_volume

        if series.low[row] <= zone_top and volume_ok:
            return row

        if series.low[row] < surge_open:
            return -1

    return -1


def main() -> int:
    stocks, _, _ = study.load_inputs(LAST_DATE)
    records = []

    for series in stocks:
        rows = len(series.dates)
        flags = study.surge_flags(series, rows, GAIN)

        for surge_row in np.nonzero(flags)[0]:
            period = period_of(int(series.dates[surge_row]))
            surge_open, surge_close = series.open_price[surge_row], series.close[surge_row]
            body = surge_close - surge_open

            if period is None or body <= 0:
                continue

            zone_top = surge_close - ZONE_UPPER * body
            zone_bottom = surge_close - ZONE_LOWER * body
            turnover = surge_close * series.factor[surge_row] * series.volume[surge_row]
            market_cap = series.market_cap_at(surge_row)

            for volume_rule in ("none", "volume"):
                buy_row = find_buy(series, rows, surge_row, flags, zone_top, zone_bottom, volume_rule)

                if buy_row < 0 or buy_row + 1 >= rows:
                    continue

                at_open = series.open_price[buy_row] <= zone_top
                entry = series.tick_up(series.open_price[buy_row], buy_row) if at_open else series.floor_to_tick(zone_top, buy_row)
                stop = series.floor_to_tick(surge_open * study.STOP_BELOW_LOW, buy_row)
                target = series.high[surge_row]

                if series.low[buy_row] <= stop:
                    exit_row, exit_price, reason, closed = buy_row, series.tick_down(stop, buy_row), "stop_same_day", True
                else:
                    result = study.run_exit(series, rows, buy_row + 1, entry, stop, target, HOLD - 1, series.delisted)
                    exit_row, exit_price, reason, closed = result.exit_row, result.exit_price, result.reason, result.closed

                if not closed:
                    continue

                records.append({
                    "period": period, "rule": volume_rule, "code": series.code, "name": series.name,
                    "surge_date": int(series.dates[surge_row]), "pullback_date": int(series.dates[buy_row]),
                    "signal_date": int(series.dates[buy_row]), "buy_date": int(series.dates[buy_row]),
                    "exit_date": int(series.dates[exit_row]), "entry": entry, "entry_at_open": at_open,
                    "zone_top": zone_top, "zone_bottom": zone_bottom, "stop": stop, "target": target,
                    "target_on": target > entry, "exit_price": exit_price, "exit_reason": reason,
                    "hold_days": exit_row - buy_row, "net_pct": study.net_percent(entry, exit_price),
                    "surge_change_pct": (surge_close / series.close[surge_row - 1] - 1.0) * 100.0,
                    "turnover_eok": turnover / study.EOK, "market_cap_eok": market_cap / study.EOK,
                })

    trades = pd.DataFrame(records)
    trades.to_csv(STUDY / "trades_zone.tsv", sep="\t", index=False)
    metrics = trades.groupby(["rule", "period"], sort=False)["net_pct"].agg(
        count="size", average="mean", win_rate=lambda values: (values > 0).mean() * 100,
        t=lambda values: study.t_value(values)).round(2).reset_index()
    metrics.to_csv(STUDY / "metrics_zone.tsv", sep="\t", index=False)
    print(metrics.to_string(index=False))
    print(trades.groupby(["rule", "exit_reason"])["net_pct"].agg(["size", "mean"]).round(2).to_string())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
