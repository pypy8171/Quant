"""
ZMQ Subscriber — C++ 엔진이 publish하는 데이터를 수신
C++ PUB  tcp://*:5555  →  Python SUB

메시지 형식 (멀티파트):
  frame1: topic  bytes  (b"TRADE" | b"SIGNAL" | b"ORDER" | b"HEALTH")
  frame2: payload bytes (JSON 문자열)

사용 예:
    sub = ZmqSubscriber()
    for topic, data in sub.iter_events():
        print(topic, data)
"""
import json
import time
from typing import Callable, Iterator, Optional

from core.logger import setup_logger

logger = setup_logger("quant.ipc")

try:
    import zmq
    _ZMQ_AVAILABLE = True
except ImportError:
    _ZMQ_AVAILABLE = False


class ZmqSubscriber:
    def __init__(self, host: str = "localhost", pub_port: int = 5555):
        if not _ZMQ_AVAILABLE:
            raise RuntimeError("pyzmq가 설치되지 않았습니다: pip install pyzmq")
        self._ctx  = zmq.Context()
        self._sock = self._ctx.socket(zmq.SUB)
        self._sock.connect(f"tcp://{host}:{pub_port}")
        self._sock.setsockopt(zmq.SUBSCRIBE, b"")  # 모든 토픽 구독
        self._sock.setsockopt(zmq.RCVTIMEO, 1000)  # 1초 타임아웃
        logger.info(f"ZMQ-SUB tcp://{host}:{pub_port} 연결")

    def recv_one(self) -> Optional[tuple[str, dict]]:
        """이벤트 1개 수신. 타임아웃이면 None 반환."""
        try:
            frames = self._sock.recv_multipart()
            if len(frames) < 2:
                return None
            topic   = frames[0].decode("utf-8")
            payload = json.loads(frames[1].decode("utf-8"))
            return topic, payload
        except zmq.Again:
            return None

    def iter_events(self) -> Iterator[tuple[str, dict]]:
        """이벤트 무한 반복. KeyboardInterrupt로 종료."""
        while True:
            result = self.recv_one()
            if result:
                yield result

    def subscribe(self, *topics: str):
        """특정 토픽만 구독 (기본: 전체)."""
        # 기존 전체 구독 취소
        self._sock.setsockopt(zmq.UNSUBSCRIBE, b"")
        for t in topics:
            self._sock.setsockopt(zmq.SUBSCRIBE, t.encode())

    def close(self):
        self._sock.close()
        self._ctx.term()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


# ── 이벤트별 콜백 기반 모니터링 ────────────────────────────────────────────
class EngineMonitor:
    """
    C++ 엔진 이벤트를 수신해 콜백으로 전달하는 고수준 모니터.

    사용 예:
        monitor = EngineMonitor()
        monitor.on_trade  = lambda d: print(f"체결 {d['ticker']} {d['price']}")
        monitor.on_signal = lambda d: print(f"신호 {d['strategy']} {d['side']}")
        monitor.run()
    """
    def __init__(self, host: str = "localhost", pub_port: int = 5555):
        self._sub = ZmqSubscriber(host, pub_port)
        self.on_trade:  Optional[Callable[[dict], None]] = None
        self.on_signal: Optional[Callable[[dict], None]] = None
        self.on_order:  Optional[Callable[[dict], None]] = None
        self.on_health: Optional[Callable[[dict], None]] = None

    def run(self):
        logger.info("Monitor 수신 시작 (Ctrl+C로 종료)")
        try:
            for topic, data in self._sub.iter_events():
                if   topic == "TRADE"  and self.on_trade:  self.on_trade(data)
                elif topic == "SIGNAL" and self.on_signal: self.on_signal(data)
                elif topic == "ORDER"  and self.on_order:  self.on_order(data)
                elif topic == "HEALTH" and self.on_health: self.on_health(data)
        except KeyboardInterrupt:
            logger.info("Monitor 종료")
        finally:
            self._sub.close()
