"""
백테스팅 결과 출력
"""
from backtest.engine import BacktestResult


def print_report(result: BacktestResult):
    print(f"\n{'='*60}")
    print("  백테스팅 결과")
    print('='*60)
    print(f"  총 거래수  : {result.trade_count}회")
    print(f"  총 손익    : {result.total_pnl:+,.0f}원")
    print(f"  총 수익률  : {result.total_return:+.2f}%")
    print(f"  최대낙폭   : -{result.mdd:.2f}%")
    print(f"  샤프지수   : {result.sharpe:.2f}")
    print(f"  승률       : {result.win_rate:.1f}%")
    print('='*60)

    if not result.trades:
        print("  거래 없음")
        return

    print("\n  ── 거래 내역 ──────────────────────────────────────")
    print(f"  {'날짜':<12} {'종목':<8} {'구분':<5} {'가격':>9} {'수량':>4} {'손익':>10}")
    print(f"  {'-'*56}")
    sells = [t for t in result.trades if t.side == "SELL"]
    for t in sells[-20:]:   # 최근 20건만
        pnl_str = f"{t.pnl:+,.0f}" if t.pnl != 0 else "-"
        print(f"  {t.date:<12} {t.ticker:<8} {'매도':<5} "
              f"{t.price:>9,.0f} {t.quantity:>4} {pnl_str:>10}")

    wins  = [t for t in sells if t.pnl > 0]
    loses = [t for t in sells if t.pnl <= 0]
    if wins:
        print(f"\n  평균 수익 거래: +{sum(t.pnl for t in wins)/len(wins):,.0f}원")
    if loses:
        print(f"  평균 손실 거래:  {sum(t.pnl for t in loses)/len(loses):,.0f}원")
