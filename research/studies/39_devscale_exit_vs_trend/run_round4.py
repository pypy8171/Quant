"""스터디 39 round4 — 추세 청산(E3·E4j)을 진입 다음 날부터 판정하고, 코스닥 종목은 코스닥 지수와 비교한다.

바꾼 것(그 밖은 round3 그대로 — run_round3.py 의 함수를 부른다):
  1. E3 k=2–30·E4j 1–10 은 진입 다음 날 종가부터 본다. 진입 당일엔 손절만 보는데 이 칸들은 손절이 없어 당일 매도 없음.
  2. 비교 지수: 코스피 종목은 코스피, 코스닥 종목은 코스닥(네이버 siseJson, fetch_kosdaq_daily.py). 63 칸 전부·무작위 대조군도.
  3. 슬롯 판은 코스닥 보유 연환산도 넘어야 통과.
사전 등록: results/round4/choices_log.csv(실행 전 15 행).
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round4.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))

import run_round3 as round3     # noqa: E402  스터디 38 경로·numba 설정을 먼저 넣는다
import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402

round2, exit_test = round3.round2, round3.exit_test
OUTPUT = STUDY / "results" / "round4"
ROUND3 = STUDY / "results" / "round3"
KOSDAQ = STUDY / "data" / "kosdaq_daily_naver.parquet"
BARS = exit_test.REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
PULLBACK, ENTRY_UPPER, HOLD_WIDEN = exit_test.PULLBACK, exit_test.ENTRY_UPPER, exit_test.HOLD_WIDEN
TREND_KINDS = (exit_test.KIND_E3, exit_test.KIND_E4)


# ── 청산 커널(round3 커널에서 E3·E4j 만 진입 다음 날부터) ─────────────────────

@njit(cache=True)
def exit_rule_next_day(kind, tp, sl, parameter, entry, end, approach, fill, open_price, high, low, close, before19, aligned_close,
                       aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier):
    tp_level = fill * (1.0 + tp / 100.0) if tp == tp else np.inf
    sl_level = fill * (1.0 - sl / 100.0) if sl == sl else -np.inf
    lower_band = (1.0 - PULLBACK - HOLD_WIDEN)
    upper_band = (1.0 + ENTRY_UPPER + HOLD_WIDEN)
    run = 0

    for row in range(entry, end):
        if kind <= 1:
            lower = sl_level
            upper = tp_level
            lower_reason = 2
            upper_reason = 1

            if kind == 1:
                zone_low = lower_band * before19[row] / (20.0 - lower_band)
                zone_high = upper_band * before19[row] / (20.0 - upper_band)

                if zone_low > lower:
                    lower = zone_low
                    lower_reason = 3

                if zone_high < upper:
                    upper = zone_high
                    upper_reason = 5

            if row > entry:
                if kind == 1 and not aligned_previous[row] and not folded_open[row]:
                    return row, open_price[row], 14

                if open_price[row] <= lower:
                    return row, open_price[row], 10 + lower_reason

                if open_price[row] >= upper:
                    return row, open_price[row], 10 + upper_reason

                if low[row] <= lower:
                    return row, min(lower, open_price[row]), lower_reason

                if high[row] >= upper:
                    return row, max(upper, open_price[row]), upper_reason
            else:
                low_after = close[row] if approach == 1 else low[row]
                high_after = close[row] if approach == 2 else high[row]

                if low_after <= lower:
                    return row, min(lower, fill), lower_reason

                if high_after >= upper:
                    return row, max(upper, fill), upper_reason

            if kind == 1 and not aligned_previous[row] and not aligned_close[row]:
                return row, close[row], 4
        elif kind == 2:
            if row > entry and states[parameter, row] == -1:
                return row, close[row], 0
        elif kind == 3:
            if row > entry:
                if close[row] < sma20[row]:
                    run += 1
                else:
                    run = 0

                if run >= parameter:
                    return row, close[row], 0
        else:
            if position[row] >= position[entry] + parameter:
                return row, close[row], 0

    last = end - 1

    if listed:
        return last, close[last], 8

    return last, close[last] * delist_multiplier, 9


@njit(parallel=True, cache=True)
def run_exits_next_day(entries, approach, fills, ends, listed, delist_multiplier, kinds, tps, sls, parameters, open_price, high, low,
                       close, before19, aligned_close, aligned_previous, folded_open, sma20, states, position):
    count = entries.shape[0]
    rules = kinds.shape[0]
    exit_row = np.empty((count, rules), dtype=np.int64)
    exit_price = np.empty((count, rules))
    reason = np.empty((count, rules), dtype=np.int8)

    for index in prange(count):
        for rule in range(rules):
            row, price, code = exit_rule_next_day(kinds[rule], tps[rule], sls[rule], parameters[rule], entries[index], ends[index],
                                                  approach[index], fills[index], open_price, high, low, close, before19, aligned_close,
                                                  aligned_previous, folded_open, sma20, states, position, listed[index],
                                                  delist_multiplier[index])
            exit_row[index, rule] = row
            exit_price[index, rule] = price
            reason[index, rule] = code

    return exit_row, exit_price, reason


# ── 지수 ────────────────────────────────────────────────────────────────────

def index_daily(path: Path, market_dates: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """시장일에 맞춘 (시가, 종가, 빠진 날 수). 종가는 앞 값, 시가는 전날 종가로 채운다(round3 과 같음)."""
    frame = pd.read_parquet(path).set_index("date").reindex(market_dates)
    close = frame["close"].ffill().to_numpy()
    open_price = frame["open"].fillna(pd.Series(close, index=frame.index).shift(1)).to_numpy()
    return open_price, close, int(frame["close"].isna().sum())


def stock_markets(codes: list[str]) -> np.ndarray:
    """종목마다 0 코스피·1 코스닥(KOSDAQ GLOBAL 포함)."""
    frame = pd.read_parquet(BARS, columns=["code", "market"]).drop_duplicates("code").set_index("code")["market"]
    market = frame.reindex(codes)

    if market.isna().any():
        raise SystemExit(f"시장 구분 없는 종목 {int(market.isna().sum())}")

    return market.str.startswith("KOSDAQ").to_numpy().astype(np.int64)


def renamed(frame: pd.DataFrame) -> pd.DataFrame:
    return frame.rename(columns=lambda name: name if "kospi_hold" in name else name.replace("kospi_same_weight", "index_same_weight")
                        .replace("control_kospi", "control_index").replace("kospi", "index"))


def with_kosdaq_hold(table: pd.DataFrame, kosdaq_annual: float, kosdaq_mdd: float) -> pd.DataFrame:
    table = table.copy()
    table["kosdaq_hold_annual_pct"] = kosdaq_annual
    table["kosdaq_hold_mdd_pct"] = kosdaq_mdd
    table["passes"] = table["passes"] & (table["slot_annual_pct"] > kosdaq_annual)
    table["status"] = np.where(~table["slot_judgeable"], "판정 불가", np.where(table["passes"], "통과", "불통과"))
    return table


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = exit_test.build_rules()
    data = exit_test.Data()
    panel = data.panel
    exit_test.MONTHS = panel.month_count
    engine = round2.Engine(data)
    flags = round2.flags_of(data, engine)
    live_cell = int(rules.loc[rules["rule"] == exit_test.LIVE_RULE, "cell"].iloc[0])
    kospi_open, kospi_close, kospi_missing = index_daily(exit_test.KOSPI, panel.market_dates)
    kosdaq_open, kosdaq_close, kosdaq_missing = index_daily(KOSDAQ, panel.market_dates)
    index_open, index_close = np.vstack([kospi_open, kosdaq_open]), np.vstack([kospi_close, kosdaq_close])
    market_of_stock = stock_markets(panel.codes)
    first_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_FIRST))
    last_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_LAST, side="right")) - 1
    years = ((pd.Timestamp(str(exit_test.SIGNAL_LAST)) - pd.Timestamp(str(exit_test.SIGNAL_FIRST))).days) / 365.25
    kospi_curve = round3.CAPITAL * kospi_close[first_day:last_day + 1] / kospi_open[first_day]
    kosdaq_curve = round3.CAPITAL * kosdaq_close[first_day:last_day + 1] / kosdaq_open[first_day]
    kosdaq_annual, kosdaq_mdd = round3.curve_statistics(kosdaq_curve, years)
    market_years = (panel.market_dates // 10000).astype(np.int64)
    year_of_day = np.clip(market_years - panel.first_year, 0, None)
    month_of_day = np.clip(year_of_day * 12 + (panel.market_dates // 100) % 100 - 1, 0, None).astype(np.int64)
    month_count, year_count = int(month_of_day.max()) + 1, int(year_of_day.max()) + 1
    generator = np.random.default_rng(common.SEED)
    signs_month = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    signs_year = np.where(generator.random((round3_test.FLIPS, year_count)) < 0.5, -1.0, 1.0)
    groups = (month_of_day, year_of_day, month_count, year_count)
    print(f"준비 {time.time() - started:.0f}초 · 빠진 날 코스피 {kospi_missing}·코스닥 {kosdaq_missing} · 코스닥 종목 {int(market_of_stock.sum())}"
          f"/{len(market_of_stock)}", flush=True)
    view = round3.OpenEntry(data, engine)
    first_rows, all_bars, sizes, hashed = [], {}, {}, {}
    trend = rules["kind"].isin(TREND_KINDS).to_numpy()

    for board, extra in round3.BOARDS:
        mask = np.ones(panel.row_count, dtype=bool)

        for name in round2.BASE_FLAGS + extra:
            mask &= flags[name]

        candidates = np.flatnonzero(mask).astype(np.int64)
        open_price = data.open[candidates]
        approach = np.where(open_price > engine.upper[candidates], 2, np.where(open_price < engine.lower[candidates], 1, 0)).astype(np.int64)
        strategy = round3.evaluate(data, engine, rules, candidates, approach, runner=run_exits_next_day)
        control_rows, no_pool = round3.random_entries(data, flags, extra, candidates)
        control = round3.evaluate(data, view, rules, control_rows, np.zeros(len(control_rows), dtype=np.int64), runner=run_exits_next_day)
        strategy["index_id"] = market_of_stock[strategy["stock"]]
        control["index_id"] = market_of_stock[control["stock"]]
        live_keep = strategy["keep"][:, live_cell]
        sizes[board] = {"signals": int(len(candidates)), "control_rows": int(len(control_rows)), "signals_without_pool": no_pool,
                        "a_trades": int(live_keep.sum()), "a_kosdaq_share": float(strategy["index_id"][live_keep].mean()),
                        "control_kosdaq_share": float(control["index_id"].mean())}
        print(f"{board}: 신호 {len(candidates)} · 대조 {len(control_rows)} ({time.time() - started:.0f}초)", flush=True)
        suffix = "" if board == "U2" else "_u0"
        slot_frames = []

        for label, multiplier in round3.VALUATIONS:
            table, year_table, bars, equity, daily = round3.board_tables(data, rules, strategy, control, multiplier, index_open, index_close,
                                                                     groups, (signs_month, signs_year), first_day, last_day, years,
                                                                     kospi_curve)
            table = with_kosdaq_hold(table, kosdaq_annual, kosdaq_mdd)
            all_bars[f"{board}·{label}"] = {("index" if key == "kospi" else "control_index" if key == "control_kospi" else key): value
                                            for key, value in bars.items()}
            row = round3.first_row(board, label, table, live_cell)
            row.update({"kosdaq_hold_annual_pct": kosdaq_annual, "kosdaq_hold_mdd_pct": kosdaq_mdd})
            first_rows.append(row)
            slot_frames.append(table[["cell", "rule", "slot_trades", "slot_judgeable", "slot_annual_pct", "slot_mdd_pct", "slot_end_krw",
                                      "slot_below_min_order_date", "control_slot_trades", "control_slot_annual_pct", "control_slot_mdd_pct",
                                      "kospi_hold_annual_pct", "kospi_hold_mdd_pct", "kosdaq_hold_annual_pct", "kosdaq_hold_mdd_pct"]]
                               .assign(board=board, valuation=label))

            if label == "종가":
                main_table = table
                common.write_csv(OUTPUT / f"year_table{suffix}.csv", renamed(year_table))

                if board == "U2":
                    curves = pd.DataFrame(equity.T, columns=[f"cell{cell}" for cell in rules["cell"]])
                    curves.insert(0, "date", panel.market_dates[first_day:last_day + 1])
                    curves["kospi_hold"] = kospi_curve
                    curves["kosdaq_hold"] = kosdaq_curve
                    curves.to_parquet(OUTPUT / "slot_equity.parquet", index=False)
                    days = np.arange(first_day, last_day + 1)
                    long = pd.DataFrame({"date": np.tile(panel.market_dates[days], len(rules)),
                                         "cell": np.repeat(rules["cell"].to_numpy(), len(days)).astype(np.int16),
                                         "portfolio_pct": daily["portfolio"][:, days].ravel().astype(np.float32),
                                         "index_same_weight_pct": daily["kospi"][:, days].ravel().astype(np.float32),
                                         "control_pct": daily["control"][:, days].ravel().astype(np.float32)})
                    long.to_parquet(OUTPUT / "daily_returns.parquet", index=False)
                    day0 = []

                    for cell in rules["cell"]:
                        keep = strategy["keep"][:, cell]
                        hold = panel.market_position[strategy["exit_row"][keep, cell]] - strategy["day"][keep]
                        day0.append(float((hold == 0).mean()))

                    main_table["day0_exit_share"] = day0
            else:
                short = "m10" if multiplier == 0.9 else "m20"
                main_table = main_table.merge(table[["cell", "kospi_t_min", "random_t_min", "live_t_min", "slot_annual_pct", "passes", "status"]]
                                              .rename(columns=lambda name: name if name == "cell" else f"{name}_{short}"), on="cell")

        main_table["passes_all_valuations"] = main_table["passes"] & main_table["passes_m10"] & main_table["passes_m20"]
        main_table = renamed(main_table)
        common.write_csv(OUTPUT / f"cell_table{suffix}.csv", main_table)
        common.write_csv(OUTPUT / f"slot_table{suffix}.csv", pd.concat(slot_frames, ignore_index=True))
        print(main_table[["cell", "rule", "trades", "net_mean", "hold_median", "index_annual_pct", "index_t_min", "random_t_min",
                          "live_annual_pct", "live_t_min", "slot_trades", "slot_annual_pct", "status", "status_m20"]].to_string(index=False), flush=True)

        if board == "U2":
            before = pd.read_csv(ROUND3 / "cell_table.csv")
            old_trades = pd.read_parquet(ROUND3 / "trades.parquet", columns=["cell", "hold_days"])
            before["day0_exit_share"] = before["cell"].map(old_trades.groupby("cell")["hold_days"].apply(lambda hold: float((hold == 0).mean())))
            columns = ["trades", "day0_exit_share", "net_mean", "hold_median", "kospi_annual_pct", "kospi_t_min", "random_annual_pct",
                       "random_t_min", "live_annual_pct", "live_t_min", "slot_trades", "slot_annual_pct", "status"]
            compare = before[["cell", "rule"] + columns].rename(columns={name: f"round3_{name}" for name in columns})
            after = main_table[["cell", "trades", "day0_exit_share", "net_mean", "hold_median", "index_annual_pct", "index_t_min",
                                "random_annual_pct", "random_t_min", "live_annual_pct", "live_t_min", "slot_trades", "slot_annual_pct", "status"]]
            compare = compare.merge(after.rename(columns={name: f"round4_{name}" for name in after.columns if name != "cell"}), on="cell")
            compare.insert(2, "trend_cell", trend[compare["cell"].to_numpy()])
            common.write_csv(OUTPUT / "round3_vs_round4.csv", compare)
            common.write_csv(OUTPUT / "trend_cells_compare.csv", compare[compare["trend_cell"]].drop(columns="trend_cell"))
            keep = strategy["keep"]
            index, cell = np.nonzero(keep)
            exit_rows = strategy["exit_row"][index, cell]
            pd.DataFrame({"code": np.array(panel.codes)[strategy["stock"][index]], "entry_date": panel.dates[strategy["entries"][index]],
                          "market": strategy["index_id"][index].astype(np.int8), "approach": strategy["approach"][index].astype(np.int8),
                          "cell": cell.astype(np.int16), "exit_date": panel.dates[exit_rows], "reason": strategy["reason"][index, cell],
                          "hold_days": (panel.market_position[exit_rows] - strategy["day"][index]).astype(np.int32),
                          "net_pct": strategy["net"][index, cell].astype(np.float32)}).to_parquet(OUTPUT / "trades.parquet", index=False)

        del strategy, control

    first = renamed(pd.DataFrame(first_rows))
    common.write_csv(OUTPUT / "first_table_a.csv", first)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round4.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")

    for name in ("first_table_a.csv", "cell_table.csv", "cell_table_u0.csv", "year_table.csv", "year_table_u0.csv", "slot_table.csv",
                 "slot_table_u0.csv", "round3_vs_round4.csv", "trend_cells_compare.csv"):
        hashed[name] = exit_test.file_hash(OUTPUT / name)

    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1), "years": years,
               "boards": sizes, "bars": all_bars, "index_missing_market_days": {"kospi": kospi_missing, "kosdaq": kosdaq_missing},
               "kosdaq_source": str(KOSDAQ.relative_to(exit_test.REPOSITORY)).replace("\\", "/"), "kosdaq_hash": exit_test.file_hash(KOSDAQ),
               "kosdaq_stocks": int(market_of_stock.sum()), "stocks": int(len(market_of_stock)),
               "kosdaq_hold_annual_pct": kosdaq_annual, "kosdaq_hold_mdd_pct": kosdaq_mdd, "flips": round3_test.FLIPS,
               "seed": common.SEED, "random_seed": round3.RANDOM_SEED, "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    print(first.T.to_string(), flush=True)
    print({key: value["index"]["main_bar"] for key, value in all_bars.items()}, flush=True)
    print(f"끝 {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
