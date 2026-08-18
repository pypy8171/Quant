"""
지수/매크로 일봉 소스 — yfinance 기반(국면 판정용).

datagokr(getStockPriceInfo)는 *주식* 전용이라 ETF·지수·해외지표를 못 준다.
국면(regime) 신호로 쓸 지수는 여기서 받는다. 무료·전체 히스토리.

쓰는 티커(yfinance 심볼):
  ^KS11  코스피            ^KQ11  코스닥
  ^GSPC  S&P500           ^IXIC  나스닥 종합
  ^TNX   미국채 10년물 금리

캐시는 datagokr와 동일하게 parquet(.index_cache). date는 "YYYY-MM-DD" 문자열
(엔진의 Bar.date와 동일 포맷 — look-ahead 비교가 문자열 정렬로 성립).

⚠️ 시차 주의(W-2): ^KS11/^KQ11(한국 지수)은 한국 종목과 같은 거래일이라 엔진의
`b.date <= date`(당일 종가 판정) 비교가 정당하다. 그러나 ^GSPC/^IXIC/^TNX(미국)는
한국보다 시간대가 뒤라 "당일" 종가가 한국 장 마감 후에 확정된다 → 한국 종목 백테스트에서
같은 date로 비교하면 look-ahead 소지. 미국 지수를 regime으로 쓸 경우 엔진에서 전일(`< date`)
정렬로 바꿔야 한다(현재 엔진은 `<= date` 고정 — 한국 지수 전제).
"""
import os
import time
from pathlib import Path

from kis.client import Bar


class IndexSource:
    def __init__(self, cache_dir: str | None = None, sleep: float = 0.0):
        base = (cache_dir or os.environ.get("INDEX_CACHE_DIR")
                or Path(__file__).parents[1] / ".index_cache")
        self._cache = Path(base)
        self._cache.mkdir(parents=True, exist_ok=True)
        self.sleep = sleep

    def get_historical_ohlcv(self, ticker: str, start: str, end: str) -> list[Bar]:
        """지수 일봉을 [start, end] 범위로 반환(오름차순). 실패 시 빈 리스트."""
        import pandas as pd
        from datetime import date as _date, timedelta
        safe = ticker.replace("^", "_").replace("/", "_")
        # 캐시키에 _adj(auto_adjust=True) 표기 — 향후 옵션화 시 stale 충돌 방지(W-1).
        cache = self._cache / f"idx_{safe}_{start}_{end}_adj.parquet"
        try:
            if cache.exists():
                df = pd.read_parquet(cache)
                return [Bar(r.date, r.open, r.high, r.low, r.close, int(r.volume))
                        for r in df.itertuples()]
        except Exception:
            pass

        if os.environ.get("INDEX_OFFLINE"):
            raise RuntimeError(
                f"INDEX_OFFLINE — 캐시 미스로 네트워크 차단: {cache.name} "
                f"(재현성 게이트는 커밋된 캐시에서만 돈다; 먼저 온라인 워밍 필요)")

        try:
            import yfinance as yf
        except ModuleNotFoundError:
            # C-1: 설치 누락은 네트워크 실패와 다른 처방(pip install) → 명확히 구분해 전파.
            raise RuntimeError("yfinance 미설치 — 'pip install yfinance' 필요(지수 regime용).")
        try:
            # end는 배타상한 → 하루 더해 그날 포함. auto_adjust로 배당/분할 보정.
            end_excl = (_date.fromisoformat(end) + timedelta(days=1)).isoformat()
            df = yf.download(ticker, start=start, end=end_excl, interval="1d",
                             auto_adjust=True, progress=False, threads=False)
        except Exception as e:
            print(f"[index] {ticker} 다운로드 실패: {type(e).__name__}: {e}")
            return []
        if df is None or df.empty:
            print(f"[index] {ticker} 빈 응답")
            return []

        # yfinance 멀티인덱스 컬럼(단일 티커도 ('Close','^KS11') 형태일 수 있음) 평탄화
        if hasattr(df.columns, "nlevels") and df.columns.nlevels > 1:
            df.columns = df.columns.get_level_values(0)

        out: list[Bar] = []
        for idx, row in df.iterrows():
            d = idx.strftime("%Y-%m-%d")
            try:
                o = float(row["Open"]); h = float(row["High"])
                lo = float(row["Low"]); c = float(row["Close"])
                v = int(row["Volume"]) if not pd.isna(row.get("Volume", 0)) else 0
            except (KeyError, TypeError, ValueError):
                continue
            if c <= 0 or any(x != x for x in (o, h, lo, c)):  # NaN/0 종가 스킵
                continue
            out.append(Bar(d, o, h, lo, c, v))
        out.sort(key=lambda b: b.date)

        # W-1: end가 오늘 이후면 마지막 봉이 미완성(장중)일 수 있어 영구캐시 금지.
        # 또 yfinance는 과거 배당/분할을 소급조정하므로, 과거 확정구간만 캐시.
        try:
            if out and end < _date.today().isoformat():
                pd.DataFrame([b.__dict__ for b in out]).to_parquet(cache)
        except Exception:
            pass
        if self.sleep:
            time.sleep(self.sleep)
        return out
