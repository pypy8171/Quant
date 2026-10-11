"""스터디 43 하네스 — DevScale 실제 매수·매도 규칙을 슬롯 계좌로 일봉 재현한다(2026-10-11).

무엇을 하나:
  - 매수 판정은 스터디 38 daily_stop_grid.py 를 import 해 그대로 쓴다(entry_allowed·entry_price·atr_series).
  - 매도는 두 규칙: D158(엔진 judge_trend_exit 그대로 — 매수 뒤 종가가 SMA60 위에 한 번 선 다음 종가 < SMA60 이면
    다음 거래일 시가), OLD(daily_stop_grid.CURRENT_RULE — 평단 −6.5% 장중 손절·+3% 익절·존 이탈, exit_for_entry 그대로).
  - 계좌: 슬롯 S 개, 슬롯당 금액 고정 500만 원 또는 전날 종가 평가금 ÷ S. 정수 주식, 남는 현금 이자 0.
  - 대조: 같은 날 같은 건수를 무작위 종목으로 시가에 사서 같은 매도 규칙·같은 비용으로 판다.
  - 43-D: D158 에 장중 익절·손절(평단 %)을 덧붙인다(take_profit·stop_loss, 0 = 없음). 엔진 실제 설정 계좌(MODE_ENGINE)는
    engine_scan() 이 낸 스캔 점수·금액(270만–400만)으로 사고, 자본·총노출 상한(capital·gross_cap)을 따른다.

가격 기준(내가 정함):
  - 날짜·종목 격자는 스터디 38 일봉 패널(2009-01 – 2026-10-08, 정지일 행 포함)을 쓰고 종목은 스터디 41 입력의 3,190개로 한정한다.
  - 41 입력은 정지 점프를 고친 가격(clean_close)이다. 두 가격의 비(41 종가 ÷ 38 종가)는 종목마다 구간별 상수라
    38 시가·고가·저가·종가에 그 비를 곱해 41 기준으로 맞춘다(비가 바뀌는 날 82종목 85일 — 38의 가짜 점프를 지운다).
    2009년 구간은 첫 비를 쓴다. 매수 재현 대조(reproduce.py)는 비를 곱하지 않은 38 원본으로 따로 한다.
  - 미끄러짐 호가는 무수정가(41 raw_close ÷ close 비)와 그날 시장(41 PIT 시장)으로 매긴다. 매수 지정가 자체의 호가 맞춤은
    38 재현(수정주가·현재 시장)을 그대로 둔다.
거래 정지·상장폐지(스터디 41 basket.py 방식):
  - 정지일(38 halted)은 판정도 매매도 하지 않는다. 시가 매도일에 시가가 하한가 잠김(41 lock 비트 2)이면 다음 거래일 시가로 미룬다.
  - 마지막 거래 행(정지 꼬리 포함) 다음 날 아침에 마지막 종가 × 41 폐지 배수 × (1 − 수수료 − 매도세)로 현금화한다.
  - 데이터 끝까지 상장 중이면 시험 끝 보유로 마지막 종가 평가(매매 통계에는 매도 비용을 뗀 값).
하루 순서: ① 폐지 현금화·시가 매도(그 슬롯은 그날 매수에 쓸 수 있다) ② 시가 매수(신호 단독·무작위 대조)
  ③ 장중 지정가 매수(DevScale) ④ 장중 매도(OLD 손절·익절·존 이탈 — 그 슬롯은 다음 날부터) ⑤ 종가 평가.
  판 종목은 그날 다시 사지 않는다(다음 날부터).
"""
from __future__ import annotations

import sys
from pathlib import Path

STUDY = Path(__file__).resolve().parent
STUDIES = STUDY.parent
REPOSITORY = STUDIES.parent.parent
sys.path.insert(0, str(STUDIES / "41_universe_basket"))
sys.path.insert(0, str(STUDIES / "38_trend_stop_intraday"))

import study41     # noqa: E402  numba 스레드 수를 먼저 정한다(스터디 38 common)
import basket     # noqa: E402
import daily_stop_grid as grid     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

RESULTS = STUDY / "results"
SLOT_AMOUNT = 5_000_000.0
COMMISSION = grid.COMMISSION
MIN_PRICE = grid.MIN_PRICE
OLD_RULE = np.asarray(grid.CURRENT_RULE, dtype=np.float64)
RULE_TREND, RULE_OLD = 0, 1
RULE_TEXT = {RULE_TREND: "D158(SMA60 종가 이탈 → 다음 날 시가)", RULE_OLD: "OLD(−6.5% 손절·+3% 익절·존 이탈)"}
MODE_TURNOVER, MODE_RANDOM_ORDER, MODE_SIGNAL_ORDER, MODE_SIGNAL_FILTER, MODE_SIGNAL_ONLY = 0, 1, 2, 3, 4
MODE_ENGINE = 5
AMOUNT_FIXED, AMOUNT_COMPOUND, AMOUNT_ENGINE = 0, 1, 2
# 엔진 실제 설정(Quant/config/config_dev_paper.json DevScale 블록·risk 블록, 2026-10-11 확인)
ENGINE_FLOOR_KRW = 2_700_000.0     # notional_floor_krw
ENGINE_CAP_KRW = 4_000_000.0       # notional_cap_krw
ENGINE_CAP_Z = 1.5                 # notional_cap_z
ENGINE_CAPITAL = 68_000_000.0      # fallback_equity
ENGINE_MAX_POSITIONS = 100         # risk.max_concurrent_positions
ENGINE_GROSS_CAP = 0.95            # risk.max_gross_exposure_pct
ENGINE_TOP_SCORE = 100             # score_top_n = max_universe
ENGINE_TURNOVER_TOP = 200          # turnover_top_n
ENGINE_VOLUME_RATIO_TOP = 40       # value_top_n
ENGINE_CHANGE_TOP = 100            # change_top_n
ENGINE_CHANGE_MIN = 0.01           # change_min_pct 1.0 %
ENGINE_MIN_TURNOVER = 1e9          # min_turnover
ENGINE_MAX_DEVIATION = 0.05        # max_dev_pct
ENGINE_WEIGHT_VOLATILITY = 0.5     # score_w_vol (score_w_trend·score_w_pullback 은 기본 1)
MODE_TEXT = {MODE_TURNOVER: "DevScale·거래대금 순", MODE_RANDOM_ORDER: "DevScale·무작위 순",
             MODE_SIGNAL_ORDER: "(A) DevScale·신호 순", MODE_SIGNAL_FILTER: "(B) DevScale·신호 상위만",
             MODE_SIGNAL_ONLY: "(C) 신호 단독·다음 날 시가", MODE_ENGINE: "(E) 엔진 실제 설정(스캔 상위 100·점수 순)"}
AMOUNT_TEXT = {AMOUNT_FIXED: "고정 500만", AMOUNT_COMPOUND: "평가금÷S 복리", AMOUNT_ENGINE: "엔진 점수 금액 270만–400만"}
REASON_STOP, REASON_TAKE_PROFIT, REASON_ZONE_LOW, REASON_ZONE_HIGH = 1, 2, 3, 4
REASON_TREND, REASON_DELIST, REASON_END = 10, 11, 12
REASON_LEVEL_STOP, REASON_LEVEL_TAKE = 13, 14
REASON_TEXT = {REASON_STOP: "손절 −6.5%", REASON_TAKE_PROFIT: "익절 +3%", REASON_ZONE_LOW: "존 이탈(아래)",
               REASON_ZONE_HIGH: "존 이탈(위)", REASON_TREND: "SMA60 종가 이탈", REASON_DELIST: "상장폐지 현금화",
               REASON_END: "시험 끝 보유", REASON_LEVEL_STOP: "격자 손절", REASON_LEVEL_TAKE: "격자 익절"}
SIGNALS = ("VALUE", "QUAL", "HIGH250")
PARAMETER_FIELDS = ("mode", "rule", "slots", "amount_mode", "slippage", "signal", "quantile", "seed", "control",
                    "pair_row", "log", "take_profit", "stop_loss", "capital", "gross_cap", "fixed_amount")
LOG_FIELDS = ("stock", "entry_day", "exit_day", "reason", "spend", "proceeds", "entry_price", "exit_price")
SCALAR_FIELDS = ("buys", "closed_trades", "net_sum", "spend_sum", "hold_day_sum", "held_day_sum", "commission",
                 "tax", "slippage", "log_overflow", "no_cash_skips", "gross_skips")


class Market:
    """매매에 쓰는 종목 × 날짜 배열(스터디 38 격자, 41 종목·가격 기준)."""

    def __init__(self):
        panel = np.load(grid.PANEL_PATH, allow_pickle=True)
        inputs = np.load(study41.CACHE / "inputs.npz", allow_pickle=False)
        dates = pd.DatetimeIndex(panel["dates"])
        input_dates = pd.to_datetime(inputs["dates"].astype(str))
        self.first_day = int(dates.get_loc(input_dates[0]))
        positions = dates.get_indexer(input_dates)

        if not np.array_equal(positions, np.arange(self.first_day, self.first_day + len(input_dates))):
            raise SystemExit("38 패널과 41 입력의 날짜가 이어지지 않는다")

        self.dates = dates
        self.input_dates = input_dates
        self.codes = inputs["codes"]
        self.names = inputs["names"]
        rows = pd.Index(panel["tickers"]).get_indexer(self.codes)

        if (rows < 0).any():
            raise SystemExit("41 종목 중 38 패널에 없는 종목이 있다")

        self.panel_rows = rows
        self.excluded_codes = np.setdiff1d(panel["tickers"], self.codes)
        day_count = len(dates)
        stock_count = len(self.codes)
        self.day_count, self.stock_count = day_count, stock_count
        close_panel = panel["close"][rows]
        ratio = np.full((stock_count, day_count), np.nan)

        with np.errstate(invalid="ignore", divide="ignore"):
            ratio[:, self.first_day:] = inputs["close"].T / close_panel[:, self.first_day:]

        ratio = pd.DataFrame(ratio.T).ffill().bfill().fillna(1.0).to_numpy().T
        # 같은 가격을 나눈 비는 1 ± 1e-14 로 흔들려 38 의 등호 체결 판정(저가 == 지정가)을 뒤집는다. 1 근처는 1로 맞춘다
        ratio = np.where(np.abs(ratio - 1.0) < 1e-9, 1.0, ratio)
        self.ratio_change_days = int((np.abs(np.diff(np.log(ratio), axis=1)) > 1e-9).sum())
        self.open = np.ascontiguousarray(panel["open"][rows] * ratio)
        self.high = np.ascontiguousarray(panel["high"][rows] * ratio)
        self.low = np.ascontiguousarray(panel["low"][rows] * ratio)
        self.close = np.ascontiguousarray(close_panel * ratio)
        self.halted = np.ascontiguousarray(panel["halted"][rows])
        self.static_market = panel["market"][rows].astype(np.int64)

        def padded(matrix, fill):
            out = np.full((stock_count, day_count), fill, dtype=np.float64)
            out[:, self.first_day:] = matrix.T
            return out

        with np.errstate(invalid="ignore", divide="ignore"):
            raw_factor = padded(inputs["raw_close"] / inputs["close"], np.nan)

        self.raw_factor = np.ascontiguousarray(pd.DataFrame(raw_factor.T).ffill().bfill().fillna(1.0).to_numpy().T)
        market = padded(np.where(inputs["market"] >= 0, inputs["market"], np.nan), np.nan)
        market = pd.DataFrame(market.T).ffill().bfill().to_numpy().T
        self.market = np.ascontiguousarray(np.where(np.isfinite(market), market, self.static_market[:, None])
                                           .astype(np.int64))
        lock = padded(inputs["lock"].astype(np.float64), 0.0).astype(np.int64)
        self.lock_down = np.ascontiguousarray((lock & 2) != 0)
        self.lock_up = np.ascontiguousarray((lock & 1) != 0)
        self.previous_20d = np.ascontiguousarray(padded(inputs["previous_20d"].astype(np.float64), np.nan))
        tax_dates = pd.DatetimeIndex([day for day, _ in grid.SELL_TAX_SCHEDULE])
        self.tax = (np.array([rate for _, rate in grid.SELL_TAX_SCHEDULE])
                    [np.searchsorted(tax_dates, dates, "right") - 1] / 100.0)
        self.new_from = int(np.searchsorted(dates, pd.Timestamp(grid.NEW_TICK_TABLE_FROM)))
        self.start_index = int(np.searchsorted(dates, pd.Timestamp(grid.TEST_START)))
        filled_close = pd.DataFrame(self.close.T).ffill()
        self.sma60 = np.ascontiguousarray(filled_close.rolling(60).mean().to_numpy().T)
        self.last_valid = np.array([grid.last_valid_index(self.close[stock]) for stock in range(stock_count)],
                                   dtype=np.int64)
        self.delist_multiplier = inputs["delist_multiplier"].astype(np.float64)
        self.kospi_close = inputs["kospi_close"].astype(np.float64)
        self.kosdaq_close = inputs["kosdaq_close"].astype(np.float64)
        self.decision_days = inputs["decision_days"].astype(np.int64)
        position = np.arange(day_count) - self.first_day
        decision = np.searchsorted(self.decision_days, position, "left") - 1
        self.decision_of_day = np.where(position > 0, decision, -1).astype(np.int64)
        self._signal_tables(inputs)
        self._eligible(inputs)

    def _signal_tables(self, inputs) -> None:
        """신호 백분위(그날 41 '전 종목' 소속·값 있는 종목 안, 1 = 가장 큼)와 신호 큰 순 종목 목록."""
        member = ((inputs["universe"] >> np.uint32(study41.UNIVERSE_NAMES.index("ALL"))) & 1).astype(bool)
        decision_count = member.shape[0]
        self.signal_percentile = np.full((len(SIGNALS), decision_count, self.stock_count), np.nan)
        order_start = np.zeros((len(SIGNALS), decision_count + 1), dtype=np.int64)
        orders = []

        for number, name in enumerate(SIGNALS):
            score = inputs["scores"][study41.SIGNAL_NAMES.index(name)].astype(np.float64)
            score = np.where(member & np.isfinite(score), score, np.nan)
            percentile = pd.DataFrame(score).rank(axis=1, pct=True).to_numpy()
            self.signal_percentile[number] = percentile
            filled = np.where(np.isfinite(percentile), -percentile, np.inf)
            ranked = np.argsort(filled, axis=1, kind="stable")
            counts = np.isfinite(percentile).sum(axis=1)
            order_start[number, 1:] = np.cumsum(counts)
            orders.append(np.concatenate([ranked[decision, :counts[decision]] for decision in range(decision_count)]))

        self.signal_order_start = order_start
        self.signal_order = np.zeros((len(SIGNALS), max(len(order) for order in orders)), dtype=np.int64)

        for number, order in enumerate(orders):
            self.signal_order[number, :len(order)] = order

    def _eligible(self, inputs) -> None:
        """무작위 대조에서 시가에 살 수 있는 종목(날짜별 목록): 시가 ≥ 2,000원, 정지·상한가 잠김 아님."""
        with np.errstate(invalid="ignore"):
            eligible = (np.isfinite(self.open) & ~self.halted & (self.open * self.raw_factor >= MIN_PRICE) & ~self.lock_up
                        & np.isfinite(self.close))

        eligible[:, :self.first_day] = False
        self.open_buyable = np.ascontiguousarray(eligible)
        days, stocks = np.nonzero(eligible.T)
        self.eligible_start = np.searchsorted(days, np.arange(self.day_count + 1)).astype(np.int64)
        self.eligible_stock = stocks.astype(np.int64)

    def candidates(self) -> None:
        """DevScale 매수 후보(종목·날짜별 지정가). 같은 종목 보유 여부와 무관한 판정이라 미리 낸다."""
        self.entry_fill = np.full((self.stock_count, self.day_count), np.nan)
        self.entry_path = np.zeros((self.stock_count, self.day_count), dtype=np.int8)
        candidate_matrix(self.open, self.high, self.low, self.close, self.halted, self.static_market, self.new_from,
                         self.start_index, self.entry_fill, self.entry_path)
        self.entry_fill[:, :self.first_day] = np.nan
        # 엔진은 그날 실제 가격으로 2,000원 하한을 본다. 수정주가 판정(38 entry_allowed)에 무수정 시가 조건을 더한다
        with np.errstate(invalid="ignore"):
            self.entry_fill[~(self.open * self.raw_factor >= MIN_PRICE)] = np.nan
        days, stocks = np.nonzero(np.isfinite(self.entry_fill.T))
        self.candidate_start = np.searchsorted(days, np.arange(self.day_count + 1)).astype(np.int64)
        self.candidate_stock = stocks.astype(np.int64)
        self.candidate_score = np.full(len(stocks), np.nan)
        self.candidate_notional = np.full(len(stocks), np.nan)

    def engine_scan(self) -> dict:
        """엔진 스캔(상위 100)과 점수 금액을 매수 후보마다 낸다(candidate_score·candidate_notional, 스캔 밖이면 NaN).

        풀은 전날 값(거래대금·거래량 비·등락률), 점수 모집단은 그날 시가, 후보 자신은 체결가로 본다(README 43-D).
        """
        inputs = np.load(study41.CACHE / "inputs.npz", allow_pickle=False)
        volume = np.zeros((self.stock_count, self.day_count))
        volume[:, self.first_day:] = np.nan_to_num(inputs["volume"].T.astype(np.float64))
        average_range = np.full((self.stock_count, self.day_count), np.nan)
        atr_matrix(self.high, self.low, self.close, average_range)
        pool_size = np.zeros(self.day_count, dtype=np.int64)
        passed_size = np.zeros(self.day_count, dtype=np.int64)
        engine_scan_kernel(self.open, self.close, self.raw_factor, volume, self.halted, average_range, self.first_day,
                           self.entry_fill, self.candidate_start, self.candidate_stock, self.candidate_score,
                           self.candidate_notional, pool_size, passed_size)
        span = slice(self.first_day + 2, self.day_count)
        member = np.isfinite(self.candidate_score)
        return {"pool_mean": float(pool_size[span].mean()), "passed_mean": float(passed_size[span].mean()),
                "candidates": int(len(member)), "candidates_in_scan": int(member.sum()),
                "notional_mean": float(np.nanmean(self.candidate_notional)),
                "notional_floor_share": float(np.mean(self.candidate_notional[member] <= ENGINE_FLOOR_KRW + 1e-6)),
                "notional_cap_share": float(np.mean(self.candidate_notional[member] >= ENGINE_CAP_KRW - 1e-6))}

    def run(self, parameters: np.ndarray, target_buys: np.ndarray, log_capacity: int):
        count = parameters.shape[0]
        span = self.day_count - self.first_day
        equity = np.zeros((count, span))
        buys = np.zeros((count, span), dtype=np.int32)
        scalars = np.zeros((count, len(SCALAR_FIELDS)))
        logs = np.zeros((count, max(log_capacity, 1), len(LOG_FIELDS)))
        log_counts = np.zeros(count, dtype=np.int64)
        run_batch(parameters.astype(np.float64), target_buys, self.open, self.high, self.low, self.close, self.halted,
                  self.sma60, self.lock_down, self.previous_20d, self.raw_factor, self.market, self.tax, self.new_from,
                  self.first_day, self.last_valid, self.delist_multiplier, self.entry_fill, self.candidate_start,
                  self.candidate_stock, self.eligible_start, self.eligible_stock, self.open_buyable, self.decision_of_day,
                  self.signal_percentile, self.signal_order_start, self.signal_order, OLD_RULE, equity, buys, scalars,
                  logs, log_counts, log_capacity, self.candidate_score, self.candidate_notional)
        return equity, buys, scalars, logs, log_counts


@njit(parallel=True, cache=True)
def candidate_matrix(open_price, high, low, close, halted, market, new_from, start_index, entry_fill, entry_path):
    for stock in prange(close.shape[0]):
        average_range = grid.atr_series(high[stock], low[stock], close[stock])

        for day in range(21, close.shape[1]):
            if not grid.entry_allowed(open_price[stock], close[stock], halted[stock], average_range, day, start_index):
                continue

            price, path = grid.entry_price(open_price[stock], high[stock], low[stock], close[stock], day,
                                           market[stock], day >= new_from)

            if path > 0:
                entry_fill[stock, day] = price
                entry_path[stock, day] = path


@njit(parallel=True, cache=True)
def atr_matrix(high, low, close, out):
    """종목 × 날짜 ATR14(그날 쓰는 값 = 전날까지 14봉, 38 atr_series 그대로)."""
    for stock in prange(close.shape[0]):
        out[stock] = grid.atr_series(high[stock], low[stock], close[stock])


@njit(cache=True)
def folded_features(close, average_range, stock, day, price):
    """오늘 가격 price 를 접은 SMA5·10·20 으로 (통과 여부, 추세, 눌림, ATR%). 엔진 UniverseFeatures 그대로.

    전날까지 19봉이 다 있어야 하고, 정배열(SMA5 > SMA10 > SMA20)·이격 ≤ 5% 를 넘어야 통과다.
    """
    if not (price > 0) or day < 20:
        return False, 0.0, 0.0, 0.0

    sum4 = 0.0
    sum9 = 0.0
    sum19 = 0.0

    for back in range(1, 20):
        value = close[stock, day - back]

        if not np.isfinite(value):
            return False, 0.0, 0.0, 0.0

        sum19 += value

        if back <= 9:
            sum9 += value

        if back <= 4:
            sum4 += value

    average5 = (sum4 + price) / 5.0
    average10 = (sum9 + price) / 10.0
    average20 = (sum19 + price) / 20.0
    previous_close = close[stock, day - 1]
    range_today = average_range[stock, day]

    if not (average5 > average10 and average10 > average20) or not np.isfinite(range_today):
        return False, 0.0, 0.0, 0.0

    pullback = (price - average20) / average20

    if pullback > ENGINE_MAX_DEVIATION:
        return False, 0.0, 0.0, 0.0

    return True, (average5 - average20) / average20, pullback, range_today / previous_close


@njit(cache=True)
def clipped_z(value, mean, deviation):
    if not (deviation > 1e-12):
        return 0.0

    return min(2.0, max(-2.0, (value - mean) / deviation))


@njit(cache=True)
def mark_top(values, valid, count, mark):
    """valid 인 종목 중 values 큰 순 count 개를 mark 에 켠다(동점은 종목 번호 순 — 코드 사전순)."""
    order = np.argsort(-np.where(valid, values, -np.inf), kind="mergesort")
    taken = 0

    for place in range(order.shape[0]):
        if taken >= count:
            break

        stock = order[place]

        if not valid[stock]:
            break

        mark[stock] = True
        taken += 1


@njit(cache=True)
def engine_scan_kernel(open_price, close, raw_factor, volume, halted, average_range, first_day, entry_fill,
                       candidate_start, candidate_stock, candidate_score, candidate_notional, pool_size, passed_size):
    """하루마다 엔진 스캔을 일봉으로 재현한다(README 43-D "일봉 재현 방법").

    풀: 전날 거래대금 상위 200 ∪ 전날 거래량 비 상위 40 ∪ 전날 등락률(≥ 1%) 상위 100. 거래대금 ≥ 10억(전날),
    그날 무수정 시가 ≥ 2,000원, 그날 정지 아님. 점수 모집단 = 풀 중 그날 시가를 접어 통과한 종목.
    후보 자신은 체결가를 접어 다시 판정하고 모집단 평균·표준편차로 점수를 매긴다.
    """
    stock_count, day_count = close.shape
    eligible = np.zeros(stock_count, dtype=np.bool_)
    turnover = np.zeros(stock_count)
    ratio = np.zeros(stock_count)
    change = np.zeros(stock_count)
    ratio_valid = np.zeros(stock_count, dtype=np.bool_)
    change_valid = np.zeros(stock_count, dtype=np.bool_)
    pool = np.zeros(stock_count, dtype=np.bool_)
    passed_stock = np.empty(stock_count, dtype=np.int64)
    trend = np.empty(stock_count)
    pull = np.empty(stock_count)
    volatility = np.empty(stock_count)
    score = np.empty(stock_count)

    for day in range(first_day + 2, day_count):
        for stock in range(stock_count):
            pool[stock] = False
            previous = day - 1
            raw_close = close[stock, previous] * raw_factor[stock, previous]
            turnover[stock] = raw_close * volume[stock, previous] if np.isfinite(raw_close) else 0.0
            eligible[stock] = (not halted[stock, day] and np.isfinite(open_price[stock, day])
                               and open_price[stock, day] * raw_factor[stock, day] >= MIN_PRICE
                               and turnover[stock] >= ENGINE_MIN_TURNOVER)
            ratio_valid[stock] = eligible[stock] and volume[stock, day - 2] > 0
            ratio[stock] = volume[stock, previous] / volume[stock, day - 2] if ratio_valid[stock] else 0.0
            earlier = close[stock, day - 2]
            change[stock] = close[stock, previous] / earlier - 1.0 if earlier > 0 else np.nan
            change_valid[stock] = eligible[stock] and np.isfinite(change[stock]) and change[stock] >= ENGINE_CHANGE_MIN

        mark_top(turnover, eligible, ENGINE_TURNOVER_TOP, pool)
        mark_top(ratio, ratio_valid, ENGINE_VOLUME_RATIO_TOP, pool)
        mark_top(change, change_valid, ENGINE_CHANGE_TOP, pool)
        passed = 0

        for stock in range(stock_count):
            if not pool[stock]:
                continue

            pool_size[day] += 1
            ok, trend_value, pull_value, volatility_value = folded_features(close, average_range, stock, day,
                                                                            open_price[stock, day])

            if ok:
                passed_stock[passed] = stock
                trend[passed] = trend_value
                pull[passed] = pull_value
                volatility[passed] = volatility_value
                passed += 1

        passed_size[day] = passed
        trend_mean = 0.0
        trend_deviation = 0.0
        pull_mean = 0.0
        pull_deviation = 0.0
        volatility_mean = 0.0
        volatility_deviation = 0.0

        if passed >= 2:
            trend_mean = trend[:passed].mean()
            trend_deviation = trend[:passed].std()
            pull_mean = pull[:passed].mean()
            pull_deviation = pull[:passed].std()
            volatility_mean = volatility[:passed].mean()
            volatility_deviation = volatility[:passed].std()

        for place in range(passed):
            score[place] = (clipped_z(trend[place], trend_mean, trend_deviation)
                            - clipped_z(pull[place], pull_mean, pull_deviation)
                            - ENGINE_WEIGHT_VOLATILITY * clipped_z(volatility[place], volatility_mean, volatility_deviation))

        top = np.sort(score[:passed])[::-1][:ENGINE_TOP_SCORE].copy()
        top_mean = top.mean() if top.shape[0] > 0 else 0.0
        top_deviation = top.std() if top.shape[0] > 0 else 0.0

        for place in range(candidate_start[day], candidate_start[day + 1]):
            stock = candidate_stock[place]

            if not pool[stock]:
                continue

            ok, trend_value, pull_value, volatility_value = folded_features(close, average_range, stock, day,
                                                                            entry_fill[stock, day])

            if not ok:
                continue

            value = (clipped_z(trend_value, trend_mean, trend_deviation) - clipped_z(pull_value, pull_mean, pull_deviation)
                     - ENGINE_WEIGHT_VOLATILITY * clipped_z(volatility_value, volatility_mean, volatility_deviation))
            ahead = 0

            for other in range(passed):
                if passed_stock[other] != stock and score[other] > value:
                    ahead += 1

            if ahead >= ENGINE_TOP_SCORE:
                continue

            z_value = clipped_z(value, top_mean, top_deviation) if passed >= 2 else 0.0
            factor = min(1.0, max(0.0, z_value / ENGINE_CAP_Z))
            candidate_score[place] = value
            candidate_notional[place] = ENGINE_FLOOR_KRW + (ENGINE_CAP_KRW - ENGINE_FLOOR_KRW) * factor


@njit(cache=True)
def trend_exit(open_price, high, low, close, halted, sma60, lock_down, entry_day, last, price, take_profit, stop_loss):
    """엔진 judge_trend_exit 에 장중 익절·손절(평단 %, 0 = 없음)을 더한다.

    SMA60: 매수일 뒤 종가 판정, 매수일부터 전날까지 종가 > SMA60 이 한 번 있었어야 하고, 이탈 다음 거래일 시가에 판다.
    익절·손절(43-D): 매수 다음 날부터 시가가 이미 넘었으면 시가, 아니면 장중에 닿은 값. 같은 날 둘 다면 손절 먼저.
    매수 당일은 손절만 본다(체결 전 고가일 수 있다). 시가 하한가 잠김이면 다음 잠기지 않은 시가로 미룬다.
    둘 다 0 이면 43 1·2단계의 D158 과 같은 결과를 낸다.
    """
    take_line = price * (1.0 + take_profit / 100.0) if take_profit > 0 else np.inf
    stop_line = price * (1.0 - stop_loss / 100.0) if stop_loss > 0 else -np.inf
    armed = False
    pending_reason = 0

    for day in range(entry_day, last + 1):
        if day > entry_day:
            if halted[day]:
                continue

            day_open = open_price[day]

            if pending_reason == 0:
                if day_open <= stop_line:
                    pending_reason = REASON_LEVEL_STOP
                elif day_open >= take_line:
                    pending_reason = REASON_LEVEL_TAKE

            if pending_reason != 0:
                if not lock_down[day]:
                    return day, day_open, pending_reason, True

                continue

        if low[day] <= stop_line:
            return day, stop_line, REASON_LEVEL_STOP, False

        if day > entry_day and high[day] >= take_line:
            return day, take_line, REASON_LEVEL_TAKE, False

        if day > entry_day and armed and close[day] < sma60[day]:
            pending_reason = REASON_TREND
            continue

        if close[day] > sma60[day]:
            armed = True

    return -1, np.nan, 0, True


@njit(cache=True)
def exit_of(rule, stock, entry_day, price, open_price, high, low, close, halted, sma60, lock_down, last_valid,
            old_rule, empty_days, empty_levels, take_profit, stop_loss):
    """(매도일, 매도가(수정), 이유, 시가 매도 여부). 매도일 = 날짜 수면 시험 끝까지 보유."""
    last = last_valid[stock]
    end_index = close.shape[1] - 1
    exit_day, exit_price, reason, at_open = -1, np.nan, 0, True

    if rule == RULE_TREND:
        exit_day, exit_price, reason, at_open = trend_exit(open_price[stock], high[stock], low[stock], close[stock],
                                                           halted[stock], sma60[stock], lock_down[stock], entry_day, last,
                                                           price, take_profit, stop_loss)
    else:
        exit_day, exit_price, old_reason, ambiguous, stop_exit = grid.exit_for_entry(
            open_price[stock], high[stock], low[stock], close[stock], halted[stock], last, end_index, entry_day, price,
            old_rule, empty_days, empty_levels, price * (1.0 - old_rule[1] / 100.0))

        if old_reason == 6 or old_reason == 8:
            exit_day = -1
        else:
            reason = int(old_reason)
            at_open = exit_day > entry_day and exit_price == open_price[stock, exit_day]

            if at_open and lock_down[stock, exit_day] and (reason == REASON_STOP or reason == REASON_ZONE_LOW):
                deferred = -1

                for later in range(exit_day + 1, last + 1):
                    if not halted[stock, later] and not lock_down[stock, later]:
                        deferred = later
                        break

                if deferred < 0:
                    exit_day = -1
                else:
                    exit_day = deferred
                    exit_price = open_price[stock, deferred]

    if exit_day < 0:
        if last < end_index:
            return last + 1, close[stock, last], REASON_DELIST, True

        return end_index + 1, close[stock, last], REASON_END, True

    return exit_day, exit_price, reason, at_open


@njit(cache=True)
def slip_fraction(price, raw_factor, market, new_table, slippage):
    """미끄러짐 호가 수를 가격 비율로(무수정가 호가 기준)."""
    if slippage <= 0:
        return 0.0

    raw = price * raw_factor
    return slippage * grid.tick_size(raw, market, new_table) / raw


@njit(cache=True)
def simulate(mode, rule, slots, amount_mode, slippage, signal, quantile, seed, control, take_profit, stop_loss,
             capital_given, gross_cap, fixed_amount, target_buys,
             open_price, high, low, close, halted, sma60, lock_down, previous_20d, raw_factor, market, tax, new_from,
             first_day, last_valid, delist_multiplier, entry_fill, candidate_start, candidate_stock, eligible_start,
             eligible_stock, open_buyable, decision_of_day, signal_percentile, signal_order_start, signal_order, old_rule,
             out_equity, out_buys, scalars, log, log_capacity, candidate_score, candidate_notional):
    stock_count, day_count = close.shape
    empty_days = np.empty(0, dtype=np.int64)
    empty_levels = np.empty(0)
    held = np.zeros(stock_count, dtype=np.bool_)
    units = np.zeros(stock_count)
    spend_of = np.zeros(stock_count)
    entry_day_of = np.zeros(stock_count, dtype=np.int64)
    entry_price_of = np.zeros(stock_count)
    exit_day_of = np.zeros(stock_count, dtype=np.int64)
    exit_price_of = np.zeros(stock_count)
    reason_of = np.zeros(stock_count, dtype=np.int64)
    at_open_of = np.zeros(stock_count, dtype=np.bool_)
    last_price = np.zeros(stock_count)
    sold_day = np.full(stock_count, -1, dtype=np.int64)
    held_list = np.empty(stock_count, dtype=np.int64)
    held_count = 0
    log_count = 0
    candidate_key = np.empty(stock_count)
    candidate_second = np.empty(stock_count)
    candidate_list = np.empty(stock_count, dtype=np.int64)
    pick_size = max(64, int(slots))     # 대조가 하루에 고를 수 있는 최대 건수(슬롯 100 계좌는 100)
    pick_key = np.empty(pick_size)
    pick_stock = np.empty(pick_size, dtype=np.int64)
    amount_of = np.zeros(stock_count)
    capital = capital_given if capital_given > 0 else slots * SLOT_AMOUNT
    slot_amount = fixed_amount if fixed_amount > 0 else SLOT_AMOUNT
    cash = capital
    equity_previous = capital

    for field in range(scalars.shape[0]):
        scalars[field] = 0.0

    for day in range(first_day, day_count):
        position = day - first_day
        new_table = day >= new_from

        # ① 폐지 현금화·시가 매도
        for phase in range(2):
            index = 0

            while index < held_count:
                stock = held_list[index]

                if exit_day_of[stock] != day or at_open_of[stock] != (phase == 0):
                    index += 1
                    continue

                price = exit_price_of[stock]

                if reason_of[stock] == REASON_DELIST:
                    gross = units[stock] * price * delist_multiplier[stock]
                    slip_cost = 0.0
                else:
                    fraction = slip_fraction(price, raw_factor[stock, day], market[stock, day], new_table, slippage)
                    gross = units[stock] * price * max(1.0 - fraction, 0.0)
                    slip_cost = units[stock] * price - gross

                commission_cost = gross * COMMISSION
                tax_cost = gross * tax[day]
                proceeds = gross - commission_cost - tax_cost
                cash += proceeds
                scalars[6] += commission_cost
                scalars[7] += tax_cost
                scalars[8] += slip_cost
                scalars[1] += 1.0
                scalars[2] += proceeds / spend_of[stock] - 1.0
                scalars[4] += day - entry_day_of[stock] + 1

                if log_capacity > 0:
                    if log_count < log_capacity:
                        log[log_count, 0] = stock
                        log[log_count, 1] = entry_day_of[stock]
                        log[log_count, 2] = day
                        log[log_count, 3] = reason_of[stock]
                        log[log_count, 4] = spend_of[stock]
                        log[log_count, 5] = proceeds
                        log[log_count, 6] = entry_price_of[stock]
                        log[log_count, 7] = price
                        log_count += 1
                    else:
                        scalars[9] = 1.0

                held[stock] = False
                units[stock] = 0.0
                sold_day[stock] = day
                held_count -= 1
                held_list[index] = held_list[held_count]

            if phase == 0:
                # ②·③ 매수(시가 매도 뒤, 장중 매도 앞)
                free = slots - held_count
                picked = 0
                at_open = False

                if free > 0 and control:
                    at_open = True
                    wanted = min(target_buys[position], free, pick_key.shape[0])

                    for slot in range(wanted):
                        pick_key[slot] = np.inf
                        pick_stock[slot] = -1

                    for place in range(eligible_start[day], eligible_start[day + 1]):
                        stock = eligible_stock[place]

                        if held[stock] or sold_day[stock] == day:
                            continue

                        key = basket.random_key(seed, day, stock)

                        if wanted > 0 and key < pick_key[wanted - 1]:
                            slot = wanted - 1

                            while slot > 0 and pick_key[slot - 1] > key:
                                pick_key[slot] = pick_key[slot - 1]
                                pick_stock[slot] = pick_stock[slot - 1]
                                slot -= 1

                            pick_key[slot] = key
                            pick_stock[slot] = stock

                    for slot in range(wanted):
                        if pick_stock[slot] >= 0:
                            candidate_list[picked] = pick_stock[slot]
                            picked += 1
                elif free > 0 and mode == MODE_SIGNAL_ONLY:
                    at_open = True
                    decision = decision_of_day[day]

                    if decision >= 0:
                        for place in range(signal_order_start[signal, decision], signal_order_start[signal, decision + 1]):
                            if picked >= free:
                                break

                            stock = signal_order[signal, place]

                            if held[stock] or sold_day[stock] == day or not open_buyable[stock, day]:
                                continue

                            candidate_list[picked] = stock
                            picked += 1
                elif free > 0:
                    count = 0
                    decision = decision_of_day[day]

                    for place in range(candidate_start[day], candidate_start[day + 1]):
                        stock = candidate_stock[place]

                        if held[stock] or sold_day[stock] == day:
                            continue

                        percentile = np.nan

                        if signal >= 0 and decision >= 0:
                            percentile = signal_percentile[signal, decision, stock]

                        if mode == MODE_SIGNAL_FILTER and not (percentile >= 1.0 - quantile):
                            continue

                        if mode == MODE_ENGINE and not np.isfinite(candidate_score[place]):
                            continue

                        turnover = previous_20d[stock, day]
                        second = -turnover if np.isfinite(turnover) else np.inf

                        if mode == MODE_ENGINE:
                            key = -candidate_score[place]
                            amount_of[stock] = candidate_notional[place]
                        elif mode == MODE_RANDOM_ORDER:
                            key = basket.random_key(seed, day, stock)
                        elif mode == MODE_SIGNAL_ORDER:
                            key = -percentile if np.isfinite(percentile) else 1.0
                        else:
                            key = second

                        candidate_list[count] = stock
                        candidate_key[count] = key
                        candidate_second[count] = second
                        count += 1

                    if count > 0:
                        order = np.argsort(candidate_second[:count], kind="mergesort")
                        primary = candidate_key[:count][order]
                        order = order[np.argsort(primary, kind="mergesort")]
                        chosen = np.empty(count, dtype=np.int64)

                        for place in range(count):
                            chosen[place] = candidate_list[order[place]]

                        for place in range(count):
                            candidate_list[place] = chosen[place]

                        picked = count

                bought = 0
                exposure = 0.0

                if gross_cap > 0:
                    for index in range(held_count):
                        exposure += units[held_list[index]] * last_price[held_list[index]]

                for place in range(picked):
                    if held_count >= slots:
                        break

                    stock = candidate_list[place]
                    price = open_price[stock, day] if at_open else entry_fill[stock, day]

                    if amount_mode == AMOUNT_ENGINE:
                        amount = amount_of[stock]
                    elif amount_mode == AMOUNT_FIXED:
                        amount = slot_amount
                    else:
                        amount = equity_previous / slots

                    # 총노출 상한(엔진 OrderGate reject_by_gross_exposure): 넘으면 그 주문을 거부한다
                    if gross_cap > 0 and exposure + amount > gross_cap * equity_previous:
                        scalars[11] += 1.0
                        continue

                    amount = min(amount, cash)
                    fraction = slip_fraction(price, raw_factor[stock, day], market[stock, day], new_table, slippage)
                    raw_fill = price * raw_factor[stock, day] * (1.0 + fraction)
                    per_share = raw_fill * (1.0 + COMMISSION)
                    shares = np.floor(amount / per_share) if per_share > 0 else 0.0

                    if shares < 1.0:
                        scalars[10] += 1.0
                        continue

                    spend = shares * per_share
                    cash -= spend
                    exposure += spend
                    units[stock] = shares * raw_factor[stock, day]
                    spend_of[stock] = spend
                    scalars[6] += shares * raw_fill * COMMISSION
                    scalars[8] += shares * raw_fill - units[stock] * price
                    scalars[0] += 1.0
                    scalars[3] += spend
                    entry_day_of[stock] = day
                    entry_price_of[stock] = price
                    last_price[stock] = price
                    exit_day, exit_price, reason, exit_at_open = exit_of(
                        rule, stock, day, price, open_price, high, low, close, halted, sma60, lock_down, last_valid,
                        old_rule, empty_days, empty_levels, take_profit, stop_loss)
                    exit_day_of[stock] = exit_day
                    exit_price_of[stock] = exit_price
                    reason_of[stock] = reason
                    at_open_of[stock] = exit_at_open
                    held[stock] = True
                    held_list[held_count] = stock
                    held_count += 1
                    bought += 1

                out_buys[position] = bought

        # ⑤ 종가 평가
        value = cash

        for index in range(held_count):
            stock = held_list[index]
            price = close[stock, day]

            if np.isfinite(price):
                last_price[stock] = price

            value += units[stock] * last_price[stock]

        out_equity[position] = value
        equity_previous = value
        scalars[5] += held_count

    # 시험 끝 보유: 마지막 종가에서 매도 비용을 뗀 값으로 매매 통계에 넣는다(평가금에는 비용 없음)
    for index in range(held_count):
        stock = held_list[index]
        gross = units[stock] * last_price[stock]
        proceeds = gross * (1.0 - COMMISSION - tax[day_count - 1])
        scalars[1] += 1.0
        scalars[2] += proceeds / spend_of[stock] - 1.0
        scalars[4] += day_count - entry_day_of[stock]

        if log_capacity > 0:
            if log_count < log_capacity:
                log[log_count, 0] = stock
                log[log_count, 1] = entry_day_of[stock]
                log[log_count, 2] = day_count - 1
                log[log_count, 3] = REASON_END
                log[log_count, 4] = spend_of[stock]
                log[log_count, 5] = proceeds
                log[log_count, 6] = entry_price_of[stock]
                log[log_count, 7] = last_price[stock]
                log_count += 1
            else:
                scalars[9] = 1.0

    return log_count


@njit(parallel=True, cache=True)
def run_batch(parameters, target_buys, open_price, high, low, close, halted, sma60, lock_down, previous_20d,
              raw_factor, market, tax, new_from, first_day, last_valid, delist_multiplier, entry_fill, candidate_start,
              candidate_stock, eligible_start, eligible_stock, open_buyable, decision_of_day, signal_percentile,
              signal_order_start, signal_order, old_rule, out_equity, out_buys, out_scalars, logs, log_counts, log_capacity,
              candidate_score, candidate_notional):
    """parameters 열: PARAMETER_FIELDS. 대조(control=1)는 target_buys[pair_row] 를 그날 매수 건수로 쓴다."""
    for run in prange(parameters.shape[0]):
        row = parameters[run]
        capacity = log_capacity if row[10] > 0 else 0
        log_counts[run] = simulate(int(row[0]), int(row[1]), int(row[2]), int(row[3]), int(row[4]), int(row[5]),
                                   row[6], int(row[7]), row[8] > 0, row[11], row[12], row[13], row[14], row[15],
                                   target_buys[int(row[9])], open_price, high, low,
                                   close, halted, sma60, lock_down, previous_20d, raw_factor, market, tax, new_from,
                                   first_day, last_valid, delist_multiplier, entry_fill, candidate_start,
                                   candidate_stock, eligible_start, eligible_stock, open_buyable, decision_of_day,
                                   signal_percentile, signal_order_start, signal_order, old_rule, out_equity[run], out_buys[run],
                                   out_scalars[run], logs[run], capacity, candidate_score, candidate_notional)
