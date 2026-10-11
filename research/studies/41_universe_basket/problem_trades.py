"""스터디 41 — 대표 칸의 문제 매매·문제 종목·차트 자료.

대표 칸 = results/representative_cells.csv(사전 등록 23행 규칙으로 run_stage_b.py 가 고른 칸).
문제 매매 = 순수익률 < 0 이거나 지수 대비 초과 < 0 인 매매 전부(24행). 문제 종목 = 누적 손익 < 0 이거나 지수 대비 누적 초과 < 0(25행).
원인 분류(26행)·사건(27행)·수익 정의(35행)·손실 기여(36행)·경로 지표(37행)·차트 자료(28행)는 choices_log 그대로다.
실행: py -I -X utf8 research/studies/41_universe_basket/problem_trades.py
"""
from __future__ import annotations

import json
import math
import sys
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd

import study41 as study
import common
import run_stage_b

CHART = study.OUTPUT / "chart_data"
BEFORE_BARS = 120          # 첫 매수 전 거래일 수(내가 정함, 28행)
AFTER_BARS = 60            # 마지막 매도 뒤 거래일 수(내가 정함, 28행)
FILE_LIMIT = 3_000_000       # 3MB(10진, 보수적으로)
SHARE = 0.5                # 원인 ②·③ 문턱(내가 정함, 26행)
GAP_DOWN = -0.10           # 갭 하락 사건 문턱(지시)
DISTRESS_DAYS = 365        # 관리종목 근사 창(내가 정함, 27행)
COARSE_PRICE = 100.0      # 수정가가 이 아래면 원 단위 반올림 오차가 1%를 넘는다(내가 정함, 40행)
CLOSE_DOWN_BIT = 8
KIND_TEXT = {0: "매도", 1: "상장폐지", 2: "미청산(자료 끝)"}
MARKET_TEXT = {0: "코스피", 1: "코스닥"}


def filled(series: np.ndarray) -> np.ndarray:
    return pd.Series(series).ffill().to_numpy()


def to_date(value: int) -> pd.Timestamp:
    return pd.Timestamp(str(int(value)))


class Paths:
    """한 매매의 보유 경로(밤·장중 로그, MAE/MFE, 사건)."""

    def __init__(self, inputs: study.Inputs, index_open: dict, index_close: dict, distress: dict):
        self.inputs = inputs
        self.index_open = index_open
        self.index_close = index_close
        self.distress = distress

    def measure(self, stock: int, buy_day: int, sell_day: int, kind: int, buy_open: bool, buy_price: float,
                sell_price: float, market: int) -> dict:
        inputs = self.inputs
        sell_open = buy_open and kind == 0
        last = int(inputs.stock_last[stock]) if kind == 1 else (sell_day if kind == 0 else inputs.day_count - 1)
        span = slice(buy_day, last + 1)
        open_row = inputs.open[span, stock]
        close_row = inputs.close[span, stock]
        high_row = inputs.high[span, stock].astype(np.float64)
        low_row = inputs.low[span, stock].astype(np.float64)
        close_known = filled(close_row)
        days = np.arange(buy_day, last + 1)
        length = len(days)

        # 장중 구간: 시가 매수면 매수일부터, 종가 매수면 다음 날부터. 시가 매도면 매도 전날까지.
        first_intraday = 0 if buy_open else 1
        last_intraday = length - 2 if sell_open else length - 1
        intraday_mask = np.zeros(length, dtype=bool)

        if last_intraday >= first_intraday:
            intraday_mask[first_intraday:last_intraday + 1] = True

        night_mask = np.zeros(length, dtype=bool)
        night_mask[1:] = True
        valid_open = np.isfinite(open_row)
        valid_close = np.isfinite(close_row)

        with np.errstate(invalid="ignore", divide="ignore"):
            night = np.where(night_mask & valid_open, np.log(open_row / np.r_[np.nan, close_known[:-1]]), np.nan)
            intraday = np.where(intraday_mask & valid_open & valid_close, np.log(close_row / open_row), np.nan)

        # 종가 매수일 장중은 없음, 시가 매수 첫날은 매수가부터 — 매수가 = 그날 시가 이므로 위 식과 같다
        index_open = filled(self.index_open[market][span])
        index_close = filled(self.index_close[market][span])

        with np.errstate(invalid="ignore", divide="ignore"):
            index_night = np.where(night_mask, np.log(index_open / np.r_[np.nan, index_close[:-1]]), np.nan)
            index_intraday = np.where(intraday_mask, np.log(index_close / index_open), np.nan)

        index_buy = index_open[0] if buy_open else index_close[0]
        index_sell = index_open[-1] if sell_open else index_close[-1]

        path_high = np.where(intraday_mask, high_row, np.nan)
        path_low = np.where(intraday_mask, low_row, np.nan)
        points_high = np.r_[path_high, sell_price]
        points_low = np.r_[path_low, sell_price]
        point_days = np.r_[days, days[-1]]
        mae_at = int(np.nanargmin(points_low)) if np.isfinite(points_low).any() else -1
        mfe_at = int(np.nanargmax(points_high)) if np.isfinite(points_high).any() else -1
        mae = min(points_low[mae_at] / buy_price - 1.0, 0.0) if mae_at >= 0 else 0.0
        mfe = max(points_high[mfe_at] / buy_price - 1.0, 0.0) if mfe_at >= 0 else 0.0
        mae_day = point_days[mae_at] if mae_at >= 0 and mae < 0 else buy_day
        mfe_day = point_days[mfe_at] if mfe_at >= 0 and mfe > 0 else buy_day

        # 추세 이탈: 보유 장중일 중 전날 종가가 SMA20 이상이다가 그날 종가가 아래로 내려간 첫 날(등록 26행, 교차 기준)
        average = inputs.sma20[span, stock].astype(np.float64)
        previous_span = slice(max(buy_day - 1, 0), last)
        previous_close = np.r_[np.nan, inputs.close[previous_span, stock].astype(np.float64)][-length:]
        previous_average = np.r_[np.nan, inputs.sma20[previous_span, stock].astype(np.float64)][-length:]

        with np.errstate(invalid="ignore"):
            below = intraday_mask & valid_close & np.isfinite(average) & (close_row < average)
            crossed = below & np.isfinite(previous_close) & np.isfinite(previous_average) & (previous_close >= previous_average)

        trend_log, trend_index_log, trend_day = np.nan, np.nan, -1
        before = buy_day - 1
        below_at_buy = (before >= 0 and np.isfinite(inputs.close[before, stock]) and np.isfinite(inputs.sma20[before, stock])
                        and float(inputs.close[before, stock]) < float(inputs.sma20[before, stock]))
        coarse = bool((np.nanmin(np.r_[close_row, buy_price]) if np.isfinite(close_row).any() else buy_price) < COARSE_PRICE)

        if crossed.any():
            first = int(np.flatnonzero(crossed)[0])
            trend_day = int(days[first])
            trend_log = math.log(sell_price / close_row[first])
            trend_index_log = math.log(index_sell / index_close[first])

        events = []

        if kind == 1:
            events.append("상장폐지")

        if coarse:
            events.append("수정가 100원 미만")

        if below_at_buy:
            events.append("SMA20 아래에서 매수")

        held_market = inputs.market[buy_day:min(last, int(inputs.stock_last[stock])) + 1, stock]

        if (held_market < 0).any():
            events.append("거래정지")

        distress_date = self.distress.get(stock)

        if distress_date is not None:
            sell_date = to_date(inputs.dates[min(sell_day, inputs.day_count - 1)])

            if sell_date <= distress_date <= sell_date + pd.Timedelta(days=DISTRESS_DAYS):
                events.append("관리종목(근사)")

        if ((inputs.lock[span, stock] & CLOSE_DOWN_BIT) != 0)[intraday_mask].any():
            events.append("하한가")

        if (night[np.isfinite(night)] <= math.log1p(GAP_DOWN)).any():
            events.append("갭하락-10%")

        return {
            "night": float(np.nansum(night)), "intraday": float(np.nansum(intraday)),
            "index_night": float(np.nansum(index_night)), "index_intraday": float(np.nansum(index_intraday)),
            "index_return": float(index_sell / index_buy - 1.0) if index_buy > 0 else np.nan,
            "mae": mae, "mae_day": int(mae_day), "mfe": mfe, "mfe_day": int(mfe_day),
            "trend_log": trend_log, "trend_index_log": trend_index_log, "trend_day": trend_day, "events": events,
            "below_at_buy": below_at_buy, "coarse": coarse,
        }


def classify(row: dict) -> str:
    """사전 등록 26행 순서 그대로."""
    if row["kind"] == 1:
        return "상장폐지"

    if row["coarse"]:
        return "100원 미만 가격"

    lag_only = row["ret_net"] >= 0
    shortfall = math.log1p(row["ret_net"]) - (math.log1p(row["index_return"]) if lag_only else 0.0)

    if row["cost_log"] <= SHARE * shortfall:
        return "비용"

    trend = row["trend_log"] - (row["trend_index_log"] if lag_only else 0.0)

    if np.isfinite(trend) and trend < 0 and trend <= SHARE * shortfall:
        return "추세 이탈을 계속 보유"

    night = row["night"] - (row["index_night"] if lag_only else 0.0)
    intraday = row["intraday"] - (row["index_intraday"] if lag_only else 0.0)
    return "밤사이 갭" if night < intraday else "장중"


def ranked_share(frame: pd.DataFrame, value: str, rank: str, share: str) -> None:
    """칸마다 value < 0 인 행을 손실 큰 순으로 순위·누적 비중."""
    frame[rank] = np.nan
    frame[share] = np.nan

    for _, part in frame.groupby("칸", sort=False):
        losing = part[part[value] < 0].sort_values([value, "매수일", "종목코드"], kind="mergesort")
        total = losing[value].sum()

        if total >= 0:
            continue

        frame.loc[losing.index, rank] = np.arange(1, len(losing) + 1)
        frame.loc[losing.index, share] = (losing[value].cumsum() / total).to_numpy()


def split_files(blobs: dict[str, str]) -> dict[str, list[str]]:
    """코드 앞 2자리로 묶고, 3MB 넘는 묶음은 한 자리씩 더 길게 나눈다."""
    result = {}
    pending = [(2, sorted(blobs))]

    while pending:
        width, codes = pending.pop()
        groups = defaultdict(list)

        for code in codes:
            groups[code[:width]].append(code)

        for prefix, members in sorted(groups.items()):
            size = sum(len(blobs[code].encode("utf-8")) + len(code) + 6 for code in members) + 2

            if size > FILE_LIMIT and width < 6:
                pending.append((width + 1, members))
            else:
                result[prefix] = members

    return dict(sorted(result.items()))


def main() -> None:
    started = time.time()
    inputs = study.Inputs()
    cells = run_stage_b.family_cells()
    representative = pd.read_csv(study.OUTPUT / "representative_cells.csv")
    codes = inputs.codes.astype(str)
    names = inputs.names.astype(str)
    dates = inputs.dates.astype(np.int64)
    index_open = {0: filled(inputs.kospi_open), 1: filled(inputs.kosdaq_open)}
    index_close = {0: filled(inputs.kospi_close), 1: filled(inputs.kosdaq_close)}
    delist = pd.read_csv(study.CACHE / "delist_table.csv", dtype={"code": str})
    distress = {int(row.stock): to_date(row.last_date) for row in delist.itertuples() if row.category == "부실"}
    paths = Paths(inputs, index_open, index_close, distress)
    rows = []

    for item in representative.itertuples():
        cell = cells[int(item.행)]
        cell_name = item.칸
        signal = int(cell[0])
        buy_open = int(cell[4]) == 1
        log_int, log_float, _, _ = inputs.trade_log(cell)

        for entry in range(len(log_int)):
            stock, buy_day, decision, rank, sell_day, kind, candidates = (int(value) for value in log_int[entry])
            buy_price, cash_in, cash_out, sell_price, score, cost, _ = log_float[entry]
            market = int(inputs.market[buy_day, stock])
            market = market if market >= 0 else 0
            path = paths.measure(stock, buy_day, sell_day, kind, buy_open, buy_price, sell_price, market)
            ret_net = cash_out / cash_in - 1.0
            sell_date_day = min(sell_day, inputs.day_count - 1)
            row = {
                "칸": cell_name, "신호": study.SIGNAL_NAMES[signal], "종목코드": codes[stock], "종목명": names[stock],
                "시장": MARKET_TEXT[market], "매수일": int(dates[buy_day]), "매수가": buy_price,
                "매도일": int(dates[sell_date_day]), "매도가": sell_price, "보유일": sell_date_day - buy_day,
                "매매_종류": KIND_TEXT[kind], "낸_돈": cash_in, "받은_돈": cash_out, "손익_원": cash_out - cash_in,
                "순수익률": ret_net, "지수_수익률": path["index_return"], "지수_대비_초과": ret_net - path["index_return"],
                "MAE": path["mae"], "MAE_날짜": int(dates[path["mae_day"]]), "MFE": path["mfe"],
                "MFE_날짜": int(dates[path["mfe_day"]]),
                "신호값": score, "신호_순위": rank + 1, "후보_수": candidates,
                "거래대금_순위_20일": float(inputs.turnover_rank[decision, stock]),
                "시가총액_전날": float(inputs.previous_cap[decision, stock]),
                "변동성_20일": float(inputs.previous_rv[decision, stock]),
                "진입_근거": (f"{study.SIGNAL_NAMES[signal]} 순위 {rank + 1}/{candidates}(점수 {score:.4g}), "
                          f"{dates[inputs.decision_days[decision]]} 결정, {item.종목군} 소속, {item.리밸런싱} {item.체결}"),
                "사건": "·".join(path["events"]),
                "비용_로그": -math.log1p(cost / cash_in) if cash_in > 0 else 0.0,
                "밤_로그": path["night"], "장중_로그": path["intraday"],
                "지수_밤_로그": path["index_night"], "지수_장중_로그": path["index_intraday"],
                "추세이탈_날짜": int(dates[path["trend_day"]]) if path["trend_day"] >= 0 else None,
                "추세이탈_뒤_로그": path["trend_log"],
                "kind": kind, "cost_log": 0.0, "stock": stock, "buy_day": buy_day, "sell_day": sell_date_day,
                "ret_net": ret_net, "index_return": path["index_return"], "trend_log": path["trend_log"],
                "trend_index_log": path["trend_index_log"], "night": path["night"], "intraday": path["intraday"],
                "index_night": path["index_night"], "index_intraday": path["index_intraday"],
                "below_at_buy": path["below_at_buy"], "coarse": path["coarse"],
            }
            row["cost_log"] = row["비용_로그"]
            row["문제"] = bool(ret_net < 0 or row["지수_대비_초과"] < 0)
            row["주된_손실_원인"] = classify(row) if row["문제"] else ""
            rows.append(row)

        print(f"{cell_name} 매매 {len(log_int)} 건", flush=True)

    trades = pd.DataFrame(rows)
    trades["지수대비_부족액_원"] = np.where(trades["지수_대비_초과"] < 0, trades["낸_돈"] * trades["지수_대비_초과"], 0.0)
    ranked_share(trades, "손익_원", "손실_기여_순위", "칸_손실_대비_누적_비중")
    ranked_share(trades, "지수대비_부족액_원", "부족액_순위", "칸_부족액_대비_누적_비중")
    internal = ["kind", "cost_log", "stock", "buy_day", "sell_day", "ret_net", "index_return", "trend_log", "trend_index_log",
                "night", "intraday", "index_night", "index_intraday"]
    public = trades.drop(columns=internal)
    problem = public[public["문제"]].drop(columns=["문제"]).sort_values(["칸", "손익_원", "매수일", "종목코드"], kind="mergesort")
    rounded = problem.copy()

    for column in rounded.columns:
        if rounded[column].dtype.kind == "f":
            rounded[column] = rounded[column].round(6)

    common.write_csv(study.OUTPUT / "problem_trades.csv", rounded)

    # 원인별 표
    cause = problem.groupby(["칸", "주된_손실_원인"], sort=True).agg(건수=("손익_원", "size"),
                                                              손실_합계_원=("손익_원", lambda v: v[v < 0].sum()),
                                                              지수대비_부족액_합계_원=("지수대비_부족액_원", "sum")).reset_index()
    total = problem.groupby("주된_손실_원인", sort=True).agg(건수=("손익_원", "size"),
                                                       손실_합계_원=("손익_원", lambda v: v[v < 0].sum()),
                                                       지수대비_부족액_합계_원=("지수대비_부족액_원", "sum")).reset_index()
    total.insert(0, "칸", "대표 칸 합계")
    common.write_csv(study.OUTPUT / "problem_causes.csv", pd.concat([cause, total], ignore_index=True).round(0))

    # 문제 종목
    symbol_rows = []

    for code, part in trades.groupby("종목코드", sort=True):
        pnl = part["손익_원"].sum()
        excess_won = (part["낸_돈"] * part["지수_대비_초과"]).sum()

        if not (pnl < 0 or excess_won < 0):
            continue

        problem_part = part[part["문제"]]
        weight = problem_part.groupby("주된_손실_원인")["손익_원"].apply(lambda v: -v[v < 0].sum())
        main_cause = weight.idxmax() if len(weight) and weight.max() > 0 else (
            problem_part["주된_손실_원인"].mode().iloc[0] if len(problem_part) else "")
        symbol_rows.append({"종목코드": code, "종목명": part["종목명"].iloc[0], "시장": part["시장"].iloc[-1],
                            "매매_수": len(part), "문제_매매_수": len(problem_part), "누적_손익_원": round(pnl, 0),
                            "지수_대비_누적_초과_원": round(excess_won, 0),
                            "손실_매매_비중": round(float((part["손익_원"] < 0).mean()), 4),
                            "주된_손실_원인": main_cause, "칸": "·".join(sorted(part["칸"].unique()))})

    symbols = pd.DataFrame(symbol_rows).sort_values(["누적_손익_원", "종목코드"], kind="mergesort")
    common.write_csv(study.OUTPUT / "problem_symbols.csv", symbols)

    # 차트 자료
    CHART.mkdir(parents=True, exist_ok=True)

    for old in CHART.glob("*.json"):
        old.unlink()

    blobs = {}
    problem_codes = set(symbols["종목코드"])
    stock_of = {code: index for index, code in enumerate(codes)}

    for code, part in trades[trades["종목코드"].isin(problem_codes)].groupby("종목코드", sort=True):
        stock = stock_of[code]
        valid = np.flatnonzero(np.isfinite(inputs.close[:, stock]))
        first = np.searchsorted(valid, part["buy_day"].min())
        last = np.searchsorted(valid, part["sell_day"].max(), side="right") - 1
        chosen = valid[max(first - BEFORE_BARS, 0):min(last + AFTER_BARS, len(valid) - 1) + 1]
        bars = [[int(dates[day]), round(float(inputs.open[day, stock]), 2), round(float(inputs.high[day, stock]), 2),
                 round(float(inputs.low[day, stock]), 2), round(float(inputs.close[day, stock]), 2),
                 int(inputs.volume[day, stock]) if np.isfinite(inputs.volume[day, stock]) else 0] for day in chosen]
        trade_items = [{"cell": trade.칸, "buy_date": int(trade.매수일), "buy_px": round(float(trade.매수가), 2),
                        "sell_date": int(trade.매도일), "sell_px": round(float(trade.매도가), 2),
                        "ret_net": round(float(trade.순수익률) * 100, 2), "excess": round(float(trade.지수_대비_초과) * 100, 2),
                        "mae": round(float(trade.MAE) * 100, 2), "mae_date": int(trade.MAE_날짜),
                        "mfe": round(float(trade.MFE) * 100, 2), "mfe_date": int(trade.MFE_날짜),
                        "cause": trade.주된_손실_원인 if trade.문제 else "문제 아님",
                        "events": [event for event in trade.사건.split("·") if event]}
                       for trade in part.sort_values(["매수일", "칸"], kind="mergesort").itertuples()]
        payload = {"name": part["종목명"].iloc[0], "market": part["시장"].iloc[-1], "bars": bars, "trades": trade_items}
        blobs[code] = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

    files = split_files(blobs)
    sizes = {}

    for prefix, members in files.items():
        text = "{" + ",".join(f"\"{code}\":{blobs[code]}" for code in members) + "}\n"
        path = CHART / f"{prefix}.json"
        common.write_text(path, text)
        sizes[path.name] = len(text.encode("utf-8"))

    index_payload = {"kospi": [[int(dates[day]), round(float(inputs.kospi_close[day]), 2)] for day in range(inputs.day_count)
                               if np.isfinite(inputs.kospi_close[day])],
                     "kosdaq": [[int(dates[day]), round(float(inputs.kosdaq_close[day]), 2)] for day in range(inputs.day_count)
                                if np.isfinite(inputs.kosdaq_close[day])]}
    common.write_json(CHART / "index_daily.json", index_payload, indent=None)
    sizes["index_daily.json"] = (CHART / "index_daily.json").stat().st_size
    info = {"trades_all": int(len(trades)), "problem_trades": int(len(problem)), "problem_symbols": int(len(symbols)),
            "chart_files": len(sizes), "chart_bytes": int(sum(sizes.values())), "largest_file": max(sizes.values()),
            "units": "ret_net·excess·mae·mfe 는 % (소수 2자리), 가격은 정제 수정가", "seconds": round(time.time() - started, 1)}
    common.write_json(study.OUTPUT / "problem_info.json", info)
    print(json.dumps(info, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
