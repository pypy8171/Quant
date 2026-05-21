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
        return Path(__file__).parents[2] / "Quant" / "config" / ".token_cache.json"

    def _load_cached_token(self) -> bool:
        try:
            with open(self._cache_path, encoding="utf-8") as f:
                data = json.load(f)
            if data.get("app_key") != self.app_key:
                return False
            exp = float(data.get("expires_at", 0))
            if time.time() < exp - 300:
                self._token     = data["access_token"]
                self._token_exp = exp
                logger.info(f"캐시 토큰 사용 (만료: {data.get('expired_str', '?')})")
                return True
        except (OSError, json.JSONDecodeError, KeyError, ValueError) as e:
            logger.debug(f"토큰 캐시 로드 실패: {e}")
        return False

    def _save_token_cache(self, access_token: str, expires_at: float, expired_str: str):
        try:
            self._cache_path.parent.mkdir(parents=True, exist_ok=True)
            with open(self._cache_path, "w", encoding="utf-8") as f:
                json.dump({
                    "app_key":      self.app_key,
                    "access_token": access_token,
                    "expires_at":   expires_at,
                    "expired_str":  expired_str,
                }, f)
            # 소유자만 읽기/쓰기 (0600) — Windows는 ACL 모델이 달라 무시됨
            if os.name != "nt":
                os.chmod(self._cache_path, stat.S_IRUSR | stat.S_IWUSR)
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
        try:
            r = requests.post(url, json=body, timeout=10)
            r.raise_for_status()
            data = r.json()
            self._token     = data["access_token"]
            self._token_exp = time.time() + int(data.get("expires_in", 86400))
            expired_str     = data.get("access_token_token_expired", "?")
            self._save_token_cache(self._token, self._token_exp, expired_str)
            logger.info(f"인증 성공 (만료: {expired_str})")
            return True
        except requests.RequestException as e:
            logger.error(f"인증 네트워크 오류: {e}")
            return False
        except (KeyError, ValueError) as e:
            logger.error(f"인증 응답 파싱 실패: {e}")
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
        try:
            r = requests.get(
                self.base_url + path,
                params=params,
                headers=self._headers(tr_id),
                timeout=10,
            )
            return r.json()
        except requests.RequestException as e:
            logger.error(f"GET 네트워크 오류 {path}: {e}")
            return {}
        except ValueError as e:
            logger.error(f"GET 응답 JSON 파싱 실패 {path}: {e}")
            return {}

    def _post(self, path: str, body: dict, tr_id: str) -> dict:
        try:
            r = requests.post(
                self.base_url + path,
                json=body,
                headers=self._headers(tr_id),
                timeout=10,
            )
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
