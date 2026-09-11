#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
A-1 · v0 국면 라벨의 forward 수익 분리력 순열검정 + A-2 라벨 품질표.

무엇을 하나 (책임 경계):
  * 코스피(^KS11) 일봉 한 파일에서 봉마다 결정일 D 종가까지의 closes로 score_v0 → classify_v0
    (BULL/NEUTRAL/BEAR, 워밍업 None은 UNKNOWN으로 제외). 루프 방식은 ablate.py와 같다.
  * fwdH = open[D+1+H] / open[D+1] − 1 (D+1 시가 진입, D+1+H 시가 청산). H=20이 사전 고정
    주지표, 5/10/60은 탐색용 병기. 마지막 H+1 결정일은 미래 봉이 없어 버린다.
  * 통계량 = mean(fwd|BEAR) − mean(fwd|non-BEAR), 중앙값 차이도 같이.
  * 순열 = 에피소드 블록 셔플 1,000회. 연속된 같은 라벨 구간을 블록으로 묶어 블록 순서만 섞는다
    (라벨 run 길이·BEAR 일수 보존, 일별 셔플 금지). 시드 고정.
  * 감도: 1997-98 제외 / 2008 제외 / 둘 다 제외(달력 연도 기준). 2022는 홀드아웃 — 부호와 관측
    통계량만 내고 순열 p는 내지 않는다.
  * A-2: 라벨별 에피소드 수·평균 dwell·연간 flips, 위기 창별 max-dd 저점 기준점 대비 BEAR 진입/해제 lag.

무엇을 안 하나:
  * 엣지 판정·편향 판정은 하지 않는다(@quant-analyst / @bias-auditor). 숫자와 표만 낸다.
  * 스코어러(PYQuant/backtest/regime_scorer.py)는 import만 하고 고치지 않는다.

입력(고정, 네트워크 없음):
  PYQuant/.index_cache/idx__KS11_1996-01-01_2026-08-15_adj.parquet
실행:
  cd research/studies/10_regime_scorer && py a1_permutation.py
산출: a1_results.tsv (순열 결과 + 품질표, 섹션 주석으로 구분) · A1_REPORT.md
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]
_PYQ = _REPO / "PYQuant"
if str(_PYQ) not in sys.path:
    sys.path.insert(0, str(_PYQ))

from backtest.regime_scorer import score_v0, classify_v0, BEAR, BULL, NEUTRAL, UNKNOWN  # noqa: E402

# ════════════════════════════════════════════════════════════════════════════
# 재현 정보 / 사전 고정 파라미터
# ════════════════════════════════════════════════════════════════════════════
SEED = 20260911
N_PERM = 1000
DATA_PATH = _PYQ / ".index_cache" / "idx__KS11_1996-01-01_2026-08-15_adj.parquet"
HORIZONS = [20, 5, 10, 60]           # 20 = 사전 고정 주지표, 나머지는 탐색용
PRIMARY_H = 20
START_YEAR = 1997
HOLDOUT_YEAR = 2022
OUT_TSV = _HERE.parent / "a1_results.tsv"
OUT_MD = _HERE.parent / "A1_REPORT.md"

# 위기 창 — research/studies/08_crisis_response/backtest_crisis_response.py EVENTS 중
# ^KS11 커버리지(1996-12~) 안에 드는 것만 재사용(발명 아님). 저점 기준점은 창 안 종가 max-dd 저점.
CRISIS_WINDOWS = [
    ("1997_imf_asian",         "1997-06-01", "1999-06-30"),
    ("1998_ltcm_russia",       "1998-06-01", "1999-01-31"),
    ("2000_dotcom",            "2000-01-01", "2003-06-30"),
    ("2001_911",               "2001-07-01", "2002-01-31"),
    ("2003_sars_iraq",         "2002-12-01", "2003-09-30"),
    ("2008_gfc",               "2007-08-01", "2010-06-30"),
    ("2010_flash_euro",        "2010-03-01", "2010-12-31"),
    ("2011_us_downgrade_euro", "2011-05-01", "2012-06-30"),
    ("2015_china_yuan",        "2015-06-01", "2016-04-30"),
    ("2018_fed_q4",            "2018-08-01", "2019-06-30"),
    ("2020_covid",             "2020-01-01", "2021-01-31"),
    ("2022_inflation_bear",    "2021-11-01", "2023-06-30"),
    ("2023_svb",               "2023-02-01", "2023-08-31"),
    ("2024_yen_carry",         "2024-07-01", "2024-12-31"),
]

# 검정 범위(scope). 감도는 달력 연도로 뺀다. holdout=True면 순열 p를 내지 않는다.
SCOPES = [
    ("main_1997_2026_ex2022", "본검정 1997~2026 (2022 홀드아웃 제외)", {HOLDOUT_YEAR}, False),
    ("sens_ex_1997_98",       "감도: 1997-98 제외",                    {1997, 1998, HOLDOUT_YEAR}, False),
    ("sens_ex_2008",          "감도: 2008 제외",                       {2008, HOLDOUT_YEAR}, False),
    ("sens_ex_both",          "감도: 1997-98·2008 제외",               {1997, 1998, 2008, HOLDOUT_YEAR}, False),
    ("holdout_2022",          "홀드아웃 2022 (부호만)",                None, True),
    ("ref_all_incl_2022",     "참고: 1997~2026 전 봉 (2022 포함)",     set(), False),
]


def _git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(_REPO), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


# ════════════════════════════════════════════════════════════════════════════
# 1. 데이터 + 라벨 (look-ahead 차단: score는 closes[:i+1]만)
# ════════════════════════════════════════════════════════════════════════════
def load_bars() -> pd.DataFrame:
    df = pd.read_parquet(DATA_PATH)
    need = {"date", "open", "close"}
    if not need.issubset(df.columns):
        raise RuntimeError(f"parquet 컬럼 불일치: {list(df.columns)}")
    df = df.copy()
    df["date"] = pd.to_datetime(df["date"])
    df = df.sort_values("date").drop_duplicates("date").reset_index(drop=True)
    df = df[(df["close"] > 0) & (df["open"] > 0)].reset_index(drop=True)
    return df


def label_bars(closes: list[float]) -> list[str]:
    """봉 i의 라벨 = classify_v0(score_v0(closes[:i+1])). None(워밍업)은 UNKNOWN."""
    out = []
    for i in range(len(closes)):
        s = score_v0(closes[:i + 1])
        out.append(UNKNOWN if s is None else classify_v0(s))
    return out


def fwd_returns(opens: np.ndarray, h: int) -> np.ndarray:
    """fwd[i] = open[i+1+h]/open[i+1] − 1. 미래 봉이 없는 마지막 h+1개는 NaN."""
    n = len(opens)
    out = np.full(n, np.nan)
    if n > h + 1:
        out[: n - h - 1] = opens[h + 1:] / opens[1: n - h] - 1.0
    return out


# ════════════════════════════════════════════════════════════════════════════
# 2. 에피소드 블록 셔플 순열검정
# ════════════════════════════════════════════════════════════════════════════
def runs_of(labels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """연속 같은 라벨 구간 → (블록 라벨, 블록 길이)."""
    if len(labels) == 0:
        return np.array([]), np.array([], dtype=int)
    change = np.flatnonzero(labels[1:] != labels[:-1]) + 1
    starts = np.concatenate([[0], change])
    ends = np.concatenate([change, [len(labels)]])
    return labels[starts], ends - starts


def stat_pair(fwd: np.ndarray, is_bear: np.ndarray) -> tuple[float, float]:
    a, b = fwd[is_bear], fwd[~is_bear]
    return float(a.mean() - b.mean()), float(np.median(a) - np.median(b))


def block_permutation(fwd: np.ndarray, labels: np.ndarray, rng: np.random.Generator,
                      n_perm: int) -> dict:
    """블록 순서만 섞어 라벨을 재배치(run 길이·BEAR 일수 보존), fwd는 제자리."""
    blk_lab, blk_len = runs_of(labels)
    is_bear = labels == BEAR
    obs_mean, obs_med = stat_pair(fwd, is_bear)
    perm_mean = np.empty(n_perm)
    perm_med = np.empty(n_perm)
    bear_blk = blk_lab == BEAR
    for k in range(n_perm):
        p = rng.permutation(len(blk_len))
        mask = np.repeat(bear_blk[p], blk_len[p])
        perm_mean[k], perm_med[k] = stat_pair(fwd, mask)

    def p_one(obs, dist):   # 관측이 음(BEAR가 더 나쁨)인 방향의 단측
        return (1 + int((dist <= obs).sum())) / (1 + n_perm)

    def p_two(obs, dist):
        return (1 + int((np.abs(dist) >= abs(obs)).sum())) / (1 + n_perm)

    return {
        "mean_diff": obs_mean, "median_diff": obs_med,
        "p_one_mean": p_one(obs_mean, perm_mean), "p_two_mean": p_two(obs_mean, perm_mean),
        "p_one_median": p_one(obs_med, perm_med), "p_two_median": p_two(obs_med, perm_med),
        "n_blocks": int(len(blk_len)), "n_bear_blocks": int(bear_blk.sum()),
        "perm_mean_sd": float(perm_mean.std(ddof=1)),
    }


# ════════════════════════════════════════════════════════════════════════════
# 3. A-2 라벨 품질
# ════════════════════════════════════════════════════════════════════════════
def label_quality(dates: pd.Series, labels: np.ndarray) -> list[dict]:
    blk_lab, blk_len = runs_of(labels)
    years = (dates.iloc[-1] - dates.iloc[0]).days / 365.25
    flips = max(len(blk_len) - 1, 0)
    rows = []
    for lab in (BULL, NEUTRAL, BEAR):
        m = blk_lab == lab
        rows.append({
            "label": lab, "episodes": int(m.sum()),
            "days": int(blk_len[m].sum()),
            "mean_dwell": float(blk_len[m].mean()) if m.any() else float("nan"),
            "median_dwell": float(np.median(blk_len[m])) if m.any() else float("nan"),
            "share_pct": float(blk_len[m].sum() / len(labels) * 100.0),
        })
    rows.append({"label": "ALL", "episodes": int(len(blk_len)), "days": int(len(labels)),
                 "mean_dwell": float(blk_len.mean()), "median_dwell": float(np.median(blk_len)),
                 "share_pct": 100.0, "flips_total": flips, "flips_per_year": flips / years,
                 "years": years})
    return rows


def crisis_lags(df: pd.DataFrame, labels: np.ndarray) -> list[dict]:
    """창별: 창 안 종가 max-dd 저점(T) 대비 BEAR 진입(첫 BEAR 결정일)·해제(T를 포함하거나 T 직전에
    시작한 BEAR 에피소드가 끝난 뒤 첫 non-BEAR일) lag, 거래일 단위(음수=저점보다 앞)."""
    dates = df["date"]
    close = df["close"].to_numpy()
    rows = []
    for name, d0, d1 in CRISIS_WINDOWS:
        idx = np.flatnonzero((dates >= pd.Timestamp(d0)) & (dates <= pd.Timestamp(d1)))
        if len(idx) == 0:
            continue
        seg = close[idx]
        peak = np.maximum.accumulate(seg)
        dd = seg / peak - 1.0
        t_local = int(dd.argmin())
        t = idx[t_local]
        peak_local = int(seg[: t_local + 1].argmax())
        lab_w = labels[idx]
        bear_pos = np.flatnonzero(lab_w == BEAR)
        row = {
            "window": name, "start": d0, "end": d1,
            "peak_date": dates.iloc[idx[peak_local]].date().isoformat(),
            "trough_date": dates.iloc[t].date().isoformat(),
            "max_dd_pct": float(dd[t_local] * 100.0),
            "bear_days": int(len(bear_pos)), "window_days": int(len(idx)),
            "bear_cover_pct": float(len(bear_pos) / len(idx) * 100.0),
        }
        if len(bear_pos) == 0:
            row.update({"entry_lag": None, "exit_lag": None, "note": "창 안 BEAR 없음"})
            rows.append(row)
            continue
        first_bear = idx[bear_pos[0]]
        row["entry_lag"] = int(first_bear - t)
        row["first_bear_date"] = dates.iloc[first_bear].date().isoformat()
        notes = []
        if first_bear == idx[0] and first_bear > 0 and labels[first_bear - 1] == BEAR:
            notes.append("창 시작 시점에 이미 BEAR(진입은 창 이전)")
        # BEAR 에피소드 [s, e] 목록 → 저점을 덮는 것 > 저점 전에 시작한 마지막 것 > 저점 뒤 첫 것
        episodes = []
        i = 0
        while i < len(labels):
            if labels[i] == BEAR:
                s = i
                while i + 1 < len(labels) and labels[i + 1] == BEAR:
                    i += 1
                episodes.append((s, i))
            i += 1
        in_win = [(s, e) for s, e in episodes if e >= idx[0] and s <= idx[-1]]
        cover = [(s, e) for s, e in in_win if s <= t <= e]
        before = [(s, e) for s, e in in_win if e < t]
        after = [(s, e) for s, e in in_win if s > t]
        if cover:
            s, e = cover[0]
        elif before:
            s, e = before[-1]
            notes.append("저점 시점엔 BEAR 아님(저점 전에 해제)")
        else:
            s, e = after[0]
            notes.append("저점 뒤 진입한 에피소드 기준")
        exit_i = e + 1
        if exit_i >= len(labels):
            row["exit_lag"] = None
            notes.append("데이터 끝까지 BEAR")
        else:
            row["exit_lag"] = int(exit_i - t)
            row["exit_date"] = dates.iloc[exit_i].date().isoformat()
        row["note"] = "; ".join(notes)
        rows.append(row)
    return rows


# ════════════════════════════════════════════════════════════════════════════
# 4. 산출
# ════════════════════════════════════════════════════════════════════════════
def fmt(x, nd=4):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:.{nd}f}"
    return str(x)


def main() -> None:
    commit = _git_commit()
    print(f"A-1 v0 라벨 순열검정  seed={SEED}  n_perm={N_PERM}  commit={commit}")
    df = load_bars()
    print(f"  봉 {len(df)}개  {df['date'].iloc[0].date()} ~ {df['date'].iloc[-1].date()}")

    labels_all = np.array(label_bars(df["close"].tolist()))
    opens = df["open"].to_numpy(dtype=float)
    years = df["date"].dt.year.to_numpy()
    fwd_by_h = {h: fwd_returns(opens, h) for h in HORIZONS}

    known = labels_all != UNKNOWN
    print(f"  UNKNOWN(워밍업) {int((~known).sum())}봉 제외, 첫 라벨일 {df['date'][known].iloc[0].date()}")

    # ── 순열 결과 ──
    perm_rows = []
    for scope_id, scope_desc, excl_years, is_holdout in SCOPES:
        for h in HORIZONS:
            fwd = fwd_by_h[h]
            valid = known & ~np.isnan(fwd) & (years >= START_YEAR)
            if is_holdout:
                valid &= years == HOLDOUT_YEAR
            else:
                valid &= ~np.isin(years, list(excl_years))
            f, lab = fwd[valid], labels_all[valid]
            is_bear = lab == BEAR
            row = {"scope": scope_id, "desc": scope_desc, "horizon": h,
                   "n_bars": int(valid.sum()), "n_bear": int(is_bear.sum()),
                   "n_nonbear": int((~is_bear).sum()),
                   "mean_bear": float(f[is_bear].mean()) if is_bear.any() else None,
                   "mean_nonbear": float(f[~is_bear].mean()) if (~is_bear).any() else None}
            blk_lab, blk_len = runs_of(lab)
            row["n_blocks"] = int(len(blk_len))
            row["n_bear_blocks"] = int((blk_lab == BEAR).sum())
            if is_holdout or not is_bear.any() or is_bear.all():
                md, mdn = stat_pair(f, is_bear) if (is_bear.any() and not is_bear.all()) else (None, None)
                row.update({"mean_diff": md, "median_diff": mdn,
                            "sign": ("음(BEAR 더 나쁨)" if md is not None and md < 0 else
                                     "양(BEAR 더 좋음)" if md is not None else "-"),
                            "p_one_mean": None, "p_two_mean": None,
                            "p_one_median": None, "p_two_median": None, "perm_mean_sd": None})
            else:
                rng = np.random.default_rng(SEED)          # scope×horizon마다 같은 시드로 재현
                res = block_permutation(f, lab, rng, N_PERM)
                row.update(res)
                row["sign"] = "음(BEAR 더 나쁨)" if res["mean_diff"] < 0 else "양(BEAR 더 좋음)"
            perm_rows.append(row)
            print(f"  [{scope_id:22s}] H={h:2d} n={row['n_bars']:5d} bear={row['n_bear']:4d}/{row['n_bear_blocks']:3d}ep "
                  f"mean_diff={fmt(row['mean_diff'])} median_diff={fmt(row['median_diff'])} "
                  f"p1={fmt(row['p_one_mean'])} p2={fmt(row['p_two_mean'])}")

    # ── A-2 품질 ──
    q_mask = known & (years >= START_YEAR)
    quality = label_quality(df["date"][q_mask].reset_index(drop=True), labels_all[q_mask])
    lags = crisis_lags(df, labels_all)

    # ── TSV ──
    L = [f"# quant.regime_a1_permutation/v1  seed={SEED}  n_perm={N_PERM}  commit={commit}",
         f"# data={DATA_PATH.relative_to(_REPO).as_posix()}  label=score_v0→classify_v0 (closes[:D+1])",
         f"# fwdH=open[D+1+H]/open[D+1]-1  primary_H={PRIMARY_H}  shuffle=episode-block  p=(1+k)/(1+N)",
         "# section=permutation"]
    pcols = ["scope", "horizon", "n_bars", "n_bear", "n_nonbear", "n_blocks", "n_bear_blocks",
             "mean_bear", "mean_nonbear", "mean_diff", "median_diff", "sign",
             "p_one_mean", "p_two_mean", "p_one_median", "p_two_median", "perm_mean_sd"]
    L.append("\t".join(pcols))
    for r in perm_rows:
        L.append("\t".join(fmt(r.get(c), 6) for c in pcols))
    L.append("# section=label_quality (1997~ UNKNOWN 제외 전 봉)")
    qcols = ["label", "episodes", "days", "mean_dwell", "median_dwell", "share_pct", "flips_total", "flips_per_year", "years"]
    L.append("\t".join(qcols))
    for r in quality:
        L.append("\t".join(fmt(r.get(c), 2) for c in qcols))
    L.append("# section=crisis_lags (lag 단위=거래일, 음수=저점보다 앞)")
    ccols = ["window", "start", "end", "peak_date", "trough_date", "max_dd_pct", "window_days", "bear_days",
             "bear_cover_pct", "first_bear_date", "entry_lag", "exit_date", "exit_lag", "note"]
    L.append("\t".join(ccols))
    for r in lags:
        L.append("\t".join(fmt(r.get(c), 1) for c in ccols))
    OUT_TSV.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"  [산출] {OUT_TSV}")

    # ── MD ──
    def prow(r):
        return (f"| {r['desc']} | {r['horizon']} | {r['n_bars']} | {r['n_bear']} / {r['n_bear_blocks']} | "
                f"{fmt(r['mean_diff']*100 if r['mean_diff'] is not None else None, 2)} | "
                f"{fmt(r['median_diff']*100 if r['median_diff'] is not None else None, 2)} | "
                f"{r['sign']} | {fmt(r['p_one_mean'], 3)} | {fmt(r['p_two_mean'], 3)} | "
                f"{fmt(r['p_one_median'], 3)} | {fmt(r['p_two_median'], 3)} |")

    main_rows = [r for r in perm_rows if r["scope"] == "main_1997_2026_ex2022"]
    primary = next(r for r in main_rows if r["horizon"] == PRIMARY_H)
    passed = primary["p_one_mean"] < 0.05 and primary["mean_diff"] < 0
    if passed:
        verdict = "라벨에 정보 있음 → 다음은 배정 문제"
    else:
        verdict = "라벨이 지수 수익을 못 가름 → 배정 튜닝 무의미"

    M = ["# A-1 · v0 국면 라벨의 forward 수익 분리력 순열검정 (+ A-2 라벨 품질표)", ""]
    M.append("> 회의 `MEETING_2026-09-11_regime_selection.md` A-1·A-2 실행 결과. 판정 기준은 회의에서 사전 고정한 대로 "
             "주지표 H=20의 단측 p<0.05 그리고 관측 통계량 음(BEAR가 더 나쁨).")
    M.append("")
    M.append("## 스펙")
    M.append(f"- 입력: `{DATA_PATH.relative_to(_REPO).as_posix()}` 한 파일 (봉 {len(df)}개, "
             f"{df['date'].iloc[0].date()} ~ {df['date'].iloc[-1].date()}). 네트워크 없음.")
    M.append("- 라벨: 봉마다 결정일 D 종가까지의 closes로 `score_v0` → `classify_v0`(임계 ±2, C++ 패리티). "
             f"워밍업 None은 UNKNOWN으로 제외({int((~known).sum())}봉, 첫 라벨일 {df['date'][known].iloc[0].date()}).")
    M.append("- 수익: fwdH = open[D+1+H] / open[D+1] − 1 (D+1 시가 진입, D+1+H 시가 청산). H=20 사전 고정 주지표, 5/10/60 탐색용. "
             "미래 봉이 없는 마지막 H+1 결정일은 버린다.")
    M.append("- 통계량: mean(fwd|BEAR) − mean(fwd|non-BEAR), 중앙값 차이 병기. 표 단위는 %p.")
    M.append(f"- 순열: 에피소드 블록 셔플 {N_PERM}회(같은 라벨 연속 구간을 블록으로 묶어 블록 순서만 섞음, run 길이·BEAR 일수 보존). "
             f"seed={SEED}. p=(1+k)/(1+N). 단측은 관측이 음인 방향, 양측은 |통계량| 기준.")
    M.append(f"- 구간: 본검정은 {START_YEAR}~2026 전 봉에서 2022(홀드아웃)만 뺀 것. 감도는 달력 연도 1997-98·2008을 뺀다. "
             "2022는 부호와 관측 통계량만 적고 순열 p는 내지 않는다. '참고' 행은 2022를 포함한 전 봉으로, 사전 고정 판정에는 쓰지 않는다.")
    M.append("- 비용·슬리피지 없음(라벨 분리력 검정이라 체결 비용은 대상이 아님). fwd 수익은 H일 겹침이 있어 일별 독립이 아니며, "
             "블록 셔플이 이 자기상관을 부분적으로만 다룬다.")
    M.append("")

    hdr = ("| 구간 | H | 표본(봉) | BEAR 일수 / 에피소드 | 평균차 %p | 중앙값차 %p | 부호 | p 단측(평균) | p 양측(평균) | p 단측(중앙값) | p 양측(중앙값) |\n"
           "|---|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|")
    M.append("## 결과 — 본검정")
    M.append(hdr)
    for r in main_rows:
        M.append(prow(r))
    M.append("")
    M.append("## 결과 — 감도 (H=20 주지표, 다른 H는 tsv)")
    M.append(hdr)
    for sid in ("sens_ex_1997_98", "sens_ex_2008", "sens_ex_both"):
        for r in perm_rows:
            if r["scope"] == sid and r["horizon"] == PRIMARY_H:
                M.append(prow(r))
    M.append("")
    M.append("## 결과 — 2022 홀드아웃 (부호·관측 통계량만, 순열 p 없음)")
    M.append(hdr)
    for r in perm_rows:
        if r["scope"] == "holdout_2022":
            M.append(prow(r))
    M.append("")
    M.append("## 참고 — 2022 포함 전 봉")
    M.append(hdr)
    for r in perm_rows:
        if r["scope"] == "ref_all_incl_2022" and r["horizon"] == PRIMARY_H:
            M.append(prow(r))
    M.append("")

    M.append("## A-2 라벨 품질표 (1997~2026, UNKNOWN 제외)")
    M.append("| 라벨 | 에피소드 수 | 일수 | 비중 % | 평균 dwell(거래일) | 중앙값 dwell |")
    M.append("|---|---:|---:|---:|---:|---:|")
    for r in quality:
        if r["label"] != "ALL":
            M.append(f"| {r['label']} | {r['episodes']} | {r['days']} | {fmt(r['share_pct'], 1)} | {fmt(r['mean_dwell'], 1)} | {fmt(r['median_dwell'], 1)} |")
    a = next(r for r in quality if r["label"] == "ALL")
    M.append(f"| 전체 | {a['episodes']} | {a['days']} | 100 | {fmt(a['mean_dwell'], 1)} | {fmt(a['median_dwell'], 1)} |")
    M.append("")
    M.append(f"- flips(라벨 전환) 총 {a['flips_total']}회, 연간 {fmt(a['flips_per_year'], 1)}회 ({fmt(a['years'], 1)}년).")
    M.append("")
    M.append("### 위기 창별 저점 기준점 대비 BEAR 진입·해제 lag")
    M.append("창은 `research/studies/08_crisis_response/backtest_crisis_response.py`의 EVENTS 중 코스피 데이터 범위 안에 드는 14개를 재사용했다. "
             "저점 기준점 = 창 안 종가 기준 max-dd 저점. 진입 lag = 창 안 첫 BEAR 결정일 − 저점(거래일, 음수=저점보다 앞). "
             "해제 lag = 저점을 덮거나 저점 전에 시작한 마지막 BEAR 에피소드가 끝난 뒤 첫 non-BEAR일 − 저점.")
    M.append("")
    M.append("| 창 | 기간 | 고점 | 저점 | max-dd % | BEAR 일수/창 일수 | 첫 BEAR | 진입 lag | 해제일 | 해제 lag | 비고 |")
    M.append("|---|---|---|---|---:|---:|---|---:|---|---:|---|")
    for r in lags:
        M.append(f"| {r['window']} | {r['start']}~{r['end']} | {r['peak_date']} | {r['trough_date']} | {fmt(r['max_dd_pct'], 1)} | "
                 f"{r['bear_days']}/{r['window_days']} | {r.get('first_bear_date', '-')} | {fmt(r.get('entry_lag'))} | "
                 f"{r.get('exit_date', '-')} | {fmt(r.get('exit_lag'))} | {r.get('note', '')} |")
    M.append("")

    M.append("## 결론")
    M.append(f"- 주지표 H=20 본검정: 평균차 {primary['mean_diff']*100:+.2f}%p, 중앙값차 {primary['median_diff']*100:+.2f}%p, "
             f"p 단측 {primary['p_one_mean']:.3f} / 양측 {primary['p_two_mean']:.3f} "
             f"(BEAR {primary['n_bear']}일 / {primary['n_bear_blocks']}에피소드, 표본 {primary['n_bars']}봉).")
    M.append(f"- 회의 판정 기준 적용: **{verdict}**.")
    M.append("- 이 문서는 결과이지 결론 승격이 아니다. 편향 감사(@bias-auditor)와 성과 판정(@quant-analyst)은 별도. "
             "통과하더라도 로스터 승격은 A-3 조건 뒤다.")
    M.append("")
    M.append("## 재현 정보")
    M.append(f"- seed={SEED} · n_perm={N_PERM} · commit={commit} · 스코어러 `PYQuant/backtest/regime_scorer.py`(미수정 import)")
    M.append(f"- 재실행: `cd research/studies/10_regime_scorer && py a1_permutation.py` → `a1_results.tsv`, `A1_REPORT.md`")
    M.append("")
    M.append("← [스터디 README](README.md)")
    OUT_MD.write_text("\n".join(M) + "\n", encoding="utf-8")
    print(f"  [산출] {OUT_MD}")
    print(f"판정(H=20): {verdict}")


if __name__ == "__main__":
    main()
