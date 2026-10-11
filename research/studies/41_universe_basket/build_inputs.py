"""스터디 41 입력 빌드 — 종목군(23개)·신호(10개)·체결용 가격 행렬을 한 번 만들어 캐시에 둔다.

자료(새로 받은 것 없음):
  - 스터디 38 Panel(PYQuant/data/bars_all_pit_v2.parquet 보통주, 상장폐지 포함) + round3 무수정 종가·거래대금 보정
    + round11 정지 점프 고침(clean_close)·폐지 배수(delisting_table). 가격은 전부 '정제 수정가'(매매와 차트가 같은 기준).
  - 시가총액 주식수: PYQuant/data/fin/shares_point_in_time.parquet(2016년부터 씀).
  - 수급: PYQuant/data/investor_flow_pit_2010.parquet(외국인+기관 순매수 수량 × 무수정 종가, 등급 B).
  - 재무: PYQuant/data/fin/fin_point_in_time.parquet(effective_date 기준, 2015-06부터, 등급 B).
  - 시장(PIT): 현재 시장 + KIND 폐지표(PYQuant/data/universe/kind_delisted.parquet)의 코스닥 → 코스피 이전 기록.
  - 지수: 스터디 38 data/kospi_daily_naver.parquet, 스터디 39 data/kosdaq_daily_naver.parquet.
미래 정보 차단(구조):
  - 종목군 소속은 전부 결정일 d 전날(d−1)까지 확정된 값: 전날까지 20일 평균 거래대금 순위, 전날 무수정 종가,
    전날까지 20일 변동성, 전날 주식수 × 전날 무수정 종가(시가총액). 시장은 그날 PIT 시장.
  - 가격 신호(존·250일 고가·저변동·모멘텀·반전)는 d 종가까지(같은 날 종가 체결은 15:20 근사).
  - 수급·재무는 d−1 까지(재무는 effective_date < d).
출력: PYQuant/data/cache/study41/inputs.npz (gitignore). 재실행하면 덮어쓴다(같은 입력이면 같은 해시).
실행: py -I -X utf8 research/studies/41_universe_basket/build_inputs.py
"""
from __future__ import annotations

import hashlib
import re
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY.parent / "38_trend_support_realtime"))
sys.path.insert(0, str(STUDY.parent / "39_devscale_exit_vs_trend"))

import common     # noqa: E402  numba 보다 먼저
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402

import round3_test     # noqa: E402
import round4_test     # noqa: E402
import round11_test     # noqa: E402

REPOSITORY = common.REPOSITORY
CACHE = REPOSITORY / "PYQuant" / "data" / "cache" / "study41"
KOSPI = STUDY.parent / "38_trend_support_realtime" / "data" / "kospi_daily_naver.parquet"
KOSDAQ = STUDY.parent / "39_devscale_exit_vs_trend" / "data" / "kosdaq_daily_naver.parquet"
BARS = REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
FLOW = REPOSITORY / "PYQuant" / "data" / "investor_flow_pit_2010.parquet"
FIN = REPOSITORY / "PYQuant" / "data" / "fin" / "fin_point_in_time.parquet"
KIND = REPOSITORY / "PYQuant" / "data" / "universe" / "kind_delisted.parquet"

FIRST_DATE = 20100104
LAST_DATE = 20261008
CAP_FROM = 20160101                 # 시가총액 종목군은 2016년부터(shares_point_in_time 덮는 범위)
LIMIT_CHANGE = 20150615             # 가격제한폭 15% → 30%
LIMIT_MARGIN = 0.005                # 내가 정함: 제한폭 − 0.5%p 이상이면 잠김으로 본다(호가 반올림 여유)
ZONE_LOW, ZONE_HIGH, ZONE_ATR = -0.08, 0.05, 0.045     # DevScale 존(엔진 기본값)
# 보통주가 아닌 투자회사형 상장사(선박·유전·인프라·리츠·해외개발 펀드). 이름 패턴으로 뽑고 일반 기업 3개를 손으로 뺐다(내가 정함, choices_log 41행)
FUND_PATTERN = re.compile(r"리츠|REIT|선박|바다로|하이골드|아시아퍼시픽|유전|개발\d|인프라|맥쿼리|부동산투자|\d+호|펀드|투자회사|베트남")
FUND_NOT = ("HD현대인프라코어", "NICE인프라", "바이오인프라")
ACTION_RATIO = 1.5                  # 내가 정함: 무수정÷수정 비율이 하루에 이만큼 넘게 바뀌면 분할·감자·자본 반환 사건으로 본다(42행)
ACTION_LEAD = 30                    # 내가 정함: 사건 날까지 이 거래일 수만큼 종목군에서 뺀다(공시가 먼저 나온다고 봄, 42행)
TRANSFER_WORDS = ("유가증권시장 상장", "증권거래소 상장", "이전상장")

UNIVERSES = (("T50", "거래대금 상위 50"), ("T100", "거래대금 상위 100"), ("T200", "거래대금 상위 200"),
             ("T500", "거래대금 상위 500"), ("T500OUT", "거래대금 500위 밖"),
             ("CAP1", "시가총액 1분위(작음)"), ("CAP2", "시가총액 2분위"), ("CAP3", "시가총액 3분위"),
             ("CAP4", "시가총액 4분위"), ("CAP5", "시가총액 5분위(큼)"),
             ("KOSPI", "코스피"), ("KOSDAQ", "코스닥"),
             ("PRICE1", "가격 1분위(쌈)"), ("PRICE2", "가격 2분위"), ("PRICE3", "가격 3분위"), ("PRICE4", "가격 4분위"),
             ("PRICE5", "가격 5분위(비쌈)"),
             ("VOL1", "변동성 1분위(낮음)"), ("VOL2", "변동성 2분위"), ("VOL3", "변동성 3분위"), ("VOL4", "변동성 4분위"),
             ("VOL5", "변동성 5분위(높음)"), ("ALL", "전 종목"))
SIGNALS = (("ZONE", "DevScale 존(정배열·SMA20 −8 – +5%·ATR ≤4.5%, 거래대금 큰 순)"), ("HIGH250", "종가 ÷ 250일 고가"),
           ("LOWVOL", "20일 변동성 낮은 순"), ("FLOW5", "5일 외국인+기관 순매수 ÷ 거래대금(양수만)"),
           ("FLOW20", "20일 외국인+기관 순매수 ÷ 거래대금(양수만)"), ("MOM", "12-1개월 모멘텀"), ("REV", "1개월 반전(21일 수익 낮은 순)"),
           ("VALUE", "장부가 ÷ 시가총액(자본 > 0)"), ("QUAL", "연간 영업이익 ÷ 총자산"), ("VQ", "가치·퀄리티 순위 평균"))


def shift_rows(values: np.ndarray, panel, rows: int) -> np.ndarray:
    out = np.full(values.shape[0], np.nan)
    out[rows:] = values[:-rows]
    offset = np.arange(panel.row_count) - panel.starts[panel.stock_of_row]
    out[offset < rows] = np.nan
    return out


def previous_known(values: np.ndarray, panel, previous_traded: np.ndarray) -> np.ndarray:
    out = round4_test.shift_within_stock(values.astype(np.float64), panel.starts)
    out[~previous_traded] = np.nan
    return out


def day_quantile(values: np.ndarray, position: np.ndarray, parts: int) -> np.ndarray:
    """그날 단면 안 분위(0 = 가장 작음). 값 없으면 −1."""
    out = np.full(values.shape[0], -1, dtype=np.int8)
    valid = np.flatnonzero(np.isfinite(values))
    frame = pd.DataFrame({"day": position[valid], "value": values[valid]})
    rank = frame.groupby("day")["value"].rank(method="first").to_numpy()
    size = frame.groupby("day")["value"].transform("size").to_numpy()
    out[valid] = np.minimum((rank - 1) * parts // size, parts - 1).astype(np.int8)
    return out


def day_rank_descending(values: np.ndarray, position: np.ndarray) -> np.ndarray:
    """그날 단면 안 큰 순 순위(1 = 가장 큼). 값 없으면 NaN."""
    out = np.full(values.shape[0], np.nan)
    valid = np.flatnonzero(np.isfinite(values))
    frame = pd.DataFrame({"day": position[valid], "value": values[valid]})
    out[valid] = frame.groupby("day")["value"].rank(ascending=False, method="first").to_numpy()
    return out


def day_percentile(values: np.ndarray, position: np.ndarray) -> np.ndarray:
    out = np.full(values.shape[0], np.nan)
    valid = np.flatnonzero(np.isfinite(values))
    frame = pd.DataFrame({"day": position[valid], "value": values[valid]})
    out[valid] = frame.groupby("day")["value"].rank(pct=True).to_numpy()
    return out


def pit_market(panel) -> tuple[np.ndarray, pd.DataFrame]:
    """행마다 0 코스피·1 코스닥. 현재 코스피인데 KIND 에 코스닥 → 코스피 이전 기록이 있으면 그날 전은 코스닥."""
    current = pd.read_parquet(BARS, columns=["code", "market"]).drop_duplicates("code").set_index("code")["market"]
    current = current.reindex(panel.codes)

    if current.isna().any():
        raise SystemExit(f"시장 구분 없는 종목 {int(current.isna().sum())}")

    stock_market = current.str.startswith("KOSDAQ").to_numpy().astype(np.int8)
    kind = pd.read_parquet(KIND)
    moved = kind[(kind["market"] == "KOSDAQ") & kind["reason"].astype(str).apply(lambda text: any(transfer_word in text for transfer_word in TRANSFER_WORDS))]
    moved = moved.assign(day=pd.to_datetime(moved["delist_date"]).dt.strftime("%Y%m%d").astype(np.int64))
    row_market = stock_market[panel.stock_of_row].copy()
    index_of = {code: index for index, code in enumerate(panel.codes)}
    applied = []

    for code, day in zip(moved["code"], moved["day"]):
        stock = index_of.get(code)

        if stock is None or stock_market[stock] != 0:
            continue

        first, last = panel.starts[stock], panel.starts[stock + 1]
        part = panel.dates[first:last] < day
        row_market[first:last][part] = 1
        applied.append({"code": code, "name": panel.names[stock], "moved_on": int(day), "rows_set_kosdaq": int(part.sum())})

    return row_market, pd.DataFrame(applied)


def fin_features(panel, previous_cap: np.ndarray) -> tuple[np.ndarray, np.ndarray, dict]:
    """B/P(가장 최근 보고서 자본 ÷ 전날 시가총액)와 퀄리티(가장 최근 연간 보고서 영업이익 ÷ 총자산). effective_date < d."""
    fin = pd.read_parquet(FIN, columns=["stock_code", "bsns_year", "reprt_code", "fs_div", "effective_date", "period_end",
                                        "operating_income_ytd", "total_assets", "total_equity"])
    fin["cfs"] = (fin["fs_div"] == "CFS").astype(np.int8)
    fin = fin.sort_values(["stock_code", "bsns_year", "reprt_code", "cfs"]).drop_duplicates(["stock_code", "bsns_year", "reprt_code"], keep="last")
    fin["effective_int"] = fin["effective_date"].dt.strftime("%Y%m%d").astype(np.int64)
    fin["period_int"] = fin["period_end"].dt.strftime("%Y%m%d").astype(np.int64)
    rows = pd.DataFrame({"ticker": np.array(panel.codes)[panel.stock_of_row], "date": panel.dates, "row": np.arange(panel.row_count)})
    rows = rows.sort_values(["date", "ticker"], kind="mergesort")

    def as_of(table: pd.DataFrame, column: str) -> np.ndarray:
        table = table[np.isfinite(table[column])].sort_values(["effective_int", "period_int", "stock_code"], kind="mergesort")
        table = table.drop_duplicates(["stock_code", "effective_int"], keep="last")
        merged = pd.merge_asof(rows, table[["stock_code", "effective_int", column]].rename(columns={"stock_code": "ticker"}),
                               left_on="date", right_on="effective_int", by="ticker", direction="backward", allow_exact_matches=False)
        out = np.full(panel.row_count, np.nan)
        out[merged["row"].to_numpy()] = merged[column].to_numpy()
        return out

    equity = as_of(fin, "total_equity")
    annual = fin[fin["reprt_code"] == "11011"].copy()

    with np.errstate(invalid="ignore", divide="ignore"):
        annual["quality"] = annual["operating_income_ytd"] / annual["total_assets"].where(annual["total_assets"] > 0)
        quality = as_of(annual, "quality")
        value = np.where((equity > 0) & (previous_cap > 0), equity / previous_cap, np.nan)

    return value, quality, {"fin_rows_after_dedupe": int(len(fin)), "annual_rows": int(len(annual))}


def main() -> None:
    started = time.time()
    CACHE.mkdir(parents=True, exist_ok=True)
    panel = common.Panel()
    correction = round3_test.apply_corrections(panel)
    panel.build_deciles()
    close, jumps = round11_test.clean_close(panel, panel.shares_issued)
    delist = round11_test.delisting_table(panel, close)
    starts, position = panel.starts, panel.market_position
    multiplier = close / panel.close
    open_adj = panel.open_price * multiplier
    high_adj = panel.high * multiplier
    low_adj = panel.low * multiplier
    unadjust_clean = (panel.raw_close / panel.close) / multiplier
    raw_open = open_adj * unadjust_clean
    raw_close = panel.raw_close
    previous_row_ok = np.r_[False, position[1:] == position[:-1] + 1]
    previous_row_ok[starts[:-1]] = False
    print(f"패널 {panel.stock_count} 종목 {panel.row_count} 행 · {time.time() - started:.0f}초", flush=True)

    # ── 가격 신호(d 종가까지) ──
    moving_average = {window: common.rolling_mean_known(close, starts, window, True) for window in (5, 10, 20)}
    previous_close = round4_test.shift_within_stock(close, starts)
    true_range = (np.maximum(high_adj, np.where(np.isfinite(previous_close), previous_close, -np.inf))
                  - np.minimum(low_adj, np.where(np.isfinite(previous_close), previous_close, np.inf)))
    atr14 = common.rolling_mean_known(true_range, starts, 14, True)
    one_day = close / previous_close - 1.0
    filled = np.nan_to_num(one_day)
    mean20 = common.rolling_mean_known(filled, starts, 20, True)
    square20 = common.rolling_mean_known(filled * filled, starts, 20, True)
    offset = np.arange(panel.row_count) - starts[panel.stock_of_row]
    rv20 = np.sqrt(np.maximum(square20 - mean20 * mean20, 0.0) * 20.0 / 19.0)
    rv20[offset < 21] = np.nan
    high250 = (pd.Series(high_adj).groupby(panel.stock_of_row).rolling(250, min_periods=250).max()
               .reset_index(level=0, drop=True).sort_index().to_numpy())

    with np.errstate(invalid="ignore", divide="ignore"):
        deviation = close / moving_average[20] - 1.0
        zone = ((moving_average[5] > moving_average[10]) & (moving_average[10] > moving_average[20]) & (deviation >= ZONE_LOW) & (deviation <= ZONE_HIGH)
                & (atr14 / moving_average[20] <= ZONE_ATR))
        signal = {
            "ZONE": np.where(zone, panel.turnover20, np.nan),
            "HIGH250": close / high250,
            "LOWVOL": -rv20,
            "MOM": shift_rows(close, panel, 21) / shift_rows(close, panel, 252) - 1.0,
            "REV": -(close / shift_rows(close, panel, 21) - 1.0),
        }

    # ── 수급(d−1 까지) ──
    flow = pd.read_parquet(FLOW, columns=["date", "ticker", "foreign_net_quantity", "institution_net_quantity"])
    flow["date"] = flow["date"].dt.strftime("%Y%m%d").astype(np.int64)
    flow["net"] = (flow["foreign_net_quantity"] + flow["institution_net_quantity"]).astype(np.float64)
    rows = pd.DataFrame({"ticker": np.array(panel.codes)[panel.stock_of_row], "date": panel.dates})
    net = rows.merge(flow[["ticker", "date", "net"]], on=["ticker", "date"], how="left")["net"].to_numpy()
    value = net * raw_close
    known = np.isfinite(value).astype(np.float64)
    flow_coverage = float(np.isfinite(value[(panel.dates >= FIRST_DATE) & (panel.dates <= LAST_DATE)]).mean())
    del flow, rows

    for span in (5, 20):
        flow_mean = common.rolling_mean_known(np.nan_to_num(value), starts, span, True)
        known_mean = common.rolling_mean_known(known, starts, span, True)
        turnover_mean = common.rolling_mean_known(np.nan_to_num(panel.day_turnover), starts, span, True)

        with np.errstate(invalid="ignore", divide="ignore"):
            ratio = np.where((known_mean >= 1.0) & (turnover_mean > 0), flow_mean / turnover_mean, np.nan)

        ratio = previous_known(ratio, panel, previous_row_ok)
        signal[f"FLOW{span}"] = np.where(ratio > 0, ratio, np.nan)

    # ── 종목군 재료(d−1 까지) ──
    previous_20d = round4_test.shift_within_stock(panel.turnover20, starts)
    previous_raw = previous_known(raw_close, panel, previous_row_ok)
    previous_rv = previous_known(rv20, panel, previous_row_ok)
    previous_cap = previous_known(panel.market_cap, panel, previous_row_ok)
    value_ratio, quality, fin_info = fin_features(panel, previous_cap)
    signal["VALUE"] = value_ratio
    signal["QUAL"] = quality

    with np.errstate(invalid="ignore"):
        signal["VQ"] = np.where(np.isfinite(value_ratio) & np.isfinite(quality),
                                (day_percentile(value_ratio, position) + day_percentile(quality, position)) / 2.0, np.nan)

    row_market, transfers = pit_market(panel)

    # ── 종목군에서 빼는 행: 투자회사형 종목 전부, 수정 비율 사건 날까지 ACTION_LEAD 거래일 ──
    fund_stock = np.array([bool(FUND_PATTERN.search(str(name))) and str(name) not in FUND_NOT for name in panel.names])
    excluded = fund_stock[panel.stock_of_row].copy()

    with np.errstate(invalid="ignore", divide="ignore"):
        factor = raw_close / close
        change = factor / round4_test.shift_within_stock(factor, starts)

    events = np.flatnonzero(np.isfinite(change) & ((change > ACTION_RATIO) | (change < 1.0 / ACTION_RATIO)))
    stock_start = starts[panel.stock_of_row]

    for row in events:
        excluded[max(int(stock_start[row]), int(row) - ACTION_LEAD):int(row) + 1] = True

    print(f"종목군 제외: 투자회사형 {int(fund_stock.sum())} 종목, 수정 비율 사건 {len(events)} 건, 제외 행 {int(excluded.sum())}", flush=True)

    def ranked(values):
        return np.where(excluded, np.nan, values)

    turnover_rank = day_rank_descending(ranked(previous_20d), position)
    cap_part = day_quantile(ranked(np.where(panel.dates >= CAP_FROM, previous_cap, np.nan)), position, 5)
    price_part = day_quantile(ranked(previous_raw), position, 5)
    volatility_part = day_quantile(ranked(previous_rv), position, 5)
    bits = np.zeros(panel.row_count, dtype=np.uint32)
    names = [name for name, _ in UNIVERSES]

    def put(name, mask):
        nonlocal bits
        bits |= np.where(mask, np.uint32(1) << np.uint32(names.index(name)), np.uint32(0)).astype(np.uint32)

    with np.errstate(invalid="ignore"):
        for top in (50, 100, 200, 500):
            put(f"T{top}", turnover_rank <= top)

        put("T500OUT", turnover_rank > 500)

    for part in range(5):
        put(f"CAP{part + 1}", cap_part == part)
        put(f"PRICE{part + 1}", price_part == part)
        put(f"VOL{part + 1}", volatility_part == part)

    put("KOSPI", row_market == 0)
    put("KOSDAQ", row_market == 1)
    put("ALL", np.isfinite(previous_raw))
    bits[excluded] = 0

    # ── 시장일 행렬 ──
    market_dates = panel.market_dates
    first_day = int(np.searchsorted(market_dates, FIRST_DATE))
    last_day = int(np.searchsorted(market_dates, LAST_DATE, side="right"))
    dates = market_dates[first_day:last_day].astype(np.int64)
    day_count, stock_count = len(dates), panel.stock_count
    keep = (position >= first_day) & (position < last_day)
    day_of_row = position[keep] - first_day
    stock_of_row = panel.stock_of_row[keep]

    def matrix(values, dtype=np.float64, fill=np.nan):
        out = np.full((day_count, stock_count), fill, dtype=dtype)
        out[day_of_row, stock_of_row] = values[keep]
        return out

    with np.errstate(invalid="ignore", divide="ignore"):
        limit = np.where(panel.dates >= LIMIT_CHANGE, 0.30, 0.15) - LIMIT_MARGIN
        open_ratio = open_adj / previous_close - 1.0
        close_ratio = close / previous_close - 1.0
        lock = ((open_ratio >= limit).astype(np.int8) | ((open_ratio <= -limit).astype(np.int8) << 1)
                | ((close_ratio >= limit).astype(np.int8) << 2) | ((close_ratio <= -limit).astype(np.int8) << 3)).astype(np.int8)

    stock_last = np.full(stock_count, day_count, dtype=np.int64)     # 상장 끝 종목은 마지막 거래일 다음 날 현금화
    delist_multiplier = np.ones(stock_count)

    for _, row in delist.iterrows():
        stock = int(row["stock"])
        last_position = int(position[starts[stock + 1] - 1]) - first_day
        stock_last[stock] = last_position
        delist_multiplier[stock] = float(row["multiplier"])

    kospi = pd.read_parquet(KOSPI).set_index("date").reindex(dates)
    kosdaq = pd.read_parquet(KOSDAQ).set_index("date").reindex(dates)

    def index_pair(frame):
        index_close = frame["close"].ffill().to_numpy()
        index_open = frame["open"].fillna(pd.Series(index_close, index=frame.index).shift(1)).to_numpy()
        return index_open, index_close, int(frame["close"].isna().sum())

    kospi_open, kospi_close, kospi_missing = index_pair(kospi)
    kosdaq_open, kosdaq_close, kosdaq_missing = index_pair(kosdaq)
    week = pd.to_datetime(pd.Series(dates.astype(str))).dt.isocalendar()
    week_key = (week["year"].to_numpy() * 100 + week["week"].to_numpy()).astype(np.int64)
    month_key = dates // 100
    weekly_end = np.r_[week_key[1:] != week_key[:-1], True]
    monthly_end = np.r_[month_key[1:] != month_key[:-1], True]
    weekly_end[-1] = monthly_end[-1] = False       # 마지막 날은 체결할 다음 날이 없다
    decision_days = np.flatnonzero(weekly_end | monthly_end).astype(np.int64)
    scores = np.full((len(SIGNALS), len(decision_days), stock_count), np.nan, dtype=np.float32)

    for index, (name, _) in enumerate(SIGNALS):
        scores[index] = matrix(signal[name])[decision_days].astype(np.float32)

    universe = matrix(bits.astype(np.float64), np.float64, 0.0)[decision_days].astype(np.uint32)
    payload = {
        "dates": dates, "codes": np.array(panel.codes), "names": np.array(panel.names),
        "open": matrix(open_adj), "close": matrix(close), "high": matrix(high_adj).astype(np.float32),
        "low": matrix(low_adj).astype(np.float32), "raw_open": matrix(raw_open), "raw_close": matrix(raw_close),
        "volume": matrix(panel.volume).astype(np.float32),
        "previous_20d": matrix(previous_20d).astype(np.float32), "lock": matrix(lock.astype(np.float64), np.float64, 0).astype(np.int8),
        "market": matrix(row_market.astype(np.float64), np.float64, -1).astype(np.int8),
        "sma20": matrix(moving_average[20]).astype(np.float32),
        "stock_last": stock_last, "delist_multiplier": delist_multiplier, "listed_at_end": panel.listed_at_end,
        "kospi_open": kospi_open, "kospi_close": kospi_close, "kosdaq_open": kosdaq_open, "kosdaq_close": kosdaq_close,
        "decision_days": decision_days, "weekly_end": weekly_end, "monthly_end": monthly_end,
        "scores": scores, "universe": universe,
        "turnover_rank": matrix(turnover_rank)[decision_days].astype(np.float32),
        "previous_cap": matrix(previous_cap)[decision_days].astype(np.float32),
        "previous_rv": matrix(previous_rv)[decision_days].astype(np.float32),
        "tax": round11_test.sell_tax(dates), "new_tick": dates >= round11_test.NEW_TICK_FROM,
    }
    np.savez(CACHE / "inputs.npz", **payload)
    delist.to_csv(CACHE / "delist_table.csv", index=False, lineterminator="\n")
    transfers.to_csv(CACHE / "market_transfers.csv", index=False, lineterminator="\n")
    digest = hashlib.sha256((CACHE / "inputs.npz").read_bytes()).hexdigest()
    coverage = {name: float(np.isfinite(scores[index]).sum(axis=1).mean()) for index, (name, _) in enumerate(SIGNALS)}
    sizes = {name: float(((universe >> np.uint32(index)) & 1).sum(axis=1).mean()) for index, (name, _) in enumerate(UNIVERSES)}
    info = {"inputs_sha256": digest, "days": day_count, "stocks": stock_count, "decision_days": int(len(decision_days)),
            "first_date": int(dates[0]), "last_date": int(dates[-1]), "jump_fixes": int(len(jumps)),
            "delisted_stocks": int(len(delist)), "market_transfers_applied": int(len(transfers)),
            "kospi_missing_days": kospi_missing, "kosdaq_missing_days": kosdaq_missing, "flow_coverage": flow_coverage,
            "correction": correction, "fin": fin_info, "mean_candidates_per_decision": coverage,
            "mean_universe_size": sizes, "seconds": round(time.time() - started, 1), "reproduction": panel.reproduction()}
    common.write_json(CACHE / "inputs_info.json", info)
    print(f"끝 {time.time() - started:.0f}초 · sha256 {digest[:16]}", flush=True)


if __name__ == "__main__":
    main()
