"""pykrx 런타임 확인 — 수급 백테스트 데이터 가용성/컬럼명 검증."""
from pykrx import stock

print("=== 1) OHLCV (삼성전자 005930, 2024-01-02~05) ===")
o = stock.get_market_ohlcv("20240102", "20240105", "005930")
print("columns:", o.columns.tolist())
print(o.head(2))

print("\n=== 2) 투자자별 순매수 금액 (by_date) ===")
v = stock.get_market_trading_value_by_date("20240102", "20240108", "005930")
print("columns:", v.columns.tolist())
print(v.head(3))

print("\n=== 3) 투자자별 순매수 수량 (by_date) ===")
q = stock.get_market_trading_volume_by_date("20240102", "20240108", "005930")
print("columns:", q.columns.tolist())

print("\n=== 4) 시총 유니버스 (KOSPI, 2024-01-05) ===")
cap = stock.get_market_cap_by_ticker("20240105", market="KOSPI")
print("columns:", cap.columns.tolist())
print("종목수:", len(cap))
print(cap.sort_values("시가총액", ascending=False).head(3)[["시가총액", "거래대금"]])

print("\n=== 5) KOSPI 지수 (벤치마크, 1001) ===")
idx = stock.get_index_ohlcv("20240102", "20240105", "1001")
print("columns:", idx.columns.tolist())

print("\nPASS: pykrx 데이터 가용")
