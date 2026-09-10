# -*- coding: utf-8 -*-
"""비중·종목수 규칙을 실측 분포에서 뽑는다.

전제: 이 스터디는 bias-auditor REJECT(엣지 없음)다. 여기 수치는 "엣지가 있다고 가정한
사이징"이 아니라 "이 분포에서 계좌가 버티려면 얼마까지가 한계인가"의 상한 계산이다.
배선 근거로 쓰지 않는다.

검정은 전부 부트스트랩(scipy 없음). 사건 10건은 독립이 아니라 episode 클러스터
부트스트랩을 같이 낸다.

실행:
    py -X utf8 -W ignore research/studies/12_base_breakout/run_sizing.py
    py -X utf8 -W ignore research/studies/12_base_breakout/run_sizing.py --staged
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
rng = np.random.default_rng(20260909)

B = 20000
NS = [10, 20, 30, 50, 100]
FEATS = ["낙폭깊이", "시총로그", "거래대금로그", "거래량급증", "변동성", "저점대비반등",
         "바닥후경과일", "상대강도120", "상대강도20", "직전1년상승", "주가로그", "이평200이격"]

# 사건은 독립이 아니다. 같은 급락 국면에서 나온 사건을 하나로 묶는다.
EPISODE = {
    "2018-11-16": "2018Q4", "2019-01-09": "2018Q4",
    "2019-08-21": "2019H2",
    "2020-03-31": "2020COVID", "2020-04-03": "2020COVID",
    "2022-05-20": "2022H1", "2022-07-11": "2022H1",
    "2022-10-18": "2022Q4", "2022-10-24": "2022Q4",
    "2024-12-12": "2024Q4",
}


def hr(title):
    print("\n" + "=" * 78)
    print(title)
    print("=" * 78)


def spearman(a, b):
    a = pd.Series(a).rank().values
    b = pd.Series(b).rank().values
    if np.std(a) == 0 or np.std(b) == 0:
        return np.nan
    return float(np.corrcoef(a, b)[0, 1])


def cluster_boot_ci(per_event, ep_of, n=8000):
    """episode 단위 재표집. 사건 간 상관을 부분적으로 반영한다."""
    eps = sorted(set(ep_of[k] for k in per_event))
    by = {e: np.array([v for k, v in per_event.items() if ep_of[k] == e], float) for e in eps}
    obs = float(np.mean(list(per_event.values())))
    draws = np.empty(n)
    for b in range(n):
        pick = rng.choice(len(eps), size=len(eps), replace=True)
        draws[b] = np.concatenate([by[eps[i]] for i in pick]).mean()
    return obs, float(np.percentile(draws, 2.5)), float(np.percentile(draws, 97.5))


def draw_portfolios(vals, ns, b=B):
    """vals에서 N종목 비복원 추출한 동일가중 포트 수익. {N: (b,) array}."""
    v = np.asarray(vals, float)
    m = len(v)
    perm = np.argsort(rng.random((b, m)), axis=1)
    cm = np.cumsum(v[perm], axis=1) / np.arange(1, m + 1)
    return {n: cm[:, min(n, m) - 1].copy() for n in ns}


def tail_hit(vals, ns, q=0.95):
    """상위 q분위 이상 종목을 최소 1개 잡을 확률(해석식, 비복원)."""
    m = len(vals)
    k = int(np.ceil(m * (1 - q)))
    out = {}
    for n in ns:
        p = 1.0
        for j in range(min(n, m)):
            p *= (m - k - j) / (m - j) if (m - k - j) > 0 else 0.0
        out[n] = 1 - p
    return out


def hit_sub(sub, thr, ns, m_full):
    """부분집합 sub에서 N개 뽑을 때 전체풀 상위분위(thr 이상)를 1개 이상 잡을 확률."""
    m = len(sub)
    k = int((sub >= thr).sum())
    out = {}
    for n in ns:
        p = 1.0
        for j in range(min(n, m)):
            p *= (m - k - j) / (m - j) if (m - k - j) > 0 else 0.0
        out[n] = 1 - p
    return out


def kelly(returns, fmax=3.0, steps=3001):
    r = np.asarray(returns, float)
    fs = np.linspace(0, fmax, steps)
    best, bf = -1e18, 0.0
    for f in fs:
        y = 1 + f * r
        if np.any(y <= 1e-9):
            break
        g = float(np.mean(np.log(y)))
        if g > best:
            best, bf = g, f
    return bf, best


# ─── 0. 표본·기간·홀드아웃 (지표보다 먼저) ────────────────────────────────────
A = pd.read_csv(RAW / "crosssec.csv", dtype={"code": str})
A26 = pd.read_csv(RAW / "crosssec26.csv", dtype={"code": str})
AP = pd.read_csv(RAW / "apply_2026.csv", dtype={"code": str})
A["event"] = A["event"].astype(str)
A26["event"] = A26["event"].astype(str)
A["r"] = A["x"] + A["bench"]
A26["r"] = A26["x"] + A26["bench"]
AP["r"] = AP["ret"]

EVENTS = sorted(A["event"].unique())
EP_OF = {e: EPISODE[e] for e in EVENTS}

hr("0. 표본 고정 표기 — 지표보다 먼저 읽는다")
print("기간        : %s ~ %s (in-sample 10사건) + 2026-08 (홀드아웃)" % (EVENTS[0], EVENTS[-1]))
print("관측 단위   : 사건 %d건 / 종목-사건 %d건 / 홀드아웃 %d건" % (len(EVENTS), len(A), len(AP)))
print("독립 단위   : episode %d개 %s" % (len(set(EP_OF.values())), sorted(set(EP_OF.values()))))
print("              유효표본은 10이 아니라 %d — 2022년에 4건이 몰려 있다" % len(set(EP_OF.values())))
print("홀드아웃    : 2026-08 사건. 과거 부호로만 만든 점수를 적용")
print("*** 표본 부족(독립 사건 6) — 아래 수치는 크기 추정이고 유의성 결론이 아니다 ***")

print("\n사건별 패널")
head = "%-12s%-7s%6s%9s%9s%9s%9s%9s%11s" % ("event", "mkt", "n", "평균x", "중앙x", "평균r", "중앙r", "지수", "episode")
print(head)
for (ev, mkt), g in A.groupby(["event", "market"]):
    print("%-12s%-7s%6d%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%11s" % (
        ev, mkt, len(g), g.x.mean() * 100, g.x.median() * 100, g.r.mean() * 100,
        g.r.median() * 100, g.bench.iloc[0] * 100, EP_OF[ev]))
g = AP
print("%-12s%-7s%6d%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%11s" % (
    "2026-08", "BOTH", len(g), g.x.mean() * 100, g.x.median() * 100, g.r.mean() * 100,
    g.r.median() * 100, g.bench.mean() * 100, "HOLDOUT"))
print("(단위 %)")

print("\n통합 분포(pooled, 사건 무가중)")
for lab, v in [("초과 x", A.x.values), ("절대 r", A.r.values)]:
    v = np.asarray(v)
    win = v > 0
    print("  %s: 평균 %+.1f%% 중앙 %+.1f%% 표준편차 %.1f%% 왜도 %.2f 승률 %.0f%% 승평균 %+.1f%% 패평균 %+.1f%%" % (
        lab, v.mean() * 100, np.median(v) * 100, v.std(ddof=1) * 100,
        pd.Series(v).skew(), win.mean() * 100, v[win].mean() * 100, v[~win].mean() * 100))
    t95 = np.percentile(v, 95)
    print("        상위5%% 제거시 평균 %+.1f%%   상위5%%가 전체 합의 %.0f%%" % (
        v[v < t95].mean() * 100, v[v >= t95].sum() / v.sum() * 100 if v.sum() != 0 else float("nan")))


def loeo_score(panel):
    """leave-one-event-out 부호로 만든 12피처 순위평균 점수."""
    ic = {}
    for f in FEATS:
        ic[f] = {ev: spearman(g[f].values, g["x"].values) for ev, g in panel.groupby("event")}
    out = panel.copy()
    out["score"] = np.nan
    for ev, g in panel.groupby("event"):
        s = np.zeros(len(g))
        for f in FEATS:
            others = [v for k, v in ic[f].items() if k != ev and np.isfinite(v)]
            sign = np.sign(np.mean(others)) if others else 0.0
            s += sign * (g[f].rank(pct=True).values - 0.5)
        out.loc[g.index, "score"] = s / len(FEATS)
    return out


A = loeo_score(A)

# ─── 1. 종목 수 ───────────────────────────────────────────────────────────────
hr("1. 종목 수 — 꼬리를 잡으려면 몇 종목인가 (무작위 추출, 사건별 부트스트랩 B=%d)" % B)
port_x, port_r, hits = {}, {}, {}
for ev, g in A.groupby("event"):
    port_x[ev] = draw_portfolios(g.x.values, NS)
    port_r[ev] = draw_portfolios(g.r.values, NS)
    hits[ev] = tail_hit(g.x.values, NS)

mix_x, mix_r = {}, {}
print("\n(a) 사건 1회 배치 — 초과수익 x (사건 동일가중 혼합)  단위 %")
print("%4s%9s%9s%9s%9s%9s%9s%9s%11s%9s" % ("N", "평균", "5%", "25%", "중앙", "75%", "95%", "P(x<0)", "P(꼬리>=1)", "표준편차"))
for n in NS:
    v = np.concatenate([port_x[ev][n] for ev in EVENTS])
    mix_x[n] = v
    q = np.percentile(v, [5, 25, 50, 75, 95])
    ph = float(np.mean([hits[ev][n] for ev in EVENTS]))
    print("%4d%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%9.0f%11.0f%9.1f" % (
        n, v.mean() * 100, q[0] * 100, q[1] * 100, q[2] * 100, q[3] * 100, q[4] * 100,
        (v < 0).mean() * 100, ph * 100, v.std() * 100))

print("\n(b) 같은 것 — 절대수익 r (지수 미차감, 계좌가 실제로 겪는 값)  단위 %")
print("%4s%9s%9s%9s%9s%9s%9s%12s" % ("N", "평균", "1%", "5%", "중앙", "95%", "P(r<0)", "P(r<-20%)"))
for n in NS:
    v = np.concatenate([port_r[ev][n] for ev in EVENTS])
    mix_r[n] = v
    q = np.percentile(v, [1, 5, 50, 95])
    print("%4d%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%9.0f%12.1f" % (
        n, v.mean() * 100, q[0] * 100, q[1] * 100, q[2] * 100, q[3] * 100,
        (v < 0).mean() * 100, (v < -0.20).mean() * 100))

print("\n(c) 사건별 P(포트 초과수익 > 0) — N을 키우면 사건 고유 부호로 수렴  단위 %")
print("%-12s" % "event" + "".join("%9s" % ("N=" + str(n)) for n in NS) + "%10s" % "풀평균x")
for ev in EVENTS:
    print("%-12s" % ev + "".join("%9.0f" % ((port_x[ev][n] > 0).mean() * 100) for n in NS)
          + "%+10.1f" % (A.loc[A.event == ev, "x"].mean() * 100))

print("\n(d) 10사건 전부 겪었을 때의 사건평균(전략 수명 전체)  단위 %")
print("%4s%9s%9s%9s%9s%8s   %s" % ("N", "평균", "5%", "중앙", "95%", "P(<0)", "episode-클러스터 95%CI"))
for n in NS:
    life = np.mean([port_x[ev][n] for ev in EVENTS], axis=0)
    q = np.percentile(life, [5, 50, 95])
    per_ev = {ev: float(port_x[ev][n].mean()) for ev in EVENTS}
    o, lo, hi = cluster_boot_ci(per_ev, EP_OF)
    print("%4d%+9.1f%+9.1f%+9.1f%+9.1f%8.0f   [%+.1f%%, %+.1f%%]" % (
        n, life.mean() * 100, q[0] * 100, q[1] * 100, q[2] * 100,
        (life < 0).mean() * 100, lo * 100, hi * 100))

print("\n(e) 한계효용 — N을 늘릴 때 표준편차와 5분위")
prev = None
for n in NS:
    sd = float(mix_x[n].std())
    p5 = float(np.percentile(mix_x[n], 5))
    tail = "" if prev is None else "  (표준편차 %+.0f%%, 5분위 %+.1f%%p)" % ((sd / prev[0] - 1) * 100, (p5 - prev[1]) * 100)
    print("  N=%3d  표준편차 %.1f%%  5분위 %+.1f%%%s" % (n, sd * 100, p5 * 100, tail))
    prev = (sd, p5)


# ─── 1-f. 꼬리 의존도 — 꼬리를 못 잡으면 무엇이 남나 ──────────────────────────
def section1f():
    hr("1-f. 꼬리 의존도 — 상위5% 종목을 잡았을 때와 못 잡았을 때 (초과수익 x)")
    print("%4s%14s%14s%10s%12s" % ("N", "꼬리 잡음", "꼬리 놓침", "차이", "P(꼬리)"))
    for n in NS:
        hit_m, miss_m, ph = [], [], []
        for ev, g in A.groupby("event"):
            v = g.x.values
            thr = np.percentile(v, 95)
            m = len(v)
            perm = np.argsort(rng.random((4000, m)), axis=1)[:, :min(n, m)]
            sel = v[perm]
            pm = sel.mean(axis=1)
            h = (sel >= thr).any(axis=1)
            hit_m.append(pm[h])
            miss_m.append(pm[~h])
            ph.append(h.mean())
        H = np.concatenate(hit_m)
        M = np.concatenate([x for x in miss_m if len(x)])
        print("%4d%+13.1f%%%+13.1f%%%+9.1f%%%11.0f%%" % (
            n, H.mean() * 100, M.mean() * 100 if len(M) else float("nan"),
            (H.mean() - M.mean()) * 100 if len(M) else float("nan"), np.mean(ph) * 100))
    print("\n상위5% 꼬리를 1개 이상 잡을 확률이 목표치가 되려면 필요한 N (풀 450종 기준 근사)")
    for target in [0.5, 0.8, 0.9, 0.95, 0.99]:
        n = int(np.ceil(np.log(1 - target) / np.log(0.95)))
        print("  P>=%.0f%% -> N>=%d" % (target * 100, n))


section1f()


# ─── 2. 점수 상위20% vs 무작위 ────────────────────────────────────────────────
hr("2. 점수 상위20%로 좁힐 때 vs 무작위 — 같은 부트스트랩")
print("점수 = leave-one-event-out 부호로 만든 12피처 순위평균(README 합성점수와 같은 구성).")
print("\n(a) in-sample 10사건 — 초과수익 x  단위 %")
print("%4s%11s%12s%9s%10s%10s%10s%10s%9s%9s" % (
    "N", "무작위평균", "상위20%평균", "차이", "무작위5%", "상위5%", "무P(꼬리)", "상P(꼬리)", "무P(<0)", "상P(<0)"))
top_port = {}
for ev, g in A.groupby("event"):
    top = g[g.score >= g.score.quantile(0.8)]
    top_port[ev] = (draw_portfolios(top.x.values, NS),
                    hit_sub(top.x.values, g.x.quantile(0.95), NS, len(g)),
                    draw_portfolios(top.r.values, NS))
for n in NS:
    R = np.concatenate([port_x[ev][n] for ev in EVENTS])
    T = np.concatenate([top_port[ev][0][n] for ev in EVENTS])
    hr_ = float(np.mean([hits[ev][n] for ev in EVENTS]))
    ht_ = float(np.mean([top_port[ev][1][n] for ev in EVENTS]))
    print("%4d%+11.1f%+12.1f%+9.1f%+10.1f%+10.1f%10.0f%10.0f%9.0f%9.0f" % (
        n, R.mean() * 100, T.mean() * 100, (T.mean() - R.mean()) * 100,
        np.percentile(R, 5) * 100, np.percentile(T, 5) * 100,
        hr_ * 100, ht_ * 100, (R < 0).mean() * 100, (T < 0).mean() * 100))

print("\n(b) 사건별 상위20% − 무작위 스프레드 (N=30)")
diffs = {}
for ev in EVENTS:
    d = float(top_port[ev][0][30].mean() - port_x[ev][30].mean())
    diffs[ev] = d
    print("  %s (%9s) %+.1f%%" % (ev, EP_OF[ev], d * 100))
o, lo, hi = cluster_boot_ci(diffs, EP_OF)
print("  사건평균 %+.1f%%  양수 %d/%d  episode-클러스터 95%%CI[%+.1f%%, %+.1f%%]" % (
    o * 100, sum(v > 0 for v in diffs.values()), len(diffs), lo * 100, hi * 100))

print("\n(c) 홀드아웃 2026-08 — apply_2026.score(과거 부호로 계산)  단위 %")
gh = AP
toph = gh[gh.score >= gh.score.quantile(0.8)]
pr, pt = draw_portfolios(gh.x.values, NS), draw_portfolios(toph.x.values, NS)
prr, ptr = draw_portfolios(gh.r.values, NS), draw_portfolios(toph.r.values, NS)
hh = tail_hit(gh.x.values, NS)
ht = hit_sub(toph.x.values, gh.x.quantile(0.95), NS, len(gh))
print("%4s%10s%11s%9s%10s%11s%10s%10s" % ("N", "무작위x", "상위20%x", "차이", "무작위r", "상위20%r", "무P(꼬리)", "상P(꼬리)"))
for n in NS:
    print("%4d%+10.1f%+11.1f%+9.1f%+10.1f%+11.1f%10.0f%10.0f" % (
        n, pr[n].mean() * 100, pt[n].mean() * 100, (pt[n].mean() - pr[n].mean()) * 100,
        prr[n].mean() * 100, ptr[n].mean() * 100, hh[n] * 100, ht[n] * 100))
print("  홀드아웃 지수 평균 %+.1f%% — 절대수익이 양수여도 지수에 못 미친다" % (gh.bench.mean() * 100))

# ─── 3. 종목당 비중과 최대손실 ────────────────────────────────────────────────
hr("3. 종목당 비중과 최대손실 — 손절 없이 한 종목이 낼 수 있는 최악")
v = A.r.values
vx = A.x.values
print("절대수익 r  최소 %+.1f%%  0.5%% %+.1f%%  1%% %+.1f%%  5%% %+.1f%%" % (
    v.min() * 100, np.percentile(v, 0.5) * 100, np.percentile(v, 1) * 100, np.percentile(v, 5) * 100))
print("초과수익 x  최소 %+.1f%%  1%% %+.1f%%  5%% %+.1f%%" % (
    vx.min() * 100, np.percentile(vx, 1) * 100, np.percentile(vx, 5) * 100))
print("홀드아웃 r  최소 %+.1f%%  1%% %+.1f%%" % (AP.r.min() * 100, np.percentile(AP.r, 1) * 100))
print("사건별 최악 r: " + "  ".join("%s %+.0f%%" % (ev, A.loc[A.event == ev, "r"].min() * 100) for ev in EVENTS))
print("주의: 60거래일 보유 종점 수익이다. 장중 최대 MAE는 이 패널에 없어 계산 불가 —")
print("      실제 최악은 이보다 나쁘다. MAE를 재려면 로그에 보유기간 일별 저가가 있어야 한다.")

L1 = abs(float(np.percentile(v, 1)))
LMIN = abs(float(v.min()))
print("\n(a) 단일종목 한 방으로 계좌 X%를 넘지 않게 하는 종목당 비중 w = X / |손실|")
print("%5s%24s%24s" % ("X", "w (1%%분위 -%.0f%%)" % (L1 * 100), "w (최악 -%.0f%%)" % (LMIN * 100)))
for X in [0.05, 0.10, 0.20]:
    print("%5.0f%%%23.1f%%%23.1f%%" % (X * 100, X / L1 * 100, X / LMIN * 100))

print("\n(b) 포트 전체 기준 — N종목 동일가중에서 계좌손실을 X%로 묶는 총노출 E")
print("    계좌손실 = E x 포트수익. 포트 절대수익 1%분위를 손실 앵커로 쓴다(상한 100%).")
print("%4s%10s%10s%10s%10s%10s" % ("N", "포트r 1%", "포트r 5%", "E@X=5%", "E@X=10%", "E@X=20%"))
for n in NS:
    p1 = abs(float(np.percentile(mix_r[n], 1)))
    p5 = abs(float(np.percentile(mix_r[n], 5)))
    row = "".join("%9.0f%%" % (min(X / p1, 1.0) * 100) for X in [0.05, 0.10, 0.20])
    print("%4d%+10.1f%+10.1f%s" % (n, -p1 * 100, -p5 * 100, row))

print("\n(c) 두 제약을 동시에 만족하는 조합 (종목당 w = E/N)")
for X in [0.05, 0.10, 0.20]:
    print("  X=%.0f%%  (단일종목 제약 w <= %.1f%%)" % (X * 100, X / L1 * 100))
    for n in NS:
        p1 = abs(float(np.percentile(mix_r[n], 1)))
        E = min(X / p1, 1.0)
        w_port = E / n
        binding = "포트" if w_port <= X / L1 else "단일종목"
        print("    N=%3d  총노출 E=%5.0f%%  종목당 w=%5.1f%%  묶는 제약: %s" % (n, E * 100, w_port * 100, binding))


# ─── 4. 총 노출 ───────────────────────────────────────────────────────────────
hr("4. 총 노출 — 사건 때 계좌의 몇 %를 넣나")
print("(a) 노출 E별 사건 1회 계좌 손익 (N=30, 절대수익)  단위 %")
print("%6s%9s%9s%9s%9s%9s%14s" % ("E", "평균", "1%", "5%", "중앙", "95%", "P(계좌<-10%)"))
v30 = mix_r[30]
for E in [0.20, 0.40, 0.60, 0.80, 1.00]:
    a = v30 * E
    q = np.percentile(a, [1, 5, 50, 95])
    print("%6.0f%%%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%14.1f" % (
        E * 100, a.mean() * 100, q[0] * 100, q[1] * 100, q[2] * 100, q[3] * 100, (a < -0.10).mean() * 100))

span = (pd.Timestamp(EVENTS[-1]) - pd.Timestamp(EVENTS[0])).days / 365.25
print("\n(b) 사건 빈도 — 평소 현금인 전략의 자본효율")
print("  %s~%s = %.1f년에 %d건 -> 연 %.2f건" % (EVENTS[0], EVENTS[-1], span, len(EVENTS), len(EVENTS) / span))
print("  보유 60거래일(약 0.24년) -> 시장 노출 시간비중 약 %.0f%% (E=100%% 가정)" % (len(EVENTS) / span * 60 / 250 * 100))

hr("4-b. 익일시가 일괄 진입 vs 5거래일 분할 진입")
staged = RAW / "staged_entry.csv"
if staged.exists():
    S = pd.read_csv(staged, dtype={"code": str})
    S["event"] = S["event"].astype(str)
    print("패널 %d건 / 사건 %d건 (raw/staged_entry.csv) — 청산일은 동일 고정" % (len(S), S.event.nunique()))
    print("\n%-12s%6s%9s%9s%9s%9s%9s%9s" % ("event", "n", "일괄x", "분할x", "차이", "일괄r", "분할r", "차이"))
    per = {}
    for ev, g in S.groupby("event"):
        d = float(g.x5.mean() - g.x1.mean())
        per[ev] = d
        print("%-12s%6d%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f%+9.1f" % (
            ev, len(g), g.x1.mean() * 100, g.x5.mean() * 100, d * 100,
            g.r1.mean() * 100, g.r5.mean() * 100, (g.r5.mean() - g.r1.mean()) * 100))
    o, lo, hi = cluster_boot_ci(per, {k: EPISODE.get(k, k) for k in per})
    print("\n사건평균 차이 %+.2f%%  양수 %d/%d  episode-클러스터 95%%CI[%+.2f%%, %+.2f%%]" % (
        o * 100, sum(x > 0 for x in per.values()), len(per), lo * 100, hi * 100))
    p1 = np.concatenate([draw_portfolios(g.x1.values, [30])[30] for _, g in S.groupby("event")])
    p5 = np.concatenate([draw_portfolios(g.x5.values, [30])[30] for _, g in S.groupby("event")])
    print("\nN=30 포트 분포 비교 (사건 동일가중 혼합)")
    for lab, p in [("일괄", p1), ("5분할", p5)]:
        q = np.percentile(p, [5, 50, 95])
        print("  %-6s 평균 %+.1f%% 표준편차 %.1f%% 5%% %+.1f%% 중앙 %+.1f%% 95%% %+.1f%% P(<0) %.0f%%" % (
            lab, p.mean() * 100, p.std() * 100, q[0] * 100, q[1] * 100, q[2] * 100, (p < 0).mean() * 100))
else:
    print("staged_entry.csv 없음 — 계산 불가.")
    print("crosssec.csv 는 신호 익일 시가 단일 진입가만 담고 있어 이 파일만으로는 분할진입을 못 만든다.")
    print("`--staged` 로 universe_long.parquet 에서 d+1..d+5 시가를 다시 뽑아야 한다.")

# ─── 5. 실패 사건의 비용 ──────────────────────────────────────────────────────
hr("5. 실패 사건의 비용 — 2022-05-20 (KOSPI, 반등 실패)")
FAIL = "2022-05-20"
gf = A[A.event == FAIL]
print("패널 %d종  평균x %+.1f%%  중앙x %+.1f%%  평균r %+.1f%%  중앙r %+.1f%%  지수 %+.1f%%" % (
    len(gf), gf.x.mean() * 100, gf.x.median() * 100, gf.r.mean() * 100, gf.r.median() * 100, gf.bench.iloc[0] * 100))
print("승률(r>0) %.0f%%  최악 %+.1f%%  최고 %+.1f%%" % (
    (gf.r > 0).mean() * 100, gf.r.min() * 100, gf.r.max() * 100))

pf_r = draw_portfolios(gf.r.values, NS)
print("\n(a) 규칙별 계좌 손익 (X=10% 규칙: E = 10%% / |포트r 1%분위|)  단위 %")
print("%4s%7s%11s%10s%10s%11s" % ("N", "E", "계좌 평균", "계좌 5%", "계좌 1%", "P(계좌<0)"))
for n in NS:
    p1 = abs(float(np.percentile(mix_r[n], 1)))
    E = min(0.10 / p1, 1.0)
    a = pf_r[n] * E
    print("%4d%6.0f%%%+11.2f%+10.2f%+10.2f%11.0f" % (
        n, E * 100, a.mean() * 100, np.percentile(a, 5) * 100, np.percentile(a, 1) * 100, (a < 0).mean() * 100))

n = 30
p1 = abs(float(np.percentile(mix_r[n], 1)))
E = min(0.10 / p1, 1.0)
loss = float(pf_r[n].mean() * E)
gains = {ev: float(port_r[ev][n].mean() * E) for ev in EVENTS}
pos = [x for k, x in gains.items() if x > 0]
rest = [x for k, x in gains.items() if k != FAIL]
print("\n(b) 회복 — N=30, E=%.0f%%" % (E * 100))
print("  2022-05-20 계좌손익 %+.2f%%" % (loss * 100))
print("  양수 사건 %d/%d, 그 평균 %+.2f%% -> 회복에 %.1f회" % (
    len(pos), len(EVENTS), np.mean(pos) * 100, abs(loss) / np.mean(pos)))
if np.mean(rest) > 0:
    print("  실패건 제외 9사건 평균 %+.2f%% -> 회복에 %.1f회 (= %.1f년)" % (
        np.mean(rest) * 100, abs(loss) / np.mean(rest), abs(loss) / np.mean(rest) / (len(EVENTS) / span)))
else:
    print("  실패건 제외 9사건 평균 %+.2f%% (음수) -> 회복 불가" % (np.mean(rest) * 100))
print("\n  사건별 계좌손익 (E 고정, N=30)")
for ev in EVENTS:
    print("    %s (%9s) %+.2f%%" % (ev, EP_OF[ev], gains[ev] * 100))
cum = float(np.prod([1 + gains[ev] for ev in EVENTS]) - 1)
bl = A.groupby("event").bench.first().values
print("  10사건 연속 복리 %+.1f%% (사건 사이 현금)" % (cum * 100))
print("  같은 사건창을 지수로만 보유했을 때 복리 %+.1f%% (E=100%%), 사건평균 %+.1f%%" % (
    (np.prod([1 + b for b in bl]) - 1) * 100, bl.mean() * 100))
print("  같은 E=%.0f%%로 지수만 담았다면 복리 %+.1f%%" % (E * 100, (np.prod([1 + b * E for b in bl]) - 1) * 100))

# ─── 6. 켈리 ──────────────────────────────────────────────────────────────────
hr("6. 켈리 — 계산은 하되 그대로 쓰지 않는다")
f1, g1 = kelly(A.r.values)
print("(a) 1종목 몰빵 켈리 (절대수익 r 전체 분포)")
print("  f* = %.1f%%  E[log]=%+.4f   최악 %+.0f%% 이라 파산회피가 f를 묶는다" % (
    f1 * 100, g1, A.r.min() * 100))

print("\n(b) 사건 1회를 한 번의 베팅으로 본 켈리 — N종목 동일가중 포트 절대수익")
print("%4s%9s%10s%8s%11s%8s%8s%8s%11s" % ("N", "평균", "표준편차", "왜도", "전켈리 f*", "1/2", "1/4", "1/8", "E[log]@f*"))  # f*>100%%는 레버리지

kf = {}
for n in NS:
    vv = mix_r[n]
    f, gg = kelly(vv)
    kf[n] = f
    print("%4d%+9.2f%10.1f%8.2f%11.0f%8.0f%8.0f%8.0f%+11.4f" % (
        n, vv.mean() * 100, vv.std() * 100, pd.Series(vv).skew(),
        f * 100, f / 2 * 100, f / 4 * 100, f / 8 * 100, gg))

print("\n(c) 켈리 추정의 불확실성 — episode 클러스터 부트스트랩 (N=30, 400회)")
eps = sorted(set(EP_OF.values()))
by_ep = {e: [ev for ev in EVENTS if EP_OF[ev] == e] for e in eps}
fs = []
for b in range(400):
    pick = rng.choice(len(eps), size=len(eps), replace=True)
    evs = [ev for i in pick for ev in by_ep[eps[i]]]
    vv = np.concatenate([port_r[ev][30][:2000] for ev in evs])
    f, _ = kelly(vv, steps=601)
    fs.append(f)
fs = np.array(fs)
print("  f* 점추정 %.0f%%   95%%CI [%.0f%%, %.0f%%]   중앙 %.0f%%   f*<=5%% 비율 %.0f%%" % (
    kf[30] * 100, np.percentile(fs, 2.5) * 100, np.percentile(fs, 97.5) * 100,
    np.median(fs) * 100, (fs <= 0.05).mean() * 100))

fx, _ = kelly(mix_x[30])
print("\n(d) 초과수익 기준 켈리 (지수를 빼고 엣지만 놓고 볼 때, N=30): f* = %.0f%%" % (fx * 100))

print("\n(e) 노출 E별 사건당 로그성장 — 켈리 곡선이 얼마나 평평한가 (N=30, 절대수익)")
vv = mix_r[30]
for E in [0.10, 0.20, 0.25, 0.40, 0.50, 0.60, 0.80, 1.00]:
    y = 1 + E * vv
    gg = float(np.mean(np.log(y))) if np.all(y > 0) else float("nan")
    print("  E=%5.0f%%  E[log] %+.4f  기하평균 %+.2f%%/사건  1%%분위 %+.1f%%" % (
        E * 100, gg, (np.exp(gg) - 1) * 100, np.percentile(E * vv, 1) * 100))
print("\n주: 사건 6개(독립단위)로 켈리를 추정하는 것은 추정오차가 f*보다 크다.")


# ─── 7. 벤치마크 대비 강제 비교 + 켈리 스트레스 ───────────────────────────────
def section7():
    hr("7. 매수 후 보유(BH) 대비 — 같은 기간·같은 비용가정·같은 유니버스")
    n = 30
    p1 = abs(float(np.percentile(mix_r[n], 1)))
    E = min(0.10 / p1, 1.0)
    bl = A.groupby("event").bench.first()
    print("규칙: N=30 동일가중, 총노출 E=%.0f%%, 사건 사이 현금. BH = 같은 사건창을 지수로, 같은 E." % (E * 100))
    print("비용 미반영(왕복 0.31% 적용 시 전략은 사건당 -0.31%p, BH도 사건당 1회라 차이는 거의 안 바뀐다).")
    print("\n%-12s%12s%12s%12s%12s" % ("event", "전략(계좌)", "BH(계좌)", "차이", "전략-지수(x)"))
    st, bh = [], []
    for ev in EVENTS:
        s = float(port_r[ev][n].mean() * E)
        b = float(bl[ev] * E)
        st.append(s)
        bh.append(b)
        print("%-12s%+11.2f%%%+11.2f%%%+11.2f%%%+11.2f%%" % (
            ev, s * 100, b * 100, (s - b) * 100, float(port_x[ev][n].mean()) * 100))
    d = {ev: st[i] - bh[i] for i, ev in enumerate(EVENTS)}
    o, lo, hi = cluster_boot_ci(d, EP_OF)
    print("%-12s%+11.2f%%%+11.2f%%%+11.2f%%" % ("사건평균", np.mean(st) * 100, np.mean(bh) * 100, o * 100))
    print("%-12s%11.1f%%%11.1f%%%+11.1f%%" % (
        "10사건 복리", (np.prod([1 + v for v in st]) - 1) * 100,
        (np.prod([1 + v for v in bh]) - 1) * 100,
        ((np.prod([1 + v for v in st]) - 1) - (np.prod([1 + v for v in bh]) - 1)) * 100))
    print("차이 양수 %d/%d  episode-클러스터 95%%CI[%+.2f%%, %+.2f%%]" % (
        sum(v > 0 for v in d.values()), len(d), lo * 100, hi * 100))
    print("판정: CI가 0을 포함한다 -> BH 대비 결정적 우위 없음. '우세'라고 쓰지 않는다.")

    hr("6-f. 켈리 스트레스 — 왼쪽 꼬리를 실측 밖으로 늘리면 f*가 어떻게 되나")
    v = mix_r[30]
    print("실측 그대로: 포트 최소 %+.1f%%, 1%%분위 %+.1f%% -> 전켈리가 상한(300%%)에 붙는다." % (
        v.min() * 100, np.percentile(v, 1) * 100))
    print("표본에 2000·2008 같은 '반등 실패 후 2차 하락'이 없다(패널이 2015년부터).")
    print("\n가상의 실패 사건을 1건 추가했을 때 (사건 11건 중 1건 확률)")
    print("%14s%12s%12s%14s" % ("추가 사건 수익", "혼합 평균", "전켈리 f*", "1/4 켈리"))
    for shock in [-0.20, -0.30, -0.40, -0.50]:
        add = np.full(len(v) // 10, shock)
        vv = np.concatenate([v, add])
        f, _ = kelly(vv)
        print("%13.0f%%%+11.2f%%%11.0f%%%13.0f%%" % (shock * 100, vv.mean() * 100, f * 100, f / 4 * 100))
    print("\n같은 것 — 초과수익 기준(엣지만)")
    vx = mix_x[30]
    fx, _ = kelly(vx)
    print("실측 f* %.0f%% (평균 %+.2f%%, episode CI [-2.0%%, +1.9%%] -> 부호 미확정)" % (fx * 100, vx.mean() * 100))
    for shock in [-0.10, -0.20]:
        vv = np.concatenate([vx, np.full(len(vx) // 10, shock)])
        f, _ = kelly(vv)
        print("  실패 1건(%+.0f%%) 추가 시 f* %.0f%%" % (shock * 100, f * 100))


section7()


# ─── 분할 진입 패널 재계산 (--staged) ─────────────────────────────────────────
def build_staged():
    """d+1..d+5 시가 균등 분할 진입 패널을 universe_long.parquet 에서 다시 만든다.

    청산일은 일괄 진입과 동일(신호일 i 기준 i+1+60)이라 진입가 효과만 분리된다.
    벤치도 같은 방식으로 분할해 초과수익을 맞춘다.
    """
    sys.path.insert(0, str(HERE))
    from signals import add_features, add_market, t1_ma20_reclaim
    HZ, WINDOW, K = 60, 15, 5
    meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
    big_all = pd.read_parquet(RAW / "universe_long.parquet")
    rows = []
    for sym, label, thr in [("KS11", "KOSPI", -0.20), ("KQ11", "KOSDAQ", -0.25)]:
        d = pd.read_csv(RAW / ("%s_long.csv" % sym), parse_dates=["Date"]).rename(columns={"Date": "date"})
        idx = d.sort_values("date").reset_index(drop=True)
        c = idx["Close"]
        ma20 = c.rolling(20).mean()
        dd = c / c.rolling(250, min_periods=120).max() - 1
        sig = (c > ma20) & (c.shift(1) <= ma20.shift(1)) & (dd <= thr)
        keep, last = [], -999
        for i in np.flatnonzero(sig.values):
            if i - last >= 60:
                keep.append(i)
                last = i
        codes = set(meta.loc[meta["market"] == label, "code"])
        big = big_all[big_all["code"].isin(codes)]
        idx_s = idx.rename(columns={"Close": "mclose"})[["date", "mclose"]]
        feat = {}
        for code, dfc in big.groupby("code", sort=False):
            if len(dfc) < 300:
                continue
            feat[code] = add_market(add_features(dfc), idx_s).reset_index(drop=True)
        print("  %s: 사건 %d건, 종목 %d" % (label, len(keep), len(feat)))
        for i0 in keep:
            if i0 + 1 + HZ >= len(idx):
                continue
            ev = idx["date"].iloc[i0]
            n_before = len(rows)
            for code, dfc in feat.items():
                w = (dfc["date"] >= ev) & (dfc["date"] <= idx["date"].iloc[min(i0 + WINDOW, len(idx) - 1)])
                if not w.any():
                    continue
                if dfc.loc[w, "tv20"].median() < 5e8 or dfc.loc[w, "close"].median() < 1000:
                    continue
                s = t1_ma20_reclaim(dfc).fillna(False) & w
                pos = np.flatnonzero(s.values)
                if len(pos) == 0:
                    continue
                i = pos[0]
                if i + K + HZ >= len(dfc):
                    continue
                ent = dfc["open"].iloc[i + 1:i + 1 + K].values
                if not np.all(np.isfinite(ent)) or np.any(ent <= 0):
                    continue
                exit_px = dfc["close"].iloc[i + 1 + HZ]
                # 지수는 종목 신호일과 같은 날짜로 정렬해서 뽑는다
                jd = idx.index[idx["date"] == dfc["date"].iloc[i]]
                if len(jd) == 0 or jd[0] + K + HZ >= len(idx):
                    continue
                j = jd[0]
                bent = idx["Open"].iloc[j + 1:j + 1 + K].values
                bexit = idx["Close"].iloc[j + 1 + HZ]
                r1 = exit_px / ent[0] - 1
                r5 = float(np.mean(exit_px / ent - 1))
                b1 = bexit / bent[0] - 1
                b5 = float(np.mean(bexit / bent - 1))
                rows.append(dict(event=str(ev.date()), market=label, code=code,
                                 r1=r1, r5=r5, b1=b1, b5=b5, x1=r1 - b1, x5=r5 - b5))
            if len(rows) - n_before < 20:
                del rows[n_before:]
    S = pd.DataFrame(rows)
    S.to_csv(RAW / "staged_entry.csv", index=False, encoding="utf-8-sig")
    print("  저장: raw/staged_entry.csv  %d행" % len(S))
    return S


if "--staged" in sys.argv:
    hr("분할 진입 패널 재계산 (universe_long.parquet)")
    build_staged()
    print("\n재계산 끝. 인자 없이 다시 실행하면 4-b 절에 반영된다.")
