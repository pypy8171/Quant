"""부하시험 주문 인젝터 — 가상 주문을 미리 다 만들어 두고 C++ 수신단(exchange::ZmqOrderFeed)에 쏟아붓는다.

    py -m tools.load_injector --symbols 2700 --orders-per-symbol 10000
    py -m tools.load_injector --symbols 2700 --orders-per-symbol 100000 --lanes 4

보내는 바이트 모양은 Quant/include/exchange/OrderWire.h 하나가 정본이다. 아래 _WIRE_DTYPE 가 그 구조체와
한 바이트도 다르면 안 된다 — 다르면 받는 쪽이 엉뚱한 값을 주문으로 읽는다.

세 단계로 돈다.
  1단계(동시호가): 미리 만든 주문을 sleep 없이 전부 쏟는다. 받는 쪽은 맞추지 않고 쌓기만 한다.
  2단계(단일가): RUN_AUCTION 한 건씩 보내 쌓인 것을 종목마다 한 번에 맞춘다.
  3단계(연속매매): 1분 동안 초마다, 종목값을 최소호가단위로 흔들면서 시장가와 지정가를 섞어 보낸다.

KIS에는 아무것도 나가지 않는다. 주문은 전부 C++ 오더북 안에서만 산다.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import numpy
import zmq

# 콘솔 기본 코드페이지가 cp949 라 한글 진행 표시가 줄표 하나에 죽는다 — 출력만 UTF-8 로 돌린다.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCAN_PATH = _REPO_ROOT / "Quant" / "config" / "universe_scan.json"
_FULL_PATH = _REPO_ROOT / "Quant" / "config" / "universe_full.json"

# ── 전문 (Quant/include/exchange/OrderWire.h 와 짝) ──────────────────────────
_WIRE_MAGIC = 0x44524F51  # 'QORD'
_WIRE_VERSION = 1
_WIRE_HEADER_BYTES = 16
_WIRE_RECORD_BYTES = 32
_WIRE_HEADER_FORMAT = "<IHHQ"  # magic, version, record_count, sent_unix_ns

_WIRE_DTYPE = numpy.dtype(
    [
        ("order_id", "<u8"),
        ("symbol_index", "<u4"),
        ("quantity", "<i4"),
        ("price_krw", "<i8"),
        ("side", "u1"),
        ("command", "u1"),
        ("reserved", "V6"),
    ]
)
assert _WIRE_DTYPE.itemsize == _WIRE_RECORD_BYTES, "주문 한 건은 32바이트여야 한다"
assert struct.calcsize(_WIRE_HEADER_FORMAT) == _WIRE_HEADER_BYTES, "묶음머리는 16바이트여야 한다"

_SIDE_BUY = 0
_SIDE_SELL = 1

_COMMAND_ACCUMULATE = 0
_COMMAND_MATCH = 1
_COMMAND_RUN_AUCTION = 2
_COMMAND_CONFIGURE = 3

_MARKET_ORDER_PRICE = 0  # 0이면 시장가 — exchange::kMarketOrderPrice 와 같은 약속

# ── 국내 호가단위 (Quant/include/core/TickSize.h 와 짝) ───────────────────────
# [formula] 유가증권시장 업무규정 시행세칙 제3조 호가가격단위.
_TICK_BANDS = ((2000, 1), (5000, 5), (20000, 10), (50000, 50), (200000, 100), (500000, 500))
_TICK_ABOVE = 1000

# [formula] 가격제한폭 ±30%(유가증권시장 업무규정 제20조).
_PRICE_LIMIT_RATIO = 0.30

# 보내는 쪽이 한 번에 묶는 주문 수. 스펙이 정한 세 가지를 그대로 쓴다.
_BATCH_SIZES = (1, 30, 100)

_SECONDS_PER_PHASE = 60

# 1단계 주문을 한 번에 몇 건씩 만들어 보낼지. 전문 자체는 32바이트지만 값을 고르는 동안 float64 배열
# 여섯 개를 같이 들어 건당 100바이트 넘게 쓴다 — 이 상수가 그 봉우리를 묶음 하나 크기로 묶어 둔다.
_ACCUMULATE_CHUNK_RECORDS = 8_000_000


def tick_size(price_krw: int) -> int:
    """그 값에서의 호가단위."""
    for upper_bound, step in _TICK_BANDS:
        if price_krw < upper_bound:
            return step

    return _TICK_ABOVE


def round_to_tick(price_krw: int) -> int:
    """호가단위 격자에 내림으로 맞춘다. 격자 밖 값은 받는 쪽이 버리므로 보내기 전에 맞춰 둔다."""
    step = tick_size(price_krw)

    return max(step, (price_krw // step) * step)


# ── 유니버스 ────────────────────────────────────────────────────────────────
@dataclass
class Universe:
    """부하시험이 쓸 종목 목록. 순서 자체가 약속이다 — 받는 쪽도 이 순서로 종목 순번을 매긴다."""

    tickers: list[str]
    reference_prices: numpy.ndarray  # int64, 종목별 기준가
    weights: numpy.ndarray  # float64, 시가총액 가중치(합 1)

    def __len__(self) -> int:
        return len(self.tickers)


def _zipf_weights(count: int, skew: float) -> numpy.ndarray:
    ranks = numpy.arange(1, count + 1, dtype=numpy.float64)

    return ranks ** (-skew)


def _market_cap_weights(count: int, skew: float) -> numpy.ndarray:
    """시가총액 분포 대용. 실제 시총을 받아 오지 않고 순위-크기 법칙으로 대신한다 —
    부하시험에 필요한 것은 '대형주 몇 종목에 거래대금이 쏠린다'는 모양뿐이라 그렇다."""
    weights = _zipf_weights(count, skew)

    return weights / weights.sum()


def _load_known_prices() -> dict[str, int]:
    """종가를 아는 종목만 모은다. 나머지 기준가는 순위로 합성한다."""
    if not _SCAN_PATH.exists():
        return {}

    scanned = json.loads(_SCAN_PATH.read_text(encoding="utf-8"))
    prices: dict[str, int] = {}

    for entry in scanned.get("universe", []):
        close = int(entry.get("close", 0))

        if close > 0:
            prices[str(entry["ticker"])] = close

    return prices


def _load_known_tickers() -> list[str]:
    """전체 종목코드. universe_full.json 이 있으면 그것을, 없으면 스캔 목록을 쓴다."""
    if _FULL_PATH.exists():
        full = json.loads(_FULL_PATH.read_text(encoding="utf-8"))
        codes = [str(code) for code in full.get("codes", [])]

        if codes:
            return codes

    if _SCAN_PATH.exists():
        scanned = json.loads(_SCAN_PATH.read_text(encoding="utf-8"))

        return [str(entry["ticker"]) for entry in scanned.get("universe", [])]

    return []


def _synthetic_price_for_rank(rank: int, count: int) -> int:
    """기준가를 모르는 종목의 값. 순위가 앞설수록 비싸게 — 실제 시장의 값 분포와 비슷한 모양만 맞춘다."""
    position = rank / max(count, 1)
    price = 400000.0 * math.exp(-4.0 * position) + 1000.0

    return round_to_tick(int(price))


def load_universe_file(path: Path) -> list[str]:
    """받는 쪽이 기동 때 적어 둔 종목 순번표. 이 순서가 곧 전문의 symbol_index 다 —
    보내는 쪽이 config를 따로 읽어 순서가 조용히 어긋나는 것을 막으려고 받는 쪽 것을 그대로 쓴다."""
    written = json.loads(path.read_text(encoding="utf-8"))

    return [str(ticker) for ticker in written["tickers"]]


def build_universe(symbol_count: int, skew: float, universe_file: Path | None = None) -> Universe:
    """종목 목록·기준가·가중치를 만든다. 모자란 종목은 합성 코드(9로 시작)로 채운다."""
    known_prices = _load_known_prices()

    if universe_file is not None:
        tickers = load_universe_file(universe_file)
        symbol_count = len(tickers)
    else:
        known_tickers = _load_known_tickers()

        # 종가를 아는 종목을 앞에 둔다 — 시총 상위에 실제 값이 실리게 하려고.
        ordered = [ticker for ticker in known_tickers if ticker in known_prices]
        ordered += [ticker for ticker in known_tickers if ticker not in known_prices]

        tickers = ordered[:symbol_count]

        for extra_index in range(len(tickers), symbol_count):
            tickers.append(f"9{extra_index:05d}")

    prices = numpy.empty(symbol_count, dtype=numpy.int64)

    for rank, ticker in enumerate(tickers):
        known = known_prices.get(ticker)
        prices[rank] = round_to_tick(known) if known else _synthetic_price_for_rank(rank, symbol_count)

    return Universe(tickers=tickers, reference_prices=prices, weights=_market_cap_weights(symbol_count, skew))


# ── 주문 미리 만들기 ────────────────────────────────────────────────────────
def _order_quantities(universe: Universe, total_value_krw: float, orders_per_symbol: int) -> numpy.ndarray:
    """종목별 주문 한 건의 수량. 거래대금을 시총 가중으로 나눠 주문 수로 쪼갠 뒤 값으로 나눈다.

    [formula] 수량 = max(1, 종목거래대금 / 종목주문수 / 기준가)
    """
    symbol_values = universe.weights * total_value_krw
    per_order_value = symbol_values / max(orders_per_symbol, 1)
    quantities = numpy.maximum(1, numpy.round(per_order_value / universe.reference_prices))

    return quantities.astype(numpy.int32)


def build_configure_records(universe: Universe) -> numpy.ndarray:
    """종목마다 기준가를 알려 호가 격자를 만들게 하는 전문. 주문보다 먼저 보낸다."""
    records = numpy.zeros(len(universe), dtype=_WIRE_DTYPE)
    records["symbol_index"] = numpy.arange(len(universe), dtype=numpy.uint32)
    records["price_krw"] = universe.reference_prices
    records["command"] = _COMMAND_CONFIGURE

    return records


def build_auction_records(universe: Universe) -> numpy.ndarray:
    """쌓인 것을 단일가로 맞추라는 전문. 종목당 한 건."""
    records = numpy.zeros(len(universe), dtype=_WIRE_DTYPE)
    records["symbol_index"] = numpy.arange(len(universe), dtype=numpy.uint32)
    records["command"] = _COMMAND_RUN_AUCTION

    return records


def iterate_accumulate_records(
    universe: Universe,
    orders_per_symbol: int,
    quantities: numpy.ndarray,
    generator: numpy.random.Generator,
    records_per_chunk: int = _ACCUMULATE_CHUNK_RECORDS,
) -> Iterator[numpy.ndarray]:
    """1단계에 쏟을 주문을 종목 묶음 단위로 만들어 내놓는다. 종목당 orders_per_symbol 건,
    주문번호는 전역에서 겹치지 않는다.

    [inv] 묶음 경계는 종목 경계다 — 한 종목의 주문은 반드시 한 묶음 안에 다 들어간다. 주문번호를
      종목 순번에서 바로 뽑을 수 있는 것도, 받는 쪽 수신 스레드가 섞이지 않는 것도 이 덕이다.

    전부를 한 배열로 들면 주문 한 건에 100바이트 넘게 든다 — 32바이트짜리 전문 말고도 값을 고르는
    float64 배열 여섯 개를 같이 들어서다. 묶음으로 내놓으면 그 100바이트가 묶음 크기에만 걸린다.

    값은 기준가 둘레 ±30% 안에서 고른다 — 매수는 기준가 위로도 걸리게, 매도는 아래로도 걸리게 해서
    단일가에서 실제로 맞는 물량이 생기도록.
    """
    symbol_count = len(universe)
    symbols_per_chunk = max(1, records_per_chunk // max(orders_per_symbol, 1))

    for first_symbol in range(0, symbol_count, symbols_per_chunk):
        last_symbol = min(first_symbol + symbols_per_chunk, symbol_count)
        total = (last_symbol - first_symbol) * orders_per_symbol
        records = numpy.zeros(total, dtype=_WIRE_DTYPE)

        # 주문번호: 종목 순번 × 종목당 건수 + 1.. — 종목 간에도, 묶음 간에도 안 겹친다.
        first_order_id = first_symbol * orders_per_symbol + 1
        records["order_id"] = numpy.arange(first_order_id, first_order_id + total, dtype=numpy.uint64)
        records["symbol_index"] = numpy.repeat(
            numpy.arange(first_symbol, last_symbol, dtype=numpy.uint32), orders_per_symbol
        )
        records["quantity"] = numpy.repeat(quantities[first_symbol:last_symbol], orders_per_symbol)
        records["side"] = generator.integers(0, 2, size=total, dtype=numpy.uint8)
        records["command"] = _COMMAND_ACCUMULATE

        # 값: 기준가 × (1 + 정규난수). 매수는 위로, 매도는 아래로 조금 치우치게 해서 교차가 생기게 한다.
        reference = numpy.repeat(
            universe.reference_prices[first_symbol:last_symbol].astype(numpy.float64), orders_per_symbol
        )
        drift = numpy.where(records["side"] == _SIDE_BUY, 0.004, -0.004)
        noise = generator.normal(0.0, 0.010, size=total)
        prices = reference * (1.0 + drift + noise)

        lower = reference * (1.0 - _PRICE_LIMIT_RATIO)
        upper = reference * (1.0 + _PRICE_LIMIT_RATIO)
        prices = numpy.clip(prices, lower, upper)

        records["price_krw"] = _round_array_to_tick(prices)

        yield records


def _round_array_to_tick(prices: numpy.ndarray) -> numpy.ndarray:
    """값 배열을 호가단위 격자에 한꺼번에 맞춘다. 종목마다 따로 도는 것을 피하려고 구간별로 자른다."""
    integers = prices.astype(numpy.int64)
    steps = numpy.full(integers.shape, _TICK_ABOVE, dtype=numpy.int64)

    for upper_bound, step in reversed(_TICK_BANDS):
        steps = numpy.where(integers < upper_bound, step, steps)

    return numpy.maximum(steps, (integers // steps) * steps)


def build_continuous_records(
    universe: Universe,
    quantities: numpy.ndarray,
    orders_per_symbol: int,
    limit_orders_per_second: int,
    generator: numpy.random.Generator,
) -> list[numpy.ndarray]:
    """3단계 60초치를 초 단위로 미리 만든다. 초마다 종목값이 최소호가단위 몇 칸씩 흔들리고,
    그 값 둘레로 지정가를, 그리고 단일가 체결량의 1/60 만큼 시장가를 낸다.

    [inv] 실제 단일가 체결량은 받는 쪽만 안다 — 돌아오는 길이 없어 적재 물량에서 어림잡는다.
      어림값 = 종목당 주문수 × 주문수량 × 0.5(양쪽 중 작은 쪽이 맞는다).
    """
    symbol_count = len(universe)
    estimated_auction_quantity = (quantities.astype(numpy.int64) * orders_per_symbol) // 2
    market_quantity_per_second = numpy.maximum(1, estimated_auction_quantity // _SECONDS_PER_PHASE)

    # 값은 기준가에서 출발해 초마다 랜덤워크한다. 한 걸음은 그 종목 호가단위의 몇 칸.
    current_prices = universe.reference_prices.astype(numpy.int64).copy()
    tick_steps = _tick_sizes(current_prices)

    lower_bound = (universe.reference_prices * (1.0 - _PRICE_LIMIT_RATIO)).astype(numpy.int64)
    upper_bound = (universe.reference_prices * (1.0 + _PRICE_LIMIT_RATIO)).astype(numpy.int64)

    next_order_id = symbol_count * orders_per_symbol + 1
    per_second: list[numpy.ndarray] = []

    for _ in range(_SECONDS_PER_PHASE):
        # ±3% 안에서 흔든다 — 한 걸음은 호가단위 −3..+3 칸.
        walk = generator.integers(-3, 4, size=symbol_count, dtype=numpy.int64)
        current_prices = numpy.clip(current_prices + walk * tick_steps, lower_bound, upper_bound)
        current_prices = _round_array_to_tick(current_prices.astype(numpy.float64))
        tick_steps = _tick_sizes(current_prices)

        market_count = symbol_count
        limit_count = symbol_count * limit_orders_per_second
        records = numpy.zeros(market_count + limit_count, dtype=_WIRE_DTYPE)

        # 시장가: 종목당 한 건, 그 초에 낼 물량 전부를 싣는다.
        market_view = records[:market_count]
        market_view["symbol_index"] = numpy.arange(symbol_count, dtype=numpy.uint32)
        market_view["quantity"] = market_quantity_per_second.astype(numpy.int32)
        market_view["price_krw"] = _MARKET_ORDER_PRICE
        market_view["side"] = generator.integers(0, 2, size=market_count, dtype=numpy.uint8)
        market_view["command"] = _COMMAND_MATCH

        # 지정가 보충: 흔들린 값 바로 옆에 걸어 둔다 — 시장가가 먹을 상대를 계속 채워 주는 몫.
        limit_view = records[market_count:]
        limit_view["symbol_index"] = numpy.tile(
            numpy.arange(symbol_count, dtype=numpy.uint32), limit_orders_per_second
        )
        limit_view["quantity"] = numpy.tile(quantities, limit_orders_per_second)
        limit_view["side"] = generator.integers(0, 2, size=limit_count, dtype=numpy.uint8)
        limit_view["command"] = _COMMAND_MATCH

        offsets = generator.integers(-2, 3, size=limit_count, dtype=numpy.int64)
        base = numpy.tile(current_prices, limit_orders_per_second)
        base_ticks = numpy.tile(tick_steps, limit_orders_per_second)
        limit_prices = numpy.clip(
            base + offsets * base_ticks,
            numpy.tile(lower_bound, limit_orders_per_second),
            numpy.tile(upper_bound, limit_orders_per_second),
        )
        limit_view["price_krw"] = _round_array_to_tick(limit_prices.astype(numpy.float64))

        records["order_id"] = numpy.arange(next_order_id, next_order_id + len(records), dtype=numpy.uint64)
        next_order_id += len(records)

        generator.shuffle(records)
        per_second.append(records)

    return per_second


def _tick_sizes(prices: numpy.ndarray) -> numpy.ndarray:
    """값 배열의 호가단위를 한꺼번에 구한다."""
    steps = numpy.full(prices.shape, _TICK_ABOVE, dtype=numpy.int64)

    for upper_bound, step in reversed(_TICK_BANDS):
        steps = numpy.where(prices < upper_bound, step, steps)

    return steps


# ── 보내기 ──────────────────────────────────────────────────────────────────
class LaneSender:
    """수신 스레드 하나에 붙는 보내는 쪽. 종목 순번 % 수신스레드수 로 갈라지므로 여기 오는 주문은
    모두 같은 수신 스레드가 받는다 — 그래서 받는 쪽 오더북에 락이 필요 없다."""

    def __init__(self, context: zmq.Context, address: str, send_high_water_mark: int):
        self.socket = context.socket(zmq.PUSH)
        self.socket.setsockopt(zmq.SNDHWM, send_high_water_mark)
        self.socket.connect(address)
        self.address = address
        self.batches = 0
        self.records = 0
        # 가장 큰 묶음이 들어갈 자리를 한 번만 잡아 두고 계속 쓴다 — 보낼 때마다 할당하지 않으려고.
        self.buffer = bytearray(_WIRE_HEADER_BYTES + _WIRE_RECORD_BYTES * max(_BATCH_SIZES))

    def send(self, records: numpy.ndarray) -> None:
        count = len(records)
        body_bytes = count * _WIRE_RECORD_BYTES
        struct.pack_into(
            _WIRE_HEADER_FORMAT, self.buffer, 0, _WIRE_MAGIC, _WIRE_VERSION, count, time.time_ns()
        )
        self.buffer[_WIRE_HEADER_BYTES : _WIRE_HEADER_BYTES + body_bytes] = records.tobytes()
        self.socket.send(memoryview(self.buffer)[: _WIRE_HEADER_BYTES + body_bytes])
        self.batches += 1
        self.records += count

    def close(self) -> None:
        self.socket.close(linger=1000)


def split_by_lane(records: numpy.ndarray, lane_count: int) -> list[numpy.ndarray]:
    """종목 순번으로 수신 스레드를 골라 나눈다. 받는 쪽이 쓰는 규칙(symbol_index % lane_count)과 같아야 한다."""
    if lane_count == 1:
        return [records]

    lane_of_record = records["symbol_index"] % lane_count

    return [numpy.ascontiguousarray(records[lane_of_record == lane]) for lane in range(lane_count)]


def dump_saturated(senders: list[LaneSender], per_lane: list[numpy.ndarray], generator: random.Random) -> int:
    """sleep 없이 전부 쏟는다. 묶음 크기만 1·30·100 중에서 매번 다시 고른다."""
    cursors = [0] * len(per_lane)
    sent = 0
    remaining = sum(len(records) for records in per_lane)

    while remaining > 0:
        for lane, records in enumerate(per_lane):
            cursor = cursors[lane]

            if cursor >= len(records):
                continue

            size = min(generator.choice(_BATCH_SIZES), len(records) - cursor)
            senders[lane].send(records[cursor : cursor + size])
            cursors[lane] = cursor + size
            sent += size
            remaining -= size

    return sent


def send_one_per_symbol(senders: list[LaneSender], records: numpy.ndarray, lane_count: int) -> int:
    """종목당 한 건짜리 전문(격자 만들기·단일가 체결)을 보낸다. 묶음 크기는 최대치로 고정 —
    이 둘은 부하가 아니라 신호라 빨리 넘기는 것이 맞다."""
    sent = 0

    for lane, lane_records in enumerate(split_by_lane(records, lane_count)):
        for cursor in range(0, len(lane_records), max(_BATCH_SIZES)):
            chunk = lane_records[cursor : cursor + max(_BATCH_SIZES)]
            senders[lane].send(chunk)
            sent += len(chunk)

    return sent


def _report(label: str, sent: int, elapsed_seconds: float) -> None:
    rate = sent / elapsed_seconds if elapsed_seconds > 0 else 0.0
    print(f"  {label}: {sent:,}건  {elapsed_seconds:.2f}초  초당 {rate:,.0f}건")


def parse_arguments(argument_list: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="부하시험 주문 인젝터")
    parser.add_argument("--symbols", type=int, default=2700, help="종목 수")
    parser.add_argument(
        "--universe-file",
        default=None,
        help="받는 쪽이 기동 때 적은 종목 순번표(load_test.universe_out). 주면 --symbols 보다 이것이 이긴다",
    )
    parser.add_argument("--orders-per-symbol", type=int, default=10000, help="1단계 종목당 주문 수")
    parser.add_argument("--lanes", type=int, default=1, help="받는 쪽 수신 스레드 수")
    parser.add_argument(
        "--accumulate-chunk-records",
        type=int,
        default=_ACCUMULATE_CHUNK_RECORDS,
        help="1단계 주문을 한 번에 몇 건씩 만들어 보낼지 — 이 값이 메모리 봉우리를 정한다",
    )
    parser.add_argument("--base-port", type=int, default=5600, help="수신 스레드 0번 포트")
    parser.add_argument("--host", default="127.0.0.1", help="받는 쪽 주소")
    parser.add_argument(
        "--total-value-krw", type=float, default=640e9, help="1단계에 배분할 거래대금(원)"
    )
    parser.add_argument("--skew", type=float, default=1.0, help="시총 쏠림 정도. 클수록 대형주에 쏠린다")
    parser.add_argument(
        "--limit-orders-per-second", type=int, default=2, help="3단계에 종목당 초당 지정가 보충 건수"
    )
    parser.add_argument("--send-high-water-mark", type=int, default=100000, help="보내는 쪽 큐 길이")
    parser.add_argument("--seed", type=int, default=20260923, help="난수 시드")
    parser.add_argument(
        "--phase", choices=("all", "auction", "continuous"), default="all", help="돌릴 단계"
    )
    parser.add_argument("--dry-run", action="store_true", help="소켓을 열지 않고 만들기만 한다")

    return parser.parse_args(argument_list)


def main(argument_list: list[str] | None = None) -> int:
    arguments = parse_arguments(argument_list)

    numpy_generator = numpy.random.default_rng(arguments.seed)
    batch_generator = random.Random(arguments.seed)

    print("유니버스를 만든다...")
    universe_file = Path(arguments.universe_file) if arguments.universe_file else None

    if universe_file is not None and not universe_file.exists():
        print(f"종목 순번표가 없다: {universe_file} — 받는 쪽(quant_trader)을 먼저 띄운다.")

        return 2

    universe = build_universe(arguments.symbols, arguments.skew, universe_file)
    quantities = _order_quantities(universe, arguments.total_value_krw, arguments.orders_per_symbol)
    planned_value = float((quantities.astype(numpy.float64) * universe.reference_prices).sum()) * (
        arguments.orders_per_symbol
    )
    print(
        f"  종목 {len(universe):,}개  1단계 주문 {len(universe) * arguments.orders_per_symbol:,}건"
        f"  거래대금 {planned_value / 1e8:,.0f}억원"
    )

    print("주문을 미리 만든다...")
    build_started = time.perf_counter()
    configure_records = build_configure_records(universe)
    auction_records = build_auction_records(universe)
    continuous_seconds = (
        build_continuous_records(
            universe,
            quantities,
            arguments.orders_per_symbol,
            arguments.limit_orders_per_second,
            numpy_generator,
        )
        if arguments.phase in ("all", "continuous")
        else []
    )
    # 1단계는 보내면서 만든다 — 여기 잡힌 것은 3단계 60초치뿐이다.
    built_bytes = sum(records.nbytes for records in continuous_seconds)
    print(
        f"  {time.perf_counter() - build_started:.1f}초  메모리 {built_bytes / (1 << 30):.2f}GB"
        f"  (1단계는 {arguments.accumulate_chunk_records:,}건씩 만들어 가며 보낸다)"
    )

    if arguments.dry_run:
        print("--dry-run 이라 보내지 않는다.")

        return 0

    context = zmq.Context(io_threads=max(1, arguments.lanes))
    senders = [
        LaneSender(
            context,
            f"tcp://{arguments.host}:{arguments.base_port + lane}",
            arguments.send_high_water_mark,
        )
        for lane in range(arguments.lanes)
    ]

    try:
        print("0단계: 호가 격자를 만든다")
        send_one_per_symbol(senders, configure_records, arguments.lanes)

        if arguments.phase in ("all", "auction"):
            planned = len(universe) * arguments.orders_per_symbol
            print(f"1단계: 동시호가 적재 — {planned:,}건을 쉬지 않고 쏟는다")
            started = time.perf_counter()
            sent = 0

            for chunk in iterate_accumulate_records(
                universe,
                arguments.orders_per_symbol,
                quantities,
                numpy_generator,
                arguments.accumulate_chunk_records,
            ):
                sent += dump_saturated(senders, split_by_lane(chunk, arguments.lanes), batch_generator)

            _report("적재", sent, time.perf_counter() - started)

            print("2단계: 단일가 일괄 체결")
            started = time.perf_counter()
            sent = send_one_per_symbol(senders, auction_records, arguments.lanes)
            _report("체결 지시", sent, time.perf_counter() - started)

        if arguments.phase in ("all", "continuous"):
            total_continuous = sum(len(records) for records in continuous_seconds)
            print(f"3단계: 연속매매 {_SECONDS_PER_PHASE}초 — 모두 {total_continuous:,}건")
            started = time.perf_counter()

            for second, records in enumerate(continuous_seconds):
                second_started = time.perf_counter()
                dump_saturated(senders, split_by_lane(records, arguments.lanes), batch_generator)
                # 초 경계는 지킨다 — 이 단계는 '초당 얼마'가 시나리오라 포화로 밀면 1분이 안 된다.
                slack = 1.0 - (time.perf_counter() - second_started)

                if slack > 0:
                    time.sleep(slack)
                elif second % 10 == 0:
                    print(f"    {second}초: 보내는 쪽이 1초를 못 지켰다({-slack:.2f}초 밀림)")

            _report("연속매매", total_continuous, time.perf_counter() - started)

        print("보낸 것 정리:")

        for lane, sender in enumerate(senders):
            print(f"  수신스레드 {lane} ({sender.address}): 묶음 {sender.batches:,}통  주문 {sender.records:,}건")
    finally:
        for sender in senders:
            sender.close()

        context.term()

    return 0


if __name__ == "__main__":
    sys.exit(main())
