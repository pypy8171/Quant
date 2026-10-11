"""스터디 38 손절 시험 — 매매마다 '같은 날·같은 보유 기간·같은 거래대금 구간 전 종목 균등 매수' 대조를 빼고, 보유 기간별로 나눈다.

왜: 손절 변형의 수익이 손절 자리 덕인지, 오래 들고 가서 시장·구간 상승분을 먹은 덕인지 가른다. 무작위 진입 비교
  (daily_stop_grid.py random)는 매수일만 섞어서 이 둘을 가르지 못한다. 38_trend_support_realtime 8회차가 같은 대조로
  보유 기간 효과를 찾아 권고했다(2026-10-11).

대조(내가 정함, 8회차 정의를 이 시험기에 맞춘 판):
  매매마다 매수일 t 에 그 종목과 같은 거래대금 20일 10분위(전날까지 값, daily_panel.py)에 든 보통주 전부를 t 시가에 똑같이
  사서 그 매매의 매도일 e 종가에 판 순수익 평균. 수수료 양쪽·매도일 세율은 신호와 같고 슬리피지는 대조 0, 신호 1호가
  (신호에 불리한 쪽). t 에 거래정지거나 시가가 없는 종목은 빼고, e 에 값이 없으면(정지·상장폐지 뒤) 마지막 종가를 쓴다.
  신호 종목 자신도 대조에 들어 있다.
  판정 값 = 신호 순수익 − 대조 순수익(%p). 월 묶음 t(매수 달별 평균의 t). 변형 309개를 다 보므로 본페로니 기준 |t| ≥ 3.6
  (양쪽 5% ÷ 309)을 같이 적는다.
[limit] 상장폐지 뒤 마지막 종가를 쓰므로 대조 쪽 폐지 손실이 빠진다(대조가 좋게 나와 판정 값은 보수적으로 작아진다).

산출(data/): control_variants.csv(변형별), control_hold.csv(변형 × 보유 구간), control_years.csv(변형 × 연도),
  control_months.csv(변형 × 매수 달 — 두 변형의 차이를 달별로 짝지어 t 를 낼 때 쓴다)
  combo 모드는 사건 손절 610개에 지금 규칙의 익절 3%·존 이탈을 그대로 붙여(매수일 익절 끔·켬 두 벌) 손절선만 바꾼 효과를
  같은 대조로 잰다. 이 조합은 격자가 매매 파일을 남기지 않아 여기서 다시 돌린다 → control_combo_*.csv.
  fixed 모드는 평단 −x%(2–15%, 6.5%) × 판정 시점(장중·종가·다음 날 시가)에 같은 익절·존 이탈을 붙인다 — combo 에서
  나은 변형이 손절선 종류 덕인지 판정 시점 덕인지 가른다 → control_fixed_*.csv.
재실행: py research/studies/38_trend_stop_intraday/control_test.py [combo|fixed]
"""
from __future__ import annotations

import sys
import time

import numba
import numpy as np
import pandas as pd

import daily_stop_grid as grid

HOLD_EDGES = [(1, 20, "1–20일"), (21, 60, "21–60일"), (61, 10**9, "61일 초과")]
BONFERRONI_T = 3.6


@numba.njit(parallel=True, cache=True)
def pool_ratio(days_in, days_out, groups, member_start, member_list, open_price, close_filled):
    """조합마다 그 날·그 구간 종목들의 (매도일 종가 ÷ 매수일 시가) 평균과 종목 수."""
    count = days_in.shape[0]
    result = np.full(count, np.nan)
    members = np.zeros(count, np.int64)

    for index in numba.prange(count):
        slot = groups[index]
        total = 0.0
        number = 0

        for position in range(member_start[slot], member_start[slot + 1]):
            stock = member_list[position]
            sell = close_filled[stock, days_out[index]]

            if sell > 0:
                total += sell / open_price[stock, days_in[index]]
                number += 1

        if number > 0:
            result[index] = total / number

        members[index] = number

    return result, members


def month_t(values: np.ndarray, months: np.ndarray) -> float:
    frame = pd.DataFrame({"value": values, "month": months}).groupby("month")["value"].mean()

    if len(frame) < 3 or frame.std(ddof=1) == 0:
        return float("nan")

    return float(frame.mean() / (frame.std(ddof=1) / np.sqrt(len(frame))))


def main() -> None:
    started = time.time()
    panel = grid.Panel()
    decile = np.load(grid.PANEL_PATH)["turnover20_decile"]
    stocks, days = panel.close.shape
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    eligible = (decile >= 0) & np.isfinite(panel.open) & (panel.open > 0) & ~panel.halted
    # 날짜 × 10분위마다 종목 목록(CSR). 칸 번호 = 날짜 × 10 + 분위.
    day_index, stock_index = np.nonzero(eligible.T)
    slot = day_index * 10 + decile.T[day_index, stock_index]
    order = np.argsort(slot, kind="stable")
    member_list = stock_index[order].astype(np.int64)
    member_start = np.searchsorted(slot[order], np.arange(days * 10 + 1)).astype(np.int64)
    months = panel.dates.year * 100 + panel.dates.month

    if len(sys.argv) > 1 and sys.argv[1] == "combo":
        prefix = "control_combo"
        variants = [grid.CURRENT_RULE, [0, 6.5, 0, 0, 3.0, 1, 1]]
        variants += [base[:4] + [3.0, 1, entry_day] for base in grid.stop_variants() for entry_day in (0, 1)]
        sources = {grid.variant_key(variant): variant for variant in variants}
    elif len(sys.argv) > 1 and sys.argv[1] == "fixed":
        prefix = "control_fixed"
        depths = sorted(set(range(2, 16)) | {6.5})
        variants = [[0, depth, timing, 0, 3.0, 1, entry_day] for depth in depths for timing in (0, 1, 2) for entry_day in (0, 1)]
        sources = {grid.variant_key(variant): variant for variant in variants}
    elif len(sys.argv) > 1 and sys.argv[1] == "ma":
        import ma_exit

        prefix = "control_ma"
        entries = pd.read_parquet(grid.DATA_DIR / "entries_current.parquet")
        lines = ma_exit.Lines(panel)
        sources = {grid.variant_key(grid.CURRENT_RULE): lambda: entries}
        sources.update({ma_exit.moving_average_key(variant): (lambda variant=variant: ma_exit.moving_average_trades(panel, lines, entries, variant))
                        for variant in ma_exit.moving_average_variants()})
    else:
        prefix = "control"
        sources = {path.stem[4:]: path for path in (grid.DATA_DIR / "trades").glob("seq_*.parquet")}

    keys = sorted(sources)
    loaded = {}
    combos = []

    for key in keys:
        source = sources[key]

        if callable(source):
            trades = source()
        elif isinstance(source, type(grid.DATA_DIR)):
            trades = pd.read_parquet(source)
        else:
            trades = panel.run(source)[0]

        # 변형이 천 개를 넘으면 매매 표 전체를 들고 있기엔 메모리가 모자라 쓰는 열만 남긴다.
        trades = trades[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price", "reason"]].copy()
        ticker = trades["ticker_index"].to_numpy(int)
        entry_t = trades["entry_t"].to_numpy(int)
        exit_t = trades["exit_t"].to_numpy(int)
        group = decile[ticker, entry_t].astype(np.int64)
        loaded[key] = (trades, ticker, entry_t, exit_t, group)
        valid = group >= 0
        combos.append(np.stack([entry_t[valid], exit_t[valid], entry_t[valid] * 10 + group[valid]], axis=1))

    unique = np.unique(np.concatenate(combos), axis=0)
    print(f"변형 {len(keys)}개 · 대조 조합 {len(unique):,}개 · {time.time() - started:.0f}초", flush=True)
    ratio, members = pool_ratio(unique[:, 0].copy(), unique[:, 1].copy(), unique[:, 2].copy(), member_start, member_list,
                                panel.open, close_filled)
    tax = panel.tax[unique[:, 1]]
    control_net = ratio * (1.0 - grid.COMMISSION - tax) / (1.0 + grid.COMMISSION) - 1.0
    lookup = pd.DataFrame({"entry_t": unique[:, 0], "exit_t": unique[:, 1], "slot": unique[:, 2],
                           "control": control_net * 100, "members": members})
    print(f"대조 계산 끝 · {time.time() - started:.0f}초", flush=True)

    variant_rows, hold_rows, year_rows, month_rows = [], [], [], []

    for key in keys:
        trades, ticker, entry_t, exit_t, group = loaded[key]
        signal = panel.net_return(ticker, entry_t, trades["entry_price"].to_numpy(), exit_t,
                                  trades["exit_price"].to_numpy(), 1) * 100
        frame = pd.DataFrame({"entry_t": entry_t, "exit_t": exit_t, "slot": entry_t * 10 + group, "signal": signal,
                              "hold": exit_t - entry_t + 1, "valid": group >= 0})
        frame = frame[frame["valid"]].merge(lookup, on=["entry_t", "exit_t", "slot"], how="left")
        frame = frame[np.isfinite(frame["control"])]
        frame["value"] = frame["signal"] - frame["control"]
        frame["month"] = months[frame["entry_t"].to_numpy()]
        frame["year"] = panel.dates[frame["entry_t"].to_numpy()].year
        by_year = frame.groupby("year")["value"].mean()
        t_value = month_t(frame["value"].to_numpy(), frame["month"].to_numpy())
        variant_rows.append({"key": key, "trades": len(trades), "matched": len(frame),
                             "mean_signal": frame["signal"].mean(), "mean_control": frame["control"].mean(),
                             "mean_value": frame["value"].mean(), "t_value": t_value,
                             "win_vs_control": (frame["value"] > 0).mean() * 100,
                             "share_positive_years": (by_year > 0).mean() * 100,
                             "mean_hold": frame["hold"].mean(), "mean_members": frame["members"].mean(),
                             "median_hold": frame["hold"].median(), "p90_hold": frame["hold"].quantile(0.9),
                             "max_hold": frame["hold"].max(), "median_signal": frame["signal"].median(),
                             "worst_signal": frame["signal"].min(), "share_signal_positive": (frame["signal"] > 0).mean() * 100,
                             "share_held_to_end": (trades["reason"].to_numpy() == 6).mean() * 100,
                             "per_hold_day_value": frame["value"].sum() / frame["hold"].sum(),
                             "passes_bonferroni": bool(t_value >= BONFERRONI_T)})

        for low, high, label in HOLD_EDGES:
            part = frame[(frame["hold"] >= low) & (frame["hold"] <= high)]
            hold_rows.append({"key": key, "bucket": label, "trades": len(part),
                              "share": len(part) / max(len(frame), 1) * 100,
                              "mean_signal": part["signal"].mean() if len(part) else np.nan,
                              "mean_control": part["control"].mean() if len(part) else np.nan,
                              "mean_value": part["value"].mean() if len(part) else np.nan,
                              "t_value": month_t(part["value"].to_numpy(), part["month"].to_numpy()) if len(part) else np.nan})

        by_month = frame.groupby("month")["value"].agg(["mean", "size"])
        month_rows.append(pd.DataFrame({"key": key, "month": by_month.index, "mean_value": by_month["mean"].to_numpy(),
                                        "trades": by_month["size"].to_numpy()}))

        for year, value in by_year.items():
            year_rows.append({"key": key, "year": int(year), "mean_value": value,
                              "trades": int((frame["year"] == year).sum())})

    pd.DataFrame(variant_rows).to_csv(grid.DATA_DIR / f"{prefix}_variants.csv", index=False)
    pd.DataFrame(hold_rows).to_csv(grid.DATA_DIR / f"{prefix}_hold.csv", index=False)
    pd.DataFrame(year_rows).to_csv(grid.DATA_DIR / f"{prefix}_years.csv", index=False)
    pd.concat(month_rows).to_csv(grid.DATA_DIR / f"{prefix}_months.csv", index=False)
    print(f"끝 · {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
