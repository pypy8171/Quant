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

# 임계값. 넘으면 그날 운영이 실제로 상했던 수준이다.
MAX_STALE_ORDERS = 25     # 유령주문 재부활 — 취소 왕복이 초당한도를 밀어낸다
MIN_SESSION_SEC = 30      # 이보다 짧게 죽으면 배선/바이너리 문제(정상 재기동 아님)
GUARD_QUIET_SEC = 90      # 청산 관리 부착 직후 이 시간 안의 본전탈출은 재기동 투매다
CHURN_SEC = 120           # 같은 종목 매도→매수가 이 안에 오면 회전
MAX_CHURN = 3
MAX_RATE_HITS = 50


def secs(m: re.Match) -> int:
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=dt.date.today().isoformat())
    ap.add_argument("--log", default=str(DEFAULT_LOG))
    # 하루 로그는 누적이라, 방금 고친 결함의 과거 이력까지 같이 잡힌다.
    #  "고친 뒤로 다시 났나"를 보려면 수정 반영 시각을 준다.
    ap.add_argument("--since", default="", help="HH:MM — 이 시각 이후만 본다")
    a = ap.parse_args()

    since = 0
    if a.since:
        hh, _, mm = a.since.partition(":")
        since = int(hh) * 3600 + int(mm or 0) * 60

    log = Path(a.log)
    if not log.exists():
        print(f"로그 없음: {log}")
        return 1

    starts: list[int] = []       # 엔진 시작 시각(초)
    last_ts = 0
    stale_max = 0
    guard_at: list[int] = []
    breakeven: list[int] = []
    fills: list[tuple[int, str, str]] = []
    rate_hits = 0
    ws_fallbacks = 0

    with log.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TS_RE.match(line)
            if not m or m.group(1) != a.date:
                continue
            t = secs(m)
            if t < since:
                continue
            last_ts = t
            if START_RE.search(line):
                starts.append(t)
            mm = STALE_RE.search(line)
            if mm:
                stale_max = max(stale_max, int(mm.group(1)))
            if GUARD_RE.search(line):
                guard_at.append(t)
            if BREAKEVEN_RE.search(line):
                breakeven.append(t)
            mm = FILL_RE.search(line)
            if mm:
                fills.append((t, mm.group(1), mm.group(2)))
            if RATE_RE.search(line):
                rate_hits += 1
            if WSFALL_RE.search(line):
                ws_fallbacks += 1

    if not starts:
        print(f"{a.date}: 엔진 시작 기록이 없다 — 점검할 세션이 없음")
        return 0

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
    for t, tk, side in fills:
        if side == "SELL":
            last_sell[tk] = t
        elif tk in last_sell and t - last_sell[tk] <= CHURN_SEC:
            churn += 1
            del last_sell[tk]

    def hhmm(t: int) -> str:
        return f"{t // 3600:02d}:{t % 3600 // 60:02d}:{t % 60:02d}"

    rows = [
        ("유령주문 재부활", stale_max <= MAX_STALE_ORDERS, "FAIL",
         f"기동 시 미체결 최대 {stale_max}건 (허용 {MAX_STALE_ORDERS})"),
        ("조기 사망 세션", not short, "FAIL",
         f"{MIN_SESSION_SEC}초 미만 종료 {len(short)}회"
         + (f" — {', '.join(hhmm(t) for t in short[:5])}" if short else "")),
        ("재기동 투매", not dump, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 본전탈출 {len(dump)}건"
         + (f" — {', '.join(hhmm(t) for t in dump[:5])}" if dump else "")),
        ("매도→재매수 회전", churn <= MAX_CHURN, "FAIL",
         f"{CHURN_SEC}초 내 반대매매 {churn}회 (허용 {MAX_CHURN})"),
        ("초당한도 압박", rate_hits <= MAX_RATE_HITS, "WARN",
         f"초당 거래건수 거부 {rate_hits}건 (허용 {MAX_RATE_HITS})"),
        ("WS 폴백", ws_fallbacks == 0, "WARN",
         f"REST 폴링 폴백 {ws_fallbacks}회 — 틱 주기 30초"),
    ]

    scope = f" {a.since}~" if a.since else ""
    print(f"=== 실행 건전성 점검 {a.date}{scope} (세션 {len(starts)}회) ===")
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
