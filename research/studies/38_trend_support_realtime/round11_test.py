"""스터디 38 11회차 — 편향 감사(9회차 보류) 지적을 반영해 2–5억·5–10억 균등 바구니를 크기 맞춤 대조로 다시 본다.

사용자 원래 질문(추세가 꺾인 자리에서 파는 손절 자리)과 다른 갈래다(지시 2026-10-11, 감사 스크립트 audit9·9b·9c 를 따라 씀).

실행 전에 고정한 칸(내가 정함, 지시 문구 그대로): 보유 {20, 60} 시장일 × 구간 {2–5억, 5–10억} × 판정 값 {시가총액 대조, 가격·변동성 대조}
  × 매매 방식 {매번 전부 다시 삼, 남는 종목 유지} = 16 칸, 주문 금액 100만(9·10회차와 같음).

고친 것
 1. 정지·폐지. 매도일에 거래가 없으면 다음 거래일(거래 재개일) 종가에 판다. 그 뒤 거래가 없으면(상장폐지·자료 끝까지 정지):
    KIND 상장폐지 사유가 합병·신청·이전상장·완전자회사화·해산·존속기간 만료면 마지막 종가, 그 밖(부실·사유 모름)은
    자료 마지막 7 거래일 수익이 −50% 아래(정리매매가 자료에 들어 있음, 감사 기준)면 마지막 종가, 아니면 마지막 종가 −90%(내가 정함, 감사 제안값).
    자료 끝까지 정지 중이고 KIND 에 폐지 기록이 없는 종목은 마지막 종가(내가 정함).
    정지를 건넌 가격 점프: 행 사이 시장일 간격 ≥ 5 이고 비율 > 2.5 또는 < 0.4 이면, 양쪽 상장주식수(shares_point_in_time, 그날 이전 최근값)가
    있고 1.5 배 넘게 바뀌었으면 비율 × 주식수 비율(무수정 감자·분할), 주식수로 설명 안 되는 상승(> 2.5)은 비율 1(자료 오류, 내가 정함),
    설명 안 되는 하락은 그대로 둔다(보수적). 052670 제일바이오: 2023-07-20 2,080원 → 정지 621 시장일 → 2026-02-09 625,000원(×300.5).
    금융위 월말 표(data.go.kr)는 2026-01 까지 상장주식수 29,129,064 주·종가 2,080원 그대로라 감자 기록이 없다 → 자료 오류로 보고 비율 1,
    그 뒤 정리매매 7 일(625,000 → 320,000, −48.8%)만 수익에 들어가고, 부실 사유 폐지·정리매매 −50% 미만이라 마지막 종가 −90% 를 더 매긴다.
 2. 비용. PYQuant/backtest/costs.py RESEARCH_MID(수수료 0.015%/편, 슬리피지 1 호가, 충격 0.05%/편) + 매도일 기준 연도별 증권거래세
    (표 출처: research/studies/38_trend_stop_intraday/daily_stop_grid.py SELL_TAX_SCHEDULE — 2019-05-29 까지 0.30%, 0.25%, 2021 0.23%,
    2023 0.20%, 2024 0.18%, 2025 0.15%, 2026 0.20%) + 9회차 선형 충격(0.3 × 금액 ÷ 전날까지 20일 평균 거래대금, 편마다).
    호가 단위: 2023-01-25 부터 새 표, 그 전 옛 표(유가증권 기준, 같은 파일의 표). 대조 바구니 종목도 같은 비용·같은 충격 식(자기 거래대금).
    시장 B 는 회전 비용 없는 보유(비용 0, 같은 정지·폐지 처리) — 진단에만 쓴다.
 3. 판정 값 = 바구니 순수익 − 크기 맞춤 대조 순수익(바구니 종목마다 같은 날 같은 묶음 전 종목 균등 총수익에 그 바구니 종목의 비용
    배수를 그대로 곱함 = 같은 비용, 내가 정함). 대조 종목이 자기 거래대금으로 충격을 내게 하면 거래대금 2억 미만 얇은 종목이
    충격 1%/편 넘게 물어 대조가 불리해진다 — 그 판은 진단(value_control_own_cost)으로만 낸다.
    시가총액 10분위: 상장주식수가 있는 2016-01-01 이후 매수분(2016–2019 는 DART 등급 B, 표에 따로). 가격·변동성: 무수정 종가 5분위 × 전날까지
    60일 변동성 5분위(40일 이상), 전 기간. 남는 종목 유지 판은 바구니 종목의 사고팖 여부가 비용 배수에 들어가 대조에도 같이 붙는다.
판정(감사 제안): 시가총액 대조 칸 t ≥ 기준선, 옆 구간(2–5억 ↔ 5–10억, 같은 보유·판정 값·매매 방식) 부호 같음, 양수 해 ≥ 0.7, 평균 > 0.
  기준선 = max(4.16(8회차 가족 (나) 기준선), 이번 16 칸 (가)·(나)). 가격·변동성 칸은 같은 규칙으로 보조 판정.
진단(판정 안 씀): 충격 루트 모형(편마다 1.0 × 60일 일간 변동성 × √(금액 ÷ 20일 평균 거래대금), 내가 정함), 2025–2026 표,
  회전 비용 없는 B 대비 값, 정지·폐지 처리 건수, 정지 점프 고친 목록.
재실행: py research/studies/38_trend_support_realtime/round11_test.py
"""
from __future__ import annotations

import time

import common     # numba 보다 먼저: common 이 NUMBA_NUM_THREADS(12)를 넣는다
import numpy as np
import pandas as pd

import round3_test
import round4_test
import round5_test
import round6_test
import round10_test

OUTPUT = common.STUDY / "results" / "round11"
AMOUNT = 1_000_000.0
HOLDS = (20, 60)
BINS = (2, 3)
BIN_LOWS = {2: 2e8, 3: 5e8}
BIN_HIGHS = {2: 5e8, 3: 1e9}
BIN_LABELS = round6_test.ABSOLUTE_LABELS
CONTROLS = ("시가총액 대조(2016–)", "가격·변동성 대조")
MODES = ("매번 전부 다시 삼", "남는 종목 유지")
IMPACTS = ("선형 0.3", "루트 1.0×변동성")
COMMISSION = 0.015 / 100            # RESEARCH_MID
FLAT_IMPACT = 0.05 / 100            # RESEARCH_MID impact_percent
SLIPPAGE_TICKS = 1                  # RESEARCH_MID
LINEAR = 0.3
ROOT = 1.0
SELL_TAX_SCHEDULE = ((20000101, 0.30), (20190530, 0.25), (20210101, 0.23), (20230101, 0.20), (20240101, 0.18),
                     (20250101, 0.15), (20260101, 0.20))
NEW_TICK_FROM = 20230125
CAP_FROM = 20160101
GRADE_A_FROM = 20200102
FAMILY8_BAR = 4.16
HALT_GAP = 5
JUMP_UP = 2.5
JUMP_DOWN = 0.4
SHARE_CHANGE = 1.5
DELIST_LOSS = 0.9
CRASH_TAIL = -0.5
NON_DISTRESS = ("합병", "신청에 의한", "시장 상장", "거래소 상장", "완전자회사", "해산", "존속기간", "이전상장")


def tick_of(price: np.ndarray, date: int) -> np.ndarray:
    if date >= NEW_TICK_FROM:
        return np.select([price < 2000, price < 5000, price < 20000, price < 50000, price < 200000, price < 500000],
                         [1, 5, 10, 50, 100, 500], 1000).astype(float)

    return np.select([price < 1000, price < 5000, price < 10000, price < 50000, price < 100000, price < 500000],
                     [1, 5, 10, 50, 100, 500], 1000).astype(float)


def sell_tax(dates: np.ndarray) -> np.ndarray:
    starts = np.array([day for day, _ in SELL_TAX_SCHEDULE])
    rates = np.array([rate for _, rate in SELL_TAX_SCHEDULE]) / 100.0
    return rates[np.searchsorted(starts, dates, side="right") - 1]


def clean_close(panel, shares_asof: np.ndarray):
    """정지를 건넌 점프를 고친 수익 기준 종가(행 순서). 고친 목록도 낸다."""
    close = panel.close.copy()
    ratio = np.ones(panel.row_count)
    events = []

    for stock in range(panel.stock_count):
        first, last = panel.starts[stock], panel.starts[stock + 1]

        if last - first < 2:
            continue

        step = close[first + 1:last] / close[first:last - 1]
        gap = np.diff(panel.market_position[first:last])
        adjusted = step.copy()

        for index in np.flatnonzero((gap >= HALT_GAP) & ((step > JUMP_UP) | (step < JUMP_DOWN))):
            before, after = shares_asof[first + index], shares_asof[first + index + 1]
            known = np.isfinite(before) and np.isfinite(after) and before > 0 and after > 0
            change = after / before if known else np.nan

            if known and abs(np.log(change)) > np.log(SHARE_CHANGE):
                adjusted[index] = step[index] * change
                kind = "주식수 변화로 고침"
            elif step[index] > JUMP_UP:
                adjusted[index] = 1.0
                kind = "설명 안 되는 상승 → 비율 1"
            else:
                kind = "설명 안 되는 하락 → 그대로"

            events.append({"code": panel.codes[stock], "last_date": int(panel.dates[first + index]),
                           "resume_date": int(panel.dates[first + index + 1]), "gap_market_days": int(gap[index]),
                           "ratio": float(step[index]), "shares_change": float(change), "used_ratio": float(adjusted[index]),
                           "treatment": kind})

        ratio[first + 1:last] = adjusted
        ratio[first] = 1.0
        close[first:last] = close[first] * np.cumprod(ratio[first:last])

    return close, pd.DataFrame(events)


def delisting_table(panel, close: np.ndarray) -> pd.DataFrame:
    """상장 끝 종목마다 폐지 처리(마지막 종가 배수)."""
    kind = pd.read_parquet(common.REPOSITORY / "PYQuant" / "data" / "universe" / "kind_delisted.parquet")
    kind["delist_int"] = pd.to_datetime(kind["delist_date"]).dt.strftime("%Y%m%d").astype(np.int64)
    rows = []

    for stock in range(panel.stock_count):
        first, last = panel.starts[stock], panel.starts[stock + 1]
        last_date = int(panel.dates[last - 1])

        if panel.listed_at_end[stock]:
            continue

        candidates = kind[(kind["code"] == panel.codes[stock]) & (kind["delist_int"] >= last_date)]
        tail = close[last - 1] / close[max(first, last - 8)] - 1.0 if last - first >= 2 else np.nan

        if len(candidates) == 0:
            reason = ""
            category = "자료 끝까지 정지(폐지 기록 없음)" if not (kind["code"] == panel.codes[stock]).any() else "사유 모름"
        else:
            reason = str(candidates.sort_values("delist_int").iloc[0]["reason"])
            category = "부실 아님" if any(keyword in reason for keyword in NON_DISTRESS) else "부실"

        if category in ("부실 아님", "자료 끝까지 정지(폐지 기록 없음)"):
            multiplier = 1.0
        elif np.isfinite(tail) and tail < CRASH_TAIL:
            multiplier = 1.0
        else:
            multiplier = 1.0 - DELIST_LOSS

        rows.append({"stock": stock, "code": panel.codes[stock], "last_date": last_date, "category": category, "reason": reason,
                     "last7_return": float(tail), "multiplier": multiplier})

    return pd.DataFrame(rows)


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    panel = common.Panel()
    round3_test.apply_corrections(panel)
    panel.build_deciles()
    shares_asof = panel.shares_issued
    close_rows, jump_events = clean_close(panel, shares_asof)
    delist = delisting_table(panel, close_rows)
    previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
    volatility = round10_test.past_volatility(close_rows, panel.starts, round10_test.VOLATILITY_DAYS, round10_test.VOLATILITY_MIN)
    market_count, stock_count = panel.market_count, panel.stock_count
    stock_of_row = panel.stock_of_row.astype(np.int64)

    def matrix(values: np.ndarray) -> np.ndarray:
        result = np.full((market_count, stock_count), np.nan)
        result[panel.market_position, stock_of_row] = values
        return result

    close_m = matrix(close_rows)
    traded = np.isfinite(close_m)
    raw_m = matrix(panel.raw_close)
    turnover_m = matrix(previous_20d)
    cap_m = matrix(panel.market_cap)
    volatility_m = matrix(volatility)
    # 다음 거래일 위치(그날 포함), 없으면 −1
    next_traded = np.full((market_count, stock_count), -1, dtype=np.int64)
    running = np.full(stock_count, -1, dtype=np.int64)

    for day in range(market_count - 1, -1, -1):
        running = np.where(traded[day], day, running)
        next_traded[day] = running

    last_position = np.array([panel.market_position[panel.starts[stock + 1] - 1] for stock in range(stock_count)])
    delist_multiplier = np.ones(stock_count)
    delist_multiplier[delist["stock"].to_numpy()] = delist["multiplier"].to_numpy()
    print(f"준비 끝 {time.time() - started:.0f}초, 정지 점프 {len(jump_events)} 건, 폐지 종목 {len(delist)}", flush=True)

    period_rows, exit_rows = [], []

    for hold in HOLDS:
        days = np.arange(panel.signal_start, market_count - hold, hold)
        exits = days + hold
        count = len(days)
        bins_member = {bin_key: [] for bin_key in BINS}
        cap_group, style_group, pool = [], [], []

        for day in days:
            turnover = np.nan_to_num(turnover_m[day], nan=-1.0)

            for absolute_bin in BINS:
                bins_member[absolute_bin].append(traded[day] & (turnover >= max(BIN_LOWS[absolute_bin], AMOUNT / 0.005))
                                                 & (turnover < BIN_HIGHS[absolute_bin]))

            alive = traded[day] & (turnover > 0)
            pool.append(alive)
            market_capitalization = cap_m[day]
            cap_valid = alive & np.isfinite(market_capitalization) & (np.nan_to_num(market_capitalization) > 0)
            cap_group.append(round10_test.quantile_of(market_capitalization, cap_valid, 10) if panel.market_dates[day] >= CAP_FROM
                             else np.full(stock_count, -1, dtype=np.int64))
            price, day_volatility = raw_m[day], volatility_m[day]
            style_valid = alive & np.isfinite(price) & np.isfinite(day_volatility)
            style_group.append(np.where(style_valid, round10_test.quantile_of(price, style_valid, 5) * 5
                                        + round10_test.quantile_of(day_volatility, style_valid, 5), -1))

        groups_by_control = (cap_group, style_group)

        for position in range(count):
            day, exit_day = days[position], exits[position]
            buy_date = int(panel.market_dates[day])
            alive = pool[position]
            # 파는 자리: 매도일 이후 첫 거래일, 없으면 마지막 거래일 × 폐지 배수
            sell_position = next_traded[exit_day]
            gone = sell_position < 0
            sell_position = np.where(gone, last_position, sell_position)
            multiplier = np.where(gone, delist_multiplier, 1.0)
            safe_sell = np.clip(sell_position, 0, market_count - 1)
            stock_index = np.arange(stock_count)
            ratio = np.where(alive, close_m[safe_sell, stock_index] / np.where(alive, close_m[day], 1.0) * multiplier, np.nan)
            sell_raw = raw_m[safe_sell, stock_index] * multiplier
            sell_dates = panel.market_dates[safe_sell]
            buy_raw = raw_m[day]
            buy_tick = tick_of(np.nan_to_num(buy_raw, nan=1.0), buy_date)
            sell_tick = np.where(sell_dates >= NEW_TICK_FROM, tick_of(np.nan_to_num(sell_raw, nan=1.0), NEW_TICK_FROM),
                                 tick_of(np.nan_to_num(sell_raw, nan=1.0), 0))
            buy_cost = buy_raw / (buy_raw + SLIPPAGE_TICKS * buy_tick) / (1.0 + COMMISSION + FLAT_IMPACT)
            sell_cost = (np.maximum(sell_raw - SLIPPAGE_TICKS * sell_tick, 0.0) / np.where(sell_raw > 0, sell_raw, 1.0)
                         * (1.0 - COMMISSION - sell_tax(sell_dates) - FLAT_IMPACT))
            buy_ratio = AMOUNT / np.where(alive, turnover_m[day], 1.0)
            sell_base = turnover_m[safe_sell, stock_index]
            sell_ratio = np.where(np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0), AMOUNT / np.where(sell_base > 0, sell_base, 1.0),
                                  buy_ratio)
            sigma = volatility_m[day]
            sigma = np.where(np.isfinite(sigma), sigma, np.nanmedian(sigma[alive]))
            impact_sides = {IMPACTS[0]: (1.0 / (1.0 + LINEAR * buy_ratio), np.maximum(0.0, 1.0 - LINEAR * sell_ratio)),
                            IMPACTS[1]: (1.0 / (1.0 + ROOT * sigma * np.sqrt(buy_ratio)),
                                         np.maximum(0.0, 1.0 - ROOT * sigma * np.sqrt(sell_ratio)))}
            market_clean = float((np.nanmean(ratio[alive]) - 1.0) * 100.0)
            status = np.where(gone, 2, np.where(sell_position > exit_day, 1, 0))

            def cost_factor(entering, leaving, impact):
                buy_impact, sell_impact = impact_sides[impact]
                return np.where(entering, buy_cost * buy_impact, 1.0) * np.where(leaving, sell_cost * sell_impact, 1.0)

            def net(entering, leaving, impact):
                return (ratio * cost_factor(entering, leaving, impact) - 1.0) * 100.0

            for absolute_bin in BINS:
                member = bins_member[absolute_bin][position]
                members = int(member.sum())

                if members == 0:
                    continue

                exit_rows.append({"hold_days": hold, "absolute_label": BIN_LABELS[absolute_bin], "buy_date": buy_date, "members": members,
                                  "sold_on_exit_day": int((member & (status == 0)).sum()), "sold_after_resume": int((member & (status == 1)).sum()),
                                  "delisted_last_close": int((member & gone & (multiplier == 1.0)).sum()),
                                  "delisted_minus_90": int((member & gone & (multiplier < 1.0)).sum())})
                before = bins_member[absolute_bin][position - 1] if position > 0 else np.zeros(stock_count, dtype=bool)
                after = bins_member[absolute_bin][position + 1] if position + 1 < count else np.zeros(stock_count, dtype=bool)

                for mode in MODES:
                    keep = mode == MODES[1]
                    entering = ~before if keep else np.ones(stock_count, dtype=bool)
                    leaving = ~after if keep else np.ones(stock_count, dtype=bool)

                    for impact in IMPACTS:
                        member_factor = cost_factor(entering, leaving, impact)
                        member_net = (ratio * member_factor - 1.0) * 100.0
                        basket = float(member_net[member].mean())

                        for control_index, control in enumerate(CONTROLS):
                            group = groups_by_control[control_index][position]
                            known = member & (group >= 0)

                            if known.sum() == 0:
                                continue

                            if keep:
                                group_before = groups_by_control[control_index][position - 1] if position > 0 else None
                                group_after = groups_by_control[control_index][position + 1] if position + 1 < count else None
                                control_entering = (np.ones(stock_count, dtype=bool) if group_before is None
                                                    else ~(pool[position - 1] & (group_before == group)))
                                control_leaving = (np.ones(stock_count, dtype=bool) if group_after is None
                                                   else ~(pool[position + 1] & (group_after == group)))
                            else:
                                control_entering = control_leaving = np.ones(stock_count, dtype=bool)

                            # 판정: 대조 묶음 총수익에 맞춘 바구니 종목의 비용을 그대로 붙인다(같은 비용).
                            usable = alive & (group >= 0) & np.isfinite(ratio)
                            minimum = int(group.max()) + 1
                            sizes = np.bincount(group[usable], minlength=minimum)
                            gross_mean = np.bincount(group[usable], weights=ratio[usable], minlength=minimum) / np.maximum(sizes, 1)
                            control_value = float(((gross_mean[group[known]] * member_factor[known] - 1.0) * 100.0).mean())
                            # 진단: 대조 종목이 자기 거래대금으로 충격을 내는 판(얇은 종목 충격이 커서 대조가 불리해진다)
                            own_net = net(control_entering, control_leaving, impact)
                            own_ok = usable & np.isfinite(own_net)
                            own_mean = (np.bincount(group[own_ok], weights=own_net[own_ok], minlength=minimum)
                                        / np.maximum(np.bincount(group[own_ok], minlength=minimum), 1))
                            control_own = float(own_mean[group[known]].mean())
                            period_rows.append({
                                "hold_days": hold, "absolute_bin": absolute_bin, "absolute_label": BIN_LABELS[absolute_bin],
                                "control": control, "mode": mode, "impact": impact, "buy_date": buy_date,
                                "sell_date": int(panel.market_dates[exit_day]), "year": buy_date // 10000,
                                "month": int((buy_date // 10000 - panel.first_year) * 12 + (buy_date // 100) % 100 - 1),
                                "members": members, "members_matched": int(known.sum()),
                                "entering": int((member & entering).sum()), "leaving": int((member & leaving).sum()),
                                "basket_net_percent": basket, "basket_matched_net_percent": float(member_net[known].mean()),
                                "control_net_percent": control_value, "market_clean_no_cost_percent": market_clean,
                                "value": float(member_net[known].mean()) - control_value,
                                "value_vs_market_no_cost": basket - market_clean,
                                "control_own_cost_net_percent": control_own,
                                "value_control_own_cost": float(member_net[known].mean()) - control_own})

        print(f"보유 {hold} 끝 {time.time() - started:.0f}초", flush=True)

    periods = pd.DataFrame(period_rows)
    common.write_csv(OUTPUT / "periods.csv", periods)
    common.write_csv(OUTPUT / "jump_fixes.csv", jump_events)
    common.write_csv(OUTPUT / "delisting.csv", delist)
    common.write_csv(OUTPUT / "exit_handling.csv", pd.DataFrame(exit_rows).groupby(["hold_days", "absolute_label"]).sum(numeric_only=True)
                     .drop(columns=["buy_date"]).reset_index())

    # 판정: 선형 충격 16 칸
    judged = periods[(periods["impact"] == IMPACTS[0]) & np.isfinite(periods["value"])]
    keys = [(hold, control, mode, absolute_bin) for hold in HOLDS for control in CONTROLS for mode in MODES for absolute_bin in BINS]
    cell_of = {key: index for index, key in enumerate(keys)}
    cell = np.array([cell_of[key] for key in zip(judged["hold_days"], judged["control"], judged["mode"], judged["absolute_bin"])])
    month_count = panel.month_count
    years = list(range(panel.first_year, panel.first_year + panel.year_count))
    month_sum = np.zeros((len(keys), month_count))
    month_n = np.zeros((len(keys), month_count))
    year_sum = np.zeros((len(keys), panel.year_count, 2))
    np.add.at(month_sum, (cell, judged["month"].to_numpy()), judged["value"].to_numpy())
    np.add.at(month_n, (cell, judged["month"].to_numpy()), 1.0)
    year_index = judged["year"].to_numpy() - panel.first_year
    np.add.at(year_sum, (cell, year_index, 0), 1.0)
    np.add.at(year_sum, (cell, year_index, 1), judged["value"].to_numpy())
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    frame = pd.DataFrame(keys, columns=["hold_days", "control", "mode", "turnover_decile"])
    frame["order_amount_krw"] = AMOUNT
    frame["absolute_label"] = np.array(BIN_LABELS)[frame["turnover_decile"].to_numpy()]
    table, summary = round5_test.family_table("11회차 16 칸", month_sum, month_n, year_sum, frame, years, signs,
                                              ["family", "hold_days", "control", "mode"])
    table = table.rename(columns={"turnover_decile": "absolute_bin", "n": "rebalances"})
    bar = max(FAMILY8_BAR, summary["bar_a_24_months"], summary["bar_b_normal_equivalent_z"])
    table["bar_used"] = bar
    table["passes_final"] = ((table["t_excess"] >= bar) & table["neighbours_all_same_sign"]
                             & (table["share_positive_excess_years"] >= round5_test.JUDGE_POSITIVE_YEARS) & (table["mean_excess"] > 0))
    table["headline"] = table["control"] == CONTROLS[0]
    common.write_csv(OUTPUT / "cell_table.csv", table)
    common.write_json(OUTPUT / "multiple_comparison.json", {"cells_fixed": len(keys), "family": summary, "bar_used": bar,
                                                            "bar_rule": "max(4.16 8회차 가족, 16 칸 (가), 16 칸 (나))"})

    # 진단: 구간별 표(2016–2019 등급 B 따로, 2025–2026), 충격 모형, 비용 없는 B 대비
    spans = (("전 기간", 0, 99999999), ("2010–2015", 20100101, 20151231), ("2016–2019(등급 B)", 20160101, 20191231),
             ("2020–2024", 20200101, 20241231), ("2025–2026", 20250101, 20261231))
    diagnostic_rows = []

    for (hold, control, mode, impact, absolute_bin), part in periods.groupby(["hold_days", "control", "mode", "impact", "absolute_bin"]):
        for name, low, high in spans:
            piece = part[(part["buy_date"] >= low) & (part["buy_date"] <= high) & np.isfinite(part["value"])]

            if len(piece) < 3:
                continue

            diagnostic_rows.append({"hold_days": hold, "control": control, "mode": mode, "impact": impact,
                                    "absolute_label": BIN_LABELS[absolute_bin], "span": name, "rebalances": len(piece),
                                    "mean_value": float(piece["value"].mean()),
                                    "t_value": round6_test.month_cluster_t(piece["value"].to_numpy(), piece["month"].to_numpy()),
                                    "mean_basket_net": float(piece["basket_net_percent"].mean()),
                                    "mean_control_net": float(piece["control_net_percent"].mean()),
                                    "mean_market_no_cost": float(piece["market_clean_no_cost_percent"].mean()),
                                    "mean_vs_market_no_cost": float(piece["value_vs_market_no_cost"].mean()),
                                    "t_vs_market_no_cost": round6_test.month_cluster_t(piece["value_vs_market_no_cost"].to_numpy(),
                                                                                       piece["month"].to_numpy()),
                                    "mean_value_control_own_cost": float(piece["value_control_own_cost"].mean()),
                                    "t_value_control_own_cost": round6_test.month_cluster_t(piece["value_control_own_cost"].to_numpy(),
                                                                                            piece["month"].to_numpy()),
                                    "entering_share": float((piece["entering"] / piece["members"]).median()),
                                    "orders_total": int((piece["entering"] + piece["leaving"]).sum())})

    common.write_csv(OUTPUT / "diagnostics.csv", pd.DataFrame(diagnostic_rows))

    history = pd.read_csv(common.STUDY / "results" / "round10" / "choices_log.csv")
    new_rows = pd.DataFrame([
        (11, "보유 {20,60} × 구간 {2–5억,5–10억} × 판정 값 {시가총액 대조, 가격·변동성 대조} × 매매 방식 {전부 다시 삼, 남는 종목 유지}",
         "내가 정함(지시 문구)", "예", "칸 16", "감사 뒤 다시 — 원래 질문과 다른 갈래, 실행 전 고정"),
        (11, "기준선 max(4.16, 16 칸 (가)·(나)), 판정은 시가총액 대조 칸", "지시(감사 제안)", "예", "–", ""),
        (11, "정지 매도 = 재개일 종가, 부실 폐지·정리매매 없음 −90%, 정지 점프 고침(간격 ≥5, >2.5·<0.4, 주식수 1.5 배)", "지시 + 내가 정함(문턱값)",
         "아니오", "–", "052670 은 비율 1"),
        (11, "비용 RESEARCH_MID + 연도별 매도세 + 선형 충격 0.3, B 는 비용 없는 보유", "지시", "아니오", "–", ""),
        (11, "루트 충격 1.0 × 변동성 × √비율(진단)", "내가 정함", "아니오", "모형 × 2", "판정 안 씀"),
        (11, "대조에 바구니 종목과 같은 비용 배수(판정), 대조 자기 비용 판은 진단", "내가 정함(지시 해석)", "아니오", "–",
         "첫 실행에서 자기 비용 판이 대조를 불리하게 만든 것을 보고 판정 값을 바꿈 — 칸 수 그대로"),
    ], columns=history.columns)
    common.write_csv(OUTPUT / "choices_log.csv", pd.concat([history, new_rows], ignore_index=True))
    common.write_json(OUTPUT / "round11_metrics.json", {
        "reproduction": panel.reproduction(), "seconds": time.time() - started, "cells_fixed": len(keys), "bar_used": bar,
        "jump_fixes_by_treatment": jump_events["treatment"].value_counts().to_dict() if len(jump_events) else {},
        "delisting_by_category": delist.groupby("category")["multiplier"].agg(["size", lambda x: int((x < 1).sum())]).rename(
            columns={"size": "stocks", "<lambda_0>": "minus_90"}).to_dict("index"),
        "passing_cells": table[table["passes_final"]][["hold_days", "control", "mode", "absolute_label", "rebalances", "mean_excess",
                                                       "t_excess", "share_positive_excess_years"]].to_dict("records"),
        "hand_set": {"cells": "16 칸 구성(지시 문구) — 내가 정함", "delist_loss": "부실 폐지·정리매매 없음 −90% — 내가 정함(감사 제안)",
                     "jump": "정지 간격 ≥5 시장일, 비율 >2.5·<0.4, 주식수 변화 1.5 배, 설명 안 되는 상승은 비율 1 — 내가 정함",
                     "root_impact": "루트 모형 계수 1.0, 변동성 결측은 그날 중앙값 — 내가 정함",
                     "old_tick": "2023-01-25 전 호가는 유가증권 표(코스닥 5만원 이상 100원 차이 무시) — 내가 정함",
                     "halted_to_end": "자료 끝까지 정지·폐지 기록 없음은 마지막 종가 — 내가 정함"}})
    print(f"전체 {time.time() - started:.0f}초, 기준선 {bar:.2f}, 통과 {int(table['passes_final'].sum())} 칸", flush=True)


if __name__ == "__main__":
    main()
