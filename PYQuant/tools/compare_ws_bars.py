#!/usr/bin/env python3
"""WS 틱 집계 3분봉(로그의 `봉 닫힘 src=ws` DEBUG 줄)과 KIS REST 1분봉을 3분으로 모은 봉을 하루치 비교한다.

  P-1 3단계 실측(D-068·D-069). 트레이더를 `bar_source: "ws"`, `log_level: "DEBUG"`로 하루 돌린 뒤 장 마감 후에 돌린다.
  REST 쪽은 과거일 1분봉 TR(FHKST03010230)이라 며칠 뒤에 돌려도 같은 값이 온다.

사용:
  py PYQuant/tools/compare_ws_bars.py --date 2026-09-14 [--log logs/quant_trader.log]
      [--config Quant/config/config_dev_paper.json] [--tickers 005930,000660] [--sma 20] [--out 파일.md]
  py PYQuant/tools/compare_ws_bars.py --selftest     # 파서·집계·비교만 가짜 자료로 확인(REST 안 부름)

표가 답하는 것:
  - 봉 수: 로컬이 빈 구간을 건너뛴 봉 수(REST에는 있고 로컬에 없음), 로컬이 앞서 닫은 봉(REST에 없음).
  - OHLC 일치율과 거래량 비율. 종가는 같아야 정상이고 고저는 빠진 틱만큼 어긋난다.
  - 1분봉 라벨 뜻(D-068 표의 열린 항목): REST 행 시각을 그대로 버킷에 넣은 집계와 1분 당긴 집계 중 어느 쪽이
    로컬 시가와 더 맞는지로 판정한다.
  - SMA(sma 기간) 차이의 최대·평균(%) — 전략이 실제로 쓰는 값.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "PYQuant"))

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

LINE_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2}):\d{2}\.\d+ \[DEBUG\] \[[A-Z]+_(\d{6})\] 봉 닫힘 src=ws t=(\d{4}) "
    r"o=([\d.]+) h=([\d.]+) l=([\d.]+) c=([\d.]+) v=(\d+)"
)


def kst_date_of(utc_date: str, utc_hh: int) -> str:
    """로그 시각은 UTC다. 15시 이후(UTC)면 KST로는 다음 날."""
    from datetime import date, timedelta

    d = date.fromisoformat(utc_date)
    if utc_hh >= 15:
        d += timedelta(days=1)
    return d.isoformat()


def parse_local(log_path: Path, kst_date: str) -> dict[str, dict[str, dict]]:
    """{ticker: {HHMM: bar}} — 같은 자리가 두 번 닫히면(재기동·재시드) 뒤 것이 남는다."""
    out: dict[str, dict[str, dict]] = defaultdict(dict)
    with log_path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line)
            if not m:
                continue
            udate, uhh, _umm, ticker, t, o, h, l, c, v = m.groups()
            if kst_date_of(udate, int(uhh)) != kst_date:
                continue
            out[ticker][t] = {"open": float(o), "high": float(h), "low": float(l), "close": float(c), "volume": int(v)}
    return out


def aggregate(rows: list[dict], interval_min: int = 3, shift_min: int = 0) -> dict[str, dict]:
    """1분봉(오래된→최신, time=HHMM)을 C++ aggregate_minutes와 같은 규칙으로 모은다.
    shift_min=-1이면 행 시각을 1분 당겨 '라벨=분의 끝' 가설을 본다. 키는 버킷 시작 HHMM."""
    buckets: dict[int, list[dict]] = defaultdict(list)
    for r in rows:
        hh, mm = int(r["time"][:2]), int(r["time"][2:4])
        tot = hh * 60 + mm + shift_min
        if tot < 0:
            continue
        buckets[tot // interval_min].append(r)
    out: dict[str, dict] = {}
    for b, rs in sorted(buckets.items()):
        start = b * interval_min
        key = f"{start // 60:02d}{start % 60:02d}"
        out[key] = {
            "open": rs[0]["open"],
            "high": max(r["high"] for r in rs),
            "low": min(r["low"] for r in rs),
            "close": rs[-1]["close"],
            "volume": sum(r["volume"] for r in rs),
        }
    return out


def sma_series(bars: dict[str, dict], period: int) -> dict[str, float]:
    keys = sorted(bars)
    out = {}
    for i in range(period - 1, len(keys)):
        window = keys[i - period + 1 : i + 1]
        out[keys[i]] = sum(bars[k]["close"] for k in window) / period
    return out


def compare_ticker(ticker: str, local: dict[str, dict], rest_rows: list[dict], sma: int) -> tuple[list[str], dict]:
    rest = aggregate(rest_rows, 3, 0)
    rest_shift = aggregate(rest_rows, 3, -1)
    keys = sorted(set(local) | set(rest))
    lines = [f"### {ticker}", "",
             "| 봉(KST) | 로컬 O/H/L/C/V | REST O/H/L/C/V | 차이 |", "|---|---|---|---|"]
    stats = {"both": 0, "ohlc_eq": 0, "close_eq": 0, "open_eq": 0, "open_eq_shift": 0,
             "only_local": 0, "only_rest": 0, "vol_ratio": [], "sma_diff_pct": []}
    for k in keys:
        lb, rb = local.get(k), rest.get(k)
        if lb and rb:
            stats["both"] += 1
            flags = []
            if (lb["open"], lb["high"], lb["low"], lb["close"]) == (rb["open"], rb["high"], rb["low"], rb["close"]):
                stats["ohlc_eq"] += 1
                flags.append("OHLC 같음")
            else:
                for f_ in ("open", "high", "low", "close"):
                    if lb[f_] != rb[f_]:
                        flags.append(f"{f_[0].upper()} {lb[f_]:g}/{rb[f_]:g}")
            if lb["close"] == rb["close"]:
                stats["close_eq"] += 1
            if lb["open"] == rb["open"]:
                stats["open_eq"] += 1
            sb = rest_shift.get(k)
            if sb and lb["open"] == sb["open"]:
                stats["open_eq_shift"] += 1
            if rb["volume"] > 0:
                stats["vol_ratio"].append(lb["volume"] / rb["volume"])
                if abs(lb["volume"] - rb["volume"]) > 0:
                    flags.append(f"V {lb['volume']}/{rb['volume']}")
            lines.append(f"| {k} | {lb['open']:g}/{lb['high']:g}/{lb['low']:g}/{lb['close']:g}/{lb['volume']} "
                         f"| {rb['open']:g}/{rb['high']:g}/{rb['low']:g}/{rb['close']:g}/{rb['volume']} | {', '.join(flags)} |")
        elif lb:
            stats["only_local"] += 1
            lines.append(f"| {k} | {lb['open']:g}/{lb['high']:g}/{lb['low']:g}/{lb['close']:g}/{lb['volume']} | — | 로컬만 |")
        else:
            stats["only_rest"] += 1
            lines.append(f"| {k} | — | {rb['open']:g}/{rb['high']:g}/{rb['low']:g}/{rb['close']:g}/{rb['volume']} | REST만(빈 구간·빠진 틱) |")
    ls, rs = sma_series(local, sma), sma_series(rest, sma)
    for k in sorted(set(ls) & set(rs)):
        if rs[k] > 0:
            stats["sma_diff_pct"].append(abs(ls[k] - rs[k]) / rs[k] * 100.0)
    lines.append("")
    return lines, stats


def summary_row(ticker: str, s: dict) -> str:
    both = s["both"] or 1
    vr = sum(s["vol_ratio"]) / len(s["vol_ratio"]) if s["vol_ratio"] else 0.0
    sd = s["sma_diff_pct"]
    label = "그대로" if s["open_eq"] >= s["open_eq_shift"] else "1분 당김"
    return (f"| {ticker} | {s['both']} | {s['only_local']} | {s['only_rest']} | {s['ohlc_eq'] / both * 100:.0f}% "
            f"| {s['close_eq'] / both * 100:.0f}% | {vr:.2f} | {max(sd) if sd else 0:.3f}% / {sum(sd) / len(sd) if sd else 0:.3f}% "
            f"| {label}({s['open_eq']}/{s['open_eq_shift']}) |")


def selftest() -> int:
    log = ("2026-09-14 00:03:01.123 [DEBUG] [DEVSCALE_005930] 봉 닫힘 src=ws t=0900 o=100 h=105 l=99 c=104 v=300\n"
           "2026-09-14 00:06:00.001 [DEBUG] [DEVSCALE_005930] 봉 닫힘 src=ws t=0903 o=104 h=106 l=103 c=105 v=200\n"
           "2026-09-13 06:00:00.000 [DEBUG] [DEVSCALE_005930] 봉 닫힘 src=ws t=0900 o=1 h=1 l=1 c=1 v=1\n")
    p = Path(__file__).resolve().parent / "_compare_ws_bars_selftest.log"
    p.write_text(log, encoding="utf-8")
    try:
        local = parse_local(p, "2026-09-14")
    finally:
        p.unlink()
    assert list(local) == ["005930"] and sorted(local["005930"]) == ["0900", "0903"], local
    assert local["005930"]["0900"]["close"] == 104.0  # 09-13 06:00 UTC 줄은 KST 09-13이라 걸러진다
    rows = [{"time": "0900", "open": 100, "high": 101, "low": 99, "close": 101, "volume": 100},
            {"time": "0901", "open": 101, "high": 105, "low": 101, "close": 103, "volume": 100},
            {"time": "0902", "open": 103, "high": 104, "low": 102, "close": 104, "volume": 100},
            {"time": "0903", "open": 104, "high": 106, "low": 103, "close": 105, "volume": 200},
            {"time": "0906", "open": 105, "high": 105, "low": 105, "close": 105, "volume": 10}]
    agg = aggregate(rows)
    assert agg["0900"] == {"open": 100, "high": 105, "low": 99, "close": 104, "volume": 300}, agg
    assert sorted(agg) == ["0900", "0903", "0906"]
    lines, st = compare_ticker("005930", local["005930"], rows, sma=2)
    assert st["both"] == 2 and st["ohlc_eq"] == 2 and st["only_rest"] == 1 and st["only_local"] == 0, st
    assert aggregate(rows, 3, -1)["0900"]["open"] == 101  # 1분 당기면 0901 행이 0900 버킷의 첫 행
    print("\n".join(lines))
    print(summary_row("005930", st))
    print("selftest ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="KST 날짜 YYYY-MM-DD")
    ap.add_argument("--log", default="logs/quant_trader.log")
    ap.add_argument("--config", default="Quant/config/config_dev_paper.json")
    ap.add_argument("--tickers", default="", help="쉼표 구분. 비우면 로그에 나온 종목 전부")
    ap.add_argument("--sma", type=int, default=20)
    ap.add_argument("--out", default="")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.date:
        ap.error("--date가 필요하다")

    local_all = parse_local(Path(a.log), a.date)
    tickers = [t for t in a.tickers.split(",") if t] or sorted(local_all)
    if not tickers:
        print(f"로그에 {a.date}의 '봉 닫힘 src=ws' 줄이 없다 — bar_source=ws·log_level=DEBUG로 돌렸는지 본다")
        return 1

    from kis.client import KisClient  # noqa: E402

    cfg = json.load(open(a.config, encoding="utf-8"))
    k = cfg.get("quote_kis") or cfg["kis"]
    c = KisClient(app_key=k["app_key"], app_secret=k["app_secret"],
                  account_no=k.get("account_no", cfg["kis"].get("account_no", "")),
                  is_paper=k.get("is_paper", False))
    c.authenticate()
    ymd = a.date.replace("-", "")

    body: list[str] = []
    summ = [f"# WS 집계 봉 대 REST 봉 — {a.date} (sma={a.sma})", "",
            "| 종목 | 양쪽 | 로컬만 | REST만 | OHLC 일치 | 종가 일치 | 거래량 비(로컬/REST) | SMA 차이 최대/평균 | 라벨 판정(그대로/당김) |",
            "|---|---|---|---|---|---|---|---|---|"]
    for t in tickers:
        rows = c.get_past_minute_ohlcv(t, ymd)
        lines, st = compare_ticker(t, local_all.get(t, {}), rows, a.sma)
        body += lines
        summ.append(summary_row(t, st))
    text = "\n".join(summ + [""] + body)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
        print(f"wrote {a.out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
