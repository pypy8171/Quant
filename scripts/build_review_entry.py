"""장 마감 리뷰 탭 항목(quant.review/v1)을 원장·로그 사실에서 만든다.

사실(손익 곡선·거부 사유·세션 수·인시던트 후보)은 이 스크립트가 채우고,
해석(축·개선안·게이트)은 사람 또는 /eod-review 가 덧쓴다. 기존 항목이 있으면
해석 키는 그대로 두고 사실 키만 갱신한다 — 손으로 쓴 문장을 자동 실행이
지우지 않게 하려는 것이다.

    py scripts/build_review_entry.py --date 2026-09-07
    py scripts/build_review_entry.py --date 2026-09-07 --dry-run
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter
from datetime import date as _date
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
REVIEWS = REPO / "research" / "dashboard" / "reviews.json"

import eod_collect  # noqa: E402  (경로 주입 뒤에 임포트)

# 이 스크립트가 소유하는 키. 나머지(axes·improvements·gate·dissent·kpi…)는 손대지 않는다.
AUTO_KEYS = ("eyebrow", "headline_html", "verdict_html", "meta_html",
             "doc_path", "log_path", "pnl", "incidents", "strategy")


def signed(x) -> str:
    return ("+" if x > 0 else "−" if x < 0 else "") + f"{abs(int(x)):,}"


def find_journal(ymd: str) -> str | None:
    hits = sorted((REPO / "strategies").glob(f"*/live/{ymd}.md"))
    if hits:
        return hits[0].relative_to(REPO).as_posix()
    p = REPO / "docs" / "eod" / f"{ymd}.md"
    return p.relative_to(REPO).as_posix() if p.exists() else None


def build_pnl(track: list, ymd: str):
    """일중 손익 폴링에서 시작·고점·저점·마감 네 지점만 남긴다.

    폴링 전량을 그리면 잔고 대조 노이즈가 곡선을 지배한다. 일지에 실제로 인용되는
    값도 이 네 개뿐이라 여기에 맞춘다.
    """
    if not track:
        return None
    pts = sorted(track, key=lambda r: r["at"])
    hi = max(pts, key=lambda r: r["pnl"])
    lo = min(pts, key=lambda r: r["pnl"])
    picks = {}
    for r in (pts[0], hi, lo, pts[-1]):
        picks[r["at"]] = r["pnl"]
    series = [{"t": at[:5], "v": v} for at, v in sorted(picks.items())]
    floor_, ceil_ = lo["pnl"], hi["pnl"]
    step = int(max(100_000, round(max(abs(floor_), abs(ceil_)) / 2, -5) or 100_000))
    grid = [g for g in (2 * step, step, 0, -step, -2 * step)
            if floor_ - step <= g <= ceil_ + step]
    return {
        "title_html": (ymd[5:] + " 세션 당일손익 "
                       "<span class=\"u\">KRW · 잔고 대조 폴링에서 시작·고점·저점·마감 4점</span>"),
        "series": series,
        "floor": floor_,
        "ceil": ceil_,
        "gridlines": grid or [0],
        "last_label": "세션손익",
        "note_html": ("고점 <b class=\"pos\">" + signed(ceil_) + "</b>@" + hi["at"][:5]
                      + " · 저점 <b class=\"neg\">" + signed(floor_) + "</b>@" + lo["at"][:5]
                      + " · 마감 <b>" + signed(pts[-1]["pnl"]) + "</b>@" + pts[-1]["at"][:5]
                      + ". 폴링 " + str(len(pts)) + "건 중 4점만 표시했다(연속 곡선 아님)."),
    }


def build_incidents(pack: dict) -> list:
    """거부 사유 히스토그램과 ERROR/WARN 상위를 인시던트 후보로 올린다.

    자동으로는 "무엇이 몇 번"까지만 쓴다. 원인·영향은 사람이 채우는 자리라
    impact_html은 비워 두고 표시만 남긴다.
    """
    ymd = pack["date"]
    led, lg = pack["ledger"], pack.get("log", {})
    ledger_path = pack["ledger_path"].replace("\\", "/")   # 문서에 박히는 경로는 posix로
    out = []
    for reason, n in led.get("rejects", [])[:3]:
        if n < 5:
            continue
        out.append({
            "sev": "거부",
            "when": ymd[5:] + " 종일",
            "title": "주문 거부 " + str(n) + "건 — " + reason[:48],
            "desc_html": ("원장 REJECTED 사유 상위. 정규화 문구 "
                          "<span class=\"mono\">" + reason[:70] + "</span>."),
            "evidence": ledger_path + " · REJECTED " + str(n) + "건",
            "impact_html": "",
        })
    for msg, n in lg.get("errors", [])[:3]:
        if n < 3:
            continue
        out.append({
            "sev": "로그",
            "when": ymd[5:] + " 종일",
            "title": "ERROR/WARN " + str(n) + "건 — " + msg[:48],
            "desc_html": "로그 상위 경고. <span class=\"mono\">" + msg[:80] + "</span>.",
            "evidence": (pack.get("log_path") or "quant_trader.log").replace("\\", "/") + " · " + str(n) + "건",
            "impact_html": "",
        })
    gap = led.get("longest_order_gap")
    if gap and gap["minutes"] >= 45:
        out.append({
            "sev": "공백",
            "when": gap["from"][11:16] + "~" + gap["to"][11:16],
            "title": "주문 이벤트 공백 " + str(gap["minutes"]) + "분",
            "desc_html": ("접수·체결·거부가 한 건도 없는 최장 구간. "
                          "신호 고갈인지 배관 정지인지는 로그로 갈라야 한다."),
            "evidence": ledger_path,
            "impact_html": "",
        })
    return out


def build_entry(pack: dict) -> dict:
    ymd = pack["date"]
    led, lg = pack["ledger"], pack.get("log", {})
    ev = led.get("events", {})
    fills = ev.get("FILL", 0)
    acc = ev.get("ACCEPTED", 0)
    rej = ev.get("REJECTED", 0)
    sessions = len(lg.get("sessions", []))
    track = lg.get("pnl_track", [])
    close = track[-1]["pnl"] if track else None
    # 전략 id는 종목별로 갈라진다(DEVSCALE_042500 …). 리뷰 라벨은 계열로 묶는다.
    fam = Counter()
    for sid, n in led.get("by_strategy", []):
        fam[re.sub(r"_[0-9]{4,}$", "", sid or "-")] += n
    strat = " · ".join(k for k, _ in fam.most_common(2)) if fam else "-"
    realized = led.get("realized_gross", 0)
    trips = led.get("roundtrips", [])
    wins = sum(1 for t in trips if t["gross"] > 0)

    head = ymd[5:] + " 세션 정리 — "
    head += ("종료 " + signed(close) + "원") if close is not None else "손익 기록 없음"

    verdict = ("원장 " + str(led.get("rows", 0)) + "이벤트(접수 " + str(acc)
               + " · 체결 " + str(fills) + " · 거부 " + str(rej) + "), 재기동 "
               + str(sessions) + "회. ")
    if trips:
        verdict += ("당일 라운드트립 " + str(len(trips)) + "건 중 이익 " + str(wins)
                    + "건, 실현 총액 " + signed(realized) + "원(수수료·세금 제외). ")
    if close is not None:
        verdict += "잔고 대조 기준 종료 당일손익은 " + signed(close) + "원이다. "
    verdict += "여기까지는 원장·로그에서 기계로 뽑은 사실이고, 원인과 다음 조치는 아래 항목에 사람이 적는다."

    logname = Path(pack.get("log_path") or "quant_trader.log").name
    meta = ["근거 로그 <span class=\"mono\">" + logname + "</span> · 원장 "
            "<span class=\"mono\">" + str(led.get("rows", 0)) + "</span>이벤트",
            "재기동 <span class=\"mono\">" + str(sessions) + "</span>회 · 접수 "
            "<span class=\"mono\">" + str(acc) + "</span> / 체결 "
            "<span class=\"mono\">" + str(fills) + "</span> / 거부 "
            "<span class=\"mono\">" + str(rej) + "</span>"]
    if led.get("by_strategy"):
        meta.append("전략별 이벤트 " + " · ".join(
            "<span class=\"mono\">" + (s or "-") + " " + str(n) + "</span>"
            for s, n in led["by_strategy"][:4]))

    entry = {
        "id": ymd + "_session_review",
        "strategy": strat or "-",
        "eyebrow": "세션 리뷰 · " + ymd[5:] + " · 원장/로그 자동 집계",
        "headline_html": head,
        "verdict_html": verdict,
        "meta_html": meta,
        "doc_path": find_journal(ymd),
        "log_path": (pack.get("log_path") or "").replace("\\", "/") or None,
        "incidents": build_incidents(pack),
    }
    pnl = build_pnl(track, ymd)
    if pnl:
        entry["pnl"] = pnl
    return {k: v for k, v in entry.items() if v not in (None, [], "")}


def merge(existing: dict, fresh: dict) -> dict:
    """기존 항목의 해석 키는 보존하고 사실 키만 덮는다.

    항목에 "locked": [...] 가 있으면 그 키는 자동 갱신에서 제외한다.
    손으로 고쳐 쓴 문장을 야간 자동 실행이 되돌리는 사고를 막는 장치다.
    """
    locked = set(existing.get("locked") or ())
    out = dict(existing)
    for k in AUTO_KEYS:
        if k in locked or k not in fresh:
            continue
        out[k] = fresh[k]

    # incidents는 목록 전체가 자동 생성이지만 impact_html("그래서 어떻게 됐나")만은 사람이 쓴다.
    # 통째로 갈아 끼우면 그 문장이 매 실행마다 지워지므로 제목이 같은 항목의 것을 옮겨 온다.
    if "incidents" in out and "incidents" in fresh:
        kept = {i.get("title"): i.get("impact_html") for i in existing.get("incidents", [])}
        for inc in out["incidents"]:
            prior = kept.get(inc.get("title"))
            if prior and not inc.get("impact_html"):
                inc["impact_html"] = prior
    return out


def _write_atomic(path: Path, text: str) -> None:
    """임시 파일에 쓰고 os.replace로 바꿔 끼운다 — 도중에 죽어도 손글씨가 든 원본은 남는다."""
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="YYYY-MM-DD (기본: 오늘)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    ymd = a.date or _date.today().isoformat()
    pack = eod_collect.build(ymd.replace("-", ""))
    fresh = build_entry(pack)

    if not REVIEWS.exists():
        # 사람이 쓴 해석이 같이 든 파일이다. 없다고 빈 파일을 새로 만들면 경로 착오를 덮는다.
        print("[build_review_entry] reviews.json 없음: " + str(REVIEWS), file=sys.stderr)
        return 1
    doc = json.loads(REVIEWS.read_text(encoding="utf-8"))
    reviews = doc.get("reviews", [])
    idx = next((i for i, r in enumerate(reviews) if r.get("id") == fresh["id"]), None)
    if idx is None:
        reviews.insert(0, fresh)          # 최신이 위 — 렌더 순서가 배열 순서다
        action = "추가"
    else:
        reviews[idx] = merge(reviews[idx], fresh)
        action = "갱신(해석 키 보존)"
    doc["reviews"] = reviews

    if a.dry_run:
        print(json.dumps(fresh, ensure_ascii=False, indent=1))
        print("[build_review_entry] (dry) " + fresh["id"] + " " + action)
        return 0

    _write_atomic(REVIEWS, json.dumps(doc, ensure_ascii=False, indent=1) + "\n")
    print("[build_review_entry] " + fresh["id"] + " " + action
          + " · 인시던트 " + str(len(fresh.get("incidents", [])))
          + "건 · 손익점 " + str(len(fresh.get("pnl", {}).get("series", []))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
