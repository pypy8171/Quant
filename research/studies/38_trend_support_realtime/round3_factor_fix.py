"""스터디 38 3회차 — 무수정 종가·거래대금 보정표를 만든다(공용 PYQuant/data/bars_all_pit_v2.parquet 은 고치지 않는다).

무엇이 틀렸나(2회차 turnover_outliers.csv 25 종목에서 시작해 전수로 대조):
  - 2회차까지 무수정 종가 = 수정 종가 × factor(스터디 33 load_inputs: data.go.kr 월말 무수정 종가 ÷ 수정 종가, 2020-01 부터).
    2020 이전은 첫 월말 factor 를 뒤로 늘려 쓰고, 월 중간 감자·분할은 다음 월말까지 안 바뀐다.
  - 거래대금 = 무수정 종가 × 거래량. 그런데 네이버 siseJson 거래량은 행마다 수정 여부가 섞여 있다(다음 무수정 거래량과
    같은 행 약 61%, 나머지 대부분은 1 ÷ 실제 factor 배 — 수정 거래량). 그래서 factor 를 곱하면 오히려 틀리는 행이 많다.
  이 파일은 "그날 실제 무수정 종가·실제 거래대금"을 원 출처에서 가져와 행마다 넣는다.

출처(받은 날·등급은 data/*_meta.json):
  - 상장 중 종목: 다음 차트 adjusted=false (fetch_daum_unadjusted.py) — 종가·거래대금(candleAccTradePrice), 2006-06 –.
  - 상장폐지 종목 2020-01 이후: data.go.kr 주식시세정보 (fetch_datagokr_delisted.py) — 종가(clpr)·거래대금(trPrc).
  - 둘 다 없는 행(상장폐지 종목의 2020 이전, 출처 날짜 빠짐): 수정 종가 × 네이버 거래량을 거래대금 대용으로 쓴다.
    상장 중 종목에서 대용값을 실제 거래대금과 대조한 오차를 같이 낸다(factor_fix_summary.json proxy_validation).
    무수정 종가는 2회차 값(수정 종가 × factor) 그대로 둔다(3회차 판정에는 거래대금만 쓴다).
  대용 행을 빼지 않는 까닭: 빼면 상장폐지 종목이 거래대금 분위에서 빠져 생존 편향이 생긴다.

산출:
  data/raw_close_corrections.parquet     패널 행 순서(종목 코드·날짜 순) 전 행: code, date, raw_close_panel, raw_close_corrected,
                                         turnover_panel(2회차 무수정 종가 × 거래량), turnover_corrected(그날 거래대금), status
  results/round3/factor_fix_summary.json 출처별 행 수, 2회차 값과 어긋난 행·종목 수(2% · 2 배 · 10 배 · 30 배), 무수정 종가 튐 전수 검사,
                                         대용값 검증, 25 종목 대조
  results/round3/factor_fix_stocks.csv   종목별 어긋난 행 수·최대 배수·출처
  results/round3/turnover_outliers_fixed.csv  2회차 25 종목: 2회차 최대 20일 거래대금 vs 보정 값, 실제 factor 중앙
재실행: py research/studies/38_trend_support_realtime/round3_factor_fix.py
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd

import common

DATA = common.STUDY / "data"
OUTPUT = common.STUDY / "results" / "round3"
DAUM_DIRECTORY = DATA / "daum_unadjusted"
DATAGOKR_DIRECTORY = DATA / "datagokr_unadjusted"
CORRECTIONS = DATA / "raw_close_corrections.parquet"
OUTLIERS = common.STUDY / "results" / "round2" / "turnover_outliers.csv"
LEVELS = (1.02, 2.0, 10.0, 30.0)     # 어긋남 배수 보고 칸(내가 정함 — 세는 칸이고 보정 여부를 정하지 않는다)
JUMP_LEVEL = 10.0                    # 무수정 종가 튐 검사: 전 행 대비 10 배 이상(지시 "수십 배" 의 아래 끝, 내가 정함)


def load_truth(directory: Path, label: str) -> pd.DataFrame:
    frames = []

    for path in sorted(directory.glob("*.parquet")):
        frame = pd.read_parquet(path, columns=["date", "close", "trade_value"])
        frame.insert(0, "code", path.stem)
        frames.append(frame)

    if not frames:
        return pd.DataFrame(columns=["code", "date", "close", "trade_value", "source"])

    truth = pd.concat(frames, ignore_index=True)
    truth["source"] = label
    return truth


def off_counts(error: np.ndarray, codes: np.ndarray) -> dict:
    result = {}

    for level in LEVELS:
        chosen = np.abs(error) > np.log(level)
        result[f"over_{level:g}x"] = {"rows": int(chosen.sum()), "stocks": int(len(np.unique(codes[chosen])))}

    return result


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    stocks, _, _ = common.study33.load_inputs(common.LAST_DATE)
    panel = pd.DataFrame({
        "code": np.concatenate([np.full(len(series.dates), series.code) for series in stocks]),
        "date": np.concatenate([series.dates for series in stocks]),
        "close": np.concatenate([series.close for series in stocks]),
        "volume": np.concatenate([series.volume for series in stocks]),
        "factor": np.concatenate([series.factor for series in stocks]),
        "factor_source": np.concatenate([series.factor_source for series in stocks]),
        "delisted": np.concatenate([np.full(len(series.dates), series.delisted) for series in stocks])})
    panel["raw_close_panel"] = panel["close"] * panel["factor"]
    panel["turnover_panel"] = panel["raw_close_panel"] * panel["volume"]
    truth = pd.concat([load_truth(DAUM_DIRECTORY, "daum"), load_truth(DATAGOKR_DIRECTORY, "datagokr")], ignore_index=True)
    truth = truth[(truth["close"] > 0) & (truth["trade_value"] > 0)].drop_duplicates(["code", "date"])
    merged = panel.merge(truth.rename(columns={"close": "truth_close", "trade_value": "truth_value"}),
                         on=["code", "date"], how="left")
    assert len(merged) == len(panel)
    has_truth = merged["truth_close"].notna().to_numpy()
    proxy_turnover = (merged["close"] * merged["volume"]).to_numpy()
    merged["raw_close_corrected"] = np.where(has_truth, merged["truth_close"], merged["raw_close_panel"])
    merged["turnover_corrected"] = np.where(has_truth, merged["truth_value"], proxy_turnover)
    merged["status"] = np.where(has_truth, merged["source"].fillna(""), "proxy_adjusted_close_times_volume")
    output = merged[["code", "date", "raw_close_panel", "raw_close_corrected", "turnover_panel", "turnover_corrected", "status"]]
    output = output.astype({"status": "category"})
    output.to_parquet(CORRECTIONS, index=False)

    # 전수 대조: 2회차 값 vs 실제(출처 있는 행만)
    signal = (merged["date"] >= common.SIGNAL_FROM).to_numpy()
    codes = merged["code"].to_numpy()
    part = has_truth & signal

    with np.errstate(divide="ignore", invalid="ignore"):
        close_error = np.log(merged["raw_close_panel"].to_numpy() / merged["truth_close"].to_numpy())
        turnover_error = np.log(merged["turnover_panel"].to_numpy() / merged["truth_value"].to_numpy())
        proxy_error = np.log(proxy_turnover / merged["truth_value"].to_numpy())

    valid_turnover = part & np.isfinite(turnover_error)
    listed = ~merged["delisted"].to_numpy()

    # 무수정 종가 튐 전수 검사(2회차 값): 전 행 대비 10 배 이상인데 수정 종가는 ±35% 안
    previous_raw = np.r_[np.nan, merged["raw_close_panel"].to_numpy()[:-1]]
    previous_close = np.r_[np.nan, merged["close"].to_numpy()[:-1]]
    first_row = np.r_[True, codes[1:] != codes[:-1]]
    previous_raw[first_row] = np.nan
    previous_close[first_row] = np.nan

    with np.errstate(divide="ignore", invalid="ignore"):
        raw_jump = np.abs(np.log(merged["raw_close_panel"].to_numpy() / previous_raw)) >= np.log(JUMP_LEVEL)
        adjusted_calm = np.abs(merged["close"].to_numpy() / previous_close - 1.0) <= common.PRICE_BREAK

    jumps = raw_jump & adjusted_calm & signal
    jump_codes = np.unique(codes[jumps])
    jump_frame = merged.loc[jumps, ["code", "date", "status"]].copy()
    jump_frame["truth_available"] = has_truth[jumps]
    stock_error = pd.DataFrame({"code": codes[valid_turnover], "error": np.abs(turnover_error[valid_turnover])})
    stocks_off_ten = set(stock_error.loc[stock_error["error"] > np.log(10), "code"])

    summary = {
        "sources": {"daum": json.loads((DATA / "daum_unadjusted_meta.json").read_text(encoding="utf-8")),
                    "datagokr": json.loads((DATA / "datagokr_unadjusted_meta.json").read_text(encoding="utf-8"))
                    if (DATA / "datagokr_unadjusted_meta.json").exists() else None},
        "rows_total": int(len(merged)), "rows_signal_period": int(signal.sum()),
        "rows_by_status_signal_period": {str(key): int(value) for key, value in
                                         merged.loc[signal, "status"].value_counts().items()},
        "stocks_by_status": {str(key): int(value) for key, value in
                             merged.loc[signal].groupby("status")["code"].nunique().items()},
        "round2_raw_close_vs_truth": off_counts(close_error[part & np.isfinite(close_error)], codes[part & np.isfinite(close_error)]),
        "round2_turnover_vs_truth": off_counts(turnover_error[valid_turnover], codes[valid_turnover]),
        "round2_turnover_vs_truth_by_factor_source": {
            str(source): off_counts(turnover_error[valid_turnover & (merged["factor_source"].to_numpy() == source)],
                                    codes[valid_turnover & (merged["factor_source"].to_numpy() == source)])
            for source in (0, 1, 2)},
        "proxy_validation_listed": {
            "note": "상장 중 종목에서 대용값(수정 종가 × 네이버 거래량)을 실제 거래대금과 대조. 출처 없는 행의 오차 추정치.",
            **off_counts(proxy_error[valid_turnover & listed], codes[valid_turnover & listed]),
            "share_within_20_percent": float(np.mean(np.abs(proxy_error[valid_turnover & listed]) < np.log(1.2))),
            "round2_value_share_within_20_percent": float(np.mean(np.abs(turnover_error[valid_turnover & listed]) < np.log(1.2)))},
        "raw_close_jump_scan": {
            "rule": f"2회차 무수정 종가가 전 행 대비 {JUMP_LEVEL:g} 배 이상 바뀌고 수정 종가 변화는 ±35% 안인 날(신호 기간)",
            "rows": int(jumps.sum()), "stocks": int(len(jump_codes)),
            "rows_with_truth": int(jump_frame["truth_available"].sum()),
            "stocks_without_truth": int(jump_frame.loc[~jump_frame["truth_available"], "code"].nunique()),
            "jump_stocks_also_turnover_off_10x": int(len(set(jump_codes) & stocks_off_ten)),
            "turnover_off_10x_stocks_total": int(len(stocks_off_ten))}}

    # 종목별 표
    per_stock = pd.DataFrame({"code": codes, "status": merged["status"].to_numpy(), "signal": signal,
                              "turnover_error": np.where(valid_turnover, np.abs(turnover_error), np.nan),
                              "close_error": np.where(part, np.abs(close_error), np.nan), "jump": jumps})
    grouped = per_stock[per_stock["signal"]].groupby("code")
    stock_table = pd.DataFrame({
        "rows": grouped.size(), "rows_with_truth": grouped["turnover_error"].count(),
        "main_status": grouped["status"].agg(lambda values: values.value_counts().index[0]),
        "turnover_rows_off_2x": grouped["turnover_error"].apply(lambda values: int((values > np.log(2)).sum())),
        "turnover_rows_off_10x": grouped["turnover_error"].apply(lambda values: int((values > np.log(10)).sum())),
        "turnover_max_off_multiple": np.exp(grouped["turnover_error"].max()),
        "raw_close_rows_off_10x": grouped["close_error"].apply(lambda values: int((values > np.log(10)).sum())),
        "raw_close_jump_days": grouped["jump"].sum()}).reset_index()
    names = {series.code: series.name for series in stocks}
    stock_table.insert(1, "name", stock_table["code"].map(names))
    common.write_csv(OUTPUT / "factor_fix_stocks.csv", stock_table)

    # 25 종목 대조: 20일 평균 거래대금 최대값 2회차 vs 보정
    outliers = pd.read_csv(OUTLIERS, dtype={"code": str})
    rows = []

    for _, outlier in outliers.iterrows():
        chosen = merged[merged["code"] == outlier["code"]]
        before = chosen["turnover_panel"].rolling(common.TURNOVER_DAYS).mean()
        after = chosen["turnover_corrected"].rolling(common.TURNOVER_DAYS).mean()
        in_signal = chosen["date"] >= common.SIGNAL_FROM
        truth_factor = (chosen["truth_close"] / chosen["close"])[in_signal]
        rows.append({"code": outlier["code"], "name": outlier["name"],
                     "round2_max_turnover20_krw": float(before[in_signal].max()),
                     "corrected_max_turnover20_krw": float(after[in_signal].max()),
                     "max_ratio_round2_over_corrected": float((before / after)[in_signal].max()),
                     "round2_median_factor": float(chosen.loc[in_signal, "factor"].median()),
                     "truth_median_factor": float(truth_factor.median()) if truth_factor.notna().any() else np.nan,
                     "rows_with_truth_share": float(chosen.loc[in_signal, "truth_close"].notna().mean()),
                     "main_status": chosen.loc[in_signal, "status"].value_counts().index[0]})

    fixed = pd.DataFrame(rows)
    common.write_csv(OUTPUT / "turnover_outliers_fixed.csv", fixed)
    summary["outliers_25"] = {"stocks": len(fixed),
                              "with_truth": int((fixed["rows_with_truth_share"] > 0.5).sum()),
                              "round2_inflated_over_10x": int((fixed["max_ratio_round2_over_corrected"] > 10).sum()),
                              "round2_inflated_over_2x": int((fixed["max_ratio_round2_over_corrected"] > 2).sum())}
    common.write_json(OUTPUT / "factor_fix_summary.json", summary)
    print(json.dumps({key: summary[key] for key in ("rows_by_status_signal_period", "round2_turnover_vs_truth",
                                                    "raw_close_jump_scan", "outliers_25")}, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
