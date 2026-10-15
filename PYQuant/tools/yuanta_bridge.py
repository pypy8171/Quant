"""유안타 티레이더 오픈API 연결 프로그램 — 실시간 등록 한도 실측(measure-limit)과 엔진으로 시세 내보내기(serve).

유안타 DLL(YuantaCOM.dll)은 32비트라 64비트 엔진(quant_trader) 안에서 부를 수 없다. 그래서 이 파일은 32비트
파이썬으로 따로 돈다. 저장소의 PYQuant 가상환경(64비트)으로는 안 돈다.

    C:\\tools\\py313-x86\\python.exe PYQuant\\tools\\yuanta_bridge.py measure-limit --user <유안타 ID>

  - 비밀번호·인증서 비밀번호는 실행할 때 화면에서 받는다(파일·인자에 남기지 않는다).
  - 기본 접속은 국내 모의투자 서버다. --server real 은 운영 서버.
  - COM 등록(관리자 cmd 에서 SysWOW64\\regsvr32.exe YuantaCOM.dll)이 먼저 되어 있어야 한다.

    C:\\tools\\py313-x86\\python.exe PYQuant\\tools\\yuanta_bridge.py serve --user <유안타 ID>

serve 는 국내 주식 전 종목 현재가 실시간(AUTO11)을 구독해 체결마다 48바이트 전문으로 묶어 ZMQ PUB
(기본 tcp://127.0.0.1:5700)로 내보낸다. 엔진(config "yuanta_feed": {"enabled": true})의 yuanta::YuantaFeed 가
받아 구독 목록에 있는 종목만 올린다. 전문 모양의 정본은 Quant/include/api/YuantaFeed.h 다. Ctrl+C 로 멈춘다.

measure-limit 은 국내 주식 현재가 실시간(AUTO11)을 종목 수를 늘려 가며 등록하고, 등록이 처음 거부되는 개수와
오류 문구를 적은 뒤 --listen-seconds 동안 실제로 시세가 들어온 종목 수와 초당 건수를 센다. 등록이 받아들여져도
서버가 일부만 보내 줄 수 있어서, 장중에 돌려 "받은 종목 수"까지 봐야 한도를 안다. 결과는 _private/yuanta/ 에 json.
"""

from __future__ import annotations

import argparse
import collections
import faulthandler
import getpass
import json
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

if struct.calcsize("P") != 4:
    sys.exit("32비트 파이썬으로 실행해야 한다 — C:\\tools\\py313-x86\\python.exe")

import pythoncom
import win32com.client

_REPO_ROOT = Path(__file__).resolve().parents[2]
_OUTPUT_DIR = _REPO_ROOT / "_private" / "yuanta"
_DEFAULT_API_DIRECTORY = Path.home() / "Desktop" / "이직" / "유안타증권" / "YuantaAPI" / "YuantaAPI"
_SERVERS = {
    "simul": "simul.tradar.api.com",
    "real": "real.tradar.api.com",
}

# 도움말(help/init/YOA_Initial.html, help/etc/YOA_GetLastError.html) 값. 확인 2026-10-15.
_RESULT_SUCCESS = 1000
_ERROR_MAX_CODE = 1000          # 요청·등록 함수는 이보다 크면 요청 번호, 작거나 같으면 오류코드를 돌려준다
_MARKET_DOMESTIC_STOCK = 3
_CODE_INFO_CODE = 0
_AUTO_STOCK_PRICE = "11"        # qry/auto/AUTO11.qry — 국내 주식 현재가 실시간

# AUTO11 OutBlock1 필드 이름(qry/auto/AUTO11.qry, 확인 2026-10-15). --dump 가 이 순서로 찍는다.
_AUTO11_FIELDS = (
    "jongcode", "curjuka", "chejuka", "nowvol", "volume", "time", "time2", "chegyulrate",
    "medohoka", "mesuhoka", "jang",
)

# 엔진 쪽 전문(Quant/include/api/YuantaFeed.h)과 한 바이트도 달라서는 안 된다.
_WIRE_MAGIC = 0x44525459                   # 'YTRD'
_WIRE_VERSION = 1
_WIRE_HEADER = struct.Struct("<IHHQ")      # magic, version, record_count, sent_unix_ns — 16바이트
_WIRE_RECORD = struct.Struct("<8siiqqqd")  # code, hhmmss, direction, price_krw, quantity, accumulated_volume, strength — 48바이트
_WIRE_MAX_RECORDS = 4000                   # 한 통에 담는 최대 건수(record_count 가 uint16)
_DEFAULT_ENDPOINT = "tcp://127.0.0.1:5700"
_DIRECTION_BUY = 1
_DIRECTION_SELL = 5

assert _WIRE_HEADER.size == 16 and _WIRE_RECORD.size == 48


class _Events:
    """COM 이벤트 받는 쪽. DispatchWithEvents 가 이 클래스를 COM 객체에 섞으므로 self 로 API 함수를 부른다."""

    def __init__(self) -> None:
        self.login_result: int | None = None
        self.login_message = ""
        self.errors: list[dict] = []
        self.real_count_by_code: collections.Counter[str] = collections.Counter()
        self.real_total = 0
        self.first_real_at: float | None = None
        self.dump_remaining = 0                           # 원문 필드를 찍을 남은 건수(--dump)
        self.pending_records: list[bytes] | None = None   # serve 일 때만 리스트 — 보낼 체결 전문을 모아 둔다
        self.bad_records = 0

    def OnLogin(self, nResult, bstrMsg):  # noqa: N802 — COM 이벤트 이름 그대로
        self.login_result = int(nResult)
        self.login_message = str(bstrMsg)
        print(f"[로그인 응답] {nResult} {bstrMsg}")

    def OnReceiveError(self, nReqID, nErrCode, bstrErrMsg):  # noqa: N802
        self.errors.append({"request_id": int(nReqID), "code": int(nErrCode), "message": str(bstrErrMsg),
                            "at": datetime.now().isoformat(timespec="seconds")})
        print(f"[서버 오류] 요청 {nReqID} 코드 {nErrCode} {bstrErrMsg}")

    def OnReceiveData(self, nReqID, bstrDSOID):  # noqa: N802
        pass

    def OnReceiveRealData(self, nReqID, bstrAutoID):  # noqa: N802
        try:
            code = self.YOA_GetTRFieldString(bstrAutoID, "OutBlock1", "jongcode", 0)
        except Exception as error:  # COM 쪽 예외가 이벤트 펌프 밖으로 새면 원인을 못 본다
            print(f"[실시간 처리 오류] 요청 {nReqID} {bstrAutoID}: {error!r}", flush=True)
            return

        code = str(code).strip()
        self.real_count_by_code[code] += 1
        self.real_total += 1

        if self.first_real_at is None:
            self.first_real_at = time.monotonic()

        if self.dump_remaining > 0:
            self.dump_remaining -= 1
            fields = {name: str(self._field(bstrAutoID, name)) for name in _AUTO11_FIELDS}
            print(f"[원문] {fields}", flush=True)

        if self.pending_records is not None:
            try:
                self.pending_records.append(self._encode_trade(bstrAutoID, code))
            except (ValueError, struct.error, UnicodeEncodeError) as error:
                self.bad_records += 1

                if self.bad_records <= 5:
                    print(f"[전문 변환 실패] {code}: {error!r}", flush=True)

    def _field(self, auto_id: str, name: str) -> str:
        return str(self.YOA_GetTRFieldString(auto_id, "OutBlock1", name, 0)).strip()

    def _field_integer(self, auto_id: str, name: str) -> int:
        text = self._field(auto_id, name).replace(",", "")
        return int(float(text)) if text else 0

    def _encode_trade(self, auto_id: str, code: str) -> bytes:
        price = abs(self._field_integer(auto_id, "chejuka")) or abs(self._field_integer(auto_id, "curjuka"))
        ask = abs(self._field_integer(auto_id, "medohoka"))
        bid = abs(self._field_integer(auto_id, "mesuhoka"))

        # AUTO11 에는 매수·매도 체결 구분 칸이 따로 없다. 체결가가 매도호가 이상이면 매수 체결, 매수호가 이하면
        #  매도 체결로 본다. nowvol 부호가 구분을 싣는지는 10-16 장중 원문(--dump)으로 확인한다.
        direction = 0

        if ask > 0 and price >= ask:
            direction = _DIRECTION_BUY
        elif bid > 0 and price <= bid:
            direction = _DIRECTION_SELL

        strength_text = self._field(auto_id, "chegyulrate")
        return _WIRE_RECORD.pack(
            code.encode("ascii")[:8],
            _hhmmss(self._field(auto_id, "time2"), self._field(auto_id, "time")),
            direction,
            price,
            abs(self._field_integer(auto_id, "nowvol")),
            abs(self._field_integer(auto_id, "volume")),
            float(strength_text) if strength_text else 0.0,
        )

    def OnReceiveSystemMessage(self, nID, bstrMsg):  # noqa: N802
        print(f"[시스템 메시지] {nID} {bstrMsg}")


def _hhmmss(time2: str, time_hhmm: str) -> int:
    """time2(8칸)에서 숫자 여섯 개를 HHMMSS 로 읽는다. 모자라면 time(HH:MM)에 00초를 붙이고, 그것도 없으면 0(모름)."""
    digits = "".join(character for character in time2 if character.isdigit())

    if len(digits) >= 6:
        return int(digits[:6])

    digits = "".join(character for character in time_hhmm if character.isdigit())
    return int(digits[:4]) * 100 if len(digits) >= 4 else 0


def _pump(seconds: float) -> None:
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        pythoncom.PumpWaitingMessages()
        time.sleep(0.001)


def _connect(api_directory: Path, server: str, user: str):
    api = win32com.client.DispatchWithEvents("YuantaAPICOM.YuantaAPI", _Events)
    result = api.YOA_Initial(_SERVERS[server], str(api_directory) + "\\")

    if result != _RESULT_SUCCESS:
        sys.exit(f"초기화 실패: {result} {api.YOA_GetErrorMessage(result)}")

    password = getpass.getpass("유안타 비밀번호: ")
    certificate_password = getpass.getpass("인증서 비밀번호: ")
    result = api.YOA_Login(user, password, certificate_password)

    if result != _RESULT_SUCCESS:
        sys.exit(f"로그인 요청 실패: {result} {api.YOA_GetErrorMessage(result)}")

    # 로그인 뒤 종목 목록·명세 파일을 내려받느라 시간이 걸린다(시험 프로그램에서 2초쯤).
    deadline = time.monotonic() + 60

    while api.login_result is None and time.monotonic() < deadline:
        _pump(0.1)

    if api.login_result is None:
        sys.exit("60초 안에 로그인 응답이 없다")

    _pump(3)
    return api


def _domestic_codes(api) -> list[str]:
    count = api.YOA_GetCodeCount(_MARKET_DOMESTIC_STOCK)
    codes = []

    for index in range(count):
        code = str(api.YOA_GetCodeInfoByIndex(_MARKET_DOMESTIC_STOCK, _CODE_INFO_CODE, index)).strip()

        if len(code) == 6:
            codes.append(code)

    return codes


def serve(arguments: argparse.Namespace) -> None:
    import zmq

    context = zmq.Context()
    socket = context.socket(zmq.PUB)
    socket.setsockopt(zmq.SNDHWM, 100000)
    socket.bind(arguments.endpoint)
    print(f"ZMQ PUB {arguments.endpoint}", flush=True)

    api = _connect(arguments.api_directory, arguments.server, arguments.user)
    api.dump_remaining = arguments.dump
    api.pending_records = []
    codes = _domestic_codes(api)
    registered = 0

    for code in codes:
        api.YOA_SetTRFieldString(_AUTO_STOCK_PRICE, "InBlock1", "jongcode", code, 0)

        if api.YOA_RegistAuto(_AUTO_STOCK_PRICE) > _ERROR_MAX_CODE:
            registered += 1

    print(f"구독: 국내 주식 {len(codes)}종목 중 {registered}종목", flush=True)

    sent_messages = 0
    sent_records = 0
    last_report = time.monotonic()

    try:
        while True:
            pythoncom.PumpWaitingMessages()

            # 펌프 한 번에 쌓인 체결을 한 통(최대 _WIRE_MAX_RECORDS 건)씩 묶어 보낸다.
            while api.pending_records:
                batch = api.pending_records[:_WIRE_MAX_RECORDS]
                del api.pending_records[:_WIRE_MAX_RECORDS]
                header = _WIRE_HEADER.pack(_WIRE_MAGIC, _WIRE_VERSION, len(batch), time.time_ns())
                socket.send(header + b"".join(batch))
                sent_messages += 1
                sent_records += len(batch)

            if time.monotonic() - last_report >= arguments.report_seconds:
                print(f"  {datetime.now():%H:%M:%S} 받은 체결 {api.real_total}건 / 종목 {len(api.real_count_by_code)}개 / "
                      f"보낸 통 {sent_messages} / 보낸 건 {sent_records} / 변환 실패 {api.bad_records} / "
                      f"서버 오류 {len(api.errors)}", flush=True)
                last_report = time.monotonic()

            time.sleep(0.001)
    except KeyboardInterrupt:
        print("멈춤 요청", flush=True)

    api.YOA_UnRegistAllAuto()
    _pump(1)
    # YOA_UnInitial 은 부르지 않는다 — measure_limit 의 주석과 같은 까닭.
    socket.close(linger=0)
    context.term()
    print("정리 끝", flush=True)


def measure_limit(arguments: argparse.Namespace) -> None:
    api = _connect(arguments.api_directory, arguments.server, arguments.user)
    api.dump_remaining = arguments.dump
    all_codes = _domestic_codes(api)
    print(f"국내 주식 종목 수(YOA_GetCodeCount): {len(all_codes)}")
    codes = all_codes[: arguments.max_count]

    registered: list[str] = []
    rejected: list[dict] = []
    consecutive_rejections = 0
    started = time.monotonic()

    for index, code in enumerate(codes):
        api.YOA_SetTRFieldString(_AUTO_STOCK_PRICE, "InBlock1", "jongcode", code, 0)
        result = api.YOA_RegistAuto(_AUTO_STOCK_PRICE)

        if result > _ERROR_MAX_CODE:
            registered.append(code)
            consecutive_rejections = 0
        else:
            consecutive_rejections += 1
            rejected.append({"order": index + 1, "code": code, "result": int(result),
                             "message": str(api.YOA_GetErrorMessage(result))})

            if len(rejected) == 1:
                print(f"[첫 거부] {index + 1}번째 {code}: {result} {rejected[0]['message']}")

            # 한도에 걸렸으면 뒤도 다 거부된다. 연속 20건 거부면 그만 센다.
            if consecutive_rejections >= 20:
                break

        if (index + 1) % 100 == 0:
            _pump(0.05)
            print(f"  등록 시도 {index + 1} / 성공 {len(registered)} / 거부 {len(rejected)} / 서버 오류 {len(api.errors)}")

    register_seconds = time.monotonic() - started
    print(f"등록 끝: 성공 {len(registered)}, 거부 {len(rejected)}, {register_seconds:.1f}초")
    print(f"{arguments.listen_seconds}초 동안 시세를 받는다")
    listen_deadline = time.monotonic() + arguments.listen_seconds

    while time.monotonic() < listen_deadline:
        _pump(min(10, listen_deadline - time.monotonic()))
        print(f"  수신 {api.real_total}건 / 종목 {len(api.real_count_by_code)}개", flush=True)

    received_codes = len(api.real_count_by_code)
    listened = time.monotonic() - (api.first_real_at or time.monotonic())
    summary = {
        "measured_at": datetime.now().isoformat(timespec="seconds"),
        "server": arguments.server,
        "domestic_code_count": len(all_codes),
        "attempted": len(registered) + len(rejected),
        "registered": len(registered),
        "rejected": len(rejected),
        "first_rejection": rejected[0] if rejected else None,
        "server_errors": api.errors[:50],
        "register_seconds": round(register_seconds, 2),
        "listen_seconds": arguments.listen_seconds,
        "received_code_count": received_codes,
        "received_total": api.real_total,
        "received_per_second": round(api.real_total / listened, 1) if listened > 0 else 0,
        "registered_but_silent": len(set(registered) - set(api.real_count_by_code)),
        "top_codes": api.real_count_by_code.most_common(20),
    }
    # 결과부터 남기고 정리한다 — 10-15 첫 실행은 수신 중이나 정리 중에 Traceback 없이 죽어 파일이 안 남았다.
    output_path = _OUTPUT_DIR / f"limit_{datetime.now():%Y%m%d_%H%M}.json"
    output_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items() if key not in ("server_errors", "top_codes")},
                     ensure_ascii=False, indent=2))
    print(f"저장: {output_path}", flush=True)

    print("등록 해제", flush=True)
    api.YOA_UnRegistAllAuto()
    _pump(1)
    # YOA_UnInitial 은 부르지 않는다 — 10-15 실측에서 DLL 안 access violation 으로 죽었다(_private/yuanta/crash_20261015_163120.log).
    # 프로세스가 끝나면 접속도 닫힌다.
    print("정리 끝", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="유안타 오픈API 연결 프로그램")
    commands = parser.add_subparsers(dest="command", required=True)

    limit = commands.add_parser("measure-limit", help="실시간 등록 한도 실측")
    limit.add_argument("--user", required=True, help="유안타 계정 ID")
    limit.add_argument("--server", choices=sorted(_SERVERS), default="simul")
    limit.add_argument("--api-dir", dest="api_directory", type=Path, default=_DEFAULT_API_DIRECTORY)
    limit.add_argument("--max-count", type=int, default=5000, help="등록을 시도할 최대 종목 수")
    limit.add_argument("--listen-seconds", type=int, default=120, help="등록 뒤 시세를 받는 시간")
    limit.add_argument("--dump", type=int, default=3, help="원문 필드를 화면에 찍을 체결 건수")

    serve_parser = commands.add_parser("serve", help="전 종목 체결을 ZMQ PUB 로 엔진에 보낸다")
    serve_parser.add_argument("--user", required=True, help="유안타 계정 ID")
    serve_parser.add_argument("--server", choices=sorted(_SERVERS), default="simul")
    serve_parser.add_argument("--api-dir", dest="api_directory", type=Path, default=_DEFAULT_API_DIRECTORY)
    serve_parser.add_argument("--endpoint", default=_DEFAULT_ENDPOINT, help="ZMQ PUB 주소. 같은 기계 안에서만 연다")
    serve_parser.add_argument("--dump", type=int, default=3, help="원문 필드를 화면에 찍을 체결 건수")
    serve_parser.add_argument("--report-seconds", type=int, default=10, help="진행을 찍는 간격(초)")

    arguments = parser.parse_args()

    # DLL 안에서 죽으면 파이썬 Traceback 이 안 나온다. faulthandler 가 그때 파이썬 쪽 위치를 파일에 남긴다.
    _OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    crash_log = open(_OUTPUT_DIR / f"crash_{datetime.now():%Y%m%d_%H%M%S}.log", "w", encoding="utf-8")
    faulthandler.enable(file=crash_log, all_threads=True)

    if arguments.command == "measure-limit":
        measure_limit(arguments)
    elif arguments.command == "serve":
        serve(arguments)


if __name__ == "__main__":
    main()
