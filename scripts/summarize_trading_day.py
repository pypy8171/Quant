# -*- coding: utf-8 -*-
"""하루치 매매 사실 요약 — 매매일지(/trade-log)를 쓰기 전에 근거를 모으는 도구.

엔진 로그(`quant_trader.log`)와 원장(`trades_YYYYMMDD.csv`)에서 검증 가능한 수치만
뽑아 콘솔에 찍는다. 해석·서술은 하지 않는다.

사용법:
  py scripts/summarize_trading_day.py 2026-09-04
  py scripts/summarize_trading_day.py 2026-08-20 2026-08-21
"""
import os, re, csv, sys, glob, collections

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import _logdir  # noqa: E402
from log_patterns import PNL_RE  # noqa: E402

LOG_DIRS = [str(d) for d in _logdir.candidate_dirs()]

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


def find_logs():
    out = []
    for d in LOG_DIRS:
        for p in glob.glob(os.path.join(d, "quant_trader.log")) + \
                 glob.glob(os.path.join(d, "archive", "*.log")):
            out.append(p)
    return out


def find_ledger(date):
    """(경로, 행 수) — 행 수 최대, 동률이면 mtime 최신. 규칙은 _logdir 하나다."""
    c = _logdir.ledger_candidates(date)
    return (str(c[0][0]), c[0][1]) if c else None


def log_lines(date):
    """해당 날짜 라인을 가진 로그를 전부 모아 시각순으로 합친다.

    엔진이 실행 cwd 하위 logs/ 에 쓰기 때문에 같은 날짜가 여러 파일로 갈릴 수 있다.
    한 파일만 보면 그 날의 일부 구간만 보게 된다."""
    srcs, buf = [], []
    for p in find_logs():
        got, first, last = 0, None, None
        for i, l in enumerate(open(p, encoding="utf-8", errors="replace"), 1):
            if l.startswith(date):
                if first is None:
                    first = i
                last = i
                got += 1
                buf.append(l.rstrip("\n"))
        if got:
            srcs.append("%s %d~%d줄(%d행)" % (os.path.relpath(p, REPO), first, last, got))
    buf.sort(key=lambda l: l[:23])
    return srcs, buf


def norm(s, n=110):
    s = re.sub(r"\d+", "#", s)
    return s[:n]


def head(t):
    print("\n" + "─" * 4 + " " + t)


def summarize(date):
    print("=" * 72)
    print("== " + date)
    print("=" * 72)

    srcs, lines = log_lines(date)
    if lines:
        print("로그 " + " + ".join(srcs) + "  -> 합계 %d행" % len(lines))
        print("시간범위 %s ~ %s" % (lines[0][11:19], lines[-1][11:19]))

        lv = collections.Counter()
        comp = collections.Counter()
        for l in lines:
            m = re.search(r"\[(INFO |WARN |ERROR|DEBUG)\]\s+\[([^\]]+)\]", l)
            if m:
                lv[m.group(1).strip()] += 1
                comp[m.group(2)] += 1
        print("레벨 %s" % dict(lv))
        print("컴포넌트 %s" % dict(comp.most_common(8)))

        head("세션 경계 (=== Quant Trader ===)")
        sess = [l[11:19] for l in lines if "=== Quant Trader" in l]
        print("  %d개: %s" % (len(sess), ", ".join(sess)))
        for kw in ("프로그램 종료", "종료", "crash", "예외"):
            hits = [l for l in lines if kw in l][:3]
            if hits and kw in ("프로그램 종료",):
                for h in hits:
                    print("  " + h[:150])

        head("국면")
        for l in [x for x in lines if "[Regime]" in x][:2]:
            print("  " + l[:170])
        rs = [x for x in lines if "RegimeSelect" in x]
        print("  RegimeSelect %d회" % len(rs))
        if rs:
            print("  " + rs[0][:210])

        head("유니버스·전략 등록")
        for kw in ("프리필터", "재스캔 완료", "청산 관리", "전략 등록"):
            hits = [x for x in lines if kw in x]
            print("  [%s] %d건" % (kw, len(hits)))
            for h in hits[:3]:
                print("     " + h[:190])

        head("WARN 상위")
        for k, v in collections.Counter(
                norm(x.split("] ", 2)[-1]) for x in lines if "[WARN " in x).most_common(8):
            print("  %5d  %s" % (v, k))

        head("ERROR 상위")
        errs = [x for x in lines if "[ERROR]" in x]
        print("  총 %d건" % len(errs))
        for k, v in collections.Counter(norm(x.split("] ", 2)[-1]) for x in errs).most_common(8):
            print("  %5d  %s" % (v, k))

        head("당일손익 (잔고 대조)")
        pnl = []
        for l in lines:
            m = PNL_RE.search(l)
            if m:
                pnl.append((l[11:19], int(m.group(1)), int(m.group(2))))
        if pnl:
            hi = max(pnl, key=lambda x: x[1])
            lo = min(pnl, key=lambda x: x[1])
            print("  표본 %d | 시작 %s %+d | 피크 %s %+d | 저점 %s %+d | 종료 %s %+d (총평가 %d)"
                  % (len(pnl), pnl[0][0], pnl[0][1], hi[0], hi[1], lo[0], lo[1],
                     pnl[-1][0], pnl[-1][1], pnl[-1][2]))
            byh = collections.OrderedDict()
            for t, v, e in pnl:
                byh[t[:2]] = (t, v)
            print("  시간대별 마지막: " + " | ".join("%s %+d" % (v[0], v[1]) for v in byh.values()))
        else:
            print("  잔고 대조 기록 없음")
        prev = [l for l in lines if "전일총자산" in l]
        if prev:
            print("  " + prev[-1][11:150])
    else:
        print("로그 없음 — 이 날짜 라인을 가진 quant_trader.log를 찾지 못함")

    led = find_ledger(date)
    if not led:
        print("\n원장 없음")
        return
    lpath, _ = led
    rows = list(csv.DictReader(open(lpath, encoding="utf-8-sig")))
    g = lambda r, k: (r.get(k) or "")
    head("원장 %s (%d 이벤트)" % (os.path.relpath(lpath, REPO), len(rows)))
    print("  컬럼 %s" % list(rows[0].keys()))
    print("  시간 %s ~ %s" % (g(rows[0], "ts_kst"), g(rows[-1], "ts_kst")))
    print("  event  %s" % dict(collections.Counter(g(r, "event") for r in rows)))
    print("  status %s" % dict(collections.Counter(g(r, "status") for r in rows)))
    print("  side   %s" % dict(collections.Counter(g(r, "side") for r in rows)))
    tk = {g(r, "ticker") for r in rows if g(r, "ticker")}
    print("  종목 %d개 %s" % (len(tk), sorted(tk)[:12]))

    head("전략별 (상위 12)")
    agg = collections.defaultdict(collections.Counter)
    for r in rows:
        agg[g(r, "strategy")][g(r, "event")] += 1
    for k, v in sorted(agg.items(), key=lambda x: -sum(x[1].values()))[:12]:
        print("  %-20s 접수%4d 거부%4d 취소%4d 체결%4d"
              % (k, v["ACCEPTED"], v["REJECTED"], v["CANCELLED"], v["FILL"] + v["FILLED"]))

    head("거부 사유")
    for k, v in collections.Counter(
            norm(g(r, "reason"), 90) for r in rows if g(r, "event") == "REJECTED").most_common(12):
        print("  %5d  %s" % (v, k))

    head("entry_reason 상위")
    er = collections.Counter(g(r, "entry_reason")[:34] for r in rows if g(r, "entry_reason"))
    if er:
        for k, v in er.most_common(10):
            print("  %5d  %s" % (v, k))
    else:
        print("  (컬럼 없음 또는 전부 빈값)")

    fills = [r for r in rows if g(r, "event") in ("FILL", "FILLED")]
    if fills:
        head("체결 %d건" % len(fills))
        print("  시간 %s ~ %s" % (g(fills[0], "ts_kst"), g(fills[-1], "ts_kst")))
        for side in ("BUY", "SELL"):
            f = [r for r in fills if g(r, "side") == side]
            if not f:
                continue
            amt = sum(float(g(r, "fill_qty") or 0) * float(g(r, "fill_price") or 0) for r in f)
            print("  %s %d건 명목 %s원" % (side, len(f), format(int(amt), ",")))
        print("  종목별 %s" % collections.Counter(g(r, "ticker") for r in fills).most_common(10))

    head("시간 분포 (시각대별 이벤트)")
    byh = collections.Counter(g(r, "ts_kst")[11:13] for r in rows)
    print("  " + " | ".join("%s시 %d" % (h, n) for h, n in sorted(byh.items())))


def main():
    dates = sys.argv[1:]
    if not dates:
        print(__doc__)
        sys.exit(2)
    for d in dates:
        summarize(d)


if __name__ == "__main__":
    main()
