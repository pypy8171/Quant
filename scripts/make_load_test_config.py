"""부하시험용 config 를 만든다 — 실계좌 config 는 건드리지 않는다.

    py scripts/make_load_test_config.py --symbols 2700

받는 쪽(quant_trader)이 이 config 로 뜨면 KIS 대신 부하시험 주문 수신단이 피드 자리에 들어가고,
기동하면서 종목 순번표(load_test.universe_out)를 적는다. 인젝터는 그 파일을 읽어 순번을 맞춘다.

인증 정보는 넣지 않는다 — 이 경로는 KisClient 를 만들지 않으므로 자리만 채운 더미면 된다.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
_SCAN_PATH = _REPO_ROOT / "Quant" / "config" / "universe_scan.json"
_FULL_PATH = _REPO_ROOT / "Quant" / "config" / "universe_full.json"
_OUT_PATH = _REPO_ROOT / "Quant" / "config" / "config_load_test.json"
_UNIVERSE_OUT = _REPO_ROOT / "Quant" / "config" / "load_test_universe.json"


def collect_tickers(symbol_count: int) -> list[str]:
    """종목코드를 모은다. 실제 목록이 모자라면 합성 코드로 채운다 — 부하시험이라 코드가 실재할 필요는 없다."""
    tickers: list[str] = []

    if _FULL_PATH.exists():
        full = json.loads(_FULL_PATH.read_text(encoding="utf-8"))
        tickers = [str(code) for code in full.get("codes", [])]

    if not tickers and _SCAN_PATH.exists():
        scanned = json.loads(_SCAN_PATH.read_text(encoding="utf-8"))
        tickers = [str(entry["ticker"]) for entry in scanned.get("universe", [])]

    tickers = tickers[:symbol_count]

    for extra_index in range(len(tickers), symbol_count):
        tickers.append(f"9{extra_index:05d}")

    return tickers


def build_config(tickers: list[str], arguments: argparse.Namespace) -> dict:
    # 체결(TradeData)에 반응하는 전략이어야 한다 — MA_CROSS 는 on_data(분봉/REST)만 받아서 부하시험 체결을
    #  한 건도 보지 못한다. FIXED_INTERVAL 은 on_trade 에서 매수·매도를 번갈아 내므로 OrderGate·원장·모의체결까지
    #  실제로 태운다.
    strategies = [
        {
            "type": "FIXED_INTERVAL",
            "ticker": ticker,
            "buy_qty": arguments.quantity,
            "sell_qty": arguments.quantity,
            "interval_sec": arguments.interval_sec,
        }
        for ticker in tickers
    ]

    return {
        "//": "부하시험 전용. KIS 로 나가는 주문 없음 — 피드 자리에 exchange::ZmqOrderFeed 가 들어간다.",
        "kis": {
            "app_key": "LOAD_TEST_NO_KEY",
            "app_secret": "LOAD_TEST_NO_SECRET",
            "account_no": "00000000",
            "account_type": "01",
            "is_paper": True,
        },
        "mode": "TRADE",
        "strategy_shards": arguments.strategy_shards,
        "load_test": {
            "enabled": True,
            "lanes": arguments.lanes,
            "base_port": arguments.base_port,
            "bind_addr": "tcp://127.0.0.1",
            # 시험을 아무 때나 돌리니 체결 시각은 장중으로 찍는다 — 전략의 09:00~15:30 창에 걸리지 않게.
            "session_start_hhmmss": arguments.session_start_hhmmss,
            "universe_out": str(_UNIVERSE_OUT.relative_to(_REPO_ROOT)).replace("\\", "/"),
        },
        "replay_cash": arguments.cash,
        # 주문 의도·수락·체결의 순서를 남긴다. 주문 수는 체결 수보다 네 자리 적어 부하에 영향이 없다.
        "ledger_journal_dir": arguments.ledger_journal_dir,
        # 받은 체결 원본. 체결 한 건이 84바이트라 전속력 회차에서는 초당 수백 MB가 되어 재려던 처리량을
        #  바꿔 놓는다 — 같은 씨앗으로 입력이 재현되므로, 결과 대조가 필요한 짧은 회차에서만 켠다.
        "capture_dir": arguments.capture_dir,
        "tickers": tickers,
        "risk": {
            # 부하시험은 게이트에 걸려 주문이 사라지면 잴 것이 없어진다 — 한도를 크게 연다.
            "max_qty_per_ticker": 1_000_000_000,
            "max_qty_per_order": 1_000_000,
            "max_orders_per_sec": 1_000_000,
            "max_orders_per_min": 60_000_000,
            "dedup_window_sec": 0,
            # KIS 초당 거래건수 상한을 피하려고 두는 발주 간격. 모의 체결기에는 상한이 없고,
            #  350ms 그대로면 주문 스레드가 초당 세 건밖에 못 내보내 부하가 거기서 멈춘다.
            "order_min_interval_ms": 0,
            # 손실 한도는 음수로 준다 — daily_pnl <= limit 이면 BUY 거부라, 양수면 첫 주문부터 전부 막힌다.
            "daily_loss_limit": -1e15,
            "max_concurrent_positions": 0,
        },
        "strategies": strategies,
    }


def parse_arguments(argument_list: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="부하시험용 config 생성기")
    parser.add_argument("--symbols", type=int, default=2700, help="종목 수")
    parser.add_argument("--lanes", type=int, default=1, help="주문 수신 스레드 수")
    parser.add_argument("--base-port", type=int, default=5600, help="수신 스레드 0번 포트")
    parser.add_argument("--strategy-shards", type=int, default=4, help="전략 샤드 스레드 수")
    parser.add_argument("--interval-sec", type=int, default=1,
                        help="종목 하나가 신호를 내는 최소 간격(초). 0 이면 체결마다 낸다")
    parser.add_argument("--quantity", type=int, default=1, help="전략이 낼 주문 수량")
    parser.add_argument("--session-start-hhmmss", type=int, default=90000,
                        help="체결에 찍을 장중 시각 시작점. 0 이면 실제 시계")
    parser.add_argument("--cash", type=float, default=1e13, help="모의 체결기 현금")
    parser.add_argument("--ledger-journal-dir", default="logs",
                        help="원장 저널(ledger_YYYYMMDD.bin) 폴더. 빈 문자열이면 안 남긴다")
    parser.add_argument("--capture-dir", default="",
                        help="체결 원본(ticks_*.bin) 폴더. 기본은 끔 — 전속력 회차에서 켜면 처리량이 달라진다")
    parser.add_argument("--out", default=str(_OUT_PATH), help="출력 config 경로")

    return parser.parse_args(argument_list)


def main(argument_list: list[str] | None = None) -> int:
    arguments = parse_arguments(argument_list)
    tickers = collect_tickers(arguments.symbols)
    config = build_config(tickers, arguments)

    out_path = Path(arguments.out)
    out_path.write_bytes(json.dumps(config, ensure_ascii=False, indent=1).encode("utf-8"))

    print(f"적었다: {out_path}  종목 {len(tickers):,}개  전략 {len(config['strategies']):,}개")
    print(f"종목 순번표는 받는 쪽이 기동하면서 적는다: {_UNIVERSE_OUT}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
