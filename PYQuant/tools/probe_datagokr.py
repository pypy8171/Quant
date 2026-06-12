"""
data.go.kr 금융위 주식시세정보 소스 검증 프로브.

목적: DataGoKrSource가 (1) 인증, (2) per-ticker OHLCV, (3) point-in-time
유니버스(과거 시총 상위) 를 제대로 받는지 한 번에 확인. 첫 응답의 원본 필드명도
출력해 스펙이 코드 추정과 일치하는지 검증한다.

사용 (키는 환경변수로 — 채팅/코드에 박지 말 것):
  WSL/Linux:   export DATA_GO_KR_KEY='발급받은_일반인증키'
               python3 PYQuant/tools/probe_datagokr.py
  Windows PS:  $env:DATA_GO_KR_KEY='발급받은_일반인증키'
               python PYQuant/tools/probe_datagokr.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 패키지 루트

from data.datagokr_source import DataGoKrSource


def main() -> None:
    src = DataGoKrSource(market="KOSPI")
    print("=" * 60)
    print("[1] 인증 확인")
    if not src.authenticate():
        print("  → DATA_GO_KR_KEY 환경변수 설정 필요. 중단.")
        return
    print("  OK (키 로드됨)")

    print("\n[2] per-ticker OHLCV — 삼성전자(005930) 2024-01-02~2024-01-31")
    bars = src.get_historical_ohlcv("005930", "2024-01-02", "2024-01-31")
    print(f"  받은 봉 수: {len(bars)}")
    for b in bars[:3]:
        print(f"   {b.date}  O{b.open:,.0f} H{b.high:,.0f} L{b.low:,.0f} C{b.close:,.0f} V{b.volume:,}")
    if bars:
        print(f"   ... 마지막: {bars[-1].date} C{bars[-1].close:,.0f}")

    print("\n[3] point-in-time 유니버스 — 2023-01-02 KOSPI 시총 상위 10")
    top = src.universe_top("2023-01-02", 10)
    print(f"  받은 종목 수: {len(top)}")
    for i, code in enumerate(top, 1):
        print(f"   {i:>2}. {code}  {src.ticker_name(code)}")

    print("\n" + "=" * 60)
    if bars and top:
        print("✔ 전부 정상 — DataGoKrSource 사용 가능. main.py --source datagokr 배선 가능.")
    else:
        print("⚠ 일부 빈 결과 — 위 '(스펙확인) 첫 응답 필드' 줄을 붙여주면 필드명 보정.")


if __name__ == "__main__":
    main()
