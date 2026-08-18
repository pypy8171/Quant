#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""계열 A(종목 포트폴리오) 백테스트 → 정규화 지표 `metrics.json` 백필.

대시보드는 `metrics.json`만 읽는다(데이터 계약①). 계열 A 실행은 두 출처에 기록돼 있다:

  1. `research/BACKTEST_LOG.md` — **계열 A 실행 원문의 단일 소유자**(스스로 선언).
     실행 #1~#5의 총수익·MDD·샤프·승률·거래수·α 표. 여기서 백필한다(재실행 아님).
     이유: 재실행은 DATA_GO_KR_KEY·네트워크·PIT 유니버스 스냅샷이 필요해 재현 리스크가 있고,
     BACKTEST_LOG가 이미 검증·편향 caveat까지 담은 권위 소스다. 각 행에 source=실행#N 명기.
  2. `research/studies/06_bear_market/summary_{yf,datagokr}.tsv` — (window,strat,regime)별
     지표를 이미 파일로 보유 → **프로그램적으로 파싱**(전사 리스크 0).

편향 규율(bias-auditor): 숫자만으로 오독을 부르는 행은 정직성 라벨/‑caveat로 못박는다.
  - 생존편향: 유니버스=현생존 종목 고정 → α 절대값 신뢰 금지(상대비교용). (실행 #1/#2/#5)
  - 비참여(현금): regime-ON 0체결·샤프0은 '실력'이 아니라 non-participation → 라벨 '맥락필수'.
  - 소표본: N<200은 통계 아닌 일화 → overfit_suspect. (실행 #4)
  - 수정주가: yfinance 액면분할 반영가 → 당시 실호가와 다름(손익률 비교엔 무방). (06 yf 구간)

실행:  python PYQuant/dashboard/backfill_series_a.py
"""
import csv
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[2]                      # .../Quant
sys.path.insert(0, str(_REPO / "PYQuant"))    # backtest.report 임포트용
from backtest.report import metrics_row, write_metrics_rows  # noqa: E402

STUDIES = _REPO / "research" / "studies"

# 공통 caveat 문구
CV_SURV = "생존편향: 유니버스=현생존 종목 고정 → α 절대값 신뢰금지(구성 간 상대비교로만)."
CV_CASH = "비참여(현금): regime-ON 전구간 현금화. 샤프0=무변동 — '현금이 시장을 이김'은 실력 아님(ON/OFF 짝으로 해석)."
CV_SMALL = "소표본 N=10~89: 게이트(≥200) 미달 — 통계 아닌 일화. 전기간 재검증(BT-05)로만 판정."
CV_YF = "수정주가(yfinance, 액면분할 반영) — 당시 실호가와 다를 수 있음. 손익률 비교엔 무방."


def A(study_id, strategy, event, *, ret, mdd, sharpe, win, alpha, n,
      benchmark="등가중 B&H", honesty="robust", caveat="", source="",
      bench_ret=None, window=""):
    """계열 A 한 행 — BACKTEST_LOG 기록값을 quant.metrics/v1로. mdd는 양수 크기."""
    return metrics_row(
        study_id=study_id, strategy=strategy, family="A_portfolio",
        benchmark=benchmark, event=event, window=window,
        total_return=ret, mdd=(abs(mdd) if mdd is not None else None),
        sharpe=sharpe, win_rate=win, n_trades=n, alpha=alpha,
        bench_return=bench_ret,
        honesty_label=honesty, caveat=caveat, source=source)


# ── BT-01 · 모멘텀×국면필터 롤링 (BACKTEST_LOG 실행 #1) ──────────────────────────
BT01 = [
    A("BT-01", "모멘텀+국면 top10/rb5", "최근 1년", ret=146.72, mdd=-23.54, sharpe=2.11,
      win=53.7, alpha=96.74, n=95, bench_ret=None, honesty="robust",
      caveat=CV_SURV + " 1년 샤프2.11엔 국면 운빨 성분 큼.", source="BACKTEST_LOG 실행 #1"),
    A("BT-01", "모멘텀+국면 top10/rb5", "최근 2년", ret=203.70, mdd=-23.41, sharpe=1.68,
      win=49.7, alpha=90.23, n=157, honesty="robust", caveat=CV_SURV,
      source="BACKTEST_LOG 실행 #1"),
    A("BT-01", "모멘텀+국면 top10/rb5", "최근 3년", ret=159.56, mdd=-35.56, sharpe=1.12,
      win=49.2, alpha=36.77, n=256, honesty="robust",
      caveat=CV_SURV + " 3년창 엣지 거의 소멸(샤프1.12 vs 벤치1.10), MDD는 벤치보다 나쁨.",
      source="BACKTEST_LOG 실행 #1"),
    A("BT-01", "모멘텀+국면 top10/rb5", "최근 4년", ret=203.03, mdd=-22.72, sharpe=1.10,
      win=51.1, alpha=77.28, n=284, honesty="robust",
      caveat=CV_SURV + " 4년만 위험조정 알파 판정(표본 284로 신뢰↑), MDD 게이트(≤15%)는 불합격.",
      source="BACKTEST_LOG 실행 #1"),
]

# ── BT-02 · 변동성 타게팅 도입 (실행 #2) ────────────────────────────────────────
BT02 = [
    A("BT-02", "모멘텀+vol타깃0.15", "최근 1년", ret=68.21, mdd=-11.61, sharpe=2.21,
      win=65.0, alpha=18.22, n=157, honesty="robust",
      caveat="MDD 게이트(≤15%) 첫 통과(-11.61%). " + CV_SURV,
      source="BACKTEST_LOG 실행 #2"),
    A("BT-02", "모멘텀+vol타깃0.15", "최근 2년", ret=92.89, mdd=-15.72, sharpe=1.66,
      win=64.4, alpha=-20.57, n=278, honesty="robust",
      caveat="α음수 = 노출축소 탓(벤치는 풀노출) — 위험조정(샤프)으론 미달 아님. " + CV_SURV,
      source="BACKTEST_LOG 실행 #2"),
    A("BT-02", "모멘텀+vol타깃0.15", "최근 3년", ret=88.58, mdd=-25.88, sharpe=1.19,
      win=57.8, alpha=-34.20, n=445, honesty="robust",
      caveat="거래수 445 급증(비용 드래그 주의), 3년 MDD 게이트 불합격. " + CV_SURV,
      source="BACKTEST_LOG 실행 #2"),
]

# ── BT-03 · 2022 약세장 OOS + 국면 ON/OFF ablation (실행 #3) ─────────────────────
BT03 = [
    A("BT-03", "모멘텀 regime ON", "2022 약세장 OOS", ret=0.00, mdd=0.00, sharpe=0.00,
      win=None, alpha=20.50, n=0, honesty="context_required",
      caveat=CV_CASH + " breadth<0.5가 1년내내 → 50회 전부 현금화. 재앙(-35%) 완전 회피가 국면필터의 값.",
      source="BACKTEST_LOG 실행 #3"),
    A("BT-03", "모멘텀 regime OFF", "2022 약세장 OOS", ret=-35.35, mdd=-37.64, sharpe=-1.95,
      win=44.6, alpha=-14.85, n=121, honesty="honest_failure",
      caveat="모멘텀 크래시 실증 — 벌거벗은 모멘텀은 약세장에서 벤치(-20.50%)보다도 나쁨(음의 skew 발현).",
      source="BACKTEST_LOG 실행 #3"),
]

# ── BT-04 · 2026 월별 시작 민감도 스윕 (실행 #4, 소표본) ─────────────────────────
_BT04 = [  # (월, ret, mdd, sharpe, win, n, bench)
    ("1월 시작", 36.29, -8.05, 2.09, 65.2, 89, 21.76),
    ("2월 시작", 30.69, -20.60, 1.74, 52.8, 72, 7.92),
    ("3월 시작", 3.09, -17.48, 0.10, 49.2, 61, -7.09),
    ("4월 시작", 42.63, -9.66, 3.36, 54.3, 46, -1.18),
    ("5월 시작", 5.74, -17.42, 1.09, 31.6, 38, -15.97),
    ("6월 시작", -6.38, -16.83, -0.97, 34.6, 26, -18.46),
    ("7월 시작", -10.02, -15.07, -5.18, 10.0, 10, -7.77),
]
BT04 = [
    A("BT-04", "baseline(regime+vol타깃) 2026 월별", ev, ret=r, mdd=m, sharpe=s,
      win=w, alpha=round(r - b, 2), n=n, honesty="overfit_suspect", caveat=CV_SMALL,
      source="BACKTEST_LOG 실행 #4")
    for (ev, r, m, s, w, n, b) in _BT04
]

# ── BT-05 · 전기간 지표4종 재검증 2021~2026, N≥750 (실행 #5) ─────────────────────
_BT05 = [  # (구성, ret, mdd, sharpe, win, n)  — 채택 게이트 통과 0개(honest_failure)
    ("baseline (regime+vol타깃)", 295.71, -18.48, 1.40, 58.7, 758),
    ("no_regime", 344.95, -21.26, 1.33, 58.6, 1047),
    ("trend200 (개별200MA)", 300.13, -19.47, 1.41, 60.9, 751),
    ("trend120 (개별120MA)", 273.59, -18.69, 1.35, 59.4, 785),
    ("vol_adjust (변동성조정)", 227.97, -18.78, 1.27, 56.9, 801),
    ("absmom (절대모멘텀 게이트)", 295.71, -18.48, 1.40, 58.7, 758),
]
BT05 = [
    A("BT-05", cfg, "전기간(2021~2026)", ret=r, mdd=m, sharpe=s, win=w, alpha=None, n=n,
      honesty="honest_failure",
      caveat="채택 게이트 통과 지표 0개 — 견고한 개선 없음. " + CV_SURV
             + " 유일 유효 레버는 국면필터(수익 일부↔낙폭).",
      source="BACKTEST_LOG 실행 #5")
    for (cfg, r, m, s, w, n) in _BT05
]

# ── BT-06 · 하락장 이벤트 백테스트 (summary_*.tsv 파싱) ──────────────────────────
BT06_DIR = STUDIES / "06_bear_market"
WIN_LABEL = {
    "2011euro": "2011 유럽재정위기", "2018semi": "2018 반도체·미중무역",
    "2020covid": "2020 COVID 팬데믹", "2022bear": "2022 금리인상 약세장",
    "2024blackmon": "2024 블랙먼데이", "2026now": "2026 현재 변동장",
}
STRAT_SHORT = {"momentum": "모멘텀", "mean_reversion": "역추세"}


def _f(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def load_bt06():
    rows = []
    for tsv, src, yf in (("summary_yf.tsv", "yfinance(수정주가)", True),
                         ("summary_datagokr.tsv", "datagokr(PIT)", False)):
        p = BT06_DIR / tsv
        if not p.exists():
            continue
        with open(p, encoding="utf-8-sig") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                win, strat, reg = r["window"], r["strategy"], r["regime"]
                n = int(_f(r["trades"]) or 0)
                cash = (reg == "on" and n == 0)  # 비참여(국면필터 전구간 현금)
                cav = []
                if cash:
                    cav.append(CV_CASH)
                if yf:
                    cav.append(CV_YF)
                honesty = "context_required" if cash else "robust"
                rows.append(A(
                    "BT-06", f"{STRAT_SHORT.get(strat, strat)} {reg.upper()}",
                    WIN_LABEL.get(win, win),
                    ret=_f(r["ret_pct"]), mdd=_f(r["mdd_pct"]), sharpe=_f(r["sharpe"]),
                    win=_f(r["win_pct"]), alpha=_f(r["alpha_p"]), n=n,
                    bench_ret=_f(r["bench_ret"]), benchmark=src,
                    honesty=honesty, caveat=" ".join(cav),
                    source=f"06_bear_market/{tsv}"))
    return rows


def main():
    plans = [
        ("01_momentum_regime", BT01 + BT04 + BT05),  # CrossMomentum 계열 저널 묶음
        ("02_vol_target", BT02),
        ("03_2022_ablation", BT03),
        ("06_bear_market", load_bt06()),
    ]
    total = 0
    for folder, rows in plans:
        d = STUDIES / folder
        d.mkdir(parents=True, exist_ok=True)
        write_metrics_rows(str(d / "metrics.json"), rows)
        total += len(rows)
    print(f"✅ 계열 A 백필 완료: {total}행 ({len(plans)}파일)")


if __name__ == "__main__":
    main()
