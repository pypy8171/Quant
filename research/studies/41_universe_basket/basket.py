"""스터디 41 바구니 시뮬레이터(numba) — 칸 하나 = (종목군, 신호, N, 리밸런싱, 체결, 버퍼, 자본, 비용, 정수 주식).

하루 순서(한 시장일 t):
  1. 상장폐지: 마지막 거래일 다음 날 아침에 마지막 종가 × 폐지 배수 × (1 − 수수료 − 매도세)로 현금화.
  2. 시가: 체결이 '다음 날 시가'면 이 시점에 매도 대기분 → 리밸런싱(매도 → 규모 맞춤 → 매수).
  3. 종가: 체결이 '같은 날 종가'·'다음 날 종가'면 이 시점에.
  4. 순자산 = 현금 + Σ 보유 단위 × 그날 종가(정지면 마지막 종가).
리밸런싱:
  - 결정일 d 의 점수·종목군(d−1 까지·가격 신호는 d 종가까지)으로 순위를 낸다. 체결일은 d(같은 날 종가) 또는 d+1.
  - 버퍼: 보유 종목이 순위 1.5N 안이면 유지하고, 나머지 칸을 위에서부터 채운다.
  - 규모 맞춤: 체결 직전 순자산을 자본 C 로 맞춘다(넘거나 모자란 만큼 현금 출입, 수익률 계산에서는 뺀다).
  - 엔진 밴드: 유지 종목은 |보유 금액 − 목표| ≥ 10% × 목표일 때만 차이만큼 사고판다(TARGET_BASKET 과 같다).
  - 상한가 잠김이면 매수 막음, 하한가 잠김·정지면 매도 대기(다음 거래 가능 체결 시점). 다음 날 체결에서 못 산 칸은 현금.
비용(cost_on=1): 수수료 0.015% + 충격 0.05% + 호가 ticks 틱 + 선형 0.3 × 주문 ÷ 전날까지 20일 평균 거래대금, 매도세는 매도일 세율.
정수 주식(integer=1): 수량 = floor(금액 ÷ 1주 비용). 사는 가격 조건(결정일 무수정 종가 ≤ C/N)은 후보 단계에서 거른다.
"""
from __future__ import annotations

import numpy as np
from numba import njit, prange

COMMISSION = 0.015 / 100
FLAT_IMPACT = 0.05 / 100
LINEAR = 0.3
BAND = 0.10
BUFFER = 1.5
SIGNAL_RANDOM = -1        # 결정일마다 새로 뽑는 무작위 점수(단계 A, 단계 B 참고 열)
SIGNAL_EQUAL_ALL = -2
SIGNAL_RANDOM_FIXED = -3  # 시드마다 종목 점수를 한 번 뽑아 고정(단계 B 판정 대조, choices_log 41행)
EXEC_SAME_CLOSE, EXEC_NEXT_OPEN, EXEC_NEXT_CLOSE = 0, 1, 2
LOG_INT = 7      # stock, buy_day, decision, rank, sell_day, kind(0 매도 1 폐지 2 미청산), candidates
LOG_FLOAT = 7    # buy_price, cash_in, cash_out, sell_price, score, cost, first_cash
SCALARS = 10     # log on, log id, log cost, bench log on, bench log id, traded/nav, buys, invested days, flows, cost krw


@njit(cache=True)
def tick_size(price, new):
    if new:
        if price < 2000:
            return 1.0
        if price < 5000:
            return 5.0
        if price < 20000:
            return 10.0
        if price < 50000:
            return 50.0
        if price < 200000:
            return 100.0
        if price < 500000:
            return 500.0
        return 1000.0

    if price < 1000:
        return 1.0
    if price < 5000:
        return 5.0
    if price < 10000:
        return 10.0
    if price < 50000:
        return 50.0
    if price < 100000:
        return 100.0
    if price < 500000:
        return 500.0
    return 1000.0


@njit(cache=True)
def random_key(seed, decision, stock):
    """splitmix64 로 만든 [0, 1) 값. 같은 (seed, 결정, 종목)이면 같다."""
    value = np.uint64(seed) * np.uint64(1000003) + np.uint64(decision) * np.uint64(8191) + np.uint64(stock)
    value = value + np.uint64(0x9E3779B97F4A7C15)
    value = (value ^ (value >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    value = (value ^ (value >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    value = value ^ (value >> np.uint64(31))
    return float(value >> np.uint64(11)) / 9007199254740992.0


@njit(cache=True)
def simulate_one(signal, ubit, slots, rebalance, execution, use_buffer, capital, ticks, cost_on, integer, seed,
                 open_price, close_price, raw_open, raw_close, previous_20d, lock, market, stock_last, delist_multiplier,
                 kospi_open, kospi_close, kosdaq_open, kosdaq_close, tax, new_tick,
                 decision_days, weekly_end, monthly_end, scores, universe,
                 out_return, out_bench, out_scalar, contrib, do_contrib, log_int, log_float, log_capacity):
    day_count, stock_count = close_price.shape
    units = np.zeros(stock_count)
    held = np.zeros(stock_count, dtype=np.bool_)
    pending = np.zeros(stock_count, dtype=np.bool_)
    in_target = np.zeros(stock_count, dtype=np.bool_)
    rank_of = np.full(stock_count, -1, dtype=np.int64)
    last_price = np.full(stock_count, np.nan)
    position_log = np.full(stock_count, -1, dtype=np.int64)
    held_list = np.empty(stock_count, dtype=np.int64)
    held_count = 0
    log_count = 0
    candidate = np.empty(stock_count, dtype=np.int64)
    key = np.empty(stock_count)
    target = np.empty(stock_count, dtype=np.int64)
    exec_of_day = np.full(day_count, -1, dtype=np.int64)

    for index in range(decision_days.shape[0]):
        day = decision_days[index]
        is_rebalance = weekly_end[day] if rebalance == 0 else monthly_end[day]

        if not is_rebalance:
            continue

        when = day if execution == EXEC_SAME_CLOSE else day + 1

        if when < day_count:
            exec_of_day[when] = index

    cash = capital
    nav_previous = capital
    kosdaq_weight = 0.0
    started = False

    for scalar in range(SCALARS):
        out_scalar[scalar] = 0.0

    for day in range(day_count):
        out_return[day] = np.nan
        out_bench[day] = np.nan

        # 1. 상장폐지 현금화
        index = 0

        while index < held_count:
            stock = held_list[index]

            if stock_last[stock] < day:
                gross = units[stock] * last_price[stock] * delist_multiplier[stock]
                proceeds = gross * (1.0 - COMMISSION - tax[day]) if cost_on else gross
                cash += proceeds
                out_scalar[9] += gross - proceeds

                if log_capacity > 0 and position_log[stock] >= 0:
                    entry = position_log[stock]
                    log_float[entry, 2] += proceeds
                    log_float[entry, 3] = last_price[stock] * delist_multiplier[stock]
                    log_float[entry, 5] += gross - proceeds
                    log_int[entry, 4] = day
                    log_int[entry, 5] = 1
                    position_log[stock] = -1

                units[stock] = 0.0
                held[stock] = False
                pending[stock] = False
                held_count -= 1
                held_list[index] = held_list[held_count]
                continue

            index += 1

        # 시작 시점 시장 구성(지수 비교용)과 기여 기준
        value_kospi = 0.0
        value_kosdaq = 0.0

        for index in range(held_count):
            stock = held_list[index]
            amount = units[stock] * last_price[stock]

            if market[day, stock] == 1 or (market[day, stock] < 0 and day > 0 and market[day - 1, stock] == 1):
                value_kosdaq += amount
            else:
                value_kospi += amount

        if value_kospi + value_kosdaq > 0:
            kosdaq_weight = value_kosdaq / (value_kospi + value_kosdaq)

        weight_overnight = kosdaq_weight

        if do_contrib and day > 0:
            for index in range(held_count):
                stock = held_list[index]
                price = close_price[day, stock]

                if np.isfinite(price) and nav_previous > 0:
                    bench_day = ((1.0 - kosdaq_weight) * (kospi_close[day] / kospi_close[day - 1] - 1.0)
                                 + kosdaq_weight * (kosdaq_close[day] / kosdaq_close[day - 1] - 1.0))
                    contrib[stock] += units[stock] * last_price[stock] / nav_previous * (price / last_price[stock] - 1.0 - bench_day)

        # 2·3. 시가 → 종가
        factor = 1.0
        flow_total = 0.0
        base = 0.0

        for phase in range(2):
            if phase == 0:
                price_row = open_price[day]
                raw_row = raw_open[day]
                up_bit, down_bit = 1, 2
            else:
                price_row = close_price[day]
                raw_row = raw_close[day]
                up_bit, down_bit = 4, 8

            value_now = 0.0

            for index in range(held_count):
                stock = held_list[index]
                price = price_row[stock]
                value_now += units[stock] * (price if np.isfinite(price) else last_price[stock])

            nav_now = cash + value_now

            if phase == 0:
                factor *= nav_now / nav_previous if nav_previous > 0 else 1.0
            else:
                factor *= nav_now / base if base > 0 else 1.0

            trade_phase = 0 if execution == EXEC_NEXT_OPEN else 1
            decision = exec_of_day[day]
            has_pending = False

            for index in range(held_count):
                if pending[held_list[index]]:
                    has_pending = True
                    break

            if phase == trade_phase and (decision >= 0 or has_pending):
                if decision >= 0:
                    started = True

                traded = 0.0
                # 매도 대기분
                index = 0

                while index < held_count:
                    stock = held_list[index]

                    if pending[stock] and np.isfinite(price_row[stock]) and (lock[day, stock] & down_bit) == 0:
                        shares_value = units[stock] * price_row[stock]
                        raw = raw_row[stock]
                        proceeds = shares_value

                        if cost_on:
                            impact = LINEAR * shares_value / previous_20d[day, stock] if previous_20d[day, stock] > 0 else 0.0
                            proceeds = (shares_value * max(raw - ticks * tick_size(raw, new_tick[day]), tick_size(raw, new_tick[day])) / raw
                                        * (1.0 - COMMISSION - tax[day] - FLAT_IMPACT) * max(0.0, 1.0 - impact))

                        cash += proceeds
                        traded += shares_value
                        out_scalar[9] += shares_value - proceeds

                        if log_capacity > 0 and position_log[stock] >= 0:
                            entry = position_log[stock]
                            log_float[entry, 2] += proceeds
                            log_float[entry, 3] = price_row[stock]
                            log_float[entry, 5] += shares_value - proceeds
                            log_int[entry, 4] = day
                            log_int[entry, 5] = 0
                            position_log[stock] = -1

                        units[stock] = 0.0
                        held[stock] = False
                        pending[stock] = False
                        held_count -= 1
                        held_list[index] = held_list[held_count]
                        continue

                    index += 1

                if decision >= 0:
                    decided = decision_days[decision]
                    count = 0

                    for stock in range(stock_count):
                        if (universe[decision, stock] >> ubit) & 1 == 0:
                            continue

                        if not np.isfinite(close_price[decided, stock]) or pending[stock]:
                            continue

                        if integer and signal != SIGNAL_EQUAL_ALL and not (raw_close[decided, stock] <= capital / slots):
                            continue

                        if signal >= 0:
                            score = scores[signal, decision, stock]

                            if not np.isfinite(score):
                                continue

                            key[count] = -score
                        elif signal == SIGNAL_RANDOM:
                            key[count] = random_key(seed, decision, stock)
                        elif signal == SIGNAL_RANDOM_FIXED:
                            key[count] = random_key(seed, 0, stock)
                        else:
                            key[count] = 0.0

                        candidate[count] = stock
                        count += 1

                    order = np.argsort(key[:count], kind="mergesort")
                    picked = 0

                    if signal == SIGNAL_EQUAL_ALL:
                        for position in range(count):
                            target[picked] = candidate[order[position]]
                            picked += 1
                    else:
                        limit = min(slots, count)

                        for position in range(count):
                            rank_of[candidate[order[position]]] = position

                        if use_buffer:
                            keep_rank = int(BUFFER * slots)

                            for index in range(held_count):
                                stock = held_list[index]

                                if not pending[stock] and rank_of[stock] >= 0 and rank_of[stock] < keep_rank and picked < limit:
                                    target[picked] = stock
                                    in_target[stock] = True
                                    picked += 1

                        for position in range(count):
                            if picked >= limit:
                                break

                            stock = candidate[order[position]]

                            if not in_target[stock]:
                                target[picked] = stock
                                in_target[stock] = True
                                picked += 1

                    for position in range(picked):
                        in_target[target[position]] = True

                    # 목표 밖 보유 매도
                    index = 0

                    while index < held_count:
                        stock = held_list[index]

                        if in_target[stock] or pending[stock]:
                            index += 1
                            continue

                        if np.isfinite(price_row[stock]) and (lock[day, stock] & down_bit) == 0:
                            shares_value = units[stock] * price_row[stock]
                            raw = raw_row[stock]
                            proceeds = shares_value

                            if cost_on:
                                impact = LINEAR * shares_value / previous_20d[day, stock] if previous_20d[day, stock] > 0 else 0.0
                                proceeds = (shares_value * max(raw - ticks * tick_size(raw, new_tick[day]), tick_size(raw, new_tick[day])) / raw
                                            * (1.0 - COMMISSION - tax[day] - FLAT_IMPACT) * max(0.0, 1.0 - impact))

                            cash += proceeds
                            traded += shares_value
                            out_scalar[9] += shares_value - proceeds

                            if log_capacity > 0 and position_log[stock] >= 0:
                                entry = position_log[stock]
                                log_float[entry, 2] += proceeds
                                log_float[entry, 3] = price_row[stock]
                                log_float[entry, 5] += shares_value - proceeds
                                log_int[entry, 4] = day
                                log_int[entry, 5] = 0
                                position_log[stock] = -1

                            units[stock] = 0.0
                            held[stock] = False
                            held_count -= 1
                            held_list[index] = held_list[held_count]
                            continue

                        pending[stock] = True
                        index += 1

                    # 규모 맞춤
                    value_now = 0.0

                    for index in range(held_count):
                        stock = held_list[index]
                        price = price_row[stock]
                        value_now += units[stock] * (price if np.isfinite(price) else last_price[stock])

                    flow = capital - (cash + value_now)
                    cash += flow
                    flow_total += flow
                    out_scalar[8] += abs(flow)
                    each = capital / max(picked, 1) if signal == SIGNAL_EQUAL_ALL else capital / slots

                    # 유지 종목 줄이기(밴드)
                    for position in range(picked):
                        stock = target[position]

                        if not held[stock] or pending[stock] or not np.isfinite(price_row[stock]) or (lock[day, stock] & down_bit) != 0:
                            continue

                        current = units[stock] * price_row[stock]

                        if current - each < BAND * each:
                            continue

                        raw = raw_row[stock]
                        sell_value = current - each

                        if integer:
                            sell_value = np.floor(sell_value / raw) * raw

                        if sell_value <= 0:
                            continue

                        proceeds = sell_value

                        if cost_on:
                            impact = LINEAR * sell_value / previous_20d[day, stock] if previous_20d[day, stock] > 0 else 0.0
                            proceeds = (sell_value * max(raw - ticks * tick_size(raw, new_tick[day]), tick_size(raw, new_tick[day])) / raw
                                        * (1.0 - COMMISSION - tax[day] - FLAT_IMPACT) * max(0.0, 1.0 - impact))

                        cash += proceeds
                        traded += sell_value
                        out_scalar[9] += sell_value - proceeds
                        units[stock] -= sell_value / price_row[stock]

                        if log_capacity > 0 and position_log[stock] >= 0:
                            log_float[position_log[stock], 2] += proceeds
                            log_float[position_log[stock], 5] += sell_value - proceeds

                    # 사기(순위 순)
                    for position in range(picked):
                        stock = target[position]
                        price = price_row[stock]

                        if pending[stock] or not np.isfinite(price) or (lock[day, stock] & up_bit) != 0:
                            continue

                        current = units[stock] * price if held[stock] else 0.0

                        if held[stock] and each - current < BAND * each:
                            continue

                        wanted_amount = min(each - current, cash)

                        if wanted_amount <= 0:
                            continue

                        raw = raw_row[stock]
                        per_share = raw

                        if cost_on:
                            impact = LINEAR * wanted_amount / previous_20d[day, stock] if previous_20d[day, stock] > 0 else 0.0
                            per_share = (raw + ticks * tick_size(raw, new_tick[day])) * (1.0 + COMMISSION + FLAT_IMPACT) * (1.0 + impact)

                        shares = wanted_amount / per_share

                        if integer:
                            shares = np.floor(shares)

                        if shares <= 0:
                            continue

                        spend = shares * per_share
                        notional = shares * raw
                        cash -= spend
                        traded += notional
                        out_scalar[9] += spend - notional
                        out_scalar[6] += 1.0
                        units[stock] += notional / price

                        if log_capacity > 0:
                            if not held[stock] and log_count < log_capacity:
                                entry = log_count
                                log_count += 1
                                position_log[stock] = entry
                                log_int[entry, 0] = stock
                                log_int[entry, 1] = day
                                log_int[entry, 2] = decision
                                log_int[entry, 3] = rank_of[stock]
                                log_int[entry, 4] = -1
                                log_int[entry, 5] = 2
                                log_int[entry, 6] = count
                                log_float[entry, 0] = price
                                log_float[entry, 1] = spend
                                log_float[entry, 2] = 0.0
                                log_float[entry, 3] = np.nan
                                log_float[entry, 4] = scores[signal, decision, stock] if signal >= 0 else 0.0
                                log_float[entry, 5] = spend - notional
                                log_float[entry, 6] = spend
                            elif held[stock] and position_log[stock] >= 0:
                                log_float[position_log[stock], 1] += spend
                                log_float[position_log[stock], 5] += spend - notional

                        if not held[stock]:
                            held[stock] = True
                            held_list[held_count] = stock
                            held_count += 1

                    for position in range(picked):
                        in_target[target[position]] = False

                    for position in range(count):
                        rank_of[candidate[order[position]]] = -1

                value_after = 0.0

                for index in range(held_count):
                    stock = held_list[index]
                    price = price_row[stock]
                    value_after += units[stock] * (price if np.isfinite(price) else last_price[stock])

                nav_after = cash + value_after
                cost_factor = (nav_after - flow_total) / nav_now if nav_now > 0 else 1.0
                factor *= cost_factor
                out_scalar[2] += np.log(max(cost_factor, 1e-9))
                out_scalar[5] += traded / capital
                base = nav_after
            else:
                base = nav_now

            if phase == 0:
                # 장중 지수 구성(시가 체결 뒤)
                value_kospi = 0.0
                value_kosdaq = 0.0

                for index in range(held_count):
                    stock = held_list[index]
                    price = price_row[stock]
                    amount = units[stock] * (price if np.isfinite(price) else last_price[stock])

                    if market[day, stock] == 1:
                        value_kosdaq += amount
                    else:
                        value_kospi += amount

                if value_kospi + value_kosdaq > 0:
                    kosdaq_weight = value_kosdaq / (value_kospi + value_kosdaq)

                if nav_previous > 0 and day > 0 and started:
                    out_scalar[0] += np.log(max(nav_now / nav_previous, 1e-9))

        # 종가 갱신
        for stock_index in range(held_count):
            stock = held_list[stock_index]

            if np.isfinite(close_price[day, stock]):
                last_price[stock] = close_price[day, stock]

        # 새로 산 종목의 마지막 가격(체결이 종가면 이미 종가)
        nav_close = cash

        for index in range(held_count):
            stock = held_list[index]

            if not np.isfinite(last_price[stock]):
                last_price[stock] = close_price[day, stock] if np.isfinite(close_price[day, stock]) else open_price[day, stock]

            nav_close += units[stock] * last_price[stock]

        if day > 0 and started:
            bench_on = ((1.0 - weight_overnight) * (kospi_open[day] / kospi_close[day - 1] - 1.0)
                        + weight_overnight * (kosdaq_open[day] / kosdaq_close[day - 1] - 1.0))
            bench_id = ((1.0 - kosdaq_weight) * (kospi_close[day] / kospi_open[day] - 1.0)
                        + kosdaq_weight * (kosdaq_close[day] / kosdaq_open[day] - 1.0))
            out_return[day] = factor - 1.0
            out_bench[day] = (1.0 + bench_on) * (1.0 + bench_id) - 1.0
            out_scalar[3] += np.log(1.0 + bench_on)
            out_scalar[4] += np.log(1.0 + bench_id)

            if held_count > 0:
                out_scalar[7] += 1.0

        nav_previous = nav_close

    out_scalar[1] = 0.0

    for day in range(day_count):
        if np.isfinite(out_return[day]):
            out_scalar[1] += np.log(1.0 + out_return[day])

    out_scalar[1] -= out_scalar[0] + out_scalar[2]      # 장중 = 전체 − 밤 − 비용

    if log_capacity > 0:
        for index in range(held_count):
            stock = held_list[index]
            entry = position_log[stock]

            if entry >= 0:
                log_float[entry, 2] += units[stock] * last_price[stock]
                log_float[entry, 3] = last_price[stock]
                log_int[entry, 4] = day_count - 1
                log_int[entry, 5] = 2

    return log_count


@njit(parallel=True, cache=True)
def run_cells(cells, open_price, close_price, raw_open, raw_close, previous_20d, lock, market, stock_last, delist_multiplier,
              kospi_open, kospi_close, kosdaq_open, kosdaq_close, tax, new_tick, decision_days, weekly_end, monthly_end, scores,
              universe, out_return, out_bench, out_scalar, contrib, do_contrib):
    """cells 열: signal, ubit, slots, rebalance, execution, buffer, capital, ticks, cost_on, integer, seed."""
    dummy_int = np.zeros((1, LOG_INT), dtype=np.int64)
    dummy_float = np.zeros((1, LOG_FLOAT))

    for cell in prange(cells.shape[0]):
        row = cells[cell]
        slot = np.int64(0)

        if do_contrib:
            slot = np.int64(cell)

        simulate_one(int(row[0]), int(row[1]), int(row[2]), int(row[3]), int(row[4]), int(row[5]), row[6], row[7], int(row[8]),
                     int(row[9]), int(row[10]), open_price, close_price, raw_open, raw_close, previous_20d, lock, market,
                     stock_last, delist_multiplier, kospi_open, kospi_close, kosdaq_open, kosdaq_close, tax, new_tick,
                     decision_days, weekly_end, monthly_end, scores, universe, out_return[cell], out_bench[cell],
                     out_scalar[cell], contrib[slot], do_contrib, dummy_int, dummy_float, 0)
