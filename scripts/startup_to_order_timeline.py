"""엔진 기동부터 한 종목의 첫 매수 주문이 증권사에 접수되기까지를 단계별 시각으로 뽑는다.

실제 장중 로그(Quant/build_win/logs/quant_trader.log)만 읽는다 — 주문을 새로 내지 않는다.
세션은 "=== Quant Trader" 줄에서 다음 같은 줄 전까지다. 종목을 안 주면 그 세션에서 처음 접수된 매수 종목을 고른다.
접수 줄과 같은 주문의 Quant/build_win/logs/latency_trace.csv 줄을 찾아 프로세스 안 구간(µs)도 붙인다.

    py scripts/startup_to_order_timeline.py --list                  # 세션 목록
    py scripts/startup_to_order_timeline.py                         # 마지막으로 매수가 나간 세션, 첫 매수 종목
    py scripts/startup_to_order_timeline.py --session 2026-09-28T10:02 --ticker 033780
"""
import argparse
import csv
import datetime
import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOG = REPOSITORY_ROOT / "Quant" / "build_win" / "logs" / "quant_trader.log"
DEFAULT_TRACE = REPOSITORY_ROOT / "Quant" / "build_win" / "logs" / "latency_trace.csv"

SESSION_MARKER = "=== Quant Trader"
LINE_PATTERN = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) (?:\{([^}]*)\} )?\[(\w+)\s*\] (.*)$")
ORDER_ACCEPTED_PATTERN = re.compile(r"\[KIS\] 주문 접수: (\d{6}) BUY ")
ROUTER_RTT_PATTERN = re.compile(r"RTT=(\d+)ms 버킷대기=(\d+)ms")


def milestone_rules(ticker):
    """(이름, 줄에 들어 있어야 할 조각들) — 세션 안에서 처음 맞는 줄 하나씩을 고른다. 순서는 기동 흐름 순이다."""
    return [
        ("기동", [SESSION_MARKER]),
        ("토큰 확보", ["[KIS] ", "토큰"]),
        ("전략 등록", ["전략 등록:", ticker]),
        ("엔진 시작", ["퀀트 엔진 시작"]),
        ("주문 경로 준비", ["OrderRouter (FEP) 초기화 완료"]),
        ("이전 세션 미체결 조회 끝", ["이전 세션 미체결"]),
        ("장부 저널 다시 적용", ["저널 리플레이"]),
        ("증권사 잔고로 시드", ["부트스트랩 완료"]),
        ("WS 접속키 발급", ["Approval key 발급"]),
        ("WS 연결", ["WebSocket 연결 성공"]),
        ("체결통보 구독 확인", ["H0STCNI", "SUBSCRIBE SUCCESS"]),
        ("시세 구독 확인", [f"H0STCNT0({ticker})", "SUBSCRIBE SUCCESS"]),
        ("종목 첫 체결 도착(전략 첫 반응)", ["{Shard", f"_{ticker}]"]),
        ("일봉 조회 끝", ["일봉 조회", ticker]),
        ("분봉 시드 끝", [f"_{ticker}]", "봉 시드"]),
        ("매수 신호", ["[Strategy] 신호:", f"] {ticker}", " BUY "]),
        ("증권사 접수", ["[KIS] 주문 접수:", f"{ticker} BUY "]),
    ]


def parse_time(text):
    return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")


def read_sessions(log_path):
    """세션마다 (시작 시각, 줄 목록). 줄은 (시각, 스레드, 수준, 본문)."""
    sessions = []
    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        for raw_line in log_file:
            match = LINE_PATTERN.match(raw_line.rstrip("\n"))

            if not match:
                continue

            time_text, thread, level, body = match.groups()

            if SESSION_MARKER in body:
                sessions.append((parse_time(time_text), []))

            if sessions:
                sessions[-1][1].append((parse_time(time_text), thread or "", level, body))

    return sessions


def first_buy_ticker(lines):
    for _, _, _, body in lines:
        match = ORDER_ACCEPTED_PATTERN.search(body)

        if match:
            return match.group(1)

    return None


def pick_session(sessions, wanted):
    if wanted:
        for started_at, lines in sessions:
            if started_at.strftime("%Y-%m-%dT%H:%M").startswith(wanted):
                return started_at, lines

        sys.exit(f"세션을 못 찾음: {wanted} (--list로 목록 확인)")

    for started_at, lines in reversed(sessions):
        if first_buy_ticker(lines):
            return started_at, lines

    sys.exit("매수 접수가 있는 세션이 없음")


def find_milestones(lines, ticker):
    found = []
    search_from = 0

    for name, fragments in milestone_rules(ticker):
        for index in range(search_from, len(lines)):
            when, thread, _, body = lines[index]
            text = "{" + thread + "} " + body

            if all(fragment in text for fragment in fragments):
                found.append((name, when, thread, body))
                # 첫 체결 이후 단계는 그 뒤에서만 찾는다 — 앞 세션 잔여 줄이 끼지 않게.
                if name in ("시세 구독 확인", "종목 첫 체결 도착(전략 첫 반응)", "매수 신호"):
                    search_from = index
                break

    return found


def find_router_line(lines, ticker, accepted_at):
    for when, _, _, body in lines:
        if when >= accepted_at and "[OrderRouter] 접수" in body and f"{ticker} BUY" in body:
            return body

    return None


def find_trace_row(trace_path, ticker, accepted_at):
    """접수 시각 ±2초 안의 같은 종목 NEW BUY 줄. utc_ms는 기록 순간(= 접수 뒤) 시각이다."""
    if not trace_path.exists():
        return None

    best = None
    with open(trace_path, encoding="utf-8", errors="replace", newline="") as trace_file:
        for row in csv.DictReader(trace_file):
            if row.get("ticker") != ticker or row.get("action") != "NEW" or row.get("side") != "BUY":
                continue

            try:
                recorded_at = datetime.datetime.fromtimestamp(int(row["utc_ms"]) / 1000)
            except (KeyError, ValueError):
                continue

            gap = abs((recorded_at - accepted_at).total_seconds())

            if gap <= 2 and (best is None or gap < best[0]):
                best = (gap, row)

    return best[1] if best else None


def print_sessions(sessions):
    for started_at, lines in sessions:
        ticker = first_buy_ticker(lines) or "-"
        print(f"{started_at:%Y-%m-%dT%H:%M:%S}  줄 {len(lines):>7}  첫 매수 {ticker}")


def main():
    # 한글 콘솔(cp949)은 µ를 못 찍는다.
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE)
    parser.add_argument("--session", help="세션 시작 시각 앞부분, 예: 2026-09-28T10:02")
    parser.add_argument("--ticker", help="종목 코드 6자리. 없으면 세션의 첫 매수 접수 종목")
    parser.add_argument("--list", action="store_true", help="세션 목록만 보인다")
    arguments = parser.parse_args()

    sessions = read_sessions(arguments.log)

    if arguments.list:
        print_sessions(sessions)
        return

    started_at, lines = pick_session(sessions, arguments.session)
    ticker = arguments.ticker or first_buy_ticker(lines)

    if not ticker:
        sys.exit(f"{started_at} 세션에 매수 접수가 없음 — --ticker를 주거나 다른 세션을 고른다")

    milestones = find_milestones(lines, ticker)
    print(f"세션 {started_at:%Y-%m-%d %H:%M:%S.%f}"[:-3] + f" / 종목 {ticker} / 로그 {arguments.log.relative_to(REPOSITORY_ROOT)}")
    print(f"{'단계':<22} {'시각':<12} {'기동부터(ms)':>12} {'앞 단계부터(ms)':>15}  스레드")

    previous = started_at
    for name, when, thread, _ in milestones:
        since_start = (when - started_at).total_seconds() * 1000
        since_previous = (when - previous).total_seconds() * 1000
        print(f"{name:<22} {when:%H:%M:%S.%f}"[:-3] + f" {since_start:>12,.0f} {since_previous:>15,.0f}  {thread}")
        previous = when

    names = {name for name, *_ in milestones}
    missing = [name for name, _ in milestone_rules(ticker) if name not in names]

    if missing:
        print("못 찾은 단계: " + ", ".join(missing))

    accepted = next((when for name, when, *_ in milestones if name == "증권사 접수"), None)

    if accepted is None:
        return

    router_line = find_router_line(lines, ticker, accepted)
    rtt = ROUTER_RTT_PATTERN.search(router_line or "")

    if rtt:
        print(f"\n증권사 왕복(RTT) {rtt.group(1)}ms, 초당 한도 버킷 대기 {rtt.group(2)}ms")

    trace_row = find_trace_row(arguments.trace, ticker, accepted)

    if trace_row:
        print("latency_trace.csv 같은 주문 (µs): "
              f"체결 수신→신호 {trace_row['tick_to_signal_us']} / 신호→주문 스레드 {trace_row['signal_to_pop_us']} / "
              f"주문 스레드→접수 응답 {trace_row['pop_to_done_us']} (seq {trace_row['seq']})")


if __name__ == "__main__":
    main()
