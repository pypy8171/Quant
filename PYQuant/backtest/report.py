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
    # 위험조정까지 본 정직한 판정 — 수익률만 높고 샤프(위험조정수익)가 벤치 미달이면 "더 큰 위험의 대가"
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


def _derived_metrics(equity: list) -> dict:
    """equity 시계열 → 파생 지표(연복리 CAGR·Sortino·Calmar). 초기 exporter가 계산 안 하던 축.
    거래일 252 기준 연환산. equity는 과거만(look-ahead 없음). 표본 부족 시 0.0."""
    out = {"cagr": 0.0, "sortino": 0.0, "calmar": 0.0}
    if not equity or len(equity) < 2 or equity[0] <= 0:
        return out
    import statistics
    # CAGR — 거래일 수 기준 연수
    years = max((len(equity) - 1) / 252.0, 1e-9)
    cagr = ((equity[-1] / equity[0]) ** (1.0 / years) - 1.0) * 100.0
    # 최대낙폭(MDD) (calmar 분모)
    peak, mdd = equity[0], 0.0
    for e in equity:
        peak = max(peak, e)
        mdd = max(mdd, (peak - e) / peak * 100.0 if peak > 0 else 0.0)
    # Sortino — 하방편차(음수 수익률만)로 연환산
    rets = [(equity[i] - equity[i-1]) / equity[i-1]
            for i in range(1, len(equity)) if equity[i-1] > 0]
    sortino = 0.0
    if len(rets) > 1:
        downside = [r for r in rets if r < 0]
        if len(downside) > 1:
            dstd = statistics.pstdev(downside)
            sortino = (statistics.mean(rets) / dstd * (252 ** 0.5)) if dstd > 0 else 0.0
    out["cagr"] = cagr
    out["sortino"] = sortino
    out["calmar"] = (cagr / mdd) if mdd > 0 else 0.0
    return out


def _turnover_proxy(result: BacktestResult) -> float:
    """연환산 회전율 근사 = 총 매수체결금액 / 평균 equity / 연수. 엔진이 회전율(turnover)를 직접
    추적하지 않으므로 체결로그로 재구성한 **프록시**(대시보드에 proxy로 표기). 표본부족 시 0."""
    trades = result.trades or []
    eq = result.equity_curve or []
    if not trades or not eq:
        return 0.0
    buy_notional = sum(t.price * t.quantity for t in trades if t.side == "BUY")
    mean_eq = sum(eq) / len(eq) if eq else 0.0
    years = max(len(eq) / 252.0, 1e-9)
    return (buy_notional / mean_eq / years) if mean_eq > 0 else 0.0


def _round(v, n=4):
    return round(v, n) if isinstance(v, (int, float)) and v == v else v  # NaN 통과


def metrics_row(**over) -> dict:
    """quant.metrics/v1 한 행 — 전 키를 기본값으로 깔고 over로 덮는다(스키마 단일 소스).
    계열 A(포트폴리오)·B(지수 오버레이) 공용. 해당 없는 필드는 None으로 남겨
    대시보드가 family로 판단한다(예: 오버레이엔 win_rate/n_trades=None).
    mdd는 **항상 양수 크기**(A는 양수, B의 음수 mdd는 호출부에서 abs 정규화).
    스키마 문서: docs/design/DASHBOARD_SPEC.md"""
    row = {
        "schema": "quant.metrics/v1",
        "study_id": "", "strategy": "",
        "family": "A_portfolio",         # A_portfolio | B_overlay
        "benchmark": "",                 # B: ^GSPC/^KS11 등 오버레이 대상 지수
        "event": "", "window": "", "start_date": "", "end_date": "",
        # ── 헤드라인 성과 ──
        "total_return": None, "cagr": None, "sharpe": None, "sortino": None,
        "mdd": None, "calmar": None, "win_rate": None,
        "turnover": None, "turnover_is_proxy": False,
        "n_trades": None, "total_pnl": None,
        # ── 벤치마크/알파 ──
        "bench_return": None, "bench_mdd": None, "bench_sharpe": None,
        "alpha": None, "kodex_return": None, "regime_off": None,
        # ── 정직성/검증 라벨 (편향 감사관 필수 필드) ──
        "oos_flag": False, "holdout_flag": False, "honesty_label": "unlabeled",
        # ── 곡선·체결 링크 ──
        "equity_csv_path": "", "trades_csv_path": "",
    }
    row.update(over)
    return row


def _write_metrics(path: str, payload):
    """metrics 파일 쓰기(단일 객체 또는 배열). ensure_ascii=False, indent=2."""
    import json
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def export_metrics_json(result: BacktestResult, path: str, *,
                        study_id: str = "", strategy: str = "", event: str = "",
                        window: str = "", turnover=None,
                        family: str = "A_portfolio", benchmark: str = "",
                        oos_flag: bool = False, holdout_flag: bool = False,
                        honesty_label: str = "unlabeled",
                        equity_csv_path: str = "", trades_csv_path: str = "",
                        extra: dict | None = None):
    """정규화 요약지표 export (대시보드 데이터 계약 ①, 계열 A). 콘솔·md 산문에만 있던
    헤드라인 지표를 파일화. 곡선·체결은 CSV 경로로 링크. honesty_label로 결과 맥락 보존."""
    d = _derived_metrics(result.equity_curve or [])
    metrics = metrics_row(
        study_id=study_id, strategy=strategy or "", family=family, benchmark=benchmark,
        event=event,
        window=window or (f"{result.start_date}~{result.end_date}"
                          if result.start_date else ""),
        start_date=result.start_date, end_date=result.end_date,
        total_return=_round(result.total_return), cagr=_round(d["cagr"]),
        sharpe=_round(result.sharpe), sortino=_round(d["sortino"]),
        mdd=_round(result.mdd), calmar=_round(d["calmar"]),
        win_rate=_round(result.win_rate),
        turnover=_round(turnover if turnover is not None else _turnover_proxy(result)),
        turnover_is_proxy=turnover is None,
        n_trades=result.trade_count, total_pnl=_round(result.total_pnl, 2),
        bench_return=_round(result.bench_return), bench_mdd=_round(result.bench_mdd),
        bench_sharpe=_round(result.bench_sharpe), alpha=_round(result.alpha),
        kodex_return=(_round(result.kodex_return)
                      if result.kodex_return is not None else None),
        regime_off=result.regime_off,
        oos_flag=bool(oos_flag), holdout_flag=bool(holdout_flag),
        honesty_label=honesty_label or "unlabeled",
        equity_csv_path=equity_csv_path, trades_csv_path=trades_csv_path)
    if extra:
        metrics.update(extra)
    _write_metrics(path, metrics)
    print(f"  📄 정규화 지표 JSON 저장: {path} "
          f"(honesty={metrics['honesty_label']}, oos={metrics['oos_flag']})")


def overlay_metric_row(*, study_id: str, strategy: str, benchmark: str,
                       base: dict, bh: dict, window: str = "",
                       start_date: str = "", end_date: str = "",
                       honesty_label: str = "robust", extra: dict | None = None) -> dict:
    """계열 B(지수 익스포저 오버레이) 한 행 빌더 — BT-08/09의 curve_stats dict를
    quant.metrics/v1로 정규화. base/bh = {total,cagr,mdd(음수%),sharpe,calmar}.
    mdd를 양수 크기로 정규화(스키마 규약), win_rate/n_trades는 None(오버레이 무의미).
    alpha(초과수익) = 전략 CAGR − BH CAGR(%p, 초과연율). 저자 규율상 결과는 정직 → 기본 robust."""
    row = metrics_row(
        study_id=study_id, strategy=strategy, family="B_overlay", benchmark=benchmark,
        event="full_curve", window=window, start_date=start_date, end_date=end_date,
        total_return=_round(base.get("total")), cagr=_round(base.get("cagr")),
        sharpe=_round(base.get("sharpe")),
        mdd=_round(abs(base["mdd"]) if base.get("mdd") is not None else None),
        calmar=_round(base.get("calmar")),
        bench_return=_round(bh.get("total")),
        bench_mdd=_round(abs(bh["mdd"]) if bh.get("mdd") is not None else None),
        bench_sharpe=_round(bh.get("sharpe")),
        alpha=_round(base["cagr"] - bh["cagr"]
                     if base.get("cagr") is not None and bh.get("cagr") is not None else None),
        honesty_label=honesty_label)
    if extra:
        row.update(extra)
    return row


def write_metrics_rows(path: str, rows: list):
    """여러 metrics 행을 JSON 배열로 저장(계열 B: 한 스터디에 전략 다수)."""
    _write_metrics(path, rows)
    print(f"  📄 정규화 지표 배열 저장: {path} ({len(rows)}행)")


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
