"""data.go.kr 시총∪거래대금 top-N 유니버스 피드 → Quant/config/universe_scan.json.

C++ DeviationScale 스캐너의 4번째 후보 축(ETF-free·KIS 30행캡 우회)을 채운다.
장 전 하루 1회 실행 권장. T-1 스냅샷 기반(당일치는 익일 13시 갱신)이나 유니버스
선정엔 무관 — 시총·거래대금 랭킹은 일 단위로 안정적이고, 정배열도 어차피 일봉 기준.

핵심 이점(data-sourcer 실측 확인):
  • getStockPriceInfo(금융위 주식시세정보)는 ETF/ETN을 구조적으로 서빙 안 함
    → KIS 랭킹의 장중 ETF 잠식(드롭 9→17)을 원천 차단.
  • 스냅샷 1콜에 시총(mrktTotAmt)·거래대금(trPrc)·종가(clpr)가 다 들어있어
    시총 top-N ∪ 거래대금 top-N union이 추가 API 비용 0.
    (시총 단독은 현대해상·GS건설·한국콜마 같은 중형 트렌딩주를 놓쳐서 union 필수.)

실행:
  set PYTHONIOENCODING=utf-8
  python PYQuant/tools/universe_feed.py [--n-mktcap 100] [--n-turnover 100] [--date YYYY-MM-DD]
키: 환경변수 DATA_GO_KR_KEY (없으면 즉시 에러). [[data-source-constraints]] 참조.
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import date as _date, timedelta
from pathlib import Path

# PYQuant 루트를 path에 올려 data.datagokr_source / kis.client 임포트 가능하게.
_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from data.datagokr_source import DataGoKrSource  # noqa: E402

# 출력 경로: 레포 루트/Quant/config/universe_scan.json (C++ config의 universe_file과 일치).
_REPO_ROOT = _PYQUANT_ROOT.parent
_OUT_PATH = _REPO_ROOT / "Quant" / "config" / "universe_scan.json"


def _yesterday_iso() -> str:
    """T-1(전영업일 근사). 실제 서빙일은 _snapshot의 7일 백오프가 결정하므로 근사면 충분."""
    d = _date.today() - timedelta(days=1)
    return d.isoformat()


def build(on_date: str, n_mktcap: int, n_turnover: int,
          min_turnover: float = 1e9, market: str = "KOSPI") -> dict | None:
    src = DataGoKrSource(market=market)
    if not src.authenticate():
        print("[universe_feed] DATA_GO_KR_KEY 미설정 — 유니버스 생성 불가.")
        return None

    rows = src._snapshot(on_date)   # 전 시장 스냅샷(휴장일 7일 백오프 내장)
    if not rows:
        print(f"[universe_feed] {on_date} 스냅샷 비어있음(7일 백오프 소진).")
        return None

    # 실제 서빙된 기준일 — _snapshot이 캐시로 쓴 univ_YYYYMMDD.parquet 중 요청일 이하 최신.
    req_ymd = on_date.replace("-", "")
    cached = sorted(p.stem.split("_", 1)[1] for p in src._cache.glob("univ_*.parquet"))
    served = max((d for d in cached if len(d) == 8 and d <= req_ymd), default=req_ymd)

    pool = [r for r in rows
            if r.get("market") == market
            and r.get("turnover", 0.0) >= min_turnover
            and r.get("mktcap", 0.0) > 0.0]
    if not pool:
        print(f"[universe_feed] {market} 유효 종목 0개(turnover>={min_turnover:.0f}).")
        return None

    by_cap = sorted(pool, key=lambda r: r["mktcap"],   reverse=True)[:n_mktcap]
    by_val = sorted(pool, key=lambda r: r["turnover"], reverse=True)[:n_turnover]

    # union — 시총순 먼저(프로브 우선), 이어 거래대금 상위 중 미포함 중형주. code 중복 제거.
    seen: set[str] = set()
    universe: list[dict] = []
    for r in by_cap + by_val:
        code = r["code"]
        if not code or code in seen:
            continue
        seen.add(code)
        universe.append({
            "ticker": code,
            "name":   r.get("name", ""),
            "close":  round(r.get("close", 0.0)),
        })

    n_val_extra = len(universe) - len(by_cap)
    print(f"[universe_feed] 기준일 {served}: 시총 top{len(by_cap)} ∪ 거래대금 top{n_turnover} "
          f"= {len(universe)}종목 (거래대금 추가 {n_val_extra}). ETF-free.")
    return {
        "schema":   1,
        "source":   "data.go.kr:getStockPriceInfo",
        "market":   market,
        "basDt":    served,
        "requested_date": on_date,
        "count":    len(universe),
        "universe": universe,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="data.go.kr 시총∪거래대금 유니버스 피드")
    ap.add_argument("--n-mktcap",   type=int, default=100, help="시총 상위 N")
    ap.add_argument("--n-turnover", type=int, default=100, help="거래대금 상위 N")
    ap.add_argument("--date", default=None, help="기준일 YYYY-MM-DD (기본 T-1, 백오프 자동)")
    ap.add_argument("--min-turnover", type=float, default=1e9, help="최소 거래대금(원)")
    ap.add_argument("--out", default=str(_OUT_PATH), help="출력 JSON 경로")
    args = ap.parse_args()

    on_date = args.date or _yesterday_iso()
    doc = build(on_date, args.n_mktcap, args.n_turnover, args.min_turnover)
    if doc is None:
        return 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[universe_feed] 기록 완료 → {out} ({doc['count']}종목)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
