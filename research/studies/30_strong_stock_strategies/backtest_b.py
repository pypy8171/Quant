#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""스터디 30 전략 B(52주 신고가 근접) 백테스트 — research/studies/30_strong_stock_strategies/SPEC.md §3을 그대로 옮긴다.

사전등록(SPEC §3)에서 바꾸지 않은 것: 격자 6칸 N {20,30,50} × 60일 거래대금 하한 {10억, 30억}, 학습 3년 → 검증 1년,
검증 2010~2025(16창) + 2026(부분 창), 보유 유지 규칙(순위 ≤ 1.5N), 비용(왕복 0.31% + 매수·매도 각 0.10%), 합격선 7층.

시점(look-ahead 차단은 배열 인덱스로 한다 — 신호 계산은 month_end_index 이하 행만 자른다):
  - 신호일 M = 매월 마지막 거래일(패널 달력). 점수·유니버스는 M 종가까지의 값만 쓴다.
  - 집행일 E = M 다음 거래일. 기본은 E 종가(바스켓 14:40 집행의 근사, SPEC §3.3). 감도는 E 시가.
  - 한 기간 = E_k 집행가 → E_{k+1} 집행가. 기간 이름은 E_k의 연월(= 보유 달).
  - 보유 중 상장폐지(membership end_reason delisted·merged): 마지막 봉 종가로 청산, 감도로 −30%. 매도 비용을 붙이고 남은 돈은 현금(수익 0).
가격: 패널은 수정주가 × 무수정 거래량(005930 2018-05-04 거래량 33만 → 3,957만 확인). 그래서 거래대금·2,000원 하한은
  data.go.kr 월말 무수정 종가로 만든 수정 비율 f = 무수정 종가 / 수정 종가를 곱해 무수정 기준으로 되돌린다(2020-01~2026-08).
  2020-01 이전 날짜는 2020-01 비율을 그대로 쓴다 — 2019년 이전에 일어난 분할·병합은 되돌리지 못한다(README 의심 지점 1).
이상치: 일간 |수익| > 50%(수정 종가, 정지일은 직전 종가 이어 붙임)가 M까지 252거래일 안에 있으면 그 달 유니버스에서 뺀다.
  보유 중 생긴 이상치는 빼지 않고(사후 정보) 기여분만 따로 센다.
정지: 집행일에 거래가 없는 보유 종목은 팔지 못한 채 그 비중 그대로 묶는다(비교 기준도 같다). 정지 뒤 상장폐지면
  정리매매 마지막 종가로 나온다 — 정지 직전 종가로 파는 것으로 처리하면 상장폐지 손실이 빠진다.

재실행(저장소 루트, 약 15초):
  py -X utf8 research/studies/30_strong_stock_strategies/backtest_b.py
seed: 무작위 요소 없음(numpy seed 0을 걸어 둔다). 같은 입력 파일이면 산출 파일 바이트가 같다 — 입력 해시는 b_metrics.json "inputs".
산출(같은 폴더): b_periods.tsv(실행별 기간 행), b_walkforward.tsv(창별), b_grid.tsv(격자 6칸 고정 실행), b_variants.tsv(감도·제거실험),
  b_checks.json(데이터 대조), b_metrics.json(합격선 판정과 재현 정보).
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
np.random.seed(0)

REPO = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
BARS_PATH = REPO / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
MEMBERSHIP_PATH = REPO / "PYQuant" / "data" / "universe" / "membership.parquet"
KIND_PATH = REPO / "PYQuant" / "data" / "universe" / "kind_delisted.parquet"
DATAGOKR_DIR = REPO / "PYQuant" / "data" / "cache" / "datagokr_shares"
KOSPI_PATH = REPO / "PYQuant" / ".index_cache" / "idx__KS11_1985-01-01_2026-08-15_adj.parquet"
REGIME_PATH = REPO / "research" / "studies" / "26_regime_threshold_history" / "daily_scores.tsv"   # 읽기만 한다
CONFIG_DIR = REPO / "Quant" / "config"

LOAD_FROM = pd.Timestamp("2005-01-01")
FIRST_SIGNAL = pd.Timestamp("2006-12-01")   # 첫 학습 창(2007~2009) 첫 보유 달 2007-01의 신호
LAST_FULL_MONTH = pd.Timestamp("2026-08-31")  # 2026-09는 월말 신호가 없다(패널 끝 2026-09-29)

# SPEC §3.2 고정값
LISTED_DAYS_MIN = 260
PRICE_MIN_KRW = 2_000
VALUE_WINDOW = 60
HIGH_WINDOW = 252
ZERO_VOLUME_LOOKBACK = 5
RETENTION_MULTIPLE = 1.5
OUTLIER_ABS_RETURN = 0.50
FACTOR_TOLERANCE = 0.02    # |무수정/수정 − 1| < 2%면 수정 없음(DATA.md 흔적 판정과 같은 값)

# SPEC §3.5 격자·창
GRID_N = (20, 30, 50)
GRID_FLOOR_KRW = (1_000_000_000, 3_000_000_000)
BASE_CELL = (30, 1_000_000_000)
VALIDATION_YEARS = list(range(2010, 2027))   # 2026은 부분 창
TRAIN_YEARS = 3
HALT_SCORE = -7      # Quant/config/regime.json halt_score


@dataclass(frozen=True)
class Costs:
    """한쪽 비용률(소수). SPEC §3.5: 회전 금액에 왕복 0.31% + 매수·매도 각 0.10% 미끄러짐.
    0.31%는 매도세 0.20%(PYQuant/backtest/costs.py LIVE, 2026 세율) + 수수료 양쪽 0.055%씩으로 나눴다."""
    name: str
    commission: float
    sell_tax: float
    slippage: float
    historical_tax: bool = False

    def buy_rate(self, day: pd.Timestamp) -> float:
        return self.commission + self.slippage

    def sell_rate(self, day: pd.Timestamp) -> float:
        tax = historical_sell_tax(day) if self.historical_tax else self.sell_tax
        return self.commission + tax + self.slippage


def historical_sell_tax(day: pd.Timestamp) -> float:
    """매도세(거래세+농특세) 연도별 근사. 코스피·코스닥 구분 없이 한 값(감도 전용)."""
    if day < pd.Timestamp("2019-06-03"):
        return 0.0030

    if day < pd.Timestamp("2021-01-01"):
        return 0.0025

    if day < pd.Timestamp("2023-01-01"):
        return 0.0023

    if day < pd.Timestamp("2024-01-01"):
        return 0.0020

    if day < pd.Timestamp("2025-01-01"):
        return 0.0018

    if day < pd.Timestamp("2026-01-01"):
        return 0.0015

    return 0.0020


COSTS_SPEC = Costs("spec_0.31+0.10x2", commission=0.00055, sell_tax=0.0020, slippage=0.0010)
COSTS_LIVE = Costs("live_const_0.015/0.20+0.10x2", commission=0.00015, sell_tax=0.0020, slippage=0.0010)
COSTS_HISTORICAL = Costs("spec_comm+hist_tax+0.10x2", commission=0.00055, sell_tax=0.0020, slippage=0.0010, historical_tax=True)


@dataclass(frozen=True)
class RunConfig:
    signal: str = "PTH"           # PTH | MOM12_1 | PTH_AND_MOM | MOM_LIVE
    top_n: int = 30
    floor_krw: int = 1_000_000_000
    execution: str = "close"      # close | open
    delist_haircut: float = 0.0
    halt: bool = False
    kospi_filter: bool = False
    breadth_regime: bool = False  # MOMENTUM 슬리브 국면 ON(유니버스 200일선 위 비율 ≥ 0.5)
    retention: bool = True
    costs: Costs = COSTS_SPEC

    def key(self) -> str:
        parts = [self.signal, f"N{self.top_n}", f"F{self.floor_krw // 100_000_000}"]

        if self.execution != "close":
            parts.append(f"exec_{self.execution}")

        if self.delist_haircut:
            parts.append(f"delist-{int(self.delist_haircut * 100)}")

        if self.halt:
            parts.append("halt")

        if self.kospi_filter:
            parts.append("kospi200ma")

        if self.breadth_regime:
            parts.append("breadth_on")

        if not self.retention:
            parts.append("no_retention")

        if self.costs is not COSTS_SPEC:
            parts.append(self.costs.name)

        return "|".join(parts)


# ── 입력 ────────────────────────────────────────────────────────────────────

def file_digest(path: Path) -> dict:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"path": path.relative_to(REPO).as_posix(), "bytes": path.stat().st_size, "sha256": digest}


def load_name_rules() -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    prefixes = tuple(json.loads((CONFIG_DIR / "etf_prefixes.json").read_text(encoding="utf-8")))
    tokens = tuple(json.loads((CONFIG_DIR / "etf_name_tokens.json").read_text(encoding="utf-8"))) + ("스팩",)
    reit_suffixes = tuple(json.loads((CONFIG_DIR / "reit_name_suffixes.json").read_text(encoding="utf-8")))
    reit_names = tuple(json.loads((CONFIG_DIR / "reit_names.json").read_text(encoding="utf-8")))
    return prefixes, tokens, reit_suffixes + reit_names


def is_common_stock(code: str, name: str, rules) -> bool:
    """보통주만: 코드 끝 0(우선주 제외), ETF·ETN 접두·토큰, 스팩, 리츠 이름 제외(엔진 config와 같은 목록)."""
    prefixes, tokens, reit_tokens = rules
    name_text = name or ""

    if not code.endswith("0"):
        return False

    if name_text.startswith(prefixes):
        return False

    if any(token in name_text for token in tokens):
        return False

    if any(token in name_text for token in reit_tokens):
        return False

    return True


class Panel:
    """일봉 패널을 (날짜 × 종목) 배열로 편다. 결측은 NaN, 종가는 ffill 판(close_filled)을 따로 둔다."""

    def __init__(self):
        bars = pd.read_parquet(BARS_PATH, columns=["Date", "Open", "High", "Close", "Volume", "code"],
                               filters=[("Date", ">=", LOAD_FROM)])
        counts = bars["Date"].value_counts()
        self.calendar = pd.DatetimeIndex(sorted(counts.index[counts >= 300]))
        dropped_rows = int((~bars["Date"].isin(self.calendar)).sum())
        bars = bars[bars["Date"].isin(self.calendar)]
        membership = pd.read_parquet(MEMBERSHIP_PATH)
        self.codes = np.array(sorted(set(bars["code"]) | set(membership["code"])))
        code_position = {code: index for index, code in enumerate(self.codes)}
        day_position = {day: index for index, day in enumerate(self.calendar)}
        rows = bars["Date"].map(day_position).to_numpy()
        columns = bars["code"].map(code_position).to_numpy()
        shape = (len(self.calendar), len(self.codes))

        def spread(values) -> np.ndarray:
            array = np.full(shape, np.nan)
            array[rows, columns] = values
            return array

        self.close = spread(bars["Close"].to_numpy(dtype=float))
        self.volume = spread(bars["Volume"].to_numpy(dtype=float))
        open_price = bars["Open"].to_numpy(dtype=float)
        high = bars["High"].to_numpy(dtype=float)
        close = bars["Close"].to_numpy(dtype=float)
        # 정지일 행은 시가·고가가 0이다(2010 이후 거래량 0 행의 68%). 고가는 종가와 큰 쪽, 시가 0은 결측.
        self.high = spread(np.where(high > 0, np.maximum(high, close), close))
        self.open = spread(np.where(open_price > 0, open_price, np.nan))
        self.exists = ~np.isnan(self.close)
        self.close_filled = pd.DataFrame(self.close).ffill().to_numpy()
        self.dropped_rows = dropped_rows
        self.membership = membership.set_index("code").reindex(self.codes)
        self.day_position = day_position

        previous = np.vstack([np.full((1, shape[1]), np.nan), self.close_filled[:-1]])
        with np.errstate(divide="ignore", invalid="ignore"):
            daily_return = self.close_filled / previous - 1.0
        self.outlier_day = np.nan_to_num(np.abs(daily_return) > OUTLIER_ABS_RETURN).astype(np.int32)
        self.outlier_cumulative = np.cumsum(self.outlier_day, axis=0)
        self.daily_return = daily_return

        self.factor, self.factor_report = self.build_factor()
        traded_value = np.nan_to_num(self.close * self.volume * self.factor)
        self.value_cumulative = np.vstack([np.zeros((1, shape[1])), np.cumsum(traded_value, axis=0)])
        zero_or_missing = (np.nan_to_num(self.volume) <= 0).astype(np.int32)
        self.zero_cumulative = np.vstack([np.zeros((1, shape[1]), dtype=np.int64), np.cumsum(zero_or_missing, axis=0)])

        rules = load_name_rules()
        names = self.membership["name"].fillna("").to_numpy()
        self.common = np.array([is_common_stock(code, name, rules) for code, name in zip(self.codes, names)])
        self.effective_from = self.membership["effective_from"].to_numpy(dtype="datetime64[ns]")
        effective_to = self.membership["effective_to"]
        self.effective_to = effective_to.fillna(pd.Timestamp("2100-01-01")).to_numpy(dtype="datetime64[ns]")
        end_reason = self.membership["end_reason"].fillna("listed").to_numpy()
        self.terminates = np.isin(end_reason, ["delisted", "merged"])
        # 마지막 봉 위치(상장폐지·합병 종목만 의미 있음)
        last_index = np.full(shape[1], -1)
        any_bar = self.exists.any(axis=0)
        last_index[any_bar] = shape[0] - 1 - np.argmax(self.exists[::-1, any_bar], axis=0)
        self.last_bar_index = last_index

    def build_factor(self) -> tuple[np.ndarray, dict]:
        """data.go.kr 월말 무수정 종가 / 패널 수정 종가 = 수정 비율 f. 날짜는 같은 달의 월말 비율을 쓴다."""
        frames = [pd.read_parquet(path, columns=["ticker", "close", "date"])
                  for path in sorted(DATAGOKR_DIR.glob("univ_*.parquet"))]
        raw = pd.concat(frames, ignore_index=True)
        raw = raw[raw["ticker"].isin(set(self.codes))]
        months = sorted(raw["date"].dt.to_period("M").unique())
        month_position = {month: index for index, month in enumerate(months)}
        code_position = {code: index for index, code in enumerate(self.codes)}
        monthly = np.full((len(months), len(self.codes)), np.nan)

        for record_date, group in raw.groupby("date"):
            day_index = self.calendar.searchsorted(record_date, side="right") - 1

            if day_index < 0:
                continue

            columns = group["ticker"].map(code_position).to_numpy()
            adjusted = self.close_filled[day_index, columns]
            with np.errstate(divide="ignore", invalid="ignore"):
                ratio = group["close"].to_numpy(dtype=float) / adjusted
            ratio = np.where(np.abs(ratio - 1.0) < FACTOR_TOLERANCE, 1.0, ratio)
            monthly[month_position[record_date.to_period("M")], columns] = ratio

        monthly_frame = pd.DataFrame(monthly).ffill().bfill().fillna(1.0)
        monthly = monthly_frame.to_numpy()
        day_months = self.calendar.to_period("M")
        first_month, last_month = months[0], months[-1]
        row_of_day = np.array([month_position[min(max(month, first_month), last_month)] for month in day_months])
        factor = monthly[row_of_day]
        changed = (np.abs(monthly - 1.0) >= FACTOR_TOLERANCE).any(axis=0)
        report = {
            "months": f"{first_month}~{last_month} ({len(months)}개월)",
            "codes_with_factor_not_1": int(changed.sum()),
            "examples": [
                {"code": str(self.codes[index]), "factor_first": round(float(monthly[0, index]), 4),
                 "factor_last": round(float(monthly[-1, index]), 4)}
                for index in np.where(changed)[0][:12]
            ],
        }
        return factor, report


# ── 신호 표 ──────────────────────────────────────────────────────────────────

class SignalTable:
    """월말마다 유니버스·점수를 미리 계산한다. 모든 값은 행 month_end_index 이하만 본다."""

    def __init__(self, panel: Panel, kospi_close: pd.Series):
        self.panel = panel
        calendar = panel.calendar
        month_end = pd.Series(calendar, index=calendar).groupby(calendar.to_period("M")).max()
        month_end = month_end[(month_end >= FIRST_SIGNAL) & (month_end <= LAST_FULL_MONTH)]
        self.signal_index = np.array([panel.day_position[day] for day in month_end])
        self.execution_index = self.signal_index + 1
        self.signal_dates = calendar[self.signal_index]
        self.execution_dates = calendar[self.execution_index]
        self.end_index = np.append(self.execution_index[1:], len(calendar) - 1)
        self.rows = []

        for signal_row in self.signal_index:
            self.rows.append(self.month(signal_row, kospi_close))

    def month(self, row: int, kospi_close: pd.Series) -> dict:
        panel = self.panel
        day = np.datetime64(panel.calendar[row])
        listed = (panel.effective_from <= day) & (day <= panel.effective_to)
        start = max(row - LISTED_DAYS_MIN + 1, 0)
        # 상장 뒤 260거래일: membership 첫 거래일이 260번째 앞 거래일보다 앞이어야 한다
        aged = panel.effective_from <= np.datetime64(panel.calendar[start]) if row >= LISTED_DAYS_MIN - 1 else np.zeros(len(panel.codes), bool)
        raw_price = panel.close[row] * panel.factor[row]
        value_start = max(row + 1 - VALUE_WINDOW, 0)
        value_60 = (panel.value_cumulative[row + 1] - panel.value_cumulative[value_start]) / VALUE_WINDOW
        zero_start = max(row + 1 - ZERO_VOLUME_LOOKBACK, 0)
        zero_days = panel.zero_cumulative[row + 1] - panel.zero_cumulative[zero_start]
        outlier_start = max(row - HIGH_WINDOW, 0)
        outliers = panel.outlier_cumulative[row] - panel.outlier_cumulative[outlier_start]
        core = listed & aged & panel.common & panel.exists[row] & (zero_days == 0)
        base = core & (raw_price >= PRICE_MIN_KRW)
        high_252 = np.nanmax(panel.high[max(row - HIGH_WINDOW + 1, 0): row + 1], axis=0)
        with np.errstate(divide="ignore", invalid="ignore"):
            pth = panel.close[row] / high_252
            past_12 = panel.close_filled[row - 252] if row >= 252 else np.full(len(panel.codes), np.nan)
            mom_12_1 = panel.close_filled[row - 21] / past_12 - 1.0
            past_120 = panel.close_filled[row - 120]
            recent_20 = panel.close_filled[row - 20]
            segment = panel.daily_return[row - 119: row - 19]
            volatility = np.nanstd(segment, axis=0, ddof=1)
            mom_live = (recent_20 / past_120 - 1.0) / volatility
            moving_200 = np.nanmean(panel.close_filled[row - 199: row + 1], axis=0)
        above_200 = panel.close_filled[row] > moving_200
        kospi_until = kospi_close.loc[:panel.calendar[row]]
        kospi_on = bool(kospi_until.iloc[-1] > kospi_until.tail(200).mean()) if len(kospi_until) >= 200 else True
        return {
            "core": core, "base": base, "outliers": outliers, "value_60": value_60, "pth": pth, "mom_12_1": mom_12_1,
            "mom_live": mom_live, "above_200": above_200, "kospi_on": kospi_on,
            "raw_price": raw_price, "adjusted_price": panel.close[row],
        }

    def universe(self, month: int, floor_krw: int, signal: str) -> np.ndarray:
        row = self.rows[month]
        mask = row["base"] & (row["value_60"] >= floor_krw) & (row["outliers"] == 0)
        score = self.score(month, signal)
        return mask & np.isfinite(score)

    def score(self, month: int, signal: str) -> np.ndarray:
        row = self.rows[month]

        if signal in ("PTH", "PTH_AND_MOM"):
            return row["pth"]

        if signal == "MOM12_1":
            return row["mom_12_1"]

        if signal == "MOM_LIVE":
            return row["mom_live"]

        raise ValueError(signal)


# ── 포트폴리오 루프 ──────────────────────────────────────────────────────────

class Backtester:
    def __init__(self, panel: Panel, signals: SignalTable, halt_days: set):
        self.panel = panel
        self.signals = signals
        self.halt_days = halt_days

    def price(self, day_index: int, execution: str) -> np.ndarray:
        if execution == "open":
            open_price = self.panel.open[day_index]
            return np.where(np.isfinite(open_price), open_price, self.panel.close_filled[day_index])

        return self.panel.close_filled[day_index]

    def ranked(self, month: int, config: RunConfig) -> tuple[np.ndarray, np.ndarray]:
        """유니버스 안 종목을 점수 내림차순(같으면 60일 거래대금 큰 순)으로. 돌려주는 값: 순서 배열, 유니버스 마스크."""
        signals = self.signals
        mask = signals.universe(month, config.floor_krw, config.signal)
        score = signals.score(month, config.signal)
        value = signals.rows[month]["value_60"]
        candidates = np.where(mask)[0]

        if config.signal == "PTH_AND_MOM":
            # 제거실험 ②: PTH 상위 20% ∩ 12-1 모멘텀 상위 20%, 그 안에서 PTH 순
            momentum = signals.rows[month]["mom_12_1"][candidates]
            pth = score[candidates]
            valid = np.isfinite(momentum)
            momentum_cut = np.nanquantile(momentum[valid], 0.8) if valid.any() else np.inf
            pth_cut = np.nanquantile(pth, 0.8)
            keep = valid & (momentum >= momentum_cut) & (pth >= pth_cut)
            candidates = candidates[keep]

        order = np.lexsort((-value[candidates], -score[candidates]))
        return candidates[order], mask

    def run(self, config: RunConfig) -> pd.DataFrame:
        panel, signals = self.panel, self.signals
        n_codes = len(panel.codes)
        drifted = np.zeros(n_codes)
        bench_drifted = np.zeros(n_codes)
        records = []

        for month in range(len(signals.signal_index)):
            start = signals.execution_index[month]
            end = signals.end_index[month]
            start_day = panel.calendar[start]
            order, mask = self.ranked(month, config)
            tradable = panel.exists[start] & (np.nan_to_num(panel.volume[start]) > 0)
            # 집행일에 거래가 없는(정지·결측) 보유 종목은 팔 수 없다 — 다시 거래되거나 상장폐지될 때까지 그 비중 그대로 묶인다
            frozen = (drifted > 0) & ~tradable
            row = signals.rows[month]
            regime_off = False

            if config.kospi_filter and not row["kospi_on"]:
                regime_off = True

            if config.breadth_regime:
                breadth = row["above_200"][mask].mean() if mask.any() else 1.0
                regime_off = regime_off or breadth < 0.5

            target = np.zeros(n_codes, bool)

            if not regime_off:
                limit = int(np.floor(RETENTION_MULTIPLE * config.top_n))

                if config.retention:
                    kept = order[:limit][(drifted[order[:limit]] > 0) & tradable[order[:limit]]]
                    target[kept] = True

                for code_index in order:
                    if target.sum() >= config.top_n:
                        break

                    if not target[code_index] and tradable[code_index]:
                        target[code_index] = True

            names_new = int((target & (drifted <= 0)).sum())
            weights = allocate(target, drifted, tradable)
            buy_amount_by_code = np.maximum(weights - drifted, 0.0)
            sell_amount = float(np.maximum(drifted - weights, 0.0).sum())
            frozen_count = int(frozen.sum())

            start_price = self.price(start, config.execution)
            end_price = self.price(end, config.execution)
            is_last = month == len(signals.signal_index) - 1

            if is_last and config.execution == "open":
                end_price = panel.close_filled[end]

            # 국면 정지: 집행일 종가 점수 ≤ −7이면 매수 레그만 다음 정상일 종가로 밀린다(매도는 그날 나간다)
            buy_start = np.full(n_codes, start)
            halted = config.halt and start_day in self.halt_days
            buy_executed = np.ones(n_codes, bool)

            if halted:
                resume = next((index for index in range(start + 1, end)
                               if panel.calendar[index] not in self.halt_days), None)

                if resume is None:
                    buy_executed[:] = False

                else:
                    buy_start[:] = resume

            buy_amount_by_code = np.where(buy_executed, buy_amount_by_code, 0.0)
            buy_amount = float(buy_amount_by_code.sum())
            cost_start = buy_amount * config.costs.buy_rate(start_day) + sell_amount * config.costs.sell_rate(start_day)

            def stock_returns(weight_vector: np.ndarray, begin_index, begin_price: np.ndarray):
                held = np.where(weight_vector > 0)[0]
                returns = np.zeros(n_codes)
                delisted = np.zeros(n_codes, bool)
                first = begin_index if np.isscalar(begin_index) else np.asarray(begin_index)[held]
                last_bar = panel.last_bar_index[held]
                ends_here = panel.terminates[held] & (first <= last_bar) & (last_bar < end)
                last_close = panel.close[np.maximum(last_bar, 0), held] * (1.0 - config.delist_haircut)
                exit_price = np.where(ends_here, last_close, end_price[held])
                entry = begin_price[held]
                with np.errstate(divide="ignore", invalid="ignore"):
                    returns[held] = np.where(entry > 0, exit_price / entry - 1.0, 0.0)
                delisted[held] = ends_here
                return returns, delisted

            keep_weights = np.minimum(weights, drifted)
            keep_returns, keep_delisted = stock_returns(keep_weights, start, start_price)

            if halted:
                add_begin_price = panel.close_filled[buy_start[0]] if buy_executed.any() else start_price
                add_returns, add_delisted = stock_returns(buy_amount_by_code, buy_start, add_begin_price)

            else:
                add_returns, add_delisted = stock_returns(buy_amount_by_code, start, start_price)

            position = keep_weights + buy_amount_by_code
            contribution = keep_weights * keep_returns + buy_amount_by_code * add_returns
            end_value_by_code = keep_weights * (1 + keep_returns) + buy_amount_by_code * (1 + add_returns)
            delisted = keep_delisted | add_delisted
            delist_value = float(end_value_by_code[delisted].sum())
            delist_cost = delist_value * config.costs.sell_rate(panel.calendar[end])
            cash = 1.0 - position.sum()
            gross = float(contribution.sum())
            end_value = float(end_value_by_code.sum()) + cash - cost_start - delist_cost
            net = end_value - 1.0

            window_outliers = panel.outlier_cumulative[end] - panel.outlier_cumulative[start]
            outlier_contribution = float(contribution[(window_outliers > 0) & (position > 0)].sum())

            drifted = np.where(delisted, 0.0, end_value_by_code) / end_value
            drifted = np.where(drifted > 0, drifted, 0.0)

            # 비교 기준: 같은 유니버스 전 종목 동일가중, 비용 없음, 같은 상장폐지 처리
            bench_weights = allocate(mask & tradable, bench_drifted, tradable)
            bench_returns, bench_delisted = stock_returns(bench_weights, start, start_price)
            bench = float((bench_weights * bench_returns).sum())
            bench_end = bench_weights * (1 + bench_returns)
            bench_drifted = np.where(bench_delisted, 0.0, bench_end) / (1.0 + bench)

            records.append({
                "period": start_day.strftime("%Y-%m"),
                "signal_date": signals.signal_dates[month].strftime("%Y-%m-%d"),
                "start": start_day.strftime("%Y-%m-%d"),
                "end": panel.calendar[end].strftime("%Y-%m-%d"),
                "partial": bool(is_last),
                "gross": gross, "net": net, "bench": bench, "excess": net - bench,
                "buy": buy_amount, "sell": sell_amount, "turnover": 0.5 * (buy_amount + sell_amount),
                "cost": cost_start + delist_cost,
                "n_held": int((weights > 0).sum()), "n_universe": int(mask.sum()),
                "n_delisted": int(delisted.sum()), "bench_delisted": int(bench_delisted.sum()),
                "names_new": names_new, "frozen": frozen_count,
                "outlier_contribution": outlier_contribution, "halted": bool(halted), "regime_off": bool(regime_off),
            })

        return pd.DataFrame(records)


def allocate(target: np.ndarray, drifted: np.ndarray, tradable: np.ndarray) -> np.ndarray:
    """목표 종목 동일가중. 거래 못 하는 보유분(frozen)은 지난 비중 그대로 두고 남은 돈만 나눈다."""
    frozen = (drifted > 0) & ~tradable
    weights = np.where(frozen, drifted, 0.0)
    buyable = target & ~frozen
    remaining = max(1.0 - float(weights.sum()), 0.0)

    if buyable.any():
        weights = np.where(buyable, remaining / buyable.sum(), weights)

    return weights


# ── 성과 ────────────────────────────────────────────────────────────────────

def newey_west_t(values: np.ndarray, lags: int = 3) -> float:
    values = np.asarray(values, dtype=float)
    count = len(values)

    if count < lags + 2:
        return float("nan")

    demeaned = values - values.mean()
    variance = demeaned @ demeaned / count

    for lag in range(1, lags + 1):
        weight = 1.0 - lag / (lags + 1)
        variance += 2 * weight * (demeaned[lag:] @ demeaned[:-lag]) / count

    return float(values.mean() / np.sqrt(variance / count))


def max_drawdown(returns: np.ndarray) -> float:
    equity = np.cumprod(1.0 + np.asarray(returns))
    peak = np.maximum.accumulate(np.concatenate([[1.0], equity]))[1:]
    return float((equity / peak - 1.0).min())


def summarize(frame: pd.DataFrame) -> dict:
    months = len(frame)
    years = months / 12.0
    net_total = float(np.prod(1 + frame["net"]) - 1)
    bench_total = float(np.prod(1 + frame["bench"]) - 1)
    return {
        "months": months,
        "ann_excess_arith_pct": round(float(frame["excess"].mean() * 12 * 100), 3),
        "cagr_net_pct": round(((1 + net_total) ** (1 / years) - 1) * 100, 3),
        "cagr_bench_pct": round(((1 + bench_total) ** (1 / years) - 1) * 100, 3),
        "cagr_gross_pct": round((float(np.prod(1 + frame["gross"])) ** (1 / years) - 1) * 100, 3),
        "nw_t_excess": round(newey_west_t(frame["excess"].to_numpy()), 3),
        "mdd_net_pct": round(max_drawdown(frame["net"].to_numpy()) * 100, 2),
        "mdd_bench_pct": round(max_drawdown(frame["bench"].to_numpy()) * 100, 2),
        "turnover_mean_pct": round(float(frame["turnover"].mean() * 100), 2),
        "names_replaced_mean_pct": round(float((frame["names_new"] / frame["n_held"].clip(lower=1)).mean() * 100), 2),
        "cost_ann_pct": round(float(frame["cost"].mean() * 12 * 100), 3),
        "held_mean": round(float(frame["n_held"].mean()), 1),
        "universe_mean": round(float(frame["n_universe"].mean()), 1),
        "delisted_exits": int(frame["n_delisted"].sum()),
        "bench_delisted_exits": int(frame["bench_delisted"].sum()),
        "frozen_position_months": int(frame["frozen"].sum()),
        "outlier_contribution_ann_pct": round(float(frame["outlier_contribution"].mean() * 12 * 100), 3),
    }


def year_of(frame: pd.DataFrame) -> pd.Series:
    return frame["period"].str[:4].astype(int)


def walk_forward(cells: dict, extra: dict | None = None) -> tuple[pd.DataFrame, pd.DataFrame]:
    """각 검증 연도마다 직전 3년 순초과(월 평균)가 가장 큰 칸을 골라 그 해 월 수익을 이어 붙인다.
    extra: 칸 → 다른 실행(같은 칸으로 골라 이어 붙일 비교용, 예: 모멘텀)."""
    windows, pieces = [], []

    for year in VALIDATION_YEARS:
        train_scores = {}

        for cell, frame in cells.items():
            years = year_of(frame)
            train = frame[(years >= year - TRAIN_YEARS) & (years < year)]
            train_scores[cell] = train["excess"].mean() * 12

        chosen = max(sorted(train_scores), key=lambda cell: train_scores[cell])
        frame = cells[chosen]
        valid = frame[year_of(frame) == year].copy()
        valid["cell"] = f"N{chosen[0]}/F{chosen[1] // 100_000_000}"

        if extra:
            for name, runs in extra.items():
                other = runs[chosen]
                other_valid = other[year_of(other) == year]
                valid[f"{name}_net"] = other_valid["net"].to_numpy()
                valid[f"{name}_excess"] = other_valid["excess"].to_numpy()

        pieces.append(valid)
        windows.append({
            "year": year, "months": len(valid), "cell": valid["cell"].iloc[0],
            "train_ann_excess_pct": round(train_scores[chosen] * 100, 3),
            "net_pct": round((np.prod(1 + valid["net"]) - 1) * 100, 3),
            "bench_pct": round((np.prod(1 + valid["bench"]) - 1) * 100, 3),
            "excess_compound_pct": round(((np.prod(1 + valid["net"]) - np.prod(1 + valid["bench"]))) * 100, 3),
            "excess_sum_pct": round(valid["excess"].sum() * 100, 3),
            "turnover_mean_pct": round(valid["turnover"].mean() * 100, 2),
        })

    return pd.DataFrame(windows), pd.concat(pieces, ignore_index=True)


def validation_slice(frame: pd.DataFrame) -> pd.DataFrame:
    return frame[year_of(frame) >= VALIDATION_YEARS[0]]


# ── 데이터 대조(SPEC §3.2·§3.5 첫 단계) ──────────────────────────────────────

def data_checks(panel: Panel) -> dict:
    kind = pd.read_parquet(KIND_PATH)
    kind["year"] = kind["delist_date"].dt.year
    membership = panel.membership.reset_index()
    ended = membership[membership["end_reason"].isin(["delisted", "merged"])].copy()
    ended["year"] = ended["effective_to"].dt.year
    common_kind = kind[kind["code"].fillna("").str.endswith("0")]
    by_year = []

    for year in range(2005, 2027):
        kind_year = common_kind[common_kind["year"] == year]
        by_year.append({
            "year": year,
            "kind_delisted_common": int(len(kind_year)),
            "kind_with_bars": int(kind_year["bars_last_date"].notna().sum()),
            "panel_ended": int((ended["year"] == year).sum()),
        })

    terminal = ended.dropna(subset=["terminal_price"])
    mismatch = 0
    code_position = {code: index for index, code in enumerate(panel.codes)}

    for _, row in terminal.iterrows():
        index = code_position[row["code"]]
        last = panel.last_bar_index[index]

        if last >= 0 and abs(panel.close[last, index] - row["terminal_price"]) > 0.5:
            mismatch += 1

    split_checks = []

    for code, label in (("005930", "삼성전자 2018-05 50:1 분할"), ("035420", "NAVER 2018-10 5:1 분할"),
                        ("035720", "카카오 2021-04 5:1 분할"), ("000660", "SK하이닉스")):
        index = code_position.get(code)

        if index is None:
            continue

        returns = panel.daily_return[:, index]
        worst = int(np.nanargmax(np.abs(np.nan_to_num(returns))))
        split_checks.append({"code": code, "note": label,
                             "max_abs_daily_return_pct": round(float(abs(returns[worst])) * 100, 2),
                             "on": panel.calendar[worst].strftime("%Y-%m-%d"),
                             "factor_2020_01": round(float(panel.factor[panel.calendar.searchsorted(pd.Timestamp("2020-01-31")), index]), 4),
                             "factor_2026_08": round(float(panel.factor[panel.calendar.searchsorted(pd.Timestamp("2026-08-31")), index]), 4)})

    since_2010 = panel.calendar >= pd.Timestamp("2010-01-01")
    return {
        "dropped_rows_off_calendar": panel.dropped_rows,
        "delisting_by_year": by_year,
        "terminal_price_mismatch": {"checked": int(len(terminal)), "mismatch": mismatch},
        "outlier_days_since_2010": int(panel.outlier_day[since_2010].sum()),
        "split_checks": split_checks,
        "factor": panel.factor_report,
    }


def factor_effect(panel: Panel, signals: SignalTable) -> dict:
    """2020 이후 월말에서 f를 끄면 유니버스가 얼마나 달라지나 — 2019년 이전에 못 되돌린 분할·병합의 크기 추정용."""
    rows = []

    for month, signal_row in enumerate(signals.signal_index):
        if signals.signal_dates[month] < pd.Timestamp("2020-01-01"):
            continue

        row = signals.rows[month]
        start = max(signal_row + 1 - VALUE_WINDOW, 0)
        raw_value = np.nan_to_num(panel.close[start: signal_row + 1] * panel.volume[start: signal_row + 1]).sum(axis=0) / VALUE_WINDOW
        clean = (row["outliers"] == 0) & np.isfinite(row["pth"])

        for floor in GRID_FLOOR_KRW:
            with_factor = row["base"] & (row["value_60"] >= floor) & clean
            without = (row["core"] & (np.nan_to_num(row["adjusted_price"]) >= PRICE_MIN_KRW)
                       & (raw_value >= floor) & clean)
            rows.append({"floor": floor, "with": int(with_factor.sum()),
                         "symmetric_difference": int((with_factor ^ without).sum())})

    frame = pd.DataFrame(rows)
    return {f"F{floor // 100_000_000}": {"mean_universe": round(float(group["with"].mean()), 1),
                                          "mean_changed": round(float(group["symmetric_difference"].mean()), 1)}
            for floor, group in frame.groupby("floor")}


# ── 실행 ────────────────────────────────────────────────────────────────────

def load_kospi(panel: Panel) -> pd.Series:
    kospi = pd.read_parquet(KOSPI_PATH)
    series = pd.Series(kospi["close"].to_numpy(), index=pd.to_datetime(kospi["date"]))
    regime = pd.read_csv(REGIME_PATH, sep="\t", usecols=["date", "kospi_close"], parse_dates=["date"])
    tail = regime[regime["date"] > series.index.max()].set_index("date")["kospi_close"]
    return pd.concat([series, tail]).sort_index()


def load_halt_days() -> set:
    regime = pd.read_csv(REGIME_PATH, sep="\t", usecols=["date", "score_close"], parse_dates=["date"])
    return set(regime.loc[regime["score_close"] <= HALT_SCORE, "date"])


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()

    except Exception:
        return "unknown"


def write_tsv(frame: pd.DataFrame, path: Path) -> None:
    path.write_bytes(frame.to_csv(sep="\t", index=False, float_format="%.6f", lineterminator="\n").encode("utf-8"))


def main() -> None:
    panel = Panel()
    kospi = load_kospi(panel)
    signals = SignalTable(panel, kospi)
    halt_days = load_halt_days()
    tester = Backtester(panel, signals, halt_days)
    cells = [(top_n, floor) for top_n in GRID_N for floor in GRID_FLOOR_KRW]
    runs: dict[str, pd.DataFrame] = {}

    def run_grid(**overrides) -> dict:
        result = {}

        for top_n, floor in cells:
            config = RunConfig(top_n=top_n, floor_krw=floor, **overrides)
            frame = tester.run(config)
            runs[config.key()] = frame
            result[(top_n, floor)] = frame

        return result

    main_cells = run_grid()
    momentum_cells = run_grid(signal="MOM12_1")
    momentum_live = run_grid(signal="MOM_LIVE", retention=False)
    momentum_live_on = run_grid(signal="MOM_LIVE", retention=False, breadth_regime=True)

    variant_grids = {
        "exec_open": run_grid(execution="open"),
        "delist_-30%": run_grid(delist_haircut=0.30),
        "halt_on(2015+)": run_grid(halt=True),
        "costs_live_const": run_grid(costs=COSTS_LIVE),
        "costs_hist_tax": run_grid(costs=COSTS_HISTORICAL),
        "abl1_MOM12_1": momentum_cells,
        "abl2_PTH_and_MOM": run_grid(signal="PTH_AND_MOM"),
        "abl3_kospi_200ma": run_grid(kospi_filter=True),
    }

    windows, stitched = walk_forward(main_cells, extra={"mom12_1": momentum_cells, "mom_live": momentum_live,
                                                         "mom_live_on": momentum_live_on})
    complete = windows[windows["year"] <= 2025]
    wf_summary = summarize(stitched)
    correlations = {
        "excess_vs_mom12_1": round(float(stitched["excess"].corr(stitched["mom12_1_excess"])), 3),
        "excess_vs_mom_live": round(float(stitched["excess"].corr(stitched["mom_live_excess"])), 3),
        "excess_vs_mom_live_regime_on": round(float(stitched["excess"].corr(stitched["mom_live_on_excess"])), 3),
        "net_vs_mom12_1_net": round(float(stitched["net"].corr(stitched["mom12_1_net"])), 3),
        "net_vs_mom_live_net": round(float(stitched["net"].corr(stitched["mom_live_net"])), 3),
        "net_vs_mom_live_on_net": round(float(stitched["net"].corr(stitched["mom_live_on_net"])), 3),
    }
    momentum_wf_ann_excess = round(float(stitched["mom12_1_excess"].mean() * 12 * 100), 3)
    momentum_live_wf_ann_excess = round(float(stitched["mom_live_excess"].mean() * 12 * 100), 3)

    grid_rows = []

    for (top_n, floor), frame in main_cells.items():
        summary = summarize(validation_slice(frame))
        grid_rows.append({"cell": f"N{top_n}/F{floor // 100_000_000}", **summary})

    grid = pd.DataFrame(grid_rows)
    base_label = f"N{BASE_CELL[0]}/F{BASE_CELL[1] // 100_000_000}"
    neighbors = ["N20/F10", "N50/F10", "N30/F30"]
    neighbor_positive = int((grid.set_index("cell").loc[neighbors, "ann_excess_arith_pct"] > 0).sum())

    variant_rows = []

    for name, grid_runs in [("main", main_cells)] + list(variant_grids.items()):
        variant_windows, variant_stitched = walk_forward(grid_runs)
        summary = summarize(variant_stitched)
        base_summary = summarize(validation_slice(grid_runs[BASE_CELL]))
        variant_rows.append({
            "variant": name,
            "wf_ann_excess_pct": summary["ann_excess_arith_pct"], "wf_nw_t": summary["nw_t_excess"],
            "wf_cagr_net_pct": summary["cagr_net_pct"], "wf_cagr_bench_pct": summary["cagr_bench_pct"],
            "wf_mdd_net_pct": summary["mdd_net_pct"], "wf_mdd_bench_pct": summary["mdd_bench_pct"],
            "wf_windows_positive_of_16": int((variant_windows[variant_windows["year"] <= 2025]["excess_compound_pct"] > 0).sum()),
            "wf_turnover_pct": summary["turnover_mean_pct"],
            "base_cell_ann_excess_pct": base_summary["ann_excess_arith_pct"], "base_cell_nw_t": base_summary["nw_t_excess"],
        })

    variants = pd.DataFrame(variant_rows)

    gates = {
        "1_excess_ann_ge_3pp": {"value": wf_summary["ann_excess_arith_pct"], "cagr_diff_pct": round(wf_summary["cagr_net_pct"] - wf_summary["cagr_bench_pct"], 3),
                                "pass": wf_summary["ann_excess_arith_pct"] >= 3.0},
        "2_nw_t_ge_2": {"value": wf_summary["nw_t_excess"], "pass": wf_summary["nw_t_excess"] >= 2.0},
        "3_windows_ge_10_of_16": {"value": int((complete["excess_compound_pct"] > 0).sum()), "pass": int((complete["excess_compound_pct"] > 0).sum()) >= 10},
        "4_neighbors_ge_2_of_3": {"value": neighbor_positive, "neighbors": neighbors, "pass": neighbor_positive >= 2},
        "5_mdd_le_bench_plus_5pp": {"value": wf_summary["mdd_net_pct"], "bench": wf_summary["mdd_bench_pct"],
                                    "pass": wf_summary["mdd_net_pct"] >= wf_summary["mdd_bench_pct"] - 5.0},
        "6_corr_mom12_1_lt_0.7": {"value": correlations["excess_vs_mom12_1"], "pass": correlations["excess_vs_mom12_1"] < 0.7,
                                  "b_minus_mom_ann_excess_pp": round(wf_summary["ann_excess_arith_pct"] - momentum_wf_ann_excess, 3)},
        "7_turnover_le_40pct": {"value": wf_summary["turnover_mean_pct"], "pass": wf_summary["turnover_mean_pct"] <= 40.0},
    }

    checks = data_checks(panel)
    checks["factor_effect_2020_plus"] = factor_effect(panel, signals)
    checks["halt_days_score_close_le_-7"] = len(halt_days)

    period_frames = []

    for key, frame in runs.items():
        tagged = frame.copy()
        tagged.insert(0, "run", key)
        period_frames.append(tagged)

    write_tsv(pd.concat(period_frames, ignore_index=True), STUDY / "b_periods.tsv")
    write_tsv(windows, STUDY / "b_walkforward.tsv")
    write_tsv(grid, STUDY / "b_grid.tsv")
    write_tsv(variants, STUDY / "b_variants.tsv")
    (STUDY / "b_checks.json").write_bytes(json.dumps(checks, ensure_ascii=False, indent=1, default=str).encode("utf-8"))

    metrics = {
        "study": "30_strong_stock_strategies / B 52주 신고가 근접",
        "spec": "research/studies/30_strong_stock_strategies/SPEC.md §3 (사전등록, 바꾸지 않음)",
        "seed": 0,
        "period": {"signals": f"{signals.signal_dates[0]:%Y-%m-%d}~{signals.signal_dates[-1]:%Y-%m-%d}",
                   "validation": f"{VALIDATION_YEARS[0]}-01~{stitched['end'].iloc[-1]}", "panel_last": f"{panel.calendar[-1]:%Y-%m-%d}"},
        "costs": {"main": COSTS_SPEC.__dict__, "buy_side_pct": round(COSTS_SPEC.buy_rate(pd.Timestamp("2026-01-01")) * 100, 4),
                  "sell_side_pct": round(COSTS_SPEC.sell_rate(pd.Timestamp("2026-01-01")) * 100, 4)},
        "walk_forward": wf_summary,
        "correlations": correlations,
        "momentum_wf_ann_excess_pct": {"mom12_1": momentum_wf_ann_excess, "mom_live": momentum_live_wf_ann_excess},
        "gates": gates,
        "gates_passed": int(sum(gate["pass"] for gate in gates.values())),
        "base_cell": {"cell": base_label, **summarize(validation_slice(main_cells[BASE_CELL]))},
        "inputs": [file_digest(path) for path in (BARS_PATH, MEMBERSHIP_PATH, KIND_PATH, KOSPI_PATH, REGIME_PATH)],
        "datagokr_files": len(list(DATAGOKR_DIR.glob("univ_*.parquet"))),
        "git_commit": git_commit(),
        "rerun": "py -X utf8 research/studies/30_strong_stock_strategies/backtest_b.py",
    }
    (STUDY / "b_metrics.json").write_bytes(json.dumps(metrics, ensure_ascii=False, indent=1, default=str).encode("utf-8"))

    print(windows.to_string(index=False))
    print(grid[["cell", "ann_excess_arith_pct", "nw_t_excess", "cagr_net_pct", "cagr_bench_pct", "mdd_net_pct",
                "mdd_bench_pct", "turnover_mean_pct"]].to_string(index=False))
    print(variants.to_string(index=False))
    print(json.dumps({"walk_forward": wf_summary, "correlations": correlations, "gates": gates}, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
