"""45-M. 금리·유가·VIX·환율 수준별로 코스피가 하락장으로 갔는지 아닌지 — 서술 표(검정 없음).

입력: PYQuant/.index_cache(코스피·미국10년물 ^TNX·WTI CL=F·VIX·원달러 KRW=X), episode_quality.csv(구간 고점·바닥)
산출: macro_episodes.csv(구간 20개 고점 날의 거시 값과 낙폭), macro_buckets.csv(지표 구간별 6개월 안 −20% 하락 비율과 해당 연도)
해외 값은 한국 날짜보다 앞선 날짜의 마지막 값만 쓴다(run_crisis_regime.py와 같음).
"""
from __future__ import annotations

import pandas as pd

from run_crisis_regime import HERE, load_kospi, read_index

FORWARD = 120          # 6개월(거래일)
DROP_LINE = -0.20      # 그날 종가 대비 6개월 안 최저 종가가 이만큼 빠지면 "하락장으로 감"
CHANGE_DAYS = 120      # 금리·유가·환율 변화를 재는 기간

BUCKETS = {
    "vix": ("VIX", [0, 15, 20, 25, 30, 40, 200]),
    "tnx": ("미국 10년물 금리(%)", [0, 2, 3, 4, 5, 6, 20]),
    "tnx_chg": ("미국 10년물 6개월 변화(%p)", [-10, -1, -0.5, 0, 0.5, 1, 10]),
    "wti_chg": ("WTI 유가 6개월 변화(%)", [-100, -30, -10, 10, 30, 60, 1000]),
    "krw_chg": ("원달러 6개월 변화(%)", [-100, -5, 0, 5, 10, 1000]),
}


def attach(frame: pd.DataFrame, name: str, column: str) -> pd.DataFrame:
    other = read_index(name)[["date", "close"]].rename(columns={"close": column})
    return pd.merge_asof(frame, other, on="date", direction="backward", allow_exact_matches=False)


def build() -> pd.DataFrame:
    daily = load_kospi()[["date", "close"]]
    for name, column in (("_TNX", "tnx"), ("CL=F", "wti"), ("_VIX", "vix"), ("KRW=X", "krw")):
        daily = attach(daily, name, column)

    daily["wti"] = daily["wti"].where(daily["wti"] > 0)
    daily["tnx_chg"] = daily["tnx"] - daily["tnx"].shift(CHANGE_DAYS)
    daily["wti_chg"] = (daily["wti"] / daily["wti"].shift(CHANGE_DAYS) - 1) * 100
    daily["krw_chg"] = (daily["krw"] / daily["krw"].shift(CHANGE_DAYS) - 1) * 100
    future_min = daily["close"][::-1].rolling(FORWARD, min_periods=FORWARD).min()[::-1].shift(-1)
    daily["fwd_min"] = future_min / daily["close"] - 1
    daily["dd"] = daily["close"] / daily["close"].rolling(252, min_periods=1).max() - 1
    return daily


def episode_table(daily: pd.DataFrame) -> pd.DataFrame:
    quality = pd.read_csv(HERE / "episode_quality.csv")
    episodes = quality[quality["rule"] == "B1"][["key", "name", "grade", "peak_date", "trough_date", "maxdd"]].copy()
    indexed = daily.set_index(daily["date"].dt.strftime("%Y-%m-%d"))
    columns = ["tnx", "tnx_chg", "wti", "wti_chg", "vix", "krw_chg"]
    peak = indexed.loc[episodes["peak_date"], columns].reset_index(drop=True)
    episodes = pd.concat([episodes.reset_index(drop=True), peak], axis=1)
    episodes["vix_trough"] = indexed.loc[episodes["trough_date"], "vix"].to_numpy()
    episodes["bear"] = episodes["maxdd"] <= DROP_LINE * 100
    return episodes.round(2)


def bucket_table(daily: pd.DataFrame) -> pd.DataFrame:
    usable = daily.dropna(subset=["fwd_min"]).copy()
    usable["year"] = usable["date"].dt.year
    usable["hit"] = usable["fwd_min"] <= DROP_LINE
    rows = []
    for column, (label, edges) in BUCKETS.items():
        frame = usable.dropna(subset=[column])
        frame = frame.assign(bucket=pd.cut(frame[column], edges, right=False))
        for bucket, group in frame.groupby("bucket", observed=True):
            by_year = group.groupby("year")["hit"].agg(["sum", "count"])
            hit_years = by_year[by_year["sum"] > 0].index.tolist()
            miss_years = by_year[(by_year["sum"] == 0) & (by_year["count"] >= 20)].index.tolist()
            rows.append({"indicator": column, "label": label, "low": bucket.left, "high": bucket.right,
                         "days": len(group), "years": int(len(by_year)), "hit_share": round(group["hit"].mean() * 100, 1),
                         "hit_years": " ".join(map(str, hit_years)), "no_hit_years": " ".join(map(str, miss_years))})
    return pd.DataFrame(rows)


def main() -> None:
    daily = build()
    episodes = episode_table(daily)
    buckets = bucket_table(daily)
    episodes.to_csv(HERE / "macro_episodes.csv", index=False)
    buckets.to_csv(HERE / "macro_buckets.csv", index=False)
    print(episodes.to_string())
    print(buckets.drop(columns=["no_hit_years"]).to_string())
    print("기준 비율(전체 날):", round(daily.dropna(subset=["fwd_min"])["fwd_min"].le(DROP_LINE).mean() * 100, 1))


if __name__ == "__main__":
    main()
