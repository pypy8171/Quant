"""
KRX 데이터 소스 (pykrx 기반) — 백테스트용 OHLCV / 수급 / 유니버스.

- OHLCV는 공개 엔드포인트. 수급(투자자별 순매수)·시총은 KRX 데이터포털 로그인 필요
  (환경변수 KRX_ID / KRX_PW — pykrx가 자동 사용).
- parquet 캐시로 반복 백테스트 시 네트워크 0.
- 엔진이 기대하는 최소 계약: get_historical_ohlcv / authenticate (+ flow_history / universe_top).
  엔진은 self.kis.get_historical_ohlcv 와 self.kis.flow_history(있으면)만 호출한다.
"""
from dataclasses import dataclass
from datetime import date as _date, timedelta
from pathlib import Path
import os
import time

from kis.client import Bar

# pykrx get_market_trading_value_by_date 컬럼명 (버전별로 다를 수 있음 — 다르면 명시적 예외).
COL_FOREIGN = "외국인합계"
COL_INST    = "기관합계"
COL_TOTAL   = "전체"


@dataclass
class Flow:
    date:        str     # "YYYY-MM-DD"
    foreign_net: float   # 외국인 순매수 금액
    inst_net:    float   # 기관 순매수 금액
    turnover:    float   # 전체 거래대금 (score 정규화용)


def _ymd(iso: str) -> str:
    return iso.replace("-", "")


class KrxSource:
    def __init__(self, market: str = "KOSPI", cache_dir: str | None = None, sleep: float = 0.4):
        self.market = market
        self.sleep  = sleep
        base = cache_dir or os.environ.get("KRX_CACHE_DIR") \
            or str(Path(__file__).resolve().parents[1] / ".krx_cache")
        self._cache = Path(base)
        self._cache.mkdir(parents=True, exist_ok=True)

    def authenticate(self) -> bool:
        return True  # pykrx는 KRX_ID/KRX_PW env로 자동 로그인(수급/시총 호출 시)

    # ── 캐시 ────────────────────────────────────────────────────────────────
    def _load_or_fetch(self, kind: str, key: str, fetch):
        import pandas as pd
        p = self._cache / f"{kind}_{key}.parquet"
        try:
            if p.exists():
                return pd.read_parquet(p)
        except Exception:
            pass  # 캐시 읽기 실패 → 재조회
        df = fetch()
        time.sleep(self.sleep)
        try:
            if df is not None and not df.empty:
                df.to_parquet(p)
        except Exception:
            pass  # pyarrow 없거나 쓰기 실패 → 캐시 없이 진행
        return df

    # ── OHLCV (공개) ────────────────────────────────────────────────────────
    def get_historical_ohlcv(self, ticker: str, start: str, end: str) -> list[Bar]:
        from pykrx import stock
        df = self._load_or_fetch(
            "ohlcv", f"{ticker}_{_ymd(start)}_{_ymd(end)}",
            lambda: stock.get_market_ohlcv(_ymd(start), _ymd(end), ticker))
        bars: list[Bar] = []
        if df is None or df.empty:
            return bars
        for ts, row in df.iterrows():
            bars.append(Bar(
                date   = ts.strftime("%Y-%m-%d"),
                open   = float(row["시가"]),
                high   = float(row["고가"]),
                low    = float(row["저가"]),
                close  = float(row["종가"]),
                volume = int(row["거래량"]),
            ))
        return bars  # inclusive end (pykrx 양끝 포함 — KIS/FakeKis 규약과 동일)

    # ── 수급 (로그인 필요) ──────────────────────────────────────────────────
    def flow_history(self, ticker: str, start: str, end: str) -> list[Flow]:
        from pykrx import stock
        df = self._load_or_fetch(
            "flow", f"{ticker}_{_ymd(start)}_{_ymd(end)}",
            lambda: stock.get_market_trading_value_by_date(_ymd(start), _ymd(end), ticker))
        flows: list[Flow] = []
        if df is None or df.empty:
            return flows  # 로그인 실패/데이터 없음 → 빈 리스트(전략이 신호 0 — 크래시 없음)
        for col in (COL_FOREIGN, COL_INST, COL_TOTAL):
            if col not in df.columns:
                raise KeyError(
                    f"pykrx 수급 컬럼 '{col}' 없음. 실제: {list(df.columns)} "
                    "— krx_source.py COL_* 상수 조정 필요")
        for ts, row in df.iterrows():
            flows.append(Flow(
                date        = ts.strftime("%Y-%m-%d"),
                foreign_net = float(row[COL_FOREIGN]),
                inst_net    = float(row[COL_INST]),
                turnover    = float(row[COL_TOTAL]),
            ))
        return flows

    # ── 유니버스: 시총 상위 N (로그인 필요) ─────────────────────────────────
    def universe_top(self, on_date: str, n: int, min_turnover: float = 1e9) -> list[str]:
        from pykrx import stock
        # on_date가 휴장일이면 직전 영업일로 최대 7일 백오프
        d = _date.fromisoformat(on_date)
        for _ in range(7):
            try:
                df = stock.get_market_cap_by_ticker(d.strftime("%Y%m%d"), market=self.market)
            except Exception:
                df = None  # 로그인 실패/휴장 → 빈 리스트 반환 → 호출측이 기본 유니버스로 폴백
            if df is not None and not df.empty and "시가총액" in df.columns:
                break
            d -= timedelta(days=1)
        else:
            return []
        if "거래대금" in df.columns:
            df = df[df["거래대금"] >= min_turnover]
        return df.sort_values("시가총액", ascending=False).head(n).index.tolist()

    # ── 종목명 (캐시) ────────────────────────────────────────────────────────
    def ticker_name(self, code: str) -> str:
        from pykrx import stock
        if not hasattr(self, "_name_cache"):
            self._name_cache: dict[str, str] = {}
        if code not in self._name_cache:
            try:
                self._name_cache[code] = stock.get_market_ticker_name(code) or ""
            except Exception:
                self._name_cache[code] = ""
        return self._name_cache[code]
