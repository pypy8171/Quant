"""재무 팩터(저PBR × 고ROE) — 기준일 하나를 주면 그날 알 수 있던 값만으로 종목별 점수를 낸다.

백테스트(`research/studies/19_fundamental_factors/run_pbr_roe.py`)와 장전 잡이 같은 `compute(as_of)`를 부른다.
스펙 정본: `research/RESET_2026-09-19_R2/fundamental-quant.md` §2·§3 후보 1. 열 이름 매핑은 `load_fin()` 한 곳에서만 한다.

입력(모두 시점 고정 표)
  PYQuant/data/fin/fin_point_in_time.parquet     rcept_no당 한 행. 발효일 = 접수일. 등급 B(corpCode가 현재 목록)
  PYQuant/data/fin/shares_point_in_time.parquet  상장주식수. 2020~ data.go.kr 월말(등급 A), 2015~2019 DART 사업보고서(등급 B)
  PYQuant/data/bars_all_pit_v2.parquet           일봉(상폐 포함). 등급 A

[formula] bp = 자본총계 / (주식수 × 종가), pbr = 1 / bp.
          roe = 당기순이익_TTM / 평균(자본총계_현재, 자본총계_4분기전).
          TTM = 올해 누계 + 전년 연간 − 전년 같은 분기 누계(연간 보고서는 연간 그대로). 전년 분기가 없으면 전년 연간으로 대신한다(등급 B).
          score = w · z(ln bp) + (1 − w) · z(roe), z는 그날 유니버스 안에서 1%·99% 절단 뒤 표준화. 하나라도 결측이면 제외.

유니버스(§2-3, 기준일마다 다시): 보통주(코드 끝 0, ETF·스팩·리츠 이름 제외) · 상장 12개월 이상 · 20일 평균 거래대금 ≥ 10억 ·
  시가총액 ≥ 500억 · 최근 20거래일 거래량 0인 날 < 3 · 금융업 제외(DART 은행·보험 계정이 있거나 이름에 금융 낱말) ·
  12월 결산 · 자본총계 > 0. KIND 업종·결산월 표가 아직 없어 두 조건은 재무 표 자체에서 유추한다(등급 B).

사용:
  from PYQuant.features.fundamental import FundamentalData, compute
  frame = compute(datetime.date(2026, 8, 31))            # 한 번 쓸 때(데이터를 그때 읽는다)
  data = FundamentalData.load(); data.compute(as_of)      # 여러 기준일(백테스트)
  py PYQuant/features/fundamental.py --as-of 2026-08-31    # 표 확인
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from PYQuant.data.point_in_time import as_of_join  # noqa: E402

FIN_PATH = REPO_ROOT / "PYQuant" / "data" / "fin" / "fin_point_in_time.parquet"
SHARES_PATH = REPO_ROOT / "PYQuant" / "data" / "fin" / "shares_point_in_time.parquet"
BARS_PATH = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
ETF_TOKENS_PATH = REPO_ROOT / "Quant" / "config" / "etf_name_tokens.json"

ANNUAL_REPORT = "11011"
FINANCIAL_ACCOUNT_COLUMNS = ["account_예수부채", "account_보험계약부채", "account_보험계약자산", "account_순이자손익", "account_순수수료손익"]
FINANCIAL_NAME_TOKENS = ("은행", "금융", "보험", "증권", "캐피탈", "카드", "생명", "화재", "저축은행", "투자증권", "손해", "자산운용")
NON_COMMON_NAME_TOKENS = ("스팩", "리츠")

BARS_FROM = pd.Timestamp("2014-06-01")          # 20일 창·12개월 상장 경과 계산에 넉넉한 시작
LIQUIDITY_WINDOW_DAYS = 20
MIN_TURNOVER_KRW = 1_000_000_000
MIN_MARKET_CAP_KRW = 50_000_000_000
MAX_ZERO_VOLUME_DAYS = 2
MIN_LISTED_DAYS = 365
FIN_MAX_AGE = pd.Timedelta(days=400)             # 사업보고서 뒤 13개월 넘게 새 보고서가 없으면 옛 값 차단
SHARES_MAX_AGE = pd.Timedelta(days=460)
WINSOR_LOW, WINSOR_HIGH = 0.01, 0.99
DEFAULT_VALUE_WEIGHT = 0.5

FEATURE_COLUMNS = ["ticker", "pbr", "roe", "score", "grade", "bp", "market_cap", "total_equity", "net_income_ttm",
                   "fin_effective_date", "fin_period_end", "shares_effective_date", "shares_source", "close", "turnover_20d"]
OUTPUT_COLUMNS = FEATURE_COLUMNS + ["as_of", "name", "ttm_is_fallback", "z_value", "z_quality"]


def load_fin(path: Path = FIN_PATH) -> pd.DataFrame:
    """재무 표 열 이름을 스펙 이름으로 맞추는 유일한 자리. 실제 파일 열(2026-09-20 실측): stock_code, corp_code, bsns_year,
    reprt_code, rcept_no, fs_div, effective_date, period_end, net_income, net_income_ytd, total_equity, account_* …"""
    columns = ["stock_code", "bsns_year", "reprt_code", "rcept_no", "fs_div", "effective_date", "period_end",
               "net_income", "net_income_ytd", "total_equity"] + FINANCIAL_ACCOUNT_COLUMNS
    frame = pd.read_parquet(path, columns=columns)
    frame = frame.rename(columns={"stock_code": "ticker", "bsns_year": "fiscal_year", "reprt_code": "report_code"})
    frame["ticker"] = frame["ticker"].astype(str).str.zfill(6)
    frame["report_code"] = frame["report_code"].astype(str)
    frame["rcept_no"] = frame["rcept_no"].astype(str)
    frame["published_at"] = pd.to_datetime(frame["rcept_no"].str[:8], format="%Y%m%d", errors="coerce")
    frame["effective_date"] = pd.to_datetime(frame["effective_date"])
    frame["period_end"] = pd.to_datetime(frame["period_end"])
    return frame


def derive_report_fields(fin: pd.DataFrame) -> pd.DataFrame:
    """보고서 행마다 TTM 순이익·4분기 전 자본총계를 붙인다. 전년 값 조회는 처음 공시된 판(rcept_no 최소)만 쓴다 — 정정판은 뒤에 나오므로."""
    original = (fin.sort_values("rcept_no").drop_duplicates(["ticker", "fs_div", "fiscal_year", "report_code"], keep="first")
                .set_index(["ticker", "fs_div", "fiscal_year", "report_code"]))
    prior_index = pd.MultiIndex.from_arrays([fin["ticker"], fin["fs_div"], fin["fiscal_year"] - 1, fin["report_code"]])
    prior_annual_index = pd.MultiIndex.from_arrays([fin["ticker"], fin["fs_div"], fin["fiscal_year"] - 1,
                                                    pd.Series([ANNUAL_REPORT] * len(fin), index=fin.index)])

    prior_same_ytd = original["net_income_ytd"].reindex(prior_index).to_numpy()
    prior_same_equity = original["total_equity"].reindex(prior_index).to_numpy()
    prior_annual_net_income = original["net_income"].reindex(prior_annual_index).to_numpy()
    prior_annual_equity = original["total_equity"].reindex(prior_annual_index).to_numpy()

    is_annual = (fin["report_code"] == ANNUAL_REPORT).to_numpy()
    ttm_from_quarters = fin["net_income_ytd"].to_numpy() + prior_annual_net_income - prior_same_ytd
    ttm = np.where(is_annual, fin["net_income"].to_numpy(), ttm_from_quarters)
    ttm_fallback = np.where(is_annual, fin["net_income"].to_numpy(), prior_annual_net_income)
    ttm_grade_b = ~is_annual & np.isnan(ttm) & ~np.isnan(ttm_fallback)

    derived = fin.copy()
    derived["net_income_ttm"] = np.where(np.isnan(ttm), ttm_fallback, ttm)
    derived["ttm_is_fallback"] = ttm_grade_b
    equity_4q = np.where(is_annual, prior_annual_equity, prior_same_equity)
    derived["total_equity_4q_ago"] = np.where(np.isnan(equity_4q), prior_annual_equity, equity_4q)
    annual_month = (derived.loc[is_annual].assign(month=lambda frame: frame["period_end"].dt.month)
                    .groupby("ticker")["month"].agg(lambda series: int(series.mode().iloc[0])))
    derived["fiscal_month"] = derived["ticker"].map(annual_month).fillna(12).astype(int)
    financial_by_account = derived[FINANCIAL_ACCOUNT_COLUMNS].notna().any(axis=1)
    derived["is_financial_account"] = financial_by_account.groupby(derived["ticker"]).transform("any")
    return derived


def newest_period_only(derived: pd.DataFrame) -> pd.DataFrame:
    """발효일 순서로 볼 때 "지금까지 가장 최근 분기"인 행만 남긴다 — 옛 분기의 정정 공시가 새 분기 값을 덮어쓰지 않게."""
    ordered = derived.sort_values(["ticker", "effective_date", "published_at", "rcept_no"], kind="stable")
    running_max = ordered.groupby("ticker")["period_end"].cummax()
    return ordered[ordered["period_end"] >= running_max].copy()


def load_shares(path: Path = SHARES_PATH) -> pd.DataFrame:
    frame = pd.read_parquet(path, columns=["ticker", "published_at", "effective_date", "shares_common_issued", "source", "grade"])
    frame["ticker"] = frame["ticker"].astype(str).str.zfill(6)
    return frame.rename(columns={"source": "shares_source", "grade": "shares_grade"})


def load_etf_tokens(path: Path = ETF_TOKENS_PATH) -> tuple:
    return tuple(json.loads(path.read_text(encoding="utf-8")))


@dataclass
class BarsPanel:
    """일봉을 날짜×종목 패널 셋으로 — 종가·거래대금 20일 평균·거래량 0 일수. 상장 첫날은 전 구간에서 잰다."""
    close: pd.DataFrame
    volume: pd.DataFrame
    turnover_20d: pd.DataFrame
    zero_volume_20d: pd.DataFrame
    first_date: pd.Series
    names: pd.Series

    @classmethod
    def load(cls, path: Path = BARS_PATH, start: pd.Timestamp = BARS_FROM) -> "BarsPanel":
        first_date = pd.read_parquet(path, columns=["Date", "code"]).groupby("code")["Date"].min()
        bars = pd.read_parquet(path, columns=["Date", "Close", "Volume", "code", "name"])
        bars = bars[bars["Date"] >= start]
        names = bars.drop_duplicates("code", keep="last").set_index("code")["name"]
        close = bars.pivot_table(index="Date", columns="code", values="Close", aggfunc="last").astype(float)
        volume = bars.pivot_table(index="Date", columns="code", values="Volume", aggfunc="last").astype(float)
        turnover = (close * volume).rolling(LIQUIDITY_WINDOW_DAYS, min_periods=LIQUIDITY_WINDOW_DAYS).mean()
        zero_volume = (volume.fillna(0) <= 0).astype(float).where(volume.notna()).rolling(
            LIQUIDITY_WINDOW_DAYS, min_periods=1).sum()
        return cls(close, volume, turnover, zero_volume, first_date, names)

    @property
    def trading_days(self) -> pd.DatetimeIndex:
        return self.close.index

    def cross_section(self, as_of: pd.Timestamp) -> pd.DataFrame:
        """기준일 하루의 종목별 종가·거래대금·거래량0 일수·상장일. 그날 봉이 없는 종목은 뺀다."""
        if as_of not in self.close.index:
            raise KeyError(f"{as_of:%Y-%m-%d}는 거래일이 아니다")

        frame = pd.DataFrame({"close": self.close.loc[as_of], "turnover_20d": self.turnover_20d.loc[as_of],
                              "zero_volume_20d": self.zero_volume_20d.loc[as_of]})
        frame = frame[frame["close"].notna()]
        frame["first_date"] = self.first_date.reindex(frame.index)
        frame["name"] = self.names.reindex(frame.index)
        frame.index.name = "ticker"
        return frame.reset_index()


def winsorized_z(values: pd.Series) -> pd.Series:
    clipped = values.clip(values.quantile(WINSOR_LOW), values.quantile(WINSOR_HIGH))
    spread = clipped.std(ddof=0)
    return (clipped - clipped.mean()) / spread if spread > 0 else clipped * 0.0


@dataclass
class FundamentalData:
    """세 입력을 한 번 읽어 두고 기준일마다 `compute`를 부른다."""
    bars: BarsPanel
    fin: pd.DataFrame
    shares: pd.DataFrame
    etf_tokens: tuple

    @classmethod
    def load(cls) -> "FundamentalData":
        fin = newest_period_only(derive_report_fields(load_fin()))
        return cls(BarsPanel.load(), fin, load_shares(), load_etf_tokens())

    def is_common_stock(self, ticker: pd.Series, name: pd.Series) -> pd.Series:
        tokens = tuple(self.etf_tokens) + NON_COMMON_NAME_TOKENS
        name_text = name.fillna("")
        not_fund = ~name_text.apply(lambda text: any(token in text for token in tokens))
        return ticker.str.endswith("0") & not_fund

    def candidates(self, as_of: pd.Timestamp) -> pd.DataFrame:
        """유니버스 필터를 다 건 뒤 재무·주식수를 붙인 표. 필터 통과 이유를 열로 남겨 진단에 쓴다."""
        section = self.bars.cross_section(as_of)
        section = section[self.is_common_stock(section["ticker"], section["name"])]
        section = section[(as_of - section["first_date"]).dt.days >= MIN_LISTED_DAYS]
        section = section[section["turnover_20d"] >= MIN_TURNOVER_KRW]
        section = section[section["zero_volume_20d"] <= MAX_ZERO_VOLUME_DAYS]
        section["as_of"] = as_of

        with_shares = as_of_join(section, self.shares, on="ticker", left_time="as_of", right_time="effective_date",
                                 max_age=SHARES_MAX_AGE).rename(columns={"effective_date": "shares_effective_date"})
        with_shares["market_cap"] = with_shares["shares_common_issued"].astype(float) * with_shares["close"]
        with_shares = with_shares[with_shares["market_cap"] >= MIN_MARKET_CAP_KRW]

        fin_columns = ["ticker", "effective_date", "published_at", "period_end", "total_equity", "total_equity_4q_ago",
                       "net_income_ttm", "ttm_is_fallback", "fiscal_month", "is_financial_account"]
        with_fin = as_of_join(with_shares, self.fin[fin_columns], on="ticker", left_time="as_of", right_time="effective_date",
                              max_age=FIN_MAX_AGE).rename(columns={"effective_date": "fin_effective_date",
                                                                   "period_end": "fin_period_end"})
        financial_name = with_fin["name"].fillna("").apply(lambda text: any(token in text for token in FINANCIAL_NAME_TOKENS))
        with_fin["is_financial"] = with_fin["is_financial_account"].eq(True) | financial_name
        return with_fin

    def compute(self, as_of, value_weight: float = DEFAULT_VALUE_WEIGHT) -> pd.DataFrame:
        """DataFrame[ticker, pbr, roe, score, grade, …]. 점수는 그날 유니버스 안에서만 매긴다. 결측 종목은 뺀다."""
        as_of = pd.Timestamp(as_of)
        frame = self.candidates(as_of)
        frame = frame[~frame["is_financial"] & (frame["fiscal_month"] == 12)]
        frame = frame[(frame["total_equity"].astype(float) > 0) & frame["net_income_ttm"].notna()]

        if frame.empty:
            return pd.DataFrame(columns=OUTPUT_COLUMNS)

        equity = frame["total_equity"].astype(float)
        equity_4q = frame["total_equity_4q_ago"].astype(float).where(lambda series: series > 0)
        average_equity = pd.concat([equity, equity_4q], axis=1).mean(axis=1)
        frame["bp"] = equity / frame["market_cap"]
        frame["pbr"] = 1.0 / frame["bp"]
        frame["roe"] = frame["net_income_ttm"].astype(float) / average_equity
        frame = frame[np.isfinite(frame["bp"]) & np.isfinite(frame["roe"]) & (frame["bp"] > 0)]
        frame["z_value"] = winsorized_z(np.log(frame["bp"]))
        frame["z_quality"] = winsorized_z(frame["roe"])
        frame["score"] = value_weight * frame["z_value"] + (1.0 - value_weight) * frame["z_quality"]
        # 재무 표 자체가 등급 B(corpCode가 현재 목록·정정 반영 부분)라 지금은 전부 B. 상폐 회사 corp_code가 채워지면 A/B를 가른다.
        frame["grade"] = "B"
        frame["as_of"] = as_of
        ordered = frame.sort_values("score", ascending=False).reset_index(drop=True)
        return ordered[OUTPUT_COLUMNS]


_DEFAULT_DATA: FundamentalData | None = None


def compute(as_of: datetime.date, value_weight: float = DEFAULT_VALUE_WEIGHT) -> pd.DataFrame:
    """라이브·단발 호출용. 처음 부를 때 데이터를 읽고 프로세스 안에서 재사용한다."""
    global _DEFAULT_DATA

    if _DEFAULT_DATA is None:
        _DEFAULT_DATA = FundamentalData.load()

    return _DEFAULT_DATA.compute(as_of, value_weight)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--as-of", required=True, help="기준일 YYYY-MM-DD(거래일)")
    parser.add_argument("--top", type=int, default=30)
    arguments = parser.parse_args()
    frame = compute(datetime.date.fromisoformat(arguments.as_of))
    print(f"기준일 {arguments.as_of}: 점수 있는 종목 {len(frame):,}")
    print(frame.head(arguments.top)[["ticker", "name", "pbr", "roe", "score", "market_cap", "fin_period_end", "shares_source"]].to_string())
    return 0


if __name__ == "__main__":
    sys.exit(main())
