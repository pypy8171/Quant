"""스터디 38 대시보드 데이터 — daily_stop_grid.py 산출물을 dashboard/data/ 아래 JSON·gzip 으로 펼친다.

숫자는 모두 산출 파일(grid_*.csv · depth.npz · random.csv · verify.json)이나 같은 시험기를 다시 돌린 매매 건에서 읽는다.
페이지(dashboard/index.html)는 손으로 쓴 숫자를 갖지 않는다.

매매 건을 싣는 변형(기준은 페이지에도 적는다):
  - 지금 규칙과 그 형제 2개(매수일 익절 봄, 익절·존 이탈 없음)
  - 손절 종류 × 판정 시점 × 올림 묶음(익절·존 이탈 없음)마다 순차 매매 "보유일당 수익"이 가장 큰 저점 정의 하나
  - 전체 격자에서 순차 매매 보유일당 수익 상위 5, 수익 합 상위 5
사용: py research/studies/38_trend_stop_intraday/build_dashboard.py [control]
  control 이면 control_test.py 산출(control_*.csv · control_combo_*.csv)만 data/control.txt 로 다시 쓴다.
"""
import base64
import gzip
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import daily_stop_grid as grid  # noqa: E402

OUT = HERE / "dashboard" / "data"
BUNDLE_SIZE = 40
MISSING = -2147483648
SLICE_COLUMNS = ["price_decile", "cap_decile", "turnover5_decile", "turnover10_decile", "turnover20_decile",
                 "turnover60_decile", "in_1500"]
GRID_KEEP = (["trades", "mean_hold_days", "median_hold_days", "ambiguous_both_lines", "ambiguous_entry_day",
              "ref_fallback", "mean_stop_distance", "skip_no_reference", "no_reference"]
             + [f"s{slip}_{name}" for slip in grid.SLIPPAGE_TICKS for name in
                ("win_rate", "mean", "median", "sum", "profit_factor", "per_hold_day", "worst")]
             + [f"reason_{code}" for code in grid.REASONS] + [f"y{year}" for year in grid.YEARS])


DROPPED_TRADE_COLUMNS = ["net0", "net2", "turnover5_decile", "turnover10_decile", "turnover60_decile"]


def write_packed(path: Path, raw: bytes) -> int:
    """gzip 한 바이트를 base64 텍스트로(아티팩트는 이진 파일을 싣지 않는다). 페이지는 atob → 압축 풀기."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(base64.b64encode(gzip.compress(raw, 9)))
    return path.stat().st_size


def write_json(name: str, payload, compress=False) -> int:
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False,
                      default=lambda value: value.item() if hasattr(value, "item") else str(value))
    path = OUT / name

    if compress:
        return write_packed(path, text.encode("utf-8"))
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    return path.stat().st_size


def clean(frame: pd.DataFrame) -> dict:
    """열 단위 배열로. inf·nan 은 None."""
    result = {}

    for column in frame.columns:
        values = frame[column].to_numpy()

        if values.dtype.kind == "f":
            values = np.where(np.isfinite(values), np.round(values, 4), np.nan)
            result[column] = [None if np.isnan(value) else float(value) for value in values]
        else:
            result[column] = values.tolist()

    return result


def grid_table(name: str) -> pd.DataFrame:
    frame = pd.read_csv(grid.DATA_DIR / name)
    frame["key"] = [grid.variant_key(row) for row in frame[grid.VARIANT_FIELDS].to_numpy().tolist()]
    return frame


def chosen_variants(sequential: pd.DataFrame) -> list[dict]:
    chosen = [{"variant": grid.CURRENT_RULE, "why": "지금 규칙"},
              {"variant": [0, 6.5, 0, 0, 3.0, 1, 1], "why": "지금 규칙 + 매수한 날 익절도 봄"},
              {"variant": [0, 6.5, 0, 0, 0, 0, 0], "why": "평단 −6.5% 만(익절·존 이탈 없음)"}]
    base = sequential[(sequential["kind"] > 0) & (sequential["take_profit"] == 0) & (sequential["zone_exit"] == 0)]

    for (kind, timing, raise_stop), group in base.groupby(["kind", "timing", "raise"]):
        best = group.sort_values("s1_per_hold_day", ascending=False).iloc[0]
        chosen.append({"variant": [float(value) for value in best[grid.VARIANT_FIELDS]],
                       "why": f"{grid.KINDS[int(kind)]} · {grid.TIMINGS[int(timing)]} · "
                              f"{'올림' if raise_stop else '고정'} 묶음에서 보유일당 수익 1위"})

    for column, label in (("s1_per_hold_day", "보유일당 수익"), ("s1_sum", "수익 합")):
        for rank, (_, row) in enumerate(sequential.sort_values(column, ascending=False).head(5).iterrows()):
            chosen.append({"variant": [float(value) for value in row[grid.VARIANT_FIELDS]],
                           "why": f"격자 전체 {label} {rank + 1}위"})

    unique = {}

    for item in chosen:
        key = grid.variant_key(item["variant"])

        if key in unique:
            unique[key]["why"] += " · " + item["why"]
        else:
            unique[key] = {"key": key, **item}

    return list(unique.values())


_BIG_CUMULATIVE = {}


def big_move_cumulative(panel: grid.Panel) -> np.ndarray:
    if "value" not in _BIG_CUMULATIVE:
        _BIG_CUMULATIVE["value"] = np.cumsum(panel.big_move, axis=1, dtype=np.int32)

    return _BIG_CUMULATIVE["value"]


def trade_frame(panel: grid.Panel, trades: pd.DataFrame) -> pd.DataFrame:
    ticker_index = trades["ticker_index"].to_numpy(int)
    entry_t = trades["entry_t"].to_numpy(int)
    exit_t = trades["exit_t"].to_numpy(int)
    frame = pd.DataFrame({"ticker": ticker_index, "entry_t": entry_t, "entry_price": trades["entry_price"],
                          "exit_t": exit_t, "exit_price": trades["exit_price"], "reason": trades["reason"].astype(int),
                          "ambiguous": trades["ambiguous"].astype(int),
                          "stop_entry": trades["stop_entry"].replace(-np.inf, np.nan),
                          "stop_exit": trades["stop_exit"].replace(-np.inf, np.nan) if "stop_exit" in trades else np.nan,
                          "ref_day": trades["ref_day"].astype(int), "fallback": trades["ref_fallback"].astype(int)})

    if "entry_path" in trades:
        frame["path"] = trades["entry_path"].astype(int)
        frame["atr_percent"] = trades["atr_percent"]

    for slip in grid.SLIPPAGE_TICKS:
        frame[f"net{slip}"] = panel.net_return(ticker_index, entry_t, trades["entry_price"].to_numpy(), exit_t,
                                               trades["exit_price"].to_numpy(), slip) * 100

    for column in SLICE_COLUMNS:
        frame[column] = panel.slices[column][ticker_index, entry_t].astype(int)

    cumulative = big_move_cumulative(panel)
    big = cumulative[ticker_index, exit_t] - np.where(entry_t > 0, cumulative[ticker_index, entry_t - 1], 0) > 0
    frame["big_move"] = big.astype(int)
    return frame


def slice_tables(panel: grid.Panel, frame: pd.DataFrame) -> dict:
    hold = frame["exit_t"] - frame["entry_t"] + 1
    frame = frame.assign(hold=hold, year=panel.dates[frame["entry_t"]].year,
                         market=panel.market[frame["ticker"]])
    tables = {}

    for column in SLICE_COLUMNS + ["market", "year"]:
        rows = []

        for value, group in frame.groupby(column):
            net = group["net1"]
            losses = -net[net < 0].sum()
            rows.append({"value": int(value), "trades": len(group), "tickers": int(group["ticker"].nunique()),
                         "win_rate": float((net > 0).mean() * 100), "mean": float(net.mean()),
                         "sum": float(net.sum()), "per_hold_day": float(net.sum() / group["hold"].sum()),
                         "profit_factor": float(net[net > 0].sum() / losses) if losses > 0 else None,
                         "mean_hold": float(group["hold"].mean())})

        tables[column] = rows

    return tables


def random_summary(sequential: pd.DataFrame) -> dict:
    path = grid.DATA_DIR / "random.csv"

    if not path.exists():
        return {"rounds": 0}

    random = pd.read_csv(path)
    rounds_started = int(random["seed"].nunique())
    complete = random.groupby("seed")["variant"].count()
    random = random[random["seed"].isin(complete[complete == complete.max()].index)]
    actual = sequential.set_index("key")
    metrics = ["s1_per_hold_day", "s1_mean", "s1_sum", "s1_profit_factor"]
    per_variant = []

    for key, group in random.groupby("variant"):
        if key not in actual.index:
            continue

        row = {"key": key, "rounds": len(group)}

        for metric in metrics:
            values = group[metric].replace([np.inf, -np.inf], np.nan).dropna().to_numpy()
            mine = float(actual.loc[key, metric])
            row[metric] = {"actual": mine if np.isfinite(mine) else None,
                           "p05": float(np.percentile(values, 5)), "p50": float(np.percentile(values, 50)),
                           "p95": float(np.percentile(values, 95)),
                           "percentile": float((values < mine).mean() * 100)}

        per_variant.append(row)

    keys = set(random["variant"])
    best = {}

    for metric in metrics:
        pivot = random.pivot(index="seed", columns="variant", values=metric).replace([np.inf, -np.inf], np.nan)
        random_best = pivot.max(axis=1).to_numpy()
        actual_best = float(actual.loc[actual.index.isin(keys), metric].replace([np.inf, -np.inf], np.nan).max())
        best[metric] = {"random_best": [float(value) for value in random_best], "actual_best": actual_best,
                        "actual_best_key": str(actual.loc[actual.index.isin(keys), metric].idxmax()),
                        "percentile": float((random_best < actual_best).mean() * 100)}

    return {"rounds": int(len(random["seed"].unique())), "rounds_started": rounds_started, "variants": len(keys),
            "per_variant": per_variant, "best": best}


def write_depth() -> list[dict]:
    depth = np.load(grid.DATA_DIR / "depth.npz")
    table = depth["table"].astype(np.float32)

    for index in range(table.shape[0]):
        write_packed(OUT / "depth" / f"d{index}.txt", np.ascontiguousarray(table[index]).tobytes())

    return {"definitions": depth["definitions"].tolist(), "percents": depth["percents"].tolist(),
            "multiples": depth["multiples"].tolist(), "buckets": depth["buckets"].tolist(),
            "metrics": depth["metrics"].tolist(), "shape": list(table.shape[1:]),
            "kospi_edges": [round(float(value), 3) for value in depth["kospi_edges"]],
            "atr_edges": [round(float(value), 3) for value in depth["atr_edges"]]}


def write_bars(panel: grid.Panel, tickers: list[int]) -> dict:
    """종목 묶음마다 일봉을 Int32 이진 하나로(gzip). 값은 전날 종가와의 차(원), 값 없음은 MISSING.

    한 종목 = [시가차·고가차·저가차·종가차] × 날짜(첫 거래일부터 마지막 거래일까지). 첫날은 차가 아니라 값 그대로.
    bundle_of[종목] = [묶음, 시작 위치(Int32 개수), 첫 날짜 번호, 날짜 수].
    """
    bundle_of = {}
    tickers = sorted(tickers)

    for bundle, start in enumerate(range(0, len(tickers), BUNDLE_SIZE)):
        chunks = []
        offset = 0

        for ticker in tickers[start:start + BUNDLE_SIZE]:
            valid = np.flatnonzero(np.isfinite(panel.close[ticker]))
            first, last = int(valid[0]), int(valid[-1])
            rows = slice(first, last + 1)
            prices = np.stack([panel.open[ticker, rows], panel.high[ticker, rows], panel.low[ticker, rows],
                               panel.close[ticker, rows]], axis=1)
            base = pd.Series(panel.close[ticker, rows]).ffill().shift(1).to_numpy()
            base[0] = 0.0
            delta = np.where(np.isfinite(prices), np.round(prices - base[:, None]), MISSING).astype(np.int32)
            chunks.append(delta.ravel())
            bundle_of[str(ticker)] = [bundle, offset, first, last - first + 1]
            offset += delta.size

        write_packed(OUT / "bars" / f"b{bundle}.txt", np.concatenate(chunks).astype("<i4").tobytes())

    return bundle_of


def versus_current(prefix: str) -> pd.DataFrame:
    """변형마다 '대조 뺀 값'의 매수 달별 평균을 지금 규칙(매수일 익절 설정이 같은 것)과 같은 달끼리 빼고 그 평균과 t.
    두 변형은 매수 건이 달라 건 단위로는 짝이 안 맞으므로 달 단위로 짝짓는다."""
    path = grid.DATA_DIR / f"{prefix}_months.csv"

    if not path.exists():
        return pd.DataFrame(columns=["key", "versus_current", "versus_current_t"])

    months = pd.read_csv(path).pivot(index="month", columns="key", values="mean_value")
    rows = []

    for key in months.columns:
        base = "_".join(grid.variant_key(grid.CURRENT_RULE).split("_")[:-1] + [key.split("_")[-1]])

        # 이평선 청산(ma_*)은 지금 규칙의 매수 건을 그대로 쓰므로 지금 규칙 그대로와 비교한다.
        if base not in months.columns or key.startswith("ma_"):
            base = grid.variant_key(grid.CURRENT_RULE)

        difference = (months[key] - months[base]).dropna()
        spread = difference.std(ddof=1)
        rows.append({"key": key, "versus_current": difference.mean(),
                     "versus_current_t": difference.mean() / (spread / np.sqrt(len(difference))) if spread > 0 else np.nan})

    return pd.DataFrame(rows)


def write_control() -> int:
    """같은 날·같은 보유 기간·같은 거래대금 구간 대조를 뺀 결과(control_test.py). 두 벌: 손절 단독, 지금 규칙 익절·존 이탈 붙임."""
    payload = {}

    for name, prefix in (("alone", "control"), ("combo", "control_combo"), ("fixed", "control_fixed"), ("ma", "control_ma")):
        tables = {}

        for part in ("variants", "hold", "years"):
            path = grid.DATA_DIR / f"{prefix}_{part}.csv"

            if path.exists():
                frame = pd.read_csv(path)

                if part == "variants":
                    frame = frame.merge(versus_current(prefix), on="key", how="left")

                tables[part] = clean(frame)

        if tables:
            payload[name] = tables

    return write_json("control.txt", payload, compress=True)


def write_slot() -> int:
    """칸 제한 계좌 시험(slot_test.py): 씨앗별 요약, 연도별·평가금 곡선(씨앗 중앙값, 월말), 씨앗 0 의 체결 건 전부."""
    summary = pd.read_csv(grid.DATA_DIR / "slot_summary.csv")
    curve = pd.read_csv(grid.DATA_DIR / "slot_curve.csv", parse_dates=["date"])
    curve = curve.groupby(curve["date"].dt.to_period("M")).last().reset_index(drop=True)
    curve["date"] = curve["date"].dt.strftime("%Y-%m-%d")
    trades = {}
    paths = pd.read_parquet(grid.DATA_DIR / "entries_current.parquet", columns=["ticker_index", "entry_t", "entry_path"])

    for path in sorted(grid.DATA_DIR.glob("slot_trades_*.parquet")):
        frame = pd.read_parquet(path).merge(paths, on=["ticker_index", "entry_t"], how="left")
        trades[path.stem[len("slot_trades_"):]] = clean(frame[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price",
                                                                "reason", "net", "entry_path"]].rename(columns={"ticker_index": "ticker", "entry_path": "path"}))

    payload = {"summary": clean(summary), "years": clean(pd.read_csv(grid.DATA_DIR / "slot_years.csv")),
               "curve": clean(curve), "benchmark": clean(benchmark_curve(curve["date"])), "trades": trades}
    return write_json("slot.txt", payload, compress=True)


def benchmark_curve(month_ends: pd.Series) -> pd.DataFrame:
    """기준선: 전 종목 동일가중 매일 재조정(비용 없음, 하루 ±30% 밖은 자름). 같은 월말로 맞춘다."""
    panel = grid.Panel()
    close = pd.DataFrame(panel.close.T, index=panel.dates)
    halted = pd.DataFrame(panel.halted.T, index=panel.dates)
    daily = close.pct_change(fill_method=None).where(~halted).clip(-0.3, 0.3).mean(axis=1).fillna(0)
    start = pd.Timestamp(month_ends.iloc[0]) - pd.offsets.MonthBegin(1)
    path = (1 + daily[daily.index >= start]).cumprod()
    picked = path.reindex(pd.to_datetime(month_ends), method="ffill")
    return pd.DataFrame({"date": month_ends.to_numpy(), "equal_weight": picked.to_numpy()})


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "control":
        print(f"→ control.txt {write_control() / 1e3:.0f}KB")
        return

    if len(sys.argv) > 1 and sys.argv[1] == "slot":
        print(f"→ slot.txt {write_slot() / 1e3:.0f}KB")
        return

    panel = grid.Panel()
    sequential = grid_table("grid_sequential.csv")
    paired = grid_table("grid_paired.csv")
    current = grid.current_entries(panel)
    ticker_starts = np.searchsorted(current["ticker_index"].to_numpy(), np.arange(len(panel.tickers) + 1)).astype(np.int64)
    sizes = {}
    variants = chosen_variants(sequential)
    traded = set()

    for item in variants:
        trades, _ = panel.run(item["variant"])
        frame = trade_frame(panel, trades)
        item["trades"] = len(frame)
        item["slices"] = slice_tables(panel, frame)
        traded.update(frame["ticker"].unique().tolist())
        sizes[f"seq_{item['key']}"] = write_json(f"trades/seq_{item['key']}.txt", clean(frame.drop(columns=DROPPED_TRADE_COLUMNS)), compress=True)

        out = np.zeros((len(current), 7))
        grid.exits_fixed(panel.open, panel.high, panel.low, panel.close, panel.halted, ticker_starts,
                         current["entry_t"].to_numpy(np.int64), current["entry_price"].to_numpy(), panel.end_index,
                         np.asarray(item["variant"], float), out)
        valid = out[:, 0] >= 0
        pair = pd.DataFrame({"ticker_index": current["ticker_index"], "entry_t": current["entry_t"],
                             "entry_price": current["entry_price"], "exit_t": out[:, 0].astype(int),
                             "exit_price": out[:, 1], "reason": out[:, 2], "ambiguous": out[:, 3],
                             "stop_entry": out[:, 4], "ref_day": out[:, 5], "ref_fallback": out[:, 6]})
        pair_frame = trade_frame(panel, pair[valid])
        pair_frame.insert(0, "trade_id", np.flatnonzero(valid))
        sizes[f"pair_{item['key']}"] = write_json(f"trades/pair_{item['key']}.txt", clean(pair_frame.drop(columns=DROPPED_TRADE_COLUMNS)), compress=True)
        print(f"  {item['key']}: 순차 {len(frame)}건 · 짝 {int(valid.sum())}건", flush=True)

    traded.update(current["ticker_index"].unique().tolist())
    bundle_of = write_bars(panel, list(traded))

    big_rows = np.argwhere(panel.big_move)
    previous_close = pd.DataFrame(panel.close.T).ffill().shift(1).to_numpy().T
    big = [{"ticker": int(ticker), "t": int(day), "previous": float(previous_close[ticker, day]),
            "close": float(panel.close[ticker, day]),
            "change": float((panel.close[ticker, day] / previous_close[ticker, day] - 1) * 100)}
           for ticker, day in big_rows]

    verify = json.loads((grid.DATA_DIR / "verify.json").read_text(encoding="utf-8"))
    meta = {
        "built": pd.Timestamp.now().strftime("%Y-%m-%d %H:%M"),
        "dates": panel.dates.strftime("%Y-%m-%d").tolist(), "start_index": panel.start_index,
        "tickers": panel.tickers.tolist(), "names": [str(name) for name in panel.names],
        "market": panel.market.tolist(), "variant_fields": grid.VARIANT_FIELDS, "reasons": grid.REASONS,
        "kinds": grid.KINDS, "timings": grid.TIMINGS, "entry_paths": grid.ENTRY_PATHS,
        "tax": grid.SELL_TAX_SCHEDULE, "new_tick_from": int(np.searchsorted(panel.dates, pd.Timestamp(grid.NEW_TICK_TABLE_FROM))), "commission_percent": grid.COMMISSION * 100,
        "constants": {"ATR_PERIOD": grid.ATR_PERIOD, "ENTRY_ATR_MAX_PERCENT": grid.ENTRY_ATR_MAX_PERCENT,
                      "ENTRY_OPEN_DEVIATION_MIN_PERCENT": grid.ENTRY_OPEN_DEVIATION_MIN_PERCENT,
                      "MIN_PRICE": grid.MIN_PRICE, "PULLBACK": grid.PULLBACK, "ENTRY_UPPER": grid.ENTRY_UPPER,
                      "HOLD_LOW": grid.HOLD_LOW, "HOLD_HIGH": grid.HOLD_HIGH},
        "current_entries": len(current), "chosen": variants, "bundle_of": bundle_of,
        "panel_tickers_per_year": {str(year): int((np.isfinite(panel.close[:, panel.dates.year == year]).any(axis=1)).sum())
                                   for year in grid.YEARS},
        "big_moves": big, "verify": verify, "depth": write_depth(), "random": random_summary(sequential),
    }
    sizes["meta"] = write_json("meta.txt", meta, compress=True)
    sizes["control"] = write_control()
    keep = grid.VARIANT_FIELDS + ["key"] + [column for column in GRID_KEEP if column in sequential.columns]
    sizes["grid_seq"] = write_json("grid_seq.txt", clean(sequential[keep]), compress=True)
    keep = grid.VARIANT_FIELDS + ["key"] + [column for column in GRID_KEEP if column in paired.columns]
    sizes["grid_pair"] = write_json("grid_pair.txt", clean(paired[keep]), compress=True)
    total = sum(path.stat().st_size for path in OUT.rglob("*") if path.is_file())
    count = sum(1 for path in OUT.rglob("*") if path.is_file())
    print(f"→ {OUT} · 파일 {count}개 · {total / 1e6:.1f}MB · 묶음 {len({value[0] for value in bundle_of.values()})}개")


if __name__ == "__main__":
    main()
