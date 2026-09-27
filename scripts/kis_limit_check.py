# -*- coding: utf-8 -*-
"""KIS 실계좌 키의 두 한도를 직접 재 본다.

  (1) REST 초당 호출 수 — 30종목 묶음 현재가(FHKST11300006)를 초당 N건씩 10초 보내고 거절(EGW00201)을 센다.
  (2) WS 구독 칸 수    — 체결통보 1칸 + 종목 체결을 하나씩 걸어 몇 번째에서 MAX SUBSCRIBE OVER가 나는지 본다.

트레이더(quant_trader.exe)가 떠 있으면 멈춘다. 같은 앱키라 초당 한도를 나눠 쓰고(주문이 거절될 수 있다),
WS는 같은 키로 새 세션을 열면 트레이더 세션이 끊길 수 있다.

사용(저장소 루트에서):
  py scripts/kis_limit_check.py              REST와 WS 둘 다
  py scripts/kis_limit_check.py rest         REST만 (초당 20·22·25·30건)
  py scripts/kis_limit_check.py rest 15 20   REST만, 초당 건수를 직접 준다
  py scripts/kis_limit_check.py ws           WS만
인증정보는 Quant/config/config_live.json 의 kis 에서 읽고 화면에 찍지 않는다.
토큰은 엔진과 같은 캐시 파일(kis_token_<앱키 앞 8자>.json)을 먼저 쓴다 — 토큰 발급은 1분에 1번 제한이 있다.
"""
from __future__ import annotations

import asyncio
import json
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

import requests
import websockets

REPO = Path(__file__).resolve().parent.parent


def main_tree() -> Path:
    """config_live.json·토큰 캐시는 gitignore라 워크트리에 없다 — 없으면 메인 작업 트리에서 읽는다."""
    if (REPO / "Quant/config/config_live.json").exists():
        return REPO

    common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                            cwd=REPO, capture_output=True, text=True).stdout.strip()
    return Path(common).parent if common else REPO


ROOT = main_tree()
BASE_URL = "https://openapi.koreainvestment.com:9443"
WEBSOCKET_URL = "ws://ops.koreainvestment.com:21000"
SECONDS_PER_RATE = 10
DEFAULT_RATES = (20, 22, 25, 30)

kis_config = json.loads((ROOT / "Quant/config/config_live.json").read_text(encoding="utf-8"))["kis"]
APP_KEY = kis_config["app_key"]
APP_SECRET = kis_config["app_secret"]
HTS_ID = kis_config["hts_id"]
TOKEN_CACHE = ROOT / f"kis_token_{APP_KEY[:8]}.json"


def trader_running() -> bool:
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq quant_trader.exe"], capture_output=True, text=True).stdout
    return "quant_trader.exe" in output


def access_token() -> str:
    try:
        cached = json.loads(TOKEN_CACHE.read_text(encoding="utf-8"))
        if int(cached["expires_at"]) - time.time() > 600:
            return cached["access_token"]
    except (OSError, ValueError, KeyError):
        pass

    response = requests.post(BASE_URL + "/oauth2/tokenP", timeout=10,
                             json={"grant_type": "client_credentials", "appkey": APP_KEY, "appsecret": APP_SECRET})
    document = response.json()
    token = document["access_token"]
    expires_at = int(time.time()) + int(document.get("expires_in", 86400))
    TOKEN_CACHE.write_text(json.dumps({"access_token": token, "expires_at": expires_at}), encoding="utf-8")
    return token


def listed_codes(count: int) -> list[str]:
    """네이버 시총 목록에서 개별주 코드를 count개 모은다(시험용 종목 목록일 뿐이다)."""
    codes: list[str] = []

    for market in ("KOSPI", "KOSDAQ"):
        for page in range(1, 20):
            url = f"https://m.stock.naver.com/api/stocks/marketValue/{market}?page={page}&pageSize=100"
            stocks = requests.get(url, timeout=10, headers={"User-Agent": "Mozilla/5.0"}).json().get("stocks", [])
            codes += [stock["itemCode"] for stock in stocks if stock.get("stockEndType") == "stock"]

            if len(codes) >= count:
                return codes[:count]

    return codes


def rest_round(token: str, codes: list[str], calls_per_second: int) -> None:
    headers = {"authorization": "Bearer " + token, "appkey": APP_KEY, "appsecret": APP_SECRET,
               "tr_id": "FHKST11300006", "custtype": "P"}
    batches = [codes[begin:begin + 30] for begin in range(0, len(codes), 30)]
    results: list[tuple] = []
    results_lock = threading.Lock()
    session = requests.Session()   # 엔진처럼 연결을 다시 쓴다 — 매번 새로 붙으면 응답이 수백 ms로 늘어난다

    def call(batch: list[str]) -> None:
        parameters = {}

        for position, code in enumerate(batch, 1):
            parameters[f"FID_COND_MRKT_DIV_CODE_{position}"] = "J"
            parameters[f"FID_INPUT_ISCD_{position}"] = code

        started = time.perf_counter()

        try:
            response = session.get(BASE_URL + "/uapi/domestic-stock/v1/quotations/intstock-multprice",
                                   headers=headers, params=parameters, timeout=10)
            body = response.json()
            outcome = (body.get("rt_cd"), body.get("msg_cd"), body.get("msg1", ""), len(body.get("output") or []))
        except (requests.RequestException, ValueError) as error:
            outcome = ("EXC", type(error).__name__, str(error)[:60], 0)

        with results_lock:
            results.append(outcome + (time.perf_counter() - started,))

    total_calls = calls_per_second * SECONDS_PER_RATE
    interval = 1.0 / calls_per_second
    start = time.perf_counter()

    with ThreadPoolExecutor(max_workers=32) as pool:
        for call_index in range(total_calls):
            delay = start + call_index * interval - time.perf_counter()

            if delay > 0:
                time.sleep(delay)

            pool.submit(call, batches[call_index % len(batches)])

    succeeded = [result for result in results if result[0] == "0"]
    rate_limited = [result for result in results if result[1] == "EGW00201"]
    other_failures = [result[:3] for result in results if result[0] != "0" and result[1] != "EGW00201"]
    rows = sum(result[3] for result in succeeded)
    latencies = sorted(result[4] for result in results)
    print(f"  초당 {calls_per_second:>2}건 x {SECONDS_PER_RATE}초 = {total_calls:>3}건 | 성공 {len(succeeded):>3} | "
          f"거절(EGW00201) {len(rate_limited):>3} | 기타 실패 {len(other_failures)} | "
          f"받은 종목 초당 {rows / SECONDS_PER_RATE:>4.0f} | 응답 중앙값 {latencies[len(latencies) // 2] * 1000:>4.0f}ms")

    for failure in other_failures[:3]:
        print("     기타 실패 예:", failure)


def rest_check(rates: list[int]) -> None:
    print("\n[REST] 30종목 묶음 현재가(FHKST11300006) — 초당 건수를 올려 가며 거절을 센다")
    token = access_token()
    codes = listed_codes(900)

    for calls_per_second in rates:
        rest_round(token, codes, calls_per_second)
        time.sleep(3)   # 앞 회차의 거절이 다음 회차로 번지지 않게 쉰다


async def websocket_round(codes: list[str], with_fill_notice: bool) -> None:
    approval_key = requests.post(BASE_URL + "/oauth2/Approval", timeout=10,
                                 json={"grant_type": "client_credentials", "appkey": APP_KEY,
                                       "secretkey": APP_SECRET}).json()["approval_key"]

    def subscribe_message(transaction_id: str, key: str) -> str:
        return json.dumps({"header": {"approval_key": approval_key, "custtype": "P", "tr_type": "1",
                                      "content-type": "utf-8"},
                           "body": {"input": {"tr_id": transaction_id, "tr_key": key}}})

    requested = ([("H0STCNI0", HTS_ID)] if with_fill_notice else []) + [("H0STCNT0", code) for code in codes]
    first_failure = None
    succeeded = 0

    async with websockets.connect(WEBSOCKET_URL, ping_interval=None) as socket:
        for number, (transaction_id, key) in enumerate(requested, 1):
            await socket.send(subscribe_message(transaction_id, key))
            reply = await websocket_reply(socket)
            label = "체결통보" if transaction_id == "H0STCNI0" else key
            status = reply[2] if reply else "응답 없음"
            print(f"    {number:>2}번째 {transaction_id} {label:<8} → {status}")

            if reply and reply[0] == "0":
                succeeded += 1
            elif first_failure is None:
                first_failure = number

            await asyncio.sleep(0.05)

    title = "체결통보 포함" if with_fill_notice else "체결통보 없이"
    print(f"  => {title}: 성공 {succeeded}칸, 처음 막힌 곳 {first_failure}번째")


async def websocket_reply(socket) -> tuple | None:
    deadline = time.time() + 3

    while time.time() < deadline:
        try:
            raw = await asyncio.wait_for(socket.recv(), timeout=deadline - time.time())
        except asyncio.TimeoutError:
            return None

        if not raw.startswith("{"):
            continue   # 체결 데이터(암호화 안 된 | 구분 문자열) — 구독 응답이 아니다

        document = json.loads(raw)

        if document.get("header", {}).get("tr_id") == "PINGPONG":
            await socket.send(raw)
            continue

        body = document.get("body", {})
        return body.get("rt_cd"), body.get("msg_cd"), body.get("msg1")

    return None


def websocket_check() -> None:
    codes = listed_codes(45)
    print("\n[WS] 한 세션에 구독을 하나씩 걸어 몇 번째에서 막히는지 본다")
    asyncio.run(websocket_round(codes, with_fill_notice=True))
    time.sleep(5)   # 앞 세션이 풀릴 시간
    asyncio.run(websocket_round(codes, with_fill_notice=False))


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    rates = [int(argument) for argument in sys.argv[2:]] or list(DEFAULT_RATES)

    if trader_running():
        print("quant_trader.exe 가 떠 있다 — 같은 앱키라 주문 거절·WS 끊김이 날 수 있어 멈춘다. 장 마감 뒤에 돌린다.")
        return 2

    print("시각", datetime.now().isoformat(timespec="seconds"))

    if mode in ("all", "rest"):
        rest_check(rates)

    if mode in ("all", "ws"):
        websocket_check()

    return 0


if __name__ == "__main__":
    sys.exit(main())
