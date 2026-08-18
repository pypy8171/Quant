#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
15전략(M1~M5 + C1~C5·O1~O5) × 17이벤트 × 10억원 매매일지 생성기.

정직성:
 - 이 15전략은 전부 '지수 익스포저 오버레이'다. 개별종목을 고르지 않는다(survivorship 금지).
   실제 매매하는 '종목' = 각 이벤트 **대표지수 ETF**(미국=SPY/QQQ 근사, 한국=KODEX 200 근사).
   10억원 명목을 그 ETF에 넣고, 전략은 보유비중 e(0=현금~1.2=완만레버)를 날마다 조절한다.
 - 익스포저 e[t]는 오직 t-1 종가까지 정보(룩어헤드 차단). 오늘 손익 = e[t-1]×지수수익.
   US신호는 KR지수 거래시 직전세션값으로 정렬(시차). peak/trough는 창 '중심'을 잡는 데만 사용(평가전용).
 - KOSPI200(^KS11)은 1996-12부터 → 그 이전 위기(1929·1987·1990)는 대표지수(미국)로 대체.
 - 결측신호 → 중립(e=1) → 해당 전략은 그 구간 BH와 동일(매매 0). 이걸 그대로 노출한다.

산출: journal_by_event.md(읽기용: 이벤트별 일별 베이스라인 + 15전략 매매내역 + 요약)
      journal_tsv/<event>.tsv(전체: 날짜×[close,BH,15전략] 일별 평가액)
"""
import sys
from pathlib import Path
from datetime import date as _date

import numpy as np

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]
_PYQ = _REPO / "PYQuant"
_BT08 = _REPO / "research" / "studies" / "08_crisis_response"
for p in (str(_PYQ), str(_BT08), str(_HERE.parent)):
    if p not in sys.path:
        sys.path.insert(0, p)

from data.index_source import IndexSource            # noqa: E402
from data.asof import as_of                          # noqa: E402  (재현성: 종료일 상한 고정)
import backtest_crisis_response as bt08              # noqa: E402
import backtest_crisis_strategies as bt09            # noqa: E402

load_series = bt08.load_series
daily_returns = bt08.daily_returns
slice_window = bt08.slice_window
window_maxdd_anchor = bt08.window_maxdd_anchor
max_drawdown = bt08.max_drawdown
fmt = bt08.fmt
EVENTS = bt08.EVENTS
BEHAVIOR = bt08.BEHAVIOR
COST = bt08.COST_BASE

CAP = 1_000_000_000          # 10억원
PRE_BD = 42                  # 저점 전 거래일(~2개월)
POST_BD = 42                 # 저점 후 거래일(~2개월)

TODAY = as_of().isoformat()

# 대표지수 → 매매상품(ETF 근사) 라벨
INSTRUMENT = {
    "^GSPC": "SPY(S&P500 근사)",
    "^IXIC": "QQQ(나스닥100 근사)",
    "^KS11": "KODEX 200(KOSPI200 근사)",
}
# 지수 로드 시작(워밍업 확보)
IDX_START = {"^KS11": "1996-01-01", "^IXIC": "1971-01-01"}
DEF_START = "1927-01-01"

# 15전략: (code, label, fn, kind, side)  kind: 'm'=vix인자, 'c'=ctx인자
bt08_labels = {c: lab for c, lab, *_ in bt08.METHODS}
METHODS15 = [
    ("M1", bt08_labels["M1"], bt08.expo_m1_trend,    "m", "방어"),
    ("M2", bt08_labels["M2"], bt08.expo_m2_volz,     "m", "방어"),
    ("M3", bt08_labels["M3"], bt08.expo_m3_vix,      "m", "방어"),
    ("M4", bt08_labels["M4"], bt08.expo_m4_circuit,  "m", "방어"),
    ("M5", bt08_labels["M5"], bt08.expo_m5_voltarget,"m", "방어"),
] + [(c, lab, fn, "c", side) for c, lab, fn, side, _desc, _needs in bt09.STRATS]


def compute_e(fn, kind, closes, ret, ctx):
    if kind == "m":
        vix = ctx["vix"]
        return fn(closes, ret, vix, {})
    return fn(closes, ret, ctx, {})


def sim_window(closes, ret, e, lo, hi):
    """[lo..hi] 창 10억 시뮬. vals[k]=총자산, 진입포지션=e[lo-1]."""
    m = hi - lo + 1
    vals = np.empty(m)
    vals[0] = CAP
    prev2 = e[lo - 1] if lo >= 1 else e[lo]
    for k in range(1, m):
        t = lo + k
        pos = e[t - 1]
        turn = abs(pos - prev2)
        vals[k] = vals[k - 1] * (1.0 + pos * ret[t] - turn * (COST / 2.0))
        prev2 = pos
    return vals


def trades_in_window(dates, closes, e, vals, lo, hi):
    """창 내 실제 매매(비중변경) 로그. 체결=종가 t, 익일 반영."""
    out = []
    for t in range(lo, hi + 1):
        de = e[t] - e[t - 1] if t >= 1 else 0.0
        if abs(de) > 1e-9:
            v = vals[t - lo]
            amt = de * v                     # +매수 / -매도(원)
            sh = amt / closes[t]
            out.append(dict(date=dates[t], prev=e[t - 1], new=e[t],
                            action=("매수" if de > 0 else "매도"),
                            amt=abs(amt), shares=abs(sh), close=closes[t]))
    return out


def won(x):
    return f"{x:,.0f}"


def run(force_rep=None, tag=""):
    """force_rep=None → 이벤트 대표지수로 매매. force_rep='^KS11' → 전부 KODEX 200 기준."""
    src = IndexSource()
    out_md = _HERE.parent / (f"journal_by_event{tag}.md")
    tsv_dir = _HERE.parent / (f"journal_tsv{tag}")
    tsv_dir.mkdir(exist_ok=True)
    L = []
    if force_rep:
        inst_forced = INSTRUMENT.get(force_rep, force_rep)
        L.append(f"# 15전략 × 17이벤트 × 10억원 매매일지 — 전부 {inst_forced} 기준 (BT-09 부속)\n")
        L.append("> ⚠️ **정직성.** 한국 투자자가 **모든 위기를 KODEX 200(KOSPI200 근사)** 하나로 겪었다면. "
                 "15전략(M1~M5 방어 + C1~C5·O1~O5)은 전부 **지수 익스포저 오버레이**(개별종목 안 고름). "
                 "10억원을 KODEX 200에 넣고 전략이 **보유비중 e**(0=현금~1.2=완만레버)를 날마다 조절한다. "
                 "US신호(VIX·금리·유가·SOX·달러)는 **직전 미국세션값**으로 정렬(한국장이 먼저 마감 → 시차 차단). "
                 "익스포저는 t-1 종가까지 정보로만, 손익=e[t-1]×KOSPI수익, 비용 왕복 0.21%. "
                 "창 = **KOSPI 저점일 ±약2개월=총 4개월**. 저점 앵커는 창 중심잡기(평가)에만 씀. "
                 "**^KS11은 1996-12부터라 1929·1987·1990 위기는 KOSPI 데이터 없음 → 제외.** "
                 "결측신호 전략은 그 구간 BH와 동일(매매0). "
                 f"yfinance 무키. 생성 {TODAY}.\n")
    else:
        L.append("# 15전략 × 17이벤트 × 10억원 매매일지 (BT-09 부속)\n")
        L.append("> ⚠️ **정직성.** 이 15전략(M1~M5 방어 + C1~C5·O1~O5)은 전부 **지수 익스포저 오버레이**다. "
                 "개별종목을 고르지 않는다. 매매하는 '종목' = 각 이벤트 **대표지수 ETF 근사**(미국 SPY/QQQ, 한국 KODEX 200). "
                 "10억원을 그 ETF에 넣고 전략이 **보유비중 e**(0=현금~1.2=완만레버)를 날마다 조절한다. "
                 "익스포저는 t-1 종가까지 정보로만 산출(룩어헤드 차단), 손익=e[t-1]×지수수익, 비용 왕복 0.21%. "
                 "창 = **저점일(climax) ±약2개월(거래일 42일씩)=총 4개월**. 저점 앵커는 창 중심잡기(평가)에만 씀. "
                 "KOSPI200은 1996-12부터라 그 이전 위기는 대표지수(미국)로 대체. 결측신호 전략은 그 구간 BH와 동일(매매0). "
                 f"yfinance 무키. 생성 {TODAY}.\n")
    L.append("**전략 범례**: M1 200일선추세 · M2 실현변동성z축소 · M3 VIX게이트 · M4 서킷브레이커 · M5 변동성타깃 "
             "／ C1 유가·금리공급충격 · C2 리스크오프삼중 · C3 VIX급등컷 · C4 금리충격 · C5 US오버나잇갭 "
             "／ O1 국면전환 · O2 저변동성돌파 · O3 SOX선행 · O4 유가수요견인 · O5 크로스에셋재진입\n")

    scoreboard = {code: {"ret": [], "beat": 0, "n": 0} for code, *_ in METHODS15}
    bh_board = []

    for eid, cause, rep0, d0, d1 in EVENTS:
        rep = force_rep or rep0
        start = IDX_START.get(rep, DEF_START)
        dates, closes = load_series(src, rep, start, TODAY)
        if len(closes) < 260:
            L.append(f"\n## {eid} — 데이터 부족({rep}), 건너뜀\n")
            continue
        ret = daily_returns(closes)
        is_kr = (rep == "^KS11")
        ctx = bt09.build_ctx(src, dates, is_kr)

        # 저점 앵커(매매지수 자신의 이벤트 광의창)
        lo_b, hi_b = slice_window(dates, d0, d1)
        if lo_b is None or hi_b - lo_b < 10:
            L.append(f"\n## {eid} · {cause} — {INSTRUMENT.get(rep,rep)} 데이터가 이 시기에 없음 → 제외\n")
            continue
        pk_i, tr_i, bh_dd = window_maxdd_anchor(closes[lo_b:hi_b])
        center = lo_b + tr_i
        w_lo = max(1, center - PRE_BD)
        w_hi = min(len(closes) - 1, center + POST_BD)
        wdates = dates[w_lo:w_hi + 1]
        wcloses = closes[w_lo:w_hi + 1]

        inst = INSTRUMENT.get(rep, rep)
        L.append(f"\n\n## {eid} · {cause} · {BEHAVIOR.get(eid,'?')}\n")
        L.append(f"- **상품(대표지수)**: {inst}  ·  **창**: {wdates[0]} ~ {wdates[-1]} "
                 f"({len(wdates)}거래일, 저점 {dates[center]} 기준 ±2개월)")
        # BH 결과
        bh_e = np.ones(len(closes))
        bh_vals = sim_window(closes, ret, bh_e, w_lo, w_hi)
        bh_ret = (bh_vals[-1] / CAP - 1.0) * 100.0
        bh_mdd = max_drawdown(bh_vals)
        L.append(f"- **그냥 보유(BH) 10억 → {won(bh_vals[-1])}원 ({fmt(bh_ret,1)}%)**, 창내 MDD {fmt(bh_mdd,1)}%")
        bh_board.append(bh_ret)

        # 일별 베이스라인 표
        L.append("\n### 일별 베이스라인 (지수 · BH 10억 평가액)\n")
        L.append("| 날짜 | 지수종가 | 일간% | BH 평가액(원) |")
        L.append("|---|---|---|---|")
        for k, dt in enumerate(wdates):
            r = (wcloses[k] / wcloses[k - 1] - 1.0) * 100.0 if k > 0 else 0.0
            L.append(f"| {dt} | {wcloses[k]:,.2f} | {fmt(r,2)} | {won(bh_vals[k])} |")

        # 전략별 매매내역 + 요약
        summ = []
        tsv_cols = {"date": list(wdates), "close": [f"{c:.2f}" for c in wcloses],
                    "BH": [f"{v:.0f}" for v in bh_vals]}
        L.append("\n### 전략별 매매내역 (비중변경 = 실제 매매; 결측신호는 매매0=BH동일)\n")
        for code, label, fn, kind, side in METHODS15:
            e = compute_e(fn, kind, closes, ret, ctx)
            vals = sim_window(closes, ret, e, w_lo, w_hi)
            tsv_cols[code] = [f"{v:.0f}" for v in vals]
            tl = trades_in_window(dates, closes, e, vals, w_lo, w_hi)
            wret = (vals[-1] / CAP - 1.0) * 100.0
            wmdd = max_drawdown(vals)
            enter = e[w_lo - 1] if w_lo >= 1 else e[w_lo]
            beat = wret > bh_ret
            scoreboard[code]["ret"].append(wret)
            scoreboard[code]["beat"] += int(beat)
            scoreboard[code]["n"] += 1
            summ.append((code, side, wret, wmdd, len(tl), enter, vals[-1]))
            # 매매 로그(희소)
            L.append(f"\n**{code} {label}** ({side}) — 진입비중 {fmt(enter*100,0)}% · "
                     f"매매 {len(tl)}회 · 10억→{won(vals[-1])}원({fmt(wret,1)}%) · 창내MDD {fmt(wmdd,1)}%"
                     + ("  ✅BH초과" if beat else ""))
            if tl:
                L.append("")
                L.append("| 날짜 | 액션 | 비중 | 체결가 | 금액(원) | 주수 |")
                L.append("|---|---|---|---|---|---|")
                for x in tl:
                    L.append(f"| {x['date']} | {x['action']} | {fmt(x['prev']*100,0)}→{fmt(x['new']*100,0)}% | "
                             f"{x['close']:,.2f} | {won(x['amt'])} | {x['shares']:,.1f} |")
            else:
                L.append("- (창 내 매매 없음)")

        # 이벤트 요약표
        L.append("\n### 이벤트 요약 (10억 → 최종평가액)\n")
        L.append("| 전략 | 구분 | 창내수익% | 창내MDD% | 매매횟수 | 진입비중% | 최종평가액(원) | vs BH |")
        L.append("|---|---|---|---|---|---|---|---|")
        L.append(f"| BH 그냥보유 | 기준 | {fmt(bh_ret,1)} | {fmt(bh_mdd,1)} | 0 | 100 | {won(bh_vals[-1])} | — |")
        for code, side, wret, wmdd, ntr, enter, fv in summ:
            L.append(f"| {code} | {side} | {fmt(wret,1)} | {fmt(wmdd,1)} | {ntr} | {fmt(enter*100,0)} | "
                     f"{won(fv)} | {fmt(wret-bh_ret,1)}%p |")

        # TSV
        keys = ["date", "close", "BH"] + [c for c, *_ in METHODS15]
        lines = ["\t".join(keys)]
        for i in range(len(wdates)):
            lines.append("\t".join(str(tsv_cols[k][i]) for k in keys))
        (tsv_dir / f"{eid}.tsv").write_text("\n".join(lines), encoding="utf-8")
        print(f"[ok] {eid}: {inst} {wdates[0]}~{wdates[-1]} BH {fmt(bh_ret,1)}%")

    # 종합 스코어보드
    L.append("\n\n## 전 이벤트 종합 스코어보드 (창내수익 평균·BH초과 빈도)\n")
    L.append(f"BH 평균 창내수익 {fmt(float(np.mean(bh_board)),1)}% (n={len(bh_board)}). "
             "아래는 각 전략의 17이벤트 평균·중앙값과 BH를 이긴 이벤트 수.\n")
    L.append("| 전략 | 구분 | 평균수익% | 중앙값% | BH초과 이벤트 | 표본 |")
    L.append("|---|---|---|---|---|---|")
    side_of = {c: s for c, _, _, _, s in METHODS15}
    for code, *_ in METHODS15:
        sb = scoreboard[code]
        if not sb["ret"]:
            continue
        arr = np.array(sb["ret"])
        L.append(f"| {code} | {side_of[code]} | {fmt(float(arr.mean()),1)} | "
                 f"{fmt(float(np.median(arr)),1)} | {sb['beat']}/{sb['n']} | {sb['n']} |")
    L.append("\n*'창내수익'은 저점±2개월 4개월 구간 수익이라 대부분 음수(위기창)거나 반등포함. "
             "방어전략은 낙폭을 줄이는 대신 수익이 BH보다 낮을 수 있고(정상), 공세전략은 반등참여로 초과를 노린다. "
             "BH초과 이벤트 수가 절반 이상이면 그 창에서 값어치. 단 n≈13~17, 성격별로는 더 적어 과대해석 금지.*\n")
    L.append(f"\n---\n생성: `research/studies/09_crisis_strategies/gen_trade_journal.py`"
             f"(단독실행, IndexSource·BT-08/09 재사용, 결정론적). 전체 일별×전략 평가액은 `journal_tsv/<event>.tsv`.\n")

    out_md.write_text("\n".join(L), encoding="utf-8")
    print(f"[done] {out_md}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "kr":
        run(force_rep="^KS11", tag="_kr")
    else:
        run()
