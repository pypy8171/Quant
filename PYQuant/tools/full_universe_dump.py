"""KRX 상장 보통주 전종목 코드 덤프 → Quant/config/universe_full.json.

bench_market_firehose(전종목 규모 시세 파이프라인 부하테스트)가 실제 상장 종목 수
규모로 팬아웃을 걸 수 있도록, data.go.kr getStockPriceInfo 전 시장 스냅샷에서
**필터 없이 전 종목 코드**를 뽑아 코드 배열로 저장한다.

universe_feed.py(시총∪거래대금 top-N 큐레이션)와 같은 DataGoKrSource._snapshot을
쓰되 top-N 필터를 걸지 않는다는 점만 다르다. 지연 측정에는 실제 종목명이 아니라
"종목 수 + 종목별 tick rate 분포"가 지배적이므로, 코드 문자열만 있으면 충분하다.

주의(정직 경계): getStockPriceInfo는 ETF/ETN을 구조적으로 서빙하지 않는다
  → 여기서 "전종목"은 상장 **보통주** 전종목(코스피+코스닥, ~2,600)이다.
  (universe_feed.py 헤더 및 [[data-source-constraints]] 참조)

실행:
  set PYTHONIOENCODING=utf-8
  py PYQuant/tools/full_universe_dump.py [--date YYYY-MM-DD] [--out PATH]

키: 환경변수 DATA_GO_KR_KEY (없으면 안내 후 종료 — 하네스는 합성 폴백으로 실행 가능).
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import date as _date, timedelta
from pathlib import Path

# PYQuant 루트를 path에 올려 data.datagokr_source 임포트 가능하게 (universe_feed.py 관례).
_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from data.datagokr_source import DataGoKrSource  # noqa: E402

_REPO_ROOT = _PYQUANT_ROOT.parent
_OUT_PATH = _REPO_ROOT / "Quant" / "config" / "universe_full.json"


def _yesterday_iso() -> str:
    """T-1 근사(당일 데이터는 익일 13시 갱신). 실제 서빙일은 _snapshot의 7일 백오프가 결정."""
    return (_date.today() - timedelta(days=1)).isoformat()


def build(on_date: str) -> dict | None:
    src = DataGoKrSource(market="ALL")
    if not src.authenticate():
        print("[full_universe_dump] DATA_GO_KR_KEY 미설정 — 전종목 덤프 불가.")
        print("  → 키 없이도 bench_market_firehose는 --tickers N 합성 폴백으로 실행됩니다.")
        return None

    rows = src._snapshot(on_date)   # 전 시장 전종목(보통주). 필터 없음.
    if not rows:
        print(f"[full_universe_dump] {on_date} 스냅샷 비어있음(7일 백오프 소진).")
        return None

    # 실제 서빙된 기준일 (universe_feed.py와 동일 로직).
    req_ymd = on_date.replace("-", "")
    cached = sorted(p.stem.split("_", 1)[1] for p in src._cache.glob("univ_*.parquet"))
    served = max((d for d in cached if len(d) == 8 and d <= req_ymd), default=req_ymd)

    seen: set[str] = set()
    codes: list[str] = []
    by_market: dict[str, int] = {}
    for r in rows:
        code = r.get("code", "")
        if not code or len(code) != 6 or code in seen:
            continue
        seen.add(code)
        codes.append(code)
        mk = r.get("market", "") or "UNKNOWN"
        by_market[mk] = by_market.get(mk, 0) + 1

    if not codes:
        print("[full_universe_dump] 유효 코드 0개.")
        return None

    codes.sort()
    breakdown = " ".join(f"{mk}={n}" for mk, n in sorted(by_market.items()))
    print(f"[full_universe_dump] 기준일 {served}: 상장 보통주 전종목 {len(codes)}개 ({breakdown}). "
          f"ETF/ETN 미포함.")
    return {
        "schema":    1,
        "source":    "data.go.kr:getStockPriceInfo (보통주 전종목, ETF/ETN 제외)",
        "basDt":     served,
        "requested_date": on_date,
        "count":     len(codes),
        "by_market": by_market,
        "codes":     codes,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="KRX 상장 보통주 전종목 코드 덤프")
    ap.add_argument("--date", default=None, help="기준일 YYYY-MM-DD (기본 T-1, 백오프 자동)")
    ap.add_argument("--out", default=str(_OUT_PATH), help="출력 JSON 경로")
    args = ap.parse_args()

    doc = build(args.date or _yesterday_iso())
    if doc is None:
        return 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[full_universe_dump] 기록 완료 → {out} ({doc['count']}종목)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
