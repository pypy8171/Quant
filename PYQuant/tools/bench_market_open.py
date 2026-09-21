"""
09:00 개장 폭주 재현 DB 부하 벤치마크 — bench_ticks/bench_signals/bench_orders/bench_fills/
bench_positions(실거래 테이블과 분리, schema.sql 참조)에 합성 이벤트를 실시간으로 흘려 넣으면서
그라파나 "테이블별 초당 insert"·"DB 활성 커넥션 수"·"DB 캐시 히트율" 패널에 실제 매매 폭주와
비슷한 모양의 부하를 만든다. 실계좌/모의계좌 원장은 건드리지 않으므로 장중에 돌려도 안전하다.

두 모드가 있다:
  - 기본(연속 매매): 개장 초반 --burst-seconds 동안 틱 유입이 --burst-multiplier 배로 튀었다가
    선형으로 가라앉는 패턴을 흉내낸다.
  - --call-auction(동시호가): 08:50~09:00 단일가 매매 구간을 재현한다 — 이 구간은 체결이 계속
    나는 게 아니라 호가(매수·매도 주문)만 계속 쌓이고 체결은 0이다가, 09:00에 쌓인 주문이
    한 번에 몰아서 체결(개장 단일가)된다. 실제 10분을 --preopen-seconds로 압축해서 재현하고,
    개장 순간의 체결 몰림을 --auction-seconds에 걸쳐 나눠 넣어 DB insert 스파이크로 관찰한다.

사용 (PYQuant/ 디렉토리에서):
    py -m tools.bench_market_open                                  # 기본(연속 매매) 41종목
    py -m tools.bench_market_open --symbols 100 --duration 120
    py -m tools.bench_market_open --call-auction --truncate
        # --call-auction 파라미터(종목 수·목표 틱/주문 건수·시총 쏠림 비중·09:00 목표 체결대금)는
        # config/bench_market_open.json에서 고친다. 값을 바꿔가며 재실행해 부하 변화를 관찰하려면
        # 이 파일을 수정 → 재실행. 일회성으로만 다르게 돌리고 싶으면 --preopen-orders 등으로 덮어쓴다.
"""
import argparse
import json
import random
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 를 패키지 루트로

DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[1] / "config" / "bench_market_open.json"

from core.logger import setup_logger  # noqa: E402
from db.client import DbClient  # noqa: E402

logger = setup_logger("quant.bench")

STRATEGIES = ["DevScale", "Momentum", "ValueTilt"]
COMMISSION_RATE = 0.00015  # 0.015%
TAX_RATE = 0.0018          # 0.18% (매도)


def _make_symbols(symbol_count: int) -> list[str]:
    return [f"BENCH{symbol_index:04d}" for symbol_index in range(1, symbol_count + 1)]


def _zipf_weights(item_count: int, skew: float) -> list[float]:
    """순위(index 0=최상위 대형주) 역수 가중치 — skew가 클수록 상위 종목 쏠림이 심해진다.
    삼성전자·SK하이닉스처럼 소수 대형주가 주문·호가 대부분을 차지하는 실제 분포를 흉내낸다."""
    return [1.0 / ((rank + 1) ** skew) for rank in range(item_count)]


def _alloc_by_ramp(total: int, ramp_values: list[float]) -> list[int]:
    """총 건수(total)를 ramp_values 비율대로 정수 스텝별 건수로 나눈다 — 나머지는 오차를 다음
    스텝으로 이월(Bresenham 방식)해서 합이 정확히 total이 되도록 한다."""
    ramp_sum = sum(ramp_values) or 1.0
    result = []
    carry = 0.0
    for value in ramp_values:
        exact = total * value / ramp_sum + carry
        allocated = int(exact)
        carry = exact - allocated
        result.append(allocated)
    result[-1] += total - sum(result)  # 누적 반올림 오차는 마지막 스텝에 몰아서 정확히 맞춘다
    return result


def _order_quantity_for_rank(rank: int, n_symbols: int) -> int:
    """순위 구간별 주문 수량(틱 테이블용) — 상위 1%(초대형주급)는 5만~50만주, 하위권은 10~500주로
    실제 호가창의 종목별 물량 격차를 반영한다. 09:00 체결 대금은 이 함수가 아니라
    _order_quantity_for_value()로 정확히 목표치에 맞춘다."""
    tier = rank / max(n_symbols, 1)
    if tier < 0.01:
        return random.randint(50_000, 500_000)
    if tier < 0.05:
        return random.randint(5_000, 50_000)
    if tier < 0.2:
        return random.randint(500, 5_000)
    return random.randint(10, 500)


def _market_cap_weights(symbol_count: int, top2_share: float, top2_split: float, skew: float) -> list[float]:
    """시총 가중치 — 1·2위(삼성전자·SK하이닉스 역할)가 합쳐서 top2_share를 갖고(내부는
    top2_split : 1-top2_split로 나뉨), 나머지 종목은 Zipf 꼬리로 남은 몫을 나눠 갖는다."""
    if symbol_count <= 2:
        return [1.0 / symbol_count] * symbol_count
    tail_weights = _zipf_weights(symbol_count - 2, skew)
    tail_sum = sum(tail_weights) or 1.0
    tail_share = 1.0 - top2_share
    tail = [tail_share * weight / tail_sum for weight in tail_weights]
    return [top2_share * top2_split, top2_share * (1.0 - top2_split)] + tail


def _order_quantity_for_value(price: float, value_per_order: float) -> int:
    """09:00 체결 대금 목표(auction_value_krw)를 preopen_orders 건수로 나눈 건당 평균 대금을
    가격으로 나눠 수량을 뽑는다. 종목 선택 빈도 자체가 시총 가중치를 따르므로, 이 수량을 그대로
    곱하면 기대값상 종목별 합산 대금이 정확히 시총 비중대로 배분된다."""
    return max(1, round(value_per_order / max(price, 1.0)))


def _phase_multiplier(elapsed: float, burst_seconds: float, burst_multiplier: float) -> float:
    if burst_seconds <= 0 or elapsed >= burst_seconds:
        return 1.0
    # burst_multiplier → 1.0 로 선형 감쇠 (개장 호가 폭주가 가라앉는 모양)
    frac = elapsed / burst_seconds
    return burst_multiplier - (burst_multiplier - 1.0) * frac


def _apply_fill(positions: dict, symbol: str, side: str, quantity: int, filled_price: float):
    position = positions.setdefault(symbol, {"quantity": 0, "avg_price": 0.0, "realized_pnl": 0.0})
    if side == "BUY":
        new_quantity = position["quantity"] + quantity
        position["avg_price"] = (
            (position["avg_price"] * position["quantity"] + filled_price * quantity) / new_quantity
            if new_quantity > 0 else 0.0
        )
        position["quantity"] = new_quantity
    else:
        position["realized_pnl"] += (filled_price - position["avg_price"]) * min(quantity, position["quantity"])
        position["quantity"] = max(0, position["quantity"] - quantity)


def _continuous_phase(db, symbols, prices, positions, counts, duration, arguments, burst_seconds=0.0,
                       burst_multiplier=1.0, label="연속매매"):
    """틱→(확률)시그널→(확률)주문→(확률)체결 캐스케이드를 duration초 동안 반복 재현하고
    flush_interval마다 배치 insert한다. run()의 기본 모드와 동시호가 모드의 개장 직후 꼬리
    구간이 이 함수를 공유한다."""
    tick_buffer, signal_buffer, order_buffer, fill_buffer = [], [], [], []
    touched_positions: set[str] = set()

    start = time.monotonic()
    last_flush = start
    last_report = start
    step = 0.2

    while True:
        now = time.monotonic()
        elapsed = now - start
        if elapsed >= duration:
            break

        multiplier = _phase_multiplier(elapsed, burst_seconds, burst_multiplier)
        expected = arguments.ticks_per_sec_per_symbol * multiplier * len(symbols) * step
        n_ticks = max(0, int(random.gauss(expected, max(expected * 0.15, 0.5))))

        timestamp = datetime.now(timezone.utc)
        for _ in range(n_ticks):
            symbol = random.choice(symbols)
            prices[symbol] = max(100.0, prices[symbol] * (1 + random.gauss(0, 0.0015)))
            price = round(prices[symbol], 0)
            tick_buffer.append((timestamp, symbol, price, random.randint(1, 500), random.choice((1, 5)), "KR"))
            counts["ticks"] += 1

            if random.random() < arguments.signal_rate:
                strategy = random.choice(STRATEGIES)
                side = random.choice(("BUY", "SELL"))
                quantity = random.choice((10, 20, 30, 50, 100))
                signal_buffer.append((timestamp, strategy, symbol, side, quantity, price, "KR"))
                counts["signals"] += 1

                if random.random() < arguments.order_rate:
                    ok = random.random() < 0.97
                    order_buffer.append((timestamp, symbol, side, quantity, price, ok, "KR", "bench"))
                    counts["orders"] += 1

                    if ok and random.random() < arguments.fill_rate:
                        slip = price * random.uniform(-0.001, 0.001)
                        filled_price = round(price + slip, 0)
                        commission = round(quantity * filled_price * COMMISSION_RATE, 2)
                        tax = round(quantity * filled_price * TAX_RATE, 2) if side == "SELL" else 0.0
                        kis_order_no = f"B{counts['fills']:012d}"
                        fill_buffer.append((timestamp, kis_order_no, symbol, side, quantity, filled_price, commission, tax, "KR", "bench"))
                        counts["fills"] += 1
                        _apply_fill(positions, symbol, side, quantity, filled_price)
                        touched_positions.add(symbol)

        if now - last_flush >= arguments.flush_interval:
            db.insert_bench_ticks_batch(tick_buffer)
            db.insert_bench_signals_batch(signal_buffer)
            db.insert_bench_orders_batch(order_buffer)
            db.insert_bench_fills_batch(fill_buffer)
            for symbol in touched_positions:
                position = positions[symbol]
                db.upsert_bench_position(symbol, position["quantity"], position["avg_price"], position["realized_pnl"])
            flushed = len(tick_buffer) + len(signal_buffer) + len(order_buffer) + len(fill_buffer)
            tick_buffer, signal_buffer, order_buffer, fill_buffer = [], [], [], []
            touched_positions.clear()
            last_flush = now

            if now - last_report >= 5.0:
                rate = flushed / arguments.flush_interval
                logger.info(f"[{label} {elapsed:5.1f}/{duration:.0f}초 배율={multiplier:.1f}] 플러시 {flushed}건 "
                            f"(~{rate:.0f}건/초) 누적: {counts}")
                last_report = now

        time.sleep(max(0.0, step - (time.monotonic() - now)))

    db.insert_bench_ticks_batch(tick_buffer)
    db.insert_bench_signals_batch(signal_buffer)
    db.insert_bench_orders_batch(order_buffer)
    db.insert_bench_fills_batch(fill_buffer)
    for symbol in touched_positions:
        position = positions[symbol]
        db.upsert_bench_position(symbol, position["quantity"], position["avg_price"], position["realized_pnl"])


def run(arguments):
    """기본 모드 — 개장 버스트 후 정상 구간의 연속 매매 재현."""
    db = DbClient()
    db.ensure_bench_tables()
    if arguments.truncate:
        db.truncate_bench_tables()
        logger.info("bench_* 테이블 비움")

    symbols = _make_symbols(arguments.symbols)
    prices = {symbol: random.uniform(5_000, 150_000) for symbol in symbols}
    positions: dict[str, dict] = {}
    counts = {"ticks": 0, "signals": 0, "orders": 0, "fills": 0}

    total_seconds = arguments.burst_seconds + arguments.duration
    logger.info(
        f"벤치마크 시작: symbols={len(symbols)} burst={arguments.burst_seconds}초(x{arguments.burst_multiplier}) "
        f"steady={arguments.duration}초 목표틱률={arguments.ticks_per_sec_per_symbol}/초/종목"
    )
    start = time.monotonic()
    _continuous_phase(db, symbols, prices, positions, counts, total_seconds, arguments,
                       burst_seconds=arguments.burst_seconds, burst_multiplier=arguments.burst_multiplier)
    wall = time.monotonic() - start
    logger.info(f"벤치마크 종료: {wall:.1f}초 소요, 누적 insert={counts}")
    db.close()


def run_call_auction(arguments):
    """동시호가(08:50~09:00) 재현 — 이 구간엔 체결이 없다(호가만 누적), 09:00에 누적 주문이
    한꺼번에 개장 단일가로 체결된다. 실제 10분(--preopen-real-minutes)을 --preopen-seconds로
    압축해서 재생하고, 개장 체결은 --auction-seconds에 걸쳐 나눠 넣어 스파이크로 관찰한다."""
    db = DbClient()
    db.ensure_bench_tables()
    if arguments.truncate:
        db.truncate_bench_tables()
        logger.info("bench_* 테이블 비움")

    symbols = _make_symbols(arguments.symbols)
    ranks = {symbol: rank for rank, symbol in enumerate(symbols)}  # rank 0 = 최상위 대형주
    weights = _market_cap_weights(len(symbols), arguments.top2_share, arguments.top2_split, arguments.symbol_skew)
    value_per_order = arguments.auction_value_krw / max(arguments.preopen_orders, 1)
    prices = {symbol: random.uniform(5_000, 150_000) for symbol in symbols}
    positions: dict[str, dict] = {}
    counts = {"ticks": 0, "signals": 0, "orders": 0, "fills": 0}
    pending_orders: list[dict] = []

    preopen_real_seconds = arguments.preopen_real_minutes * 60
    time_scale = preopen_real_seconds / arguments.preopen_seconds
    logger.info(
        f"동시호가 재현 시작: symbols={len(symbols)} 목표 틱={arguments.preopen_ticks} 목표 주문={arguments.preopen_orders} "
        f"목표 09:00 체결대금={arguments.auction_value_krw:,.0f}원 상위2종목비중={arguments.top2_share:.0%} "
        f"압축 {arguments.preopen_seconds:.0f}초 = 실제 {arguments.preopen_real_minutes:.0f}분"
        f"(x{time_scale:.1f} 가속), 09:00 체결은 {arguments.auction_seconds:.0f}초에 걸쳐 일괄 반영"
    )

    step = 0.2
    n_steps = max(1, int(arguments.preopen_seconds / step))
    # 09:00에 가까워질수록 막판 쏠림(0.3배 → 3배 선형 증가) — 이 곡선 비율대로 목표 건수를 스텝별로 나눈다
    ramp_values = [0.3 + 2.7 * (step_index / max(1, n_steps - 1)) for step_index in range(n_steps)]
    tick_targets = _alloc_by_ramp(arguments.preopen_ticks, ramp_values)
    order_targets = _alloc_by_ramp(arguments.preopen_orders, ramp_values)

    tick_buffer, signal_buffer, order_buffer = [], [], []
    start = time.monotonic()
    last_flush = start
    last_report = start

    for step_index in range(n_steps):
        step_start = time.monotonic()
        elapsed = step_start - start
        timestamp = datetime.now(timezone.utc)

        n_ticks = tick_targets[step_index]
        for symbol in random.choices(symbols, weights=weights, k=n_ticks):
            prices[symbol] = max(100.0, prices[symbol] * (1 + random.gauss(0, 0.001)))
            tick_buffer.append((timestamp, symbol, round(prices[symbol], 0),
                              _order_quantity_for_rank(ranks[symbol], len(symbols)) // 100 or 1,
                              random.choice((1, 5)), "KR"))
        counts["ticks"] += n_ticks

        n_orders = order_targets[step_index]
        for symbol in random.choices(symbols, weights=weights, k=n_orders):
            side = random.choice(("BUY", "SELL"))
            price = round(prices[symbol], 0)
            quantity = _order_quantity_for_value(price, value_per_order)
            strategy = random.choice(STRATEGIES)
            signal_buffer.append((timestamp, strategy, symbol, side, quantity, price, "KR"))
            counts["signals"] += 1

            ok = random.random() < 0.98
            order_buffer.append((timestamp, symbol, side, quantity, price, ok, "KR", "bench"))
            counts["orders"] += 1
            if ok:
                pending_orders.append({"symbol": symbol, "side": side, "quantity": quantity, "price": price})

        if step_start - last_flush >= arguments.flush_interval:
            db.insert_bench_ticks_batch(tick_buffer)
            db.insert_bench_signals_batch(signal_buffer)
            db.insert_bench_orders_batch(order_buffer)
            tick_buffer, signal_buffer, order_buffer = [], [], []
            last_flush = step_start

            if step_start - last_report >= 5.0:
                logger.info(f"[동시호가 {elapsed:5.1f}/{arguments.preopen_seconds:.0f}초 "
                            f"배율={ramp_values[step_index]:.1f}] 대기주문 누적 {len(pending_orders)}건, {counts}")
                last_report = step_start

        time.sleep(max(0.0, step - (time.monotonic() - step_start)))

    db.insert_bench_ticks_batch(tick_buffer)
    db.insert_bench_signals_batch(signal_buffer)
    db.insert_bench_orders_batch(order_buffer)

    logger.info(f"09:00 개장 단일가 체결 시작 — 대기주문 {len(pending_orders)}건 중 "
                f"{arguments.auction_first_chunk_ratio:.0%}를 첫 청크에 응축, 나머지는 "
                f"{arguments.auction_seconds:.0f}초에 걸쳐 꼬리로 분산 반영")

    # 실제 단일가 체결은 거의 한순간에 응축되어 회원사로 통지된다 — 첫 청크에 물량 대부분을
    # 몰아넣어 균등분할이 감추던 순간 피크(장벽 형태 스파이크)를 그대로 드러낸다.
    first_chunk_n = max(1, round(len(pending_orders) * arguments.auction_first_chunk_ratio))
    remaining_orders = pending_orders[first_chunk_n:]
    tail_chunks = max(1, int(arguments.auction_seconds / max(arguments.flush_interval, 0.1)) - 1)
    tail_chunk_size = max(1, len(remaining_orders) // tail_chunks + 1)
    chunk_list = [pending_orders[:first_chunk_n]]
    tail_index = 0
    while tail_index < len(remaining_orders):
        chunk_list.append(remaining_orders[tail_index: tail_index + tail_chunk_size])
        tail_index += tail_chunk_size

    done = 0
    for chunk_index, chunk in enumerate(chunk_list):
        timestamp = datetime.now(timezone.utc)
        fill_buffer = []
        for order in chunk:
            symbol, side, quantity = order["symbol"], order["side"], order["quantity"]
            clearing_price = round(prices[symbol], 0)
            slip = clearing_price * random.uniform(-0.0005, 0.0005)
            filled_price = round(clearing_price + slip, 0)
            commission = round(quantity * filled_price * COMMISSION_RATE, 2)
            tax = round(quantity * filled_price * TAX_RATE, 2) if side == "SELL" else 0.0
            kis_order_no = f"A{counts['fills']:012d}"
            fill_buffer.append((timestamp, kis_order_no, symbol, side, quantity, filled_price, commission, tax, "KR", "bench"))
            counts["fills"] += 1
            _apply_fill(positions, symbol, side, quantity, filled_price)
        db.insert_bench_fills_batch(fill_buffer)
        done += len(chunk)
        tag = " (첫 청크 응축)" if chunk_index == 0 else ""
        logger.info(f"개장 체결 진행 {done}/{len(pending_orders)}건{tag}")
        time.sleep(max(0.0, arguments.flush_interval * 0.3))

    for symbol, position in positions.items():
        db.upsert_bench_position(symbol, position["quantity"], position["avg_price"], position["realized_pnl"])
    logger.info(f"개장 단일가 체결 완료: {counts['fills']}건")

    if arguments.dead_zone_seconds > 0:
        logger.info(f"소강 구간 {arguments.dead_zone_seconds:.1f}초 — 이벤트 없이 대기(버스트 직후 백로그 해소 관측용)")
        time.sleep(arguments.dead_zone_seconds)

    if arguments.post_open_seconds > 0:
        logger.info(f"개장 직후 연속 매매 {arguments.post_open_seconds:.0f}초 재현(배율 {arguments.post_open_multiplier:.1f}배)")
        _continuous_phase(db, symbols, prices, positions, counts, arguments.post_open_seconds, arguments,
                           burst_seconds=arguments.post_open_seconds, burst_multiplier=arguments.post_open_multiplier,
                           label="개장직후")

    wall = time.monotonic() - start
    logger.info(f"동시호가 벤치마크 종료: {wall:.1f}초 소요, 누적: {counts}")
    db.close()


def _load_config(path: Path) -> dict:
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as config_file:
        return json.load(config_file)


def main():
    # --config로 지정한 값을 먼저 읽어 각 옵션의 기본값으로 쓴다 — CLI 인자 없이 config 파일만
    # 고쳐서 재실행해도 되고, CLI로 그때그때 덮어써도 된다(CLI가 항상 우선).
    pre_parser = argparse.ArgumentParser(add_help=False)
    pre_parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH)
    pre_arguments, _ = pre_parser.parse_known_args()
    config = _load_config(pre_arguments.config)

    parser = argparse.ArgumentParser(description="개장 DB 부하 벤치마크(연속 매매 / 동시호가 재현)",
                                      parents=[pre_parser])
    parser.add_argument("--symbols", type=int, default=config.get("symbols", 41),
                         help="합성 종목 수(기본: KIS 41종목 규모, 부하테스트는 --config로 2700 등 지정)")
    parser.add_argument("--truncate", action="store_true", help="시작 전 bench_* 테이블 비우기")
    parser.add_argument("--flush-interval", type=float, default=config.get("flush_interval", 0.5),
                         help="DB 배치 flush 주기(초)")

    parser.add_argument("--call-auction", action="store_true",
                         help="08:50~09:00 동시호가 재현 모드(체결 없이 호가만 누적 → 09:00 일괄 체결)")

    # 기본(연속 매매) 모드 전용
    parser.add_argument("--duration", type=float, default=60.0, help="버스트 이후 정상 구간 길이(초)")
    parser.add_argument("--burst-seconds", type=float, default=10.0, help="개장 버스트 길이(초)")
    parser.add_argument("--burst-multiplier", type=float, default=8.0, help="버스트 구간 틱률 배수")
    parser.add_argument("--ticks-per-sec-per-symbol", type=float, default=2.0, help="정상 구간 종목당 초당 틱")
    parser.add_argument("--signal-rate", type=float, default=0.01, help="틱 대비 시그널 발생 비율")
    parser.add_argument("--order-rate", type=float, default=0.6, help="시그널 대비 주문 발행 비율")
    parser.add_argument("--fill-rate", type=float, default=0.9, help="체결 성공 주문 대비 체결 비율")

    # --call-auction 모드 전용 — 값은 PYQuant/config/bench_market_open.json에서 고친다
    parser.add_argument("--preopen-real-minutes", type=float, default=config.get("preopen_real_minutes", 10.0),
                         help="재현할 실제 동시호가 길이(분)")
    parser.add_argument("--preopen-seconds", type=float, default=config.get("preopen_seconds", 70.0),
                         help="동시호가를 압축할 실행 시간(초)")
    parser.add_argument("--auction-seconds", type=float, default=config.get("auction_seconds", 4.0),
                         help="09:00 개장 체결을 나눠 넣을 시간(초)")
    parser.add_argument("--preopen-ticks", type=int, default=config.get("preopen_ticks", 40_000),
                         help="동시호가 구간 전체 틱(호가/시세 갱신) 목표 총량")
    parser.add_argument("--preopen-orders", type=int, default=config.get("preopen_orders", 1_500),
                         help="동시호가 구간 전체 주문 제출 목표 총량(=09:00 대기주문 건수, 대금을 이 건수로 나눠 건당 수량을 정함)")
    parser.add_argument("--symbol-skew", type=float, default=config.get("symbol_skew", 1.2),
                         help="상위 2종목을 제외한 나머지 종목의 꼬리 쏠림 지수(Zipf) — 클수록 3위 이하 안에서도 상위권 쏠림이 심해짐")
    parser.add_argument("--top2-share", type=float, default=config.get("top2_share", 0.45),
                         help="1·2위 종목(삼성전자·SK하이닉스 역할)이 합쳐서 가져가는 틱/주문 선택 비중(0~1)")
    parser.add_argument("--top2-split", type=float, default=config.get("top2_split", 0.55),
                         help="top2-share를 1위:2위로 나누는 비율(1위 몫), 0.55=1위가 2위보다 약간 큼")
    parser.add_argument("--auction-value-krw", type=float, default=config.get("auction_value_krw", 700_000_000_000),
                         help="09:00 개장 단일가 전체 체결대금 목표(원) — 전체 preopen-orders 건수로 나눈 뒤 종목 가격으로 환산해 건당 수량을 정함")
    parser.add_argument("--auction-first-chunk-ratio", type=float, default=config.get("auction_first_chunk_ratio", 0.85),
                         help="09:00 체결 대기주문 중 첫 청크(단일가 응축 순간)에 한 번에 반영할 비율(0~1), 나머지는 꼬리로 분산")
    parser.add_argument("--dead-zone-seconds", type=float, default=config.get("dead_zone_seconds", 2.0),
                         help="09:00 체결 직후 이벤트 없이 대기하는 소강 구간 길이(초) — 백로그 해소 여부 관측용, 0이면 생략")
    parser.add_argument("--post-open-seconds", type=float, default=config.get("post_open_seconds", 90.0),
                         help="소강 구간 이후 연속 매매(알고리즘 재진입) 재현 길이(초), 0이면 생략")
    parser.add_argument("--post-open-multiplier", type=float, default=config.get("post_open_multiplier", 4.0),
                         help="재진입 구간 시작 시점 틱률 배수 — post-open-seconds에 걸쳐 1배로 선형 감쇠")

    arguments = parser.parse_args()
    if arguments.call_auction:
        run_call_auction(arguments)
    else:
        run(arguments)


if __name__ == "__main__":
    main()
