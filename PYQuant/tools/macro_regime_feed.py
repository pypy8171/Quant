"""
매크로 레짐 보조 프로세스 — risk-on/off 게이트 프로듀서 (2026-08-09 전략회의 Task 3).

목적:
  환율·미국채금리·나스닥선물·VIX 는 서로 상관 0.6~0.9인 "베타(시장 전체 방향)"라
  종목 간 우열을 못 가린다 → 종목 스코어 항이 아니라 **단일 국면 스위치**로 묶는다.
  이 스크립트가 그 스위치를 계산해 regime.json 한 파일로 원자적 발행하면,
  C++ 엔진 데이터 스레드가 폴링 주기마다 읽어 OrderGate::set_entry_halt()를 토글한다.

전송(MVP): ZMQ 대신 **원자적 파일 전달**(tmp+os.replace).
  이유 — 지금 C++(WinHTTP-only·무의존 빌드)에 libzmq를 새로 얹으면 빌드 리스크.
  파이프라인이 결정론적으로 검증된 뒤 ZMQ PUB/SUB로 업그레이드(목표 아키텍처).
  regime.json 스키마(=C++ 리더 계약)는 아래 build_regime() 반환부 주석 참조.

데이터(무료): FinanceDataReader(FDR).
  이 게이트는 "전일 종가 대비 당일 %"를 하루 단위로 계산한다. 그래서 장중 실시간 틱이
  필요한 선물이 아니라, 완료된 간밤 미국 종가(코스피 시가가 반응하는 신호)면 충분하고
  오히려 더 잘 정의된다. yfinance는 이 환경에서 Yahoo 크럼 SSL 차단으로 전 심볼 no_data라
  폐기하고(2026-09-04 확인), FDR로 교체했다:
    US500(S&P500) IXIC(나스닥) VIX / USD/KRW / FRED:DGS10(10Y 국채금리, %).
  참고(표 없음): FRED:DGS30(30Y) FRED:DGS2(2Y) FRED:BAMLH0A0HYM2(HY 스프레드) FRED:DCOILWTICO(WTI).
  수준 평가는 assess_levels().
  FDR는 소스별 라우팅(naver·stooq·FRED)이라 Yahoo 단일 장애에 덜 취약하다.

⚠️ 임계값은 전부 **검증 필요 가정**(STRATEGIES.md 회의 §검증 필요 가정 3).
   과최적화·국면 표본 부족 위험 — 라이브로 관찰하며 보정할 것.

사용 (PYQuant/ 디렉토리에서):
    python -m tools.macro_regime_feed                       # 3분 주기 무한 발행
    python -m tools.macro_regime_feed --once                # 1회 계산 후 종료(점검용)
    python -m tools.macro_regime_feed --interval 180 --out ../Quant/config/regime.json
주문 없음 — 시세 조회·파일 쓰기 전용, 안전.
"""
import argparse
import json
import os

# numpy가 딸려오는 OpenBLAS는 코어 수만큼 작업 버퍼를 미리 잡는다(16코어 PC에서 프로세스당 500MB).
#  이 스크립트는 행렬 연산이 없으니 스레드 1개로 묶는다. numpy를 처음 import하기 전에 있어야 한다.
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

KST = timezone(timedelta(hours=9))

# cp949 콘솔에서 ⚠️ 같은 이모지 출력이 UnicodeEncodeError로 기동 즉시 죽는다.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# ── 심볼 정의 (FinanceDataReader) — 간밤 미국 종가 기준 일일 게이트 ──────────
#  vote_dir: 이 지표가 "오르면" 위험선호(+1)인지 위험회피(-1)인지.
#    나스닥/S&P↑ → risk-on(+1). VIX↑ → risk-off(지표값↑이 위험이므로 -1).
#    10Y 금리↑ → 금리상승 → risk-off(-1). USD/KRW↑ → 원화약세 → risk-off(-1).
#  키(NQ_F/ES_F 등)는 C++ 리더·기존 로그와의 호환을 위해 유지한다.
#  소스는 Yahoo chart(yahoo 키, 장중 현재가 vs 전일 종가)가 먼저다. FDR 일봉은 T-1 종가끼리의 변화라
#   한국 장중에 움직이는 미국 선물을 못 본다(09-14: 투표는 금요일 현물 +0.96/+0.86 → RISK_ON 인데 실제
#   나스닥 선물 −1.32%, S&P 선물 −0.60%). Yahoo 가 막히면 FDR(fdr 키)로 내려간다. [why D-081]
#  나스닥·S&P는 선물 현재가만으로는 반대로 미국 정규장 하루치가 빠진다 — 현물 직전 세션 등락을 더한다
#   (CASH_REF_SYMBOLS, 09-18).
#  FRED:DGS10 은 'Close' 컬럼이 없고 시리즈명이 컬럼 → fetch_changes 가 첫 수치열로 폴백.
#  코스피·코스닥은 이 시스템이 사는 시장 그 자체인데 표가 없었다(09-14 코스피 −3.3%·코스닥 −1.7%인 날
#   해외 5개만 세어 RISK_ON +4). FDR 일봉은 장중 당일 행이 없어 처음 만들 때 뺐던 것이라, Yahoo 현재가로
#   넣는다. FDR 폴백은 없다(fdr=None) — 장중에 전일 종가를 "오늘"로 읽느니 표를 비우는 편이 낫다. [why D-083]
#  WTI는 참고 지표(FRED, 이틀 늦음)였다가 등락 표로 승격 — 09-14 102달러인데 화면은 97.26이었다.
#   수준(100달러 위)은 표가 아니라 note로만 남긴다: 수준을 표로 넣으면 100달러 위 석 달 내내 −1이 깔려
#   정지선이 그만큼 내려온 것과 같아진다. 표는 "급등한 날"에만 나간다.
SYMBOLS = {
    "KOSPI": {"yahoo": "^KS11", "fdr": None,         "vote_dir": +1, "label": "코스피"},
    "KOSDAQ":{"yahoo": "^KQ11", "fdr": None,         "vote_dir": +1, "label": "코스닥"},
    "NQ_F":  {"yahoo": "NQ=F",  "fdr": "IXIC",       "vote_dir": +1, "label": "나스닥 (마감+선물)"},
    "ES_F":  {"yahoo": "ES=F",  "fdr": "US500",      "vote_dir": +1, "label": "S&P500 (마감+선물)"},
    "TNX10": {"yahoo": "^TNX",  "fdr": "FRED:DGS10", "vote_dir": -1, "label": "10Y 미국채금리"},
    "VIX":   {"yahoo": "^VIX",  "fdr": "VIX",        "vote_dir": -1, "label": "VIX"},
    "USDKRW":{"yahoo": "KRW=X", "fdr": "USD/KRW",    "vote_dir": -1, "label": "USD/KRW"},
    "WTI":   {"yahoo": "CL=F",  "fdr": "FRED:DCOILWTICO", "vote_dir": -1, "label": "WTI 유가"},
}

# 장초 대비 방향표 — 전일 대비 표는 개장 때 이미 빨간 날 장중에 돌아서는 것을 못 본다(09:00 −1.3%였던
#  선물이 11:00 −0.3%면 여전히 −1표). 그날 09:00 첫 계산값을 기준점으로 두고, 거기서의 방향을 ±1로 센다.
#  사용자가 지목한 둘만: 나스닥 선물(오르면 +1), 10년물 금리(내리면 +1). warn은 기준점 대비 %.
#  ^TNX 값이 수익률(%)이라 0.6% 변화 ≈ 3bp. 기준점은 logs/regime_open_ref.json에 남겨 재기동에도 유지.
INTRA_SYMBOLS = {
    "NQ_F":  {"warn": 0.3, "vote_dir": +1},
    "TNX10": {"warn": 0.6, "vote_dir": -1},
}

YAHOO_CHART = "https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=1d&interval=5m"
YAHOO_UA    = "Mozilla/5.0"   # 기본 python UA 는 429/403 을 받는다(16:03 실측)


def fetch_yahoo(sym: str, timeout: float = 8.0) -> dict:
    """Yahoo chart meta 의 현재가·전일 종가로 % 변화.
    선물(NQ=F·ES=F)은 거의 24시간 움직여 한국 장중에도 현재가가 갱신된다. VX=F 는 404 라 VIX 는 ^VIX(현물).

    코스피·코스닥처럼 하루 몇 시간만 여는 시장은, 그날 정규장이 아직 안 열렸으면
    regularMarketPrice/previousClose가 전날 종가-전전날 종가(=어제 하루치 등락)로 멈춰 있다.
    이걸 "오늘"로 세면 개장 전 내내 어제 등락이 오늘 표결·score에 그대로 들어간다(사용자 보고,
    09-15 08:34 코스피 -3.26%가 그 사고). currentTradingPeriod.regular.start(오늘 정규장 시작
    epoch)로 아직 열리기 전인지 판정해 — 열리기 전이면 오늘 실시간 pct=0.0(정의상 아직 안 움직임),
    방금 받은 값은 prev_pct(어제 등락, 비교용·표결 제외)로 돌린다.
    반환: {"pct","price","err","prev_pct","premarket"}.
    """
    import time as _time
    import urllib.request
    try:
        req = urllib.request.Request(YAHOO_CHART.format(sym=sym), headers={"User-Agent": YAHOO_UA})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            doc = json.load(r)
        meta = doc["chart"]["result"][0]["meta"]
        last = meta.get("regularMarketPrice")
        prev = meta.get("previousClose") or meta.get("chartPreviousClose")
        if last is None or prev in (None, 0):
            return {"pct": None, "price": None, "err": "yahoo_no_meta", "prev_pct": None, "premarket": False}
        raw_pct = (float(last) - float(prev)) / float(prev) * 100.0
        regular_start = ((meta.get("currentTradingPeriod") or {}).get("regular") or {}).get("start")
        premarket = regular_start is not None and _time.time() < regular_start
        if premarket:
            return {"pct": 0.0, "price": float(last), "err": None, "prev_pct": raw_pct, "premarket": True}
        return {"pct": raw_pct, "price": float(last), "err": None, "prev_pct": None, "premarket": False}
    except Exception as e:  # noqa: BLE001 — 심볼 하나 실패가 전체를 멈추면 안 됨
        return {"pct": None, "price": None, "err": f"yahoo:{type(e).__name__}:{e}", "prev_pct": None, "premarket": False}


# 코스피·코스닥은 Yahoo 대신 네이버 실시간 지수를 쓴다 — Yahoo regularMarketPrice는 KRX 개장
#  직후 몇 분간 전날 종가에 멈춰 있는데(09-15 09:01 실측: 개장 1분 지났는데도 premarket 판정을
#  못 벗어나 어제 -3.26%를 그대로 오늘로 표결, 매수비율이 30%까지 눌림 — 사용자 보고), 네이버는
#  marketStatus로 개장 여부를 직접 주고 지연도 없다(같은 시각 KIS 실측 -0.30%/+0.38%와 일치).
#  scripts/live_prices_feed.py가 이미 같은 네이버 벌크 시세로 종목 가격을 받는 패턴이라 인증도 새로 안 든다.
NAVER_INDEX_UA = {"User-Agent": "Mozilla/5.0", "Referer": "https://finance.naver.com/"}
NAVER_INDEX_SYMBOLS = {"KOSPI": "KOSPI", "KOSDAQ": "KOSDAQ"}  # SYMBOLS 키 → 네이버 itemCode


def fetch_naver_index(timeout: float = 8.0) -> dict:
    """네이버 실시간 지수 일괄 조회. 반환: {itemCode: {"pct","price","open"}}. 실패하면 {}."""
    import urllib.request
    url = ("https://polling.finance.naver.com/api/realtime/domestic/index/"
           + ",".join(NAVER_INDEX_SYMBOLS.values()))
    try:
        req = urllib.request.Request(url, headers=NAVER_INDEX_UA)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.load(r)
        out = {}
        for d in data.get("datas", []):
            code, pct, price = d.get("itemCode"), d.get("fluctuationsRatioRaw"), d.get("closePriceRaw")
            if code is None or pct is None or price is None:
                continue
            out[code] = {"pct": float(pct), "price": float(price), "open": d.get("marketStatus") == "OPEN"}
        return out
    except Exception:
        return {}


# 나스닥·S&P500 표는 "현물 직전 세션 등락 + 선물의 정산 후 변동"으로 센다.
#  선물(NQ=F·ES=F)만 보면 Yahoo previousClose 가 어제 17:00 ET 정산가(오늘 06:00 KST)라 그 뒤 움직임만 남고
#  미국 정규장 하루치가 통째로 빠진다(09-18 08:51 실측: 나스닥 종합 +1.69% 마감인데 선물 −0.15% → 0표).
#  반대로 현물(^IXIC·^GSPC)만 보면 한국 장중 선물 급락을 못 본다(09-14, D-081). 둘을 더하면 어제 종가 대비
#  지금 선물이 어디 있는지가 되고, 한국 장중(미국 정규장 밖) 내내 창이 겹치지 않는다. 정산(17:00 ET)과
#  현물 마감(16:00 ET) 사이 한 시간은 무시한다.
#  예전 fetch_yahoo_daily_prev 는 선물 일봉으로 "어제"를 구했는데, 선물 일봉은 ET 달력일 봉이라 그저께
#  값이 나왔다(09-18 실측 +0.03% / −0.43%). 현물 일봉으로 바꾼다.
CASH_REF_SYMBOLS = {"NQ_F": "^IXIC", "ES_F": "^GSPC"}


def fetch_yahoo_last_session_pct(sym: str, timeout: float = 8.0) -> float | None:
    """현물 지수의 가장 최근 *완결된* 정규장 종가 등락 %(그 전 세션 종가 대비). 못 구하면 None.

    일봉 타임스탬프는 그 세션의 정규장 시작이라, 시작+정규장 길이가 지금보다 앞이면 완결된 봉이다.
    미국 정규장 중(22:30~05:00 KST)에 부르면 진행 중인 마지막 봉을 빼고 그 앞 두 봉으로 센다.
    """
    import time as _time
    import urllib.request
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=10d&interval=1d"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": YAHOO_UA})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            doc = json.load(r)
        result = doc["chart"]["result"][0]
        regular = ((result["meta"].get("currentTradingPeriod") or {}).get("regular") or {})
        session_length = (regular.get("end") or 0) - (regular.get("start") or 0)
        if session_length <= 0:
            return None
        now = _time.time()
        timestamps = result.get("timestamp") or []
        closes = ((result.get("indicators") or {}).get("quote") or [{}])[0].get("close") or []
        completed = [c for t, c in zip(timestamps, closes) if t + session_length <= now and c is not None]
        if len(completed) < 2 or not completed[-2]:
            return None
        return (completed[-1] - completed[-2]) / completed[-2] * 100.0
    except Exception:
        return None

# 참고 지표 — 표를 내지 않고 valid_count에도 들어가지 않는다. 패널에 수치와 수준 평가만 보인다.
#  게이트 지표로 승격하려면 SYMBOLS·THRESHOLDS로 옮기고 halt/liq 임계를 같이 다시 정한다.
#  FRED 일별 시리즈라 KST 기준 1~2일 늦다. 코스피·코스닥 장중값은 FDR에 당일 행이 없어
#  대시보드가 KIS 지수 현재가로 따로 붙인다(scripts/dashboard_server.py).
INFO_SYMBOLS = {
    "TYX30": {"fdr": "FRED:DGS30",         "label": "30Y 미국채금리 (FRED, 1~2일 지연)"},
    "DGS2":  {"fdr": "FRED:DGS2",          "label": "2Y 미국채금리 (FRED, 1~2일 지연)"},
    "HY":    {"fdr": "FRED:BAMLH0A0HYM2",  "label": "미국 하이일드 스프레드 (FRED, 1~2일 지연)"},
}

# 지표별 % 변화 임계(검증 필요) — |chg| 가 warn 이상이면 방향표 1표, strong 이상이면 2표.
#  VIX 는 절대 % 변화가 크므로 별도 임계. TNX10(금리)은 하루 %변동이 채권선물보다 커 별도.
THRESHOLDS = {
    "KOSPI":  {"warn": 0.7, "strong": 1.5},
    "KOSDAQ": {"warn": 0.8, "strong": 1.8},
    "NQ_F":   {"warn": 0.4, "strong": 0.9},
    "ES_F":   {"warn": 0.4, "strong": 0.9},
    "TNX10":  {"warn": 1.5, "strong": 3.0},
    "VIX":    {"warn": 4.0, "strong": 9.0},
    "USDKRW": {"warn": 0.4, "strong": 0.9},
    "WTI":    {"warn": 2.0, "strong": 4.0},
}

# 종합 판정(검증 필요):
#   risk_score = Σ(방향표) + Σ(장초 대비표). 음수일수록 위험회피.
#   entry_halt = risk_score <= HALT_SCORE  (신규 진입 정지 — entry_scale 0과 같은 뜻, 예전 필드 호환)
#   entry_scale = 점수를 매수 명목 비율(0~1)로 옮긴 값. 엔진은 이 값을 분할 단계 명목에 곱한다(D-083).
#   force_liquidate = risk_score <= LIQ_SCORE
#   주의: 이 값이 true가 되면 C++ 전략 스레드가 보유 전량을 시장가로 매도한다(FORCE_LIQ,
#   2초 간격 재발주). 로그만 찍는 값이 아니다. 임계값을 낮출 때 그 무게로 다룬다.
# 09-10: -3은 5개 지표 중 3개만 음수여도 걸린다. 흔한 조정에서 하루 종일 진입이 막혀
#         매도만 나가는 편향이 생겨 -4(4개 음수)로 낮췄다. 청산선(-6)은 그대로 둔다.
# 이 -4는 잠정값이다. 근거는 관측 4거래일·토글 6회뿐이고(그중 진입이 실제로 있던 날은 09-10
#  하루), 다년 재구성으로 검증하지 않았다. 재구성 시 look-ahead 함정 둘을 먼저 처리한다 —
#  KST 당일에 보이는 미국 종가는 T-1 세션이고, USD/KRW 종가는 같은 세션이라 09:00 게이트로
#  새어 들어간다. 검증 전까지 이 값을 확정된 임계로 인용하지 않는다. [why D-033]
# 09-14(D-083): 표 항목이 5개(±10)에서 8개+장초 2표(±18)로 늘어 같은 비율로 옮겼다 — 정지 −4/10 → −7/18,
#  청산 −6/10 → −11/18. 09-14 아침을 새 항목으로 다시 세면 −9 안팎이라 개장 정지·청산 아님이 된다.
#  이 값도 검증 전 잠정값이다.
HALT_SCORE = -7
LIQ_SCORE  = -11
ON_SCORE   = 3    # 이 위면 RISK_ON 표시(표시·로그용, 게이트 아님)
# 드릴용 덮어쓰기. 하락장에서 진입 경로를 시험하려고 halt를 잠시 끌 때 상수를 고치지 않고
#  환경변수로 내린다(예: QUANT_HALT_SCORE=-99). 청산선은 따로 QUANT_LIQ_SCORE.
HALT_SCORE = int(os.environ.get("QUANT_HALT_SCORE", HALT_SCORE))
LIQ_SCORE  = int(os.environ.get("QUANT_LIQ_SCORE", LIQ_SCORE))


def now_kst_iso() -> str:
    return datetime.now(KST).isoformat(timespec="seconds")


def fetch_changes(symbols: dict) -> dict:
    """FinanceDataReader로 각 심볼의 당일 % 변화(전일 종가 대비)를 best-effort 수집.

    반환: {key: {"pct": float|None, "price": float|None, "err": str|None, "src": str}}
    네트워크/패키지 실패는 pct=None 으로 격리(하나 죽어도 나머지로 판정).
    yahoo 키가 있으면 Yahoo chart(장중 현재가)를 먼저 쓰고, 실패한 심볼만 FDR 일봉으로 내려간다.
    """
    out = {}
    pending = {}
    naver_idx = None
    for key, meta in symbols.items():
        if key in NAVER_INDEX_SYMBOLS:
            if naver_idx is None:
                naver_idx = fetch_naver_index()
            row = naver_idx.get(NAVER_INDEX_SYMBOLS[key])
            if row is not None:
                if row["open"]:
                    out[key] = {"pct": row["pct"], "price": row["price"], "err": None,
                                "src": "naver", "prev_pct": None, "premarket": False}
                else:
                    out[key] = {"pct": 0.0, "price": row["price"], "err": None, "src": "naver",
                                "prev_pct": row["pct"], "premarket": True}
                continue
            print(f"[!] {key} 네이버 지수 실패 — Yahoo로 내려간다", file=sys.stderr)
        if meta.get("yahoo"):
            y = fetch_yahoo(meta["yahoo"])
            if y["pct"] is not None:
                pct = y["pct"]
                prev_pct = y.get("prev_pct")
                since_settle_pct = None
                if key in CASH_REF_SYMBOLS:
                    # 현물 직전 세션 + 선물 정산 후 변동. 현물을 못 받으면 선물 변동만으로 표를 낸다(예전 동작).
                    cash_pct = fetch_yahoo_last_session_pct(CASH_REF_SYMBOLS[key])
                    since_settle_pct = pct
                    prev_pct = cash_pct
                    if cash_pct is not None:
                        pct = cash_pct + since_settle_pct
                    else:
                        print(f"[!] {key} 현물 {CASH_REF_SYMBOLS[key]} 직전 세션을 못 받아 선물 변동만 센다", file=sys.stderr)
                out[key] = {"pct": pct, "price": y["price"], "err": None, "src": "yahoo",
                            "prev_pct": prev_pct, "premarket": y.get("premarket", False),
                            "since_settle_pct": since_settle_pct}
                continue
            print(f"[!] {key} Yahoo 실패({y['err']}) — FDR 일봉으로 내려간다", file=sys.stderr)
        pending[key] = meta
    if not pending:
        return out

    try:
        import FinanceDataReader as fdr
    except ImportError:
        print("[!] FinanceDataReader 미설치 — `pip install finance-datareader` 후 재실행",
              file=sys.stderr)
        out.update({k: {"pct": None, "price": None, "err": "no_fdr", "src": "fdr"} for k in pending})
        return out

    # 최근 15일치를 받아 유효 종가 2개(전일·당일)로 % 변화 산출. 휴장·형성중 봉은 dropna 로 제거.
    start = (datetime.now(KST).date() - timedelta(days=15)).isoformat()
    for key, meta in pending.items():
        pct = price = None
        err = None
        try:
            df = fdr.DataReader(meta["fdr"], start)
            if df is None or len(df) == 0:
                err = "no_data"
            else:
                # FRED 등 'Close' 없는 시리즈는 첫 수치열로 폴백(시리즈명이 컬럼).
                col = "Close" if "Close" in df.columns else df.columns[0]
                s = df[col].dropna()
                if len(s) >= 2:
                    prev = float(s.iloc[-2])
                    last = float(s.iloc[-1])
                    if prev != 0:
                        price = last
                        pct = (last - prev) / prev * 100.0
                    else:
                        err = "zero_prev"
                else:
                    err = "no_data"
        except Exception as e:  # noqa: BLE001 — 심볼 하나 실패가 전체를 멈추면 안 됨
            err = f"{type(e).__name__}:{e}"
        out[key] = {"pct": pct, "price": price, "err": err, "src": "fdr"}
    return out


def vote_for(key: str, pct: float) -> tuple[int, int]:
    """(방향표, |표수|) — 방향표는 risk-on(+)/off(-) 부호, |표수|는 0/1/2 강도."""
    th = THRESHOLDS[key]
    mag = abs(pct)
    strength = 2 if mag >= th["strong"] else (1 if mag >= th["warn"] else 0)
    if strength == 0:
        return 0, 0
    # 지표 상승(pct>0)의 위험선호 방향 = vote_dir. 하락이면 반대.
    direction = SYMBOLS[key]["vote_dir"] * (1 if pct > 0 else -1)
    return direction * strength, strength


# [formula] 수준 경계. 일간 등락 표(THRESHOLDS)와 달리 절대 수준이라 며칠씩 켜져 있다.
#  게이트가 아니라 설명용이다. 근거는 통상 인용되는 구간이지 이 시스템에서 검증한 값이 아니다.
#   30Y/10Y 5.0%·4.5%: 장기 할인율이 주식 밸류에이션을 누르기 시작한다고 보는 통상 구간.
#   VIX 20/30: 경계/공포 구간. USD/KRW 1350/1400: 원화 약세·외국인 이탈 압력이 커지는 구간.
#   WTI 90/100: 인플레·금리 경로에 다시 부담을 주는 구간.
#   HY 스프레드(ICE BofA US High Yield OAS, %p) 4.0/5.0: 신용 스트레스가 주식보다 먼저 드러나는 축.
#     장기 중앙값이 4%대라 3 아래는 완화, 5 위는 위험회피 국면으로 읽는다.
#   10Y−2Y: 0 아래면 역전(경기 둔화 신호로 통용), 커브 자체는 표 없이 note로만 남긴다.
LEVELS = {
    "TYX30":  [(5.0, "5% 위. 장기 할인율 부담이 큰 구간"), (4.5, "4.5~5%. 고점권")],
    "TNX10":  [(4.5, "4.5% 위. 고점권"), (4.0, "4~4.5%")],
    "VIX":    [(30.0, "30 위. 공포 구간"), (20.0, "20~30. 경계 구간"), (15.0, "15~20. 보통")],
    "USDKRW": [(1400.0, "1400 위. 원화 약세 경계"), (1350.0, "1350~1400. 약세 구간")],
    "WTI":    [(100.0, "100달러 위. 인플레 재점화 우려"), (90.0, "90~100달러. 부담 구간")],
    "HY":     [(5.0, "5%p 위. 신용 스트레스 확대"), (4.0, "4~5%p. 확대 조짐"), (3.0, "3~4%p. 보통"), (0.0, "3%p 아래. 신용 시장 평온")],
    "DGS2":   [(4.5, "4.5% 위. 긴축 기대 유지"), (4.0, "4~4.5%")],
}
# 종합 경고에 세는 경계(지표당 1개). 이 개수만 세지 점수에는 더하지 않는다.
WARN_LEVEL = {"TYX30": 5.0, "TNX10": 4.5, "VIX": 20.0, "USDKRW": 1400.0, "WTI": 90.0, "HY": 4.0}


def assess_levels(components: dict, score: int) -> dict:
    """절대 수준 기준으로 지표별 note와 종합 한 줄을 만든다. 게이트(score)는 건드리지 않는다.

    반환: {"flags": [label…], "summary": str}. 각 component에는 "note"를 채워 넣는다.
    """
    flags = []
    for key, bands in LEVELS.items():
        c = components.get(key)
        if not c or c.get("price") is None:
            continue
        price = c["price"]
        note = ""
        for lo, text in bands:
            if price >= lo:
                note = text
                break
        # 금리는 %등락보다 bp가 읽기 쉽다. pct와 price로 전일값을 되살려 bp를 붙인다.
        if key in ("TYX30", "TNX10", "DGS2") and c.get("pct") is not None:
            prev = price / (1 + c["pct"] / 100.0)
            bp = (price - prev) * 100.0
            if abs(bp) >= 5:
                note = (note + " · " if note else "") + f"전일비 {bp:+.0f}bp"
        if note:
            c["note"] = note
        if key in WARN_LEVEL and price >= WARN_LEVEL[key]:
            flags.append(c["label"])

    t30 = (components.get("TYX30") or {}).get("price")
    t10 = (components.get("TNX10") or {}).get("price")
    if t30 is not None and t10 is not None:
        spread = t30 - t10
        c = components["TYX30"]
        add = f"30Y-10Y {spread * 100:+.0f}bp"
        if spread >= 0.5:
            add += " (장기 프리미엄 확대)"
        elif spread <= 0.0:
            add += " (역전 근접)"
        c["note"] = (c.get("note", "") + " · " if c.get("note") else "") + add

    t2 = (components.get("DGS2") or {}).get("price")
    if t10 is not None and t2 is not None:
        spread = t10 - t2
        c = components["DGS2"]
        add = f"10Y-2Y {spread * 100:+.0f}bp" + (" (역전)" if spread < 0 else "")
        c["note"] = (c.get("note", "") + " · " if c.get("note") else "") + add

    n = len(flags)
    if n >= 3:
        judge = "수준 부담이 겹쳐 있다. 신규 진입은 비중을 줄이고, 청산은 score 기준을 따른다"
    elif n >= 1:
        judge = "부담 요인이 있다. 당일 방향은 등락 표(score)로 본다"
    else:
        judge = "수준 경고 없음. 당일 등락 표(score)로 본다"
    summary = f"수준 경고 {n}/{len(WARN_LEVEL)}" + (f" ({', '.join(flags)})" if flags else "") +               f" · 등락 score {score:+d} · {judge}"
    return {"flags": flags, "summary": summary}


# [formula] 점수 → 매수 명목 비율. 스위치(−3이면 100%, −4면 0%)의 절벽을 없앤다. RISK_ON 기준(ON_SCORE, +3)
#  이상이면 100%, 0이면 70%, 정지선 절반이면 40%, 정지선 아래는 0. 사이 값은 직선. 엔진은 이 값을 분할 단계 명목에 곱한다.
#  0.1 단위로 끊어 3분마다 미세하게 바뀌어 분할 매수가 재구성되는 일을 막는다. [why D-083]
#  09-18: 100%가 되는 점수를 +2에서 ON_SCORE로 옮겼다. +2에서 끝나면 라벨은 NEUTRAL인데 비율은 100%인 칸이
#  생긴다(09-17 09:25 score 2). NEUTRAL이면 최대 90%, RISK_ON일 때만 100%.
def entry_scale(score: int, halt: int = None, on: int = None) -> float:
    halt = HALT_SCORE if halt is None else halt
    on = ON_SCORE if on is None else on
    pts = [(float(halt), 0.0), (halt / 2.0, 0.4), (0.0, 0.7), (float(on), 1.0)]
    if score <= pts[0][0]:
        return 0.0
    if score >= pts[-1][0]:
        return 1.0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x0 <= score <= x1:
            return round(y0 + (y1 - y0) * (score - x0) / (x1 - x0), 1)
    return 1.0


def load_open_ref(path, prices: dict) -> dict:
    """그날 09:00 이후 첫 계산의 가격을 기준점으로 잡아 파일에 남긴다. 이미 오늘 기준점이 있으면 그것을 쓴다.
    반환 {"date": "YYYY-MM-DD", "prices": {key: price}} 또는 개장 전이면 {}."""
    now = datetime.now(KST)
    today = now.date().isoformat()
    ref = {}
    try:
        if path and path.exists():
            ref = json.loads(path.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 — 기준점 파일이 깨졌으면 오늘 다시 잡는다
        ref = {}
    if ref.get("date") == today and ref.get("prices"):
        return ref
    if now.hour < 9:
        return {}
    ref = {"date": today, "ts": now.isoformat(timespec="seconds"),
           "prices": {k: v for k, v in prices.items() if k in INTRA_SYMBOLS and v is not None}}
    if not ref["prices"]:
        return {}
    try:
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(ref, ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 장초 기준점 저장 실패: {type(e).__name__}: {e}", file=sys.stderr)
    return ref


def build_regime(changes: dict, open_ref: dict | None = None) -> dict:
    """지표 변화 → 레짐 판정. 반환 dict 가 곧 regime.json 스키마(=C++ 리더 계약).

    C++ 리더 계약:
      entry_halt(bool)  → OrderGate::set_entry_halt(entry_halt) 로 그대로 토글.
      entry_scale(float|null) → OrderGate::set_entry_scale(). null(무효)이면 엔진은 1.0으로 본다.
      force_liquidate(bool) → 보유 전량 시장가 매도(FORCE_LIQ). strategy_thread가 체결될 때까지
                          2초 간격으로 재발주한다. 가장 무거운 신호이므로 valid=false면 절대 true가 아니다.
      stale_after_sec   → C++는 (지금 - ts) > 이 값이면 파일을 신뢰하지 말 것(페일세이프).
                          권장: stale 시 halt를 새로 켜지 말고, 자신이 켠 halt만 유지/해제.
      valid(bool)       → 유효 지표가 부족하면 false. C++는 false면 게이트 변경 금지.
    """
    components = {}
    score = 0
    valid_count = 0
    for key, meta in SYMBOLS.items():
        ch = changes.get(key, {})
        pct = ch.get("pct")
        if pct is None:
            components[key] = {"label": meta["label"], "pct": None, "vote": 0, "err": ch.get("err")}
            continue
        vote, _strength = vote_for(key, pct)
        score += vote
        valid_count += 1
        components[key] = {"label": meta["label"], "pct": round(pct, 3), "vote": vote,
                           "price": ch.get("price"), "src": ch.get("src", "fdr")}
        if ch.get("premarket"):
            components[key]["premarket"] = True
        if ch.get("prev_pct") is not None:
            components[key]["prev_pct"] = round(ch["prev_pct"], 3)
        if ch.get("since_settle_pct") is not None:
            components[key]["since_settle_pct"] = round(ch["since_settle_pct"], 3)

    # 장초 대비 방향표. 기준점이 없으면(개장 전·첫 계산) 0표.
    ref_prices = (open_ref or {}).get("prices") or {}
    for key, meta in INTRA_SYMBOLS.items():
        c = components.get(key)
        base = ref_prices.get(key)
        if not c or c.get("price") is None or not base:
            continue
        ipct = (c["price"] - base) / base * 100.0
        ivote = 0
        if abs(ipct) >= meta["warn"]:
            ivote = meta["vote_dir"] * (1 if ipct > 0 else -1)
        score += ivote
        c["intra"] = {"pct": round(ipct, 3), "vote": ivote, "base": base}

    # 참고 지표는 표 0·tier=info로 넣는다. 아래 valid 계산은 SYMBOLS 개수만 본다.
    for key, meta in INFO_SYMBOLS.items():
        ch = changes.get(key, {})
        pct = ch.get("pct")
        if pct is None:
            components[key] = {"label": meta["label"], "pct": None, "vote": 0, "tier": "info",
                               "err": ch.get("err")}
            continue
        components[key] = {"label": meta["label"], "pct": round(pct, 3), "vote": 0, "tier": "info",
                           "price": ch.get("price")}

    assessment = assess_levels(components, score)

    # 유효 지표 절반 미만이면 판정 보류(valid=false) — 데이터 공백에 게이트 오작동 방지.
    valid = valid_count >= max(1, (len(SYMBOLS) + 1) // 2)
    entry_halt = valid and score <= HALT_SCORE
    force_liquidate = valid and score <= LIQ_SCORE

    if not valid:
        regime = "UNKNOWN"
    elif score <= HALT_SCORE:
        regime = "RISK_OFF"
    elif score >= ON_SCORE:
        regime = "RISK_ON"
    else:
        regime = "NEUTRAL"

    return {
        "schema": 1,
        "ts": now_kst_iso(),
        "regime": regime,
        "entry_halt": entry_halt,
        "force_liquidate": force_liquidate,
        "risk_score": score,
        "entry_scale": (0.0 if force_liquidate else entry_scale(score)) if valid else None,
        "open_ref_ts": (open_ref or {}).get("ts"),
        "valid": valid,
        "valid_count": valid_count,
        "stale_after_sec": 600,
        "thresholds": {"halt_score": HALT_SCORE, "liq_score": LIQ_SCORE, "on_score": ON_SCORE},
        "components": components,
        "assessment": assessment,
        "source": ("Yahoo chart(장중 현재가)" if any(c.get("src") == "yahoo" for c in components.values())
                   else "FinanceDataReader"),
    }


def write_atomic(path: Path, obj: dict) -> None:
    """tmp 파일에 쓰고 os.replace 로 원자 교체 — 리더가 반쪽 파일을 읽지 않게."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)  # 원자적(같은 볼륨)


def append_history(path: Path, obj: dict) -> None:
    """사이클마다 한 줄씩 누적한다.

    regime.json은 매 사이클 덮어써져 과거 점수가 남지 않는다. 09-10 사고를 되짚을 때
    남아 있던 증거가 엔진 로그 4일치뿐이었던 것이 이 파일이 생긴 이유다. 임계값을
    다시 정할 때 필요한 최소 단위(시각·점수·판정·지표별 등락률)만 적는다. [why D-033]

    실패는 삼킨다 — 관측용 부산물이 보조 프로세스 본체를 멈추면 안 된다.
    """
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        row = {
            "ts": obj.get("ts"),
            "regime": obj.get("regime"),
            "risk_score": obj.get("risk_score"),
            "entry_halt": obj.get("entry_halt"),
            "force_liquidate": obj.get("force_liquidate"),
            "valid": obj.get("valid"),
            "halt_score": HALT_SCORE,
            "liq_score": LIQ_SCORE,
            "entry_scale": obj.get("entry_scale"),
            "intra": {k: v["intra"]["vote"] for k, v in (obj.get("components") or {}).items() if v.get("intra")},
            "pct": {k: v.get("pct") for k, v in (obj.get("components") or {}).items()},
            "vote": {k: v.get("vote") for k, v in (obj.get("components") or {}).items()},
        }
        with open(path, "a", encoding="utf-8") as f:
            print(json.dumps(row, ensure_ascii=False), file=f)
    except Exception as e:  # noqa: BLE001
        print(f"[WARN] 이력 append 실패: {type(e).__name__}: {e}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description="매크로 레짐 보조 프로세스 (risk-on/off 게이트 프로듀서)")
    ap.add_argument("--out", default=None, help="regime.json 경로 (기본 Quant/config/regime.json)")
    ap.add_argument("--interval", type=float, default=180.0, help="발행 주기(초, 기본 180=3분)")
    ap.add_argument("--once", action="store_true", help="1회 계산 후 종료(점검용)")
    ap.add_argument("--history", default=None,
                    help="이력 jsonl 경로 (기본 logs/regime_history.jsonl, 빈 문자열이면 끄기)")
    args = ap.parse_args()

    if args.out:
        out_path = Path(args.out)
    else:
        # PYQuant/tools/ → repo 루트/Quant/config/regime.json
        out_path = Path(__file__).resolve().parents[2] / "Quant" / "config" / "regime.json"

    # 이력은 저장소 기준 고정 경로다 — cwd가 달라도 한 파일에 모이게.
    if args.history is None:
        hist_path = Path(__file__).resolve().parents[2] / "logs" / "regime_history.jsonl"
    elif args.history == "":
        hist_path = None
    else:
        hist_path = Path(args.history)

    # 장초 기준점은 이력과 같은 logs/ 아래. 날짜가 바뀌면 load_open_ref가 스스로 새로 잡는다.
    ref_path = Path(__file__).resolve().parents[2] / "logs" / "regime_open_ref.json"

    print(f"매크로 레짐 보조 프로세스 | 출력={out_path} | 이력={hist_path or '끔'} | "
          f"주기={args.interval}s | once={args.once}")
    print("⚠️ 임계값은 검증 필요 가정 — 라이브 관찰하며 보정(STRATEGIES.md 참조)")

    while True:
        # 사이클 단위 예외 격리 — yfinance/네트워크/IO 실패가 프로세스를 죽이지 않게(외부
        #  재기동 래퍼와 별개의 in-process 내성). fetch_changes는 심볼별로 이미 격리되지만
        #  build/write 레벨 throw는 여기서 잡아 다음 주기에 재시도한다.
        regime = None
        try:
            changes = fetch_changes({**SYMBOLS, **INFO_SYMBOLS})
            open_ref = load_open_ref(ref_path, {k: v.get("price") for k, v in changes.items()})
            regime = build_regime(changes, open_ref)
            write_atomic(out_path, regime)
        except KeyboardInterrupt:
            print("\n중단 — 마지막 regime.json 유지")
            break
        except Exception as e:  # noqa: BLE001
            print(f"[WARN] 사이클 실패: {type(e).__name__}: {e} — 다음 주기 재시도", file=sys.stderr)
        if regime is not None:
            if hist_path is not None:
                append_history(hist_path, regime)
            comp = " ".join(
                (f"{k}={v['pct']}%({v['vote']:+d})"
                 + (f"[장초{v['intra']['pct']:+.2f}%({v['intra']['vote']:+d})]" if v.get("intra") else ""))
                if v.get("pct") is not None else f"{k}=NA"
                for k, v in regime["components"].items()
            )
            print(f"[{regime['ts']}] regime={regime['regime']} score={regime['risk_score']} "
                  f"scale={regime['entry_scale']} halt={regime['entry_halt']} "
                  f"liq={regime['force_liquidate']} | {comp}")
            print(f"    {regime['assessment']['summary']}")
        if args.once:
            break
        try:
            time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\n중단 — 마지막 regime.json 유지")
            break


if __name__ == "__main__":
    main()
