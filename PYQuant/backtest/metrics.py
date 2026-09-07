"""
추세·돌파 매매 평가 지표

평균 수익률과 승률만 보면 추세매매는 구조적으로 음성이 나온다. 승률이 낮고 우측 꼬리가
수익을 끌고 가는 분포이기 때문이다. 여기서는 손절을 명시한 경로 시뮬레이션으로 실현수익을
내고, R배수(위험 1단위당 손익) 기준으로 기대값을 잰다.

핵심 함수는 `evaluate(bars, entries, rule)` 하나다. 게이트가 만든 진입 신호 마스크와 일봉을
받아 지표 dict를 돌려준다.

경로 규칙(보수적으로):
  - 진입은 신호 다음날 시가.
  - 손절·트레일링은 매일 저가로 먼저 판정한다. 같은 날 고가와 저가가 둘 다 닿으면 저가를
    먼저 맞은 것으로 본다(최악 가정).
  - 손절 체결가는 손절선 그 자체로 본다. 갭하락으로 시가가 손절선 아래면 시가로 체결한다.
  - 보유 상한일에 남아 있으면 그날 종가로 청산한다.
비용은 engine.CostModel과 같은 값을 쓴다(왕복 0.31%).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd

# engine.CostModel과 같은 값. 왕복 = 수수료 0.015%×2 + 세 0.18% + 슬리피지 5bp×2 = 0.31%
COMMISSION_RATE = 0.00015
TAX_RATE = 0.0018
SLIPPAGE_BPS = 5.0
ROUNDTRIP_COST_PCT = (COMMISSION_RATE * 2 + TAX_RATE + SLIPPAGE_BPS / 10_000 * 2) * 100


@dataclass
class ExitRule:
    """청산 규칙. 손절폭이 R(위험 1단위)의 정의다."""
    stop_pct: float = 6.0        # 진입가 대비 손절 %(양수)
    trail_pct: float = 0.0       # 최고 종가 대비 트레일링 %. 0이면 트레일링 없음
    max_hold: int = 20           # 보유 상한(거래일)
    target_r: float = 0.0        # 목표 R배수 도달 시 익절. 0이면 목표 없음

    def label(self) -> str:
        parts = [f"손절{self.stop_pct:g}%"]
        if self.trail_pct:
            parts.append(f"트레일{self.trail_pct:g}%")
        if self.target_r:
            parts.append(f"목표{self.target_r:g}R")
        parts.append(f"최대{self.max_hold}일")
        return " ".join(parts)


def simulate(bars: pd.DataFrame, rule: ExitRule) -> pd.DataFrame:
    """한 종목의 모든 날짜에 대해 "그날 신호였다면" 경로를 시뮬레이션한다.

    bars는 Open/High/Low/Close 컬럼과 날짜 인덱스를 갖는다. 진입은 다음날 시가.
    반환 컬럼: entry, exit_price, hold_days, ret_pct(비용 후), r_mult, mfe_pct, mae_pct, exit_why
    """
    n = len(bars)
    op = bars["Open"].to_numpy(float)
    hi = bars["High"].to_numpy(float)
    lo = bars["Low"].to_numpy(float)
    cl = bars["Close"].to_numpy(float)

    entry = np.full(n, np.nan)
    exitp = np.full(n, np.nan)
    hold = np.zeros(n, dtype=int)
    mfe = np.full(n, np.nan)
    mae = np.full(n, np.nan)
    why = np.array(["" for _ in range(n)], dtype=object)

    for i in range(n - 1):
        e = op[i + 1]
        if not np.isfinite(e) or e <= 0:
            continue
        stop = e * (1 - rule.stop_pct / 100.0)
        risk = e - stop
        target = e + rule.target_r * risk if rule.target_r else np.inf
        peak_close = -np.inf
        hi_seen, lo_seen = -np.inf, np.inf

        last = min(i + rule.max_hold, n - 1)
        px, reason, days = cl[last], "기간만료", last - i
        for j in range(i + 1, last + 1):
            hi_seen = max(hi_seen, hi[j])
            lo_seen = min(lo_seen, lo[j])
            days = j - i
            # 갭하락으로 시가가 이미 손절선 아래면 시가 체결
            if op[j] <= stop:
                px, reason = op[j], "손절(갭)"
                break
            # 같은 날 고·저가 모두 닿으면 저가 먼저(최악 가정)
            if lo[j] <= stop:
                px, reason = stop, "손절"
                break
            if rule.trail_pct and peak_close > -np.inf:
                trail = peak_close * (1 - rule.trail_pct / 100.0)
                if lo[j] <= trail:
                    px, reason = min(trail, op[j]), "트레일링"
                    break
            if rule.target_r and hi[j] >= target:
                px, reason = target, "목표"
                break
            peak_close = max(peak_close, cl[j])
            px, reason = cl[j], "기간만료"

        entry[i] = e
        exitp[i] = px
        hold[i] = days
        mfe[i] = (hi_seen / e - 1) * 100 if np.isfinite(hi_seen) else np.nan
        mae[i] = (lo_seen / e - 1) * 100 if np.isfinite(lo_seen) else np.nan
        why[i] = reason

    ret = (exitp / entry - 1) * 100 - ROUNDTRIP_COST_PCT
    return pd.DataFrame(
        {"entry": entry, "exit_price": exitp, "hold_days": hold, "ret_pct": ret,
         "r_mult": ret / rule.stop_pct, "mfe_pct": mfe, "mae_pct": mae, "exit_why": why},
        index=bars.index,
    )


def summarize(ret_pct: pd.Series, r_mult: pd.Series, mfe: pd.Series, mae: pd.Series,
              hold_days: pd.Series | None = None, max_hold: int = 20) -> dict:
    """실현수익 분포를 지표 dict로. 표본은 겹침 보정한 유효 수도 같이 낸다.

    유효표본은 시간축(보유구간 겹침)과 횡단면축(같은 날 종목들이 같이 움직임) 양쪽을 줄인다.
    같은 날짜의 관측은 전부 상관돼 있다고 보고 날짜 하나를 한 표본으로 세는데, 이건 상한이
    아니라 하한이다 — 실제 독립도는 그 사이 어딘가다. 과대추정으로 유의하다고 잘못 말하느니
    과소추정으로 판단을 보류하는 쪽을 택한다.
    """
    r = pd.Series(ret_pct).dropna()
    if len(r) == 0:
        return {"n": 0}
    rm = pd.Series(r_mult).dropna()
    win, loss = r[r > 0], r[r <= 0]
    gross_win, gross_loss = win.sum(), -loss.sum()
    # 보유구간이 겹치면 인접 표본이 독립이 아니다(시간축). 게다가 패널에서는 같은 날 수백
    # 종목이 함께 움직여 횡단면으로도 독립이 아니다. n/max_hold만 쓰면 후자를 통째로 무시해
    # t를 크게 부풀린다 — 날짜 블록 수로도 함께 눌러 둘 중 작은 쪽을 쓴다.
    idx = getattr(r, "index", None)
    n_dates = int(pd.Index(idx).nunique()) if idx is not None else len(r)
    eff = max(1.0, min(len(r), n_dates) / max(1.0, float(max_hold)))
    return {
        "n": len(r),
        "n_dates": n_dates,
        "n_eff": round(eff, 1),
        "expectancy_r": round(rm.mean(), 4),
        "expectancy_pct": round(r.mean(), 4),
        "win_rate": round(len(win) / len(r), 4),
        "payoff": round(win.mean() / abs(loss.mean()), 3) if len(loss) and loss.mean() != 0 else None,
        "profit_factor": round(gross_win / gross_loss, 3) if gross_loss > 0 else None,
        "median_pct": round(r.median(), 4),
        "p75_pct": round(r.quantile(0.75), 4),
        "p90_pct": round(r.quantile(0.90), 4),
        "p10_pct": round(r.quantile(0.10), 4),
        "mfe_med": round(pd.Series(mfe).median(), 3),
        "mae_med": round(pd.Series(mae).median(), 3),
        "mfe_mae": round(abs(pd.Series(mfe).median() / pd.Series(mae).median()), 3)
        if pd.Series(mae).median() else None,
        "hold_med": round(pd.Series(hold_days).median(), 1) if hold_days is not None else None,
        # 겹침·횡단면 보정 t값. 날짜 블록으로 눌러 계산하므로 원표본 t보다 크게 작다.
        # 패널 측정에서는 이 값보다 월 동일가중 1표본 t를 우선한다(D-012 규약).
        "t_eff": round(rm.mean() / (rm.std(ddof=1) / np.sqrt(eff)), 2)
        if rm.std(ddof=1) > 0 else None,
    }


def evaluate(paths: pd.DataFrame, mask: pd.Series, rule: ExitRule) -> dict:
    """게이트 마스크를 경로 시뮬레이션 결과에 적용해 지표를 낸다."""
    s = paths[mask.reindex(paths.index).fillna(False).astype(bool)]
    out = summarize(s.ret_pct, s.r_mult, s.mfe_pct, s.mae_pct, s.hold_days, rule.max_hold)
    out["rule"] = rule.label()
    if len(s):
        out["exit_mix"] = {k: round(v, 3) for k, v in s.exit_why.value_counts(normalize=True).items()}
    return out


def is_significant(m: dict, min_eff: float = 30.0, min_t: float = 2.0) -> bool:
    """표본 부족·유의성 미달을 한 곳에서 판정한다. 겹침·횡단면 보정 유효표본 기준.

    패널(여러 종목 × 여러 날) 측정에서는 이것만으로 판정하지 말고 월 동일가중 초과의
    1표본 t를 함께 봐야 한다. 여기 t_eff는 날짜 블록까지 눌러 놓은 하한 쪽 추정이다.
    """
    return bool(m.get("n_eff", 0) >= min_eff and m.get("t_eff") is not None
                and abs(m["t_eff"]) >= min_t)
