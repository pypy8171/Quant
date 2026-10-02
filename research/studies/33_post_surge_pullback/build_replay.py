#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""스터디 33 매매 리플레이 자료 — 기본 칸(+10%·지켜보기 5일·보유 10일) 매매마다 캔들·근거·급등 위치를 만든다.

입력: 같은 폴더 trades.tsv(backtest.py 산출), PYQuant/data/bars_all_pit_v2.parquet(수정주가 일봉).
산출: replay/replay_<구간>.json(차트·근거), location.tsv(급등 위치·거래량 배수별 묶음 — 사후 탐색).

급등 위치 값은 급등일 전날 종가까지만 쓴다(급등일 뒤 값은 안 씀):
  고점 대비 = 전일 종가 / 직전 250거래일 종가 최고 − 1
  저점 대비 = 전일 종가 / 직전 250거래일 종가 최저 − 1
  거래량 배수 = 급등일 거래량 / 직전 20거래일 거래량 평균

재실행(저장소 루트): py -X utf8 research/studies/33_post_surge_pullback/build_replay.py [--rule zone]
  --rule zone: backtest_zone.py 산출 trades_zone.tsv(40–60% 되돌림 구간에서 산 칸, 거래량 조건 없음)로
  replay/zone_<구간>.json·location_zone.tsv 를 만든다.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
BARS_PATH = REPO / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
OUT_DIR = STUDY / "replay"

sys.stdout.reconfigure(encoding="utf-8")

BASE_GAIN = 10.0
BASE_WINDOW = 5
BASE_HOLD = 10
STOP_BELOW_LOW = 0.99
BARS_BEFORE = 60          # 차트에 급등일 앞으로 보여 줄 거래일
BARS_AFTER_EXIT = 10      # 매도 뒤로 보여 줄 거래일
ZONE_BARS_BEFORE = 30     # --rule zone 은 매매가 2만 건이 넘어 차트 앞쪽을 줄인다
CHUNK_BYTES = 7_000_000   # --rule zone 차트 묶음 파일 하나의 목표 크기(아티팩트 파일당 16MB 한도 아래)
LOOKBACK_YEAR = 250
LOOKBACK_VOLUME = 20
MIN_HISTORY = 120         # 고점·저점 비교에 필요한 최소 이력

DRAWDOWN_BANDS = [(-math.inf, -50.0, "1년 고점 대비 −50% 이하"), (-50.0, -30.0, "−50~−30%"),
                  (-30.0, -15.0, "−30~−15%"), (-15.0, math.inf, "−15% 위(고점 근처)")]
VOLUME_BANDS = [(0.0, 3.0, "평소의 3배 미만"), (3.0, 10.0, "3~10배"), (10.0, math.inf, "10배 이상")]
PERIOD_NAMES = {"development": "개발 2010~2022", "validation": "검증 2023~2025-09", "recent_1y": "최근 1년"}
REASON_NAMES = {"stop": "손절", "stop_same_day": "매수 당일 손절", "take": "익절", "time": "시간 청산", "delisted": "상장폐지",
                "series_end": "시세 끝", "open_at_period_end": "구간 끝 미청산"}


def band_label(value: float, bands: list) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "이력 부족"

    for lower, upper, label in bands:
        if lower <= value < upper:
            return label

    return "이력 부족"


def date_text(date_int: int) -> str:
    text = str(date_int)
    return f"{text[:4]}-{text[4:6]}-{text[6:]}"


def load_bars(codes: list[str]) -> dict[str, pd.DataFrame]:
    columns = ["Date", "Open", "High", "Low", "Close", "Volume", "code"]
    bars = pd.read_parquet(BARS_PATH, columns=columns, filters=[("code", "in", codes)])
    bars = bars[bars["Volume"] > 0].sort_values(["code", "Date"], kind="mergesort").reset_index(drop=True)
    previous_close = bars.groupby("code")["Close"].shift(1)
    open_zero = bars["Open"] <= 0
    bars.loc[open_zero, "Open"] = previous_close[open_zero].fillna(bars.loc[open_zero, "Close"])
    bars["Low"] = np.where(bars["Low"] <= 0, np.minimum(bars["Open"], bars["Close"]), bars["Low"])
    bars["High"] = np.where(bars["High"] <= 0, np.maximum(bars["Open"], bars["Close"]), bars["High"])
    bars["date_int"] = bars["Date"].dt.strftime("%Y%m%d").astype(np.int64)
    return {code: group.reset_index(drop=True) for code, group in bars.groupby("code", sort=True)}


def row_of(dates: np.ndarray, date_int: int) -> int:
    position = int(np.searchsorted(dates, date_int, side="left"))

    if position >= len(dates) or dates[position] != date_int:
        raise KeyError(date_int)

    return position


def round_price(value: float) -> float:
    return round(float(value), 2)


def candle_price(value: float) -> float:
    value = float(value)
    return round(value) if value >= 100 else round(value, 2)


def build_trade(trade: pd.Series, group: pd.DataFrame) -> dict:
    dates = group["date_int"].to_numpy(np.int64)
    open_price = group["Open"].to_numpy(float)
    high = group["High"].to_numpy(float)
    low = group["Low"].to_numpy(float)
    close = group["Close"].to_numpy(float)
    volume = group["Volume"].to_numpy(float)

    surge_row = row_of(dates, int(trade["surge_date"]))
    pullback_row = row_of(dates, int(trade["pullback_date"]))
    signal_row = row_of(dates, int(trade["signal_date"]))
    buy_row = row_of(dates, int(trade["buy_date"]))
    exit_row = row_of(dates, int(trade["exit_date"]))

    # 급등 위치(전날까지)
    previous = surge_row - 1
    history_start = max(0, surge_row - LOOKBACK_YEAR)
    history = close[history_start:surge_row]
    drawdown = from_low = float("nan")

    if surge_row - history_start >= MIN_HISTORY and previous >= 0:
        drawdown = (close[previous] / history.max() - 1.0) * 100.0
        from_low = (close[previous] / history.min() - 1.0) * 100.0

    volume_start = max(0, surge_row - LOOKBACK_VOLUME)
    average_volume = volume[volume_start:surge_row].mean() if surge_row > volume_start else float("nan")
    volume_multiple = volume[surge_row] / average_volume if average_volume and average_volume > 0 else float("nan")

    middle = (open_price[surge_row] + close[surge_row]) / 2.0
    zone_rule = "zone_top" in trade.index
    lowest_low = low[surge_row + 1:signal_row + 1].min()
    entry = float(trade["entry"])
    stop = float(trade["stop"])
    target = float(trade["target"])
    target_on = str(trade["target_on"]) == "True"
    net = float(trade["net_pct"])

    start = max(0, surge_row - (ZONE_BARS_BEFORE if zone_rule else BARS_BEFORE))
    end = min(len(dates), exit_row + BARS_AFTER_EXIT + 1)
    candles = [[int(dates[row]), candle_price(open_price[row]), candle_price(high[row]), candle_price(low[row]),
                candle_price(close[row]), int(volume[row])] for row in range(start, end)]

    reasons_buy = [
        f"급등일 {date_text(dates[surge_row])}: 전일 대비 {trade['surge_change_pct']:+.1f}%, 거래대금 {trade['turnover_eok']:,.0f}억, "
        f"거래량은 직전 20일 평균의 {volume_multiple:.1f}배",
        f"급등 전 위치: 1년 고점 대비 {drawdown:+.1f}%, 1년 저점 대비 {from_low:+.1f}%" if not math.isnan(drawdown)
        else "급등 전 위치: 상장 1년 미만이라 계산 안 함",
    ] + ([
        f"되돌림 구간: 급등일 몸통({open_price[surge_row]:,.0f}→{close[surge_row]:,.0f})의 40% 자리 {float(trade['zone_top']):,.0f}, "
        f"60% 자리 {float(trade['zone_bottom']):,.0f}",
        f"매수 {date_text(dates[buy_row])}: 급등 뒤 {buy_row - surge_row}거래일째 저가 {low[buy_row]:,.0f}가 40% 자리에 처음 닿음 → "
        + (f"시가 {open_price[buy_row]:,.0f}가 이미 구간 안이라 시가 + 1호가 = {entry:,.0f}" if str(trade["entry_at_open"]) == "True"
           else f"지정가 {entry:,.0f} 체결"),
    ] if zone_rule else [
        f"눌림 표시 {date_text(dates[pullback_row])}: 종가 {close[pullback_row]:,.0f}이 급등일 몸통 중간값 {middle:,.0f}의 "
        f"{(close[pullback_row] / middle - 1) * 100:+.1f}% (±2% 안), 거래량은 급등일의 {volume[pullback_row] / volume[surge_row] * 100:.0f}% (50% 이하)",
        f"재상승 신호 {date_text(dates[signal_row])}: 종가 {close[signal_row]:,.0f} > 전일 고가 {high[signal_row - 1]:,.0f}",
        f"매수 {date_text(dates[buy_row])}: 다음 날 시가 {open_price[buy_row]:,.0f} + 1호가 = {entry:,.0f}",
    ])
    reasons_sell = [
        (f"손절선 {stop:,.0f}: 급등일 시가 {open_price[surge_row]:,.0f} × 0.99 (매수가 대비 {(stop / entry - 1) * 100:.1f}%)" if zone_rule
         else f"손절선 {stop:,.0f}: 지켜본 기간 최저가 {lowest_low:,.0f} × 0.99 (매수가 대비 {(stop / entry - 1) * 100:.1f}%)"),
        f"익절선 {target:,.0f}: 급등일 고가 (매수가 대비 {(target / entry - 1) * 100:+.1f}%)" if target_on
        else f"익절선 없음: 매수가 {entry:,.0f}가 이미 급등일 고가 {target:,.0f} 이상",
        f"시간 청산: 매수 뒤 {BASE_HOLD}거래일 종가",
        f"매도 {date_text(dates[exit_row])}: {REASON_NAMES.get(trade['exit_reason'], trade['exit_reason'])}, "
        f"{float(trade['exit_price']):,.0f}원, {int(trade['hold_days'])}거래일 보유, 비용 뺀 손익 {net:+.2f}%",
    ]

    return {
        "period": trade["period"], "code": trade["code"], "name": trade["name"],
        "surge": int(dates[surge_row]), "pullback": int(dates[pullback_row]), "signal": int(dates[signal_row]),
        "buy": int(dates[buy_row]), "exit": int(dates[exit_row]),
        "entry": round_price(entry), "stop": round_price(stop), "target": round_price(target), "target_on": target_on,
        "middle": round_price(middle), "exit_price": round_price(float(trade["exit_price"])),
        "zone_top": round_price(float(trade["zone_top"])) if zone_rule else None,
        "zone_bottom": round_price(float(trade["zone_bottom"])) if zone_rule else None,
        "reason": trade["exit_reason"], "net": round(net, 2), "hold_days": int(trade["hold_days"]),
        "change": round(float(trade["surge_change_pct"]), 1), "turnover": round(float(trade["turnover_eok"]), 0),
        "cap": None if pd.isna(trade["market_cap_eok"]) else round(float(trade["market_cap_eok"]), 0),
        "drawdown": None if math.isnan(drawdown) else round(drawdown, 1),
        "from_low": None if math.isnan(from_low) else round(from_low, 1),
        "volume_multiple": None if math.isnan(volume_multiple) else round(float(volume_multiple), 1),
        "location": band_label(drawdown, DRAWDOWN_BANDS),
        "volume_band": band_label(volume_multiple, VOLUME_BANDS),
        "why_buy": reasons_buy, "why_sell": reasons_sell, "candles": candles,
    }


def summarize(frame: pd.DataFrame) -> dict:
    values = frame["net"].to_numpy(float)
    count = len(values)

    if count == 0:
        return {"count": 0}

    deviation = values.std(ddof=1) if count > 1 else float("nan")
    t_value = values.mean() / (deviation / math.sqrt(count)) if count > 1 and deviation > 0 else float("nan")
    return {"count": count, "average": round(float(values.mean()), 2), "win_rate": round(float((values > 0).mean() * 100), 1),
            "t": None if math.isnan(t_value) else round(float(t_value), 2)}


def split_chunks(items: list[dict], period: str, folder: str = "zone") -> list[dict]:
    """차트·근거 문장은 묶음 파일 replay/<folder>/<구간>_<번호>.json 으로 떼고, 목록에는 번호만 남긴다(화면이 고를 때 읽는다)."""
    chunk_directory = OUT_DIR / folder
    chunk_directory.mkdir(exist_ok=True)

    for old in chunk_directory.glob(f"{period}_*.json"):
        old.unlink()

    light = []
    chunk, chunk_size, chunk_number = {}, 0, 0

    def flush():
        nonlocal chunk, chunk_size, chunk_number
        if chunk:
            (chunk_directory / f"{period}_{chunk_number}.json").write_text(
                json.dumps(chunk, ensure_ascii=False, separators=(",", ":"), allow_nan=False), encoding="utf-8")
            chunk_number += 1
            chunk, chunk_size = {}, 0

    for index, item in enumerate(sorted(items, key=lambda value: (value["code"], value["surge"]))):
        heavy = {key: item.pop(key) for key in ("candles", "why_buy", "why_sell")}
        text = json.dumps(heavy, ensure_ascii=False, separators=(",", ":"), allow_nan=False)

        if chunk_size + len(text.encode("utf-8")) > CHUNK_BYTES:
            flush()

        chunk[str(index)] = heavy
        chunk_size += len(text.encode("utf-8"))
        light.append({**item, "id": str(index), "chunk": f"{folder}/{period}_{chunk_number}.json"})

    flush()
    print(f"{period}: 차트 묶음 {chunk_number}개")
    return light


def main() -> int:
    zone = "--rule" in sys.argv and sys.argv[sys.argv.index("--rule") + 1] == "zone"
    prefix = "zone_" if zone else "replay_"

    if zone:
        trades = pd.read_csv(STUDY / "trades_zone.tsv", sep="\t", dtype={"code": str})
        trades = trades[trades["rule"] == "none"].reset_index(drop=True)
    else:
        trades = pd.read_csv(STUDY / "trades.tsv", sep="\t", dtype={"code": str})
        trades = trades[(trades["gain"] == BASE_GAIN) & (trades["window"] == BASE_WINDOW) & (trades["hold"] == BASE_HOLD)
                        & (trades["closed"] == True)].reset_index(drop=True)   # noqa: E712
    bars = load_bars(sorted(trades["code"].unique()))
    built = []
    skipped = 0

    for _, trade in trades.iterrows():
        group = bars.get(trade["code"])

        if group is None:
            skipped += 1
            continue

        try:
            built.append(build_trade(trade, group))
        except KeyError:
            skipped += 1

    OUT_DIR.mkdir(exist_ok=True)
    table = pd.DataFrame([{key: item[key] for key in ("period", "net", "location", "volume_band")} for item in built])
    location_rows = []

    for period in PERIOD_NAMES:
        subset = table[table["period"] == period]

        for axis, column in (("급등 전 위치", "location"), ("거래량 배수", "volume_band")):
            for label, group in subset.groupby(column, sort=False):
                location_rows.append({"period": period, "axis": axis, "band": label, **summarize(group)})

        for (location, volume_band), group in subset.groupby(["location", "volume_band"], sort=False):
            location_rows.append({"period": period, "axis": "위치×거래량", "band": f"{location} / {volume_band}", **summarize(group)})

    location = pd.DataFrame(location_rows)
    location.to_csv(STUDY / ("location_zone.tsv" if zone else "location.tsv"), sep="\t", index=False)

    # 브라우저 JSON은 NaN을 못 읽는다 — 표의 빈 값(건수 1건 묶음의 t 등)은 null로 바꾼다
    location = location.astype(object).where(location.notna(), None)

    for period in PERIOD_NAMES:
        items = [item for item in built if item["period"] == period]

        if zone:
            items = split_chunks(items, period)
        payload = {"period": period, "period_name": PERIOD_NAMES[period], "trades": items,
                   "summary": summarize(table[table["period"] == period]),
                   "location": location[location["period"] == period].to_dict("records")}
        (OUT_DIR / f"{prefix}{period}.json").write_text(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False), encoding="utf-8")
        print(f"{period}: {len(items)}건 → replay/{prefix}{period}.json")

    print(f"건너뜀 {skipped}건")
    print(location[location["axis"] != "위치×거래량"].to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
