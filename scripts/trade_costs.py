#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""체결 원장(logs/trades_YYYYMMDD.csv)에서 날짜별·종목별 매매 비용과 실현손익을 뽑아 누적 파일로 남긴다.

거래가 잦은 날은 비용이 손익만큼 커진다(09-08~09-14 실측: 비용 1.09M vs 실현 +0.18M). 그 경계를 계속 보려면
날마다 같은 기준으로 숫자를 쌓아야 하고, 이 스크립트가 그 한 곳이다. 요율은 인자로 준다(실계좌 명세와 맞춘다).

  py scripts/trade_costs.py                       # 전 원장 → logs/trade_costs.json + 표
  py scripts/trade_costs.py --days 7              # 최근 7일 표만
  py scripts/trade_costs.py --symbol 005930       # 종목 하나의 일별
  py scripts/trade_costs.py --log-dir Quant/build_win/logs --commission 0.00014 --tax 0.0015

산출 logs/trade_costs.json:
  {"rates": {...}, "days": {"20260914": {"fills", "buy", "sell", "commission", "tax", "cost", "realized",
   "symbols": {"005930": {"fills","buy","sell","cost","realized"}}}}, "cumulative": {"cost","realized","fills"}}
실현손익은 원장의 realized_pnl 열(09-07부터)이고, 열이 없는 날은 null로 둔다 — 비용만 있는 날과 구분하기 위해.
"""
from __future__ import annotations

import argparse
import csv
import glob
import io
import json
import os
import sys
from collections import defaultdict

sys.stdout.reconfigure(encoding="utf-8")

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if os.path.join(_REPO, "PYQuant") not in sys.path:
    sys.path.insert(0, os.path.join(_REPO, "PYQuant"))
from backtest.costs import LIVE  # noqa: E402  요율은 라이브 원장과 한 소스


def _empty() -> dict:
    return {"fills": 0, "buy": 0.0, "sell": 0.0, "commission": 0.0, "tax": 0.0, "cost": 0.0, "realized": None}


def _add(acc: dict, side: str, value: float, commission: float, tax: float, pnl: float | None) -> None:
    acc["fills"] += 1
    acc["buy" if side == "BUY" else "sell"] += value
    acc["commission"] += value * commission
    if side != "BUY":
        acc["tax"] += value * tax
    acc["cost"] = acc["commission"] + acc["tax"]
    if pnl is not None:
        acc["realized"] = (acc["realized"] or 0.0) + pnl


def build(log_dir: str, commission: float, tax: float) -> dict:
    days: dict[str, dict] = {}
    for path in sorted(glob.glob(os.path.join(log_dir, "trades_20*.csv"))):
        day = os.path.basename(path)[7:15]
        acc = _empty()
        syms: dict[str, dict] = defaultdict(_empty)
        with io.open(path, encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.DictReader(f):
                if row.get("event") != "FILL":
                    continue
                qty = float(row.get("fill_qty") or 0)
                px = float(row.get("fill_price") or 0)
                if qty <= 0 or px <= 0:
                    continue
                side = row.get("side", "")
                rp = (row.get("realized_pnl") or "").strip()
                pnl = float(rp) if rp else None
                _add(acc, side, qty * px, commission, tax, pnl)
                _add(syms[row.get("ticker", "")], side, qty * px, commission, tax, pnl)
        if acc["fills"] == 0:
            continue
        acc["symbols"] = dict(sorted(syms.items(), key=lambda kv: -(kv[1]["realized"] or 0.0)))
        days[day] = acc
    cum = {"fills": sum(d["fills"] for d in days.values()),
           "cost": sum(d["cost"] for d in days.values()),
           "realized": sum(d["realized"] or 0.0 for d in days.values())}
    return {"rates": {"commission": commission, "tax": tax}, "days": days, "cumulative": cum}


def _k(v: float) -> str:
    return f"{v / 1e3:.0f}K"


def _pnl(v: float | None) -> str:
    return "(열 없음)" if v is None else f"{v / 1e3:+.0f}K"


def print_days(doc: dict, days: int) -> None:
    items = list(doc["days"].items())[-days:] if days > 0 else list(doc["days"].items())
    print("| 날짜 | 체결 | 매수대금 | 매도대금 | 수수료 | 거래세 | 비용합 | 실현손익 | 비용/|손익| |")
    print("|---|---|---|---|---|---|---|---|---|")
    for day, d in items:
        ratio = "—" if not d["realized"] else f"{d['cost'] / abs(d['realized']):.1f}x"
        print(f"| {day[4:6]}-{day[6:8]} | {d['fills']} | {d['buy'] / 1e6:.1f}M | {d['sell'] / 1e6:.1f}M | "
              f"{_k(d['commission'])} | {_k(d['tax'])} | {_k(d['cost'])} | {_pnl(d['realized'])} | {ratio} |")
    c = doc["cumulative"]
    print(f"\n누적: 체결 {c['fills']}건, 비용 {c['cost'] / 1e6:.2f}M, 실현손익 {c['realized'] / 1e6:+.2f}M "
          f"(요율 수수료 {doc['rates']['commission'] * 100:.3f}%·거래세 {doc['rates']['tax'] * 100:.2f}%)")


def print_symbol(doc: dict, symbol: str) -> None:
    print(f"| 날짜 | 체결 | 매수대금 | 매도대금 | 비용 | 실현손익 |  ({symbol})")
    print("|---|---|---|---|---|---|")
    for day, d in doc["days"].items():
        s = d["symbols"].get(symbol)
        if s:
            print(f"| {day[4:6]}-{day[6:8]} | {s['fills']} | {s['buy'] / 1e6:.1f}M | {s['sell'] / 1e6:.1f}M | "
                  f"{_k(s['cost'])} | {_pnl(s['realized'])} |")


def main() -> int:
    ap = argparse.ArgumentParser(description="체결 원장의 매매 비용·실현손익 누적")
    ap.add_argument("--log-dir", default="Quant/build_win/logs")
    ap.add_argument("--commission", type=float, default=LIVE.commission_rate,
                    help="매수·매도 수수료율(기본 backtest/costs.py LIVE = 원장 OrderGate.cpp와 같은 수)")
    ap.add_argument("--tax", type=float, default=LIVE.sell_tax_rate,
                    help="매도 거래세율(농특세 포함, 기본 backtest/costs.py LIVE = 원장 OrderGate.cpp와 같은 수)")
    ap.add_argument("--days", type=int, default=0, help="표에 보일 최근 일수(0=전부)")
    ap.add_argument("--symbol", default=None, help="종목 하나의 일별 표")
    ap.add_argument("--out", default=None, help="누적 JSON 경로(기본 <log-dir>/trade_costs.json)")
    a = ap.parse_args()
    doc = build(a.log_dir, a.commission, a.tax)
    out = a.out or os.path.join(a.log_dir, "trade_costs.json")
    with io.open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    if a.symbol:
        print_symbol(doc, a.symbol)
    else:
        print_days(doc, a.days)
    print(f"기록: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
