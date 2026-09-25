# -*- coding: utf-8 -*-
"""틱 캡처 파일(ticks_<UTC초>.bin)에서 엔진이 실제로 받은 체결 수를 종목별로 센다.

엔진이 체결을 빠짐없이 받는지 보려는 도구다. KIS가 체결마다 주는 누적거래량(accumulated_volume)과
엔진이 받은 체결 수량의 합을 맞대 본다 — 받은 체결이 빠졌으면 수량 합이 누적거래량 증가분보다 작다.

형식 정본은 Quant/include/core/TickCapture.h (머리 16바이트, 레코드 = uint16 길이 + uint8 종류 + uint8 버전 + 본문).

사용:  py scripts/capture_stats.py <캡처 파일|폴더> [--ticker 000660] [--date YYYY-MM-DD] [--minutes]
"""
from __future__ import annotations

import argparse
import datetime as dt
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

MAGIC = b"QTCAP\x00"
HEADER_BYTES = 16
KIND_TRADE = 1
KIND_BOOK = 2
KIND_UNIVERSE = 4
# Common 48바이트: received_ns(q) wall_us(q) ticker(12s) time(8s) symbol_id(I) market(B) direction(B) pad(6)
COMMON = struct.Struct("<qq12s8sIBB6x")
# TradeBody 80바이트: Common + price(d) quantity(q) strength(d) accumulated_volume(q)
TRADE = struct.Struct("<qq12s8sIBB6xdqdq")
UNIVERSE_TRADE_ONLY_OFFSET = COMMON.size
KST = dt.timezone(dt.timedelta(hours=9))


@dataclass
class TickerStats:
    trades: int = 0
    books: int = 0
    quantity_sum: int = 0
    first_quantity: int = 0
    first_accumulated: int = -1
    last_accumulated: int = -1
    accumulated_backward: int = 0  # 누적거래량이 줄어든 횟수(순서가 뒤바뀐 체결)
    per_minute: Counter = field(default_factory=Counter)
    per_second_peak: int = 0
    first_kst: str = ""
    last_kst: str = ""
    _second: int = -1
    _second_count: int = 0

    def add_trade(self, wall_us: int, quantity: int, accumulated: int) -> None:
        stamp = dt.datetime.fromtimestamp(wall_us / 1_000_000, KST)
        self.trades += 1
        self.quantity_sum += quantity
        self.per_minute[stamp.strftime("%H:%M")] += 1

        second = wall_us // 1_000_000

        if second == self._second:
            self._second_count += 1
        else:
            self._second, self._second_count = second, 1

        self.per_second_peak = max(self.per_second_peak, self._second_count)

        if self.first_accumulated < 0:
            self.first_accumulated, self.first_quantity = accumulated, quantity
            self.first_kst = stamp.strftime("%H:%M:%S")
        elif accumulated < self.last_accumulated:
            self.accumulated_backward += 1

        self.last_accumulated = max(self.last_accumulated, accumulated)
        self.last_kst = stamp.strftime("%H:%M:%S")

    def volume_match(self) -> float | None:
        """첫 체결 뒤 받은 수량 합 ÷ 누적거래량 증가분. 1.0이면 빠진 체결이 없다."""
        grown = self.last_accumulated - self.first_accumulated

        if self.trades < 2 or grown <= 0:
            return None

        return (self.quantity_sum - self.first_quantity) / grown


@dataclass
class CaptureSummary:
    path: Path
    start_kst: dt.datetime
    universe: dict[str, bool] = field(default_factory=dict)  # 종목 → 체결만 구독
    tickers: dict[str, TickerStats] = field(default_factory=lambda: defaultdict(TickerStats))
    truncated: bool = False


def text_of(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def summarize(path: Path) -> CaptureSummary:
    """캡처 파일 하나를 끝까지 읽어 종목별 집계를 낸다. 꼬리가 잘렸으면 그 앞까지만 센다."""
    data = path.read_bytes()

    if len(data) < HEADER_BYTES or not data.startswith(MAGIC):
        raise ValueError(f"캡처 파일이 아니다: {path}")

    start_ms = struct.unpack_from("<q", data, 8)[0]
    summary = CaptureSummary(path, dt.datetime.fromtimestamp(start_ms / 1000, KST))
    offset = HEADER_BYTES

    while offset + 4 <= len(data):
        length, kind, _version = struct.unpack_from("<HBB", data, offset)
        body = offset + 4

        if body + length > len(data):
            summary.truncated = True
            break

        if kind == KIND_TRADE and length >= TRADE.size:
            _received, wall_us, ticker, _time, _symbol, _market, _direction, _price, quantity, _strength, accumulated = (
                TRADE.unpack_from(data, body))
            summary.tickers[text_of(ticker)].add_trade(wall_us, quantity, accumulated)
        elif kind == KIND_BOOK and length >= COMMON.size:
            summary.tickers[text_of(COMMON.unpack_from(data, body)[2])].books += 1
        elif kind == KIND_UNIVERSE and length > UNIVERSE_TRADE_ONLY_OFFSET:
            ticker = text_of(COMMON.unpack_from(data, body)[2])
            summary.universe[ticker] = data[body + UNIVERSE_TRADE_ONLY_OFFSET] != 0

        offset = body + length

    return summary


def capture_files(target: Path, date: str | None) -> list[Path]:
    """파일이면 그것 하나, 폴더면 ticks_*.bin 중 시작일(KST)이 date인 것."""
    if target.is_file():
        return [target]

    files = []

    for path in sorted(target.glob("ticks_*.bin")):
        try:
            start = dt.datetime.fromtimestamp(int(path.stem.split("_", 1)[1]), KST)
        except ValueError:
            continue

        if date is None or start.strftime("%Y-%m-%d") == date:
            files.append(path)

    return files


def print_summary(summary: CaptureSummary, only: str | None, minutes: bool) -> None:
    trade_only = sum(summary.universe.values())
    print(f"{summary.path.as_posix()}  시작 {summary.start_kst:%Y-%m-%d %H:%M:%S} KST  "
          f"구독 {len(summary.universe)}종목(체결만 {trade_only})" + ("  [꼬리 잘림]" if summary.truncated else ""))

    for ticker, statistics in sorted(summary.tickers.items(), key=lambda item: -item[1].trades):
        if only and ticker != only:
            continue

        match = statistics.volume_match()
        match_text = "-" if match is None else f"{match * 100:.2f}%"
        busiest = statistics.per_minute.most_common(1)
        busiest_text = f"{busiest[0][0]} {busiest[0][1]}건" if busiest else "-"
        print(f"  {ticker:8} 체결 {statistics.trades:7}건 호가 {statistics.books:7}건  {statistics.first_kst}~{statistics.last_kst}  "
              f"분당 최대 {busiest_text}  초당 최대 {statistics.per_second_peak}건  "
              f"수량/누적거래량 {match_text}  역순 {statistics.accumulated_backward}")

        if minutes:
            for minute in sorted(statistics.per_minute):
                print(f"      {minute} {statistics.per_minute[minute]:5}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", type=Path, help="캡처 파일 또는 ticks_*.bin 폴더")
    parser.add_argument("--ticker", help="이 종목만 찍는다")
    parser.add_argument("--date", help="폴더일 때 시작일(KST) YYYY-MM-DD")
    parser.add_argument("--minutes", action="store_true", help="분별 체결 수를 같이 찍는다")
    arguments = parser.parse_args()

    files = capture_files(arguments.target, arguments.date)

    if not files:
        print(f"캡처 파일 없음: {arguments.target.as_posix()} {arguments.date or ''}")
        return 1

    for path in files:
        print_summary(summarize(path), arguments.ticker, arguments.minutes)

    return 0


if __name__ == "__main__":
    sys.exit(main())
