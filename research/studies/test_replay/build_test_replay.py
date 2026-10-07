"""시험 매매 리플레이 — 네 시험의 매매를 종목별 캔들 위에 그리고, 원장 값과 차트 가격을 건마다 자동 대조한다(사용자 요청 2026-10-07).

대상(원장 parquet → 다시 계산에 쓰는 시험 코드):
  1) 스터디 35 급등 뒤 박스 돌파 263건  halt_trigger_trades.parquet → close_depth.path_exits, halt_trigger.conditions
  2) 스터디 37 쌍바닥·추세 단독 시험   standalone_swing_trades.parquet → swing_definitions, standalone_swing.simulate_exit
     칸마다 이익 상위 10·손실 하위 10·나머지에서 무작위 20(seed 20261007)
  3) DevScale 하향 추세 거름          devscale_trend_trades.parquet → devscale_trend_filter.simulate_ticker
     빠지는 매매(k=3 또는 k=5 하향) 20건·남는 매매 20건, 각각 이익 상위 5·손실 하위 5·무작위 10
  4) 이평선 지지 자산 곡선            ma_support_portfolio_trades.parquet → ma_support_exits.Market·SignalSet.exits
     모양 있음 전부, 모양 없음 비교는 무작위 50
근거 값은 시험 스크립트 함수를 import 해서 같은 코드로 다시 계산한다. 차트 일봉도 시험 스크립트가 읽는 경로·처리 그대로다
(스터디 33 load_inputs 수정주가, DevScale은 devscale_trend_filter.load_bars).
대조: 매수가·매도가·매도일·청산 사유·수익률을 원장 값과 다시 계산한 값으로 맞춰 보고, 매도가가 그날 고가·저가 안인지도 본다.
재실행(저장소 루트): py -X utf8 research/studies/test_replay/build_test_replay.py
산출: research/studies/test_replay/index.html, 시험별 목록 json(surge·swing·devscale·support.json)과 차트 묶음 json(<시험>_<번호>.json).
"""
from __future__ import annotations

import gc
import importlib.util
import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
STUDIES = HERE.parent
SURGE = STUDIES / "35_surge_box_breakout"
SWING = STUDIES / "37_swing_trend"
sys.path.insert(0, str(SWING))
sys.path.insert(0, str(STUDIES))

SEED = 20261007
BEFORE_DAYS = 60            # 차트: 신호 전 60거래일부터
AFTER_DAYS = 10             # 청산 뒤 10거래일까지
CHUNK_BYTES = 6_000_000     # 차트 묶음 파일 하나의 크기 상한(아티팩트 파일당 16MB 아래)
PRICE_TOLERANCE = 0.01      # 가격 대조 허용치(원) — 상대 1e-6 과 큰 쪽
PERCENT_TOLERANCE = 1e-6


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


close_depth = load_module("replay_close_depth", SURGE / "close_depth.py")
halt = load_module("replay_halt_trigger", SURGE / "halt_trigger.py")
import build_test_ledger as ledger_labels  # noqa: E402
import devscale_trend_filter as devscale  # noqa: E402
import ma_support_exits as support  # noqa: E402
import standalone_swing as standalone  # noqa: E402
import swing_definitions as swing  # noqa: E402


# ── 숫자·날짜 표기 ─────────────────────────────────────────────────────────────

def finite(value) -> bool:
    return value is not None and isinstance(value, (int, float, np.integer, np.floating)) and math.isfinite(float(value))


def price_text(value) -> str:
    if not finite(value):
        return "—"

    value = float(value)
    return f"{value:,.0f}" if abs(value - round(value)) < 0.005 else f"{value:,.2f}"


def date_text(date) -> str:
    text = str(int(date))
    return f"{text[:4]}-{text[4:6]}-{text[6:]}"


def percent_text(value, digits: int = 2) -> str:
    return f"{float(value):+.{digits}f}%" if finite(value) else "—"


def rounded(value):
    return round(float(value), 2) if finite(value) else None


def clean(value):
    """json 으로 쓸 수 있게 numpy 값·NaN 을 파이썬 값으로 바꾼다."""
    if isinstance(value, dict):
        return {str(key): clean(item) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [clean(item) for item in value]

    if isinstance(value, (bool, np.bool_)):
        return bool(value)

    if isinstance(value, (np.integer,)):
        return int(value)

    if isinstance(value, (float, np.floating)):
        return float(value) if math.isfinite(float(value)) else None

    return value


# ── 대조 한 줄: [항목, 원장 값, 차트 값, 맞음] ─────────────────────────────────────

def same_price(left, right) -> bool:
    if not (finite(left) and finite(right)):
        return False

    return abs(float(left) - float(right)) <= max(PRICE_TOLERANCE, 1e-6 * abs(float(right)))


def price_check(label: str, ledger, chart) -> list:
    return [label, price_text(ledger), price_text(chart), same_price(ledger, chart)]


def percent_check(label: str, ledger, chart, digits: int = 4) -> list:
    matched = finite(ledger) and finite(chart) and abs(float(ledger) - float(chart)) <= PERCENT_TOLERANCE
    return [label, percent_text(ledger, digits), percent_text(chart, digits), matched]


def exact_check(label: str, ledger, chart, formatter=str) -> list:
    return [label, formatter(ledger), formatter(chart), ledger == chart]


def range_check(label: str, price, low, high) -> list:
    inside = finite(price) and float(low) - PRICE_TOLERANCE <= float(price) <= float(high) + PRICE_TOLERANCE
    return [label, price_text(price), f"저가 {price_text(low)} – 고가 {price_text(high)}", inside]


# ── 차트 자료 ─────────────────────────────────────────────────────────────────

def chart_window(length: int, signal_row: int, earliest_row: int, exit_row: int) -> tuple[int, int]:
    first = max(0, min(signal_row - BEFORE_DAYS, earliest_row - 5))
    last = min(length - 1, exit_row + AFTER_DAYS)
    return first, last


def candle_rows(dates, open_price, high, low, close, volume, first: int, last: int) -> list:
    return [[int(dates[row]), rounded(open_price[row]), rounded(high[row]), rounded(low[row]), rounded(close[row]), int(volume[row])]
            for row in range(first, last + 1)]


def series_values(values, first: int, last: int) -> list:
    return [rounded(values[row]) for row in range(first, last + 1)]


def pivot_pairs(pivots: dict, at_row: int, count: int, side: str) -> list[dict]:
    """at_row 에서 확정된 마지막 두 점(앞, 뒤). latest_two 의 순번을 그대로 쓴다."""
    rows = pivots[f"{side}_rows"]
    confirm_rows = pivots[f"{side}_confirm_rows"]
    values = pivots[f"{side}_values"]
    _, _, position = swing.latest_two(confirm_rows, values, count)
    last = int(position[at_row])
    return [{"row": int(rows[index]), "confirm": int(confirm_rows[index]), "value": float(values[index])}
            for index in (last - 1, last) if index >= 0]


def pivot_markers(pivots: dict, first: int, last: int, dates, used: set) -> list[dict]:
    """창 안의 확정 저점·고점을 흐린 점으로(근거로 쓴 점은 따로 그린다)."""
    markers = []

    for side, kind in (("low", "pivot_low"), ("high", "pivot_high")):
        for row, value in zip(pivots[f"{side}_rows"].tolist(), pivots[f"{side}_values"].tolist()):
            if first <= row <= last and (side, row) not in used:
                markers.append({"date": int(dates[row]), "price": rounded(value), "kind": kind, "label": ""})

    return markers


def trend_rows_text(pairs_low: list[dict], pairs_high: list[dict], dates, state_name: str) -> list[str]:
    lines = []

    for title, pairs in (("저점", pairs_low), ("고점", pairs_high)):
        if len(pairs) < 2:
            lines.append(f"확정 {title} 둘이 아직 없다 → {state_name}")
            continue

        older, newer = pairs
        direction = "높아짐" if newer["value"] > older["value"] else "낮아짐" if newer["value"] < older["value"] else "같음"
        lines.append(f"확정 {title}: {price_text(older['value'])} ({date_text(dates[older['row']])}, 확정 {date_text(dates[older['confirm']])})"
                     f" → {price_text(newer['value'])} ({date_text(dates[newer['row']])}, 확정 {date_text(dates[newer['confirm']])}) {direction}")

    return lines


def pivot_drawings(pairs_low: list[dict], pairs_high: list[dict], dates, kind: str, prefix: str) -> tuple[list, list, list]:
    """근거로 쓴 저점·고점 두 쌍을 점·잇는 선·확정일 표시로."""
    markers, segments, flags = [], [], []

    for side, pairs, title in (("low", pairs_low, "저"), ("high", pairs_high, "고")):
        for order, pivot in enumerate(pairs, start=1):
            markers.append({"date": int(dates[pivot["row"]]), "price": rounded(pivot["value"]), "kind": f"{side}_{kind}",
                            "label": f"{prefix}{title}{order} {price_text(pivot['value'])}"})
            flags.append({"date": int(dates[pivot["confirm"]]), "label": f"{prefix}{title}{order} 확정", "kind": "muted"})

        if len(pairs) == 2:
            segments.append({"points": [[int(dates[pairs[0]["row"]]), rounded(pairs[0]["value"])],
                                        [int(dates[pairs[1]["row"]]), rounded(pairs[1]["value"])]],
                             "kind": kind, "dash": side == "high", "label": f"{prefix}{'저점 잇는 선' if side == 'low' else '고점 잇는 선(점선)'}"})

    return markers, segments, flags


class Collector:
    """시험 하나의 가벼운 목록과 무거운 차트 자료."""

    def __init__(self, key: str) -> None:
        self.key = key
        self.listing: list[dict] = []
        self.heavy: dict[str, dict] = {}

    def add(self, entry: dict, chart: dict) -> None:
        trade_id = f"{self.key}{len(self.listing):05d}"
        failed = [item for item in chart["check"] if not item[3]]
        entry.update({"id": trade_id, "ok": not failed, "bad": len(failed), "checks": len(chart["check"])})
        self.listing.append(entry)
        self.heavy[trade_id] = chart


def write_bytes(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


def write_test(collector: Collector, meta: dict, filters: list[dict]) -> list[Path]:
    """목록 json 하나와 묶음 json 여러 개를 쓴다. 묶음은 CHUNK_BYTES 를 넘기 전에 자른다."""
    written = []
    chunk_number, chunk, chunk_size = 1, {}, 0

    def flush() -> None:
        nonlocal chunk_number, chunk, chunk_size

        if chunk:
            path = HERE / f"{collector.key}_{chunk_number}.json"
            write_bytes(path, json.dumps(clean(chunk), ensure_ascii=False, separators=(",", ":"), allow_nan=False))
            written.append(path)
            chunk_number, chunk, chunk_size = chunk_number + 1, {}, 0

    for entry in collector.listing:
        text = json.dumps(clean(collector.heavy[entry["id"]]), ensure_ascii=False, separators=(",", ":"), allow_nan=False)

        if chunk and chunk_size + len(text.encode("utf-8")) > CHUNK_BYTES:
            flush()

        chunk[entry["id"]] = collector.heavy[entry["id"]]
        chunk_size += len(text.encode("utf-8"))
        entry["chunk"] = f"{collector.key}_{chunk_number}.json"

    flush()
    checks_total = sum(entry["checks"] for entry in collector.listing)
    checks_bad = sum(entry["bad"] for entry in collector.listing)
    meta.update({"trades_total": len(collector.listing), "trades_ok": sum(entry["ok"] for entry in collector.listing),
                 "checks_total": checks_total, "checks_ok": checks_total - checks_bad})
    meta["mismatches"] = [{"id": entry["id"], "code": entry["code"], "name": entry["name"], "buy": entry["buy"],
                           "items": [item[:3] for item in collector.heavy[entry["id"]]["check"] if not item[3]]}
                          for entry in collector.listing if not entry["ok"]]
    listing_path = HERE / f"{collector.key}.json"
    write_bytes(listing_path, json.dumps(clean({"meta": meta, "filters": filters, "trades": collector.listing}),
                                         ensure_ascii=False, separators=(",", ":"), allow_nan=False))
    return [listing_path] + written


def reason_group(options: dict[str, str]) -> dict:
    return {"name": "청산", "options": [{"key": f"reason:{key}", "label": label} for key, label in options.items()]}


# ── 일봉 캐시: 시험 스크립트의 load_inputs 를 한 번만 읽게 바꿔 끼운다 ───────────────

STOCK_CACHE: dict[int, tuple] = {}
original_load_inputs = standalone.study33.load_inputs


def cached_load_inputs(last_date: int):
    if last_date not in STOCK_CACHE:
        STOCK_CACHE.clear()
        gc.collect()
        STOCK_CACHE[last_date] = original_load_inputs(last_date)

    return STOCK_CACHE[last_date]


standalone.study33.load_inputs = cached_load_inputs


# ── 1) 스터디 35 급등 뒤 박스 돌파 ──────────────────────────────────────────────

SURGE_REASONS = {1: "손절", 2: "익절", 0: "기한"}
SURGE_REASON_KEYS = {1: "stop", 2: "take", 0: "time"}


def build_surge() -> list[Path]:
    trades = pd.read_parquet(halt.TRADES_PATH)
    halt_input = trades.copy()

    for column in [name for name in trades.columns if name.endswith(("_above20", "_above60", "_above120"))]:
        halt_input[column] = trades[column].map({True: 1.0, False: 0.0}).astype(float)   # 저장하며 None 이 된 칸 → NaN

    index_frame = pd.read_parquet(halt.INDEX_PATH).sort_index()
    calendar = index_frame.index.to_numpy()
    items = halt.conditions(halt_input, calendar)
    kosdaq = index_frame["KQ11"]
    kosdaq_average = {days: kosdaq.rolling(days).mean().to_numpy() for days in (20, 120)}
    kosdaq_drawdown = ((kosdaq / kosdaq.rolling(halt.HIGH_WINDOW, min_periods=20).max() - 1.0) * 100.0).to_numpy()
    kosdaq_close = kosdaq.to_numpy()
    overnight = pd.read_parquet(SURGE / "overnight_us_trades.parquet", columns=["code", "entry_date", "vix_level", "vix_change"])
    overnight = overnight.drop_duplicates(["code", "entry_date"]).set_index(["code", "entry_date"])
    stocks, _, _ = close_depth.study33.load_inputs(close_depth.base.LAST_DATE)
    by_code = {series.code: series for series in stocks}
    market_dates = np.unique(np.concatenate([series.dates for series in stocks]))
    base = close_depth.base
    cost = close_depth.sweep.COST
    collector = Collector("surge")
    condition_options = []

    for number, item in enumerate(items):
        dropped = int((~item["keep"]).sum())
        label = f"{ledger_labels.HALT_FAMILY_LABELS.get(item['family'], item['family'])} · {ledger_labels.halt_label(item['name'])} ({dropped}건 빠짐)"
        condition_options.append({"key": f"halt:{number}", "label": label})

    for position, record in enumerate(trades.to_dict("records")):
        series = by_code[record["code"]]
        dates, close, low, high, open_price = series.dates, series.close, series.low, series.high, series.open_price
        rows = len(dates)
        surge_row = int(np.searchsorted(dates, record["surge"]))

        def change_percent(row: int) -> float:
            return (close[row] / close[row - 1] - 1.0) * 100.0

        run_start = surge_row

        while run_start > 1 and change_percent(run_start - 1) >= base.GAIN - 1e-9:
            run_start -= 1

        watch_last = surge_row + 4
        entry_row = watch_last + 1
        watch_lows = low[surge_row + 1:watch_last + 1]
        lowest_low = float(watch_lows.min())
        lowest_low_row = surge_row + 1 + int(watch_lows.argmin())
        lowest_close = float(close[surge_row + 1:watch_last + 1].min())
        move = close[surge_row] - close[run_start - 1]
        move_percent = move / close[run_start - 1] * 100.0
        depth_low = (close[surge_row] - lowest_low) / move * 100.0
        last = watch_last + base.HORIZON
        horizon_short = last > rows - 1

        if horizon_short:
            last = rows - 1

        entry = float(open_price[entry_row])
        exits = close_depth.path_exits(series, entry_row, entry, lowest_low, lowest_close, last)
        return_percent = exits[close_depth.exit_column("low3", 15, "ret")]
        kind = int(exits[close_depth.exit_column("low3", 15, "kind")])
        hold = int(exits[close_depth.exit_column("low3", 15, "hold")])
        exit_row = entry_row + hold - 1
        stop_price = min(lowest_low * 0.97, entry * 0.999)
        take_price = entry * 1.15

        if kind == 1:
            rule_price = min(float(open_price[exit_row]), stop_price)
        elif kind == 2:
            rule_price = max(float(open_price[exit_row]), take_price)
        else:
            rule_price = float(close[last])

        ledger_exit_price = (record["ret"] / 100.0 + 1.0) / cost * record["entry_price"]
        gap = (open_price[entry_row] / close[watch_last] - 1.0) * 100.0
        limit_percent = 15.0 if int(dates[entry_row]) < close_depth.LIMIT_CHANGE_DATE else 30.0
        index_position = int(np.searchsorted(calendar, record["entry_date"], side="left")) - 1
        index_date = int(calendar[index_position])
        market_previous = int(market_dates[np.searchsorted(market_dates, record["entry_date"]) - 1])

        def above_text(value) -> str:
            return "자료 없음" if value is None or (isinstance(value, float) and math.isnan(value)) else ("위" if bool(value) else "아래")

        recomputed_above = {}

        for days in (20, 120):
            average = kosdaq_average[days][index_position]
            recomputed_above[days] = None if not finite(average) else bool(kosdaq_close[index_position] >= average)

        check = [
            exact_check("매수일(D4 다음 거래일)", int(record["entry_date"]), int(dates[entry_row]), date_text),
            price_check("매수가 = 매수일 시가", record["entry_price"], entry),
            percent_check("넘어간 폭", record["move_pct"], move_percent),
            percent_check("되돌림 깊이(저가)", record["depth_low"], depth_low),
            exact_check("청산 사유", SURGE_REASONS[int(record["kind"])], SURGE_REASONS[kind]),
            exact_check("보유 거래일(매수일 포함)", int(record["hold"]), hold),
            exact_check("매도일", int(record["exit_date"]), int(dates[exit_row]), date_text),
            price_check("매도가(원장 수익률에서 거꾸로) vs 규칙 가격", ledger_exit_price, rule_price),
            range_check("매도가가 그날 가격 범위 안", ledger_exit_price, low[exit_row], high[exit_row]),
            percent_check("순수익", record["ret"], return_percent),
            exact_check("지수 값을 쓴 날 = 매수 전 마지막 거래일", int(record["signal_date"]), market_previous, date_text),
            exact_check("코스닥 20일 평균 위·아래", above_text(record["kosdaq_above20"]), above_text(recomputed_above[20])),
            exact_check("코스닥 120일 평균 위·아래", above_text(record["kosdaq_above120"]), above_text(recomputed_above[120])),
        ]

        if horizon_short:
            check.append(["기한 120거래일이 시세 안에 있음", "있음", f"모자람(상장폐지 {bool(series.delisted)})", bool(series.delisted)])

        if kind == 1:
            exit_text = (f"{date_text(dates[exit_row])} 저가 {price_text(low[exit_row])} ≤ 손절 {price_text(stop_price)} → "
                         f"min(시가 {price_text(open_price[exit_row])}, 손절 {price_text(stop_price)}) = {price_text(rule_price)}")
        elif kind == 2:
            exit_text = (f"{date_text(dates[exit_row])} 고가 {price_text(high[exit_row])} ≥ 익절 {price_text(take_price)} → "
                         f"max(시가 {price_text(open_price[exit_row])}, 익절 {price_text(take_price)}) = {price_text(rule_price)}")
        else:
            exit_text = f"손절·익절에 안 닿아 기한 {date_text(dates[last])} 종가 {price_text(rule_price)}"

        dropped_by = [number for number, item in enumerate(items) if not item["keep"][position]]
        vix = overnight.loc[(record["code"], record["entry_date"])] if (record["code"], record["entry_date"]) in overnight.index else None
        index_note = "" if index_date == market_previous else f" (지수 자료 끝 {date_text(calendar[-1])} 뒤라 {date_text(index_date)} 값)"
        why = [
            {"title": "매수 근거", "rows": [
                f"급등 묶음: {date_text(dates[run_start])} – {date_text(dates[surge_row])} {surge_row - run_start + 1}일 연속 +10% 이상 (D0 = {date_text(dates[surge_row])})",
                f"넘어간 폭: (D0 종가 {price_text(close[surge_row])} − 묶음 전날 종가 {price_text(close[run_start - 1])}) ÷ "
                f"{price_text(close[run_start - 1])} = {move_percent:.1f}% ≥ 30%",
                f"관찰 D1–D4 {date_text(dates[surge_row + 1])} – {date_text(dates[watch_last])}: 최저 저가 {price_text(lowest_low)} ({date_text(dates[lowest_low_row])})",
                f"되돌림 깊이: (D0 종가 {price_text(close[surge_row])} − 관찰 저점 {price_text(lowest_low)}) ÷ 폭 {price_text(move)} = {depth_low:.1f}% < 15%",
                f"매수: {date_text(dates[entry_row])} 시가 {price_text(entry)} (갭 {gap:+.1f}% < 제한폭 {limit_percent:.0f}% − 0.5)",
            ]},
            {"title": "청산 규칙과 매도", "rows": [
                f"손절: min(관찰 저점 {price_text(lowest_low)} × 0.97 = {price_text(lowest_low * 0.97)}, 매수가 × 0.999 = {price_text(entry * 0.999)}) = {price_text(stop_price)}",
                f"익절: 매수가 {price_text(entry)} × 1.15 = {price_text(take_price)}",
                f"기한: D4 + {base.HORIZON}거래일 = {date_text(dates[last])}" + (" (시세 끝까지)" if horizon_short else ""),
                f"매도: {exit_text}",
                f"순수익: ({price_text(rule_price)} ÷ {price_text(entry)} × 비용 {cost:.6f} − 1) × 100 = {return_percent:+.2f}%",
            ]},
            {"title": "시장 상황과 정지 조건", "rows": [
                f"국면 점수: 매수일 개장 전 {record['score_pre_entry']:.0f} · 신호일 종가 {record['score_close_signal']:.0f} (정지 기준 −7 이하)"
                if finite(record["score_pre_entry"]) and finite(record["score_close_signal"]) else "국면 점수: 자료 없음",
                f"코스닥 {date_text(index_date)}{index_note}: 종가 {kosdaq_close[index_position]:,.2f} · 20일 평균 {kosdaq_average[20][index_position]:,.2f} → "
                f"{above_text(recomputed_above[20])} · 120일 평균 {kosdaq_average[120][index_position]:,.2f} → {above_text(recomputed_above[120])}",
                f"코스닥 250일 고점 대비 {kosdaq_drawdown[index_position]:+.1f}%",
                f"전날 밤 VIX 종가 {vix['vix_level']:.2f} (하루 {vix['vix_change']:+.1f}%)" if vix is not None and finite(vix["vix_level"]) else "전날 밤 VIX: 자료 없음",
                ("이 매매를 빼는 조건: " + ", ".join(ledger_labels.halt_label(items[number]["name"]) for number in dropped_by))
                if dropped_by else "이 매매를 빼는 정지 조건: 없음",
            ]},
        ]
        first, last_shown = chart_window(rows, watch_last, run_start - 1, exit_row)
        chart = {
            "candles": candle_rows(dates, open_price, high, low, close, series.volume, first, last_shown),
            "lines": [{"price": rounded(take_price), "label": f"익절 {price_text(take_price)}", "kind": "take", "from": int(dates[entry_row])},
                      {"price": rounded(entry), "label": f"매수 {price_text(entry)}", "kind": "accent", "from": int(dates[entry_row])},
                      {"price": rounded(lowest_low), "label": f"관찰 저점 {price_text(lowest_low)}", "kind": "mark", "from": int(dates[surge_row])},
                      {"price": rounded(stop_price), "label": f"손절 {price_text(stop_price)}", "kind": "stop", "from": int(dates[entry_row])}],
            "series": [], "segments": [],
            "markers": [{"date": int(dates[lowest_low_row]), "price": rounded(lowest_low), "kind": "low_mark", "label": "관찰 저점"}],
            "flags": [{"date": int(dates[run_start]), "label": "묶음 시작", "kind": "mark"},
                      {"date": int(dates[surge_row]), "label": "급등 D0", "kind": "mark"},
                      {"date": int(dates[watch_last]), "label": "D4", "kind": "muted"}],
            "shades": [{"from": int(dates[surge_row + 1]), "to": int(dates[watch_last]), "kind": "watch"},
                       {"from": int(dates[entry_row]), "to": int(dates[exit_row]), "kind": "hold"}],
            "buy": {"date": int(dates[entry_row]), "price": rounded(entry)},
            "sell": {"date": int(dates[exit_row]), "price": rounded(ledger_exit_price)},
            "why": why, "check": check,
        }
        entry_item = {"code": record["code"], "name": record["name"], "buy": int(record["entry_date"]), "sell": int(record["exit_date"]),
                      "net": float(record["ret"]), "reason": SURGE_REASON_KEYS[int(record["kind"])], "reason_label": SURGE_REASONS[int(record["kind"])],
                      "note": f"급등 {date_text(record['surge'])} · 폭 {record['move_pct']:.0f}% · {record['market']}",
                      "tags": [f"halt:{number}" for number in dropped_by] + [f"reason:{SURGE_REASON_KEYS[int(record['kind'])]}"]}
        collector.add(entry_item, chart)

    meta = {"key": "surge", "title": "급등 뒤 박스 돌파 (스터디 35)", "code_name": "halt_trigger_trades.parquet",
            "summary": ["+10% 이상 2일 이상 이어진 급등 묶음의 마지막 날 D0, 넘어간 폭 ≥ 30%, D1–D4 최저 저가의 되돌림 깊이 < 15% → D5 시가 매수",
                        "손절 = min(관찰 저점 × 0.97, 매수가 × 0.999), 익절 = 매수가 × 1.15, 기한 D4 + 120거래일",
                        f"정지 조건 {len(items)}개 중 하나를 고르면 그 조건이 빼는 매매만 보인다. 일봉은 스터디 33 load_inputs({close_depth.base.LAST_DATE}) 수정주가"]}
    filters = [{"name": "정지 조건이 빼는 매매", "options": condition_options}, reason_group({"stop": "손절", "take": "익절", "time": "기한"})]
    del stocks, by_code
    STOCK_CACHE.clear()
    gc.collect()
    return write_test(collector, meta, filters)


# ── 2) 스터디 37 쌍바닥·추세 단독 ──────────────────────────────────────────────

SWING_REASONS = {"stop": "손절", "take": "익절", "time": "기한", "data_end": "시세 끝"}


def sample_extremes(group: pd.DataFrame, edge: int, middle: int, generator: np.random.Generator, value_column: str = "net") -> list[tuple[int, str]]:
    """(원래 행 번호, 뽑은 방법). 손익 순으로 하위 edge·상위 edge, 나머지에서 무작위 middle."""
    ordered = group.sort_values([value_column, "entry_date", "code"], kind="mergesort")
    labels = [(int(index), "worst") for index in ordered.index[:edge]] + [(int(index), "best") for index in ordered.index[-edge:]]
    rest = ordered.index[edge:-edge].to_numpy()
    picked = generator.choice(rest, size=min(middle, len(rest)), replace=False) if len(rest) else []
    return labels + [(int(index), "random") for index in sorted(picked)]


SAMPLE_LABELS = {"best": "이익 상위", "worst": "손실 하위", "random": "무작위"}


def build_swing(stocks: list, last_market_date: int) -> list[Path]:
    columns = ["code", "cell", "kind", "pivot_span", "window", "signal_date", "entry_date", "exit_date", "stop", "entry",
               "exit_price", "reason", "hold_days", "net"]
    trades = pd.read_parquet(standalone.OUTPUT_TRADES, columns=columns)
    generator = np.random.default_rng(SEED)
    picks = []
    cells = sorted(trades["cell"].unique(), key=lambda name: (not name.startswith("DB"), not name.startswith("UP"), name))

    for cell in cells:
        picks.extend(sample_extremes(trades[trades["cell"] == cell], 10, 20, generator))

    by_code = {series.code: series for series in stocks}
    pivot_cache: dict[tuple[str, int], dict] = {}
    collector = Collector("swing")

    for index, sample in picks:
        record = trades.loc[index].to_dict()
        series = by_code[record["code"]]
        dates, close, low, high, open_price = series.dates, series.close, series.low, series.high, series.open_price
        rows = len(dates)
        span = int(record["pivot_span"])
        key = (record["code"], span)

        if key not in pivot_cache:
            pivot_cache[key] = swing.confirmed_pivots(low, high, span)

        pivots = pivot_cache[key]
        signal_row = int(np.searchsorted(dates, record["signal_date"]))
        entry_row = signal_row + 1
        market_ended = int(dates[-1]) < last_market_date
        check = [exact_check("신호일이 시세에 있음", int(record["signal_date"]), int(dates[signal_row]), date_text)]
        lines, series_list, segments, markers, flags, why = [], [], [], [], [], []
        used: set = set()
        earliest_row = signal_row
        base_low = float("nan")

        if record["kind"] == "double_bottom":
            found = [item for item in swing.double_bottom_signals(close, high, pivots) if item["signal_row"] == signal_row]
            check.append(exact_check("쌍바닥 신호 다시 계산", "있음", "있음" if found else "없음"))

            if found:
                item = found[0]
                base_low = float(item["base_low"])
                second_index = int(np.searchsorted(pivots["low_rows"], item["base_row"]))
                first_row, second_row = int(pivots["low_rows"][second_index - 1]), int(item["base_row"])
                first_value, second_value = float(pivots["low_values"][second_index - 1]), float(item["base_low"])
                first_confirm, second_confirm = first_row + span, second_row + span
                neck_row = first_row + 1 + int(high[first_row + 1:second_row].argmax())
                neckline = float(item["neckline"])
                earliest_row = first_row
                used |= {("low", first_row), ("low", second_row), ("high", neck_row)}
                markers += [{"date": int(dates[first_row]), "price": rounded(first_value), "kind": "low_accent", "label": f"저점1 {price_text(first_value)}"},
                            {"date": int(dates[second_row]), "price": rounded(second_value), "kind": "low_accent", "label": f"저점2 {price_text(second_value)}"},
                            {"date": int(dates[neck_row]), "price": rounded(neckline), "kind": "high_time", "label": "넥라인 고가"}]
                segments.append({"points": [[int(dates[first_row]), rounded(first_value)], [int(dates[second_row]), rounded(second_value)]],
                                 "kind": "accent", "label": "두 저점 잇는 선"})
                lines.append({"price": rounded(neckline), "label": f"넥라인 {price_text(neckline)}", "kind": "time",
                              "from": int(dates[first_row]), "to": int(dates[min(entry_row, rows - 1)])})
                flags += [{"date": int(dates[first_confirm]), "label": "저점1 확정", "kind": "muted"},
                          {"date": int(dates[second_confirm]), "label": "저점2 확정", "kind": "muted"}]
                gap = second_row - first_row
                slope = (second_value / first_value - 1.0) * 100.0
                why.append({"title": "매수 근거 — 쌍바닥 돌파", "rows": [
                    f"저점1 {price_text(first_value)} ({date_text(dates[first_row])}, 확정 {date_text(dates[first_confirm])} = {span}거래일 뒤)",
                    f"저점2 {price_text(second_value)} ({date_text(dates[second_row])}, 확정 {date_text(dates[second_confirm])})",
                    f"간격 {gap}거래일 (10–60) · 두 저점 차 |{price_text(second_value)} ÷ {price_text(first_value)} − 1| = {abs(slope):.2f}% ≤ 3% (기울기 {slope:+.2f}%)",
                    f"넥라인 = 두 저점 사이 최고 고가 {price_text(neckline)} ({date_text(dates[neck_row])}) ≥ 높은 저점 "
                    f"{price_text(max(first_value, second_value))} × 1.10 = {price_text(max(first_value, second_value) * 1.10)}",
                    f"신호 {date_text(dates[signal_row])}: 종가 {price_text(close[signal_row])} > 넥라인 {price_text(neckline)} (저점2 확정 뒤 처음)",
                    f"매수 {date_text(dates[entry_row])} 시가 {price_text(open_price[entry_row])}",
                ]})
        else:
            trend = swing.trend_state(pivots, rows)
            window = int(record["window"])
            wanted = swing.TREND_UP if record["kind"] == "support_up" else swing.TREND_DOWN
            average = swing.moving_average(close, window)
            mask = swing.moving_average_support_mask(close, low, average, trend["state"], wanted)
            check.append(exact_check("모양 조건 다시 계산(추세·이평선 지지)", "성립", "성립" if mask[signal_row] else "불성립"))
            base_low = float(trend["latest_low"][signal_row])
            pairs_low = pivot_pairs(pivots, signal_row, rows, "low")
            pairs_high = pivot_pairs(pivots, signal_row, rows, "high")
            used |= {("low", pivot["row"]) for pivot in pairs_low} | {("high", pivot["row"]) for pivot in pairs_high}
            earliest_row = min([pivot["row"] for pivot in pairs_low + pairs_high] + [signal_row])
            new_markers, new_segments, new_flags = pivot_drawings(pairs_low, pairs_high, dates, "accent", "")
            markers += new_markers
            segments += new_segments
            flags += new_flags
            series_list.append({"label": f"{window}일선", "kind": "mark", "values": None, "source": average})
            state_name = support.TREND_NAMES[int(trend["state"][signal_row])]
            previous = average[signal_row - 1]
            slope_text = ""

            if len(pairs_low) == 2:
                older, newer = pairs_low
                slope_text = (f"두 저점 기울기: ({price_text(newer['value'])} − {price_text(older['value'])}) ÷ {price_text(older['value'])} = "
                              f"{(newer['value'] / older['value'] - 1.0) * 100.0:+.2f}% / {newer['row'] - older['row']}거래일")

            why.append({"title": f"매수 근거 — {ledger_labels.standalone_label(record['cell'])}", "rows": [
                f"추세(확정 저점·고점, k = {span}, 신호일 {date_text(dates[signal_row])}까지 확정된 점만): {state_name}",
                *trend_rows_text(pairs_low, pairs_high, dates, state_name),
                *([slope_text] if slope_text else []),
                f"저가 {price_text(low[signal_row])} ≤ {window}일선 {price_text(average[signal_row])} × 1.01 = {price_text(average[signal_row] * 1.01)}",
                f"종가 {price_text(close[signal_row])} > {window}일선 {price_text(average[signal_row])} · {window}일선 {price_text(average[signal_row])} > 전날 {price_text(previous)}",
                f"매수 {date_text(dates[entry_row])} 시가 {price_text(open_price[entry_row])}",
            ]})

        stop = base_low * standalone.STOP_BELOW
        entry = float(open_price[entry_row])
        take = entry * standalone.TAKE_PROFIT
        result = standalone.simulate_exit(series, rows, entry_row, stop, market_ended) if finite(stop) else None
        check.append(price_check("손절선 = 근거 저점 × 0.97", record["stop"], stop))
        check.append(price_check("매수가 = 다음 날 시가", record["entry"], entry))

        if result is None:
            check.append(["청산 다시 계산", "있음", "결과 없음", False])
            exit_row = min(rows - 1, entry_row + standalone.HOLD_MAX)
        else:
            exit_row = int(result["exit_row"])

            if result["reason"] == "stop":
                rule_price = min(float(open_price[exit_row]), stop)
            elif result["reason"] == "take":
                rule_price = max(float(open_price[exit_row]), take)
            else:
                rule_price = float(close[exit_row])

            check += [exact_check("청산 사유", SWING_REASONS[record["reason"]], SWING_REASONS[result["reason"]]),
                      exact_check("매도일", int(record["exit_date"]), int(dates[exit_row]), date_text),
                      price_check("매도가 vs 규칙 가격", record["exit_price"], rule_price),
                      range_check("매도가가 그날 가격 범위 안", record["exit_price"], low[exit_row], high[exit_row]),
                      exact_check("보유 거래일", int(record["hold_days"]), int(result["hold_days"])),
                      percent_check("순수익", record["net"], result["net"])]

        exit_rule = {"stop": f"저가 {price_text(low[exit_row])} ≤ 손절 {price_text(stop)} → min(시가 {price_text(open_price[exit_row])}, 손절) = {price_text(record['exit_price'])}",
                     "take": f"고가 {price_text(high[exit_row])} ≥ 익절 {price_text(take)} → max(시가 {price_text(open_price[exit_row])}, 익절) = {price_text(record['exit_price'])}",
                     "time": f"60거래일 기한 종가 {price_text(record['exit_price'])}",
                     "data_end": f"시세 끝 종가 {price_text(record['exit_price'])}"}[record["reason"]]
        why.append({"title": "청산 규칙과 매도", "rows": [
            f"손절: 근거 저점 {price_text(base_low)} × 0.97 = {price_text(stop)}",
            f"익절: 매수가 {price_text(entry)} × 1.15 = {price_text(take)}",
            f"기한: 매수일 + {standalone.HOLD_MAX}거래일",
            f"매도 {date_text(dates[exit_row])}: {exit_rule}",
            f"순수익: ({price_text(record['exit_price'])} ÷ {price_text(entry)} × 비용 {standalone.COST:.6f} − 1) × 100 = {record['net']:+.2f}%",
        ]})
        first, last_shown = chart_window(rows, signal_row, earliest_row, exit_row)

        for item in series_list:
            item["values"] = series_values(item.pop("source"), first, last_shown)

        markers += pivot_markers(pivots, first, last_shown, dates, used)
        lines += [{"price": rounded(take), "label": f"익절 {price_text(take)}", "kind": "take", "from": int(dates[entry_row])},
                  {"price": rounded(entry), "label": f"매수 {price_text(entry)}", "kind": "accent", "from": int(dates[entry_row])},
                  {"price": rounded(stop), "label": f"손절 {price_text(stop)}", "kind": "stop", "from": int(dates[entry_row])}]
        flags.append({"date": int(dates[signal_row]), "label": "신호", "kind": "mark"})
        chart = {"candles": candle_rows(dates, open_price, high, low, close, series.volume, first, last_shown),
                 "lines": lines, "series": series_list, "segments": segments, "markers": markers, "flags": flags,
                 "shades": [{"from": int(dates[entry_row]), "to": int(dates[exit_row]), "kind": "hold"}],
                 "buy": {"date": int(dates[entry_row]), "price": rounded(entry)},
                 "sell": {"date": int(dates[exit_row]), "price": rounded(record["exit_price"])}, "why": why, "check": check}
        entry_item = {"code": record["code"], "name": series.name, "buy": int(record["entry_date"]), "sell": int(record["exit_date"]),
                      "net": float(record["net"]), "reason": record["reason"], "reason_label": SWING_REASONS[record["reason"]],
                      "note": f"{ledger_labels.standalone_label(record['cell'])} · {SAMPLE_LABELS[sample]}",
                      "tags": [f"cell:{record['cell']}", f"sample:{sample}", f"reason:{record['reason']}"]}
        collector.add(entry_item, chart)

    meta = {"key": "swing", "title": "쌍바닥·추세 (스터디 37 단독)", "code_name": "standalone_swing_trades.parquet",
            "summary": [f"원장 {len(trades):,}건, 칸 {len(cells)}개마다 손실 하위 10·이익 상위 10·나머지에서 무작위 20 (seed {SEED})",
                        "확정 저점 = 앞뒤 k일 중 최저(확정은 k거래일 뒤), 쌍바닥 = 두 저점 차 ≤ 3%·간격 10–60·넥라인 ≥ 높은 저점 × 1.10, 종가가 넥라인을 넘은 다음 날 시가 매수",
                        "추세·이평선 지지 = 신호일 추세가 상승(하향) · 저가 ≤ N일선 × 1.01 · 종가 > N일선 · N일선 상승, 다음 날 시가 매수",
                        "손절 = 근거 저점 × 0.97, 익절 = 매수가 × 1.15, 최대 60거래일. 일봉은 스터디 33 load_inputs(20261006) 수정주가"]}
    filters = [{"name": "칸", "options": [{"key": f"cell:{cell}", "label": ledger_labels.standalone_label(cell)} for cell in cells]},
               {"name": "뽑은 방법", "options": [{"key": f"sample:{key}", "label": label} for key, label in SAMPLE_LABELS.items()]},
               reason_group(SWING_REASONS)]
    return write_test(collector, meta, filters)


# ── 3) 이평선 지지 자산 곡선 ──────────────────────────────────────────────────

SUPPORT_REASONS = {1: "손절", 2: "익절", 0: "기한"}
SUPPORT_REASON_KEYS = {1: "stop", 2: "take", 0: "time"}


def build_support(stocks: list) -> list[Path]:
    market = support.Market()
    trades = pd.read_parquet(support.OUTPUT_PORTFOLIO_TRADES)
    metrics = json.loads(support.OUTPUT_METRICS.read_text(encoding="utf-8"))
    choices = {int(item["year"]): item["cell"] for item in metrics["walk"]["choices"]}
    shape = trades[trades["side"] == "shape"]
    baseline = trades[trades["side"] == "baseline"]
    picked_baseline = baseline.sample(n=min(50, len(baseline)), random_state=SEED)
    chosen = pd.concat([shape, picked_baseline]).sort_values(["entry_date", "code"], kind="mergesort")
    column_of = {code: column for column, code in enumerate(market.codes)}
    starts = np.searchsorted(market.stock, np.arange(len(market.codes)))
    by_code = {series.code: series for series in stocks}
    signal_rows = []

    for record in chosen.to_dict("records"):
        column = column_of[record["code"]]
        start, end = int(starts[column]), int(market.stock_end[starts[column]])
        entry_row = start + int(np.searchsorted(market.dates[start:end + 1], record["entry_date"]))
        signal_rows.append(entry_row - 1)

    chosen = chosen.assign(signal_row=signal_rows, cell=[choices[int(year)] for year in chosen["entry_year"]])
    results: dict[int, dict] = {}

    for cell, group in chosen.groupby("cell"):
        rows_array = np.unique(group["signal_row"].to_numpy())
        signals = support.SignalSet(market, rows_array)
        window, take, stop_name, hold = support.parse_cell(cell)
        exits = signals.exits(take, stop_name, hold)

        for position, row in enumerate(rows_array.tolist()):
            results[(cell, row)] = {name: exits[name][position] for name in ("offset", "exit_rows", "exit_ratio", "net", "reason", "usable", "broken")}
            results[(cell, row)]["entry_price"] = float(signals.entry_price[position])
            results[(cell, row)]["stop_ratio"] = float(signals.stop_ratio[stop_name][position])

    collector = Collector("support")
    pivot_cache: dict[str, dict] = {}

    for record in chosen.to_dict("records"):
        cell = record["cell"]
        window, take, stop_name, hold = support.parse_cell(cell)
        signal_row = int(record["signal_row"])
        result = results[(cell, signal_row)]
        series = by_code[record["code"]]
        column = column_of[record["code"]]
        start = int(starts[column])
        local_signal = signal_row - start
        local_entry = local_signal + 1
        local_exit = int(result["exit_rows"]) - start
        dates, close, low, high, open_price = series.dates, series.close, series.low, series.high, series.open_price
        rows = len(dates)

        if record["code"] not in pivot_cache:
            pivot_cache[record["code"]] = swing.confirmed_pivots(low, high, support.TREND_SPAN)

        pivots = pivot_cache[record["code"]]
        average = swing.moving_average(close, window)
        entry = float(open_price[local_entry])
        stop = float(result["stop_ratio"]) * entry
        take_price = entry * (1.0 + take / 100.0)
        reason = int(result["reason"])
        exit_price_chart = float(result["exit_ratio"]) * entry

        if reason == 1:
            rule_price = min(float(open_price[local_exit]), stop)
        elif reason == 2:
            rule_price = max(float(open_price[local_exit]), take_price)
        else:
            rule_price = float(close[local_exit])

        ledger_exit_price = (record["net"] / 100.0 + 1.0) / support.COST * entry
        latest_low = float(market.latest_low[signal_row])
        pairs_low = pivot_pairs(pivots, local_signal, rows, "low")
        base_pivot = pairs_low[-1] if pairs_low else None
        shape_now = bool(market.shape[window][signal_row])
        check = [exact_check("매수일 = 신호 다음 거래일", int(record["entry_date"]), int(dates[local_entry]), date_text),
                 exact_check("시세 배열 맞춤(전 종목 배열과 종목 배열)", int(market.dates[signal_row]), int(dates[local_signal]), date_text),
                 price_check("근거 저점(신호일까지 확정된 마지막 저점)", latest_low, base_pivot["value"] if base_pivot else float("nan")),
                 exact_check("청산일", int(record["exit_date"]), int(dates[local_exit]), date_text),
                 price_check("매도가(원장 수익률에서 거꾸로) vs 규칙 가격", ledger_exit_price, rule_price),
                 range_check("매도가가 그날 가격 범위 안", ledger_exit_price, low[local_exit], high[local_exit]),
                 percent_check("순수익", record["net"], float(result["net"]))]

        if record["side"] == "shape":
            check.insert(1, exact_check(f"모양 조건({window}일선 지지) 다시 계산", "성립", "성립" if shape_now else "불성립"))

        previous = average[local_signal - 1]
        touch = average[local_signal] * swing.SUPPORT_TOUCH
        side_text = "모양 있음" if record["side"] == "shape" else "모양 없음 비교(거래대금 상위 15에서 고름)"
        why = [{"title": f"매수 근거 — {side_text}", "rows": [
            f"칸: {ledger_labels.support_label(cell)} ({record['entry_year']}년에 그 전 매매만으로 고른 칸)",
            f"신호일 {date_text(dates[local_signal])}: 저가 {price_text(low[local_signal])} {'≤' if low[local_signal] <= touch else '>'} "
            f"{window}일선 {price_text(average[local_signal])} × 1.01 = {price_text(touch)}",
            f"종가 {price_text(close[local_signal])} {'>' if close[local_signal] > average[local_signal] else '≤'} {window}일선 {price_text(average[local_signal])}",
            f"{window}일선 {price_text(average[local_signal])} {'>' if average[local_signal] > previous else '≤'} 전날 {window}일선 {price_text(previous)}",
            f"모양 성립: {'예' if shape_now else '아니오'}",
            f"매수 {date_text(dates[local_entry])} 시가 {price_text(entry)}",
        ]}]
        exit_rule = {1: f"저가 {price_text(low[local_exit])} ≤ 손절 {price_text(stop)} → min(시가 {price_text(open_price[local_exit])}, 손절) = {price_text(rule_price)}",
                     2: f"고가 {price_text(high[local_exit])} ≥ 익절 {price_text(take_price)} → max(시가 {price_text(open_price[local_exit])}, 익절) = {price_text(rule_price)}",
                     0: f"{hold}거래일 기한(또는 시세 끝) 종가 {price_text(rule_price)}"}[reason]
        why.append({"title": "청산 규칙과 매도", "rows": [
            (f"손절: 근거 저점 {price_text(latest_low)} ({date_text(dates[base_pivot['row']])}, 확정 {date_text(dates[base_pivot['confirm']])}) × 0.97 = {price_text(stop)}"
             if base_pivot else "손절: 근거 저점 없음"),
            f"익절: 매수가 {price_text(entry)} × {1 + take / 100:.2f} = {price_text(take_price)}",
            f"매도 {date_text(dates[local_exit])}: {exit_rule}",
            f"순수익: ({price_text(rule_price)} ÷ {price_text(entry)} × 비용 {support.COST:.6f} − 1) × 100 = {float(result['net']):+.2f}%",
        ]})
        earliest_row = base_pivot["row"] if base_pivot else local_signal
        first, last_shown = chart_window(rows, local_signal, earliest_row, local_exit)
        used = {("low", base_pivot["row"])} if base_pivot else set()
        markers = pivot_markers(pivots, first, last_shown, dates, used)
        flags = [{"date": int(dates[local_signal]), "label": "신호", "kind": "mark"}]

        if base_pivot:
            markers.append({"date": int(dates[base_pivot["row"]]), "price": rounded(base_pivot["value"]), "kind": "low_accent",
                            "label": f"근거 저점 {price_text(base_pivot['value'])}"})
            flags.append({"date": int(dates[base_pivot["confirm"]]), "label": "저점 확정", "kind": "muted"})

        chart = {"candles": candle_rows(dates, open_price, high, low, close, series.volume, first, last_shown),
                 "lines": [{"price": rounded(take_price), "label": f"익절 {price_text(take_price)}", "kind": "take", "from": int(dates[local_entry])},
                           {"price": rounded(entry), "label": f"매수 {price_text(entry)}", "kind": "accent", "from": int(dates[local_entry])},
                           {"price": rounded(stop), "label": f"손절 {price_text(stop)}", "kind": "stop", "from": int(dates[local_entry])}],
                 "series": [{"label": f"{window}일선", "kind": "mark", "values": series_values(average, first, last_shown)}],
                 "segments": [], "markers": markers, "flags": flags,
                 "shades": [{"from": int(dates[local_entry]), "to": int(dates[local_exit]), "kind": "hold"}],
                 "buy": {"date": int(dates[local_entry]), "price": rounded(entry)},
                 "sell": {"date": int(dates[local_exit]), "price": rounded(ledger_exit_price)}, "why": why, "check": check}
        entry_item = {"code": record["code"], "name": series.name, "buy": int(record["entry_date"]), "sell": int(record["exit_date"]),
                      "net": float(record["net"]), "reason": SUPPORT_REASON_KEYS[reason], "reason_label": SUPPORT_REASONS[reason],
                      "note": f"{ledger_labels.support_label(cell)} · {'모양 있음' if record['side'] == 'shape' else '모양 없음 비교'}",
                      "tags": [f"side:{record['side']}", f"cell:{cell}", f"reason:{SUPPORT_REASON_KEYS[reason]}"]}
        collector.add(entry_item, chart)

    used_cells = sorted(set(chosen["cell"]))
    meta = {"key": "support", "title": "이평선 지지 (스터디 37 자산 곡선)", "code_name": "ma_support_portfolio_trades.parquet",
            "summary": [f"모양 있음 {len(shape)}건 전부, 모양 없음 비교 {len(baseline)}건 중 무작위 {len(picked_baseline)}건 (seed {SEED})",
                        "모양: 신호일 저가 ≤ N일선 × 1.01 · 종가 > N일선 · N일선 > 전날 N일선, 다음 날 시가 매수",
                        "손절 = 신호일까지 확정된 마지막 저점(k = 5) × 0.97, 익절·최대 보유는 해마다 고른 칸. 일봉은 스터디 33 load_inputs(20261006) 수정주가"]}
    filters = [{"name": "구분", "options": [{"key": "side:shape", "label": "모양 있음"}, {"key": "side:baseline", "label": "모양 없음 비교"}]},
               {"name": "칸", "options": [{"key": f"cell:{cell}", "label": ledger_labels.support_label(cell)} for cell in used_cells]},
               reason_group({"stop": "손절", "take": "익절", "time": "기한"})]
    del market
    gc.collect()
    return write_test(collector, meta, filters)


# ── 4) DevScale 하향 추세 거름 ─────────────────────────────────────────────────

DEVSCALE_REASONS = {"stop": "손절", "stop_gap": "시가 손절", "take_profit": "익절", "take_profit_gap": "시가 익절", "zone_open": "시가 구간 이탈",
                    "zone_band_low": "띠 하단", "zone_band_high": "띠 상단", "zone_align": "정배열 깨짐", "end_of_data": "시세 끝"}
DEVSCALE_REASON_KEYS = {"stop": "stop", "stop_gap": "stop", "zone_band_low": "stop", "zone_align": "stop", "take_profit": "take",
                        "take_profit_gap": "take", "zone_band_high": "take", "zone_open": "time", "end_of_data": "time"}


def build_devscale() -> list[Path]:
    trades = pd.read_parquet(devscale.TRADES_PATH)
    down_k3 = trades["trend_k3"].to_numpy() == swing.TREND_DOWN
    down_k5 = trades["trend_k5"].to_numpy() == swing.TREND_DOWN
    generator = np.random.default_rng(SEED)
    picks = [(index, "dropped", sample) for index, sample in sample_extremes(trades[down_k3 | down_k5], 5, 10, generator, "return_percent_mid")]
    picks += [(index, "kept", sample) for index, sample in sample_extremes(trades[~(down_k3 | down_k5)], 5, 10, generator, "return_percent_mid")]
    bars = devscale.load_bars()
    wanted_codes = sorted({trades.loc[index, "code"] for index, _, _ in picks})
    frames = {code: frame.reset_index(drop=True) for code, frame in bars[bars["code"].isin(wanted_codes)].groupby("code", sort=True)}
    del bars
    gc.collect()
    collector = Collector("devscale")
    simulated: dict[str, list] = {}
    cell_rules = [(name, span, rule) for name, span, rule in [(cell[0], cell[2], cell[3]) for cell in devscale.CELLS] if span is not None]

    for index, pool, sample in picks:
        record = trades.loc[index].to_dict()
        frame = frames[record["code"]]

        if record["code"] not in simulated:
            simulated[record["code"]] = devscale.simulate_ticker(frame, devscale.TAKE_PROFIT_PERCENT, 0.0, "in_universe", False)[0]

        dates = frame["Date"].dt.strftime("%Y%m%d").astype(np.int64).to_numpy()
        open_price, high, low, close = (frame[name].to_numpy(float) for name in ("Open", "High", "Low", "Close"))
        volume = frame["Volume"].to_numpy()
        rows = len(dates)
        entry_date = int(pd.Timestamp(record["entry_date"]).strftime("%Y%m%d"))
        exit_date = int(pd.Timestamp(record["exit_date"]).strftime("%Y%m%d"))
        entry_row = int(np.searchsorted(dates, entry_date))
        exit_row = int(np.searchsorted(dates, exit_date))
        matched = [item for item in simulated[record["code"]] if int(pd.Timestamp(item["entry_date"]).strftime("%Y%m%d")) == entry_date]
        again = matched[0] if matched else None
        features = devscale.ticker_features(open_price, high, low, close)
        band_low = devscale.band_price(features["fold_base20"], -(devscale.PULLBACK_PERCENT + devscale.HYSTERESIS_PERCENT))
        band_high = devscale.band_price(features["fold_base20"], devscale.ENTRY_UPPER_PERCENT + devscale.HYSTERESIS_PERCENT)
        entry_kind = record["entry_kind"]
        entry_chart = open_price[entry_row] if entry_kind == "open" else close[entry_row]
        entry = float(record["entry_price"])
        take_price = devscale.ceil_to_tick(entry * (1.0 + devscale.TAKE_PROFIT_PERCENT / 100.0))
        stop_price = entry * (1.0 - devscale.STOP_LOSS_PERCENT / 100.0)
        reason = record["exit_reason"]
        same_day = exit_row == entry_row

        if reason in ("stop", "zone_band_low", "zone_align"):
            line = {"stop": stop_price, "zone_band_low": band_low[exit_row], "zone_align": features["align_threshold"][exit_row]}[reason]
            rule_price = line if same_day else min(line, open_price[exit_row])
        elif reason == "take_profit":
            rule_price = take_price
        elif reason == "zone_band_high":
            rule_price = band_high[exit_row]
        elif reason in ("stop_gap", "take_profit_gap", "zone_open"):
            rule_price = open_price[exit_row]
        else:
            rule_price = close[rows - 1]

        check = [exact_check("다시 돌린 매매에 같은 매수일이 있음", "있음", "있음" if again else "없음"),
                 price_check(f"매수가 = 매수일 {'시가' if entry_kind == 'open' else '종가'}", entry, entry_chart),
                 price_check("매도가 vs 규칙 가격", record["exit_price"], rule_price),
                 range_check("매도가가 그날 가격 범위 안", record["exit_price"], low[exit_row], high[exit_row])]

        if again:
            check += [exact_check("매도일", exit_date, int(pd.Timestamp(again["exit_date"]).strftime("%Y%m%d")), date_text),
                      exact_check("청산 사유", DEVSCALE_REASONS.get(reason, reason), DEVSCALE_REASONS.get(again["exit_reason"], again["exit_reason"])),
                      price_check("매도가(다시 돌린 값)", record["exit_price"], again["exit_price"]),
                      price_check("순손익(원, 50만 원 매매)", record["net_won_mid"], again["net_won_mid"])]

        markers, segments, flags = [], [], []
        trend_rows = []
        used: set = set()
        earliest_row = entry_row
        state_row = entry_row - 1

        for span, kind, prefix in ((3, "accent", "k3 "), (5, "mark", "k5 ")):
            pivots = swing.confirmed_pivots(low, high, span)
            state = int(swing.trend_state(pivots, rows)["state"][state_row])
            check.append(exact_check(f"추세 k = {span} (매수 전날 마감 기준)", devscale.TREND_NAMES[int(record[f"trend_k{span}"])], devscale.TREND_NAMES[state]))
            pairs_low = pivot_pairs(pivots, state_row, rows, "low")
            pairs_high = pivot_pairs(pivots, state_row, rows, "high")
            new_markers, new_segments, new_flags = pivot_drawings(pairs_low, pairs_high, dates, kind, prefix)
            markers += new_markers
            segments += new_segments

            if span == 3:
                flags += new_flags
                markers += pivot_markers(pivots, 0, rows - 1, dates, set())

            earliest_row = min([earliest_row] + [pivot["row"] for pivot in pairs_low + pairs_high])
            trend_rows.append(f"k = {span}: {devscale.TREND_NAMES[state]}")
            trend_rows += [f"  {text}" for text in trend_rows_text(pairs_low, pairs_high, dates, devscale.TREND_NAMES[state])]

        dropped_cells = [name for name, span, rule in cell_rules
                         if not (record[f"trend_k{span}"] != swing.TREND_DOWN if rule == "not_down" else record[f"trend_k{span}"] == swing.TREND_UP)]
        trend_rows.append("이 매매를 빼는 칸: " + (", ".join(f"{name} {dict((cell[0], cell[1]) for cell in devscale.CELLS)[name]}" for name in dropped_cells)
                                                  if dropped_cells else "없음"))
        deviation = devscale.folded_deviation_percent(entry_chart, features["fold_base20"][entry_row])
        down_lines = f"손절 {price_text(stop_price)}, 띠 하단 {price_text(band_low[exit_row])}"

        if not features["aligned_hold"][exit_row]:
            down_lines += f", 정배열 문턱 {price_text(features['align_threshold'][exit_row])}"

        exit_rule = {"stop": f"저가 {price_text(low[exit_row])} ≤ 아래쪽 선 max({down_lines}) → 매도 {price_text(rule_price)}",
                     "zone_band_low": f"저가 {price_text(low[exit_row])} ≤ 아래쪽 선 max({down_lines}) → 매도 {price_text(rule_price)}",
                     "zone_align": f"저가 {price_text(low[exit_row])} ≤ 아래쪽 선 max({down_lines}) → 매도 {price_text(rule_price)}",
                     "take_profit": f"고가 {price_text(high[exit_row])} ≥ 익절 {price_text(take_price)} → 매도 {price_text(rule_price)}",
                     "zone_band_high": f"고가 {price_text(high[exit_row])} ≥ 띠 상단 {price_text(band_high[exit_row])} → 매도 {price_text(rule_price)}",
                     "stop_gap": f"시가 {price_text(open_price[exit_row])} ≤ 손절 {price_text(stop_price)} → 시가 매도",
                     "take_profit_gap": f"시가 {price_text(open_price[exit_row])} ≥ 익절 {price_text(take_price)} → 시가 매도",
                     "zone_open": f"시가 {price_text(open_price[exit_row])}에서 정배열·보유 띠(이격 −12–+9%) 밖 → 시가 매도",
                     "end_of_data": f"시세 끝 종가 {price_text(rule_price)}"}.get(reason, reason)
        why = [{"title": "매수 근거 — DevScale 정배열 눌림", "rows": [
                    f"{date_text(entry_date)} {'시가' if entry_kind == 'open' else '종가'} {price_text(entry_chart)} > 정배열 문턱 {price_text(features['align_threshold'][entry_row])}",
                    f"접은 20일선 대비 이격 {deviation:+.2f}% (−8–+5% 안)",
                    f"전날 거래대금 순위 상위 {devscale.UNIVERSE_TOP} 안 · ATR ≤ {devscale.ATR_MAX_PERCENT}% · 시가 이격 ≥ {devscale.OPEN_DEVIATION_MIN_PERCENT}%",
                ]},
               {"title": "추세 판정 (매수 전날 " + date_text(dates[state_row]) + " 마감까지 확정된 점)", "rows": trend_rows},
               {"title": "청산 규칙과 매도", "rows": [
                    f"익절: 올림틱(매수가 {price_text(entry)} × 1.03 = {price_text(entry * 1.03)}) = {price_text(take_price)}",
                    f"손절: 매수가 × 0.935 = {price_text(stop_price)}",
                    f"매도 {date_text(exit_date)} {DEVSCALE_REASONS.get(reason, reason)}: {exit_rule}",
                    f"순손익: {record['net_won_mid']:+,.0f}원 (50만 원 매매, {record['return_percent_mid']:+.2f}%)",
                ]}]
        first, last_shown = chart_window(rows, entry_row, earliest_row, exit_row)
        markers = [item for item in markers if dates[first] <= item["date"] <= dates[last_shown]]
        flags.append({"date": entry_date, "label": "매수일", "kind": "mark"})
        chart = {"candles": candle_rows(dates, open_price, high, low, close, volume, first, last_shown),
                 "lines": [{"price": rounded(take_price), "label": f"익절 {price_text(take_price)}", "kind": "take", "from": entry_date},
                           {"price": rounded(entry), "label": f"매수 {price_text(entry)}", "kind": "accent", "from": entry_date},
                           {"price": rounded(stop_price), "label": f"손절 {price_text(stop_price)}", "kind": "stop", "from": entry_date}],
                 "series": [{"label": "보유 띠 하단", "kind": "muted", "values": series_values(band_low, first, last_shown)},
                            {"label": "보유 띠 상단", "kind": "muted", "values": series_values(band_high, first, last_shown)}],
                 "segments": segments, "markers": markers, "flags": flags,
                 "shades": [{"from": entry_date, "to": exit_date, "kind": "hold"}],
                 "buy": {"date": entry_date, "price": rounded(entry)}, "sell": {"date": exit_date, "price": rounded(record["exit_price"])},
                 "why": why, "check": check}
        entry_item = {"code": record["code"], "name": record["name"], "buy": entry_date, "sell": exit_date,
                      "net": float(record["return_percent_mid"]), "reason": DEVSCALE_REASON_KEYS.get(reason, "time"),
                      "reason_label": DEVSCALE_REASONS.get(reason, reason),
                      "note": f"{'빠지는 매매' if pool == 'dropped' else '남는 매매'} · {SAMPLE_LABELS[sample]} · k3 {devscale.TREND_NAMES[int(record['trend_k3'])]} · k5 {devscale.TREND_NAMES[int(record['trend_k5'])]}",
                      "tags": [f"pool:{pool}", f"sample:{sample}", f"reason:{DEVSCALE_REASON_KEYS.get(reason, 'time')}"] + [f"cell:{name}" for name in dropped_cells]}
        collector.add(entry_item, chart)

    meta = {"key": "devscale", "title": "DevScale 하향 추세 거름 (스터디 37)", "code_name": "devscale_trend_trades.parquet",
            "summary": [f"원장 {len(trades):,}건. 빠지는 매매(k = 3 또는 k = 5 하향) 20건·남는 매매 20건, 각각 손실 하위 5·이익 상위 5·무작위 10 (seed {SEED})",
                        "추세는 매수 전날 마감까지 확정된 저점·고점 둘씩으로 판정. 상승 = 저점↑·고점↑, 하향 = 저점↓",
                        "익절 = 올림틱(매수가 × 1.03), 손절 = 매수가 × 0.935, 정배열·보유 띠를 벗어나면 청산. 일봉은 devscale_trend_filter.load_bars(수정주가)"]}
    filters = [{"name": "표본", "options": [{"key": "pool:dropped", "label": "빠지는 매매"}, {"key": "pool:kept", "label": "남는 매매"}]},
               {"name": "빼는 칸", "options": [{"key": f"cell:{cell[0]}", "label": f"{cell[0]} {cell[1]}"} for cell in devscale.CELLS if cell[2] is not None]},
               {"name": "뽑은 방법", "options": [{"key": f"sample:{key}", "label": label} for key, label in SAMPLE_LABELS.items()]},
               reason_group({"stop": "손절 쪽", "take": "익절 쪽", "time": "시가 이탈·시세 끝"})]
    return write_test(collector, meta, filters)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    written = build_surge()
    print("surge 끝", flush=True)
    stocks, _, _ = standalone.study33.load_inputs(standalone.LAST_DATE)
    last_market_date = int(max(int(series.dates[-1]) for series in stocks))
    written += build_swing(stocks, last_market_date)
    print("swing 끝", flush=True)
    written += build_support(stocks)
    print("support 끝", flush=True)
    del stocks
    STOCK_CACHE.clear()
    gc.collect()
    written += build_devscale()
    print("devscale 끝", flush=True)
    write_bytes(HERE / "index.html", PAGE)
    written.append(HERE / "index.html")

    for path in sorted(HERE.glob("*.json")):
        if path not in written:
            path.unlink()   # 지난 실행의 묶음 파일 정리

    total = sum(path.stat().st_size for path in written)
    print(f"파일 {len(written)}개, 합계 {total / 1e6:.2f}MB, 가장 큰 파일 {max(path.stat().st_size for path in written) / 1e6:.2f}MB")

    for key in ("surge", "swing", "devscale", "support"):
        meta = json.loads((HERE / f"{key}.json").read_text(encoding="utf-8"))["meta"]
        print(f"{key}: 매매 {meta['trades_ok']}/{meta['trades_total']} 전부 일치, 항목 {meta['checks_ok']}/{meta['checks_total']}")

        for item in meta["mismatches"][:40]:
            print(f"  어긋남 {item['code']} {item['name']} 매수 {item['buy']}: " + " | ".join(f"{label} 원장 {left} / 차트 {right}" for label, left, right in item["items"]))

    return 0


PAGE = r"""<title>시험 매매 리플레이</title>
<style>
:root{--bg:#f6f7f9;--panel:#ffffff;--fg:#1b2230;--muted:#5d6878;--line:#dde2e9;--accent:#2c5fa8;--up:#d23a3a;--down:#2a62c9;--stop:#c0392b;--take:#1e8a4c;--time:#8a6d1e;--mark:#7a3fb0;--soft:#eef1f6;--watch:#f3ecd9;--bad:#c0392b;--badsoft:#fbe9e7;color-scheme:light}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#12161d;--panel:#1a2029;--fg:#e4e8ef;--muted:#9aa5b5;--line:#2c3441;--accent:#7aa7e8;--up:#f06868;--down:#6b9cf0;--stop:#ef6f61;--take:#4cc283;--time:#d6b45a;--mark:#b98ae6;--soft:#222a35;--watch:#2e2a1c;--bad:#ef6f61;--badsoft:#3a2020;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#12161d;--panel:#1a2029;--fg:#e4e8ef;--muted:#9aa5b5;--line:#2c3441;--accent:#7aa7e8;--up:#f06868;--down:#6b9cf0;--stop:#ef6f61;--take:#4cc283;--time:#d6b45a;--mark:#b98ae6;--soft:#222a35;--watch:#2e2a1c;--bad:#ef6f61;--badsoft:#3a2020;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:"IBM Plex Sans KR",system-ui,sans-serif;font-size:14px;line-height:1.5}
.wrap{max-width:1500px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:0 0 4px}
.top{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:baseline;justify-content:space-between}
.score{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.chip{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:4px 8px;font-size:13px}
.chip b{font-family:"IBM Plex Mono",monospace}
.chip.badchip{border-color:var(--bad);color:var(--bad)}
.code{color:var(--muted);font-size:11px;font-family:"IBM Plex Mono",monospace}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
.tabs button,.theme{background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px 10px;font:inherit;cursor:pointer}
.tabs button[aria-pressed="true"]{background:var(--accent);border-color:var(--accent);color:var(--panel)}
.tabs button[aria-pressed="true"] .code{color:var(--panel)}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px;min-width:0}
.summary{margin:0 0 8px;padding-left:18px;color:var(--muted);font-size:13px}
.main{display:grid;grid-template-columns:330px minmax(0,1fr);gap:12px;align-items:start}
@media (max-width:860px){.main{grid-template-columns:minmax(0,1fr)}}
.filters{display:flex;flex-direction:column;gap:6px;margin-bottom:8px}
.filters select,.filters input{width:100%;font:inherit;padding:5px 6px;border:1px solid var(--line);border-radius:6px;background:var(--panel);color:var(--fg)}
.count{color:var(--muted);font-size:12px}
.list{max-height:70vh;overflow-y:auto;border-top:1px solid var(--line)}
@media (max-width:860px){.list{max-height:40vh}}
.row{padding:6px 4px;border-bottom:1px solid var(--line);cursor:pointer}
.row:hover{background:var(--soft)}
.row.sel{background:var(--soft);box-shadow:inset 3px 0 0 var(--accent)}
.row.badrow{background:var(--badsoft)}
.row .head{display:flex;justify-content:space-between;gap:8px}
.row .nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sub{color:var(--muted);font-size:12px;overflow-wrap:anywhere}
.pnl{font-family:"IBM Plex Mono",monospace;white-space:nowrap}
.pos{color:var(--up)}
.neg{color:var(--down)}
.tag{display:inline-block;font-size:11px;border-radius:4px;padding:0 4px;margin-left:4px;border:1px solid currentColor}
.tag.stop{color:var(--stop)}
.tag.take{color:var(--take)}
.tag.time{color:var(--time)}
.tag.other{color:var(--muted)}
.tag.bad{color:var(--bad)}
.chartbox{position:relative;width:100%}
.chartbox canvas{display:block;width:100%;height:440px}
@media (max-width:860px){.chartbox canvas{height:360px}}
.title{display:flex;flex-wrap:wrap;gap:4px 12px;align-items:baseline;margin-bottom:6px}
.title h2{font-size:17px;margin:0}
.legend{display:flex;flex-wrap:wrap;gap:4px 12px;font-size:12px;color:var(--muted);margin:4px 0}
.legend i{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:4px}
.why h3{font-size:14px;margin:12px 0 4px}
.why ul{margin:0;padding-left:18px}
.why li{overflow-wrap:anywhere}
table.check{border-collapse:collapse;width:100%;font-size:13px;margin-top:4px;table-layout:fixed}
table.check td,table.check th{border-bottom:1px solid var(--line);padding:3px 4px;text-align:left;vertical-align:top;overflow-wrap:anywhere}
table.check td.num{font-family:"IBM Plex Mono",monospace}
table.check tr.badline td{color:var(--bad);background:var(--badsoft);font-weight:600}
.mis{font-size:12px;color:var(--bad);margin:4px 0 0;max-height:120px;overflow-y:auto;overflow-wrap:anywhere}
#tip{position:absolute;pointer-events:none;background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:4px 6px;font-size:12px;font-family:"IBM Plex Mono",monospace;display:none;white-space:nowrap;z-index:2}
.empty{color:var(--muted);padding:24px;text-align:center}
</style>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=IBM+Plex+Sans+KR:wght@400;600&display=swap" rel="stylesheet">
<div class="wrap">
  <div class="top">
    <div>
      <p class="sub"><a href="https://claude.ai/artifact/Ahgj1CbDYc7aPfkujiQRQR#studies">← 스터디 목록</a></p>
      <h1>시험 매매 리플레이</h1>
      <div class="sub">원장의 매수가·매도가·매도일·청산 사유·수익률을 시험 코드로 다시 계산한 값과 일봉 가격에 대조한다. 어긋난 건은 빨갛게 표시한다. ↑↓ 키로 다음 건.</div>
    </div>
    <button class="theme" id="theme" type="button">밝게·어둡게</button>
  </div>
  <div class="score" id="score"></div>
  <div class="tabs" id="tabs"></div>
  <div class="panel" style="margin-bottom:12px">
    <ul class="summary" id="summary"></ul>
    <div class="mis" id="mis"></div>
  </div>
  <div class="main">
    <div class="panel">
      <div class="filters" id="filters"></div>
      <div class="count" id="count"></div>
      <div class="list" id="list"></div>
    </div>
    <div class="panel">
      <div class="title" id="title"></div>
      <div class="legend" id="legend"></div>
      <div class="chartbox" id="chartbox"><canvas id="chart"></canvas><div id="tip"></div></div>
      <div class="why" id="why"></div>
    </div>
  </div>
</div>
<script>
const TESTS = [
  {key: "surge", label: "급등 뒤 박스 돌파", code: "스터디 35"},
  {key: "swing", label: "쌍바닥·추세", code: "스터디 37 단독"},
  {key: "devscale", label: "DevScale 하향 추세 거름", code: "스터디 37"},
  {key: "support", label: "이평선 지지", code: "스터디 37 자산 곡선"}
];
const data = {};
const chunks = {};
const state = {test: "surge", filters: {}, sort: "recent", search: "", selected: null, visible: []};
const MAX_ROWS = 600;

function css(name) {
  return getComputedStyle(document.documentElement).getPropertyValue("--" + name).trim();
}

function money(value) {
  if (value === null || value === undefined || !isFinite(value)) {
    return "—";
  }
  const whole = Math.abs(value - Math.round(value)) < 0.005;
  return value.toLocaleString("ko-KR", {minimumFractionDigits: whole ? 0 : 2, maximumFractionDigits: whole ? 0 : 2});
}

function dateText(date) {
  const text = String(date);
  return text.slice(0, 4) + "-" + text.slice(4, 6) + "-" + text.slice(6);
}

function percent(value) {
  return (value > 0 ? "+" : "") + value.toFixed(2) + "%";
}

function escapeText(text) {
  return String(text).replace(/[&<>"]/g, (character) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;"}[character]));
}

function reasonClass(reason) {
  return ["stop", "take", "time"].includes(reason) ? reason : "other";
}

function renderScore() {
  let checks = 0;
  let checksOk = 0;
  let trades = 0;
  let tradesOk = 0;
  const parts = [];
  for (const test of TESTS) {
    const meta = data[test.key].meta;
    checks += meta.checks_total;
    checksOk += meta.checks_ok;
    trades += meta.trades_total;
    tradesOk += meta.trades_ok;
    const bad = meta.trades_ok < meta.trades_total;
    parts.push(`<span class="chip${bad ? " badchip" : ""}">${escapeText(test.label)} 매매 <b>${meta.trades_ok}/${meta.trades_total}</b> · 항목 <b>${meta.checks_ok}/${meta.checks_total}</b></span>`);
  }
  const rate = (checksOk / checks * 100).toFixed(2);
  const tradeRate = (tradesOk / trades * 100).toFixed(1);
  const badClass = tradesOk < trades ? " badchip" : "";
  document.getElementById("score").innerHTML =
    `<span class="chip${badClass}">전체 대조 항목 일치 <b>${rate}%</b> (${checksOk.toLocaleString()}/${checks.toLocaleString()})</span>` +
    `<span class="chip${badClass}">모든 항목이 맞는 매매 <b>${tradeRate}%</b> (${tradesOk}/${trades})</span>` + parts.join("");
}

function renderTabs() {
  document.getElementById("tabs").innerHTML = TESTS.map((test) =>
    `<button type="button" data-key="${test.key}" aria-pressed="${test.key === state.test}">${escapeText(test.label)} <span class="code">${escapeText(test.code)}</span></button>`).join("");
  for (const button of document.querySelectorAll("#tabs button")) {
    button.onclick = () => selectTest(button.dataset.key);
  }
}

function selectTest(key) {
  state.test = key;
  state.filters = {};
  state.search = "";
  state.selected = null;
  renderTabs();
  const test = data[key];
  document.getElementById("summary").innerHTML = test.meta.summary.map((line) => `<li>${escapeText(line)}</li>`).join("") +
    `<li class="code">원장 ${escapeText(test.meta.code_name)}</li>`;
  const mismatches = test.meta.mismatches;
  document.getElementById("mis").innerHTML = mismatches.length ? "어긋난 매매 " + mismatches.length + "건<br>" + mismatches.slice(0, 30).map((item) =>
    `${escapeText(item.name)}(${item.code}) ${dateText(item.buy)} — ` + item.items.map((part) => `${escapeText(part[0])} 원장 ${escapeText(part[1])} / 차트 ${escapeText(part[2])}`).join("; ")).join("<br>") : "";
  const groups = test.filters.map((group, index) =>
    `<select data-group="${index}" aria-label="${escapeText(group.name)}"><option value="">${escapeText(group.name)}: 전체</option>` +
    group.options.map((option) => `<option value="${escapeText(option.key)}">${escapeText(option.label)}</option>`).join("") + "</select>").join("");
  document.getElementById("filters").innerHTML = groups +
    `<select id="sort" aria-label="정렬"><option value="recent">최근 매수 순</option><option value="best">수익 높은 순</option><option value="worst">수익 낮은 순</option><option value="bad">어긋남 먼저</option></select>` +
    `<input id="search" type="search" placeholder="종목명·코드 검색">`;
  for (const select of document.querySelectorAll("#filters select[data-group]")) {
    select.onchange = () => {
      state.filters[select.dataset.group] = select.value;
      renderList(true);
    };
  }
  const sort = document.getElementById("sort");
  sort.value = state.sort;
  sort.onchange = () => {
    state.sort = sort.value;
    renderList(true);
  };
  document.getElementById("search").oninput = (event) => {
    state.search = event.target.value.trim().toLowerCase();
    renderList(true);
  };
  renderList(true);
}

function filtered() {
  const test = data[state.test];
  const rows = test.trades.filter((trade) => {
    for (const value of Object.values(state.filters)) {
      if (value && !trade.tags.includes(value)) {
        return false;
      }
    }
    if (state.search && !(trade.name.toLowerCase().includes(state.search) || trade.code.includes(state.search))) {
      return false;
    }
    return true;
  });
  const order = {
    recent: (first, second) => second.buy - first.buy,
    best: (first, second) => second.net - first.net,
    worst: (first, second) => first.net - second.net,
    bad: (first, second) => (second.bad - first.bad) || (second.buy - first.buy)
  }[state.sort];
  return rows.slice().sort(order);
}

function renderList(resetSelection) {
  const rows = filtered();
  state.visible = rows;
  const shown = rows.slice(0, MAX_ROWS);
  document.getElementById("count").textContent = `${rows.length.toLocaleString()}건` + (rows.length > MAX_ROWS ? ` (앞 ${MAX_ROWS}건만 보임)` : "");
  document.getElementById("list").innerHTML = shown.map((trade) =>
    `<div class="row${trade.ok ? "" : " badrow"}" data-id="${trade.id}">` +
    `<div class="head"><span class="nm">${escapeText(trade.name)} <span class="code">${trade.code}</span></span>` +
    `<span class="pnl ${trade.net >= 0 ? "pos" : "neg"}">${percent(trade.net)}</span></div>` +
    `<div class="sub">${dateText(trade.buy)} – ${dateText(trade.sell)}<span class="tag ${reasonClass(trade.reason)}">${escapeText(trade.reason_label)}</span>` +
    (trade.ok ? "" : `<span class="tag bad">어긋남 ${trade.bad}</span>`) + `</div>` +
    `<div class="sub">${escapeText(trade.note)}</div></div>`).join("") || `<div class="empty">조건에 맞는 매매가 없다</div>`;
  for (const row of document.querySelectorAll("#list .row")) {
    row.onclick = () => selectTrade(row.dataset.id);
  }
  if (resetSelection || !rows.some((trade) => trade.id === state.selected)) {
    if (shown.length) {
      selectTrade(shown[0].id);
    } else {
      clearChart();
    }
  }
}

function clearChart() {
  state.selected = null;
  current = null;
  document.getElementById("title").innerHTML = "";
  document.getElementById("legend").innerHTML = "";
  document.getElementById("why").innerHTML = "";
  const canvas = document.getElementById("chart");
  canvas.getContext("2d").clearRect(0, 0, canvas.width, canvas.height);
}

async function selectTrade(id) {
  state.selected = id;
  for (const row of document.querySelectorAll("#list .row")) {
    row.classList.toggle("sel", row.dataset.id === id);
  }
  const selectedRow = document.querySelector(`#list .row[data-id="${id}"]`);
  if (selectedRow) {
    selectedRow.scrollIntoView({block: "nearest"});
  }
  const trade = data[state.test].trades.find((item) => item.id === id);
  if (!trade.candles) {
    if (!chunks[trade.chunk]) {
      chunks[trade.chunk] = fetch(trade.chunk).then((response) => response.json());
    }
    const chunk = await chunks[trade.chunk];
    Object.assign(trade, chunk[trade.id]);
  }
  if (state.selected !== id) {
    return;
  }
  renderDetail(trade);
}

function renderDetail(trade) {
  document.getElementById("title").innerHTML =
    `<h2>${escapeText(trade.name)} <span class="code">${trade.code}</span></h2>` +
    `<span>매수 ${dateText(trade.buy.date)} ${money(trade.buy.price)} → 매도 ${dateText(trade.sell.date)} ${money(trade.sell.price)}</span>` +
    `<span class="pnl ${trade.net >= 0 ? "pos" : "neg"}">${percent(trade.net)}</span>` +
    `<span class="tag ${reasonClass(trade.reason)}">${escapeText(trade.reason_label)}</span>` +
    (trade.ok ? `<span class="tag take">대조 ${trade.checks}항목 일치</span>` : `<span class="tag bad">대조 ${trade.bad}/${trade.checks}항목 어긋남</span>`);
  const legend = [["accent", "매수가"], ["stop", "손절"], ["take", "익절"]];
  for (const item of trade.series) {
    legend.push([item.kind, item.label]);
  }
  for (const item of trade.segments) {
    legend.push([item.kind, item.label]);
  }
  for (const item of trade.lines) {
    if (!["accent", "stop", "take"].includes(item.kind)) {
      legend.push([item.kind, item.label.split(" ")[0]]);
    }
  }
  const seen = new Set();
  document.getElementById("legend").innerHTML = legend.filter((item) => {
    const key = item.join("|");
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  }).map((item) => `<span><i style="background:var(--${item[0]})"></i>${escapeText(item[1])}</span>`).join("") +
    `<span><i style="background:var(--soft);height:10px"></i>보유 구간</span>` +
    (trade.shades.some((shade) => shade.kind === "watch") ? `<span><i style="background:var(--watch);height:10px"></i>관찰 구간</span>` : "");
  const checkRows = trade.check.map((item) =>
    `<tr class="${item[3] ? "" : "badline"}"><td>${escapeText(item[0])}</td><td class="num">${escapeText(item[1])}</td><td class="num">${escapeText(item[2])}</td><td>${item[3] ? "맞음" : "어긋남"}</td></tr>`).join("");
  document.getElementById("why").innerHTML = trade.why.map((block) =>
    `<h3>${escapeText(block.title)}</h3><ul>${block.rows.map((line) => `<li>${escapeText(line)}</li>`).join("")}</ul>`).join("") +
    `<h3>가격 대조</h3><table class="check"><thead><tr><th style="width:34%">항목</th><th>원장</th><th>차트·다시 계산</th><th style="width:56px">판정</th></tr></thead><tbody>${checkRows}</tbody></table>`;
  drawChart(trade);
}

let current = null;

function drawChart(trade) {
  current = trade;
  const canvas = document.getElementById("chart");
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const candles = trade.candles;
  const count = candles.length;
  const left = 8;
  const right = 96;
  const top = 12;
  const priceBottom = top + (height - top) * 0.72;
  const volumeTop = priceBottom + 18;
  const volumeBottom = height - 18;
  const plotWidth = width - left - right;
  const step = plotWidth / count;
  const indexOf = new Map(candles.map((candle, index) => [candle[0], index]));
  const xAt = (index) => left + step * (index + 0.5);
  const xOfDate = (date) => {
    if (indexOf.has(date)) {
      return xAt(indexOf.get(date));
    }
    let index = candles.findIndex((candle) => candle[0] >= date);
    if (index < 0) {
      index = count - 1;
    }
    return xAt(index);
  };
  let low = Infinity;
  let high = -Infinity;
  for (const candle of candles) {
    low = Math.min(low, candle[3]);
    high = Math.max(high, candle[2]);
  }
  for (const line of trade.lines) {
    if (line.price !== null) {
      low = Math.min(low, line.price);
      high = Math.max(high, line.price);
    }
  }
  const pad = (high - low) * 0.06 || high * 0.05;
  low -= pad;
  high += pad;
  const yAt = (price) => top + (high - price) / (high - low) * (priceBottom - top);
  const maxVolume = Math.max(1, ...candles.map((candle) => candle[5]));
  context.font = "11px 'IBM Plex Mono', monospace";

  for (const shade of trade.shades) {
    const from = xOfDate(shade.from) - step / 2;
    const to = xOfDate(shade.to) + step / 2;
    context.fillStyle = css(shade.kind === "watch" ? "watch" : "soft");
    context.fillRect(from, top, to - from, volumeBottom - top);
  }

  context.strokeStyle = css("line");
  context.lineWidth = 1;
  context.fillStyle = css("muted");
  for (let tick = 0; tick <= 4; tick += 1) {
    const y = yAt(low + (high - low) * tick / 4);
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(width - right, y);
    context.stroke();
  }
  const labelEvery = Math.max(1, Math.ceil(70 / step));
  context.textAlign = "center";
  for (let index = 0; index < count; index += labelEvery) {
    context.fillText(dateText(candles[index][0]).slice(2), xAt(index), height - 4);
  }

  const bodyWidth = Math.max(1, step * 0.65);
  for (let index = 0; index < count; index += 1) {
    const candle = candles[index];
    const color = css(candle[4] >= candle[1] ? "up" : "down");
    const x = xAt(index);
    context.strokeStyle = color;
    context.fillStyle = color;
    context.beginPath();
    context.moveTo(x, yAt(candle[2]));
    context.lineTo(x, yAt(candle[3]));
    context.stroke();
    const yOpen = yAt(candle[1]);
    const yClose = yAt(candle[4]);
    context.fillRect(x - bodyWidth / 2, Math.min(yOpen, yClose), bodyWidth, Math.max(1, Math.abs(yOpen - yClose)));
    const barHeight = candle[5] / maxVolume * (volumeBottom - volumeTop);
    context.globalAlpha = 0.5;
    context.fillRect(x - bodyWidth / 2, volumeBottom - barHeight, bodyWidth, barHeight);
    context.globalAlpha = 1;
  }

  for (const item of trade.series) {
    context.strokeStyle = css(item.kind);
    context.lineWidth = 1.4;
    context.setLineDash(item.kind === "muted" ? [4, 3] : []);
    context.beginPath();
    let started = false;
    item.values.forEach((value, index) => {
      if (value === null || value < low || value > high) {
        started = false;
        return;
      }
      if (started) {
        context.lineTo(xAt(index), yAt(value));
      } else {
        context.moveTo(xAt(index), yAt(value));
        started = true;
      }
    });
    context.stroke();
  }
  context.setLineDash([]);

  for (const segment of trade.segments) {
    context.strokeStyle = css(segment.kind);
    context.lineWidth = 2;
    context.setLineDash(segment.dash ? [6, 4] : []);
    context.beginPath();
    context.moveTo(xOfDate(segment.points[0][0]), yAt(segment.points[0][1]));
    context.lineTo(xOfDate(segment.points[1][0]), yAt(segment.points[1][1]));
    context.stroke();
  }

  const labels = [];
  for (const line of trade.lines) {
    if (line.price === null) {
      continue;
    }
    const y = yAt(line.price);
    const from = line.from ? xOfDate(line.from) - step / 2 : left;
    const to = line.to ? xOfDate(line.to) + step / 2 : width - right;
    context.strokeStyle = css(line.kind);
    context.lineWidth = 1.2;
    context.setLineDash([5, 3]);
    context.beginPath();
    context.moveTo(from, y);
    context.lineTo(to, y);
    context.stroke();
    labels.push({y: y, text: line.label, color: css(line.kind)});
  }
  context.setLineDash([]);

  for (const marker of trade.markers) {
    if (marker.price === null || !indexOf.has(marker.date)) {
      continue;
    }
    const parts = marker.kind.split("_");
    const isLow = parts[0] === "low" || parts[1] === "low";
    const x = xAt(indexOf.get(marker.date));
    const y = yAt(marker.price);
    if (parts[0] === "pivot") {
      context.fillStyle = css("muted");
      context.globalAlpha = 0.7;
      context.beginPath();
      context.arc(x, y + (isLow ? 5 : -5), 2.2, 0, Math.PI * 2);
      context.fill();
      context.globalAlpha = 1;
      continue;
    }
    context.fillStyle = css(parts[1] || "mark");
    context.beginPath();
    context.arc(x, y, 4, 0, Math.PI * 2);
    context.fill();
    if (marker.label) {
      context.textAlign = "center";
      const half = context.measureText(marker.label).width / 2 + 2;
      const extra = parts[1] === "mark" ? 12 : 0;
      context.fillText(marker.label, Math.min(Math.max(x, left + half), width - right - half), isLow ? y + 16 + extra : y - 9 - extra);
    }
  }

  const rowsUsed = [];
  context.textAlign = "left";
  for (const flag of trade.flags) {
    if (!indexOf.has(flag.date)) {
      continue;
    }
    const x = xAt(indexOf.get(flag.date));
    const textWidth = context.measureText(flag.label).width + 6;
    const textX = Math.min(x + 2, width - right - textWidth);
    let row = 0;
    while (rowsUsed.some((used) => used.row === row && textX < used.end && textX + textWidth > used.start)) {
      row += 1;
    }
    rowsUsed.push({row: row, start: textX, end: textX + textWidth});
    context.strokeStyle = css(flag.kind);
    context.setLineDash([2, 2]);
    context.beginPath();
    context.moveTo(x, top);
    context.lineTo(x, volumeTop);
    context.stroke();
    context.setLineDash([]);
    context.fillStyle = css(flag.kind);
    context.fillText(flag.label, textX, volumeTop - 4 - row * 12);
  }

  const arrow = (point, isBuy) => {
    if (!indexOf.has(point.date)) {
      return;
    }
    const x = xAt(indexOf.get(point.date));
    const y = yAt(point.price);
    const color = css(isBuy ? "up" : "down");
    context.fillStyle = color;
    context.strokeStyle = css("panel");
    context.lineWidth = 1;
    context.beginPath();
    const side = isBuy ? -1 : 1;
    context.moveTo(x + side * 5, y);
    context.lineTo(x + side * 15, y - 6);
    context.lineTo(x + side * 15, y + 6);
    context.closePath();
    context.fill();
    context.stroke();
    context.textAlign = isBuy ? "right" : "left";
    context.fillStyle = color;
    context.fillText(isBuy ? "매수" : "매도", x + side * 17, y + 4);
  };
  arrow(trade.buy, true);
  arrow(trade.sell, false);

  context.fillStyle = css("panel");
  context.fillRect(width - right + 1, 0, right, priceBottom + 2);
  context.textAlign = "left";
  context.fillStyle = css("muted");
  for (let tick = 0; tick <= 4; tick += 1) {
    const price = low + (high - low) * tick / 4;
    context.fillText(money(Math.round(price)), width - right + 4, yAt(price) + 4);
  }
  labels.sort((first, second) => first.y - second.y);
  let lastY = -Infinity;
  for (const label of labels) {
    const y = Math.max(label.y, lastY + 12);
    lastY = y;
    context.fillStyle = css("panel");
    context.fillRect(width - right + 1, y - 9, right, 12);
    context.fillStyle = label.color;
    context.fillText(label.text, width - right + 4, y + 1, right - 6);
  }
  canvas.layout = {step: step, left: left, count: count};
}

function attachTip() {
  const canvas = document.getElementById("chart");
  const tip = document.getElementById("tip");
  canvas.addEventListener("mousemove", (event) => {
    if (!current || !canvas.layout) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const index = Math.floor((x - canvas.layout.left) / canvas.layout.step);
    if (index < 0 || index >= canvas.layout.count) {
      tip.style.display = "none";
      return;
    }
    const candle = current.candles[index];
    tip.innerHTML = `${dateText(candle[0])}<br>시 ${money(candle[1])} 고 ${money(candle[2])}<br>저 ${money(candle[3])} 종 ${money(candle[4])}<br>량 ${candle[5].toLocaleString()}`;
    tip.style.display = "block";
    const tipLeft = x + 12 + tip.offsetWidth > rect.width ? x - tip.offsetWidth - 12 : x + 12;
    tip.style.left = tipLeft + "px";
    tip.style.top = (event.clientY - rect.top + 12) + "px";
  });
  canvas.addEventListener("mouseleave", () => {
    tip.style.display = "none";
  });
}

function move(delta) {
  const rows = state.visible.slice(0, MAX_ROWS);
  const index = rows.findIndex((trade) => trade.id === state.selected);
  const next = rows[Math.min(rows.length - 1, Math.max(0, index + delta))];
  if (next && next.id !== state.selected) {
    selectTrade(next.id);
  }
}

document.addEventListener("keydown", (event) => {
  const tag = (event.target.tagName || "").toLowerCase();
  if (tag === "input" || tag === "select" || tag === "textarea") {
    return;
  }
  if (event.key === "ArrowDown") {
    event.preventDefault();
    move(1);
  } else if (event.key === "ArrowUp") {
    event.preventDefault();
    move(-1);
  }
});

document.getElementById("theme").onclick = () => {
  const root = document.documentElement;
  const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
  root.dataset.theme = dark ? "light" : "dark";
};

function redraw() {
  if (current) {
    drawChart(current);
  }
}

window.addEventListener("resize", redraw);
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", redraw);
new MutationObserver(redraw).observe(document.documentElement, {attributes: true, attributeFilter: ["data-theme"]});

async function start() {
  const listings = await Promise.all(TESTS.map((test) => fetch(test.key + ".json").then((response) => response.json())));
  TESTS.forEach((test, index) => {
    data[test.key] = listings[index];
  });
  const wanted = new URLSearchParams(location.search);
  if (wanted.get("theme")) {
    document.documentElement.dataset.theme = wanted.get("theme");
  }
  attachTip();
  renderScore();
  selectTest(TESTS.some((test) => test.key === wanted.get("test")) ? wanted.get("test") : "surge");
}

start().catch((error) => {
  document.getElementById("summary").innerHTML = `<li>자료를 읽지 못했다: ${escapeText(error.message)}</li>`;
});
</script>
"""

if __name__ == "__main__":
    raise SystemExit(main())
