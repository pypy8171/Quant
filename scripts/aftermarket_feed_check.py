"""애프터마켓(16:00~20:00) 체결이 우리가 이미 구독 중인 WS 채널로 들어오는지 한 번 재 본다.

2026-09-14에 시간외 단일가(16:00~18:00, 10분마다 모아서 한 가격에 체결)가 없어지고 접속매매
애프터마켓이 열렸다(D-097). 접속매매는 정규장과 같은 방식이라 체결도 같은 채널(H0STCNT0·H0UNCNT0)로
올 것으로 보이지만 잰 적이 없다. 공식 샘플이 아직 구 제도를 설명하고 있어 문서로는 못 가린다(D-120).

FEED 모드는 주문을 내지 않으므로, 감시견이 트레이더를 내린 뒤(Until 기본 15:35) 띄워도 엔진이 겹치지
않는다. 시세 WS는 모의 config로 띄워도 실전 도메인을 쓴다(Quant/src/core/AppConfig.cpp의 "시세는 실전 도메인").

  py scripts/aftermarket_feed_check.py --at 16:05 --minutes 10

판정은 이 스크립트가 낸다 — 사람이 화면을 지켜볼 일은 없다.
"""
from __future__ import annotations

import argparse
import datetime as datetime_module
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = REPO_ROOT / "Quant" / "build_win" / "quant_trader.exe"
DEFAULT_CONFIG = REPO_ROOT / "Quant" / "config" / "config.json"
LOG_DIRECTORY = REPO_ROOT / "logs"

# FEED 화면 한 줄: "  [삼성전자 005930]  체결: 71,000원 ▲  (16:03:12)"
#  체결이 아직 없는 종목은 시각이 "--:--:--" 라 이 정규식에 안 걸린다 — 수신한 종목만 세진다.
FEED_LINE = re.compile(
    r"\[(?P<name>[^\[\]]+?) (?P<ticker>\d{6})\]\s+체결:\s*(?P<price>[\d,]+)원.*?\((?P<hhmmss>\d{2}:\d{2}:\d{2})\)"
)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

AFTER_MARKET_OPEN = "16:00:00"

# 관측용 종목. TRADE 모드는 그날 바스켓으로 종목을 받지만 FEED 모드는 config 최상위 "tickers" 만
#  읽으므로(Quant/src/core/AppConfig.cpp) 여기서 넣어 준다. 애프터마켓에 체결이 도는지만 보면 되니
#  거래가 가장 많은 대형주로 고정한다 — 유동성이 얕은 종목은 "안 왔다"가 채널 탓인지 거래 탓인지 못 가른다.
DEFAULT_TICKERS = [
    "005930",  # 삼성전자
    "000660",  # SK하이닉스
    "373220",  # LG에너지솔루션
    "207940",  # 삼성바이오로직스
    "005380",  # 현대차
    "000270",  # 기아
    "035420",  # NAVER
    "035720",  # 카카오
]


def prepare_config(source: Path, work_directory: Path, tickers: list) -> Path:
    """hts_id 를 비운 config 사본을 만든다.

    WS 클라이언트는 연결할 때 hts_id 가 있으면 체결통보(H0STCNI0)를 자동으로 구독한다
    (Quant/src/api/WebSocketClient.cpp). 체결통보는 한 세션만 받아야 한다 — 둘이 받으면
    원장이 체결을 두 번 세고, KIS가 새 세션으로 옛 세션을 밀어내면 트레이더가 체결을 통째로
    놓친다(A등급). 우리는 시세만 보면 되므로 아예 끄고 띄운다.

    사본에는 실계좌 인증 정보가 그대로 담기므로 저장소 밖 임시 폴더에만 두고 끝나면 지운다.
    """
    config = json.loads(source.read_text(encoding="utf-8"))
    kis_node = config.get("kis")

    if isinstance(kis_node, dict):
        kis_node["hts_id"] = ""

    # 체결통보 담당을 따로 지정하는 키가 있으면 그것도 비운다(AppConfig 의 fill_notice 분기).
    for key in ("fill_notice", "fill_notice_session"):
        node = config.get(key)

        if isinstance(node, dict):
            node["hts_id"] = ""

    config["mode"] = "FEED"
    config["tickers"] = tickers
    copy_path = work_directory / "config_feed_only.json"
    copy_path.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
    return copy_path


def wait_until(target: str) -> None:
    """HH:MM 까지 기다린다. 이미 지났으면 바로 돌아온다."""
    hour, minute = (int(part) for part in target.split(":"))
    now = datetime_module.datetime.now()
    deadline = now.replace(hour=hour, minute=minute, second=0, microsecond=0)

    if deadline <= now:
        return

    seconds = (deadline - now).total_seconds()
    print(f"[대기] {target} 까지 {int(seconds // 60)}분 {int(seconds % 60)}초", flush=True)
    time.sleep(seconds)


def run_feed(exe: Path, config: Path, minutes: int, capture: Path) -> None:
    """FEED 모드를 minutes 분 띄우고 화면 출력을 capture 에 받는다."""
    capture.parent.mkdir(parents=True, exist_ok=True)
    print(f"[실행] {exe.name} FEED — {minutes}분, 출력 {capture}", flush=True)

    with capture.open("w", encoding="utf-8", errors="replace", newline="") as sink:
        process = subprocess.Popen(
            [str(exe), str(config), "FEED"],
            cwd=str(REPO_ROOT),          # 반드시 repo 루트에서 띄운다
            stdout=sink,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

        try:
            process.wait(timeout=minutes * 60)
        except subprocess.TimeoutExpired:
            process.terminate()

            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()


def parse_capture(capture: Path) -> dict:
    """받아 둔 화면에서 종목별 마지막 체결 시각을 뽑는다.

    FEED는 커서를 맨 위로 되돌려 덮어쓰므로 파일에는 같은 표가 초마다 쌓인다.
    종목마다 가장 늦은 체결 시각만 남기면 그 종목이 마지막으로 체결된 때가 된다.
    """
    latest_by_ticker: dict[str, dict] = {}

    with capture.open("r", encoding="utf-8", errors="replace") as source:
        for raw_line in source:
            line = ANSI_ESCAPE.sub("", raw_line)
            match = FEED_LINE.search(line)

            if match is None:
                continue

            ticker = match.group("ticker")
            hhmmss = match.group("hhmmss")
            previous = latest_by_ticker.get(ticker)

            if previous is None or hhmmss > previous["hhmmss"]:
                latest_by_ticker[ticker] = {
                    "name": match.group("name").strip(),
                    "hhmmss": hhmmss,
                    "price": match.group("price"),
                }

    return latest_by_ticker


def judge(latest_by_ticker: dict) -> tuple[bool, str]:
    """애프터마켓 체결이 들어왔는지 판정한다."""
    if not latest_by_ticker:
        return (False, "체결이 한 건도 안 왔다 — 구독 자체가 안 붙었는지 로그(logs/)의 WS 연결 줄부터 본다")

    after_market = {ticker: row for ticker, row in latest_by_ticker.items()
                    if row["hhmmss"] >= AFTER_MARKET_OPEN}

    if not after_market:
        newest = max(row["hhmmss"] for row in latest_by_ticker.values())

        # 16시 전에 돌린 회차는 애프터마켓을 잰 것이 아니다 — 구독이 붙는지만 확인된 셈이라 판정을 미룬다.
        if datetime_module.datetime.now().strftime("%H:%M:%S") < AFTER_MARKET_OPEN:
            return (True,
                    f"{len(latest_by_ticker)}종목 구독·마지막 체결 {newest} — 아직 16:00 전이라 애프터마켓은 "
                    "판정 안 함(경로가 살아 있는 것은 확인됐다)")

        return (False,
                f"{len(latest_by_ticker)}종목이 붙었으나 마지막 체결이 {newest} — 16:00 뒤 체결이 없다. "
                "애프터마켓은 이 채널로 안 온다는 뜻이라 전용 채널을 찾아야 한다")

    sample = sorted(after_market.items(), key=lambda pair: pair[1]["hhmmss"], reverse=True)[:3]
    sample_text = ", ".join(f"{row['name']} {row['hhmmss']} {row['price']}원" for _, row in sample)
    return (True,
            f"애프터마켓 체결 {len(after_market)}종목 (구독 {len(latest_by_ticker)}종목 중) — {sample_text}. "
            "지금 구독 중인 H0STCNT0·H0UNCNT0 로 들어온다")


def main() -> int:
    # 윈도우 콘솔 기본값(cp949)은 em dash 같은 글자를 못 찍는다. 판정 문구가 예외로 날아가지 않게 고정한다.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    parser = argparse.ArgumentParser(description="애프터마켓 체결이 기존 WS 채널로 오는지 잰다")
    parser.add_argument("--at", default="", help="이 시각(HH:MM)까지 기다렸다가 띄운다. 비우면 바로")
    parser.add_argument("--minutes", type=int, default=10, help="FEED를 몇 분 띄울지 (기본 10)")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--capture", default="", help="화면을 받을 파일. 비우면 logs/ 아래 날짜 이름")
    parser.add_argument("--tickers", default="", help="관측할 종목 6자리 코드를 쉼표로. 비우면 대형주 기본값")
    arguments = parser.parse_args()

    exe = Path(arguments.exe)
    config = Path(arguments.config)

    if not exe.exists():
        print(f"[오류] exe 없음: {exe}", file=sys.stderr)
        return 2

    if not config.exists():
        print(f"[오류] config 없음: {config}", file=sys.stderr)
        return 2

    today = datetime_module.date.today().isoformat()
    capture = Path(arguments.capture) if arguments.capture else LOG_DIRECTORY / f"aftermarket_feed_{today}.txt"

    if arguments.at:
        wait_until(arguments.at)

    # 인증 정보가 담긴 사본이라 저장소 밖에 두고 반드시 지운다.
    work_directory = Path(tempfile.mkdtemp(prefix="aftermarket_feed_"))

    try:
        tickers = ([code.strip() for code in arguments.tickers.split(",") if code.strip()]
                   or DEFAULT_TICKERS)
        feed_config = prepare_config(config, work_directory, tickers)
        run_feed(exe, feed_config, arguments.minutes, capture)
    finally:
        shutil.rmtree(work_directory, ignore_errors=True)

    latest_by_ticker = parse_capture(capture)
    passed, detail = judge(latest_by_ticker)

    result = {
        "date": today,
        "checked_at": datetime_module.datetime.now().strftime("%H:%M:%S"),
        "minutes": arguments.minutes,
        "tickers_seen": len(latest_by_ticker),
        "after_market_tickers": sum(1 for row in latest_by_ticker.values()
                                    if row["hhmmss"] >= AFTER_MARKET_OPEN),
        "passed": passed,
        "detail": detail,
        "latest_by_ticker": latest_by_ticker,
    }
    result_path = LOG_DIRECTORY / f"aftermarket_feed_{today}.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")

    print()
    print("═" * 78)
    print(f"  애프터마켓 시세 판정 — {'들어온다' if passed else '안 들어온다'}")
    print(f"  {detail}")
    print(f"  결과 {result_path}")
    print("═" * 78)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
