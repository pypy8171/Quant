"""
yfinance 기반 KR 일봉 소스 — datagokr 하한(2020) 이전 과거 구간(2011·2018 등) 백테스트용.

배경: datagokr(금융위 getStockPriceInfo)는 실측상 2020년부터만 서빙(2019 이전 빈값).
pykrx는 개별종목 일봉만(2015~) + 지수·시총 스냅샷은 KRX 로그인 차단. 따라서 2011·2018
같은 과거 하락장은 무키·장기 커버가 되는 Yahoo Finance(^KS11, {code}.KS, 2000~)로 받는다.

엔진 계약(datagokr_source와 동일): authenticate / get_historical_ohlcv / prefetch_ohlcv /
universe_top / ticker_name 만 맞추면 드롭인.

⚠️ 한계:
  - 유니버스: Yahoo엔 KR 시총 스냅샷이 없어 정적 KOSPI 대형주(universe_kospi.KOSPI_LARGE_MID)
    를 후보로 쓰고, as-of 날짜의 거래대금(종가×거래량)으로 랭킹한다. survivorship bias 존재
    (현재 우량주 집합 → 과거 미상장 종목은 yf 빈값으로 자동 제외). 방향성 비교용으로만.
  - 가격: auto_adjust=True(수정주가) — 액면분할/배당 반영(예: 삼성전자 2018 50:1 자동 처리).
"""
from __future__ import annotations

from pathlib import Path
from datetime import date as _date, timedelta

from kis.client import Bar
from data.universe_kospi import KOSPI_LARGE_MID, universe_codes


class YFinanceSource:
    HISTORY_START = "2005-01-01"   # 티커당 1회 최대범위 수집 → 모든 창 슬라이스 재사용
    HISTORY_END = "2027-01-01"
    # Yahoo Finance 시장 접미사. KOSPI=.KS / KOSDAQ=.KQ (하드코딩 `.KS` 리터럴 externalize)
    SUFFIX = {"KOSPI": ".KS", "KOSDAQ": ".KQ"}

    def __init__(self, market: str = "KOSPI", cache_dir: str | None = None):
        self.market = market
        self._suffix = self.SUFFIX.get(str(market).upper(), ".KS")   # 미지정 시 KOSPI 기본
        base = cache_dir or str(Path(__file__).resolve().parents[1] / ".yf_cache")
        self._cache = Path(base)
        self._cache.mkdir(parents=True, exist_ok=True)
        self._mem: dict[str, object] = {}   # ticker -> DataFrame (프로세스 내 재사용)

    def _cache_path(self, ticker: str) -> Path:
        # 캐시키: KOSPI(.KS)는 레거시 파일명 유지(기존 캐시 무효화 방지), 그 외 시장은
        # 접두로 동일 종목코드의 시장 간 충돌(005930.KS vs .KQ) 방지.
        key = ticker if self._suffix == ".KS" else f"{self.market}_{ticker}"
        return self._cache / f"{key}.parquet"

    def authenticate(self) -> bool:
        try:
            import yfinance  # noqa: F401
            return True
        except Exception:
            print("[yfinance] 미설치 — pip install yfinance")
            return False

    # ── 티커당 최대범위 1회 수집 후 캐시 (이후 슬라이스는 전부 캐시 히트) ──
    def _frame(self, ticker: str):
        import pandas as pd
        if ticker in self._mem:
            return self._mem[ticker]
        p = self._cache_path(ticker)
        if p.exists():
            try:
                df = pd.read_parquet(p)
                self._mem[ticker] = df
                return df
            except Exception:
                pass
        import yfinance as yf
        raw = yf.download(f"{ticker}{self._suffix}", start=self.HISTORY_START, end=self.HISTORY_END,
                          auto_adjust=True, progress=False, threads=False)
        if raw is None or len(raw) == 0:
            self._mem[ticker] = None
            return None
        # 단일 티커라도 최신 yfinance는 MultiIndex 컬럼을 줄 수 있어 평탄화
        if hasattr(raw.columns, "nlevels") and raw.columns.nlevels > 1:
            raw.columns = raw.columns.get_level_values(0)
        raw = raw.reset_index()
        rows = []
        for r in raw.itertuples(index=False):
            d = getattr(r, "Date", None)
            if d is None:
                continue
            ds = str(d)[:10]
            try:
                o = float(r.Open); h = float(r.High); lo = float(r.Low)
                c = float(r.Close); v = int(r.Volume) if r.Volume == r.Volume else 0
            except Exception:
                continue
            if c != c or c <= 0:   # NaN/이상치 스킵
                continue
            rows.append({"date": ds, "open": o, "high": h, "low": lo, "close": c, "volume": v})
        df = pd.DataFrame(rows)
        try:
            df.to_parquet(p)
        except Exception:
            pass
        self._mem[ticker] = df
        return df

    def get_historical_ohlcv(self, ticker: str, start: str, end: str) -> list[Bar]:
        df = self._frame(ticker)
        if df is None or len(df) == 0:
            return []
        out = []
        for r in df.itertuples(index=False):
            if start <= r.date <= end:
                out.append(Bar(r.date, r.open, r.high, r.low, r.close, int(r.volume)))
        return out

    def prefetch_ohlcv(self, tickers: list[str], start: str, end: str, workers: int = 8) -> None:
        from concurrent.futures import ThreadPoolExecutor
        todo = [t for t in tickers if t not in self._mem and not self._cache_path(t).exists()]
        if not todo:
            return
        print(f"[yfinance] 병렬 수집 {len(todo)}/{len(tickers)}종목 (workers={workers})...")
        with ThreadPoolExecutor(max_workers=workers) as ex:
            list(ex.map(self._frame, todo))

    # ── as-of 유니버스: 정적 후보를 그날 거래대금(종가×거래량)으로 랭킹 ──
    def universe_top(self, on_date: str, n: int, min_turnover: float = 1e8,
                     sizes: dict | None = None) -> list[str]:
        cands = universe_codes()
        self.prefetch_ohlcv(cands, self.HISTORY_START, on_date)
        scored = []
        for code in cands:
            df = self._frame(code)
            if df is None or len(df) == 0:
                continue
            # on_date 이하 마지막 봉 (as-of, look-ahead 차단)
            sub = df[df["date"] <= on_date]
            if len(sub) == 0:
                continue
            last = sub.iloc[-1]
            turnover = float(last["close"]) * float(last["volume"])
            if turnover < min_turnover:
                continue
            scored.append((turnover, code))
        scored.sort(reverse=True)
        k = (sizes or {}).get("KOSPI", n)
        return [c for _, c in scored[:k]]

    def ticker_name(self, code: str) -> str:
        return KOSPI_LARGE_MID.get(code, "")
