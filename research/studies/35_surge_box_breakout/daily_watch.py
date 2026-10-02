"""스터디 35 — 급등 뒤 되돌림이 작은 강한 종목을 매일 장 마감 뒤 기록한다(주문 없음, 사용자 2026-10-02).

목적  : 과거 자료가 아닌 앞으로의 자료로 판정한다. 규칙은 아직 바뀔 수 있어 한 규칙의 결과만이 아니라 사건과 관찰 자료를
        남긴다 — 나중에 관찰일·거래량 비율·되돌림 기준을 바꾼 변형도 이 기록만으로 다시 셀 수 있게.
자료  : 예약작업 'Quant Daily Bars'가 밤마다 다시 받는 일봉 PYQuant/data/bars_all_pit_v2.parquet(네이버 siseJson, 수정주가).
        읽기·정리·보통주 거르기·수정 전 배율은 strength.py 와 같은 study33.load_inputs 를 쓴다(읽는 시작일만 앞당긴다).
        장중에 받은 파일(수정 시각이 그날 15:40 전)에는 그날 봉이 반쪽이라 그날 행을 버린다.
사건  : strength.py·optimize.py 와 같다 — 종가 +10% 이상, 연속 급등은 묶음 하나, D0 = 묶음 마지막 날,
        상승폭 = D0 종가 − 묶음 전날 종가. 기준 필터 = 묶음 중 가장 큰 거래대금 비율 ≥ 0.2%, 묶음 전날 수정 전 종가 ≥ 2,000원,
        D0 거래량 > 0. 그날 +10% 종가는 다음 날 묶음이 이어질 수 있어 잠정 D0 다. 이어지면 앞날 사건은 extended 로 끝난다.
        관찰은 D1..D7. 관찰 중(D2 이후) +10% 종가가 다시 나오면 그 사건은 끝나고 그날이 새 묶음이 된다.
        관찰 대상은 기준 필터보다 넓게(거래대금 비율 ≥ 0.1%, 전날 수정 전 종가 ≥ 1,000원) 잡아 문턱을 바꾼 변형도 셀 수 있게 한다.
규칙  : RULES 6개(사용자 2026-10-02). 칸 정의·청산 계산은 optimize.py 를 그대로 쓴다 — 사건 × k 표를 optimize.build 와 같은
        열로 만들고 optimize.path_exits 로 청산 칸을, optimize.cell_trades 로 규칙별 매수를 고른다. 국면·종가 조건은 여섯 규칙
        모두 "무관"이라 regime 열은 비워 둔다. 120거래일이 안 찼고 손절·익절에 안 닿은 매수는 "보유"로 둔다.
판정  : 규칙마다 2026-10-02 이후 새 표본(실시간 기록) 청산 50건 이상에서 평균 > 0 이고 t ≥ 2(사용자 2026-10-02).
산출  : live/YYYY-MM-DD.jsonl — 한 줄 한 기록. 첫 줄 kind=day(그날 요약), 이어서
          surge    그날 +10% 종가 중 관찰 대상(필터 값·통과 여부·묶음 일수·상승폭)
          observe  관찰 중 사건의 Dk 상태(시·고·저·종가, 거래량, D0 대비 거래량, 관찰 최저 저가, depth%, 거래량 날마다 감소 여부)
          end      관찰 중 새 +10% 종가로 끝난 사건(extended = 다음 날 묶음이 이어짐)
          signal   규칙별 매수 신호
          position 규칙별 가상 포지션의 그날 상태(hold·stop·take·time)
        live/ledger.tsv — 규칙별 가상 원장. "기록" 칸은 실시간(신호일 밤에 처음 적음) · 따라잡기(나중에 거슬러 적음).
        live/summary.txt — 규칙별 누적 요약, 백테스트 나란히, 판정.
멱등  : 같은 일봉이면 같은 결과. 빠진 날 파일은 WATCH_FROM 부터 채우고, 있는 날 파일은 두되 마지막 날 파일은 다시 쓴다.
        원장은 매번 다시 계산하고 "기록" 칸만 앞 원장에서 이어받는다. 규칙을 바꾸면 이름을 새로 붙여 새로 쌓는다.
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/daily_watch.py [--rewrite] [--wait-minutes 45]
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import importlib.util
import json
import math
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
_specification = importlib.util.spec_from_file_location("study35_optimize", STUDY / "optimize.py")
optimize = importlib.util.module_from_spec(_specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
sys.modules["study35_optimize"] = optimize
_specification.loader.exec_module(optimize)

sweep = optimize.sweep
base = optimize.base
study33 = optimize.study33

LIVE = STUDY / "live"
LEDGER = LIVE / "ledger.tsv"
SUMMARY = LIVE / "summary.txt"
WATCH_FROM = 20260903       # 2026-10-02 기준 20거래일 전 — 따라잡기 시작일
LIVE_FROM = 20261002        # 이날부터 신호일 밤에 적은 신호가 "실시간"
LOAD_MARGIN_DAYS = 120      # WATCH_FROM 앞 읽기 여유(달력일) — 20일 평균 거래량·묶음 시작·앞선 사건 관찰
OBSERVE_DAYS = 7
OBSERVE_SHARE_MIN = 0.1     # 관찰 대상 거래대금 비율 하한(%) — 기준 0.2 보다 넓게
OBSERVE_PREVIOUS_MIN = 1_000.0
PARTIAL_BEFORE = dt.time(15, 40)   # 이 시각 전에 받은 일봉 파일의 그날 행은 장중 반쪽
JUDGE_MIN = 50
JUDGE_T = 2.0


def rule(wait: int, depth: int, move: int, stop: str, take: int) -> dict:
    return {"wait": wait, "depth": depth, "run": 2, "move": move, "close_rule": "any", "regime": "any", "stop": stop, "take": take}


# 이름 → optimize 칸. 백테스트 = optimize.parquet 같은 칸의 전체 기간(2010–2026-10-02) 건수·평균·t(2026-10-02 실행).
RULES = {"base": rule(5, 20, 0, "low1", 20), "c1": rule(4, 15, 30, "low3", 15), "c2": rule(7, 20, 30, "entry10", 30),
         "c3": rule(7, 20, 0, "entry8", 25), "c4": rule(7, 15, 30, "low1", 30), "c5": rule(7, 15, 30, "low3", 30)}
BACKTEST = {"base": (350, 2.33, 3.01), "c1": (264, 3.52, 4.32), "c2": (162, 6.49, 4.12), "c3": (208, 4.50, 3.96),
            "c4": (107, 8.26, 4.11), "c5": (107, 8.58, 4.10)}
STOP_OF = {name: (kind, percent) for name, kind, percent in optimize.STOPS}
LEDGER_COLUMNS = ["규칙", "종목코드", "이름", "D0", "묶음일수", "상승폭%", "depth%", "매수일", "매수가", "손절가", "익절가",
                  "청산일", "청산가", "사유", "순수익%", "보유일", "기록"]
REASONS = {0: "time", 1: "stop", 2: "take"}


def text_date(value: int) -> str:
    text = str(value)
    return f"{text[:4]}-{text[4:6]}-{text[6:]}"


def rounded(value, digits: int = 2):
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return None

    return round(float(value), digits)


def rule_label(name: str) -> str:
    cell = RULES[name]
    kind, percent = STOP_OF[cell["stop"]]
    stop = f"관찰 저가 −{percent}%" if kind == "low" else f"매수가 −{percent}%"
    move = f", 상승 {cell['move']}%+" if cell["move"] else ""
    return f"k{cell['wait']}, d{cell['depth']}, 묶음 {cell['run']}일+{move}, {stop} / +{cell['take']}%"


def bars_last_usable(wait_minutes: int) -> int:
    """오늘 행을 써도 되면 오늘, 아니면 어제까지. 장 마감 뒤 갱신을 wait_minutes 동안 기다린다."""
    now = dt.datetime.now()
    deadline = now + dt.timedelta(minutes=wait_minutes)
    close_cut = dt.datetime.combine(now.date(), PARTIAL_BEFORE)

    while now >= close_cut and now.weekday() < 5 and dt.datetime.now() < deadline:
        if dt.datetime.fromtimestamp(study33.BARS_PATH.stat().st_mtime) >= close_cut:
            break

        time.sleep(60)

    modified = dt.datetime.fromtimestamp(study33.BARS_PATH.stat().st_mtime)

    if modified.date() == now.date() and modified.time() < PARTIAL_BEFORE:
        return int((now - dt.timedelta(days=1)).strftime("%Y%m%d"))

    return int(now.strftime("%Y%m%d"))


def load(last_date: int):
    study33.LOAD_FROM = pd.Timestamp(str(WATCH_FROM)) - pd.Timedelta(days=LOAD_MARGIN_DAYS)
    stocks, _, _ = study33.load_inputs(last_date)
    stocks = [series for series in stocks if len(series.dates) >= 2]
    totals: dict[int, float] = {}

    for series in stocks:
        turnover = series.close * series.factor * series.volume

        for date, value in zip(series.dates.tolist(), turnover.tolist()):
            totals[date] = totals.get(date, 0.0) + value

    return stocks, totals


class Recorder:
    """날짜별 기록과 optimize 형식의 사건 × k 행을 모은다."""

    def __init__(self):
        self.by_date: dict[int, list[dict]] = {}
        self.event_rows: list[dict] = []
        self.series_of: dict[str, object] = {}

    def add(self, date: int, record: dict) -> None:
        self.by_date.setdefault(date, []).append(record)


def scan_stock(series, totals: dict[int, float], recorder: Recorder) -> None:
    rows = len(series.dates)
    dates = series.dates
    close, low, volume = series.close, series.low, series.volume.astype(float)
    raw_close = close * series.factor
    share = raw_close * volume / np.array([max(totals[int(date)], 1.0) for date in dates]) * 100.0
    change = np.r_[np.nan, close[1:] / close[:-1] - 1.0] * 100.0
    surge = change >= base.GAIN - 1e-9
    average5 = base.moving(close, 5)
    first_row = int(np.searchsorted(dates, WATCH_FROM))
    recorder.series_of[series.code] = series

    for surge_row in np.flatnonzero(surge):
        if surge_row < 1 or surge_row + OBSERVE_DAYS < first_row:
            continue

        run_start = surge_row

        while run_start > 1 and surge[run_start - 1]:
            run_start -= 1

        run = int(surge_row - run_start + 1)
        share_max = float(share[run_start:surge_row + 1].max())
        previous_raw = float(raw_close[run_start - 1])
        move = float(close[surge_row] - close[run_start - 1])
        move_percent = move / close[run_start - 1] * 100
        average20 = float(volume[max(surge_row - 20, 0):surge_row].mean())
        base_filter = share_max >= base.SHARE_MIN and previous_raw >= study33.MIN_PREVIOUS_CLOSE_KRW and volume[surge_row] > 0
        observed = share_max >= OBSERVE_SHARE_MIN and previous_raw >= OBSERVE_PREVIOUS_MIN and volume[surge_row] > 0 and move > 0
        surge_date = int(dates[surge_row])
        event = {"code": series.code, "name": series.name, "d0": surge_date, "run": run}

        if surge_date >= WATCH_FROM and (observed or base_filter):
            recorder.add(surge_date, {"kind": "surge", **event, "run_start": int(dates[run_start]),
                                      "change_pct": rounded(change[surge_row]), "close": rounded(close[surge_row]),
                                      "previous_close": rounded(close[run_start - 1]), "previous_close_raw": rounded(previous_raw, 0),
                                      "move": rounded(move), "move_pct": rounded(move_percent),
                                      "share": rounded(share[surge_row], 3), "share_max": rounded(share_max, 3),
                                      "turnover_eok": rounded(raw_close[surge_row] * volume[surge_row] / 1e8, 1),
                                      "volume": int(volume[surge_row]), "spike20": rounded(volume[surge_row] / max(average20, 1.0)),
                                      "base_filter": bool(base_filter), "observed": bool(observed),
                                      "extends_d0": int(dates[surge_row - 1]) if run >= 2 else None})

        if not observed:
            continue

        surge_volume = max(volume[surge_row], 1.0)

        for wait in range(1, OBSERVE_DAYS + 1):
            row = surge_row + wait

            if row > rows - 1:
                break

            row_date = int(dates[row])

            if surge[row]:
                if row_date >= WATCH_FROM:
                    recorder.add(row_date, {"kind": "end", **event, "k": wait, "reason": "extended" if wait == 1 else "new_surge"})

                break

            lowest = float(low[surge_row + 1:row + 1].min())
            depth = (close[surge_row] - lowest) / move * 100
            path = volume[surge_row:row + 1]

            if row_date >= WATCH_FROM:
                recorder.add(row_date, {"kind": "observe", **event, "k": wait, "base_filter": bool(base_filter),
                                        "move_pct": rounded(move_percent),
                                        "open": rounded(series.open_price[row]), "high": rounded(series.high[row]),
                                        "low": rounded(low[row]), "close": rounded(close[row]), "volume": int(volume[row]),
                                        "volume_vs_d0": rounded(volume[row] / surge_volume, 3),
                                        "mean_volume_vs_d0": rounded(volume[surge_row + 1:row + 1].mean() / surge_volume, 3),
                                        "volume_falling_daily": bool(np.all(np.diff(path) < 0)),
                                        "lowest_low": rounded(lowest), "depth_pct": rounded(depth),
                                        "hold_pct": rounded((close[row] / close[surge_row] - 1) * 100),
                                        "above_ma5": bool(not math.isnan(average5[row]) and close[row] >= average5[row]),
                                        "share": rounded(share[row], 3)})

            # optimize.build 와 같은 열: 묶음 2일+, 기준 필터, 되돌림 < 30%, k 2–7
            if (wait not in optimize.WAITS or not base_filter or run < min(optimize.RUNS) or depth >= max(optimize.DEPTHS)
                    or row_date < WATCH_FROM):
                continue

            entry = float(close[row])
            last = min(row + base.HORIZON, rows - 1)
            event_row = {"code": series.code, "surge": surge_date, "entry_date": row_date, "year": surge_date // 10000,
                         "run": run, "wait": wait, "share": share_max, "depth": depth, "move_pct": move_percent,
                         "above_d0": bool(close[row] >= close[surge_row]),
                         "above_ma5": bool(not math.isnan(average5[row]) and close[row] >= average5[row]),
                         "regime": np.nan, "entry_row": row, "lowest": lowest}
            event_row.update(optimize.path_exits(series, row, entry, lowest, last))
            recorder.event_rows.append(event_row)


def stop_take_prices(cell: dict, entry: float, lowest: float) -> tuple[float, float]:
    """optimize.path_exits 의 손절·익절 값(기록용)."""
    kind, percent = STOP_OF[cell["stop"]]
    stop_price = min(lowest * (1 - percent / 100), entry * 0.999) if kind == "low" else entry * (1 - percent / 100)
    return stop_price, entry * (1 + cell["take"] / 100)


def build_ledger(recorder: Recorder) -> list[dict]:
    """규칙마다 optimize.cell_trades 로 매수를 고르고 원장 행·signal·position 기록을 만든다."""
    if not recorder.event_rows:
        return []

    events = pd.DataFrame(recorder.event_rows)
    ledger = []

    for name, cell in RULES.items():
        trades = optimize.cell_trades(events, cell)

        for index in trades.index:
            row = events.loc[index]
            series = recorder.series_of[row["code"]]
            entry_row = int(row["entry_row"])
            entry = float(series.close[entry_row])
            stop_price, take_price = stop_take_prices(cell, entry, float(row["lowest"]))
            kind = int(trades.at[index, "kind"])
            held = int(trades.at[index, "hold"])
            closed = kind != 0 or held >= base.HORIZON
            exit_row = entry_row + held
            net = float(trades.at[index, "ret"])
            record = {"규칙": name, "종목코드": row["code"], "이름": series.name, "D0": text_date(int(row["surge"])),
                      "묶음일수": int(row["run"]), "상승폭%": rounded(row["move_pct"]), "depth%": rounded(row["depth"]),
                      "매수일": text_date(int(row["entry_date"])), "매수가": rounded(entry), "손절가": rounded(stop_price),
                      "익절가": rounded(take_price), "청산일": text_date(int(series.dates[exit_row])) if closed else "",
                      "청산가": rounded((net / 100 + 1) * entry / sweep.COST) if closed else "",
                      "사유": REASONS[kind] if closed else "보유", "순수익%": rounded(net) if closed else "",
                      "보유일": held}
            ledger.append(record)
            common = {"rule": name, "code": row["code"], "name": series.name, "d0": int(row["surge"]),
                      "entry_date": int(row["entry_date"]), "entry": rounded(entry), "stop": rounded(stop_price),
                      "take": rounded(take_price)}
            recorder.add(int(row["entry_date"]), {"kind": "signal", **common, "k": int(row["wait"]), "run": int(row["run"]),
                                                  "depth_pct": rounded(row["depth"]), "move_pct": rounded(row["move_pct"])})

            for day in range(1, held + 1):
                position_row = entry_row + day
                close = series.close[position_row]
                status = REASONS[kind] if closed and day == held else "hold"
                position = {"kind": "position", **common, "day": day, "high": rounded(series.high[position_row]),
                            "low": rounded(series.low[position_row]), "close": rounded(close), "status": status}

                if status == "hold":
                    position["unrealized_pct"] = rounded((close / entry * sweep.COST - 1) * 100)
                else:
                    position["net_pct"] = rounded(net)

                recorder.add(int(series.dates[position_row]), position)

    return ledger


def statistics(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "mean": None, "t": None, "win": None}

    array = np.asarray(values, dtype=float)
    t_value = study33.t_value(array)
    return {"n": len(array), "mean": round(float(array.mean()), 2), "t": None if math.isnan(t_value) else round(t_value, 2),
            "win": round(float((array > 0).mean() * 100), 1)}


def previous_labels() -> dict:
    if not LEDGER.exists():
        return {}

    with LEDGER.open(encoding="utf-8", newline="") as file:
        return {(row["규칙"], row["종목코드"], row["D0"], row["매수일"]): row["기록"] for row in csv.DictReader(file, delimiter="\t")}


def write_ledger(ledger: list[dict], last_bar: int) -> None:
    labels = previous_labels()

    for record in ledger:
        key = (record["규칙"], record["종목코드"], record["D0"], record["매수일"])
        live = record["매수일"] == text_date(last_bar) and last_bar >= LIVE_FROM
        record["기록"] = labels.get(key) or ("실시간" if live else "따라잡기")

    ledger.sort(key=lambda record: (list(RULES).index(record["규칙"]), record["매수일"], record["종목코드"]))

    with LEDGER.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=LEDGER_COLUMNS, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(ledger)


def closed_returns(ledger: list[dict], name: str, until: str, only_live: bool = False) -> list[float]:
    return [float(record["순수익%"]) for record in ledger
            if record["규칙"] == name and record["사유"] != "보유" and record["청산일"] <= until
            and (not only_live or record["기록"] == "실시간")]


def format_statistics(values: dict) -> str:
    if not values["n"]:
        return "0건"

    t_text = f"t{values['t']:+.1f}" if values["t"] is not None else "t –"
    return f"{values['n']}건 {values['mean']:+.2f}% {t_text} 승률 {values['win']:.0f}%"


def judgement(values: dict) -> str:
    if values["n"] < JUDGE_MIN:
        return f"진행 중({values['n']}/{JUDGE_MIN})"

    if values["mean"] > 0 and values["t"] is not None and values["t"] >= JUDGE_T:
        return "통과"

    return "불통과"


def write_summary(ledger: list[dict], last_bar: int, day_counts: dict) -> None:
    until = text_date(last_bar)
    lines = [f"스터디 35 앞으로의 기록 — 자료 끝 {until}, 기록 시작 {text_date(WATCH_FROM)}(따라잡기), 실시간 {text_date(LIVE_FROM)}부터",
             f"판정: 규칙마다 {text_date(LIVE_FROM)} 이후 새 표본(실시간) 청산 {JUDGE_MIN}건 이상에서 평균 > 0, t ≥ {JUDGE_T:.0f} 이면 통과",
             "매수는 Dk 종가, 청산은 다음 날부터 손절·익절·120거래일 종가. 같은 날 둘 다 닿으면 손절, 갭은 시가. 비용 포함 순수익.",
             ""]

    for name in RULES:
        rows = [record for record in ledger if record["규칙"] == name]
        everything = statistics(closed_returns(ledger, name, until))
        live_only = statistics(closed_returns(ledger, name, until, only_live=True))
        count, mean, t_value = BACKTEST[name]
        open_count = sum(1 for record in rows if record["사유"] == "보유")
        lines += [f"[{name}] {rule_label(name)}",
                  f"  실시간 청산   {format_statistics(live_only):34s} 판정 {judgement(live_only)}",
                  f"  청산 전체     {format_statistics(everything):34s} (따라잡기 포함)",
                  f"  백테스트      {count}건 {mean:+.2f}% t{t_value:+.1f} (2010–2026)",
                  f"  신호 {len(rows)}건, 보유 중 {open_count}"]

    lines += ["", "날짜별: 새 사건(기준 필터 통과 +10% 종가) · 관찰 중 사건 · 규칙별 신호 · 청산"]

    for date in sorted(day_counts):
        counts = day_counts[date]
        signals = " ".join(f"{name}{value}" for name, value in counts["signals"].items() if value) or "-"
        lines.append(f"  {text_date(date)}  새 사건 {counts['surges']:3d}  관찰 {counts['observing']:3d}  신호 {signals:24s}  청산 {counts['closed']}")

    SUMMARY.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="스터디 35 앞으로의 기록(장 마감 뒤)")
    parser.add_argument("--rewrite", action="store_true", help="있는 날짜 파일도 다시 쓴다")
    parser.add_argument("--wait-minutes", type=int, default=0, help="일봉 파일이 오늘 마감 뒤 갱신될 때까지 기다리는 시간")
    arguments = parser.parse_args()

    last_usable = bars_last_usable(arguments.wait_minutes)
    stocks, totals = load(last_usable)
    trading_dates = sorted(date for date in totals if date >= WATCH_FROM)

    if not trading_dates:
        print(f"[study35] {text_date(WATCH_FROM)} 뒤 일봉이 없다")
        return 1

    last_bar = trading_dates[-1]
    recorder = Recorder()

    for series in stocks:
        scan_stock(series, totals, recorder)

    ledger = build_ledger(recorder)
    LIVE.mkdir(exist_ok=True)
    write_ledger(ledger, last_bar)
    day_counts = {}
    written = 0
    order = {"surge": 0, "end": 1, "observe": 2, "signal": 3, "position": 4}

    for date in trading_dates:
        records = recorder.by_date.get(date, [])
        until = text_date(date)
        counts = {"surges": sum(1 for record in records if record["kind"] == "surge" and record["base_filter"]),
                  "observing": len({(record["code"], record["d0"]) for record in records if record["kind"] == "observe"}),
                  "signals": {name: sum(1 for record in records if record["kind"] == "signal" and record["rule"] == name) for name in RULES},
                  "closed": sum(1 for record in records if record["kind"] == "position" and record["status"] != "hold")}
        day_counts[date] = counts
        path = LIVE / f"{until}.jsonl"

        if path.exists() and not arguments.rewrite and date != last_bar:
            continue

        head = {"kind": "day", "date": date, **counts,
                "open_positions": sum(1 for record in records if record["kind"] == "position" and record["status"] == "hold"),
                "cumulative_closed": {name: statistics(closed_returns(ledger, name, until)) for name in RULES},
                "cumulative_closed_live": {name: statistics(closed_returns(ledger, name, until, only_live=True)) for name in RULES},
                "bars_last_usable": last_usable}
        records = sorted(records, key=lambda record: (order[record["kind"]], record.get("rule", ""), record["code"], record.get("d0", 0)))

        with path.open("w", encoding="utf-8", newline="\n") as file:
            for record in [head, *records]:
                file.write(json.dumps(record, ensure_ascii=False, separators=(",", ":"), default=lambda value: value.item()) + "\n")

        written += 1

    write_summary(ledger, last_bar, day_counts)
    today = day_counts[last_bar]
    base_closed = statistics(closed_returns(ledger, "base", text_date(last_bar)))
    mean_text = f"{base_closed['mean']:+.2f}%" if base_closed["n"] else "–"
    print(f"[study35] 자료 끝 {text_date(last_bar)}: 새 사건 {today['surges']}, 신호 {sum(today['signals'].values())}, "
          f"base 누적 청산 {base_closed['n']}건 평균 {mean_text}, 원장 {len(ledger)}건, 파일 {written}개 씀")
    return 0


if __name__ == "__main__":
    sys.exit(main())
