"""KIS 접속점(도메인·포트) 한 곳. C++ 쪽 정본은 Quant/include/api/KisEndpoints.h — 값이 바뀌면 두 파일을 같이 고친다.

모의투자와 실계좌는 REST 도메인·포트와 WebSocket 포트가 다르고, 고르는 기준은 is_paper 하나다.
실계좌 전환 때 놓치는 리터럴이 없도록 URL·포트는 여기에만 적는다(T-13-3).
"""

REST_BASE_URL_REAL = "https://openapi.koreainvestment.com:9443"
REST_BASE_URL_PAPER = "https://openapivts.koreainvestment.com:29443"
WEBSOCKET_HOST = "ops.koreainvestment.com"
WEBSOCKET_PORT_REAL = 21000
WEBSOCKET_PORT_PAPER = 31000


def rest_base_url(is_paper: bool) -> str:
    """REST 기본 URL(경로 없음). 토큰 발급·주문·잔고·시세 전부 이 앞에 붙는다."""
    return REST_BASE_URL_PAPER if is_paper else REST_BASE_URL_REAL


def websocket_port(is_paper: bool) -> int:
    return WEBSOCKET_PORT_PAPER if is_paper else WEBSOCKET_PORT_REAL
