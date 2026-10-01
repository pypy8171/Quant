"""스터디 30-A: 강한 종목 VWAP 눌림 백테스트에 필요한 (종목,날짜) 1분봉 쌍을 고르고, 이미 있는 것과 빠진 것을 나눈다.

상위집합 규칙(미래 참조 없음 — 받을 대상만 넓게 고르고, 최종 선정은 09:30 분봉으로만 한다):
  그날 고가 / 전일 종가 >= 1.06  (09:30 가격 <= 그날 고가이므로 09:30에 +6% 이상이던 종목은 전부 들어온다)
  그날 저가 / 전일 종가 <= 1.20  (하루 내내 +20% 위였던 종목은 09:30 조건 +6~20%에 들 수 없다)
일봉은 PYQuant/data/bars_all_pit_v2.parquet(네이버 siseJson, 수정주가, 상폐 포함)을 쓴다. 비율이라 수정주가여도 같다.

산출:
  data/pairs_all.json      {YYYYMMDD: [code...]}  상위집합 전체
  data/pairs_missing.json  {YYYYMMDD: [code...]}  PYQuant/data/minute 과 data/minute 어디에도 없는 쌍(받을 목록)
  data/pairs_summary.json  날짜·쌍 수·보유율

실행(저장소 루트): py research/studies/30_strong_stock_strategies/build_minute_pairs.py --start 2025-09-01 --end 2026-09-30
"""
import argparse
import json
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
BARS = ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
SHARED_MINUTE = ROOT / "PYQuant" / "data" / "minute"
STUDY_MINUTE = STUDY / "data" / "minute"
OUT = STUDY / "data"


def already_fetched(code: str, trade_date: str) -> bool:
    """공용 분봉 폴더나 스터디 분봉 폴더에 그 (종목,날짜) 파일이 있는지."""
    return (SHARED_MINUTE / code / f"{trade_date}.parquet").exists() or (STUDY_MINUTE / code / f"{trade_date}.parquet").exists()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--start", default="2025-09-01")
    parser.add_argument("--end", default="2026-09-30")
    parser.add_argument("--min-gain", type=float, default=0.06)
    parser.add_argument("--max-gain", type=float, default=0.20)
    arguments = parser.parse_args()

    bars = pd.read_parquet(BARS, columns=["Date", "code", "High", "Low", "Close", "Volume"])
    bars = bars.sort_values(["code", "Date"])
    bars["prev_close"] = bars.groupby("code")["Close"].shift(1)
    window = bars[(bars.Date >= arguments.start) & (bars.Date <= arguments.end)
                  & (bars.Volume > 0) & (bars.prev_close > 0)]
    hit = window[(window.High / window.prev_close >= 1 + arguments.min_gain)
                 & (window.Low / window.prev_close <= 1 + arguments.max_gain)]

    pairs_all, pairs_missing, per_day = {}, {}, []
    for date, group in hit.groupby("Date"):
        trade_date = date.strftime("%Y%m%d")
        codes = sorted(group.code.tolist())
        missing = [code for code in codes if not already_fetched(code, trade_date)]
        pairs_all[trade_date] = codes
        if missing:
            pairs_missing[trade_date] = missing
        per_day.append({"ymd": trade_date, "pairs": len(codes), "missing": len(missing)})

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "pairs_all.json").write_text(json.dumps(pairs_all), encoding="utf-8")
    (OUT / "pairs_missing.json").write_text(json.dumps(pairs_missing), encoding="utf-8")
    total = sum(len(day_codes) for day_codes in pairs_all.values())
    missing_total = sum(len(day_codes) for day_codes in pairs_missing.values())
    trading_days = window.Date.nunique()
    summary = {"start": arguments.start, "end": arguments.end, "bars_last_date": str(bars.Date.max().date()),
               "trading_days": int(trading_days), "days_with_pairs": len(pairs_all),
               "pairs": total, "pairs_per_day_mean": round(total / max(len(pairs_all), 1), 1),
               "have": total - missing_total, "missing": missing_total,
               "unique_codes": int(hit.code.nunique()), "per_day": per_day}
    (OUT / "pairs_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print({key: value for key, value in summary.items() if key != "per_day"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
