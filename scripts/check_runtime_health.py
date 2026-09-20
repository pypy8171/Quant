# -*- coding: utf-8 -*-
"""장중·마감 후 실행 로그에서 '이미 한 번 당한' 실패 유형이 다시 났는지 기계적으로 본다.

사람이 로그를 눈으로 훑어 찾아낸 것만 여기 들어온다. 새 항목을 넣을 때는 그 항목이
실제로 하루를 망친 적이 있어야 한다 — 가정으로 만든 체크는 경보만 늘리고 신뢰를 깎는다.

사용:  py scripts/check_runtime_health.py [--date YYYY-MM-DD] [--log <경로>]
종료코드: 0 = FAIL 없음, 1 = FAIL 있음
"""
from __future__ import annotations

import argparse
import datetime as dt
import gzip
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402
from log_patterns import GUARD_ATTACH_RE as GUARD_RE  # noqa: E402

DEFAULT_LOG = _logdir.log_dir() / "quant_trader.log"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})")
START_RE = re.compile(r"퀀트 엔진 시작")
STALE_RE = re.compile(r"이전 세션 미체결 (\d+)건 발견")
BREAKEVEN_RE = re.compile(r"본전탈출\)")
FILL_RE = re.compile(r"체결통보 ODNO=\d+ (\d{6}) (BUY|SELL) (\d+)주")
RATE_RE = re.compile(r"EGW00201|초당 거래건수")
WSFALL_RE = re.compile(r"WS → REST 폴링 폴백")
# 주문 접수·거부 한 줄의 왕복 시간. 버킷대기는 09-19 이후 바이너리만 찍는다(없으면 None).
RTT_RE = re.compile(r"\[OrderRouter\] (?:접수|KIS 거부) .*?RTT=(\d+)ms(?: 버킷대기=(\d+)ms)?")
# D-100 — 잔고 조회가 한 사이클(500ms)을 넘겨 뒤 사이클에서 적용된 건
RECON_SLOW_RE = re.compile(r"잔고 대조: 조회 소요 (\d+)ms \(사이클 (\d+)회 걸침\)")
# 서버가 15초 안에 답을 안 준 요청 — WinHTTP 12002·curl 28. 한 요청에 한 줄(재시도 래퍼의 안내 줄은 세지 않는다)
HTTP_TIMEOUT_RE = re.compile(r"ReceiveResponse 실패: 12002|\[CURL\] 요청 실패: Timeout was reached")
# D-101 결정 2 — TRENDX max_universe 0이면 초기 등록이 0종목이어야 한다(스코어 경로 상한은 09-19 수정)
TRENDX_REGISTER_RE = re.compile(r"TRENDX universe_from_scan: 초기 (\d+)종목 등록")
# D-101 결정 3 — 마감 청산이 매매 창 안(모의 15:15·실계좌 19:50, 접속매매)에 나가면 이 거부는 0건이다(09-18 2,188건이 25종목 이월을 만들었다)
SESSION_WINDOW_REJECT_RE = re.compile(r"\[OrderRouter\] 거부 .*세션 창 밖")
SIGNAL_RE = re.compile(r"\[Strategy\] 신호: \[([A-Z_+0-9]+)\] (\d{6})(?:\([^)]*\))? (BUY|SELL) (\d+)")
ITB_ATTACH_RE = re.compile(r"\[Main\]   \+ ITB (\d{6}) ")
# 09-20 청산선 재설정(config_dev_paper.json "//exit_09-20") — 12일 원장에서 손실을 낸 네 경로가 닫혔는지 본다:
#  교체 매도(728체결 -89만)는 0건, 승계 직후 매도(부착 90초 안 47건 -43만, 유예 분기 버그)는 0건,
#  손절 뒤 같은 날 같은 종목 재매수(쿨다운 21600초)는 0건, 손절이 매수의 25%를 넘으면 -2.0%가 잡음에 걸리는 것(예측 21%).
DEVSCALE_STOP_RE = re.compile(r"신호: \[DEVSCALE_(\d{6})\] \d{6}.* SELL \d+ \| 근거: 청산:손절\(평단")
MAX_STOP_PER_BUY = 0.25
# D-111 넘김·진입 필터(09-21 리플레이): 장 마감 청산을 끄고(market_close_exit_hhmm 2400) 전일 ATR·개장 이격으로 그날 진입을
#  거른다. 두 행은 새 바이너리 표식(진입 필터 판정 줄)이 있는 날만 판정한다 — 배포 전 로그에서는 건너뛴다.
DEVSCALE_CLOSE_EXIT_RE = re.compile(r"신호: \[DEVSCALE_(\d{6})\] \d{6}.* SELL \d+ \| 근거: 청산:장 마감\(")
DEVSCALE_ENTRY_FILTER_RE = re.compile(r"\[DEVSCALE_(\d{6})\] 진입 필터 (통과|차단)\(")

# 임계값. 넘으면 그날 운영이 실제로 상했던 수준이다.
MAX_STALE_ORDERS = 25     # 유령주문 재부활 — 취소 왕복이 초당한도를 밀어낸다
MIN_SESSION_SEC = 30      # 이보다 짧게 죽으면 배선/바이너리 문제(정상 재기동 아님)
GUARD_QUIET_SEC = 90      # 청산 관리 부착 직후 이 시간 안의 본전탈출은 재기동 투매다
CHURN_SEC = 120           # 같은 종목 매도→매수가 이 안에 오면 회전
MAX_CHURN = 3
MAX_RATE_HITS = 50
SLOW_ORDER_MS = 3000      # 접수까지 이보다 오래 걸리면 청산 지정가가 시세를 놓친다(T-24)
MAX_SLOW_ORDER_RATIO = 0.2  # 접수 중 이 비율 넘게 느리면 그날 서버(또는 버킷)가 상한 것
BUCKET_WAIT_MS = 300      # 버킷대기 중앙값이 이 위면 지연의 주범은 서버가 아니라 초당한도 버킷
MAX_HTTP_TIMEOUTS = 50    # 15초 제한 초과 요청 — 09-18 192건(09-17은 3건)이 잔고 대조를 100초까지 붙잡았다


def median(values: list[int]) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def secs(m: re.Match) -> int:
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))


def collect(date: str, log: Path, since: int = 0):
    """로그 한 파일에서 그날 점검 행을 만든다.

    반환 (rows, session_count). rows 원소는 (이름, 통과, 등급, 설명). 세션이 없으면 rows 빈 리스트.
    market_close_autodoc이 마감 문서 4절에 이 표를 그대로 싣는다 — 사람이 따로 돌려 보지 않아도 되게.
    """

    starts: list[int] = []       # 엔진 시작 시각(초)
    last_ts = 0
    stale_max = 0
    guard_at: list[int] = []
    breakeven: list[int] = []
    fills: list[tuple[int, str, str]] = []
    rate_hits = 0
    ws_fallbacks = 0
    rtts: list[int] = []
    bucket_waits: list[int] = []
    recon_slow: list[tuple[int, int]] = []   # (ms, 사이클)
    http_timeouts = 0
    trendx_registered: list[int] = []
    session_window_rejects = 0
    signals: list[tuple[int, str, str, str]] = []  # (초, 전략 id, 종목, BUY|SELL)
    itb_attached: list[str] = []
    devscale_stops: list[tuple[int, str]] = []     # (초, 종목) — DEVSCALE 손절 신호
    devscale_close_exits: list[tuple[int, str]] = []   # (초, 종목) — DEVSCALE 장 마감 청산 신호(넘김 모드면 0이어야 한다)
    entry_filter: dict[str, int] = {"통과": 0, "차단": 0}  # 진입 필터 판정 줄 수

    # 7일 지난 날은 archive/quant_trader_<날짜>.log.gz — market_close_autodoc이 그 경로를 그대로 넘긴다
    opener = (lambda: gzip.open(log, "rt", encoding="utf-8", errors="replace")) if log.suffix == ".gz"         else (lambda: log.open(encoding="utf-8", errors="replace"))
    with opener() as log_file:
        for line in log_file:
            m = TS_RE.match(line)
            if not m or m.group(1) != date:
                continue
            second = secs(m)
            if second < since:
                continue
            last_ts = second
            if START_RE.search(line):
                starts.append(second)
            found = STALE_RE.search(line)
            if found:
                stale_max = max(stale_max, int(found.group(1)))
            if GUARD_RE.search(line):
                guard_at.append(second)
            if BREAKEVEN_RE.search(line):
                breakeven.append(second)
            found = FILL_RE.search(line)
            if found:
                fills.append((second, found.group(1), found.group(2)))
            if RATE_RE.search(line):
                rate_hits += 1
            if WSFALL_RE.search(line):
                ws_fallbacks += 1
            found = RTT_RE.search(line)
            if found:
                rtts.append(int(found.group(1)))
                if found.group(2) is not None:
                    bucket_waits.append(int(found.group(2)))
            found = RECON_SLOW_RE.search(line)
            if found:
                recon_slow.append((int(found.group(1)), int(found.group(2))))
            if HTTP_TIMEOUT_RE.search(line):
                http_timeouts += 1
            if found := TRENDX_REGISTER_RE.search(line):
                trendx_registered.append(int(found.group(1)))
            if SESSION_WINDOW_REJECT_RE.search(line):
                session_window_rejects += 1
            if found := SIGNAL_RE.search(line):
                signals.append((second, found.group(1), found.group(2), found.group(3)))
            if found := ITB_ATTACH_RE.search(line):
                itb_attached.append(found.group(1))
            if found := DEVSCALE_STOP_RE.search(line):
                devscale_stops.append((second, found.group(1)))
            if found := DEVSCALE_CLOSE_EXIT_RE.search(line):
                devscale_close_exits.append((second, found.group(1)))
            if found := DEVSCALE_ENTRY_FILTER_RE.search(line):
                entry_filter[found.group(2)] += 1

    if not starts:
        return [], 0

    # 세션 길이 — 마지막 세션은 지금까지 살아 있는 것으로 본다.
    bounds = starts + [last_ts]
    short = [starts[i] for i in range(len(starts))
             if bounds[i + 1] - starts[i] < MIN_SESSION_SEC]

    # 재기동 투매 — 청산 관리 부착 직후 창에 들어온 본전탈출.
    dump = [b for b in breakeven
            if any(0 <= b - g <= GUARD_QUIET_SEC for g in guard_at)]

    # 회전 — 같은 종목 매도 체결 뒤 CHURN_SEC 안에 매수 체결.
    churn = 0
    last_sell: dict[str, int] = {}
    for second, ticker, side in fills:
        if side == "SELL":
            last_sell[ticker] = second
        elif ticker in last_sell and second - last_sell[ticker] <= CHURN_SEC:
            churn += 1
            del last_sell[ticker]

    def hhmm(second: int) -> str:
        return f"{second // 3600:02d}:{second % 3600 // 60:02d}:{second % 60:02d}"

    # 주문 접수 지연 — RTT가 큰데 버킷대기가 작으면 서버 응답 지연, 버킷대기가 크면 초당한도 압박.
    slow_orders = sum(1 for rtt in rtts if rtt >= SLOW_ORDER_MS)
    orders_ok = not rtts or slow_orders <= len(rtts) * MAX_SLOW_ORDER_RATIO
    if not rtts:
        order_detail = "주문 없음"
    else:
        order_detail = (f"접수·거부 {len(rtts)}건 RTT 중앙값 {median(rtts)}ms, "
                        f"{SLOW_ORDER_MS // 1000}초↑ {slow_orders}건")
        if not bucket_waits:
            order_detail += " — 버킷대기 계측 없음(09-19 이전 바이너리)"
        elif median(bucket_waits) >= BUCKET_WAIT_MS:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms → 초당한도 버킷이 원인"
        elif not orders_ok:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms → KIS 서버 응답 지연(버킷 아님)"
        else:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms"

    # 잔고 조회 지연(D-100) — 조회가 사이클을 넘긴 횟수와 제한 시간 초과 GET. 사이클은 멈추지 않았어야 한다.
    if recon_slow:
        worst_ms = max(milliseconds for milliseconds, _ in recon_slow)
        worst_cycles = max(cycles for _, cycles in recon_slow)
        recon_detail = (f"사이클 넘긴 조회 {len(recon_slow)}회 (최대 {worst_ms}ms·{worst_cycles}사이클), "
                        f"제한 시간 초과 {http_timeouts}건 (허용 {MAX_HTTP_TIMEOUTS})")
    else:
        recon_detail = f"사이클 넘긴 조회 0회, 제한 시간 초과 {http_timeouts}건 (허용 {MAX_HTTP_TIMEOUTS})"

    # 09-20 청산선 재설정 — 네 손실 경로. 승계 직후 매도는 그 종목의 마지막 부착 시각 기준.
    displace_sells = [(second, ticker) for second, sid, ticker, side in signals if sid == "DISPLACE" and side == "SELL"]
    #  부착은 묶음 한 줄(청산 관리 N종목 부착)로만 찍히므로 그 시각 기준.
    itb_early_sells = [(second, ticker) for second, sid, ticker, side in signals
                       if sid.startswith("ITB_") and side == "SELL"
                       and any(0 <= second - attach_t <= GUARD_QUIET_SEC for attach_t in guard_at)]
    stop_rebuys = [(buy_t, ticker) for buy_t, sid, ticker, side in signals
                   if sid.startswith("DEVSCALE_") and side == "BUY"
                   and any(stop_ticker == ticker and buy_t > stop_t for stop_t, stop_ticker in devscale_stops)]
    devscale_buys = sum(1 for _, sid, _, side in signals if sid.startswith("DEVSCALE_") and side == "BUY")
    stop_ratio = len(devscale_stops) / devscale_buys if devscale_buys else 0.0

    filter_judged = entry_filter["통과"] + entry_filter["차단"]
    devscale_v2 = filter_judged > 0     # 진입 필터 줄이 있으면 D-111 넘김 바이너리

    def devscale_v2_row(name: str, ok: bool, level: str, detail: str):
        if not devscale_v2:
            return (name, True, level, "진입 필터 판정 줄 없음(D-111 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    rows = [
        devscale_v2_row("장 마감 청산(넘김)", not devscale_close_exits, "FAIL",
                        f"DEVSCALE 장 마감 청산 신호 {len(devscale_close_exits)}건 (기대 0 — market_close_exit_hhmm 2400, D-111)"
                        + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in devscale_close_exits[:5])}" if devscale_close_exits else "")),
        devscale_v2_row("진입 필터 판정", entry_filter["차단"] > 0, "WARN",
                        f"진입 필터 통과 {entry_filter['통과']} / 차단 {entry_filter['차단']} 종목 (리플레이 기대: 존 안 종목의 절반쯤 차단, 차단 0이면 필터 값이 안 실린 것)"),
        ("교체 매도", not displace_sells, "FAIL",
         f"교체 매도 신호 {len(displace_sells)}건 (기대 0 — displace_enabled false, 09-20)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in displace_sells[:5])}" if displace_sells else "")),
        ("승계 직후 매도", not itb_early_sells, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 매도 {len(itb_early_sells)}건 (기대 0 — guard_warmup_sec 0 우회, 09-20)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in itb_early_sells[:5])}" if itb_early_sells else "")),
        ("손절 뒤 당일 재매수", not stop_rebuys, "FAIL",
         f"DEVSCALE 손절 {len(devscale_stops)}건 뒤 같은 종목 재매수 {len(stop_rebuys)}건 (기대 0 — stop_cooldown_sec 21600)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in stop_rebuys[:5])}" if stop_rebuys else "")),
        ("손절 비율", devscale_buys < 20 or stop_ratio <= MAX_STOP_PER_BUY, "WARN",
         f"DEVSCALE 손절 {len(devscale_stops)} / 매수 신호 {devscale_buys} = {stop_ratio:.0%} (허용 {MAX_STOP_PER_BUY:.0%}, 분봉 예측 21% — 넘으면 -2.0%가 잡음에 걸리는 것)"
         + (" — 표본 20건 미만, 판정 보류" if devscale_buys < 20 else "")),
        ("유령주문 재부활", stale_max <= MAX_STALE_ORDERS, "FAIL",
         f"기동 시 미체결 최대 {stale_max}건 (허용 {MAX_STALE_ORDERS})"),
        ("조기 사망 세션", not short, "FAIL",
         f"{MIN_SESSION_SEC}초 미만 종료 {len(short)}회"
         + (f" — {', '.join(hhmm(second) for second in short[:5])}" if short else "")),
        ("재기동 투매", not dump, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 본전탈출 {len(dump)}건"
         + (f" — {', '.join(hhmm(second) for second in dump[:5])}" if dump else "")),
        ("매도→재매수 회전", churn <= MAX_CHURN, "FAIL",
         f"{CHURN_SEC}초 내 반대매매 {churn}회 (허용 {MAX_CHURN})"),
        ("초당한도 압박", rate_hits <= MAX_RATE_HITS, "WARN",
         f"초당 거래건수 거부 {rate_hits}건 (허용 {MAX_RATE_HITS})"),
        ("WS 폴백", ws_fallbacks == 0, "WARN",
         f"REST 폴링 폴백 {ws_fallbacks}회 — 틱 주기 30초"),
        ("주문 접수 지연", orders_ok, "WARN", order_detail),
        ("잔고 조회 지연", http_timeouts <= MAX_HTTP_TIMEOUTS, "WARN", recon_detail),
        ("TRENDX 정지", max(trendx_registered, default=0) == 0, "FAIL",
         (f"초기 등록 최대 {max(trendx_registered)}종목 (기대 0, D-101 결정 2)" if trendx_registered
          else "TRENDX 등록 줄 없음 — 전략 미로드 또는 등록 0")),
        ("매매 창 밖 거부", session_window_rejects == 0, "FAIL",
         f"세션 창 밖 거부 {session_window_rejects}건 (기대 0 — 마감 청산 모의 15:15·실계좌 19:50, D-101 결정 3)"),
    ]
    return rows, len(starts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--date", default=dt.date.today().isoformat())
    parser.add_argument("--log", default=str(DEFAULT_LOG))
    # 하루 로그는 누적이라, 방금 고친 결함의 과거 이력까지 같이 잡힌다.
    #  "고친 뒤로 다시 났나"를 보려면 수정 반영 시각을 준다.
    parser.add_argument("--since", default="", help="HH:MM — 이 시각 이후만 본다")
    arguments = parser.parse_args()

    since = 0
    if arguments.since:
        hour_text, _, minute_text = arguments.since.partition(":")
        since = int(hour_text) * 3600 + int(minute_text or 0) * 60

    log = Path(arguments.log)
    if not log.exists():
        print(f"로그 없음: {log}")
        return 1

    rows, session_count = collect(arguments.date, log, since)
    if not rows:
        print(f"{arguments.date}: 엔진 시작 기록이 없다 — 점검할 세션이 없음")
        return 0

    scope = f" {arguments.since}~" if arguments.since else ""
    print(f"=== 실행 건전성 점검 {arguments.date}{scope} (세션 {session_count}회) ===")
    bad = 0
    for name, ok, level, detail in rows:
        tag = "PASS" if ok else level
        if not ok and level == "FAIL":
            bad += 1
        print(f"  [{tag:4}] {name:16} {detail}")
    print(f"--- FAIL {bad}건 ---")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
