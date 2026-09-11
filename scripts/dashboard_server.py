#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
실시간 모의계좌 매매 대시보드 (의존성 0 — stdlib http.server + requests).

엔진(C++)을 재빌드하지 않고, 이미 존재하는 데이터 소스만 모아 브라우저에 실시간 표시한다:
  - 계좌/보유종목/평가손익/손실률 : KisClient.get_kr_balance() (엔진과 토큰 캐시 공유 → 충돌 없음)
  - 국면(regime)                  : Quant/config/regime.json  (매크로 보조 프로세스가 씀)
  - 국내 지수 현재가(코스피·코스닥·KOSPI200) : KisClient.get_index_price() — regime.json은 간밤 종가
                                     기준이라 장중 국내 지수는 여기서 따로 붙인다
  - 투자자별·프로그램 순매수, KOSPI200 최근월 선물 : KisClient.get_investor_daily_by_market()
                                     get_program_trade_today() get_future_board()/get_future_price()
                                     실전 도메인 시세 키(quote_kis) 전용, 30초 주기
  - 매매 리스트(유니버스)          : Quant/config/universe_scan.json
  - 장중 매매 기준                 : config 전략/리스크 블록 (정적 서술)
  - 콘솔 이벤트(신호/주문/체결/거부): logs/quant_trader.log tail 분류
  - 당일 체결 원장                 : logs/trades_YYYYMMDD.csv
  - 거래대금 상위 N종목(스냅샷)     : KisClient.get_volume_ranking()

실행:
  py scripts/dashboard_server.py                       # 기본 config_dev_paper.json, 포트 8787
  py scripts/dashboard_server.py --config Quant/config/config_mm_paper.json --port 8790
브라우저에서 http://127.0.0.1:8787 열기. 3초마다 /api/state 폴링.
"""
import argparse
import csv
import io
from collections import deque
import json
import math
import os
import re
import statistics
import sys
import time
import threading
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "PYQuant"))    # kis.client / core.logger 해석용
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402

try:
    from kis.client import KisClient, KisAuthError
except Exception as e:                        # pragma: no cover
    print(f"[치명] PYQuant/kis/client.py 임포트 실패: {e}", file=sys.stderr)
    raise

KST = timezone(timedelta(hours=9))

# ─────────────────────────────────────────────────────────────────────────────
# 로그 폴더 해석
#   엔진(main.cpp)은 QUANT_LOG_DIR 또는 실행파일 옆 logs/(=build_win/logs)에 쓴다.
#   repo 루트에서 exe를 띄워도 로그·원장은 build_win/logs 로 간다(cwd 무관).
#   그래서 후보들 중 quant_trader.log mtime이 가장 최신인 폴더를 실제 출력지로 따라간다.
# ─────────────────────────────────────────────────────────────────────────────
LOGS_OVERRIDE = None  # --logs 로 명시하면 항상 이걸 씀


def logs_dir() -> Path:
    # 규칙은 _logdir 하나(QUANT_LOG_DIR > 최신 quant_trader.log를 가진 후보).
    return Path(LOGS_OVERRIDE) if LOGS_OVERRIDE else _logdir.log_dir()


def logfile() -> Path:
    return logs_dir() / "quant_trader.log"

# 로그 라인:  2026-09-03 09:53:49.011 [INFO ] [태그] 메시지  (parse_quant_log.py와 동일 규약)
_LINE = re.compile(r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.\d{3}\s+\[(?P<lvl>\w+)\s*\]\s+(?P<rest>.*)$")
_GATE = re.compile(r"→\s*(?P<reason>.+?)\s*$")

# ─────────────────────────────────────────────────────────────────────────────
# 캐시: (값, 만료ts). KIS 호출을 브라우저 폴링 주기와 분리한다.
# ─────────────────────────────────────────────────────────────────────────────
class TTLCache:
    def __init__(self):
        self._d = {}
        self._lock = threading.Lock()

    def get_or(self, key, ttl, producer):
        now = time.time()
        with self._lock:
            hit = self._d.get(key)
            if hit and now < hit[1]:
                return hit[0]
        # 락 밖에서 생산(네트워크). 경쟁 시 중복 호출은 허용(TTL 짧음).
        try:
            val = producer()
            err = None
        except Exception as e:
            val, err = None, str(e)
        with self._lock:
            if err is None:
                self._d[key] = (val, now + ttl)
                return val
            # 실패 시 직전 값이 있으면 잠깐 유지(stale-on-error), 없으면 에러 표식
            hit = self._d.get(key)
            if hit:
                return hit[0]
            return {"__error__": err}


CACHE = TTLCache()


# ─────────────────────────────────────────────────────────────────────────────
# 데이터 소스
# ─────────────────────────────────────────────────────────────────────────────
def market_status(now=None):
    now = now or datetime.now(KST)
    is_weekday = now.weekday() < 5
    open_t = now.replace(hour=9, minute=0, second=0, microsecond=0)
    close_t = now.replace(hour=15, minute=30, second=0, microsecond=0)
    is_open = is_weekday and open_t <= now <= close_t
    return {"open": is_open, "weekday": is_weekday,
            "now_kst": now.strftime("%Y-%m-%d %H:%M:%S")}


def engine_status():
    """로그 파일 mtime으로 엔진 가동 추정."""
    d = logs_dir()
    try:
        mtime = (d / "quant_trader.log").stat().st_mtime
        age = time.time() - mtime
        return {"alive": age < 120, "log_age_sec": round(age, 1),
                "log_mtime": datetime.fromtimestamp(mtime, KST).strftime("%H:%M:%S"),
                "log_dir": str(d)}
    except OSError:
        return {"alive": False, "log_age_sec": None, "log_mtime": None,
                "log_dir": str(d)}


def read_regime(regime_path: Path):
    try:
        raw = regime_path.read_text(encoding="utf-8")
        j = json.loads(raw)
        age = time.time() - regime_path.stat().st_mtime
        j["_age_sec"] = round(age, 1)
        j["_stale"] = age > j.get("stale_after_sec", 600)
        return j
    except (OSError, json.JSONDecodeError) as e:
        return {"__error__": f"regime.json 없음/파싱실패: {e}"}


def read_entry_scores():
    """엔진이 스캔마다 남기는 진입 점수표(로그 폴더 entry_scores.json). 보유 종목 카드가
    점수순 정렬에 쓴다. 파일이 없으면(엔진 미기동·구버전) 빈 표를 돌려 카드는 잔고순으로 남는다."""
    path = logs_dir() / "entry_scores.json"
    try:
        j = json.loads(path.read_text(encoding="utf-8"))
        j["_age_sec"] = round(time.time() - path.stat().st_mtime, 1)
        return j
    except (OSError, json.JSONDecodeError) as e:
        return {"__error__": f"entry_scores.json 없음/파싱실패: {e}", "scores": {}}


def read_universe(uni_path: Path):
    try:
        j = json.loads(uni_path.read_text(encoding="utf-8"))
        return {"basDt": j.get("basDt"), "market": j.get("market"),
                "count": j.get("count", len(j.get("universe", []))),
                "source": j.get("source"),
                "universe": j.get("universe", [])}
    except (OSError, json.JSONDecodeError) as e:
        return {"__error__": f"universe_scan.json 없음/파싱실패: {e}"}


def tail_bytes(path: Path, nbytes: int) -> str:
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            if size > nbytes:
                f.seek(size - nbytes)
                f.readline()  # 잘린 첫 줄 버림
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


_NAME_IN_LOG = re.compile(r"\b(\d{6})\(([^)]{1,30})\)")


# 한 번 알아낸 이름은 세션 내내 들고 간다. 로그 출처가 tail이라 창이 밀리면 같은 종목의
#  이름이 사라졌다 나타났다 했다(2026-09-08 장중, 145995가 이름 없이 표시). 유니버스 top-N에
#  없고 보유도 아닌 종목은 로그에 `코드(이름)` 줄이 잡히는 순간에만 이름이 붙기 때문이다.
_NAME_CACHE = {}
_NAME_SEEDED = False


def seed_name_cache():
    """기동 시 1회, 로그 뒤쪽을 넓게 훑어 이름을 미리 담는다.

    tick마다 보는 tail(400KB)에는 그 순간 조용한 종목의 이름 줄이 없을 수 있다. 캐시가
    비어 있는 첫 화면에서 코드만 보이는 것을 막으려고, 기동 때만 더 넓게 한 번 읽는다.
    """
    global _NAME_SEEDED
    if _NAME_SEEDED:
        return
    _NAME_SEEDED = True
    try:
        for tk, nm in _NAME_IN_LOG.findall(tail_bytes(logfile(), 16_000_000)):
            _NAME_CACHE.setdefault(tk, nm)
    except Exception:
        pass


def build_name_map(bal, uni, log_text=""):
    """종목코드→종목명. 원장·이벤트 피드가 코드만 보여줘 매번 검색해야 했던 것을 없앤다.

    세 출처를 겹쳐 쓴다(뒤가 우선): 유니버스 파일 → 로그의 `123456(이름)` → 잔고 보유분.
    잔고를 마지막에 두는 것은 계좌가 실제로 들고 있는 종목의 표기를 정본으로 삼기 위해서다.
    이번 tick에서 찾은 것은 _NAME_CACHE에 누적하고, 반환은 캐시 전체로 한다.
    """
    seed_name_cache()
    names = {}
    for row in (uni or {}).get("universe", []) or []:
        tk, nm = row.get("ticker"), row.get("name")
        if tk and nm:
            names[tk] = nm
    for tk, nm in _NAME_IN_LOG.findall(log_text or ""):
        names[tk] = nm
    for pos in (bal or {}).get("positions", []) or []:
        tk, nm = pos.get("ticker"), pos.get("name")
        if tk and nm:
            names[tk] = nm
    _NAME_CACHE.update(names)   # tick 안의 우선순위는 유지, tick 간에는 새로 찾은 쪽이 이긴다
    return dict(_NAME_CACHE)


def read_log_events(max_events=40):
    """콘솔 상당 이벤트 피드 + 헤더용 최신 상태(당일손익/국면선택/스캔)."""
    text = tail_bytes(logfile(), 400_000)
    events, latest = [], {}
    for raw in text.splitlines():
        m = _LINE.match(raw)
        if not m:
            continue
        ts, lvl, rest = m.group("ts"), m.group("lvl"), m.group("rest")
        hhmmss = ts[11:]
        # 헤더/모니터용 최신값(마지막 매치가 최신) — 콘솔에 뜨는 엔진 계산 라인
        if "당일손익" in rest:
            latest["daily_pnl_line"] = rest
        if "[RegimeSelect]" in rest:
            latest["regime_select"] = rest
        if "정배열 프리필터" in rest or "[Main] DEVSCALE" in rest:
            latest["scan_line"] = rest
        if "[섹터]" in rest:
            latest["sector"] = rest
        if "[수급추정]" in rest:
            latest["supply"] = rest
        if rest.startswith("[매크로]") and "score=" in rest:
            latest["macro"] = rest
        # 이벤트 분류(가장 관심 있는 것만)
        cat = None
        if "체결 확인" in rest or ("체결" in rest and "통보" in rest):
            cat = "fill"
        elif "[OrderRouter] 접수 [ORD-" in rest:
            cat = "order"
        elif "[Strategy] 신호:" in rest:
            cat = "signal"
        elif "청산차단" in rest:
            cat = "liq_block"
        elif "KIS 거부" in rest or "주문 오류" in rest:
            cat = "reject"
        elif lvl.startswith("WARN") and "[OrderRouter] 거부" in rest:
            g = _GATE.search(rest)
            cat, rest = "gate_block", (g.group("reason") if g else rest)
        elif lvl.startswith("ERROR"):
            cat = "error"
        if cat:
            events.append({"ts": hhmmss, "cat": cat, "msg": rest[:220]})
    events = events[-max_events:]
    events.reverse()  # 최신 먼저
    return {"events": events, "latest": latest, "_text": text}


# 체결 원장에서 실현손익을 뽑는다. 엔진이 realized_pnl 컬럼을 남긴 행은 그 값을 쓰고,
#  컬럼이 없던 시절 행은 원장의 매수·매도 체결로 평단을 굴려 재구성한다. 재구성분은
#  전일 이월분처럼 장부에 매수 기록이 없는 매도를 계산할 수 없어 unknown으로 센다.
COMMISSION_RATE = 0.00015  # 위탁수수료 0.015% (매수·매도 공통)
SELL_TAX_RATE = 0.0018     # 증권거래세 0.18% (매도에만)


def _accrue_realized(row, book, acc):
    if (row.get("event") or "").strip() != "FILL":
        return
    try:
        qty = int(float(row.get("fill_qty") or 0))
        px = float(row.get("fill_price") or 0)
    except (TypeError, ValueError):
        return
    if qty <= 0 or px <= 0:
        return
    ticker = (row.get("ticker") or "").strip()
    side = (row.get("side") or "").strip().upper()

    if side.startswith("B"):
        held, avg = book.get(ticker, (0, 0.0))
        book[ticker] = (held + qty, (avg * held + px * qty) / (held + qty))
        return
    if not side.startswith("S"):
        return

    pnl = None
    col = (row.get("realized_pnl") or "").strip()
    if col:
        try:
            pnl = float(col)
        except ValueError:
            pnl = None
    held, avg = book.get(ticker, (0, 0.0))
    if pnl is None:
        if held > 0 and avg > 0:
            pnl = (px - avg) * qty - px * qty * COMMISSION_RATE - px * qty * SELL_TAX_RATE
        else:
            acc["unknown"] += 1
    book[ticker] = (max(0, held - qty), avg)

    if pnl is None:
        return
    if pnl > 0:
        acc["profit"] += pnl
        acc["win"] += 1
    elif pnl < 0:
        acc["loss"] += pnl
        acc["lose"] += 1
    else:
        acc["flat"] += 1


def read_trades_today(now=None, seed_avg=None):
    # seed_avg: 잔고의 종목별 평단. 원장에 매수 기록이 없는 매도(전일 이월분)의 평단을
    #  메워준다. 매도는 평단을 바꾸지 않으므로 잔여 보유분 평단이 매도 당시 평단과 같다.
    #  전량 매도돼 잔고에서 사라진 종목은 여전히 메울 수 없어 unknown으로 남는다.
    now = now or datetime.now(KST)
    d = logs_dir()
    # 같은 날짜 원장이 여러 폴더에 있으면 행 수 최대인 것(_logdir 규칙). --logs 지정 시엔 그 폴더만.
    path = d / f"trades_{now.strftime('%Y%m%d')}.csv"
    if not LOGS_OVERRIDE:
        path = _logdir.find_ledger(now.strftime("%Y%m%d")) or path
    if not path.exists():
        return {"date": now.strftime("%Y%m%d"), "rows": [],
                "realized": None, "note": f"당일 원장 없음 ({d})"}
    try:
        # 화면에 쓰는 건 최근 40행뿐이다. 원장이 길어져도 메모리에 통째로 올리지
        # 않도록 deque(maxlen)로 흘려 읽는다. 총 행수는 헤더에 표시하므로 세면서 간다.
        total = 0
        book = {}   # ticker -> [보유수량, 평단] — 매도 실현손익을 재구성하기 위한 장부
        for tk, av in (seed_avg or {}).items():
            book[tk] = (10 ** 9, av)   # 이월분: 수량은 충분히 크게 두고 평단만 쓴다
        realized = {"profit": 0.0, "loss": 0.0, "net": 0.0,
                    "win": 0, "lose": 0, "flat": 0, "unknown": 0}
        with open(path, encoding="utf-8") as f:
            recent = deque(maxlen=40)
            for row in csv.DictReader(f):
                total += 1
                recent.append(row)
                _accrue_realized(row, book, realized)
        realized["net"] = realized["profit"] + realized["loss"]
        return {"date": now.strftime("%Y%m%d"), "rows": list(recent)[::-1],
                "total": total, "realized": realized}
    except OSError as e:
        return {"date": now.strftime("%Y%m%d"), "rows": [], "realized": None, "note": str(e)}


# ─────────────────────────────────────────────────────────────────────────────
# 라이브 수집 (백그라운드) — KIS REST를 HTTP 요청 스레드에서 절대 기다리지 않는다.
#   모의 도메인 inquire-balance가 타임아웃(20s×3=최대 60s)이면 /api/state가
#   그만큼 블로킹돼 대시보드 전체가 멈추므로, 잔고·랭킹은 데몬 스레드가 주기 수집한다.
#   요청은 마지막 스냅샷만 즉시 읽는다(실패 시 마지막 정상값 유지 + 지연 표식).
# ─────────────────────────────────────────────────────────────────────────────
LIVE = {}
LIVE_LOCK = threading.Lock()


def _live_set(key, val):
    with LIVE_LOCK:
        LIVE[key] = {"val": val, "ts": time.time(), "err": None}


def _live_err(key, msg):
    with LIVE_LOCK:
        cur = LIVE.get(key)
        if cur and cur.get("val") is not None:
            cur["err"] = msg            # 마지막 정상값은 유지, 현재 조회지연만 표식
        else:
            LIVE[key] = {"val": None, "ts": time.time(), "err": msg}


def _live_get(key):
    with LIVE_LOCK:
        cur = LIVE.get(key)
        if not cur:
            return {"__error__": "수집 대기 중…"}
        if cur["val"] is None:
            return {"__error__": cur.get("err") or "조회 실패"}
        out = dict(cur["val"])
        if cur.get("err"):
            out["_stale_err"] = cur["err"]
            out["_stale_age"] = round(time.time() - cur["ts"], 0)
        return out


def _fetch_balance(kis: KisClient):
    items, summ = kis.get_kr_balance()
    # _get가 실패 시 {}를 돌려주고 get_kr_balance는 이를 전부 0으로 파싱한다.
    # 자금이 있는 계좌가 전부 0 + 보유 0이면 성공한 빈 계좌가 아니라 조회 실패로 본다.
    if (not items and summ.cash == 0 and summ.total_eval == 0
            and summ.total_pnl == 0 and summ.total_pnl_rate == 0):
        raise RuntimeError("잔고 조회 실패/지연 (모의 도메인 inquire-balance 타임아웃 추정)")
    return {
        "summary": {
            "cash": summ.cash, "total_eval": summ.total_eval,
            "total_pnl": summ.total_pnl, "total_pnl_rate": summ.total_pnl_rate,
            # 총매수금액(보유분 원가)과 주문가능현금. 총매수금액은 OrderGate §3d 총노출과
            # 같은 원가 기준이라 한도 소진율을 그대로 비교할 수 있다.
            "buy_amount": summ.buy_amount, "cash_avail": summ.cash_avail,
        },
        "positions": [
            {"ticker": b.ticker, "name": b.name, "qty": b.quantity,
             "avg": b.avg_price, "cur": b.current_price, "eval": b.eval_amount,
             "pnl": b.pnl, "pnl_rate": b.pnl_rate}
            for b in items
        ],
    }


# 국내 지수 코드(FID_INPUT_ISCD, 업종 U). 패널 표시 순서 그대로.
KR_INDEX = [("0001", "코스피"), ("1001", "코스닥"), ("2001", "KOSPI200")]


def _fetch_kr_index(quote: KisClient):
    """지수 세 개를 한 번에. 하나가 실패해도 나머지는 싣고, 전부 실패면 예외로 올려 stale 표식."""
    rows = []
    for code, label in KR_INDEX:
        try:
            ip = quote.get_index_price(code)
            rows.append({"code": code, "label": label, "price": ip.price if ip.ok else None,
                         "pct": ip.change_rate if ip.ok else None})
        except Exception as e:  # noqa: BLE001 — 지수 하나 실패가 나머지를 막지 않게
            rows.append({"code": code, "label": label, "price": None, "pct": None, "err": str(e)})
    if all(r["price"] is None for r in rows):
        raise RuntimeError("지수 조회 실패: " + "; ".join(r.get("err", "") for r in rows))
    return {"rows": rows}


def _num(v, scale=1.0):
    try:
        return round(float(v) / scale, 2)
    except (TypeError, ValueError):
        return None


# 시장 단위 수급·프로그램·선물. 시장별 코드 = (업종코드, 프로그램 시장구분, 라벨). [why D-044]
KR_FLOW_MARKETS = [("0001", "K", "코스피"), ("1001", "Q", "코스닥")]


def _fetch_kr_flow(quote: KisClient):
    """투자자별 순매수(일별 TR 당일 잠정행)·프로그램 순매수(시간 TR 최신행)·KOSPI200 최근월 선물.
    KIS 응답은 백만원이라 억원으로 나눠 싣는다(/100). 세 묶음 중 하나가 실패해도 나머지는 싣고,
    전부 실패면 예외로 올려 stale 표식."""
    out, errs = {"investor": [], "program": [], "future": None}, []
    for code, pcls, label in KR_FLOW_MARKETS:
        try:
            rows = quote.get_investor_daily_by_market(code)
            r = rows[0] if rows else {}
            out["investor"].append({"label": label, "date": r.get("stck_bsop_date"),
                                    "frgn": _num(r.get("frgn_ntby_tr_pbmn"), 100),
                                    "orgn": _num(r.get("orgn_ntby_tr_pbmn"), 100),
                                    "prsn": _num(r.get("prsn_ntby_tr_pbmn"), 100)})
        except Exception as e:  # noqa: BLE001
            errs.append(f"수급 {label}: {e}")
        try:
            rows = quote.get_program_trade_today(pcls)
            r = rows[0] if rows else {}
            out["program"].append({"label": label, "hour": r.get("bsop_hour"),
                                   "whol": _num(r.get("whol_smtn_ntby_tr_pbmn"), 100),
                                   "arbt": _num(r.get("arbt_smtn_ntby_tr_pbmn"), 100),
                                   "nabt": _num(r.get("nabt_smtn_ntby_tr_pbmn"), 100)})
        except Exception as e:  # noqa: BLE001
            errs.append(f"프로그램 {label}: {e}")
    try:
        # K2I 전광판은 만기 오름차순이라 첫 행이 최근월물. 기초지수(output3)는 선물 현재가 응답에 같이 온다.
        board = quote.get_future_board("K2I")
        if not board:
            raise RuntimeError("K2I 전광판 빈 응답")
        front = board[0]
        fp = quote.get_future_price(front["futs_shrn_iscd"])
        o1, o3 = fp.get("output1") or {}, fp.get("output3") or {}
        out["future"] = {"code": front.get("futs_shrn_iscd"), "name": front.get("hts_kor_isnm"),
                         "price": _num(o1.get("futs_prpr")), "pct": _num(o1.get("futs_prdy_ctrt")),
                         "basis": _num(o1.get("mrkt_basis")), "dprt": _num(o1.get("dprt")),
                         "theo": _num(o1.get("hts_thpr")), "oi": _num(o1.get("hts_otst_stpl_qty")),
                         "oi_chg": _num(o1.get("otst_stpl_qty_icdc")),
                         "expiry": o1.get("futs_last_tr_date"), "days": o1.get("hts_rmnn_dynu"),
                         "k200": _num(o3.get("bstp_nmix_prpr"))}
    except Exception as e:  # noqa: BLE001
        errs.append(f"선물: {e}")
    if not out["investor"] and not out["program"] and out["future"] is None:
        raise RuntimeError("; ".join(errs))
    if errs:
        out["partial"] = "; ".join(errs)
    return out


# 섹터 카드에 싣는 업종·테마 지수. 코드는 PYQuant/tools/check_sector_index.py 훑기(2026-09-11)에서 이름이
#  돌아온 것만 골랐다. 코스닥 1005~1018은 코스피 업종과 같은 값이 돌아와(별칭) 뺐다. [why D-050]
KR_SECTOR_GROUPS = [
    ("코스피 업종", [f"{n:04d}" for n in range(5, 31)]),
    ("코스닥 업종", ["1006", "1009", "1010", "1011", "1013", "1014", "1015"]
                  + [f"{n:04d}" for n in range(1019, 1034)]),
    ("KRX 테마", ["1046", "1047", "1050", "1052", "1053", "1055", "1056", "1057", "1058",
               "1061", "1062", "1063", "1064", "1065"]),
]
SECTOR_VOL_DAYS = 20


def _sector_stats(idx: dict) -> dict | None:
    """지수 하나의 변동성 통계. 완성된 봉(당일 제외)으로 일간 로그수익률 표준편차를 재고,
    당일 등락을 그 σ로 나눈 z가 '평소 대비 오늘 움직임'이다. [formula] 연율 = σ_일 × √252."""
    bars = idx.get("bars") or []
    if not idx.get("name") or idx.get("price", 0) <= 0 or len(bars) < 6:
        return None
    today = datetime.now(KST).strftime("%Y%m%d")
    done = [b for b in bars if b["date"] != today]
    closes = [b["close"] for b in done][-(SECTOR_VOL_DAYS + 1):]
    rets = [math.log(closes[i] / closes[i - 1]) for i in range(1, len(closes)) if closes[i - 1] > 0]
    sd = statistics.pstdev(rets) * 100 if len(rets) >= 5 else None
    pct = idx.get("pct") or 0.0
    prev = idx.get("prev") or (done[-1]["close"] if done else 0)
    rng = (idx["high"] - idx["low"]) / prev * 100 if prev > 0 and idx.get("high", 0) > 0 else None
    r5 = (idx["price"] / done[-5]["close"] - 1) * 100 if len(done) >= 5 and done[-5]["close"] > 0 else None
    # 당일 진폭도 σ로 재면 "오늘 얼마나 흔들렸나"가 된다(방향 무관).
    return {"code": idx["code"], "name": idx["name"], "price": round(idx["price"], 2),
            "pct": round(pct, 2), "r5": _num(r5), "vol": _num(sd * math.sqrt(252)) if sd else None,
            "sd": _num(sd), "z": _num(pct / sd) if sd else None, "rng": _num(rng),
            "rng_z": _num(rng / sd) if (sd and rng is not None) else None, "n": len(rets)}


def _fetch_kr_sectors(quote: KisClient):
    """업종별 REST 1회씩(약 60회·8초). 하나가 실패해도 나머지는 싣고, 전부 비면 예외로 stale 표식."""
    groups, errs = [], []
    for label, codes in KR_SECTOR_GROUPS:
        rows = []
        for code in codes:
            try:
                st = _sector_stats(quote.get_index_daily(code))
                if st:
                    rows.append(st)
            except Exception as e:  # noqa: BLE001 — 업종 하나 실패가 카드 전체를 막지 않게
                errs.append(f"{code}: {e}")
            time.sleep(0.06)
        groups.append({"label": label, "rows": rows})
    if not any(g["rows"] for g in groups):
        raise RuntimeError("; ".join(errs) or "업종 응답 없음")
    out = {"groups": groups, "days": SECTOR_VOL_DAYS,
           "asof": datetime.now(KST).strftime("%H:%M")}
    if errs:
        out["partial"] = f"{len(errs)}개 업종 실패"
    return out


SECTOR_MEMBER_TTL = 300.0
_SECTOR_MEMBERS: dict = {}          # code → {"ts", "val"}. HTTP 스레드가 만지므로 LIVE_LOCK으로 감싼다.
_KRX_THEME_CODES = set(KR_SECTOR_GROUPS[2][1])


def sector_members(quote: KisClient, code: str) -> dict:
    """업종 대표 종목(시총 상위 10). 클릭할 때만 부르고 5분 캐시. KRX 테마는 구성종목 조회 경로가
    없어 빈 목록에 안내만 싣는다."""
    if code in _KRX_THEME_CODES:
        return {"code": code, "rows": [], "note": "KRX 테마 지수는 KIS 순위 TR에 구성종목이 오지 않는다"}
    with LIVE_LOCK:
        cur = _SECTOR_MEMBERS.get(code)
        if cur and time.time() - cur["ts"] < SECTOR_MEMBER_TTL:
            return cur["val"]
    rows = quote.get_market_cap_ranking(code, top_n=10)
    val = {"code": code, "rows": rows, "asof": datetime.now(KST).strftime("%H:%M")}
    if rows:
        with LIVE_LOCK:
            _SECTOR_MEMBERS[code] = {"ts": time.time(), "val": val}
    return val


def _sector_loop(quote: KisClient, interval=300.0):
    """섹터 변동성은 5분 주기면 충분하고 호출이 많아 워밍 루프와 스레드를 나눈다."""
    while True:
        try:
            _live_set("kr_sector", _fetch_kr_sectors(quote))
        except Exception as e:
            _live_err("kr_sector", str(e))
        time.sleep(interval)


# ── 테마(인포스탁 분류, 네이버 증권 경유) [why D-054] ─────────────────────────────
# 목록(266개 당일 등락률·상승/하락 수)은 60초마다, 구성 종목은 클릭할 때만 받아 5분 캐시한다.
# 종목→테마 역색인은 일일 스냅샷 PYQuant/data/themes/latest.json(fetch_naver_themes.py)에서 읽는다.
try:
    from naver.theme import fetch_theme_list, fetch_theme_members
except Exception as e:                        # pragma: no cover
    fetch_theme_list = fetch_theme_members = None
    print(f"[경고] PYQuant/naver/theme.py 임포트 실패 — 테마 카드 없이 간다: {e}", file=sys.stderr)

THEME_SNAPSHOT = REPO / "PYQuant" / "data" / "themes" / "latest.json"
THEME_MEMBER_TTL = 300.0
_THEME_MEMBERS: dict = {}           # no → {"ts", "val"}. LIVE_LOCK으로 감싼다.
_THEME_INDEX = {"mtime": 0.0, "by_ticker": {}, "asof": ""}


def _load_theme_index():
    """스냅샷 파일(5MB)은 mtime이 바뀔 때만 다시 읽는다. 없으면 빈 색인 — 카드가 '스냅샷 없음'을 보인다."""
    try:
        st = THEME_SNAPSHOT.stat()
    except OSError:
        return
    if st.st_mtime == _THEME_INDEX["mtime"]:
        return
    d = json.loads(THEME_SNAPSHOT.read_text(encoding="utf-8"))
    _THEME_INDEX.update(mtime=st.st_mtime, by_ticker=d.get("by_ticker") or {}, asof=d.get("asof", ""))


def holding_themes(bal) -> dict:
    """보유 종목이 어느 테마에 몰렸는지. 테마별 보유 종목 수와 이름, 스냅샷에 없는 종목(ETF 등) 목록.
    by_no는 테마 순위표의 '보유' 열이 쓴다(테마 번호 → 보유 종목 수)."""
    _load_theme_index()
    idx = _THEME_INDEX["by_ticker"]
    cnt: dict = {}
    untagged = []
    for p in ((bal or {}).get("positions") or []):
        tk = (p.get("ticker") or "").strip()
        if not tk:
            continue
        hits = idx.get(tk)
        if not hits:
            untagged.append(p.get("name") or tk)
            continue
        for t in hits:
            c = cnt.setdefault(t["no"], {"no": t["no"], "name": t["name"], "n": 0, "names": []})
            c["n"] += 1
            c["names"].append(p.get("name") or tk)
    rows = sorted(cnt.values(), key=lambda c: (-c["n"], c["name"]))
    return {"asof": _THEME_INDEX["asof"], "rows": [r for r in rows if r["n"] >= 2][:40],
            "by_no": {str(r["no"]): r["n"] for r in rows}, "untagged": untagged,
            "n_pos": sum(1 for p in ((bal or {}).get("positions") or []) if (p.get("ticker") or "").strip())}


def theme_members(no: str) -> dict:
    """테마 구성 종목(등락률순)과 종목별 편입 사유. 클릭할 때만 부르고 5분 캐시."""
    if fetch_theme_members is None:
        return {"no": no, "rows": [], "note": "naver.theme 모듈 없음"}
    with LIVE_LOCK:
        cur = _THEME_MEMBERS.get(no)
        if cur and time.time() - cur["ts"] < THEME_MEMBER_TTL:
            return cur["val"]
    val = fetch_theme_members(int(no))
    val["asof"] = datetime.now(KST).strftime("%H:%M")
    if val.get("rows"):
        with LIVE_LOCK:
            _THEME_MEMBERS[no] = {"ts": time.time(), "val": val}
    return val


def _theme_loop(interval=60.0):
    """테마 목록 3회 호출. 네이버 쪽 형식이 바뀌면 오류를 남기고 옛 값에 갱신 지연 표시가 붙는다."""
    while True:
        try:
            rows = fetch_theme_list()
            _live_set("kr_theme", {"rows": rows, "asof": datetime.now(KST).strftime("%H:%M")})
        except Exception as e:
            _live_err("kr_theme", str(e))
        time.sleep(interval)


def _warm_loop(kis: KisClient, quote: KisClient, interval=5.0, flow_every=6):
    tick = 0
    while True:
        try:
            _live_set("balance", _fetch_balance(kis))
        except Exception as e:
            _live_err("balance", str(e))
        try:
            _live_set("kr_index", _fetch_kr_index(quote))
        except Exception as e:
            _live_err("kr_index", str(e))
        try:
            _live_set("ranking", {"rows": quote.get_volume_ranking(top_n=25)})
        except Exception as e:
            _live_err("ranking", str(e))
        # 수급·프로그램·선물은 REST 6회(약 30초)라 매 주기 돌리지 않는다. 잠정치 갱신도 그 정도 간격이다.
        if tick % flow_every == 0:
            try:
                _live_set("kr_flow", _fetch_kr_flow(quote))
            except Exception as e:
                _live_err("kr_flow", str(e))
        tick += 1
        time.sleep(interval)


def _resample(bars: list, n: int) -> list:
    """1분봉 → n분봉. bars: [{hms/time, open, high, low, close, volume}] (오래된→최신)."""
    out, cur, cur_key = [], None, None
    for b in bars:
        hms = b.get("hms") or (b.get("time", "0000") + "00")
        try:
            mins = int(hms[:2]) * 60 + int(hms[2:4])
        except ValueError:
            continue
        key = mins // n
        if key != cur_key:
            if cur:
                out.append(cur)
            cur_key = key
            label = f"{(key*n)//60:02d}{(key*n)%60:02d}"
            cur = {"time": label, "open": b["open"], "high": b["high"],
                   "low": b["low"], "close": b["close"], "volume": b["volume"]}
        else:
            cur["high"] = max(cur["high"], b["high"])
            cur["low"] = min(cur["low"], b["low"])
            cur["close"] = b["close"]
            cur["volume"] += b["volume"]
    if cur:
        out.append(cur)
    return out


# 차트 로컬 캐시. 일봉·주봉은 PYQuant/data/{daily,weekly}/<ticker>.parquet에 쌓고, 분봉은
#  마감 뒤 백필(PYQuant/data/minute/<ticker>/<YYYYMMDD>.parquet, 하루치 완본)이 있으면 그것을 읽고
#  없으면 장중 증분본 PYQuant/data/minute_live/(같은 스키마)에 쌓는다. 장중본을 백필 경로에 쓰면
#  백필이 "이미 받았다"고 보고 그날을 건너뛰어 반쪽 파일이 굳는다. KIS에는 마지막 봉 이후 증분만 묻는다. 처음 보는 종목의 일봉은 .datagokr_cache의 과거분으로
#  먼저 채운다. 파일 쓰기는 tmp→os.replace라 21:00 백필과 겹쳐도 반쪽 파일이 남지 않는다.
_DATA_DIR = REPO / "PYQuant" / "data"
_DGK_DIR = REPO / "PYQuant" / ".datagokr_cache"
_CHART_KEYS = ("open", "high", "low", "close", "volume")
_CHART_IO_LOCK = threading.Lock()


def _parquet_read(path: Path) -> list:
    try:
        import pandas as pd
        if not path.exists():
            return []
        return pd.read_parquet(path).to_dict("records")
    except Exception:
        return []


def _parquet_write(path: Path, rows: list):
    if not rows:
        return
    try:
        import pandas as pd
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp.parquet")
        with _CHART_IO_LOCK:
            pd.DataFrame(rows).to_parquet(tmp, index=False)
            os.replace(tmp, path)
    except Exception as e:
        print(f"[chart-cache] 쓰기 실패 {path}: {e}", file=sys.stderr)


def _merge_bars(local: list, fresh: list, key: str) -> list:
    """키(date/hms) 기준 합치기. 같은 키는 KIS 쪽(fresh)이 이긴다 — 진행 중 봉 갱신용."""
    d = {str(r[key]): r for r in local if r.get(key)}
    for r in fresh:
        if r.get(key):
            d[str(r[key])] = r
    return [d[k] for k in sorted(d)]


def _bar_rows(rows: list, key: str) -> list:
    """직렬화 가능한 순수 dict 열로 정리(넘파이 스칼라 → 파이썬 스칼라)."""
    out = []
    for r in rows:
        try:
            b = {key: str(r[key])}
            for k in _CHART_KEYS:
                v = r.get(k, 0)
                b[k] = int(v) if k == "volume" else float(v)
            out.append(b)
        except (KeyError, TypeError, ValueError):
            continue
    return out


def _seed_daily_from_datagokr(ticker: str) -> list:
    """data.go.kr 캐시(ohlcv_<ticker>_<start>_<end>.parquet) 중 가장 늦게 끝나는 파일로 일봉을 시드한다."""
    cands = sorted(_DGK_DIR.glob(f"ohlcv_{ticker}_*.parquet"), key=lambda p: p.stem.split("_")[-1])
    if not cands:
        return []
    return _bar_rows(_parquet_read(cands[-1]), "date")


def _local_chart_dw(quote: KisClient, ticker: str, period: str, count: int) -> list:
    """일봉('D')/주봉('W')을 로컬 우선으로. 반환 [{date, open, high, low, close, volume}] 오래된→최신, 최근 count개."""
    path = _DATA_DIR / ("daily" if period == "D" else "weekly") / f"{ticker}.parquet"
    local = _bar_rows(_parquet_read(path), "date")
    if not local and period == "D":
        local = _seed_daily_from_datagokr(ticker)
    now = datetime.now(KST)
    today = now.strftime("%Y-%m-%d")
    last = local[-1]["date"] if local else ""
    # 증분 폭: 마지막 봉부터 오늘까지의 달력일을 봉 수로 환산(일봉 5/7, 주봉 1/7) + 여유 3.
    #  겹치는 봉은 병합에서 KIS 값으로 덮인다. 로컬이 비었거나 너무 오래됐으면 count 전부.
    if last:
        try:
            gap_days = (now.date() - datetime.strptime(last, "%Y-%m-%d").date()).days
        except ValueError:
            gap_days = 10 ** 6
        need = (gap_days * 5 // 7 + 3) if period == "D" else (gap_days // 7 + 3)
    else:
        need = count
    fetch_n = min(count, max(need, 1))
    # 장 밖(16:00 이후·주말)이고 로컬 마지막 봉이 직전 거래일이면 REST를 아예 안 부른다.
    #  휴장일은 모르므로 그날은 한 번 더 묻고 끝난다(빈 증분).
    skip = False
    if last and period == "D":
        wd = now.weekday()
        last_biz = now.date() - timedelta(days={5: 1, 6: 2}.get(wd, 0))
        if wd >= 5 or now.hour >= 16:
            skip = last >= last_biz.strftime("%Y-%m-%d")
        elif now.hour < 9:
            skip = last >= (last_biz - timedelta(days=1 if wd else 3)).strftime("%Y-%m-%d")
    if len(local) < count and last and last < today:
        skip = False   # 히스토리가 짧으면 채우러 간다
        fetch_n = count
    if skip:
        return local[-count:]
    fresh = _bar_rows(quote.get_chart_ohlcv(ticker, period, fetch_n), "date")
    if fresh:
        merged = _merge_bars(local, fresh, "date")
        if merged != local:
            _parquet_write(path, merged)
        local = merged
    return local[-count:]


def _local_minute_today(quote: KisClient, ticker: str, count: int) -> list:
    """당일 1분봉을 로컬 파일과 합쳐 돌려준다. 장중엔 마지막 로컬 봉 이후 분수만큼만 KIS에 묻는다."""
    now = datetime.now(KST)
    ymd = now.strftime("%Y%m%d")
    done = _DATA_DIR / "minute" / ticker / f"{ymd}.parquet"       # 마감 뒤 백필 완본
    path = _DATA_DIR / "minute_live" / ticker / f"{ymd}.parquet"   # 장중 증분본
    raw = _parquet_read(done)
    if raw:
        path = done
    else:
        raw = _parquet_read(path)
    local = []
    for r in raw:
        try:
            hms = str(r.get("hms") or (str(r.get("time", "")) + "00"))
            b = {"date": ymd, "hms": hms, "time": hms[:4]}
            for k in _CHART_KEYS:
                v = r.get(k, 0)
                b[k] = int(v) if k == "volume" else float(v)
            local.append(b)
        except (TypeError, ValueError):
            continue
    local.sort(key=lambda b: b["hms"])
    last_hms = local[-1]["hms"] if local else ""
    hm = now.hour * 60 + now.minute
    market = 9 * 60 <= hm <= 15 * 60 + 40 and now.weekday() < 5
    # 장 끝난 뒤 로컬이 마감 근처(15:20 이후)까지 있으면 그날치는 끝난 것으로 본다(백필 산출물 재사용).
    if not market and last_hms >= "152000":
        return local[-count:]
    if not market and not local and (hm < 9 * 60 or now.weekday() >= 5):
        return []
    need = count
    if last_hms:
        try:
            elapsed = hm - (int(last_hms[:2]) * 60 + int(last_hms[2:4]))
        except ValueError:
            elapsed = count
        need = max(2, min(count, elapsed + 2))
    fresh = quote.get_minute_ohlcv(ticker, need)
    fresh = [dict(b, date=ymd) for b in fresh if b.get("hms")]
    if fresh:
        merged = _merge_bars(local, fresh, "hms")
        if merged != local:
            _parquet_write(path, merged)
        local = merged
    return local[-count:]


def build_chart(quote: KisClient, ticker: str, tf: str):
    tf = (tf or "D").upper()
    ttl = 60.0 if tf in ("D", "W") else 20.0

    def _fetch():
        if tf == "D":
            bars = _local_chart_dw(quote, ticker, "D", 380)
            return {"tf": "D", "label": "일봉", "bars": bars, "x": "date", "show": 140}
        if tf == "W":
            bars = _local_chart_dw(quote, ticker, "W", 300)
            return {"tf": "W", "label": "주봉", "bars": bars, "x": "date", "show": 80}
        n = 5 if tf == "5" else 3
        raw = _local_minute_today(quote, ticker, 300)
        bars = _resample(raw, n)
        return {"tf": tf, "label": f"{n}분봉", "bars": bars, "x": "time", "show": len(bars)}

    return CACHE.get_or(f"chart:{ticker}:{tf}", ttl, _fetch)


# ─────────────────────────────────────────────────────────────────────────────
# 전략/기준 서술 (config에서 추출한 정적 설명)
# ─────────────────────────────────────────────────────────────────────────────
def build_criteria(cfg: dict):
    strat = (cfg.get("strategies") or [{}])
    dev = next((s for s in strat if s.get("type") == "DEVIATION_SCALE"), {})
    out = {
        "regime_strategies": cfg.get("regime_strategies", {}),
        "fetch_interval_sec": cfg.get("fetch_interval_sec"),
        "regime_stale_sec": cfg.get("regime_stale_sec"),
        "kosdaq_enabled": cfg.get("kosdaq_enabled", False),
        "rest_price_feed": cfg.get("rest_price_feed", False),
        "risk": cfg.get("risk", {}),
        "strategies": [],
    }
    for s in strat:
        if not isinstance(s, dict) or "type" not in s:
            continue
        out["strategies"].append({
            "type": s.get("type"),
            "scan_top_n": s.get("scan_top_n"),
            "value_top_n": s.get("value_top_n"),
            "max_universe": s.get("max_universe"),
            "require_aligned": s.get("require_aligned"),
            "min_price": s.get("min_price"),
            "max_price": s.get("max_price"),
            "max_dev_pct": s.get("max_dev_pct"),
            "risk_off_index_pct": s.get("risk_off_index_pct"),
            "base_pct": s.get("base_pct"),
            "max_pct": s.get("max_pct"),
        })
    return out


# ─────────────────────────────────────────────────────────────────────────────
# 상태 집계
# ─────────────────────────────────────────────────────────────────────────────
# 원장 strategy 칸에 들어가지만 전략이 아닌 값들. 코드값 그대로 두면 무슨 주문인지 읽히지
#  않으므로 표시할 때만 한국어로 바꾼다(원장·집계 키는 영문 그대로여야 과거 기록과 맞는다).
#  notify_sidecar.py가 이 표를 그대로 import해서 쓴다 — 정의는 여기 하나뿐이다.
STRATEGY_LABEL = {
    "ORPHAN": "이전 세션 주문",
    "STARTUP_PROBE": "기동 점검 주문",
    "TEST": "테스트 주문",
}


def build_state(kis, quote, cfg, regime_path, uni_path):
    _bal = _live_get("balance")
    _seed = {}
    for p in ((_bal or {}).get("positions") or []):
        try:
            av = float(p.get("avg") or 0)
        except (TypeError, ValueError):
            av = 0.0
        tk = (p.get("ticker") or "").strip()
        if tk and av > 0:
            _seed[tk] = av
    _log = read_log_events()
    _uni = read_universe(uni_path)
    return {
        "server_ts": datetime.now(KST).strftime("%Y-%m-%d %H:%M:%S"),
        "account_no": _mask_acct(cfg.get("kis", {}).get("account_no", "")),
        "is_paper": cfg.get("kis", {}).get("is_paper", False),
        "mode": cfg.get("mode"),
        "market": market_status(),
        "engine": engine_status(),
        "account": _bal,
        "regime": read_regime(regime_path),
        "kr_index": _live_get("kr_index"),
        "kr_flow": _live_get("kr_flow"),
        "kr_sector": _live_get("kr_sector"),
        "kr_theme": _live_get("kr_theme"),
        "holding_themes": holding_themes(_bal),
        "universe": _uni,
        "entry_scores": read_entry_scores(),
        "criteria": build_criteria(cfg),
        "log": _log,
        "trades": read_trades_today(seed_avg=_seed),
        "ranking": _live_get("ranking"),
        "names": build_name_map(_bal, _uni, _log.pop("_text", "")),
        "labels": STRATEGY_LABEL,
    }


def _mask_acct(a: str) -> str:
    return (a[:2] + "****" + a[-2:]) if len(a) >= 6 else "****"


# ─────────────────────────────────────────────────────────────────────────────
# HTTP
# ─────────────────────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    kis = None
    quote = None
    cfg = None
    cfg_path = None
    cfg_mtime = 0.0
    regime_path = None
    uni_path = None

    # /api/state의 criteria(한도 등)는 config에서 읽는다. 기동 때 한 번만 읽으면 config를 고쳐도
    #  재기동 전까지 옛 값이 화면에 남는다(09-11 25→40 반영 안 됨). mtime이 바뀐 때만 다시 읽고,
    #  파싱 실패면 마지막 정상본을 유지한다. 인증 정보(kis)는 기동 시점 것을 그대로 쓴다.
    @classmethod
    def current_cfg(cls):
        try:
            m = os.path.getmtime(cls.cfg_path)
            if m != cls.cfg_mtime:
                with open(cls.cfg_path, encoding="utf-8") as f:
                    cls.cfg = json.load(f)
                cls.cfg_mtime = m
        except Exception:
            pass
        return cls.cfg

    def log_message(self, *a):
        pass  # 콘솔 조용히

    def _send(self, code, ctype, body: bytes):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
            # 브라우저가 느린 응답(잔고 조회 지연 등)을 기다리다 폴링 연결을 끊은 경우.
            # 서버는 계속 살아 있으므로 트레이스백 없이 조용히 넘어간다.
            pass

    def do_GET(self):
        if self.path.startswith("/api/state"):
            try:
                state = build_state(self.kis, self.quote, self.current_cfg(), self.regime_path, self.uni_path)
                body = json.dumps(state, ensure_ascii=False).encode("utf-8")
            except Exception as e:
                body = json.dumps({"__error__": str(e)}, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif self.path.startswith("/api/sector"):
            q = parse_qs(urlparse(self.path).query)
            code = (q.get("code", [""])[0] or "").strip()
            if not re.fullmatch(r"\d{4}", code):
                self._send(400, "application/json; charset=utf-8",
                           json.dumps({"__error__": "code는 4자리 업종코드"}, ensure_ascii=False).encode("utf-8"))
                return
            try:
                body = json.dumps(sector_members(self.quote, code), ensure_ascii=False).encode("utf-8")
            except Exception as e:
                body = json.dumps({"__error__": str(e)}, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif self.path.startswith("/api/theme"):
            q = parse_qs(urlparse(self.path).query)
            no = (q.get("no", [""])[0] or "").strip()
            if not re.fullmatch(r"\d{1,6}", no):
                self._send(400, "application/json; charset=utf-8",
                           json.dumps({"__error__": "no는 테마 번호"}, ensure_ascii=False).encode("utf-8"))
                return
            try:
                body = json.dumps(theme_members(no), ensure_ascii=False).encode("utf-8")
            except Exception as e:
                body = json.dumps({"__error__": str(e)}, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif self.path.startswith("/api/chart"):
            q = parse_qs(urlparse(self.path).query)
            ticker = (q.get("ticker", [""])[0] or "").strip()
            tf = (q.get("tf", ["D"])[0] or "D").strip()
            if not re.fullmatch(r"\d{6}", ticker):
                self._send(400, "application/json; charset=utf-8",
                           json.dumps({"__error__": "ticker는 6자리 숫자"}, ensure_ascii=False).encode("utf-8"))
                return
            try:
                data = build_chart(self.quote, ticker, tf)
                body = json.dumps(data, ensure_ascii=False).encode("utf-8")
            except Exception as e:
                body = json.dumps({"__error__": str(e)}, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif self.path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", HTML.encode("utf-8"))
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found")


# ─────────────────────────────────────────────────────────────────────────────
# 프론트엔드 (자체완결 HTML)
# ─────────────────────────────────────────────────────────────────────────────
HTML = r"""<!doctype html><html lang="ko"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>실시간 매매 대시보드</title>
<style>
:root{
  --bg:#0f1115; --panel:#171a21; --panel2:#1e222b; --bd:#2a2f3a; --fg:#e6e9ef; --mut:#8b93a7;
  --up:#2ec26b; --dn:#ff5d5d; --acc:#5b9dff; --warn:#ffb020; --chip:#232834;
}
@media(prefers-color-scheme:light){:root{
  --bg:#f4f6fa; --panel:#fff; --panel2:#f0f2f7; --bd:#dde1ea; --fg:#1a1d24; --mut:#5c6473;
  --up:#0a9d52; --dn:#d92b2b; --acc:#2f6fe0; --warn:#b3760a; --chip:#eef1f7;}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.45 -apple-system,'Segoe UI',Roboto,'Malgun Gothic',sans-serif}
header{display:flex;flex-wrap:wrap;align-items:center;gap:10px;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--bd);position:sticky;top:0;z-index:5}
h1{font-size:15px;margin:0;font-weight:700}
.chip{background:var(--chip);border:1px solid var(--bd);border-radius:999px;padding:2px 10px;font-size:12px;color:var(--mut)}
.chip b{color:var(--fg)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;vertical-align:middle}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:12px;padding:12px 16px}
.card{background:var(--panel);border:1px solid var(--bd);border-radius:10px;padding:12px 14px;overflow:hidden}
.card h2{font-size:12px;letter-spacing:.02em;color:var(--mut);margin:0 0 10px;text-transform:uppercase;font-weight:600}
.col12{grid-column:span 12}.col8{grid-column:span 8}.col6{grid-column:span 6}.col4{grid-column:span 4}.col3{grid-column:span 3}
@media(max-width:1100px){.col8,.col6,.col4,.col3{grid-column:span 12}}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.kpi{background:var(--panel2);border:1px solid var(--bd);border-radius:8px;padding:10px 12px}
.kpi .l{color:var(--mut);font-size:11px}.kpi .v{font-size:19px;font-weight:700;margin-top:3px}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{text-align:right;padding:5px 8px;border-bottom:1px solid var(--bd);white-space:nowrap}
th{color:var(--mut);font-weight:600;position:sticky;top:0;background:var(--panel)}
td.l,th.l{text-align:left}
.up{color:var(--up)}.dn{color:var(--dn)}.mut{color:var(--mut)}
.scroll{max-height:340px;overflow:auto}
.sec-scroll{max-height:560px}.sec-h{color:var(--mut);font-size:11px;background:var(--panel2)}.sec-hot{font-weight:700}
.sec-d td{padding:0 8px 8px 24px;border-bottom:1px solid var(--bd)}.sec-d table{font-size:11px}.sec-d td td,.sec-d th{padding:2px 6px;border:0}.sec-d th{position:static}
.th-rs{color:var(--mut);font-size:10px;max-width:420px;white-space:normal}
#secsort,#secgrp,#thsort{background:var(--panel2);color:var(--fg);border:1px solid var(--bd);border-radius:6px;font-size:11px}
/* 보유 종목은 옆 국면 카드 높이만큼 채우고 넘칠 때만 스크롤. 절대 배치라 표 길이가 행 높이를 키우지 않는다. */
.card.fill{position:relative;min-height:340px}
.card.fill .scroll{position:absolute;top:38px;left:14px;right:14px;bottom:12px;max-height:none}
.feed{max-height:360px;overflow:auto;font-family:'Cascadia Code',Consolas,monospace;font-size:11.5px}
.ev{display:flex;gap:8px;padding:3px 0;border-bottom:1px dashed var(--bd)}
.ev .t{color:var(--mut);flex:0 0 62px}
.tag{flex:0 0 74px;font-weight:700;border-radius:4px;padding:0 6px;text-align:center;height:16px;line-height:16px;font-size:10px}
.t-signal{background:#2a3550;color:var(--acc)}.t-order{background:#2a3d33;color:var(--up)}
.t-fill{background:#123d24;color:#67e39a}.t-reject,.t-error{background:#3d1f1f;color:var(--dn)}
.t-gate_block{background:#3d331a;color:var(--warn)}.t-liq_block{background:#3d1f1f;color:#ff9d5d}
.pill{border-radius:6px;padding:2px 8px;font-weight:700;font-size:12px}
.crit{font-size:12px;color:var(--fg)}.crit div{padding:3px 0;border-bottom:1px solid var(--bd)}
.crit .k{color:var(--mut);display:inline-block;min-width:150px}
.bad{color:var(--dn)}.warnc{color:var(--warn)}
small.err{color:var(--dn)}
.muted{color:var(--mut);font-size:11px;margin-top:6px}
.clk{cursor:pointer}.clk:hover td{background:var(--panel2)}
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,.55);display:none;z-index:50;align-items:center;justify-content:center}
.modal-bg.on{display:flex}
.modal{background:var(--panel);border:1px solid var(--bd);border-radius:12px;width:min(920px,94vw);max-height:92vh;overflow:hidden;box-shadow:0 20px 60px rgba(0,0,0,.4)}
.modal .mh{display:flex;align-items:center;gap:12px;padding:12px 16px;border-bottom:1px solid var(--bd)}
.modal .mh b{font-size:15px}.modal .mh .x{margin-left:auto;cursor:pointer;color:var(--mut);font-size:20px;line-height:1}
.tabs{display:flex;gap:6px;padding:10px 16px 0}
.tab{background:var(--chip);border:1px solid var(--bd);border-radius:7px;padding:5px 12px;cursor:pointer;font-size:12px;color:var(--mut)}
.tab.on{background:var(--acc);color:#fff;border-color:var(--acc)}
.chartwrap{padding:12px 16px 16px;position:relative}
#chartcv{width:100%;height:380px;display:block}
.chartinfo{color:var(--mut);font-size:11px;margin-top:6px;min-height:14px}
.regime-comp{display:grid;grid-template-columns:1fr auto auto auto;gap:2px 10px;font-size:12px;margin-top:8px}
.regime-comp .rp{font-variant-numeric:tabular-nums}
.regime-comp .rn{color:var(--mut)}
.regime-comp .note{grid-column:1/-1;color:var(--mut);font-size:11px;margin:-2px 0 4px 8px}
.regime-comp .sec{grid-column:1/-1;color:var(--mut);font-size:11px;margin-top:6px;border-top:1px solid var(--chip);padding-top:4px}
.regime-sum{font-size:12px;margin-top:8px;line-height:1.4}
.tickrow td:first-child{color:var(--mut)}
</style></head><body>
<header>
  <h1>실시간 매매 대시보드</h1>
  <span class="chip">계좌 <b id="acct">–</b></span>
  <span class="chip" id="paper">–</span>
  <span class="chip" id="engine">–</span>
  <span class="chip" id="mkt">–</span>
  <span class="chip" id="daily">–</span>
  <span style="flex:1"></span>
  <span class="chip">갱신 <b id="ts">–</b></span>
  <span class="chip" id="conn">연결중…</span>
</header>
<div class="grid">
  <div class="card col12"><h2>계좌 현황</h2><div class="kpis" id="kpis"></div><div class="muted" id="acctnote"></div></div>

  <div class="card col8 fill"><h2>보유 종목 (점수순 · 평가손익)</h2><div class="scroll"><table id="pos">
    <thead><tr><th>순위</th><th>점수 z</th><th class="l">종목</th><th class="l">코드</th><th>수량</th><th>평단</th><th>현재가</th><th>평가금</th><th>손익</th><th>손익률</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col4"><h2>국면 (Regime)</h2><div id="regime"></div></div>

  <div class="card col6"><h2>장중 매매 기준</h2><div class="crit" id="criteria"></div></div>

  <div class="card col12"><h2>엔진 로그 최신 (콘솔 상당)</h2><div class="crit" id="monitor"></div></div>

  <div class="card col6"><h2>매매 리스트 (유니버스)</h2>
    <div class="muted" id="uninote"></div>
    <div class="scroll"><table id="uni">
    <thead><tr><th>#</th><th class="l">종목</th><th class="l">코드</th><th>종가</th><th class="l">시장</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col6"><h2>이벤트 피드 (콘솔 상당)</h2><div class="feed" id="feed"></div></div>

  <div class="card col6"><h2>거래대금 상위 (스냅샷)</h2><div class="scroll"><table id="rank">
    <thead><tr><th>#</th><th class="l">종목</th><th>현재가</th><th>등락%</th><th>거래대금</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col6"><h2>섹터 변동성 <span class="mut" id="secnote"></span>
    <span class="mut" style="float:right"><select id="secgrp"><option value="">전체</option><option value="0">코스피</option><option value="1">코스닥</option><option value="2">KRX 테마</option></select>
      정렬: <select id="secsort"><option value="pct">당일%</option><option value="z">z</option><option value="vol">20일σ</option><option value="rng">진폭</option><option value="r5">5일%</option></select> · 행 클릭 → 시총 상위 10</span></h2>
    <div class="scroll sec-scroll"><table id="sector">
    <thead><tr><th>#</th><th class="l">업종</th><th class="l">시장</th><th>현재</th><th>당일%</th><th>5일%</th><th>20일σ<span class="mut">연율</span></th><th>진폭%</th><th>z</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col6"><h2>테마 순위 <span class="mut" id="thnote"></span>
    <span class="mut" style="float:right">정렬: <select id="thsort"><option value="pct">당일%</option><option value="breadth">상승 비율</option><option value="hold">보유</option></select> · 행 클릭 → 구성 종목</span></h2>
    <div class="scroll sec-scroll"><table id="theme">
    <thead><tr><th>#</th><th class="l">테마</th><th>당일%</th><th>상승/하락</th><th>종목</th><th>보유</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col6"><h2>보유 종목 테마 분포 <span class="mut" id="htnote"></span></h2>
    <div class="scroll sec-scroll"><table id="htheme">
    <thead><tr><th class="l">테마</th><th>보유</th><th class="l">종목</th></tr></thead>
    <tbody></tbody></table></div></div>

  <div class="card col12"><h2>당일 체결 원장 <span class="mut" id="trdate"></span> <span class="mut" style="float:right">행 클릭 → 차트</span></h2><div class="scroll"><table id="trades">
    <thead><tr><th>시각</th><th class="l">이벤트</th><th class="l">전략</th><th class="l">종목</th><th class="l">방향</th><th>주문</th><th>체결</th><th>체결가</th><th class="l">상태</th><th class="l">사유</th></tr></thead>
    <tbody></tbody></table></div></div>
</div>

<div class="modal-bg" id="modalbg">
  <div class="modal">
    <div class="mh"><b id="mname">–</b><span class="mut" id="mtk"></span><span class="x" id="mx">×</span></div>
    <div class="tabs" id="tabs">
      <span class="tab on" data-tf="D">일봉</span><span class="tab" data-tf="W">주봉</span>
      <span class="tab" data-tf="5">5분봉</span><span class="tab" data-tf="3">3분봉</span>
    </div>
    <div class="chartwrap"><canvas id="chartcv"></canvas><div class="chartinfo" id="chartinfo"></div></div>
  </div>
</div>
<script>
const won=n=>n==null||isNaN(n)?'–':Math.round(n).toLocaleString('ko-KR');
const pct=n=>n==null||isNaN(n)?'–':(n>=0?'+':'')+Number(n).toFixed(2)+'%';
const cls=n=>n>0?'up':(n<0?'dn':'');
const ipx=v=>{const n=Number(v);
  if(!isFinite(n)||v===null||v===undefined) return '-';
  return Math.abs(n)>=1000 ? n.toLocaleString('ko-KR',{maximumFractionDigits:0})
                           : n.toLocaleString('ko-KR',{minimumFractionDigits:2,maximumFractionDigits:2});};
const eb=v=>String(v==null?'':v).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function setDot(el,ok,txt){el.innerHTML='<span class="dot" style="background:'+(ok?'var(--up)':'var(--dn)')+'"></span>'+txt;}

// 종목코드→종목명. 코드만 보고 따로 검색해야 하던 것을 없앤다. 매 tick마다 s.names로 갱신된다.
let NAMES={};
const nmOf=tk=>NAMES[String(tk||'').trim()]||'';
// 문장 속 6자리 코드에 이름을 붙인다. 이미 '코드(이름)' 형태면 건드리지 않는다.
function withNames(msg){
  return eb(msg).replace(/(\d{6})(\()?/g,(m,tk,paren)=>{
    if(paren) return m;                 // 로그가 이미 이름을 달고 있다
    const nm=nmOf(tk);
    return nm? tk+'('+eb(nm)+')' : tk;
  });
}

document.getElementById('secsort').addEventListener('change',renderSector);
document.getElementById('secgrp').addEventListener('change',renderSector);
document.getElementById('thsort').addEventListener('change',renderTheme);
let lastTheme={}, lastHT={};
// 테마 순위 — 266개를 당일 등락률(구성 종목 단순평균)로 세운다. 소형주 상한가 하나가 평균을 끌어올리므로
//  상승/하락 종목 수를 같이 보인다. '보유'는 일일 스냅샷 역색인으로 센 보유 종목 수.
function renderTheme(){
  const ks=lastTheme; const tb=document.querySelector('#theme tbody'); const tn=document.getElementById('thnote');
  if(ks.__error__){ tb.innerHTML='<tr><td class="l err" colspan="6">테마 조회 실패: '+eb(ks.__error__)+'</td></tr>'; tn.textContent=''; return; }
  const key=document.getElementById('thsort').value; const byNo=(lastHT.by_no||{});
  const rows=(ks.rows||[]).map(r=>({...r, breadth:r.count?r.rise/r.count:0, hold:byNo[String(r.no)]||0}));
  rows.sort((a,b)=>(b[key]??-1e9)-(a[key]??-1e9) || (b.pct??-1e9)-(a.pct??-1e9));
  tn.textContent=(ks.asof?'· '+ks.asof+' 기준 · '+rows.length+'개':'')+(ks._stale_err?' · 갱신 지연':'');
  tb.innerHTML=rows.length? rows.map((r,i)=>`<tr class="clk" data-th="${r.no}"><td>${i+1}</td><td class="l">${eb(r.name)}</td><td class="${cls(r.pct)}">${pct(r.pct)}</td><td><span class="up">${r.rise}</span><span class="mut">/</span><span class="dn">${r.fall}</span></td><td class="mut">${r.count}</td><td>${r.hold||''}</td></tr>`+(thOpen.has(String(r.no))?thDetailRow(String(r.no)):'')).join('')
    : '<tr><td class="l mut" colspan="6">데이터 없음</td></tr>';
}
function renderHT(){
  const h=lastHT; const tb=document.querySelector('#htheme tbody'); const tn=document.getElementById('htnote');
  if(!h.asof){ tb.innerHTML='<tr><td class="l mut" colspan="3">스냅샷 없음 — py -X utf8 PYQuant/tools/fetch_naver_themes.py</td></tr>'; tn.textContent=''; return; }
  tn.textContent='· 스냅샷 '+h.asof.slice(0,10)+' · 보유 '+(h.n_pos||0)+'종목 · 2종목 이상 겹친 테마만';
  let html=(h.rows||[]).map(r=>`<tr class="clk" data-th="${r.no}"><td class="l">${eb(r.name)}</td><td>${r.n}</td><td class="l mut th-rs">${eb(r.names.join(' · '))}</td></tr>`).join('');
  if((h.untagged||[]).length) html+=`<tr><td class="l mut">테마 없음</td><td class="mut">${h.untagged.length}</td><td class="l mut th-rs">${eb(h.untagged.join(' · '))}</td></tr>`;
  tb.innerHTML=html||'<tr><td class="l mut" colspan="3">겹치는 테마 없음</td></tr>';
}
let lastSector={};
function renderSector(){
  // 섹터 변동성 — 60여 업종을 선택한 열 기준으로 한 순위표에 세운다(그룹 필터 선택 가능).
  //  z는 당일 등락/20일 일간σ. 열린 업종(secOpen)은 아래에 시총 상위 10을 붙이고, 3초 재렌더에도 유지한다.
  const ks=lastSector; const sb=document.querySelector('#sector tbody'); const sn=document.getElementById('secnote');
  if(ks.__error__){ sb.innerHTML='<tr><td class="l err" colspan="9">업종 조회 실패: '+eb(ks.__error__)+'</td></tr>'; sn.textContent=''; }
  else{
    const key=document.getElementById('secsort').value; const grp=document.getElementById('secgrp').value;
    const f2=v=>v==null?'—':Number(v).toFixed(2); const fz=v=>v==null?'—':(v>0?'+':'')+Number(v).toFixed(1)+'σ';
    const zc=v=>v==null?'mut':(Math.abs(v)>=2?'sec-hot':'');
    const GL=['코스피','코스닥','KRX'];
    sn.textContent=(ks.asof?'· '+ks.asof+' 기준':'')+(ks.days?' · σ는 '+ks.days+'일 로그수익률':'')+(ks.partial?' · '+ks.partial:'')+(ks._stale_err?' · 갱신 지연':'');
    const rows=[]; (ks.groups||[]).forEach((g,gi)=>{ if(grp===''||grp===String(gi)) g.rows.forEach(r=>rows.push({...r,g:GL[gi]})); });
    rows.sort((a,b)=>(b[key]??-1e9)-(a[key]??-1e9));
    sb.innerHTML=rows.length? rows.map((r,i)=>`<tr class="clk sec-row${secOpen.has(r.code)?' sec-on':''}" data-sec="${eb(r.code)}"><td>${i+1}</td><td class="l">${eb(r.name)}</td><td class="l mut">${r.g}</td><td>${f2(r.price)}</td><td class="${cls(r.pct)}">${pct(r.pct)}</td><td class="${cls(r.r5)}">${pct(r.r5)}</td><td>${r.vol==null?'—':f2(r.vol)+'%'}</td><td>${r.rng==null?'—':f2(r.rng)+'%'}</td><td class="${cls(r.z)} ${zc(r.z)}">${fz(r.z)}</td></tr>`+(secOpen.has(r.code)?secDetailRow(r.code):'')).join('')
      : '<tr><td class="l mut" colspan="9">데이터 없음</td></tr>';
  }
}
async function tick(){
  let s;
  try{ s=await (await fetch('/api/state',{cache:'no-store'})).json(); }
  catch(e){ document.getElementById('conn').textContent='연결끊김'; return; }
  const conn=document.getElementById('conn'); conn.textContent='LIVE'; conn.style.color='var(--up)';
  if(s.__error__){ conn.textContent='서버오류'; return; }

  document.getElementById('acct').textContent=s.account_no||'–';
  NAMES=s.names||{};
  document.getElementById('ts').textContent=(s.server_ts||'').slice(11);
  const paper=document.getElementById('paper'); paper.innerHTML=(s.is_paper?'모의계좌':'실계좌')+' · '+eb(s.mode);
  setDot(document.getElementById('engine'), s.engine&&s.engine.alive, '엔진 '+(s.engine&&s.engine.alive?'가동중':'정지')+(s.engine&&s.engine.log_mtime?' ('+s.engine.log_mtime+')':''));
  setDot(document.getElementById('mkt'), s.market&&s.market.open, s.market&&s.market.open?'장중':'장마감');

  // 당일손익 (로그에서)
  const dl=s.log&&s.log.latest&&s.log.latest.daily_pnl_line;
  document.getElementById('daily').innerHTML= dl? '당일손익 '+eb(dl.replace(/^.*당일손익[^\-\d]*/,'').slice(0,40)) : '당일손익 –';

  // KPI
  const a=s.account||{}, sm=(a.summary)||{};
  if(a.__error__){
    document.getElementById('kpis').innerHTML='';
    document.getElementById('acctnote').innerHTML='<small class="err">잔고 조회 실패/지연: '+eb(a.__error__)+' (모의 도메인 응답 지연 시 자동 재시도 중)</small>';
  }else{
    document.getElementById('acctnote').innerHTML= a._stale_err
      ? '<small class="warnc">현재 조회 지연('+eb(a._stale_age)+'s) — 마지막 정상값 표시 중: '+eb(a._stale_err)+'</small>' : '';
    // 총노출 = 보유분 원가합(총매수금액) / 총평가금. OrderGate §3d와 같은 기준이라 한도 소진율을 그대로 읽는다.
    const gross=sm.buy_amount||0, capPct=(((s.criteria||{}).risk||{}).max_gross_exposure_pct)||0;
    const useRate= sm.total_eval? gross/sm.total_eval*100 : null;
    const capTxt = !capPct? '한도 미설정'
                 : (useRate==null?'–':useRate.toFixed(1)+'% / '+(capPct*100).toFixed(0)+'%');
    const capCls = (capPct&&useRate!=null)? (useRate>=capPct*100?'dn':(useRate>=capPct*80?'warnc':'up')) : '';
    const maxN=(((s.criteria||{}).risk||{}).max_concurrent_positions)||0;
    const rz=(s.trades&&s.trades.realized)||{profit:0,loss:0,net:0,win:0,lose:0,unknown:0};
    document.getElementById('kpis').innerHTML=[
      ['총평가금액',won(sm.total_eval)+' 원',''],
      ['총매수금액(원가)',won(gross)+' 원',''],
      ['가용현금(D+2)',won(sm.cash_avail)+' 원',''],
      ['예수금(총·결제전)',won(sm.cash)+' 원','mut'],
      ['총노출 / 한도',capTxt,capCls],
      ['평가손익',won(sm.total_pnl)+' 원',cls(sm.total_pnl)],
      ['총수익률',pct(sm.total_pnl_rate),cls(sm.total_pnl_rate)],
      ['보유 종목수',(a.positions?a.positions.length:0)+(maxN?' / '+maxN:'')+' 종목',''],
      ['오늘 익절',won(rz.profit)+' 원'+(rz.win?' ('+rz.win+'건)':''),rz.profit>0?'up':''],
      ['오늘 손절',won(rz.loss)+' 원'+(rz.lose?' ('+rz.lose+'건)':''),rz.loss<0?'dn':''],
      ['오늘 실현손익',won(rz.net)+' 원'+(rz.unknown?' *'+rz.unknown+'건 평단미상':''),cls(rz.net)],
    ].map(k=>`<div class="kpi"><div class="l">${k[0]}</div><div class="v ${k[2]}">${k[1]}</div></div>`).join('');
  }

  // 보유종목
  const pb=document.querySelector('#pos tbody');
  // 점수는 엔진 entry_scores.json(스캔마다 갱신). 점수 없는 종목은 교체 진입에서 unscored_z로
  //  취급되어 먼저 밀려나므로 맨 아래에 둔다. [why D-047]
  const es=s.entry_scores||{}; const sc=es.scores||{}; const tot=es.total||0;
  const pos=((a.positions)||[]).map(p=>{const e=sc[p.ticker]; return Object.assign({},p,{_rank:e?e.rank:null,_z:e?e.z:null});})
    .sort((x,y)=>{ if(x._z==null&&y._z==null) return 0; if(x._z==null) return 1; if(y._z==null) return -1; return y._z-x._z; });
  const zs=v=>v==null?'<span class="mut">점수 없음</span>':(v>=0?'+':'')+Number(v).toFixed(2)+'σ';
  const zc=v=>v==null?'dn':(v>0?'up':(v<0?'dn':''));
  pb.innerHTML= pos.length? pos.map(p=>`<tr class="clk" data-tk="${eb(p.ticker)}" data-nm="${eb(p.name)}">
    <td class="mut">${p._rank!=null?p._rank+(tot?'/'+tot:''):'–'}</td><td class="${zc(p._z)}">${zs(p._z)}</td>
    <td class="l">${eb(p.name)}</td><td class="l mut">${eb(p.ticker)}</td>
    <td>${won(p.qty)}</td><td>${won(p.avg)}</td><td>${won(p.cur)}</td><td>${won(p.eval)}</td>
    <td class="${cls(p.pnl)}">${won(p.pnl)}</td><td class="${cls(p.pnl_rate)}">${pct(p.pnl_rate)}</td></tr>`).join('')
    : '<tr><td class="l mut" colspan="10">보유 종목 없음</td></tr>';

  // 국면
  const r=s.regime||{}; const rd=document.getElementById('regime');
  if(r.__error__){ rd.innerHTML='<small class="err">'+eb(r.__error__)+'</small>'; }
  else{
    const halt=r.entry_halt, liq=r.force_liquidate, stale=r._stale;
    const rc=r.regime==='RISK_ON'?'up':(r.regime==='RISK_OFF'?'dn':'mut');
    // 국내 지수(KIS 실시간)는 regime.json과 별도 수집이라 앞에 붙인다. pct는 전일 종가 대비.
    const ki=s.kr_index||{}; let comp='';
    if(ki.__error__){ comp+=`<div class="sec">국내 지수 — ${eb(ki.__error__)}</div>`; }
    else{
      comp+=`<div class="sec">국내 지수 (KIS 실시간${ki._stale_err?' · 지연 '+eb(ki._stale_age)+'s':''})</div>`;
      for(const c of (ki.rows||[])){
        comp+=`<div class="rn">${eb(c.label)}</div><div class="rp">${ipx(c.price)}</div><div class="${cls(c.pct)}">${pct(c.pct)}</div><div class="mut">현물</div>`;}
    }
    // 국내 수급·프로그램·선물(KIS 실시간, 억원). 외인 선물 순매수는 REST 경로가 없어 미확정. [why D-044]
    const kf=s.kr_flow||{}; const sgn=v=>v==null?'—':(v>0?'+':'')+Math.round(v).toLocaleString();
    const wcl=v=>v==null?'mut':(v>0?'up':(v<0?'dn':'mut'));
    const cell3=(a,b,c)=>`<div class="rp ${wcl(a)}">${sgn(a)}</div><div class="rp ${wcl(b)}">${sgn(b)}</div><div class="rp ${wcl(c)}">${sgn(c)}</div>`;
    const hhmm=h=>h?String(h).slice(0,2)+':'+String(h).slice(2,4):'';
    const fx2=v=>v==null?'—':Number(v).toLocaleString('ko-KR',{minimumFractionDigits:2,maximumFractionDigits:2});
    if(kf.__error__){ comp+=`<div class="sec">국내 수급·선물 — ${eb(kf.__error__)}</div>`; }
    else{
      comp+=`<div class="sec">투자자 순매수 (당일 잠정 · 억원${kf._stale_err?' · 지연 '+eb(kf._stale_age)+'s':''})</div>`;
      comp+=`<div class="rn"></div><div class="mut">외인</div><div class="mut">기관</div><div class="mut">개인</div>`;
      for(const r of (kf.investor||[])){ comp+=`<div class="rn">${eb(r.label)}</div>`+cell3(r.frgn,r.orgn,r.prsn); }
      const p0=(kf.program||[])[0];
      comp+=`<div class="sec">프로그램 순매수 (억원${p0&&p0.hour?' · '+eb(hhmm(p0.hour)):''})</div>`;
      comp+=`<div class="rn"></div><div class="mut">전체</div><div class="mut">차익</div><div class="mut">비차익</div>`;
      for(const r of (kf.program||[])){ comp+=`<div class="rn">${eb(r.label)}</div>`+cell3(r.whol,r.arbt,r.nabt); }
      const f=kf.future;
      if(f){
        comp+=`<div class="sec">KOSPI200 선물 최근월 (${eb(f.name||f.code||'')} · 만기 ${eb(f.expiry||'?')} · 잔존 ${eb(f.days||'?')}일)</div>`;
        comp+=`<div class="rn">선물가</div><div class="rp">${fx2(f.price)}</div><div class="${cls(f.pct)}">${pct(f.pct)}</div><div class="mut">현물 ${fx2(f.k200)}</div>`;
        comp+=`<div class="rn">시장 베이시스</div><div class="rp ${wcl(f.basis)}">${f.basis==null?'—':f.basis.toFixed(2)}</div><div class="${wcl(f.dprt)}">${f.dprt==null?'—':'괴리 '+f.dprt.toFixed(2)+'%'}</div><div class="mut">이론가 ${fx2(f.theo)}</div>`;
        comp+=`<div class="rn">미결제약정</div><div class="rp">${f.oi==null?'—':Math.round(f.oi).toLocaleString()}</div><div class="${wcl(f.oi_chg)}">${f.oi_chg==null?'—':(f.oi_chg>0?'+':'')+Math.round(f.oi_chg).toLocaleString()}</div><div class="mut">외인 선물 순매수 미확정</div>`;
        if(f.basis!=null){ comp+=`<div class="note">${eb(f.basis<0?'백워데이션(선물이 현물 아래). 헤지 매도·약세 기대가 실린 상태':(f.basis>1?'콘탱고 확대. 차익 매수 유입 여지':'베이시스 중립'))}</div>`; }
      }
      if(kf.partial){ comp+=`<div class="note">일부 실패: ${eb(kf.partial)}</div>`; }
    }
    // 간밤 해외(regime.json). tier=info는 표를 내지 않는 참고 지표. note는 절대 수준 평가.
    const comps=r.components||{}; const gate=[], info=[];
    for(const k in comps){ (comps[k].tier==='info'?info:gate).push(comps[k]); }
    const row=(c,tag)=>`<div class="rn">${eb(c.label||'')}</div><div class="rp">${ipx(c.price)}</div><div class="${cls(c.pct)}">${pct(c.pct)}</div><div class="mut">${tag}</div>`
      +(c.note?`<div class="note">${eb(c.note)}</div>`:'');
    comp+=`<div class="sec">간밤 해외 (전일 종가 대비 · 표결)</div>`;
    for(const c of gate){ comp+=row(c,'vote '+eb(c.vote)); }
    if(info.length){ comp+=`<div class="sec">참고 (표 없음)</div>`; for(const c of info){ comp+=row(c,'참고'); } }
    const sum=(r.assessment||{}).summary;
    rd.innerHTML=`
      <div><span class="pill ${rc}" style="background:var(--chip)">${eb(r.regime||'?')}</span>
        <span class="mut"> score ${eb(r.risk_score)}</span>
        ${stale?'<span class="pill bad" style="background:var(--chip)"> STALE '+eb(r._age_sec)+'s</span>':''}</div>
      <div style="margin-top:8px">
        신규매수: <b class="${halt?'bad':'up'}">${halt?'차단(entry_halt)':'허용'}</b><br>
        강제청산: <b class="${liq?'bad':''}">${liq?'ON(force_liquidate)':'off'}</b>
      </div>
      ${sum?`<div class="regime-sum">${eb(sum)}</div>`:''}
      <div class="regime-comp">${comp}</div>
      <div class="muted">기준 halt≤${eb((r.thresholds||{}).halt_score)} · liq≤${eb((r.thresholds||{}).liq_score)} · ${eb(r.ts||'')}</div>`;
  }

  // 기준
  const c=s.criteria||{}; const st=(c.strategies&&c.strategies[0])||{};
  const rs=c.regime_strategies||{}; const rk=c.risk||{};
  document.getElementById('criteria').innerHTML=`
    <div><span class="k">국면별 전략</span> BULL=[${eb((rs.BULL||[]).join(', '))}] · NEUTRAL=[${eb((rs.NEUTRAL||[]).join(', '))}] · BEAR=[${eb((rs.BEAR||[]).join(', '))||'없음(청산)'}]</div>
    <div><span class="k">진입 로직</span> 시총상위∪거래대금상위 스캔 → 일봉 정배열(SMA5&gt;10&gt;20&gt;60)${st.require_aligned?' 필수':''} + 눌림 존</div>
    <div><span class="k">스캔 규모</span> 시총 top ${eb(st.scan_top_n)} ∪ 거래대금 top ${eb(st.value_top_n)} → 등록상한 ${eb(st.max_universe)}종목</div>
    <div><span class="k">가격 필터</span> ${won(st.min_price)}원 이상${st.max_price?(' ~ '+won(st.max_price)+'원'):' (상한 무제한)'} · 과확장컷 ${eb(st.max_dev_pct)}</div>
    <div><span class="k">코스닥</span> ${c.kosdaq_enabled?'참여':'미참여(코스피만)'} · 폴링 ${eb(c.fetch_interval_sec)}s · 시세 ${c.rest_price_feed?'REST폴링':'WS'}</div>
    <div><span class="k">리스크 한도</span> 동시보유 ${eb(rk.max_concurrent_positions)} · 종목당 명목 ${won(rk.max_notional_per_ticker)}원 · 총노출 ${rk.max_gross_exposure_pct?(rk.max_gross_exposure_pct*100).toFixed(0)+'%':'미설정'} · 일손실 한도 ${won(rk.daily_loss_limit)}원</div>
    <div><span class="k">발주 제한</span> ${eb(rk.max_orders_per_sec)}/s · ${eb(rk.max_orders_per_min)}/min · 재시도 ${eb(rk.order_max_retries)}</div>`;

  // 엔진 로그 최신(콘솔 상당: 국면선택/스캔/섹터/수급/매크로)
  const L=(s.log&&s.log.latest)||{};
  const mrow=(k,v)=>v?`<div><span class="k">${k}</span>${eb(v)}</div>`:'';
  const mon=[
    mrow('국면 선택', L.regime_select),
    mrow('유니버스 스캔', L.scan_line),
    mrow('섹터 강약', L.sector),
    mrow('수급 추정', L.supply),
    mrow('매크로', L.macro),
    mrow('당일손익', L.daily_pnl_line),
  ].join('');
  document.getElementById('monitor').innerHTML= mon || '<div class="mut">엔진 로그 최신 라인 없음 (엔진 미가동)</div>';

  // 유니버스
  const u=s.universe||{}; const ub=document.querySelector('#uni tbody');
  if(u.__error__){ document.getElementById('uninote').innerHTML='<small class="err">'+eb(u.__error__)+'</small>'; ub.innerHTML=''; }
  else{
    document.getElementById('uninote').textContent=`기준일 ${u.basDt||'?'} · ${u.market||''} · ${u.count||0}종목 · ${u.source||''}`;
    const uni=(u.universe||[]).slice(0,120);
    ub.innerHTML=uni.map((x,i)=>`<tr class="tickrow clk" data-tk="${eb(x.ticker)}" data-nm="${eb(x.name)}"><td>${i+1}</td><td class="l">${eb(x.name)}</td><td class="l mut">${eb(x.ticker)}</td><td>${won(x.close)}</td><td class="l mut">${eb(x.market)}</td></tr>`).join('');
  }

  // 이벤트 피드 — 코드만 있으면 종목명을 붙여준다(로그가 이미 '코드(이름)'이면 그대로 둔다)
  const fe=document.getElementById('feed'); const evs=(s.log&&s.log.events)||[];
  fe.innerHTML= evs.length? evs.map(e=>`<div class="ev"><span class="t">${eb(e.ts)}</span><span class="tag t-${e.cat}">${eb(e.cat)}</span><span>${withNames(e.msg)}</span></div>`).join('')
    : '<div class="mut">최근 이벤트 없음 (엔진 미가동이거나 조용)</div>';

  // 거래대금 상위
  const rr=s.ranking||{}; const rb=document.querySelector('#rank tbody');
  if(rr.__error__){ rb.innerHTML='<tr><td class="l err" colspan="5">랭킹 조회 실패: '+eb(rr.__error__)+'</td></tr>'; }
  else{ const rows=(rr.rows)||[];
    rb.innerHTML= rows.length? rows.map(x=>`<tr class="clk" data-tk="${eb(x.ticker)}" data-nm="${eb(x.name)}"><td>${eb(x.rank)}</td><td class="l">${eb(x.name)}</td><td>${won(x.price)}</td><td class="${cls(x.change_rate)}">${pct(x.change_rate)}</td><td>${won(x.trade_value/1e8)}억</td></tr>`).join('')
      : '<tr><td class="l mut" colspan="5">데이터 없음</td></tr>';
  }

  lastSector=s.kr_sector||{}; renderSector();
  lastTheme=s.kr_theme||{}; lastHT=s.holding_themes||{}; renderTheme(); renderHT();

  // 원장
  const SL=s.labels||{}; const t=s.trades||{}; document.getElementById('trdate').textContent=(t.date||'')+(t.total?(' · 총 '+t.total+'행'):'');
  const tb=document.querySelector('#trades tbody');
  tb.innerHTML=(t.rows&&t.rows.length)? t.rows.map(x=>{
    const side=(x.side||''); const sc=side==='BUY'?'up':(side==='SELL'?'dn':'');
    const tkc=/^\d{6}$/.test(x.ticker||'')?'clk':'';
    return `<tr class="${tkc}" data-tk="${eb(x.ticker)}" data-nm="${eb(nmOf(x.ticker)||x.ticker)}"><td>${eb((x.ts_kst||'').slice(11,19))}</td><td class="l">${eb(x.event)}</td><td class="l mut">${eb(SL[x.strategy]||x.strategy||"")}</td>
      <td class="l">${eb(x.ticker)}${nmOf(x.ticker)?' <span class="mut">'+eb(nmOf(x.ticker))+'</span>':''}</td><td class="l ${sc}">${eb(side)}</td><td>${eb(x.order_qty)}</td><td>${eb(x.fill_qty)}</td>
      <td>${won(x.fill_price)}</td><td class="l">${eb(x.status)}</td><td class="l mut">${eb(x.reason||x.entry_reason||'')}</td></tr>`;
  }).join('') : `<tr><td class="l mut" colspan="10">${eb(t.note||'당일 체결 없음')}</td></tr>`;
}
// ── 차트 모달 ──────────────────────────────────────────────────────────────
let curTk='', curNm='', curTf='D';
const bg=document.getElementById('modalbg');
// 섹터 대표 종목: 열린 업종 코드 집합과 받아 둔 응답. 종목 행을 누르면 차트가 열린다(tr.clk data-tk).
const secOpen=new Set(); const secCache={};
function secDetailRow(code){
  const d=secCache[code];
  let inner;
  if(!d) inner='<span class="mut">불러오는 중…</span>';
  else if(d.__error__) inner='<span class="err">조회 실패: '+eb(d.__error__)+'</span>';
  else if(!d.rows||!d.rows.length) inner='<span class="mut">'+eb(d.note||'구성 종목 없음')+'</span>';
  else inner='<table><thead><tr><th>#</th><th class="l">종목</th><th>현재가</th><th>등락%</th><th>시총</th></tr></thead><tbody>'+
    d.rows.map(x=>`<tr class="clk" data-tk="${eb(x.ticker)}" data-nm="${eb(x.name)}"><td>${x.rank}</td><td class="l">${eb(x.name)}</td><td>${won(x.price)}</td><td class="${cls(x.change_rate)}">${pct(x.change_rate)}</td><td>${x.market_cap>=10000?(x.market_cap/10000).toFixed(1)+'조':won(x.market_cap)+'억'}</td></tr>`).join('')+
    '</tbody></table>'+(d.asof?'<span class="mut">시총 상위 10 · '+eb(d.asof)+'</span>':'');
  return `<tr class="sec-d"><td colspan="9">${inner}</td></tr>`;
}
// 테마 구성 종목: 등락률순 + 편입 사유(인포스탁 한 줄). 보유 분포 카드의 행도 같은 토글을 쓴다.
const thOpen=new Set(); const thCache={};
function thDetailRow(no){
  const d=thCache[no];
  let inner;
  if(!d) inner='<span class="mut">불러오는 중…</span>';
  else if(d.__error__) inner='<span class="err">조회 실패: '+eb(d.__error__)+'</span>';
  else if(!d.rows||!d.rows.length) inner='<span class="mut">'+eb(d.note||'구성 종목 없음')+'</span>';
  else inner=(d.description?'<div class="th-rs" style="max-width:none;margin:2px 0 6px">'+eb(d.description)+'</div>':'')+
    '<table><thead><tr><th>#</th><th class="l">종목</th><th>현재가</th><th>등락%</th><th>거래대금</th><th class="l">편입 사유</th></tr></thead><tbody>'+
    d.rows.map((x,i)=>`<tr class="clk" data-tk="${eb(x.ticker)}" data-nm="${eb(x.name)}"><td>${i+1}</td><td class="l">${eb(x.name)}</td><td>${won(x.price)}</td><td class="${cls(x.change_rate)}">${pct(x.change_rate)}</td><td>${x.value==null?'—':won(x.value)+'억'}</td><td class="l th-rs">${eb(x.reason)}</td></tr>`).join('')+
    '</tbody></table>'+(d.asof?'<span class="mut">'+eb(d.asof)+' 기준</span>':'');
  return `<tr class="sec-d"><td colspan="6">${inner}</td></tr>`;
}
async function toggleTheme(no){
  if(thOpen.has(no)){ thOpen.delete(no); renderTheme(); return; }
  thOpen.add(no); renderTheme();
  document.getElementById('theme').closest('.card').scrollIntoView({block:'nearest'});
  if(!thCache[no]){
    try{ thCache[no]=await (await fetch('/api/theme?no='+no,{cache:'no-store'})).json(); }
    catch(e){ thCache[no]={__error__:String(e)}; }
    renderTheme();
  }
}
async function toggleSector(code){
  if(secOpen.has(code)){ secOpen.delete(code); renderSector(); return; }
  secOpen.add(code); renderSector();
  if(!secCache[code]){
    try{ secCache[code]=await (await fetch('/api/sector?code='+code,{cache:'no-store'})).json(); }
    catch(e){ secCache[code]={__error__:String(e)}; }
    renderSector();
  }
}
document.addEventListener('click', e=>{
  const tr=e.target.closest('tr.clk');
  if(!tr) return;
  if(tr.dataset.tk){ openChart(tr.dataset.tk, tr.dataset.nm||tr.dataset.tk); }
  else if(tr.dataset.sec){ toggleSector(tr.dataset.sec); }
  else if(tr.dataset.th){ toggleTheme(tr.dataset.th); }
});
document.getElementById('mx').onclick=()=>bg.classList.remove('on');
bg.onclick=e=>{ if(e.target===bg) bg.classList.remove('on'); };
document.addEventListener('keydown',e=>{ if(e.key==='Escape') bg.classList.remove('on'); });
document.querySelectorAll('#tabs .tab').forEach(t=>t.onclick=()=>{
  document.querySelectorAll('#tabs .tab').forEach(x=>x.classList.remove('on'));
  t.classList.add('on'); curTf=t.dataset.tf; loadChart();
});
function openChart(tk,nm){
  curTk=tk; curNm=nm; curTf='D';
  document.getElementById('mname').textContent=nm;
  document.getElementById('mtk').textContent=tk;
  document.querySelectorAll('#tabs .tab').forEach(x=>x.classList.toggle('on',x.dataset.tf==='D'));
  bg.classList.add('on'); loadChart();
}
async function loadChart(){
  const info=document.getElementById('chartinfo'); info.textContent='불러오는 중…';
  const cv=document.getElementById('chartcv'); const ctx=cv.getContext('2d');
  ctx.clearRect(0,0,cv.width,cv.height);
  let d;
  try{ d=await (await fetch(`/api/chart?ticker=${curTk}&tf=${curTf}`,{cache:'no-store'})).json(); }
  catch(e){ info.textContent='조회 실패'; return; }
  if(d.__error__){ info.textContent='오류: '+d.__error__; return; }
  const bars=d.bars||[];
  if(!bars.length){ info.textContent=d.label+' 데이터 없음 (분봉은 장중에만)'; return; }
  const legend=drawCandles(cv,ctx,bars,d);
  const show=Math.max(1,Math.min(bars.length,d.show||bars.length));
  const vis=bars.slice(bars.length-show), last=vis[vis.length-1];
  const ma=legend.map(m=>`<span style="color:${m.c}">━ ${m.p} ${Math.round(m.last).toLocaleString('ko-KR')}</span>`).join('  ');
  const miss=MA_DEFS.filter(x=>!legend.some(m=>m.p===x[0])).map(x=>x[0]);
  info.innerHTML=`${eb(d.label)} · ${vis.length}봉 · 종가 ${Math.round(last.close).toLocaleString('ko-KR')} · ${eb(vis[0][d.x]||'')}~${eb(last[d.x]||'')}`
    +`<div style="margin-top:4px">${ma}`
    +(miss.length?` <span class="mut">(${miss.join('/')}이평은 봉 수 부족)</span>`:'')+`</div>`;
}
// 이동평균 정의 — 기간과 선 색. 전략의 정배열 판정(SMA5>10>20>60)과 같은 기간을 포함한다.
const MA_DEFS=[[5,'#ff9f43'],[10,'#5b9dff'],[20,'#2ec26b'],[60,'#c678dd'],[120,'#ffd166'],[240,'#9aa4bb']];
// 단순이동평균. 값이 아직 없는 앞구간은 null(그리지 않음).
function sma(bars,p){
  const out=new Array(bars.length).fill(null); let s=0;
  for(let i=0;i<bars.length;i++){
    s+=bars[i].close;
    if(i>=p) s-=bars[i-p].close;
    if(i>=p-1) out[i]=s/p;
  }
  return out;
}
function drawCandles(cv,ctx,bars,d){
  const DPR=window.devicePixelRatio||1;
  const W=cv.clientWidth, H=380;
  cv.width=W*DPR; cv.height=H*DPR; cv.style.height=H+'px'; ctx.setTransform(DPR,0,0,DPR,0,0);
  const css=getComputedStyle(document.documentElement);
  const up=css.getPropertyValue('--up').trim(), dn=css.getPropertyValue('--dn').trim();
  const mut=css.getPropertyValue('--mut').trim(), bd=css.getPropertyValue('--bd').trim();
  const padL=8, padR=64, padT=10, volH=64, gap=8, priceH=H-volH-gap-padT-16;
  // 표시 구간: 뒤쪽 show개. 이평은 워밍업분을 포함한 전체로 계산한 뒤 이 구간만 잘라 쓴다.
  const show=Math.max(1,Math.min(bars.length, d.show||bars.length));
  const start=bars.length-show, vis=bars.slice(start);
  const mas=MA_DEFS.map(([p,c])=>({p:p, c:c, v:sma(bars,p).slice(start)}))
                   .filter(m=>m.v.some(x=>x!=null));
  let hi=-1e18, lo=1e18, vmax=0;
  vis.forEach(b=>{ hi=Math.max(hi,b.high); lo=Math.min(lo,b.low); vmax=Math.max(vmax,b.volume); });
  // 이평선이 화면 밖으로 나가지 않게 스케일에 포함
  mas.forEach(m=>m.v.forEach(x=>{ if(x!=null){ hi=Math.max(hi,x); lo=Math.min(lo,x); } }));
  const pad=(hi-lo)*0.06||1; hi+=pad; lo-=pad;
  const cw=(W-padL-padR)/vis.length;
  const bw=Math.max(1,Math.min(14,cw*0.7));
  const py=v=>padT+(hi-v)/(hi-lo)*priceH;
  // 가격 그리드 + 우측 라벨
  ctx.font='10px sans-serif'; ctx.textBaseline='middle';
  for(let i=0;i<=4;i++){ const v=lo+(hi-lo)*i/4, y=py(v);
    ctx.strokeStyle=bd; ctx.globalAlpha=.5; ctx.beginPath(); ctx.moveTo(padL,y); ctx.lineTo(W-padR,y); ctx.stroke(); ctx.globalAlpha=1;
    ctx.fillStyle=mut; ctx.textAlign='left'; ctx.fillText(Math.round(v).toLocaleString('ko-KR'), W-padR+4, y);
  }
  // 캔들 + 거래량
  const volTop=padT+priceH+gap;
  vis.forEach((b,i)=>{
    const x=padL+cw*i+cw/2; const col=b.close>=b.open?up:dn;
    ctx.strokeStyle=col; ctx.fillStyle=col; ctx.lineWidth=1;
    ctx.beginPath(); ctx.moveTo(x,py(b.high)); ctx.lineTo(x,py(b.low)); ctx.stroke();
    const y1=py(b.open), y2=py(b.close); const top=Math.min(y1,y2), hgt=Math.max(1,Math.abs(y2-y1));
    ctx.fillRect(x-bw/2, top, bw, hgt);
    const vh=vmax?(b.volume/vmax)*volH:0;
    ctx.globalAlpha=.55; ctx.fillRect(x-bw/2, volTop+volH-vh, bw, vh); ctx.globalAlpha=1;
  });
  // 이평선 — 값이 있는 구간만 이어 그린다
  ctx.lineWidth=1.3; ctx.lineJoin='round';
  mas.forEach(m=>{
    ctx.strokeStyle=m.c; ctx.beginPath();
    let pen=false;
    m.v.forEach((val,i)=>{
      if(val==null){ pen=false; return; }
      const x=padL+cw*i+cw/2, y=py(val);
      if(pen) ctx.lineTo(x,y); else { ctx.moveTo(x,y); pen=true; }
    });
    ctx.stroke();
  });
  ctx.lineWidth=1;
  // 마지막 값이 있는 이평만 범례로 돌려준다
  return mas.map(m=>({p:m.p, c:m.c, last:m.v[m.v.length-1]})).filter(m=>m.last!=null);
}
tick(); setInterval(tick, 3000);
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser(description="실시간 모의계좌 매매 대시보드")
    ap.add_argument("--config", default="Quant/config/config_dev_paper.json",
                    help="엔진 config 경로 (계좌/전략/경로를 여기서 읽음)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--logs", default=None,
                    help="로그/원장 폴더 강제 지정 (기본: build_win/logs 등 후보 중 최신 quant_trader.log 추적)")
    args = ap.parse_args()

    global LOGS_OVERRIDE
    if args.logs:
        LOGS_OVERRIDE = args.logs

    cfg_path = (REPO / args.config) if not os.path.isabs(args.config) else Path(args.config)
    if not cfg_path.exists():
        # cwd 기준으로도 시도
        cfg_path = Path(args.config)
    with open(cfg_path, encoding="utf-8") as f:
        cfg = json.load(f)
    k = cfg["kis"]
    kis = KisClient(app_key=k["app_key"], app_secret=k["app_secret"],
                    account_no=k["account_no"], account_type=k.get("account_type", "01"),
                    is_paper=k.get("is_paper", False))
    # 시세 전용(실전 도메인) 키가 있으면 차트·랭킹은 그쪽으로 — 모의 도메인 시세 제약 회피
    qk = cfg.get("quote_kis")
    if qk and qk.get("app_key"):
        quote = KisClient(app_key=qk["app_key"], app_secret=qk["app_secret"],
                          account_no=k["account_no"], account_type=k.get("account_type", "01"),
                          is_paper=qk.get("is_paper", False))
    else:
        quote = kis

    # 경로: config의 상대경로는 리포 루트 기준으로 해석
    regime_path = REPO / cfg.get("regime_file", "Quant/config/regime.json")
    strat = cfg.get("strategies") or [{}]
    uni_rel = next((s.get("universe_file") for s in strat
                    if isinstance(s, dict) and s.get("universe_file")),
                   "Quant/config/universe_scan.json")
    uni_path = REPO / uni_rel

    Handler.kis, Handler.quote, Handler.cfg = kis, quote, cfg
    Handler.cfg_path = cfg_path
    Handler.cfg_mtime = os.path.getmtime(cfg_path)
    Handler.regime_path, Handler.uni_path = regime_path, uni_path

    # 잔고·랭킹은 백그라운드로 수집 → HTTP 요청 스레드가 KIS 지연에 물리지 않음
    threading.Thread(target=_warm_loop, args=(kis, quote), daemon=True).start()
    threading.Thread(target=_sector_loop, args=(quote,), daemon=True).start()
    if fetch_theme_list is not None:
        threading.Thread(target=_theme_loop, daemon=True).start()

    print(f"[대시보드] config={cfg_path.name} 계좌={_mask_acct(k['account_no'])} "
          f"모의={k.get('is_paper')}", flush=True)
    print(f"[대시보드] regime={regime_path}  universe={uni_path}", flush=True)
    print(f"[대시보드] logs={logs_dir()}  (원장·엔진로그 추적 위치)", flush=True)
    print(f"[대시보드] http://{args.host}:{args.port}  (3초 폴링, Ctrl+C 종료)", flush=True)

    # Windows에서는 http.server 기본값 allow_reuse_address=True(SO_REUSEADDR)가 같은 포트의 이중 bind를
    # 막지 않아, 재기동 때 옛 서버가 남아 있으면 새 서버가 나란히 떠서 응답이 섞인다(09-11 실측: 리스너 셋).
    # 배타 bind로 두 번째 기동이 즉시 죽게 한다. 포트 이어받기가 필요하면 옛 프로세스를 먼저 내린다.
    ThreadingHTTPServer.allow_reuse_address = False
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[대시보드] 종료", flush=True)
        srv.shutdown()


if __name__ == "__main__":
    main()
