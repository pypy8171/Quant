"""바스켓 목표 비중표 작성기 — 두 슬리브의 목표 종목을 뽑아 `Quant/config/basket_targets.json` 한 장으로 쓴다.

주문은 내지 않는다. 집행은 C++ 엔진의 TARGET_BASKET 슬리브(`Quant/src/strategy/TargetBasketStrategy.cpp`)가 장중 14:40~15:00에
"시드 × 슬리브 share × weight − 원장 보유"의 차이만 낸다(D-109). 여기는 신호 계산과 파일 쓰기만 맡는다 — 원장·리스크가 두 프로세스로
갈라지지 않게(설계 원칙 4).

슬리브(2026-09-20 기준 둘):
  - VALUE     저PBR×고ROE 상위 30, 가치 비중 0.7, 분기 리밸, 거래대금 하한 30억
              (스터디 22·23 채택 후보. 신호는 스터디와 같은 `features.fundamental.FundamentalData.compute`)
  - MOMENTUM  횡단면 모멘텀 상위 30, 변동성 조정, 20거래일 리밸, 국면 ON(스터디 01~03. 시세는 VALUE와 같은 네이버 패널)

파일 계약(정본 docs/DECISIONS.md D-109, 읽는 쪽 `Quant/include/strategy/TargetBasketPlan.h`):
  { "schema": 1, "generated_at", "as_of": "YYYY-MM-DD"(집행일 = 오늘), "count": 행 수,
    "sleeves": { "VALUE": {"share": 0.5, "is_rebalance_day": true}, "MOMENTUM": {...} },
    "targets": [ {"ticker","name","sleeve","weight","reference_price"(전일 종가),"action": "NEW|KEEP|DROP","reason"} ... ] }
  - weight는 슬리브 안 동일가중(합 1). 리밸 날이 아닌 슬리브는 상태 파일의 지난 목표를 KEEP으로 그대로 쓴다.
  - 지난 목표에 있었는데 이번에 빠진 종목은 DROP 행으로 명시한다(엔진은 파일에 없는 종목을 건드리지 않는다).
  - 기준가가 없는 종목은 빼고 나머지를 다시 동일가중한다(엔진이 reference_price 0을 거부한다).
  - 상태 파일(`PYQuant/live/.basket_state.json`)은 슬리브별 직전 리밸 날짜·종목. 파일을 쓴 시점에 갱신한다(엔진이 그날 못 냈어도
    다음 날 파일이 KEEP으로 다시 내고, 보유 0이면 엔진이 신규 매수한다).
  - 거래일 달력은 일봉 패널(`PYQuant/data/bars_all_pit_v2.parquet`)의 날짜다. 패널이 5일 넘게 낡으면 경고한다.

실행: 평일 08:40 예약작업(`py PYQuant/main.py basket`). 판정은 `scripts/check_runtime_health.py`가 한다.
"""
import json
import os
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta
from pathlib import Path

import pandas as pd

BILLION_KRW = 100_000_000
STALE_DAYS = 5
REPO_ROOT = Path(__file__).resolve().parents[2]
STATE_PATH = Path(__file__).resolve().parent / ".basket_state.json"
TARGETS_DIR = Path(__file__).resolve().parent / "basket_targets"
OUTPUT_PATH = REPO_ROOT / "Quant" / "config" / "basket_targets.json"


@dataclass
class PanelBar:
    """패널 한 칸을 `CrossMomentumStrategy`·`market_risk_on`이 읽는 모양(.date·.close)으로."""
    date: str
    close: float


@dataclass
class SleeveTargets:
    sleeve: str
    as_of: str                 # 신호를 낸 데이터일(마지막 일봉)
    tickers: list
    note: str = ""
    table: pd.DataFrame | None = field(default=None, repr=False)
    prices: dict = field(default_factory=dict)   # 종목 → 마지막 종가(기준가). 없으면 작성기가 패널에서 찾는다
    names: dict = field(default_factory=dict)


def trading_days_after(calendar: pd.DatetimeIndex, last_date: str | None) -> int:
    if not last_date:
        return 10 ** 6

    return int((calendar > pd.Timestamp(last_date)).sum())


def quarter_index(day: pd.Timestamp) -> int:
    return day.year * 4 + (day.month - 1) // 3


class ValueTiltSleeve:
    """스터디 23 중심 칸. 분기 첫 거래일에 그날까지 보이는 재무·주식수·일봉으로 상위 N을 뽑는다."""
    name = "VALUE"

    def __init__(self, data, top_n: int = 30, value_weight: float = 0.7, turnover_floor_krw: float = 30 * BILLION_KRW,
                 min_universe: int = 100):
        self.data = data
        self.top_n = top_n
        self.value_weight = value_weight
        self.turnover_floor_krw = turnover_floor_krw
        self.min_universe = min_universe

    def is_due(self, last_rebalance: str | None, calendar: pd.DatetimeIndex) -> bool:
        if not last_rebalance:
            return True

        return quarter_index(calendar[-1]) > quarter_index(pd.Timestamp(last_rebalance))

    def targets(self, as_of: pd.Timestamp) -> SleeveTargets:
        from features import fundamental
        previous_floor = fundamental.MIN_TURNOVER_KRW
        fundamental.MIN_TURNOVER_KRW = self.turnover_floor_krw

        try:
            frame = self.data.compute(as_of, self.value_weight)

        finally:
            fundamental.MIN_TURNOVER_KRW = previous_floor

        if len(frame) < self.min_universe:
            raise RuntimeError(f"{self.name}: 유니버스 {len(frame)}종목 < {self.min_universe} — 재무·일봉 패널 확인")

        top = frame.head(self.top_n)
        note = (f"유니버스 {len(frame)} · 하한 {self.turnover_floor_krw / BILLION_KRW:.0f}억 · 가치 비중 {self.value_weight}"
                f" · PBR 중앙값 {top['pbr'].median():.2f} · ROE 중앙값 {top['roe'].median() * 100:.1f}%")
        return SleeveTargets(self.name, as_of.date().isoformat(), top["ticker"].tolist(), note,
                             top[["ticker", "name", "pbr", "roe", "score", "market_cap", "turnover_20d", "close"]],
                             prices=dict(zip(top["ticker"], top["close"].astype(float))),
                             names=dict(zip(top["ticker"], top["name"].fillna(""))))


class MomentumSleeve:
    """횡단면 모멘텀 칸. 시세는 가치 슬리브와 같은 네이버 일봉 패널(`data.bars`)만 쓴다 — data.go.kr 시세는 영업일+1 13시에야
    올라와 하루 늦다(메모리 data_source_constraints). 유니버스는 패널의 보통주 중 거래대금·시총 필터를 통과한 시총 상위 N.
    신호는 스터디 01~03의 `CrossMomentumStrategy` 그대로, 국면은 유니버스 breadth(200일선 위 비율 ≥ 0.5)로 스터디 03의 ON."""
    name = "MOMENTUM"

    def __init__(self, data, top_n: int = 30, lookback: int = 120, skip: int = 20, rebalance_every: int = 20,
                 volatility_adjust: bool = True, regime_on: bool = True, regime_moving_average: int = 200, regime_thresh: float = 0.5,
                 universe_size: int = 300):
        from strategy.cross_momentum import CrossMomentumStrategy
        self.data = data
        self.strategy = CrossMomentumStrategy(top_n=top_n, rebalance_every=1, lookback=lookback, skip=skip,
                                              vol_adjust=volatility_adjust)
        self.lookback = lookback
        self.skip = skip
        self.rebalance_every = rebalance_every
        self.regime_on = regime_on
        self.regime_moving_average = regime_moving_average
        self.regime_thresh = regime_thresh
        self.universe_size = universe_size

    def is_due(self, last_rebalance: str | None, calendar: pd.DatetimeIndex) -> bool:
        return trading_days_after(calendar, last_rebalance) >= self.rebalance_every

    def targets(self, as_of: pd.Timestamp) -> SleeveTargets:
        from backtest.engine import market_risk_on
        candidates = self.data.candidates(as_of).sort_values("market_cap", ascending=False).head(self.universe_size)

        if candidates.empty:
            raise RuntimeError(f"{self.name}: 유니버스 비어 있음 — 일봉·주식수 패널 확인")

        need = max(self.lookback + self.skip + 1, self.regime_moving_average)
        window = self.data.bars.close.loc[:as_of].tail(need + 5)
        visible = {}

        for ticker in candidates["ticker"]:
            if ticker not in window.columns:
                continue

            series = window[ticker].dropna()

            if len(series) < need:
                continue

            visible[ticker] = [PanelBar(day.date().isoformat(), float(close)) for day, close in series.items()]

        if not visible:
            raise RuntimeError(f"{self.name}: 워밍업 {need}일을 채운 종목 없음")

        latest = max(bars[-1].date for bars in visible.values())
        chosen = sorted(self.strategy.on_rebalance(latest, visible) or [])
        risk_on = True

        if self.regime_on:
            risk_on = market_risk_on(visible, self.regime_moving_average, self.regime_thresh)

            if not risk_on:
                chosen = []

        note = (f"유니버스 {len(visible)} · 국면 {'ON' if self.regime_on else 'OFF'}"
                f"{'' if risk_on else '(risk-off, 현금)'} · 데이터일 {latest}")
        names = candidates.set_index("ticker")["name"].fillna("").to_dict()
        names = {ticker: names.get(ticker, "") for ticker in chosen}
        prices = {ticker: visible[ticker][-1].close for ticker in chosen}
        table = pd.DataFrame({"ticker": chosen, "name": [names[ticker] for ticker in chosen],
                              "close": [prices[ticker] for ticker in chosen]})
        return SleeveTargets(self.name, latest, chosen, note, table, prices=prices, names=names)

class BasketTargetsWriter:
    def __init__(self, sleeves: list, shares: dict, calendar: pd.DatetimeIndex, close_prices: pd.Series, *,
                 output_path: Path = OUTPUT_PATH, state_path: Path = STATE_PATH):
        self.sleeves = sleeves
        self.shares = shares                  # 슬리브 이름 → 바스켓 자본 몫(합 ≤ 1)
        self.calendar = calendar
        self.close_prices = close_prices      # 패널 최신 종가(슬리브가 가격을 못 준 종목의 기준가)
        self.output_path = output_path
        self.state_path = state_path

    # ── 상태 ─────────────────────────────────────────────────────────────
    def load_state(self) -> dict:
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))

        except Exception:
            return {"sleeves": {}}

    def save_state(self, state: dict) -> None:
        atomic_write_json(self.state_path, state)

    # ── 실행 ─────────────────────────────────────────────────────────────
    def run_once(self, force: set | None = None, execution_date: date | None = None) -> dict:
        """목표 비중표를 만들어 파일로 쓰고 그 문서를 돌려준다. 슬리브 하나라도 실패하면 파일을 건드리지 않는다
        (엔진은 as_of가 오늘이 아닌 직전 파일을 무시하니, 안 쓰는 것이 잘못 쓰는 것보다 안전하다)."""
        force = force or set()
        execution_date = execution_date or date.today()
        as_of = self.calendar[-1]
        print(f"\n{'=' * 64}\nBasketTargetsWriter  집행일 {execution_date}  데이터 기준일 {as_of:%Y-%m-%d}")
        print("=" * 64)
        gap = (execution_date - as_of.date()).days

        if gap > STALE_DAYS:
            print(f"일봉 패널 최신일({as_of:%Y-%m-%d})이 {gap}일 지남 — `py PYQuant/tools/naver_bars_backfill.py`로 갱신 뒤 다시")

        state = self.load_state()
        sleeve_state = state.setdefault("sleeves", {})
        refreshed = {}
        active = {}          # 슬리브 → 종목 목록
        rebalance_day = {}
        previous = {}        # 슬리브 → 지난 목표(DROP 계산용)
        names = {}
        prices = {}

        for sleeve in self.sleeves:
            saved = sleeve_state.get(sleeve.name, {})
            previous[sleeve.name] = list(saved.get("tickers") or [])
            last_rebalance = saved.get("last_rebalance_date")
            due = sleeve.name in force or "all" in force or sleeve.is_due(last_rebalance, self.calendar)

            if due:
                targets = sleeve.targets(as_of)

                # 슬리브가 패널보다 낡은 시세로 순위를 매기면 파일을 쓰지 않는다(09-20 data.go.kr 하루 지연 사고 재발 방지).
                if targets.as_of != as_of.date().isoformat():
                    raise RuntimeError(f"{sleeve.name}: 데이터일 {targets.as_of} ≠ 패널 최신일 {as_of:%Y-%m-%d} — 시세 입구 확인")

                refreshed[sleeve.name] = targets
                active[sleeve.name] = list(targets.tickers)
                rebalance_day[sleeve.name] = True
                names.update(targets.names)
                prices.update({ticker: price for ticker, price in targets.prices.items() if price and price > 0})
                self.write_sleeve_csv(targets)
                print(f"[{sleeve.name}] 리밸런스 — 목표 {len(targets.tickers)}종목 ({targets.note})")

            else:
                active[sleeve.name] = previous[sleeve.name]
                rebalance_day[sleeve.name] = False
                names.update(saved.get("names") or {})
                since = trading_days_after(self.calendar, last_rebalance)
                print(f"[{sleeve.name}] 유지 — 지난 목표 {len(active[sleeve.name])}종목 (직전 리밸 {last_rebalance}, {since}거래일 전)")

        document = self.build_document(active, rebalance_day, previous, names, prices, execution_date)
        atomic_write_json(self.output_path, document)
        print(f"목표 비중표 → {self.output_path} (행 {document['count']}, "
              + ", ".join(f"{name} {'리밸' if flag else '유지'}" for name, flag in rebalance_day.items()) + ")")

        for name, targets in refreshed.items():
            sleeve_state[name] = {"last_rebalance_date": targets.as_of, "tickers": active[name], "note": targets.note,
                                 "names": {ticker: names.get(ticker, "") for ticker in active[name]},
                                 "written_on": execution_date.isoformat()}

        # 리밸이 아닌 슬리브도 기준가가 없어 빠진 종목이 있으면 목록을 줄여 둔다(다음 날 또 DROP을 만들지 않게).
        for name in active:
            if name in sleeve_state and sleeve_state[name].get("tickers") != active[name]:
                sleeve_state[name]["tickers"] = active[name]

        self.save_state(state)
        return document

    def build_document(self, active: dict, rebalance_day: dict, previous: dict, names: dict, prices: dict,
                       execution_date: date) -> dict:
        """엔진이 읽는 문서. 기준가 없는 종목은 빼고 슬리브 안에서 다시 동일가중, 지난 목표에서 빠진 종목은 DROP."""
        rows = []
        kept_total = set()

        for name, tickers in active.items():
            priced = []

            for ticker in tickers:
                price = prices.get(ticker) or float(self.close_prices.get(ticker, 0) or 0)

                if price > 0:
                    priced.append((ticker, price))

                else:
                    print(f"  (기준가 없음 — {name} {ticker} 뺌)")

            active[name] = [ticker for ticker, _ in priced]
            kept_total.update(active[name])
            weight = 1.0 / len(priced) if priced else 0.0

            for ticker, price in priced:
                action = "NEW" if ticker not in previous.get(name, []) else "KEEP"
                rows.append({"ticker": ticker, "name": names.get(ticker, ""), "sleeve": name, "weight": weight,
                             "reference_price": price, "action": action, "reason": ""})

        for name, tickers in previous.items():
            for ticker in tickers:
                if ticker in kept_total:
                    continue

                price = prices.get(ticker) or float(self.close_prices.get(ticker, 0) or 0)
                rows.append({"ticker": ticker, "name": names.get(ticker, ""), "sleeve": name, "weight": 0.0,
                             "reference_price": price, "action": "DROP",
                             "reason": "리밸런스에서 빠짐" if rebalance_day.get(name) else "기준가 없음"})

        sleeves = {name: {"share": float(self.shares.get(name, 0.0)), "is_rebalance_day": bool(rebalance_day.get(name, False))}
                   for name in active}
        return {"schema": 1, "generated_at": datetime.now().isoformat(timespec="seconds"),
                "as_of": execution_date.isoformat(), "count": len(rows), "liquidate_all": False,
                "sleeves": sleeves, "targets": rows}

    # ── 기록 ─────────────────────────────────────────────────────────────
    @staticmethod
    def write_sleeve_csv(targets: SleeveTargets) -> None:
        """리밸 날 슬리브 표를 남긴다(점검·스터디 대조용)."""
        if targets.table is None:
            return

        TARGETS_DIR.mkdir(exist_ok=True)
        path = TARGETS_DIR / f"{targets.sleeve}_{targets.as_of}.csv"
        targets.table.to_csv(path, index=False, encoding="utf-8-sig")
        print(f"  슬리브 표 → {path.relative_to(REPO_ROOT)}")


def atomic_write_json(path: Path, document: dict) -> None:
    """임시 파일에 쓰고 바꿔 넣는다 — 엔진이 60초마다 읽는 파일이라 반쯤 쓰인 상태를 보이지 않게."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)
