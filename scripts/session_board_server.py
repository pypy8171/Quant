#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""세션 현황판만 내주는 작은 HTTP 서버 — Quant 세션이 하나라도 살아 있으면 http://127.0.0.1:8788 에서 본다.

매매 대시보드(`scripts/dashboard_server.py`, :8787)의 `/sessions`는 트레이더가 돌 때만 뜬다. 현황판은 코드 세션끼리
인계 시점을 보는 표라 트레이더와 무관하게 늘 열려야 해서(09-14) 이 서버를 따로 둔다. SessionStart 훅
`.claude/hooks/session-board-server.ps1`이 세션마다 띄우고, 포트가 이미 쓰이면(먼저 뜬 서버) 조용히 끝난다.
이 저장소를 cwd로 쓰는 세션이 하나도 없는 채로 두 번(2분) 확인되면 스스로 내려간다.

  py scripts/session_board_server.py            # 포트 8788
  py scripts/session_board_server.py --port N

경로: `/`·`/sessions` → `_private/session_board.html`(30초보다 낡았으면 `session_board.py --quiet`로 다시 만듦),
`/sessions.json` → `_private/session_board.json`.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import session_board  # noqa: E402  live_sessions()·OUT_HTML·OUT_JSON을 같이 쓴다

STALE_SEC = 30        # 파일이 이보다 낡으면 요청 때 다시 만든다(dashboard_server.py /sessions 와 같은 값)
IDLE_CHECK_SEC = 60   # 살아 있는 세션 확인 주기
IDLE_STRIKES = 2      # 연속으로 이만큼 비어 있으면 내려간다


def refresh_if_stale() -> None:
    f = session_board.OUT_HTML
    try:
        if not f.exists() or time.time() - f.stat().st_mtime > STALE_SEC:
            subprocess.run([sys.executable, str(ROOT / "scripts" / "session_board.py"), "--quiet"],
                           timeout=10, capture_output=True)
    except Exception:
        pass  # 못 만들면 있는 파일을 그대로 내보낸다


class Server(ThreadingHTTPServer):
    # 기본값(True)이면 Windows에서 SO_REUSEADDR 때문에 같은 포트에 두 번 묶여 두 서버가 겹친다. 두 번째는 bind에서 실패해야 한다.
    allow_reuse_address = False


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass  # 숨긴 창에서 도는 서버라 접속 로그는 남기지 않는다

    def _send(self, code: int, ctype: str, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/sessions", "/sessions/", "/index.html"):
            refresh_if_stale()
            f = session_board.OUT_HTML
            if f.exists():
                self._send(200, "text/html; charset=utf-8", f.read_bytes())
            else:
                self._send(404, "text/plain; charset=utf-8",
                           "세션 현황판 없음 — py scripts\\session_board.py 를 한 번 돌릴 것".encode("utf-8"))
        elif path == "/sessions.json":
            refresh_if_stale()
            f = session_board.OUT_JSON
            if f.exists():
                self._send(200, "application/json; charset=utf-8", f.read_bytes())
            else:
                self._send(404, "text/plain; charset=utf-8", b"not found")
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found")


def idle_watch(srv: Server) -> None:
    """이 저장소의 세션이 IDLE_STRIKES번 연속 비어 있으면 서버를 내린다 — 프로젝트를 닫으면 같이 사라지게."""
    strikes = 0
    while True:
        time.sleep(IDLE_CHECK_SEC)
        try:
            strikes = strikes + 1 if not session_board.live_sessions() else 0
        except Exception:
            strikes = 0

        if strikes >= IDLE_STRIKES:
            srv.shutdown()
            return


def main() -> int:
    ap = argparse.ArgumentParser(description="세션 현황판 서버")
    ap.add_argument("--port", type=int, default=8788)
    a = ap.parse_args()
    try:
        srv = Server(("127.0.0.1", a.port), Handler)
    except OSError:
        return 0  # 이미 다른 세션이 띄운 서버가 그 포트를 쓴다

    threading.Thread(target=idle_watch, args=(srv,), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
