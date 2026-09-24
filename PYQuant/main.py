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
import time
import unicodedata
from pathlib import Path

# 리코더 틱 배치 — 27종목이면 초당 수십 건, 전 시장으로 넓히면 수천 건이다. 건수와 시간 중 먼저 닿는 쪽에서 비운다.
TICK_FLUSH_ROWS = 500
TICK_FLUSH_SECONDS = 5.0

# python/ 폴더를 패키지 루트로
sys.path.insert(0, str(Path(__file__).parent))

# 예약작업·cp949 콘솔에서도 한글·대시(—)가 깨지지 않게 표준출력을 UTF-8로 둔다.
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="replace")

from core.logger import setup_logger
from kis.client import from_config, KisAuthError

logger = setup_logger("quant.main")
from strategy.value_contrary import ValueContraryStrategy
from backtest.engine import BacktestEngine
from backtest.report import print_report
from live.trader import LiveTrader
from ipc.subscriber import EngineMonitor
from ipc.operator import ZmqOperator

# 기본 Universe — KOSPI 시가총액 상위 20 (분기 단위 검토).
# 하드코딩 상수는 config/default_universe.json 부재·파싱실패 시의 **최종 폴백**으로만 남긴다
# (빈 유니버스로 조용히 진행하지 않도록 — 로드 실패는 WARN 후 이 상수 사용).
_FALLBACK_UNIVERSE = [
    "005930","000660","207940","005490","005380",
    "000270","105560","055550","035420","068270",
    "051910","066570","012330","035720","003550",
    "086790","017670","009150","402340","316140",
]
DEFAULT_UNIVERSE_FILE = Path(__file__).parent / "config" / "default_universe.json"


def load_default_universe(path=None, market: str = "kospi") -> list[str]:
    """기본 유니버스를 외부 json에서 로드. 실패 시 WARN 후 내장 상수 폴백(빈 유니버스 금지).
    json 형식: {"kospi": [...], "kosdaq": [...]}. path 미지정 시 DEFAULT_UNIVERSE_FILE."""
    import json
    p = Path(path) if path else DEFAULT_UNIVERSE_FILE
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
        codes = data.get(str(market).lower(), [])
        codes = [str(c) for c in codes if str(c).strip()]
        if codes:
            return codes
        logger.warning(f"기본 유니버스 파일에 '{market}' 항목이 비어있음({p}) — 내장 상수 폴백")
    except FileNotFoundError:
        logger.warning(f"기본 유니버스 파일 없음({p}) — 내장 상수 폴백")
    except Exception as e:
        logger.warning(f"기본 유니버스 파일 로드 실패({p}: {e}) — 내장 상수 폴백")
    return _FALLBACK_UNIVERSE


# 모듈 로드 시 1회 확정(외부 json → 실패 시 상수). 기존 참조는 그대로 이 이름을 쓴다.
DEFAULT_UNIVERSE = load_default_universe()


def make_source(source: str, market: str = "kospi"):
    """데이터 소스 생성. datagokr(공공데이터포털 금융위, 권장)/krx(pykrx)/kis(REST)."""
    if source == "datagokr":
        from data.datagokr_source import DataGoKrSource
        return DataGoKrSource(market={"kospi": "KOSPI", "kosdaq": "KOSDAQ",
                                       "all": "ALL"}[market])
    elif source == "krx":
        from data.krx_source import KrxSource
        return KrxSource(market="KOSPI")
    elif source == "yf":
        # datagokr 하한(2020) 이전 과거 하락장(2011·2018·2020 full) — 무키 Yahoo
        from data.yfinance_source import YFinanceSource
        return YFinanceSource(market="KOSPI")
    # 백테스트는 OHLCV만(계좌 무관) — 모의 config로 엔진과 토큰 공유(403 회피)
    return from_config(_resolve_config(True))


def make_strategy(name: str, *, top_n: int, rebalance_every: int, lookback: int,
                  skip: int, vol_adjust: bool, pbr: float = 1.0, qty: int = 1,
                  trend_ma: int = 0, abs_mom: bool = False):
    if name == "value_contrary":
        return ValueContraryStrategy(pbr_max=pbr, quantity=qty)
    if name == "strategy_a":
        from strategy.strategy_a import StrategyA
        return StrategyA()
    if name == "momentum":
        from strategy.cross_momentum import CrossMomentumStrategy
        return CrossMomentumStrategy(top_n=top_n, rebalance_every=rebalance_every,
                                     lookback=lookback, skip=skip, vol_adjust=vol_adjust,
                                     trend_ma=trend_ma, abs_mom=abs_mom)
    if name == "supply_demand":
        from strategy.supply_demand_rank import SupplyDemandRankStrategy
        return SupplyDemandRankStrategy(top_n=top_n, rebalance_every=rebalance_every)
    if name == "mean_reversion":
        # 횡단면 이격도 역추세(실행 #4). lookback을 이격 기준 SMA 창으로 재사용(예: --lookback 20).
        from strategy.mean_reversion import MeanReversionContraryStrategy
        return MeanReversionContraryStrategy(top_n=top_n, rebalance_every=rebalance_every,
                                             sma_period=lookback)
    raise ValueError(f"알 수 없는 전략: {name}")


def select_universe(kis, source: str, *, from_date: str, universe_size: int,
                    kosdaq_size: int | None, use_kis_universe: bool = False,
                    pbr: float = 1.0, default_universe: list[str] | None = None) -> list[str]:
    """유니버스 선정: datagokr/krx 시총상위(as-of 시작일 고정) > 정적 > KIS PBR > 기본.
    스윕에서 1회 호출해 재사용(같은 from_date/market/size면 동일 유니버스)."""
    if source in ("datagokr", "krx", "yf") and hasattr(kis, "universe_top"):
        if source == "datagokr":
            sizes = {"KOSPI": universe_size, "KOSDAQ": kosdaq_size or universe_size}
            u = kis.universe_top(from_date, universe_size, sizes=sizes)
        else:
            u = kis.universe_top(from_date, universe_size)
        if u:
            return u
        if source in ("krx", "yf"):
            from data.universe_kospi import universe_codes
            return universe_codes()   # ⚠ 폴백 — survivorship bias
        logger.error("datagokr 유니버스 조회 실패 — DATA_GO_KR_KEY 확인")
    elif use_kis_universe:
        return kis.fetch_universe(max_pbr=pbr)
    return default_universe if default_universe is not None else DEFAULT_UNIVERSE


def run_backtest(kis, *, strategy_name: str, universe: list[str], from_date: str, to_date: str,
                 top_n: int = 20, rebalance_every: int = 20, lookback: int = 120, skip: int = 20,
                 vol_adjust: bool = False, regime: bool = False, regime_ma: int = 200,
                 regime_thresh: float = 0.5, daily_regime: bool = False,
                 regime_mode: str = "breadth", regime_index: str = "^KS11",
                 vol_target: float = 0.0, vol_window: int = 20,
                 trend_ma: int = 0, abs_mom: bool = False,
                 cash: float = 100_000_000, pbr: float = 1.0, qty: int = 1, verbose: bool = True):
    """엔진 1회 실행 — 재사용 가능(스윕·단발 공용). (result, names) 반환."""
    strategy = make_strategy(strategy_name, top_n=top_n, rebalance_every=rebalance_every,
                             lookback=lookback, skip=skip, vol_adjust=vol_adjust, pbr=pbr, qty=qty,
                             trend_ma=trend_ma, abs_mom=abs_mom)
    warmup = (int((lookback + skip) * 2.1) + 20 if strategy_name == "momentum"
              else lookback + 20 if strategy_name == "mean_reversion"   # 이격 SMA 창 워밍업
              else 30)
    if regime:
        warmup = max(warmup, int(regime_ma * 1.5) + 20)
    if trend_ma:
        warmup = max(warmup, int(trend_ma * 1.5) + 20)
    engine = BacktestEngine(kis, strategy, initial_cash=cash, target_positions=top_n,
                            warmup_days=warmup, regime_on=regime, regime_ma=regime_ma,
                            regime_thresh=regime_thresh, daily_regime=daily_regime,
                            regime_mode=regime_mode, regime_index=regime_index,
                            vol_target=vol_target, vol_window=vol_window)
    result = engine.run(universe, start_date=from_date, end_date=to_date, verbose=verbose)
    return result, engine._names


def cmd_backtest(args):
    try:
        kis = make_source(args.source, args.market)
        if not kis.authenticate():
            raise KisAuthError("초기 인증 실패")
        default_uni = (load_default_universe(args.universe_file, args.market)
                       if getattr(args, "universe_file", None) else None)
        universe = select_universe(kis, args.source, from_date=args.from_date,
                                   universe_size=args.universe_size, kosdaq_size=args.kosdaq_size,
                                   use_kis_universe=args.universe, pbr=args.pbr,
                                   default_universe=default_uni)
        logger.info(f"유니버스: {len(universe)}종목 (source={args.source}, as-of {args.from_date})")
        result, names = run_backtest(
            kis, strategy_name=args.strategy, universe=universe,
            from_date=args.from_date, to_date=args.to_date,
            top_n=args.top_n, rebalance_every=args.rebalance_every,
            lookback=args.lookback, skip=args.skip, vol_adjust=args.vol_adjust,
            regime=args.regime, regime_ma=args.regime_ma, daily_regime=args.daily_regime,
            regime_mode=args.regime_mode, regime_index=args.regime_index,
            vol_target=args.vol_target, vol_window=args.vol_window,
            cash=args.cash, pbr=args.pbr, qty=args.qty)
        print_report(result, names=names)
        if args.export:
            import os
            from backtest.report import (export_daily_csv, export_trades_csv,
                                         export_holdings_csv, export_metrics_json)
            base = args.export.rsplit(".", 1)[0]
            export_daily_csv(result, args.export)
            export_trades_csv(result, base + "_trades.csv", names=names)
            export_holdings_csv(result, base + "_holdings.csv", names=names)
            # 정규화 지표 JSON (대시보드 데이터 계약 ① — docs/design/DASHBOARD_SPEC.md)
            export_metrics_json(
                result, base + "_metrics.json",
                study_id=args.study or "", strategy=args.strategy,
                event=args.event or "",
                window=f"{args.from_date}~{args.to_date}",
                oos_flag=args.oos, holdout_flag=args.holdout,
                honesty_label=args.honesty,
                equity_csv_path=os.path.basename(args.export),
                trades_csv_path=os.path.basename(base + "_trades.csv"))
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


# 역할별 발행 포트. 안 주면 주문 포트에서 끌어온다 — 엔진이 config를 읽을 때 쓰는 규칙과 같은 +2·+3이라
#  설정을 안 고쳐도 갈라 뜬 엔진에 그대로 붙는다. 0 이하는 "그 역할은 안 본다"가 아니라 "끌어와라"다.
#  [wire] Quant/src/core/AppConfig.cpp — zmq_feed_pub_port·zmq_strategy_pub_port 기본값
def role_pub_ports(order_port: int, feed_port: int, strategy_port: int) -> tuple[int, int]:
    return (feed_port if feed_port > 0 else order_port + 2,
            strategy_port if strategy_port > 0 else order_port + 3)


def cmd_monitor(args):
    from datetime import datetime, timezone
    monitor = EngineMonitor(host=args.host, pub_port=args.port,
                            extra_pub_ports=role_pub_ports(args.port, args.feed_port, args.strategy_port))

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
    db.ensure_fills_amount_columns()

    monitor = EngineMonitor(host=args.host, pub_port=args.port,
                            extra_pub_ports=role_pub_ports(args.port, args.feed_port, args.strategy_port))

    # 엔진이 아닌 것이 같은 포트를 물 수 있다. Engine 을 그대로 띄우는 테스트·부하 하네스(test_engine·
    #  bench_engine_load)도 setup_zmq_bridge 로 127.0.0.1:5555 에 bind 하는데, 트레이더가 WSL 안에 있으면
    #  Windows 쪽 bind 가 생기는 순간 이 리코더가 그쪽을 잡는다 — 09-22 장중에 합성 주문 46건·체결 51건이
    #  운영 표에 들어갔다. 주문·체결 메시지에는 계좌번호가 실려 오므로(ZmqBridge::publish_order/publish_fill)
    #  기대한 계좌가 아니면 버린다. 원장이 걸린 두 표만이라도 남의 데이터를 안 받게 하는 방어다.
    expected_account = (args.account or "").strip()
    rejected_accounts: set[str] = set()

    def is_our_account(data: dict) -> bool:
        if not expected_account:
            return True

        seen = str(data.get("account", "")).strip()

        if seen == expected_account:
            return True

        if seen not in rejected_accounts:
            rejected_accounts.add(seen)
            reason = ("계좌 없이 온 메시지 — 계좌를 안 주는 하네스이거나 account 필드 이전 엔진 바이너리다"
                      if not seen else "다른 엔진이 같은 ZMQ 포트를 쓰고 있다")
            logger.warning(f"REC 버림 계좌 '{seen}' — 기대 '{expected_account}'. {reason}.")

        return False

    def _rec_fill(d):
        if not is_our_account(d):
            return

        db.insert_fill(d)
        db.upsert_position(d["ticker"], d["net_qty"], d["avg_price"],
                           d.get("realized_pnl", 0.0),
                           account=d.get("account", "unknown"))
        sg = '+' if d.get("realized_pnl", 0) >= 0 else ''
        logger.info(f"REC FILL   {d.get('ticker')} {d.get('side')} "
                    f"{d.get('filled_qty')}주 @{d.get('filled_price'):,.0f}  "
                    f"avg={d.get('avg_price'):,.0f}  "
                    f"pnl={sg}{d.get('realized_pnl', 0):,.0f}")

    # 체결 틱은 리플레이 입력이 아니라(그건 엔진의 .bin 캡처가 맡는다) 그라파나 "피드 지연"·"초당 틱 유입"
    #  패널의 재료다. 틱마다 insert+commit 하면 커밋이 초당 수십 번이고 로그도 그만큼 불어나므로, 모아서
    #  executemany 로 한 번에 넣고 로그는 flush 단위로만 남긴다. flush 조건은 건수·시간 둘 다 — 조용한 구간에도
    #  버퍼가 몇 분씩 묶여 있으면 피드 지연 패널이 실제보다 늦게 보인다.
    tick_buffer: list[dict] = []
    tick_flush_deadline = [time.monotonic() + TICK_FLUSH_SECONDS]
    tick_total = [0]

    def flush_ticks(force: bool = False):
        if not tick_buffer:
            tick_flush_deadline[0] = time.monotonic() + TICK_FLUSH_SECONDS
            return

        if not force and len(tick_buffer) < TICK_FLUSH_ROWS and time.monotonic() < tick_flush_deadline[0]:
            return

        db.insert_trade_batch(tick_buffer)
        tick_total[0] += len(tick_buffer)
        logger.info(f"REC TRADE  {len(tick_buffer)}건 적재 (누적 {tick_total[0]})")
        tick_buffer.clear()
        tick_flush_deadline[0] = time.monotonic() + TICK_FLUSH_SECONDS

    def _rec_health(data: dict):
        flush_ticks(force=True)       # 30초 주기 HEALTH 가 조용한 구간의 flush 시계 노릇을 한다
        db.insert_health(data)
        logger.info(f"REC HEALTH data={data.get('data')} sig={data.get('signal')} ord={data.get('order')}")

    # 틱에도 계좌가 실린다(ZmqBridge::format_trade). ticks 표에는 계좌 열이 없어 주문·체결처럼 뒤에서 가려낼 수
    #  없으므로 들어오기 전에 버린다 — 09-22 장중 부하 하네스의 합성 틱이 09:42~09:57 사이 운영 표에 섞였다.
    if args.record_ticks:
        monitor.on_trade = lambda d: is_our_account(d) and (tick_buffer.append(d), flush_ticks())
    monitor.on_signal = lambda d: is_our_account(d) and (db.insert_signal(d), logger.info(f"REC SIGNAL {d.get('ticker')} {d.get('side')}"))
    monitor.on_order  = lambda d: is_our_account(d) and (db.insert_order(d),  logger.info(f"REC ORDER  {d.get('ticker')} {'OK' if d.get('ok') else 'FAIL'}"))
    monitor.on_health = _rec_health
    monitor.on_fill   = _rec_fill

    logger.info(f"ZMQ({args.host}:{args.port}) → TimescaleDB 적재 시작 (Ctrl+C로 종료)")
    try:
        monitor.run()
    finally:
        db.close()


def command_procwatch(arguments):
    from db.client import DbClient
    from core import proc_watch

    db = DbClient()
    db.ensure_proc_statistics_table()
    db.ensure_query_statistics()   # 그라파나 "DB 부하 쿼리" 표의 원천(pg_stat_statements)
    try:
        proc_watch.run(db, process_name=arguments.process_name, interval=arguments.interval,
                       wsl_distro=arguments.wsl_distro, perf_interval=arguments.perf_interval,
                       perf_seconds=arguments.perf_seconds)
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

        universe = (load_default_universe(args.universe_file)
                    if getattr(args, "universe_file", None) else DEFAULT_UNIVERSE)
        if args.universe:
            logger.info("Universe 조회 중...")
            universe = kis.fetch_universe(max_pbr=args.pbr)

        trader.run(universe)
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def cmd_forward(args):
    """forward-test: 검증된 regime-모멘텀을 KIS 모의계좌로 매매(기본 dry-run).
    데이터=datagokr(OHLCV/유니버스), 매매=KIS 모의계좌(is_paper). 실거래 경로 없음(안전)."""
    try:
        kis = from_config(_resolve_config(True))   # 항상 모의계좌(config_paper.json)
        if not kis.authenticate():
            raise KisAuthError("KIS 모의 인증 실패 — config_paper.json 확인")
        source = make_source("datagokr", "all")
        if not source.authenticate():
            raise KisAuthError("datagokr 인증 실패 — DATA_GO_KR_KEY 확인")
        from strategy.cross_momentum import CrossMomentumStrategy
        from live.forward_trader import ForwardTrader
        # footgun 가드: 백테스트로 검증·채택된 전략은 no-regime + vol_adjust. 플래그 누락 시 다른 전략을
        # 조용히 매매하게 되므로 불일치를 크게 경고(주문 전 사용자 인지 — S/W-1).
        if not (args.no_regime and args.vol_adjust):
            logger.warning(
                "채택 전략은 [--no-regime --vol-adjust] 임. 현재 "
                f"regime={'OFF' if args.no_regime else 'ON'}, vol_adjust={args.vol_adjust} "
                "→ 백테스트 검증 전략과 불일치. 의도한 게 아니면 중단하고 플래그 확인.")
        strategy = CrossMomentumStrategy(top_n=args.top_n, rebalance_every=args.rebalance_every,
                                         lookback=args.lookback, skip=args.skip,
                                         vol_adjust=args.vol_adjust)
        ft = ForwardTrader(kis, strategy, source, top_n=args.top_n, lookback=args.lookback,
                           skip=args.skip, rebalance_every=args.rebalance_every,
                           regime_on=not args.no_regime, regime_ma=args.regime_ma,
                           universe_size=args.universe_size, kosdaq_size=args.kosdaq_size,
                           dry_run=not args.execute)   # allow_real 미노출 = 모의 전용(안전)
        ft.run_once(force=args.force)
    except KisAuthError as e:
        logger.error(f"인증 오류: {e}")


def command_basket(arguments):
    """바스켓 목표 비중표 작성(주문 없음): 가치 기울임(스터디 22·23) + 국면 모멘텀(02·03) 두 슬리브의 목표를
    Quant/config/basket_targets.json에 쓴다. 집행은 엔진 TARGET_BASKET 슬리브 몫이다(D-109)."""
    from pathlib import Path
    from features.fundamental import FundamentalData
    from live.basket_forward import BasketTargetsWriter, MomentumSleeve, ValueTiltSleeve
    data = FundamentalData.load()
    sleeves = []
    shares = {}

    if not arguments.no_value:
        sleeves.append(ValueTiltSleeve(data, top_n=arguments.value_top_n, value_weight=arguments.value_weight,
                                       turnover_floor_krw=arguments.value_floor_billion * 100_000_000))
        shares["VALUE"] = arguments.value_share

    if not arguments.no_momentum:
        # 시세는 VALUE와 같은 네이버 일봉 패널 — data.go.kr 시세는 하루 늦는다.
        sleeves.append(MomentumSleeve(data, top_n=arguments.momentum_top_n, regime_on=not arguments.no_regime))
        shares["MOMENTUM"] = 1.0 - arguments.value_share if not arguments.no_value else 1.0

    if not sleeves:
        logger.error("슬리브가 없다 — --no-value와 --no-momentum을 같이 줄 수 없다")
        return

    writer_kwargs = {}

    if arguments.out:
        writer_kwargs["output_path"] = Path(arguments.out)

    if arguments.state:
        writer_kwargs["state_path"] = Path(arguments.state)

    writer = BasketTargetsWriter(sleeves, shares, data.bars.close.index, data.bars.close.iloc[-1], **writer_kwargs)
    writer.run_once(force=set(arguments.force))


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
    bp.add_argument("--universe-file", dest="universe_file", default=None,
                    help="기본 유니버스 json 경로(미지정 시 config/default_universe.json). "
                         "동적조회/시총상위 미사용 시의 폴백 유니버스를 외부 파일로 지정")
    bp.add_argument("--strategy", default="value_contrary",
                    choices=["value_contrary", "strategy_a", "momentum", "supply_demand", "mean_reversion"],
                    help="백테스트 전략 (기본: value_contrary)")
    bp.add_argument("--source", default="kis", choices=["kis", "krx", "datagokr", "yf"],
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
    bp.add_argument("--vol-target", dest="vol_target", type=float, default=0.0,
                    help="변동성 타게팅 연환산 목표변동성(예 0.20=20%%). 0=비활성. 실현>목표면 노출↓(MDD완화)")
    bp.add_argument("--vol-window", dest="vol_window", type=int, default=20,
                    help="변동성 타게팅 실현변동성 측정 거래일(기본 20)")
    bp.add_argument("--regime", action="store_true",
                    help="시장국면 필터 — 유니버스 200일선 breadth<50%%면 전량 현금(하락장 방어)")
    bp.add_argument("--regime-ma", dest="regime_ma", type=int, default=200,
                    help="국면판정 이동평균 기간(거래일, 기본 200)")
    bp.add_argument("--daily-regime", dest="daily_regime", action="store_true",
                    help="매일 국면체크(전환시 즉시 현금화/재진입). 기본은 리밸런싱일만 체크")
    bp.add_argument("--regime-mode", dest="regime_mode", default="breadth",
                    choices=["breadth", "index"],
                    help="국면 신호: breadth=유니버스 이평위 비율, index=지수 200MA(매끄러워 whipsaw 적음)")
    bp.add_argument("--regime-index", dest="regime_index", default="^KS11",
                    help="index 모드 지수 티커(yfinance): ^KS11 코스피, ^KQ11 코스닥, ^GSPC S&P, ^IXIC 나스닥")
    bp.add_argument("--study",   default=None, help="스터디 ID(예: 모멘텀·국면필터 롤링검증(1~5년)) — metrics.json 라벨")
    bp.add_argument("--event",   default=None, help="이벤트/구간명(예: 2022bear) — metrics.json 라벨")
    bp.add_argument("--honesty", default="unlabeled",
                    choices=["robust", "honest_failure", "overfit_suspect", "unlabeled"],
                    help="정직성 라벨(편향 감사관) — 결과 맥락을 대시보드 카드에 보존")
    bp.add_argument("--oos",     action="store_true", help="out-of-sample 구간 결과로 표기")
    bp.add_argument("--holdout", action="store_true", help="홀드아웃 검증 결과로 표기")
    bp.add_argument("--export", default=None,
                    help="일별 상태 CSV 저장 경로 (매일매일 자산/수익률/낙폭/벤치)")

    # ── live ────────────────────────────────────────────────────────────────
    lp = sub.add_parser("live", help="실전 매매")
    lp.add_argument("--pbr",      type=float,  default=1.0)
    lp.add_argument("--qty",      type=int,    default=1)
    lp.add_argument("--dry-run",  action="store_true", help="주문 없이 시뮬")
    lp.add_argument("--universe", action="store_true")
    lp.add_argument("--universe-file", dest="universe_file", default=None,
                    help="기본 유니버스 json 경로(미지정 시 config/default_universe.json)")

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
    rp2.add_argument("--snapshot", action="store_true", help="현재 잔고를 스냅샷으로 적재 (장 마감 1회 권장)")
    rp2.add_argument("--deposit",  type=float, default=None, help="입금액 기록 (실전)")
    rp2.add_argument("--withdraw", type=float, default=None, help="출금액 기록 (실전)")
    rp2.add_argument("--memo",     default=None, help="입출금 메모")
    rp2.add_argument("--trades",   action="store_true", help="거래내역(체결) 표시")
    rp2.add_argument("--no-report", action="store_true", help="적재만 하고 리포트 출력 생략")

    # ── monitor ─────────────────────────────────────────────────────────────
    mp = sub.add_parser("monitor", help="C++ 엔진 이벤트 실시간 출력")
    mp.add_argument("--host",   default="localhost")
    mp.add_argument("--port",   type=int, default=5555, help="주문 프로세스 발행 포트")
    mp.add_argument("--feed-port", dest="feed_port", type=int, default=0,
                    help="시세 프로세스 발행 포트 (기본: 주문 포트 + 2)")
    mp.add_argument("--strategy-port", dest="strategy_port", type=int, default=0,
                    help="전략 프로세스 발행 포트 (기본: 주문 포트 + 3)")
    mp.add_argument("--topics", nargs="*",
                    choices=["TRADE", "SIGNAL", "ORDER", "HEALTH", "FILL"],
                    help="구독할 토픽 (기본: 전체)")

    # ── record ──────────────────────────────────────────────────────────────
    rp = sub.add_parser("record", help="ZMQ 이벤트 → TimescaleDB 적재")
    rp.add_argument("--host",   default="localhost")
    rp.add_argument("--port",   type=int, default=5555, help="주문 프로세스 발행 포트")
    rp.add_argument("--feed-port", dest="feed_port", type=int, default=0,
                    help="시세 프로세스 발행 포트 (기본: 주문 포트 + 2)")
    rp.add_argument("--strategy-port", dest="strategy_port", type=int, default=0,
                    help="전략 프로세스 발행 포트 (기본: 주문 포트 + 3)")
    rp.add_argument("--record-ticks", action="store_true",
                    help="체결 틱(TRADE)도 ticks 테이블에 넣는다 (기본: 안 넣음)")
    rp.add_argument("--account", default="",
                    help="이 계좌번호의 주문·체결만 넣는다. 테스트·부하 하네스가 같은 ZMQ 포트를 물었을 때 "
                         "남의 합성 데이터가 운영 표에 섞이는 것을 막는다 (기본: 안 거름)")

    # ── procwatch (엔진 프로세스 CPU/메모리 표본 → 그라파나) ─────────────────────
    pw = sub.add_parser("procwatch", help="엔진 프로세스 CPU/메모리 표본 수집 → TimescaleDB(proc_stats)")
    pw.add_argument("--process-name", dest="process_name", default="",
                    help="기본은 Windows quant_trader.exe / 리눅스·WSL quant_trader")
    pw.add_argument("--interval", type=float, default=5.0, help="표본 주기(초)")
    pw.add_argument("--wsl-distro", dest="wsl_distro", default="",
                    help="Windows에서 WSL 안의 엔진을 볼 때 배포판 이름(예: Ubuntu-24.04) — /proc를 읽어 스레드별 CPU까지")
    pw.add_argument("--perf-interval", dest="perf_interval", type=float, default=300.0,
                    help="perf 함수별 핫스팟 표본 주기(초), 0이면 끔 (리눅스·WSL만)")
    pw.add_argument("--perf-seconds", dest="perf_seconds", type=float, default=10.0, help="perf 한 회차 표본 길이(초)")

    # ── operate ─────────────────────────────────────────────────────────────
    op = sub.add_parser("operate", help="C++ 엔진 원격 제어")
    op.add_argument("action", choices=["status", "kill"], help="실행할 명령")
    op.add_argument("--host", default="localhost")
    op.add_argument("--port", type=int, default=5556)

    # ── forward (모의계좌 forward-test) ──────────────────────────────────────
    fp = sub.add_parser("forward", help="regime-모멘텀 모의계좌 forward-test (기본 dry-run)")
    fp.add_argument("--execute", action="store_true", help="실제 모의주문 발행(기본은 dry-run, 주문 안 냄)")
    fp.add_argument("--force", action="store_true", help="리밸런싱 주기 무시하고 강제 실행")
    fp.add_argument("--top-n", dest="top_n", type=int, default=30)
    fp.add_argument("--rebalance-every", dest="rebalance_every", type=int, default=20)
    fp.add_argument("--lookback", type=int, default=120)
    fp.add_argument("--skip", type=int, default=20)
    fp.add_argument("--regime-ma", dest="regime_ma", type=int, default=200)
    fp.add_argument("--no-regime", dest="no_regime", action="store_true")
    fp.add_argument("--vol-adjust", dest="vol_adjust", action="store_true",
                    help="변동성조정 모멘텀(채택 전략) — 점수=수익률/변동성. 백테스트 검증 승자와 일치시킬 것")
    fp.add_argument("--universe-size", dest="universe_size", type=int, default=200)
    fp.add_argument("--kosdaq-size", dest="kosdaq_size", type=int, default=100)

    # ── basket (모의계좌 바스켓 forward — 가치 기울임 + 국면 모멘텀) ────────────
    bk = sub.add_parser("basket", help="가치 기울임+국면 모멘텀 두 슬리브의 목표 비중표 작성(주문 없음, 집행은 엔진 TARGET_BASKET)")
    bk.add_argument("--out", default=None, help="비중표 출력 경로(기본 Quant/config/basket_targets.json)")
    bk.add_argument("--state", default=None, help="리밸 상태 파일 경로(기본 PYQuant/live/.basket_state.json)")
    bk.add_argument("--force", nargs="*", default=[], choices=["VALUE", "MOMENTUM", "all"],
                    help="리밸 주기 무시하고 이 슬리브의 목표를 다시 뽑는다")
    bk.add_argument("--value-share", dest="value_share", type=float, default=0.5, help="가치 슬리브 예산 비율(나머지는 모멘텀)")
    bk.add_argument("--value-top-n", dest="value_top_n", type=int, default=30)
    bk.add_argument("--value-weight", dest="value_weight", type=float, default=0.7)
    bk.add_argument("--value-floor-billion", dest="value_floor_billion", type=float, default=30,
                    help="가치 유니버스 20일 평균 거래대금 하한(억)")
    bk.add_argument("--momentum-top-n", dest="momentum_top_n", type=int, default=30)
    bk.add_argument("--no-regime", dest="no_regime", action="store_true",
                    help="모멘텀 슬리브의 breadth 국면 게이트를 끈다(기본 ON — 스터디 03: OFF는 2022년 -35%, ON은 현금)")
    bk.add_argument("--no-value", dest="no_value", action="store_true")
    bk.add_argument("--no-momentum", dest="no_momentum", action="store_true")

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
    elif args.cmd == "procwatch":
        command_procwatch(args)
    elif args.cmd == "operate":
        cmd_operate(args)
    elif args.cmd == "forward":
        cmd_forward(args)
    elif args.cmd == "basket":
        command_basket(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
