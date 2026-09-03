#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
실시간 모의계좌 매매 대시보드 (의존성 0 — stdlib http.server + requests).

엔진(C++)을 재빌드하지 않고, 이미 존재하는 데이터 소스만 모아 브라우저에 실시간 표시한다:
  - 계좌/보유종목/평가손익/손실률 : KisClient.get_kr_balance() (엔진과 토큰 캐시 공유 → 충돌 없음)
  - 국면(regime)                  : Quant/config/regime.json  (매크로 사이드카가 씀)
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
import json
import os
import re
import sys
import time
import threading
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "PYQuant"))    # kis.client / core.logger 해석용

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


def _candidate_log_dirs():
    cands = []
    env = os.environ.get("QUANT_LOG_DIR")
    if env:
        cands.append(Path(env))
    cands += [
        REPO / "Quant" / "build_win" / "logs",   # 기본 exe 위치 옆 logs
        REPO / "logs",
        Path.cwd() / "logs",
    ]
    seen, out = set(), []
    for c in cands:
        try:
            rc = c.resolve()
        except OSError:
            rc = c
        if rc not in seen:
            seen.add(rc)
            out.append(c)
    return out


def logs_dir() -> Path:
    if LOGS_OVERRIDE:
        return Path(LOGS_OVERRIDE)
    best, best_mt = None, -1.0
    for c in _candidate_log_dirs():
        try:
            mt = (c / "quant_trader.log").stat().st_mtime
        except OSError:
            continue
        if mt > best_mt:
            best, best_mt = c, mt
    return best or (REPO / "logs")


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
    return {"events": events, "latest": latest}


def read_trades_today(now=None):
    now = now or datetime.now(KST)
    d = logs_dir()
    path = d / f"trades_{now.strftime('%Y%m%d')}.csv"
    if not path.exists():
        return {"date": now.strftime("%Y%m%d"), "rows": [],
                "note": f"당일 원장 없음 ({d})"}
    try:
        with open(path, encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        return {"date": now.strftime("%Y%m%d"), "rows": rows[-40:][::-1], "total": len(rows)}
    except OSError as e:
        return {"date": now.strftime("%Y%m%d"), "rows": [], "note": str(e)}


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
        },
        "positions": [
            {"ticker": b.ticker, "name": b.name, "qty": b.quantity,
             "avg": b.avg_price, "cur": b.current_price, "eval": b.eval_amount,
             "pnl": b.pnl, "pnl_rate": b.pnl_rate}
            for b in items
        ],
    }


def _warm_loop(kis: KisClient, quote: KisClient, interval=5.0):
    while True:
        try:
            _live_set("balance", _fetch_balance(kis))
        except Exception as e:
            _live_err("balance", str(e))
        try:
            _live_set("ranking", {"rows": quote.get_volume_ranking(top_n=25)})
        except Exception as e:
            _live_err("ranking", str(e))
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


def build_chart(quote: KisClient, ticker: str, tf: str):
    tf = (tf or "D").upper()
    ttl = 60.0 if tf in ("D", "W") else 20.0
    def _fetch():
        if tf == "D":
            bars = quote.get_chart_ohlcv(ticker, "D", 120)
            return {"tf": "D", "label": "일봉", "bars": bars, "x": "date"}
        if tf == "W":
            bars = quote.get_chart_ohlcv(ticker, "W", 60)
            return {"tf": "W", "label": "주봉", "bars": bars, "x": "date"}
        n = 5 if tf == "5" else 3
        raw = quote.get_minute_ohlcv(ticker, 300)
        bars = _resample(raw, n)
        return {"tf": tf, "label": f"{n}분봉", "bars": bars, "x": "time"}
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
def build_state(kis, quote, cfg, regime_path, uni_path):
    return {
        "server_ts": datetime.now(KST).strftime("%Y-%m-%d %H:%M:%S"),
        "account_no": _mask_acct(cfg.get("kis", {}).get("account_no", "")),
        "is_paper": cfg.get("kis", {}).get("is_paper", False),
        "mode": cfg.get("mode"),
        "market": market_status(),
        "engine": engine_status(),
        "account": _live_get("balance"),
        "regime": read_regime(regime_path),
        "universe": read_universe(uni_path),
        "criteria": build_criteria(cfg),
        "log": read_log_events(),
        "trades": read_trades_today(),
        "ranking": _live_get("ranking"),
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
    regime_path = None
    uni_path = None

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
                state = build_state(self.kis, self.quote, self.cfg, self.regime_path, self.uni_path)
                body = json.dumps(state, ensure_ascii=False).encode("utf-8")
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
.regime-comp{display:grid;grid-template-columns:1fr auto auto;gap:2px 10px;font-size:12px;margin-top:8px}
.regime-comp .rn{color:var(--mut)}
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

  <div class="card col8"><h2>보유 종목 (평가손익)</h2><div class="scroll"><table id="pos">
    <thead><tr><th class="l">종목</th><th class="l">코드</th><th>수량</th><th>평단</th><th>현재가</th><th>평가금</th><th>손익</th><th>손익률</th></tr></thead>
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
const eb=v=>String(v==null?'':v).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function setDot(el,ok,txt){el.innerHTML='<span class="dot" style="background:'+(ok?'var(--up)':'var(--dn)')+'"></span>'+txt;}

async function tick(){
  let s;
  try{ s=await (await fetch('/api/state',{cache:'no-store'})).json(); }
  catch(e){ document.getElementById('conn').textContent='연결끊김'; return; }
  const conn=document.getElementById('conn'); conn.textContent='LIVE'; conn.style.color='var(--up)';
  if(s.__error__){ conn.textContent='서버오류'; return; }

  document.getElementById('acct').textContent=s.account_no||'–';
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
    document.getElementById('kpis').innerHTML=[
      ['총평가금액',won(sm.total_eval)+' 원',''],
      ['예수금(현금)',won(sm.cash)+' 원',''],
      ['평가손익',won(sm.total_pnl)+' 원',cls(sm.total_pnl)],
      ['총수익률',pct(sm.total_pnl_rate),cls(sm.total_pnl_rate)],
      ['보유 종목수',(a.positions?a.positions.length:0)+' 종목',''],
    ].map(k=>`<div class="kpi"><div class="l">${k[0]}</div><div class="v ${k[2]}">${k[1]}</div></div>`).join('');
  }

  // 보유종목
  const pb=document.querySelector('#pos tbody');
  const pos=(a.positions)||[];
  pb.innerHTML= pos.length? pos.map(p=>`<tr class="clk" data-tk="${eb(p.ticker)}" data-nm="${eb(p.name)}">
    <td class="l">${eb(p.name)}</td><td class="l mut">${eb(p.ticker)}</td>
    <td>${won(p.qty)}</td><td>${won(p.avg)}</td><td>${won(p.cur)}</td><td>${won(p.eval)}</td>
    <td class="${cls(p.pnl)}">${won(p.pnl)}</td><td class="${cls(p.pnl_rate)}">${pct(p.pnl_rate)}</td></tr>`).join('')
    : '<tr><td class="l mut" colspan="8">보유 종목 없음</td></tr>';

  // 국면
  const r=s.regime||{}; const rd=document.getElementById('regime');
  if(r.__error__){ rd.innerHTML='<small class="err">'+eb(r.__error__)+'</small>'; }
  else{
    const halt=r.entry_halt, liq=r.force_liquidate, stale=r._stale;
    const rc=r.regime==='RISK_ON'?'up':(r.regime==='RISK_OFF'?'dn':'mut');
    let comp='';
    for(const k in (r.components||{})){const c=r.components[k];
      comp+=`<div class="rn">${eb(c.label||k)}</div><div class="${cls(c.pct)}">${pct(c.pct)}</div><div class="mut">vote ${eb(c.vote)}</div>`;}
    rd.innerHTML=`
      <div><span class="pill ${rc}" style="background:var(--chip)">${eb(r.regime||'?')}</span>
        <span class="mut"> score ${eb(r.risk_score)}</span>
        ${stale?'<span class="pill bad" style="background:var(--chip)"> STALE '+eb(r._age_sec)+'s</span>':''}</div>
      <div style="margin-top:8px">
        신규매수: <b class="${halt?'bad':'up'}">${halt?'차단(entry_halt)':'허용'}</b><br>
        강제청산: <b class="${liq?'bad':''}">${liq?'ON(force_liquidate)':'off'}</b>
      </div>
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
    <div><span class="k">리스크 한도</span> 동시보유 ${eb(rk.max_concurrent_positions)} · 종목당 명목 ${won(rk.max_notional_per_ticker)}원 · 일손실 한도 ${won(rk.daily_loss_limit)}원</div>
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

  // 이벤트 피드
  const fe=document.getElementById('feed'); const evs=(s.log&&s.log.events)||[];
  fe.innerHTML= evs.length? evs.map(e=>`<div class="ev"><span class="t">${eb(e.ts)}</span><span class="tag t-${e.cat}">${eb(e.cat)}</span><span>${eb(e.msg)}</span></div>`).join('')
    : '<div class="mut">최근 이벤트 없음 (엔진 미가동이거나 조용)</div>';

  // 거래대금 상위
  const rr=s.ranking||{}; const rb=document.querySelector('#rank tbody');
  if(rr.__error__){ rb.innerHTML='<tr><td class="l err" colspan="5">랭킹 조회 실패: '+eb(rr.__error__)+'</td></tr>'; }
  else{ const rows=(rr.rows)||[];
    rb.innerHTML= rows.length? rows.map(x=>`<tr class="clk" data-tk="${eb(x.ticker)}" data-nm="${eb(x.name)}"><td>${eb(x.rank)}</td><td class="l">${eb(x.name)}</td><td>${won(x.price)}</td><td class="${cls(x.change_rate)}">${pct(x.change_rate)}</td><td>${won(x.trade_value/1e8)}억</td></tr>`).join('')
      : '<tr><td class="l mut" colspan="5">데이터 없음</td></tr>';
  }

  // 원장
  const t=s.trades||{}; document.getElementById('trdate').textContent=(t.date||'')+(t.total?(' · 총 '+t.total+'행'):'');
  const tb=document.querySelector('#trades tbody');
  tb.innerHTML=(t.rows&&t.rows.length)? t.rows.map(x=>{
    const side=(x.side||''); const sc=side==='BUY'?'up':(side==='SELL'?'dn':'');
    const tkc=/^\d{6}$/.test(x.ticker||'')?'clk':'';
    return `<tr class="${tkc}" data-tk="${eb(x.ticker)}" data-nm="${eb(x.ticker)}"><td>${eb((x.ts_kst||'').slice(11,19))}</td><td class="l">${eb(x.event)}</td><td class="l mut">${eb(x.strategy)}</td>
      <td class="l">${eb(x.ticker)}</td><td class="l ${sc}">${eb(side)}</td><td>${eb(x.order_qty)}</td><td>${eb(x.fill_qty)}</td>
      <td>${won(x.fill_price)}</td><td class="l">${eb(x.status)}</td><td class="l mut">${eb(x.reason||x.entry_reason||'')}</td></tr>`;
  }).join('') : `<tr><td class="l mut" colspan="10">${eb(t.note||'당일 체결 없음')}</td></tr>`;
}
// ── 차트 모달 ──────────────────────────────────────────────────────────────
let curTk='', curNm='', curTf='D';
const bg=document.getElementById('modalbg');
document.addEventListener('click', e=>{
  const tr=e.target.closest('tr.clk');
  if(tr && tr.dataset.tk){ openChart(tr.dataset.tk, tr.dataset.nm||tr.dataset.tk); }
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
  drawCandles(cv,ctx,bars,d);
  const last=bars[bars.length-1];
  info.textContent=`${d.label} · ${bars.length}봉 · 종가 ${Math.round(last.close).toLocaleString('ko-KR')} · ${bars[0][d.x]||''}~${last[d.x]||''}`;
}
function drawCandles(cv,ctx,bars,d){
  const DPR=window.devicePixelRatio||1;
  const W=cv.clientWidth, H=380;
  cv.width=W*DPR; cv.height=H*DPR; cv.style.height=H+'px'; ctx.setTransform(DPR,0,0,DPR,0,0);
  const css=getComputedStyle(document.documentElement);
  const up=css.getPropertyValue('--up').trim(), dn=css.getPropertyValue('--dn').trim();
  const mut=css.getPropertyValue('--mut').trim(), bd=css.getPropertyValue('--bd').trim();
  const padL=8, padR=64, padT=10, volH=64, gap=8, priceH=H-volH-gap-padT-16;
  let hi=-1e18, lo=1e18, vmax=0;
  bars.forEach(b=>{ hi=Math.max(hi,b.high); lo=Math.min(lo,b.low); vmax=Math.max(vmax,b.volume); });
  const pad=(hi-lo)*0.06||1; hi+=pad; lo-=pad;
  const cw=(W-padL-padR)/bars.length;
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
  bars.forEach((b,i)=>{
    const x=padL+cw*i+cw/2; const col=b.close>=b.open?up:dn;
    ctx.strokeStyle=col; ctx.fillStyle=col; ctx.lineWidth=1;
    ctx.beginPath(); ctx.moveTo(x,py(b.high)); ctx.lineTo(x,py(b.low)); ctx.stroke();
    const y1=py(b.open), y2=py(b.close); const top=Math.min(y1,y2), hgt=Math.max(1,Math.abs(y2-y1));
    ctx.fillRect(x-bw/2, top, bw, hgt);
    const vh=vmax?(b.volume/vmax)*volH:0;
    ctx.globalAlpha=.55; ctx.fillRect(x-bw/2, volTop+volH-vh, bw, vh); ctx.globalAlpha=1;
  });
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
    Handler.regime_path, Handler.uni_path = regime_path, uni_path

    # 잔고·랭킹은 백그라운드로 수집 → HTTP 요청 스레드가 KIS 지연에 물리지 않음
    threading.Thread(target=_warm_loop, args=(kis, quote), daemon=True).start()

    print(f"[대시보드] config={cfg_path.name} 계좌={_mask_acct(k['account_no'])} "
          f"모의={k.get('is_paper')}", flush=True)
    print(f"[대시보드] regime={regime_path}  universe={uni_path}", flush=True)
    print(f"[대시보드] logs={logs_dir()}  (원장·엔진로그 추적 위치)", flush=True)
    print(f"[대시보드] http://{args.host}:{args.port}  (3초 폴링, Ctrl+C 종료)", flush=True)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[대시보드] 종료", flush=True)
        srv.shutdown()


if __name__ == "__main__":
    main()
