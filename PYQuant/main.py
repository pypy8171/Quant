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
  python main.py record                          # ZMQ 이벤트 → TimescaleDB 적재
  python main.py operate status                  # 엔진 상태 조회
  python main.py operate kill                    # 엔진 종료
"""
import argparse
import sys
import unicodedata
from pathlib import Path

# python/ 폴더를 패키지 루트로
sys.path.insert(0, str(Path(__file__).parent))

from core.logger import setup_logger
from kis.client import from_config, KisAuthError

logger = setup_logger("quant.main")
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


def make_source(source: str, market: str = "kospi"):
    """데이터 소스 생성. datagokr(공공데이터포털 금융위, 권장)/krx(pykrx)/kis(REST)."""
    if source == "datagokr":
        from data.datagokr_source import DataGoKrSource
        return DataGoKrSource(market={"kospi": "KOSPI", "kosdaq": "KOSDAQ",
                                       "all": "ALL"}[market])
    elif source == "krx":
        from data.krx_source import KrxSource
        return KrxSource(market="KOSPI")
    # 백테스트는 OHLCV만(계좌 무관) — 모의 config로 엔진과 토큰 공유(403 회피)
    return from_config(_resolve_config(True))


def make_strategy(name: str, *, top_n: int, rebalance_every: int, lookback: int,
                  skip: int, vol_adjust: bool, pbr: float = 1.0, qty: int = 1):
    if name == "value_contrary":
        return ValueContraryStrategy(pbr_max=pbr, quantity=qty)
    if name == "strategy_a":
        from strategy.strategy_a import StrategyA
        return StrategyA()
    if name == "momentum":
        from strategy.cross_momentum import CrossMomentumStrategy
        return CrossMomentumStrategy(top_n=top_n, rebalance_every=rebalance_every,
                                     lookback=lookback, skip=skip, vol_adjust=vol_adjust)
    if name == "supply_demand":
        from strategy.supply_demand_rank import SupplyDemandRankStrategy
        return SupplyDemandRankStrategy(top_n=top_n, rebalance_every=rebalance_every)
    raise ValueError(f"알 수 없는 전략: {name}")


def select_universe(kis, source: str, *, from_date: str, universe_size: int,
                    kosdaq_size: int | None, use_kis_universe: bool = False,
                    pbr: float = 1.0) -> list[str]:
    """유니버스 선정: datagokr/krx 시총상위(as-of 시작일 고정) > 정적 > KIS PBR > 기본.
    스윕에서 1회 호출해 재사용(같은 from_date/market/size면 동일 유니버스)."""
    if source in ("datagokr", "krx") and hasattr(kis, "universe_top"):
        if source == "datagokr":
            sizes = {"KOSPI": universe_size, "KOSDAQ": kosdaq_size or universe_size}
            u = kis.universe_top(from_date, universe_size, sizes=sizes)
        else:
            u = kis.universe_top(from_date, universe_size)
        if u:
            return u
        if source == "krx":
            from data.universe_kospi import universe_codes
            return universe_codes()   # ⚠ KRX 차단 폴백 — survivorship bias
        logger.error("datagokr 유니버스 조회 실패 — DATA_GO_KR_KEY 확인")
    elif use_kis_universe:
        return kis.fetch_universe(max_pbr=pbr)
    return DEFAULT_UNIVERSE


def run_backtest(kis, *, strategy_name: str, universe: list[str], from_date: str, to_date: str,
                 top_n: int = 20, rebalance_every: int = 20, lookback: int = 120, skip: int = 20,
                 vol_adjust: bool = False, regime: bool = False, regime_ma: int = 200,
                 cash: float = 100_000_000, pbr: float = 1.0, qty: int = 1, verbose: bool = True):
    """엔진 1회 실행 — 재사용 가능(스윕·단발 공용). (result, names) 반환."""
    strategy = make_strategy(strategy_name, top_n=top_n, rebalance_every=rebalance_every,
                             lookback=lookback, skip=skip, vol_adjust=vol_adjust, pbr=pbr, qty=qty)
    warmup = (int((lookback + skip) * 2.1) + 20 if strategy_name == "momentum" else 30)
    if regime:
        warmup = max(warmup, int(regime_ma * 1.5) + 20)
    engine = BacktestEngine(kis, strategy, initial_cash=cash, target_positions=top_n,
                            warmup_days=warmup, regime_on=regime, regime_ma=regime_ma)
    result = engine.run(universe, start_date=from_date, end_date=to_date, verbose=verbose)
    return result, engine._names


def cmd_backtest(args):
    try:
        kis = make_source(args.source, args.market)
        if not kis.authenticate():
            raise KisAuthError("초기 인증 실패")
        universe = select_universe(kis, args.source, from_date=args.from_date,
                                   universe_size=args.universe_size, kosdaq_size=args.kosdaq_size,
                                   use_kis_universe=args.universe, pbr=args.pbr)
        logger.info(f"유니버스: {len(universe)}종목 (source={args.source}, as-of {args.from_date})")
        result, names = run_backtest(
            kis, strategy_name=args.strategy, universe=universe,
            from_date=args.from_date, to_date=args.to_date,
            top_n=args.top_n, rebalance_every=args.rebalance_every,
            lookback=args.lookback, skip=args.skip, vol_adjust=args.vol_adjust,
            regime=args.regime, regime_ma=args.regime_ma, cash=args.cash,
            pbr=args.pbr, qty=args.qty)
        print_report(result, names=names)
        if args.export:
            from backtest.report import (export_daily_csv, export_trades_csv,
                                         export_holdings_csv)
            base = args.export.rsplit(".", 1)[0]
            export_daily_csv(result, args.export)
            export_trades_csv(result, base + "_trades.csv", names=names)
            export_holdings_csv(result, base + "_holdings.csv", names=names)
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def cmd_monitor(args):
    from datetime import datetime, timezone
    monitor = EngineMonitor(host=args.host, pub_port=args.port)

    if args.topics:
        monitor._sub.subscribe(*args.topics)

    def fmt_ts(data: dict) -> str:
        ts = data.get("ts", 0)
        return datetime.fromtimestamp(ts / 1000, tz=timezone.utc).strftime("%H:%M:%S.%f")[:-3]

    monitor.on_trade = lambda d: logger.info(
        f"[{fmt_ts(d)}] TRADE  {d.get('ticker')} "
        f"{'▲' if d.get('direction') == 1 else '▼'} "
        f"{d.get('price'):,.0f}  vol={d.get('volume')}"
    )
    monitor.on_signal = lambda d: logger.info(
        f"[{fmt_ts(d)}] SIGNAL [{d.get('strategy')}] "
        f"{d.get('ticker')} {d.get('side')} {d.get('qty')}주"
    )
    monitor.on_order = lambda d: logger.info(
        f"[{fmt_ts(d)}] ORDER  {d.get('ticker')} {d.get('side')} "
        f"{d.get('qty')}주  {'✓' if d.get('ok') else '✗'}"
    )
    monitor.on_health = lambda d: logger.info(
        f"[{fmt_ts(d)}] HEALTH data={d.get('data')} "
        f"signal={d.get('signal')} order={d.get('order')}"
    )
    monitor.run()


def cmd_record(args):
    from db.client import DbClient
    db = DbClient()

    monitor = EngineMonitor(host=args.host, pub_port=args.port)
    def _rec_fill(d):
        db.insert_fill(d)
        db.upsert_position(d["ticker"], d["net_qty"], d["avg_price"],
                           d.get("realized_pnl", 0.0))
        sg = '+' if d.get("realized_pnl", 0) >= 0 else ''
        logger.info(f"REC FILL   {d.get('ticker')} {d.get('side')} "
                    f"{d.get('filled_qty')}주 @{d.get('filled_price'):,.0f}  "
                    f"avg={d.get('avg_price'):,.0f}  "
                    f"pnl={sg}{d.get('realized_pnl', 0):,.0f}")

    monitor.on_trade  = lambda d: (db.insert_trade(d),  logger.info(f"REC TRADE  {d.get('ticker')} {d.get('price'):,.0f}"))
    monitor.on_signal = lambda d: (db.insert_signal(d), logger.info(f"REC SIGNAL {d.get('ticker')} {d.get('side')}"))
    monitor.on_order  = lambda d: (db.insert_order(d),  logger.info(f"REC ORDER  {d.get('ticker')} {'OK' if d.get('ok') else 'FAIL'}"))
    monitor.on_health = lambda d: (db.insert_health(d), logger.info(f"REC HEALTH data={d.get('data')} sig={d.get('signal')} ord={d.get('order')}"))
    monitor.on_fill   = _rec_fill

    logger.info(f"ZMQ({args.host}:{args.port}) → TimescaleDB 적재 시작 (Ctrl+C로 종료)")
    try:
        monitor.run()
    finally:
        db.close()


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


def _dwidth(s) -> int:
    """터미널 표시 너비 — 한글/전각 문자는 2칸으로 계산 (East-Asian Width)."""
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in str(s))


def _ljust_d(s, width: int) -> str:
    return str(s) + " " * max(0, width - _dwidth(s))


def _rjust_d(s, width: int) -> str:
    return " " * max(0, width - _dwidth(s)) + str(s)


def _resolve_config(paper: bool) -> str:
    """host/docker 양쪽에서 config 경로 해석. docker 판별은 cwd의 config/ 존재
    여부(취약)가 아니라 /.dockerenv 명시 신호로 — 실/모의 오선택 방지 (P-1)."""
    fname = "config_paper.json" if paper else "config.json"
    if Path("/.dockerenv").exists():                       # docker: WORKDIR /app + config 볼륨 마운트
        return str(Path("config") / fname)
    return str(Path(__file__).parents[1] / "Quant" / "config" / fname)  # host: repo/Quant/config


def cmd_balance(args):
    import time, os
    try:
        kis = from_config(_resolve_config(args.paper))
        if not kis.authenticate():
            raise KisAuthError("초기 인증 실패")

        def show():
            items, s = kis.get_kr_balance()
            if args.watch:
                os.system("cls" if os.name == "nt" else "clear")
            from datetime import datetime
            print(f"  [{datetime.now().strftime('%H:%M:%S')}] 국내주식 잔고")
            print(f"  {'─'*60}")
            print(f"  예수금       {s.cash:>15,.0f}원")
            print(f"  총평가금액   {s.total_eval:>15,.0f}원")
            sg = '+' if s.total_pnl >= 0 else ''
            print(f"  총손익       {sg}{s.total_pnl:>14,.0f}원  ({sg}{s.total_pnl_rate:.2f}%)")
            print(f"  {'─'*60}")
            if items:
                print(f"  {_ljust_d('종목명', 14)} {_rjust_d('수량', 6)} "
                      f"{_rjust_d('평균단가', 11)} {_rjust_d('현재가', 11)} "
                      f"{_rjust_d('평가손익', 13)} {_rjust_d('수익률', 8)}")
                print(f"  {'─'*60}")
                for it in items:
                    sg = '+' if it.pnl >= 0 else ''
                    print(f"  {_ljust_d(it.name, 14)} {_rjust_d(f'{it.quantity:,}', 6)} "
                          f"{_rjust_d(f'{it.avg_price:,.0f}', 11)} {_rjust_d(f'{it.current_price:,.0f}', 11)} "
                          f"{_rjust_d(f'{sg}{it.pnl:,.0f}', 13)} {_rjust_d(f'{sg}{it.pnl_rate:.2f}%', 8)}")
            else:
                print("  보유 종목 없음")
            print()

        if args.watch:
            logger.info(f"잔고 {args.interval}초마다 갱신 (Ctrl+C로 종료)")
            while True:
                show()
                time.sleep(args.interval)
        else:
            show()
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def cmd_report(args):
    try:
        kis = from_config(_resolve_config(args.paper))
        if not kis.authenticate():
            raise KisAuthError("초기 인증 실패")
        account = "paper" if kis.is_paper else "real"

        db = None
        try:
            from db.client import DbClient
            db = DbClient()
            db.ensure_report_tables()
        except Exception as e:
            logger.warning(f"DB 미연결 — 현재 잔고만 표시 (스냅샷/입출금 비활성): {e}")

        from report.account import AccountReport
        rpt = AccountReport(kis, db, account)

        if args.snapshot:
            _, s = rpt.snapshot()
            print(f"[{account}] 잔고 스냅샷 적재: 총평가 {s.total_eval:,.0f}원")
        if args.deposit is not None:
            rpt.record_cash_flow("DEPOSIT", args.deposit, args.memo or "")
            print(f"[{account}] 입금 {args.deposit:,.0f}원 기록")
        if args.withdraw is not None:
            rpt.record_cash_flow("WITHDRAW", args.withdraw, args.memo or "")
            print(f"[{account}] 출금 {args.withdraw:,.0f}원 기록")

        if not args.no_report:
            rpt.print_report(args.from_date, args.to_date, show_trades=args.trades)

        if db:
            db.close()
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def cmd_live(args):
    try:
        kis = from_config()
        if not kis.authenticate():
            raise KisAuthError("초기 인증 실패 — app_key/app_secret 확인 필요")

        strategy = ValueContraryStrategy(pbr_max=args.pbr, quantity=args.qty)
        trader   = LiveTrader(kis, strategy, poll_sec=60, dry_run=args.dry_run)

        universe = DEFAULT_UNIVERSE
        if args.universe:
            logger.info("Universe 조회 중...")
            universe = kis.fetch_universe(max_pbr=args.pbr)

        trader.run(universe)
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def main():
    parser = argparse.ArgumentParser(description="Python 퀀트 트레이딩")
    sub = parser.add_subparsers(dest="cmd")

    # ── backtest ────────────────────────────────────────────────────────────
    bp = sub.add_parser("backtest", help="백테스팅")
    bp.add_argument("--from",     dest="from_date", default="2023-01-01")
    bp.add_argument("--to",       dest="to_date",   default="2024-12-31")
    bp.add_argument("--pbr",      type=float,        default=1.0)
    bp.add_argument("--qty",      type=int,          default=1)
    bp.add_argument("--cash",     type=float,        default=100_000_000)
    bp.add_argument("--universe", action="store_true", help="KIS API로 Universe 동적 조회")
    bp.add_argument("--strategy", default="value_contrary",
                    choices=["value_contrary", "strategy_a", "momentum", "supply_demand"],
                    help="백테스트 전략 (기본: value_contrary)")
    bp.add_argument("--source", default="kis", choices=["kis", "krx", "datagokr"],
                    help="데이터 소스 (datagokr=공공데이터포털 금융위 point-in-time 권장, "
                         "krx=pykrx OHLCV, kis=REST)")
    bp.add_argument("--top-n",           dest="top_n",           type=int, default=10,
                    help="동일가중 보유 종목수 (수급/모멘텀)")
    bp.add_argument("--rebalance-every", dest="rebalance_every", type=int, default=5,
                    help="리밸런싱 주기 (거래일)")
    bp.add_argument("--universe-size",   dest="universe_size",   type=int, default=100,
                    help="시총 상위 N 유니버스 (datagokr ALL이면 시장별 각 N)")
    bp.add_argument("--market", default="kospi", choices=["kospi", "kosdaq", "all"],
                    help="datagokr 유니버스 시장 (all=KOSPI 상위N + KOSDAQ 상위M)")
    bp.add_argument("--kosdaq-size", dest="kosdaq_size", type=int, default=None,
                    help="datagokr all 모드에서 KOSDAQ 종목수 (미지정 시 --universe-size와 동일)")
    bp.add_argument("--lookback", type=int, default=120,
                    help="모멘텀 lookback 거래일 (기본 120≈6개월)")
    bp.add_argument("--skip", type=int, default=20,
                    help="모멘텀 최근 제외 거래일 (기본 20≈1개월)")
    bp.add_argument("--vol-adjust", dest="vol_adjust", action="store_true",
                    help="변동성조정 모멘텀(점수=수익률/변동성) — 펌프주 강등")
    bp.add_argument("--regime", action="store_true",
                    help="시장국면 필터 — 유니버스 200일선 breadth<50%%면 전량 현금(하락장 방어)")
    bp.add_argument("--regime-ma", dest="regime_ma", type=int, default=200,
                    help="국면판정 이동평균 기간(거래일, 기본 200)")
    bp.add_argument("--export", default=None,
                    help="일별 상태 CSV 저장 경로 (매일매일 자산/수익률/낙폭/벤치)")

    # ── live ────────────────────────────────────────────────────────────────
    lp = sub.add_parser("live", help="실전 매매")
    lp.add_argument("--pbr",      type=float,  default=1.0)
    lp.add_argument("--qty",      type=int,    default=1)
    lp.add_argument("--dry-run",  action="store_true", help="주문 없이 시뮬")
    lp.add_argument("--universe", action="store_true")

    # ── balance ─────────────────────────────────────────────────────────────
    blp = sub.add_parser("balance", help="국내주식 잔고 조회")
    blp.add_argument("--paper",    action="store_true", help="모의투자 계좌(config_paper.json) 대상")
    blp.add_argument("--watch",    action="store_true", help="N초마다 자동 갱신")
    blp.add_argument("--interval", type=int, default=30, help="갱신 주기(초, 기본 30)")

    # ── report ──────────────────────────────────────────────────────────────
    rp2 = sub.add_parser("report", help="계좌 입출금/수익률/거래내역 리포트")
    rp2.add_argument("--paper",    action="store_true", help="모의투자 계좌(config_paper.json) 대상")
    rp2.add_argument("--from",     dest="from_date", default=None, help="기간 시작 YYYY-MM-DD")
    rp2.add_argument("--to",       dest="to_date",   default=None, help="기간 종료 YYYY-MM-DD")
    rp2.add_argument("--snapshot", action="store_true", help="현재 잔고를 스냅샷으로 적재 (EOD 1회 권장)")
    rp2.add_argument("--deposit",  type=float, default=None, help="입금액 기록 (실전)")
    rp2.add_argument("--withdraw", type=float, default=None, help="출금액 기록 (실전)")
    rp2.add_argument("--memo",     default=None, help="입출금 메모")
    rp2.add_argument("--trades",   action="store_true", help="거래내역(체결) 표시")
    rp2.add_argument("--no-report", action="store_true", help="적재만 하고 리포트 출력 생략")

    # ── monitor ─────────────────────────────────────────────────────────────
    mp = sub.add_parser("monitor", help="C++ 엔진 이벤트 실시간 출력")
    mp.add_argument("--host",   default="localhost")
    mp.add_argument("--port",   type=int, default=5555)
    mp.add_argument("--topics", nargs="*",
                    choices=["TRADE", "SIGNAL", "ORDER", "HEALTH"],
                    help="구독할 토픽 (기본: 전체)")

    # ── record ──────────────────────────────────────────────────────────────
    rp = sub.add_parser("record", help="ZMQ 이벤트 → TimescaleDB 적재")
    rp.add_argument("--host",   default="localhost")
    rp.add_argument("--port",   type=int, default=5555)

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
    elif args.cmd == "balance":
        cmd_balance(args)
    elif args.cmd == "report":
        cmd_report(args)
    elif args.cmd == "monitor":
        cmd_monitor(args)
    elif args.cmd == "record":
        cmd_record(args)
    elif args.cmd == "operate":
        cmd_operate(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
