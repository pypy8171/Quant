# -*- coding: utf-8 -*-
"""장중·마감 후 실행 로그에서 '이미 한 번 당한' 실패 유형이 다시 났는지 기계적으로 본다.

사람이 로그를 눈으로 훑어 찾아낸 것만 여기 들어온다. 새 항목을 넣을 때는 그 항목이
실제로 하루를 망친 적이 있어야 한다 — 가정으로 만든 체크는 경보만 늘리고 신뢰를 깎는다.

사용:  py scripts/check_runtime_health.py [--date YYYY-MM-DD] [--log <경로>]
종료코드: 0 = FAIL 없음, 1 = FAIL 있음
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import datetime as dt
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
# 전 종목 시세가 10분 넘게 안 바뀌면 엔진이 재스캔마다 이 경고를 찍는다 — 그동안 정배열·이격 판정이
#  전일 종가로 얼어붙는 가장 조용한 실패다(UniverseQuotes.cpp). 시세는 엔진 안 시세판(MarketBoard, 5초)이
#  받는다. 이 줄은 0건이어야 정상이다.
PRICES_STALE_RE = re.compile(r"전 종목 시세가 (\d+)초 지났다")
# 시세판 한 바퀴(5초)의 모든 요청이 빈 본문으로 끝난 줄. 몇 번은 네이버 쪽 일시 오류지만 1분치(12번)를
#  넘으면 판이 멈춘 것이다. 재랭킹 보류는 장 시작 직후(누적 거래대금이 비어 가는 동안)에만 정상이다. [why D-147]
BOARD_SWEEP_FAIL_RE = re.compile(r"\[MarketBoard\] 시세 한 바퀴 전부 실패")
BOARD_RERANK_HOLD_RE = re.compile(r"\[MarketBoard\] 재랭킹 보류")
MAX_BOARD_SWEEP_FAILS = 12
BOARD_RERANK_HOLD_UNTIL = 9 * 3600 + 10 * 60   # 09:10 뒤의 보류는 이상
# 장 전 일봉 캐시 데우기(D-147). 끝 줄에 "마감에 멈춤"이 붙거나 목록·인증 실패로 건너뛰면 08:00 첫 스캔이
#  후보 일봉을 그 자리에서 받느라 늦어진다. 마감 뒤 기동(장중 재기동)의 건너뜀은 정상이라 세지 않는다.
DAILY_WARM_DONE_RE = re.compile(r"\[DailyWarm\] 장 전 일봉 캐시 데우기 끝 — (\d+)/(\d+)종목, (\d+)초( \(마감에 멈춤)?")
DAILY_WARM_FAIL_RE = re.compile(r"\[DailyWarm\] .*(2분 안에 못 받아|인증 실패)")
# 거래대금 랭킹: 축·ETF드롭·생존 행수. ETF드롭이 0이 아니면 API단 제외 마스크가 안 먹는 것이다.
VALUE_RANK_DIAG_RE = re.compile(r"거래대금랭킹 진단\(축=(\d).*?ETF드롭=(\d+).*?생존=(\d+)")
VALUE_RANK_DONE_RE = re.compile(r"거래대금 랭킹 조회 완료: (\d+)종목 \(요청 count=(\d+)\)")
# 시세 표 거래대금 상위 축(D-146): 재스캔마다 몇 종목을 뽑았나. 시세 파일이 비거나 낡으면 0으로 떨어진다.
TURNOVER_AXIS_RE = re.compile(r"DEVSCALE 거래대금 상위 축: 상위 (\d+)종목")
TURNOVER_AXIS_MIN = 100   # 가격·거래대금 하한을 넘는 종목이 09-25 마감 기준 632개라 200을 다 채우는 게 보통이다
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
# 주문구분이 그 시장·그 시각에 안 받는 값이라 되돌아온 거부. APBK1943 = 최유리지정가호가불가 —
#  KRX 애프터마켓(16:00~20:00)이 최유리지정가를 안 받는데 정규장 밖 시장가를 그것으로 보내던 배선이
#  2026-09-23 실계좌에서 12건 되돌아왔다. 지금은 지정가(00)+현재가로 보낸다.
ORDER_DIVISION_REJECT_RE = re.compile(r"\[OrderRouter\] KIS 거부 .*\[APBK1943\]")
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
# 보호 주문 표(D-114 단계 1)는 기본이 shadow 다 — 표는 판정만 남기고 발주는 전략이 하던 대로 한다.
#  owner 로 올리려면 표의 판정과 전략의 실제 청산이 같은 자리에서 나는지를 며칠 봐야 한다(D-114 "남은 것").
#  그림자: "[보호주문] 그림자 판정 005930 손절(평단 70000.0 -6.5%) 보유=10 (발주는 전략이 한다)"
PROTECTIVE_SHADOW_RE = re.compile(r"\[보호주문\] 그림자 판정 (\d{6}) (손절|트레일)\(")
#  owner 로 올린 날은 표가 직접 낸다. 그 줄이 있으면 맞댈 짝이 없으므로 대조를 접는다.
PROTECTIVE_FIRED_RE = re.compile(r"\[보호주문\] 청산 (\d{6}) (손절|트레일)\(")
#  전략 쪽 청산. 표에 규칙을 거는 전략은 지금 DeviationScale 하나뿐이라 그것만 맞댄다(D-114 "남은 것").
#  손절은 DEVSCALE_STOP_RE 가 이미 모으고 있어 트레일만 더한다 — 표가 보는 것도 이 둘뿐이다.
DEVSCALE_TRAIL_RE = re.compile(r"신호: \[DEVSCALE_(\d{6})\] \d{6}.* SELL \d+ \| 근거: 청산:트레일\(")
# 같은 종목의 그림자 판정과 전략 청산을 한 자리로 볼 시간 폭(초). 표의 재발주 간격이 30초이고
#  (protective_orders_retry_ms 기본값) 전략 쪽 청산 백오프 상한도 30초라 둘을 더한 만큼 벌려 둔다.
PROTECTIVE_MATCH_SEC = 60

# 임계값. 넘으면 그날 운영이 실제로 상했던 수준이다.
MAX_STALE_ORDERS = 25     # 유령주문 재부활 — 취소 왕복이 초당한도를 밀어낸다
MIN_SESSION_SEC = 30      # 이보다 짧게 죽으면 배선/바이너리 문제(정상 재기동 아님)
# 갈라 띄운 날(D-114)에는 주문·전략·시세 프로세스가 같은 기동에서 엔진 시작 줄을 각각 찍는다.
#  두 줄 사이가 이 안이면 한 번의 기동으로 센다(09-23 실측 1.1초 — 설정 로드·유니버스 스캔에 걸린 시간 차이다).
SAME_BOOT_SEC = 60
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
LEDGER_UNNUMBERED_RE = re.compile(r"\[OrderRouter\] 재기동 미결 주문 짝 ")
LEDGER_RESOLVE_RE = re.compile(r"\[Engine\] 원장 미결 주문 대조: 되살림 (\d+)건 · 선점해제 (\d+)건 · 저널기록실패 (\d+)건")
# 주문 앞 선기록(OrderRouter)과 잠금 밖 묶음 기록(PositionLedger, W-2) 실패를 같이 센다.
LEDGER_WRITE_FAIL_RE = re.compile(r"\[(?:OrderRouter|PositionLedger)\] 원장 저널 기록 실패")
# 엔진 DB 관리자(D-148) — 시작 줄이 있으면 켜진 것, 종료 줄에 받은·넣은·버린 행 수가 있다.
DB_WRITER_START_RE = re.compile(r"\[DbManager\] 시작 — ")
DB_WRITER_NO_PASSWORD_RE = re.compile(r"\[DbManager\] TSDB_PASSWORD 환경변수가 없다")
DB_WRITER_END_RE = re.compile(r"\[DbManager\] 종료 — 받음 (\d+), 넣음 (\d+), 큐 넘쳐 버림 (\d+), 거절 (\d+), 모호 (\d+)")
# 하루 리셋은 거래일당 한 번이다 — 같은 날짜로 두 번 찍히면 재기동이 되살린 선점·당일 손익을 지운 것이다(전수조사 A-4)
DAILY_RESET_RE = re.compile(r"\[OrderGate\] 하루 리셋 - 거래일\((\d{8})\)")
# 미체결 조회가 실패하면 재기동 대조는 접수된 주문을 살려 두고 넘어간다 — 잦으면 선점 대조가 그날 안 된 것이다(전수조사 B1-2)
OPEN_ORDER_FAIL_RE = re.compile(r"\[OrderRouter\] (?:재기동 미체결 조회 실패|미체결 조회 실패|미체결 보충 조회 실패|전송 타임아웃 되묻기 — 미체결 조회 실패)")
# 엔진이 모르는 채 브로커에 살아 있던 주문 — 전송이 타임아웃 나면 KIS에는 접수됐는데 ODNO를 못 받아
#  부속 파일에 못 적는다. 그 주문이 보유분을 묶으면 손절이 닿아도 못 판다(2026-09-23 09:26 021240,
#  ODNO=0000007886 매도 18주). 기동 때 브로커 조회로 보충하면 이 줄이 남는다 — 남았다는 건 그날 샜다는 뜻이다.
UNTRACKED_OPEN_RE = re.compile(r"부속 파일에 없는 미체결 — 브로커 조회로 보충 (\d{6})")
# 청산이 막혔는데 풀지 못한 채 넘어간 것. 위와 같은 뿌리이나 이쪽은 재기동 전까지 방치된다.
BLOCKED_SELL_RE = re.compile(r"청산차단 미해소 (\d{6})")
# 전략 사망 마무리(D-114 단계 2) — 주문 스레드가 전략 박동 공백만 보고 낸 판정.
BEAT_DEAD_RE = re.compile(r"\[마무리\] 전략 박동이 끊겼다")
BEAT_BACK_RE = re.compile(r"\[마무리\] 전략 박동이 돌아왔다")
# [큐 고수위] 줄 꼬리 — 없으면 D-114 배포 전 바이너리라 이 세 행을 판정하지 않는다.
BEAT_GAP_RE = re.compile(r"(?<![a-z_])beat_gap_max=(\d+)ms")
# 시세 사망 마무리(D-137) — 주문 스레드가 시세 박동 공백만 보고 낸 판정.
FEED_DEAD_RE = re.compile(r"\[마무리\] 시세 박동이 끊겼다")
FEED_BACK_RE = re.compile(r"\[마무리\] 시세 박동이 돌아왔다")
FEED_BEAT_GAP_RE = re.compile(r"feed_beat_gap_max=(\d+)ms")
ORDER_DUPLICATE_RE = re.compile(r"order_duplicate=(\d+)")
ORDER_RESPONSE_DROP_RE = re.compile(r"order_response_dropped=(\d+)")
# 요청 면(D-114 단계 4) — 값이 말이 안 돼 버린 요청 수, 판단 근거가 칸을 넘어 잘린 신호 수.
ORDER_IMPLAUSIBLE_RE = re.compile(r"order_implausible=(\d+)")
ORDER_TRUNCATED_RE = re.compile(r"order_truncated=(\d+)")
# 장부 사본(D-114 단계 2.5) — 낸 판 수와, 한 계좌만 담는 사본에 못 실은 남의 계좌 줄 수.
LEDGER_GEN_RE = re.compile(r"ledger_gen=(\d+)")
LEDGER_FOREIGN_RE = re.compile(r"ledger_foreign=(\d+)")
# 제어 요청(D-114 단계 2.5 갈래 B) — 전략이 큐가 가득 차 못 보낸 줄 수, 주문 쪽이 반쪽 표로 보고 버린 줄 수.
CONTROL_DROP_RE = re.compile(r"control_dropped=(\d+)")
# 체결통보 큐(D-056) — [큐 고수위] 줄의 최고 수위·버린 건수와, 잔고 대조가 메운 줄(CODE_REVIEW W-1).
FILL_QUEUE_RE = re.compile(r" fill=(\d+)/(\d+) fill_dropped=(\d+)(?: fill_overflowed=(\d+))?")
FILL_ABSORB_RE = re.compile(r"놓친 (매수|매도) 체결로 보고")
CONTROL_RELAY_DROP_RE = re.compile(r"control_relay_dropped=(\d+)")
CONTROL_DISCARD_RE = re.compile(r"control_discarded=(\d+)")
# 티커→번호(D-114 단계 4) — 등록을 주문 쪽에서 못 받은 수, 표에 없는 티커로 잦은 자리가 불린 수.
SYMBOL_REGISTER_TIMEOUT_RE = re.compile(r"symbol_register_timeout=(\d+)")
SYMBOL_LOOKUP_MISS_RE = re.compile(r"symbol_lookup_miss=(\d+)")
# 전략 이름→번호(D-114 단계 4) — 이름표 등록을 주문 쪽에서 못 받은 수.
STRATEGY_REGISTER_TIMEOUT_RE = re.compile(r"strategy_register_timeout=(\d+)")
WATCH_OVERFLOW_RE = re.compile(r"watch_overflow=(\d+)")
# 구독 칸 우선순위 배정(D-132). 넘침 종목이 REST 대체조차 없으면 틱이 아예 안 온다.
WATCH_NO_REST_RE = re.compile(r"WS 구독 상한 — \S+ 는 REST 대체가 아직 없어")
WS_SLOT_RELEASE_RE = re.compile(r"\[WS칸\] 칸 내줌 — ")
WS_SLOT_PROTECTED_OFF_RE = re.compile(r"\[WS칸\] 보유·선점 종목이 칸 밖 — (\S+)")
WS_SLOT_RELEASE_LIMIT = 200  # 하루 칸 내줌 횟수 문턱 — 넘으면 교체가 잦다
# 시세 통로(D-114 단계 4 배선 2') — 큐가 차서 못 넘긴 건수, 꺼낸 값이 말이 안 돼 버린 건수.
FEED_CHANNEL_OVERFLOW_RE = re.compile(r"feed_channel_overflow=(\d+)")
FEED_CHANNEL_DISCARD_RE = re.compile(r"feed_channel_discarded=(\d+)")
# 통로를 지나간 건수(D-114 단계 5) — 보낸 쪽과 받은 쪽을 따로 센다. 갈라 띄운 날에 둘 다 0이면
#  통로가 붙지 않은 것이라, 버린 건수가 0이어도 그날 시세는 경계를 넘지 못했다.
FEED_CHANNEL_SENT_RE = re.compile(r"feed_channel_sent=(\d+)")
FEED_CHANNEL_RECEIVED_RE = re.compile(r"feed_channel_received=(\d+)")
# 체결 통로(D-114 단계 5) — 체결통보는 시세 소켓에 실려 오므로 시세 프로세스가 받아 주문 쪽으로 넘긴다.
#  이 길이 끊기면 주문 쪽 선점분이 안 풀려 총노출을 이중계상하고 원장에 체결이 안 실린다.
FILL_CHANNEL_SENT_RE = re.compile(r"fill_channel_sent=(\d+)")
FILL_CHANNEL_RECEIVED_RE = re.compile(r"fill_channel_received=(\d+)")
# 체결통보 세션(D-114 단계 3) — 기동마다 한 줄. 맡은 소켓이 몇 번인지, 아무도 안 맡았는지, 둘 이상이 맡았는지.
FILL_SESSION_ONE_RE = re.compile(r"\[Engine\] 체결통보 세션: 소켓 (\d+)")
FILL_SESSION_NONE_RE = re.compile(r"\[Engine\] 체결통보 세션: 없음")
FILL_SESSION_MANY_RE = re.compile(r"\[Engine\] 체결통보 세션: (\d+)개")
# 재연결 뒤 같은 체결통보가 다시 와서 원장에 안 넣은 줄(CODE_REVIEW C-1). 수량은 잔고 대조가 맞춘다.
FILL_REPLAY_RE = re.compile(r"\[OrderRouter\] 재연결 뒤 같은 체결통보")
FILL_PRODUCER_OVERLAP_RE = re.compile(r"\[Engine\] 체결통보 생산자 겹침")
# 공유 쪽지 종료 사유(D-114) — 짝이 사유를 적고 나간 것을 보고 따라 내려간 줄에 그 번호가 실린다.
PEER_EXIT_REASON_RE = re.compile(r"건너편이 종료 사유를 적고 나갔다\(사유 번호 (\d+)\)")
# Quant/include/ipc/SharedRegion.h 의 SharedShutdownReason 중 기동하다 접은 값. 0 은 적기 전에 죽은 것이고,
#  1(장 마감 자기 종료)·2(사람·감시견이 내렸다, 배포 교체 포함)는 정상이다.
SHUTDOWN_REASON_STARTUP_FAIL = 3
# ZMQ PUB/REP 포트를 먼저 뜬 엔진이 잡고 있으면 나중에 뜬 쪽은 bind 에 실패한 뒤 ERROR 한 줄만 남기고
#  계속 돈다 — 체결·시그널이 TimescaleDB 에 하나도 안 들어간 채로 매매한다. 계좌를 둘 돌리는 날의
#  가장 조용한 실패라 판정 행으로 둔다(실계좌는 5565/5566 으로 옮겨 놨다). [why D-122]
ZMQ_BIND_FAIL_RE = re.compile(r"\[ZMQ\] 소켓 bind 실패")

BASKET_BUY_LEG_DEADLINE = 15 * 3600 + 5 * 60  # 매수 레그는 15:05까지 끝나야 마감 청산(15:15)과 겹치지 않는다(D-109)


# 전략 박동 문턱 — Quant/include/ipc/Heartbeat.h의 HeartbeatConfig 기본값과 같은 값이다(D-114 단계 2).
BEAT_SUSPECT_MS = 250
BEAT_DEAD_MS = 1000

# 시세 박동 문턱 — 전략·주문 칸과 다르다. 이 칸을 찍는 자리가 5초마다 도는 제어 바퀴라(hot loop 가 아니다)
#  그 간격 위에서 잡고, 2,700종목 기동이 종목 번호를 다 받기까지 걸린 27초 실측보다 넉넉히 위에 둔다.
#  좁히는 근거는 HEALTH 줄의 feed_beat_gap_max 누적이다(D-137).
FEED_BEAT_SUSPECT_MS = 30_000
FEED_BEAT_DEAD_MS = 90_000

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
             + (" — 수집기가 멈췄다 되살아난 구간이다. logs/procwatch.log를 본다" if float(max_gap_seconds) > 120 else ""))]


def feed_ledger_rows(date: str) -> list:
    """리코더가 그날 적재한 체결 틱과 원장 계좌를 본다 — 09-22 실측한 두 가지를 다시 겪지 않기 위한 행.

    ① 체결 틱이 ticks 표에 안 들어가면 그라파나 "피드 지연"·"초당 틱 유입" 패널이 며칠 전 시각을
       가리킨다(피드는 멀쩡한데 화면만 죽는다).
    ② Engine을 그대로 띄우는 테스트·부하 하네스가 같은 ZMQ 포트(5555)에 bind하면 리코더가 그쪽을
       잡는다. 09-22 장중에 합성 주문 52건·체결 58건이 계좌 없이 운영 표에 들어갔다. 주문·체결은 이제
       원장 저널에서 옮기므로(journal 열) 저널마다 계좌가 하나인지 본다 — 모의·실계좌가 같은 날 돌면
       표 전체로는 계좌가 둘인 것이 정상이다. journal이 빈 행은 ZMQ로 받던 옛 행이다.
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
            # 체결 건수는 시세 프로세스가 채운다 — 갈라 뜬 날은 role='feed' 행에 있다 [why D-129]
            #  계좌도 가른다. 모의와 실계좌가 같은 DB 에 쌓여서, 안 가르면 한쪽이 0 이어도
            #  다른 쪽 수치에 가려 보이지 않는다 [why D-129]
            cursor.execute(
                "SELECT COALESCE(NULLIF(TRIM(account), ''), '(빈칸)'), COALESCE(MAX(data_cnt), 0)"
                " FROM health WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s"
                " AND role IN ('both','order','feed') GROUP BY 1", (date,))
            data_by_account = dict(cursor.fetchall())
            data_count = min(data_by_account.values()) if data_by_account else 0
            accounts: dict[str, dict[str, int]] = {}

            for table in ("orders", "fills"):
                cursor.execute(
                    f"SELECT COALESCE(journal, '(ZMQ)'), COALESCE(NULLIF(TRIM(account), ''), '(빈칸)'), COUNT(*)"
                    f" FROM {table} WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s GROUP BY 1, 2", (date,))

                for journal, account, count in cursor.fetchall():
                    by_account = accounts.setdefault(journal, {})
                    by_account[account] = by_account.get(account, 0) + count

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
                f"ticks {tick_count}행·{tick_tickers}종목, HEALTH data 계좌별 "
                + (", ".join(f"{name} {value}" for name, value in sorted(data_by_account.items())) or "행 없음")
                + (" — 0인 계좌가 있으면 그 엔진이 WS 경로 계수 배포 전 바이너리다" if data_count == 0 else ""))
    # 한 저널 = 한 엔진 = 한 계좌다. 한 저널에 계좌가 둘이면 두 엔진이 같은 ledger_journal_dir을 쓴 것
    mixed = [journal for journal, by_account in accounts.items() if len(by_account) > 1]
    ledger_row = ("원장 계좌 단일", not mixed, "FAIL",
                  "주문·체결 계좌 " + ("; ".join(
                      f"{journal}: " + ", ".join(f"{name} {count}건" for name, count in sorted(by_account.items()))
                      for journal, by_account in sorted(accounts.items())) or "행 없음")
                  + (f" — {', '.join(mixed)}에 계좌가 둘 이상이다. 두 엔진이 같은 저널 폴더(ledger_journal_dir)를"
                     " 쓰거나, (ZMQ)면 다른 엔진이 같은 ZMQ 포트를 물었다" if mixed else ""))

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
                "SELECT COALESCE(NULLIF(TRIM(account), ''), '(빈칸)'),"
                " COUNT(*) FILTER (WHERE queue_shard_capacity IS NOT NULL),"
                " COALESCE(MAX(dropped_shard), 0) + COALESCE(MAX(dropped_order), 0)"
                " + COALESCE(MAX(dropped_fill), 0),"
                " COALESCE(MAX(100.0 * queue_shard_high_water / NULLIF(queue_shard_capacity, 0)), 0),"
                " COALESCE(MAX(100.0 * queue_order_high_water / NULLIF(queue_order_capacity, 0)), 0),"
                " COALESCE(MAX(total_p99_us), -1)"
                # 샤드 큐는 전략, 주문·체결 큐와 지연은 주문 프로세스가 채운다 [why D-129]
                #  계좌로도 가른다 — MAX 를 계좌까지 합쳐 잡으면 버린 계좌가 안 버린 계좌 수치에 묻힌다
                " FROM health WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s"
                " AND role IN ('both','order','strategy') GROUP BY 1", (date,))
            by_account = {account: (metric_rows, dropped, shard_percent, order_percent, total_p99)
                          for account, metric_rows, dropped, shard_percent, order_percent, total_p99
                          in cursor.fetchall()}

        connection.close()
    except psycopg2.errors.UndefinedColumn:   # 열을 아직 안 만든 DB — 새 적재기가 첫 HEALTH에서 만든다
        return ("큐·지연 적재", True, "WARN", "health 표에 큐·지연 열 없음 — 적재기 배포 전")
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return ("큐·지연 적재", True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})")

    if not any(metric_rows for metric_rows, _, _, _, _ in by_account.values()):
        return ("큐·지연 적재", True, "WARN", "HEALTH에 큐·지연 수치 없음 — 그 수치를 안 싣는 옛 exe")

    detail = " / ".join(
        f"{account}: HEALTH {metric_rows}건, 버린 건수 {dropped} (기대 0),"
        f" 고수위 샤드 {float(shard_percent):.1f}%·주문 큐 {float(order_percent):.1f}%, 전체 지연 p99 "
        + (f"{total_p99 / 1000:.0f}ms" if total_p99 >= 0 else "표본 없음")
        for account, (metric_rows, dropped, shard_percent, order_percent, total_p99)
        in sorted(by_account.items()))

    dropped_total = sum(dropped for _, dropped, _, _, _ in by_account.values())

    return ("큐·지연 적재", dropped_total == 0, "FAIL", detail)


def role_publish_verdict(by_role: dict) -> tuple:
    """계좌 하나의 역할별 HEALTH 집계를 보고 (통과 여부, 문구) 를 낸다."""
    if set(by_role) <= {"both"}:
        count, data, signal, order = by_role["both"]
        return (True, f"한 프로세스(both)로 떴다 — HEALTH {count}건, 체결 {data}·신호 {signal}·주문 {order}")

    missing = [role for role in ("order", "strategy", "feed") if role not in by_role]

    if missing:
        return (False, f"갈라 떴는데 {'·'.join(missing)} 역할이 HEALTH를 한 건도 안 냈다"
                       f" — 그 프로세스의 PUB 포트 설정이나 zmq_enabled를 본다"
                       f" (있는 역할: {'·'.join(sorted(by_role))})")

    empty = []

    if by_role["feed"][1] == 0:
        empty.append("시세 프로세스 체결 건수 0")

    if by_role["order"][3] == 0:
        empty.append("주문 프로세스 주문 건수 0")

    if empty:
        return (False, f"세 역할이 다 HEALTH는 냈으나 {', '.join(empty)}")

    return (True, f"세 역할이 각자 발행 — 시세 체결 {by_role['feed'][1]},"
                  f" 전략 신호 {by_role['strategy'][2]}, 주문 {by_role['order'][3]}")


def role_publish_row(date: str) -> tuple:
    """갈라 띄운 날 세 역할이 각자 제 포트로 발행했는지 본다.

    D-114로 프로세스를 셋으로 가른 뒤, 발행 채널이 주문 쪽 하나뿐이라 갈라 띄운 날에는 틱도 신호도
    아무 데도 안 나갔다. D-129에서 역할마다 PUB 포트를 하나씩 두어 걷었는데, 설정이 어긋나 다시
    한 쪽만 발행하게 되면 화면은 그냥 조용해서 눈으로는 안 보인다 — 그래서 여기서 판정한다.
    한 프로세스(both)로 뜬 날은 가를 것이 없으므로 그대로 통과시킨다.
    """
    password = tsdb_password()

    if not password:
        return ("역할별 발행", True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return ("역할별 발행", False, "WARN",
                "psycopg2 없음 — venv(PYQuant/.venv*)로 부르거나 pip install psycopg2-binary")

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            cursor.execute(
                # 계좌까지 가른다 — 한 계좌가 셋 다 냈다는 것만으로 다른 계좌가 한 역할밖에
                #  안 낸 것이 가려진다 (모의·실계좌가 같은 DB 에 쌓인다) [why D-129]
                "SELECT COALESCE(NULLIF(TRIM(account), ''), '(빈칸)'), role, COUNT(*),"
                " COALESCE(MAX(data_cnt), 0), COALESCE(MAX(signal_cnt), 0),"
                " COALESCE(MAX(order_cnt), 0) FROM health"
                " WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s GROUP BY 1, 2", (date,))
            by_account = {}

            for account, role, count, data, signal, order in cursor.fetchall():
                by_account.setdefault(account, {})[role] = (count, data, signal, order)

        connection.close()
    except psycopg2.errors.UndefinedColumn:   # role 열이 아직 없는 DB — 새 적재기가 첫 HEALTH에서 만든다
        return ("역할별 발행", True, "WARN", "health 표에 role 열 없음 — 적재기 배포 전")
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return ("역할별 발행", True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})")

    if not by_account:
        return ("역할별 발행", True, "WARN", "그날 HEALTH 행이 없음 — 엔진이 안 떴거나 발행이 꺼졌다")

    verdicts = [(account, role_publish_verdict(by_role)) for account, by_role in sorted(by_account.items())]

    return ("역할별 발행", all(passed for _, (passed, _) in verdicts), "FAIL",
            " / ".join(f"{account}: {reason}" for account, (_, reason) in verdicts))


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


# 애프터마켓 주문을 되돌려보낸 KIS 오류. 주문구분·거래소를 잘못 실으면 이 코드로 온다.
#   APBK1943 최유리지정가호가불가 — 애프터마켓은 최유리·최우선을 안 받는다
#   APBK3009 SOR 시장에서 거래가 불가능한 종목 — 애프터마켓은 거래소를 KRX 로 못박아야 한다
AFTER_MARKET_REJECT_CODES = ("APBK1943", "APBK3009")
AFTER_MARKET_OPEN_HHMM = "16:00"
AFTER_MARKET_CLOSE_HHMM = "20:00"


# 스캔 로그 한 줄에서 "이름=숫자" 를 전부 뽑는다. 데이터부족 은 "데이터부족(<60봉)=4" 처럼
#  괄호가 끼어 있어 이름과 = 사이를 건너뛴다.
SCAN_COUNTER_PATTERN = re.compile(r"([가-힣]+)(?:\([^)]*\))?=(\d+)")


def after_market_order_row(date: str) -> tuple:
    """애프터마켓(16:00~20:00) 주문이 주문구분·거래소 때문에 되돌아왔는지.

    모의계좌는 애프터마켓 주문 자체를 받지 않아, 이 경로는 실계좌에서만 드러난다. 2026-09-23
    실계좌 첫날 청산 주문 12건이 이 자리에서 전부 거부됐다 — 최유리지정가(03)로 8건, 거래소를
    SOR 로 둔 지정가(00)로 4건. 보유분이 청산되지 않고 다음 날로 이월된다. [why D-097]
    원장은 한 곳이 아니다 — 실계좌·모의·리눅스 빌드가 저마다 폴더를 쓰므로 전부 훑는다.
    """
    name = "애프터마켓 주문구분"
    date_compact = date.replace("-", "")
    rejects: list[str] = []
    after_market_orders = 0

    for ledger in sorted(REPO.glob(f"Quant/build*/logs*/trades_{date_compact}.csv")):
        try:
            with ledger.open(encoding="utf-8", errors="replace", newline="") as handle:
                for record in csv.DictReader(handle):
                    stamp = (record.get("ts_kst") or "")[11:16]

                    if not AFTER_MARKET_OPEN_HHMM <= stamp < AFTER_MARKET_CLOSE_HHMM:
                        continue

                    after_market_orders += 1
                    reason = record.get("reason") or ""

                    if any(code in reason for code in AFTER_MARKET_REJECT_CODES):
                        rejects.append(f"{stamp} {record.get('ticker', '')} [{ledger.parent.name}]")
        except OSError:
            continue

    if not after_market_orders:
        return (name, True, "WARN", "애프터마켓 시간대 주문이 없다 — 판정 안 함")

    if rejects:
        return (name, False, "FAIL",
                f"애프터마켓 주문 {after_market_orders}건 중 주문구분·거래소 거부 {len(rejects)}건"
                f" — {', '.join(rejects[:3])}"
                " (16:00~20:00 은 주문구분 41 + 거래소 KRX 여야 한다, D-097)")

    return (name, True, "FAIL", f"애프터마켓 주문 {after_market_orders}건, 주문구분·거래소 거부 0건")


def restart_verify_row(date: str) -> tuple:
    """그날 배포 재기동이 기동 단계를 다 찍었는지.

    scripts/deploy_trader.py 가 트레이더를 내리고 감시견이 다시 띄우면, scripts/restart_verify.py 가 로그의
    기동 표지(FEP 초기화 → 모든 스레드 시작, 그 뒤 20초 생존)로 판정해 _private/state/restart_verify.jsonl 에
    한 줄 남긴다. 떠 있기만 하고 기동을 못 끝낸 재기동을 사람이 로그를 열어 보지 않아도 잡으려는 행이다.
    """
    name = "재기동 기동 판정"
    path = REPO / "_private" / "state" / "restart_verify.jsonl"
    rows: list[dict] = []

    try:
        with path.open(encoding="utf-8") as handle:
            for line in handle:
                if line.startswith('{"time": "' + date):
                    rows.append(json.loads(line))
    except (OSError, ValueError):
        pass

    if not rows:
        return (name, True, "WARN", "배포 재기동 기록 없음 — 판정 안 함")

    failed = [row for row in rows if row["result"] == "실패"]
    unknown = [row for row in rows if row["result"] == "판정불가"]
    detail = f"배포 재기동 {len(rows)}회 — 실패 {len(failed)} · 판정불가 {len(unknown)}"

    if failed or unknown:
        worst = (failed or unknown)[-1]
        reasons = "; ".join(log["detail"] for log in worst["logs"] if log["detail"])[:160]
        return (name, False, "FAIL" if failed else "WARN", f"{detail} (마지막 {worst['time'][11:]} {reasons})")

    return (name, True, "FAIL", detail)


def engine_logs() -> list:
    """계좌마다 그날 실행 로그를 이름표와 함께 낸다.

    갈라 띄운 날에는 로그 폴더에 quant_trader.log 가 없고 역할별 파일 셋뿐이라, 파일 이름으로
    훑던 전역 판정이 통째로 비었다. 이름표는 한 프로세스면 계좌 이름, 갈라 띄웠으면 "계좌/역할" 이다 —
    어느 역할이 사유를 안 적고 내려갔는지 판정 문구에서 바로 보이게 한다. [why D-114 단계 5]
    """
    labelled = []

    for directory in sorted(REPO.glob("Quant/build*/logs*")):
        if not directory.is_dir():
            continue

        for engine_log in _logdir.live_logs(directory):
            role = _logdir.role_of(engine_log)
            labelled.append((directory.name if role == "both" else f"{directory.name}/{role}",
                             engine_log))

    return labelled


def shared_region_exit_row(date: str) -> tuple:
    """앞선 기동이 종료 사유를 적고 내려갔는지, 짝이 몰래 다시 떴는지.

    공유 쪽지 머리에는 종료 사유 칸이 있다. 스레드를 다 회수한 뒤 Engine::stop() 이 거기에 사유를
    적는다 — 1 은 장 마감 자기 종료, 2 는 사람·감시견이 내린 것(배포 교체 포함), 3 은 기동하다
    접은 것이다. 비어 있는(0) 채로 남았다면 적기 전에 죽었다는 뜻이고, 다음 기동이 그 쪽지를
    물려받으면서 "앞선 기동이 정상 종료로 끝나지 않았다" 를 로그에 남긴다 — 사람이 로그를 열어
    보지 않아도 되게 그 줄을 센다. [why D-114]

    전에는 빈 칸을 그날 배포 재기동 기록(_private/state/restart_verify.jsonl)과 맞대 보고 기록보다
    많을 때만 FAIL 했다. scripts/deploy_trader.py 가 taskkill /F 로 내려 Quant/src/main.cpp 의
    시그널 처리기를 건너뛰었고, 그러면 stop() 이 아예 안 돌아 정상 배포도 빈 칸을 남겼기 때문이다.
    이제 배포는 운영단말 SHUTDOWN 전문으로 곱게 내리고(사유 2 가 적힌다), 짝 하나가 내려가면 남은
    쪽도 쪽지의 사유를 보고 스스로 나간다. 강제 종료는 곱게 내리기가 실패했을 때의 대비책으로만
    남았으므로 맞대 보기를 걷었다 — 빈 칸은 그대로 FAIL 이다.

    기동 실패(3)를 적고 내려간 것도 FAIL 이다. 기동 번호가 바뀐 것을 제어 스레드가 잡은 줄도
    FAIL 이다 — 짝 프로세스가 죽고 다시 떠서 한쪽이 옛 판을 들고 주문을 내려 했다는 뜻이다.

    갈라 띄운 날에는 붙은 프로세스가 셋(주문·전략·시세)이고 저마다 제 칸에 사유를 적으므로, 로그도
    역할별로 읽어 어느 역할이 안 적고 내려갔는지 문구에 이름표로 남긴다. [why D-114 단계 5]
    """
    name = "공유 쪽지 종료 판정"
    blank_reasons: dict[str, int] = {}
    startup_fails: dict[str, int] = {}
    clean_exits: dict[str, int] = {}
    desyncs: dict[str, int] = {}
    saw_any_line = False

    for account, engine_log in engine_logs():
        try:
            body = engine_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for line in body.splitlines():
            if date not in line:
                continue

            saw_any_line = True

            if "앞선 기동이 정상 종료로 끝나지 않았다" in line:
                blank_reasons[account] = blank_reasons.get(account, 0) + 1
                continue

            if "건너편이 다시 떴다" in line:
                desyncs[account] = desyncs.get(account, 0) + 1
                continue

            matched = PEER_EXIT_REASON_RE.search(line)

            if matched is None:
                continue

            if int(matched.group(1)) == SHUTDOWN_REASON_STARTUP_FAIL:
                startup_fails[account] = startup_fails.get(account, 0) + 1
            else:
                clean_exits[account] = clean_exits.get(account, 0) + 1

    if not saw_any_line:
        # 공유 쪽지는 이름 붙은 메모리라 엔진이 안 떠 있으면 읽을 것이 없다. 없는 상태를 실패로
        #  읽지 않는다 — 다른 행과 같이 판정을 접는다.
        return (name, True, "WARN", f"{date} 기동 로그가 없다 — 판정 안 함")

    if desyncs:
        detail = ", ".join(f"{account} {count}회" for account, count in sorted(desyncs.items()))
        return (name, False, "FAIL",
                f"짝 프로세스 재기동을 제어 스레드가 잡았다 — {detail}"
                " (한쪽이 옛 공유 판을 들고 있었다, 주문이 허공으로 나갔을 수 있다)")

    total_blank = sum(blank_reasons.values())

    if total_blank:
        detail = ", ".join(f"{account} {count}회" for account, count in sorted(blank_reasons.items()))
        return (name, False, "FAIL",
                f"종료 사유가 안 적힌 기동 {total_blank}회 — {detail}"
                " (곱게 내리기가 실패했거나 크래시다:"
                " _private/deploy_guard.log 와 트레이더 로그의 [Ops] SHUTDOWN 줄을 본다)")

    total_startup_fails = sum(startup_fails.values())

    if total_startup_fails:
        detail = ", ".join(f"{account} {count}회" for account, count in sorted(startup_fails.items()))
        return (name, False, "FAIL",
                f"기동하다 접고 내려간 기동 {total_startup_fails}회 — {detail}"
                " (그 기동은 매매를 못 했다: 트레이더 로그 기동 구간의 ERROR 줄을 본다)")

    return (name, True, "FAIL",
            f"앞선 기동이 모두 사유를 적고 내려갔다 (짝 따라 내려간 것 {sum(clean_exits.values())}회)")


def order_answer_row(date: str) -> tuple:
    """전략이 낸 주문 요청에 답이 돌아왔는지, 주문 쪽이 살아 있었는지.

    전략 스레드는 보낸 요청의 순번을 들고 있다가 답이 오면 지운다. 시한(60초)을 넘겨도 안 지워진
    것이 있으면 "답이 없는 주문 요청" 을 찍고, 누계는 [큐 고수위] 줄의 order_answer_overdue 에
    실린다. 답이 안 가는 갈래는 셋이다 — 응답 큐가 차서 못 보냈거나, 재시도를 예약한 채 주문
    스레드가 내려갔거나, 주문 쪽 프로세스가 죽고 다시 안 떴거나. [why D-114]

    다시 보내지는 않는다. KIS 주식주문(현금) 요청 전문에 우리가 채우는 식별자 칸이 없어
    (2026-09-24 확인) 증권사가 같은 주문을 걸러 주지 못하고, 이미 접수된 주문을 다시 보내면 두
    건이 된다. 그래서 이 행이 하는 일은 사람 대신 세어 두는 것까지다.

    박동이 끊긴 것은 FAIL 이다 — 주문 쪽이 죽고 다시 안 뜬 갈래는 이것 말고 잡을 길이 없다.
    기동 번호는 짝이 다시 떠야 바뀌고, 요청 큐가 가득 차 뜨는 경고는 "주문 쪽이 바쁘다" 는 뜻이라
    원인을 반대로 가리킨다.

    답이 늦은 것만 있으면 WARN 이다. 시한 60초는 실측이 아니라 증권사 왕복 상한(윈도 전송 10초·
    수신 15초)에서 잡은 첫 값이라, 한 번 걸렸다고 곧장 고장으로 읽지 않는다. 장중 값이 쌓이면
    좁힌다.
    """
    name = "주문 답 판정"
    beat_lost: dict[str, int] = {}
    overdue: dict[str, int] = {}
    gap_max_ms = -1

    for account, engine_log in engine_logs():
        try:
            body = engine_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for line in body.splitlines():
            if date not in line:
                continue

            if "주문 박동이 끊겼다" in line:
                beat_lost[account] = beat_lost.get(account, 0) + 1
                continue

            # 누계는 [큐 고수위] 줄이 싣는다 — 경고는 첫 건과 100 배수에만 찍히므로 줄 수로 세면 모자란다.
            found = re.search(r"order_answer_overdue=(\d+)", line)

            if found:
                overdue[account] = max(overdue.get(account, 0), int(found.group(1)))

            found = re.search(r"order_beat_gap_max=(\d+)ms", line)

            if found:
                gap_max_ms = max(gap_max_ms, int(found.group(1)))

    gap_note = f", 주문 박동 최대 공백 {gap_max_ms}ms" if gap_max_ms >= 0 else ""

    if beat_lost:
        detail = ", ".join(f"{account} {count}회" for account, count in sorted(beat_lost.items()))
        return (name, False, "FAIL",
                f"주문 박동이 끊긴 기동 — {detail}"
                f" (그동안 전략이 낸 주문은 증권사로 나가지 않았다{gap_note})")

    total_overdue = sum(overdue.values())

    if total_overdue:
        detail = ", ".join(f"{account} {count}건" for account, count in sorted(overdue.items()))
        return (name, True, "WARN",
                f"시한 60초를 넘겨도 답이 안 온 요청 {total_overdue}건 — {detail}"
                f" (시한은 증권사 왕복 상한에서 잡은 첫 값이다{gap_note})")

    return (name, True, "FAIL", f"보낸 요청에 모두 답이 돌아왔다{gap_note}")


def scan_registration_row(date: str) -> tuple:
    """유니버스 스캔이 하루 종일 한 종목도 등록하지 못한 계좌가 있는지.

    스캔이 0종목으로 끝나면 신호가 만들어지지 않아 매매가 통째로 없다. 그런데 엔진은
    아무 경고도 내지 않는다 — "오늘은 후보가 없었다" 와 "거르는 조건이 어긋나 있다" 가
    로그에서 똑같이 보이기 때문이다. 2026-09-23 실계좌 애프터마켓이 그랬다. 거래대금
    문턱을 10억에서 1억으로 낮춰 검사 대상을 23에서 36으로 늘렸는데도 늘어난 13종목이
    전부 역배열컷에 걸려 등록은 0 그대로였다. 후보는 급등락 랭킹에서 오는데 진입은
    정배열을 요구해, 두 기준이 서로 반대쪽을 본다. [why D-097]

    로그 폴더별로 가른다 — 실계좌·모의·리눅스가 저마다 폴더를 쓰는데 한데 합치면 모의의
    정상 등록이 실계좌의 0을 덮는다(09-23 실측: 합치면 948회 11,108종목으로 통과했다).
    등록이 한 번도 안 난 폴더는 무엇이 걸렀는지를 같이 낸다 — 그 내역이 곧 다음에 고칠 자리다.
    """
    name = "스캔 등록"
    scans_by_account: dict = {}
    registered_by_account: dict = {}
    breakdown_by_account: dict = {}

    for account, engine_log in engine_logs():
        try:
            body = engine_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for line in body.splitlines():
            if date not in line or "정배열 프리필터" not in line:
                continue

            counters = dict(SCAN_COUNTER_PATTERN.findall(line))
            scans_by_account[account] = scans_by_account.get(account, 0) + 1
            registered_by_account[account] = (registered_by_account.get(account, 0)
                                              + int(counters.get("등록", "0")))

            # 마지막 스캔의 내역만 남긴다 — 하루치를 합치면 재스캔 주기만큼 부풀어
            #  "몇 종목이 왜 걸렸나" 를 못 읽는다.
            breakdown_by_account[account] = (
                f"후보 {counters.get('후보', '?')} 중"
                f" 거래대금미달 {counters.get('거래대금미달', '?')}"
                f"·역배열 {counters.get('역배열컷', '?')}"
                f"·과확장 {counters.get('과확장컷', '?')}"
                f"·데이터부족 {counters.get('데이터부족', '?')}"
            )

    if not scans_by_account:
        return (name, True, "WARN", f"{date} 스캔 로그가 없다 — 판정 안 함")

    empty = [account for account, total in registered_by_account.items() if not total]

    if empty:
        details = "; ".join(
            f"{account} 스캔 {scans_by_account[account]}회 모두 0종목"
            f" ({breakdown_by_account[account]})" for account in sorted(empty))
        return (name, False, "FAIL",
                f"{details} (신호가 안 만들어져 매매가 통째로 없다, D-097)")

    summary = ", ".join(f"{account} {registered_by_account[account]}종목"
                        for account in sorted(registered_by_account))
    return (name, True, "FAIL", f"모든 계좌가 등록했다 — {summary}")


THREAD_LABEL_RE = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+ \{([^}]*?)(?: thread)?\} \[")  # 뒤의 " thread"는 이름이 아니라 표기라 뗀다
UNNAMED_THREAD_RE = re.compile(r"^T\d+$")


def thread_label_row(date: str) -> tuple:
    """로그 줄마다 찍은 스레드 이름이 붙었는지, 이름 없이 번호(T12345)로 찍힌 스레드가 있는지.

    여러 스레드가 섞어 쓰는 로그는 누가 찍었는지 없으면 선후 관계를 못 가린다. 스레드는 만들 때
    thread_name::set_current 로 이름을 받고, 안 받은 스레드는 운영체제 번호로 찍힌다 — 번호가 보이면
    이름 붙이기를 빠뜨린 스레드가 새로 생긴 것이다. 칸이 아예 없으면 그날 돈 exe가 이 변경 전 빌드다.
    """
    name = "로그 스레드 칸"
    total = 0
    labelled = 0
    unnamed: dict = {}

    for _account, engine_log in engine_logs():
        try:
            body = engine_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for line in body.splitlines():
            if not line.startswith(date):
                continue

            total += 1
            match = THREAD_LABEL_RE.match(line)

            if match is None:
                continue

            labelled += 1
            label = match.group(1)

            if UNNAMED_THREAD_RE.match(label):
                unnamed[label] = unnamed.get(label, 0) + 1

    if total == 0:
        return (name, True, "WARN", f"{date} 엔진 로그 줄 없음 — 판정 안 함")

    if labelled == 0:
        return (name, False, "WARN", f"{total}줄 모두 스레드 칸 없음 — 스레드 이름을 찍기 전 빌드가 돌았다")

    if unnamed:
        top = ", ".join(f"{label} {count}줄" for label, count in sorted(unnamed.items(), key=lambda item: -item[1])[:3])
        return (name, False, "WARN",
                f"이름 없는 스레드 {len(unnamed)}개가 찍었다({top}) — 그 스레드 시작점에 set_current를 넣는다")

    return (name, True, "WARN", f"{labelled}/{total}줄에 스레드 이름이 찍혔다")


def fill_notice_session_row(date: str) -> tuple:
    """체결통보 세션이 붙었는지. 없으면 체결이 원장에 안 실린다.

    2026-09-23 실계좌 첫날, 설정에 quote_kis 가 없어 유니버스 스캔이 통째 건너뛰었고,
    구독 종목이 0개라 WS 자체가 안 열려 체결통보까지 끈겼다. 그 탓에 16:59 청산 체결이
    원장에 안 실리고, 잔고 대조가 보유를 지우는(PRUNE) 것으로 끝났다 — 손익 귀속이 통째 비었다.
    기동 로그 한 줄로 드러나므로 그걸 본다. [why D-097]
    """
    name = "체결통보 세션"
    missing: list[str] = []
    attached = 0

    for account, engine_log in engine_logs():
        try:
            body = engine_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        # 기동 하나를 "WS 구독 종목" 줄로 끊는다. 그 뒤에 "체결통보 세션" 줄이 아예
        #  안 나오면 WS 를 안 열었다는 뜻이다 — 그 갈래가 조용했던 탓에 09-23 실계좌 기동
        #  다섯 번이 통보 없이 돌았는데도 판정이 통과했다. 이제 엔진이 그 줄을 남기지만
        #  지나간 날 로그에는 없어 순서로도 가른다. 한 파일 안에 옆 엔진과 새 엔진의 기동이
        #  섞여 있어(09-23 모의는 13:29부터 새 exe) 파일 단위로는 가를 수 없다.
        pending_start = ""

        for line in body.splitlines():
            if date not in line:
                continue

            if "WS 구독 종목" in line:
                if pending_start:
                    missing.append(f"{pending_start} [{account}]")

                # 구독이 한 종목이라도 있으면 WS 는 열렸다 — 문제는 0개일 때였다.
                pending_start = line[11:19] if "0개" in line else ""
                continue

            if "체결통보 세션" not in line:
                continue

            pending_start = ""

            if "없음" in line:
                missing.append(f"{line[11:19]} [{account}]")
            else:
                attached += 1

        if pending_start:
            missing.append(f"{pending_start} [{account}]")

    if not attached and not missing:
        return (name, True, "WARN", f"{date} 기동 로그가 없다 — 판정 안 함")

    if missing:
        return (name, False, "FAIL",
                f"체결통보 없이 둔 기동 {len(missing)}회 — {', '.join(missing[:3])}"
                " (체결이 원장에 안 실려 손익 귀속이 비고, 잔고 대조가 보유를 지운다, D-097)")

    return (name, True, "FAIL", f"기동 {attached}회 모두 체결통보 세션을 잡았다")


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
                # 구간 지연은 전부 주문 프로세스가 채우는 칸이다 [why D-129]
                " FROM health WHERE (ts AT TIME ZONE 'Asia/Seoul')::date = %s"
                " AND role IN ('both','order')", (date,))
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


def job_attach_row(date: str) -> tuple:
    """감시견이 띄운 프로세스가 작업 개체에 전부 들어갔는지.

    감시견은 부속 창과 트레이더를 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 잡에 넣는다 —
    감시견이 어떻게 죽든 커널이 자식을 같이 정리하게 하려는 것이다. 편입에 실패한 프로세스는
    감시견이 사라져도 혼자 남는다. 트레이더가 그렇게 남으면 아무도 보지 않는 채 발주가 이어지고,
    다음 기동은 중복 프로세스 검사에 막혀 그날 매매가 통째로 빈다.
    감시견은 실패를 WARN 한 줄로만 남기고 지나가므로 여기서 판정한다.
    """
    name = "부속 잡 편입"
    date_compact = date.replace("-", "")
    # 계좌마다 감시견이 따로 돌고 로그도 갈린다 — 모의는 auto_trade_day_, 실계좌는
    #  auto_trade_day_live_. 한쪽만 보면 나머지 계좌의 실패를 통째로 놓친다. [why D-122]
    run_logs = sorted((REPO / "logs").glob(f"auto_trade_day_*{date_compact}.log"))

    if not run_logs:
        return (name, True, "WARN",
                f"감시견 실행 로그 없음(auto_trade_day_*{date_compact}.log) — 감시견이 안 돌았다, 판정 안 함")

    attached = 0
    create_failures = 0
    add_failures: list[str] = []

    for run_log in run_logs:
        try:
            log_text = run_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        attached += log_text.count("잡에 묶음")
        create_failures += log_text.count("Job Object 생성 실패")
        # 어느 계좌에서 났는지 남긴다 — 파일 이름의 auto_trade_day_ 뒤, 날짜 앞이 인스턴스다.
        instance = run_log.stem[len("auto_trade_day_"):-len(date_compact)].strip("_") or "모의"

        for line in log_text.splitlines():
            if "잡 편입 실패" in line:
                add_failures.append(f"{instance}: {line.strip()[:70]}")

    ok = not add_failures and create_failures == 0
    detail = f"편입 {attached}건 성공, 편입 실패 {len(add_failures)}건, 잡 생성 실패 {create_failures}회"

    if add_failures:
        detail += " — " + " / ".join(add_failures[:3])

    if create_failures:
        detail += " — 잡이 아예 없는 세션은 부속 창이 감시견보다 오래 남는다"

    return (name, ok, "FAIL", detail)


def gross_exposure_config_row() -> tuple:
    """실계좌 설정의 총노출 한도가 자본의 100%를 넘는지 본다.

    총노출은 보유를 현재가로 재고(2026-09-26부터, 그 전에는 평단) 분모는 총평가금이다. 1.0을 넘기면
    가진 돈보다 많이 사는 것을 허용한다는 뜻이라 한도가 사실상 꺼진다. 평단으로 재던 때 물린 이월분에
    BUY가 다 막혀 2.0으로 임시로 연 일이 있어(09-25), 그 완화가 남아 있으면 매매일지에 뜨게 한다.
    값을 바꾸는 것은 사람이 정한다 — 이 행은 알리기만 해서 WARN이다.
    """
    name = "실계좌 총노출 한도"
    config_path = REPO / "Quant" / "config" / "config_live.json"

    if not config_path.exists():
        return (name, True, "WARN", "Quant/config/config_live.json 없음 — 판정 안 함")

    try:
        risk = json.loads(config_path.read_text(encoding="utf-8")).get("risk", {})
    except (OSError, ValueError) as error:
        return (name, False, "WARN", f"Quant/config/config_live.json 읽기 실패: {error}")

    limit = risk.get("max_gross_exposure_pct")

    if not isinstance(limit, (int, float)):
        return (name, True, "WARN", "risk.max_gross_exposure_pct 없음 — 게이트 꺼짐(0과 같다)")

    return (name, limit <= 1.0, "WARN",
            f"risk.max_gross_exposure_pct={limit} (기준 1.0 이하 — 넘으면 자본보다 많이 살 수 있다)")


def orphan_process_rows() -> list:
    """부모가 이미 죽었는데 혼자 남은 프로세스를 지금 이 순간 기준으로 센다.

    엔진 계열이 그렇게 남는 것은 잡이 제 일을 못 했거나 누가 손으로 띄운 것이라 이중 발주로 이어진다.
    개발 도구 쪽은 아직 하루를 망친 적이 없어 수치만 적는다 — 며칠 쌓아 보고 기준값을 정한다
    (2026-09-23 시작. 그날 재부팅 전 도구들이 물리 메모리 15GB를 물고 있었고, 부모 없이 남은 것은
    9MB 하나뿐이라 원인이 그게 아니라 안 닫은 창이었다. 그 판단을 수치로 이어 두려는 행이다).
    """
    engine_name = "엔진 남은 프로세스"
    tool_name = "도구 남은 프로세스"

    try:
        import psutil
    except ImportError:
        return [(engine_name, True, "WARN", "psutil 없음 — 판정 안 함 (py -m pip install psutil)"),
                (tool_name, True, "WARN", "psutil 없음 — 판정 안 함")]

    # 감시견 잡이 지켜야 하는 것들. 부모 없이 남으면 감시 밖에서 도는 중이다.
    engine_names = ("quant_trader", "ops_terminal")
    # 창을 닫아도 남는지 보려고 세는 것들. 판정이 아니라 관측이라 목록이 넉넉해도 된다.
    tool_names = ("Code", "devenv", "node", "python", "py", "claude",
                  "cpptools", "cpptools-srv", "vcpkgsrv", "copilot-language-server",
                  "msedgewebview2", "ServiceHub.Host.dotnet.x64")

    engine_orphans: list[str] = []
    tool_orphans: list[str] = []
    tool_bytes = 0

    for process in psutil.process_iter(["name"]):
        try:
            raw_name = process.info["name"] or ""
            base_name = raw_name[:-4] if raw_name.lower().endswith(".exe") else raw_name

            if base_name not in engine_names and base_name not in tool_names:
                continue

            # parent() 는 부모 PID 를 찾은 뒤 그 프로세스의 생성 시각이 자식보다 이른지까지 본다 —
            #  윈도는 PID 를 재사용하므로 이 확인이 없으면 남의 프로세스를 부모로 착각한다.
            if process.parent() is not None:
                continue

            resident_bytes = process.memory_info().rss
        except (psutil.Error, OSError):
            continue

        if base_name in engine_names:
            engine_orphans.append(f"{base_name} pid={process.pid}")
        else:
            tool_orphans.append(base_name)
            tool_bytes += resident_bytes

    engine_detail = (f"부모 없이 남은 엔진 프로세스 {len(engine_orphans)}개"
                     + (" — " + ", ".join(engine_orphans[:5]) if engine_orphans
                        else " (감시견 잡이 지키고 있다)"))

    # 이 한도는 근거가 얇다. 재부팅 전 도구들이 물고 있던 15GB 에 견줘 눈에 띄는 크기로 잡았을 뿐이라
    #  며칠 수치를 보고 고친다. 넘어도 매매와는 무관하니 WARN 이다.
    tool_limit_bytes = 1 << 30
    tool_counts = {name: tool_orphans.count(name) for name in sorted(set(tool_orphans))}
    tool_detail = (f"부모 없이 남은 도구 프로세스 {len(tool_orphans)}개 "
                   f"{tool_bytes / (1 << 20):.0f}MB (기준 {tool_limit_bytes >> 30}GB)"
                   + (" — " + ", ".join(f"{name} {count}개" for name, count in tool_counts.items())
                      if tool_counts else ""))

    return [(engine_name, not engine_orphans, "FAIL", engine_detail),
            (tool_name, tool_bytes < tool_limit_bytes, "WARN", tool_detail)]


def pinned_capture_row(date: str) -> tuple:
    """WS 고정 종목(websocket_pin_tickers)의 체결을 엔진이 실제로 받았는지 캡처로 센다.

    엔진이 체결을 빠짐없이 받는지 확인하려고 한 종목(000660)을 칸에 고정하고 캡처에 담는다. [why D-138]
    받은 체결 수가 0이면 칸을 못 쥐었거나 캡처가 꺼진 것이고, 수량 합이 KIS 누적거래량 증가분보다
    작으면 받는 길에서 체결이 빠진 것이다. 09-21 모의 캡처 25종목은 99.8~99.96%였다(356680 하나만 94.7%)
    — 100%가 아닌 까닭을 아직 모르므로 99% 아래는 경고로만 둔다.
    """
    import capture_stats  # noqa: PLC0415 — scripts/ 가 sys.path 에 있다

    name = "고정 종목 체결 수신"
    pins: set[str] = set()
    folders: set[Path] = set()

    for config_path in sorted((REPO / "Quant" / "config").glob("config*.json")):
        try:
            document = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue

        if document.get("websocket_pin_tickers") and document.get("capture_dir"):
            pins.update(document["websocket_pin_tickers"])
            folders.add(REPO / document["capture_dir"])

    if not pins:
        return (name, True, "WARN", "websocket_pin_tickers 를 켠 설정이 없다 — 판정 안 함")

    files = [path for folder in sorted(folders) for path in capture_stats.capture_files(folder, date)]

    if not files:
        return (name, True, "WARN", f"{date} 캡처 파일이 없다 — 판정 안 함")

    trades: dict[str, int] = {pin: 0 for pin in pins}
    matches: dict[str, list[float]] = {pin: [] for pin in pins}
    anything = 0

    for path in files:
        try:
            summary = capture_stats.summarize(path)
        except (OSError, ValueError):
            continue

        for ticker, statistics in summary.tickers.items():
            anything += statistics.trades

            if ticker in pins:
                trades[ticker] += statistics.trades
                match = statistics.volume_match()

                if match is not None:
                    matches[ticker].append(match)

    if not anything:
        return (name, True, "WARN", f"{date} 캡처 {len(files)}개에 체결이 하나도 없다(장 밖 기동) — 판정 안 함")

    details = []

    for pin in sorted(pins):
        lowest = min(matches[pin]) if matches[pin] else None
        details.append(f"{pin} {trades[pin]}건" + ("" if lowest is None else f" 수량/누적거래량 {lowest * 100:.2f}%"))

    missing = [pin for pin in pins if trades[pin] == 0]

    if missing:
        return (name, False, "FAIL", f"다른 종목은 받았는데 고정 종목 {', '.join(sorted(missing))} 체결 0건 — 칸을 못 쥐었다 ({'; '.join(details)})")

    short = [pin for pin in pins if matches[pin] and min(matches[pin]) < 0.99]

    if short:
        return (name, False, "WARN", f"수량 합이 누적거래량보다 1% 넘게 모자란다 — 받는 길에서 체결이 빠졌을 수 있다 ({'; '.join(details)})")

    return (name, True, "WARN", f"캡처 {len(files)}개 — {'; '.join(details)}")


def journal_mirror_row(date: str) -> tuple:
    """원장 저널의 주문 결과·체결이 DB(ledger_events → orders·fills)에 빠짐없이 옮겨졌는지 저널마다 센다.

    주문·체결은 원장 저널 적재기(PYQuant/tools/ledger_recorder.py)가 엔진이 주문 전에 적는 파일에서 옮긴다.
    ZMQ로 받던 때는 리코더가 죽어 있거나 대기칸이 차면 조용히 빠졌다 — 이제 정본 파일과 건수를 맞춰 본다. [why D-113]
    적재기는 2초마다 따라가므로 마지막 60초 안에 적힌 레코드는 세지 않는다.
    """
    import time  # noqa: PLC0415

    sys.path.insert(0, str(REPO / "PYQuant" / "tools"))
    import ledger_dump  # noqa: PLC0415

    name = "원장 저널 적재"
    files = sorted((REPO / "Quant" / "build_win").glob(f"*/ledger_{date.replace('-', '')}.bin"))

    if not files:
        return (name, True, "WARN", f"{date} 원장 저널 파일이 없다 — 판정 안 함")

    cutoff_us = int((time.time() - 60) * 1_000_000)
    expected: dict[str, tuple[int, int, int]] = {}

    for path in files:
        try:
            records = [record for record in ledger_dump.read_records(path).records if record.wall_us < cutoff_us]
        except (OSError, ValueError) as error:
            return (name, False, "WARN", f"{path.parent.name}/{path.name} 읽기 실패 — {error}")

        fill_count = sum(1 for record in records if record.kind == "FILL")
        order_count = sum(1 for record in records if record.kind in ("ACCEPT", "REJECT"))
        last_sequence = records[-1].sequence if records else 0
        expected[path.parent.name] = (fill_count, order_count, last_sequence)

    password = tsdb_password()

    if not password:
        return (name, True, "WARN", ".env에 TSDB_PASSWORD 없음 — 판정 안 함")

    psycopg2 = import_psycopg2()

    if psycopg2 is None:
        return (name, False, "WARN", "psycopg2 없음 — venv(PYQuant/.venv*)로 부른다")

    actual: dict[str, tuple[int, int, int]] = {}

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
        with connection.cursor() as cursor:
            for journal, (_, _, last_sequence) in expected.items():
                cursor.execute(
                    "SELECT (SELECT COUNT(*) FROM ledger_events WHERE trade_date = %(day)s AND journal = %(journal)s"
                    "   AND seq <= %(last)s AND kind = 'FILL'),"
                    " (SELECT COUNT(*) FROM fills WHERE journal_date = %(day)s AND journal = %(journal)s"
                    "   AND journal_seq <= %(last)s),"
                    " (SELECT COUNT(*) FROM orders WHERE journal_date = %(day)s AND journal = %(journal)s"
                    "   AND journal_seq <= %(last)s)",
                    {"day": date, "journal": journal, "last": last_sequence})
                actual[journal] = cursor.fetchone()

        connection.close()
    except Exception as error:   # DB가 없거나 잠든 날은 판정을 미룬다
        return (name, True, "WARN", f"DB 조회 실패 — 판정 안 함 ({str(error).strip()[:80]})")

    details = []
    short = []

    for journal, (fill_count, order_count, _) in expected.items():
        event_fills, table_fills, table_orders = actual[journal]
        details.append(f"{journal} 체결 {fill_count}/{event_fills}/{table_fills}건·주문 결과 {order_count}/{table_orders}건")

        if (event_fills, table_fills, table_orders) != (fill_count, fill_count, order_count):
            short.append(journal)

    if short:
        return (name, False, "FAIL",
                f"저널과 DB 건수가 다르다({', '.join(short)}) — 적재기(PYQuant/tools/ledger_recorder.py)가 그 폴더를"
                f" 안 따라갔거나 옮기기가 실패했다. 저널/ledger_events/fills 순: {'; '.join(details)}")

    return (name, True, "FAIL", "저널/ledger_events/fills 순 " + "; ".join(details))


def regime_feed_row(date: str) -> tuple:
    """엔진 안 국면 판정 피드(Quant/src/regime/RegimeFeed.cpp)가 정규장 동안 끊기지 않고 이력을 남겼는지 본다.

    09-27부터 regime.json 을 쓰는 곳은 이 피드 하나다(파이썬 피드는 걷었다). 180초마다 한 줄이 쌓이므로 09:00~15:30 사이
    이력이 없거나 두 줄 사이가 15분을 넘으면 그동안 국면 게이트가 마지막 값으로 굳어 있었다는 뜻이다. [why D-147]
    """
    name = "국면 판정 피드 이력"
    path = REPO / "logs" / "regime_history.jsonl"
    stamps = []

    if path.exists():
        with path.open(encoding="utf-8") as file:
            for line in file:
                if date not in line[:60]:
                    continue

                try:
                    row = json.loads(line)
                except ValueError:
                    continue

                stamp_text = str(row.get("ts", ""))

                if stamp_text.startswith(date) and "09:00" <= stamp_text[11:16] <= "15:30":
                    stamps.append(dt.datetime.fromisoformat(stamp_text).timestamp())

    if not stamps:
        return (name, False, "FAIL", f"{date} 정규장 국면 이력(logs/regime_history.jsonl)이 한 줄도 없다"
                                    " — config regime_feed 설정과 엔진 로그의 RegimeFeed 줄을 본다")

    stamps.sort()
    widest = max((later - earlier for earlier, later in zip(stamps, stamps[1:])), default=0)
    detail = f"정규장 {len(stamps)}줄, 가장 긴 공백 {widest / 60:.1f}분"

    if widest > 15 * 60:
        return (name, False, "WARN", detail + " — 15분을 넘게 국면 판정이 멈췄다")

    return (name, True, "WARN", detail)


def global_rows(date: str) -> list:
    """계좌와 무관한 판정 — 하루에 한 번만 낸다.

    자원·피드·큐·부속 잡·TSAN 은 로그 폴더를 스스로 훑거나 저장소 전체를 본다.
    계좌별로 다시 부르면 같은 줄이 계좌 수만큼 반복된다.
    """
    return [
        *resource_sampling_rows(date),
        *feed_ledger_rows(date),
        journal_mirror_row(date),
        regime_feed_row(date),
        queue_latency_row(date),
        role_publish_row(date),
        order_latency_breakdown_row(date),
        market_open_gate_row(date),
        after_market_order_row(date),
        restart_verify_row(date),
        shared_region_exit_row(date),
        order_answer_row(date),
        fill_notice_session_row(date),
        pinned_capture_row(date),
        scan_registration_row(date),
        thread_label_row(date),
        job_attach_row(date),
        gross_exposure_config_row(),
        *orphan_process_rows(),
        tsan_row(date),
    ]


def print_rows(rows: list) -> int:
    """판정 줄을 찍고 FAIL 수를 돌려준다."""
    bad = 0

    for name, ok, level, detail in rows:
        tag = "PASS" if ok else level

        if not ok and level == "FAIL":
            bad += 1

        print(f"  [{tag:4}] {name:16} {detail}")

    return bad


def log_lines(date: str, log: Path):
    """판정이 읽을 줄을 낸다(개행 포함).

    로그 폴더를 주면 그 폴더의 실행 로그와 그날 회전본을 줄머리 시각으로 합쳐 낸다 — 갈라 띄운 날에는
    역할마다 파일을 나눠 쓰므로, 이어 붙이면 시각이 되감겨 '마지막 줄'을 보는 판정이 틀린다.
    파일 하나를 주면 그 파일만 읽는다(7일 지난 날의 archive/*.log.gz 도 그대로). [why D-114]
    """
    if log.is_dir():
        yield from _logdir.iter_log_lines(date, log)
        return

    with _logdir.open_log(log) as log_file:
        yield from log_file


def role_process_count(directory: Path, date: str) -> int:
    """그 폴더에서 그날 로그를 쓴 역할 프로세스 수. 갈라 띄운 날은 3(주문·전략·시세), 한 프로세스면 1이다."""
    roles = {_logdir.role_of(path) for path in _logdir.log_sources(date, directory)}
    roles.discard("both")
    return max(1, len(roles))


def boot_starts(starts: list[int], process_count: int) -> list[int]:
    """엔진 시작 줄을 기동 단위로 묶는다.

    갈라 띄우면 역할마다 같은 기동에서 한 줄씩 찍는다. 그대로 세면 세션 수가 역할 수만큼 불고,
    두 줄 사이가 몇 초뿐이라 '일찍 끝난 세션'도 기동마다 헛으로 잡힌다. 붙어 있는 줄을 프로세스 수만큼까지만
    한 기동으로 본다 — 한 프로세스로 띄운 날은 아무것도 묶지 않아 판정이 예전 그대로다.
    """
    if process_count <= 1:
        return starts

    boots: list[int] = []
    grouped = 0

    for second in starts:
        if boots and grouped < process_count and second - boots[-1] < SAME_BOOT_SEC:
            grouped += 1
            continue

        boots.append(second)
        grouped = 1

    return boots


def account_name(target: Path) -> str:
    """판정 묶음에 붙일 계좌 이름 — 로그 폴더 이름이다. 파일 하나를 받으면 그 파일이 든 로그 폴더 이름."""
    return target.name if target.is_dir() else _logdir.dir_of(target).name


def health_targets() -> list[Path]:
    """계좌마다 판정할 로그 폴더. 갈라 띄운 날에는 폴더에 quant_trader.log 가 없고 역할별 파일 셋뿐이라,
    파일 이름으로 훑으면 그날 판정이 통째로 빈다 — 그래서 폴더째로 고른다. [why D-114]"""
    return [directory for directory in sorted(REPO.glob("Quant/build*/logs*"))
            if directory.is_dir() and _logdir.live_logs(directory)]


def collect(date: str, log: Path, since: int = 0, include_global: bool = True):
    """로그 폴더 하나(또는 로그 파일 하나)에서 그날 점검 행을 만든다.

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
    value_rank_etf_drops = 0          # 랭킹 응답에 ETF가 섞여 들어온 행수
    value_rank_short: list[tuple[int, int]] = []   # (받은 행수, 요청 행수) — 요청보다 모자랐던 회차
    turnover_axis_taken: list[int] = []   # 재스캔마다 거래대금 상위 축이 뽑은 종목 수
    untracked_opens: list[tuple[int, str]] = []
    blocked_sells: list[tuple[int, str]] = []
    ws_fallbacks = 0
    prices_stale_ages: list[int] = []   # 전 종목 시세 낡음 경고의 초 수 — 시세판·보조 프로세스 멈춤 흔적
    board_sweep_fails = 0
    board_late_holds: list[int] = []    # 09:10 뒤 재랭킹 보류 시각(초)
    daily_warm_done: list[tuple[int, int, int, bool]] = []   # (받음, 대상, 초, 마감에 멈춤)
    daily_warm_fails = 0
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
    order_division_rejects = 0
    basket_as_of: list[str] = []                 # 목표 비중표 읽음 줄의 as_of(YYYYMMDD)
    basket_orders: list[tuple[int, str, str]] = []  # (초, 종목, 매수|매도) — 바스켓이 낸 주문
    basket_run_end: list[int] = []
    basket_window_closed = 0
    basket_lines = 0
    signals: list[tuple[int, str, str, str]] = []  # (초, 전략 id, 종목, BUY|SELL)
    itb_attached: list[str] = []
    devscale_stops: list[tuple[int, str]] = []     # (초, 종목) — DEVSCALE 손절 신호
    devscale_close_exits: list[tuple[int, str]] = []   # (초, 종목) — DEVSCALE 장 마감 청산 신호(넘김 모드면 0이어야 한다)
    devscale_trails: list[tuple[int, str]] = []    # (초, 종목) — DEVSCALE 트레일 청산 신호
    protective_shadow: list[tuple[int, str]] = []  # (초, 종목) — 보호 주문 표의 그림자 판정
    protective_fired = 0                           # 표가 직접 낸 청산 수(owner 모드면 0보다 크다)
    entry_filter: dict[str, int] = {"통과": 0, "차단": 0}  # 진입 필터 판정 줄 수
    platforms: list[str] = []                    # 엔진이 실제로 뜬 기동의 실행 플랫폼(Windows|Linux)
    pending_platform = ""                        # 플랫폼 줄은 봤지만 아직 엔진이 뜨지 않은 기동
    ledger_replays: list[int] = []               # 기동마다 원장 저널에서 되적용한 레코드 수
    ledger_truncated = 0                         # 꼬리 잘린 기동 수 — 쓰다 만 레코드, 곧 비정상 종료 흔적
    ledger_restored = 0                          # 재기동 때 이력에 되살린 미체결 주문
    ledger_released = 0                          # 재기동 때 선점만 푼 주문(KIS가 모르는 주문)
    ledger_unnumbered = 0                        # 되살림 가운데 접수 응답 전에 끊겨 미체결과 짝지은 주문
    ledger_start_failures = 0                    # 기동 시점 저널 기록 실패 누계(기동마다 한 줄)
    ledger_write_fails = 0                       # 장중 저널 기록 실패(안 나간 주문 + 파일이 원장보다 뒤처진 묶음)
    db_writer_starts = 0                         # 엔진 DB 관리자 시작 줄 수(D-148)
    db_writer_counts = [0, 0, 0, 0, 0]           # 종료 줄 합: 받음·넣음·큐 넘쳐 버림·거절·모호
    db_writer_no_password = 0                    # 켰는데 비밀번호가 없어 적재 워커를 못 띄운 기동 수
    daily_resets: dict[str, int] = {}            # 거래일(yyyymmdd) → 하루 리셋 횟수. 1보다 크면 A-4 재발
    open_order_fails = 0                         # 미체결 조회 실패 줄 수
    beat_dead = 0                                # 주문 스레드가 전략을 죽었다고 본 횟수
    beat_back = 0                                # 박동이 돌아와 진입 정지를 푼 횟수
    beat_gap_max = -1                            # 전략 박동의 가장 긴 공백(ms). -1이면 그 줄이 없는 구 exe
    feed_dead = 0                                # 주문 스레드가 시세를 죽었다고 본 횟수(D-137)
    feed_back = 0                                # 시세 박동이 돌아와 진입 정지를 푼 횟수
    feed_beat_gap_max = -1                       # 시세 박동의 가장 긴 공백(ms). -1이면 그 칸이 없는 옛 exe
    order_duplicate = 0                          # 주문 쪽이 같은 순번을 두 번 받아 거른 수
    order_response_dropped = 0                   # 전략이 답을 안 가져가 버린 수
    order_implausible = -1                       # 값이 말이 안 돼 버린 요청 수. -1이면 그 칸이 없는 옛 바이너리
    order_truncated = -1                         # 판단 근거·주문 이름이 칸을 넘어 잘린 신호 수. -1도 같다
    ledger_gen = 0                               # 장부 사본이 낸 판 수
    ledger_gen_previous = -1                     # 직전 고수위 줄의 판 번호. 같으면 그사이에 한 판도 안 나간 것
    ledger_stall_at = []                         # 판이 안 늘어난 지점의 초
    ledger_foreign = -1                          # 사본에 못 실은 남의 계좌 줄 수. -1이면 그 줄이 없는 구 exe
    control_dropped = 0                          # 앞 토막이 가득 차 전략이 못 보낸 제어 요청 줄 수
    fill_queue_high = -1                         # 체결통보 큐 최고 수위. -1이면 [큐 고수위] 줄이 없다
    fill_queue_capacity = 0
    fill_dropped = 0                             # 체결통보 큐가 가득 차 버린 건수(누계라 최댓값)
    fill_overflowed = 0                          # 큐가 가득 차 넘침 목록에 둔 건수(누계라 최댓값). 버리지는 않는다
    fill_absorbed = 0                            # 잔고 대조가 놓친 체결로 보고 메운 줄 수
    control_relay_dropped = -1                   # 경계 너머 제어 면이 가득 차 못 옮긴 줄 수. -1이면 그 줄이 없는 옛 바이너리
    control_discarded = -1                       # 주문 쪽이 반쪽 표로 보고 버린 줄 수. -1이면 그 줄이 없는 구 exe
    symbol_register_timeout = -1                 # 등록을 주문 쪽에서 못 받은 수. -1이면 그 줄이 없는 구 exe
    symbol_lookup_miss = 0                       # 표에 없는 티커로 잦은 자리가 불린 수
    strategy_register_timeout = -1               # 이름표 등록을 못 받은 수. -1이면 그 줄이 없는 구 exe
    watch_overflow = -1                          # 구독 상한에 밀린 종목 수. -1이면 그 줄이 없는 구 exe
    watch_no_rest = 0                            # 상한에 밀렸는데 REST 대체도 없던 종목 수
    websocket_slot_releases = 0                         # 칸 우선순위 배정이 칸을 내준 횟수(D-132)
    websocket_slot_protected_off = []                   # 보유·선점인데 칸 밖으로 밀린 종목(D-132)
    feed_channel_overflow = -1                   # 통로가 차서 못 넘긴 시세 건수. -1이면 그 줄이 없는 구 exe
    feed_channel_discarded = -1                  # 꺼낸 값이 말이 안 돼 버린 건수. -1이면 그 줄이 없는 구 exe
    feed_channel_sent = -1                       # 시세 통로로 보낸 건수. -1이면 그 칸이 없는 구 exe
    feed_channel_received = -1                   # 시세 통로에서 꺼낸 건수. -1이면 그 칸이 없는 구 exe
    fill_channel_sent = -1                       # 체결 통로로 보낸 체결통보 수. -1이면 그 칸이 없는 구 exe
    fill_channel_received = -1                   # 체결 통로에서 꺼낸 체결통보 수. -1이면 그 칸이 없는 구 exe
    fill_session_socket = -1                     # 체결통보를 맡은 소켓 번호. -1이면 그 줄이 없는 구 exe
    fill_session_none = 0                        # 맡은 소켓이 없다고 찍힌 기동 수
    fill_session_many = 0                        # 둘 이상이 맡았다고 찍힌 기동 수
    fill_replayed = 0                            # 재연결 뒤 다시 온 체결통보로 보고 버린 수
    fill_producer_overlap = 0                    # 체결통보를 두 스레드가 같이 넣으려 한 수(W-6)
    zmq_bind_fail = 0                            # ZMQ 포트 bind 실패(포트 충돌) 횟수

    # 폴더면 갈라 띄운 로그를 시각순으로 합쳐 본다. 파일 하나면 그 파일만 —
    #  7일 지난 날은 archive/quant_trader_<날짜>.log.gz 를 market_close_autodoc이 그대로 넘긴다.
    process_count = role_process_count(log, date) if log.is_dir() else 1

    with contextlib.closing(log_lines(date, log)) as log_file:
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

                # 플랫폼은 엔진이 실제로 뜬 기동만 센다. 설정 로드 실패(지운 모드 포함, D-130)는 플랫폼 줄까지만
                #  찍고 엔진 시작 줄이 없다 — 주문을 한 건도 못 내므로 "엔진 둘" 판정의 대상이 아니다.
                #  09-23 에 Windows FEED 점검 6회가 이 판정을 FAIL 로 만들었다.
                if pending_platform:
                    platforms.append(pending_platform)
                    pending_platform = ""

            if found := PLATFORM_RE.search(line):
                pending_platform = found.group(1)
            found = STALE_RE.search(line)
            if found:
                stale_max = max(stale_max, int(found.group(1)))
            if found := LEDGER_REPLAY_RE.search(line):
                ledger_replays.append(int(found.group(1)))
                ledger_truncated += 1 if found.group(3) else 0
            if LEDGER_UNNUMBERED_RE.search(line):
                ledger_unnumbered += 1
            if found := LEDGER_RESOLVE_RE.search(line):
                ledger_restored += int(found.group(1))
                ledger_released += int(found.group(2))
                ledger_start_failures = max(ledger_start_failures, int(found.group(3)))
            if LEDGER_WRITE_FAIL_RE.search(line):
                ledger_write_fails += 1
            if DB_WRITER_START_RE.search(line):
                db_writer_starts += 1
            if DB_WRITER_NO_PASSWORD_RE.search(line):
                db_writer_no_password += 1
            if found := DB_WRITER_END_RE.search(line):
                db_writer_counts = [total + int(value) for total, value in zip(db_writer_counts, found.groups())]
            if found := DAILY_RESET_RE.search(line):
                daily_resets[found.group(1)] = daily_resets.get(found.group(1), 0) + 1
            if OPEN_ORDER_FAIL_RE.search(line):
                open_order_fails += 1
            if BEAT_DEAD_RE.search(line):
                beat_dead += 1
            if BEAT_BACK_RE.search(line):
                beat_back += 1
            if found := BEAT_GAP_RE.search(line):
                beat_gap_max = max(beat_gap_max, int(found.group(1)))
            if FEED_DEAD_RE.search(line):
                feed_dead += 1
            if FEED_BACK_RE.search(line):
                feed_back += 1
            if found := FEED_BEAT_GAP_RE.search(line):
                feed_beat_gap_max = max(feed_beat_gap_max, int(found.group(1)))
            if found := ORDER_DUPLICATE_RE.search(line):
                order_duplicate = max(order_duplicate, int(found.group(1)))
            if found := ORDER_IMPLAUSIBLE_RE.search(line):
                order_implausible = max(order_implausible, int(found.group(1)))
            if found := ORDER_TRUNCATED_RE.search(line):
                order_truncated = max(order_truncated, int(found.group(1)))
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
            if found := FILL_QUEUE_RE.search(line):
                fill_queue_high = max(fill_queue_high, int(found.group(1)))
                fill_queue_capacity = int(found.group(2))
                fill_dropped = max(fill_dropped, int(found.group(3)))
                fill_overflowed = max(fill_overflowed, int(found.group(4) or 0))
            if FILL_ABSORB_RE.search(line):
                fill_absorbed += 1
            if found := CONTROL_DROP_RE.search(line):
                control_dropped = max(control_dropped, int(found.group(1)))
            if found := CONTROL_RELAY_DROP_RE.search(line):
                control_relay_dropped = max(control_relay_dropped, int(found.group(1)))
            if found := CONTROL_DISCARD_RE.search(line):
                control_discarded = max(control_discarded, int(found.group(1)))
            if found := SYMBOL_REGISTER_TIMEOUT_RE.search(line):
                symbol_register_timeout = max(symbol_register_timeout, int(found.group(1)))
            if found := SYMBOL_LOOKUP_MISS_RE.search(line):
                symbol_lookup_miss = max(symbol_lookup_miss, int(found.group(1)))
            if found := STRATEGY_REGISTER_TIMEOUT_RE.search(line):
                strategy_register_timeout = max(strategy_register_timeout, int(found.group(1)))
            if found := WATCH_OVERFLOW_RE.search(line):
                watch_overflow = max(watch_overflow, int(found.group(1)))
            if WATCH_NO_REST_RE.search(line):
                watch_no_rest += 1
            if WS_SLOT_RELEASE_RE.search(line):
                websocket_slot_releases += 1
            if found := WS_SLOT_PROTECTED_OFF_RE.search(line):
                websocket_slot_protected_off.append(found.group(1))
            if found := FEED_CHANNEL_OVERFLOW_RE.search(line):
                feed_channel_overflow = max(feed_channel_overflow, int(found.group(1)))
            if found := FEED_CHANNEL_DISCARD_RE.search(line):
                feed_channel_discarded = max(feed_channel_discarded, int(found.group(1)))
            if found := FEED_CHANNEL_SENT_RE.search(line):
                feed_channel_sent = max(feed_channel_sent, int(found.group(1)))
            if found := FEED_CHANNEL_RECEIVED_RE.search(line):
                feed_channel_received = max(feed_channel_received, int(found.group(1)))
            if found := FILL_CHANNEL_SENT_RE.search(line):
                fill_channel_sent = max(fill_channel_sent, int(found.group(1)))
            if found := FILL_CHANNEL_RECEIVED_RE.search(line):
                fill_channel_received = max(fill_channel_received, int(found.group(1)))
            if found := FILL_SESSION_ONE_RE.search(line):
                fill_session_socket = int(found.group(1))
            elif FILL_SESSION_NONE_RE.search(line):
                fill_session_none += 1
            elif FILL_SESSION_MANY_RE.search(line):
                fill_session_many += 1
            elif FILL_REPLAY_RE.search(line):
                fill_replayed += 1
            elif FILL_PRODUCER_OVERLAP_RE.search(line):
                fill_producer_overlap += 1
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
            found = VALUE_RANK_DIAG_RE.search(line)
            if found:
                value_rank_etf_drops += int(found.group(2))
            found = VALUE_RANK_DONE_RE.search(line)
            if found and int(found.group(1)) < int(found.group(2)):
                value_rank_short.append((int(found.group(1)), int(found.group(2))))
            found = TURNOVER_AXIS_RE.search(line)
            if found:
                turnover_axis_taken.append(int(found.group(1)))
            found = UNTRACKED_OPEN_RE.search(line)
            if found:
                untracked_opens.append((second, found.group(1)))
            found = BLOCKED_SELL_RE.search(line)
            if found:
                blocked_sells.append((second, found.group(1)))
            if WSFALL_RE.search(line):
                ws_fallbacks += 1
            found = PRICES_STALE_RE.search(line)
            if found:
                prices_stale_ages.append(int(found.group(1)))
            if BOARD_SWEEP_FAIL_RE.search(line):
                board_sweep_fails += 1
            if BOARD_RERANK_HOLD_RE.search(line) and second >= BOARD_RERANK_HOLD_UNTIL:
                board_late_holds.append(second)
            found = DAILY_WARM_DONE_RE.search(line)
            if found:
                daily_warm_done.append((int(found.group(1)), int(found.group(2)), int(found.group(3)), bool(found.group(4))))
            if DAILY_WARM_FAIL_RE.search(line):
                daily_warm_fails += 1
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
            if ORDER_DIVISION_REJECT_RE.search(line):
                order_division_rejects += 1
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
            if found := DEVSCALE_TRAIL_RE.search(line):
                devscale_trails.append((second, found.group(1)))
            if found := PROTECTIVE_SHADOW_RE.search(line):
                protective_shadow.append((second, found.group(1)))
            if PROTECTIVE_FIRED_RE.search(line):
                protective_fired += 1

    starts = boot_starts(starts, process_count)

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
    # 보호 주문 표의 그림자 판정과 전략의 실제 청산을 종목·시각으로 맞댄다. 표가 보는 것은 손절과 트레일
    #  둘뿐이라 전략 쪽도 그 둘만 센다. 짝이 없는 쪽을 어긋남으로 세어 둔다.
    devscale_guard_exits = devscale_stops + devscale_trails
    shadow_without_exit = [(second, ticker) for second, ticker in protective_shadow
                           if not any(exit_ticker == ticker and abs(second - exit_second) <= PROTECTIVE_MATCH_SEC
                                      for exit_second, exit_ticker in devscale_guard_exits)]
    exit_without_shadow = [(second, ticker) for second, ticker in devscale_guard_exits
                           if not any(shadow_ticker == ticker and abs(second - shadow_second) <= PROTECTIVE_MATCH_SEC
                                      for shadow_second, shadow_ticker in protective_shadow)]
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

    # 시세 박동(D-137) — 이 칸은 전략 박동 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def feed_beat_row(name: str, ok: bool, level: str, detail: str):
        if feed_beat_gap_max < 0:
            return (name, True, level, "시세 박동 칸 없음(D-137 배포 전 바이너리) — 판정 안 함")

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

    # 티커→번호(D-114 단계 4) — 이 줄도 제어 요청 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def symbol_row(name: str, ok: bool, level: str, detail: str):
        if symbol_register_timeout < 0:
            return (name, True, level, "티커→번호 수치 줄 없음(D-114 단계 4 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 구독(D-114 단계 4 배선 ②) — 이 줄도 티커→번호 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def watch_row(name: str, ok: bool, level: str, detail: str):
        if watch_overflow < 0:
            return (name, True, level, "구독 수치 줄 없음(D-114 단계 4 배선 ② 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 시세 통로(D-114 단계 4 배선 2') — 이 줄도 구독 줄보다 늦게 붙었으므로 따로 건너뛴다.
    def feed_channel_row(name: str, ok: bool, level: str, detail: str):
        if feed_channel_overflow < 0 and feed_channel_discarded < 0:
            return (name, True, level, "시세 통로 수치 줄 없음(D-114 단계 4 배선 2' 배포 전 바이너리) — 판정 안 함")
        return (name, ok, level, detail)

    # 통로 흐름(D-114 단계 5) — 역할 셋(주문·전략·시세)으로 갈라 띄운 날에만 본다. 한 프로세스로 뜬 날은
    #  경계가 없어 통로가 아예 안 만들어지고, 그 칸이 없는 옛 바이너리도 같이 건너뛴다.
    three_roles = process_count >= 3

    def feed_flow_row(name: str, ok: bool, level: str, detail: str):
        if feed_channel_sent < 0 and feed_channel_received < 0:
            return (name, True, level, "시세 통로 흐름 수치 줄 없음(D-114 단계 5 배포 전 바이너리) — 판정 안 함")

        if not three_roles:
            return (name, True, level, "한 프로세스로 뜬 날 — 경계가 없어 판정 안 함")

        return (name, ok, level, detail)

    def fill_flow_row(name: str, ok: bool, level: str, detail: str):
        if fill_channel_sent < 0 and fill_channel_received < 0:
            return (name, True, level, "체결 통로 수치 줄 없음(D-114 단계 5 배포 전 바이너리) — 판정 안 함")

        if not three_roles:
            return (name, True, level, "한 프로세스로 뜬 날 — 경계가 없어 판정 안 함")

        return (name, ok, level, detail)

    # 보호 주문 대조(D-114 단계 1) — 표가 shadow 인 날만 맞댈 것이 있다. owner 로 올린 날이나
    #  걸린 규칙도 청산도 없던 날은 판정하지 않는다.
    def protective_row(name: str, ok: bool, level: str, detail: str):
        if protective_fired > 0:
            return (name, True, level,
                    f"표가 직접 낸 청산 {protective_fired}건(owner 모드) — 대조 판정 안 함")

        if not protective_shadow and not devscale_guard_exits:
            return (name, True, level, "그림자 판정도 전략 청산도 없던 날 — 대조 판정 안 함")

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
        # 시세가 죽으면 체결통보가 주문 쪽에 안 들어와 예약 수량이 안 풀린다(총노출 이중계상). 09-25 Kill_feed
        #  회차에서 죽여도 90초간 로그가 한 줄도 안 났던 자리다 — 이제 판정이 여기서 난다. [why D-137]
        feed_beat_row("시세 박동", feed_dead == 0, "FAIL",
                    f"사망 판정 {feed_dead}회 · 복귀 {feed_back}회 · 가장 긴 공백 {feed_beat_gap_max}ms"
                    f" (기대 0회, 사망 문턱 {FEED_BEAT_DEAD_MS}ms — 판정이 나면 그 사이 신규 진입이 막힌다)"),
        # 문턱 아래여도 의심 문턱을 넘은 날은 시세 소켓이나 제어 바퀴가 오래 붙들린 것이라 미리 본다.
        #  사망 문턱을 좁힐 근거도 이 값이다.
        feed_beat_row("시세 박동 여유", feed_beat_gap_max <= FEED_BEAT_SUSPECT_MS, "WARN",
                    f"가장 긴 공백 {feed_beat_gap_max}ms (의심 문턱 {FEED_BEAT_SUSPECT_MS}ms,"
                    f" 찍는 간격은 제어 바퀴 5초)"),
        # 포트를 못 잡은 엔진은 매매는 하면서 적재만 안 한다 — 로그에 ERROR 한 줄뿐이라 놓치기 쉽다.
        ("ZMQ 포트", zmq_bind_fail == 0, "FAIL",
                    f"bind 실패 {zmq_bind_fail}회 (기대 0 — 실패하면 그 엔진의 체결·시그널이"
                    f" TimescaleDB 에 하나도 안 들어간다. 계좌를 둘 돌리면 포트를 갈라야 한다: D-122)"),
        # 통로가 새면 같은 주문이 두 번 가거나 전략이 답을 영영 못 받아 기다림 표가 샌다.
        channel_row("주문 통로 무결", order_duplicate == 0 and order_response_dropped == 0, "FAIL",
                    f"중복 거름 {order_duplicate}건 · 버린 응답 {order_response_dropped}건 (둘 다 기대 0)"),
        # 요청 면에서 꺼낸 값이 말이 안 되면 그 신호는 주문이 되지 않고 사라진다 — 건너편이 덮였다는 뜻이라 FAIL이다.
        channel_row("요청 값 성함", order_implausible <= 0, "FAIL",
                    f"말이 안 돼 버린 요청 {order_implausible}건 (기대 0 — 0이 아니면 그만큼 주문이 안 나갔다)"),
        # 잘린 것은 주문이 아니라 판단 근거다. 주문은 나갔지만 원장 CSV의 "왜 샀나"가 짧아져 있다.
        channel_row("판단 근거 온전", order_truncated <= 0, "WARN",
                    f"칸을 넘어 잘린 신호 {order_truncated}건 (주문은 나갔다 · 원장 근거 글만 짧아진다)"),
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
        # 체결통보 큐가 가득 차면 버리지 않고 넘침 목록에 둔다(D-056). 버림은 0이어야 하고, 수위가 절반을
        #  넘으면 소비 스레드가 밀린 것이다. [why CODE_REVIEW W-1]
        ("체결통보 큐",
         fill_queue_high < 0 or (fill_dropped == 0 and fill_queue_high * 2 < fill_queue_capacity),
         "WARN",
         ("[큐 고수위] 줄 없음 — 판정 안 함" if fill_queue_high < 0 else
          f"최고 수위 {fill_queue_high}/{fill_queue_capacity} · 버림 {fill_dropped}건 · 잔고 대조가 메운 줄 {fill_absorbed}건 "
          "(버림 기대 0, 수위 절반 미만)")),
        # 넘침 목록을 쓴 날은 1024건이 밀린 날이다 — 체결은 늦게라도 반영되지만 소비 스레드가 멈췄던 것이라 본다.
        ("체결통보 넘침 목록",
         fill_overflowed == 0,
         "FAIL",
         f"넘침 목록에 둔 체결통보 {fill_overflowed}건 (기대 0 — 0이 아니면 체결 반영 스레드가 멈췄었다)"),
        control_row("제어 요청 표",
                    control_dropped == 0 and control_discarded == 0 and control_relay_dropped <= 0,
                    "FAIL",
                    f"못 보낸 줄 {control_dropped}건 · "
                    # 옮기는 자리가 붙기 전 바이너리는 이 칸이 없다 — 그때는 줄을 비워 두고 나머지 둘로만 본다.
                    + (f"못 옮긴 줄 {control_relay_dropped}건 · " if control_relay_dropped >= 0 else "")
                    + f"버린 줄 {control_discarded}건 (모두 기대 0)"),
        # 종목 표에 넣는 쪽은 주문 프로세스 하나다. 등록을 못 받으면 그 종목 줄을 접으므로 전략이 그 종목을
        #  아예 못 보고(신호 유실), 표에 없는 티커로 잦은 자리가 불리면 그 틱·신호가 번호 없이 버려진다.
        symbol_row("종목 번호 등록", symbol_register_timeout == 0 and symbol_lookup_miss == 0, "FAIL",
                   f"등록 못 받음 {symbol_register_timeout}건 · 표에 없는 티커 {symbol_lookup_miss}건 (둘 다 기대 0)"),
        # 전략 이름표도 넣는 쪽은 주문 프로세스 하나다. 등록을 못 받으면 그 전략의 주문이 번호 없이 나가고
        #  손익이 남의 칸이나 빈 칸에 붙는다 — 매매일지의 전략별 손익이 조용히 틀어진다.
        ("전략 이름 등록",
         True if strategy_register_timeout < 0 else strategy_register_timeout == 0,
         "FAIL",
         "전략 이름→번호 수치 줄 없음(D-114 단계 4 배선 ③ 배포 전 바이너리) — 판정 안 함"
         if strategy_register_timeout < 0
         else f"등록 못 받음 {strategy_register_timeout}건 (기대 0)"),
        # 구독은 KIS 상한(41건)에 걸리면 거절된다. 앱키가 계좌당 하나라 칸은 못 늘리고, 밀린 종목은 REST
        #  현재가로 받는다(D-132) — 그래서 밀린 것 자체는 정상이고, REST 대체조차 없는 종목만 FAIL이다.
        watch_row("구독 상한", watch_no_rest == 0, "FAIL",
                  f"소켓에 못 건 종목 {watch_overflow}건(REST로 받음) · REST 대체도 없는 종목 {watch_no_rest}건 (기대 0)"),
        # 칸은 보유 → 선점 → 점수 순으로 준다. 보유·선점 종목이 칸 밖이면 청산 판단이 REST 주기만큼 늦는다.
        #  칸 전부를 보유·선점이 쥔 날에만 생긴다(2칸 종목 20개면 찬다) — 보유 한도와 칸 수를 같이 볼 일이라 WARN.
        ("구독 칸 보유 우선",
         not websocket_slot_protected_off,
         "WARN",
         "칸 밖으로 밀린 보유·선점 종목 0건"
         if not websocket_slot_protected_off
         else f"칸 밖으로 밀린 보유·선점 종목 {len(websocket_slot_protected_off)}건 "
              f"({', '.join(sorted(set(websocket_slot_protected_off))[:5])}) — REST로만 받았다"),
        # 칸을 바꿀 때마다 해제·등록 프레임이 나가고 그 종목 틱이 잠깐 끊긴다. 유지 60초·순위 차 10·한 번 4종목
        #  문턱이 있어 하루 수십 번이면 정상이다. 넘으면 문턱(websocket_slot::Rules)을 다시 본다.
        ("구독 칸 교체",
         websocket_slot_releases <= WS_SLOT_RELEASE_LIMIT,
         "WARN",
         f"칸 내줌 {websocket_slot_releases}회 (기대 {WS_SLOT_RELEASE_LIMIT}회 이하)"),
        # 갈라 띄우면 시세는 주문 쪽 소켓에서 통로를 지나 전략 쪽으로 간다. 통로가 차서 버린 건은 그 종목의
        #  체결·호가가 전략에 아예 안 닿은 것이고, 말이 안 돼 버린 건은 건너편 프로세스를 의심할 일이다.
        feed_channel_row("시세 통로", feed_channel_overflow <= 0 and feed_channel_discarded <= 0, "FAIL",
                         f"못 넘긴 시세 {max(feed_channel_overflow, 0)}건 · 값이 이상해 버린 시세 "
                         f"{max(feed_channel_discarded, 0)}건 (둘 다 기대 0)"),
        # 버린 건수가 0이어도 한 건도 안 지나갔으면 통로가 안 붙은 것이다. 갈라 띄운 날에 둘 다 0이면
        #  전략은 그날 틱을 하나도 못 본 채 돌았다 — 신호가 아예 안 났으므로 조용한 실패다.
        feed_flow_row("시세 통로 흐름", feed_channel_sent > 0 or feed_channel_received > 0, "FAIL",
                      f"보낸 시세 {max(feed_channel_sent, 0)}건 · 꺼낸 시세 {max(feed_channel_received, 0)}건"
                      " (둘 다 0이면 시세가 경계를 못 넘어 전략이 틱을 하나도 못 봤다)"),
        # 체결통보는 시세 소켓에 같이 실려 온다. 시세 쪽이 주문 쪽으로 넘기지 못하면 주문 쪽은 자기가 낸
        #  주문이 체결된 줄을 모른다 — 선점분(reserved_)이 안 풀려 총노출을 이중계상하고 원장도 빈다.
        fill_flow_row("체결 통로", not fills or fill_channel_received > 0, "FAIL",
                      f"체결통보 {len(fills)}건 · 통로로 보낸 {max(fill_channel_sent, 0)}건 ·"
                      f" 꺼낸 {max(fill_channel_received, 0)}건"
                      " (체결이 난 날에 꺼낸 건수가 0이면 주문 쪽 선점분이 안 풀린다)"),
        # 보호 주문 표는 아직 shadow — 판정만 남기고 발주는 전략이 한다. 표의 판정과 전략의 청산이 같은
        #  종목·같은 자리에서 나는 날이 쌓여야 owner 로 올릴 수 있다. 어긋나면 표가 먼저 보거나 놓친 것이다.
        protective_row("보호 주문 대조",
                       not shadow_without_exit and not exit_without_shadow, "WARN",
                       f"그림자 판정 {len(protective_shadow)}건 · 전략 청산 {len(devscale_guard_exits)}건 · "
                       f"어긋남 {len(shadow_without_exit) + len(exit_without_shadow)}건"
                       f"(판정만 {len(shadow_without_exit)} · 청산만 {len(exit_without_shadow)}, 맞대는 폭 ±{PROTECTIVE_MATCH_SEC}초)"
                       + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in (shadow_without_exit + exit_without_shadow)[:5])}"
                          if shadow_without_exit or exit_without_shadow else "")
                       + " | 다음에 볼 곳: docs/DECISIONS.md D-114 '남은 것' — 어긋남 0인 날이 며칠 쌓이면"
                         " protective_orders 를 owner 로 올린다"),
                # 체결통보는 WS 세션 하나만 들어야 한다. 아무도 안 들으면 체결이 원장에 안 들어와 선점이 안 풀리고,
        #  둘이 들으면 KIS가 세션마다 같은 통보를 보내 원장이 체결을 두 번 센다 — 둘 다 A등급이다.
        fill_session_row("체결 세션", fill_session_many == 0 and fill_session_none == 0, "FAIL",
                         (f"소켓 {fill_session_socket}번이 맡는다" if fill_session_socket >= 0 else "맡은 소켓 없음")
                         + f" · 맡은 곳 없음 {fill_session_none}회 · 둘 이상 {fill_session_many}회 (둘 다 기대 0)"),
        # 재연결 직후 KIS가 같은 체결통보를 다시 보내면 원장에 안 넣고 버린다. 같은 초·같은 수량·같은 값의 실체결이
        #  마침 재연결 순간에 오면 그것도 버려지므로, 0이 아니면 그날 잔고 대조가 수량을 맞췄는지 본다.
        ("체결 재전송", fill_replayed == 0, "WARN",
         f"재연결 뒤 다시 온 체결통보 {fill_replayed}건을 원장에 안 넣음 (기대 0 — 있으면 잔고 대조가 수량을 맞춘다)"),
        ("체결통보 생산자 하나", fill_producer_overlap == 0, "FAIL",
         f"체결통보를 두 스레드가 같이 넣으려 한 {fill_producer_overlap}건 (기대 0 — 한 줄로 세워 넣었지만 생산자가 둘 생긴 것이다)"),
        devscale_v2_row("장 마감 청산(넘김)", not devscale_close_exits, "FAIL",
                        f"DEVSCALE 장 마감 청산 신호 {len(devscale_close_exits)}건 (기대 0 — market_close_exit_hhmm 2400, D-111)"
                        + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in devscale_close_exits[:5])}" if devscale_close_exits else "")),
        devscale_v2_row("진입 필터 판정", entry_filter["차단"] > 0, "WARN",
                        f"진입 필터 통과 {entry_filter['통과']} / 차단 {entry_filter['차단']} 종목 (리플레이 기대: 존 안 종목의 절반쯤 차단, 차단 0이면 필터 값이 안 실린 것)"),
        # 리눅스 실행일(09-22~)은 Windows 감시견이 -NoTrader라 Windows 기동이 0이어야 한다. 둘이 섞이면 같은 계좌에 엔진 둘.
        ("실행 플랫폼", len(set(platforms)) <= 1, "FAIL",
         ("·".join(f"{name} {platforms.count(name)}회" for name in sorted(set(platforms)))
          + " (엔진이 뜬 기동만)") if platforms else "플랫폼 줄 없음(구 exe)"),
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
        ("DB 적재(엔진)",
         db_writer_no_password == 0 and (db_writer_starts == 0 or sum(db_writer_counts[2:]) == 0),
         "FAIL" if db_writer_no_password else "WARN",
         f"database.enabled인데 TSDB_PASSWORD가 없어 체결을 넣지 않은 기동 {db_writer_no_password}회"
         " — 리코더도 --record-ticks 없이 떠서 그날 ticks가 빈다(저장소 루트 .env 확인)" if db_writer_no_password else
         "적재기 꺼짐 — 판정 안 함" if db_writer_starts == 0 else
         f"받음 {db_writer_counts[0]} · 넣음 {db_writer_counts[1]} · 큐 넘쳐 버림 {db_writer_counts[2]}"
         f" · 거절 {db_writer_counts[3]} · 모호 {db_writer_counts[4]} (기대: 버림·거절·모호 0."
         f" 모호는 들어갔는지 모르는 행, 종료 줄이 없는 기동은 세지 못한다)"),
        ledger_row("원장 재기동 대조", ledger_released == 0 and ledger_truncated == 0, "WARN",
                   f"되살림 {ledger_restored}건(접수 응답 전 끊김 짝지음 {ledger_unnumbered}) · 선점해제 {ledger_released}건 · 꼬리 잘림 {ledger_truncated}회"
                   f" (리플레이 최대 {max(ledger_replays, default=0)}건 — 선점해제는 원장에 적고 KIS엔 안 간 주문,"
                   f" 꼬리 잘림은 쓰다 만 레코드)"),
        ledger_row("하루 리셋 한 번", all(count == 1 for count in daily_resets.values()), "FAIL",
                   f"거래일별 리셋 {daily_resets or '줄 없음'} (기대 각 1회 — 2회 이상이면 재기동·US 개장이"
                   f" 되살린 선점과 당일 손익을 지웠다)"),
        ledger_row("미체결 조회", open_order_fails == 0, "WARN",
                   f"조회 실패 {open_order_fails}건 (기대 0 — 실패한 회차는 접수된 주문을 살려 두고 대조를 건너뛴다)"),
        ("일찍 끝난 세션", not short, "FAIL",
         f"{MIN_SESSION_SEC}초 미만 종료 {len(short)}회"
         + (f" — {', '.join(hhmm(second) for second in short[:5])}" if short else "")),
        ("재기동 투매", not dump, "FAIL",
         f"청산 관리 부착 {GUARD_QUIET_SEC}초 내 본전탈출 {len(dump)}건"
         + (f" — {', '.join(hhmm(second) for second in dump[:5])}" if dump else "")),
        ("매도→재매수 회전", churn <= MAX_CHURN, "FAIL",
         f"{CHURN_SEC}초 내 반대매매 {churn}회 (허용 {MAX_CHURN})"),
        ("엔진밖 미체결", not untracked_opens, "FAIL",
         f"부속 파일에 없던 브로커 미체결 {len(untracked_opens)}건"
         " — 전송 타임아웃 난 주문이 실제로는 접수돼 엔진 장부 밖에 살아 있었다는 뜻이다."
         " 그 종목은 보유분이 묶여 손절이 닿아도 못 판다"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in untracked_opens[:5])}"
            if untracked_opens else "")),
        ("청산차단 해소", not blocked_sells, "FAIL",
         f"청산차단 미해소 {len(blocked_sells)}건 — 예약매도를 못 찾아 청산이 막힌 채 넘어갔다"
         + (f" — {', '.join(f'{hhmm(second)} {ticker}' for second, ticker in blocked_sells[:5])}"
            if blocked_sells else "")),
        ("초당한도 압박", rate_hits <= MAX_RATE_HITS, "WARN",
         f"초당 거래건수 거부 {rate_hits}건 (허용 {MAX_RATE_HITS})"),
        # 유니버스 후보의 한 축이다. ETF가 섞이면 그만큼 개별주 자리가 밀리고, 요청보다 적게 오면
        #  가격 구간을 갈라 합치는 쪽이 한 페이지에서 멈춘 것이다(30행이 API 상한이라 그 위는 합쳐야 한다).
        ("거래대금 랭킹 폭", value_rank_etf_drops == 0 and not value_rank_short, "WARN",
         f"ETF 섞임 {value_rank_etf_drops}행 (기대 0 — 제외 마스크가 먹으면 0)"
         + (f" · 요청보다 모자란 회차 {len(value_rank_short)}건"
            f" (가장 적을 때 {min(short[0] for short in value_rank_short)}/"
            f"{max(short[1] for short in value_rank_short)}행)" if value_rank_short else " · 행수 모자람 없음")),
        # 시총 축을 대신하는 후보 축(D-146). 한 번도 안 돌았으면 설정(turnover_top_n)이 빠졌거나 스캔이 안 돈 것이고,
        #  적게 뽑힌 회차는 시세 파일이 비었거나 낡은 것이다.
        ("거래대금 상위 축", bool(turnover_axis_taken) and min(turnover_axis_taken) >= TURNOVER_AXIS_MIN, "WARN",
         (f"재스캔 {len(turnover_axis_taken)}회, 뽑은 종목 최소 {min(turnover_axis_taken)} · 최대 {max(turnover_axis_taken)}"
          f" (기대 최소 {TURNOVER_AXIS_MIN} 이상)") if turnover_axis_taken
         else "거래대금 상위 축 로그 없음 — turnover_top_n 설정이 빠졌거나 DEVSCALE 스캔이 안 돌았다"),
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
        ("전 종목 시세 낡음", not prices_stale_ages, "WARN",
         f"시세 파일 낡음 경고 {len(prices_stale_ages)}건"
         + (f" (최대 {max(prices_stale_ages)}초 전 갱신)" if prices_stale_ages else "")
         + " — 시세판(엔진 안, 5초 주기)이나 보조 프로세스가 멈추면 정배열·이격 판정이 전일 종가로 얼어붙는다"),
        ("시세판 받기 실패", board_sweep_fails <= MAX_BOARD_SWEEP_FAILS, "WARN",
         f"시세판 한 바퀴 전부 실패 {board_sweep_fails}회 (허용 {MAX_BOARD_SWEEP_FAILS} = 1분치) — 넘으면 네이버 응답이 끊긴 것"),
        ("시세판 재랭킹 보류", not board_late_holds, "WARN",
         f"09:10 뒤 재랭킹 보류 {len(board_late_holds)}회"
         + (f" — {', '.join(hhmm(second) for second in board_late_holds[:5])}" if board_late_holds else "")
         + " (기대 0 — 거래대금이 절반 넘는 종목에 잡힌 뒤에도 보류면 시세 응답의 거래대금 칸이 비는 것)"),
        ("장 전 일봉 데우기", not daily_warm_fails and not any(done[3] for done in daily_warm_done), "WARN",
         (f"마지막 회 {daily_warm_done[-1][0]}/{daily_warm_done[-1][1]}종목 {daily_warm_done[-1][2]}초"
          + (" — 마감(08:00)에 멈춤" if daily_warm_done[-1][3] else "") if daily_warm_done else "끝 줄 없음(꺼짐 또는 마감 뒤 기동)")
         + (f", 목록·인증 실패로 건너뜀 {daily_warm_fails}회" if daily_warm_fails else "")
         + " — 못 끝내면 첫 스캔이 후보 일봉을 그 자리에서 받느라 늦어진다"),
        ("주문 접수 지연", orders_ok, "WARN", order_detail),
        ("잔고 조회 지연", http_timeouts <= MAX_HTTP_TIMEOUTS, "WARN", recon_detail),
        ("TRENDX 정지", max(trendx_registered, default=0) == 0, "FAIL",
         (f"초기 등록 최대 {max(trendx_registered)}종목 (기대 0, D-101 결정 2)" if trendx_registered
          else "TRENDX 등록 줄 없음 — 전략 미로드 또는 등록 0")),
        ("매매 창 밖 거부", session_window_rejects == 0, "FAIL",
         f"세션 창 밖 거부 {session_window_rejects}건 (기대 0 — 마감 청산 모의 15:15·실계좌 19:50, D-101 결정 3)"),
        ("주문구분 거부", order_division_rejects == 0, "FAIL",
         f"주문구분을 안 받아 되돌아온 거부 {order_division_rejects}건 (기대 0 — 정규장 밖 시장가는 지정가+현재가로 나간다)"),
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
    ]

    if include_global:
        rows.extend(global_rows(date))

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

    if arguments.log != str(DEFAULT_LOG):
        targets = [log] if log.exists() else []
    else:
        # 계좌마다 로그 폴더가 다르다. 기본값 하나만 보면 _logdir.log_dir() 이 그날 마지막으로
        #  쓰인 폴더를 고르므로, 실계좌를 돌린 날에도 모의 로그만 판정하는 일이 생긴다
        #  (2026-09-23 실계좌 첫날 실측 — FAIL 7건 중 실계좌 것은 하나였는데 구분이 안 됐다).
        #  계좌 폴더를 모두 돌고 계좌별로 낸다. [why D-097]
        targets = health_targets()

    if not targets:
        print(f"로그 없음: {log}")
        return 1

    scope = f" {arguments.since}~" if arguments.since else ""
    print(f"=== 실행 건전성 점검 {arguments.date}{scope} ===")

    bad = 0
    seen_session = False

    for target in targets:
        rows, session_count = collect(arguments.date, target, since, include_global=False)

        if not rows:
            continue

        seen_session = True
        print(f"-- 계좌 {account_name(target)} (세션 {session_count}회) --")
        bad += print_rows(rows)

    if not seen_session:
        print(f"{arguments.date}: 엔진 시작 기록이 없다 — 점검할 세션이 없음")
        return 0

    print("-- 계좌 무관 --")
    bad += print_rows(global_rows(arguments.date))

    print(f"--- FAIL {bad}건 ---")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
