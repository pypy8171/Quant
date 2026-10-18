"""45-W. 구간 20개 고점 날에 거시 지표가 어땠는지 — 지표 28개를 한 표로(검정 없음).

입력: macro_conditions.build()(코스피·미 10년물·WTI·VIX·원달러), raw/fred/*.csv(FRED 공개 CSV, 2026-10-16 받음),
      PYQuant/data/macro/ecos_cpi·ecos_exports(FRED 한국 CPI가 2023-11에서 끊겨 그 뒤를 잇는다),
      PYQuant/.index_cache S&P500·달러인덱스
산출: macro_wide.csv — 구간마다 고점 날 지표값과 고점→바닥 코스피·S&P500 하락 폭
월간 지표는 발표가 늦으니 관측 달 첫날에 LAG_MONTHLY일을 더한 날부터 보이는 것으로 붙인다(미국 CPI·PPI·실업률은
다음 달 둘째 주, 한국 CPI·수출은 다음 달 첫날 발표). 일간 해외 값은 한국 날짜 전날 값이다.
"""
from __future__ import annotations

import pandas as pd

from macro_conditions import build
from run_crisis_regime import HERE, REPO, read_index

FRED = HERE / "raw" / "fred"
MACRO = REPO / "PYQuant" / "data" / "macro"
LAG_MONTHLY = 45       # 월간 지표: 관측 달 첫날 + 45일부터 쓴다
LAG_WEEKLY = 12        # 주간 실업수당 청구: 주 끝 + 5일 발표, 넉넉히 12일
YEAR = 252


def fred(series: str) -> pd.DataFrame:
    frame = pd.read_csv(FRED / f"{series}.csv")
    frame.columns = ["date", "value"]
    frame["date"] = pd.to_datetime(frame["date"])
    frame["value"] = pd.to_numeric(frame["value"], errors="coerce")
    return frame.dropna().reset_index(drop=True)


def yoy_monthly(frame: pd.DataFrame) -> pd.DataFrame:
    frame = frame.sort_values("date").copy()
    frame["value"] = (frame["value"] / frame["value"].shift(12) - 1) * 100
    return frame.dropna()


def ecos_monthly(name: str) -> pd.DataFrame:
    frame = pd.read_parquet(MACRO / f"ecos_{name}.parquet")
    frame = frame.sort_values("fetched_at").drop_duplicates("observation_date", keep="last")
    return frame.rename(columns={"observation_date": "date"})[["date", "value"]].sort_values("date").reset_index(drop=True)


def spliced_yoy(old: pd.DataFrame, new: pd.DataFrame) -> pd.DataFrame:
    """두 출처의 전년비를 이어 붙인다 — 기준연도가 달라도 전년비는 같다. 겹치면 한국은행(new) 값을 쓴다
    (FRED 한국 수출은 원화 기준이라 달러 기준 한국은행 값과 다르다)."""
    old_yoy = yoy_monthly(old)
    new_yoy = yoy_monthly(new)
    return pd.concat([old_yoy[old_yoy["date"] < new_yoy["date"].min()], new_yoy], ignore_index=True)


def published(frame: pd.DataFrame, lag_days: int) -> pd.DataFrame:
    frame = frame.copy()
    frame["date"] = frame["date"] + pd.Timedelta(days=lag_days)
    return frame


def attach(daily: pd.DataFrame, frame: pd.DataFrame, column: str) -> pd.DataFrame:
    other = frame[["date", "value"]].rename(columns={"value": column}).sort_values("date")
    return pd.merge_asof(daily, other, on="date", direction="backward", allow_exact_matches=False)


def build_wide() -> pd.DataFrame:
    daily = build()
    daily = attach(daily, fred("DFF"), "fed")
    daily = attach(daily, fred("T10Y2Y"), "t10y2y")
    daily = attach(daily, fred("T10Y3M"), "t10y3m")
    daily = attach(daily, fred("BAA10Y"), "baa")
    daily = attach(daily, published(yoy_monthly(fred("CPIAUCSL")), LAG_MONTHLY), "cpi")
    daily = attach(daily, published(yoy_monthly(fred("CPILFESL")), LAG_MONTHLY), "core_cpi")
    daily = attach(daily, published(yoy_monthly(fred("PPIACO")), LAG_MONTHLY), "ppi")
    daily = attach(daily, published(fred("UNRATE"), LAG_MONTHLY), "unrate")
    daily = attach(daily, published(fred("UMCSENT"), LAG_MONTHLY), "sentiment")
    claims = fred("ICSA")
    claims["value"] = claims["value"].rolling(4).mean()
    daily = attach(daily, published(claims.dropna(), LAG_WEEKLY), "claims")
    daily = attach(daily, published(fred("IRSTCI01KRM156N"), LAG_MONTHLY), "kr_rate")
    daily = attach(daily, published(spliced_yoy(fred("KORCPIALLMINMEI"), ecos_monthly("cpi")), LAG_MONTHLY), "kr_cpi")
    daily = attach(daily, published(spliced_yoy(fred("XTEXVA01KRM664S"), ecos_monthly("exports")), LAG_MONTHLY), "kr_export")
    for name, column in (("DX-Y.NYB", "dxy"), ("_GSPC", "spx")):
        other = read_index(name)[["date", "close"]].rename(columns={"close": "value"})
        daily = attach(daily, other, column)

    daily["fed_chg"] = daily["fed"] - daily["fed"].shift(YEAR)
    daily["kr_rate_chg"] = daily["kr_rate"] - daily["kr_rate"].shift(YEAR)
    daily["cpi_chg"] = daily["cpi"] - daily["cpi"].shift(YEAR // 2)
    daily["unrate_rise"] = daily["unrate"] - daily["unrate"].rolling(YEAR, min_periods=1).min()
    daily["claims_chg"] = (daily["claims"] / daily["claims"].shift(YEAR // 2) - 1) * 100
    daily["dxy_chg"] = (daily["dxy"] / daily["dxy"].shift(YEAR // 2) - 1) * 100
    daily["real_tnx"] = daily["tnx"] - daily["cpi"]      # 실질금리: 그때 물가상승률을 뺀 금리
    daily["real_fed"] = daily["fed"] - daily["cpi"]
    daily["spx_dd"] = (daily["spx"] / daily["spx"].rolling(YEAR, min_periods=1).max() - 1) * 100
    return daily


def episode_rows(daily: pd.DataFrame) -> pd.DataFrame:
    quality = pd.read_csv(HERE / "episode_quality.csv")
    episodes = quality[quality["rule"] == "B1"][["key", "name", "peak_date", "trough_date", "maxdd"]].reset_index(drop=True)
    indexed = daily.set_index(daily["date"].dt.strftime("%Y-%m-%d"))
    columns = ["tnx", "real_tnx", "tnx_chg", "fed", "real_fed", "fed_chg", "t10y2y", "t10y3m", "baa", "cpi", "cpi_chg", "core_cpi", "ppi",
               "unrate", "unrate_rise", "claims_chg", "sentiment", "vix", "wti_chg", "dxy_chg", "krw", "krw_chg",
               "kr_rate", "kr_rate_chg", "kr_cpi", "kr_export", "spx_dd"]
    peak = indexed.loc[episodes["peak_date"], columns].reset_index(drop=True)
    rows = pd.concat([episodes, peak], axis=1)
    rows["vix_trough"] = indexed.loc[episodes["trough_date"], "vix"].to_numpy()
    spx_window = []
    for peak_date, trough_date in zip(episodes["peak_date"], episodes["trough_date"]):
        window = indexed.loc[peak_date:trough_date, "spx"]
        spx_window.append((window.min() / window.iloc[0] - 1) * 100)
    rows["spx_drop"] = spx_window
    rows["bear"] = rows["maxdd"] <= -20
    return rows.round(2)


def main() -> None:
    rows = episode_rows(build_wide())
    rows.to_csv(HERE / "macro_wide.csv", index=False)
    print(rows.to_string())


if __name__ == "__main__":
    main()
