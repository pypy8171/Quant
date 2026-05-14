"""
Python 퀀트 트레이딩 시스템 진입점

사용법:
  python main.py backtest                        # 기본 설정 백테스팅
  python main.py backtest --from 2025-01-01      # 기간 지정
  python main.py backtest --pbr 1.5             # PBR 상한 지정
  python main.py live                            # 실전 매매
  python main.py live --dry-run                  # 주문 없이 시뮬
  python main.py monitor                         # C++ 엔진 이벤트 실시간 출력
  python main.py monitor --topics TRADE SIGNAL   # 특정 토픽만 구독
  python main.py operate status                  # 엔진 상태 조회
  python main.py operate kill                    # 엔진 종료
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
from ipc.subscriber import EngineMonitor
from ipc.operator import ZmqOperator

# 기본 Universe — KOSPI 시가총액 상위 20 (분기 단위 검토)
DEFAULT_UNIVERSE = [
    "005930","000660","207940","005490","005380",
    "000270","105560","055550","035420","068270",
    "051910","066570","012330","035720","003550",
    "086790","017670","009150","402340","316140",
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


def cmd_monitor(args):
    from datetime import datetime, timezone
    monitor = EngineMonitor(host=args.host, pub_port=args.port)

    if args.topics:
        monitor._sub.subscribe(*args.topics)

    def fmt_ts(data: dict) -> str:
        ts = data.get("ts", 0)
        return datetime.fromtimestamp(ts / 1000, tz=timezone.utc).strftime("%H:%M:%S.%f")[:-3]

    monitor.on_trade = lambda d: print(
        f"[{fmt_ts(d)}] TRADE  {d.get('ticker')} "
        f"{'▲' if d.get('direction') == 1 else '▼'} "
        f"{d.get('price'):,.0f}  vol={d.get('volume')}"
    )
    monitor.on_signal = lambda d: print(
        f"[{fmt_ts(d)}] SIGNAL [{d.get('strategy')}] "
        f"{d.get('ticker')} {d.get('side')} {d.get('qty')}주"
    )
    monitor.on_order = lambda d: print(
        f"[{fmt_ts(d)}] ORDER  {d.get('ticker')} {d.get('side')} "
        f"{d.get('qty')}주  {'✓' if d.get('ok') else '✗'}"
    )
    monitor.on_health = lambda d: print(
        f"[{fmt_ts(d)}] HEALTH data={d.get('data')} "
        f"signal={d.get('signal')} order={d.get('order')}"
    )
    monitor.run()


def cmd_operate(args):
    with ZmqOperator(host=args.host, rep_port=args.port) as op:
        if args.action == "status":
            st = op.status()
            if st:
                print(f"엔진 상태: 실행중={st.get('running')}")
                print(f"  데이터 수집: {st.get('data')}건")
                print(f"  시그널:      {st.get('signal')}건")
                print(f"  주문:        {st.get('order')}건")
            else:
                print("엔진 응답 없음 (실행 중인지 확인하세요)")
        elif args.action == "kill":
            confirm = input("엔진을 종료하시겠습니까? (yes/no): ")
            if confirm.strip().lower() == "yes":
                ok = op.kill()
                print("종료 명령 전송 성공" if ok else "종료 실패 또는 타임아웃")
        else:
            print(f"알 수 없는 명령: {args.action}")


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

    # ── monitor ─────────────────────────────────────────────────────────────
    mp = sub.add_parser("monitor", help="C++ 엔진 이벤트 실시간 출력")
    mp.add_argument("--host",   default="localhost")
    mp.add_argument("--port",   type=int, default=5555)
    mp.add_argument("--topics", nargs="*",
                    choices=["TRADE", "SIGNAL", "ORDER", "HEALTH"],
                    help="구독할 토픽 (기본: 전체)")

    # ── operate ─────────────────────────────────────────────────────────────
    op = sub.add_parser("operate", help="C++ 엔진 원격 제어")
    op.add_argument("action", choices=["status", "kill"], help="실행할 명령")
    op.add_argument("--host", default="localhost")
    op.add_argument("--port", type=int, default=5556)

    args = parser.parse_args()

    if args.cmd == "backtest":
        cmd_backtest(args)
    elif args.cmd == "live":
        cmd_live(args)
    elif args.cmd == "monitor":
        cmd_monitor(args)
    elif args.cmd == "operate":
        cmd_operate(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
