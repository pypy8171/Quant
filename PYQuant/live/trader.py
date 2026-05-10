"""
실전 매매 실행기
REST 폴링 방식 (60초 간격)
"""
import time
import signal
from datetime import datetime
from kis.client import KisClient
from strategy.base import StrategyBase


class LiveTrader:
    def __init__(self, kis: KisClient, strategy: StrategyBase,
                 poll_sec: int = 60, dry_run: bool = False):
        self.kis      = kis
        self.strategy = strategy
        self.poll_sec = poll_sec
        self.dry_run  = dry_run   # True면 주문 안 냄 (시뮬만)
        self._running = True
        signal.signal(signal.SIGINT,  self._stop)
        signal.signal(signal.SIGTERM, self._stop)

    def _stop(self, *_):
        print("\n[LiveTrader] 종료 신호 수신")
        self._running = False

    def _is_market_open(self) -> bool:
        now = datetime.now()
        if now.weekday() >= 5:   # 토/일
            return False
        hhmm = now.hour * 100 + now.minute
        return 900 <= hhmm < 1530

    def run(self, universe: list[str]):
        print(f"\n{'='*60}")
        print(f"실전 매매 시작: {self.strategy.id()}")
        print(f"폴링 간격: {self.poll_sec}초  |  {'DRY RUN (주문 안 냄)' if self.dry_run else '실거래 모드'}")
        print('='*60)

        self.strategy.set_kis(self.kis)
        watch = self.strategy.on_start(universe)
        print(f"구독 종목: {len(watch)}개\n")

        while self._running:
            if not self._is_market_open():
                now = datetime.now()
                print(f"\r[{now.strftime('%H:%M:%S')}] 장 외 시간 대기 중...", end="")
                time.sleep(30)
                continue

            now = datetime.now()
            print(f"[{now.strftime('%H:%M:%S')}] 폴링 시작 ({len(watch)}종목)")

            for ticker in watch:
                if not self._running:
                    break
                bars = self.kis.get_daily_ohlcv(ticker, 10)
                time.sleep(0.3)   # rate limit

                signal_obj = self.strategy.on_data(ticker, bars)
                if signal_obj is None:
                    continue

                print(f"  신호: {ticker} {signal_obj.side} {signal_obj.quantity}주")
                if not self.dry_run:
                    self.kis.send_order(signal_obj)
                else:
                    print(f"  [DRY RUN] 주문 생략")

            time.sleep(self.poll_sec)

        self.strategy.on_stop()
        print("[LiveTrader] 종료")
