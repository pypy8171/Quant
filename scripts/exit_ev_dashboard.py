#!/usr/bin/env python3
"""study 17(청산 사유별 조건부 기대값)을 눈으로 확인하는 정적 HTML 한 장 — 원장 통계·매매 규칙·백테스트 세 탭.

scripts/exit_ev.py 의 적재·통계 함수를 그대로 쓰고, 표 한 줄마다 "왜 이 숫자가 나왔나"를 날짜별·종목별·레그별로
펼쳐 볼 수 있게 한다. 판정이 안 된 셀도 이유와 근거 레그를 같이 보인다 — 판정 결과보다 근거를 보는 화면이다.
규칙 탭은 실행 중인 config 값을 읽어 DEVSCALE·승계분(ITB)이 어떤 지표의 어떤 수치로 사고 파는지 적고,
각 청산 규칙이 원장에서 어떤 결과를 냈는지 같은 표의 셀로 잇는다. 백테스트 탭은 research/studies/**/metrics.json.

    py scripts/exit_ev_dashboard.py                      # 마지막 날 = 원장 최신 파일. research/studies/17_exit_ev/exit_ev_dashboard.html
    py scripts/exit_ev_dashboard.py --last-day 20260925  # 날짜를 고정할 때

매일 장 마감 뒤 scripts/refresh_dashboard.py 가 부르고(예약작업 Quant Market Close AutoDoc), 발행본 갱신은 /dashboard-sync 4단계.
데이터는 생성 시점에 본문 JSON으로 박힌다(대시보드 관례와 같다). 손으로 HTML을 고치지 않는다.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import re
import statistics
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "PYQuant" / "dashboard"))
import _logdir  # noqa: E402
import exit_ev  # noqa: E402
import gen_tuning_sheet  # noqa: E402
from build_dashboard import NAME_MAP as STUDY_NAME_MAP  # noqa: E402  (BT-NN → 서술형 이름, 한 곳만 정본)

DEFAULT_HTML = exit_ev.DEFAULT_OUT / "exit_ev_dashboard.html"
HISTOGRAM_EDGES = (-8.0, -5.0, -3.0, -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 3.0, 5.0, 8.0)
UNIVERSE_SCAN = exit_ev.REPO / "Quant" / "config" / "universe_scan.json"
TICKER_NAMES = exit_ev.REPO / "Quant" / "config" / "ticker_names.json"
TRADER_LOG = exit_ev.REPO / "Quant" / "build_win" / "logs" / "quant_trader.log"
STUDIES_DIR = exit_ev.REPO / "research" / "studies"
NAME_RE = re.compile(r"(\d{6})\(([^)]{1,24})\)")  # market_close_autodoc.py 와 같은 모양 — 엔진 로그의 `123456(종목명)`
KIS_NAME_RE = re.compile(r'"(?:stck|mksc)_shrn_iscd":"(\d{6})"[^{}]*?"hts_kor_isnm":"([^"]{1,24})"')  # KIS 응답 JSON 한 항목
LEDGER_FILE_RE = re.compile(r"trades_(\d{8})\.csv$")
GATE_WORDS = exit_ev.REPO / "_private" / "gate_words.txt"  # commit_gate.py 비공개 단어 — 이 낱말이 든 종목명은 코드만 남긴다

# 판정 문자열 → 화면에 보이는 뜻. README 1절 판정 규칙을 사람 말로 옮긴 것.
VERDICT_MEANING = {
    "판정 제외(부호 고정)": "규칙 정의상 손익 부호가 정해진다(익절이면 +, 손절이면 −). 승률·부호는 정보가 아니고 크기와 빈도만 본다.",
    "판정 제외(전략 아님)": "전략 판단이 아니라 슬롯 교체·귀속 불명·기동 정리 같은 운영 묶음이다. 표에 남기되 규칙 평가는 하지 않는다.",
    "판정불가(한쪽 구간 없음)": "구간 A(09-08~10)와 B(09-11~) 중 한쪽에 레그가 없다. 한 구간만으로는 config 변경 효과와 규칙 효과를 못 가른다.",
    "판정불가(N일<3)": "어느 구간이든 거래일이 3일 미만이다. 날 단위 부트스트랩 CI가 두세 날 값의 범위에 지나지 않아 판정에 못 쓴다.",
    "기대값 음수": "두 구간 모두 수익률 CI 상단이 0 아래다. 이 규칙으로 나간 청산은 평균적으로 손실이다.",
    "기대값 양수": "두 구간 모두 수익률 CI 하단이 0 위다. 이 규칙으로 나간 청산은 평균적으로 이익이다.",
    "보류": "두 구간의 CI가 같은 쪽에 있지 않다. 규칙 효과인지 종목·시장 탓인지 이 표만으로는 못 가른다.",
}

CLAIMS = (
    {
        "claim": "데이터와 지표로 확률을 만들어 기준으로 삼고",
        "evidence": "원장 탭의 승률·평균손익·수익률과 95% CI가 그 확률이다. 셀을 누르면 어느 날·어느 종목·어느 레그가 그 숫자를 만들었는지 다 펼쳐진다. 규칙 탭은 그 매매가 어떤 지표의 어떤 수치로 들어가고 나왔는지를 실행 config 값으로 적는다.",
        "limit": "표본이 짧아 CI가 넓다. 기준으로 '삼았다'가 아니라 '삼으려고 이렇게 재고 있다'까지가 사실이다.",
    },
    {
        "claim": "판단 근거를 기록하고",
        "evidence": "청산 한 건마다 원장에 사유 문자열(익절밴드·손절·장 마감·seed-trail…)이 남고, 이 화면은 그 사유를 범주로 묶어 집계한다. 사유가 없는 청산은 '기타'로 따로 센다. 매일 장 마감 뒤 자동으로 다시 만든다.",
        "limit": "매수 근거(어느 조건이 맞아 샀는지)는 원장에 없고 규칙 탭의 config 값으로만 안다. 매수↔매도 짝짓기는 다음 단계.",
    },
    {
        "claim": "결과가 나쁜 이유를 확인할 수 있게",
        "evidence": "판정이 안 난 셀도 이유(구간 없음·N일 부족·부호 고정)와 함께 날짜별 손익·상위 손실 종목·레그 분포를 그대로 보인다. 예: ITB seed-trail(보류)을 펼치면 손실 상위 3레그가 합계의 6할을 넘고 구간 A·B 부호가 갈린다는 것이 보인다.",
        "limit": "'안 잘랐으면 어땠나'는 이 표에 없다 — 가격 경로 리플레이(study 16)가 붙어야 규칙 탓인지 가른다.",
    },
)

# 지표 정의 — 규칙 탭 머리. 코드 정본은 Quant/include/strategy/DeviationScaleStrategy.h 설계 주석(25~57행).
INDICATORS = (
    ("정배열", "일봉 종가 단순이동평균 SMA5 > SMA10 > SMA20 (60일선 조건은 D-141에서 뺐다). 마지막 조건(SMA10>SMA20)만 align_ma_tol_pct 허용오차. 전일까지의 일봉 250개(daily_lookback)에 오늘 현재가를 최신 봉 자리로 접어 넣어 재스캔마다 다시 본다."),
    ("이격(%)", "(현재가 ÷ 일봉 SMA20 − 1) × 100. 진입 존은 이 값의 구간이다 — DEVSCALE은 SMA20 아래로 눌린 쪽(−pullback_pct ~ +entry_upper_pct)."),
    ("존 히스테리시스", "존 경계에 zone_hyst_pct 를 더한 폭을 벗어나야 '존 이탈'로 본다. 경계에서 들락거리며 사고팔기를 막는다."),
    ("3분봉 SMA", "주문 가격 기준. interval_min 분봉 sma_period 개의 단순이동평균."),
    ("스캔 점수", "장중 유니버스는 거래대금·등락률·섹터 순위로 점수를 매겨 score_top_n 까지 가져온다(score_w_vol·score_w_liquidity 가중). 가격 min_price 원 미만·거래대금 min_turnover 원 미만은 뺀다."),
)

# 전략별 규칙 — {키}는 실행 config 값으로 채운다. exit_category 가 있으면 원장 탭의 그 셀 결과를 옆에 붙인다.
RULES = (
    {
        "family": "DEVSCALE", "index": 0, "title": "DEVSCALE — 눌림 되돌림", "one_line": "일봉 정배열 종목이 SMA20 근처로 눌렸을 때 3분봉 SMA 기준으로 사고, 평단 위에서 판다. 평단 대비 손절과 존 이탈로 나가고, 장 마감 청산 시각이 2400이면 팔지 않고 다음 날로 넘긴다.",
        "steps": (
            {"phase": "후보", "what": "장중 스캔 상위 {scan_top_n} → 점수 상위 {score_top_n}(max_universe {max_universe}). 가격 {min_price}원 이상, 거래대금 {min_turnover}원 이상, 코스닥 포함({kosdaq_enabled}). 지수가 {risk_off_index_pct}% 아래면 신규 진입 안 함."},
            {"phase": "하루 진입 필터", "what": "전일 ATR14/SMA20이 {entry_atr_max_pct}%를 넘거나 개장 봉 이격이 {entry_open_dev_min_pct}% 아래인 날은 새로 사지 않는다(보유분 관리는 그대로)."},
            {"phase": "진입 조건", "what": "정배열이고 이격이 −{pullback_pct}% ~ +{entry_upper_pct}% 안(존, max_dev_pct {max_dev_pct}). 존 히스테리시스 {zone_hyst_pct}%."},
            {"phase": "매수 주문", "what": "3분봉(interval_min {interval_min}) SMA{sma_period} 기준 지정가. SMA 아래에서만(add_below_sma_only {add_below_sma_only}). 추가 매수 없음(buy_split_steps {buy_split_steps}). 금액 = 자산 × {base_pct} (바닥 {notional_floor_krw}원 ~ 상한 {notional_cap_krw}원, 스프레드 가중 {weight_spread})."},
            {"phase": "익절", "what": "기준점 + {dev_sell_pct}% × 단계, {split_step_count}단 지정가 매도. 기준점은 sell_base_average {sell_base_average}(켬 = 평단, 끔 = 3분봉 SMA). 체결 뒤 {reentry_cooldown_sec}초 재진입 금지.", "exit_category": "익절밴드"},
            {"phase": "손절", "what": "평단 − {stop_loss_pct}% 에 닿으면 존 상태와 무관하게 시장가 전량(0이면 끔). 뒤 {stop_cooldown_sec}초 재진입 금지.", "exit_category": "손절"},
            {"phase": "존 이탈", "what": "정배열이 깨지거나 이격이 존±히스테리시스 밖 → 시장가 청산.", "exit_category": "존이탈"},
            {"phase": "장 마감", "what": "{market_close_exit_hhmm} 에 전량 청산. 2400이면 팔지 않고 다음 날로 넘긴다(D-111).", "exit_category": "장마감"},
            {"phase": "먼지", "what": "평가금액 {dust_krw}원 미만 잔량 정리.", "exit_category": "먼지정리"},
        ),
    },
    {
        "family": "ITB", "index": 0, "sub": "manage_holdings", "title": "ITB — 승계 보유분 관리", "one_line": "신호 전략이 아니다. 재기동·이전 세션에서 넘어온 보유분을 평단 기준 트레일·본전탈출·하드스탑으로 정리하는 래퍼.",
        "steps": (
            {"phase": "대상", "what": "enabled {enabled} — 기동 시 잔고에 있으나 이 세션이 사지 않은 종목. 기동 뒤 {guard_warmup_sec}초는 손대지 않는다."},
            {"phase": "seed 트레일", "what": "고점 대비 −{seed_trail_pct}% 되돌리면 청산.", "exit_category": "seed-trail"},
            {"phase": "본전탈출", "what": "평단 +{exit_near_avg_arm_pct}% 를 한 번 넘긴 뒤(arm) 평단 ±{exit_near_avg_pct}% 로 돌아오면 청산.", "exit_category": "본전탈출"},
            {"phase": "하드스탑", "what": "평단 −{seed_hard_pct}% 를 {seed_hard_confirm_bars}봉 연속 확인하면 청산({seed_hard_from_hhmm} 이후, −{seed_hard_skip_pct}% 넘게 빠진 건 건너뜀).", "exit_category": "trail"},
            {"phase": "장 마감", "what": "{market_close_exit_hhmm} 에 전량 청산.", "exit_category": "장마감"},
        ),
    },
    {
        "family": "DISPLACE", "risk": True, "title": "DISPLACE — 슬롯 교체(운영)", "one_line": "전략이 아니라 리스크 게이트. 동시 보유 {max_concurrent_positions}슬롯이 찼을 때 점수가 더 높은 후보가 오면 가장 낮은 보유분을 밀어낸다.",
        "steps": (
            {"phase": "조건", "what": "displace_enabled {displace_enabled}. 새 후보 점수(z) − 보유 최저 z ≥ {displace_min_z_gap}, 보유 {displace_min_hold_sec}초 이상, 같은 슬롯 교체 뒤 {displace_cooldown_sec}초 쿨다운. 하루 상한 {displace_max_per_day}(0 = 없음)."},
            {"phase": "교체 청산", "what": "밀려나는 보유분은 시장가 청산 — 원장 사유 '교체 진입'.", "exit_category": "교체진입"},
        ),
    },
)


def nan_to_none(value: float) -> float | None:
    return None if value != value else value


def stats_row(legs: list[exit_ev.ExitLeg], rounds: int) -> dict:
    row = exit_ev.cell_stats(legs, 0.0)
    row.update(exit_ev.day_block_bootstrap(legs, 0.0, rounds, exit_ev.BOOTSTRAP_SEED))
    return {name: (nan_to_none(value) if isinstance(value, float) else value) for name, value in row.items()}


def leg_return_pct(leg: exit_ev.ExitLeg) -> float:
    return leg.pnl / leg.cost_basis * 100.0 if leg.cost_basis > 0.0 else 0.0


def latest_ledger_day(ledger_dir: Path) -> str:
    days = [match.group(1) for path in ledger_dir.glob("trades_*.csv") if (match := LEDGER_FILE_RE.search(path.name))]
    return max(days) if days else exit_ev.LAST_DAY


def load_names(tickers: set[str]) -> dict[str, str]:
    """종목명 — 스캔 유니버스 → 수기 표 → 엔진 로그의 `123456(이름)` 순. 못 찾은 것은 코드만 남는다."""
    names: dict[str, str] = {}
    if UNIVERSE_SCAN.exists():
        for item in json.loads(UNIVERSE_SCAN.read_text(encoding="utf-8-sig")).get("universe", []):
            if item.get("ticker") and item.get("name"):
                names.setdefault(item["ticker"], item["name"])

    if TICKER_NAMES.exists():
        for ticker, name in json.loads(TICKER_NAMES.read_text(encoding="utf-8-sig")).items():
            names.setdefault(ticker, name)

    missing = tickers - names.keys()
    if missing and _logdir.log_sources(None, TRADER_LOG.parent):
        # 7일 지난 날의 줄은 archive/*.log.gz 에 있다(maintain.py --rotate-logs) — 옛 날짜에만 나온 종목 이름도 거기서 찾는다
        with contextlib.closing(_logdir.iter_log_lines(None, TRADER_LOG.parent)) as handle:
            for line in handle:
                if "(" not in line and "hts_kor_isnm" not in line:
                    continue

                for ticker, name in NAME_RE.findall(line) + KIS_NAME_RE.findall(line):
                    if ticker in missing:
                        names[ticker] = name
                        missing.discard(ticker)

                if not missing:
                    break

    # 회사명이 게이트 단어와 겹치면(같은 이름의 상장사) 이름을 빼고 코드만 둔다 — 산출 HTML이 커밋 게이트에 걸린다.
    if GATE_WORDS.exists():
        gate_words = [word.strip() for word in GATE_WORDS.read_text(encoding="utf-8").splitlines()
                      if word.strip() and not word.startswith("#")]
        names = {ticker: name for ticker, name in names.items() if not any(word in name for word in gate_words)}

    return names


def by_day_rows(legs: list[exit_ev.ExitLeg]) -> list[dict]:
    grouped: dict[str, list[exit_ev.ExitLeg]] = defaultdict(list)
    for leg in legs:
        grouped[leg.day].append(leg)

    rows = []
    for day in sorted(grouped):
        day_legs = grouped[day]
        cost = sum(leg.cost_basis for leg in day_legs)
        pnl = sum(leg.pnl for leg in day_legs)
        rows.append({
            "day": day,
            "n_legs": len(day_legs),
            "wins": sum(1 for leg in day_legs if leg.pnl > 0.0),
            "pnl": pnl,
            "return_pct": pnl / cost * 100.0 if cost > 0.0 else 0.0,
            "segment": "A" if day <= exit_ev.SEGMENT_A_LAST_DAY else "B",
        })

    return rows


def by_ticker_rows(legs: list[exit_ev.ExitLeg], names: dict[str, str], limit: int = 12) -> list[dict]:
    grouped: dict[str, list[exit_ev.ExitLeg]] = defaultdict(list)
    for leg in legs:
        grouped[leg.ticker].append(leg)

    rows = []
    for ticker, ticker_legs in grouped.items():
        cost = sum(leg.cost_basis for leg in ticker_legs)
        pnl = sum(leg.pnl for leg in ticker_legs)
        rows.append({
            "ticker": ticker,
            "name": names.get(ticker, ""),
            "n_legs": len(ticker_legs),
            "wins": sum(1 for leg in ticker_legs if leg.pnl > 0.0),
            "pnl": pnl,
            "return_pct": pnl / cost * 100.0 if cost > 0.0 else 0.0,
            "days": sorted({leg.day for leg in ticker_legs}),
        })

    rows.sort(key=lambda row: abs(row["pnl"]), reverse=True)
    return rows[:limit]


def histogram(legs: list[exit_ev.ExitLeg]) -> list[dict]:
    returns = [leg_return_pct(leg) for leg in legs]
    edges = (float("-inf"),) + HISTOGRAM_EDGES + (float("inf"),)
    bins = []
    for low, high in zip(edges[:-1], edges[1:]):
        count = sum(1 for value in returns if low <= value < high)
        label_low = "" if low == float("-inf") else f"{low:+.1f}"
        label_high = "" if high == float("inf") else f"{high:+.1f}"
        bins.append({"low": label_low, "high": label_high, "count": count, "negative": high <= 0.0})

    return bins


def why_text(family: str, category: str, legs: list[exit_ev.ExitLeg], all_row: dict,
             segment_a: dict | None, segment_b: dict | None, names: dict[str, str]) -> list[str]:
    """숫자를 만든 원인을 데이터에서 뽑아 문장으로. 해석이 아니라 분해다."""
    lines: list[str] = []
    total = all_row["total_pnl"]
    days = by_day_rows(legs)
    tickers = by_ticker_rows(legs, names, limit=3)
    losses = sorted((leg for leg in legs if leg.pnl < 0.0), key=lambda leg: leg.pnl)
    gains = sorted((leg for leg in legs if leg.pnl > 0.0), key=lambda leg: leg.pnl, reverse=True)

    def label(ticker: str) -> str:
        return f"{names[ticker]}({ticker})" if ticker in names else ticker

    lines.append(
        f"{all_row['n_legs']}레그가 {all_row['n_days']}거래일에 걸쳐 합계 {total:+,.0f}원. "
        f"승률 {all_row['win_rate'] * 100:.0f}%, 레그당 평균 {all_row['mean_pnl']:+,.0f}원, 중앙값 {all_row['median_pnl']:+,.0f}원."
    )
    if all_row["mean_pnl"] != 0.0 and abs(all_row["median_pnl"]) < abs(all_row["mean_pnl"]) * 0.5:
        lines.append("평균과 중앙값이 크게 다르다 — 소수의 큰 레그가 평균을 끈다. 아래 상위 레그를 본다.")

    if days:
        worst = min(days, key=lambda row: row["pnl"])
        best = max(days, key=lambda row: row["pnl"])
        if total < 0.0 and worst["pnl"] < 0.0:
            share = worst["pnl"] / total * 100.0
            lines.append(f"손실이 가장 큰 날은 {worst['day']}({worst['pnl']:+,.0f}원, {worst['n_legs']}레그) — 합계의 {share:.0f}%.")

        if total > 0.0 and best["pnl"] > 0.0:
            share = best["pnl"] / total * 100.0
            lines.append(f"이익이 가장 큰 날은 {best['day']}({best['pnl']:+,.0f}원, {best['n_legs']}레그) — 합계의 {share:.0f}%.")

        negative_days = sum(1 for row in days if row["pnl"] < 0.0)
        lines.append(f"{len(days)}일 중 손실로 끝난 날 {negative_days}일.")

    if losses and total < 0.0:
        top = losses[:3]
        top_sum = sum(leg.pnl for leg in top)
        text = ", ".join(f"{label(leg.ticker)} {leg.day[4:6]}-{leg.day[6:]} {leg.pnl:+,.0f}" for leg in top)
        lines.append(f"손실 상위 3레그 — {text} — 가 합계의 {top_sum / total * 100:.0f}%.")

    if gains and total > 0.0:
        top = gains[:3]
        top_sum = sum(leg.pnl for leg in top)
        text = ", ".join(f"{label(leg.ticker)} {leg.day[4:6]}-{leg.day[6:]} {leg.pnl:+,.0f}" for leg in top)
        lines.append(f"이익 상위 3레그 — {text} — 가 합계의 {top_sum / total * 100:.0f}%.")

    if tickers and len(tickers) >= 2:
        text = ", ".join(f"{label(row['ticker'])} {row['pnl']:+,.0f}원({row['n_legs']}레그)" for row in tickers)
        lines.append(f"종목별로는 {text}.")

    if segment_a and segment_b:
        lines.append(
            f"구간 A {segment_a['return_pct']:+.2f}%({segment_a['n_legs']}레그/{segment_a['n_days']}일) vs "
            f"구간 B {segment_b['return_pct']:+.2f}%({segment_b['n_legs']}레그/{segment_b['n_days']}일)."
            + (" 부호가 갈린다 — 09-11 config 변경(TRENDX stop_loss 2.5) 전후로 다른 결과다." if (segment_a["return_pct"] > 0.0) != (segment_b["return_pct"] > 0.0) else "")
        )
    elif segment_a and not segment_b:
        lines.append("구간 A(09-08~10)에만 있다. 09-11 config 변경 뒤에는 이 사유로 나간 청산이 없다.")
    elif segment_b and not segment_a:
        lines.append("구간 B(09-11~)에만 있다. 09-11 config 변경으로 생긴 규칙이거나 그 전엔 안 걸린 규칙이다.")

    if category == "손절" and family == "TRENDX":
        lines.append("스탑 2.5%에 비용 0.195%와 갭이 붙어 레그당 약 −2.8%. 익절밴드(+3%)와 한 레그 크기는 비슷해서 순이익은 빈도 차이가 정한다.")

    if category == "본전탈출":
        lines.append("평단 근처 청산이라 비용만큼 손실이 정상인데, 중앙값이 비용보다 아래면 '본전'보다 낮게 체결된다는 뜻이다(D-024).")

    return lines


def format_config_value(value) -> str:
    if isinstance(value, bool):
        return "켬" if value else "끔"

    if isinstance(value, float):
        if value >= 10000.0 and value == int(value):
            return f"{int(value):,}"

        return f"{value:g}"

    if isinstance(value, int):
        return f"{value:,}" if value >= 10000 else str(value)

    if value is None:
        return "없음"

    return str(value)


def rules_payload(config_path: Path) -> dict:
    """실행 중 config 로 RULES 의 {키}를 채운다. 값이 없는 키는 '없음' — 코드 기본값이 쓰인다는 뜻이라 그대로 보인다."""
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    strategies = config.get("strategies", [])
    blocks = []
    for rule in RULES:
        if rule.get("risk"):
            values = dict(config.get("risk", {}))
        else:
            strategy = strategies[rule["index"]] if rule["index"] < len(strategies) else {}
            values = dict(strategy.get(rule["sub"], {})) if rule.get("sub") else dict(strategy)

        class Filler(dict):
            def __missing__(self, key):
                return "없음"

        filled = Filler({key: (value if isinstance(value, str) else format_config_value(value)) for key, value in values.items()})
        steps = []
        for step in rule["steps"]:
            keys = sorted(set(re.findall(r"\{(\w+)\}", step["what"])))
            steps.append({
                "phase": step["phase"],
                "what": step["what"].format_map(filled),
                "exit_category": step.get("exit_category"),
                "keys": [{"key": key, "value": filled[key]} for key in keys],
            })

        blocks.append({
            "family": rule["family"],
            "title": rule["title"],
            "one_line": rule["one_line"].format_map(filled),
            "config_scope": "risk" if rule.get("risk") else f"strategies[{rule['index']}]" + (f".{rule['sub']}" if rule.get("sub") else ""),
            "steps": steps,
        })

    return {
        "config_path": str(config_path.relative_to(exit_ev.REPO)).replace("\\", "/") if config_path.is_relative_to(exit_ev.REPO) else str(config_path),
        "indicators": [{"name": name, "text": text} for name, text in INDICATORS],
        "blocks": blocks,
    }


def backtest_payload() -> dict:
    """research/studies/**/metrics.json(quant.metrics/v1 배열). 벤치마크 행(strategy == BUY_AND_HOLD(옛 BH))은 비교 기준으로만 쓴다."""
    studies = []
    all_strategy_rows = 0
    beat_bench = 0
    honesty = defaultdict(int)
    for path in sorted(STUDIES_DIR.rglob("metrics.json")):
        try:
            rows = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue

        if not isinstance(rows, list) or not rows:
            continue

        study_id = rows[0].get("study_id", "")
        number = study_id.split("-")[-1]
        strategy_rows = [row for row in rows if row.get("strategy") not in ("BH", "BUY_AND_HOLD")]
        alphas = [row["alpha"] for row in strategy_rows if isinstance(row.get("alpha"), (int, float))]
        sharpes = [row["sharpe"] for row in strategy_rows if isinstance(row.get("sharpe"), (int, float))]
        mdds = [row["mdd"] for row in strategy_rows if isinstance(row.get("mdd"), (int, float))]
        for row in strategy_rows:
            honesty[row.get("honesty_label") or "unlabeled"] += 1

        all_strategy_rows += len(strategy_rows)
        beat_bench += sum(1 for value in alphas if value > 0.0)
        studies.append({
            "study_id": study_id,
            "name": STUDY_NAME_MAP.get(number, study_id),
            "folder": str(path.parent.relative_to(exit_ev.REPO)).replace("\\", "/"),
            "family": rows[0].get("family", ""),
            "windows": sorted({row.get("window", "") for row in rows if row.get("window")}),
            "n_rows": len(strategy_rows),
            "alpha_positive": sum(1 for value in alphas if value > 0.0),
            "alpha_median": statistics.median(alphas) if alphas else None,
            "sharpe_median": statistics.median(sharpes) if sharpes else None,
            "mdd_median": statistics.median(mdds) if mdds else None,
            "rows": [
                {key: row.get(key) for key in (
                    "strategy", "event", "label", "window", "total_return", "cagr", "sharpe", "mdd", "calmar", "win_rate",
                    "n_trades", "alpha", "bench_return", "bench_mdd", "honesty_label", "oos_flag", "holdout_flag",
                    "holdout_calmar", "train_calmar", "caveat", "source",
                )}
                for row in rows
            ],
        })

    return {
        "n_studies": len(studies),
        "n_strategy_rows": all_strategy_rows,
        "beat_bench": beat_bench,
        "honesty": dict(honesty),
        "studies": studies,
    }


def build_payload(ledger_dir: Path, first_day: str, last_day: str, rounds: int, config_path: Path) -> dict:
    load = exit_ev.load_legs(ledger_dir, first_day, last_day)
    legs = load.legs
    names = load_names({leg.ticker for leg in legs})
    segment_a_legs = [leg for leg in legs if leg.day <= exit_ev.SEGMENT_A_LAST_DAY]
    segment_b_legs = [leg for leg in legs if leg.day > exit_ev.SEGMENT_A_LAST_DAY]

    cells: dict[tuple[str, str], list[exit_ev.ExitLeg]] = defaultdict(list)
    for leg in legs:
        cells[(leg.family, leg.category)].append(leg)

    table_a = exit_ev.build_table(segment_a_legs, 0.0, rounds, exit_ev.BOOTSTRAP_SEED)
    table_b = exit_ev.build_table(segment_b_legs, 0.0, rounds, exit_ev.BOOTSTRAP_SEED)

    cell_rows = []
    for (family, category), cell_legs in sorted(cells.items(), key=lambda item: (-len(item[1]), item[0])):
        all_row = stats_row(cell_legs, rounds)
        segment_a = stats_row([leg for leg in cell_legs if leg.day <= exit_ev.SEGMENT_A_LAST_DAY], rounds) if (family, category) in table_a else None
        segment_b = stats_row([leg for leg in cell_legs if leg.day > exit_ev.SEGMENT_A_LAST_DAY], rounds) if (family, category) in table_b else None
        verdict = exit_ev.verdict(family, category, table_a.get((family, category)), table_b.get((family, category)))
        cost_rows = []
        for rate in exit_ev.EXTRA_COST_RATES:
            cost_row = exit_ev.cell_stats(cell_legs, rate)
            cost_rows.append({"extra_cost_pct": rate * 100.0, "total_pnl": cost_row["total_pnl"], "return_pct": cost_row["return_pct"], "win_rate": cost_row["win_rate"]})

        cell_rows.append({
            "family": family,
            "category": category,
            "verdict": verdict,
            "verdict_meaning": VERDICT_MEANING.get(verdict, ""),
            "excluded": verdict.startswith("판정 제외"),
            "all": all_row,
            "segment_a": segment_a,
            "segment_b": segment_b,
            "unknown_basis": load.unknown_basis.get((family, category), 0),
            "why": why_text(family, category, cell_legs, all_row, segment_a, segment_b, names),
            "by_day": by_day_rows(cell_legs),
            "by_ticker": by_ticker_rows(cell_legs, names),
            "histogram": histogram(cell_legs),
            "cost": cost_rows,
            "legs": [
                {
                    "day": leg.day, "ticker": leg.ticker, "name": names.get(leg.ticker, ""), "quantity": leg.quantity,
                    "sell_notional": leg.sell_notional, "cost_basis": leg.cost_basis,
                    "pnl": leg.pnl, "return_pct": leg_return_pct(leg), "rows": leg.rows,
                }
                for leg in sorted(cell_legs, key=lambda leg: (leg.day, leg.ticker))
            ],
        })

    families: dict[str, list[exit_ev.ExitLeg]] = defaultdict(list)
    for leg in legs:
        families[leg.family].append(leg)

    family_rows = []
    for family, family_legs in sorted(families.items(), key=lambda item: -len(item[1])):
        row = stats_row(family_legs, rounds)
        row["family"] = family
        row["non_strategy"] = family in exit_ev.NON_STRATEGY_FAMILIES
        family_rows.append(row)

    days = sorted({leg.day for leg in legs})
    daily = []
    for day in days:
        day_legs = [leg for leg in legs if leg.day == day]
        per_family = {family: sum(leg.pnl for leg in day_legs if leg.family == family) for family in families}
        daily.append({
            "day": day,
            "n_legs": len(day_legs),
            "pnl": sum(leg.pnl for leg in day_legs),
            "per_family": per_family,
            "segment": "A" if day <= exit_ev.SEGMENT_A_LAST_DAY else "B",
        })

    all_pnls = [leg.pnl for leg in legs]
    return {
        "generated": date.today().isoformat(),
        "first_day": first_day,
        "last_day": last_day,
        "segment_a_last_day": exit_ev.SEGMENT_A_LAST_DAY,
        "segment_b_first_day": next((day for day in days if day > exit_ev.SEGMENT_A_LAST_DAY), last_day),
        "rounds": rounds,
        "seed": exit_ev.BOOTSTRAP_SEED,
        "ledger_dir": str(ledger_dir.relative_to(exit_ev.REPO)).replace("\\", "/") if ledger_dir.is_relative_to(exit_ev.REPO) else str(ledger_dir),
        "n_legs": len(legs),
        "n_days": len(days),
        "total_pnl": sum(all_pnls),
        "win_rate": sum(1 for value in all_pnls if value > 0.0) / len(all_pnls) if all_pnls else 0.0,
        "median_pnl": statistics.median(all_pnls) if all_pnls else 0.0,
        "unknown_basis_total": sum(load.unknown_basis.values()),
        "skipped_files": load.skipped_files,
        "names_missing": sorted({leg.ticker for leg in legs} - names.keys()),
        "sell_cost_rate_pct": exit_ev.SELL_COST_RATE_IN_LEDGER * 100.0,
        "min_days_for_verdict": exit_ev.MIN_DAYS_FOR_VERDICT,
        "verdict_meaning": VERDICT_MEANING,
        "claims": CLAIMS,
        "families": family_rows,
        "daily": daily,
        "cells": cell_rows,
        "rules": rules_payload(config_path),
        "backtest": backtest_payload(),
    }


def rounded(value):
    """본문 JSON의 실수는 소수 4자리까지 — 긴 자릿수는 크기만 키우고 커밋 게이트의 번호 모양 검사에도 걸린다."""
    if isinstance(value, float):
        return round(value, 4)

    if isinstance(value, dict):
        return {key: rounded(item) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [rounded(item) for item in value]

    return value


def render(payload: dict) -> str:
    template = (Path(__file__).resolve().parent / "exit_ev_dashboard_template.html").read_text(encoding="utf-8")
    data = json.dumps(rounded(payload), ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    return template.replace("__PAYLOAD__", data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ledger-dir", type=Path, default=exit_ev.DEFAULT_LEDGER_DIR)
    parser.add_argument("--out", type=Path, default=DEFAULT_HTML)
    parser.add_argument("--first-day", default=exit_ev.FIRST_DAY)
    parser.add_argument("--last-day", default=None, help="기본은 원장 폴더의 최신 trades_*.csv 날짜")
    parser.add_argument("--rounds", type=int, default=exit_ev.BOOTSTRAP_ROUNDS)
    parser.add_argument("--config", type=Path, default=None, help="규칙 탭에 보일 config. 기본은 감시견이 띄운 실행 중 config")
    arguments = parser.parse_args()

    last_day = arguments.last_day or latest_ledger_day(arguments.ledger_dir)
    config_path = arguments.config or Path(gen_tuning_sheet.running_config_path([]))
    if not config_path.is_absolute():
        config_path = exit_ev.REPO / config_path

    payload = build_payload(arguments.ledger_dir, arguments.first_day, last_day, arguments.rounds, config_path)
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text(render(payload), encoding="utf-8")
    missing = f" · 이름 없음 {len(payload['names_missing'])}종목" if payload["names_missing"] else ""
    print(f"{arguments.out.relative_to(exit_ev.REPO)}: {arguments.first_day}~{last_day} 레그 {payload['n_legs']}건 · {payload['n_days']}일 · 셀 {len(payload['cells'])}개 · "
          f"백테스트 {payload['backtest']['n_studies']}스터디 · config {payload['rules']['config_path']}{missing} · {arguments.out.stat().st_size // 1024}KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
