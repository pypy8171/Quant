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
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402
from log_patterns import GUARD_ATTACH_RE as GUARD_RE  # noqa: E402

DEFAULT_LOG = _logdir.log_dir() / "quant_trader.log"
# ThreadSanitizer 회차가 남기는 한 줄 요약. scripts/tsan_round.sh 가 쓴다.
TSAN_STATE = REPO / "_private" / "state" / "tsan_last.json"
# KIS 국내휴장일조회(CTCA0903R) 24일치 캐시. scripts/check_market_open.py 가 쓴다.
HOLIDAY_CALENDAR = REPO / "logs" / "holiday_calendar.json"
# 그 회차가 본 커밋 뒤로 여기가 바뀌었으면 회차를 다시 돌 때다 — 스레드가 여럿 붙는 코드만 고른다.
TSAN_WATCH_PATHS = ("Quant/include/core", "Quant/src/core", "Quant/include/risk", "Quant/src/risk",
                    "Quant/include/ipc", "Quant/src/ipc", "Quant/include/feed", "Quant/src/feed")

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
# 재시도 없이 버린 요청(제한 시간 초과). 위 HTTP_TIMEOUT_RE는 시도 하나가 시간을 넘긴 것이고, 이쪽은 그래서 포기한 호출이다.
CURL_GIVEUP_RE = re.compile(r"수신 제한 시간 초과 — 재시도 없이 실패 처리")
# 전송 한 겹이 초당 한도(EGW00201 응답)를 보고 같은 요청을 되보낸 줄. 주문 거부(RATE_RE)와는 다른 층이다.
RATE_RETRY_RE = re.compile(r"\(초당 한도\) — 재시도")
# 프리페치의 기준자본 조회가 실패해 쿨다운에 들어간 줄(쿨다운이 60초라 하루 최대 수백 건이 아니라 수 건이어야 한다).
EQUITY_FAIL_RE = re.compile(r"기준자본\(총평가금\) 조회 실패")
# 잔고조회 연속 실패로 신규 매수만 멈춘 창(B2). 켜짐↔풀림을 짝지어 누적 분을 잰다.
B2_ON_RE = re.compile(r"BUY NEW 보수 정지\(B2\)")
B2_OFF_RE = re.compile(r"신규 진입 정지 해제\(B2\)")
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
# 아래 셋은 2026-09-22 리눅스 첫 거래일에 관측한 값을 고친 뒤의 기대치로 잡은 것이다(리뷰 docs/market_close/2026-09-22.md).
MAX_CURL_GIVEUPS = 5      # 수신 제한 시간 초과로 재시도 없이 버린 요청 — 09-22 21건. 연결 재사용을 넣었으니 줄어야 한다
MAX_EQUITY_FAILS = 3      # 기준자본 조회 실패 — 09-22엔 로그가 없었고 쿨다운과 같이 들어갔다. 쿨다운이 60초라 하루 상한이 곧 이 수다
MAX_RATE_RETRIES = 10     # 초당 한도로 되보낸 HTTP 요청 — 09-22 37건. 모의 스캔 간격을 600ms로 벌렸으니 줄어야 한다
# 원장 저널(D-113) — 기동 줄 둘과 장중 기록 실패. 저널에 못 적은 주문은 아예 나가지 않는다.
LEDGER_REPLAY_RE = re.compile(r"\[Engine\] 원장 저널 리플레이: (\d+)건 \(마지막 seq (\d+)(, 꼬리 잘림)?\)")
LEDGER_RESOLVE_RE = re.compile(r"\[Engine\] 원장 미결 주문 대조: 되살림 (\d+)건 · 선점해제 (\d+)건 · 저널기록실패 (\d+)건")
LEDGER_WRITE_FAIL_RE = re.compile(r"\[OrderRouter\] 원장 저널 기록 실패")
# 엔진이 모르는 채 브로커에 살아 있던 주문 — 전송이 타임아웃 나면 KIS에는 접수됐는데 ODNO를 못 받아
#  부속 파일에 못 적는다. 그 주문이 보유분을 묶으면 손절이 닿아도 못 판다(2026-09-23 09:26 021240,
#  ODNO=0000007886 매도 18주). 기동 때 브로커 조회로 보충하면 이 줄이 남는다 — 남았다는 건 그날 샜다는 뜻이다.
UNTRACKED_OPEN_RE = re.compile(r"부속 파일에 없는 미체결 — 브로커 조회로 보충")
# 청산이 막혔는데 풀지 못한 채 넘어간 것. 위와 같은 뿌리이나 이쪽은 재기동 전까지 방치된다.
BLOCKED_SELL_RE = re.compile(r"청산차단 미해소")
# 전략 사망 마무리(D-114 단계 2) — 주문 스레드가 전략 박동 공백만 보고 낸 판정.
BEAT_DEAD_RE = re.compile(r"\[마무리\] 전략 박동이 끊겼다")
BEAT_BACK_RE = re.compile(r"\[마무리\] 전략 박동이 돌아왔다")
# [큐 고수위] 줄 꼬리 — 없으면 D-114 배포 전 바이너리라 이 세 행을 판정하지 않는다.
BEAT_GAP_RE = re.compile(r"beat_gap_max=(\d+)ms")
ORDER_DUPLICATE_RE = re.compile(r"order_duplicate=(\d+)")
ORDER_RESPONSE_DROP_RE = re.compile(r"order_response_dropped=(\d+)")
# 장부 사본(D-114 단계 2.5) — 낸 판 수와, 한 계좌만 담는 사본에 못 실은 남의 계좌 줄 수.
LEDGER_GEN_RE = re.compile(r"ledger_gen=(\d+)")
LEDGER_FOREIGN_RE = re.compile(r"ledger_foreign=(\d+)")
# 제어 요청(D-114 단계 2.5 갈래 B) — 전략이 큐가 가득 차 못 보낸 줄 수, 주문 쪽이 반쪽 표로 보고 버린 줄 수.
CONTROL_DROP_RE = re.compile(r"control_dropped=(\d+)")
CONTROL_DISCARD_RE = re.compile(r"control_discarded=(\d+)")
# 체결통보 세션(D-114 단계 3) — 기동마다 한 줄. 맡은 소켓이 몇 번인지, 아무도 안 맡았는지, 둘 이상이 맡았는지.
FILL_SESSION_ONE_RE = re.compile(r"\[Engine\] 체결통보 세션: 소켓 (\d+)")
FILL_SESSION_NONE_RE = re.compile(r"\[Engine\] 체결통보 세션: 없음")
FILL_SESSION_MANY_RE = re.compile(r"\[Engine\] 체결통보 세션: (\d+)개")
# ZMQ PUB/REP 포트를 먼저 뜬 엔진이 잡고 있으면 나중에 뜬 쪽은 bind 에 실패한 뒤 ERROR 한 줄만 남기고
#  계속 돈다 — 체결·시그널이 TimescaleDB 에 하나도 안 들어간 채로 매매한다. 계좌를 둘 돌리는 날의
#  가장 조용한 실패라 판정 행으로 둔다(실계좌는 5565/5566 으로 옮겨 놨다). [why D-122]
ZMQ_BIND_FAIL_RE = re.compile(r"\[ZMQ\] 소켓 bind 실패")

BASKET_BUY_LEG_DEADLINE = 15 * 3600 + 5 * 60  # 매수 레그는 15:05까지 끝나야 마감 청산(15:15)과 겹치지 않는다(D-109)


# 전략 박동 문턱 — Quant/include/ipc/Heartbeat.h의 HeartbeatConfig 기본값과 같은 값이다(D-114 단계 2).
BEAT_SUSPECT_MS = 250
BEAT_DEAD_MS = 1000

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

    # 윈도우 venv(Lib/site-packages)와 리눅스 venv(lib/python*/site-packages)는 서로 붙이지 않는다 —
    # WSL에서 윈도우 psycopg2를 붙였다가 wheel 안 os.add_dll_directory 호출로 AttributeError가 나
    # 마감 판정이 통째로 죽었다(2026-09-22 리눅스 첫날)
    if sys.platform == "win32":
        candidates = sorted(REPO.glob("PYQuant/.venv*/Lib/site-packages"))
    else:
        candidates = sorted(REPO.glob("PYQuant/.venv*/lib/python*/site-packages"))

    for site_packages in candidates:
        if not site_packages.is_dir():
            continue

        sys.path.append(str(site_packages))

        try:
            import psycopg2
            return psycopg2
        except Exception:
            # ImportError만 잡으면 안 된다 — 다른 플랫폼용 wheel은 import 도중 AttributeError 같은
            # 것을 던지고, 그것이 판정기 전체를 끝내 버린다. 붙인 경로를 되돌리고 다음 후보로 간다
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
    ③ 신호는 주문·체결과 달리 계좌를 안 싣고 발행했다. 그것을 고친 뒤로 `signals.account`가 비어 있으면
       배포 전 exe가 떠 있다는 뜻이고, 받는 쪽 계좌 필터가 전부 떨궈 signals 적재가 통째로 멈춘다.
    """
    password = tsdb_password()

    if not password:
        return [("피드 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("개장부터 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("원장 계좌 단일", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함"),
                ("신호 계좌 적재", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")]

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return [("피드 적재", False, "WARN", "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary"),
                ("개장부터 적재", False, "WARN", "psycopg2 없음 — 위와 같다"),
                ("원장 계좌 단일", False, "WARN", "psycopg2 없음 — 위와 같다"),
                ("신호 계좌 적재", False, "WARN", "psycopg2 없음 — 위와 같다")]

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

            cursor.execute(
                "SELECT COUNT(*), COUNT(*) FILTER (WHERE COALESCE(TRIM(account), '') = '') FROM signals"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            signal_count, signal_no_account = cursor.fetchone()
            cursor.execute(
                "SELECT COUNT(*) FROM orders WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            order_count = cursor.fetchone()[0]

        connection.close()
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        detail = f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})"
        return [("피드 적재", True, "WARN", detail), ("개장부터 적재", True, "WARN", detail),
                ("원장 계좌 단일", True, "WARN", detail), ("신호 계좌 적재", True, "WARN", detail)]

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

    # 신호에 계좌가 실리는지. 주문은 났는데 신호가 0이면 받는 쪽 계좌 필터가 전부 떨군 것이고(옛 exe가 떠 있다),
    #  신호는 들어왔는데 계좌가 비면 그 행으로는 남의 엔진 신호를 가려낼 수 없다.
    if order_count > 0 and signal_count == 0:
        signal_row = ("신호 계좌 적재", False, "FAIL",
                      f"주문 {order_count}건인데 신호 0건 — 계좌를 안 싣는 배포 전 exe가 떠 있고"
                      " PYQuant/main.py 계좌 필터가 전부 떨궜다. exe와 파이썬을 같이 올린다")
    elif signal_count == 0:
        signal_row = ("신호 계좌 적재", True, "WARN", "신호·주문 모두 없는 날 — 판정 안 함")
    else:
        signal_row = ("신호 계좌 적재", signal_no_account == 0, "FAIL",
                      f"신호 {signal_count}건 중 계좌 없음 {signal_no_account}건"
                      + (" — 배포 전 exe가 섞여 있다" if signal_no_account else ""))

    return [feed_row, opening_row, ledger_row, signal_row]


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


def tsan_stale_commits(commit: str) -> int:
    """그 커밋 뒤로 스레드가 여럿 붙는 코드가 몇 번 바뀌었나. -1 = 셀 수 없음.

    날짜로 재면 아무도 그 코드를 안 건드린 주에도 경보가 울린다. 바뀐 횟수로 재면
    "돌 이유가 생겼는데 안 돌았다"만 걸린다.
    """
    if not commit or commit == "unknown":
        return -1

    try:
        counted = subprocess.run(["git", "rev-list", "--count", f"{commit}..HEAD", "--", *TSAN_WATCH_PATHS],
                                 cwd=REPO, capture_output=True, text=True, timeout=20)

        if counted.returncode != 0:
            return -1

        return int(counted.stdout.strip() or 0)
    except (OSError, ValueError, subprocess.SubprocessError):
        return -1


def tsan_row(date: str) -> tuple:
    """ThreadSanitizer 회차(scripts/tsan_round.sh)가 돌았는지, 경합 보고가 났는지.

    스레드 경합은 Release 테스트를 그대로 통과한다 — 값이 어긋난 채 장중까지 가서야 드러난다.
    그래서 마지막 회차 뒤로 동시성 코드가 바뀐 것도 경보로 본다(D-116 후속).
    """
    try:
        state = json.loads(TSAN_STATE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return ("TSAN 회차", True, "WARN",
                "회차 기록 없음 — WSL2 저장소 루트에서 bash scripts/tsan_round.sh")
    except (OSError, ValueError) as error:
        return ("TSAN 회차", True, "WARN", f"회차 기록을 못 읽음 — 판정 안 함 ({str(error).strip()[:80]})")

    stage = str(state.get("stage", "?"))
    finished = str(state.get("finished", ""))[:10]
    races = int(state.get("races", 0))
    tests_failed = int(state.get("tests_failed", 0))
    tests_total = int(state.get("tests_total", 0))
    failed_names = [str(name) for name in state.get("failed_names", [])]
    commit = str(state.get("commit", "?"))
    minutes = int(state.get("elapsed_sec", 0)) // 60

    # 빌드·설정이 안 된 회차는 그 자체가 경합 판정이 아니다 — 고칠 곳이 다르므로 WARN으로 둔다.
    if stage != "done":
        return ("TSAN 회차", True, "WARN",
                f"{finished} 회차가 {stage} 에서 멈췄다 (커밋 {commit}) — logs/tsan/ 의 그 회차 로그 끝을 본다")

    if races or tests_failed:
        detail = (f"{finished} 회차 (커밋 {commit}, {minutes}분): 테스트 {tests_total - tests_failed}/{tests_total}, "
                  f"경합 보고 {races}건 (기대 0)")
        if failed_names:
            detail += f" — 떨어진 테스트 {', '.join(failed_names[:5])}"
        return ("TSAN 회차", False, "FAIL", detail)

    stale = tsan_stale_commits(commit)

    if stale > 0:
        return ("TSAN 회차", True, "WARN",
                f"{finished} 회차(커밋 {commit}) 뒤로 스레드가 여럿 붙는 코드가 {stale}번 바뀌었다 — "
                f"머지 전에 bash scripts/tsan_round.sh")

    return ("TSAN 회차", True, "FAIL",
            f"{finished} 회차 (커밋 {commit}, {minutes}분): 테스트 {tests_total - tests_failed}/{tests_total}, "
            f"경합 보고 {races}건, 그 뒤 동시성 코드 변경 "
            + (f"{stale}건" if stale >= 0 else "셀 수 없음"))


def market_open_gate_row(date: str) -> tuple:
    """감시견의 휴장일 관문(scripts/auto_trade_day.ps1)이 그날 제대로 갈렸는지.

    휴장일에 떠도 주문은 안 나가지만 토큰을 새로 받고 WS 재접속을 되풀이한다. 반대로 개장일에 관문이
    잘못 걸리면 그날 매매가 통째로 없다 — 이쪽이 훨씬 비싸서 FAIL로 본다. [why D-120]
    개장 여부를 모르는 날(조회 실패)은 판정하지 않는다 — 관문 자체가 그때는 통과시키기로 돼 있다.
    """
    name = "휴장일 관문"
    date_compact = date.replace("-", "")

    try:
        cached = json.loads(HOLIDAY_CALENDAR.read_text(encoding="utf-8"))
        calendar = {str(row.get("bass_dt", "")): str(row.get("opnd_yn", "")).upper()
                    for row in cached.get("rows", [])}
    except (OSError, ValueError):
        calendar = {}

    open_flag = calendar.get(date_compact, "")

    if open_flag not in ("Y", "N"):
        return (name, True, "WARN",
                f"{date_compact} 개장 여부를 달력에서 못 찾았다 — 판정 안 함"
                " (py scripts/check_market_open.py 로 달력을 받는다)")

    # 감시견은 엔진 로그 폴더가 아니라 저장소 logs/ 에 쓴다(scripts/auto_trade_day.ps1 $RunLog).
    run_log = REPO / "logs" / f"auto_trade_day_{date_compact}.log"

    try:
        log_text = run_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return (name, True, "WARN", f"감시견 실행 로그 없음({run_log.name}) — 감시견이 안 돌았다, 판정 안 함")

    skipped = "휴장일 — 트레이더도 부속 창도 띄우지 않는다" in log_text
    unknown = "개장 여부를 확인하지 못했다" in log_text

    if open_flag == "N":
        return (name, skipped, "FAIL",
                ("휴장일을 걸러 아무것도 안 띄웠다" if skipped
                 else "휴장일인데 관문이 안 걸렸다 — 감시견이 창을 띄우고 하루를 헛돌았다"))

    if skipped:
        return (name, False, "FAIL",
                "개장일인데 휴장으로 걸러 아무것도 안 띄웠다 — 그날 매매가 통째로 없다")

    return (name, True, "FAIL",
            "개장일을 그대로 통과했다" + (" (개장 여부 조회는 실패했고 관문이 통과시켰다)" if unknown else ""))


def order_latency_breakdown_row(date: str) -> tuple:
    """주문 한 건이 어디서 시간을 썼는지. 우리 쪽 구간(리스크 점검·원장 선기록)만 판정하고
    증권사 쪽(초당 한도 대기·왕복)은 수치만 적는다 — 우리가 줄일 수 없는 것으로 FAIL을 내면 판정이 무뎌진다.

    누적 열이 아니라 구간 열(직전 HEALTH 이후)을 본다. 누적은 기동 후 한 번 튄 값이 하루 내내 남아
    '오늘 느렸나'에 답하지 못한다."""
    password = tsdb_password()

    if not password:
        return ("주문 구간 지연", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return ("주문 구간 지연", False, "WARN",
                "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary")

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COALESCE(SUM(latency_interval_samples), 0),"
                " COALESCE(MAX(gate_p99_interval_us), -1),"
                " COALESCE(MAX(journal_p99_interval_us), -1),"
                " COALESCE(MAX(pop_to_send_p99_interval_us), -1),"
                " COALESCE(MAX(bucket_wait_p99_interval_us), -1),"
                " COALESCE(MAX(transport_p99_interval_us), -1)"
                " FROM health WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s", (date,))
            samples, gate, journal, rate_limit, bucket_wait, transport = cursor.fetchone()

        connection.close()
    except psycopg2.errors.UndefinedColumn:   # 열을 아직 안 만든 DB — 새 적재기가 첫 HEALTH에서 만든다
        return ("주문 구간 지연", True, "WARN", "health 표에 구간 열 없음 — 적재기 배포 전")
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return ("주문 구간 지연", True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})")

    if samples == 0:
        return ("주문 구간 지연", True, "WARN", "그날 주문이 없어 구간 표본이 없다 — 판정 안 함")

    def milliseconds(value) -> str:
        return f"{value / 1000:.1f}ms" if value >= 0 else "표본 없음"

    # 우리 쪽 두 구간의 한도. 리스크 점검은 락을 품은 메모리 연산이고 원장 선기록은 로컬 파일 쓰기라
    # 정상이면 1ms 아래다 — 50ms를 넘으면 락 경합이나 디스크 쪽에 무언가 생긴 것이다.
    limit_us = 50_000
    ours_ok  = max(gate, journal) < limit_us

    return ("주문 구간 지연", ours_ok, "FAIL",
            f"주문 {samples}건 — 우리 쪽: 리스크 점검 {milliseconds(gate)}·원장 선기록 {milliseconds(journal)}"
            f" (한도 {limit_us // 1000}ms), 증권사 쪽: 초당 한도 대기 {milliseconds(bucket_wait)}"
            f"·왕복 {milliseconds(transport)}, 우리가 건 호출 간격 조절 {milliseconds(rate_limit)}")


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
    untracked_opens = 0
    blocked_sells = 0
    ws_fallbacks = 0
    rtts: list[int] = []
    bucket_waits: list[int] = []
    recon_slow: list[tuple[int, int]] = []   # (ms, 사이클)
    http_timeouts = 0
    curl_giveups = 0
    rate_retries = 0
    equity_fails = 0
    b2_on: list[int] = []    # BUY NEW 보수 정지가 켜진 초
    b2_off: list[int] = []   # 풀린 초
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
    beat_dead = 0                                # 주문 스레드가 전략을 죽었다고 본 횟수
    beat_back = 0                                # 박동이 돌아와 진입 정지를 푼 횟수
    beat_gap_max = -1                            # 전략 박동의 가장 긴 공백(ms). -1이면 그 줄이 없는 구 exe
    order_duplicate = 0                          # 주문 쪽이 같은 순번을 두 번 받아 거른 수
    order_response_dropped = 0                   # 전략이 답을 안 가져가 버린 수
    ledger_gen = 0                               # 장부 사본이 낸 판 수
    ledger_gen_previous = -1                     # 직전 고수위 줄의 판 번호. 같으면 그사이에 한 판도 안 나간 것
    ledger_stall_at = []                         # 판이 안 늘어난 지점의 초
    ledger_foreign = -1                          # 사본에 못 실은 남의 계좌 줄 수. -1이면 그 줄이 없는 구 exe
    control_dropped = 0                          # 큐가 가득 차 전략이 못 보낸 제어 요청 줄 수
    control_discarded = -1                       # 주문 쪽이 반쪽 표로 보고 버린 줄 수. -1이면 그 줄이 없는 구 exe
    fill_session_socket = -1                     # 체결통보를 맡은 소켓 번호. -1이면 그 줄이 없는 구 exe
    fill_session_none = 0                        # 맡은 소켓이 없다고 찍힌 기동 수
    fill_session_many = 0                        # 둘 이상이 맡았다고 찍힌 기동 수
    zmq_bind_fail = 0                            # ZMQ 포트 bind 실패(포트 충돌) 횟수

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
            if BEAT_DEAD_RE.search(line):
                beat_dead += 1
            if BEAT_BACK_RE.search(line):
                beat_back += 1
            if found := BEAT_GAP_RE.search(line):
                beat_gap_max = max(beat_gap_max, int(found.group(1)))
            if found := ORDER_DUPLICATE_RE.search(line):
                order_duplicate = max(order_duplicate, int(found.group(1)))
            if found := ORDER_RESPONSE_DROP_RE.search(line):
                order_response_dropped = max(order_response_dropped, int(found.group(1)))
            if found := LEDGER_GEN_RE.search(line):
                generation = int(found.group(1))
                # 같은 판 번호가 두 번 실리면 그사이에 사본이 한 판도 안 나간 것이다. 줄어든 것은
                #  멈춤이 아니라 재기동이다 — 새 프로세스는 0부터 다시 센다.
                if generation == ledger_gen_previous:
                    ledger_stall_at.append(second)
                ledger_gen_previous = generation
                ledger_gen = max(ledger_gen, generation)
            if found := LEDGER_FOREIGN_RE.search(line):
                ledger_foreign = max(ledger_foreign, int(found.group(1)))
            if found := CONTROL_DROP_RE.search(line):
                control_dropped = max(control_dropped, int(found.group(1)))
            if found := CONTROL_DISCARD_RE.search(line):
                control_discarded = max(control_discarded, int(found.group(1)))
            if found := FILL_SESSION_ONE_RE.search(line):
                fill_session_socket = int(found.group(1))
            elif FILL_SESSION_NONE_RE.search(line):
                fill_session_none += 1
            elif FILL_SESSION_MANY_RE.search(line):
                fill_session_many += 1
            if ZMQ_BIND_FAIL_RE.search(line):
                zmq_bind_fail += 1
            if GUARD_RE.search(line):
                guard_at.append(second)
            if BREAKEVEN_RE.search(line):
                breakeven.append(second)
            found = FILL_RE.search(line)
            if found:
                fills.append((second, found.group(1), found.group(2)))
            if RATE_RE.search(line):
                rate_hits += 1
            if UNTRACKED_OPEN_RE.search(line):
                untracked_opens += 1
            if BLOCKED_SELL_RE.search(line):
                blocked_sells += 1
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
            if CURL_GIVEUP_RE.search(line):
                curl_giveups += 1
            if RATE_RETRY_RE.search(line):
                rate_retries += 1
            if EQUITY_FAIL_RE.search(line):
                equity_fails += 1
            if B2_ON_RE.search(line):
                b2_on.append(second)
            if B2_OFF_RE.search(line):
                b2_off.append(second)
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

    # 보수 정지(B2) 누적 분. 풀림 줄이 없으면 그 창은 장 끝(15:30)까지 열려 있던 것으로 본다.
    b2_minutes = 0
    for on_second in b2_on:
        off_second = next((second for second in b2_off if second >= on_second), 15 * 3600 + 30 * 60)
        b2_minutes += max(0, off_second - on_second) // 60

    def devscale_v2_row(name: str, ok: bool, level: str, detail: str):
        if not devscale_v2:
            return (name, True, level, "진입 필터 판정 줄 없음(D-111 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 통로·박동(D-114 단계 2) — [큐 고수위] 꼬리가 없으면 그 이전 바이너리다. 없는 기능을 실패로 적지 않는다.
    channel_skip = beat_gap_max < 0

    def channel_row(name: str, ok: bool, level: str, detail: str):
        if channel_skip:
            return (name, True, level, "통로 수치 줄 없음(D-114 단계 2 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 장부 사본(D-114 단계 2.5) — 이 줄은 박동 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def ledger_row(name: str, ok: bool, level: str, detail: str):
        if ledger_foreign < 0:
            return (name, True, level, "사본 수치 줄 없음(D-114 단계 2.5 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 제어 요청(D-114 단계 2.5 갈래 B) — 이 줄도 사본 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def control_row(name: str, ok: bool, level: str, detail: str):
        if control_discarded < 0:
            return (name, True, level, "제어 요청 수치 줄 없음(D-114 단계 2.5 갈래 B 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 체결통보 세션(D-114 단계 3) — 이 줄도 사본 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def fill_session_row(name: str, ok: bool, level: str, detail: str):
        if fill_session_socket < 0 and fill_session_none == 0 and fill_session_many == 0:
            return (name, True, level, "체결통보 세션 줄 없음(D-114 단계 3 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    rows = [
        # 사망 판정이 한 번이라도 났으면 그날 그만큼 신규 진입이 막혔다. 공백 문턱은 부하 실측 24ms 위의 1,000ms다.
        channel_row("전략 박동", beat_dead == 0, "FAIL",
                    f"사망 판정 {beat_dead}회 · 복귀 {beat_back}회 · 가장 긴 공백 {beat_gap_max}ms"
                    f" (기대 0회, 사망 문턱 {BEAT_DEAD_MS}ms — 판정이 나면 그 사이 신규 매수가 막힌다)"),
        # 문턱 아래여도 의심 문턱을 넘은 날은 전략 스레드가 한 바퀴에 오래 붙들린 것이라 미리 본다.
        channel_row("전략 박동 여유", beat_gap_max <= BEAT_SUSPECT_MS, "WARN",
                    f"가장 긴 공백 {beat_gap_max}ms (의심 문턱 {BEAT_SUSPECT_MS}ms, 부하 하네스 실측 24ms)"),
        # 포트를 못 잡은 엔진은 매매는 하면서 적재만 안 한다 — 로그에 ERROR 한 줄뿐이라 놓치기 쉽다.
        ("ZMQ 포트", zmq_bind_fail == 0, "FAIL",
                    f"bind 실패 {zmq_bind_fail}회 (기대 0 — 실패하면 그 엔진의 체결·시그널이"
                    f" TimescaleDB 에 하나도 안 들어간다. 계좌를 둘 돌리면 포트를 갈라야 한다: D-122)"),
        # 통로가 새면 같은 주문이 두 번 가거나 전략이 답을 영영 못 받아 기다림 표가 샌다.
        channel_row("주문 통로 무결", order_duplicate == 0 and order_response_dropped == 0, "FAIL",
                    f"중복 거름 {order_duplicate}건 · 버린 응답 {order_response_dropped}건 (둘 다 기대 0)"),
        # 사본은 한 계좌만 담는다(계좌당 프로세스). 남의 계좌 줄이 세어지면 전략이 보는 장부가 원장과 다르다.
        ledger_row("장부 사본 어긋남", ledger_foreign == 0, "FAIL",
                   f"다른 계좌 줄 {ledger_foreign}건 · 낸 판 {ledger_gen}판 (어긋남 기대 0)"),
        # 전략은 이제 장부가 아니라 사본을 본다 — 판이 안 늘면 보유·여력이 굳어 같은 종목을 또 산다(A등급).
        #  주문이 없는 회차에도 주문 스레드가 100ms마다 한 판씩 내므로, 고수위 줄 사이에 0판은 멈춘 것이다.
        ledger_row("장부 사본 갱신", not ledger_stall_at, "FAIL",
                   f"판이 안 늘어난 구간 {len(ledger_stall_at)}곳 · 낸 판 {ledger_gen}판 (기대 0곳)"
                   + (f" — {', '.join(hhmm(second) for second in ledger_stall_at[:5])}" if ledger_stall_at else "")),
        # 슬롯 면제 집합·진입 우선순위 표는 전략이 여러 줄로 보내고 주문 쪽이 모아서 건다. 한 줄이라도 새면
        #  그 표는 통째로 안 걸린다 — 면제가 빠진 바스켓 보유분이 남의 슬롯을 먹고, 랭크를 잃은 종목이
        #  우선순위 바를 건너뛴다. 둘 다 0이어야 전략이 고친 표가 그날 실제로 걸린 것이다.
        control_row("제어 요청 표", control_dropped == 0 and control_discarded == 0, "FAIL",
                    f"못 보낸 줄 {control_dropped}건 · 버린 줄 {control_discarded}건 (둘 다 기대 0)"),
        # 체결통보는 WS 세션 하나만 들어야 한다. 아무도 안 들으면 체결이 원장에 안 들어와 선점이 안 풀리고,
        #  둘이 들으면 KIS가 세션마다 같은 통보를 보내 원장이 체결을 두 번 센다 — 둘 다 A등급이다.
        fill_session_row("체결 세션", fill_session_many == 0 and fill_session_none == 0, "FAIL",
                         (f"소켓 {fill_session_socket}번이 맡는다" if fill_session_socket >= 0 else "맡은 소켓 없음")
                         + f" · 맡은 곳 없음 {fill_session_none}회 · 둘 이상 {fill_session_many}회 (둘 다 기대 0)"),
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
        ("일찍 끝난 세션", not short, "FAIL",
         f"{MIN_SESSION_SEC}초 미만 종료 {len(short)}회"
         + (f" — {', '.join(hhmm(second) for second in short[:5])}" if short else "")),
        ("재기동 투매", not dump, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 본전탈출 {len(dump)}건"
         + (f" — {', '.join(hhmm(second) for second in dump[:5])}" if dump else "")),
        ("매도→재매수 회전", churn <= MAX_CHURN, "FAIL",
         f"{CHURN_SEC}초 내 반대매매 {churn}회 (허용 {MAX_CHURN})"),
        ("엔진밖 미체결", untracked_opens == 0, "FAIL",
         f"부속 파일에 없던 브로커 미체결 {untracked_opens}건"
         " — 전송 타임아웃 난 주문이 실제로는 접수돼 엔진 장부 밖에 살아 있었다는 뜻이다."
         " 그 종목은 보유분이 묶여 손절이 닿아도 못 판다(2026-09-23 09:26 021240)"),
        ("청산차단 해소", blocked_sells == 0, "FAIL",
         f"청산차단 미해소 {blocked_sells}건 — 예약매도를 못 찾아 청산이 막힌 채 넘어갔다"),
        ("초당한도 압박", rate_hits <= MAX_RATE_HITS, "WARN",
         f"초당 거래건수 거부 {rate_hits}건 (허용 {MAX_RATE_HITS})"),
        ("HTTP 연결 재사용", curl_giveups <= MAX_CURL_GIVEUPS, "WARN",
         f"제한 시간 초과로 버린 요청 {curl_giveups}건 (허용 {MAX_CURL_GIVEUPS}, 09-22 21건)"
         " — 넘으면 스레드별 상주 핸들이 안 살아 매 요청이 TCP+TLS를 다시 맺는 것"),
        ("초당한도 되보냄", rate_retries <= MAX_RATE_RETRIES, "WARN",
         f"초당 한도로 되보낸 요청 {rate_retries}건 (허용 {MAX_RATE_RETRIES}, 09-22 37건)"
         " — 넘으면 스캐너 종목 간 간격이 계좌 한도보다 촘촘한 것"),
        ("기준자본 조회", equity_fails <= MAX_EQUITY_FAILS, "WARN",
         f"프리페치 기준자본 조회 실패 {equity_fails}건 (허용 {MAX_EQUITY_FAILS})"
         " — 실패는 60초 쿨다운이라 이보다 많으면 장 내내 실패한 것이고, 그날 사이징은 폴백 자본으로 갔다"),
        ("보수 정지 누적", b2_minutes == 0, "WARN",
         f"BUY NEW 보수 정지(B2) 누적 {b2_minutes}분 / {len(b2_on)}회 (기대 0 — 잔고조회가 끊기면 그 시간만큼 신규 매수가 없다)"
         + (f" — {', '.join(hhmm(second) for second in b2_on[:5])}" if b2_on else "")),
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
        order_latency_breakdown_row(date),
        market_open_gate_row(date),
        tsan_row(date),
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
