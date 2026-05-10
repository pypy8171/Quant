"""
Python 퀀트 트레이딩 시스템 진입점

사용법:
  python main.py backtest                        # 기본 설정 백테스팅
  python main.py backtest --from 2023-01-01      # 기간 지정
  python main.py backtest --pbr 1.5             # PBR 상한 지정
  python main.py live                            # 실전 매매
  python main.py live --dry-run                  # 주문 없이 시뮬
"""
import argparse
import sys
from pathlib import Path

# python/ 폴더를 패키지 루트로
sys.path.insert(0, str(Path(__file__).parent))

from kis.client import from_config
from strategy.value_contrary import ValueContraryStrategy
from backtest.engine import BacktestEngine
from backtest.report import print_report
from live.trader import LiveTrader

# 기본 Universe (KOSPI 주요 20종목)
DEFAULT_UNIVERSE = [
    "005930","000660","005380","035420","051910",
    "006400","035720","028260","068270","012330",
    "066570","105560","055550","000270","017670",
    "030200","003550","009150","047050","096770",
]


def cmd_backtest(args):
    kis = from_config()
    if not kis.authenticate():
        print("인증 실패"); return

    strategy = ValueContraryStrategy(pbr_max=args.pbr, quantity=args.qty)
    engine   = BacktestEngine(kis, strategy, initial_cash=args.cash)

    universe = DEFAULT_UNIVERSE
    if args.universe:
        # Universe 동적 조회 (PBR 필터 포함)
        print("Universe 조회 중...")
        universe = kis.fetch_universe(max_pbr=args.pbr)
        print(f"Universe: {len(universe)}종목")

    result = engine.run(universe, start_date=args.from_date, end_date=args.to_date)
    print_report(result)


def cmd_live(args):
    kis = from_config()
    if not kis.authenticate():
        print("인증 실패"); return

    strategy = ValueContraryStrategy(pbr_max=args.pbr, quantity=args.qty)
    trader   = LiveTrader(kis, strategy, poll_sec=60, dry_run=args.dry_run)

    universe = DEFAULT_UNIVERSE
    if args.universe:
        print("Universe 조회 중...")
        universe = kis.fetch_universe(max_pbr=args.pbr)

    trader.run(universe)


def main():
    parser = argparse.ArgumentParser(description="Python 퀀트 트레이딩")
    sub = parser.add_subparsers(dest="cmd")

    # ── backtest ────────────────────────────────────────────────────────────
    bp = sub.add_parser("backtest", help="백테스팅")
    bp.add_argument("--from",     dest="from_date", default="2023-01-01")
    bp.add_argument("--to",       dest="to_date",   default="2024-12-31")
    bp.add_argument("--pbr",      type=float,        default=1.0)
    bp.add_argument("--qty",      type=int,          default=1)
    bp.add_argument("--cash",     type=float,        default=10_000_000)
    bp.add_argument("--universe", action="store_true", help="KIS API로 Universe 동적 조회")

    # ── live ────────────────────────────────────────────────────────────────
    lp = sub.add_parser("live", help="실전 매매")
    lp.add_argument("--pbr",      type=float,  default=1.0)
    lp.add_argument("--qty",      type=int,    default=1)
    lp.add_argument("--dry-run",  action="store_true", help="주문 없이 시뮬")
    lp.add_argument("--universe", action="store_true")

    args = parser.parse_args()

    if args.cmd == "backtest":
        cmd_backtest(args)
    elif args.cmd == "live":
        cmd_live(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
