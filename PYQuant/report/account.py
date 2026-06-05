"""
계좌 리포트 — 원금추적 / 기간수익률 / 거래내역

데이터 소스 (하이브리드, ADR 참조):
  - 현재 계좌 현황·종목별 평가손익: KIS 잔고 API (항상 정확, SSOT)
  - 원금추적·기간수익률:           로컬 DB account_snapshots (EOD 누적) + cash_flows
  - 거래내역(체결):                 로컬 DB fills (이 시스템이 낸 주문) — 수동매매는 미포함

모의(account='paper')는 입출금이 없어 cash_flows가 비어 있음 — 원금=시작 평가금.
실전(account='real')은 입출금을 cash_flows에 기록해야 원금이 정확해진다.
"""
from datetime import datetime, timezone


def _fmt(n) -> str:
    try:
        return f"{float(n):,.0f}"
    except (TypeError, ValueError):
        return "-"


def _fmt_dt(ts, fmt: str = "%Y-%m-%d") -> str:
    """ts가 datetime이면 포맷, str로 오는 드라이버면 그대로 반환 (C-4 — TypeError 방지)."""
    try:
        return ts.strftime(fmt)
    except AttributeError:
        return str(ts)


class AccountReport:
    def __init__(self, kis, db=None, account: str = "real"):
        self.kis = kis
        self.db = db
        self.account = account

    # ── 적재 ────────────────────────────────────────────────────────────────
    def snapshot(self):
        """현재 KIS 잔고를 account_snapshots에 적재하고 (items, summary) 반환."""
        items, s = self.kis.get_kr_balance()
        if self.db:
            self.db.insert_account_snapshot(
                self.account, s.cash, s.total_eval, s.total_pnl, s.total_pnl_rate)
        return items, s

    def record_cash_flow(self, flow_type: str, amount: float, memo: str = ""):
        if not self.db:
            raise RuntimeError("DB 미연결 — 입출금 기록 불가")
        self.db.insert_cash_flow(self.account, flow_type, amount, memo)

    # ── 계산 ────────────────────────────────────────────────────────────────
    def period_summary(self, start=None, end=None) -> dict:
        """스냅샷 + 입출금으로 원금추적·기간수익률 계산.

        순수익 = 종료평가 - 시작평가 - 순입금
        원금(투입자본) = 시작평가 + 순입금
        수익률 = 순수익 / 원금 * 100
        """
        if not self.db:
            return {}
        snaps = self.db.get_snapshots(self.account, start, end)
        flows = self.db.get_cash_flows(self.account, start, end)

        net_deposit = sum(
            (f["amount"] if f["flow_type"] == "DEPOSIT" else -f["amount"])
            for f in flows
        )
        begin_eval = float(snaps[0]["total_eval"]) if snaps else None
        end_eval = float(snaps[-1]["total_eval"]) if snaps else None

        pnl = base = ret = None
        if begin_eval is not None and end_eval is not None:
            pnl = end_eval - begin_eval - float(net_deposit)
            base = begin_eval + float(net_deposit)
            ret = (pnl / base * 100) if base else None

        return {
            "snap_count": len(snaps),
            "begin_ts": snaps[0]["ts"] if snaps else None,
            "end_ts": snaps[-1]["ts"] if snaps else None,
            "begin_eval": begin_eval,
            "end_eval": end_eval,
            "net_deposit": float(net_deposit),
            "principal": base,
            "pnl": pnl,
            "return_pct": ret,
            "flows": flows,
        }

    # ── 출력 ────────────────────────────────────────────────────────────────
    def print_report(self, start=None, end=None, show_trades: bool = False):
        acct_label = "모의투자" if self.account == "paper" else "실전"
        print(f"\n{'='*64}")
        print(f"  계좌 리포트 — {acct_label} (account={self.account})")
        rng = ""
        if start or end:
            rng = f"  기간: {start or '처음'} ~ {end or '지금'}"
        print(f"  생성: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}{rng}")
        print('='*64)

        # 1) 현재 계좌 현황 (KIS — 항상 정확)
        items, s = self.kis.get_kr_balance()
        print("\n[현재 계좌 현황 — KIS 실시간]")
        print(f"  예수금        {_fmt(s.cash):>16}원")
        print(f"  총평가금액    {_fmt(s.total_eval):>16}원")
        sg = '+' if s.total_pnl >= 0 else ''
        print(f"  총평가손익    {sg}{_fmt(s.total_pnl):>15}원  ({sg}{s.total_pnl_rate:.2f}%)")

        # 2) 종목별 평가손익
        print("\n[종목별 평가손익]")
        if items:
            print(f"  {'종목명':<14}{'수량':>6}{'평균단가':>11}{'현재가':>11}{'평가손익':>13}{'수익률':>8}")
            for it in items:
                sg = '+' if it.pnl >= 0 else ''
                print(f"  {it.name:<14}{it.quantity:>6}{_fmt(it.avg_price):>11}"
                      f"{_fmt(it.current_price):>11}{sg}{_fmt(it.pnl):>12}{sg}{it.pnl_rate:>6.2f}%")
        else:
            print("  보유 종목 없음")

        # 3) 원금추적 / 기간수익률 (DB 스냅샷)
        summ = self.period_summary(start, end)
        print("\n[원금추적 / 기간수익률 — DB 스냅샷]")
        if not self.db:
            print("  DB 미연결 — 스냅샷 기반 원금추적 불가 (현재 현황만 표시)")
        elif summ.get("snap_count", 0) < 1:
            print("  스냅샷 없음 — `report --snapshot`으로 적재를 시작하세요 (EOD 1회 권장)")
        else:
            print(f"  스냅샷 {summ['snap_count']}개  "
                  f"({_fmt_dt(summ['begin_ts'])} ~ {_fmt_dt(summ['end_ts'])})")
            print(f"  시작 평가금   {_fmt(summ['begin_eval']):>16}원")
            print(f"  순입금        {('+' if summ['net_deposit']>=0 else '')}{_fmt(summ['net_deposit']):>15}원")
            print(f"  원금(투입)    {_fmt(summ['principal']):>16}원")
            print(f"  종료 평가금   {_fmt(summ['end_eval']):>16}원")
            if summ["pnl"] is not None:
                sg = '+' if summ["pnl"] >= 0 else ''
                rp = f"{sg}{summ['return_pct']:.2f}%" if summ["return_pct"] is not None else "-"
                print(f"  기간 순수익   {sg}{_fmt(summ['pnl']):>15}원  ({rp})")

        # 4) 입출금 내역
        flows = summ.get("flows", []) if self.db else []
        print("\n[입출금 내역]")
        if self.account == "paper":
            print("  모의투자 — 입출금 없음")
        elif not flows:
            print("  기록 없음 — `report --deposit/--withdraw`로 기록 (KIS 입출금 TR 확정 전까지 수동)")
        else:
            for f in flows:
                sign = '+' if f["flow_type"] == "DEPOSIT" else '-'
                print(f"  {_fmt_dt(f['ts'])} {f['flow_type']:<8} {sign}{_fmt(f['amount']):>14}원"
                      f"  {f.get('memo') or ''}")

        # 5) 거래내역 (DB fills — 이 시스템 주문)
        if show_trades:
            print("\n[거래내역 — DB fills (시스템 주문 체결)]")
            fills = self.db.get_fills(start, end) if self.db else []
            if not fills:
                print("  체결 기록 없음")
            else:
                print(f"  {'시각':<20}{'종목':>8}{'구분':>5}{'수량':>6}{'단가':>11}{'수수료':>9}{'세금':>9}")
                for f in fills:
                    print(f"  {_fmt_dt(f['ts'], '%Y-%m-%d %H:%M:%S')}{f['ticker']:>8}{f['side']:>5}"
                          f"{f['filled_qty']:>6}{_fmt(f['filled_price']):>11}"
                          f"{_fmt(f['commission']):>9}{_fmt(f['tax']):>9}")
        print(f"\n{'='*64}\n")
