"""
KIS (한국투자증권) REST API 클라이언트
C++ KisClient와 동일한 기능을 Python으로 구현
"""
import json
import os
import stat
import time
import requests
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional

from core.logger import setup_logger

logger = setup_logger("quant.kis")


class KisAuthError(Exception):
    """KIS 인증 실패 — app_key/app_secret 오류 또는 네트워크 문제"""


@dataclass
class Bar:
    date:   str
    open:   float
    high:   float
    low:    float
    close:  float
    volume: int


@dataclass
class Fundamentals:
    ticker:      str
    last:        float = 0.0   # 현재가
    diff:        float = 0.0   # 전일대비
    rate:        float = 0.0   # 등락률(%)
    pbr:         float = 0.0
    per:         float = 0.0
    open:        float = 0.0
    high:        float = 0.0
    low:         float = 0.0


@dataclass
class IndexPrice:
    code:        str
    price:       float = 0.0   # bstp_nmix_prpr (현재 지수)
    change:      float = 0.0   # bstp_nmix_prdy_vrss (전일대비)
    change_rate: float = 0.0   # bstp_nmix_prdy_ctrt (전일종가 대비 %) — 장중이동 아님 주의
    sign:        int   = 3     # prdy_vrss_sign (1상한 2상승 3보합 4하한 5하락)
    ok:          bool  = False


@dataclass
class OrderSignal:
    ticker:   str
    side:     str          # "BUY" | "SELL"
    quantity: int
    order_type: str = "MARKET"   # "MARKET" | "LIMIT"
    price:    float = 0.0


@dataclass
class OrderResult:
    ok:     bool
    odno:   str = ""   # KIS 주문번호 (접수 시에만)
    rt_cd:  str = ""   # KIS 응답코드 ("0"=성공)
    msg_cd: str = ""   # KIS 메시지코드
    msg1:   str = ""   # KIS 응답 메시지


@dataclass
class BalanceItem:
    ticker:        str
    name:          str
    quantity:      int
    avg_price:     float
    current_price: float
    eval_amount:   float
    pnl:           float
    pnl_rate:      float


@dataclass
class AccountSummary:
    cash:           float   # 예수금
    total_eval:     float   # 총평가금액
    total_pnl:      float   # 총손익금액
    total_pnl_rate: float   # 총수익률(%)


class KisClient:
    REAL_URL  = "https://openapi.koreainvestment.com:9443"
    PAPER_URL = "https://openapivts.koreainvestment.com:29443"

    def __init__(self, app_key: str, app_secret: str,
                 account_no: str, account_type: str = "01",
                 is_paper: bool = False):
        self.app_key      = app_key
        self.app_secret   = app_secret
        self.account_no   = account_no
        self.account_type = account_type
        self.is_paper     = is_paper
        self.base_url     = self.PAPER_URL if is_paper else self.REAL_URL
        self._token: Optional[str] = None
        self._token_exp: float = 0.0

    # ── 인증 ────────────────────────────────────────────────────────────────
    @property
    def _cache_path(self) -> Path:
        # 엔진(C++)과 공유: KIS_TOKEN_CACHE_DIR(docker 공유 볼륨) 우선, 없으면 host 기본.
        # 파일명 kis_token_<app_key[:8]>.json — C++ KisClient와 동일 규약(실/모의 분리 + 캐시 공유)
        cache_dir = os.environ.get("KIS_TOKEN_CACHE_DIR")
        base = Path(cache_dir) if cache_dir else (Path(__file__).parents[2] / "Quant" / "config")
        return base / f"kis_token_{self.app_key[:8]}.json"

    def _load_cached_token(self) -> bool:
        try:
            with open(self._cache_path, encoding="utf-8") as f:
                data = json.load(f)
            # 공유 포맷 {"access_token", "expires_at": "YYYY-MM-DD HH:MM:SS"} (C++ 엔진과 동일).
            # 파일명에 app_key[:8]가 들어가 계좌 분리 — 별도 app_key 필드 검증 불필요.
            exp_str = data.get("expires_at", "")
            exp = datetime.strptime(exp_str, "%Y-%m-%d %H:%M:%S").timestamp()
            if time.time() < exp - 300:
                self._token     = data["access_token"]
                self._token_exp = exp
                logger.info(f"캐시 토큰 사용 (만료: {exp_str})")
                return True
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as e:
            logger.debug(f"토큰 캐시 로드 실패: {e}")
        return False

    def _save_token_cache(self, access_token: str, expired_str: str):
        try:
            self._cache_path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self._cache_path.with_suffix(".tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                # C++ 엔진과 공유 가능한 포맷 (expires_at = KIS ISO "YYYY-MM-DD HH:MM:SS")
                json.dump({
                    "access_token": access_token,
                    "expires_at":   expired_str,
                }, f)
                f.flush()
                os.fsync(f.fileno())   # durability — os.replace 전 디스크 반영 (V-2)
            # 소유자만 읽기/쓰기 (0600) — Windows는 ACL 모델이 달라 무시됨
            if os.name != "nt":
                os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
            os.replace(tmp, self._cache_path)  # 원자적 교체 (C-2) — 읽는 쪽이 완전한 JSON만 봄
        except OSError as e:
            logger.warning(f"토큰 캐시 저장 실패: {e}")

    def authenticate(self) -> bool:
        if self._load_cached_token():
            return True
        url  = f"{self.base_url}/oauth2/tokenP"
        body = {
            "grant_type": "client_credentials",
            "appkey":     self.app_key,
            "appsecret":  self.app_secret,
        }
        last_err = None
        for attempt in range(3):   # KIS(특히 모의) 서버 지연 대비 재시도(타임아웃 30s)
            try:
                r = requests.post(url, json=body, timeout=30)
                r.raise_for_status()
                data = r.json()
                self._token     = data["access_token"]
                self._token_exp = time.time() + int(data.get("expires_in", 86400))
                expired_str     = data.get("access_token_token_expired", "?")
                self._save_token_cache(self._token, expired_str)
                logger.info(f"인증 성공 (만료: {expired_str})")
                return True
            except requests.RequestException as e:
                last_err = e
                if attempt < 2:
                    time.sleep(2 * (attempt + 1))   # 2s, 4s 백오프
            except (KeyError, ValueError) as e:
                logger.error(f"인증 응답 파싱 실패: {e}")
                return False
        logger.error(f"인증 네트워크 오류(재시도 3회 소진): {last_err}")
        return False

    def _ensure_token(self):
        if not self._token or time.time() > self._token_exp - 300:
            if not self.authenticate():
                raise KisAuthError("토큰 발급 실패 — app_key/app_secret 또는 네트워크 확인 필요")

    @staticmethod
    def _mask(s: str) -> str:
        if not s or len(s) <= 8:
            return "***"
        return s[:4] + "****" + s[-4:]

    def _headers(self, tr_id: str) -> dict:
        """헤더 생성. appsecret을 포함하므로 절대 직접 로깅하지 말 것 — _mask() 사용."""
        self._ensure_token()
        if not self._token:
            raise KisAuthError("토큰이 없습니다")
        return {
            "authorization": f"Bearer {self._token}",
            "appkey":        self.app_key,
            "appsecret":     self.app_secret,
            "tr_id":         tr_id,
            "Content-Type":  "application/json",
        }

    def _get(self, path: str, params: dict, tr_id: str) -> dict:
        last = None
        for attempt in range(3):   # KIS(특히 모의) 지연 대비 재시도, 타임아웃 20s
            try:
                r = requests.get(self.base_url + path, params=params,
                                 headers=self._headers(tr_id), timeout=20)
                return r.json()
            except requests.RequestException as e:
                last = e
                if attempt < 2:
                    time.sleep(1.5 * (attempt + 1))
            except ValueError as e:
                logger.error(f"GET 응답 JSON 파싱 실패 {path}: {e}")
                return {}
        logger.error(f"GET 네트워크 오류(재시도 3회) {path}: {last}")
        return {}

    def _post(self, path: str, body: dict, tr_id: str) -> dict:
        # ⚠️ 자동 재시도 없음 — _post는 주문(send_order) 등 상태변경에 쓰여,
        #    타임아웃 후 재시도하면 이중주문 위험. 타임아웃만 20s로 완화.
        try:
            r = requests.post(self.base_url + path, json=body,
                              headers=self._headers(tr_id), timeout=20)
            return r.json()
        except requests.RequestException as e:
            logger.error(f"POST 네트워크 오류 {path}: {e}")
            return {}
        except ValueError as e:
            logger.error(f"POST 응답 JSON 파싱 실패 {path}: {e}")
            return {}

    # ── 현재가 + PBR/PER ────────────────────────────────────────────────────
    def get_fundamentals(self, ticker: str) -> Fundamentals:
        data = self._get(
            "/uapi/domestic-stock/v1/quotations/inquire-price",
            {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": ticker},
            "FHKST01010100",
        )
        f = Fundamentals(ticker=ticker)
        out = data.get("output", {})
        def d(k):
            try:
                return float(out.get(k, 0) or 0)
            except (ValueError, TypeError):
                return 0.0
        f.last = d("stck_prpr")
        f.diff = d("prdy_vrss")
        f.rate = d("prdy_ctrt")
        f.open = d("stck_oprc")
        f.high = d("stck_hgpr")
        f.low  = d("stck_lwpr")
        f.pbr  = d("pbr")
        f.per  = d("per")
        return f

    # ── 국내 지수 현재가 (Track B 장중국면 폴링용) ────────────────────────────
    def get_index_price(self, code: str) -> IndexPrice:
        """국내 지수 현재가. C++ KisClient::get_index_price 미러 (TR FHPUP02100000,
        FID_COND_MRKT_DIV_CODE=U). code: "0001"=코스피종합 "1001"=코스닥종합 "2001"=KOSPI200.
        change_rate(prdy_ctrt)는 전일종가 대비라 장중이동(시가 대비)엔 못 씀 — price 시계열로 파생할 것."""
        data = self._get(
            "/uapi/domestic-stock/v1/quotations/inquire-index-price",
            {"FID_COND_MRKT_DIV_CODE": "U", "FID_INPUT_ISCD": code},
            "FHPUP02100000",
        )
        o = data.get("output", {}) or {}
        def d(k):
            try:
                return float(o.get(k, 0) or 0)
            except (ValueError, TypeError):
                return 0.0
        ip = IndexPrice(code=code)
        ip.price       = d("bstp_nmix_prpr")
        ip.change      = d("bstp_nmix_prdy_vrss")
        ip.change_rate = d("bstp_nmix_prdy_ctrt")
        try:
            ip.sign = int(o.get("prdy_vrss_sign", 3) or 3)
        except (ValueError, TypeError):
            ip.sign = 3
        ip.ok = ip.price > 0
        return ip

    # ── 최근 일봉 OHLCV (최근 ~30봉, 라이브용) ─────────────────────────────
    def get_daily_ohlcv(self, ticker: str, count: int = 30) -> list[Bar]:
        data = self._get(
            "/uapi/domestic-stock/v1/quotations/inquire-daily-price",
            {
                "FID_COND_MRKT_DIV_CODE": "J",
                "FID_INPUT_ISCD":         ticker,
                "FID_PERIOD_DIV_CODE":    "D",
                "FID_ORG_ADJ_PRC":        "0",  # "0"=수정주가, "1"=원주가. 분할이력 종목으로 실측 검증 필요
            },
            "FHKST01010400",
        )
        bars = []
        for item in data.get("output2", []):
            try:
                raw = item.get("stck_bsop_date", "")
                date = f"{raw[:4]}-{raw[4:6]}-{raw[6:]}" if len(raw) == 8 else raw
                bars.append(Bar(
                    date   = date,
                    open   = float(item.get("stck_oprc", 0) or 0),
                    high   = float(item.get("stck_hgpr", 0) or 0),
                    low    = float(item.get("stck_lwpr", 0) or 0),
                    close  = float(item.get("stck_clpr", 0) or 0),
                    volume = int(item.get("acml_vol", 0) or 0),
                ))
            except (ValueError, KeyError, TypeError) as e:
                logger.warning(f"bar 파싱 실패 ticker={ticker} item={item}: {e}")
                continue
        bars.reverse()
        return bars[:count]

    # ── 날짜 범위 일봉 OHLCV (백테스팅용) ──────────────────────────────────
    def get_historical_ohlcv(self, ticker: str, start_date: str, end_date: str) -> list[Bar]:
        from datetime import date as _date, timedelta
        start_ymd = start_date.replace("-", "")
        all_bars: list[Bar] = []
        current_end = end_date.replace("-", "")

        for _ in range(10):  # 최대 10페이지 (~1000 거래일)
            data = self._get(
                "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice",
                {
                    "FID_COND_MRKT_DIV_CODE": "J",
                    "FID_INPUT_ISCD":         ticker,
                    "FID_INPUT_DATE_1":        start_ymd,
                    "FID_INPUT_DATE_2":        current_end,
                    "FID_PERIOD_DIV_CODE":     "D",
                    "FID_ORG_ADJ_PRC":         "0",  # "0"=수정주가, "1"=원주가. 분할이력 종목으로 실측 검증 필요
                },
                "FHKST03010100",
            )
            items = data.get("output2", [])
            if not items:
                break
            batch: list[Bar] = []
            for item in items:
                try:
                    raw = item.get("stck_bsop_date", "")
                    bar_date = f"{raw[:4]}-{raw[4:6]}-{raw[6:]}" if len(raw) == 8 else raw
                    batch.append(Bar(
                        date   = bar_date,
                        open   = float(item.get("stck_oprc", 0) or 0),
                        high   = float(item.get("stck_hgpr", 0) or 0),
                        low    = float(item.get("stck_lwpr", 0) or 0),
                        close  = float(item.get("stck_clpr", 0) or 0),
                        volume = int(item.get("acml_vol", 0) or 0),
                    ))
                except (ValueError, KeyError, TypeError) as e:
                    logger.warning(f"bar 파싱 실패 ticker={ticker} item={item}: {e}")
                    continue
            all_bars.extend(batch)
            if len(items) < 100:
                break
            dated = [b.date for b in batch if b.date >= start_date]
            if not dated:
                break
            oldest = min(dated)
            if oldest <= start_date:
                break
            current_end = (_date.fromisoformat(oldest) - timedelta(days=1)).strftime("%Y%m%d")

        seen: set[str] = set()
        result: list[Bar] = []
        for b in sorted(all_bars, key=lambda b: b.date):
            if b.date not in seen:
                seen.add(b.date)
                result.append(b)
        return result

    # ── 시총 상위 Universe 조회 ──────────────────────────────────────────────
    def fetch_universe(self, market: str = "J", max_pbr: float = 999.0) -> list[str]:
        data = self._get(
            "/uapi/domestic-stock/v1/ranking/market-cap",
            {
                "FID_COND_MRKT_DIV_CODE": market,
                "FID_COND_SCR_DIV_CODE":  "20171",
                "FID_INPUT_ISCD":         "0000",
                "FID_DIV_CLS_CODE":       "0",
                "FID_TRGT_CLS_CODE":      "0",
                "FID_RANK_SORT_CLS_CODE": "0",
                "FID_INPUT_CNT_1":        "0",
                "FID_PBLICATN_NM":        "",
            },
            "FHPST01720000",
        )
        result = []
        for item in data.get("output2", []):
            ticker = item.get("mksc_shrn_iscd", "")
            if not ticker:
                continue
            try:
                pbr = float(item.get("hts_pbr", 0) or 0)
                if pbr > 0 and pbr > max_pbr:
                    continue
            except (ValueError, TypeError) as e:
                logger.warning(f"PBR 파싱 실패 ticker={ticker}: {e}")
            result.append(ticker)
        return result

    # ── 차트용 OHLCV (일/주봉 + 분봉) ─────────────────────────────────────────
    def get_chart_ohlcv(self, ticker: str, period: str = "D", count: int = 120) -> list[dict]:
        """일봉("D")/주봉("W")/월봉("M") 차트용. TR FHKST03010100.
        반환: [{date, open, high, low, close, volume}] (오래된→최신 순)."""
        from datetime import date as _date, timedelta
        span_days = {"D": count * 2 + 10, "W": count * 8 + 30, "M": count * 34 + 60}.get(period, count * 2 + 10)
        end = _date.today()
        start = end - timedelta(days=span_days)
        rows: list[dict] = []
        cur_end = end.strftime("%Y%m%d")
        start_ymd = start.strftime("%Y%m%d")
        for _ in range(6):
            data = self._get(
                "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice",
                {
                    "FID_COND_MRKT_DIV_CODE": "J",
                    "FID_INPUT_ISCD":         ticker,
                    "FID_INPUT_DATE_1":       start_ymd,
                    "FID_INPUT_DATE_2":       cur_end,
                    "FID_PERIOD_DIV_CODE":    period,
                    "FID_ORG_ADJ_PRC":        "0",
                },
                "FHKST03010100",
            )
            items = data.get("output2", []) or []
            if not items:
                break
            for it in items:
                try:
                    raw = it.get("stck_bsop_date", "")
                    d = f"{raw[:4]}-{raw[4:6]}-{raw[6:]}" if len(raw) == 8 else raw
                    if not it.get("stck_clpr"):
                        continue
                    rows.append({
                        "date":   d,
                        "open":   float(it.get("stck_oprc", 0) or 0),
                        "high":   float(it.get("stck_hgpr", 0) or 0),
                        "low":    float(it.get("stck_lwpr", 0) or 0),
                        "close":  float(it.get("stck_clpr", 0) or 0),
                        "volume": int(it.get("acml_vol", 0) or 0),
                    })
                except (ValueError, TypeError):
                    continue
            if len(items) < 100:
                break
            oldest = min((r["date"] for r in rows), default=None)
            if not oldest or oldest.replace("-", "") <= start_ymd:
                break
            cur_end = (_date.fromisoformat(oldest) - timedelta(days=1)).strftime("%Y%m%d")
        seen, out = set(), []
        for r in sorted(rows, key=lambda x: x["date"]):
            if r["date"] not in seen:
                seen.add(r["date"])
                out.append(r)
        return out[-count:]

    def get_minute_ohlcv(self, ticker: str, count: int = 150) -> list[dict]:
        """당일 1분봉. TR FHKST03010200 (inquire-time-itemchartprice). 여러 번 페이지네이션.
        반환: [{time("HHMM"), open, high, low, close, volume}] (오래된→최신 순). 3/5분봉은 서버에서 리샘플."""
        rows: dict[str, dict] = {}
        hour1 = ""   # 빈 값=최신부터
        for _ in range(max(1, count // 30 + 1)):
            data = self._get(
                "/uapi/domestic-stock/v1/quotations/inquire-time-itemchartprice",
                {
                    "FID_ETC_CLS_CODE":       "",
                    "FID_COND_MRKT_DIV_CODE": "J",
                    "FID_INPUT_ISCD":         ticker,
                    "FID_INPUT_HOUR_1":       hour1,
                    "FID_PW_DATA_INCU_YN":    "N",
                },
                "FHKST03010200",
            )
            items = data.get("output2", []) or []
            if not items:
                break
            times = []
            for it in items:
                try:
                    hms = it.get("stck_cntg_hour", "")
                    if not hms or not it.get("stck_prpr"):
                        continue
                    times.append(hms)
                    rows[hms] = {
                        "time":   hms[:4],
                        "hms":    hms,
                        "open":   float(it.get("stck_oprc", 0) or 0),
                        "high":   float(it.get("stck_hgpr", 0) or 0),
                        "low":    float(it.get("stck_lwpr", 0) or 0),
                        "close":  float(it.get("stck_prpr", 0) or 0),
                        "volume": int(it.get("cntg_vol", 0) or 0),
                    }
                except (ValueError, TypeError):
                    continue
            if not times or len(rows) >= count:
                break
            earliest = min(times)
            # 다음 페이지: 가장 이른 시각 1초 전부터
            try:
                nxt = int(earliest) - 1
                hour1 = f"{nxt:06d}"
            except ValueError:
                break
        out = [rows[k] for k in sorted(rows.keys())]
        return out[-count:]

    # ── 거래대금 상위 랭킹 (대시보드 스냅샷용) ────────────────────────────────
    def get_volume_ranking(self, top_n: int = 30, by_value: bool = True) -> list[dict]:
        """국내주식 거래량/거래대금 순위 (TR FHPST01710000). 코스콤 실시간이 아닌
        REST 스냅샷이다. by_value=True면 거래금액순(FID_BLNG_CLS_CODE=3), False면 거래량순(0).
        반환: [{rank, ticker, name, price, change_rate, volume, trade_value}] (trade_value=누적거래대금 원)."""
        data = self._get(
            "/uapi/domestic-stock/v1/quotations/volume-rank",
            {
                "FID_COND_MRKT_DIV_CODE": "J",
                "FID_COND_SCR_DIV_CODE":  "20171",
                "FID_INPUT_ISCD":         "0000",
                "FID_DIV_CLS_CODE":       "0",
                "FID_BLNG_CLS_CODE":      "3" if by_value else "0",
                "FID_TRGT_CLS_CODE":      "111111111",
                "FID_TRGT_EXLS_CLS_CODE": "0000000000",
                "FID_INPUT_PRICE_1":      "",
                "FID_INPUT_PRICE_2":      "",
                "FID_VOL_CNT":            "",
                "FID_INPUT_DATE_1":       "",
            },
            "FHPST01710000",
        )
        rows: list[dict] = []
        for item in data.get("output", [])[:top_n]:
            try:
                rows.append({
                    "rank":        int(item.get("data_rank", 0) or 0),
                    "ticker":      item.get("mksc_shrn_iscd", ""),
                    "name":        item.get("hts_kor_isnm", ""),
                    "price":       float(item.get("stck_prpr", 0) or 0),
                    "change_rate": float(item.get("prdy_ctrt", 0) or 0),
                    "volume":      int(item.get("acml_vol", 0) or 0),
                    "trade_value": float(item.get("acml_tr_pbmn", 0) or 0),
                })
            except (ValueError, TypeError):
                continue
        return rows

    # ── 잔고 조회 ────────────────────────────────────────────────────────────
    def get_kr_balance(self) -> tuple[list[BalanceItem], AccountSummary]:
        tr_id = "VTTC8434R" if self.is_paper else "TTTC8434R"
        data = self._get(
            "/uapi/domestic-stock/v1/trading/inquire-balance",
            {
                "CANO":                 self.account_no,
                "ACNT_PRDT_CD":         self.account_type,
                "AFHR_FLPR_YN":         "N",
                "OFL_YN":               "",
                "INQR_DVSN":            "02",
                "UNPR_DVSN":            "01",
                "FUND_STTL_ICLD_YN":    "N",
                "FNCG_AMT_AUTO_RDPT_YN":"N",
                "PRCS_DVSN":            "01",
                "CTX_AREA_FK100":       "",
                "CTX_AREA_NK100":       "",
            },
            tr_id,
        )
        items: list[BalanceItem] = []
        for item in data.get("output1", []):
            try:
                qty = int(item.get("hldg_qty", 0) or 0)
                if qty == 0:
                    continue
                items.append(BalanceItem(
                    ticker        = item.get("pdno", ""),
                    name          = item.get("prdt_name", ""),
                    quantity      = qty,
                    avg_price     = float(item.get("pchs_avg_pric", 0) or 0),
                    current_price = float(item.get("prpr", 0) or 0),
                    eval_amount   = float(item.get("evlu_amt", 0) or 0),
                    pnl           = float(item.get("evlu_pfls_amt", 0) or 0),
                    pnl_rate      = float(item.get("evlu_pfls_rt", 0) or 0),
                ))
            except (ValueError, TypeError):
                continue

        out2 = data.get("output2", [{}])
        r = out2[0] if out2 else {}
        summary = AccountSummary(
            cash           = float(r.get("dnca_tot_amt", 0) or 0),
            total_eval     = float(r.get("tot_evlu_amt", 0) or 0),
            total_pnl      = float(r.get("evlu_pfls_smtl_amt", 0) or 0),
            total_pnl_rate = float(r.get("evlu_erng_rt", 0) or 0),
        )
        return items, summary

    # ── 주문 실행 ────────────────────────────────────────────────────────────
    def send_order(self, signal: OrderSignal) -> OrderResult:
        tr_id = ("VTTC0802U" if self.is_paper else "TTTC0802U") \
                if signal.side == "BUY" \
                else ("VTTC0801U" if self.is_paper else "TTTC0801U")

        body = {
            "CANO":         self.account_no,
            "ACNT_PRDT_CD": self.account_type,
            "PDNO":         signal.ticker,
            "ORD_DVSN":     "01" if signal.order_type == "MARKET" else "00",
            "ORD_QTY":      str(signal.quantity),
            "ORD_UNPR":     "0" if signal.order_type == "MARKET"
                            else str(int(signal.price)),
        }
        data = self._post(
            "/uapi/domestic-stock/v1/trading/order-cash", body, tr_id
        )
        rt_cd  = data.get("rt_cd", "")
        msg_cd = data.get("msg_cd", "")
        msg1   = data.get("msg1", "")
        odno   = data.get("output", {}).get("ODNO", "") if isinstance(data.get("output"), dict) else ""
        ok = rt_cd == "0"
        if ok:
            logger.info(f"주문 성공: {signal.ticker} {signal.side} {signal.quantity}주 ODNO={odno}")
        else:
            logger.warning(f"주문 실패: {signal.ticker} rt_cd={rt_cd} msg_cd={msg_cd} {msg1}")
        return OrderResult(ok=ok, odno=odno, rt_cd=rt_cd, msg_cd=msg_cd, msg1=msg1)


# ── config.json에서 자동 로드 ────────────────────────────────────────────────
def from_config(config_path: str = None) -> KisClient:
    if config_path is None:
        config_path = Path(__file__).parents[2] / "Quant" / "config" / "config.json"
    with open(config_path, encoding="utf-8") as f:
        cfg = json.load(f)
    k = cfg["kis"]
    return KisClient(
        app_key      = k["app_key"],
        app_secret   = k["app_secret"],
        account_no   = k["account_no"],
        account_type = k.get("account_type", "01"),
        is_paper     = k.get("is_paper", False),
    )
