#!/usr/bin/env python3
"""DeviationScale TRENDX 슬리브의 3분봉 리플레이 — 실행 층(rung 구간·물타기·스탑) 비교용.

`Quant/include/strategy/DeviationScaleStrategy.h`의 `on_trade_batch` 경로 하나만 옮겼다. 스캐너·주문
라우터·OrderGate는 옮기지 않는다. 옮긴 규칙과 근사는 README(research/studies/13_trendx_gate/README.md)에
표로 있고, 여기서는 코드 옆에 짧게만 적는다.

  - 존 게이트: 정배열·이격 밴드 모두 전일 확정 일봉 SMA로 판정한다(엔진은 진입에 fold_today를 쓰지만
    보수적으로 전일 값). 히스테리시스 진입 5~35 / 유지 1~39.
  - 워밍업: 3분봉 < sma_period(20)이면 기준선 = 일봉 SMA20, BUY rung 차단(베이스 BUY는 허용, 엔진과 같다).
  - rung 구간: 앵커 = 현재가. 베이스 BUY = 현재가 −1틱, SELL rung = 평단×(1+3%), BUY rung = 현재가×(1−1%).
    지정가 체결은 다음 3분봉 범위로 판정(시가가 이미 넘겨 있으면 시가 체결). 부분체결 없음.
  - 재구성: 봉마다 취소·재발주(min_rebuild_sec=8은 틱이 없어 봉 단위로 근사).
  - 존 이탈·하드 스탑·트레일·15:15: 전량 시장가 = 다음 봉 시가 −1틱. 마지막 봉이면 그 종가.
  - 비용: engine.CostModel(수수료 0.015%·세금 0.18%·슬리피지 5bp).

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
from backtest.engine import CostModel  # noqa: E402

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
    n_rungs: int = 1
    buy_rungs: int = 1
    notional_krw: float = 2_500_000.0
    base_pct: float = 0.015
    max_pct: float = 0.022
    stop_loss_pct: float = 0.0
    stop_atr_mult: float = 0.0           # >0이면 평단 − 배수×전일확정 일봉 ATR14를 손절선으로 쓴다(고정 %와 배타)
    stop_cooldown_bars: int = 5          # 900초 / 3분
    entry_confirm_bars: int = 0          # >0이면 직전 닫힌 봉 N개가 연속 종가 상승일 때만 베이스 BUY
    trail_sma_exit: bool = False
    trail_sma_tol_pct: float = 1.0
    eod_hhmm: int = 1515
    interval_min: int = 3
    max_notional_per_ticker: float = 15_000_000.0   # OrderGate risk.max_notional_per_ticker — 전략 밖 상한


VARIANTS = {
    "v1_current":         Params(buy_rungs=1),
    "v2_norung":          Params(buy_rungs=0),
    "v3_norung_stop2.5":  Params(buy_rungs=0, stop_loss_pct=2.5),
    "v4_norung_stop_trail": Params(buy_rungs=0, stop_loss_pct=2.5, trail_sma_exit=True),
}

# 16_trendx_execution 검증 격자. 기저 v3(=라이브 config_dev_paper TRENDX)에서 한 번에 하나만 바꾼다.
#   atr* — 손절을 고정 2.5%에서 일봉 ATR14 배수로. 1.5~2.5배는 일중 기준으로 아주 넓어(중앙 9~16%)
#          사실상 손절 없음에 가깝다. 라이브 2.5%와 폭이 비슷한 0.3·0.5배를 같이 둔다.
#   delay* — 진입을 존 활성화 즉시가 아니라 닫힌 3분봉 N개 연속 종가 상승 확인 뒤로 미룬다.
EXEC_VARIANTS = {
    "e0_base_stop2.5":  Params(buy_rungs=0, stop_loss_pct=2.5),
    "e1_atr0.3":        Params(buy_rungs=0, stop_atr_mult=0.3),
    "e2_atr0.5":        Params(buy_rungs=0, stop_atr_mult=0.5),
    "e3_atr1.5":        Params(buy_rungs=0, stop_atr_mult=1.5),
    "e4_atr2.0":        Params(buy_rungs=0, stop_atr_mult=2.0),
    "e5_atr2.5":        Params(buy_rungs=0, stop_atr_mult=2.5),
    "e6_delay1":        Params(buy_rungs=0, stop_loss_pct=2.5, entry_confirm_bars=1),
    "e7_delay2":        Params(buy_rungs=0, stop_loss_pct=2.5, entry_confirm_bars=2),
    "e8_delay3":        Params(buy_rungs=0, stop_loss_pct=2.5, entry_confirm_bars=3),
    "e9_nostop":        Params(buy_rungs=0),
}


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
            ymd = p.stem
            if ymd <= last:
                continue
            m = pd.read_parquet(p, columns=["high", "low", "close"])
            if len(m) >= 300:
                extra[ymd] = (float(m["high"].max()), float(m["low"].min()), float(m["close"].iloc[-1]))
        if extra:
            add = pd.DataFrame.from_dict(extra, orient="index", columns=["High", "Low", "Close"])
            self.bars[t] = pd.concat([b, add]).sort_index()

    def _prev(self, t: str, ymd: str) -> pd.DataFrame | None:
        self._extend_from_minute(t)
        b = self.bars.get(t)
        if b is None:
            return None
        return b[b.index < ymd]

    def smas_prev(self, t: str, ymd: str) -> dict | None:
        """ymd 전 거래일까지의 SMA5/10/20/60. 60일이 안 되면 None."""
        b = self._prev(t, ymd)
        if b is None or len(b) < 60:
            return None
        v = b["Close"].to_numpy()
        return {n: float(v[-n:].mean()) for n in (5, 10, 20, 60)}

    def atr14_prev(self, t: str, ymd: str) -> float:
        """ymd 전 거래일까지의 일봉 ATR14(참범위 14일 단순평균). 14일이 안 되면 0."""
        b = self._prev(t, ymd)
        if b is None or len(b) < 15:
            return 0.0
        b = b.iloc[-15:]
        prev_close = b["Close"].shift(1)
        tr = pd.concat([(b["High"] - b["Low"]).abs(),
                        (b["High"] - prev_close).abs(),
                        (b["Low"] - prev_close).abs()], axis=1).max(axis=1)
        return float(tr.iloc[1:].mean())


def aligned(sm: dict, tol_pct: float) -> bool:
    return sm[5] > sm[10] and sm[10] > sm[20] and sm[20] > sm[60] * (1.0 - tol_pct / 100.0)


def replay_day(bars: pd.DataFrame, smas: dict, p: Params, cost: CostModel, atr14: float = 0.0) -> dict:
    """하루 리플레이. 반환: 체결 수·손익·MAE·최대 투입 명목.

    atr14는 **전일 확정 일봉**까지로 만든 ATR14(원). p.stop_atr_mult>0일 때만 쓴다.
    """
    al = aligned(smas, p.align_tol_pct)
    s20 = smas[20]
    base_share = p.base_pct / p.max_pct if p.max_pct > p.base_pct else 1.0
    base_notional = p.notional_krw * base_share
    rung_notional = (p.notional_krw - base_notional) / p.buy_rungs if p.buy_rungs > 0 else 0.0

    pos, avg = 0, 0.0
    cost_basis = 0.0                   # 보유분 매입 원가(비용 포함)
    max_deployed = 0.0
    realized = 0.0                     # 순손익(비용 반영)
    gross = 0.0
    buys = sells = 0
    buy_notional = sell_notional = 0.0
    orders: list[tuple] = []           # (kind, price, qty, tag)  kind ∈ BUY/SELL/MKT
    in_zone = False
    cooldown_until = -1
    mae = 0.0
    exits: dict[str, int] = {}
    closes: list[float] = []
    n = len(bars)
    done = False

    def fill_buy(px: float, q: int) -> None:
        nonlocal pos, avg, cost_basis, buys, buy_notional, max_deployed
        c = cost.buy_total_cost(px, q)
        avg = (avg * pos + px * q) / (pos + q)
        pos += q
        cost_basis += c
        max_deployed = max(max_deployed, cost_basis)
        buys += 1
        buy_notional += px * q

    def fill_sell(px: float, q: int, tag: str) -> None:
        nonlocal pos, avg, cost_basis, realized, gross, sells, sell_notional
        q = min(q, pos)
        if q <= 0:
            return
        proceeds = cost.sell_net_proceeds(px, q)
        basis = cost_basis * q / pos
        realized += proceeds - basis
        gross += (px - avg) * q
        cost_basis -= basis
        pos -= q
        sells += 1
        sell_notional += px * q
        exits[tag] = exits.get(tag, 0) + 1
        if pos == 0:
            avg, cost_basis = 0.0, 0.0

    arr = bars[["open", "high", "low", "close", "hhmm"]].to_numpy(dtype=float)   # 봉당 .iloc 회피(같은 값)
    for i in range(n):
        o, h, l, c = float(arr[i, 0]), float(arr[i, 1]), float(arr[i, 2]), float(arr[i, 3])
        hhmm = int(arr[i, 4])
        # 1) 직전 봉에서 낸 주문을 이 봉 범위로 체결 판정한다. 하락봉이면 BUY부터, 상승봉이면 SELL부터.
        up = c >= o
        for kind, px, q, tag in sorted(orders, key=lambda x: (x[0] != ("SELL" if up else "BUY"))):
            if kind == "MKT":
                fill_sell(round_tick(o - tick_size(o), "BUY"), q, tag)
            elif kind == "BUY":
                if o <= px:
                    fill_buy(o, q)
                elif l <= px:
                    fill_buy(px, q)
            elif kind == "SELL":
                if o >= px:
                    fill_sell(o, q, tag)
                elif h >= px:
                    fill_sell(px, q, tag)
        orders = []
        closes.append(c)
        if pos > 0 and avg > 0:
            mae = min(mae, (l / avg - 1.0) * 100.0)
        if done:
            continue
        # 2) 존 게이트 — 전일 확정 SMA, 현재가는 봉 종가
        dev = (c - s20) / s20 * 100.0
        up_th = p.entry_upper_pct + (p.zone_hyst_pct if in_zone else 0.0)
        low_th = p.entry_lower_pct - (p.zone_hyst_pct if in_zone else 0.0)
        band = low_th <= dev <= up_th
        zone = al and band
        hold_zone = zone
        in_zone = zone
        last_bar = i == n - 1

        def liquidate(tag: str) -> None:
            nonlocal orders
            if pos > 0:
                if last_bar:
                    fill_sell(c, pos, tag)
                else:
                    orders = [("MKT", 0.0, pos, tag)]

        if hhmm >= p.eod_hhmm:
            liquidate("eod")
            done = True
            continue
        if not hold_zone:
            liquidate("zone_exit")
            continue
        stop_px = 0.0                     # 0이면 손절 없음
        if p.stop_atr_mult > 0 and atr14 > 0:
            stop_px = avg - p.stop_atr_mult * atr14
        elif p.stop_loss_pct > 0:
            stop_px = avg * (1.0 - p.stop_loss_pct / 100.0)
        if stop_px > 0 and pos > 0 and c <= stop_px:
            liquidate("stop")
            cooldown_until = i + p.stop_cooldown_bars
            continue
        if not zone:
            continue
        warming = len(closes) < p.sma_period
        sma = s20 if warming else float(np.mean(closes[-p.sma_period:]))
        if p.trail_sma_exit and not warming and pos > 0 and c < sma * (1.0 - p.trail_sma_tol_pct / 100.0):
            liquidate("trail")
            cooldown_until = i + p.stop_cooldown_bars     # 엔진도 트레일 뒤 같은 쿨다운(L469)
            continue
        in_cooldown = i < cooldown_until
        room = p.max_notional_per_ticker - pos * avg       # 게이트 명목 상한(전략은 누적 상한이 없다)
        plan = []
        # 진입 확인 — 닫힌 봉만 본다. closes[-1]은 방금 닫힌 이 봉이고 주문은 다음 봉에서 체결되므로
        # 미완성 봉 종가를 보는 look-ahead가 아니다. N=2면 closes[-1]>closes[-2]>closes[-3].
        confirmed = True
        if p.entry_confirm_bars > 0:
            need = p.entry_confirm_bars + 1
            confirmed = len(closes) >= need and all(
                closes[-k] > closes[-k - 1] for k in range(1, p.entry_confirm_bars + 1))
        if pos <= 0 and not in_cooldown and confirmed:
            bp = round_tick(c, "BUY")
            if bp >= c:
                bp = round_tick(c - tick_size(c), "BUY")
            q = int(base_notional // bp)
            if q > 0:
                plan.append(("BUY", bp, q, "base"))
        if pos > 0:
            per = math.ceil(pos / p.n_rungs) if p.n_rungs > 0 else pos
            left = pos
            for k in range(1, p.n_rungs + 1):
                if left <= 0:
                    break
                sp = round_tick(avg * (1.0 + p.dev_sell_pct * k / 100.0), "SELL")
                if sp <= c:
                    sp = round_tick(c, "SELL")
                q = min(per, left)
                plan.append(("SELL", sp, q, "tp"))
                left -= q
        if p.buy_rungs > 0 and not warming and not in_cooldown:
            for k in range(1, p.buy_rungs + 1):
                bp = round_tick(c * (1.0 - p.dev_buy_pct * k / 100.0), "BUY")
                q = int(min(rung_notional, max(room, 0.0)) // bp)
                if q > 0:
                    plan.append(("BUY", bp, q, "rung"))
        orders = plan

    if pos > 0:                      # 마지막 봉까지 남았으면 종가 청산(자료 절단)
        fill_sell(closes[-1], pos, "trunc")
    ret_pct = realized / max_deployed * 100.0 if max_deployed > 0 else 0.0
    return {"buys": buys, "sells": sells, "buy_notional": buy_notional, "sell_notional": sell_notional,
            "gross_pnl": gross, "net_pnl": realized, "max_deployed": max_deployed, "ret_pct": ret_pct,
            "r": ret_pct / R_DENOM_PCT, "mae_pct": mae, "exits": json.dumps(exits, ensure_ascii=False)}


def load_pairs(path: Path) -> list[tuple[str, str]]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        out += [(str(t).zfill(6), str(rec["ymd"])) for t in rec["tickers"]]
    return out


def run(pairs: list[tuple[str, str]], variants: dict[str, Params], since: str, until: str) -> pd.DataFrame:
    cost = CostModel()
    book = DailyBook({t for t, _ in pairs})
    rows, missing, short, no_sma = [], 0, 0, 0
    for t, ymd in pairs:
        if not (since <= ymd <= until):
            continue
        f = MINUTE_DIR / t / f"{ymd}.parquet"
        if not f.exists():
            missing += 1
            continue
        m = pd.read_parquet(f)
        if len(m) < 300:
            short += 1
            continue
        smas = book.smas_prev(t, ymd)
        if smas is None:
            no_sma += 1
            continue
        bars = resample(m, 3)
        atr14 = book.atr14_prev(t, ymd)
        for name, p in variants.items():
            r = replay_day(bars, smas, p, cost, atr14)
            rows.append({"variant": name, "ticker": t, "ymd": ymd, "aligned_prev": aligned(smas, p.align_tol_pct),
                         "atr14_pct": atr14 / smas[20] * 100.0,
                         "open_dev_pct": (float(bars.iloc[0].close) - smas[20]) / smas[20] * 100.0, **r})
    print(f"[replay] pairs {len(pairs)} · 분봉 없음 {missing} · 짧음 {short} · SMA 부족 {no_sma} · "
          f"리플레이 {len(rows)//max(len(variants),1)}(종목,일)", flush=True)
    return pd.DataFrame(rows)


def monthly_table(df: pd.DataFrame) -> pd.DataFrame:
    """변형×월 요약: 거래 수·평균 R·MAE 중앙값·승률·회전율·비용 비중."""
    d = df[(df["buys"] > 0)].copy()
    d["month"] = d["ymd"].str[:6]
    d["cost"] = d["gross_pnl"] - d["net_pnl"]
    g = d.groupby(["variant", "month"])
    out = g.agg(n_days=("ymd", "size"), n_buy_fills=("buys", "sum"), n_sell_fills=("sells", "sum"),
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
    ap.add_argument("--set", dest="vset", default="base", choices=("base", "exec"),
                    help="base=v1~v4, exec=16_trendx_execution 격자(ATR 스탑·진입 지연)")
    args = ap.parse_args()
    table = VARIANTS if args.vset == "base" else EXEC_VARIANTS
    variants = table if not args.variant else {args.variant: table[args.variant]}
    pairs = load_pairs(Path(args.pairs))
    df = run(pairs, variants, args.since.replace("-", ""), args.until.replace("-", ""))
    out = Path(args.out)
    df.to_csv(out.with_name(out.stem + "_days.tsv"), sep="\t", index=False, float_format="%.4f")
    mt = monthly_table(df)
    mt.to_csv(out, sep="\t", index=False, float_format="%.4f")
    pd.set_option("display.width", 220)
    print(mt.to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
