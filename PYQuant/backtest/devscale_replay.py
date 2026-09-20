#!/usr/bin/env python3
"""DeviationScale TRENDX 슬리브의 3분봉 리플레이 — 실행 층(분할 단계 구간·물타기·스탑) 비교용.

`Quant/include/strategy/DeviationScaleStrategy.h`의 `on_trade_batch` 경로 하나만 옮겼다. 스캐너·주문
라우터·OrderGate는 옮기지 않는다. 옮긴 규칙과 근사는 README(research/studies/13_trendx_gate/README.md)에
표로 있고, 여기서는 코드 옆에 짧게만 적는다.

  - 존 게이트: 정배열·이격 밴드 모두 전일 확정 일봉 SMA로 판정한다(엔진은 진입에 fold_today를 쓰지만
    보수적으로 전일 값). 히스테리시스 진입 5~35 / 유지 1~39.
  - 워밍업: 3분봉 < sma_period(20)이면 기준선 = 일봉 SMA20, BUY 분할 단계 차단(베이스 BUY는 허용, 엔진과 같다).
  - 분할 단계 구간: 기준점 = 현재가. 베이스 BUY = 현재가 −1틱, SELL 분할 단계 = 평단×(1+3%), BUY 분할 단계 = 현재가×(1−1%).
    지정가 체결은 다음 3분봉 범위로 판정(시가가 이미 넘겨 있으면 시가 체결). 부분체결 없음.
  - 재구성: 봉마다 취소·재발주(min_rebuild_sec=8은 틱이 없어 봉 단위로 근사).
  - 존 이탈·하드 스탑·트레일·15:15: 전량 시장가 = 다음 봉 시가 −1틱. 마지막 봉이면 그 종가.
  - 비용: backtest/costs.py LIVE(라이브 원장과 같은 수수료 0.015%·매도세 0.20%, 슬리피지·충격 0).

    py PYQuant/backtest/devscale_replay.py --pairs research/studies/13_trendx_gate/gate_pairs.jsonl \\
        --out research/studies/13_trendx_gate/replay_results.tsv
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np
import pandas as pd

_PYQUANT = Path(__file__).resolve().parents[1]
_REPO = _PYQUANT.parent
if str(_PYQUANT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT))
from backtest.costs import LIVE, CostSpec, fill_result  # noqa: E402

MINUTE_DIR = _PYQUANT / "data" / "minute"
DAILY_PARQUET = _PYQUANT / "data" / "bars_all_pit.parquet"
R_DENOM_PCT = 6.0


def tick_size(px: float) -> float:
    """Quant/include/core/TickSize.h와 같은 격자."""
    if px < 2000:
        return 1
    if px < 5000:
        return 5
    if px < 20000:
        return 10
    if px < 50000:
        return 50
    if px < 200000:
        return 100
    if px < 500000:
        return 500
    return 1000


def round_tick(px: float, side: str) -> float:
    t = tick_size(px)
    return math.floor(px / t) * t if side == "BUY" else math.ceil(px / t) * t


@dataclass
class Params:
    entry_lower_pct: float = 5.0
    entry_upper_pct: float = 35.0
    zone_hyst_pct: float = 4.0
    align_tol_pct: float = 0.0
    sma_period: int = 20
    dev_sell_pct: float = 3.0
    dev_buy_pct: float = 1.0
    split_step_count: int = 1
    buy_split_steps: int = 1
    notional_krw: float = 2_500_000.0
    base_pct: float = 0.015
    max_pct: float = 0.022
    stop_loss_pct: float = 0.0
    stop_atr_mult: float = 0.0           # >0이면 평단 − 배수×전일확정 일봉 ATR14를 손절선으로 쓴다(고정 %와 배타)
    stop_cooldown_bars: int = 5          # 900초 / 3분
    entry_confirm_bars: int = 0          # >0이면 직전 닫힌 봉 N개가 연속 종가 상승일 때만 베이스 BUY
    trail_sma_exit: bool = False
    trail_sma_tol_pct: float = 1.0
    market_close_hhmm: int = 1515
    interval_min: int = 3
    max_notional_per_ticker: float = 15_000_000.0   # OrderGate risk.max_notional_per_ticker — 전략 밖 상한
    # ── 눌림 슬리브(라이브 DEVSCALE, 09-21) — entry_lower_pct=0이면 존 하단은 SMA20 아래 −pullback_percent ──
    pullback_percent: float = 8.0
    base_on_price: bool = True           # False면 베이스 BUY 기준선 = 3분봉 SMA(워밍업은 일봉 SMA20), 현재가보다 높으면 현재가 −1틱
    sell_base_average: bool = True       # False면 익절 기준점 = max(기준선, 현재가) — 09-20 전 라이브(가격을 쫓아 익절이 안 걸림)
    reentry_cooldown_bars: int = 0       # 전량 청산 뒤 새 베이스 대기(봉). 라이브 600초 → 4봉
    trail_arm_percent: float = 0.0           # >0이면 진입 후 고가가 평단×(1+arm)에 닿은 뒤 고가 대비 −trail_pct에서 전량 청산
    trail_percent: float = 1.0
    entry_from_hhmm: int = 0             # >0이면 이 시각(HHMM)부터만 베이스 BUY(개장 직후 변동 구간 회피)
    carry_overnight: bool = False        # True면 장 마감에 팔지 않고 다음 날로 보유를 넘긴다(익절·손절·존 이탈로만 청산)
    carry_max_days: int = 0              # >0이면 이 일수를 넘긴 보유는 그날 마감에 판다(0=무제한)
    entry_atr_max_percent: float = 0.0       # >0이면 전일 ATR14/SMA20(%)가 이 값을 넘는 날은 새로 사지 않는다(변동 큰 종목 회피)
    entry_open_deviation_min_percent: float = -99.0  # 개장 봉 종가의 SMA20 이격(%)이 이 범위 밖인 날은 새로 사지 않는다
    entry_open_deviation_max_percent: float = 99.0


VARIANTS = {
    "v1_current":         Params(buy_split_steps=1),
    "v2_norung":          Params(buy_split_steps=0),
    "v3_norung_stop2.5":  Params(buy_split_steps=0, stop_loss_pct=2.5),
    "v4_norung_stop_trail": Params(buy_split_steps=0, stop_loss_pct=2.5, trail_sma_exit=True),
}

# 16_trendx_execution 검증 격자. 기저 v3(=라이브 config_dev_paper TRENDX)에서 한 번에 하나만 바꾼다.
#   atr* — 손절을 고정 2.5%에서 일봉 ATR14 배수로. 1.5~2.5배는 일중 기준으로 아주 넓어(중앙 9~16%)
#          사실상 손절 없음에 가깝다. 라이브 2.5%와 폭이 비슷한 0.3·0.5배를 같이 둔다.
#   delay* — 진입을 존 활성화 즉시가 아니라 닫힌 3분봉 N개 연속 종가 상승 확인 뒤로 미룬다.
EXEC_VARIANTS = {
    "e0_base_stop2.5":  Params(buy_split_steps=0, stop_loss_pct=2.5),
    "e1_atr0.3":        Params(buy_split_steps=0, stop_atr_mult=0.3),
    "e2_atr0.5":        Params(buy_split_steps=0, stop_atr_mult=0.5),
    "e3_atr1.5":        Params(buy_split_steps=0, stop_atr_mult=1.5),
    "e4_atr2.0":        Params(buy_split_steps=0, stop_atr_mult=2.0),
    "e5_atr2.5":        Params(buy_split_steps=0, stop_atr_mult=2.5),
    "e6_delay1":        Params(buy_split_steps=0, stop_loss_pct=2.5, entry_confirm_bars=1),
    "e7_delay2":        Params(buy_split_steps=0, stop_loss_pct=2.5, entry_confirm_bars=2),
    "e8_delay3":        Params(buy_split_steps=0, stop_loss_pct=2.5, entry_confirm_bars=3),
    "e9_nostop":        Params(buy_split_steps=0),
}

# 라이브 DEVSCALE 눌림 슬리브(Quant/config/config_dev_paper.json 첫 DEVIATION_SCALE). 존 −8%~+5%(유지 +4), 3분봉 SMA20
#  기준선, 베이스 = 자본 6,800만×3.2% ≈ 218만, 분할 매수 없음, 재진입 대기 600초(4봉).
#  손절 뒤 대기 21600초는 하루를 넘으므로 120봉(=당일 재진입 없음).
_LIVE = Params(pullback_percent=8.0, entry_upper_pct=5.0, entry_lower_pct=0.0, zone_hyst_pct=4.0, sma_period=20,
               base_on_price=False, buy_split_steps=0, notional_krw=4_350_000.0, base_pct=0.032, max_pct=0.064,
               reentry_cooldown_bars=4, stop_cooldown_bars=120,
               dev_sell_pct=1.2, sell_base_average=True, split_step_count=1, stop_loss_pct=2.0)


def _live_grid() -> dict[str, Params]:
    """09-21 라이브(l0)를 기준으로 손절·익절·트레일을 한 번에 하나씩 바꾼 격자 + 09-20 전 설정(old)."""
    out = {
        "old_09-04":  replace(_LIVE, sell_base_average=False, dev_sell_pct=1.5, split_step_count=2, stop_loss_pct=0.0),
        "l0_live":    _LIVE,
    }
    for stop in (0.0, 1.5, 2.5, 3.0):
        out[f"stop{stop:.1f}"] = replace(_LIVE, stop_loss_pct=stop)
    for take in (0.8, 1.0, 1.5, 2.0, 3.0):
        out[f"take{take:.1f}"] = replace(_LIVE, dev_sell_pct=take)
    for arm, trail in ((1.0, 1.0), (1.0, 0.7), (1.5, 1.0), (0.7, 0.7), (2.0, 1.0)):
        # 트레일은 지정가 익절과 같이 둔다(먼저 닿는 쪽). 익절선을 트레일 무장선 위로 올려 트레일이 주된 청산이 되게 한다.
        out[f"trail_arm{arm:.1f}_t{trail:.1f}"] = replace(_LIVE, trail_arm_percent=arm, trail_percent=trail, dev_sell_pct=3.0)
    out["trail_arm1.0_t1.0_take1.2"] = replace(_LIVE, trail_arm_percent=1.0, trail_percent=1.0)
    out["trail_arm1.0_t1.0_take2.0"] = replace(_LIVE, trail_arm_percent=1.0, trail_percent=1.0, dev_sell_pct=2.0)
    return out


def _entry_grid() -> dict[str, Params]:
    """진입 시각 격자 — 라이브(l0)와 트레일(arm1.0/t1.0, 익절 3.0)에 개장 뒤 대기를 붙인다."""
    trail = replace(_LIVE, trail_arm_percent=1.0, trail_percent=1.0, dev_sell_pct=3.0)
    out = {}
    for from_hhmm in (0, 930, 1000, 1030, 1100, 1300):
        out[f"live_from{from_hhmm:04d}"] = replace(_LIVE, entry_from_hhmm=from_hhmm)
        out[f"trail_from{from_hhmm:04d}"] = replace(trail, entry_from_hhmm=from_hhmm)
    return out


ENTRY_VARIANTS = _entry_grid()


def _carry_grid() -> dict[str, Params]:
    """하룻밤 넘김 격자 — 15:15 강제 청산을 끄고 익절·손절·트레일·존 이탈로만 판다. 보유 상한 일수도 본다."""
    out = {"live_day": _LIVE}
    for take in (1.2, 2.0, 3.0, 5.0):
        for stop in (1.5, 2.0, 3.0):
            out[f"carry_take{take}_stop{stop}"] = replace(_LIVE, carry_overnight=True, dev_sell_pct=take, stop_loss_pct=stop)
    for arm, trail in ((1.0, 1.0), (1.5, 1.5), (2.0, 2.0), (3.0, 2.0)):
        out[f"carry_trail{arm}_t{trail}_take5"] = replace(_LIVE, carry_overnight=True, dev_sell_pct=5.0, stop_loss_pct=2.0,
                                                          trail_arm_percent=arm, trail_percent=trail)
    for days in (2, 3, 5):
        out[f"carry_take3.0_stop2.0_max{days}d"] = replace(_LIVE, carry_overnight=True, dev_sell_pct=3.0, stop_loss_pct=2.0,
                                                          carry_max_days=days)
    return out


CARRY_VARIANTS = _carry_grid()


def _carry_filter_grid() -> dict[str, Params]:
    """하룻밤 넘김 + 진입 필터 격자 — 넘김에서 좋았던 익절 1.0~1.5·손절 3~5에 ATR·개장 이격 필터를 얹는다."""
    filters = {"f0": {}, "f_atr5_dev0_5": dict(entry_atr_max_percent=5.0, entry_open_deviation_min_percent=0.0, entry_open_deviation_max_percent=5.0),
               "f_atr5_devm3": dict(entry_atr_max_percent=5.0, entry_open_deviation_min_percent=-3.0),
               "f_atr5": dict(entry_atr_max_percent=5.0), "f_devm3": dict(entry_open_deviation_min_percent=-3.0),
               "f_atr4_devm3": dict(entry_atr_max_percent=4.0, entry_open_deviation_min_percent=-3.0),
               "f_atr6_devm3": dict(entry_atr_max_percent=6.0, entry_open_deviation_min_percent=-3.0)}
    out = {}
    for take in (1.2,):
        for stop in (3.0, 5.0):
            for fname, filter_values in filters.items():
                out[f"c_take{take}_stop{stop}_{fname}"] = replace(_LIVE, carry_overnight=True, dev_sell_pct=take, stop_loss_pct=stop, **filter_values)
    return out


CARRY_FILTER_VARIANTS = _carry_filter_grid()

# 넘김 + 필터에서 가장 나았던 조합(익절 1.2·손절 3.0·ATR≤5%·개장 이격≥−3%) — 건당 금액·넓은 격자의 기준점
_CARRY_BEST = replace(_LIVE, carry_overnight=True, dev_sell_pct=1.2, stop_loss_pct=3.0,
                      entry_atr_max_percent=5.0, entry_open_deviation_min_percent=-3.0)


def _notional_grid() -> dict[str, Params]:
    """건당 매수 금액 격자 — 규칙은 _CARRY_BEST 그대로 두고 금액만 바꿔 손익이 금액에 비례하는지(체결 가정이 유지되는지) 본다."""
    return {f"n{int(notional // 10_000)}만": replace(_CARRY_BEST, notional_krw=float(notional))
            for notional in (4_350_000, 7_000_000, 10_000_000, 15_000_000)}


NOTIONAL_VARIANTS = _notional_grid()


def _carry_wide_grid() -> dict[str, Params]:
    """넘김 + 필터 넓은 격자(108개) — 익절 4 × 손절 3 × ATR 상한 3 × 개장 이격 하한 3. 표본을 많이 만들어 고른다."""
    out = {}
    for take in (1.0, 1.2, 1.5, 2.0):
        for stop in (3.0, 4.0, 5.0):
            for atr in (4.0, 4.5, 5.0):
                for deviation in (-3.0, -1.0, 0.0):
                    out[f"w_take{take}_stop{stop}_atr{atr}_dev{deviation}"] = replace(
                        _LIVE, carry_overnight=True, dev_sell_pct=take, stop_loss_pct=stop,
                        entry_atr_max_percent=atr, entry_open_deviation_min_percent=deviation)
    return out


CARRY_WIDE_VARIANTS = _carry_wide_grid()


def _carry_wide2_grid() -> dict[str, Params]:
    """넓은 격자 2차(36개) — 1차에서 익절↑·손절↑·ATR↓가 전부 단조로 좋아져 그 방향으로 더 민다. 손절 0은 손절 없음."""
    out = {}
    for take in (2.0, 2.5, 3.0):
        for stop in (5.0, 6.0, 8.0, 0.0):
            for atr in (3.5, 4.0, 5.0):
                out[f"w2_take{take}_stop{stop}_atr{atr}_dev-1.0"] = replace(
                    _LIVE, carry_overnight=True, dev_sell_pct=take, stop_loss_pct=stop,
                    entry_atr_max_percent=atr, entry_open_deviation_min_percent=-1.0)
    return out


CARRY_WIDE2_VARIANTS = _carry_wide2_grid()


def _carry_wide3_grid() -> dict[str, Params]:
    """넓은 격자 3차(45개) — 2차에서도 익절 3.0이 끝단이라 더 민다(8.0은 사실상 존 이탈·손절로만 파는 '보유' 변형).
    손절은 2차에서 5가 가장 나빠 6·8·없음만, ATR은 4(건당 수익)와 5(달 안정) 사이 4.5를 넣는다."""
    out = {}
    for take in (3.0, 3.5, 4.0, 5.0, 8.0):
        for stop in (6.0, 8.0, 0.0):
            for atr in (4.0, 4.5, 5.0):
                out[f"w3_take{take}_stop{stop}_atr{atr}_dev-1.0"] = replace(
                    _LIVE, carry_overnight=True, dev_sell_pct=take, stop_loss_pct=stop,
                    entry_atr_max_percent=atr, entry_open_deviation_min_percent=-1.0)
    return out


CARRY_WIDE3_VARIANTS = _carry_wide3_grid()


def _carry_fine_grid() -> dict[str, Params]:
    """고른 칸 주변 촘촘한 격자(20개) — 3차까지의 후보(익절 3.0·손절 6·ATR 4~5)가 뾰족한 최댓값인지 이웃 칸으로 확인한다.
    손절 5.5~7, ATR 4~6(ATR 4.5가 4·5보다 나빠 축이 매끈하지 않았다)."""
    out = {}
    for stop in (5.5, 6.0, 6.5, 7.0):
        for atr in (4.0, 4.5, 5.0, 5.5, 6.0):
            out[f"f_take3.0_stop{stop}_atr{atr}_dev-1.0"] = replace(
                _LIVE, carry_overnight=True, dev_sell_pct=3.0, stop_loss_pct=stop,
                entry_atr_max_percent=atr, entry_open_deviation_min_percent=-1.0)
    return out


CARRY_FINE_VARIANTS = _carry_fine_grid()


LIVE_VARIANTS = _live_grid()


def resample(df1: pd.DataFrame, n: int) -> pd.DataFrame:
    """1분봉 → n분봉. scripts/dashboard_server.py::_resample과 같은 절대 분 버킷(09:00·09:03…)."""
    mins = df1["time"].str[:2].astype(int) * 60 + df1["time"].str[2:4].astype(int)
    key = mins // n
    g = df1.groupby(key, sort=True)
    out = g.agg(open=("open", "first"), high=("high", "max"), low=("low", "min"),
                close=("close", "last"), volume=("volume", "sum"))
    out["hhmm"] = [(k * n) // 60 * 100 + (k * n) % 60 for k in out.index]
    return out.reset_index(drop=True)


class DailyBook:
    """전일 확정 일봉 SMA·ATR14. 일봉 parquet(~2026-09-04) 뒤는 분봉 파일의 일중 고저종으로 잇는다."""

    def __init__(self, tickers: set[str]):
        d = pd.read_parquet(DAILY_PARQUET, columns=["Date", "code", "High", "Low", "Close"])
        d = d[d["code"].isin(tickers)]
        self.bars: dict[str, pd.DataFrame] = {}
        for code, g in d.groupby("code"):
            b = g.set_index("Date")[["High", "Low", "Close"]].astype(float)
            b.index = b.index.strftime("%Y%m%d")
            self.bars[code] = b
        self._extended: set[str] = set()

    def _extend_from_minute(self, t: str) -> None:
        if t in self._extended:
            return
        self._extended.add(t)
        b = self.bars.get(t)
        if b is None:
            b = pd.DataFrame(columns=["High", "Low", "Close"], dtype=float)
        last = b.index.max() if len(b) else "00000000"
        extra = {}
        for p in sorted((MINUTE_DIR / t).glob("*.parquet")):
            day = p.stem
            if day <= last:
                continue
            m = pd.read_parquet(p, columns=["high", "low", "close"])
            if len(m) >= 300:
                extra[day] = (float(m["high"].max()), float(m["low"].min()), float(m["close"].iloc[-1]))
        if extra:
            add = pd.DataFrame.from_dict(extra, orient="index", columns=["High", "Low", "Close"])
            self.bars[t] = pd.concat([b, add]).sort_index()

    def _bars_before(self, ticker: str, day: str) -> pd.DataFrame | None:
        self._extend_from_minute(ticker)
        bars = self.bars.get(ticker)
        if bars is None:
            return None
        return bars[bars.index < day]

    def moving_averages_before(self, ticker: str, day: str) -> dict | None:
        """ymd 전 거래일까지의 SMA5/10/20/60. 60일이 안 되면 None."""
        bars = self._bars_before(ticker, day)
        if bars is None or len(bars) < 60:
            return None
        closes = bars["Close"].to_numpy()
        return {window: float(closes[-window:].mean()) for window in (5, 10, 20, 60)}

    def is_next_trading_day(self, ticker: str, previous_ymd: str | None, day: str) -> bool:
        """previous_ymd 바로 다음 거래일이 ymd인가(이 종목 일봉 기준). prev가 없으면 False."""
        if previous_ymd is None:
            return False
        bars = self.bars.get(ticker)
        return False if bars is None else bool(len(bars[(bars.index > previous_ymd) & (bars.index < day)]) == 0)

    def atr14_before(self, ticker: str, day: str) -> float:
        """ymd 전 거래일까지의 일봉 ATR14(참범위 14일 단순평균). 14일이 안 되면 0."""
        bars = self._bars_before(ticker, day)
        if bars is None or len(bars) < 15:
            return 0.0
        bars = bars.iloc[-15:]
        previous_close = bars["Close"].shift(1)
        true_range = pd.concat([(bars["High"] - bars["Low"]).abs(),
                        (bars["High"] - previous_close).abs(),
                        (bars["Low"] - previous_close).abs()], axis=1).max(axis=1)
        return float(true_range.iloc[1:].mean())


def aligned(sm: dict, tol_pct: float) -> bool:
    return sm[5] > sm[10] and sm[10] > sm[20] and sm[20] > sm[60] * (1.0 - tol_pct / 100.0)


def replay_day(bars: pd.DataFrame, moving_averages: dict, parameters: Params, cost: CostSpec, atr14: float = 0.0,
               carry: dict | None = None) -> dict:
    """하루 리플레이. 반환: 체결 수·손익·MAE·최대 투입 명목.

    atr14는 **전일 확정 일봉**까지로 만든 ATR14(원). parameters.stop_atr_mult>0일 때만 쓴다.
    carry는 전날에서 넘어온 보유(carry_overnight일 때) — 반환 dict의 "carry"에 오늘 마감 보유를 담아 되돌린다.
    """
    al = aligned(moving_averages, parameters.align_tol_pct)
    average_20 = moving_averages[20]
    base_share = parameters.base_pct / parameters.max_pct if parameters.max_pct > parameters.base_pct else 1.0
    base_notional = parameters.notional_krw * base_share
    split_step_notional = (parameters.notional_krw - base_notional) / parameters.buy_split_steps if parameters.buy_split_steps > 0 else 0.0

    position, average = 0, 0.0
    cost_basis = 0.0                   # 보유분 매입 원가(비용 포함)
    max_deployed = 0.0
    held_days = 0                      # 넘어온 보유의 경과 일수(carry_max_days용)
    if carry:
        position, average, cost_basis = carry["position"], carry["average"], carry["cost_basis"]
        held_days = carry["held_days"] + 1
        max_deployed = cost_basis
    realized = 0.0                     # 순손익(비용 반영)
    gross = 0.0
    buys = sells = 0
    buy_notional = sell_notional = 0.0
    orders: list[tuple] = []           # (kind, price, qty, tag)  kind ∈ BUY/SELL/MKT
    in_zone = False
    cooldown_until = -1
    reentry_until = -1                 # 전량 청산 뒤 새 베이스 대기(봉 번호)
    peak_high = carry["peak_high"] if carry else 0.0   # 보유 중 고가(트레일)
    mae = 0.0
    # 하루 단위 진입 필터 — 전일 ATR과 개장 봉 이격은 첫 봉이 닫히면 아는 값이라 룩어헤드가 아니다.
    open_deviation_percent = (float(bars.iloc[0].close) - average_20) / average_20 * 100.0
    atr_percent = atr14 / average_20 * 100.0 if atr14 > 0 else 0.0
    entry_allowed = ((parameters.entry_atr_max_percent <= 0.0 or atr_percent <= parameters.entry_atr_max_percent)
                     and parameters.entry_open_deviation_min_percent <= open_deviation_percent <= parameters.entry_open_deviation_max_percent)
    exits: dict[str, int] = {}
    closes: list[float] = []
    n = len(bars)
    done = False

    def fill_buy(price: float, quantity: int) -> None:
        nonlocal position, average, cost_basis, buys, buy_notional, max_deployed
        buy_net = fill_result("BUY", price, quantity, cost).net
        average = (average * position + price * quantity) / (position + quantity)
        position += quantity
        cost_basis += buy_net
        max_deployed = max(max_deployed, cost_basis)
        buys += 1
        buy_notional += price * quantity

    def fill_sell(price: float, quantity: int, tag: str) -> None:
        nonlocal position, average, cost_basis, realized, gross, sells, sell_notional, peak_high, reentry_until
        quantity = min(quantity, position)
        if quantity <= 0:
            return
        proceeds = fill_result("SELL", price, quantity, cost).net
        basis = cost_basis * quantity / position
        realized += proceeds - basis
        gross += (price - average) * quantity
        cost_basis -= basis
        position -= quantity
        sells += 1
        sell_notional += price * quantity
        exits[tag] = exits.get(tag, 0) + 1
        if position == 0:
            average, cost_basis = 0.0, 0.0
            peak_high = 0.0
            reentry_until = current_bar + 1 + parameters.reentry_cooldown_bars

    arr = bars[["open", "high", "low", "close", "hhmm"]].to_numpy(dtype=float)   # 봉당 .iloc 회피(같은 값)
    current_bar = 0
    for i in range(n):
        current_bar = i
        o, h, l, c = float(arr[i, 0]), float(arr[i, 1]), float(arr[i, 2]), float(arr[i, 3])
        hhmm = int(arr[i, 4])
        # 1) 직전 봉에서 낸 주문을 이 봉 범위로 체결 판정한다. 하락봉이면 BUY부터, 상승봉이면 SELL부터.
        up = c >= o
        for kind, price, quantity, tag in sorted(orders, key=lambda x: (x[0] != ("SELL" if up else "BUY"))):
            if kind == "MKT":
                fill_sell(round_tick(o - tick_size(o), "BUY"), quantity, tag)
            elif kind == "BUY":
                if o <= price:
                    fill_buy(o, quantity)
                elif l <= price:
                    fill_buy(price, quantity)
            elif kind == "SELL":
                if o >= price:
                    fill_sell(o, quantity, tag)
                elif h >= price:
                    fill_sell(price, quantity, tag)
        orders = []
        closes.append(c)
        if position > 0 and average > 0:
            mae = min(mae, (l / average - 1.0) * 100.0)
            peak_high = max(peak_high, h)
        if done:
            continue
        # 2) 존 게이트 — 전일 확정 SMA, 현재가는 봉 종가
        deviation = (c - average_20) / average_20 * 100.0
        up_threshold = parameters.entry_upper_pct + (parameters.zone_hyst_pct if in_zone else 0.0)
        if parameters.entry_lower_pct > 0.0:
            low_threshold = parameters.entry_lower_pct - (parameters.zone_hyst_pct if in_zone else 0.0)
        else:                                          # 눌림 슬리브: SMA20 아래 −pullback(유지는 −(pullback+hyst))
            low_threshold = -(parameters.pullback_percent + (parameters.zone_hyst_pct if in_zone else 0.0))
        band = low_threshold <= deviation <= up_threshold
        zone = al and band
        hold_zone = zone
        in_zone = zone
        last_bar = i == n - 1

        def liquidate(tag: str) -> None:
            nonlocal orders
            if position > 0:
                if last_bar:
                    fill_sell(c, position, tag)
                else:
                    orders = [("MKT", 0.0, position, tag)]

        if hhmm >= parameters.market_close_hhmm:
            if not parameters.carry_overnight or (parameters.carry_max_days > 0 and held_days >= parameters.carry_max_days):
                liquidate("market_close")
            else:
                orders = []                          # 미체결 주문만 거두고 보유는 내일로
            done = True
            continue
        if not hold_zone:
            liquidate("zone_exit")
            continue
        stop_price = 0.0                     # 0이면 손절 없음
        if parameters.stop_atr_mult > 0 and atr14 > 0:
            stop_price = average - parameters.stop_atr_mult * atr14
        elif parameters.stop_loss_pct > 0:
            stop_price = average * (1.0 - parameters.stop_loss_pct / 100.0)
        if stop_price > 0 and position > 0 and c <= stop_price:
            liquidate("stop")
            cooldown_until = i + parameters.stop_cooldown_bars
            continue
        if (parameters.trail_arm_percent > 0 and position > 0 and average > 0
                and peak_high >= average * (1.0 + parameters.trail_arm_percent / 100.0)
                and c <= peak_high * (1.0 - parameters.trail_percent / 100.0)):
            liquidate("trail_peak")
            cooldown_until = i + parameters.stop_cooldown_bars
            continue
        if not zone:
            continue
        warming = len(closes) < parameters.sma_period
        simple_moving_average = average_20 if warming else float(np.mean(closes[-parameters.sma_period:]))
        if parameters.trail_sma_exit and not warming and position > 0 and c < simple_moving_average * (1.0 - parameters.trail_sma_tol_pct / 100.0):
            liquidate("trail")
            cooldown_until = i + parameters.stop_cooldown_bars     # 엔진도 트레일 뒤 같은 쿨다운(L469)
            continue
        in_cooldown = i < cooldown_until
        room = parameters.max_notional_per_ticker - position * average       # 게이트 명목 상한(전략은 누적 상한이 없다)
        plan = []
        # 진입 확인 — 닫힌 봉만 본다. closes[-1]은 방금 닫힌 이 봉이고 주문은 다음 봉에서 체결되므로
        # 미완성 봉 종가를 보는 look-ahead가 아니다. N=2면 closes[-1]>closes[-2]>closes[-3].
        confirmed = True
        if parameters.entry_confirm_bars > 0:
            need = parameters.entry_confirm_bars + 1
            confirmed = len(closes) >= need and all(
                closes[-split_step_index] > closes[-split_step_index - 1] for split_step_index in range(1, parameters.entry_confirm_bars + 1))
        if (position <= 0 and not in_cooldown and confirmed and i >= reentry_until and hhmm >= parameters.entry_from_hhmm
                and entry_allowed):
            buy_price = round_tick(c if parameters.base_on_price else min(simple_moving_average, c), "BUY")
            if buy_price >= c:
                buy_price = round_tick(c - tick_size(c), "BUY")
            quantity = int(base_notional // buy_price)
            if quantity > 0:
                plan.append(("BUY", buy_price, quantity, "base"))
        if position > 0:
            per = math.ceil(position / parameters.split_step_count) if parameters.split_step_count > 0 else position
            left = position
            sell_base = average if parameters.sell_base_average else max(simple_moving_average, c)
            for split_step_index in range(1, parameters.split_step_count + 1):
                if left <= 0:
                    break
                sell_price = round_tick(sell_base * (1.0 + parameters.dev_sell_pct * split_step_index / 100.0), "SELL")
                if sell_price <= c:
                    sell_price = round_tick(c, "SELL")
                quantity = min(per, left)
                plan.append(("SELL", sell_price, quantity, "tp"))
                left -= quantity
        if parameters.buy_split_steps > 0 and not warming and not in_cooldown:
            for split_step_index in range(1, parameters.buy_split_steps + 1):
                buy_price = round_tick(c * (1.0 - parameters.dev_buy_pct * split_step_index / 100.0), "BUY")
                quantity = int(min(split_step_notional, max(room, 0.0)) // buy_price)
                if quantity > 0:
                    plan.append(("BUY", buy_price, quantity, "split_step"))
        orders = plan

    carry_out = None
    if position > 0 and parameters.carry_overnight and done:   # 마감까지 갔고 보유가 남았으면 내일로
        carry_out = {"position": position, "average": average, "cost_basis": cost_basis, "peak_high": peak_high,
                     "held_days": held_days, "last_close": closes[-1]}
    elif position > 0:                    # 마지막 봉까지 남았으면 종가 청산(자료 절단)
        fill_sell(closes[-1], position, "trunc")
    ret_pct = realized / max_deployed * 100.0 if max_deployed > 0 else 0.0
    return {"buys": buys, "sells": sells, "buy_notional": buy_notional, "sell_notional": sell_notional,
            "gross_pnl": gross, "net_pnl": realized, "max_deployed": max_deployed, "ret_pct": ret_pct,
            "r": ret_pct / R_DENOM_PCT, "mae_pct": mae, "exits": json.dumps(exits, ensure_ascii=False),
            "carry": carry_out}


def close_carry(carry: dict, cost: CostSpec) -> dict:
    """다음 날 분봉이 없어 더 못 넘기는 보유를 마지막 종가로 정리한 손익 행(carry_end)."""
    price, quantity = carry["last_close"], carry["position"]
    proceeds = fill_result("SELL", price, quantity, cost).net
    return {"buys": 0, "sells": 1, "buy_notional": 0.0, "sell_notional": price * quantity,
            "gross_pnl": (price - carry["average"]) * quantity, "net_pnl": proceeds - carry["cost_basis"],
            "max_deployed": carry["cost_basis"], "ret_pct": 0.0, "r": 0.0, "mae_pct": 0.0,
            "exits": json.dumps({"carry_end": 1}, ensure_ascii=False), "carry": None}


def load_pairs(path: Path) -> list[tuple[str, str]]:
    out = []
    text = path.read_text(encoding="utf-8")
    if text.lstrip().startswith("{"):                  # minute_backfill_pairs.py: {"YYYYMMDD": [ticker, ...]}
        for day, tickers in json.loads(text).items():
            out += [(str(ticker).zfill(6), str(day)) for ticker in tickers]
        return out
    for line in text.splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        out += [(str(t).zfill(6), str(rec["ymd"])) for t in rec["tickers"]]
    return out


def _replay_pairs(pairs: list[tuple[str, str]], variants: dict[str, Params], since: str, until: str) -> tuple[list[dict], dict]:
    """한 프로세스 몫. 반환: 행 목록과 건너뛴 사유별 수."""
    cost = LIVE
    book = DailyBook({t for t, _ in pairs})
    rows, skipped = [], {"missing": 0, "short": 0, "no_sma": 0}
    carries: dict[tuple[str, str], dict] = {}      # (variant, ticker) → 어제 마감 보유(carry_overnight)
    last_ymd: dict[str, str] = {}                  # ticker → 마지막으로 재생한 날짜(연속성 판정)

    def flush_carry(name: str, ticker: str, day: str) -> None:
        carry = carries.pop((name, ticker), None)
        if carry:
            rows.append({"variant": name, "ticker": ticker, "ymd": day, "aligned_prev": False, "atr14_pct": 0.0,
                         "open_dev_pct": 0.0, **close_carry(carry, cost)})

    for ticker, day in sorted(pairs, key=lambda x: (x[0], x[1])):   # 종목별 날짜 순 — 보유를 다음 날로 넘기려면 필수
        if not (since <= day <= until):
            continue
        minute_file = MINUTE_DIR / ticker / f"{day}.parquet"
        if not minute_file.exists():
            skipped["missing"] += 1
            continue
        minute_bars = pd.read_parquet(minute_file)
        if len(minute_bars) < 300:
            skipped["short"] += 1
            continue
        moving_averages = book.moving_averages_before(ticker, day)
        if moving_averages is None:
            skipped["no_sma"] += 1
            continue
        bars = resample(minute_bars, 3)
        atr14 = book.atr14_before(ticker, day)
        contiguous = book.is_next_trading_day(ticker, last_ymd.get(ticker), day)
        last_ymd[ticker] = day
        for name, p in variants.items():
            if not contiguous:                     # 사이에 재생 안 한 날이 있으면 그 보유는 마지막 종가로 정리
                flush_carry(name, ticker, day)
            result = replay_day(bars, moving_averages, p, cost, atr14, carries.pop((name, ticker), None))
            if result["carry"]:
                carries[(name, ticker)] = result["carry"]
            rows.append({"variant": name, "ticker": ticker, "ymd": day, "aligned_prev": aligned(moving_averages, p.align_tol_pct),
                         "atr14_pct": atr14 / moving_averages[20] * 100.0,
                         "open_dev_pct": (float(bars.iloc[0].close) - moving_averages[20]) / moving_averages[20] * 100.0, **result})
    for (name, ticker) in list(carries):
        flush_carry(name, ticker, last_ymd[ticker])
    return rows, skipped


def run(pairs: list[tuple[str, str]], variants: dict[str, Params], since: str, until: str, jobs: int = 1) -> pd.DataFrame:
    """jobs>1이면 종목 단위로 나눠 프로세스마다 돌린다(일봉 장부는 프로세스마다 자기 종목만 읽는다)."""
    if jobs <= 1:
        rows, skipped = _replay_pairs(pairs, variants, since, until)
    else:
        from concurrent.futures import ProcessPoolExecutor
        tickers = sorted({ticker for ticker, _ in pairs})
        chunks = [set(tickers[index::jobs]) for index in range(jobs)]
        parts = [[pair for pair in pairs if pair[0] in chunk] for chunk in chunks]
        rows, skipped = [], {"missing": 0, "short": 0, "no_sma": 0}
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            for part_rows, part_skipped in pool.map(_replay_pairs, parts, [variants] * jobs, [since] * jobs, [until] * jobs):
                rows += part_rows
                for key, value in part_skipped.items():
                    skipped[key] += value
    print(f"[replay] pairs {len(pairs)} · 분봉 없음 {skipped['missing']} · 짧음 {skipped['short']} · SMA 부족 {skipped['no_sma']} · "
          f"리플레이 {len(rows)//max(len(variants),1)}(종목,일)", flush=True)
    return pd.DataFrame(rows)


def monthly_table(df: pd.DataFrame) -> pd.DataFrame:
    """변형×월 요약: 거래 수·평균 R·MAE 중앙값·승률·회전율·비용 비중."""
    traded = df[(df["buys"] > 0) | (df["sells"] > 0)].copy()   # carry: buy without sell one day, sell only next day
    traded["month"] = traded["ymd"].str[:6]
    traded["cost"] = traded["gross_pnl"] - traded["net_pnl"]
    grouped = traded.groupby(["variant", "month"])
    out = grouped.agg(n_days=("ymd", "size"), n_buy_fills=("buys", "sum"), n_sell_fills=("sells", "sum"),
                mean_r=("r", "mean"), mae_med=("mae_pct", "median"),
                win_rate=("net_pnl", lambda s: float((s > 0).mean())),
                turnover=("buy_notional", "sum"), deployed=("max_deployed", "sum"),
                gross=("gross_pnl", "sum"), net=("net_pnl", "sum"), cost=("cost", "sum")).reset_index()
    out["turnover_x"] = out["turnover"] / out["deployed"]
    out["cost_share"] = out["cost"] / out["gross"].abs().where(out["gross"].abs() > 0)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--since", default="00000000")
    ap.add_argument("--until", default="99999999")
    ap.add_argument("--variant", default=None, help="하나만 돌릴 때")
    ap.add_argument("--set", dest="vset", default="base", choices=("base", "exec", "live", "entry", "carry", "carry_filter", "notional", "carry_wide", "carry_wide2", "carry_wide3", "carry_fine"),
                    help="base=v1~v4, exec=16_trendx_execution 격자(ATR 스탑·진입 지연), live=09-21 눌림 슬리브 손절·익절·트레일 격자")
    ap.add_argument("--jobs", type=int, default=1, help="프로세스 수(종목 단위로 나눈다)")
    args = ap.parse_args()
    table = {"base": VARIANTS, "exec": EXEC_VARIANTS, "live": LIVE_VARIANTS, "entry": ENTRY_VARIANTS, "carry": CARRY_VARIANTS, "carry_filter": CARRY_FILTER_VARIANTS,
             "notional": NOTIONAL_VARIANTS, "carry_wide": CARRY_WIDE_VARIANTS,
             "carry_wide2": CARRY_WIDE2_VARIANTS,
             "carry_wide3": CARRY_WIDE3_VARIANTS,
             "carry_fine": CARRY_FINE_VARIANTS}[args.vset]
    variants = table if not args.variant else {args.variant: table[args.variant]}
    pairs = load_pairs(Path(args.pairs))
    df = run(pairs, variants, args.since.replace("-", ""), args.until.replace("-", ""), args.jobs)
    out = Path(args.out)
    df.to_csv(out.with_name(out.stem + "_days.tsv"), sep="\t", index=False, float_format="%.4f")
    mt = monthly_table(df)
    mt.to_csv(out, sep="\t", index=False, float_format="%.4f")
    pd.set_option("display.width", 220)
    print(mt.to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
