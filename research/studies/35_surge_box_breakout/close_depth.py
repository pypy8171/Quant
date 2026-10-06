"""스터디 35 — 1번 칸의 되돌림을 저가 대신 종가로 재면 어떻게 되나, 그리고 실제로 낼 수 있는 주문(다음 날 시가 매수)으로도 버티나
(사용자 2026-10-06, 계기 HD현대에너지솔루션(322000) 2026-02: D1 장중 저가 77,200 하나로 되돌림 17.0%, 종가로 재면 10.9%).

사건  : optimize.build 와 같다(+10% 종가, 전날 원종가 ≥ 2,000, 거래대금 시장 비중 ≥ 0.2%, 연속 급등은 묶음, D0 = 묶음 마지막 날,
        상승폭 = close[D0] − close[묶음 시작 전날], 관찰 D1..Dk 중 +10% 종가가 나오면 그 k 와 더 긴 k 는 뺀다). 묶음 2일 이상 고정.
        기간 = 자료 시작(D0 2009-06)–2026-10-02 전부. 구간 2010–17 / 2018–26 은 D0 연도(2009 사건 1–2건은 앞 구간에 들어간다).
칸    : 되돌림 측정(저가 = D1..Dk 최저 저가 / 종가 = D1..Dk 최저 종가) × 한도 10·15·20·25·30% × k 3·4·5 × 상승폭 하한 0·20·30·40%
        × 손절(관찰 최저 저가 −3% / 관찰 최저 종가 −3% / 매수가 −8% / 매수가 −10%) × 익절 15·20·25·30% × 매수(Dk 종가 / D(k+1) 시가)
        = 3,840칸. 값은 사용자가 정함. 묶음 2일 이상이면 상승폭이 이미 21% 이상이라 하한 0% 와 20% 칸은 같은 결과다.
청산  : optimize.path_exits 와 같은 규칙(같은 날 둘 다 닿으면 손절, 갭은 시가, 비용 ma_sweep.COST, 120거래일 안에 안 닿으면
        D(k+120) 종가). 관찰 손절가 = min(관찰 최저값 × 0.97, 매수가 × 0.999). Dk 종가 매수는 D(k+1)부터, D(k+1) 시가 매수는
        그날부터 판정한다. 마지막 날은 두 매수 모두 Dk + 120거래일(내가 정함 — 같은 날 끝나게 해 매수 시점만 비교되게).
시가 매수: D(k+1)는 다음 거래일(거래정지 날은 자료에 없다). 시가가 전날 종가 대비 가격제한폭 − 0.5%p 이상(2015-06-15 전 15%,
        뒤 30%)이면 상한가 시가라 체결 못 한 것으로 보고 뺀다(내가 정함). 몇 건인지 적는다.
판정(사용자 2026-10-06):
  1. 칸 합격 = 전체 t ≥ 3, 2010–17·2018–26 평균 둘 다 ≥ 0, 이웃 칸(축 하나만 한 칸 옮김. 측정·매수는 다른 값, 손절은 위 순서로
     옆 칸 — 손절 순서는 내가 정함) 평균 전부 같은 부호(건수 0 인 이웃은 뺀다).
  2. 탐색 보정: 사건(종목·D0)마다 부호를 뒤집어(그 사건의 모든 k·매수·청산 칸에 같은 부호) 3,840칸 최대 t 를 200번(seed 20261006).
     대상 칸은 전체 건수 ≥ 30(내가 정함), 실제 최대 t 도 같은 대상에서.
  3. 연도 단위 교차검증: 2010–2026 의 해 하나를 빼고, 나머지 해(2009 포함)에서 건수 ≥ 30(내가 정함)인 칸 중 t 최대 칸을 골라 뺀 해의
     매매에 적용 — 17번. 뺀 해 매매를 이어 붙여 평균·t·연도별 부호. 보유가 해를 넘는 매매는 그대로 둔다(D0 연도로 나눔).
  4. 저가 vs 종가, 종가 매수 vs 시가 매수: 나머지 축이 같은 짝끼리 비교. 1번 칸은 사건 단위 짝 차이도 낸다.
산출  : close_depth.txt(요약), close_depth_events.parquet(사건 × k × 매수 한 줄, 다시 돌릴 때 재사용), close_depth_cells.parquet(칸 한 줄).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/close_depth.py [--rebuild]
"""
from __future__ import annotations

import argparse
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
base = optimize.base
sweep = optimize.sweep
study33 = optimize.study33
SEED = 20261006
FLIPS = 200
SPLIT_YEAR = optimize.SPLIT_YEAR
NULL_MIN = 30
SELECT_MIN = 30
FOLD_YEARS = list(range(2010, 2027))
MEASURES = ["low", "close"]
DEPTHS = [10, 15, 20, 25, 30]
WAITS = [3, 4, 5]
MOVES = [0, 20, 30, 40]
STOPS = [("low3", "low", 3), ("close3", "close", 3), ("entry8", "entry", 8), ("entry10", "entry", 10)]
TAKES = [15, 20, 25, 30]
ENTRIES = ["close", "open"]
EXITS = [(stop[0], take) for stop in STOPS for take in TAKES]
AXES = ["measure", "depth", "wait", "move", "stop", "take", "entry"]
AXIS_VALUES = {"measure": MEASURES, "depth": DEPTHS, "wait": WAITS, "move": MOVES, "stop": [stop[0] for stop in STOPS],
               "take": TAKES, "entry": ENTRIES}
CATEGORICAL = {"measure", "entry"}
FIRST_CELL = {"measure": "low", "depth": 15, "wait": 4, "move": 30, "stop": "low3", "take": 15, "entry": "close"}
LIMIT_CHANGE_DATE = 20150615
CASE_CODE, CASE_SURGE = "322000", 20260205
EVENTS_PATH = HERE / "close_depth_events.parquet"
CELLS_PATH = HERE / "close_depth_cells.parquet"
TEXT_PATH = HERE / "close_depth.txt"


def exit_column(stop: str, take: int, field: str) -> str:
    return optimize.exit_column(stop, take, field)


def path_exits(series, first_row: int, entry: float, lowest_low: float, lowest_close: float, last: int) -> dict:
    """first_row 부터 last 까지 16개 청산 칸. 규칙은 optimize.path_exits 와 같고 판정 시작 행만 받는다."""
    opens = series.open_price[first_row:last + 1]
    lows = series.low[first_row:last + 1]
    highs = series.high[first_row:last + 1]
    anchors = {"low": lowest_low, "close": lowest_close}
    result = {}

    for name, kind, percent in STOPS:
        stop_price = entry * (1 - percent / 100) if kind == "entry" else min(anchors[kind] * (1 - percent / 100), entry * 0.999)
        stop_hits = np.flatnonzero(lows <= stop_price)
        stop_at = stop_hits[0] if len(stop_hits) else math.inf

        for take in TAKES:
            take_price = entry * (1 + take / 100)
            take_hits = np.flatnonzero(highs >= take_price)
            take_at = take_hits[0] if len(take_hits) else math.inf

            if stop_at <= take_at and stop_at < math.inf:
                price, code, held = min(opens[stop_at], stop_price), 1, stop_at + 1
            elif take_at < math.inf:
                price, code, held = max(opens[take_at], take_price), 2, take_at + 1
            else:
                price, code, held = series.close[last], 0, last - first_row + 1

            result[exit_column(name, take, "ret")] = (price / entry * sweep.COST - 1) * 100
            result[exit_column(name, take, "kind")] = code
            result[exit_column(name, take, "hold")] = int(held)

    return result


def build() -> tuple[pd.DataFrame, dict]:
    """사건 × k × 매수 한 줄. 사건 조건은 optimize.build 를 그대로 옮겼다(되돌림 거르기만 두 측정 중 작은 쪽 < 30%)."""
    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    frames = []

    for series in stocks:
        if len(series.dates) >= 2:
            frames.append(pd.DataFrame({"code": series.code, "date": series.dates,
                                        "turnover": series.close * series.factor * series.volume,
                                        "change": np.r_[np.nan, series.close[1:] / series.close[:-1] - 1.0] * 100.0}))

    market = pd.concat(frames, ignore_index=True)
    market["share"] = market["turnover"] / market.groupby("date")["turnover"].transform("sum") * 100.0
    candidates = market[market["change"] >= base.GAIN - 1e-9]
    relative = dict(zip(zip(candidates["code"], candidates["date"].astype(int)), candidates["share"]))
    del market, frames, candidates
    records = []
    counts = {"open_limit_skipped": 0}
    limit = max(DEPTHS)

    for series in stocks:
        rows = len(series.dates)

        if rows < 30:
            continue

        close = series.close
        volume = series.volume.astype(float)
        raw_close = close * series.factor
        change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

        for surge_row in range(1, rows - 1):
            if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
                continue

            run_start = surge_row

            while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
                run_start -= 1

            run = surge_row - run_start + 1

            if run < 2:
                continue

            share = max(relative.get((series.code, int(series.dates[row])), 0.0) for row in range(run_start, surge_row + 1))

            if share < base.SHARE_MIN or raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or volume[surge_row] <= 0:
                continue

            move = close[surge_row] - close[run_start - 1]

            if move <= 0:
                continue

            for wait in WAITS:
                row = surge_row + wait

                if row >= rows - 1:
                    break

                if any(change(day) >= base.GAIN - 1e-9 for day in range(surge_row + 2, row + 1)):
                    break

                last = row + base.HORIZON

                if last > rows - 1:
                    if not series.delisted:
                        continue

                    last = rows - 1

                lowest_low = float(series.low[surge_row + 1:row + 1].min())
                lowest_close = float(close[surge_row + 1:row + 1].min())
                depth_low = (close[surge_row] - lowest_low) / move * 100
                depth_close = (close[surge_row] - lowest_close) / move * 100

                if min(depth_low, depth_close) >= limit:
                    continue

                common = {"code": series.code, "surge": int(series.dates[surge_row]), "year": int(series.dates[surge_row]) // 10000,
                          "run": run, "wait": wait, "move_pct": move / close[run_start - 1] * 100,
                          "depth_low": depth_low, "depth_close": depth_close}
                record = {**common, "entry": "close", "entry_date": int(series.dates[row]), "entry_price": float(close[row])}
                record.update(path_exits(series, row + 1, float(close[row]), lowest_low, lowest_close, last))
                records.append(record)
                next_row = row + 1
                gap = (series.open_price[next_row] / close[row] - 1) * 100
                limit_percent = 15.0 if int(series.dates[next_row]) < LIMIT_CHANGE_DATE else 30.0

                if gap >= limit_percent - 0.5:
                    counts["open_limit_skipped"] += 1
                    continue

                entry = float(series.open_price[next_row])
                record = {**common, "entry": "open", "entry_date": int(series.dates[next_row]), "entry_price": entry}
                record.update(path_exits(series, next_row, entry, lowest_low, lowest_close, last))
                records.append(record)

    return pd.DataFrame(records), counts


def t_of(total: np.ndarray, square: np.ndarray, count: np.ndarray) -> np.ndarray:
    return optimize.t_of(total, square, count)


def filter_masks(frame: pd.DataFrame) -> tuple[list[tuple], np.ndarray]:
    """측정 × 한도 × 상승폭 40개 조건 마스크(행 = 조건)."""
    keys, masks = [], []

    for measure, depth, move in itertools.product(MEASURES, DEPTHS, MOVES):
        mask = (frame[f"depth_{measure}"].to_numpy() < depth) & (frame["move_pct"].to_numpy() >= move)
        keys.append((measure, depth, move))
        masks.append(mask)

    return keys, np.array(masks, dtype=float)


def cell_table(events: pd.DataFrame) -> tuple[pd.DataFrame, np.ndarray]:
    """칸 한 줄 표와 연도별 (건수, 합, 제곱합) 배열[칸, 연도, 3]. 연도 축은 events 의 모든 D0 연도."""
    years = sorted(events["year"].unique())
    rows, per_year = [], []

    for wait, entry in itertools.product(WAITS, ENTRIES):
        frame = events[(events["wait"] == wait) & (events["entry"] == entry)]
        keys, masks = filter_masks(frame)
        values = frame[[exit_column(stop, take, "ret") for stop, take in EXITS]].to_numpy(float)
        year_onehot = (frame["year"].to_numpy()[:, None] == np.array(years)[None, :]).astype(float)
        # 조건 × 연도 × 청산
        count = np.einsum("cr,ry->cy", masks, year_onehot)[:, :, None].repeat(len(EXITS), 2)
        total = np.einsum("cr,ry,re->cye", masks, year_onehot, values, optimize=True)
        square = np.einsum("cr,ry,re->cye", masks, year_onehot, values ** 2, optimize=True)
        wins = np.einsum("cr,re->ce", masks, (values > 0).astype(float))

        for key_index, (measure, depth, move) in enumerate(keys):
            for exit_index, (stop, take) in enumerate(EXITS):
                rows.append({"measure": measure, "depth": depth, "wait": wait, "move": move, "stop": stop, "take": take, "entry": entry,
                             "wins": wins[key_index, exit_index]})
                per_year.append(np.stack([count[key_index, :, exit_index], total[key_index, :, exit_index],
                                          square[key_index, :, exit_index]], axis=1))

    cells = pd.DataFrame(rows)
    per_year = np.array(per_year)
    year_array = np.array(years)

    for period, selector in (("all", np.ones(len(years), bool)), ("early", year_array < SPLIT_YEAR), ("late", year_array >= SPLIT_YEAR)):
        count, total, square = (per_year[:, selector, part].sum(1) for part in range(3))
        cells[f"n_{period}"] = count.astype(int)
        cells[f"mean_{period}"] = total / np.where(count > 0, count, np.nan)
        cells[f"t_{period}"] = t_of(total, square, count)

    cells["win"] = cells["wins"] / cells["n_all"].replace(0, np.nan) * 100
    cells["plus_years"] = ((per_year[:, :, 1] > 0) & (per_year[:, :, 0] > 0)).sum(1)
    cells["years"] = (per_year[:, :, 0] > 0).sum(1)
    return cells, per_year


def cell_key(cell: dict) -> tuple:
    return tuple(cell[axis] for axis in AXES)


def neighbors(cell: dict) -> list[dict]:
    result = []

    for axis in AXES:
        values = AXIS_VALUES[axis]
        position = values.index(cell[axis])
        others = [value for value in values if value != cell[axis]] if axis in CATEGORICAL else \
            [values[step] for step in (position - 1, position + 1) if 0 <= step < len(values)]

        for value in others:
            result.append({**cell, axis: value})

    return result


def add_verdicts(cells: pd.DataFrame) -> pd.DataFrame:
    lookup = {cell_key(row): (row["n_all"], row["mean_all"]) for row in cells.to_dict("records")}
    same_counts, near_counts, near_minimum, passes = [], [], [], []

    for row in cells.to_dict("records"):
        near = [lookup[cell_key(other)] for other in neighbors(row)]
        near = [mean for count, mean in near if count > 0]
        same = sum(1 for mean in near if np.sign(mean) == np.sign(row["mean_all"]))
        same_counts.append(same)
        near_counts.append(len(near))
        near_minimum.append(min(near) if near else float("nan"))
        passes.append(bool(row["t_all"] >= 3 and row["mean_early"] >= 0 and row["mean_late"] >= 0 and same == len(near)))

    cells["neighbor_same"], cells["neighbor_count"], cells["neighbor_min"], cells["passed"] = same_counts, near_counts, near_minimum, passes
    return cells


def cell_trades(events: pd.DataFrame, cell: dict) -> pd.DataFrame:
    frame = events[(events["wait"] == cell["wait"]) & (events["entry"] == cell["entry"])
                   & (events[f"depth_{cell['measure']}"] < cell["depth"]) & (events["move_pct"] >= cell["move"])]
    return pd.DataFrame({"code": frame["code"], "surge": frame["surge"], "entry_date": frame["entry_date"], "year": frame["year"],
                         "entry_price": frame["entry_price"],
                         "ret": frame[exit_column(cell["stop"], cell["take"], "ret")],
                         "kind": frame[exit_column(cell["stop"], cell["take"], "kind")],
                         "hold": frame[exit_column(cell["stop"], cell["take"], "hold")]}).sort_values(["entry_date", "code"], kind="mergesort")


def t_value(values) -> float:
    values = np.asarray(values, dtype=float)

    if len(values) < 2 or values.std(ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def null_max_t(events: pd.DataFrame) -> np.ndarray:
    """사건마다 부호를 뒤집어 전 칸(전체 건수 ≥ NULL_MIN) t 최대값을 FLIPS 번."""
    generator = np.random.default_rng(SEED)
    event_keys = pd.factorize(events["code"] + "_" + events["surge"].astype(str))[0]
    signs = generator.choice([-1.0, 1.0], size=(event_keys.max() + 1, FLIPS))
    best = np.full(FLIPS, -np.inf)

    for wait, entry in itertools.product(WAITS, ENTRIES):
        selector = ((events["wait"] == wait) & (events["entry"] == entry)).to_numpy()
        frame = events[selector]
        _, masks = filter_masks(frame)
        flips = signs[event_keys[selector]]
        count = masks.sum(1)
        usable = count >= NULL_MIN

        if not usable.any():
            continue

        masks, count = masks[usable], count[usable][:, None]

        for stop, take in EXITS:
            values = frame[exit_column(stop, take, "ret")].to_numpy(float)
            square = (masks @ values ** 2)[:, None]
            total = masks @ (values[:, None] * flips)
            best = np.maximum(best, np.nanmax(t_of(total, square, count), axis=0))

    return best


def year_cross_validation(events: pd.DataFrame, cells: pd.DataFrame, per_year: np.ndarray) -> tuple[pd.DataFrame, list[dict]]:
    years = sorted(events["year"].unique())
    pieces, folds = [], []

    for held_year in FOLD_YEARS:
        held = years.index(held_year)
        train = per_year.sum(1) - per_year[:, held, :]
        t_train = t_of(train[:, 1], train[:, 2], train[:, 0])
        t_train = np.where(train[:, 0] >= SELECT_MIN, t_train, -np.inf)
        best = int(np.nanargmax(t_train))
        cell = cells.iloc[best][AXES].to_dict()
        trades = cell_trades(events, cell)
        trades = trades[trades["year"] == held_year]
        pieces.append(trades.assign(held_year=held_year))
        folds.append({"year": held_year, "cell": cell, "t_train": float(t_train[best]), "n_train": int(train[best, 0]),
                      "n": len(trades), "mean": trades["ret"].mean() if len(trades) else float("nan")})

    return pd.concat(pieces, ignore_index=True), folds


def cell_name(cell: dict) -> str:
    measure = {"low": "저가", "close": "종가"}[cell["measure"]]
    entry = {"close": "Dk종가", "open": "D(k+1)시가"}[cell["entry"]]
    return f"{measure} d{cell['depth']} k{cell['wait']} 상승{cell['move']}+ {cell['stop']}/+{cell['take']} {entry}"


def format_cell(row: dict) -> str:
    return (f"{cell_name(row):44s} n{row['n_all']:4d} {row['mean_all']:+6.2f} t{row['t_all']:+5.2f} 승{row['win']:4.1f}% | "
            f"10–17 {row['mean_early']:+6.2f} n{row['n_early']:3d} | 18–26 {row['mean_late']:+6.2f} n{row['n_late']:3d} | "
            f"이웃 {row['neighbor_same']}/{row['neighbor_count']} (최저 {row['neighbor_min']:+.2f}) | 플러스해 {row['plus_years']}/{row['years']}")


def paired_cells(cells: pd.DataFrame, axis: str, left: str, right: str) -> pd.DataFrame:
    others = [name for name in AXES if name != axis]
    columns = others + ["n_all", "mean_all", "t_all"]
    pairs = cells[cells[axis] == left][columns].merge(cells[cells[axis] == right][columns], on=others, suffixes=(f"_{left}", f"_{right}"))
    return pairs[(pairs[f"n_all_{left}"] > 0) & (pairs[f"n_all_{right}"] > 0)]


def git_commit() -> str:
    return optimize.git_commit()


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--rebuild", action="store_true", help="close_depth_events.parquet 를 다시 만든다")
    arguments = parser.parse_args()
    skipped_note = ""

    if arguments.rebuild or not EVENTS_PATH.exists():
        events, counts = build()
        events.to_parquet(EVENTS_PATH, index=False)
        skipped_note = f"(이번 빌드에서 상한가 시가로 뺀 시가 매수 {counts['open_limit_skipped']}건)"

    events = pd.read_parquet(EVENTS_PATH)
    close_rows = int((events["entry"] == "close").sum())
    open_rows = int((events["entry"] == "open").sum())
    cells, per_year = cell_table(events)
    cells = add_verdicts(cells)
    cells.to_parquet(CELLS_PATH, index=False)
    lookup = {cell_key(row): row for row in cells.to_dict("records")}
    lines = [f"스터디 35 close_depth — 커밋 {git_commit()}, 자료 끝 {base.LAST_DATE}, seed {SEED}",
             f"사건 × k × 매수 {len(events):,}줄(Dk 종가 {close_rows:,} / D(k+1) 시가 {open_rows:,}, 차이 = 상한가 시가로 뺀 시가 매수 {close_rows - open_rows}건) "
             f"{skipped_note}, 사건 {events.groupby(['code', 'surge']).ngroups:,}개",
             f"칸 {len(cells):,}개(결과가 다른 칸 {len(cells[cells['n_all'] > 0].drop_duplicates(['wait', 'entry', 'stop', 'take', 'n_all', 'mean_all', 't_all'])):,}개 — "
             f"상승 0%·20% 칸은 같다), 건수 ≥ {NULL_MIN} 칸 {int((cells['n_all'] >= NULL_MIN).sum()):,}개", ""]

    first = lookup[cell_key(FIRST_CELL)]
    lines += ["재현 점검: 1번 칸(저가 d15 k4 상승30+ low3/+15 Dk종가) " + format_cell(first) + "  (기대 n264 +3.52 t4.32)", ""]

    # 1) 합격 칸
    passed = cells[cells["passed"]].sort_values("t_all", ascending=False)
    passed_unique = passed.drop_duplicates(["wait", "entry", "stop", "take", "n_all", "mean_all", "t_all"])
    lines += [f"1) 합격 칸 {len(passed):,}개(상승 0%·20% 중복 뺀 {len(passed_unique):,}개). "
              f"종가 측정 {int((passed_unique['measure'] == 'close').sum())} / 저가 측정 {int((passed_unique['measure'] == 'low').sum())}, "
              f"시가 매수 {int((passed_unique['entry'] == 'open').sum())} / 종가 매수 {int((passed_unique['entry'] == 'close').sum())}. t 상위 10칸:"]

    for order, row in enumerate(passed_unique.head(10).to_dict("records"), start=1):
        lines.append(f"  {order:2d}. " + format_cell(row))

    best_open = passed_unique[passed_unique["entry"] == "open"].head(10)
    lines += ["", f"  시가 매수 합격 칸 t 상위 {len(best_open)}칸:"]

    for order, row in enumerate(best_open.to_dict("records"), start=1):
        lines.append(f"  {order:2d}. " + format_cell(row))

    # 2) 무작위 최대 t
    null = null_max_t(events)
    observed = float(cells.loc[cells["n_all"] >= NULL_MIN, "t_all"].max())
    lines += ["", f"2) 부호 뒤집기 {FLIPS}번 최대 t(건수 ≥ {NULL_MIN} 칸): 중앙값 {np.median(null):.2f}, 95% {np.quantile(null, 0.95):.2f}, "
              f"99% {np.quantile(null, 0.99):.2f}, 최대 {null.max():.2f} — 실제 최대 t {observed:.2f}, 무작위가 그 이상인 비율 {(null >= observed).mean() * 100:.1f}%",
              f"   실제 t 가 무작위 95%({np.quantile(null, 0.95):.2f}) 이상인 칸 {int((cells['t_all'] >= np.quantile(null, 0.95)).sum())}개, "
              f"99% 이상 {int((cells['t_all'] >= np.quantile(null, 0.99)).sum())}개"]

    # 3) 연도 단위 교차검증
    held, folds = year_cross_validation(events, cells, per_year)
    yearly = held.groupby("held_year")["ret"].mean()
    lines += ["", f"3) 연도 단위 교차검증(해 하나 빼고 건수 ≥ {SELECT_MIN} 칸 중 t 최대 칸 → 뺀 해에 적용, {len(FOLD_YEARS)}번)",
              f"   이어 붙인 매매 n{len(held)} 평균 {held['ret'].mean():+.2f}% t{t_value(held['ret']):+.2f} 승{(held['ret'] > 0).mean() * 100:.1f}% | "
              f"플러스 해 {int((yearly > 0).sum())}/{len(yearly)} | 2010–17 {held.loc[held['held_year'] < SPLIT_YEAR, 'ret'].mean():+.2f} "
              f"n{int((held['held_year'] < SPLIT_YEAR).sum())} | 2018–26 {held.loc[held['held_year'] >= SPLIT_YEAR, 'ret'].mean():+.2f} "
              f"n{int((held['held_year'] >= SPLIT_YEAR).sum())}"]
    chosen = pd.Series([cell_name(fold["cell"]) for fold in folds]).value_counts()
    lines.append("   고른 칸 빈도: " + ", ".join(f"{name} ×{count}" for name, count in chosen.items()))

    for fold in folds:
        lines.append(f"   {fold['year']} 고른 칸 {cell_name(fold['cell']):44s} (학습 t{fold['t_train']:+.2f} n{fold['n_train']}) → 그해 n{fold['n']:3d} "
                     f"{fold['mean']:+6.2f}%")

    # 4) 저가 → 종가
    pairs = paired_cells(cells, "measure", "low", "close")
    differing = pairs[pairs["n_all_low"] != pairs["n_all_close"]]
    lines += ["", f"4) 저가 → 종가(나머지 축이 같은 짝 {len(pairs):,}개, 건수가 달라진 짝 {len(differing):,}개)",
              f"   종가 평균이 더 높은 짝 {(pairs['mean_all_close'] > pairs['mean_all_low']).mean() * 100:.1f}%, t 가 더 높은 짝 "
              f"{(pairs['t_all_close'] > pairs['t_all_low']).mean() * 100:.1f}%, 평균 차이(종가 − 저가) 중앙값 {(pairs['mean_all_close'] - pairs['mean_all_low']).median():+.2f}%p, "
              f"건수 비 중앙값 {(pairs['n_all_close'] / pairs['n_all_low']).median():.2f}배"]

    for entry in ENTRIES:
        part = pairs[pairs["entry"] == entry]
        lines.append(f"   매수 {entry}: 종가 평균 더 높은 짝 {(part['mean_all_close'] > part['mean_all_low']).mean() * 100:.1f}%, "
                     f"차이 중앙값 {(part['mean_all_close'] - part['mean_all_low']).median():+.2f}%p")

    for depth in DEPTHS:
        part = pairs[pairs["depth"] == depth]
        lines.append(f"   한도 {depth}%: 종가 평균 더 높은 짝 {(part['mean_all_close'] > part['mean_all_low']).mean() * 100:.1f}%, "
                     f"차이 중앙값 {(part['mean_all_close'] - part['mean_all_low']).median():+.2f}%p, 건수 비 {(part['n_all_close'] / part['n_all_low']).median():.2f}배")

    close_twin = lookup[cell_key({**FIRST_CELL, "measure": "close"})]
    lines += ["   1번 칸       " + format_cell(first), "   종가 짝       " + format_cell(close_twin)]
    trades_low = cell_trades(events, FIRST_CELL)
    trades_close = cell_trades(events, {**FIRST_CELL, "measure": "close"})
    added = trades_close.merge(trades_low[["code", "surge"]], on=["code", "surge"], how="left", indicator=True)
    added = added[added["_merge"] == "left_only"]
    lines.append(f"   종가 짝에서만 잡힌 사건 n{len(added)} 평균 {added['ret'].mean():+.2f}% t{t_value(added['ret']):+.2f} 승{(added['ret'] > 0).mean() * 100:.1f}%"
                 f" (1번 칸 사건 {len(trades_low)}건은 종가 짝에 전부 들어감: {len(trades_low.merge(trades_close[['code', 'surge']], on=['code', 'surge']))}건)")
    stop_twin = lookup[cell_key({**FIRST_CELL, "measure": "close", "stop": "close3"})]
    lines.append("   종가 짝 + 손절도 종가 " + format_cell(stop_twin))

    # 5) 종가 매수 → 다음 날 시가 매수
    pairs = paired_cells(cells, "entry", "close", "open")
    difference = pairs["mean_all_open"] - pairs["mean_all_close"]
    lines += ["", f"5) Dk 종가 매수 → D(k+1) 시가 매수(짝 {len(pairs):,}개)",
              f"   평균 차이(시가 − 종가) 중앙값 {difference.median():+.2f}%p, 평균 {difference.mean():+.2f}%p, 시가가 더 높은 짝 {(difference > 0).mean() * 100:.1f}%, "
              f"t 차이 중앙값 {(pairs['t_all_open'] - pairs['t_all_close']).median():+.2f}"]

    for stop in AXIS_VALUES["stop"]:
        part = pairs[pairs["stop"] == stop]
        gap = part["mean_all_open"] - part["mean_all_close"]
        lines.append(f"   손절 {stop:7s}: 차이 중앙값 {gap.median():+.2f}%p, 시가가 더 높은 짝 {(gap > 0).mean() * 100:.1f}%")

    for cell in (FIRST_CELL, {**FIRST_CELL, "measure": "close"}):
        left = cell_trades(events, cell)
        right = cell_trades(events, {**cell, "entry": "open"})
        joined = left.merge(right, on=["code", "surge"], suffixes=("_close", "_open"))
        gap_open = (joined["entry_price_open"] / joined["entry_price_close"] - 1) * 100
        event_gap = joined["ret_open"] - joined["ret_close"]
        lines.append(f"   {cell_name(cell)}: 시가 칸 " + format_cell(lookup[cell_key({**cell, 'entry': 'open'})]))
        lines.append(f"      같은 사건 {len(joined)}건 짝 차이(시가 − 종가) 평균 {event_gap.mean():+.2f}%p t{t_value(event_gap):+.2f}, "
                     f"D(k+1) 시가 갭 중앙값 {gap_open.median():+.2f}% (평균 {gap_open.mean():+.2f}%)")

    # 6) 322000
    lines += ["", f"6) {CASE_CODE} D0 {CASE_SURGE}"]
    case = events[(events["code"] == CASE_CODE) & (events["surge"] == CASE_SURGE)]

    for record in case[case["entry"] == "close"].to_dict("records"):
        lines.append(f"   k{record['wait']}: 되돌림 저가 {record['depth_low']:.1f}% / 종가 {record['depth_close']:.1f}%, 상승폭 {record['move_pct']:.1f}%")

    for cell in ({**FIRST_CELL, "measure": "close"}, {**FIRST_CELL, "measure": "close", "entry": "open"},
                 {**FIRST_CELL, "measure": "close", "stop": "close3"}, {**FIRST_CELL, "measure": "close", "stop": "close3", "entry": "open"}):
        trades = cell_trades(events, cell)
        hit = trades[(trades["code"] == CASE_CODE) & (trades["surge"] == CASE_SURGE)]

        if len(hit):
            record = hit.iloc[0]
            how = {0: "기한", 1: "손절", 2: "익절"}[int(record["kind"])]
            lines.append(f"   {cell_name(cell)}: 매수 {record['entry_date']} {record['entry_price']:,.0f} → {how} {record['hold']}일 {record['ret']:+.2f}%")
        else:
            lines.append(f"   {cell_name(cell)}: 안 잡힘")

    text = "\n".join(lines)
    TEXT_PATH.write_bytes((text + "\n").encode("utf-8"))
    held.to_parquet(HERE / "close_depth_cv_trades.parquet", index=False)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
