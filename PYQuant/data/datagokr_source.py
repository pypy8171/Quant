"""
공공데이터포털(data.go.kr) 금융위원회 주식시세정보 기반 백테스트 데이터 소스.

KRX 데이터포털(pykrx)이 2025-12-27 회원제 전환 + 봇 차단으로 스냅샷/리스트가 막힌
것에 대한 합법·무료 대체. 데이터 출처는 KRX(금융위 재배포)라 출처가 떳떳하다.
([[data-source-constraints]] 참조)

쓰는 API: 금융위_주식시세정보 getStockPriceInfo (data.go.kr 15094808).
  - 종목·일자별 시/고/저/종가·거래량·거래대금·상장주식수·**시가총액** 제공 (2010~).
  - 이 하나로 (1) per-ticker OHLCV, (2) 특정일 전종목 시총 랭킹 = point-in-time
    유니버스 둘 다 해결. universe_top(on_date)은 임의 과거날짜의 시총상위를 주므로
    point-in-time 조회가 가능하다. 단 완전한 survivorship-free는 리밸런싱마다 그날로
    재조회해야 성립하며, 현재 main.py는 시작일 1회만 호출(=as-of 시작일 고정)한다.

인증키: 환경변수 DATA_GO_KR_KEY (또는 생성자 인자). data.go.kr 마이페이지의
  '일반 인증키'를 사용. Encoding/Decoding 어느 형태든 허용(내부에서 unquote 후
  requests가 재인코딩).

엔진 계약: get_historical_ohlcv / authenticate (+ universe_top / ticker_name).
주의: 갱신은 기준일+1영업일 13시 이후 — 당일 데이터는 다음날 나온다(백테스트엔 무관).
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date as _date, timedelta
from pathlib import Path
from urllib.parse import unquote
import os
import time

from kis.client import Bar

BASE_URL = "https://apis.data.go.kr/1160100/service/GetStockSecuritiesInfoService/getStockPriceInfo"


def _ymd(iso: str) -> str:
    return iso.replace("-", "")


def _f(v) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def _i(v) -> int:
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


class DataGoKrSource:
    def __init__(self, service_key: str | None = None, market: str = "KOSPI",
                 cache_dir: str | None = None, sleep: float = 0.05):
        key = service_key or os.environ.get("DATA_GO_KR_KEY") or ""
        self.service_key = unquote(key)   # Encoding/Decoding 키 모두 수용
        self.market = market
        self.sleep = sleep
        self.adjust_prices = True   # 수정주가 보정(분할/병합) on. False면 원주가
        # 엔진이 종목 루프에서 참조하는 추가 throttle — prefetch+캐시로 0 (KrxSource는 미정의→0.3)
        self.fetch_sleep = 0.0
        base = cache_dir or os.environ.get("KRX_CACHE_DIR") \
            or str(Path(__file__).resolve().parents[1] / ".datagokr_cache")
        self._cache = Path(base)
        self._cache.mkdir(parents=True, exist_ok=True)
        self._name_cache: dict[str, str] = {}
        self._dumped_once = False   # 첫 응답 원본 필드명 1회 출력(스펙 검증용)

    def authenticate(self) -> bool:
        if not self.service_key:
            print("[datagokr] DATA_GO_KR_KEY 환경변수(또는 service_key 인자) 없음")
            return False
        return True

    # ── 저수준 호출 (페이지네이션) ───────────────────────────────────────────
    def _fetch_pages(self, params: dict, max_rows: int = 100_000) -> list[dict]:
        import requests
        items: list[dict] = []
        page = 1
        per = 1000
        while len(items) < max_rows:
            q = dict(params)
            q.update({
                "serviceKey": self.service_key,
                "resultType": "json",
                "numOfRows": per,
                "pageNo": page,
            })
            data = None
            for attempt in range(4):   # 502/일시 네트워크 오류 재시도 (지수 백오프)
                try:
                    r = requests.get(BASE_URL, params=q, timeout=20)
                    data = r.json()
                    break
                except Exception as e:                   # noqa: BLE001 (502는 HTML→JSONDecodeError)
                    if attempt == 3:
                        print(f"[datagokr] 요청/파싱 실패 page={page} (재시도 4회 소진): "
                              f"{type(e).__name__}: {e}")
                        try:
                            print("  raw:", r.text[:200])
                        except Exception:
                            pass
                    else:
                        time.sleep(0.5 * (attempt + 1))   # 0.5s, 1.0s, 1.5s 백오프
            if data is None:
                break
            body = (data.get("response", {}) or {}).get("body", {}) or {}
            raw = (body.get("items", {}) or {}).get("item", []) or []
            if isinstance(raw, dict):   # 단일 결과는 dict로 올 수 있음
                raw = [raw]
            if not self._dumped_once and raw:
                self._dumped_once = True
                print(f"[datagokr] (스펙확인) 첫 응답 필드: {sorted(raw[0].keys())}")
            items.extend(raw)
            total = _i(body.get("totalCount", 0))
            if not raw or len(items) >= total or len(raw) < per:
                break
            page += 1
            time.sleep(self.sleep)
        return items

    @staticmethod
    def _code6(item: dict) -> str:
        # srtnCd: 보통 6자리(때로 'A005930'/긴 형식) → 숫자 6자리 추출
        for k in ("srtnCd", "shortCd", "isinCd"):
            v = str(item.get(k, "") or "")
            digits = "".join(ch for ch in v if ch.isdigit())
            if len(digits) >= 6:
                return digits[-6:] if k != "isinCd" else digits[3:9]
        return ""

    @staticmethod
    def _bar(item: dict) -> Bar | None:
        bd = str(item.get("basDt", "") or "")
        if len(bd) != 8:
            return None
        return Bar(
            date   = f"{bd[:4]}-{bd[4:6]}-{bd[6:]}",
            open   = _f(item.get("mkp")),
            high   = _f(item.get("hipr")),
            low    = _f(item.get("lopr")),
            close  = _f(item.get("clpr")),
            volume = _i(item.get("trqu")),
        )

    @staticmethod
    def _adjust_splits(rows: list[dict]) -> list[Bar]:
        """액면분할/병합을 **가격 갭**으로 감지해 과거 가격 역보정(수정주가).
        rows: [{date,o,h,l,c,v,shares}] 날짜오름차순. 한국은 일일 가격제한 ±30%라
        '시가/전일종가'가 ±40%를 넘는 하룻밤 갭은 정상 거래로 불가능 = 액면분할/병합/병합
        등 기업행위다(거래정지 후 재개 드문 예외). 그 갭일이 곧 정확한 분할일이므로,
        그 이전 봉 가격을 갭 배율의 미래 누적곱으로 보정한다(주식수 갱신일 misalignment
        문제 회피). 주식수(shares)는 보조 확인용으로만 둔다(현재 미사용).
          예: 5:1 분할 → 시가/전일종가=0.2(step) → 분할 전 가격 ×0.2, 거래량 ÷0.2."""
        n = len(rows)
        if n == 0:
            return []
        LO, HI, WIN = 0.66, 1.45, 10   # ±30% 가격제한 바깥=기업행위 후보, 주식수확인 윈도우
        step = [1.0] * n      # step[i] = i일 가격이 점프한 배율(시가/전일종가)
        for i in range(1, n):
            pc, op = rows[i-1]["c"], rows[i]["o"]
            if pc <= 0 or op <= 0:
                continue
            g = op / pc
            if not (g <= LO or g >= HI):
                continue
            # 분할 확정 조건: 가격갭 ±WIN거래일 내 상장주식수(±20%↑) 변동 동반.
            # 거래정지 재개·단발 이상치 갭은 주식수 불변이므로 보정 제외(C-1 오인 차단).
            a = rows[max(0, i - WIN)]["shares"]
            b = rows[min(n - 1, i + WIN)]["shares"]
            if a > 0 and b > 0 and (b / a >= 1.2 or b / a <= 1 / 1.2):
                step[i] = g
        fut = [1.0] * n       # fut[i] = ∏ step[j] for j>i (i 이후 기업행위 누적배율)
        prod = 1.0
        for i in range(n-1, -1, -1):
            fut[i] = prod
            prod *= step[i]
        out: list[Bar] = []
        for i, rw in enumerate(rows):
            f = fut[i] if fut[i] > 0 else 1.0
            out.append(Bar(
                date   = rw["date"],
                open   = rw["o"] * f,
                high   = rw["h"] * f,
                low    = rw["l"] * f,
                close  = rw["c"] * f,
                volume = int(rw["v"] / f) if f > 0 else rw["v"],
            ))
        return out

    # ── per-ticker OHLCV (수정주가 보정 포함) ────────────────────────────────
    def get_historical_ohlcv(self, ticker: str, start: str, end: str) -> list[Bar]:
        import pandas as pd
        suffix = "" if self.adjust_prices else "_raw"
        cache = self._cache / f"ohlcv_{ticker}_{_ymd(start)}_{_ymd(end)}{suffix}.parquet"
        try:
            if cache.exists():
                df = pd.read_parquet(cache)
                return [Bar(r.date, r.open, r.high, r.low, r.close, int(r.volume))
                        for r in df.itertuples()]
        except Exception:
            pass
        items = self._fetch_pages({
            "beginBasDt": _ymd(start),
            "endBasDt":   _ymd(end),
            "likeSrtnCd": ticker,   # LIKE 매칭 → 아래서 정확 코드만 필터
        })
        rows: list[dict] = []
        for it in items:
            if self._code6(it) != ticker:
                continue
            bd = str(it.get("basDt", "") or "")
            if len(bd) != 8:
                continue
            d = f"{bd[:4]}-{bd[4:6]}-{bd[6:]}"
            if not (start <= d <= end):
                continue
            rows.append({"date": d, "o": _f(it.get("mkp")), "h": _f(it.get("hipr")),
                         "l": _f(it.get("lopr")), "c": _f(it.get("clpr")),
                         "v": _i(it.get("trqu")), "shares": _f(it.get("lstgStCnt"))})
        rows.sort(key=lambda r: r["date"])
        # 중복 제거(같은 날짜)
        seen, dedup = set(), []
        for r in rows:
            if r["date"] not in seen:
                seen.add(r["date"]); dedup.append(r)
        out = (self._adjust_splits(dedup) if self.adjust_prices
               else [Bar(r["date"], r["o"], r["h"], r["l"], r["c"], int(r["v"])) for r in dedup])
        try:
            if out:
                pd.DataFrame([b.__dict__ for b in out]).to_parquet(cache)
        except Exception:
            pass
        time.sleep(self.sleep)
        return out

    # ── 병렬 prefetch (엔진이 루프 돌기 전 1회 호출) ────────────────────────
    def prefetch_ohlcv(self, tickers: list[str], start: str, end: str,
                       workers: int = 10) -> None:
        """미캐시 종목을 스레드풀로 동시 수집해 parquet 캐시를 채운다.
        이후 엔진의 per-ticker get_historical_ohlcv는 캐시 히트로 즉시 반환."""
        from concurrent.futures import ThreadPoolExecutor
        sfx = "" if self.adjust_prices else "_raw"
        todo = [t for t in tickers
                if not (self._cache / f"ohlcv_{t}_{_ymd(start)}_{_ymd(end)}{sfx}.parquet").exists()]
        if not todo:
            return
        print(f"[datagokr] 병렬 수집 {len(todo)}/{len(tickers)}종목 (workers={workers})...")
        with ThreadPoolExecutor(max_workers=workers) as ex:
            for _ in ex.map(lambda t: self.get_historical_ohlcv(t, start, end), todo):
                pass

    # ── 특정일 전종목 스냅샷 (전 시장, 날짜별 캐시) ──────────────────────────
    def _snapshot(self, on_date: str) -> list[dict]:
        import pandas as pd
        d = _date.fromisoformat(on_date)
        for _ in range(7):   # 휴장일 백오프
            ymd = d.strftime("%Y%m%d")
            cache = self._cache / f"univ_{ymd}.parquet"
            try:
                if cache.exists():
                    return pd.read_parquet(cache).to_dict("records")
            except Exception:
                pass
            items = self._fetch_pages({"basDt": ymd}, max_rows=10_000)
            rows = [{
                "code":     self._code6(it),
                "name":     str(it.get("itmsNm", "") or ""),
                "market":   str(it.get("mrktCtg", "") or ""),
                "mktcap":   _f(it.get("mrktTotAmt")),
                "turnover": _f(it.get("trPrc")),
            } for it in items if self._code6(it)]
            if rows:
                try:
                    pd.DataFrame(rows).to_parquet(cache)
                except Exception:
                    pass
                return rows
            d -= timedelta(days=1)
        return []

    # ── point-in-time 유니버스: 특정일 시총 상위 N (시장별) ──────────────────
    def universe_top(self, on_date: str, n: int, min_turnover: float = 1e9,
                     sizes: dict | None = None) -> list[str]:
        """self.market: 'KOSPI'|'KOSDAQ'|'ALL'. ALL이면 시장별 상위를 각각 뽑아 합침.
        sizes={'KOSPI':200,'KOSDAQ':100} 로 시장별 종목수 개별 지정 가능(없으면 n 공통)."""
        rows = self._snapshot(on_date)
        if not rows:
            return []
        for r in rows:   # 종목명 캐시
            if r["code"] not in self._name_cache and r["name"]:
                self._name_cache[r["code"]] = r["name"]
        markets = ["KOSPI", "KOSDAQ"] if self.market == "ALL" else [self.market]
        out: list[str] = []
        for mk in markets:
            k = (sizes or {}).get(mk, n)
            pool = [r for r in rows if r["market"] == mk
                    and r["turnover"] >= min_turnover and r["mktcap"] > 0]
            pool.sort(key=lambda r: r["mktcap"], reverse=True)
            out += [r["code"] for r in pool[:k]]
        return out

    def ticker_name(self, code: str) -> str:
        return self._name_cache.get(code, "")
