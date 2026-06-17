"""
ForwardTrader — 검증된 횡단면 전략(regime-모멘텀)을 KIS 모의계좌로 forward-test.

설계 (3자 협의 결론):
  - 결정 로직은 백테스트와 **공유**(strategy.on_rebalance + engine.market_risk_on) — 신호 패리티.
  - 집행은 별도: KIS **실잔고(get_kr_balance)를 진실**로 한 무상태 diff(보유 vs 목표) →
    비목표 전량매도 + 목표 동일가중매수. 부분체결은 다음 사이클 재diff로 자가수렴.
  - 안전: ❶ 모의 강제(is_paper 아니면 거부, --real 명시해야 실거래) ❷ OrderGate(수량/금액
    상한·일일주문제한·디스크 멱등·rate limit) ❸ 기본 dry-run(주문 안 냄).
  - 트리거: 달력 아닌 **거래일 카운터**(데이터 날짜로 셈 → 공휴일 자동) + 상태파일 멱등.

⚠️ forward-test 성공 = P&L 아님. ①신호 일치(백테스트==라이브 목표집합) ②파이프라인 완주
   ③슬리피지 가정 내. 최소 1리밸런싱 사이클(~1개월).
"""
import json
import os
import time
from datetime import date, timedelta
from pathlib import Path

from kis.client import OrderSignal
from backtest.engine import market_risk_on, equal_weight_qty

_COST_RATE = 0.0020   # 수수료+슬리피지 추정(사이징용)


class OrderGate:
    """모든 주문의 단일 통과 지점 — 안전 가드. send_order 직호출 금지, 반드시 여기로."""
    def __init__(self, kis, *, allow_real: bool, max_notional: float,
                 max_orders_per_day: int, state: dict, asof: str, dry_run: bool,
                 names: dict | None = None, order_sleep: float = 0.5, persist=None):
        self.kis = kis
        self.allow_real = allow_real
        self.persist = persist   # 멱등키 즉시 디스크 영속화 콜백(크래시 중 이중발행 방지, W-1)
        self.max_notional = max_notional
        self.max_orders_per_day = max_orders_per_day
        self.state = state
        self.asof = asof
        self.dry_run = dry_run
        self.names = names or {}
        self.order_sleep = order_sleep   # 매 주문 전 대기(초당 거래건수 제한 회피)
        self._today_count = sum(1 for k in state.get("submitted_keys", [])
                                if k.startswith(asof))

    def submit(self, sig: OrderSignal, price_hint: float) -> bool:
        nm = self.names.get(sig.ticker, "")
        label = f"{sig.ticker}({nm})" if nm else sig.ticker
        # ❶ 실계좌 차단
        if not self.allow_real and not getattr(self.kis, "is_paper", False):
            print(f"  🚫 실계좌 감지 — 주문 거부 ({label})"); return False
        if sig.quantity <= 0:
            return False
        # 금액 상한 — 매수에만 적용(과대매수 방지). 매도(청산)는 항상 허용(위험 축소).
        notional = sig.quantity * price_hint
        if sig.side == "BUY" and notional > self.max_notional:
            print(f"  🚫 매수금액 상한 초과 {label} {notional:,.0f}>{self.max_notional:,.0f}")
            return False
        # 일일 주문수 제한
        if self._today_count >= self.max_orders_per_day:
            print(f"  🚫 일일 주문수 한도({self.max_orders_per_day}) — 차단"); return False
        # 멱등(디스크): 같은 (날짜,종목,방향,수량) 재전송 금지
        key = f"{self.asof}|{sig.ticker}|{sig.side}|{sig.quantity}"
        if key in self.state.get("submitted_keys", []):
            print(f"  ↺ 멱등 스킵 {label} {sig.side} {sig.quantity}"); return False

        side_kr = "매수" if sig.side == "BUY" else "매도"
        tag = "[DRY]" if self.dry_run else "[주문]"
        print(f"  {tag} {side_kr} {label} {sig.quantity}주 @{price_hint:,.0f} (~{notional:,.0f}원)")
        if not self.dry_run:
            time.sleep(self.order_sleep)   # rate limit — 매 호출 전(실패해도 적용)
            res = self.kis.send_order(sig)
            if not res.ok:
                print(f"     ❌ 주문실패 rt_cd={res.rt_cd} {res.msg1}"); return False
            print(f"     ✅ 접수 ODNO={res.odno}")
            self.state.setdefault("submitted_keys", []).append(key)
            self._today_count += 1
            if self.persist:
                self.persist(self.state)   # 접수 즉시 영속화 — 크래시/Ctrl-C 시 재발행 차단(W-1)
        return True


class ForwardTrader:
    def __init__(self, kis, strategy, source, *, top_n=30, lookback=120, skip=20,
                 rebalance_every=20, regime_on=True, regime_ma=200, regime_thresh=0.5,
                 universe_size=200, kosdaq_size=100, dry_run=True, allow_real=False,
                 max_orders_per_day=80, state_path=None):
        # ❶ 모의 강제
        if not allow_real and not getattr(kis, "is_paper", False):
            raise RuntimeError("실계좌(is_paper=False) 감지 — forward-test는 모의 전용. "
                               "실거래하려면 allow_real=True 명시(권장 안 함).")
        self.kis = kis
        self.strategy = strategy
        self.source = source
        self.top_n = top_n
        self.lookback = lookback
        self.skip = skip
        self.rebalance_every = rebalance_every
        self.regime_on = regime_on
        self.regime_ma = regime_ma
        self.regime_thresh = regime_thresh
        self.universe_size = universe_size
        self.kosdaq_size = kosdaq_size
        self.dry_run = dry_run
        self.allow_real = allow_real
        self.max_orders_per_day = max_orders_per_day
        self.state_path = Path(state_path or
                               Path(__file__).resolve().parent / ".forward_state.json")

    @staticmethod
    def _is_market_open() -> bool:
        from datetime import datetime
        now = datetime.now()
        if now.weekday() >= 5:   # 토/일
            return False
        hhmm = now.hour * 100 + now.minute
        return 900 <= hhmm < 1530

    # ── 상태(멱등·리밸런싱 카운터) ───────────────────────────────────────────
    def _load_state(self) -> dict:
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except Exception:
            return {"last_rebalance_date": None, "rebalance_count": 0, "submitted_keys": []}

    def _save_state(self, state: dict):
        tmp = self.state_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(self.state_path)

    # ── 데이터 수집(라이브 — datagokr T+1, 결정=직전 종가) ───────────────────
    def _collect(self, universe: list[str]):
        warm = int((max(self.lookback + self.skip, self.regime_ma)) * 2.1) + 30
        start = (date.today() - timedelta(days=warm)).isoformat()
        end = date.today().isoformat()
        if hasattr(self.source, "prefetch_ohlcv"):
            self.source.prefetch_ohlcv(universe, start, end)
        visible = {}
        for t in universe:
            bars = self.source.get_historical_ohlcv(t, start, end)
            if bars:
                visible[t] = bars
        return visible

    def run_once(self, force: bool = False):
        mode = "DRY-RUN(주문안냄)" if self.dry_run else ("실거래" if self.allow_real else "모의주문")
        ep = getattr(self.kis, "base_url", "?")
        print(f"\n{'='*64}\nForwardTrader [{mode}]  엔드포인트: {ep}")
        print(f"전략: {self.strategy.id()} | regime={'ON' if self.regime_on else 'OFF'}"
              f"(ma{self.regime_ma}/th{self.regime_thresh}) | rb{self.rebalance_every}일")
        print('='*64)
        state = self._load_state()

        # 1. 유니버스(as-of 오늘) + 데이터
        from main import select_universe   # 지연 import(순환 회피)
        universe = select_universe(self.source, "datagokr", from_date=date.today().isoformat(),
                                   universe_size=self.universe_size, kosdaq_size=self.kosdaq_size)
        if not universe:
            print("유니버스 비어있음 — 중단(키/데이터 확인)"); return
        visible = self._collect(universe)
        if not visible:
            print("OHLCV 수집 실패 — 중단"); return
        all_dates = sorted({b.date for bars in visible.values() for b in bars})
        asof = all_dates[-1]   # 결정 기준일 = 최신 데이터일(보통 T-1)

        # stale 가드 — 최신 데이터가 너무 오래됐으면 경고
        gap = (date.today() - date.fromisoformat(asof)).days
        if gap > 5:
            print(f"⚠️ 최신 데이터({asof})가 {gap}일 지남 — 신선도 점검 필요")

        # 2. 리밸런싱 데이 판정(거래일 카운터)
        last_rb = state.get("last_rebalance_date")
        if last_rb == asof and not force:
            print(f"이미 {asof} 리밸런싱 완료(멱등) — 스냅샷만."); self._snapshot(asof); return
        if last_rb:
            days_since = sum(1 for d in all_dates if d > last_rb)
            is_rb = days_since >= self.rebalance_every
            print(f"직전 리밸런싱 {last_rb} 이후 {days_since}거래일 (주기 {self.rebalance_every})")
        else:
            is_rb = True
            print("첫 실행 — 리밸런싱")
        if force:
            is_rb = True
        if not is_rb:
            print("리밸런싱 날 아님 — 스냅샷만."); self._snapshot(asof); return

        # 3. 신호: regime + 목표집합 (백테스트와 동일 함수)
        # ⚠️ 신호 패리티 범위(C-2): 라이브는 breadth 국면(market_risk_on)만 지원한다.
        #    백테스트의 index 모드(지수 200MA)·daily_regime은 연구용이며 아직 라이브 미배선 —
        #    백테스트에서 그 모드를 채택하려면 여기에 IndexSource+index_risk_on / 매일체크를 동일 이식해야
        #    포워드테스트가 백테스트 엣지를 재현한다. 현재 채택 신호(breadth 월간)는 일치.
        self.strategy.set_kis(self.source)
        target = set(self.strategy.on_rebalance(asof, visible) or [])
        risk_on = True
        if self.regime_on:
            risk_on = market_risk_on(visible, self.regime_ma, self.regime_thresh)
            if not risk_on:
                target = set()
        print(f"[{asof}] 시장국면: {'risk-ON' if risk_on else 'risk-OFF(현금화)'} | "
              f"목표 {len(target)}종목")

        # 4. 실잔고 reconcile(무상태 diff)
        items, summary = self.kis.get_kr_balance()
        held = {it.ticker: it.quantity for it in items if it.quantity > 0}
        cur_px = {it.ticker: it.current_price for it in items}
        equity = summary.total_eval or summary.cash
        balance_ok = bool(items) or equity > 0
        if not balance_ok:
            print("⚠️ 잔고조회 실패(타임아웃 등) — reconcile/상태저장 생략, 다음 사이클 재시도")
            self._snapshot(asof); return
        sells = sorted(set(held) - target)
        buys = sorted(target - set(held))
        # 매수 가용현금 = 예수금 + 청산 추정대금 × 0.9(buffer).
        # 한국 매도대금 T+2 결제라 전액 즉시가용 아님(C-2) → 보수적 90%만 반영해 "주문가능금액 부족"
        # 거부 줄임. 남는 매수는 다음 사이클 self-heal. ⚠️실계좌 전: KIS inquire-psbl-order(실주문가능금액)
        # 직접 조회로 대체 + 접수≠체결 inquire-ccnl 체결확인 추가(C-1) 필요.
        sell_proceeds = sum(held[t] * (cur_px.get(t, 0) or 0) for t in sells)
        avail = summary.cash + sell_proceeds * 0.9
        print(f"보유 {len(held)}종목 → 청산 {len(sells)} / 신규매수 {len(buys)} "
              f"(평가 {equity:,.0f} 예수금 {summary.cash:,.0f} +청산대금 {sell_proceeds:,.0f})")

        # 장중 가드 — execute인데 장 외(09:00~15:30 평일)면 주문 거부되니 미리 막음
        if not self.dry_run and not self._is_market_open():
            print("⚠️ 장 외 시간 — 모의주문은 장중(평일 09:00~15:30)만 가능. "
                  "주문 생략(신호/스냅샷만). 장중에 --execute 재실행.")
            self._snapshot(asof); return

        # 종목명(출력 가독성) — 보유 + 목표
        names = {t: self.source.ticker_name(t) for t in (set(held) | target)
                 if hasattr(self.source, "ticker_name")}
        gate = OrderGate(self.kis, allow_real=self.allow_real,
                         max_notional=max(equity * 0.2, 5_000_000),
                         max_orders_per_day=self.max_orders_per_day,
                         state=state, asof=asof, dry_run=self.dry_run, names=names,
                         persist=self._save_state)
        # 청산 먼저(현금 확보) — 매도는 상한 없이 항상 허용
        for t in sells:
            gate.submit(OrderSignal(t, "SELL", held[t], "MARKET"), cur_px.get(t, 0) or 1)
        # 목표 동일가중 매수 (청산대금 포함 가용현금 기준)
        for t in buys:
            px = visible[t][-1].close if visible.get(t) else 0
            if px <= 0:
                continue
            qty = equal_weight_qty(equity, len(target), px, _COST_RATE, avail)
            if qty > 0 and gate.submit(OrderSignal(t, "BUY", qty, "MARKET"), px):
                avail -= qty * px * (1 + _COST_RATE)

        # 5. 상태 갱신 + 스냅샷 (잔고조회 성공 + execute 시에만 상태 저장)
        if not self.dry_run:
            state["last_rebalance_date"] = asof
            state["rebalance_count"] = state.get("rebalance_count", 0) + 1
            self._save_state(state)
        self._snapshot(asof)
        print(f"{'='*64}\n완료 ({mode}). " +
              ("실제 집행하려면 --execute." if self.dry_run else "상태 저장됨."))

    # ── P&L 스냅샷(일별 — CSV append, 백테스트 일별기록과 동일 개념) ──────────
    def _snapshot(self, asof: str):
        try:
            items, summary = self.kis.get_kr_balance()
            # 잔고조회 실패(타임아웃→빈응답)면 0값 행 기록 금지 — P&L 곡선 오염 방지
            if not items and (summary.total_eval or 0) <= 0:
                print("  (잔고조회 실패 — P&L 스냅샷 생략)"); return
            today = date.today().isoformat()   # 데이터일(asof=T-1) 아닌 '실행일' 기록
            path = Path(__file__).resolve().parent / "forward_pnl.csv"
            new = not path.exists()
            rows = path.read_text(encoding="utf-8-sig").splitlines() if not new else []
            # 헤더·빈줄 제거 + 같은 날 중복 행 제거(하루 1행, 재실행 시 갱신) — 명시 필터(W-4)
            data_rows = [r for r in rows
                         if r and not r.startswith("date") and not r.startswith(today + ",")]
            pnl = sum(it.pnl for it in items)
            line = (f"{today},{summary.total_eval:.0f},{summary.cash:.0f},"
                    f"{len([i for i in items if i.quantity>0])},{pnl:.0f}")
            tmp = path.with_suffix(".csv.tmp")        # 원자적 교체(중단 시 원본 보존)
            with open(tmp, "w", encoding="utf-8-sig") as f:
                f.write("date,total_eval,cash,n_positions,total_pnl\n")
                for r in data_rows:
                    f.write(r + "\n")
                f.write(line + "\n")
            os.replace(tmp, path)
            print(f"  📄 P&L 스냅샷 → forward_pnl.csv ({today} 평가 {summary.total_eval:,.0f})")
        except Exception as e:                       # noqa: BLE001
            print(f"  (스냅샷 실패: {type(e).__name__})")
