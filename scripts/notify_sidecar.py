#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""매매 알림 사이드카 — 체결은 즉시, 포지션 요약은 주기적으로 메신저에 보낸다.

엔진(quant_trader)은 건드리지 않는다. 당일 체결 원장 CSV(logs/trades_YYYYMMDD.csv)를
증분으로 따라 읽어 체결이 새로 적히면 바로 보내고, 평단·손익은 KIS 잔고조회로 따로 만든다.
알림 쪽이 죽어도 매매는 그대로 돈다.

실행:
    py scripts/notify_sidecar.py --config Quant/config/config_dev_paper.json

수신처 설정 — _private/notify.json (gitignore) 또는 환경변수:
    {
      "discord_webhook": "https://discord.com/api/webhooks/...",          // 한 채널에 다 보낼 때
      "discord_webhook_fill": "...",       // 체결만 받을 채널 (없으면 discord_webhook)
      "discord_webhook_position": "...",   // 포지션 요약만 받을 채널 (없으면 discord_webhook)
      "kakao": {"rest_api_key": "...", "refresh_token": "..."}
    }
    환경변수: DISCORD_WEBHOOK_URL / KAKAO_REST_API_KEY + KAKAO_REFRESH_TOKEN
둘 다 설정하면 둘 다 보낸다.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path

import requests

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "PYQuant"))

# 콘솔이 cp949면 이모지·em dash에서 죽는다. 출력만 UTF-8로 고정한다.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

from kis.client import KisClient  # noqa: E402


# ── 로그 디렉터리 (dashboard_server와 같은 규약: 최신 quant_trader.log를 가진 곳) ──────
def _candidate_log_dirs():
    cands = []
    env = os.environ.get("QUANT_LOG_DIR")
    if env:
        cands.append(Path(env))
    cands += [REPO / "Quant" / "build_win" / "logs", REPO / "logs", Path.cwd() / "logs"]
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


def logs_dir(override):
    if override:
        return Path(override)
    best, best_mt = None, -1.0
    for c in _candidate_log_dirs():
        try:
            mt = (c / "quant_trader.log").stat().st_mtime
        except OSError:
            continue
        if mt > best_mt:
            best, best_mt = c, mt
    return best or (REPO / "Quant" / "build_win" / "logs")


# ── 수신처 어댑터 ──────────────────────────────────────────────────────────────
class Notifier:
    name = "?"
    LIMIT = 1900
    kinds = None   # None이면 종류를 가리지 않는다. 채널을 나누면 {"fill"}·{"position"}이 온다.

    def accepts(self, kind):
        return self.kinds is None or kind in self.kinds

    def send(self, text):
        raise NotImplementedError

    def clip(self, text):
        if len(text) > self.LIMIT:
            return text[: self.LIMIT - 20] + "\n… (생략)"
        return text


class DiscordNotifier(Notifier):
    """Webhook URL 하나면 끝. 만료도 토큰 갱신도 없다."""
    name = "discord"

    def __init__(self, url, kinds=None, label=""):
        self.url = url
        self.kinds = kinds
        if label:
            self.name = "discord(%s)" % label

    def send(self, text):
        payload = {"content": self.clip(text)}
        r = requests.post(self.url, json=payload, timeout=10)
        if r.status_code == 429:  # 레이트리밋이면 한 번만 기다렸다 재시도
            try:
                wait = float(r.json().get("retry_after", 1.0))
            except Exception:
                wait = 1.0
            time.sleep(min(wait, 10.0))
            r = requests.post(self.url, json=payload, timeout=10)
        r.raise_for_status()


class KakaoNotifier(Notifier):
    """카카오톡 '나에게 보내기'. access_token이 6시간이라 refresh_token으로 갱신한다."""
    name = "kakao"

    def __init__(self, rest_api_key, refresh_token):
        self.key = rest_api_key
        self.refresh = refresh_token
        self.token = None
        self.token_exp = 0.0

    def _ensure_token(self):
        if self.token and time.time() < self.token_exp - 300:
            return
        r = requests.post(
            "https://kauth.kakao.com/oauth/token",
            data={"grant_type": "refresh_token", "client_id": self.key,
                  "refresh_token": self.refresh},
            timeout=10)
        r.raise_for_status()
        d = r.json()
        self.token = d["access_token"]
        self.token_exp = time.time() + int(d.get("expires_in", 21600))
        if d.get("refresh_token"):  # 갱신 토큰이 재발급되면 갈아끼운다
            self.refresh = d["refresh_token"]

    def send(self, text):
        self._ensure_token()
        tmpl = {"object_type": "text", "text": self.clip(text),
                "link": {"web_url": "https://developers.kakao.com",
                         "mobile_web_url": "https://developers.kakao.com"}}
        r = requests.post(
            "https://kapi.kakao.com/v2/api/talk/memo/default/send",
            headers={"Authorization": "Bearer " + self.token},
            data={"template_object": json.dumps(tmpl, ensure_ascii=False)},
            timeout=10)
        r.raise_for_status()


def build_notifiers(quiet=False):
    conf = {}
    p = REPO / "_private" / "notify.json"
    if p.exists():
        try:
            conf = json.loads(p.read_text(encoding="utf-8"))
        except Exception as e:
            print("[warn] %s 파싱 실패: %s" % (p, e), flush=True)

    out = []
    # 체결과 포지션 요약을 다른 채널에서 보고 싶으면 웹훅을 둘로 준다. 하나만 주면 한 채널에 섞인다.
    base = os.environ.get("DISCORD_WEBHOOK_URL") or conf.get("discord_webhook")
    fill = os.environ.get("DISCORD_WEBHOOK_FILL") or conf.get("discord_webhook_fill") or base
    pos = os.environ.get("DISCORD_WEBHOOK_POSITION") or conf.get("discord_webhook_position") or base
    if fill and fill == pos:
        out.append(DiscordNotifier(fill))
    else:
        # 기동·테스트 같은 system 알림은 양쪽 다 받는다. 어느 채널이 죽었는지 바로 보인다.
        if fill:
            out.append(DiscordNotifier(fill, kinds={"fill", "system"}, label="체결"))
        if pos:
            out.append(DiscordNotifier(pos, kinds={"position", "system"}, label="포지션"))
    kk = conf.get("kakao") or {}
    key = os.environ.get("KAKAO_REST_API_KEY") or kk.get("rest_api_key")
    rt = os.environ.get("KAKAO_REFRESH_TOKEN") or kk.get("refresh_token")
    if key and rt:
        out.append(KakaoNotifier(key, rt))
    if not out and not quiet:
        print("[warn] 수신처가 없다 — _private/notify.json 또는 환경변수를 설정할 것. "
              "지금은 콘솔에만 출력한다.", flush=True)
    return out


class Fanout:
    """수신처 하나가 죽어도 나머지는 보낸다. 알림 실패로 사이드카가 멈추지 않게."""

    def __init__(self, targets, echo):
        self.targets = targets
        self.echo = echo
        self.fail = {}

    def send(self, text, kind="system"):
        if self.echo or not self.targets:
            print(text, flush=True)
        for t in self.targets:
            if not t.accepts(kind):
                continue
            try:
                t.send(text)
                self.fail.pop(t.name, None)
            except Exception as e:
                n = self.fail.get(t.name, 0) + 1
                self.fail[t.name] = n
                if n <= 3 or n % 20 == 0:  # 계속 실패해도 콘솔을 도배하지 않는다
                    print("[warn] %s 전송 실패(%d회): %s" % (t.name, n, e), flush=True)


# ── 포맷 ──────────────────────────────────────────────────────────────────────
def _won(v):
    try:
        return format(int(round(float(v))), ",")
    except (TypeError, ValueError):
        return str(v)


def _signed(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return str(v)
    return ("+" if f >= 0 else "") + _won(f)


def _dispw(s):
    return sum(2 if ord(c) > 0x2E80 else 1 for c in s)


def _wide_pad(s, width):
    """한글은 고정폭에서 두 칸을 먹는다. 폭 기준으로 자르고 채워 열을 맞춘다."""
    out, used = [], 0
    for c in s:
        w = 2 if ord(c) > 0x2E80 else 1
        if used + w > width - 1:
            break
        out.append(c)
        used += w
    return "".join(out) + " " * (width - used)


def _wide_rjust(s, width):
    return " " * max(0, width - _dispw(s)) + s


def fmt_fill(row, names):
    side = (row.get("side") or "").upper()
    tkr = row.get("ticker") or ""
    nm = names.get(tkr, "")
    head = "🟢 매수 체결" if side == "BUY" else "🔴 매도 체결"
    qty = row.get("fill_qty") or row.get("order_qty") or "0"
    px = row.get("fill_price") or row.get("order_price") or "0"
    try:
        amt = _won(float(qty) * float(px))
    except (TypeError, ValueError):
        amt = "?"
    lines = ["%s  %s(%s)" % (head, nm, tkr) if nm else "%s  %s" % (head, tkr),
             "　%s주 @%s원  = %s원" % (_won(qty), _won(px), amt)]
    pnl = row.get("realized_pnl")
    if side == "SELL" and pnl not in (None, "", "0"):
        lines.append("　실현손익 %s원" % _signed(pnl))
    strat = row.get("strategy") or ""
    if strat:
        lines.append("　전략 %s" % strat)
    rsn = (row.get("entry_reason") or row.get("reason") or "").strip()
    if rsn:
        lines.append("　%s" % rsn[:80])
    lines.append("　%s" % (row.get("ts_kst") or "")[-8:])
    return "\n".join(lines)


def read_realized_today(items):
    """오늘 익절·손절·실현손익. 대시보드와 같은 원장 집계를 그대로 쓴다(기준이 갈리면 안 된다)."""
    try:
        from dashboard_server import read_trades_today
    except Exception:
        return None
    seed = {b.ticker: b.avg_price for b in items if b.ticker and b.avg_price}
    try:
        return (read_trades_today(seed_avg=seed) or {}).get("realized")
    except Exception:
        return None


def read_regime(cfg):
    """매크로 사이드카가 쓰는 regime.json. 신규매수 차단·강제청산 상태를 같이 보여준다."""
    rel = cfg.get("regime_file") or "Quant/config/regime.json"
    try:
        return json.loads((REPO / rel).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def fmt_positions(items, summ, daily_pnl, mode, cfg=None, realized=None, regime=None):
    cfg = cfg or {}
    risk = cfg.get("risk") or {}
    pos = sorted(items, key=lambda b: -abs(b.eval_amount or 0))
    now = datetime.now().strftime("%H:%M")

    head = ["총평가금액   %14s 원" % _won(summ.total_eval),
            "총매수금액   %14s 원  (원가)" % _won(summ.buy_amount),
            "가용현금D+2  %14s 원" % _won(summ.cash_avail),
            "예수금       %14s 원  (총·결제전)" % _won(summ.cash)]
    cap = risk.get("max_gross_exposure_pct")
    if summ.total_eval:
        gross = 100.0 * (summ.buy_amount or 0) / summ.total_eval
        head.append("총노출/한도  %13.1f%%  / %s" % (
            gross, ("%.0f%%" % (cap * 100)) if cap else "미설정"))
    head.append("평가손익     %14s 원  (%+.2f%%)"
                % (_signed(summ.total_pnl), summ.total_pnl_rate or 0.0))
    maxn = risk.get("max_concurrent_positions")
    head.append("보유 종목수  %14s 종목" % ("%d / %d" % (len(pos), maxn) if maxn else str(len(pos))))
    if realized:
        head.append("오늘 익절    %14s 원  (%d건)" % (_won(realized.get("profit", 0)),
                                                  realized.get("win", 0)))
        head.append("오늘 손절    %14s 원  (%d건)" % (_won(realized.get("loss", 0)),
                                                  realized.get("lose", 0)))
        unk = realized.get("unknown", 0)
        head.append("오늘 실현손익%14s 원%s" % (_signed(realized.get("net", 0)),
                                            "  *%d건 평단미상" % unk if unk else ""))
    if daily_pnl is not None:
        head.append("당일손익(엔진)%13s 원" % _signed(daily_pnl))

    # 열 폭은 한 곳에서 정한다. 머리글도 같은 폭으로 만들어야 한글 종목명에서 어긋나지 않는다.
    cols = [("종목", 15), ("수량", 7), ("평단", 10), ("현재", 10), ("손익", 12), ("수익률", 8)]
    ruler = sum(w for _, w in cols)
    body = ["", _wide_pad("종목", 15) + "".join(_wide_rjust(h, w) for h, w in cols[1:]),
            "─" * ruler]
    for b in pos:
        body.append("%s%7s%10s%10s%12s%7.2f%%" % (
            _wide_pad(b.name or b.ticker, 15), _won(b.quantity), _won(b.avg_price),
            _won(b.current_price), _signed(b.pnl), b.pnl_rate or 0.0))
    if not pos:
        body.append("(보유 없음)")

    tail = []
    if regime:
        th = regime.get("thresholds") or {}
        tail = ["", "국면 %s (score %s) · 신규매수 %s · 강제청산 %s" % (
            regime.get("regime", "UNKNOWN"), regime.get("risk_score", "?"),
            "차단" if regime.get("entry_halt") else "허용",
            "on" if regime.get("force_liquidate") else "off")]
        if th:
            tail.append("기준 halt≤%s · liq≤%s · %s"
                        % (th.get("halt_score", "?"), th.get("liq_score", "?"),
                           (regime.get("ts") or "")[:19]))

    return "📊 %s 포지션 [%s]\n```\n%s\n```" % (
        now, mode, "\n".join(head + body + tail))


def load_universe_names(cfg):
    """유니버스 스캔 파일의 ticker→종목명. 첫 체결부터 이름이 나오게 시드로 쓴다."""
    strat = cfg.get("strategies") or [{}]
    rel = next((s.get("universe_file") for s in strat if s.get("universe_file")),
               "Quant/config/universe_scan.json")
    p = REPO / rel
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {u["ticker"]: u["name"] for u in (d.get("universe") or [])
            if u.get("ticker") and u.get("name")}


DAILY_RE = re.compile(r"리컨사일: 당일손익 (-?\d+)원")


def read_daily_pnl(log_path):
    """엔진이 자기 기준선으로 계산한 당일손익. 마지막 리컨사일 한 줄만 본다."""
    try:
        with open(log_path, "rb") as f:
            f.seek(0, io.SEEK_END)
            f.seek(max(0, f.tell() - 400_000))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return None
    m = DAILY_RE.findall(tail)
    return int(m[-1]) if m else None


# ── 원장 tail ─────────────────────────────────────────────────────────────────
class TradeTail:
    """당일 trades CSV를 증분으로 따라 읽는다. 날짜가 바뀌면 새 파일로 넘어간다."""

    def __init__(self, ldir, replay):
        self.ldir = ldir
        self.replay = replay
        self.path = None
        self.pos = 0
        self.header = None

    def _today_path(self):
        return self.ldir / ("trades_%s.csv" % datetime.now().strftime("%Y%m%d"))

    def poll(self):
        p = self._today_path()
        if p != self.path:
            self.path = p
            self.header = None
            # 기동 직후 과거 체결이 한꺼번에 쏟아지는 걸 막는다(--replay-today로 해제)
            try:
                end = p.stat().st_size
            except OSError:
                end = 0
            self.pos = 0 if self.replay else end
            self.replay = False
        if not p.exists():
            return []
        try:
            size = p.stat().st_size
        except OSError:
            return []
        if size < self.pos:  # 파일이 갈아엎힌 경우
            self.pos = 0
        if size == self.pos:
            return []
        with open(p, "r", encoding="utf-8", errors="replace", newline="") as f:
            if self.header is None:
                self.header = next(csv.reader(f), None)
            f.seek(self.pos)
            chunk = f.read()
            newpos = f.tell()
        if not chunk.endswith("\n"):  # 쓰다 만 마지막 줄은 다음 폴링으로 미룬다
            cut = chunk.rfind("\n")
            if cut < 0:
                return []
            newpos -= len(chunk) - cut - 1
            chunk = chunk[: cut + 1]
        self.pos = newpos
        out = []
        for rec in csv.reader(io.StringIO(chunk)):
            if not rec or rec[0] == "ts_kst":
                continue
            out.append(dict(zip(self.header or [], rec)))
        return out


def main():
    ap = argparse.ArgumentParser(description="매매 알림 사이드카")
    ap.add_argument("--config", default="Quant/config/config_dev_paper.json")
    ap.add_argument("--interval", type=float, default=1800, help="포지션 요약 주기(초)")
    ap.add_argument("--poll", type=float, default=2.0, help="체결 원장 폴링 주기(초)")
    ap.add_argument("--logs", default=None, help="로그 디렉터리 강제 지정")
    ap.add_argument("--events", default="FILL", help="알릴 event 목록(쉼표). 예: FILL,REJECTED")
    ap.add_argument("--replay-today", action="store_true", help="오늘 체결을 처음부터 다시 보냄")
    ap.add_argument("--echo", action="store_true", help="콘솔에도 출력")
    ap.add_argument("--test", action="store_true", help="테스트 메시지 한 번 보내고 종료")
    args = ap.parse_args()

    cfg_path = REPO / args.config if not os.path.isabs(args.config) else Path(args.config)
    if not cfg_path.exists():
        cfg_path = Path(args.config)
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    k = cfg["kis"]
    mode = "모의" if k.get("is_paper", False) else "실계좌"

    fan = Fanout(build_notifiers(quiet=False), args.echo)
    if args.test:
        fan.send("🔔 알림 사이드카 테스트 [%s] %s"
                 % (mode, datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
        print("수신처 %d곳으로 전송 시도 완료" % len(fan.targets), flush=True)
        return 0

    kis = KisClient(app_key=k["app_key"], app_secret=k["app_secret"],
                    account_no=k["account_no"], account_type=k.get("account_type", "01"),
                    is_paper=k.get("is_paper", False))

    ldir = logs_dir(args.logs)
    log_path = ldir / "quant_trader.log"
    tail = TradeTail(ldir, args.replay_today)
    want = set(e.strip().upper() for e in args.events.split(",") if e.strip())
    names = load_universe_names(cfg)
    print("[notify] logs=%s events=%s 요약주기=%.0fs 수신처=%s"
          % (ldir, sorted(want), args.interval,
             [t.name for t in fan.targets] or "콘솔"), flush=True)

    fan.send("🔔 알림 시작 [%s] %s — 체결 즉시 / 요약 %.0f분"
             % (mode, datetime.now().strftime("%H:%M:%S"), args.interval / 60))

    last_summary = 0.0
    first = True
    while True:
        try:
            # 1) 체결 즉시
            pending = [r for r in tail.poll()
                       if (r.get("event") or "").upper() in want]
            if pending:
                # 같은 폴링에 여러 건이면 한 메시지로 묶는다(웹훅 레이트리밋 회피)
                fan.send("\n\n".join(fmt_fill(r, names) for r in pending[:10]), "fill")
                if len(pending) > 10:
                    fan.send("…외 체결 %d건" % (len(pending) - 10), "fill")

            # 2) 주기 포지션 요약
            if first or time.time() - last_summary >= args.interval:
                try:
                    items, summ = kis.get_kr_balance()
                    for b in items:
                        if b.name:
                            names[b.ticker] = b.name
                    fan.send(fmt_positions(items, summ, read_daily_pnl(log_path), mode,
                                           cfg, read_realized_today(items), read_regime(cfg)),
                             "position")
                    last_summary = time.time()
                    first = False
                except Exception as e:
                    # 모의 도메인 잔고조회는 종종 지연된다. 다음 주기에 다시 시도.
                    print("[warn] 잔고 조회 실패: %s" % e, flush=True)
                    last_summary = time.time() - args.interval + 60

            time.sleep(args.poll)
        except KeyboardInterrupt:
            print("[notify] 종료", flush=True)
            return 0
        except Exception:
            traceback.print_exc()
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
