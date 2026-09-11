"""네이버 증권 테마(인포스탁 분류) 조회 — 테마 목록·구성 종목·종목→테마 역색인. 스레드 소유 없음(호출자 스레드에서 동기 실행). D-054

증권사 앱의 '국내 테마'는 인포스탁 분류를 그대로 쓰고, 네이버 증권이 같은 표를 JSON으로 연다(키·로그인 없음).
비공식 경로라 형식이 바뀌면 끊길 수 있다 — 호출자는 마지막 성공 스냅샷(PYQuant/data/themes/latest.json)으로 버틴다.
"""
from __future__ import annotations

import json
import time
import urllib.request
from typing import Iterable

BASE = "https://m.stock.naver.com/api/stocks/theme"
# [wire] 브라우저 UA가 아니면 HTML 오류 페이지가 온다. pageSize 상한은 100(그 위는 100으로 잘린다).
_HEADERS = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/128.0", "Accept": "application/json"}
PAGE_SIZE = 100


def _get_json(url: str, timeout: float = 8.0) -> dict:
    req = urllib.request.Request(url, headers=_HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read().decode("utf-8")
    if not body.startswith("{"):
        raise RuntimeError(f"JSON이 아닌 응답: {url}")
    return json.loads(body)


def _num(s) -> float | None:
    """'15,130' → 15130.0, 'N/A'·'' → None."""
    if s is None:
        return None
    t = str(s).replace(",", "").strip()
    if not t or t == "N/A":
        return None
    try:
        return float(t)
    except ValueError:
        return None


def fetch_theme_list() -> list[dict]:
    """테마 266개(2026-09 기준)의 당일 등락률·상승/하락 종목 수. 3회 호출."""
    out: list[dict] = []
    page = 1
    while True:
        d = _get_json(f"{BASE}?page={page}&pageSize={PAGE_SIZE}")
        groups = d.get("groups") or []
        for g in groups:
            out.append({
                "no": int(g["no"]),
                "name": g.get("name", ""),
                "count": int(g.get("totalCount") or 0),
                "pct": _num(g.get("changeRate")),
                "rise": int(g.get("riseCount") or 0),
                "fall": int(g.get("fallCount") or 0),
                "steady": int(g.get("steadyCount") or 0),
            })
        total = int(d.get("totalCount") or 0)
        if not groups or len(out) >= total:
            break
        page += 1
    return out


def fetch_theme_members(no: int) -> dict:
    """테마 하나의 구성 종목(현재가·등락률·거래대금·시총)과 종목별 편입 사유. 1회 호출.
    시총 marketValue는 억, 거래대금 accumulatedTradingValue는 백만원 단위 문자열로 온다 — 둘 다 억으로 맞춘다."""
    d = _get_json(f"{BASE}/{no}?page=1&pageSize={PAGE_SIZE}")
    info = d.get("groupInfo") or {}
    reasons = d.get("themeItemInfoMap") or {}
    rows = []
    for s in d.get("stocks") or []:
        tk = s.get("itemCode") or ""
        tv = _num(s.get("accumulatedTradingValue"))
        rows.append({
            "ticker": tk,
            "name": s.get("stockName", ""),
            "market": "KOSDAQ" if str(s.get("sosok")) == "1" else "KOSPI",
            "price": _num(s.get("closePrice")),
            "change_rate": _num(s.get("fluctuationsRatio")),
            "value": None if tv is None else round(tv / 100, 1),  # 억(원본은 백만원)
            "market_cap": _num(s.get("marketValue")),          # 억
            "reason": reasons.get(tk, ""),
        })
    rows.sort(key=lambda r: (r["change_rate"] is None, -(r["change_rate"] or 0)))
    return {
        "no": int(info.get("no") or no),
        "name": info.get("name", ""),
        "pct": _num(info.get("changeRate")),
        "rise": int(info.get("riseCount") or 0),
        "fall": int(info.get("fallCount") or 0),
        "description": d.get("themeDescription", ""),
        "rows": rows,
    }


def fetch_all(sleep: float = 0.08, nos: Iterable[int] | None = None, on_progress=None) -> dict:
    """목록 + 전 테마 구성 종목. 약 270회 호출, 30초 안팎. on_progress(i, n, name)로 진행을 알린다."""
    themes = fetch_theme_list()
    if nos is not None:
        want = set(int(n) for n in nos)
        themes = [t for t in themes if t["no"] in want]
    n = len(themes)
    for i, t in enumerate(themes, 1):
        m = fetch_theme_members(t["no"])
        t["description"] = m["description"]
        t["members"] = m["rows"]
        if on_progress:
            on_progress(i, n, t["name"])
        time.sleep(sleep)
    return {"asof": time.strftime("%Y-%m-%d %H:%M:%S"), "source": BASE, "themes": themes,
            "by_ticker": build_index(themes)}


def build_index(themes: list[dict]) -> dict[str, list[dict]]:
    """종목 → [{no, name, reason}]. 한 종목이 여러 테마에 걸리는 것이 보통이다(보안주 51종목 등)."""
    idx: dict[str, list[dict]] = {}
    for t in themes:
        for m in t.get("members") or []:
            idx.setdefault(m["ticker"], []).append({"no": t["no"], "name": t["name"], "reason": m.get("reason", "")})
    return idx
