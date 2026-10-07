"""스터디 37 — 오르는 이평선 지지 모양을 추세 조건 없이 다시 시험, 청산선부터(사용자 요청 2026-10-07).

모양(고정): 신호일 장중 저가 ≤ 이평선 × 1.01, 종가 > 이평선, 이평선 > 전일 이평선. N ∈ {5, 10, 20}. 다음 날 시가 매수.
자료·거름·가격 단절·비용·비교 기준은 standalone_swing.py 를 그대로 import 해서 쓴다(정의는 swing_definitions.py).
  E1 청산 없이 60거래일 보유 때 최대 하락(MAE)·최대 상승(MFE) 분포
  E2 청산 격자 익절 5 × 손절 4 × 최대 보유 3 = 60칸 × N 3 = 180칸
  E3 (a) 해마다 그 전에 끝난 매매만으로 180칸 중 순수익 묶음 t 최고 칸을 골라 이어붙이기 (b) 고른 칸 이웃 부호
  E4 하루 신규 5·동시 10·건당 200만 자산 곡선, 비교 = 같은 슬롯·같은 순서로 모양 없이 산 경우
  E5 고른 청산에서 추세(k=5)별 분해
미래 정보: 이평선은 신호일 종가까지, 확정 저점은 k 일 뒤 행부터(swing_definitions), 매수는 다음 날 시가, 청산은 매수일 장중부터.
재실행(저장소 루트): py -X utf8 research/studies/37_swing_trend/ma_support_exits.py
산출: ma_support_exits.txt(시험 목록·결과), ma_support_exits_metrics.json, ma_support_cells.parquet(180칸 요약),
      ma_support_walk_trades.parquet(E3(a) 이어붙인 매매), ma_support_portfolio.parquet(E4 일별 자산),
      ma_support_portfolio_trades.parquet(E4 매매 기록, side = shape/baseline).
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))
import standalone_swing as base  # noqa: E402
import swing_definitions as swing  # noqa: E402

study33 = base.study33
COST = base.COST
REPOSITORY = base.REPOSITORY
LAST_DATE = base.LAST_DATE
SIGNAL_FROM = base.SIGNAL_FROM
SPLIT_DATE = base.SPLIT_DATE
MIN_CLOSE_KRW = base.MIN_CLOSE_KRW
MIN_TURNOVER20_KRW = base.MIN_TURNOVER20_KRW
TURNOVER_DAYS = base.TURNOVER_DAYS
PRICE_BREAK = base.PRICE_BREAK

WINDOWS = (5, 10, 20)
TREND_SPAN = 5
TAKES = (5, 8, 10, 15, 20)                     # 익절 %
STOPS = ("S5", "S7", "S10", "SP")               # 고정 5·7·10% / 근거 저점 × 0.97
STOP_FIXED = {"S5": 0.05, "S7": 0.07, "S10": 0.10}
PIVOT_STOP_BELOW = 0.97
HOLDS = (10, 20, 60)
HOLD_LIMIT = max(HOLDS)
HORIZONS = (5, 10, 20, 60)
WALK_WARMUP_YEARS = 3
PASS_WALK_T = 2.0
DAILY_NEW = 5
MAX_HELD = 10
TRADE_KRW = 2_000_000.0
CAPITAL_KRW = 20_000_000.0
BASELINE_POOL = 15                              # 모양 없음 비교: 하루 거래대금 상위 15종목에서 고름(보유 10 + 신규 5)
BENCHMARK_WIDTH = 200                           # 매수일–청산일 시장 거래일 차 상한(넘으면 개별 계산)
TREND_NAMES = {swing.TREND_UP: "상승", swing.TREND_SIDEWAYS: "횡보", swing.TREND_DOWN: "하향", swing.TREND_UNKNOWN: "미정"}

OUTPUT_TEXT = STUDY / "ma_support_exits.txt"
OUTPUT_METRICS = STUDY / "ma_support_exits_metrics.json"
OUTPUT_CELLS = STUDY / "ma_support_cells.parquet"
OUTPUT_WALK = STUDY / "ma_support_walk_trades.parquet"
OUTPUT_PORTFOLIO = STUDY / "ma_support_portfolio.parquet"
OUTPUT_PORTFOLIO_TRADES = STUDY / "ma_support_portfolio_trades.parquet"
RANDOM_SEEDS = tuple(range(1, 11))              # E4 보조: 모양 있음·무작위 순서 10회(판정 안 씀)

PLAN = f"""스터디 37 오르는 이평선 지지 모양 — 추세 조건 없이, 청산선부터
=========================================================
[배경] 직전 standalone_swing.txt 에서 이 모양을 하향 추세 종목에서 갖춘 대조군 DOWN_k5_N10 이 강했다(n 89,560,
  순수익 +0.49%/건, 초과 +0.59%p, 묶음 t 6.73). 사전 등록 칸이 아니었으므로 이번이 정식 시험이다.
[모양 — 고정] 신호일 저가 ≤ 이평선 × 1.01, 종가 > 이평선, 이평선 > 전일 이평선. 추세 조건 없음. N ∈ {{5,10,20}}.
  추세 꼬리표(k=5 확정 저점·고점, 상승/횡보/하향/미정)는 신호일 기준으로 붙여 나눠 보기만 한다. 판정은 "추세 무관" 전체.
[고정 조건 — standalone_swing 과 같음]
  자료: bars_all_pit_v2.parquet 전 종목(상장폐지 포함) 수정주가 일봉, 신호 {SIGNAL_FROM}–{LAST_DATE}, 기간을 자르지 않음.
  거름: 신호일 무수정 종가 ≥ 2,000원, 20일 평균 무수정 거래대금 ≥ 5억. 매수 = 신호 다음 거래일 시가.
  가격 단절: 신호일 다음–청산일에 전날 종가 대비 ±35% 밖 시가·종가가 있으면 그 매매를 뺀다(보유 자리는 차지).
  비용: 스터디 35 ma_sweep.COST = {COST:.6f}(왕복 비율). 비교 = 같은 날 시가에 같은 거름 전 종목을 사서 같은 청산일 종가에
  판 평균, 초과 = 순수익 − 비교. 묶음 t = 매수 월로 묶은 표준오차(CR1).
[시험 칸 — 실행 전에 고정]
  E1 청산 없이 매수일 포함 60거래일 보유: 5·10·20·60일까지 MAE·MFE 중앙·하위 10%·상위 10%, N별·추세별.
     이긴 매매(60일째 종가 순수익 > 0)가 고점 전에 빠진 폭. 모든 신호를 따로 셈(겹침 허용, 60일 다 찬 것만 — 내가 정함).
  E2 청산 격자 180칸 = 익절 {{5,8,10,15,20}}% × 손절 {{고정 5,7,10% (매수가 기준), 근거 저점(k=5 마지막 확정 저점)×0.97}}
     × 최대 보유 {{10,20,60}}거래일(매수일이 0일째, H일째 종가) × N {{5,10,20}}.
     장중 저가가 손절선 이하면 손절(시가가 이미 아래면 시가), 고가가 익절선 이상이면 익절(시가가 위면 시가), 같은 날 둘 다면 손절.
     칸마다 같은 종목 보유 중 재신호 무시. 근거 저점이 아직 없거나 시가 ≤ 근거 저점 손절선이면 그 손절 칸에선 안 삼(내가 정함).
     H일이 아직 안 찬 매매(시세가 마지막 날까지 이어짐)는 뺌. 상장폐지 등으로 끊기면 마지막 종가.
     지표: 순수익 평균, 초과, 순수익 묶음 t, 초과 묶음 t, 2010–17·2018–26 순수익, 연도별, 승률, 평균 보유일.
  E3 (a) 해마다 그 해 1월 1일 전에 청산된 매매만으로 180칸의 순수익 묶음 t 를 재고 최고 칸을 골라 그 해 매수분을 이어붙임.
         2010–12 3년 쌓인 뒤 2013부터. 이어붙인 순수익·초과·묶음 t·두 기간(2013–17, 2018–26)·연도별.
     (b) 고른 칸 = 2026년에 쓰인 칸(2025년 말까지 자료로 고른 칸, 지금 라이브에 올린다면 이 칸 — 내가 정함).
         이웃 = 같은 N 에서 익절·손절·보유를 각각 한 단계 옆으로(손절 순서 5→7→10%→근거 저점 — 내가 정함). 전 기간 순수익 부호.
  E4 자산 곡선 2013-01–끝: 자본 2,000만, 건당 200만(고정, 복리 아님), 하루 신규 최대 {DAILY_NEW}건, 동시 보유 최대 {MAX_HELD}건.
     후보 = 그 해 E3(a)가 고른 칸의 N 신호(같은 칸 청산 적용), 신호일 20일 평균 거래대금 큰 순(내가 정함). 보유 중 종목 제외.
     청산한 날의 자리는 다음 날부터 씀, 가격 단절 매매는 후보에서 뺌, 끝에 열린 매매는 마지막 종가로 평가(내가 정함).
     비교 = 같은 날 신호일 거름을 지난 전 종목(모양 없음)을 같은 순서(거래대금 큰 순)·같은 슬롯·같은 청산으로 산 경우.
     연도별 수익률(그 해 초 자산 대비), 최대 낙폭(일별 종가 평가), 건수.
  E5 E3(b)의 고른 칸, 그리고 E3(a) 이어붙인 매매를 추세별로: 순수익·묶음 t, 하향 − 나머지 차이와 묶음 t.
[통과 기준 — 내가 정함]
  ① E3(a) 이어붙인 순수익 > 0, 묶음 t ≥ {PASS_WALK_T}, 2013–17·2018–26 둘 다 순수익 > 0
  ② E3(b) 이웃 칸 전 기간 순수익 모두 > 0
  ③ E4 자산 곡선 총수익 > 0 이고 모양 없음 비교보다 큼
  셋 다면 "모의 시험 후보", 아니면 빠진 항목을 적는다. 참고로 헌장 기준(180칸 비교 → 한 칸 t 3.5)도 E2 최고 칸 옆에 적는다.
"""


def month_codes(months: np.ndarray) -> np.ndarray:
    """매수 월(YYYYMM)을 0.. 정수로."""
    return np.unique(months, return_inverse=True)[1]


def fast_cluster_t(values: np.ndarray, clusters: np.ndarray) -> float:
    """평균의 묶음(CR1) t. clusters 는 0.. 정수."""
    count = len(values)

    if count < 3:
        return float("nan")

    residual = values - values.mean()
    sums = np.bincount(clusters, weights=residual)
    used = np.bincount(clusters) > 0
    groups = int(used.sum())

    if groups < 2:
        return float("nan")

    error = np.sqrt(groups / (groups - 1) * float((sums[used] ** 2).sum())) / count
    return float(values.mean() / error) if error > 0 else float("nan")


class Market:
    """전 종목 일봉을 한 줄로 이어 붙인 배열. 행 = (종목, 거래일)."""

    def __init__(self) -> None:
        stocks, _, information = study33.load_inputs(LAST_DATE)
        self.information = information
        self.codes = [series.code for series in stocks]
        self.market_dates = np.unique(np.concatenate([series.dates for series in stocks]))
        last_market_date = int(self.market_dates[-1])
        date_count, stock_count = len(self.market_dates), len(stocks)
        self.open_matrix = np.full((date_count, stock_count), np.nan)
        close_matrix = np.full((date_count, stock_count), np.nan)
        self.eligible_matrix = np.zeros((date_count, stock_count), dtype=bool)
        break_matrix = np.full((date_count, stock_count), np.nan)
        pieces = {name: [] for name in ("open", "high", "low", "close", "dates", "market_index", "break_count", "eligible",
                                        "turnover20", "stock", "latest_low", "trend", "stock_end")}
        averages = {window: [] for window in WINDOWS}
        self.stock_ended = np.zeros(stock_count, dtype=bool)
        start = 0

        for column, series in enumerate(stocks):
            rows = len(series.dates)
            positions = np.searchsorted(self.market_dates, series.dates)
            raw_close = series.close * series.factor
            turnover20 = pd.Series(raw_close * series.volume).rolling(TURNOVER_DAYS).mean().to_numpy()
            eligible = (raw_close >= MIN_CLOSE_KRW) & (turnover20 >= MIN_TURNOVER20_KRW)
            self.open_matrix[positions, column] = series.open_price
            close_matrix[positions, column] = series.close
            self.eligible_matrix[positions, column] = eligible
            self.stock_ended[column] = int(series.dates[-1]) < last_market_date
            previous_close = np.r_[np.nan, series.close[:-1]]

            with np.errstate(invalid="ignore", divide="ignore"):
                broken = ((np.abs(series.open_price / previous_close - 1.0) > PRICE_BREAK)
                          | (np.abs(series.close / previous_close - 1.0) > PRICE_BREAK))

            break_count = np.cumsum(broken)
            break_matrix[positions, column] = break_count
            pivots = swing.confirmed_pivots(series.low, series.high, TREND_SPAN)
            trend = swing.trend_state(pivots, rows)
            pieces["open"].append(series.open_price)
            pieces["high"].append(series.high)
            pieces["low"].append(series.low)
            pieces["close"].append(series.close)
            pieces["dates"].append(series.dates)
            pieces["market_index"].append(positions)
            pieces["break_count"].append(break_count)
            pieces["eligible"].append(eligible)
            pieces["turnover20"].append(turnover20)
            pieces["stock"].append(np.full(rows, column, dtype=np.int32))
            pieces["latest_low"].append(trend["latest_low"])
            pieces["trend"].append(trend["state"])
            pieces["stock_end"].append(np.full(rows, start + rows - 1, dtype=np.int64))

            for window in WINDOWS:
                average = swing.moving_average(series.close, window)
                touch = swing.moving_average_support_mask(series.close, series.low, average,
                                                          np.zeros(rows, dtype=np.int8), 0)   # 추세 조건 없음 = 전부 0 으로 넘김
                averages[window].append(touch)

            start += rows

        for name, values in pieces.items():
            setattr(self, name, np.concatenate(values))

        self.shape = {window: np.concatenate(values) for window, values in averages.items()}
        self.close_matrix = pd.DataFrame(close_matrix).ffill().to_numpy()
        self.break_matrix = pd.DataFrame(break_matrix).ffill().fillna(0.0).to_numpy()
        self.row_count = len(self.open)
        self.benchmark_rows: dict[int, int] = {}
        self.benchmark_list: list[np.ndarray] = []
        self.benchmark_table = np.empty((0, BENCHMARK_WIDTH))

    def signal_rows(self, window: int) -> np.ndarray:
        """모양·거름을 갖추고 다음 거래일이 있는 신호 행."""
        has_next = np.arange(self.row_count) < self.stock_end
        chosen = self.shape[window] & self.eligible & (self.dates >= SIGNAL_FROM) & has_next
        return np.flatnonzero(chosen)

    def prepare_benchmark(self, signal_at: np.ndarray, entry_at: np.ndarray) -> np.ndarray:
        """(신호일, 매수일) 시장 순번 쌍마다 청산일 0..WIDTH−1 의 비교 평균 순수익 줄을 만들고, 신호마다 그 줄 번호를 돌려준다."""
        date_count = len(self.market_dates)
        keys = signal_at.astype(np.int64) * 100_000 + entry_at.astype(np.int64)
        unique_keys, inverse = np.unique(keys, return_inverse=True)
        row_numbers = np.empty(len(unique_keys), dtype=np.int64)

        for position, key in enumerate(unique_keys.tolist()):
            if key in self.benchmark_rows:
                row_numbers[position] = self.benchmark_rows[key]
                continue

            signal_index, entry_index = divmod(key, 100_000)
            stop_index = min(date_count, entry_index + BENCHMARK_WIDTH)
            entry_open = self.open_matrix[entry_index]
            with np.errstate(invalid="ignore", divide="ignore"):
                ratio = self.close_matrix[entry_index:stop_index] / entry_open
            chosen = (self.eligible_matrix[signal_index] & (entry_open > 0))[None, :] & np.isfinite(ratio) \
                & (self.break_matrix[entry_index:stop_index] == self.break_matrix[signal_index][None, :])
            totals = np.where(chosen, ratio * COST - 1.0, 0.0).sum(axis=1)
            counts = chosen.sum(axis=1)
            means = np.where(counts > 0, totals / np.maximum(counts, 1) * 100.0, np.nan)
            row = np.full(BENCHMARK_WIDTH, np.nan)
            row[:len(means)] = means
            self.benchmark_rows[key] = len(self.benchmark_list)
            row_numbers[position] = len(self.benchmark_list)
            self.benchmark_list.append(row)

        self.benchmark_table = np.vstack(self.benchmark_list)
        return row_numbers[inverse]

    def benchmark(self, row_numbers: np.ndarray, signal_at: np.ndarray, entry_at: np.ndarray, exit_at: np.ndarray) -> np.ndarray:
        offset = exit_at - entry_at
        inside = offset < BENCHMARK_WIDTH
        result = np.full(len(offset), np.nan)
        result[inside] = self.benchmark_table[row_numbers[inside], offset[inside]]

        for position in np.flatnonzero(~inside).tolist():
            signal_index, entry_index, exit_index = int(signal_at[position]), int(entry_at[position]), int(exit_at[position])
            with np.errstate(invalid="ignore", divide="ignore"):
                ratio = self.close_matrix[exit_index] / self.open_matrix[entry_index]
            chosen = (self.eligible_matrix[signal_index] & np.isfinite(ratio) & (self.open_matrix[entry_index] > 0)
                      & (self.break_matrix[exit_index] == self.break_matrix[signal_index]))
            result[position] = float((ratio[chosen] * COST - 1.0).mean() * 100.0) if chosen.any() else np.nan

        return result


class SignalSet:
    """신호 행 묶음의 매수일부터 60거래일 가격 비율(매수가 = 1)과 첫 손절·익절 닿는 날."""

    def __init__(self, market: Market, signal_rows: np.ndarray) -> None:
        self.market = market
        self.signal_rows = signal_rows
        self.entry_rows = signal_rows + 1
        self.available = market.stock_end[signal_rows] - self.entry_rows    # 매수일 뒤 남은 행 수
        self.ended = market.stock_ended[market.stock[signal_rows]]
        offsets = np.arange(HOLD_LIMIT + 1)
        gather = self.entry_rows[:, None] + np.minimum(offsets[None, :], self.available[:, None])
        self.entry_price = market.open[self.entry_rows]
        self.open_ratio = market.open[gather] / self.entry_price[:, None]
        self.high_ratio = market.high[gather] / self.entry_price[:, None]
        self.low_ratio = market.low[gather] / self.entry_price[:, None]
        self.close_ratio = market.close[gather] / self.entry_price[:, None]
        self.valid_offset = offsets[None, :] <= self.available[:, None]
        pivot_stop = market.latest_low[signal_rows] * PIVOT_STOP_BELOW / self.entry_price
        self.stop_ratio = {name: np.full(len(signal_rows), 1.0 - value) for name, value in STOP_FIXED.items()}
        self.stop_ratio["SP"] = pivot_stop
        self.stop_usable = {name: np.ones(len(signal_rows), dtype=bool) for name in STOP_FIXED}
        self.stop_usable["SP"] = np.isfinite(pivot_stop) & (pivot_stop < 1.0)
        self.first_stop = {name: self._first_hit(self.low_ratio <= np.nan_to_num(ratio, nan=-1.0)[:, None])
                           for name, ratio in self.stop_ratio.items()}
        self.first_take = {take: self._first_hit(self.high_ratio >= 1.0 + take / 100.0) for take in TAKES}
        signal_market = market.market_index[signal_rows]
        self.signal_at = signal_market
        self.entry_at = market.market_index[self.entry_rows]
        self.dates = market.dates[signal_rows]
        self.entry_dates = market.dates[self.entry_rows]
        self.trend = market.trend[signal_rows]
        self.turnover20 = market.turnover20[signal_rows]
        self.stock = market.stock[signal_rows]

    def _first_hit(self, hit: np.ndarray) -> np.ndarray:
        hit = hit & self.valid_offset
        first = hit.argmax(axis=1)
        return np.where(hit.any(axis=1), first, 10 ** 6)

    def exits(self, take: int, stop: str, hold: int) -> dict:
        """칸 하나의 청산. resolved = H일이 찼거나 시세가 끊긴 매매. 안 찬 매매도 마지막 행까지로 임시 결과를 낸다."""
        resolved = (self.available >= hold) | self.ended
        last = np.minimum(hold, self.available)
        stop_at = self.first_stop[stop]
        take_at = self.first_take[take]
        is_stop = (stop_at <= take_at) & (stop_at <= last)
        is_take = ~is_stop & (take_at <= last)
        offset = np.where(is_stop, stop_at, np.where(is_take, take_at, last))
        row_index = np.arange(len(offset))
        safe_offset = np.minimum(offset, HOLD_LIMIT)
        open_at = self.open_ratio[row_index, safe_offset]
        exit_ratio = np.where(is_stop, np.minimum(open_at, self.stop_ratio[stop]),
                              np.where(is_take, np.maximum(open_at, 1.0 + take / 100.0),
                                       self.close_ratio[row_index, safe_offset]))
        exit_rows = self.entry_rows + offset
        broken = self.market.break_count[exit_rows] - self.market.break_count[self.signal_rows] > 0
        return {"usable": self.stop_usable[stop], "resolved": resolved, "offset": offset, "exit_rows": exit_rows,
                "exit_ratio": exit_ratio, "net": (exit_ratio * COST - 1.0) * 100.0, "broken": broken,
                "reason": np.where(is_stop, 1, np.where(is_take, 2, 0)).astype(np.int8)}

    def chain(self, result: dict) -> tuple[np.ndarray, int]:
        """같은 종목 보유 중 재신호 무시. 반환: 고른 신호 순번(가격 단절 매매 포함), 단절로 뺀 수는 호출자가 센다."""
        tradable = result["usable"] & result["resolved"]
        count = len(tradable)
        following = np.searchsorted(self.signal_rows, result["exit_rows"], side="left")
        # next_tradable[i] = i 이상 첫 tradable 순번
        marks = np.where(tradable, np.arange(count), count)
        next_tradable = np.minimum.accumulate(marks[::-1])[::-1]
        next_tradable = np.r_[next_tradable, count].tolist()
        following = following.tolist()
        chosen = []
        position = next_tradable[0]

        while position < count:
            chosen.append(position)
            position = next_tradable[following[position]]

        return np.asarray(chosen, dtype=np.int64), int((~result["usable"]).sum())


def summarize(net: np.ndarray, excess: np.ndarray, entry_dates: np.ndarray, hold_days: np.ndarray, reason: np.ndarray) -> dict:
    months = month_codes(entry_dates // 100)
    years = entry_dates // 10000
    early = entry_dates < SPLIT_DATE
    summary = {"n": int(len(net)), "net": float(net.mean()), "excess": float(np.nanmean(excess)),
               "t_net": fast_cluster_t(net, months), "t_excess": fast_cluster_t(excess, months),
               "net_2010_17": float(net[early].mean()) if early.any() else float("nan"),
               "net_2018_26": float(net[~early].mean()) if (~early).any() else float("nan"),
               "excess_2010_17": float(excess[early].mean()) if early.any() else float("nan"),
               "excess_2018_26": float(excess[~early].mean()) if (~early).any() else float("nan"),
               "win_rate": float((net > 0).mean() * 100.0), "hold_days": float(hold_days.mean()),
               "stop_share": float((reason == 1).mean() * 100.0), "take_share": float((reason == 2).mean() * 100.0)}
    yearly_net = pd.Series(net).groupby(years).mean()
    yearly_excess = pd.Series(excess).groupby(years).mean()
    summary["yearly_net"] = {int(year): round(float(value), 3) for year, value in yearly_net.items()}
    summary["yearly_excess"] = {int(year): round(float(value), 3) for year, value in yearly_excess.items()}
    return summary


def cell_name(window: int, take: int, stop: str, hold: int) -> str:
    return f"N{window}_T{take}_{stop}_H{hold}"


def parse_cell(name: str) -> tuple[int, int, str, int]:
    window, take, stop, hold = name.split("_")
    return int(window[1:]), int(take[1:]), stop, int(hold[1:])


def distribution_e1(signals: SignalSet) -> list[dict]:
    """청산 없이 60거래일. 모든 신호를 따로 센다(60일 다 찬 것, 가격 단절 없는 것)."""
    full = signals.available >= HOLD_LIMIT
    end_rows = signals.entry_rows + HOLD_LIMIT
    end_rows = np.minimum(end_rows, signals.market.row_count - 1)
    broken = signals.market.break_count[end_rows] - signals.market.break_count[signals.signal_rows] > 0
    keep = full & ~broken
    low_running = np.minimum.accumulate(signals.low_ratio[keep], axis=1)
    high_running = np.maximum.accumulate(signals.high_ratio[keep], axis=1)
    final_net = (signals.close_ratio[keep, HOLD_LIMIT] * COST - 1.0) * 100.0
    peak_day = signals.high_ratio[keep].argmax(axis=1)
    before_peak = low_running[np.arange(len(peak_day)), peak_day]
    trend = signals.trend[keep]
    rows = []
    groups = [("전체", np.ones(len(trend), dtype=bool))] + [(TREND_NAMES[value], trend == value) for value in
                                                           (swing.TREND_UP, swing.TREND_SIDEWAYS, swing.TREND_DOWN,
                                                            swing.TREND_UNKNOWN)]

    for label, mask in groups:
        if not mask.any():
            continue

        row = {"trend": label, "n": int(mask.sum()), "hold60_net": float(final_net[mask].mean()),
               "hold60_win": float((final_net[mask] > 0).mean() * 100.0)}

        for horizon in HORIZONS:
            adverse = (low_running[mask, horizon] - 1.0) * 100.0
            favorable = (high_running[mask, horizon] - 1.0) * 100.0
            row[f"mae{horizon}"] = [float(np.median(adverse)), float(np.quantile(adverse, 0.1)), float(np.quantile(adverse, 0.9))]
            row[f"mfe{horizon}"] = [float(np.median(favorable)), float(np.quantile(favorable, 0.1)),
                                    float(np.quantile(favorable, 0.9))]

        winners = mask & (final_net > 0)
        winner_adverse = (before_peak[winners] - 1.0) * 100.0
        row["winner_mae_before_peak"] = [float(np.median(winner_adverse)), float(np.quantile(winner_adverse, 0.1))]
        rows.append(row)

    return rows


def self_check(market: Market, signals: SignalSet) -> int:
    """칸 N10_T15_SP_H60 의 청산을 standalone_swing.simulate_exit 와 신호 2,000개에서 맞춰 본다. 반환 = 어긋난 수."""
    generator = np.random.default_rng(37)
    result = signals.exits(15, "SP", 60)
    candidates = np.flatnonzero(result["usable"] & result["resolved"])
    picks = np.sort(generator.choice(candidates, size=min(2000, len(candidates)), replace=False))
    stock_values, first_rows = np.unique(market.stock, return_index=True)
    starts = dict(zip(stock_values.tolist(), first_rows.tolist()))

    mismatches = 0

    class LocalSeries:
        pass

    for pick in picks.tolist():
        stock = int(signals.stock[pick])
        start = starts[stock]
        end = int(market.stock_end[start])
        local = LocalSeries()
        local.open_price = market.open[start:end + 1]
        local.low = market.low[start:end + 1]
        local.high = market.high[start:end + 1]
        local.close = market.close[start:end + 1]
        entry_local = int(signals.entry_rows[pick]) - start
        reference = base.simulate_exit(local, end - start + 1, entry_local, float(market.latest_low[signals.signal_rows[pick]])
                                       * PIVOT_STOP_BELOW, bool(signals.ended[pick]))

        if reference is None or abs(reference["net"] - result["net"][pick]) > 1e-9 \
                or reference["hold_days"] != int(result["offset"][pick]):
            mismatches += 1

    return mismatches


def run_portfolio(market: Market, choices: dict, candidate_sets: dict, start_date: int,
                  seed: int | None = None) -> tuple[pd.DataFrame, dict]:
    """choices[year] = 칸 이름, candidate_sets[칸] = (SignalSet, exits). 매일 신규 ≤ DAILY_NEW, 보유 ≤ MAX_HELD.
    순서는 신호일 거래대금 큰 순, seed 를 주면 무작위 순서(보조 비교)."""
    by_entry: dict[int, list] = {}
    generator = np.random.default_rng(seed) if seed is not None else None

    for year, name in choices.items():
        signals, result = candidate_sets[name]
        mask = (signals.entry_dates // 10000 == year) & result["usable"] & ~result["broken"]
        order = -signals.turnover20 if generator is None else generator.random(len(signals.turnover20))

        for position in np.flatnonzero(mask).tolist():
            by_entry.setdefault(int(signals.entry_at[position]), []).append(
                (float(order[position]), int(signals.stock[position]), float(signals.entry_price[position]),
                 int(market.market_index[result["exit_rows"][position]]), float(result["exit_ratio"][position]), year))

    first_index = int(np.searchsorted(market.market_dates, start_date))
    held: list[tuple] = []     # (종목, 매수가, 청산 순번, 청산 비율, 매수 해, 매수 순번)
    realized = 0.0
    cash = CAPITAL_KRW
    minimum_cash = cash
    no_cash_skips = 0
    records = []
    trade_log = []

    def log_trade(item: tuple, ratio: float, closed: bool) -> None:
        trade_log.append({"code": market.codes[item[0]], "entry_date": int(market.market_dates[item[5]]),
                          "exit_date": int(market.market_dates[min(item[2], len(market.market_dates) - 1)]),
                          "entry_year": item[4], "net": (ratio * (COST if closed else 1.0) - 1.0) * 100.0, "closed": closed})

    for day_index in range(first_index, len(market.market_dates)):
        staying = []

        for item in held:
            if item[2] < day_index:
                gain = TRADE_KRW * (item[3] * COST - 1.0)
                realized += gain
                cash += TRADE_KRW + gain
                log_trade(item, item[3], True)
            else:
                staying.append(item)

        held = staying
        slots = min(DAILY_NEW, MAX_HELD - len(held))
        holding_stocks = {item[0] for item in held}
        bought = 0

        for candidate in sorted(by_entry.get(day_index, [])):
            if bought >= slots:
                break

            if candidate[1] in holding_stocks:
                continue

            if cash < TRADE_KRW:
                no_cash_skips += 1
                break

            held.append((candidate[1], candidate[2], candidate[3], candidate[4], candidate[5], day_index))
            holding_stocks.add(candidate[1])
            cash -= TRADE_KRW
            bought += 1

        minimum_cash = min(minimum_cash, cash)
        open_value = 0.0

        for stock, entry_price, exit_index, exit_ratio, _, _ in held:
            if exit_index <= day_index:
                open_value += TRADE_KRW * (exit_ratio * COST - 1.0)
            else:
                open_value += TRADE_KRW * (market.close_matrix[day_index, stock] / entry_price - 1.0)

        records.append({"date": int(market.market_dates[day_index]), "equity": CAPITAL_KRW + realized + open_value,
                        "cash": cash, "held": len(held), "bought": bought})

    last_index = len(market.market_dates) - 1

    for item in held:      # 끝에 열린 매매 = 마지막 종가 평가(비용 전)
        ratio = item[3] if item[2] <= last_index else market.close_matrix[last_index, item[0]] / item[1]
        log_trade(item, ratio, item[2] <= last_index)

    curve = pd.DataFrame(records)
    equity = curve["equity"].to_numpy()
    drawdown = equity / np.maximum.accumulate(equity) - 1.0
    curve["year"] = curve["date"] // 10000
    curve["drawdown"] = drawdown * 100.0
    trades = pd.DataFrame(trade_log)
    yearly = {}
    previous = CAPITAL_KRW

    for year, group in curve.groupby("year"):
        end_value = float(group["equity"].iloc[-1])
        part = trades[trades["entry_year"] == year]
        yearly[int(year)] = {"return": (end_value / previous - 1.0) * 100.0, "profit_krw": end_value - previous,
                             "trades": int(len(part)), "trade_net": float(part["net"].mean()) if len(part) else float("nan"),
                             "drawdown": float(group["drawdown"].min())}
        previous = end_value

    summary = {"total_return": (equity[-1] / CAPITAL_KRW - 1.0) * 100.0, "max_drawdown": float(drawdown.min() * 100.0),
               "trades": int(len(trades)), "trade_net": float(trades["net"].mean()), "minimum_cash": minimum_cash,
               "no_cash_skips": no_cash_skips, "yearly": yearly, "win_rate": float((trades["net"] > 0).mean() * 100.0)}
    curve.attrs["trades"] = trades
    return curve, summary


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    clock = time.time()
    base.write_text(OUTPUT_TEXT, PLAN + "\n[결과] 실행 중\n")
    market = Market()
    print(f"자료 읽음 {time.time() - clock:.0f}s, 행 {market.row_count}", flush=True)

    signal_sets = {}
    for window in WINDOWS:
        signal_sets[window] = SignalSet(market, market.signal_rows(window))
        print(f"N{window} 신호 {len(signal_sets[window].signal_rows)}", flush=True)
        signal_sets[window].benchmark_row = market.prepare_benchmark(signal_sets[window].signal_at, signal_sets[window].entry_at)

    mismatches = self_check(market, signal_sets[10])
    print(f"자체 점검 어긋남 {mismatches}", flush=True)

    # ── E1 ──
    e1 = {window: distribution_e1(signal_sets[window]) for window in WINDOWS}

    # ── E2 ──
    cells = {}
    cell_trades = {}
    skipped = {}

    for window in WINDOWS:
        signals = signal_sets[window]

        for take in TAKES:
            for stop in STOPS:
                for hold in HOLDS:
                    name = cell_name(window, take, stop, hold)
                    result = signals.exits(take, stop, hold)
                    chosen, unusable = signals.chain(result)
                    broken = result["broken"][chosen]
                    picked = chosen[~broken]
                    exit_at = market.market_index[result["exit_rows"][picked]]
                    benchmark = market.benchmark(signals.benchmark_row[picked], signals.signal_at[picked], signals.entry_at[picked],
                                                 exit_at)
                    net = result["net"][picked]
                    keep = np.isfinite(benchmark)
                    picked, net, benchmark, exit_at = picked[keep], net[keep], benchmark[keep], exit_at[keep]
                    excess = net - benchmark
                    entry_dates = signals.entry_dates[picked]
                    cells[name] = summarize(net, excess, entry_dates, result["offset"][picked], result["reason"][picked])
                    cells[name].update({"window": window, "take": take, "stop": stop, "hold": hold})
                    cell_trades[name] = {"position": picked, "net": net, "excess": excess, "entry_dates": entry_dates,
                                         "exit_dates": market.market_dates[exit_at], "trend": signals.trend[picked],
                                         "hold_days": result["offset"][picked]}
                    skipped[name] = {"price_break": int(broken.sum()), "no_pivot_or_open_below_stop": unusable,
                                     "unresolved": int((result["usable"] & ~result["resolved"]).sum())}

        print(f"N{window} 격자 끝 {time.time() - clock:.0f}s", flush=True)

    # ── E3(a) ──
    names = sorted(cells)
    all_years = sorted({int(year) for trades in cell_trades.values() for year in np.unique(trades["entry_dates"] // 10000)})
    first_year = all_years[0]
    walk_choices = {}
    walk_parts = []
    choice_rows = []

    for year in range(first_year + WALK_WARMUP_YEARS, all_years[-1] + 1):
        boundary = year * 10000 + 101
        scores = {}

        for name in names:
            trades = cell_trades[name]
            known = trades["exit_dates"] < boundary

            if known.sum() >= 3:
                scores[name] = fast_cluster_t(trades["net"][known], month_codes(trades["entry_dates"][known] // 100))

        scores = {name: value for name, value in scores.items() if np.isfinite(value)}
        chosen_name = max(sorted(scores), key=lambda name: scores[name])
        trades = cell_trades[chosen_name]
        mask = trades["entry_dates"] // 10000 == year
        walk_choices[year] = chosen_name
        part = pd.DataFrame({"cell": chosen_name, "entry_date": trades["entry_dates"][mask], "exit_date": trades["exit_dates"][mask],
                             "net": trades["net"][mask], "excess": trades["excess"][mask], "trend": trades["trend"][mask],
                             "hold_days": trades["hold_days"][mask]})
        part["code"] = [market.codes[stock] for stock in signal_sets[parse_cell(chosen_name)[0]].stock[trades["position"][mask]]]
        walk_parts.append(part)
        choice_rows.append({"year": year, "cell": chosen_name, "past_t": scores[chosen_name], "n": int(mask.sum()),
                            "net": float(part["net"].mean()) if len(part) else float("nan"),
                            "excess": float(part["excess"].mean()) if len(part) else float("nan")})

    walk = pd.concat(walk_parts, ignore_index=True)
    walk_months = month_codes(walk["entry_date"].to_numpy() // 100)
    walk_early = walk["entry_date"].to_numpy() < SPLIT_DATE
    walk_summary = {"n": int(len(walk)), "net": float(walk["net"].mean()), "excess": float(walk["excess"].mean()),
                    "t_net": fast_cluster_t(walk["net"].to_numpy(), walk_months),
                    "t_excess": fast_cluster_t(walk["excess"].to_numpy(), walk_months),
                    "net_2013_17": float(walk.loc[walk_early, "net"].mean()),
                    "net_2018_26": float(walk.loc[~walk_early, "net"].mean()),
                    "win_rate": float((walk["net"] > 0).mean() * 100.0), "hold_days": float(walk["hold_days"].mean()),
                    "choices": choice_rows}
    walk.to_parquet(OUTPUT_WALK, index=False)

    # ── E3(b) ──
    final_cell = walk_choices[max(walk_choices)]
    window, take, stop, hold = parse_cell(final_cell)
    neighbors = []

    for options, current, build in ((TAKES, take, lambda value: cell_name(window, value, stop, hold)),
                                    (STOPS, stop, lambda value: cell_name(window, take, value, hold)),
                                    (HOLDS, hold, lambda value: cell_name(window, take, stop, value))):
        index = options.index(current)

        for step in (-1, 1):
            if 0 <= index + step < len(options):
                neighbors.append(build(options[index + step]))

    neighbor_rows = [{"cell": name, **{key: cells[name][key] for key in ("n", "net", "excess", "t_net", "net_2010_17",
                                                                         "net_2018_26")}} for name in neighbors]

    # ── E4 ──
    used_cells = sorted(set(walk_choices.values()))
    candidate_sets = {}

    for name in used_cells:
        signals = signal_sets[parse_cell(name)[0]]
        candidate_sets[name] = (signals, signals.exits(*parse_cell(name)[1:]))

    walk_start = (first_year + WALK_WARMUP_YEARS) * 10000 + 101
    curve, portfolio = run_portfolio(market, walk_choices, candidate_sets, walk_start)

    # 모양 없음 비교: 신호일 거름 통과 종목 중 거래대금 상위 BASELINE_POOL
    has_next = np.arange(market.row_count) < market.stock_end
    pool_mask = market.eligible & has_next & (market.dates >= walk_start // 10000 * 10000 - 10000)
    pool_rows = np.flatnonzero(pool_mask)
    pool_frame = pd.DataFrame({"row": pool_rows, "day": market.market_index[pool_rows], "turnover": market.turnover20[pool_rows]})
    pool_frame = pool_frame.sort_values(["day", "turnover"], ascending=[True, False], kind="mergesort")
    pool_frame = pool_frame.groupby("day", sort=False).head(BASELINE_POOL)
    baseline_signals = SignalSet(market, np.sort(pool_frame["row"].to_numpy()))
    baseline_sets = {name: (baseline_signals, baseline_signals.exits(*parse_cell(name)[1:])) for name in used_cells}
    baseline_curve, baseline = run_portfolio(market, walk_choices, baseline_sets, walk_start)
    random_runs = [run_portfolio(market, walk_choices, candidate_sets, walk_start, seed)[1] for seed in RANDOM_SEEDS]
    random_summary = {"total_return_mean": float(np.mean([run["total_return"] for run in random_runs])),
                      "total_return_min": float(np.min([run["total_return"] for run in random_runs])),
                      "total_return_max": float(np.max([run["total_return"] for run in random_runs])),
                      "trade_net_mean": float(np.mean([run["trade_net"] for run in random_runs])),
                      "max_drawdown_mean": float(np.mean([run["max_drawdown"] for run in random_runs]))}
    portfolio_trades = pd.concat([curve.attrs["trades"].assign(side="shape"), baseline_curve.attrs["trades"].assign(side="baseline")],
                                 ignore_index=True)
    portfolio_trades.to_parquet(OUTPUT_PORTFOLIO_TRADES, index=False)
    curve.attrs = {}
    baseline_curve.attrs = {}
    curve = curve.merge(baseline_curve[["date", "equity", "held"]].rename(columns={"equity": "baseline_equity",
                                                                                  "held": "baseline_held"}), on="date")
    curve.to_parquet(OUTPUT_PORTFOLIO, index=False)

    # ── E5 ──
    def trend_split(net: np.ndarray, trend: np.ndarray, entry_dates: np.ndarray) -> dict:
        months = month_codes(entry_dates // 100)
        rows = {}

        for value in (swing.TREND_UP, swing.TREND_SIDEWAYS, swing.TREND_DOWN, swing.TREND_UNKNOWN):
            mask = trend == value

            if mask.sum() >= 3:
                rows[TREND_NAMES[value]] = {"n": int(mask.sum()), "net": float(net[mask].mean()),
                                            "t_net": fast_cluster_t(net[mask], months[mask])}

        difference, t_value = base.cluster_difference_t(net, trend == swing.TREND_DOWN, entry_dates // 100)
        rows["하향−나머지"] = {"difference": difference, "t": t_value}
        return rows

    final_trades = cell_trades[final_cell]
    e5 = {"final_cell": trend_split(final_trades["net"], final_trades["trend"], final_trades["entry_dates"]),
          "walk": trend_split(walk["net"].to_numpy(), walk["trend"].to_numpy(), walk["entry_date"].to_numpy())}

    # ── 판정 ──
    pass_walk = (walk_summary["net"] > 0 and walk_summary["t_net"] >= PASS_WALK_T
                 and walk_summary["net_2013_17"] > 0 and walk_summary["net_2018_26"] > 0)
    pass_neighbors = all(row["net"] > 0 for row in neighbor_rows)
    pass_portfolio = portfolio["total_return"] > 0 and portfolio["total_return"] > baseline["total_return"]

    # ── 글 ──
    lines = [PLAN, "[실행 뒤 바꾼 것] E4 — 첫 실행에서 현금이 바닥나도 200만씩 계속 사서 자산이 음수(최대 낙폭 −164%)로 갔다."
             " 현금이 200만 미만이면 그날 더 사지 않게 고치고(현실 제약, 내가 정함), 매매 기록 파일과 매매당 평균·손익(만원) 열,"
             " 보조 비교(무작위 순서 10회, 판정 안 씀)를 더했다. 시험 칸·통과 기준·E1–E3·E5 는 그대로.", "", "[결과]",
             f"  자체 점검: N10_T15_SP_H60 청산을 standalone_swing.simulate_exit 와 신호 2,000개 대조 → 어긋남 {mismatches}건",
             f"  신호 수(거름 통과, 겹침 포함): " + ", ".join(f"N{window} {len(signal_sets[window].signal_rows):,}" for window in WINDOWS),
             "", "  E1 청산 없이 60거래일 보유 — MAE/MFE %(중앙 / 하위 10% / 상위 10%), 60일째 종가 순수익 평균·승률, 이긴 매매 고점 전 MAE(중앙 / 하위 10%)"]

    for window in WINDOWS:
        for row in e1[window]:
            lines.append(f"    N{window:<2d} {row['trend']:4s} n {row['n']:>8,d}  60일 보유 순수익 {row['hold60_net']:+6.2f} 승률 {row['hold60_win']:5.1f}"
                         f"  이긴 매매 고점 전 MAE {row['winner_mae_before_peak'][0]:+6.2f} / {row['winner_mae_before_peak'][1]:+6.2f}")
            lines.append("         " + "  ".join(
                f"{horizon}일 MAE {row[f'mae{horizon}'][0]:+5.1f}/{row[f'mae{horizon}'][1]:+5.1f}/{row[f'mae{horizon}'][2]:+5.1f}"
                f" MFE {row[f'mfe{horizon}'][0]:+5.1f}/{row[f'mfe{horizon}'][1]:+5.1f}/{row[f'mfe{horizon}'][2]:+5.1f}"
                for horizon in HORIZONS))

    header = (f"    {'칸':18s} {'n':>7s} {'순수익':>7s} {'초과':>7s} {'순t':>6s} {'초과t':>6s} {'10-17':>7s} {'18-26':>7s}"
              f" {'승률':>6s} {'보유일':>6s} {'손절%':>6s} {'익절%':>6s}")

    def cell_line(name: str) -> str:
        summary = cells[name]
        return (f"    {name:18s} {summary['n']:7d} {summary['net']:+7.2f} {summary['excess']:+7.2f} {summary['t_net']:+6.2f}"
                f" {summary['t_excess']:+6.2f} {summary['net_2010_17']:+7.2f} {summary['net_2018_26']:+7.2f}"
                f" {summary['win_rate']:6.1f} {summary['hold_days']:6.1f} {summary['stop_share']:6.1f} {summary['take_share']:6.1f}")

    ranked = sorted(names, key=lambda name: (-cells[name]["t_net"], name))
    lines += ["", "  E2 청산 격자 180칸 — 순수익 묶음 t 상위 10칸(순수익·초과·두 기간은 매매당 %, 비용 포함)", header]
    lines += [cell_line(name) for name in ranked[:10]]
    lines += ["", "  N별 최고 칸(순수익 묶음 t 기준)과 N별 순수익 > 0 칸 수", header]

    for window in WINDOWS:
        group = [name for name in ranked if cells[name]["window"] == window]
        positive = sum(1 for name in group if cells[name]["net"] > 0)
        lines.append(cell_line(group[0]) + f"   (순수익>0 {positive}/60칸)")

    best = ranked[0]
    lines.append(f"    헌장 기준 참고: 최고 칸 {best} 순수익 t {cells[best]['t_net']:+.2f} — 180칸 비교의 한 칸 기준 3.5 "
                 + ("넘음" if cells[best]["t_net"] >= 3.5 else "못 넘음"))
    full_list = ["", "  E2 전체 180칸(순수익 t 순)", header] + [cell_line(name) for name in ranked]

    lines += ["", f"  E3(a) 해마다 그 전 자료로 고르기(2013–): n {walk_summary['n']:,} 순수익 {walk_summary['net']:+.2f}%"
              f" 초과 {walk_summary['excess']:+.2f}%p 순t {walk_summary['t_net']:+.2f} 초과t {walk_summary['t_excess']:+.2f}"
              f" 2013–17 {walk_summary['net_2013_17']:+.2f} 2018–26 {walk_summary['net_2018_26']:+.2f}"
              f" 승률 {walk_summary['win_rate']:.1f} 보유일 {walk_summary['hold_days']:.1f}"]

    for row in choice_rows:
        lines.append(f"    {row['year']} {row['cell']:18s} (지난 순t {row['past_t']:+6.2f}) n {row['n']:6d}"
                     f" 순수익 {row['net']:+6.2f} 초과 {row['excess']:+6.2f}")

    lines += ["", f"  E3(b) 고른 칸 {final_cell}(2026년에 쓰인 칸)과 이웃 — 전 기간",
              f"    {'칸':18s} {'n':>7s} {'순수익':>7s} {'초과':>7s} {'순t':>6s} {'10-17':>7s} {'18-26':>7s}"]

    for row in [{"cell": final_cell, **{key: cells[final_cell][key] for key in ("n", "net", "excess", "t_net", "net_2010_17",
                                                                               "net_2018_26")}}] + neighbor_rows:
        lines.append(f"    {row['cell']:18s} {row['n']:7d} {row['net']:+7.2f} {row['excess']:+7.2f} {row['t_net']:+6.2f}"
                     f" {row['net_2010_17']:+7.2f} {row['net_2018_26']:+7.2f}")

    lines += ["", f"  E4 자산 곡선(자본 2,000만, 건당 200만, 하루 신규 ≤{DAILY_NEW}, 보유 ≤{MAX_HELD}, 2013-01–끝)",
              f"    모양 있음: 총수익 {portfolio['total_return']:+.1f}% 최대 낙폭 {portfolio['max_drawdown']:.1f}%"
              f" 건수 {portfolio['trades']:,} 매매당 {portfolio['trade_net']:+.2f}% 승률 {portfolio['win_rate']:.1f}"
              f" 현금 부족으로 못 산 날 {portfolio['no_cash_skips']}",
              f"    모양 없음: 총수익 {baseline['total_return']:+.1f}% 최대 낙폭 {baseline['max_drawdown']:.1f}%"
              f" 건수 {baseline['trades']:,} 매매당 {baseline['trade_net']:+.2f}% 승률 {baseline['win_rate']:.1f}"
              f" 현금 부족으로 못 산 날 {baseline['no_cash_skips']}",
              f"    보조(판정 안 씀) 모양 있음·무작위 순서 {len(RANDOM_SEEDS)}회: 총수익 평균 {random_summary['total_return_mean']:+.1f}%"
              f" (최저 {random_summary['total_return_min']:+.1f}, 최고 {random_summary['total_return_max']:+.1f})"
              f" 매매당 {random_summary['trade_net_mean']:+.2f}% 최대 낙폭 평균 {random_summary['max_drawdown_mean']:.1f}%",
              f"    {'해':6s} {'모양 수익%':>10s} {'손익만':>7s} {'낙폭%':>7s} {'건수':>5s} {'매매당%':>7s} |"
              f" {'없음 수익%':>10s} {'손익만':>7s} {'낙폭%':>7s} {'건수':>5s} {'매매당%':>7s}"]

    for year in sorted(portfolio["yearly"]):
        mine = portfolio["yearly"][year]
        other = baseline["yearly"].get(year, {"return": float("nan"), "profit_krw": float("nan"), "drawdown": float("nan"),
                                              "trades": 0, "trade_net": float("nan")})
        lines.append(f"    {year:<6d} {mine['return']:+10.1f} {mine['profit_krw'] / 1e4:+7.0f} {mine['drawdown']:7.1f}"
                     f" {mine['trades']:5d} {mine['trade_net']:+7.2f} | {other['return']:+10.1f} {other['profit_krw'] / 1e4:+7.0f}"
                     f" {other['drawdown']:7.1f} {other['trades']:5d} {other['trade_net']:+7.2f}")

    lines += ["", "  E5 추세(k=5)별 분해 — 순수익 %·묶음 t"]

    for label, rows in (("고른 칸 " + final_cell, e5["final_cell"]), ("E3(a) 이어붙인 매매", e5["walk"])):
        lines.append(f"    {label}")

        for key, value in rows.items():
            if key == "하향−나머지":
                lines.append(f"      하향 − 나머지 차이 {value['difference']:+.2f}%p 묶음 t {value['t']:+.2f}")
            else:
                lines.append(f"      {key:4s} n {value['n']:7,d} 순수익 {value['net']:+6.2f} 순t {value['t_net']:+6.2f}")

    year_columns = list(range(first_year, all_years[-1] + 1))
    lines += ["", "  연도별 순수익(안정성 확인용) — E3(b) 고른 칸·이웃, E3(a)",
              "    " + f"{'칸':18s}" + "".join(f"{year:>7d}" for year in year_columns)]

    for name in [final_cell] + neighbors:
        yearly = cells[name]["yearly_net"]
        lines.append("    " + f"{name:18s}" + "".join(f"{yearly[year]:+7.2f}" if year in yearly else f"{'':>7s}" for year in year_columns))

    walk_yearly = walk.groupby(walk["entry_date"] // 10000)["net"].mean()
    lines.append("    " + f"{'E3(a)':18s}" + "".join(f"{walk_yearly[year]:+7.2f}" if year in walk_yearly.index else f"{'':>7s}"
                                                    for year in year_columns))

    verdict_parts = [("① 걸어가며 고르기", pass_walk), ("② 이웃 칸", pass_neighbors), ("③ 자산 곡선", pass_portfolio)]
    verdict = "모의 시험 후보" if all(flag for _, flag in verdict_parts) else \
        "후보 아님 — 빠진 것: " + ", ".join(label for label, flag in verdict_parts if not flag)
    lines += ["", "  판정: " + " / ".join(f"{label} {'통과' if flag else '미달'}" for label, flag in verdict_parts) + f" → {verdict}"]
    bars_path = study33.BARS_PATH
    reproduction = {"commit": base.git_commit(), "bars_path": str(bars_path.relative_to(REPOSITORY)),
                    "bars_size": bars_path.stat().st_size, "bars_rows_loaded": market.information["bars_rows"],
                    "common_codes": market.information["common_codes"], "last_market_date": int(market.market_dates[-1]),
                    "cost_ratio": COST, "random": "자체 점검 표본만 seed 37, 결과 숫자는 결정론", "seconds": round(time.time() - clock)}
    lines.append("  재현: " + json.dumps(reproduction, ensure_ascii=False))
    lines.append("  재실행(저장소 루트): py -X utf8 research/studies/37_swing_trend/ma_support_exits.py")
    text = "\n".join(lines + full_list) + "\n"
    base.write_text(OUTPUT_TEXT, text)
    pd.DataFrame([{"cell": name, **{key: value for key, value in cells[name].items() if not key.startswith("yearly")}}
                  for name in names]).to_parquet(OUTPUT_CELLS, index=False)
    payload = {"e1": e1, "cells": cells, "skipped": skipped, "walk": walk_summary, "final_cell": final_cell,
               "neighbors": neighbor_rows, "portfolio": portfolio, "baseline": baseline, "random_order": random_summary, "e5": e5,
               "verdict": {"walk": pass_walk, "neighbors": pass_neighbors, "portfolio": pass_portfolio, "text": verdict},
               "self_check_mismatches": mismatches, "reproduction": reproduction}
    base.write_text(OUTPUT_METRICS, json.dumps(payload, ensure_ascii=False, indent=1, default=float) + "\n")
    print("\n".join(lines[len(PLAN.splitlines()):]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
