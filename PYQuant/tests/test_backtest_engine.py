"""
BacktestEngine 전략-불가지 리팩토링 회귀 테스트 (KIS 불필요, 합성 데이터).

검증:
- 엔진이 전략 로직을 하드코딩하지 않고 strategy.on_start()로 스크리닝한다.
- as-of 어댑터가 start_date 이전 데이터만 노출해 look-ahead를 차단한다.
- 신호가 "발생 봉의 다음 봉 시가"로 체결된다.
- on_rebalance no-op 전략(value_contrary)이 정상 동작한다.

실행: python PYQuant/tests/test_backtest_engine.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/

from kis.client import Bar
from strategy.value_contrary import ValueContraryStrategy
from backtest.engine import BacktestEngine


def bar(date, close, open_=None, vol=1000):
    o = open_ if open_ is not None else close
    return Bar(date=date, open=o, high=max(o, close), low=min(o, close), close=close, volume=vol)


# 시나리오: AAA는 start 직전 4봉 3일연속 하락(후보), BBB는 상승(비후보)
data = {
    "AAA": [
        bar("2024-01-02", 120), bar("2024-01-03", 115),
        bar("2024-01-04", 110), bar("2024-01-05", 105),   # 120>115>110>105 하락 → 후보
        bar("2024-01-08", 103, open_=104),                 # sim day1 → BUY 신호
        bar("2024-01-09", 100, open_=102),                 # BUY 체결 @102 (다음봉 시가)
        bar("2024-01-10", 101, open_=101),                 # SELL 체결 @101
        bar("2024-01-11", 102, open_=101),
        bar("2024-01-12", 103, open_=102),
    ],
    "BBB": [
        bar("2024-01-02", 100), bar("2024-01-03", 105),
        bar("2024-01-04", 110), bar("2024-01-05", 115),   # 상승 → 비후보
        bar("2024-01-08", 116), bar("2024-01-09", 117),
        bar("2024-01-10", 118), bar("2024-01-11", 119), bar("2024-01-12", 120),
    ],
}


class FakeKis:
    def __init__(self, d): self._d = d
    def authenticate(self): return True
    def get_historical_ohlcv(self, ticker, start, end):
        return [b for b in self._d.get(ticker, []) if start <= b.date <= end]


def test_value_contrary():
    engine = BacktestEngine(FakeKis(data), ValueContraryStrategy(quantity=1),
                            initial_cash=1_000_000)
    result = engine.run(["AAA", "BBB"], start_date="2024-01-08", end_date="2024-01-12")

    trades = result.trades
    buys  = [t for t in trades if t.side == "BUY"]
    sells = [t for t in trades if t.side == "SELL"]
    print("TRADES:", [(t.ticker, t.side, t.date, round(t.price, 1)) for t in trades])

    assert all(t.ticker == "AAA" for t in trades), f"비후보 BBB가 거래됨: {trades}"
    assert len(buys) == 1, f"BUY 1건 기대: {buys}"
    assert abs(buys[0].price - 102) < 1e-6, f"BUY 다음봉 시가 102 기대: {buys[0].price}"
    assert len(sells) == 1, f"SELL 1건 기대: {sells}"
    assert abs(sells[0].price - 101) < 1e-6, f"SELL 다음봉 시가 101 기대: {sells[0].price}"
    assert result.trade_count == 1, f"trade_count(SELL) 1 기대: {result.trade_count}"
    print("PASS(value_contrary): on_start(as-of) 스크리닝 + 다음봉 시가 체결 정확")


# ── 수급 랭킹 + TARGET_WEIGHT 사이징 + look-ahead 검증 ──────────────────────
def test_supply_demand_rank():
    from data.krx_source import Flow
    from strategy.supply_demand_rank import SupplyDemandRankStrategy

    dates = ["2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08", "2024-01-09"]
    bars  = {t: [bar(d, 100, open_=100) for d in dates] for t in ("AAA", "BBB", "CCC")}
    # 수급 score: AAA>BBB>0>CCC → top2 = {AAA, BBB}, CCC(음수) 제외
    flows = {
        "AAA": [Flow(d, 100, 100, 1e9) for d in dates],
        "BBB": [Flow(d,  50,  50, 1e9) for d in dates],
        "CCC": [Flow(d, -100, -50, 1e9) for d in dates],
    }

    class FakeKrx:
        def __init__(self, b, f): self._b, self._f = b, f
        def authenticate(self): return True
        def get_historical_ohlcv(self, t, s, e): return [x for x in self._b.get(t, []) if s <= x.date <= e]
        def flow_history(self, t, s, e):          return [x for x in self._f.get(t, []) if s <= x.date <= e]

    strat = SupplyDemandRankStrategy(top_n=2, rebalance_every=1, flow_window=2)

    # look-ahead 캡처: 리밸런싱 date에서 전략이 본 flow.date 최댓값 < date 인지
    seen = {}
    orig = strat.on_rebalance
    def wrapped(date, visible, flow=None):
        for fl in (flow or {}).values():
            if fl:
                seen[date] = max(seen.get(date, "0"), max(f.date for f in fl))
        return orig(date, visible, flow)
    strat.on_rebalance = wrapped

    engine = BacktestEngine(FakeKrx(bars, flows), strat, initial_cash=1_000_000, target_positions=2)
    result = engine.run(["AAA", "BBB", "CCC"], start_date="2024-01-02", end_date="2024-01-09")

    for d, maxf in seen.items():
        assert maxf < d, f"look-ahead! 리밸런싱 {d}에 flow.date {maxf}(당일/미래) 봄"

    buys   = [t for t in result.trades if t.side == "BUY"]
    bought = {t.ticker for t in buys}
    assert "CCC" not in bought, f"수급 음수 CCC 매수됨: {bought}"
    # C-1/C-2 수정: 엔진 reconcile로 top2 둘 다 체결돼야(예전엔 현금부족으로 1개만 됨)
    assert bought == {"AAA", "BBB"}, f"top2(AAA,BBB) 둘 다 체결 기대: {bought}"
    # 동일가중 ~equity/N/price(=5000)에서 비용 반영분만 차감 → 4900~5000
    assert all(4900 <= t.quantity <= 5000 for t in buys), \
        f"동일가중 사이징 ~5000주(비용반영) 기대: {[t.quantity for t in buys]}"
    print(f"PASS(수급랭킹): 매수 {bought}, 사이징 {sorted(t.quantity for t in buys)}, look-ahead 없음")


if __name__ == "__main__":
    test_value_contrary()
    test_supply_demand_rank()
