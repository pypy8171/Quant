# -*- coding: utf-8 -*-
"""장중·마감 후 실행 로그에서 '이미 한 번 당한' 실패 유형이 다시 났는지 기계적으로 본다.

사람이 로그를 눈으로 훑어 찾아낸 것만 여기 들어온다. 새 항목을 넣을 때는 그 항목이
실제로 하루를 망친 적이 있어야 한다 — 가정으로 만든 체크는 경보만 늘리고 신뢰를 깎는다.

사용:  py scripts/check_runtime_health.py [--date YYYY-MM-DD] [--log <경로>]
종료코드: 0 = FAIL 없음, 1 = FAIL 있음
"""
from __future__ import annotations

import argparse
import datetime as dt
import gzip
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402
from log_patterns import GUARD_ATTACH_RE as GUARD_RE  # noqa: E402

DEFAULT_LOG = _logdir.log_dir() / "quant_trader.log"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})")
START_RE = re.compile(r"퀀트 엔진 시작")
PLATFORM_RE = re.compile(r"\[Main\] 실행 플랫폼 (Windows|Linux)")
STALE_RE = re.compile(r"이전 세션 미체결 (\d+)건 발견")
BREAKEVEN_RE = re.compile(r"본전탈출\)")
FILL_RE = re.compile(r"체결통보 ODNO=\d+ (\d{6}) (BUY|SELL) (\d+)주")
RATE_RE = re.compile(r"EGW00201|초당 거래건수")
WSFALL_RE = re.compile(r"WS → REST 폴링 폴백")
# 주문 접수·거부 한 줄의 왕복 시간. 버킷대기는 09-19 이후 바이너리만 찍는다(없으면 None).
RTT_RE = re.compile(r"\[OrderRouter\] (?:접수|KIS 거부) .*?RTT=(\d+)ms(?: 버킷대기=(\d+)ms)?")
# D-100 — 잔고 조회가 한 사이클(500ms)을 넘겨 뒤 사이클에서 적용된 건
RECON_SLOW_RE = re.compile(r"잔고 대조: 조회 소요 (\d+)ms \(사이클 (\d+)회 걸침\)")
# 서버가 15초 안에 답을 안 준 요청 — WinHTTP 12002·curl 28. 한 요청에 한 줄(재시도 래퍼의 안내 줄은 세지 않는다)
HTTP_TIMEOUT_RE = re.compile(r"ReceiveResponse 실패: 12002|\[CURL\] 요청 실패: Timeout was reached")
# D-101 결정 2 — TRENDX max_universe 0이면 초기 등록이 0종목이어야 한다(스코어 경로 상한은 09-19 수정)
TRENDX_REGISTER_RE = re.compile(r"TRENDX universe_from_scan: 초기 (\d+)종목 등록")
# D-101 결정 3 — 마감 청산이 매매 창 안(모의 15:15·실계좌 19:50, 접속매매)에 나가면 이 거부는 0건이다(09-18 2,188건이 25종목 이월을 만들었다)
SESSION_WINDOW_REJECT_RE = re.compile(r"\[OrderRouter\] 거부 .*세션 창 밖")
# D-109 — 목표 비중표 바스켓. 격리 3종(청산관리·교체·DEVSCALE)과 재기동 중복 방지가 실제로 지켜졌는지 로그로 본다.
BASKET_LOADED_RE = re.compile(r"\[BASKET_\w+\] 목표 비중표 읽음 as_of=(\d{8})")
BASKET_ORDER_RE = re.compile(r"\[BASKET_\w+\] 주문: (\d{6}) (매수|매도) (\d+)주")
BASKET_RUN_START_RE = re.compile(r"\[BASKET_\w+\] 오늘 집행 시작")
BASKET_RUN_END_RE = re.compile(r"\[BASKET_\w+\] 오늘 집행 끝")
BASKET_WINDOW_CLOSED_RE = re.compile(r"\[BASKET_\w+\] 집행 창 종료")
SIGNAL_RE = re.compile(r"\[Strategy\] 신호: \[([A-Z_+0-9]+)\] (\d{6})(?:\([^)]*\))? (BUY|SELL) (\d+)")
ITB_ATTACH_RE = re.compile(r"\[Main\]   \+ ITB (\d{6}) ")
# 09-20 청산선 재설정(config_dev_paper.json "//exit_09-20") — 12일 원장에서 손실을 낸 네 경로가 닫혔는지 본다:
#  교체 매도(728체결 -89만)는 0건, 승계 직후 매도(부착 90초 안 47건 -43만, 유예 분기 버그)는 0건,
#  손절 뒤 같은 날 같은 종목 재매수(쿨다운 21600초)는 0건, 손절이 매수의 25%를 넘으면 -2.0%가 잡음에 걸리는 것(예측 21%).
DEVSCALE_STOP_RE = re.compile(r"신호: \[DEVSCALE_(\d{6})\] \d{6}.* SELL \d+ \| 근거: 청산:손절\(평단")
MAX_STOP_PER_BUY = 0.25
# D-111 넘김·진입 필터(09-21 리플레이): 장 마감 청산을 끄고(market_close_exit_hhmm 2400) 전일 ATR·개장 이격으로 그날 진입을
#  거른다. 두 행은 새 바이너리 표식(진입 필터 판정 줄)이 있는 날만 판정한다 — 배포 전 로그에서는 건너뛴다.
DEVSCALE_CLOSE_EXIT_RE = re.compile(r"신호: \[DEVSCALE_(\d{6})\] \d{6}.* SELL \d+ \| 근거: 청산:장 마감\(")
DEVSCALE_ENTRY_FILTER_RE = re.compile(r"\[DEVSCALE_(\d{6})\] 진입 필터 (통과|차단)\(")

# 임계값. 넘으면 그날 운영이 실제로 상했던 수준이다.
MAX_STALE_ORDERS = 25     # 유령주문 재부활 — 취소 왕복이 초당한도를 밀어낸다
MIN_SESSION_SEC = 30      # 이보다 짧게 죽으면 배선/바이너리 문제(정상 재기동 아님)
GUARD_QUIET_SEC = 90      # 청산 관리 부착 직후 이 시간 안의 본전탈출은 재기동 투매다
CHURN_SEC = 120           # 같은 종목 매도→매수가 이 안에 오면 회전
MAX_CHURN = 3
MAX_RATE_HITS = 50
SLOW_ORDER_MS = 3000      # 접수까지 이보다 오래 걸리면 청산 지정가가 시세를 놓친다(T-24)
MAX_SLOW_ORDER_RATIO = 0.2  # 접수 중 이 비율 넘게 느리면 그날 서버(또는 버킷)가 상한 것
BUCKET_WAIT_MS = 300      # 버킷대기 중앙값이 이 위면 지연의 주범은 서버가 아니라 초당한도 버킷
MAX_HTTP_TIMEOUTS = 50    # 15초 제한 초과 요청 — 09-18 192건(09-17은 3건)이 잔고 대조를 100초까지 붙잡았다
# 원장 저널(D-113) — 기동 줄 둘과 장중 기록 실패. 저널에 못 적은 주문은 아예 나가지 않는다.
LEDGER_REPLAY_RE = re.compile(r"\[Engine\] 원장 저널 리플레이: (\d+)건 \(마지막 seq (\d+)(, 꼬리 잘림)?\)")
LEDGER_RESOLVE_RE = re.compile(r"\[Engine\] 원장 미결 주문 대조: 되살림 (\d+)건 · 선점해제 (\d+)건 · 저널기록실패 (\d+)건")
LEDGER_WRITE_FAIL_RE = re.compile(r"\[OrderRouter\] 원장 저널 기록 실패")

BASKET_BUY_LEG_DEADLINE = 15 * 3600 + 5 * 60  # 매수 레그는 15:05까지 끝나야 마감 청산(15:15)과 겹치지 않는다(D-109)


def median(values: list[int]) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def secs(m: re.Match) -> int:
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))


def tsdb_password() -> str:
    """저장소 루트 .env의 TSDB_PASSWORD(감시견·리코더와 같은 출처). 없으면 빈 문자열."""
    environment_file = REPO / ".env"

    if not environment_file.exists():
        return ""

    for line in environment_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("TSDB_PASSWORD="):
            return line.split("=", 1)[1].strip()

    return ""


def import_psycopg2():
    """psycopg2는 PYQuant venv에만 깔려 있다 — 감시견이 전역 py로 이 스크립트를 부르면 import가 실패해
    DB를 보는 판정이 통째로 '판정 안 함'으로 넘어간다(2026-09-22 하루 내내 그랬다). 호출 쪽을 고치면
    감시견 두 갈래(ps1·sh)를 다 건드려야 하니, 여기서 venv의 site-packages를 찾아 붙여 본다.
    C 확장이라 파이썬 버전이 다르면 붙여도 import가 실패한다 — 그때는 그대로 None."""
    try:
        import psycopg2
        return psycopg2
    except ImportError:
        pass

    for site_packages in sorted(REPO.glob("PYQuant/.venv*/Lib/site-packages")) + \
            sorted(REPO.glob("PYQuant/.venv*/lib/python*/site-packages")):
        if not site_packages.is_dir():
            continue

        sys.path.append(str(site_packages))

        try:
            import psycopg2
            return psycopg2
        except ImportError:
            sys.path.pop()

    return None


def resource_sampling_rows(date: str) -> list:
    """procwatch가 그날 엔진 자원 표본(proc_stats)을 적재했고 스레드 이름이 실렸는지 — 09-22 리눅스 첫날
    그라파나 CPU·메모리 패널이 비어 있던 것(psutil이 WSL 프로세스를 못 봄)을 다시 겪지 않기 위한 행.
    DB 접속 정보는 저장소 루트 .env의 TSDB_PASSWORD(감시견과 같은 출처)."""
    password = tsdb_password()

    if not password:
        return [("자원 표본 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("자원 표본 공백", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")]

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return [("자원 표본 적재", False, "WARN", "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary"),
                ("자원 표본 공백", False, "WARN", "psycopg2 없음 — 위와 같다")]

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*), COUNT(DISTINCT process_name) FROM proc_stats"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            sample_count, process_kinds = cursor.fetchone()
            cursor.execute(
                "SELECT COUNT(DISTINCT thread_name) FROM proc_thread_stats"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s AND thread_name NOT LIKE 'quant_trader%%'", (date,))
            named_threads = cursor.fetchone()[0]
            # 표본 사이가 얼마나 벌어졌나 — 행수만 보면 중간에 통째로 빈 구간을 못 잡는다
            #  (2026-09-22 09:32~09:44 12분 공백을 하루 판정이 PASS로 넘겼다)
            cursor.execute(
                "SELECT COALESCE(MAX(gap), 0), COALESCE(TO_CHAR(MAX(ts) AT TIME ZONE 'Asia/Seoul', 'HH24:MI:SS'), '-')"
                " FROM (SELECT ts, EXTRACT(EPOCH FROM ts - LAG(ts) OVER (ORDER BY ts)) gap FROM proc_stats"
                "       WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s) sampled", (date,))
            max_gap_seconds, last_sample = cursor.fetchone()

        connection.close()
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return [("자원 표본 적재", True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})"),
                ("자원 표본 공백", True, "WARN", "DB 조회 실패 — 판정 안 함")]

    # 장중 6시간 30분을 5초 주기로 떠도 4,000행이 넘고, 절반만 떠도 2,000행쯤 — 300행이면 몇십 분만 돌다 죽은 것
    # 공백 기준 120초: 주기 5초 + perf 표본 10초 + 재기동 대기를 다 더해도 그 안이다.
    return [("자원 표본 적재", sample_count >= 300, "WARN",
             f"proc_stats {sample_count}행 (기대 300 이상, 5초 주기·장중 내내), 이름 붙은 스레드 {named_threads}종"
             + (" — 0이면 스레드 이름 배포 전 바이너리거나 Windows psutil 경로" if named_threads == 0 else "")),
            ("자원 표본 공백", float(max_gap_seconds) <= 120, "WARN",
             f"가장 긴 공백 {float(max_gap_seconds):.0f}초 (기대 120 이하), 마지막 표본 {last_sample}"
             + (" — 수집기가 죽었다 되살아난 구간이다. logs/procwatch.log를 본다" if float(max_gap_seconds) > 120 else ""))]


def feed_ledger_rows(date: str) -> list:
    """리코더가 그날 적재한 체결 틱과 원장 계좌를 본다 — 09-22 실측한 두 가지를 다시 겪지 않기 위한 행.

    ① 체결 틱이 ticks 표에 안 들어가면 그라파나 "피드 지연"·"초당 틱 유입" 패널이 며칠 전 시각을
       가리킨다(피드는 멀쩡한데 화면만 죽는다).
    ② Engine을 그대로 띄우는 테스트·부하 하네스가 같은 ZMQ 포트(5555)에 bind하면 리코더가 그쪽을
       잡는다. 09-22 장중에 합성 주문 52건·체결 58건이 계좌 없이 운영 표에 들어갔다.
    """
    password = tsdb_password()

    if not password:
        return [("피드 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("개장부터 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("원장 계좌 단일", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")]

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return [("피드 적재", False, "WARN", "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary"),
                ("개장부터 적재", False, "WARN", "psycopg2 없음 — 위와 같다"),
                ("원장 계좌 단일", False, "WARN", "psycopg2 없음 — 위와 같다")]

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*), COUNT(DISTINCT ticker),"
                " TO_CHAR(MIN(ts AT TIME ZONE 'Asia/Seoul'), 'HH24:MI:SS') FROM ticks"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            tick_count, tick_tickers, first_tick_time = cursor.fetchone()
            cursor.execute(
                "SELECT COALESCE(MAX(data_cnt), 0) FROM health"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            data_count = cursor.fetchone()[0]
            accounts = {}

            for table in ("orders", "fills"):
                cursor.execute(
                    f"SELECT COALESCE(NULLIF(TRIM(account), ''), '(빈칸)'), COUNT(*) FROM {table}"
                    " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s GROUP BY 1", (date,))

                for account, count in cursor.fetchall():
                    accounts[account] = accounts.get(account, 0) + count

        connection.close()
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        detail = f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})"
        return [("피드 적재", True, "WARN", detail), ("개장부터 적재", True, "WARN", detail),
                ("원장 계좌 단일", True, "WARN", detail)]

    # 27종목을 장중 내내 받으면 수만 건이다. 1,000건이면 리코더가 잠깐만 붙어 있던 것
    feed_row = ("피드 적재", tick_count >= 1000 and data_count > 0, "WARN",
                f"ticks {tick_count}행·{tick_tickers}종목, HEALTH data 최대 {data_count}"
                + (" — data가 0이면 WS 경로 계수 배포 전 바이너리" if data_count == 0 else ""))
    # 한 계좌 = 한 프로세스다(다계좌는 계좌당 프로세스). 두 종류가 보이면 남의 엔진 데이터가 섞인 것
    ledger_row = ("원장 계좌 단일", len(accounts) <= 1, "FAIL",
                  "주문·체결 계좌 " + (", ".join(f"{name} {count}건" for name, count in sorted(accounts.items())) or "행 없음")
                  + (" — 기대 1종. 다른 엔진이 같은 ZMQ 포트를 물었다(PYQuant/main.py record --account)"
                     if len(accounts) > 1 else ""))

    # 리코더는 감시견이 개장 전에 띄우므로 첫 틱은 09:00 동시호가 체결이어야 한다. 09-22에 감시견이
    #  --record-ticks 없이 띄운 리코더를 09:42에 손으로 다시 띄워 42분치가 비었다 — 그날 안에 다시 안 보이도록
    #  첫 틱 시각을 본다. 틱이 아예 없는 날은 위 "피드 적재" 행이 이미 잡으므로 여기서는 넘어간다.
    first_tick_late = tick_count > 0 and first_tick_time > "09:05:00"
    opening_row = ("개장부터 적재", not first_tick_late, "WARN",
                   f"첫 틱 {first_tick_time or '없음'}"
                   + (" — 09:05 뒤다. 리코더가 개장 뒤에 (다시) 떴거나 --record-ticks 없이 떴다"
                      "(scripts/auto_trade_day.ps1 quant-recorder 줄)" if first_tick_late else ""))

    return [feed_row, opening_row, ledger_row]


def queue_latency_row(date: str) -> tuple:
    """엔진이 HEALTH에 큐 고수위·버린 건수·구간 지연을 실었는지, 그리고 그날 버린 건이 있었는지.
    버린 건수가 0이 아니면 그만큼 틱·주문·체결통보가 파이프라인에서 빠진 것이라 FAIL이다.
    열이 NULL만 있으면 그 수치를 안 싣는 옛 exe가 돌고 있다는 뜻이다(배포 전 상태)."""
    password = tsdb_password()

    if not password:
        return ("큐·지연 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return ("큐·지연 적재", False, "WARN",
                "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary")

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FILTER (WHERE queue_shard_capacity IS NOT NULL),"
                " COALESCE(MAX(dropped_shard), 0) + COALESCE(MAX(dropped_order), 0)"
                " + COALESCE(MAX(dropped_fill), 0),"
                " COALESCE(MAX(100.0 * queue_shard_high_water / NULLIF(queue_shard_capacity, 0)), 0),"
                " COALESCE(MAX(100.0 * queue_order_high_water / NULLIF(queue_order_capacity, 0)), 0),"
                " COALESCE(MAX(total_p99_us), -1)"
                " FROM health WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            metric_rows, dropped, shard_percent, order_percent, total_p99 = cursor.fetchone()

        connection.close()
    except psycopg2.errors.UndefinedColumn:   # 열을 아직 안 만든 DB — 새 적재기가 첫 HEALTH에서 만든다
        return ("큐·지연 적재", True, "WARN", "health 표에 큐·지연 열 없음 — 적재기 배포 전")
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return ("큐·지연 적재", True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})")

    if metric_rows == 0:
        return ("큐·지연 적재", True, "WARN", "HEALTH에 큐·지연 수치 없음 — 그 수치를 안 싣는 옛 exe")

    return ("큐·지연 적재", dropped == 0, "FAIL",
            f"HEALTH {metric_rows}건, 버린 건수 {dropped} (기대 0), 고수위 샤드 {float(shard_percent):.1f}%"
            f"·주문 큐 {float(order_percent):.1f}%, 전체 지연 p99 "
            + (f"{total_p99 / 1000:.0f}ms" if total_p99 >= 0 else "표본 없음"))


def collect(date: str, log: Path, since: int = 0):
    """로그 한 파일에서 그날 점검 행을 만든다.

    반환 (rows, session_count). rows 원소는 (이름, 통과, 등급, 설명). 세션이 없으면 rows 빈 리스트.
    market_close_autodoc이 마감 문서 4절에 이 표를 그대로 싣는다 — 사람이 따로 돌려 보지 않아도 되게.
    """

    starts: list[int] = []       # 엔진 시작 시각(초)
    last_ts = 0
    stale_max = 0
    guard_at: list[int] = []
    breakeven: list[int] = []
    fills: list[tuple[int, str, str]] = []
    rate_hits = 0
    ws_fallbacks = 0
    rtts: list[int] = []
    bucket_waits: list[int] = []
    recon_slow: list[tuple[int, int]] = []   # (ms, 사이클)
    http_timeouts = 0
    trendx_registered: list[int] = []
    session_window_rejects = 0
    basket_as_of: list[str] = []                 # 목표 비중표 읽음 줄의 as_of(YYYYMMDD)
    basket_orders: list[tuple[int, str, str]] = []  # (초, 종목, 매수|매도) — 바스켓이 낸 주문
    basket_run_end: list[int] = []
    basket_window_closed = 0
    basket_lines = 0
    signals: list[tuple[int, str, str, str]] = []  # (초, 전략 id, 종목, BUY|SELL)
    itb_attached: list[str] = []
    devscale_stops: list[tuple[int, str]] = []     # (초, 종목) — DEVSCALE 손절 신호
    devscale_close_exits: list[tuple[int, str]] = []   # (초, 종목) — DEVSCALE 장 마감 청산 신호(넘김 모드면 0이어야 한다)
    entry_filter: dict[str, int] = {"통과": 0, "차단": 0}  # 진입 필터 판정 줄 수
    platforms: list[str] = []                    # 기동마다 찍히는 실행 플랫폼(Windows|Linux)
    ledger_replays: list[int] = []               # 기동마다 원장 저널에서 되적용한 레코드 수
    ledger_truncated = 0                         # 꼬리 잘린 기동 수 — 쓰다 만 레코드, 곧 비정상 종료 흔적
    ledger_restored = 0                          # 재기동 때 이력에 되살린 미체결 주문
    ledger_released = 0                          # 재기동 때 선점만 푼 주문(KIS가 모르는 주문)
    ledger_start_failures = 0                    # 기동 시점 저널 기록 실패 누계(기동마다 한 줄)
    ledger_write_fails = 0                       # 장중 저널 기록 실패로 안 나간 주문

    # 7일 지난 날은 archive/quant_trader_<날짜>.log.gz — market_close_autodoc이 그 경로를 그대로 넘긴다
    opener = (lambda: gzip.open(log, "rt", encoding="utf-8", errors="replace")) if log.suffix == ".gz"         else (lambda: log.open(encoding="utf-8", errors="replace"))
    with opener() as log_file:
        for line in log_file:
            m = TS_RE.match(line)
            if not m or m.group(1) != date:
                continue
            second = secs(m)
            if second < since:
                continue
            last_ts = second
            if START_RE.search(line):
                starts.append(second)
            if found := PLATFORM_RE.search(line):
                platforms.append(found.group(1))
            found = STALE_RE.search(line)
            if found:
                stale_max = max(stale_max, int(found.group(1)))
            if found := LEDGER_REPLAY_RE.search(line):
                ledger_replays.append(int(found.group(1)))
                ledger_truncated += 1 if found.group(3) else 0
            if found := LEDGER_RESOLVE_RE.search(line):
                ledger_restored += int(found.group(1))
                ledger_released += int(found.group(2))
                ledger_start_failures = max(ledger_start_failures, int(found.group(3)))
            if LEDGER_WRITE_FAIL_RE.search(line):
                ledger_write_fails += 1
            if GUARD_RE.search(line):
                guard_at.append(second)
            if BREAKEVEN_RE.search(line):
                breakeven.append(second)
            found = FILL_RE.search(line)
            if found:
                fills.append((second, found.group(1), found.group(2)))
            if RATE_RE.search(line):
                rate_hits += 1
            if WSFALL_RE.search(line):
                ws_fallbacks += 1
            found = RTT_RE.search(line)
            if found:
                rtts.append(int(found.group(1)))
                if found.group(2) is not None:
                    bucket_waits.append(int(found.group(2)))
            found = RECON_SLOW_RE.search(line)
            if found:
                recon_slow.append((int(found.group(1)), int(found.group(2))))
            if HTTP_TIMEOUT_RE.search(line):
                http_timeouts += 1
            if found := TRENDX_REGISTER_RE.search(line):
                trendx_registered.append(int(found.group(1)))
            if SESSION_WINDOW_REJECT_RE.search(line):
                session_window_rejects += 1
            if "[BASKET_" in line:
                basket_lines += 1
            if found := BASKET_LOADED_RE.search(line):
                basket_as_of.append(found.group(1))
            if found := BASKET_ORDER_RE.search(line):
                basket_orders.append((second, found.group(1), found.group(2)))
            if BASKET_RUN_END_RE.search(line):
                basket_run_end.append(second)
            if BASKET_WINDOW_CLOSED_RE.search(line):
                basket_window_closed += 1
            if found := SIGNAL_RE.search(line):
                signals.append((second, found.group(1), found.group(2), found.group(3)))
            if found := ITB_ATTACH_RE.search(line):
                itb_attached.append(found.group(1))
            if found := DEVSCALE_STOP_RE.search(line):
                devscale_stops.append((second, found.group(1)))
            if found := DEVSCALE_CLOSE_EXIT_RE.search(line):
                devscale_close_exits.append((second, found.group(1)))
            if found := DEVSCALE_ENTRY_FILTER_RE.search(line):
                entry_filter[found.group(2)] += 1

    if not starts:
        return [], 0

    # 세션 길이 — 마지막 세션은 지금까지 살아 있는 것으로 본다.
    bounds = starts + [last_ts]
    short = [starts[i] for i in range(len(starts))
             if bounds[i + 1] - starts[i] < MIN_SESSION_SEC]

    # 재기동 투매 — 청산 관리 부착 직후 창에 들어온 본전탈출.
    dump = [b for b in breakeven
            if any(0 <= b - g <= GUARD_QUIET_SEC for g in guard_at)]

    # 회전 — 같은 종목 매도 체결 뒤 CHURN_SEC 안에 매수 체결.
    churn = 0
    last_sell: dict[str, int] = {}
    for second, ticker, side in fills:
        if side == "SELL":
            last_sell[ticker] = second
        elif ticker in last_sell and second - last_sell[ticker] <= CHURN_SEC:
            churn += 1
            del last_sell[ticker]

    def hhmm(second: int) -> str:
        return f"{second // 3600:02d}:{second % 3600 // 60:02d}:{second % 60:02d}"

    # 주문 접수 지연 — RTT가 큰데 버킷대기가 작으면 서버 응답 지연, 버킷대기가 크면 초당한도 압박.
    slow_orders = sum(1 for rtt in rtts if rtt >= SLOW_ORDER_MS)
    orders_ok = not rtts or slow_orders <= len(rtts) * MAX_SLOW_ORDER_RATIO
    if not rtts:
        order_detail = "주문 없음"
    else:
        order_detail = (f"접수·거부 {len(rtts)}건 RTT 중앙값 {median(rtts)}ms, "
                        f"{SLOW_ORDER_MS // 1000}초↑ {slow_orders}건")
        if not bucket_waits:
            order_detail += " — 버킷대기 계측 없음(09-19 이전 바이너리)"
        elif median(bucket_waits) >= BUCKET_WAIT_MS:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms → 초당한도 버킷이 원인"
        elif not orders_ok:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms → KIS 서버 응답 지연(버킷 아님)"
        else:
            order_detail += f", 버킷대기 중앙값 {median(bucket_waits)}ms"

    # 잔고 조회 지연(D-100) — 조회가 사이클을 넘긴 횟수와 제한 시간 초과 GET. 사이클은 멈추지 않았어야 한다.
    if recon_slow:
        worst_ms = max(milliseconds for milliseconds, _ in recon_slow)
        worst_cycles = max(cycles for _, cycles in recon_slow)
        recon_detail = (f"사이클 넘긴 조회 {len(recon_slow)}회 (최대 {worst_ms}ms·{worst_cycles}사이클), "
                        f"제한 시간 초과 {http_timeouts}건 (허용 {MAX_HTTP_TIMEOUTS})")
    else:
        recon_detail = f"사이클 넘긴 조회 0회, 제한 시간 초과 {http_timeouts}건 (허용 {MAX_HTTP_TIMEOUTS})"

    # 바스켓(D-109) — 소유 종목은 그날 바스켓이 낸 주문·신호에서 모은다(파일을 다시 읽지 않는다 — 로그가 그날의 정본).
    basket_tickers = {ticker for _, ticker, _ in basket_orders} | {ticker for _, sid, ticker, _ in signals if sid.startswith("BASKET_")}
    basket_itb = sorted(set(itb_attached) & basket_tickers)
    foreign_sells = [(second, sid, ticker) for second, sid, ticker, side in signals
                     if side == "SELL" and ticker in basket_tickers and not sid.startswith("BASKET_")]
    order_counts: dict[tuple[str, str], int] = {}
    for _, ticker, side in basket_orders:
        order_counts[(ticker, side)] = order_counts.get((ticker, side), 0) + 1
    basket_duplicates = sorted(f"{ticker} {side}" for (ticker, side), count in order_counts.items() if count > 1)
    date_compact = date.replace("-", "")
    basket_file_ok = bool(basket_as_of) and basket_as_of[-1] == date_compact
    buy_leg_ok = basket_window_closed == 0 and (not basket_orders or (basket_run_end and max(basket_run_end) <= BASKET_BUY_LEG_DEADLINE))
    basket_skip = basket_lines == 0   # TARGET_BASKET 미로드(배포 전 날짜) — 판정하지 않는다

    def basket_row(name: str, ok: bool, level: str, detail: str):
        if basket_skip:
            return (name, True, level, "TARGET_BASKET 줄 없음(미로드)")
        return (name, ok, level, detail)

    # 09-20 청산선 재설정 — 네 손실 경로. 승계 직후 매도는 그 종목의 마지막 부착 시각 기준.
    displace_sells = [(second, ticker) for second, sid, ticker, side in signals if sid == "DISPLACE" and side == "SELL"]
    #  부착은 묶음 한 줄(청산 관리 N종목 부착)로만 찍히므로 그 시각 기준 — 종목별 줄(+ ITB)은 D-109 바이너리부터.
    itb_early_sells = [(second, ticker) for second, sid, ticker, side in signals
                       if sid.startswith("ITB_") and side == "SELL"
                       and any(0 <= second - attach_t <= GUARD_QUIET_SEC for attach_t in guard_at)]
    stop_rebuys = [(buy_t, ticker) for buy_t, sid, ticker, side in signals
                   if sid.startswith("DEVSCALE_") and side == "BUY"
                   and any(stop_ticker == ticker and buy_t > stop_t for stop_t, stop_ticker in devscale_stops)]
    devscale_buys = sum(1 for _, sid, _, side in signals if sid.startswith("DEVSCALE_") and side == "BUY")
    stop_ratio = len(devscale_stops) / devscale_buys if devscale_buys else 0.0

    filter_judged = entry_filter["통과"] + entry_filter["차단"]
    devscale_v2 = filter_judged > 0     # 진입 필터 줄이 있으면 D-111 넘김 바이너리

    # 원장 저널(D-113) — 리플레이 줄이 없으면 저널 이전 바이너리다. 없는 기능을 실패로 적지 않는다.
    ledger_skip = not ledger_replays
    ledger_failures = ledger_start_failures + ledger_write_fails

    def ledger_row(name: str, ok: bool, level: str, detail: str):
        if ledger_skip:
            return (name, True, level, "원장 저널 줄 없음(D-113 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    def devscale_v2_row(name: str, ok: bool, level: str, detail: str):
        if not devscale_v2:
            return (name, True, level, "진입 필터 판정 줄 없음(D-111 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    rows = [
        devscale_v2_row("장 마감 청산(넘김)", not devscale_close_exits, "FAIL",
                        f"DEVSCALE 장 마감 청산 신호 {len(devscale_close_exits)}건 (기대 0 — market_close_exit_hhmm 2400, D-111)"
                        + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in devscale_close_exits[:5])}" if devscale_close_exits else "")),
        devscale_v2_row("진입 필터 판정", entry_filter["차단"] > 0, "WARN",
                        f"진입 필터 통과 {entry_filter['통과']} / 차단 {entry_filter['차단']} 종목 (리플레이 기대: 존 안 종목의 절반쯤 차단, 차단 0이면 필터 값이 안 실린 것)"),
        # 리눅스 실행일(09-22~)은 Windows 감시견이 -NoTrader라 Windows 기동이 0이어야 한다. 둘이 섞이면 같은 계좌에 엔진 둘.
        ("실행 플랫폼", len(set(platforms)) <= 1, "FAIL",
         "·".join(f"{name} {platforms.count(name)}회" for name in sorted(set(platforms))) or "플랫폼 줄 없음(구 exe)"),
        ("교체 매도", not displace_sells, "FAIL",
         f"교체 매도 신호 {len(displace_sells)}건 (기대 0 — displace_enabled false, 09-20)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in displace_sells[:5])}" if displace_sells else "")),
        ("승계 직후 매도", not itb_early_sells, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 매도 {len(itb_early_sells)}건 (기대 0 — guard_warmup_sec 0 우회, 09-20)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in itb_early_sells[:5])}" if itb_early_sells else "")),
        ("손절 뒤 당일 재매수", not stop_rebuys, "FAIL",
         f"DEVSCALE 손절 {len(devscale_stops)}건 뒤 같은 종목 재매수 {len(stop_rebuys)}건 (기대 0 — stop_cooldown_sec 21600)"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in stop_rebuys[:5])}" if stop_rebuys else "")),
        ("손절 비율", devscale_buys < 20 or stop_ratio <= MAX_STOP_PER_BUY, "WARN",
         f"DEVSCALE 손절 {len(devscale_stops)} / 매수 신호 {devscale_buys} = {stop_ratio:.0%} (허용 {MAX_STOP_PER_BUY:.0%}, 분봉 예측 21% — 넘으면 -2.0%가 잡음에 걸리는 것)"
         + (" — 표본 20건 미만, 판정 보류" if devscale_buys < 20 else "")),
        ("유령주문 재부활", stale_max <= MAX_STALE_ORDERS, "FAIL",
         f"기동 시 미체결 최대 {stale_max}건 (허용 {MAX_STALE_ORDERS})"),
        ledger_row("원장 저널 기록", ledger_failures == 0, "FAIL",
                   f"저널 기록 실패 {ledger_failures}건 (기대 0 — 못 적은 주문은 보내지 않으니 그만큼 매매가 빈다."
                   f" 기동 {ledger_start_failures}건 · 장중 {ledger_write_fails}건)"),
        ledger_row("원장 재기동 대조", ledger_released == 0 and ledger_truncated == 0, "WARN",
                   f"되살림 {ledger_restored}건 · 선점해제 {ledger_released}건 · 꼬리 잘림 {ledger_truncated}회"
                   f" (리플레이 최대 {max(ledger_replays, default=0)}건 — 선점해제는 원장에 적고 KIS엔 안 간 주문,"
                   f" 꼬리 잘림은 쓰다 만 레코드)"),
        ("조기 사망 세션", not short, "FAIL",
         f"{MIN_SESSION_SEC}초 미만 종료 {len(short)}회"
         + (f" — {', '.join(hhmm(second) for second in short[:5])}" if short else "")),
        ("재기동 투매", not dump, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 본전탈출 {len(dump)}건"
         + (f" — {', '.join(hhmm(second) for second in dump[:5])}" if dump else "")),
        ("매도→재매수 회전", churn <= MAX_CHURN, "FAIL",
         f"{CHURN_SEC}초 내 반대매매 {churn}회 (허용 {MAX_CHURN})"),
        ("초당한도 압박", rate_hits <= MAX_RATE_HITS, "WARN",
         f"초당 거래건수 거부 {rate_hits}건 (허용 {MAX_RATE_HITS})"),
        ("WS 폴백", ws_fallbacks == 0, "WARN",
         f"REST 폴링 폴백 {ws_fallbacks}회 — 틱 주기 30초"),
        ("주문 접수 지연", orders_ok, "WARN", order_detail),
        ("잔고 조회 지연", http_timeouts <= MAX_HTTP_TIMEOUTS, "WARN", recon_detail),
        ("TRENDX 정지", max(trendx_registered, default=0) == 0, "FAIL",
         (f"초기 등록 최대 {max(trendx_registered)}종목 (기대 0, D-101 결정 2)" if trendx_registered
          else "TRENDX 등록 줄 없음 — 전략 미로드 또는 등록 0")),
        ("매매 창 밖 거부", session_window_rejects == 0, "FAIL",
         f"세션 창 밖 거부 {session_window_rejects}건 (기대 0 — 마감 청산 모의 15:15·실계좌 19:50, D-101 결정 3)"),
        basket_row("바스켓 파일 당일", basket_file_ok, "FAIL",
                   (f"목표 비중표 as_of={basket_as_of[-1]} (기대 {date_compact}, 08:40 작성기)" if basket_as_of
                    else "목표 비중표 읽음 줄 없음 — 08:40 작성기 미실행 또는 파일 검증 실패")),
        basket_row("바스켓 청산관리 부착", not basket_itb, "FAIL",
                   f"바스켓 종목에 ITB 부착 {len(basket_itb)}건 (기대 0, D-109 격리 1)"
                   + (f" — {', '.join(basket_itb[:5])}" if basket_itb else "")),
        basket_row("바스켓 타전략 매도", not foreign_sells, "FAIL",
                   f"바스켓 종목을 다른 전략이 판 신호 {len(foreign_sells)}건 (기대 0 — 교체·15:15 청산·초과분 정리, D-109 격리 2·3)"
                   + (f" — {', '.join(f'{hhmm(second)} {sid} {ticker}' for second, sid, ticker in foreign_sells[:3])}" if foreign_sells else "")),
        basket_row("바스켓 재기동 중복", not basket_duplicates, "FAIL",
                   f"같은 종목·방향 주문 2회 이상 {len(basket_duplicates)}건 (기대 0, 상태 파일 선기록)"
                   + (f" — {', '.join(basket_duplicates[:5])}" if basket_duplicates else "")),
        basket_row("바스켓 매수 레그 시각", buy_leg_ok, "WARN",
                   (f"집행 끝 {hhmm(max(basket_run_end))} (기한 15:05), 창 종료 이월 {basket_window_closed}회, 주문 {len(basket_orders)}건"
                    if basket_run_end else f"집행 끝 줄 없음, 창 종료 이월 {basket_window_closed}회, 주문 {len(basket_orders)}건")),
        *resource_sampling_rows(date),
        *feed_ledger_rows(date),
        queue_latency_row(date),
    ]
    return rows, len(starts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--date", default=dt.date.today().isoformat())
    parser.add_argument("--log", default=str(DEFAULT_LOG))
    # 하루 로그는 누적이라, 방금 고친 결함의 과거 이력까지 같이 잡힌다.
    #  "고친 뒤로 다시 났나"를 보려면 수정 반영 시각을 준다.
    parser.add_argument("--since", default="", help="HH:MM — 이 시각 이후만 본다")
    arguments = parser.parse_args()

    since = 0
    if arguments.since:
        hour_text, _, minute_text = arguments.since.partition(":")
        since = int(hour_text) * 3600 + int(minute_text or 0) * 60

    log = Path(arguments.log)
    if not log.exists():
        print(f"로그 없음: {log}")
        return 1

    rows, session_count = collect(arguments.date, log, since)
    if not rows:
        print(f"{arguments.date}: 엔진 시작 기록이 없다 — 점검할 세션이 없음")
        return 0

    scope = f" {arguments.since}~" if arguments.since else ""
    print(f"=== 실행 건전성 점검 {arguments.date}{scope} (세션 {session_count}회) ===")
    bad = 0
    for name, ok, level, detail in rows:
        tag = "PASS" if ok else level
        if not ok and level == "FAIL":
            bad += 1
        print(f"  [{tag:4}] {name:16} {detail}")
    print(f"--- FAIL {bad}건 ---")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
