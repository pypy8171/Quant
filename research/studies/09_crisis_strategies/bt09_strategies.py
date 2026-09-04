#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BT-09 전략 계층 — 방어 5(C1~C5) + 공세 5(O1~O5) 익스포저 함수 + STRATS 레지스트리.

각 expo_* 는 (closes, ret, ctx, p) → 익스포저 배열 e[t] 를 낸다.
 - 방어(C*): 기본 1, 위험 국면에서 e↓(현금비중↑).
 - 공세(O*): 기본 1, 기회 국면에서 e↑(상한 OFF_CAP=1.2, 완만 레버리지).
룩어헤드 차단은 신호 계층(align_ff strict) + run_curve e[t-1] 이 담당 — 여기선 e[t]가
오직 close t 까지의 ctx 값만 참조한다.
"""
import sys
from pathlib import Path

import numpy as np

# ── 부트스트랩(중복 실행 안전): BT-08 엔진(sma/rolling_vol) + 신호 계층 배선 ────────────
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]
_PYQ = _REPO / "PYQuant"
_BT08 = _REPO / "research" / "studies" / "08_crisis_response"
for _p in (str(_PYQ), str(_BT08)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import backtest_crisis_response as bt08                       # noqa: E402  (sma·rolling_vol)
from bt09_signals import roll_z, roll_pctrank, roll_max, roll_min, _ok   # noqa: E402


# ════════════════════════════════════════════════════════════════════════════
# 전략 10종 → 익스포저 e[t]  (방어: 기본 1, 위험시↓ / 공세: 기본 1, 기회시↑, 상한 1.2)
# ════════════════════════════════════════════════════════════════════════════
OFF_CAP = 1.2   # 공세 레버리지 상한(완만)


def expo_c1_supplyshock(closes, ret, ctx, p):
    """C1 유가·금리 동반상승 = 공급충격 게이트. oil20d≥+10% AND tnx20d≥+0.3%p AND C<SMA50 → 0.4.
    원인구분: 유가↑와 금리↑가 함께 = 비용상승(공급). 수요견인과 반대."""
    s50 = bt08.sma(closes, 50)
    o, t20 = ctx["oil_20d"], ctx["tnx_20d"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if _ok(o[t]) and _ok(t20[t]) and not np.isnan(s50[t]):
            if o[t] >= 0.10 and t20[t] >= 0.30 and closes[t] < s50[t]:
                e[t] = 0.4
    return e


def expo_c2_riskoff_triple(closes, ret, ctx, p):
    """C2 리스크오프 삼중확인. z(KRW5d)>+1 AND z(DXY20d)>+0.8 AND z(ΔVIX5d)>+0.8 → 0.3.
    (동행지표 — 사전방어 아님. 크로스에셋 동시 리스크오프 반응.) KRW 없는 US엔 2요소판."""
    zk, zd, zv = ctx["z_krw_d5"], ctx["z_dxy_20d"], ctx["z_vix_d5"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        cond_d = _ok(zd[t]) and zd[t] > 0.8
        cond_v = _ok(zv[t]) and zv[t] > 0.8
        if _ok(zk[t]):
            if zk[t] > 1.0 and cond_d and cond_v:
                e[t] = 0.3
        else:  # KRW 결측(2003 이전·US) → DXY+VIX 2요소
            if cond_d and cond_v:
                e[t] = 0.3
    return e


def expo_c3_vix_shock(closes, ret, ctx, p):
    """C3 VIX 급등 반응컷. VIX 1d≥+25% OR z(ΔVIX3d)≥+2 → COOL일 e=0. 재진입: VIX<VIX_SMA20.
    (반응·꼬리컷 — M4류. 사전방어 아님.)"""
    cool = p.get("cool", 5)
    v1, zv3, vsma = ctx["vix_1d"], ctx["z_vix_d3"], ctx["vix_sma20"]
    vix = ctx["vix"]
    e = np.ones(len(closes))
    cd, out = 0, False
    for t in range(len(closes)):
        shock = (_ok(v1[t]) and v1[t] >= 0.25) or (_ok(zv3[t]) and zv3[t] >= 2.0)
        if shock:
            out, cd = True, cool
        if out:
            if cd > 0:
                cd -= 1
                e[t] = 0.0
            else:
                reenter = _ok(vix[t]) and _ok(vsma[t]) and vix[t] < vsma[t]
                if reenter:
                    out = False
                    e[t] = 1.0
                else:
                    e[t] = 0.0
    return e


def expo_c4_rateshock(closes, ret, ctx, p):
    """C4 금리충격 디리스킹. tnx20d≥+0.6%p AND tnx>SMA100(tnx) → 0.5. (2022형)."""
    thr = p.get("thr", 0.60)
    t20, tnx, ts = ctx["tnx_20d"], ctx["tnx"], ctx["tnx_sma100"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if _ok(t20[t]) and _ok(tnx[t]) and _ok(ts[t]):
            if t20[t] >= thr and tnx[t] > ts[t]:
                e[t] = 0.5
    return e


def expo_c5_overnight_gap(closes, ret, ctx, p):
    """C5 US 오버나잇 갭 방어. 직전 US(^GSPC) 일수익≤-2% AND VIX 상승 → 익일 e=0.3.
    KR: strict정렬로 '직전 미국세션'. US: 어제 종가(run_curve e[t-1]로 합법)."""
    gr, vix, vl5 = ctx["gspc_ret"], ctx["vix"], ctx["vix_lag5"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if _ok(gr[t]) and gr[t] <= -0.02:
            vix_up = _ok(vix[t]) and _ok(vl5[t]) and vix[t] > vl5[t]
            if vix_up or not _ok(vix[t]):   # VIX결측이면 갭조건만
                e[t] = 0.3
    return e


def expo_o1_regime_switch(closes, ret, ctx, p):
    """O1 국면전환 추세/평균회귀. C>SMA200(추세국면): C>SMA50→1 else 0.5.
    C<SMA200(횡보·약세): z(close,20)<-1(과매도)→1.2 else 0.5. 가격전용(전구간)."""
    s200 = bt08.sma(closes, 200)
    s50 = bt08.sma(closes, 50)
    zc = roll_z(closes, 20)
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if np.isnan(s200[t]) or np.isnan(s50[t]):
            e[t] = 1.0
            continue
        if closes[t] > s200[t]:
            e[t] = 1.0 if closes[t] > s50[t] else 0.5
        else:
            e[t] = OFF_CAP if (_ok(zc[t]) and zc[t] < -1.0) else 0.5
    return e


def expo_o2_vol_breakout(closes, ret, ctx, p):
    """O2 저변동성 압축 후 채널돌파. rvol20 백분위<20%(압축) 상태서 C>20일고가→1.2 진입,
    C<20일저가→0.6 청산(홀드 상태유지). 가격전용."""
    rv = bt08.rolling_vol(ret, 20)
    rank = roll_pctrank(rv, 252)
    hi20 = roll_max(closes, 20)
    lo20 = roll_min(closes, 20)
    e = np.ones(len(closes))
    on = False
    for t in range(len(closes)):
        if not on:
            compressed = _ok(rank[t]) and rank[t] < 0.20
            brk = (not np.isnan(hi20[t - 1])) and t >= 1 and closes[t] > hi20[t - 1]
            if compressed and brk:
                on = True
                e[t] = OFF_CAP
            else:
                e[t] = 1.0
        else:
            if (not np.isnan(lo20[t - 1])) and closes[t] < lo20[t - 1]:
                on = False
                e[t] = 0.6
            else:
                e[t] = OFF_CAP
    return e


def expo_o3_sox_lead(closes, ret, ctx, p):
    """O3 SOX(반도체) 선행 → 지수 가산. ^SOX>SMA50 AND SMA50 상승기울기 → e=1.1.
    KR: strict(직전 US세션). US: 동일세션(SOX·GSPC 동일 tz)."""
    sox, ss = ctx["sox"], ctx["sox_sma50"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if _ok(sox[t]) and _ok(ss[t]) and t >= 20 and _ok(ss[t - 20]):
            if sox[t] > ss[t] and ss[t] > ss[t - 20]:
                e[t] = 1.1
    return e


def expo_o4_demandpull(closes, ret, ctx, p):
    """O4 유가 수요견인 리스크온. oil20d≥+10% AND tnx20d<+0.3%p(수요·공급아님) AND C>SMA50
    AND ^SOX>SMA50 → 1.2. C1의 거울(유가↑인데 금리는 잠잠 = 성장수요)."""
    s50 = bt08.sma(closes, 50)
    o, t20, sox, ss = ctx["oil_20d"], ctx["tnx_20d"], ctx["sox"], ctx["sox_sma50"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if _ok(o[t]) and _ok(t20[t]) and not np.isnan(s50[t]):
            sox_ok = (not _ok(sox[t])) or (_ok(ss[t]) and sox[t] > ss[t])
            if o[t] >= 0.10 and t20[t] < 0.30 and closes[t] > s50[t] and sox_ok:
                e[t] = OFF_CAP
    return e


def expo_o5_crossasset_rebound(closes, ret, ctx, p):
    """O5 크로스에셋 조기 재진입. 낙폭(C<SMA50)에서 VIX 롤오버(VIX<VIX5d전) AND
    tnx5d≤+0.1%p(금리안정) AND DXY 롤오버(DXY<DXY10d전) → 1.2. (동행·조기 — 사전 아님.)"""
    s50 = bt08.sma(closes, 50)
    vix, vl5 = ctx["vix"], ctx["vix_lag5"]
    t5 = ctx["tnx_5d"]
    dxy, dl10 = ctx["dxy"], ctx["dxy_lag10"]
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if np.isnan(s50[t]) or closes[t] >= s50[t]:
            continue
        vix_roll = _ok(vix[t]) and _ok(vl5[t]) and vix[t] < vl5[t]
        rate_ok = _ok(t5[t]) and t5[t] <= 0.10
        dxy_roll = _ok(dxy[t]) and _ok(dl10[t]) and dxy[t] < dl10[t]
        if vix_roll and rate_ok and dxy_roll:
            e[t] = OFF_CAP
    return e


def _n4_core(closes, ret, ctx, p):
    """N4 변동성 타게팅 연속 익스포저 코어(방어 오버레이·저토글).
    e_t = clip(target_vol_{t-1} / rvol20_{t-1}, 0.5, 1.2).
      - rvol20 = 최근 20거래일 일수익 std × √252(연율). **t-1 까지 룩백만**(당일 종가 미포함)
        → rolling_vol(ret,20)[t-1]. run_curve 의 e[t-1] 배선과 합쳐 이중 룩어헤드 차단.
      - 데드밴드 0.05: |목표 e − 직전 커밋 e| < 0.05 면 직전 e 유지(미세 토글 억제).
      - target_vol 앵커는 mode 로 결정(누출 원천 통제):
          "leak"   : std(ret[전표본])×√252 — 2022 홀드아웃 포함 = **미래정보 누출**(감사 지적, 비교용).
          "expand" : std(ret[:t])×√252 인과확장창(t 이전만). warmup 252봉 e=1(앵커 안정화 후 시작).
                     확장창이라 자동 train-only → 2022 홀드아웃 앵커오염 없음.
          "const"  : 시장무관 사전등록 상수(기본 15%). 전표본·train 어느 것도 안 봄 = 누출원천 제거.
    20일 후행이라 1987·2020 갭형 급락엔 반응이 늦다(설계상 한계)."""
    mode = p.get("mode", "expand")
    tv_const = p.get("target_vol", 0.15)
    n = len(closes)
    rv = bt08.rolling_vol(ret, 20)                     # rv[t] = close t 까지 20일 연율변동성
    if mode == "const":
        anchor = np.full(n, float(tv_const))
        warm = 20
    elif mode == "leak":                               # 감사 지적: 전표본(2022 포함) — 비교용만
        tv = float(np.std(ret[1:], ddof=1)) * bt08.ANNUAL if n > 2 else tv_const
        anchor = np.full(n, tv)
        warm = 20
    else:                                              # expand: 인과 확장창 std(ret[:t])
        anchor = np.full(n, np.nan)
        sx = sx2 = 0.0
        cnt = 0
        for k in range(1, n):                          # ret[0]=0 제외
            r = ret[k]
            sx += r; sx2 += r * r; cnt += 1
            if cnt >= 2:
                var = (sx2 - sx * sx / cnt) / (cnt - 1)
                anchor[k] = np.sqrt(var if var > 0 else 0.0) * bt08.ANNUAL
        warm = 252                                     # 앵커 안정화 후 시작
    e = np.ones(n)
    prev = 1.0                                         # 직전 커밋 익스포저(초기 BH)
    for t in range(n):
        rvl = rv[t - 1] if t >= 1 else np.nan          # t-1 까지 룩백(당일 종가 제외)
        tvl = anchor[t - 1] if t >= 1 else np.nan      # 앵커도 t-1 까지(인과)
        if t < warm or not _ok(rvl) or rvl <= 0 or not _ok(tvl) or tvl <= 0:
            e[t] = prev                                # 워밍업·결측 → 직전 유지(초기 1.0)
            continue
        target = min(1.2, max(0.5, tvl / rvl))         # clip(0.5, 1.2)
        if abs(target - prev) < 0.05:                  # 데드밴드 → 이전 e 유지
            e[t] = prev
        else:
            e[t] = target
            prev = target
    return e


def expo_n4_voltarget(closes, ret, ctx, p):
    """N4(누출본) — target=전표본 실현변동성. 감사 CONDITIONAL 지적 대상(비교용 유지)."""
    return _n4_core(closes, ret, ctx, {"mode": "leak"})


def expo_n4a_expand(closes, ret, ctx, p):
    """N4a 변형 — 인과 확장창 앵커 std(ret[:t]). 홀드아웃 자동 순수(누출 제거)."""
    return _n4_core(closes, ret, ctx, {"mode": "expand"})


def expo_n4b_const(closes, ret, ctx, p):
    """N4b 변형 — 사전등록 상수 target=15%. 전표본·train 미참조(누출원천 제거)."""
    return _n4_core(closes, ret, ctx, {"mode": "const", "target_vol": 0.15})


STRATS = [
    # code, label, fn, side, desc, needs
    ("C1", "유가·금리 공급충격 게이트",  expo_c1_supplyshock,     "방어",
     "oil20d≥+10% & tnx20d≥+0.3%p & C<SMA50 → 0.4", "CL=F·^TNX(2000+)"),
    ("C2", "리스크오프 삼중확인",        expo_c2_riskoff_triple,  "방어",
     "z(KRW5d)>1 & z(DXY20d)>0.8 & z(ΔVIX5d)>0.8 → 0.3", "KRW·DXY·VIX(동행)"),
    ("C3", "VIX 급등 반응컷",           expo_c3_vix_shock,       "방어",
     "VIX 1d≥+25% or z(ΔVIX3d)≥+2 → 5일 컷", "^VIX(1990+·반응)"),
    ("C4", "금리충격 디리스킹",          expo_c4_rateshock,       "방어",
     "tnx20d≥+0.6%p & tnx>SMA100 → 0.5", "^TNX(1985+)"),
    ("C5", "US 오버나잇 갭 방어",        expo_c5_overnight_gap,   "방어",
     "직전 US≤-2% & VIX↑ → 0.3", "^GSPC·^VIX(시차합법)"),
    ("O1", "국면전환 추세/평균회귀",     expo_o1_regime_switch,   "공세",
     "추세국면=TF, 횡보=과매도매수(1.2)", "가격전용(전구간)"),
    ("O2", "저변동성 압축 돌파",         expo_o2_vol_breakout,    "공세",
     "rvol 압축<20%ile 후 20일고가 돌파→1.2", "가격전용(전구간)"),
    ("O3", "SOX 반도체 선행 가산",       expo_o3_sox_lead,        "공세",
     "^SOX>SMA50·상승기울기 → 1.1", "^SOX(1994+)"),
    ("O4", "유가 수요견인 리스크온",     expo_o4_demandpull,      "공세",
     "oil20d≥+10% & tnx잠잠 & C>SMA50 & SOX↑→1.2", "CL=F·^TNX·^SOX"),
    ("O5", "크로스에셋 조기 재진입",     expo_o5_crossasset_rebound, "공세",
     "낙폭중 VIX·DXY 롤오버 & 금리안정 → 1.2", "VIX·TNX·DXY(조기)"),
    ("N4", "변동성 타게팅(누출본·감사지적)", expo_n4_voltarget,     "방어",
     "target=전표본 실현변동성(2022 홀드아웃 포함 누출)·비교용", "가격전용(전구간)"),
    ("N4a", "변동성 타게팅·인과확장창",   expo_n4a_expand,          "방어",
     "target=std(ret[:t]) 확장창(t이전만)·warmup252·홀드아웃 순수", "가격전용(전구간)"),
    ("N4b", "변동성 타게팅·상수15%",      expo_n4b_const,           "방어",
     "target=15% 사전등록상수(전표본·train 미참조)·clip0.5~1.2·데드밴드0.05", "가격전용(전구간)"),
]
