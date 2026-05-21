"""
ZMQ Operator — C++ 엔진에 명령 전송
Python REQ  →  C++ REP  tcp://*:5556

지원 명령:
  STATUS          → 엔진 상태 JSON 반환
  KILL            → 엔진 즉시 종료
  PAUSE <id>      → 전략 일시 정지 (미구현 시 "UNKNOWN" 반환)
  RESUME <id>     → 전략 재개

사용 예:
    op = ZmqOperator()
    print(op.status())
    op.kill()
"""
import json
from typing import Optional

from core.logger import setup_logger

logger = setup_logger("quant.ipc")

try:
    import zmq
    _ZMQ_AVAILABLE = True
except ImportError:
    _ZMQ_AVAILABLE = False


class ZmqOperator:
    def __init__(self, host: str = "localhost", rep_port: int = 5556,
                 timeout_ms: int = 5000):
        if not _ZMQ_AVAILABLE:
            raise RuntimeError("pyzmq가 설치되지 않았습니다: pip install pyzmq")
        self._ctx  = zmq.Context()
        self._timeout_ms = timeout_ms
        self._endpoint = f"tcp://{host}:{rep_port}"
        self._sock = self._ctx.socket(zmq.REQ)
        self._sock.setsockopt(zmq.RCVTIMEO, timeout_ms)
        self._sock.setsockopt(zmq.SNDTIMEO, timeout_ms)
        self._sock.connect(self._endpoint)
        logger.info(f"ZMQ-REQ {self._endpoint} 연결")

    def _send(self, cmd: str) -> str:
        try:
            self._sock.send_string(cmd)
            return self._sock.recv_string()
        except zmq.Again:
            # TIMEOUT 후 REQ 소켓은 EFSM 오염 → 닫고 재생성해야 이후 명령이 가능
            self._sock.close()
            self._sock = self._ctx.socket(zmq.REQ)
            self._sock.setsockopt(zmq.RCVTIMEO, self._timeout_ms)
            self._sock.setsockopt(zmq.SNDTIMEO, self._timeout_ms)
            self._sock.connect(self._endpoint)
            return "TIMEOUT"

    def status(self) -> Optional[dict]:
        """엔진 상태 조회. 딕셔너리 반환, 실패 시 None."""
        raw = self._send("STATUS")
        try:
            return json.loads(raw)
        except (json.JSONDecodeError, ValueError) as e:
            logger.warning(f"status 응답 파싱 실패: {e}")
            return None

    def kill(self) -> bool:
        """엔진 종료. 성공 시 True."""
        resp = self._send("KILL")
        return resp == "OK"

    def pause(self, strategy_id: str) -> bool:
        resp = self._send(f"PAUSE {strategy_id}")
        return resp == "OK"

    def resume(self, strategy_id: str) -> bool:
        resp = self._send(f"RESUME {strategy_id}")
        return resp == "OK"

    def close(self):
        self._sock.close()
        self._ctx.term()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
