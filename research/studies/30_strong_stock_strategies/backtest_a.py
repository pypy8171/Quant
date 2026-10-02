#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""스터디 30 전략 A(강한 종목 첫 VWAP 눌림) 백테스트 — research/studies/30_strong_stock_strategies/SPEC.md §2를 그대로 옮긴다.

사전등록(SPEC §2)에서 바꾸지 않은 것: 09:30 유니버스(§2.2), 상태 기계(§2.3), 청산(§2.4), 사이징(§2.5), 대조군 C1·C2·C3(§2.6),
격자 기본 칸과 이웃 6칸(§2.7), 비용(§2.10 개정판: 수수료 0.015%/편 + 매도세 0.20%, 슬리피지는 체결 규칙의 틱으로만).

시점(look-ahead 차단):
  - 유니버스는 open30/<날짜>.json의 snapshots(09:00~09:29 봉, hms < 093000의 마지막 종가와 Σ종가×거래량)만 쓴다.
    일봉 고가는 "무엇을 받아 둘지"에만 쓰였고 여기서는 읽지 않는다.
  - 상태 기계는 봉을 한 개씩 시간순으로 받는다(PullbackMachine.on_bar). 봉 k 종가로 신호가 나면 체결은 봉 k+1 시가 기준이다.
  - 진입 봉부터 손절을 검사한다(진입 봉 저가 ≤ S면 손절 — 보수적).
  - 홀드아웃: 검증 구간(2026-04-01~2026-09-29)은 --open-holdout 없이는 파일을 열지도 않는다.
가격: KIS 분봉은 수정주가라 호가 단위는 무수정 가격으로 되돌려 정한다. 되돌리는 비율 f = data.go.kr 월말 무수정 종가 / 네이버
  수정 종가(그 날짜 바로 앞 월말 값, 차이 2% 미만이면 1). 2026-09 분할·병합은 8월 말 비율을 써서 못 잡는다.

C++ 그림자 구현과의 대응: PullbackMachine은 ../Quant-wt-vwappb/Quant/src/strategy/VwapPullbackRules.cpp의
  PullbackMachine::on_bar와 같은 순서·같은 조건이다. 다른 점은 둘 — (1) 손절 폭 판정을 C++는 지정가(종가 + 2틱)로, 여기서는
  SPEC대로 실제 체결가(다음 봉 시가 + 1틱)로 한다. (2) VI 근사의 "거래량 0 봉 2개 연속"에 빈 분(봉이 아예 없는 분)도 센다
  (count_missing_minutes, 기본 켬). 끈 판(C++와 같음)은 감도 vi_gap_off로 낸다.

재실행(저장소 루트):
  py -X utf8 research/studies/30_strong_stock_strategies/backtest_a.py                  # 개발 구간만
  py -X utf8 research/studies/30_strong_stock_strategies/backtest_a.py --open-holdout   # 검증 구간까지(한 번만 연다)
seed: C3 무작위 시각은 numpy default_rng(0). 같은 입력 파일이면 산출 파일 바이트가 같다 — 입력 해시는 a_metrics.json "inputs".
산출(같은 폴더): a_candidates.tsv(기본 칸 후보 종목일별 상태 기계 결과), a_trades.tsv(실행별 체결 거래와 같은 종목일 대조군),
  a_grid.tsv(기본 칸과 이웃 6칸), a_variants.tsv(감도·제거실험), a_metrics.json(수치와 재현 정보). 합격 판정은 쓰지 않는다.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import re
import subprocess
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(REPO / "PYQuant"))
sys.path.insert(0, str(STUDY))
from backtest.costs import LIVE, fill_result, tick_size   # noqa: E402  비용·호가 단위는 라이브 원장과 한 소스
from backtest_b import is_common_stock, load_name_rules   # noqa: E402  ETF·스팩·우선주·리츠 제외는 B와 같은 규칙

sys.stdout.reconfigure(encoding="utf-8")
SEED = 0

DATA = STUDY / "data"
OPEN30_DIR = DATA / "open30"
STUDY_MINUTE = DATA / "minute"
SHARED_MINUTE = REPO / "PYQuant" / "data" / "minute"          # PYQuant/tools/minute_strong_open_fetch.py SHARED_MINUTE와 같은 경로
PAIRS_PATH = DATA / "pairs_all.json"
FETCH_LOG = DATA / "fetch.log"
BARS_PATH = REPO / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
MEMBERSHIP_PATH = REPO / "PYQuant" / "data" / "universe" / "membership.parquet"
DATAGOKR_DIR = REPO / "PYQuant" / "data" / "cache" / "datagokr_shares"
REGIME_PATH = REPO / "research" / "studies" / "26_regime_threshold_history" / "daily_scores.tsv"   # 읽기만 한다

# SPEC §2.7 기간
DEVELOPMENT = ("20250918", "20260331")
HOLDOUT = ("20260401", "20260929")

PERCENT_TOLERANCE = 1e-9   # C++ kPercentTolerance와 같은 값
PRICE_EPSILON = 1e-6
MINIMUM_GAP_FOR_BAD_FILE = 10   # 15:20 전 봉 사이가 10분 이상 비면 30분봉 등 다른 형식으로 보고 그 종목일을 뺀다
BASIS_MISMATCH_TOLERANCE = 0.005
C3_DRAWS = 200
HALT_SCORE = -7


@dataclass(frozen=True)
class RuleParams:
    """C++ vwap_pullback::RuleParams와 같은 이름·같은 기본값(SPEC 확정값)."""
    change_min_percent: float = 6.0
    change_max_percent: float = 20.0
    turnover_top_n: int = 10
    min_previous_close_krw: float = 2_000.0
    min_turnover_krw: float = 3e9
    first_arm_hhmm: int = 931
    no_new_entry_hhmm: int = 1315
    retrace_min: float = 0.38
    retrace_max: float = 0.62
    vwap_band_percent: float = 0.5
    max_run_percent: float = 25.0
    disarm_below_vwap_percent: float = 1.0
    disarm_retrace: float = 0.75
    arm_timeout_bars: int = 15
    entry_ticks: int = 2
    vi_jump_percent: float = 6.0
    vi_zero_volume_bars: int = 2
    stop_below_low_percent: float = 0.3
    min_stop_width_percent: float = 0.6
    max_stop_width_percent: float = 3.0
    exit_hhmm: int = 1510


@dataclass(frozen=True)
class Profile:
    """SPEC §2.5 계좌별 사이징과 §2.3-6 하루 진입 상한."""
    name: str
    risk_krw: float
    ticker_cap_krw: float
    turnover_cap_fraction: float   # 09:30 누적 거래대금 대비 종목 한도(0이면 없음)
    max_concurrent: int
    daily_loss_stop_krw: float
    max_entries_per_day: int


PAPER = Profile("paper_1억", risk_krw=200_000, ticker_cap_krw=10_000_000, turnover_cap_fraction=0.01,
                max_concurrent=3, daily_loss_stop_krw=600_000, max_entries_per_day=4)
LIVE_ACCOUNT = Profile("live_200만", risk_krw=10_000, ticker_cap_krw=500_000, turnover_cap_fraction=0.0,
                       max_concurrent=1, daily_loss_stop_krw=30_000, max_entries_per_day=2)


@dataclass(frozen=True)
class RunConfig:
    change_min_percent: float = 6.0
    vwap_band_percent: float = 0.5
    turnover_top_n: int = 10
    profile: Profile = PAPER
    breakeven: bool = False          # 제거실험: 고가가 진입 + 1R에 닿으면 손절을 진입 × 1.0035로
    extra_ticks: int = 0             # 비용 감도: 모든 체결에 틱을 더 얹는다
    count_missing_minutes: bool = True
    portfolio: bool = True           # False면 하루 상한·동시 보유·손실 정지 없이 신호를 전부 센다
    entry_scale: float = 1.0         # SPEC §2.8 국면 배율. 국면 리플레이 장중 값이 없어 1로 둔다

    def key(self) -> str:
        parts = [f"G{self.change_min_percent:g}", f"B{self.vwap_band_percent:g}", f"K{self.turnover_top_n}", self.profile.name]

        if self.breakeven:
            parts.append("breakeven")

        if self.extra_ticks:
            parts.append(f"plus{self.extra_ticks}tick")

        if not self.count_missing_minutes:
            parts.append("vi_gap_off")

        if not self.portfolio:
            parts.append("signal_level")

        return "|".join(parts)

    def rules(self) -> RuleParams:
        return RuleParams(change_min_percent=self.change_min_percent, vwap_band_percent=self.vwap_band_percent,
                          turnover_top_n=self.turnover_top_n)


BASE = RunConfig()
NEIGHBORS = [replace(BASE, change_min_percent=5.0), replace(BASE, change_min_percent=8.0),
             replace(BASE, vwap_band_percent=0.3), replace(BASE, vwap_band_percent=0.8),
             replace(BASE, turnover_top_n=5), replace(BASE, turnover_top_n=20)]
VARIANTS = {
    "breakeven_on": replace(BASE, breakeven=True),
    "cost_plus1tick": replace(BASE, extra_ticks=1),
    "vi_gap_off": replace(BASE, count_missing_minutes=False),
    "live_profile": replace(BASE, profile=LIVE_ACCOUNT),
    "signal_level": replace(BASE, portfolio=False),
}


# ── 가격 단위 ───────────────────────────────────────────────────────────────

def minutes_of(hhmm: int) -> int:
    return hhmm // 100 * 60 + hhmm % 100


class PriceScale:
    """수정주가 분봉 위에서 무수정 호가 단위로 움직인다. factor = 무수정 / 수정."""

    def __init__(self, factor: float):
        self.factor = factor

    def tick(self, price: float) -> float:
        return tick_size(price * self.factor) / self.factor

    def tick_below(self, price: float) -> float:
        return tick_size(price * self.factor - PRICE_EPSILON) / self.factor

    def add_ticks(self, price: float, count: int) -> float:
        for _ in range(count):
            price += self.tick(price)

        return price

    def subtract_ticks(self, price: float, count: int) -> float:
        for _ in range(count):
            price -= self.tick_below(price)

        return price

    def floor_to_tick(self, price: float) -> float:
        if price <= 0:
            return 0.0

        raw = price * self.factor
        unit = tick_size(raw)
        return math.floor(raw / unit + PRICE_EPSILON) * unit / self.factor


class RawFactor:
    """(종목, 날짜) → 무수정/수정 비율. 날짜 바로 앞 월말의 data.go.kr 무수정 종가 / 네이버 수정 종가."""

    def __init__(self, first_day: str):
        start = pd.Timestamp(first_day) - pd.Timedelta(days=62)
        frames = []

        for path in sorted(DATAGOKR_DIR.glob("univ_*.parquet")):
            stamp = pd.Timestamp(path.stem.split("_")[1])

            if stamp >= start:
                frames.append(pd.read_parquet(path, columns=["ticker", "close", "date"]))

        raw = pd.concat(frames, ignore_index=True)
        adjusted = pd.read_parquet(BARS_PATH, columns=["Date", "code", "Close"], filters=[("Date", ">=", start)])
        merged = raw.merge(adjusted, left_on=["ticker", "date"], right_on=["code", "Date"], how="inner")
        merged = merged[(merged["close"] > 0) & (merged["Close"] > 0)]
        ratio = merged["close"].to_numpy(dtype=float) / merged["Close"].to_numpy(dtype=float)
        merged["factor"] = np.where(np.abs(ratio - 1.0) < 0.02, 1.0, ratio)
        self.table: dict[str, tuple[list[str], list[float]]] = {}

        for code, group in merged.sort_values(["ticker", "date"]).groupby("ticker"):
            self.table[code] = ([day.strftime("%Y%m%d") for day in group["date"]], group["factor"].tolist())

        self.files = len(frames)
        self.codes_not_one = int(sum(1 for _, factors in self.table.values() if any(abs(value - 1.0) >= 0.02 for value in factors)))

    def factor(self, code: str, trade_date: str) -> float:
        entry = self.table.get(code)

        if entry is None:
            return 1.0

        days, factors = entry
        position = bisect.bisect_left(days, trade_date) - 1
        return factors[max(position, 0)]


# ── 상태 기계(C++ PullbackMachine::on_bar 1:1) ─────────────────────────────

@dataclass
class StepResult:
    event: str = "none"         # none | armed | disarmed | signal
    reason: str = ""
    vwap: float = 0.0
    session_high: float = 0.0
    retrace: float = 0.0
    entry_price: float = 0.0    # C++와 같은 지정가(종가 + 2틱) — 기록용
    stop: float = 0.0           # C++와 같은 지정가 기준 손절가 — 기록용(0이면 C++는 stop_too_wide로 끝낸다)
    pullback_low: float = 0.0


def retrace_ratio(session_high: float, low: float, previous_close_price: float) -> float:
    run = session_high - previous_close_price

    if run <= 0.0:
        return -1.0

    return (session_high - low) / run


def in_vwap_band(low: float, vwap: float, band_percent: float) -> bool:
    if vwap <= 0.0:
        return False

    return abs(low / vwap - 1.0) * 100.0 <= band_percent + PERCENT_TOLERANCE


def breakout_trigger(close: float, previous_high: float, vwap: float) -> bool:
    return previous_high > 0.0 and close > previous_high and close > vwap


def stop_price(pullback_low: float, entry_price: float, rules: RuleParams, scale: PriceScale) -> tuple[float, bool]:
    """(손절가, 넓혔나). 손절 폭이 max_stop_width_percent를 넘으면 (0, False) — 진입하지 않는다."""
    if pullback_low <= 0.0 or entry_price <= 0.0:
        return 0.0, False

    stop = scale.floor_to_tick(pullback_low * (1.0 - rules.stop_below_low_percent / 100.0))
    width = (entry_price - stop) / entry_price * 100.0

    if width > rules.max_stop_width_percent + PERCENT_TOLERANCE:
        return 0.0, False

    if width < rules.min_stop_width_percent - PERCENT_TOLERANCE:
        return scale.floor_to_tick(entry_price * (1.0 - rules.min_stop_width_percent / 100.0)), True

    return stop, False


class PullbackMachine:
    """SPEC §2.3 상태 기계. 봉은 hhmm 오름차순으로 한 번씩 넣는다 — 이 객체는 지난 봉만 안다."""

    def __init__(self, previous_close_price: float, rules: RuleParams, scale: PriceScale, count_missing_minutes: bool):
        self.previous_close = previous_close_price
        self.rules = rules
        self.scale = scale
        self.count_missing_minutes = count_missing_minutes
        self.phase = "waiting"
        self.last_hhmm = -1
        self.price_volume_sum = 0.0
        self.volume_sum = 0.0
        self.session_high = 0.0
        self.armed_high = 0.0
        self.pullback_low = 0.0
        self.previous_high = 0.0
        self.previous_close_bar = 0.0
        self.bars_since_arm = 0
        self.zero_volume_run = 0

    def vwap(self) -> float:
        return self.price_volume_sum / self.volume_sum if self.volume_sum > 0.0 else 0.0

    def disarm(self, result: StepResult, reason: str) -> StepResult:
        self.phase = "done"
        result.event = "disarmed"
        result.reason = reason
        return result

    def on_bar(self, hhmm: int, high: float, low: float, close: float, volume: float) -> StepResult:
        result = StepResult()

        if hhmm <= self.last_hhmm:
            return result

        previous_hhmm = self.last_hhmm
        self.last_hhmm = hhmm
        typical = (high + low + close) / 3.0
        self.price_volume_sum += typical * volume
        self.volume_sum += volume
        self.session_high = max(self.session_high, high)
        result.vwap = self.vwap() if self.vwap() > 0.0 else close
        result.session_high = self.session_high
        result.retrace = retrace_ratio(self.session_high, low, self.previous_close)

        # VI 근사(SPEC 2.3-5). 빈 분을 세는 것은 C++에 없는 부분이다(모듈 설명 참조).
        vi_suspected = False

        if hhmm >= 901:
            run_before = self.zero_volume_run

            if self.count_missing_minutes and previous_hhmm >= 0:
                missing_from = max(minutes_of(previous_hhmm) + 1, minutes_of(901))
                missing = max(0, minutes_of(hhmm) - missing_from)

                if missing > 0:
                    run_before += missing

            if self.count_missing_minutes and run_before >= self.rules.vi_zero_volume_bars:
                vi_suspected = True

            self.zero_volume_run = run_before + 1 if volume == 0 else 0
            jumped = (self.previous_close_bar > 0.0 and
                      abs(close / self.previous_close_bar - 1.0) * 100.0 >= self.rules.vi_jump_percent - PERCENT_TOLERANCE)
            vi_suspected = vi_suspected or self.zero_volume_run >= self.rules.vi_zero_volume_bars or jumped

        previous_high = self.previous_high
        self.previous_high = high
        self.previous_close_bar = close

        if self.phase == "done":
            return result

        if vi_suspected:
            return self.disarm(result, "vi")

        if self.phase == "waiting":
            if hhmm < self.rules.first_arm_hhmm:
                return result

            if hhmm > self.rules.no_new_entry_hhmm:
                return self.disarm(result, "late")

            retraced = self.rules.retrace_min <= result.retrace <= self.rules.retrace_max
            near_vwap = in_vwap_band(low, result.vwap, self.rules.vwap_band_percent)
            not_overrun = (self.previous_close > 0.0 and
                           (self.session_high / self.previous_close - 1.0) * 100.0 <= self.rules.max_run_percent + PERCENT_TOLERANCE)

            if retraced and near_vwap and not_overrun:
                self.phase = "armed"
                self.armed_high = self.session_high
                self.pullback_low = low
                self.bars_since_arm = 0
                result.event = "armed"
                result.pullback_low = self.pullback_low

            return result

        self.bars_since_arm += 1
        self.pullback_low = min(self.pullback_low, low)
        result.pullback_low = self.pullback_low

        if high > self.armed_high:
            return self.disarm(result, "new_high")

        if close < result.vwap * (1.0 - self.rules.disarm_below_vwap_percent / 100.0):
            return self.disarm(result, "below_vwap")

        if result.retrace > self.rules.disarm_retrace:
            return self.disarm(result, "deep_retrace")

        if breakout_trigger(close, previous_high, result.vwap):
            # 여기서 끝낸다. 손절 폭 판정은 체결가로 다시 한다(SPEC 2.4) — C++ 지정가 기준 값은 기록만 한다.
            entry = self.scale.add_ticks(close, self.rules.entry_ticks)
            stop, _ = stop_price(self.pullback_low, entry, self.rules, self.scale)
            self.phase = "done"
            result.event = "signal"
            result.entry_price = entry
            result.stop = stop
            return result

        if self.bars_since_arm >= self.rules.arm_timeout_bars:
            return self.disarm(result, "timeout")

        return result


# ── 입력 ────────────────────────────────────────────────────────────────────

class InputLedger:
    """읽은 파일의 경로·크기·해시를 모은다(재현 정보)."""

    def __init__(self):
        self.entries: dict[str, tuple[int, str]] = {}

    def read_bytes(self, path: Path) -> bytes:
        content = path.read_bytes()
        self.entries[path.relative_to(REPO).as_posix()] = (len(content), hashlib.sha256(content).hexdigest())
        return content

    def digest(self) -> dict:
        combined = hashlib.sha256()

        for name in sorted(self.entries):
            size, digest = self.entries[name]
            combined.update(f"{name}\t{size}\t{digest}\n".encode("utf-8"))

        return {"files": len(self.entries), "sha256_of_list": combined.hexdigest()}


@dataclass
class Bars:
    hhmm: np.ndarray
    open: np.ndarray
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    volume: np.ndarray
    source: str


def minute_path(code: str, trade_date: str) -> Path | None:
    """공용 폴더를 먼저 본다 — 적재 도구가 공용 파일이 있으면 2단계를 건너뛰므로 그 쌍의 정본은 공용 파일이다."""
    shared = SHARED_MINUTE / code / f"{trade_date}.parquet"

    if shared.exists():
        return shared

    study = STUDY_MINUTE / code / f"{trade_date}.parquet"
    return study if study.exists() else None


def load_bars(path: Path, ledger: InputLedger) -> Bars:
    import io

    frame = pd.read_parquet(io.BytesIO(ledger.read_bytes(path)), columns=["hms", "open", "high", "low", "close", "volume"])
    frame = frame.sort_values("hms", kind="mergesort").drop_duplicates("hms", keep="last")
    hhmm = frame["hms"].str[:4].astype(int).to_numpy()
    return Bars(hhmm, frame["open"].to_numpy(float), frame["high"].to_numpy(float), frame["low"].to_numpy(float),
                frame["close"].to_numpy(float), frame["volume"].to_numpy(float),
                "shared" if SHARED_MINUTE in path.parents else "study")


def bars_problem(bars: Bars, snapshot: dict) -> str:
    """하루치 파일이 쓸 만한지. 빈 문자열이면 쓴다."""
    if len(bars.hhmm) == 0:
        return "empty_file"

    regular = bars.hhmm[bars.hhmm < 1520]

    if len(regular) < 2:
        return "too_few_bars"

    gaps = np.diff([minutes_of(int(value)) for value in regular])

    if gaps.max() >= MINIMUM_GAP_FOR_BAD_FILE:
        return "long_gap_or_30min_file"

    morning = bars.close[bars.hhmm < 930]

    if len(morning) == 0:
        return "no_morning_bars"

    price = snapshot.get("price_0930") or 0.0

    if price and abs(morning[-1] / price - 1.0) > BASIS_MISMATCH_TOLERANCE:
        return "basis_mismatch_with_snapshot"

    return ""


def completed_days_from_log() -> set[str]:
    if not FETCH_LOG.exists():
        return set()

    text = FETCH_LOG.read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r"^\s*(\d{8}) 후보", text, flags=re.MULTILINE))


def trading_days(first: str, last: str) -> list[str]:
    pairs = json.loads(PAIRS_PATH.read_text(encoding="utf-8"))
    return sorted(day for day in pairs if first <= day <= last)


def load_regime_open() -> dict[str, float]:
    regime = pd.read_csv(REGIME_PATH, sep="\t", usecols=["date", "score_open"], dtype={"date": str})
    return {day.replace("-", ""): float(score) for day, score in zip(regime["date"], regime["score_open"]) if pd.notna(score)}


# ── 유니버스(SPEC §2.2) ──────────────────────────────────────────────────────

def select_candidates(snapshots: dict, rules: RuleParams, names: dict, name_rules, factors: RawFactor, trade_date: str) -> list[dict]:
    """등락률 띠·전일 종가·거래대금·이름 규칙을 지난 종목을 09:30 누적 거래대금 내림차순 K개. 같으면 코드 순(C++와 같다)."""
    passed = []

    for code, item in snapshots.items():
        gain_percent = item["gain_0930"] * 100.0

        if gain_percent < rules.change_min_percent or gain_percent > rules.change_max_percent:
            continue

        if item["value_0930"] < rules.min_turnover_krw:
            continue

        raw_previous_close = item["prev_close"] * factors.factor(code, trade_date)

        if raw_previous_close < rules.min_previous_close_krw:
            continue

        if not is_common_stock(code, names.get(code, ""), name_rules):
            continue

        passed.append({"code": code, **item})

    passed.sort(key=lambda entry: (-entry["value_0930"], entry["code"]))

    for rank, entry in enumerate(passed, start=1):
        entry["rank"] = rank

    return passed[:rules.turnover_top_n]


# ── 청산·체결 ───────────────────────────────────────────────────────────────

def run_exit(bars: Bars, start: int, entry: float, stop: float, scale: PriceScale, rules: RuleParams, extra_ticks: int,
             breakeven: bool = False, hard_stop_percent: float = 0.0, trail_percent: float = 0.0) -> tuple[int, float, str]:
    """start 봉(진입 봉)부터 청산을 찾는다. 돌려주는 값: (청산 봉 위치, 청산가, 사유).
    손절은 그 봉 시가 전에 정한 선으로만 본다 — 봉 안에서 올린 선(본전·트레일)은 다음 봉부터 쓴다."""
    risk = entry - stop
    peak = entry
    breakeven_moved = False

    for index in range(start, len(bars.hhmm)):
        if bars.hhmm[index] >= rules.exit_hhmm:
            return index, scale.subtract_ticks(bars.open[index], 1 + extra_ticks), "time"

        if bars.low[index] <= stop + PRICE_EPSILON:
            return index, scale.subtract_ticks(min(stop, bars.open[index]), 2 + extra_ticks), "stop_moved" if breakeven_moved else "stop"

        if breakeven and not breakeven_moved and bars.high[index] >= entry + risk - PRICE_EPSILON:
            stop = max(stop, scale.floor_to_tick(entry * 1.0035))
            breakeven_moved = True

        if trail_percent > 0.0:
            peak = max(peak, bars.high[index])
            stop = max(stop, scale.floor_to_tick(peak * (1.0 - trail_percent / 100.0)))

    last = len(bars.hhmm) - 1
    return last, scale.subtract_ticks(bars.close[last], 1 + extra_ticks), "last_bar"


def per_share_net(entry: float, exit_price: float) -> float:
    """주당 순손익(원). 수수료·매도세는 PYQuant/backtest/costs.py LIVE(라이브 원장과 같은 식)."""
    return fill_result("SELL", exit_price, 1, LIVE).net - fill_result("BUY", entry, 1, LIVE).net


def evaluate_pullback(bars: Bars, previous_close: float, scale: PriceScale, config: RunConfig) -> dict:
    """한 종목일의 A 신호 단위 결과(포트폴리오 제약 전)."""
    rules = config.rules()
    machine = PullbackMachine(previous_close, rules, scale, config.count_missing_minutes)
    outcome = {"armed_hhmm": None, "signal_hhmm": None, "machine_end": ""}
    signal_index = None
    signal_result = None

    for index in range(len(bars.hhmm)):
        hhmm = int(bars.hhmm[index])

        if hhmm >= 1520:
            break

        result = machine.on_bar(hhmm, bars.high[index], bars.low[index], bars.close[index], bars.volume[index])

        if result.event == "armed":
            outcome["armed_hhmm"] = hhmm

        elif result.event == "disarmed":
            outcome["machine_end"] = result.reason
            return outcome

        elif result.event == "signal":
            signal_index = index
            signal_result = result
            break

    if signal_index is None:
        outcome["machine_end"] = f"no_signal_{machine.phase}"
        return outcome

    outcome["signal_hhmm"] = int(bars.hhmm[signal_index])
    outcome["machine_end"] = "signal"
    outcome["cpp_stop_too_wide"] = signal_result.stop <= 0.0
    fill_index = signal_index + 1

    if fill_index >= len(bars.hhmm) or bars.hhmm[fill_index] >= rules.exit_hhmm:
        outcome["fill"] = "no_next_bar"
        return outcome

    limit = scale.add_ticks(bars.close[signal_index], rules.entry_ticks)

    if bars.open[fill_index] > limit + PRICE_EPSILON:
        outcome["fill"] = "gap_skip"
        return outcome

    entry = scale.add_ticks(bars.open[fill_index], 1 + config.extra_ticks)
    stop, widened = stop_price(signal_result.pullback_low, entry, rules, scale)

    if stop <= 0.0:
        outcome["fill"] = "stop_too_wide"
        return outcome

    exit_index, exit_price, reason = run_exit(bars, fill_index, entry, stop, scale, rules, config.extra_ticks, breakeven=config.breakeven)
    net = per_share_net(entry, exit_price)
    outcome.update({
        "fill": "filled", "fill_hhmm": int(bars.hhmm[fill_index]), "entry": entry, "stop": stop, "stop_widened": widened,
        "pullback_low": signal_result.pullback_low, "risk_per_share": entry - stop,
        "exit_hhmm": int(bars.hhmm[exit_index]), "exit": exit_price, "exit_reason": reason,
        "net_per_share": net, "net_bp": net / entry * 1e4, "net_r": net / (entry - stop),
        "gross_bp": (exit_price - entry) / entry * 1e4,
    })
    return outcome


def control_fixed_stop(bars: Bars, start: int, scale: PriceScale, rules: RuleParams, extra_ticks: int, stop_percent: float,
                       trail_percent: float = 0.0) -> dict | None:
    """start 봉 시가 + 1틱 매수, 진입 × (1 − stop_percent%) 손절, 15:10 청산. R = 진입 − 손절."""
    if start is None or start >= len(bars.hhmm) or bars.hhmm[start] >= rules.exit_hhmm:
        return None

    entry = scale.add_ticks(bars.open[start], 1 + extra_ticks)
    stop = scale.floor_to_tick(entry * (1.0 - stop_percent / 100.0))

    if stop <= 0.0 or stop >= entry:
        return None

    exit_index, exit_price, reason = run_exit(bars, start, entry, stop, scale, rules, extra_ticks, trail_percent=trail_percent)
    net = per_share_net(entry, exit_price)
    return {"entry": entry, "exit": exit_price, "entry_hhmm": int(bars.hhmm[start]), "exit_reason": reason,
            "net_bp": net / entry * 1e4, "net_r": net / (entry - stop)}


def first_index_at_or_after(bars: Bars, hhmm: int) -> int | None:
    position = int(np.searchsorted(bars.hhmm, hhmm, side="left"))
    return position if position < len(bars.hhmm) else None


def control_chase(bars: Bars, scale: PriceScale, rules: RuleParams, extra_ticks: int) -> dict | None:
    """C1: 09:31 봉 시가 + 1틱, 손절 −1.9%(ITB v2 hard_pct), 15:10."""
    return control_fixed_stop(bars, first_index_at_or_after(bars, 931), scale, rules, extra_ticks, 1.9)


def control_opening_range(bars: Bars, scale: PriceScale, rules: RuleParams, extra_ticks: int) -> dict | None:
    """C2: 09:30 봉부터 종가가 09:00~09:29 고가를 처음 넘은 봉의 다음 봉 시가 + 1틱, 손절 −1.9%, 고점 대비 −2.8% 트레일, 15:10.
    신호는 13:15 봉까지만 본다(A 무장 마감과 같게 — SPEC에 없는 값)."""
    morning = bars.hhmm < 930

    if not morning.any():
        return None

    range_high = bars.high[morning].max()

    for index in range(int(np.argmax(~morning)) if (~morning).any() else len(bars.hhmm), len(bars.hhmm)):
        if bars.hhmm[index] > rules.no_new_entry_hhmm:
            return None

        if bars.close[index] > range_high:
            return control_fixed_stop(bars, index + 1, scale, rules, extra_ticks, 1.9, trail_percent=2.8)

    return None


def control_random_time(bars: Bars, scale: PriceScale, rules: RuleParams, extra_ticks: int, hhmm: int, stop_width_percent: float) -> dict | None:
    """C3 한 번: 뽑은 시각 이후 첫 봉 시가 + 1틱, A와 같은 손절 폭, 15:10."""
    return control_fixed_stop(bars, first_index_at_or_after(bars, hhmm), scale, rules, extra_ticks, stop_width_percent)


# ── 포트폴리오 층(SPEC §2.3-6, §2.5) ─────────────────────────────────────────

def apply_portfolio(day_signals: list[dict], config: RunConfig) -> list[dict]:
    """체결 시각 순(같으면 09:30 순위 순)으로 하루 상한·동시 보유·일손실 정지·수량을 건다. 거부 사유는 portfolio_reject에 남긴다."""
    profile = config.profile
    ordered = sorted(day_signals, key=lambda item: (item["fill_hhmm"], item["rank"], item["code"]))
    taken: list[dict] = []

    for item in ordered:
        item = dict(item)
        fill = item["fill_hhmm"]
        cap_krw = profile.ticker_cap_krw

        if profile.turnover_cap_fraction > 0:
            cap_krw = min(cap_krw, profile.turnover_cap_fraction * item["value_0930"])

        # SPEC 2.5 식은 "위험액 / (진입 − S)"가 이미 주수라 단위가 안 맞는다. 위험액만큼 잃는 금액(위험액 × 진입 / (진입 − S))으로 읽는다.
        risk_notional = profile.risk_krw * config.entry_scale * item["entry"] / item["risk_per_share"]
        quantity = int(math.floor(min(cap_krw, risk_notional) / item["entry"]))
        item["quantity"] = quantity
        reject = ""

        if config.portfolio:
            accepted = [trade for trade in taken if not trade["portfolio_reject"]]
            realized = sum(trade["net_krw"] for trade in accepted if trade["exit_hhmm"] < fill)
            open_count = sum(1 for trade in accepted if trade["exit_hhmm"] >= fill)

            if len(accepted) >= profile.max_entries_per_day:
                reject = "daily_entry_cap"

            elif realized <= -profile.daily_loss_stop_krw:
                reject = "daily_loss_stop"

            elif open_count >= profile.max_concurrent:
                reject = "concurrent_cap"

        if not reject and quantity <= 0:
            reject = "quantity_zero"

        item["portfolio_reject"] = reject
        item["net_krw"] = 0.0 if reject else (fill_result("SELL", item["exit"], quantity, LIVE).net
                                               - fill_result("BUY", item["entry"], quantity, LIVE).net)
        item["notional_krw"] = 0.0 if reject else item["entry"] * quantity
        taken.append(item)

    return taken


# ── 성과 ────────────────────────────────────────────────────────────────────

def t_value(values: np.ndarray) -> float:
    values = np.asarray(values, dtype=float)

    if len(values) < 2 or values.std(ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def summarize(trades: pd.DataFrame, usable_days: int) -> dict:
    """체결 거래 표의 수치. 판정은 하지 않는다 — 값만 낸다."""
    if trades.empty:
        return {"trades": 0, "usable_days": usable_days}

    ordered = trades.sort_values(["date", "fill_hhmm", "rank"], kind="mergesort")
    net_r = ordered["net_r"].to_numpy(float)
    daily = ordered.groupby("date")["net_r"].sum()
    all_days = np.concatenate([daily.to_numpy(float), np.zeros(max(usable_days - len(daily), 0))])
    cumulative = np.concatenate([[0.0], np.cumsum(net_r)])
    drawdown = float((np.maximum.accumulate(cumulative) - cumulative).max())
    wins, losses = net_r[net_r > 0], net_r[net_r <= 0]
    profit_krw = ordered.loc[ordered["net_krw"] > 0, "net_krw"].sum()
    loss_krw = -ordered.loc[ordered["net_krw"] <= 0, "net_krw"].sum()
    top5 = np.sort(net_r)[::-1][:5].sum()
    monthly = ordered.groupby(ordered["date"].str[:6])["net_r"].sum()
    return {
        "trades": int(len(ordered)),
        "days_with_trades": int(len(daily)),
        "usable_days": usable_days,
        "trades_per_usable_day": round(len(ordered) / max(usable_days, 1), 3),
        "net_bp_mean": round(float(ordered["net_bp"].mean()), 2),
        "gross_bp_mean": round(float(ordered["gross_bp"].mean()), 2),
        "net_r_mean": round(float(net_r.mean()), 4),
        "net_r_median": round(float(np.median(net_r)), 4),
        "win_rate": round(float((net_r > 0).mean()), 4),
        "avg_win_r": round(float(wins.mean()), 4) if len(wins) else None,
        "avg_loss_r": round(float(losses.mean()), 4) if len(losses) else None,
        "profit_factor_krw": round(float(profit_krw / loss_krw), 3) if loss_krw > 0 else None,
        "net_krw_total": round(float(ordered["net_krw"].sum()), 0),
        "day_t_days_with_trades": round(t_value(daily.to_numpy(float)), 3),
        "day_t_all_usable_days": round(t_value(all_days), 3),
        "mdd_r": round(drawdown, 3),
        "net_r_sum": round(float(net_r.sum()), 3),
        "net_r_sum_without_top5": round(float(net_r.sum() - top5), 3),
        "net_r_sum_without_best_month": round(float(net_r.sum() - monthly.max()), 3),
        "best_month": str(monthly.idxmax()),
        "exit_reasons": {str(key): int(value) for key, value in ordered["exit_reason"].value_counts().sort_index().items()},
    }


# ── 실행 ────────────────────────────────────────────────────────────────────

@dataclass
class DayInput:
    trade_date: str
    snapshots: dict
    failed: int
    bars: dict = field(default_factory=dict)          # code → Bars
    bar_problems: dict = field(default_factory=dict)  # code → 사유


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()

    except Exception:
        return "unknown"


def write_tsv(frame: pd.DataFrame, path: Path) -> None:
    path.write_bytes(frame.to_csv(sep="\t", index=False, float_format="%.6f", lineterminator="\n").encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="스터디 30 전략 A 백테스트")
    parser.add_argument("--open-holdout", action="store_true", help="검증 구간(2026-04-01~09-29)까지 계산한다. 한 번만 쓴다.")
    arguments = parser.parse_args()

    periods = {"development": DEVELOPMENT}

    if arguments.open_holdout:
        periods["holdout"] = HOLDOUT

    ledger = InputLedger()
    name_rules = load_name_rules()
    membership = pd.read_parquet(MEMBERSHIP_PATH, columns=["code", "name"])
    names = dict(zip(membership["code"], membership["name"].fillna("")))
    factors = RawFactor(DEVELOPMENT[0])
    regime_open = load_regime_open()
    logged_complete = completed_days_from_log()
    configs = [BASE] + NEIGHBORS + list(VARIANTS.values())
    all_days = {name: trading_days(*span) for name, span in periods.items()}
    existing = sorted(path.stem for path in OPEN30_DIR.glob("*.json"))
    latest_json = existing[-1] if existing else ""

    signals: dict[str, list[dict]] = {config.key(): [] for config in configs}
    candidate_rows: list[dict] = []
    coverage = {name: {"trading_days": len(days), "no_open30_json": 0, "in_progress_skipped": 0, "used": 0,
                       "stage1_failed_codes": 0, "base_candidates": 0, "base_missing_minute_file": 0,
                       "base_bad_minute_file": {}} for name, days in all_days.items()}
    usable_by_period: dict[str, list[str]] = {name: [] for name in periods}
    bars_cache_for_controls: dict[tuple[str, str], tuple[Bars, PriceScale]] = {}

    for period, days in all_days.items():
        for trade_date in days:
            json_path = OPEN30_DIR / f"{trade_date}.json"

            if not json_path.exists():
                coverage[period]["no_open30_json"] += 1
                continue

            if trade_date == latest_json and trade_date not in logged_complete:
                coverage[period]["in_progress_skipped"] += 1   # 2단계(하루치) 적재가 아직 도는 날
                continue

            verdict = json.loads(ledger.read_bytes(json_path).decode("utf-8"))
            day = DayInput(trade_date, verdict.get("snapshots", {}), len(verdict.get("failed", [])))
            coverage[period]["used"] += 1
            coverage[period]["stage1_failed_codes"] += day.failed
            usable_by_period[period].append(trade_date)
            selections = {config.key(): select_candidates(day.snapshots, config.rules(), names, name_rules, factors, trade_date)
                          for config in configs}

            for code in sorted({entry["code"] for chosen in selections.values() for entry in chosen}):
                path = minute_path(code, trade_date)

                if path is None:
                    day.bar_problems[code] = "missing_minute_file"
                    continue

                bars = load_bars(path, ledger)
                problem = bars_problem(bars, day.snapshots[code])

                if problem:
                    day.bar_problems[code] = problem
                    continue

                day.bars[code] = bars

            for config in configs:
                key = config.key()
                chosen = selections[key]
                day_signals = []

                for entry in chosen:
                    code = entry["code"]
                    row = {"period": period, "date": trade_date, "code": code, "rank": entry["rank"],
                           "gain_0930": entry["gain_0930"], "value_0930": entry["value_0930"],
                           "prev_close": entry["prev_close"], "regime_score_open": regime_open.get(trade_date)}

                    if code not in day.bars:
                        row["machine_end"] = day.bar_problems.get(code, "missing_minute_file")

                        if config is BASE:
                            coverage[period]["base_candidates"] += 1

                            if row["machine_end"] == "missing_minute_file":
                                coverage[period]["base_missing_minute_file"] += 1

                            else:
                                bad = coverage[period]["base_bad_minute_file"]
                                bad[row["machine_end"]] = bad.get(row["machine_end"], 0) + 1

                            candidate_rows.append(row)

                        continue

                    bars = day.bars[code]
                    scale = PriceScale(factors.factor(code, trade_date))
                    outcome = evaluate_pullback(bars, entry["prev_close"], scale, config)
                    row.update(outcome)
                    row["factor"] = scale.factor
                    row["minute_source"] = bars.source

                    if config is BASE:
                        coverage[period]["base_candidates"] += 1
                        candidate_rows.append(row)

                    if outcome.get("fill") == "filled":
                        day_signals.append(row)
                        bars_cache_for_controls[(trade_date, code)] = (bars, scale)

                signals[key].extend(apply_portfolio(day_signals, config))

    # 대조군: 기본 칸 체결 거래와 같은 종목일(C1·C2), C3는 기본 칸 개발 구간 진입 시각 분포에서 200회.
    rules = BASE.rules()
    base_all = pd.DataFrame(signals[BASE.key()])
    trade_columns = ["date", "code", "rank", "fill_hhmm", "entry", "stop", "exit_hhmm", "exit", "exit_reason", "net_bp", "net_r"]
    controls: dict[str, dict] = {}
    random_engine = np.random.default_rng(SEED)

    if not base_all.empty:
        base_taken = base_all[base_all["portfolio_reject"] == ""].copy()
        development_times = np.sort(base_taken.loc[base_taken["period"] == "development", "fill_hhmm"].to_numpy(int))
        control_rows = []

        for _, trade in base_taken.sort_values(["date", "fill_hhmm", "rank"], kind="mergesort").iterrows():
            bars, scale = bars_cache_for_controls[(trade["date"], trade["code"])]
            chase = control_chase(bars, scale, rules, 0)
            opening_range = control_opening_range(bars, scale, rules, 0)
            width = (trade["entry"] - trade["stop"]) / trade["entry"] * 100.0
            draws = random_engine.choice(development_times, size=C3_DRAWS, replace=True) if len(development_times) else np.array([], int)
            random_r, random_bp, cache = [], [], {}

            for hhmm in draws:
                if hhmm not in cache:
                    cache[hhmm] = control_random_time(bars, scale, rules, 0, int(hhmm), width)

                result = cache[hhmm]
                random_r.append(result["net_r"] if result else np.nan)
                random_bp.append(result["net_bp"] if result else np.nan)

            control_rows.append({
                "date": trade["date"], "code": trade["code"],
                "c1_net_bp": chase["net_bp"] if chase else np.nan, "c1_net_r": chase["net_r"] if chase else np.nan,
                "c2_triggered": opening_range is not None,
                "c2_net_bp": opening_range["net_bp"] if opening_range else np.nan,
                "c2_net_r": opening_range["net_r"] if opening_range else np.nan,
                "c3_net_bp_mean": float(np.nanmean(random_bp)) if len(random_bp) else np.nan,
                "c3_net_r_mean": float(np.nanmean(random_r)) if len(random_r) else np.nan,
                "c3_draws_r": random_r,
            })

        control_frame = pd.DataFrame(control_rows)
        merged = base_taken.merge(control_frame.drop(columns="c3_draws_r"), on=["date", "code"], how="left")

        for period in periods:
            part = merged[merged["period"] == period]
            draws_matrix = np.array([row["c3_draws_r"] for row, keep in
                                     zip(control_rows, (base_taken.sort_values(["date", "fill_hhmm", "rank"], kind="mergesort")["period"] == period))
                                     if keep], dtype=float)

            if part.empty:
                continue

            c3_draw_means = np.nanmean(draws_matrix, axis=0) if draws_matrix.size else np.array([])
            a_mean_r = float(part["net_r"].mean())
            c2 = part[part["c2_triggered"]]
            controls[period] = {
                "paired_trades": int(len(part)),
                "a_net_bp_mean": round(float(part["net_bp"].mean()), 2), "a_net_r_mean": round(a_mean_r, 4),
                "c1_net_bp_mean": round(float(part["c1_net_bp"].mean()), 2), "c1_net_r_mean": round(float(part["c1_net_r"].mean()), 4),
                "a_minus_c1_bp": round(float((part["net_bp"] - part["c1_net_bp"]).mean()), 2),
                "a_minus_c1_r": round(float((part["net_r"] - part["c1_net_r"]).mean()), 4),
                "c3_net_bp_mean": round(float(part["c3_net_bp_mean"].mean()), 2), "c3_net_r_mean": round(float(part["c3_net_r_mean"].mean()), 4),
                "a_minus_c3_bp": round(float((part["net_bp"] - part["c3_net_bp_mean"]).mean()), 2),
                "a_minus_c3_r": round(float((part["net_r"] - part["c3_net_r_mean"]).mean()), 4),
                "a_percentile_in_c3_draws": round(float((c3_draw_means < a_mean_r).mean() * 100), 1) if len(c3_draw_means) else None,
                "c2_triggered": int(len(c2)),
                "c2_net_bp_mean": round(float(c2["c2_net_bp"].mean()), 2) if len(c2) else None,
                "c2_profit_factor_bp": (round(float(c2.loc[c2["c2_net_bp"] > 0, "c2_net_bp"].sum() / -c2.loc[c2["c2_net_bp"] <= 0, "c2_net_bp"].sum()), 3)
                                        if len(c2) and (c2["c2_net_bp"] <= 0).any() else None),
                "note": "C1·C2의 R은 진입 × 1.9%를 R로 쓴다. A와 같은 단위 비교는 bp로 본다.",
            }

        base_trades_out = merged
    else:
        base_trades_out = pd.DataFrame()

    # 실행별 요약
    def taken_frame(config: RunConfig, period: str) -> pd.DataFrame:
        frame = pd.DataFrame(signals[config.key()])

        if frame.empty:
            return frame

        return frame[(frame["portfolio_reject"] == "") & (frame["period"] == period)]

    def rejects(config: RunConfig, period: str) -> dict:
        frame = pd.DataFrame(signals[config.key()])

        if frame.empty:
            return {}

        part = frame[frame["period"] == period]
        return {str(key): int(value) for key, value in part["portfolio_reject"].replace("", "accepted").value_counts().sort_index().items()}

    grid_rows, variant_rows, summaries = [], [], {}

    for period in periods:
        usable = len(usable_by_period[period])

        for label, config in [("base", BASE)] + [(f"neighbor_{index + 1}", config) for index, config in enumerate(NEIGHBORS)]:
            summary = summarize(taken_frame(config, period), usable)
            grid_rows.append({"period": period, "cell": label, "run": config.key(), **{key: value for key, value in summary.items()
                                                                                         if not isinstance(value, dict)}})

        for label, config in VARIANTS.items():
            summary = summarize(taken_frame(config, period), usable)
            variant_rows.append({"period": period, "variant": label, "run": config.key(), **{key: value for key, value in summary.items()
                                                                                               if not isinstance(value, dict)}})

        summaries[period] = {
            "base": summarize(taken_frame(BASE, period), usable),
            "base_portfolio_rejects": rejects(BASE, period),
            "neighbors_net_r_mean": {config.key(): summarize(taken_frame(config, period), usable).get("net_r_mean") for config in NEIGHBORS},
        }

    candidates = pd.DataFrame(candidate_rows)
    machine_counts = {}

    if not candidates.empty:
        for period in periods:
            part = candidates[candidates["period"] == period]
            machine_counts[period] = {
                "machine_end": {str(key): int(value) for key, value in part["machine_end"].value_counts().sort_index().items()},
                "fill": {str(key): int(value) for key, value in part.get("fill", pd.Series(dtype=str)).dropna().value_counts().sort_index().items()},
                "cpp_stop_too_wide_on_signal": int(part.get("cpp_stop_too_wide", pd.Series(dtype=bool)).eq(True).sum()),
                "stop_widened_to_0.6pct": int(part.get("stop_widened", pd.Series(dtype=bool)).eq(True).sum()),
            }

    # 산출
    trade_frames = []

    for config in configs:
        frame = pd.DataFrame(signals[config.key()])

        if frame.empty:
            continue

        frame = frame[frame["portfolio_reject"] == ""] if config is not BASE else frame
        frame.insert(0, "run", config.key())
        trade_frames.append(frame)

    trades_out = pd.concat(trade_frames, ignore_index=True) if trade_frames else pd.DataFrame()

    if not base_trades_out.empty and not trades_out.empty:
        control_columns = ["date", "code", "c1_net_bp", "c1_net_r", "c2_triggered", "c2_net_bp", "c2_net_r", "c3_net_bp_mean", "c3_net_r_mean"]
        trades_out = trades_out.merge(base_trades_out[control_columns].assign(run=BASE.key()), on=["run", "date", "code"], how="left")

    preferred = ["run", "period", "date", "code", "rank", "gain_0930", "value_0930", "prev_close", "factor", "minute_source",
                 "regime_score_open", "armed_hhmm", "signal_hhmm", "fill_hhmm", "entry", "pullback_low", "stop", "stop_widened",
                 "risk_per_share", "exit_hhmm", "exit", "exit_reason", "gross_bp", "net_bp", "net_r", "quantity", "notional_krw",
                 "net_krw", "portfolio_reject", "c1_net_bp", "c1_net_r", "c2_triggered", "c2_net_bp", "c2_net_r", "c3_net_bp_mean",
                 "c3_net_r_mean"]

    if not trades_out.empty:
        trades_out = trades_out[[column for column in preferred if column in trades_out.columns]]
        trades_out = trades_out.sort_values(["run", "date", "fill_hhmm", "rank"], kind="mergesort")

    candidate_columns = ["period", "date", "code", "rank", "gain_0930", "value_0930", "prev_close", "factor", "minute_source",
                         "regime_score_open", "machine_end", "armed_hhmm", "signal_hhmm", "cpp_stop_too_wide", "fill", "fill_hhmm",
                         "entry", "stop", "exit_hhmm", "exit_reason", "net_bp", "net_r"]

    if not candidates.empty:
        candidates = candidates[[column for column in candidate_columns if column in candidates.columns]]
        candidates = candidates.sort_values(["date", "rank"], kind="mergesort")

    write_tsv(candidates, STUDY / "a_candidates.tsv")
    write_tsv(trades_out, STUDY / "a_trades.tsv")
    write_tsv(pd.DataFrame(grid_rows), STUDY / "a_grid.tsv")
    write_tsv(pd.DataFrame(variant_rows), STUDY / "a_variants.tsv")

    metrics = {
        "study": "30_strong_stock_strategies / A 강한 종목 첫 VWAP 눌림",
        "spec": "research/studies/30_strong_stock_strategies/SPEC.md §2 (사전등록, 바꾸지 않음). 합격 판정은 이 파일에 쓰지 않는다.",
        "status": "분봉 적재 중 — 있는 날짜만 썼다. 수치는 잠정값이다.",
        "seed": SEED,
        "periods": {name: f"{span[0]}~{span[1]}" for name, span in periods.items()},
        "holdout_opened": bool(arguments.open_holdout),
        "coverage": coverage,
        "usable_days_range": {name: (f"{days[0]}~{days[-1]}" if days else "") for name, days in usable_by_period.items()},
        "costs": {"commission_percent_per_side": LIVE.commission_percent, "sell_tax_percent": LIVE.sell_tax_percent,
                  "slippage": "체결 규칙 틱만 — 진입 시가 + 1틱, 시간 청산 시가 − 1틱, 손절 min(S, 시가) − 2틱"},
        "base_cell": BASE.key(),
        "summary": summaries,
        "machine_counts_base": machine_counts,
        "controls_base": controls,
        "raw_factor": {"datagokr_files": factors.files, "codes_with_factor_not_1": factors.codes_not_one},
        "inputs": ledger.digest(),
        "git_commit": git_commit(),
        "rerun": "py -X utf8 research/studies/30_strong_stock_strategies/backtest_a.py" + (" --open-holdout" if arguments.open_holdout else ""),
    }
    (STUDY / "a_metrics.json").write_bytes(json.dumps(metrics, ensure_ascii=False, indent=1, default=str).encode("utf-8"))

    for period in periods:
        print(f"[{period}] 거래일 {coverage[period]['trading_days']} · 사용 {coverage[period]['used']} · "
              f"판정 json 없음 {coverage[period]['no_open30_json']} · 적재 중이라 건너뜀 {coverage[period]['in_progress_skipped']} · "
              f"기본 칸 후보 {coverage[period]['base_candidates']} 중 하루치 파일 없음 {coverage[period]['base_missing_minute_file']}")

    grid_frame = pd.DataFrame(grid_rows)
    variant_frame = pd.DataFrame(variant_rows)
    show = ["period", "run", "trades", "days_with_trades", "net_bp_mean", "net_r_mean", "win_rate", "day_t_days_with_trades", "mdd_r"]

    if not grid_frame.empty:
        print(grid_frame[[column for column in show if column in grid_frame.columns]].to_string(index=False))

    if not variant_frame.empty:
        print(variant_frame[[column for column in show if column in variant_frame.columns]].to_string(index=False))

    print(json.dumps({"machine_counts_base": machine_counts, "controls_base": controls}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
