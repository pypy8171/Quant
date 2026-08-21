#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""quant_trader.log 텍스트 이벤트 파서 — 감시(증분)·마감(전일) 공용.

`logs/trades_YYYYMMDD.csv`(구조화 원장)에는 없는 **운영 이벤트**를 텍스트 로그에서 뽑는다:
ERROR·KIS거부·게이트봉쇄·WS재연결·HTTP오류·주문접수·체결.

두 모드:
  --watch   증분. 상태파일(byte offset)부터 새 줄만 분류해 "유의미한 창"일 때만
            1~수 줄 다이제스트를 stdout에 찍는다(조용하면 아무것도 안 찍음 → 감시 토큰 0).
            Monitor 폴링 루프에서 20분마다 호출하는 용도.
  --full    전일 전체 요약. 카운트·사유 히스토그램·샘플을 낸다. --json 이면 대시보드용 JSON.

정직성: 손익을 지어내지 않는다. 발주/거부/체결 "카운트"와 사유·에러 원문 샘플만.

실행:
  py scripts/parse_quant_log.py --watch
  py scripts/parse_quant_log.py --full [--date YYYYMMDD] [--json]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

# Windows 콘솔(cp949)에서도 한글·기호(·—×) 깨짐/크래시 없이 출력
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[1]
LOGS = _REPO / "logs"
LOGFILE = LOGS / "quant_trader.log"
STATE = LOGS / ".watch_intraday_state.json"

# 라인 프리픽스:  2026-08-12 12:06:02.011 [INFO ] [태그] 메시지
_LINE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.\d{3}\s+"
    r"\[(?P<lvl>\w+)\s*\]\s+"
    r"(?P<rest>.*)$"
)

# HTTP 오류 코드 추출(4xx/5xx만 오류로 취급)
_HTTP = re.compile(r"HTTP\s+(?P<code>\d{3})")
# 게이트 봉쇄 사유: "거부 [ORD-xxx] ... → <사유>"
_GATE_REASON = re.compile(r"→\s*(?P<reason>.+?)\s*$")


def classify(rest: str, lvl: str):
    """한 줄(레벨 이후 본문)을 (category, detail) 로 분류. 관심 밖이면 None."""
    # 체결 (주문접수보다 먼저 검사 — 둘 다 OrderRouter)
    if "체결 확인" in rest or ("체결" in rest and "통보" in rest):
        return ("fill", rest)
    if "[OrderRouter] 접수 [ORD-" in rest:
        return ("order", rest)
    if "[Strategy] 신호:" in rest:
        return ("signal", rest)
    # 청산차단(수동 확인 필요) — 항상 즉시 노출
    if "청산차단" in rest:
        return ("liq_block", rest)
    # KIS 거부 / 주문 오류
    if "KIS 거부" in rest or "주문 오류" in rest:
        return ("kis_reject", rest)
    # 게이트 봉쇄(WARN OrderRouter 거부 → 사유)
    if lvl.startswith("WARN") and "[OrderRouter] 거부" in rest:
        m = _GATE_REASON.search(rest)
        return ("gate_block", m.group("reason") if m else rest)
    # WS 재연결
    if "[WS]" in rest and ("재연결" in rest or "수신 오류" in rest):
        return ("ws_reconnect", rest)
    # HTTP 오류
    m = _HTTP.search(rest)
    if m and m.group("code")[0] in ("4", "5"):
        return ("http_error", "HTTP " + m.group("code"))
    # 남은 ERROR 전부
    if lvl.startswith("ERROR"):
        return ("error", rest)
    return None


def scan(lines):
    """줄 이터러블 → {category: [detail...]}, 그리고 시간범위(ts_first, ts_last)."""
    buckets: dict[str, list[str]] = {}
    ts_first = ts_last = None
    for raw in lines:
        m = _LINE.match(raw)
        if not m:
            continue
        ts, lvl, rest = m.group("ts"), m.group("lvl"), m.group("rest")
        if ts_first is None:
            ts_first = ts
        ts_last = ts
        c = classify(rest, lvl)
        if c:
            buckets.setdefault(c[0], []).append(c[1])
    return buckets, ts_first, ts_last


# ── 상태(offset) ─────────────────────────────────────────────────────────────
def load_state():
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except Exception:
        return {"offset": 0}


def save_state(offset: int):
    try:
        STATE.write_text(json.dumps({"offset": offset}), encoding="utf-8")
    except Exception as e:
        print(f"  ! 상태 저장 실패: {e}", file=sys.stderr)


# ── 감시(증분) ───────────────────────────────────────────────────────────────
# HTTP 오류가 이 임계치를 넘어야만 "스파이크"로 노출(시세 REST 500은 만성 잡음).
HTTP_SPIKE = 30


def watch():
    if not LOGFILE.exists():
        return  # 엔진 미가동 → 조용
    st = load_state()
    off = int(st.get("offset", 0))
    size = LOGFILE.stat().st_size
    if size < off:            # 로그 로테이션/절단 → 처음부터
        off = 0
    with open(LOGFILE, "r", encoding="utf-8", errors="replace") as f:
        f.seek(off)
        new = f.readlines()
        new_off = f.tell()
    save_state(new_off)
    if not new:
        return

    buckets, t0, t1 = scan(new)
    n_order = len(buckets.get("order", []))
    n_fill = len(buckets.get("fill", []))
    n_kis = len(buckets.get("kis_reject", []))
    n_gate = len(buckets.get("gate_block", []))
    n_ws = len(buckets.get("ws_reconnect", []))
    n_http = len(buckets.get("http_error", []))
    n_err = len(buckets.get("error", []))
    n_liq = len(buckets.get("liq_block", []))

    # 유의미 판단: 주문·체결·거부·게이트·WS·에러·청산차단 중 하나라도 / HTTP 스파이크
    significant = any([n_order, n_fill, n_kis, n_gate, n_ws, n_err, n_liq]) or n_http > HTTP_SPIKE
    if not significant:
        return

    win = f"{t0[11:16]}–{t1[11:16]}" if t0 else "?"
    head = (f"[감시 {win}] 주문{n_order} 체결{n_fill} KIS거부{n_kis} "
            f"게이트봉쇄{n_gate} WS재연결{n_ws} HTTP오류{n_http} ERROR{n_err}")
    out = [head]
    # 반드시 즉시 노출할 것: 청산차단·ERROR·KIS거부(사유 원문 샘플 최대 5)
    for cat, label in (("liq_block", "‼ 청산차단"), ("error", "✗ ERROR"),
                       ("kis_reject", "✗ KIS거부")):
        for d in buckets.get(cat, [])[:5]:
            out.append(f"  {label}: {d[:160]}")
    # 게이트 봉쇄 사유 히스토그램
    if n_gate:
        hist = Counter(buckets["gate_block"]).most_common(4)
        out.append("  게이트사유: " + ", ".join(f"{r}×{n}" for r, n in hist))
    print("\n".join(out), flush=True)


# ── 마감(전일 전체) ──────────────────────────────────────────────────────────
def _resolve_date(date: str | None) -> str:
    if date:
        return date
    # 최신 trades_YYYYMMDD.csv 기준 오늘자 추정
    cands = sorted(LOGS.glob("trades_*.csv"))
    if cands:
        m = re.search(r"trades_(\d{8})", cands[-1].name)
        if m:
            return m.group(1)
    return ""


def full(date: str | None, as_json: bool):
    if not LOGFILE.exists():
        print("로그 파일 없음:", LOGFILE, file=sys.stderr)
        return 1
    ymd = _resolve_date(date)          # YYYYMMDD 또는 ''
    dprefix = ""
    if ymd:
        dprefix = f"{ymd[:4]}-{ymd[4:6]}-{ymd[6:]}"
    with open(LOGFILE, "r", encoding="utf-8", errors="replace") as f:
        if dprefix:
            lines = [ln for ln in f if ln.startswith(dprefix)]
        else:
            lines = f.readlines()
    buckets, t0, t1 = scan(lines)

    summary = {
        "schema": "quant.logevents/v1",
        "date": dprefix or "(전체)",
        "span": {"first": t0, "last": t1},
        "counts": {k: len(v) for k, v in buckets.items()},
        "gate_reasons": dict(Counter(buckets.get("gate_block", [])).most_common()),
        "samples": {
            k: [d[:200] for d in v[:8]]
            for k, v in buckets.items()
            if k in ("error", "kis_reject", "liq_block")
        },
    }
    if as_json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0

    c = summary["counts"]
    print(f"── 로그 이벤트 요약 {summary['date']} ({t0} ~ {t1}) ──")
    order = [
        ("신호", "signal"), ("주문접수", "order"), ("체결", "fill"),
        ("KIS거부", "kis_reject"), ("게이트봉쇄", "gate_block"),
        ("WS재연결", "ws_reconnect"), ("HTTP오류", "http_error"),
        ("ERROR", "error"), ("청산차단", "liq_block"),
    ]
    for label, key in order:
        print(f"  {label:9s}: {c.get(key, 0)}")
    if summary["gate_reasons"]:
        print("  게이트봉쇄 사유:")
        for r, n in summary["gate_reasons"].items():
            print(f"    - {r} ×{n}")
    for key, label in (("liq_block", "청산차단"), ("kis_reject", "KIS거부"), ("error", "ERROR")):
        s = summary["samples"].get(key)
        if s:
            print(f"  {label} 샘플:")
            for d in s:
                print(f"    · {d}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--watch", action="store_true", help="증분 감시(유의미 창만 출력)")
    g.add_argument("--full", action="store_true", help="전일 전체 요약")
    g.add_argument("--seek-end", action="store_true",
                   help="offset을 로그 끝으로 맞추고 종료(감시 시작 시 과거 백로그 스킵)")
    ap.add_argument("--date", help="YYYYMMDD (미지정 시 최신 trades_*.csv 날짜)")
    ap.add_argument("--json", action="store_true", help="--full 시 JSON 출력")
    ap.add_argument("--reset", action="store_true", help="--watch 상태(offset) 초기화")
    a = ap.parse_args()
    if a.seek_end:
        save_state(LOGFILE.stat().st_size if LOGFILE.exists() else 0)
        return 0
    if a.watch:
        if a.reset:
            save_state(0)
        watch()
        return 0
    return full(a.date, a.json)


if __name__ == "__main__":
    sys.exit(main())
