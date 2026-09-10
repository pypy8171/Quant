# -*- coding: utf-8 -*-
"""18종 + 지수 일봉 수집. 이 환경은 yfinance 불가, KRX StockListing 404 -> 코드는 고정."""
import os, json, sys
import FinanceDataReader as fdr

sys.stdout.reconfigure(encoding="utf-8")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "raw")
os.makedirs(OUT, exist_ok=True)
START, END = "2024-09-01", "2026-09-09"

CODES = {
    "대한전선": "001440", "두산퓨얼셀": "336260", "범한퓨얼셀": "382900",
    "우리기술": "032820", "가온전선": "000500", "한화엔진": "082740",
    "HPSP": "403870", "DB하이텍": "000990", "테스": "095610",
    "로보티즈": "108490", "두산에너빌리티": "034020", "한전기술": "052690",
    "GS건설": "006360", "대우건설": "047040", "현대건설": "000720",
    "SK하이닉스": "000660", "삼성전기": "009150", "삼화콘덴서": "001820",
}

json.dump(CODES, open(os.path.join(OUT, "codes.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=1)

for name, code in CODES.items():
    df = fdr.DataReader(code, START, END)
    df.to_csv(os.path.join(OUT, f"{code}.csv"), encoding="utf-8")
    print(f"{name:10s} {code} rows={len(df):4d} last={df.index[-1].date()} close={df['Close'].iloc[-1]:,.0f}")

for idx in ["KS11", "KQ11"]:
    df = fdr.DataReader(idx, START, END)
    df.to_csv(os.path.join(OUT, f"{idx}.csv"), encoding="utf-8")
    print(f"{idx} rows={len(df)}")
