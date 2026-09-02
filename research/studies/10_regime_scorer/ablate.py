#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Track A 구조국면 애블레이션 하네스 — 국면 스코어러 4변형(A/B/C/D)을
6개 하락장 창에서 "지수 long/flat 타이밍 필터"로 결정론적 평가.

무엇을 하나 (책임 경계):
  * 국면 스코어러(backtest/regime_scorer.py)를 지수(^KS11)만으로 백테스트 가능한
    타이밍 필터로 배선한다. 국면=전략로스터선택의 프록시 → 지수 롱/플랫으로 대리.
    BULL/NEUTRAL=지수 롱, BEAR=현금(flat).
  * 신호(스코어러)와 실행(체결관례)을 분리. 스코어러는 절대 미수정(import만).
  * 엔진(engine.py) 체결관례와 동일: 결정=종가(D) → 집행=다음봉 시가(D+1 open).
    look-ahead는 루프 구조로 차단(스코어는 closes[:i+1]만, 수익은 open[i+1]→open[i+2]).

무엇을 안 하나:
  * 엣지 판정(=@quant-analyst), 편향 판정(=@bias-auditor)은 하지 않는다.
    이 스크립트는 재현 가능한 숫자와 로그(summary.tsv/README.md)만 낸다.

데이터: PYQuant/data/index_source.py 의 IndexSource().get_historical_ohlcv 만 사용.
  ^KS11(코스피, 구조축), ^SOX(반도체, D 오버레이), ^VIX(공포, D 오버레이).
  캐시=parquet(.index_cache). ⚠️ 최초 1회는 네트워크 필요(캐시 미스 시 yfinance 다운로드).
  캐시가 커밋/워밍된 뒤에는 오프라인 결정론 재현.

창 날짜 출처(발명 아님):
  research/studies/06_bear_market/raw/<window>_momentum_on.csv 의 equity 시계열
  첫/마지막 날짜 = 그 창에서 실제 시뮬레이션된 [start, end]. (BT-06 산출물에서 재사용)

실행(WSL venv):
  cd .../PYQuant && source .venv/bin/activate \
    && cd ../research/studies/10_regime_scorer && python ablate.py
  (이 환경 Windows py에는 yfinance/pandas가 없어 여기서 실행하지 않는다.)

산출: summary.tsv (창×변형×지표) + README.md (요약표 + 결론). 콘솔에 진행상황 print.
"""
from __future__ import annotations

import os
import sys
import math
import random
import subprocess
from pathlib import Path
from datetime import date as _date, timedelta

# ── PYQuant 를 sys.path 에 (IndexSource → kis.client.Bar, backtest.regime_scorer) ──
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]                      # .../Quant
_PYQ = _REPO / "PYQuant"
if str(_PYQ) not in sys.path:
    sys.path.insert(0, str(_PYQ))

from data.index_source import IndexSource                         # noqa: E402
from backtest.regime_scorer import (                              # noqa: E402
    score_v0, classify_v0,
    score_continuous, classify_continuous,
    overlay_shift, BEAR,
)

# ════════════════════════════════════════════════════════════════════════════
# 재현 정보 / 사전등록 파라미터 (스크립트 상단 각인 — 재실행 재현 보장)
# ════════════════════════════════════════════════════════════════════════════
SEED = 0                              # RNG 미사용(결정론)이나 명시 고정
random.seed(SEED)

INDEX_KR = "^KS11"                    # 구조 국면 축
INDEX_SOX = "^SOX"                    # D 오버레이: 반도체 밤사이
INDEX_VIX = "^VIX"                    # D 오버레이: 공포지수

WARMUP_CAL_DAYS = 450                 # start 이전 ~300거래일(200MA+기울기20+실현변동성20 워밍업)
ANN = math.sqrt(252.0)               # 샤프(위험조정수익) 연율화

# 6개 하락장 창 — start/end 는 BT-06 raw equity CSV 첫/마지막 날짜에서 재사용(발명 금지).
WINDOWS: dict[str, tuple[str, str]] = {
    "2011euro":     ("2011-01-03", "2011-12-29"),   # 유럽재정위기(완만·장기 + 8월 급락)
    "2018semi":     ("2018-01-02", "2018-12-28"),   # 미중무역·반도체(박스+급락, 방향 잦은 반전)
    "2020covid":    ("2020-01-02", "2020-12-30"),   # 팬데믹(V자, 연말 플러스)
    "2022bear":     ("2022-01-03", "2022-12-29"),   # 금리인상 약세장(순수 하락) — 홀드아웃
    "2024blackmon": ("2023-12-28", "2024-12-30"),   # 엔캐리 블랙먼데이(급락 후 회복)
    "2026now":      ("2025-12-30", "2026-08-04"),   # AI·반도체 고변동(랠리+급락 공존)
}
HOLDOUT = "2022bear"                                  # 임계 교정에서 제외 → 별도 보고
CALIB = [w for w in WINDOWS if w != HOLDOUT]          # 임계 선택용 5창

VARIANTS = ["A", "B", "C", "D"]
# 연속변형(B/C/D) classify 임계 후보 — 과적합 방지 위해 소수 대칭 그리드.
BULL_GRID = [0.5, 0.8, 1.0]           # bear_th = -bull_th (대칭)
# A(이산 v0 미러)는 임계 고정 ±2 (C++ RegimeController 패리티) — 튜닝하지 않는다.
A_BULL_TH, A_BEAR_TH = 2, -2

OUT_DIR = _HERE.parent
SUMMARY_PATH = OUT_DIR / "summary.tsv"
README_PATH = OUT_DIR / "README.md"


def _git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(_REPO), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


# ════════════════════════════════════════════════════════════════════════════
# 데이터 로드 (IndexSource — <= date 관례. US심볼 캘린더 정렬은 러너가 명시적으로 처리)
# ════════════════════════════════════════════════════════════════════════════
def load_bars(src: IndexSource, ticker: str, start: str, end: str) -> list:
    """지수 일봉 → date 오름차순·중복제거 list[Bar]. 실패/빈응답이면 빈 리스트."""
    bars = src.get_historical_ohlcv(ticker, start, end) or []
    seen, out = set(), []
    for b in sorted(bars, key=lambda x: x.date):
        if b.date in seen or b.close <= 0:
            continue
        seen.add(b.date)
        out.append(b)
    return out


def px(bar) -> float:
    """체결가 = 시가(>0)면 시가, 아니면 종가 폴백 — engine._execute 관례 미러."""
    return bar.open if bar.open > 0 else bar.close


def pre_start_of(start: str) -> str:
    return (_date.fromisoformat(start) - timedelta(days=WARMUP_CAL_DAYS)).isoformat()


# ── D 오버레이: 체결일(exec_date) KR개장 이전 확정된 가장 최근 US세션만 사용 ────────
def _last_before(bars: list, exec_date: str) -> int:
    """bars(오름차순) 중 date < exec_date 인 가장 마지막 인덱스. 없으면 -1.
    휴장 갭이면 그 이전 마지막 봉(stale)을 자연히 채택(< 조건이라)."""
    idx = -1
    for i, b in enumerate(bars):
        if b.date < exec_date:
            idx = i
        else:
            break
    return idx


def overlay_for(exec_date: str, sox: list, vix: list) -> float:
    """체결일 기준 개장 오버레이 shift. 조회결측이면 해당 축 0으로 degrade(가격축과 분리).
      sox_overnight_ret = 선택 US세션 SOX 종가수익률(직전 봉 대비)
      vix_z             = (vix - vix_sma20)/vix_sma20  (선택 봉 기준)
    """
    sox_ret = None
    si = _last_before(sox, exec_date)
    if si >= 1 and sox[si - 1].close > 0:
        sox_ret = sox[si].close / sox[si - 1].close - 1.0

    vix_z = None
    vi = _last_before(vix, exec_date)
    if vi >= 19:                                   # 20봉 SMA 표본 확보
        window = [vix[k].close for k in range(vi - 19, vi + 1)]
        sma20 = sum(window) / 20.0
        if sma20 > 0:
            vix_z = (vix[vi].close - sma20) / sma20

    if sox_ret is None and vix_z is None:
        return 0.0                                 # 두 축 모두 결측 → 오버레이 미가산
    return overlay_shift(sox_overnight_ret=sox_ret, vix_z=vix_z, gain=0.5)


# ════════════════════════════════════════════════════════════════════════════
# 창별 결정 레코드 precompute (스코어는 임계와 무관 → 그리드탐색시 재계산 회피)
# ════════════════════════════════════════════════════════════════════════════
def build_records(src: IndexSource, window: str, need_overlay: bool) -> list[dict]:
    """반환: in-window 세그먼트별 레코드 list. 각 레코드:
        seg_ret : open[i+2]/open[i+1]-1   (D+1 시가 진입, D+2 시가까지 보유분 수익)
        sA      : score_v0(int|None)      결정=close ks[i] (closes[:i+1])
        sB/sC   : score_continuous(no-slope / slope)
        sD      : score_continuous(slope)  (D 기저 = C)
        shift   : D 개장 오버레이 shift (need_overlay=False면 0.0)
    look-ahead 차단: score 는 closes[:i+1]만, 수익은 open[i+1]→open[i+2](미래) 로만.
    """
    start, end = WINDOWS[window]
    pre = pre_start_of(start)
    ks = load_bars(src, INDEX_KR, pre, end)
    if len(ks) < 220:
        raise RuntimeError(
            f"[{window}] ^KS11 봉 부족({len(ks)}<220). 워밍업 미달 — "
            f"네트워크/캐시/티커 확인. (조용한 NEUTRAL 폴백 방지 위해 중단)")

    sox = load_bars(src, INDEX_SOX, pre, end) if need_overlay else []
    vix = load_bars(src, INDEX_VIX, pre, end) if need_overlay else []
    if need_overlay:
        print(f"    [{window}] overlay 소스: ^SOX {len(sox)}봉 / ^VIX {len(vix)}봉"
              + ("  (일부 결측 → 해당 세그 오버레이 0)" if not (sox and vix) else ""))

    closes = [b.close for b in ks]
    records: list[dict] = []
    # 세그먼트 i: 결정=ks[i] 종가, 진입=ks[i+1] 시가, 청산기준=ks[i+2] 시가
    for i in range(len(ks) - 2):
        entry_date = ks[i + 1].date
        if not (start <= entry_date <= end):
            continue                               # 자본은 요청 이벤트창 기준으로만
        prefix = closes[:i + 1]                    # closes[-1] = 결정 종가(look-ahead 없음)
        rec = {
            "entry_date": entry_date,
            "seg_ret": px(ks[i + 2]) / px(ks[i + 1]) - 1.0,
            "sA": score_v0(prefix),
            "sB": score_continuous(prefix, with_slope=False),
            "sC": score_continuous(prefix, with_slope=True),
            "sD": score_continuous(prefix, with_slope=True),
            "shift": overlay_for(entry_date, sox, vix) if need_overlay else 0.0,
        }
        records.append(rec)
    if not records:
        raise RuntimeError(f"[{window}] in-window 세그먼트 0개 — 창 날짜/데이터 확인.")
    return records


# ── 포지션 산출(변형별) ──────────────────────────────────────────────────────
def is_long(rec: dict, variant: str, bull_th: float, bear_th: float) -> int:
    """regime → 롱(1)/플랫(0). BULL·NEUTRAL·표본부족(None안전판)=롱, BEAR=플랫."""
    if variant == "A":
        regime = classify_v0(rec["sA"], A_BULL_TH, A_BEAR_TH)
    elif variant == "B":
        regime = classify_continuous(rec["sB"], bull_th, bear_th)
    elif variant == "C":
        regime = classify_continuous(rec["sC"], bull_th, bear_th)
    elif variant == "D":
        sh = rec["shift"]                          # 약세편향 → 두 임계 모두 위로
        regime = classify_continuous(rec["sD"], bull_th + sh, bear_th + sh)
    else:
        raise ValueError(variant)
    return 0 if regime == BEAR else 1


# ════════════════════════════════════════════════════════════════════════════
# 성과 지표 (equity 는 세그먼트 복리 — 창별로 1.0 리셋, 전구간 복리 금지)
# ════════════════════════════════════════════════════════════════════════════
def _curve_stats(rets: list[float]) -> tuple[float, float, float]:
    """세그 수익률 시계열 → (총수익%, 최대낙폭(MDD)%, 연율 샤프)."""
    eq, curve = 1.0, [1.0]
    for r in rets:
        eq *= (1.0 + r)
        curve.append(eq)
    total = (curve[-1] - 1.0) * 100.0
    peak, mdd = curve[0], 0.0
    for e in curve:
        peak = max(peak, e)
        dd = (peak - e) / peak * 100.0 if peak > 0 else 0.0
        mdd = max(mdd, dd)
    sharpe = 0.0
    if len(rets) > 1:
        m = sum(rets) / len(rets)
        var = sum((r - m) ** 2 for r in rets) / (len(rets) - 1)
        sd = math.sqrt(var)
        sharpe = (m / sd * ANN) if sd > 0 else 0.0
    return total, mdd, sharpe


def evaluate(records: list[dict], variant: str, bull_th: float, bear_th: float) -> dict:
    """변형×임계 → 지표. B&H(항상 롱) 베이스라인 동봉."""
    pos = [is_long(r, variant, bull_th, bear_th) for r in records]
    strat_rets = [p * r["seg_ret"] for p, r in zip(pos, records)]
    bh_rets = [r["seg_ret"] for r in records]           # 항상 롱

    total, mdd, sharpe = _curve_stats(strat_rets)
    bh_total, bh_mdd, bh_sharpe = _curve_stats(bh_rets)

    flips = sum(1 for k in range(1, len(pos)) if pos[k] != pos[k - 1])
    pct_inv = sum(pos) / len(pos) * 100.0
    return {
        "total": total, "mdd": mdd, "sharpe": sharpe,
        "flips": flips, "pct_inv": pct_inv,
        "bh_total": bh_total, "bh_mdd": bh_mdd, "bh_sharpe": bh_sharpe,
    }


# ════════════════════════════════════════════════════════════════════════════
# 임계 선택 (B/C/D) — CALIB 5창 평균 샤프 최대화. 2022 홀드아웃엔 그 값 그대로 적용.
# ════════════════════════════════════════════════════════════════════════════
def select_threshold(recs_by_win: dict[str, list[dict]], variant: str) -> tuple[float, float]:
    """CALIB 창들의 평균 전략 샤프를 최대화하는 (bull_th, bear_th). A는 고정 ±2."""
    if variant == "A":
        return float(A_BULL_TH), float(A_BEAR_TH)
    best, best_key = None, None
    for bth in BULL_GRID:
        sharpes = [evaluate(recs_by_win[w], variant, bth, -bth)["sharpe"] for w in CALIB]
        mean_sharpe = sum(sharpes) / len(sharpes)
        # 동점 tie-break: 평균 총수익 높은 쪽
        mean_ret = sum(evaluate(recs_by_win[w], variant, bth, -bth)["total"] for w in CALIB) / len(CALIB)
        key = (round(mean_sharpe, 6), round(mean_ret, 6))
        print(f"      [{variant}] bull={bth:+.2f}/bear={-bth:+.2f} → CALIB평균 샤프 {mean_sharpe:+.3f}, 총수익 {mean_ret:+.2f}%")
        if best_key is None or key > best_key:
            best_key, best = key, (bth, -bth)
    return best


# ════════════════════════════════════════════════════════════════════════════
# 산출물
# ════════════════════════════════════════════════════════════════════════════
_COLS = ["window", "variant", "bull_th", "bear_th", "total_ret_pct", "mdd_pct",
         "sharpe", "flips", "pct_invested", "bh_ret_pct", "bh_mdd_pct",
         "bh_sharpe", "holdout"]


def write_summary(rows: list[dict], commit: str) -> None:
    lines = [
        f"# quant.regime_ablate/v1  seed={SEED}  commit={commit}",
        f"# source=IndexSource(.index_cache)  index={INDEX_KR}/{INDEX_SOX}/{INDEX_VIX}",
        f"# warmup_cal_days={WARMUP_CAL_DAYS}  holdout={HOLDOUT}  grid_bull={BULL_GRID}",
        "# exec=decide-on-close(D) / execute-next-open(D+1)  look-ahead: score<=D, ret open[D+1]->open[D+2]",
        "\t".join(_COLS),
    ]
    for r in rows:
        lines.append("\t".join(str(r[c]) for c in _COLS))
    SUMMARY_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"  [산출] {SUMMARY_PATH}")


def write_readme(rows: list[dict], chosen: dict, commit: str) -> None:
    def by(v): return [r for r in rows if r["variant"] == v]

    L = ["# BT-10 · Track A 구조국면 스코어러 애블레이션 (A/B/C/D)", ""]
    L.append("> 국면 스코어러 4변형을 6개 하락장 창에서 **지수 long/flat 타이밍 필터**로 평가.")
    L.append("> BULL/NEUTRAL=지수 롱, BEAR=현금. 결정=종가(D)·집행=다음봉 시가(D+1). 창별 자본 리셋.")
    L.append("")
    L.append("## 변형")
    L.append("| 변형 | 정의 | 임계 |")
    L.append("|---|---|---|")
    L.append(f"| A | C++ v0 미러(이산 2축, 200MA+정배열) | 고정 ±2 |")
    L.append(f"| B | 연속화(tanh, 위치+정배열) | bull {chosen['B'][0]:+.2f} / bear {chosen['B'][1]:+.2f} |")
    L.append(f"| C | B + MA200 기울기축 | bull {chosen['C'][0]:+.2f} / bear {chosen['C'][1]:+.2f} |")
    L.append(f"| D | C + 개장 SOX/VIX 오버레이(임계 shift) | bull {chosen['D'][0]:+.2f} / bear {chosen['D'][1]:+.2f} |")
    L.append("")
    L.append(f"임계는 CALIB 5창({', '.join(CALIB)}) 평균 샤프로 선택, **{HOLDOUT} 홀드아웃**엔 그 값 그대로 적용.")
    L.append("")

    L.append("## 창×변형 지표")
    L.append("| 창 | 변형 | 총수익% | MDD% | 샤프 | flips | 투자% | BH수익% | BH MDD% | BH샤프 | HO |")
    L.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|:--:|")
    for w in WINDOWS:
        for v in VARIANTS:
            r = next(x for x in rows if x["window"] == w and x["variant"] == v)
            ho = "✅" if r["holdout"] else ""
            L.append(f"| {w} | {v} | {r['total_ret_pct']} | {r['mdd_pct']} | {r['sharpe']} "
                     f"| {r['flips']} | {r['pct_invested']} | {r['bh_ret_pct']} | {r['bh_mdd_pct']} "
                     f"| {r['bh_sharpe']} | {ho} |")
    L.append("")

    # ── 결론(집계에서 자동 산출) ──
    L.append("## 무슨 결론")

    def calib_mean(v, key):
        # rows는 지표를 포맷 문자열로 저장(f"{...:.2f}")하므로 float 파싱 후 평균.
        xs = [float(next(x for x in rows if x["window"] == w and x["variant"] == v)[key]) for w in CALIB]
        return sum(xs) / len(xs)

    L.append("")
    L.append("**CALIB 5창 평균(홀드아웃 제외):**")
    L.append("")
    L.append("| 변형 | 평균 총수익% | 평균 MDD% | 평균 샤프 |")
    L.append("|---|---:|---:|---:|")
    for v in VARIANTS:
        L.append(f"| {v} | {calib_mean(v,'total_ret_pct'):+.2f} | {calib_mean(v,'mdd_pct'):.2f} | {calib_mean(v,'sharpe'):+.3f} |")
    # B&H 평균 한 줄(변형 무관, 어느 변형 행이든 동일 세그 → A 기준)
    bh_r = sum(float(next(x for x in rows if x["window"]==w and x["variant"]=="A")["bh_ret_pct"]) for w in CALIB)/len(CALIB)
    bh_m = sum(float(next(x for x in rows if x["window"]==w and x["variant"]=="A")["bh_mdd_pct"]) for w in CALIB)/len(CALIB)
    bh_s = sum(float(next(x for x in rows if x["window"]==w and x["variant"]=="A")["bh_sharpe"]) for w in CALIB)/len(CALIB)
    L.append(f"| B&H | {bh_r:+.2f} | {bh_m:.2f} | {bh_s:+.3f} |")
    L.append("")

    best_v = max(VARIANTS, key=lambda v: calib_mean(v, "sharpe"))
    L.append(f"- CALIB 평균 샤프 최고 변형: **{best_v}** ({calib_mean(best_v,'sharpe'):+.3f}).")
    ho_best = next(x for x in rows if x["window"] == HOLDOUT and x["variant"] == best_v)
    L.append(f"- 홀드아웃({HOLDOUT}) — {best_v}: 총수익 {ho_best['total_ret_pct']}% / MDD {ho_best['mdd_pct']}% / 샤프 {ho_best['sharpe']} "
             f"vs B&H 총수익 {ho_best['bh_ret_pct']}% / MDD {ho_best['bh_mdd_pct']}%.")
    L.append(f"- 이 표는 **재현 가능한 산출물**이다. 성과 유의성 판정은 @quant-analyst, "
             f"look-ahead·편향 감사는 @bias-auditor 몫.")
    L.append("")
    L.append("## 재현 정보")
    L.append(f"- seed={SEED} · commit={commit} · warmup_cal_days={WARMUP_CAL_DAYS} · grid_bull={BULL_GRID}")
    L.append(f"- 데이터: IndexSource(.index_cache) — {INDEX_KR}/{INDEX_SOX}/{INDEX_VIX} (yfinance 수정주가)")
    L.append(f"- 체결관례: 결정=종가(D) → 집행=다음봉 시가(D+1). 비용 미반영(순수 타이밍 필터 — 게이트/비용은 라이브 배선 시).")
    L.append(f"- 창 날짜 출처: research/studies/06_bear_market/raw/<window>_momentum_on.csv (BT-06 equity 첫/마지막일).")
    L.append("- 재실행: `python ablate.py` (최초 1회 네트워크; 이후 캐시 결정론).")
    L.append("")
    L.append("← [백테스트 종합](../README.md)")
    README_PATH.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"  [산출] {README_PATH}")


# ════════════════════════════════════════════════════════════════════════════
def main() -> None:
    commit = _git_commit()
    print("=" * 68)
    print(f"BT-10 Track A 국면 스코어러 애블레이션  (seed={SEED}, commit={commit})")
    print(f"창 {len(WINDOWS)}개 · 변형 {VARIANTS} · 홀드아웃={HOLDOUT}")
    print("=" * 68)

    src = IndexSource()

    # 1. 창별 결정 레코드 precompute (D 오버레이 위해 SOX/VIX 동반 로드)
    print("[1] 데이터 로드 + 결정 레코드 precompute (look-ahead 차단 루프)")
    recs_by_win: dict[str, list[dict]] = {}
    for w in WINDOWS:
        print(f"  - {w} {WINDOWS[w]} …")
        recs_by_win[w] = build_records(src, w, need_overlay=True)
        print(f"    세그먼트 {len(recs_by_win[w])}개")

    # 2. 임계 선택 (B/C/D: CALIB 평균 샤프 최대화 / A: 고정)
    print("[2] 임계 선택 (CALIB 5창 평균 샤프 최대화)")
    chosen: dict[str, tuple[float, float]] = {}
    for v in VARIANTS:
        chosen[v] = select_threshold(recs_by_win, v)
        print(f"    [{v}] 선택 임계 = bull {chosen[v][0]:+.2f} / bear {chosen[v][1]:+.2f}")

    # 3. 전 창 최종 평가 (홀드아웃은 선택값 그대로)
    print("[3] 전 창 최종 평가")
    rows: list[dict] = []
    for w in WINDOWS:
        for v in VARIANTS:
            bth, brh = chosen[v]
            m = evaluate(recs_by_win[w], v, bth, brh)
            rows.append({
                "window": w, "variant": v,
                "bull_th": f"{bth:+.2f}", "bear_th": f"{brh:+.2f}",
                "total_ret_pct": f"{m['total']:.2f}", "mdd_pct": f"{m['mdd']:.2f}",
                "sharpe": f"{m['sharpe']:.3f}", "flips": m["flips"],
                "pct_invested": f"{m['pct_inv']:.1f}",
                "bh_ret_pct": f"{m['bh_total']:.2f}", "bh_mdd_pct": f"{m['bh_mdd']:.2f}",
                "bh_sharpe": f"{m['bh_sharpe']:.3f}",
                "holdout": (w == HOLDOUT),
            })
        print(f"  - {w} 완료")

    # 4. 산출
    print("[4] 산출물 기록")
    write_summary(rows, commit)
    write_readme(rows, chosen, commit)
    print("완료. 성과판정 → @quant-analyst / 편향감사 → @bias-auditor")


if __name__ == "__main__":
    main()
