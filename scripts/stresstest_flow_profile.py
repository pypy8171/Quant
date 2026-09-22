# -*- coding: utf-8 -*-
"""캡처한 실체결(QTCAP)에서 종목별 유량을 뽑아 전 종목 규모의 부하 프로파일을 만든다.

왜 필요한가. 부하 하네스의 1차·A회차는 초당 수십만 건을 밀어 넣는 천장 탐색이었다. 프로세스 분리 전후를 비교하려면
그 대신 **실제 장에서 오는 만큼**을 밀어야 한다. KIS WebSocket은 app_key당 41종목이 한계라 2,700종목 실시세를 직접
받을 수 없으므로, 41종목 캡처에서 종목별 초당 건수를 재고 그 모양을 순위별로 늘려 2,700종목 프로파일을 만든다.

입력은 엔진이 `capture_dir`에 남긴 `ticks_<epoch>.bin`(형식은 Quant/include/core/TickCapture.h).
출력은 JSON 하나 — 전체 초당 건수(중앙값·상위 1%)와 순위별 몫(share)이다. 하네스가 이 몫으로 종목에 유량을 나눠 준다.

쓰는 법(저장소 루트에서):
  py scripts/stresstest_flow_profile.py PYQuant/data/ticks_raw/ticks_1789947365.bin \\
      --symbols 2700 --out docs/reports/stresstest/data/2026-09-22_flow_profile.json

장 시간(09:00~15:30 KST) 표본만 쓴다 — 장 전후의 빈 구간을 섞으면 초당 건수가 실제보다 낮아진다.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

FILE_HEADER = struct.Struct("<6sBxq")      # "QTCAP\0" · 판 번호 · 시작 시각(ms)
RECORD_HEADER = struct.Struct("<HBB")      # 본문 길이 · 종류(1=체결 2=호가 3=봉 4=유니버스) · 판 번호
COMMON_PREFIX = struct.Struct("<qq12s")    # 받은 시각(ns) · 벽시계(µs) · 종목코드
KIND_TRADE = 1
FORMAT_VERSIONS = (1, 2)                   # v2에서 봉·유니버스 레코드가 늘었다. 체결 몸통은 그대로라 둘 다 읽는다


def read_trades(capture_path: Path):
    """체결 레코드를 (벽시계 초, 종목코드)로 흘려보낸다. 체결 아닌 종류는 길이만큼 건너뛴다. 잘린 꼬리에서 멈춘다."""
    with capture_path.open("rb") as handle:
        head = handle.read(FILE_HEADER.size)
        if len(head) < FILE_HEADER.size:
            return
        magic, version, _start_ms = FILE_HEADER.unpack(head)
        if magic != b"QTCAP\x00":
            raise SystemExit(f"QTCAP 파일이 아니다: {capture_path}")
        if version not in FORMAT_VERSIONS:
            raise SystemExit(f"모르는 판 번호 {version}: {capture_path}")

        while True:
            raw = handle.read(RECORD_HEADER.size)
            if len(raw) < RECORD_HEADER.size:
                return
            length, kind, _record_version = RECORD_HEADER.unpack(raw)
            body = handle.read(length)
            if len(body) < length:
                return
            if kind != KIND_TRADE:
                continue
            _received_ns, wall_us, ticker_bytes = COMMON_PREFIX.unpack_from(body, 0)
            ticker = ticker_bytes.split(b"\x00", 1)[0].decode("ascii", "replace")
            yield wall_us // 1_000_000, ticker


def percentile(sorted_values: list[int], fraction: float) -> int:
    if not sorted_values:
        return 0
    position = min(len(sorted_values) - 1, int(round(fraction * (len(sorted_values) - 1))))
    return sorted_values[position]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", type=Path, nargs="+", help="ticks_<epoch>.bin 하나 이상")
    parser.add_argument("--symbols", type=int, default=2700, help="늘릴 종목 수")
    parser.add_argument("--out", type=Path, help="프로파일 JSON 경로(없으면 화면)")
    parser.add_argument("--min-trades-per-second", type=int, default=1,
                        help="이 건수 밑인 초는 장이 멈춘 구간으로 보고 초당 건수 통계에서 뺀다")
    arguments = parser.parse_args()

    per_second: Counter[int] = Counter()
    per_symbol: Counter[str] = Counter()
    per_symbol_seconds: defaultdict[str, set] = defaultdict(set)

    for capture_path in arguments.captures:
        for second, ticker in read_trades(capture_path):
            per_second[second] += 1
            per_symbol[ticker] += 1
            per_symbol_seconds[ticker].add(second)

    if not per_second:
        print("체결 레코드가 없다", file=sys.stderr)
        return 1

    active = sorted(count for count in per_second.values() if count >= arguments.min_trades_per_second)
    observed_symbols = len(per_symbol)
    total_trades = sum(per_symbol.values())

    # 순위별 몫 — 관측한 종목 수보다 많이 늘릴 때는 꼬리를 멱법칙으로 잇는다.
    # 관측 구간의 몫이 순위 r에서 s(r)이면, 그 양 끝의 기울기로 s(r) = a * r^(-b) 를 맞춰 남은 순위를 채운다.
    ranked = [count / total_trades for _, count in per_symbol.most_common()]
    if arguments.symbols > observed_symbols and observed_symbols >= 2:
        first, last = ranked[0], ranked[-1]
        exponent = 0.0 if first <= last else (
            (__import__("math").log(first) - __import__("math").log(last)) / __import__("math").log(observed_symbols)
        )
        for rank in range(observed_symbols + 1, arguments.symbols + 1):
            ranked.append(first * rank ** (-exponent))
    shares = ranked[:arguments.symbols]
    share_total = sum(shares)
    shares = [share / share_total for share in shares]

    per_symbol_rate = {
        ticker: count / max(1, len(per_symbol_seconds[ticker]))
        for ticker, count in per_symbol.most_common(10)
    }

    profile = {
        "source_captures": [str(path) for path in arguments.captures],
        "observed_symbols": observed_symbols,
        "observed_seconds": len(active),
        "observed_trades": total_trades,
        "trades_per_second": {
            "p50": percentile(active, 0.50),
            "p90": percentile(active, 0.90),
            "p99": percentile(active, 0.99),
            "max": active[-1] if active else 0,
        },
        "scaled_symbols": arguments.symbols,
        # 관측 종목이 41개뿐이라 전 종목 초당 건수는 "관측 초당 건수 × (늘린 종목 수 / 관측 종목 수)"로 잡는다.
        # 대형주 편중을 몫으로 이미 반영하므로 이 환산은 종목 수에만 비례한다.
        "scaled_trades_per_second": {
            key: round(value * arguments.symbols / observed_symbols)
            for key, value in {
                "p50": percentile(active, 0.50),
                "p90": percentile(active, 0.90),
                "p99": percentile(active, 0.99),
                "max": active[-1] if active else 0,
            }.items()
        },
        "top_symbol_rates_per_second": {ticker: round(rate, 2) for ticker, rate in per_symbol_rate.items()},
        "rank_shares": [round(share, 8) for share in shares],
    }

    text = json.dumps(profile, ensure_ascii=False, indent=2)
    if arguments.out:
        arguments.out.parent.mkdir(parents=True, exist_ok=True)
        arguments.out.write_bytes(text.encode("utf-8"))
        summary = profile["scaled_trades_per_second"]
        print(f"{arguments.out} — 관측 {observed_symbols}종목 {total_trades}건 {len(active)}초, "
              f"{arguments.symbols}종목 환산 초당 p50 {summary['p50']} · p99 {summary['p99']} · 최대 {summary['max']}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
