"""
백테스팅 결과 출력
"""
from backtest.engine import BacktestResult


def print_report(result: BacktestResult, names: dict | None = None):
    names = names or {}
    print(f"\n{'='*60}")
    print("  백테스팅 결과")
    print('='*60)
    if result.start_date and result.end_date:
        print(f"  평가기간   : {result.start_date} ~ {result.end_date} (마지막 데이터일까지 누적)")
    print(f"  총 거래수  : {result.trade_count}회")
    print(f"  총 손익    : {result.total_pnl:+,.0f}원")
    print(f"  총 수익률  : {result.total_return:+.2f}%")
    print(f"  최대낙폭   : -{result.mdd:.2f}%")
    print(f"  샤프지수   : {result.sharpe:.2f}")
    print(f"  승률       : {result.win_rate:.1f}%")
    # ── 벤치마크 대비 (알파 vs 베타 판정) ──
    print(f"  {'-'*56}")
    print(f"  [벤치마크] 등가중 buy&hold: {result.bench_return:+.2f}% "
          f"(MDD -{result.bench_mdd:.2f}%, 샤프 {result.bench_sharpe:.2f})")
    if result.kodex_return is not None:
        print(f"             KODEX200 buy&hold: {result.kodex_return:+.2f}%")
    # 위험조정까지 본 정직한 판정 — 수익률만 높고 샤프가 벤치 미달이면 "더 큰 위험의 대가"
    if result.alpha > 0 and result.sharpe > result.bench_sharpe:
        verdict = "✅ 위험조정 알파(수익↑ & 샤프↑)"
    elif result.alpha > 0:
        verdict = (f"⚠️ 수익률만 초과 — 샤프 {result.sharpe:.2f} < 벤치 {result.bench_sharpe:.2f}, "
                   f"MDD도 큼. 위험조정 엣지 아님(더 공격적 베팅의 대가)")
    else:
        verdict = "⚠️ 벤치 미달(베타/언더퍼폼)"
    print(f"  [초과수익(α)] 수익률 {result.alpha:+.2f}%p | "
          f"샤프 {result.sharpe:.2f} vs 벤치 {result.bench_sharpe:.2f}")
    print(f"  [판정] {verdict}")
    if result.regime_off:
        print(f"  [regime] 하락국면 현금화 리밸런싱: {result.regime_off}회")
    print('='*60)

    if not result.trades:
        print("  거래 없음")
        return

    print("\n  ── 거래 내역 (매수/매도) ──────────────────────────")
    print(f"  {'날짜':<12} {'종목':<8} {'구분':<5} {'체결가':>9} {'수량':>5} {'손익':>11}  종목명")
    print(f"  {'-'*66}")
    for t in result.trades[-40:]:   # 최근 40건 (매수+매도)
        side    = "매수" if t.side == "BUY" else "매도"
        pnl_str = f"{t.pnl:+,.0f}" if t.side == "SELL" else "-"
        print(f"  {t.date:<12} {t.ticker:<8} {side:<5} "
              f"{t.price:>9,.0f} {t.quantity:>5} {pnl_str:>11}  {names.get(t.ticker, '')}")
    sells = [t for t in result.trades if t.side == "SELL"]

    wins  = [t for t in sells if t.pnl > 0]
    loses = [t for t in sells if t.pnl <= 0]
    if wins:
        print(f"\n  평균 수익 거래: +{sum(t.pnl for t in wins)/len(wins):,.0f}원")
    if loses:
        print(f"  평균 손실 거래:  {sum(t.pnl for t in loses)/len(loses):,.0f}원")


def export_daily_csv(result: BacktestResult, path: str):
    """일별 상태(매일매일) CSV — 날짜·전략equity·수익률·낙폭·벤치equity·현금·보유수.
    엑셀/Grafana로 매일의 자산 추이를 본다."""
    import csv
    dates = result.equity_dates or []
    eq    = result.equity_curve or []
    bench = result.bench_curve or [None] * len(dates)
    cash  = result.daily_cash or [None] * len(dates)
    npos  = result.daily_npos or [None] * len(dates)
    if not dates:
        print("  (일별 데이터 없음 — CSV 생략)")
        return
    init = eq[0] if eq else 0
    peak = eq[0] if eq else 0
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["date", "equity", "return_pct", "drawdown_pct",
                    "bench_equity", "cash", "n_positions"])
        for i, d in enumerate(dates):
            e = eq[i]
            peak = max(peak, e)
            ret = (e - init) / init * 100 if init else 0
            dd  = (peak - e) / peak * 100 if peak else 0
            be  = bench[i] if i < len(bench) and bench[i] is not None else ""
            w.writerow([d, f"{e:.0f}", f"{ret:.2f}", f"-{dd:.2f}",
                        f"{be:.0f}" if be != "" else "",
                        f"{cash[i]:.0f}" if i < len(cash) and cash[i] is not None else "",
                        npos[i] if i < len(npos) else ""])
    print(f"  📄 일별 상태 CSV 저장: {path} ({len(dates)}일)")


def export_trades_csv(result: BacktestResult, path: str, names: dict | None = None):
    """전체 거래(매수+매도) CSV — 화면은 최근 40건만 보이므로 전량 확인용."""
    import csv
    names = names or {}
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["date", "ticker", "name", "side", "price", "quantity", "pnl"])
        for t in result.trades:
            w.writerow([t.date, t.ticker, names.get(t.ticker, ""), t.side,
                        f"{t.price:.0f}", t.quantity,
                        f"{t.pnl:.0f}" if t.side == "SELL" else ""])
    print(f"  📄 전체 거래 CSV 저장: {path} ({len(result.trades)}건)")


def export_holdings_csv(result: BacktestResult, path: str, names: dict | None = None):
    """일별 보유종목 상세(long format) — 날짜마다 어떤 종목을 얼마나 들고 있었는지.
    한 행 = (날짜, 종목, 수량, 평가액, 비중%). 엑셀 피벗으로 날짜별 보유 확인."""
    import csv
    names = names or {}
    dates = result.equity_dates or []
    holds = result.daily_holdings or []
    eq    = result.equity_curve or []
    cash  = result.daily_cash or []
    if not dates:
        print("  (일별 보유 데이터 없음 — CSV 생략)")
        return
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["date", "ticker", "name", "quantity", "value", "weight_pct",
                    "equity", "cash"])
        for i, d in enumerate(dates):
            day_eq = eq[i] if i < len(eq) else 0
            day_cash = cash[i] if i < len(cash) else 0
            rows = holds[i] if i < len(holds) else []
            if not rows:   # 보유 없음(전량 현금)도 한 줄 남겨 상태 보존
                w.writerow([d, "", "", "", "", "", f"{day_eq:.0f}", f"{day_cash:.0f}"])
                continue
            for (t, qty, val) in rows:
                wt = (val / day_eq * 100) if day_eq else 0
                w.writerow([d, t, names.get(t, ""), qty, f"{val:.0f}",
                            f"{wt:.2f}", f"{day_eq:.0f}", f"{day_cash:.0f}"])
    print(f"  📄 일별 보유종목 CSV 저장: {path} ({len(dates)}일 × 보유종목)")
