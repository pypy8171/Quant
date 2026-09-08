#!/usr/bin/env python3
"""장 종료 사실 수집 — 그날 로그·원장에서 검증 가능한 수치만 뽑아 JSON으로 낸다.

/eod-review 커맨드의 1단계다. 판단·원인 추정은 여기서 하지 않는다.
여기서 나온 숫자만 리뷰 문서의 근거로 쓰고, 로그에 없는 값은 만들지 않는다.

사용:
  py scripts/eod_collect.py                 # 오늘
  py scripts/eod_collect.py --date 20260907
  py scripts/eod_collect.py --md            # 사람이 읽는 요약(기본은 JSON)
종료코드: 0=수집 완료, 2=그 날짜의 원장/로그를 못 찾음.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict, deque
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402
from log_patterns import PNL_ONLY_RE as PNL_RE  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

SESSION_RE = re.compile(r"=== Quant Trader")
LINE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.\d+ \[(\w+)\s*\] (.*)$")
SEED_RE = re.compile(r"시드 (\d{6}) (\S+) (\d+)주 @평단 (\d+) 주문가능=(\d+)")
DEV_REG_RE = re.compile(r"전략 등록: DeviationScale \| (\d{6})\(([^)]*)\)")
ITB_REG_RE = re.compile(r"전략 등록: ITB \| (\d{6}).*hold=(\d+)")
SCAN_RE = re.compile(r"정배열 프리필터: .*등록=(\d+)")
SIGNAL_RE = re.compile(r"신호: \[([^\]]+)\] (\d{6})\S* (BUY|SELL) (\d+)(?: \| 근거: (.*))?")
ZONE_RE = re.compile(r"\[(DEVSCALE_\d{6})\] (\d{6})\(([^)]*)\) 존 판정 (\S+) .*이격=(-?[\d.]+)%")
# 숫자·ODNO를 지워 사유를 묶는다(같은 사유가 건마다 다른 문자열로 흩어지지 않게).
NUM_RE = re.compile(r"\d{3,}")


def find_files(date: str):
    """그 날짜 원장(행 수 최대, 동률이면 mtime 최신)과 그 옆의 로그. 규칙은 _logdir 하나다."""
    csv = _logdir.find_ledger(date)
    if csv is None:
        return None, None
    log = csv.parent / "quant_trader.log"
    return (log if log.exists() else None), csv


def norm_reason(s: str) -> str:
    return NUM_RE.sub("N", s).strip()


def scan_log(log: Path, ymd: str) -> dict:
    """ymd = YYYY-MM-DD. 그 날짜 라인만 훑는다."""
    out: dict = {
        "sessions": [], "seed": [], "registered_dev": [], "registered_itb": [],
        "rescan_computed": [], "signals": [], "zone_last": {},
        "errors": Counter(), "ws_reconnects": [], "pnl_track": [],
    }
    with log.open(encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip("\n"))
            if not m:
                continue
            day, hms, lvl, rest = m.groups()
            if day != ymd:
                continue
            if SESSION_RE.search(rest):
                out["sessions"].append(hms)
                # 시드·등록·존 판정은 세션마다 새로 잡힌다. 마지막 세션 것만 남겨야
                #  "지금 무엇이 붙어 있나"를 보여준다(세션 누적은 중복으로 오독됨).
                out["seed"], out["registered_dev"] = [], []
                out["registered_itb"], out["zone_last"] = [], {}
            s = SEED_RE.search(rest)
            if s:
                out["seed"].append({"ticker": s[1], "name": s[2], "qty": int(s[3]),
                                    "avg": int(s[4]), "sellable": int(s[5])})
            s = DEV_REG_RE.search(rest)
            if s:
                out["registered_dev"].append({"ticker": s[1], "name": s[2], "at": hms})
            s = ITB_REG_RE.search(rest)
            if s:
                out["registered_itb"].append({"ticker": s[1], "hold": int(s[2]), "at": hms})
            s = SCAN_RE.search(rest)
            if s:
                out["rescan_computed"].append({"at": hms, "registered": int(s[1])})
            s = SIGNAL_RE.search(rest)
            if s:
                out["signals"].append({"at": hms, "strategy": s[1], "ticker": s[2],
                                       "side": s[3], "qty": int(s[4]),
                                       "reason": (s[5] or "").strip()})
            s = ZONE_RE.search(rest)
            if s:
                out["zone_last"][s[2]] = {"name": s[3], "state": s[4],
                                          "dev_pct": float(s[5]), "at": hms}
            if lvl in ("ERROR", "WARN"):
                out["errors"][norm_reason(rest)[:90]] += 1
            if "재연결 성공" in rest:
                out["ws_reconnects"].append(hms)
            s = PNL_RE.search(rest)
            if s:
                out["pnl_track"].append({"at": hms, "pnl": int(s[1])})
    out["errors"] = out["errors"].most_common(15)
    return out


def scan_ledger(csv: Path) -> dict:
    import csv as _csv
    rows = list(_csv.DictReader(csv.open(encoding="utf-8", errors="replace")))
    rows = [r for r in rows if r.get("strategy") != "TEST"]
    ev = Counter(r["event"] for r in rows)
    rejects = Counter()
    for r in rows:
        if r["event"] == "REJECTED":
            rejects[norm_reason(r.get("reason", ""))[:80]] += 1
    by_strat = Counter(r["strategy"] for r in rows
                       if r["event"] in ("ACCEPTED", "FILL", "REJECTED"))

    # 라운드트립 — 종목별 FIFO. 양변 모두 FILL 행이 있는 분만 실현으로 센다.
    lots = defaultdict(deque)
    trips = []
    for r in sorted(rows, key=lambda x: x["ts_kst"]):
        if r["event"] != "FILL":
            continue
        q = int(r["fill_qty"] or 0)
        px = float(r["fill_price"] or 0)
        if q <= 0:
            continue
        t = r["ticker"]
        if r["side"] == "BUY":
            lots[t].append([q, px, r["ts_kst"]])
        else:
            left = q
            while left > 0 and lots[t]:
                lot = lots[t][0]
                take = min(left, lot[0])
                trips.append({"ticker": t, "qty": take, "buy": lot[1], "sell": px,
                              "gross": round((px - lot[1]) * take),
                              "in": lot[2], "out": r["ts_kst"]})
                lot[0] -= take
                left -= take
                if lot[0] == 0:
                    lots[t].popleft()
            # 남은 left는 전일 이월분 매도 → 진입가를 모르므로 버린다(손익 날조 금지).

    # 주문 이벤트 공백 — 가장 긴 무주문 구간.
    ts = [r["ts_kst"] for r in rows if r["event"] in ("ACCEPTED", "FILL", "REJECTED")]
    gap = None
    if len(ts) >= 2:
        fmt = "%Y-%m-%d %H:%M:%S"
        d = [datetime.strptime(x, fmt) for x in sorted(ts)]
        worst = max(zip(d, d[1:]), key=lambda p: (p[1] - p[0]).total_seconds())
        gap = {"from": worst[0].strftime(fmt), "to": worst[1].strftime(fmt),
               "minutes": round((worst[1] - worst[0]).total_seconds() / 60, 1)}

    return {"rows": len(rows), "events": dict(ev), "rejects": rejects.most_common(15),
            "by_strategy": by_strat.most_common(), "roundtrips": trips,
            "realized_gross": sum(t["gross"] for t in trips), "longest_order_gap": gap}


def build(date: str) -> dict:
    log, csv = find_files(date)
    if csv is None:
        print("[eod_collect] " + date + " 원장을 찾지 못했습니다. 탐색: "
              + ", ".join(str(d) for d in _logdir.candidate_dirs()), file=sys.stderr)
        sys.exit(2)
    ymd = date[:4] + "-" + date[4:6] + "-" + date[6:]
    pack = {"date": ymd, "ledger_path": str(csv.relative_to(REPO)),
            "log_path": str(log.relative_to(REPO)) if log else None}
    pack["ledger"] = scan_ledger(csv)
    pack["log"] = scan_log(log, ymd) if log else {}
    lg = pack["log"]
    if lg:
        # 재스캔이 계산만 하고 반영됐는지 — 마지막 전략 등록 시각 이후의 스캔 계산 건수.
        last_reg = max([r["at"] for r in lg["registered_dev"]], default="00:00:00")
        lg["rescan_after_last_register"] = sum(
            1 for r in lg["rescan_computed"] if r["at"] > last_reg)
        lg["seed_blocked"] = [s for s in lg["seed"] if s["sellable"] == 0]
        lg["signal_counts"] = Counter(s["side"] for s in lg["signals"]).most_common()
    return pack


def to_md(p: dict) -> str:
    L = p["ledger"]
    lg = p.get("log", {})
    o = ["# 사실 수집 — " + p["date"], "",
         "- 원장: `" + p["ledger_path"] + "` (" + str(L["rows"]) + "행, TEST 제외)",
         "- 로그: `" + str(p["log_path"]) + "`",
         "- 세션: " + str(len(lg.get("sessions", []))) + "회 " + str(lg.get("sessions", [])),
         "- 이벤트: " + str(L["events"]),
         "- 실현(양변 체결 확인분): " + format(L["realized_gross"], ",") + "원 / "
         + str(len(L["roundtrips"])) + "건",
         "- 최장 무주문 구간: " + str(L["longest_order_gap"]), ""]
    if lg.get("signal_counts"):
        o += ["- 전략 신호: " + str(lg["signal_counts"]), ""]
    if L["rejects"]:
        o += ["## 거부 사유", ""] + ["- " + str(n) + "회 — " + r for r, n in L["rejects"]] + [""]
    if lg.get("seed_blocked"):
        o += ["## 시드 주문가능=0 (매도 봉쇄 후보)", ""] + [
            "- " + s["ticker"] + " " + s["name"] + " " + str(s["qty"]) + "주 @평단 "
            + format(s["avg"], ",") for s in lg["seed_blocked"]] + [""]
    if lg.get("zone_last"):
        o += ["## 존 판정 최종 상태", ""] + [
            "- " + t + " " + v["name"] + " " + v["state"] + " 이격=" + str(v["dev_pct"])
            + "% (" + v["at"] + ")" for t, v in sorted(lg["zone_last"].items())] + [""]
    if lg.get("rescan_computed"):
        o += ["## 재스캔 — 계산 " + str(len(lg["rescan_computed"])) + "회, 마지막 전략 등록 이후 계산 "
              + str(lg.get("rescan_after_last_register", 0)) + "회 (반영 0이면 D-14)", ""]
    if lg.get("errors"):
        o += ["## ERROR/WARN 상위", ""] + ["- " + str(n) + "회 — " + r for r, n in lg["errors"]] + [""]
    if L["roundtrips"]:
        o += ["## 라운드트립", "", "| 종목 | 수량 | 진입 | 청산 | Gross |", "|---|---|---|---|---|"] + [
            "| " + t["ticker"] + " | " + str(t["qty"]) + " | " + format(t["buy"], ",.0f")
            + " | " + format(t["sell"], ",.0f") + " | " + format(t["gross"], ",") + " |"
            for t in L["roundtrips"]] + [""]
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="YYYYMMDD (기본: 오늘)")
    ap.add_argument("--md", action="store_true", help="마크다운 요약 출력(기본 JSON)")
    a = ap.parse_args()
    pack = build(a.date or datetime.now().strftime("%Y%m%d"))
    print(to_md(pack) if a.md else json.dumps(pack, ensure_ascii=False, indent=1, default=str))


if __name__ == "__main__":
    main()
