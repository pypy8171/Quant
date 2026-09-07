#!/usr/bin/env python3
"""거래일별 point-in-time 유니버스 재구성 → PYQuant/data/pit_universe/YYYYMMDD.json

왜 필요한가(docs/DECISIONS.md D-008):
  오늘의 거래대금 상위 종목으로 1년 전 장을 리플레이하면 "1년 전에도 그 종목이 상위였다"는
  미래 정보를 쓴다. 리플레이 결과 전체가 무효가 된다. 날짜 D의 장을 재현하려면 D 시작 전에
  실제로 알 수 있었던 유니버스를 써야 한다.

선정 규칙을 여기서 새로 쓰지 않는다. 라이브가 쓰는 universe_feed.build()를 날짜만 바꿔 호출한다.
  규칙이 두 벌이 되면 리플레이와 라이브가 조용히 갈린다.

PIT 보증:
  • build(D-1)을 부르고, data.go.kr 스냅샷의 휴장일 백오프는 과거로만 간다.
  • 실제 서빙일(basDt)이 D 이상이면 미래 정보다 → 그 날짜는 기록하지 않고 실패로 센다.
  • data.go.kr은 공시 자체가 1~3영업일 늦는다(09-06 요청 → 09-03 서빙 실측).
    라이브도 같은 지연을 겪으므로 이 지연은 재현 대상이지 결함이 아니다.

사용:
  py PYQuant/tools/pit_universe_backfill.py --start 2025-09-01 --end 2026-09-05
  py PYQuant/tools/pit_universe_backfill.py --start ... --end ... --market KOSPI --force
"""
import argparse
import json
import sys
from datetime import date as _date, timedelta
from pathlib import Path

_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from tools.universe_feed import build            # noqa: E402  라이브와 같은 선정 규칙
from kis.client import from_config               # noqa: E402

_OUT_DIR = _PYQUANT_ROOT / "data" / "pit_universe"
_CAL_TICKER = "005930"     # 거래일 달력 대용(상장폐지·거래정지 이력 없는 종목)


def trading_days(cfg_path: str, start: str, end: str) -> list[str]:
    """거래일 목록(YYYYMMDD). 삼성전자 일봉이 존재하는 날 = 장이 열린 날."""
    c = from_config(cfg_path)
    if not c.authenticate():
        raise SystemExit("[pit_universe] KIS 인증 실패")
    bars = c.get_historical_ohlcv(_CAL_TICKER, start, end)
    # Bar.date는 ISO("2026-08-24")로 오고 이 파일은 YYYYMMDD로 다룬다.
    days = sorted({b.date.replace("-", "") for b in bars if b.date})
    lo, hi = start.replace("-", ""), end.replace("-", "")
    return [d for d in days if lo <= d <= hi]


def main() -> int:
    ap = argparse.ArgumentParser(description="거래일별 PIT 유니버스 재구성")
    ap.add_argument("--start", required=True, help="시작일 YYYY-MM-DD (포함)")
    ap.add_argument("--end",   required=True, help="종료일 YYYY-MM-DD (포함)")
    ap.add_argument("--market", default="KOSPI", choices=["KOSPI", "KOSDAQ", "ALL"])
    ap.add_argument("--n-mktcap",   type=int,   default=100)
    ap.add_argument("--n-turnover", type=int,   default=100)
    ap.add_argument("--min-turnover", type=float, default=1e9)
    ap.add_argument("--config", default="Quant/config/config_dev_paper.json",
                    help="거래일 달력용 KIS config")
    ap.add_argument("--out-dir", default=str(_OUT_DIR))
    ap.add_argument("--force", action="store_true", help="이미 있는 날짜도 다시 만든다")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    days = trading_days(args.config, args.start, args.end)
    print(f"[pit_universe] 거래일 {len(days)}일  {days[0] if days else '-'}~"
          f"{days[-1] if days else '-'}  시장={args.market}")

    made = skipped = failed = lookahead = 0
    for ymd in days:
        dst = out_dir / f"{ymd}.json"
        if dst.exists() and not args.force:
            skipped += 1
            continue

        d = _date(int(ymd[:4]), int(ymd[4:6]), int(ymd[6:8]))
        doc = build((d - timedelta(days=1)).isoformat(), args.n_mktcap,
                    args.n_turnover, args.min_turnover, args.market)
        if doc is None:
            failed += 1
            print(f"  {ymd}: 스냅샷 없음")
            continue

        # PIT 게이트 — 서빙일이 대상일 이상이면 그 날 장을 알고 고른 유니버스다.
        if doc.get("basDt", "") >= ymd:
            lookahead += 1
            print(f"  {ymd}: 거부 — basDt={doc['basDt']}가 대상일 이상(미래 정보)")
            continue

        doc["for_session"] = ymd
        doc["lag_days"] = (d - _date(int(doc["basDt"][:4]), int(doc["basDt"][4:6]),
                                     int(doc["basDt"][6:8]))).days
        dst.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
        made += 1

    print(f"[pit_universe] 생성 {made}  건너뜀 {skipped}  스냅샷없음 {failed}  "
          f"미래정보거부 {lookahead}  → {out_dir}")
    return 0 if lookahead == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
