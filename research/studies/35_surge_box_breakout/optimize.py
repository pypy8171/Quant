"""스터디 35 — 강한 종목 후보 B5_20 을 조건 조합으로 넓게 뒤지고, 탐색이 만든 가짜 t 를 2겹 교차검증으로 거른다(사용자 2026-10-02
"합격하게 만들어라, 수치를 바꾸든 조합을 하든").

사건  : strength.py 와 같다(+10% 종가, 전날 원종가 ≥ 2,000, 거래대금 시장 비중 ≥ 0.2%, 연속 급등은 묶음, D0 = 묶음 마지막 날,
        상승폭 move = close[D0] − close[묶음 시작 전날]). 관찰 D1..Dk 중 다시 +10% 종가가 나오면 그 사건(그 k 와 더 긴 k)은 뺀다.
        기간 = 자료 시작–2026-10-02 전부. 앞·뒤 구간은 D0 연도로 2010–2017 / 2018–2026.
칸    : 관찰 k 2–7 × 되돌림 한도 d 10·15·20·25·30% × 묶음 2일+·3일+ × 상승폭 최소 0·20·30·40% × Dk 종가 조건(무관·≥ D0 종가·≥ 5일선)
        × 국면(무관·그 종목 시장 지수 종가 > 60일선, Dk 기준) × 손절 5(관찰 최저 저가 −1·−3·−5%, 매수가 −8·−10%) × 익절 15·20·25·30%
        = 28,800칸. 값은 사용자가 정함. 묶음 2일 이상이면 상승폭이 이미 21% 이상이라 상승폭 0% 와 20% 칸은 같은 결과다.
매수  : Dk 종가. 청산은 다음 날부터, 120거래일 안에 안 닿으면 120일째 종가. 같은 날 둘 다 닿으면 손절, 갭은 시가. 비용 ma_sweep.COST.
        관찰 저가 손절은 strength.py lowbox 와 같이 min(최저 저가 × (1 − x), 매수가 × 0.999).
지수  : FinanceDataReader KS11·KQ11 종가(2009-01-02 이후). 처음 받은 값을 index_snapshot.parquet 로 고정해 다시 쓴다.
        스냅샷 끝 뒤 날짜는 국면을 모름 → 국면 칸에서 빠진다(120일 보유가 필요해 그 무렵 사건은 상폐 종목 몇 건뿐).
판정(사용자 2026-10-02):
  1. 교차검증 (가) 2010–17 에서 건수 ≥ 100 칸 중 t 최대 칸을 골라 2018–26 에서 본다, (나) 반대. 고르지 않은 구간 평균 > 0, t ≥ 2 면 통과.
  2. 두 구간 순위가 모두 상위 5% 인 칸을 두 순위 합으로 줄 세워 20칸. 순위 대상은 구간마다 건수 ≥ 30(내가 정함).
  3. 후보마다 전체·두 구간·연도·이웃 칸·상위 10건 뺀 평균·최대 연속 손절. 기준선 B5_20 같은 표.
  4. 통과선: 전체 t ≥ 3, 이웃 칸(축 하나씩 한 칸 옮김, 범주 축은 다른 값 전부) 평균 전부 > 0, 두 구간 평균 ≥ 0, 교차검증 두 방향 통과.
  5. 무작위 기대 최대 t: 사건마다 수익 부호를 무작위로 뒤집어(같은 사건의 모든 k·청산 칸에 같은 부호) 전 칸 t 를 다시 내고 최대를
     200번(seed 20261002). 대상 칸은 전체 건수 ≥ 100(내가 정함).
산출  : optimize.parquet(칸 한 줄), optimize_events.parquet(사건 × k 한 줄, 다시 돌릴 때 재사용), optimize.txt(요약).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/optimize.py [--smoke] [--rebuild]
"""
from __future__ import annotations

import argparse
import importlib.util
import itertools
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

_specification = importlib.util.spec_from_file_location("study35_ma_sweep", Path(__file__).resolve().parent / "ma_sweep.py")
sweep = importlib.util.module_from_spec(_specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
sys.modules["study35_ma_sweep"] = sweep
_specification.loader.exec_module(sweep)

base = sweep.base
study33 = sweep.study33
STUDY = base.STUDY
SEED = 20261002
FLIPS = 200
SPLIT_YEAR = 2018
SELECT_MIN = 100
RANK_MIN = 30
NULL_MIN = 100
WAITS = [2, 3, 4, 5, 6, 7]
DEPTHS = [10, 15, 20, 25, 30]
RUNS = [2, 3]
MOVES = [0, 20, 30, 40]
CLOSE_RULES = ["any", "above_d0", "above_ma5"]
REGIMES = ["any", "above_ma60"]
STOPS = [("low1", "low", 1), ("low3", "low", 3), ("low5", "low", 5), ("entry8", "entry", 8), ("entry10", "entry", 10)]
TAKES = [15, 20, 25, 30]
EXITS = [(stop[0], take) for stop in STOPS for take in TAKES]
AXES = ["wait", "depth", "run", "move", "close_rule", "regime", "stop", "take"]
AXIS_VALUES = {"wait": WAITS, "depth": DEPTHS, "run": RUNS, "move": MOVES, "close_rule": CLOSE_RULES, "regime": REGIMES,
               "stop": [stop[0] for stop in STOPS], "take": TAKES}
CATEGORICAL = {"close_rule", "regime"}
BASELINE = {"wait": 5, "depth": 20, "run": 2, "move": 0, "close_rule": "any", "regime": "any", "stop": "low1", "take": 20}
INDEX_PATH = STUDY / "index_snapshot.parquet"
EVENTS_PATH = STUDY / "optimize_events.parquet"
CELLS_PATH = STUDY / "optimize.parquet"
TEXT_PATH = STUDY / "optimize.txt"


def exit_column(stop: str, take: int, field: str) -> str:
    return f"{field}_{stop}_t{take}"


def load_index() -> pd.DataFrame:
    """KS11·KQ11 종가와 60일선 위 여부. 날짜 int 색인."""
    if not INDEX_PATH.exists():
        import FinanceDataReader as reader

        frames = [reader.DataReader(symbol, "2009-01-01", str(base.LAST_DATE))["Close"].rename(symbol) for symbol in ("KS11", "KQ11")]
        snapshot = pd.concat(frames, axis=1).dropna()
        snapshot.index = snapshot.index.strftime("%Y%m%d").astype(np.int64)
        snapshot.index.name = "date"
        snapshot.to_parquet(INDEX_PATH)

    snapshot = pd.read_parquet(INDEX_PATH)

    for symbol in ("KS11", "KQ11"):
        average = snapshot[symbol].rolling(60).mean()
        snapshot[f"{symbol}_above"] = np.where(average.notna(), (snapshot[symbol] > average).astype(float), np.nan)

    return snapshot


def path_exits(series, row: int, entry: float, lowest: float, last: int) -> dict:
    """row 다음 날부터 last 까지 20개 청산 칸. 순수익 %·종류(1 손절, 2 익절, 0 기한)·보유일. 규칙은 ma_sweep.outcome 과 같다."""
    opens = series.open_price[row + 1:last + 1]
    lows = series.low[row + 1:last + 1]
    highs = series.high[row + 1:last + 1]
    result = {}

    for name, kind, percent in STOPS:
        stop_price = min(lowest * (1 - percent / 100), entry * 0.999) if kind == "low" else entry * (1 - percent / 100)
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
                price, code, held = series.close[last], 0, last - row

            result[exit_column(name, take, "ret")] = (price / entry * sweep.COST - 1) * 100
            result[exit_column(name, take, "kind")] = code
            result[exit_column(name, take, "hold")] = int(held)

    return result


def build(waits: list[int]) -> pd.DataFrame:
    """사건 × k 한 줄. 모든 칸이 묶음 2일 이상·되돌림 < 30% 안이라 그 밖은 적지 않는다."""
    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    index = load_index()
    above = {"KS11": index["KS11_above"].to_dict(), "KQ11": index["KQ11_above"].to_dict()}
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
    limit = max(DEPTHS)

    for series in stocks:
        rows = len(series.dates)

        if rows < 30:
            continue

        close = series.close
        volume = series.volume.astype(float)
        raw_close = close * series.factor
        average5 = base.moving(close, 5)
        change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

        for surge_row in range(1, rows - 1):
            if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
                continue

            run_start = surge_row

            while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
                run_start -= 1

            run = surge_row - run_start + 1

            if run < min(RUNS):
                continue

            share = max(relative.get((series.code, int(series.dates[row])), 0.0) for row in range(run_start, surge_row + 1))

            if share < base.SHARE_MIN or raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or volume[surge_row] <= 0:
                continue

            move = close[surge_row] - close[run_start - 1]

            if move <= 0:
                continue

            for wait in waits:
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

                lowest = float(series.low[surge_row + 1:row + 1].min())
                depth = (close[surge_row] - lowest) / move * 100

                if depth >= limit:
                    continue

                entry = float(close[row])
                date = int(series.dates[row])
                symbol = "KS11" if bool(series.is_kospi[row]) else "KQ11"
                record = {"code": series.code, "surge": int(series.dates[surge_row]), "entry_date": date,
                          "year": int(series.dates[surge_row]) // 10000, "run": run, "wait": wait, "share": share,
                          "depth": depth, "move_pct": move / close[run_start - 1] * 100,
                          "above_d0": bool(close[row] >= close[surge_row]),
                          "above_ma5": bool(not math.isnan(average5[row]) and close[row] >= average5[row]),
                          "regime": above[symbol].get(date, np.nan)}
                record.update(path_exits(series, row, entry, lowest, last))
                records.append(record)

    return pd.DataFrame(records)


def t_of(total: np.ndarray, square: np.ndarray, count: np.ndarray) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        mean = total / count
        variance = (square - count * mean ** 2) / (count - 1)
        return mean / np.sqrt(np.maximum(variance, 0) / count)


def filter_masks(frame: pd.DataFrame) -> tuple[list[tuple], np.ndarray]:
    """d × run × move × close_rule × regime 240개 조건 마스크(행 = 조건)."""
    keys, masks = [], []
    regime = frame["regime"].to_numpy()

    for depth, run, move, close_rule, regime_rule in itertools.product(DEPTHS, RUNS, MOVES, CLOSE_RULES, REGIMES):
        mask = (frame["depth"].to_numpy() < depth) & (frame["run"].to_numpy() >= run) & (frame["move_pct"].to_numpy() >= move)

        if close_rule != "any":
            mask &= frame[close_rule].to_numpy()

        if regime_rule != "any":
            mask &= regime == 1.0

        keys.append((depth, run, move, close_rule, regime_rule))
        masks.append(mask)

    return keys, np.array(masks, dtype=float)


def cell_table(events: pd.DataFrame) -> pd.DataFrame:
    rows = []
    periods = {"all": np.ones(len(events), bool), "early": events["year"].to_numpy() < SPLIT_YEAR,
               "late": events["year"].to_numpy() >= SPLIT_YEAR}

    for wait in WAITS:
        frame = events[events["wait"] == wait]
        keys, masks = filter_masks(frame)
        values = frame[[exit_column(stop, take, "ret") for stop, take in EXITS]].to_numpy(float)
        period_statistics = {}

        for period, selector in periods.items():
            weighted = masks * selector[events["wait"].to_numpy() == wait]
            count = weighted.sum(1)[:, None].repeat(len(EXITS), 1)
            total = weighted @ values
            square = weighted @ values ** 2
            period_statistics[period] = (count, total / np.where(count > 0, count, np.nan), t_of(total, square, count))

        for key_index, key in enumerate(keys):
            for exit_index, (stop, take) in enumerate(EXITS):
                row = dict(zip(AXES, (wait, *key, stop, take)))

                for period, (count, mean, t_statistic) in period_statistics.items():
                    row[f"n_{period}"] = int(count[key_index, exit_index])
                    row[f"mean_{period}"] = mean[key_index, exit_index]
                    row[f"t_{period}"] = t_statistic[key_index, exit_index]

                rows.append(row)

    return pd.DataFrame(rows)


def null_max_t(events: pd.DataFrame) -> np.ndarray:
    """사건마다 부호를 뒤집어 전 칸(전체 건수 ≥ NULL_MIN) t 최대값을 FLIPS 번."""
    generator = np.random.default_rng(SEED)
    event_keys = pd.factorize(events["code"] + "_" + events["surge"].astype(str))[0]
    signs = generator.choice([-1.0, 1.0], size=(event_keys.max() + 1, FLIPS))
    best = np.full(FLIPS, -np.inf)

    for wait in WAITS:
        selector = events["wait"].to_numpy() == wait
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


def cell_trades(events: pd.DataFrame, cell: dict) -> pd.DataFrame:
    frame = events[(events["wait"] == cell["wait"]) & (events["depth"] < cell["depth"]) & (events["run"] >= cell["run"])
                   & (events["move_pct"] >= cell["move"])]

    if cell["close_rule"] != "any":
        frame = frame[frame[cell["close_rule"]]]

    if cell["regime"] != "any":
        frame = frame[frame["regime"] == 1.0]

    return pd.DataFrame({"entry_date": frame["entry_date"], "year": frame["year"],
                         "ret": frame[exit_column(cell["stop"], cell["take"], "ret")],
                         "kind": frame[exit_column(cell["stop"], cell["take"], "kind")],
                         "hold": frame[exit_column(cell["stop"], cell["take"], "hold")]}).sort_values("entry_date", kind="mergesort")


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


def describe(events: pd.DataFrame, cells: pd.DataFrame, lookup: dict, cell: dict) -> dict:
    trades = cell_trades(events, cell)
    values = trades["ret"].to_numpy(float)
    early, late = trades[trades["year"] < SPLIT_YEAR]["ret"], trades[trades["year"] >= SPLIT_YEAR]["ret"]
    yearly = trades.groupby("year")["ret"].mean()
    near = [lookup.get(tuple(other[axis] for axis in AXES)) for other in neighbors(cell)]
    near = [value for value in near if value is not None and not math.isnan(value)]
    same = sum(1 for value in near if np.sign(value) == np.sign(values.mean()))
    streak = longest = 0

    for kind in trades["kind"]:
        streak = streak + 1 if kind == 1 else 0
        longest = max(longest, streak)

    t_statistic = lambda series: series.mean() / (series.std() / math.sqrt(len(series))) if len(series) > 1 else float("nan")
    return {**cell, "n": len(values), "mean": values.mean(), "t": t_statistic(pd.Series(values)),
            "win": (values > 0).mean() * 100, "avg_win": values[values > 0].mean(), "avg_loss": values[values <= 0].mean(),
            "hold_median": float(trades["hold"].median()),
            "n_early": len(early), "mean_early": early.mean(), "t_early": t_statistic(early),
            "n_late": len(late), "mean_late": late.mean(), "t_late": t_statistic(late),
            "plus_years": int((yearly > 0).sum()), "years": int(len(yearly)),
            "neighbor_same": same, "neighbor_count": len(near), "neighbor_min": min(near) if near else float("nan"),
            "mean_without_top10": np.sort(values)[:-10].mean() if len(values) > 10 else float("nan"),
            "max_stop_streak": longest, "stop_share": (trades["kind"] == 1).mean() * 100}


def cell_name(cell: dict) -> str:
    rule = {"any": "-", "above_d0": "≥D0", "above_ma5": "≥MA5"}[cell["close_rule"]]
    regime = {"any": "-", "above_ma60": "지수>60선"}[cell["regime"]]
    return f"k{cell['wait']} d{cell['depth']} 묶음{cell['run']}+ 상승{cell['move']}+ 종가{rule} 국면{regime} {cell['stop']}/+{cell['take']}"


def format_row(label: str, row: dict) -> str:
    return (f"{label:4s} {cell_name(row):58s} n{row['n']:4d} {row['mean']:+6.2f} t{row['t']:+5.2f} 승{row['win']:4.1f}% "
            f"이익{row['avg_win']:+6.2f} 손실{row['avg_loss']:+6.2f} 보유{row['hold_median']:5.1f}일 | "
            f"10–17 {row['mean_early']:+6.2f} t{row['t_early']:+5.2f} n{row['n_early']:3d} | 18–26 {row['mean_late']:+6.2f} t{row['t_late']:+5.2f} n{row['n_late']:3d} | "
            f"플러스해 {row['plus_years']}/{row['years']} | 이웃 같은부호 {row['neighbor_same']}/{row['neighbor_count']} (최저 {row['neighbor_min']:+.2f}) | "
            f"상위10 뺌 {row['mean_without_top10']:+6.2f} | 최대연속손절 {row['max_stop_streak']}")


def verdict(row: dict, cross_pass: bool) -> tuple[bool, list[str]]:
    reasons = []

    if not row["t"] >= 3:
        reasons.append(f"전체 t {row['t']:.2f} < 3")

    if row["neighbor_same"] < row["neighbor_count"]:
        reasons.append(f"이웃 {row['neighbor_count'] - row['neighbor_same']}칸 부호 다름")

    if not (row["mean_early"] >= 0 and row["mean_late"] >= 0):
        reasons.append("한 구간 음수")

    if not cross_pass:
        reasons.append("교차검증 두 방향 중 실패 있음")

    return not reasons, reasons


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=STUDY, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"


def smoke() -> int:
    events = build([5])
    trades = cell_trades(events, BASELINE)
    values = trades["ret"]
    early, late = values[trades["year"] < SPLIT_YEAR], values[trades["year"] >= SPLIT_YEAR]
    t_statistic = lambda series: series.mean() / (series.std() / math.sqrt(len(series)))
    print(f"B5_20 재현: n{len(values)} {values.mean():+.2f}% t{t_statistic(values):+.2f} | 2010–17 {early.mean():+.2f} t{t_statistic(early):+.2f} n{len(early)}"
          f" | 2018–26 {late.mean():+.2f} t{t_statistic(late):+.2f} n{len(late)}  (기대: n350 +2.33 t+3.0 | +1.43 t+1.0 | +2.67 t+2.9)")
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", action="store_true", help="B5_20 한 칸만 재현해 본다")
    parser.add_argument("--rebuild", action="store_true", help="optimize_events.parquet 를 다시 만든다")
    arguments = parser.parse_args()

    if arguments.smoke:
        return smoke()

    if arguments.rebuild or not EVENTS_PATH.exists():
        build(WAITS).to_parquet(EVENTS_PATH, index=False)

    events = pd.read_parquet(EVENTS_PATH)
    cells = cell_table(events)
    cells.to_parquet(CELLS_PATH, index=False)
    lookup = {tuple(row[axis] for axis in AXES): row["mean_all"] for row in cells.to_dict("records")}
    null = null_max_t(events)
    unique = cells[cells["n_all"] > 0].drop_duplicates(["n_all", "mean_all", "t_all"])
    observed = float(cells.loc[cells["n_all"] >= NULL_MIN, "t_all"].max())
    lines = [f"스터디 35 optimize — 커밋 {git_commit()}, 자료 끝 {base.LAST_DATE}, 지수 스냅샷 {INDEX_PATH.name}, seed {SEED}",
             f"사건 × k {len(events):,}줄(묶음 2일+·되돌림 < 30%), 사건 {events.groupby(['code', 'surge']).ngroups:,}개",
             f"탐색 칸 {len(cells):,}개(결과가 다른 칸 {len(unique):,}개), 전체 건수 ≥ {NULL_MIN} 칸 {int((cells['n_all'] >= NULL_MIN).sum()):,}개",
             f"실제 최대 t(전체 건수 ≥ {NULL_MIN}): {observed:.3f}",
             f"무작위 부호 뒤집기 {FLIPS}번 최대 t: 중앙값 {np.median(null):.2f}, 95% {np.quantile(null, 0.95):.2f}, 99% {np.quantile(null, 0.99):.2f}, "
             f"최대 {null.max():.3f} — 실제 최대 t 이상이 나온 비율 {(null >= observed).mean() * 100:.1f}%",
             ""]

    cross = {}

    for label, pick, check in (("(가)", "early", "late"), ("(나)", "late", "early")):
        pool = cells[cells[f"n_{pick}"] >= SELECT_MIN]
        best = pool.loc[pool[f"t_{pick}"].idxmax()]
        passed = bool(best[f"mean_{check}"] > 0 and best[f"t_{check}"] >= 2)
        cross[label] = passed
        lines.append(f"교차검증 {label} {pick} 에서 고른 칸({len(pool):,}칸 중): {cell_name(best.to_dict())} — 고른 구간 {best[f'mean_{pick}']:+.2f} "
                     f"t{best[f't_{pick}']:+.2f} n{best[f'n_{pick}']} → 다른 구간 {best[f'mean_{check}']:+.2f} t{best[f't_{check}']:+.2f} "
                     f"n{best[f'n_{check}']} → {'통과' if passed else '실패'}")

    cross_pass = all(cross.values())
    # 같은 사건·같은 결과인 칸(상승 0%·20% 등)은 상승 기준이 작은 쪽 하나만 남긴다
    ranked = cells[(cells["n_early"] >= RANK_MIN) & (cells["n_late"] >= RANK_MIN)]
    ranked = ranked.drop_duplicates(["wait", "stop", "take", "n_all", "mean_all", "t_all", "n_early", "mean_early"]).copy()
    ranked["rank_early"] = ranked["t_early"].rank(ascending=False, method="min")
    ranked["rank_late"] = ranked["t_late"].rank(ascending=False, method="min")
    cutoff = len(ranked) * 0.05
    both = ranked[(ranked["rank_early"] <= cutoff) & (ranked["rank_late"] <= cutoff)].copy()
    both["rank_sum"] = both["rank_early"] + both["rank_late"]
    top = both.sort_values(["rank_sum", "t_all"], ascending=[True, False]).head(20)
    lines += ["", f"두 구간 모두 상위 5%(구간마다 건수 ≥ {RANK_MIN}·결과 중복 뺀 {len(ranked):,}칸 중 순위 ≤ {cutoff:.0f}): {len(both):,}칸, 순위 합 상위 20칸"]
    details = []

    for order, row in enumerate(top.to_dict("records"), start=1):
        cell = {axis: row[axis] for axis in AXES}
        detail = describe(events, cells, lookup, cell)
        detail["rank_early"], detail["rank_late"] = int(row["rank_early"]), int(row["rank_late"])
        details.append(detail)
        passed, reasons = verdict(detail, cross_pass)
        lines.append(format_row(f"{order:2d}.", detail) + f" | 순위 {detail['rank_early']}+{detail['rank_late']} | "
                     + ("통과" if passed else "불통과: " + ", ".join(reasons)))

    baseline = describe(events, cells, lookup, BASELINE)
    passed, reasons = verdict(baseline, cross_pass)
    lines += ["", "기준선", format_row("B5_20", baseline) + " | " + ("통과" if passed else "불통과: " + ", ".join(reasons))]
    best_all = cells[cells["n_all"] >= NULL_MIN].sort_values("t_all", ascending=False)
    best_all = best_all.drop_duplicates(["wait", "stop", "take", "n_all", "mean_all", "t_all"]).head(10)
    lines += ["", f"참고: 전체 기간 t 상위 10칸(건수 ≥ {NULL_MIN}, 고른 칸이라 무작위 최대 t 와 비교해 읽는다)"]

    for row in best_all.to_dict("records"):
        lines.append(f"  {cell_name(row):58s} n{row['n_all']:4d} {row['mean_all']:+6.2f} t{row['t_all']:+5.2f} | 10–17 {row['mean_early']:+6.2f} "
                     f"t{row['t_early']:+5.2f} n{row['n_early']} | 18–26 {row['mean_late']:+6.2f} t{row['t_late']:+5.2f} n{row['n_late']}")

    text = "\n".join(lines)
    TEXT_PATH.write_text(text + "\n", encoding="utf-8")
    pd.DataFrame(details + [{**baseline, "rank_early": None, "rank_late": None}]).to_parquet(STUDY / "optimize_candidates.parquet", index=False)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
