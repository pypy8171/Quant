"""스터디 37 — 확정 저점·고점으로 본 추세 매매 단독 시험(사용자 합의 2026-10-07).

시험 칸 8개(③ 쌍바닥 × k 2, ④ 상승 추세 이평선 지지 × k 2 × N 3) + 대조군 6개(④ 모양을 하향 추세에서 갖춘 매매).
정의는 swing_definitions.py 그대로 쓴다(미래 정보 금지 — 확정 저점은 k 일 뒤부터, 이평선은 신호일 종가까지).
자료  : PYQuant/data/bars_all_pit_v2.parquet 전 종목 일봉(상장폐지 포함, 가격은 수정주가, 거래량은 무수정)을
        스터디 33 load_inputs 로 읽는다(보통주만, 거래량 0 행 제외, factor = 무수정/수정 — 월말 무수정 종가에서 잼).
        2009-06 부터 읽고 신호는 2010-01-01 부터, 끝은 LAST_DATE. 기간을 자르지 않는다.
거름  : 신호일 무수정 종가 ≥ 2,000원, 20거래일 평균 무수정 거래대금 ≥ 5억(스터디 15 MIN_TURNOVER20·스터디 12 MIN_TURNOVER 와 같은 값).
매수  : 신호 다음 거래일 시가. 그 시가가 손절선 이하면 사지 않는다(내가 정함, 건수는 따로 센다).
청산  : 손절 = 근거 저점 × 0.97 장중 이탈(갭이면 시가), 익절 = 매수가 × 1.15(갭이면 시가), 아니면 매수일 뒤 60거래일째 종가.
        매수일 장중부터 본다. 같은 날 둘 다 닿으면 손절. 60일 전에 시세가 끊긴 종목(상장폐지 등)은 마지막 종가.
        아직 60일이 안 찬 매매(시세가 LAST_DATE 까지 이어짐)는 결과가 없어 뺀다.
가격 단절: 신호일 다음부터 청산일까지 시가·종가가 전날 종가 대비 ±35% 밖인 날이 있으면 그 매매는 빼고(보유 중 재신호 무시는 유지),
        비교 평균에서도 그 구간에 단절이 있는 종목을 뺀다(내가 정함 — 첫 실행에서 052670 2023→2026 +37,366% 같은 수정 누락 확인).
비용  : 스터디 35 ma_sweep.COST(왕복 비율, study33.net_percent = 라이브 수수료·매도세) 그대로. 전 종목 비교에도 같은 비용.
비교  : 신호일에 같은 거름을 지난 전 종목을 매수일 시가에 사서 그 매매의 청산일 종가에 판 평균(청산일에 거래 없는 종목은
        그 전 마지막 종가). 초과 수익 = 매매 순수익 − 그 평균.
t     : 매수 월로 묶은 표준오차(CR1). 차이 t 는 상수 + 차이 × 상승 회귀의 같은 묶음 표준오차.
재실행(저장소 루트): py -X utf8 research/studies/37_swing_trend/standalone_swing.py
산출  : standalone_swing.txt(시험 목록·결과), standalone_swing_trades.parquet(매매 한 건 한 줄), standalone_swing_metrics.json.
"""
from __future__ import annotations

import importlib.util
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
REPOSITORY = STUDY.parents[2]
sys.path.insert(0, str(STUDY))
import swing_definitions as swing  # noqa: E402

_specification = importlib.util.spec_from_file_location("study35_ma_sweep", STUDY.parent / "35_surge_box_breakout" / "ma_sweep.py")
sweep = importlib.util.module_from_spec(_specification)
sys.modules["study35_ma_sweep"] = sweep
_specification.loader.exec_module(sweep)
study33 = sweep.study33
COST = sweep.COST

LAST_DATE = 20261006
SIGNAL_FROM = 20100101
SPLIT_DATE = 20180101
K_VALUES = (3, 5)
WINDOWS = (10, 20, 60)
STOP_BELOW = 0.97
TAKE_PROFIT = 1.15
HOLD_MAX = 60
MIN_CLOSE_KRW = 2_000.0
MIN_TURNOVER20_KRW = 5e8
TURNOVER_DAYS = 20
PRICE_BREAK = 0.35   # 시가·종가가 전날 종가 대비 ±35% 밖 = 가격제한폭(30%) 밖 → 수정 안 된 액면 변경·재상장 등으로 본다(내가 정함)
PASS_T = 2.73
WALK_WARMUP_YEARS = 3
OUTPUT_TEXT = STUDY / "standalone_swing.txt"
OUTPUT_TRADES = STUDY / "standalone_swing_trades.parquet"
OUTPUT_METRICS = STUDY / "standalone_swing_metrics.json"

PLAN = f"""스터디 37 확정 저점·고점 추세 매매 — 단독 시험
=====================================================
[시험 칸 — 실행 전에 고정]
  ③ 쌍바닥 돌파: DB_k3, DB_k5                                   (2칸)
  ④ 상승 추세 이평선 지지: UP_k{{3,5}}_N{{10,20,60}}              (6칸)
  합 8칸. 대조군(판정 칸 아님): DOWN_k{{3,5}}_N{{10,20,60}} — ④ 모양을 하향 추세에서 갖춘 매매 6칸.
[정의 — swing_definitions.py]
  확정 저점 = 저가가 앞뒤 k일 중 최저인 날, k일 뒤 행부터 사용. 고점도 같다. k ∈ {{3,5}}.
  추세 = 마지막 확정 저점 2개·고점 2개. 상승 = 저점↑·고점↑, 하향 = 저점↓, 나머지 횡보.
  쌍바닥 = 확정 저점 2개 차이 3% 이내, 간격 10–60거래일, 사이 고가 최고(목선) ≥ 높은 저점 × 1.10,
           종가가 목선을 처음 넘는 날 신호(그 날이 두 번째 저점 확정 전이면 버림 — 내가 정함).
  이평선 지지 = 추세 상승, 저가 ≤ 이평선 × 1.01, 종가 > 이평선, 이평선 > 전일 이평선. N ∈ {{10,20,60}}.
[고정 조건]
  자료: bars_all_pit_v2.parquet 전 종목(상장폐지 포함) 수정주가 일봉, 신호 {SIGNAL_FROM}–{LAST_DATE}, 자르지 않음.
  거름: 신호일 무수정 종가 ≥ 2,000원, 20일 평균 무수정 거래대금 ≥ 5억(스터디 15·12 값). 보유 중 같은 종목 재신호 무시.
  매수: 다음 날 시가(시가 ≤ 손절선이면 안 삼 — 내가 정함). 손절 근거 저점 × 0.97, 익절 +15%, 최대 60거래일 종가,
        같은 날 둘 다면 손절. 비용 = 스터디 35 ma_sweep.COST = {COST:.6f}(왕복 비율).
  비교: 같은 날 시가에 같은 거름의 전 종목을 사서 같은 청산일 종가에 판 평균. 초과 = 매매 − 비교.
  가격 단절: 보유 구간에 전날 종가 대비 ±35% 밖 시가·종가가 있으면 매매·비교 종목에서 뺌(내가 정함, 첫 실행 뒤 추가 —
        052670 수정 누락 한 건이 평균을 +37,366%로 끌어올림. 칸 선택과 무관한 자료 정리).
[통과 기준 — 내가 정함]
  칸마다 ① 초과 평균 > 0 ② 매수 월 묶음 t ≥ {PASS_T}(0.05/8 양측) ③ 2010–17·2018–26 둘 다 초과 > 0.
  전체: 해마다 그 해 전에 끝난 매매만으로 8칸 중 묶음 t 최고 칸을 골라(2010–12 3년 쌓인 뒤 2013부터) 그 해 매매를 이어붙인 초과 > 0.
  ④ 상승 − 하향 대조군 초과 차이와 묶음 t 를 같이 적는다(판정 보조).
"""


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPOSITORY, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def write_text(path: Path, text: str) -> None:
    with open(path, "wb") as handle:
        handle.write(text.encode("utf-8"))


def cluster_mean_t(values: np.ndarray, clusters: np.ndarray) -> float:
    """평균의 묶음(CR1) t."""
    values = np.asarray(values, float)
    count = len(values)

    if count < 3:
        return float("nan")

    residual = values - values.mean()
    sums = pd.Series(residual).groupby(np.asarray(clusters)).sum().to_numpy()
    groups = len(sums)

    if groups < 2:
        return float("nan")

    error = math.sqrt(groups / (groups - 1) * float((sums ** 2).sum())) / count
    return float(values.mean() / error) if error > 0 else float("nan")


def cluster_difference_t(values: np.ndarray, flag: np.ndarray, clusters: np.ndarray) -> tuple[float, float]:
    """values = 상수 + 차이 × flag. (차이, 묶음 CR1 t)."""
    values = np.asarray(values, float)
    design = np.column_stack([np.ones(len(values)), np.asarray(flag, float)])
    inverse = np.linalg.inv(design.T @ design)
    coefficients = inverse @ design.T @ values
    residuals = values - design @ coefficients
    scores = pd.DataFrame(design * residuals[:, None]).groupby(np.asarray(clusters)).sum().to_numpy()
    groups = len(scores)
    count = len(values)
    covariance = groups / (groups - 1) * (count - 1) / (count - 2) * inverse @ (scores.T @ scores) @ inverse
    error = math.sqrt(covariance[1, 1])
    return float(coefficients[1]), float(coefficients[1] / error) if error > 0 else float("nan")


def simulate_exit(series, rows: int, entry_row: int, stop: float, market_ended: bool) -> dict | None:
    """매수일 장중부터 본다. None = 아직 결과가 없는 매매."""
    entry = float(series.open_price[entry_row])
    take = entry * TAKE_PROFIT
    last = entry_row + HOLD_MAX
    reason = "time"

    if last > rows - 1:
        if not market_ended:
            return None

        last = rows - 1
        reason = "data_end"

    opens = series.open_price[entry_row:last + 1]
    lows = series.low[entry_row:last + 1]
    highs = series.high[entry_row:last + 1]
    stop_hits = np.flatnonzero(lows <= stop)
    take_hits = np.flatnonzero(highs >= take)
    stop_at = int(stop_hits[0]) if len(stop_hits) else 10 ** 9
    take_at = int(take_hits[0]) if len(take_hits) else 10 ** 9

    if stop_at <= take_at and stop_at < 10 ** 9:
        offset, price, reason = stop_at, min(float(opens[stop_at]), stop), "stop"
    elif take_at < 10 ** 9:
        offset, price, reason = take_at, max(float(opens[take_at]), take), "take"
    else:
        offset, price = last - entry_row, float(series.close[last])

    exit_row = entry_row + offset
    return {"entry": entry, "exit_price": price, "exit_row": exit_row, "reason": reason,
            "hold_days": offset, "net": (price / entry * COST - 1.0) * 100.0,
            "mae": (float(lows[:offset + 1].min()) / entry - 1.0) * 100.0,
            "mfe": (float(highs[:offset + 1].max()) / entry - 1.0) * 100.0}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    write_text(OUTPUT_TEXT, PLAN + "\n[결과] 실행 중\n")
    stocks, _, information = study33.load_inputs(LAST_DATE)
    market_dates = np.unique(np.concatenate([series.dates for series in stocks]))
    last_market_date = int(market_dates[-1])
    date_count, stock_count = len(market_dates), len(stocks)
    open_matrix = np.full((date_count, stock_count), np.nan)
    close_matrix = np.full((date_count, stock_count), np.nan)
    eligible_matrix = np.zeros((date_count, stock_count), dtype=bool)
    break_matrix = np.full((date_count, stock_count), np.nan)
    records = []
    skipped = {}

    for column, series in enumerate(stocks):
        rows = len(series.dates)
        positions = np.searchsorted(market_dates, series.dates)
        raw_close = series.close * series.factor
        turnover20 = pd.Series(raw_close * series.volume).rolling(TURNOVER_DAYS).mean().to_numpy()
        eligible = (raw_close >= MIN_CLOSE_KRW) & (turnover20 >= MIN_TURNOVER20_KRW)
        open_matrix[positions, column] = series.open_price
        close_matrix[positions, column] = series.close
        eligible_matrix[positions, column] = eligible
        market_ended = int(series.dates[-1]) < last_market_date
        previous_close = np.r_[np.nan, series.close[:-1]]
        with np.errstate(invalid="ignore", divide="ignore"):
            broken = ((np.abs(series.open_price / previous_close - 1.0) > PRICE_BREAK)
                      | (np.abs(series.close / previous_close - 1.0) > PRICE_BREAK))
        break_count = np.cumsum(broken)
        break_matrix[positions, column] = break_count

        if rows < 2 * max(K_VALUES) + max(WINDOWS) + 2:
            continue

        averages = {window: swing.moving_average(series.close, window) for window in WINDOWS}
        usable = eligible & (series.dates >= SIGNAL_FROM)

        for span in K_VALUES:
            pivots = swing.confirmed_pivots(series.low, series.high, span)
            trend = swing.trend_state(pivots, rows)
            cells = []
            bottoms = swing.double_bottom_signals(series.close, series.high, pivots)
            cells.append((f"DB_k{span}", "double_bottom", span, 0,
                          [(item["signal_row"], item["base_low"]) for item in bottoms]))

            for window in WINDOWS:
                for label, kind, wanted in (("UP", "support_up", swing.TREND_UP), ("DOWN", "support_down", swing.TREND_DOWN)):
                    mask = swing.moving_average_support_mask(series.close, series.low, averages[window], trend["state"], wanted)
                    signal_rows = np.flatnonzero(mask)
                    cells.append((f"{label}_k{span}_N{window}", kind, span, window,
                                  [(int(row), float(trend["latest_low"][row])) for row in signal_rows]))

            for cell, kind, span_value, window, signals in cells:
                next_allowed = 0

                for signal_row, base_low in sorted(signals):
                    if signal_row < next_allowed or not usable[signal_row] or not np.isfinite(base_low):
                        continue

                    entry_row = signal_row + 1

                    if entry_row >= rows:
                        skipped[(cell, "no_entry_yet")] = skipped.get((cell, "no_entry_yet"), 0) + 1
                        continue

                    stop = base_low * STOP_BELOW

                    if series.open_price[entry_row] <= stop:
                        skipped[(cell, "open_below_stop")] = skipped.get((cell, "open_below_stop"), 0) + 1
                        continue

                    result = simulate_exit(series, rows, entry_row, stop, market_ended)

                    if result is None:
                        skipped[(cell, "unresolved")] = skipped.get((cell, "unresolved"), 0) + 1
                        break

                    next_allowed = result["exit_row"]

                    if break_count[result["exit_row"]] - break_count[signal_row] > 0:
                        skipped[(cell, "price_break")] = skipped.get((cell, "price_break"), 0) + 1
                        continue

                    records.append({"code": series.code, "cell": cell, "kind": kind, "pivot_span": span_value, "window": window,
                                    "signal_date": int(series.dates[signal_row]), "entry_date": int(series.dates[entry_row]),
                                    "exit_date": int(series.dates[result["exit_row"]]), "stop": stop,
                                    **{key: value for key, value in result.items() if key != "exit_row"}})

    close_matrix = pd.DataFrame(close_matrix).ffill().to_numpy()
    break_matrix = pd.DataFrame(break_matrix).ffill().fillna(0.0).to_numpy()
    trades = pd.DataFrame(records)
    signal_index = np.searchsorted(market_dates, trades["signal_date"].to_numpy())
    entry_index = np.searchsorted(market_dates, trades["entry_date"].to_numpy())
    exit_index = np.searchsorted(market_dates, trades["exit_date"].to_numpy())
    benchmark_cache = {}
    benchmark = np.empty(len(trades))

    for position, key in enumerate(zip(signal_index, entry_index, exit_index)):
        if key not in benchmark_cache:
            signal_at, entry_at, exit_at = key
            ratio = close_matrix[exit_at] / open_matrix[entry_at]
            chosen = (eligible_matrix[signal_at] & np.isfinite(ratio) & (open_matrix[entry_at] > 0)
                      & (break_matrix[exit_at] == break_matrix[signal_at]))
            benchmark_cache[key] = float((ratio[chosen] * COST - 1.0).mean() * 100.0) if chosen.any() else float("nan")

        benchmark[position] = benchmark_cache[key]

    trades["benchmark"] = benchmark
    trades["excess"] = trades["net"] - trades["benchmark"]
    trades["month"] = trades["entry_date"] // 100
    trades["year"] = trades["entry_date"] // 10000
    trades = trades[np.isfinite(trades["excess"])].sort_values(["cell", "entry_date", "code"], kind="mergesort").reset_index(drop=True)
    trades.to_parquet(OUTPUT_TRADES, index=False)

    # ── 칸별 표 ──
    test_cells = [f"DB_k{span}" for span in K_VALUES] + [f"UP_k{span}_N{window}" for span in K_VALUES for window in WINDOWS]
    control_cells = [f"DOWN_k{span}_N{window}" for span in K_VALUES for window in WINDOWS]
    metrics = {}

    def summarize(cell: str) -> dict:
        part = trades[trades["cell"] == cell]
        early = part[part["entry_date"] < SPLIT_DATE]
        late = part[part["entry_date"] >= SPLIT_DATE]
        winners = part[part["net"] > 0]
        summary = {"n": int(len(part)), "net": float(part["net"].mean()), "benchmark": float(part["benchmark"].mean()),
                   "excess": float(part["excess"].mean()),
                   "t": cluster_mean_t(part["excess"].to_numpy(), part["month"].to_numpy()),
                   "excess_2010_17": float(early["excess"].mean()) if len(early) else float("nan"), "n_2010_17": int(len(early)),
                   "excess_2018_26": float(late["excess"].mean()) if len(late) else float("nan"), "n_2018_26": int(len(late)),
                   "win_rate": float((part["net"] > 0).mean() * 100.0), "hold_days": float(part["hold_days"].mean()),
                   "stop_share": float((part["reason"] == "stop").mean() * 100.0),
                   "take_share": float((part["reason"] == "take").mean() * 100.0),
                   "winner_mae_median": float(winners["mae"].median()) if len(winners) else float("nan"),
                   "winner_mae_p10": float(winners["mae"].quantile(0.1)) if len(winners) else float("nan"),
                   "mfe_median": float(part["mfe"].median()), "mfe_p90": float(part["mfe"].quantile(0.9)),
                   "yearly_excess": {int(year): round(float(group["excess"].mean()), 3) for year, group in part.groupby("year")}}
        summary["pass"] = bool(summary["excess"] > 0 and summary["t"] >= PASS_T
                               and summary["excess_2010_17"] > 0 and summary["excess_2018_26"] > 0)
        return summary

    for cell in test_cells + control_cells:
        metrics[cell] = summarize(cell)

    # ── 해마다 그 전 자료로 고르기 ──
    years = sorted(trades["year"].unique())
    first_year = int(min(years))
    walk_parts = []
    walk_choices = []

    for year in range(first_year + WALK_WARMUP_YEARS, int(max(years)) + 1):
        known = trades[(trades["exit_date"] < year * 10000 + 101) & trades["cell"].isin(test_cells)]
        scores = {cell: cluster_mean_t(group["excess"].to_numpy(), group["month"].to_numpy())
                  for cell, group in known.groupby("cell")}
        scores = {cell: value for cell, value in scores.items() if np.isfinite(value)}

        if not scores:
            continue

        chosen = max(sorted(scores), key=lambda cell: scores[cell])
        part = trades[(trades["cell"] == chosen) & (trades["year"] == year)]
        walk_parts.append(part)
        walk_choices.append({"year": year, "cell": chosen, "past_t": round(scores[chosen], 2), "n": int(len(part)),
                             "excess": round(float(part["excess"].mean()), 3) if len(part) else None})

    walk = pd.concat(walk_parts, ignore_index=True)
    walk_summary = {"n": int(len(walk)), "excess": float(walk["excess"].mean()),
                    "t": cluster_mean_t(walk["excess"].to_numpy(), walk["month"].to_numpy()), "choices": walk_choices}

    # ── 상승 − 하향 대조군 ──
    differences = {}

    for span in K_VALUES:
        for window in WINDOWS:
            up = trades[trades["cell"] == f"UP_k{span}_N{window}"]
            down = trades[trades["cell"] == f"DOWN_k{span}_N{window}"]
            joined = pd.concat([up, down], ignore_index=True)
            flag = np.r_[np.ones(len(up)), np.zeros(len(down))]
            difference, t_value = cluster_difference_t(joined["excess"].to_numpy(), flag, joined["month"].to_numpy())
            differences[f"k{span}_N{window}"] = {"difference": difference, "t": t_value}

    # ── 글 ──
    lines = [PLAN, "[결과]"]
    header = (f"  {'칸':14s} {'n':>6s} {'순수익':>7s} {'비교':>7s} {'초과':>7s} {'묶음t':>6s} {'10-17초과':>9s} {'18-26초과':>9s}"
              f" {'승률':>6s} {'보유일':>6s} {'손절%':>6s} {'익절%':>6s} 판정")

    def row_text(cell: str) -> str:
        summary = metrics[cell]
        verdict = "통과" if summary["pass"] else "기각"
        return (f"  {cell:14s} {summary['n']:6d} {summary['net']:+7.2f} {summary['benchmark']:+7.2f} {summary['excess']:+7.2f}"
                f" {summary['t']:+6.2f} {summary['excess_2010_17']:+9.2f} {summary['excess_2018_26']:+9.2f}"
                f" {summary['win_rate']:6.1f} {summary['hold_days']:6.1f} {summary['stop_share']:6.1f} {summary['take_share']:6.1f}"
                + ("" if cell.startswith("DOWN") else f" {verdict}"))

    lines.append("  시험 칸(%는 매매당 평균, 비용 포함)")
    lines.append(header)
    lines += [row_text(cell) for cell in test_cells]
    lines.append("  대조군(하향 추세에서 같은 이평선 지지 모양)")
    lines.append(header)
    lines += [row_text(cell) for cell in control_cells]
    lines.append("")
    lines.append("  ④ 상승 − 하향 초과 차이")

    for key, value in differences.items():
        lines.append(f"    {key:8s} 차이 {value['difference']:+6.2f}%p 묶음 t {value['t']:+6.2f}")

    lines.append("")
    lines.append(f"  해마다 그 전 자료로 고르기(2013–): n {walk_summary['n']} 초과 {walk_summary['excess']:+.2f}% 묶음 t {walk_summary['t']:+.2f}")

    for choice in walk_choices:
        excess_text = f"{choice['excess']:+.2f}" if choice["excess"] is not None else "  없음"
        lines.append(f"    {choice['year']} {choice['cell']:12s} (지난 t {choice['past_t']:+.2f}) n {choice['n']:5d} 초과 {excess_text}")

    lines.append("")
    lines.append("  해별 초과 평균(안정성 확인용, 판정에 안 씀)")
    all_years = list(range(first_year, int(max(years)) + 1))
    lines.append("    " + f"{'칸':12s}" + "".join(f"{year:>7d}" for year in all_years))

    for cell in test_cells + control_cells:
        yearly = metrics[cell]["yearly_excess"]
        lines.append("    " + f"{cell:12s}" + "".join(f"{yearly[year]:+7.2f}" if year in yearly else f"{'':>7s}" for year in all_years))

    lines.append("")
    lines.append("  가격 분포(이긴 매매의 장중 최대 손실 MAE 중앙·하위 10%, 전체 최대 이익 MFE 중앙·상위 10%)")

    for cell in test_cells + control_cells:
        summary = metrics[cell]
        lines.append(f"    {cell:12s} 이긴 MAE {summary['winner_mae_median']:+6.2f} / {summary['winner_mae_p10']:+6.2f}"
                     f"  MFE {summary['mfe_median']:+6.2f} / {summary['mfe_p90']:+6.2f}")

    lines.append("")
    lines.append("  빠진 신호: " + ", ".join(f"{cell} {reason} {count}" for (cell, reason), count in sorted(skipped.items())))
    bars_path = study33.BARS_PATH
    reproduction = {"commit": git_commit(), "bars_path": str(bars_path.relative_to(REPOSITORY)),
                    "bars_size": bars_path.stat().st_size, "bars_rows_loaded": information["bars_rows"],
                    "common_codes": information["common_codes"], "last_market_date": last_market_date,
                    "cost_ratio": COST, "random": "없음(결정론)"}
    lines.append("  재현: " + json.dumps(reproduction, ensure_ascii=False))
    lines.append("  재실행(저장소 루트): py -X utf8 research/studies/37_swing_trend/standalone_swing.py")
    text = "\n".join(lines) + "\n"
    write_text(OUTPUT_TEXT, text)
    payload = {"cells": metrics, "differences": differences, "walk_forward": walk_summary, "skipped":
               {f"{cell}|{reason}": count for (cell, reason), count in sorted(skipped.items())}, "reproduction": reproduction}
    write_text(OUTPUT_METRICS, json.dumps(payload, ensure_ascii=False, indent=1, default=float) + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
