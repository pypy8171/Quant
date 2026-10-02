"""스터디 35 — 첫 매수가 끝난 뒤 추세가 살아 있으면 이동평균에서 다시 산다(사용자 2026-10-02 "추세가 살아 있으면 다시 산다",
보성파워텍(006910) 사례).

첫 매수 : optimize.py 칸 둘. 사건·청산은 optimize_events.parquet 와 optimize.cell_trades 를 그대로 쓴다.
          1번 칸(주) = k4 d15 묶음2+ 상승30+ 손절 관찰 저가 −3% / 익절 +15%.  B5_20(비교) = k5 d20 묶음2+ 손절 −1% / +20%.
다시 사기: 첫 매수가 청산된 다음 날부터 본다. 그날 저가 ≤ N일선 이고 종가 ≥ N일선(ma_sweep close 방식과 같은 부등호),
          그리고 추세가 살아 있음 = 그날 5일선 ≥ 20일선 이고 종가 ≥ 20일선이면 그날 종가에 산다. 이평선은 그날 종가를 넣은 값
          (그날 종가까지 아는 값만). 첫 매수일부터 W 거래일 안에서만 산다.
          after = take(직전 매매가 익절로 끝났을 때만 다음을 찾는다, 손절·기한 청산이면 사건 끝) / all(어떤 청산 뒤에도).
          한 사건에서 최대 3번, 보유 중에는 겹쳐 사지 않는다(다음 찾기는 그 매매 청산 다음 날부터).
다시 산 매매의 청산: 다음 날부터. 손절 = 매수일 저가 −3%(매수가 × 0.999 를 넘지 않게), 익절 +T%, 120거래일 안에 안 닿으면 120일째 종가.
          같은 날 둘 다 닿으면 손절, 갭은 시가, 비용 ma_sweep.COST — 판정은 sweep_view.exit_path 를 그대로 쓴다.
          120거래일이 자료 끝을 넘고 상폐 종목이 아니면 그 매매(와 그 사건의 뒤 다시 사기)는 통계에서 빼고 건수만 센다.
칸     : N 5·10·20 × W 20·40·60 × after take·all × T 15·20 = 36칸(첫 매수 규칙마다). 값은 사용자가 정함.
판정(사용자 2026-10-02): 다시 산 매매만으로 전체 t ≥ 3, 2010–17·2018–26 평균 ≥ 0, 이웃 칸(축 하나씩 한 칸, after 는 다른 값) 같은 부호.
          무작위 기대 최대 t: 사건마다 다시 산 매매 수익 부호를 같이 뒤집어(모든 칸에 같은 부호) 36칸 최대 t 를 200번(seed 20261002).
          대상 칸은 다시 산 매매 ≥ 20건(내가 정함). 구간은 D0 연도.
사건 단위: 첫 매수 + 다시 산 매매 순수익 합(매매마다 같은 금액)을 사건 하나로 보고 평균·t. 첫 매수만일 때와 나란히.
산출  : rebuy.txt(요약), rebuy_trades.parquet(다시 산 매매 원장, .gitignore).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/rebuy.py   (optimize_events.parquet 가 있어야 한다)
"""
from __future__ import annotations

import importlib.util
import itertools
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent


def load_module(name: str, file_name: str):
    specification = importlib.util.spec_from_file_location(name, HERE / file_name)
    module = importlib.util.module_from_spec(specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


optimize = load_module("study35_optimize", "optimize.py")
sys.path.insert(0, str(HERE))
view = load_module("study35_sweep_view", "sweep_view.py")
base = optimize.base
sweep = optimize.sweep
study33 = optimize.study33
SEED = optimize.SEED
FLIPS = optimize.FLIPS
SPLIT_YEAR = optimize.SPLIT_YEAR
NULL_MIN = 20
MAX_REBUYS = 3
STOP_BELOW = 3.0
WINDOWS = [5, 10, 20]
LIMITS = [20, 40, 60]
AFTERS = ["take", "all"]
TAKES = [15, 20]
AXES = ["window", "limit", "after", "take"]
AXIS_VALUES = {"window": WINDOWS, "limit": LIMITS, "after": AFTERS, "take": TAKES}
FIRST_RULES = {
    "1번 칸": {"wait": 4, "depth": 15, "run": 2, "move": 30, "close_rule": "any", "regime": "any", "stop": "low3", "take": 15},
    "B5_20": dict(optimize.BASELINE),
}
CASE_CODE = "006910"
CASE_FROM, CASE_TO = 20250101, 20261002
TEXT_PATH = HERE / "rebuy.txt"
LEDGER_PATH = HERE / "rebuy_trades.parquet"


def averages_of(series, cache: dict) -> dict:
    if series.code not in cache:
        cache[series.code] = {window: base.moving(series.close, window) for window in (5, 10, 20)}

    return cache[series.code]


def first_trades(events: pd.DataFrame, rule: dict, by_code: dict) -> pd.DataFrame:
    """첫 매수 원장. cell_trades 의 색인이 events 색인이라 code·surge 를 다시 붙인다. exit_row = 매수 행 + 보유일."""
    trades = optimize.cell_trades(events, rule)
    trades = trades.join(events.loc[trades.index, ["code", "surge"]])
    entry_rows, exit_rows = [], []

    for record in trades.itertuples():
        series = by_code[record.code]
        row = int(np.searchsorted(series.dates, record.entry_date))
        entry_rows.append(row)
        exit_rows.append(row + int(record.hold))

    trades["entry_row"] = entry_rows
    trades["exit_row"] = exit_rows
    return trades.reset_index(drop=True)


def find_rebuy(series, start: int, end: int, averages: dict, window: int) -> int:
    """start..end 중 다시 사는 첫 행. 없으면 -1. 그날 종가까지의 값만 쓴다."""
    for row in range(start, end + 1):
        level, fast, slow = averages[window][row], averages[5][row], averages[20][row]

        if math.isnan(level) or math.isnan(slow):
            continue

        close = series.close[row]

        if series.low[row] <= level <= close and fast >= slow and close >= slow:
            return row

    return -1


def chain(series, first_entry_row: int, first_exit_row: int, first_kind: int, averages: dict, cell: dict,
          allow_open: bool = False) -> tuple[list[dict], int]:
    """한 사건의 다시 산 매매 목록과, 자료 끝 때문에 뺀 건수. allow_open 이면 자료 끝에서 그날 종가로 평가해 남긴다(사례 표용)."""
    rows = len(series.dates)
    end = min(first_entry_row + cell["limit"], rows - 1)
    legs, previous_exit, previous_kind = [], first_exit_row, first_kind

    while len(legs) < MAX_REBUYS:
        if cell["after"] == "take" and previous_kind != 2:
            break

        row = find_rebuy(series, previous_exit + 1, end, averages, cell["window"])

        if row < 0:
            break

        last = row + base.HORIZON
        open_position = False

        if last > rows - 1:
            if not series.delisted and not allow_open:
                return legs, 1

            open_position = not series.delisted
            last = rows - 1

        if last <= row:
            break

        entry = float(series.close[row])
        stop_price = min(series.low[row] * (1 - STOP_BELOW / 100), entry * 0.999)
        exit_row, price, how = view.exit_path(series, row, entry, stop_price, entry * (1 + cell["take"] / 100), last)
        kind = {"손절": 1, "익절": 2, "기간": 0}[how]
        legs.append({"entry_row": row, "entry_date": int(series.dates[row]), "entry": entry, "stop": stop_price,
                     "exit_date": int(series.dates[exit_row]), "exit": price, "kind": kind,
                     "how": "보유 중(자료 끝)" if open_position and kind == 0 else how,
                     "ret": (price / entry * sweep.COST - 1) * 100, "hold": exit_row - row})
        previous_exit, previous_kind = exit_row, kind

    return legs, 0


def t_value(values: np.ndarray) -> float:
    if len(values) < 2 or np.std(values, ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def summary(values: np.ndarray) -> tuple[int, float, float]:
    return len(values), float(values.mean()) if len(values) else float("nan"), t_value(values)


def cells() -> list[dict]:
    return [dict(zip(AXES, values)) for values in itertools.product(WINDOWS, LIMITS, AFTERS, TAKES)]


def cell_key(cell: dict) -> tuple:
    return tuple(cell[axis] for axis in AXES)


def cell_name(cell: dict) -> str:
    after = {"take": "익절 뒤만", "all": "모든 청산 뒤"}[cell["after"]]
    return f"{cell['window']:2d}일선 {cell['limit']}일 안 {after:6s} 저가−3%/+{cell['take']}"


def neighbors(cell: dict) -> list[tuple]:
    result = []

    for axis in AXES:
        values = AXIS_VALUES[axis]
        position = values.index(cell[axis])
        others = [value for value in values if value != cell[axis]] if axis == "after" else \
            [values[step] for step in (position - 1, position + 1) if 0 <= step < len(values)]
        result += [cell_key({**cell, axis: value}) for value in others]

    return result


def run_rule(label: str, rule: dict, events: pd.DataFrame, by_code: dict, cache: dict) -> tuple[list[str], pd.DataFrame, dict]:
    firsts = first_trades(events, rule, by_code)
    firsts["event"] = np.arange(len(firsts))
    first_values = firsts["ret"].to_numpy(float)
    years = firsts["year"].to_numpy()
    early = years < SPLIT_YEAR
    ledger, table = [], []

    for cell in cells():
        dropped = 0
        per_event = np.zeros(len(firsts))

        for record in firsts.itertuples():
            series = by_code[record.code]
            legs, cut = chain(series, record.entry_row, record.exit_row, int(record.kind), averages_of(series, cache), cell)
            dropped += cut

            for order, leg in enumerate(legs, start=1):
                ledger.append({"rule": label, **cell, "event": record.event, "code": record.code, "surge": int(record.surge),
                               "year": int(record.year), "order": order, **{key: leg[key] for key in
                               ("entry_date", "entry", "exit_date", "exit", "kind", "ret", "hold")}})
                per_event[record.event] += leg["ret"]

        table.append({**cell, "dropped": dropped, "event_sum": per_event + first_values})

    ledger = pd.DataFrame(ledger)
    rows, lookup = [], {}

    for entry in table:
        cell = {axis: entry[axis] for axis in AXES}
        part = ledger[(ledger["rule"] == label) & np.logical_and.reduce([ledger[axis] == cell[axis] for axis in AXES])] \
            if len(ledger) else ledger
        values = part["ret"].to_numpy(float) if len(part) else np.array([])
        part_years = part["year"].to_numpy() if len(part) else np.array([])
        count, mean, t_statistic = summary(values)
        n_early, mean_early, t_early = summary(values[part_years < SPLIT_YEAR])
        n_late, mean_late, t_late = summary(values[part_years >= SPLIT_YEAR])
        event_sum = entry["event_sum"]
        _, event_mean, event_t = summary(event_sum)
        rows.append({**cell, "n": count, "mean": mean, "t": t_statistic, "win": float((values > 0).mean() * 100) if count else float("nan"),
                     "n_early": n_early, "mean_early": mean_early, "t_early": t_early,
                     "n_late": n_late, "mean_late": mean_late, "t_late": t_late,
                     "events_with_rebuy": int(part["event"].nunique()) if count else 0, "dropped": entry["dropped"],
                     "event_mean": event_mean, "event_t": event_t,
                     "event_mean_early": float(event_sum[early].mean()), "event_mean_late": float(event_sum[~early].mean())})
        lookup[cell_key(cell)] = mean

    frame = pd.DataFrame(rows)
    frame["neighbor_same"] = [sum(1 for key in neighbors(row) if not math.isnan(lookup[key]) and np.sign(lookup[key]) == np.sign(row["mean"]))
                              for row in frame.to_dict("records")]
    frame["neighbor_count"] = [len(neighbors(row)) for row in frame.to_dict("records")]
    frame["neighbor_min"] = [min(lookup[key] for key in neighbors(row)) for row in frame.to_dict("records")]
    frame["pass"] = (frame["t"] >= 3) & (frame["mean_early"] >= 0) & (frame["mean_late"] >= 0) & (frame["neighbor_same"] == frame["neighbor_count"])
    null = null_max_t(ledger, len(firsts))
    _, first_mean, first_t = summary(first_values)
    lines = [f"=== 첫 매수 {label}: {optimize.cell_name(rule)}",
             f"첫 매수만: 사건 {len(firsts)}  평균 {first_mean:+.2f}%  t{first_t:+.2f}  | 2010–17 {first_values[early].mean():+.2f} n{int(early.sum())}"
             f" | 2018–26 {first_values[~early].mean():+.2f} n{int((~early).sum())}  | 청산 익절 {int((firsts['kind'] == 2).sum())}·손절 "
             f"{int((firsts['kind'] == 1).sum())}·기한 {int((firsts['kind'] == 0).sum())}",
             f"무작위 부호 뒤집기 {FLIPS}번(seed {SEED}, 다시 산 매매 ≥ {NULL_MIN}건 칸) 36칸 최대 t: 중앙값 {np.nanmedian(null):.2f}, "
             f"95% {np.nanquantile(null, 0.95):.2f}, 99% {np.nanquantile(null, 0.99):.2f}",
             f"실제 최대 t({NULL_MIN}건 이상 칸): {frame.loc[frame['n'] >= NULL_MIN, 't'].max():.2f} — 무작위가 이 값 이상인 비율 "
             f"{(null >= frame.loc[frame['n'] >= NULL_MIN, 't'].max()).mean() * 100:.1f}%",
             f"통과 칸 {int(frame['pass'].sum())}/36",
             "",
             "칸(다시 산 매매만 t 순)                          건수  평균   t     승률  | 10–17 평균 t    | 18–26 평균 t    | 이웃 | 사건 단위 평균 t (10–17 | 18–26) | 뺀 건"]

    for row in frame.sort_values("t", ascending=False).to_dict("records"):
        lines.append(f"  {cell_name(row):40s} {row['n']:4d} {row['mean']:+6.2f} t{row['t']:+5.2f} {row['win']:5.1f}% | "
                     f"{row['mean_early']:+6.2f} t{row['t_early']:+5.2f} n{row['n_early']:3d} | {row['mean_late']:+6.2f} t{row['t_late']:+5.2f} n{row['n_late']:3d} | "
                     f"{row['neighbor_same']}/{row['neighbor_count']} | {row['event_mean']:+6.2f} t{row['event_t']:+5.2f} "
                     f"({row['event_mean_early']:+.2f} | {row['event_mean_late']:+.2f}) | {row['dropped']}"
                     + ("  통과" if row["pass"] else ""))

    return lines, frame, {"firsts": firsts, "ledger": ledger, "first_mean": first_mean, "first_t": first_t, "null": null}


def null_max_t(ledger: pd.DataFrame, event_count: int) -> np.ndarray:
    """사건마다 부호를 뒤집어(같은 사건의 모든 칸·모든 다시 사기에 같은 부호) 36칸 t 최대값을 FLIPS 번."""
    generator = np.random.default_rng(SEED)
    signs = generator.choice([-1.0, 1.0], size=(event_count, FLIPS))
    best = np.full(FLIPS, -np.inf)

    if not len(ledger):
        return best

    for _, part in ledger.groupby(AXES):
        if len(part) < NULL_MIN:
            continue

        flipped = part["ret"].to_numpy(float)[:, None] * signs[part["event"].to_numpy()]
        mean = flipped.mean(0)
        deviation = flipped.std(0, ddof=1)
        best = np.maximum(best, mean / (deviation / math.sqrt(len(part))))

    return best


def surge_runs(series, start: int, end: int) -> list[tuple[int, int]]:
    """[start, end] 안에 D0 가 있는 +10% 묶음 (묶음 첫 행, D0 행)."""
    close = series.close
    change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0
    result = []

    for surge_row in range(max(start, 1), min(end, len(close) - 2) + 1):
        if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
            continue

        run_start = surge_row

        while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
            run_start -= 1

        result.append((run_start, surge_row))

    return result


def first_check(series, run_start: int, surge_row: int, rule: dict, share_of) -> tuple[str, int, float, float]:
    """사례 표용: optimize.build 와 같은 조건을 하나씩 보고 걸린 조건을 적는다. 통과면 ("매수", 매수 행, 손절가, 관찰 최저가)."""
    close = series.close
    change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0
    run = surge_row - run_start + 1
    share = max(share_of(series, row) for row in range(run_start, surge_row + 1))
    raw_previous = close[run_start - 1] * series.factor[run_start - 1]
    move = close[surge_row] - close[run_start - 1]
    move_percent = move / close[run_start - 1] * 100
    head = f"묶음 {run}일 상승폭 {move_percent:.1f}% 비중 {share:.2f}% 전날 원종가 {raw_previous:,.0f}"

    if run < rule["run"]:
        return f"{head} → 묶음 {run}일 < {rule['run']}", -1, 0.0, 0.0

    if share < base.SHARE_MIN:
        return f"{head} → 비중 < {base.SHARE_MIN}%", -1, 0.0, 0.0

    if raw_previous < study33.MIN_PREVIOUS_CLOSE_KRW:
        return f"{head} → 전날 원종가 < 2,000", -1, 0.0, 0.0

    if move_percent < rule["move"]:
        return f"{head} → 상승폭 < {rule['move']}%", -1, 0.0, 0.0

    row = surge_row + rule["wait"]

    if row >= len(close):
        return f"{head} → D{rule['wait']} 자료 없음", -1, 0.0, 0.0

    for day in range(surge_row + 2, row + 1):
        if change(day) >= base.GAIN - 1e-9:
            return f"{head} → 관찰 중 {int(series.dates[day])} +{change(day):.1f}% 종가(새 묶음)", -1, 0.0, 0.0

    lowest = float(series.low[surge_row + 1:row + 1].min())
    depth = (close[surge_row] - lowest) / move * 100

    if depth >= rule["depth"]:
        return f"{head} → 되돌림 {depth:.1f}% ≥ {rule['depth']}%", -1, 0.0, 0.0

    percent = {"low1": 1, "low3": 3, "low5": 5}[rule["stop"]]
    return f"{head} 되돌림 {depth:.1f}% → 매수", row, min(lowest * (1 - percent / 100), float(close[row]) * 0.999), lowest


def case_table(series, rule: dict, cells_to_show: list[dict], share_of, cache: dict) -> list[str]:
    """보성파워텍 같은 사례 — 기간 안 묶음마다 첫 매수 판정, 샀으면 청산과 다시 사기. 자료 끝은 그날 종가로 평가해 '보유 중'."""
    start = int(np.searchsorted(series.dates, CASE_FROM))
    end = int(np.searchsorted(series.dates, CASE_TO, side="right")) - 1
    lines = []
    date = lambda row: int(series.dates[row])

    for run_start, surge_row in surge_runs(series, start, end):
        reason, row, stop_price, _ = first_check(series, run_start, surge_row, rule, share_of)
        lines.append(f"  묶음 {date(run_start)}–{date(surge_row)}: {reason}")

        if row < 0:
            continue

        entry = float(series.close[row])
        last = min(row + base.HORIZON, len(series.dates) - 1)
        exit_row, price, how = view.exit_path(series, row, entry, stop_price, entry * (1 + rule["take"] / 100), last)
        kind = {"손절": 1, "익절": 2, "기간": 0}[how]
        how = "보유 중(자료 끝)" if kind == 0 and last < row + base.HORIZON else how
        lines.append(f"    첫 매수 {date(row)} {entry:,.0f} → {date(exit_row)} {price:,.0f} {how} {(price / entry * sweep.COST - 1) * 100:+.2f}%"
                     f" (손절가 {stop_price:,.0f})")

        for cell in cells_to_show:
            legs, _ = chain(series, row, exit_row, kind, averages_of(series, cache), cell, allow_open=True)
            text = "; ".join(f"{leg['entry_date']} {leg['entry']:,.0f} → {leg['exit_date']} {leg['exit']:,.0f} {leg['how']} {leg['ret']:+.2f}%"
                             for leg in legs) or "다시 사기 없음"
            lines.append(f"    [{cell_name(cell)}] {text}")

    return lines


def signal_days(series, window: int, take: int, cache: dict) -> list[str]:
    """참고 — 첫 매수와 무관하게 사례 기간에 다시 사기 조건(N일선 닿고 위에서 마감·5일선 ≥ 20일선·종가 ≥ 20일선)이 선 날과,
    그날 종가에 샀다면 저가 −3% / +take 결과. 겹침 없이 하나 청산 뒤 다음을 찾는다."""
    averages = averages_of(series, cache)
    row = int(np.searchsorted(series.dates, CASE_FROM))
    end = int(np.searchsorted(series.dates, CASE_TO, side="right")) - 1
    lines = []

    while row <= end:
        row = find_rebuy(series, row, end, averages, window)

        if row < 0:
            break

        entry = float(series.close[row])
        last = min(row + base.HORIZON, len(series.dates) - 1)

        if last <= row:
            break

        stop_price = min(series.low[row] * (1 - STOP_BELOW / 100), entry * 0.999)
        exit_row, price, how = view.exit_path(series, row, entry, stop_price, entry * (1 + take / 100), last)
        how = "보유 중(자료 끝)" if how == "기간" and last < row + base.HORIZON else how
        lines.append(f"    {int(series.dates[row])} {entry:,.0f} ({window}일선 {averages[window][row]:,.0f}, 저가 {series.low[row]:,.0f}) → "
                     f"{int(series.dates[exit_row])} {price:,.0f} {how} {(price / entry * sweep.COST - 1) * 100:+.2f}%")
        row = exit_row + 1

    return lines


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    events = pd.read_parquet(optimize.EVENTS_PATH)
    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    by_code = {series.code: series for series in stocks}
    turnover = np.concatenate([series.close * series.factor * series.volume for series in stocks])
    totals = pd.Series(turnover).groupby(np.concatenate([series.dates for series in stocks])).sum().to_dict()
    share_of = lambda series, row: series.close[row] * series.factor[row] * series.volume[row] / totals[int(series.dates[row])] * 100
    cache: dict = {}
    lines = [f"스터디 35 rebuy — 커밋 {optimize.git_commit()}, 자료 끝 {base.LAST_DATE}, 사건 {optimize.EVENTS_PATH.name}, seed {SEED}", ""]
    frames, details = {}, {}

    for label, rule in FIRST_RULES.items():
        block, frame, detail = run_rule(label, rule, events, by_code, cache)
        lines += block + [""]
        frames[label], details[label] = frame, detail

    series = by_code[CASE_CODE]
    preset = {"window": 5, "limit": 40, "after": "take", "take": 15}

    for label, rule in FIRST_RULES.items():
        best = frames[label].sort_values("t", ascending=False).iloc[0]
        best_cell = {axis: best[axis] for axis in AXES}
        lines += [f"보성파워텍({CASE_CODE}) {CASE_FROM}–{CASE_TO} — 첫 매수 {label}, 칸 = 가장 좋은 칸 / N5·40일·익절 뒤만·+15"]
        lines += case_table(series, rule, [best_cell, preset] if best_cell != preset else [preset], share_of, cache) + [""]

    for window in WINDOWS:
        lines += [f"참고: 보성파워텍 첫 매수가 없어 다시 사기 조건만 본 날 — {window}일선, 저가−3%/+15"] + signal_days(series, window, 15, cache) + [""]

    ledger = pd.concat([detail["ledger"] for detail in details.values()], ignore_index=True)
    ledger.to_parquet(LEDGER_PATH, index=False)
    text = "\n".join(lines)
    TEXT_PATH.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
