"""스터디 38 2회차 — 20일 평균 거래대금 상위 값 점검(분위 칸에 자료 오류가 섞였는지 본다).

거래대금 = 무수정 종가(수정 종가 × factor) × 거래량. 병합·감자 뒤 거래량과 factor 가 서로 맞지 않으면 무수정 종가가
실제보다 크게 나와 거래대금이 부풀고, 그 종목이 거래대금 10분위에 잘못 들어간다(2회차 fill_turnover_bounds.csv 의 최대값이
하루 수 조 원인 해가 있어 확인).
산출: results/round2/turnover_outliers.csv — 신호 기간 20일 평균 거래대금 상위 2,000 종목·일을 종목별로 묶은 표
      (최대값·그 종목 평생 중앙값 대비 배수·무수정 종가 중앙값·factor 중앙값·그 기간 거래대금 분위).
기준값을 정하지 않고 순위만 낸다 — 오류로 볼 문턱은 정해야 할 값으로 남긴다.
재실행: py research/studies/38_trend_support_realtime/round2_turnover_check.py
"""
from __future__ import annotations

import numpy as np
import pandas as pd

import common

TOP_ROWS = 2000   # 상위 몇 종목·일을 볼지(점검 범위만 정함, 결과 선택에 안 씀 — 내가 정함)


def main() -> None:
    panel = common.Panel()
    panel.build_deciles()
    turnover = np.where(panel.signal_ok, panel.turnover20, np.nan)
    order = np.argsort(-np.nan_to_num(turnover, nan=-1.0))[:TOP_ROWS]
    stocks = panel.stock_of_row[order]
    lifetime_median = np.array([np.nanmedian(panel.turnover20[panel.starts[stock]:panel.starts[stock + 1]])
                                for stock in range(panel.stock_count)])
    frame = pd.DataFrame({"code": np.array(panel.codes)[stocks], "name": np.array(panel.names)[stocks], "date": panel.dates[order],
                          "turnover20_krw": turnover[order], "raw_close": panel.raw_close[order], "factor": panel.factor[order],
                          "volume": panel.volume[order], "turnover_decile": panel.deciles[order, 1],
                          "lifetime_median_turnover20_krw": lifetime_median[stocks]})
    table = frame.groupby(["code", "name"], as_index=False).agg(
        rows=("date", "size"), first_date=("date", "min"), last_date=("date", "max"), max_turnover20_krw=("turnover20_krw", "max"),
        median_raw_close=("raw_close", "median"), median_factor=("factor", "median"), median_volume=("volume", "median"),
        lifetime_median_turnover20_krw=("lifetime_median_turnover20_krw", "first"))
    table["max_over_lifetime_median"] = table["max_turnover20_krw"] / table["lifetime_median_turnover20_krw"]
    table = table.sort_values("max_turnover20_krw", ascending=False)
    common.write_csv(common.STUDY / "results" / "round2" / "turnover_outliers.csv", table)
    print(table.head(20).to_string())


if __name__ == "__main__":
    main()
