#!/usr/bin/env python3
"""
백테스트 trades CSV → 이벤트(구간)별 종목 매매 원장 트리 렌더러.

입력:
  raw/{run}_trades.csv       (열: date,ticker,name,side,price,quantity,pnl)
  summary_yf.tsv / summary_datagokr.tsv  (구간·전략·regime별 지표)
출력(events/ 트리):
  events/README.md                      전체 인덱스(6구간 요약표 + 이벤트 링크)
  events/{window}/README.md             이벤트 요약(4런 지표표 + 원장 링크 + 무슨 일)
  events/{window}/{strat}_{regime}.md   종목별 진입→청산 왕복 원장

run 키 = {window}_{strat}_{regime}  예) 2011euro_momentum_on

사용:
  python3 render_ledger.py            # 전부 재생성
  python3 render_ledger.py 2011euro   # 특정 이벤트만
"""
import csv, sys, os, glob
from collections import defaultdict, deque
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw")
OUT = os.path.join(HERE, "events")

# 구간 메타: 순번, 라벨, 무슨 일이었나, 데이터 소스
WINDOWS = {
    "2011euro":     (1, "2011 유럽재정위기", "그리스發 유로존 위기로 완만·장기 하락. 8월 미국 신용강등 급락 포함.", "yfinance"),
    "2018semi":     (2, "2018 반도체·미중무역", "미중 무역전쟁 + 반도체 고점 논쟁. 방향이 자주 뒤집힌 박스+급락장.", "yfinance"),
    "2020covid":    (3, "2020 COVID 팬데믹", "3월 코로나 급락 후 유동성 장세로 V자 반등. 연말 기준 플러스.", "yfinance"),
    "2022bear":     (4, "2022 금리인상 약세장", "연준 급격한 긴축으로 연중 내내 순수 하락 추세.", "datagokr(PIT)"),
    "2024blackmon": (5, "2024 블랙먼데이", "8월 엔캐리 청산發 급락 후 회복.", "datagokr(PIT)"),
    "2026now":      (6, "2026 현재 변동장", "AI·반도체 테마 랠리와 급락이 공존한 고변동 국면.", "datagokr(PIT)"),
}
YF_WINDOWS = {"2011euro", "2018semi", "2020covid"}
ORDER = sorted(WINDOWS, key=lambda w: WINDOWS[w][0])
STRAT_KO = {"momentum": "모멘텀(추세추종)", "mean_reversion": "역추세(이격도 복귀)"}
STRAT_SHORT = {"momentum": "모멘텀", "mean_reversion": "역추세"}


def _d(s):
    return datetime.strptime(s[:10], "%Y-%m-%d")


def load_csv(path, delim=","):
    with open(path, encoding="utf-8-sig") as f:
        return list(csv.DictReader(f, delimiter=delim))


def load_summaries():
    """(window,strat,regime) -> {ret,mdd,sharpe,win,bench_ret,bench_mdd,alpha,verdict,trades}"""
    m = {}
    for name in ("summary_yf.tsv", "summary_datagokr.tsv"):
        p = os.path.join(HERE, name)
        if not os.path.exists(p):
            continue
        for r in load_csv(p, delim="\t"):
            key = (r["window"], r["strategy"], r["regime"])
            m[key] = r
    return m


def round_trips(rows):
    lots = defaultdict(deque)
    names = {}
    trips, opens = [], []
    for r in rows:
        t = r["ticker"]; names[t] = r.get("name", "")
        side = r["side"]; price = float(r["price"]); qty = int(r["quantity"]); date = r["date"]
        if side == "BUY":
            lots[t].append([date, price, qty]); continue
        pnl = float(r["pnl"]) if r.get("pnl") not in (None, "") else 0.0
        remaining, cost, matched, entry_date = qty, 0.0, 0, None
        while remaining > 0 and lots[t]:
            lot = lots[t][0]
            take = min(remaining, lot[2])
            if entry_date is None:
                entry_date = lot[0]
            cost += lot[1] * take; matched += take
            lot[2] -= take; remaining -= take
            if lot[2] == 0:
                lots[t].popleft()
        avg_entry = cost / matched if matched else price
        pnl_pct = (pnl / cost * 100) if cost else 0.0
        hold = (_d(date) - _d(entry_date)).days if entry_date else 0
        trips.append({"ticker": t, "name": names[t], "entry_date": entry_date or date,
                      "entry_price": avg_entry, "exit_date": date, "exit_price": price,
                      "qty": matched or qty, "hold": hold, "pnl": pnl, "pnl_pct": pnl_pct})
    for t, dq in lots.items():
        for lot in dq:
            if lot[2] > 0:
                opens.append({"ticker": t, "name": names[t], "entry_date": lot[0],
                              "entry_price": lot[1], "qty": lot[2]})
    return trips, opens


def won(x):
    return f"{x:+,.0f}"


def render_ledger(window, strat, regime, summ):
    run = f"{window}_{strat}_{regime}"
    path = os.path.join(RAW, f"{run}_trades.csv")
    if not os.path.exists(path):
        return None
    trips, opens = round_trips(load_csv(path))
    win = [t for t in trips if t["pnl"] > 0]
    total = sum(t["pnl"] for t in trips)
    winrt = len(win) / len(trips) * 100 if trips else 0.0
    label = WINDOWS[window][1]
    s = summ.get((window, strat, regime), {})

    L = [f"# {label} — {STRAT_KO[strat]}, regime {regime.upper()}", ""]
    if s:
        L.append(f"- 총수익률 **{s['ret_pct']}%** · MDD {s['mdd_pct']}% · 샤프 {s['sharpe']} · "
                 f"승률 {s['win_pct']}% · 벤치 {s['bench_ret']}% · α {s['alpha_p']}%p")
        L.append(f"- 판정: {s['verdict']}")
    if window in YF_WINDOWS:
        L.append("- ⚠️ 가격은 **수정주가(yfinance, 액면분할 반영)** — 당시 실호가와 다를 수 있음. 손익률 비교엔 무방.")
    L.append(f"- 왕복 **{len(trips)}건** · 승 {len(win)}/패 {len(trips)-len(win)} · 승률 {winrt:.1f}% · 실현손익 합 **{won(total)}원**")
    L.append("")

    if trips:
        srt = sorted(trips, key=lambda t: t["pnl"], reverse=True)
        L += ["## 손익 상위/하위 5", "", "| | 종목 | 진입일→청산일 | 손익(원) | 손익% |", "|---|---|---|---:|---:|"]
        for t in srt[:5]:
            L.append(f"| 🟢 | {t['name']}({t['ticker']}) | {t['entry_date']}→{t['exit_date']} | {won(t['pnl'])} | {t['pnl_pct']:+.1f}% |")
        for t in srt[-5:][::-1]:
            L.append(f"| 🔴 | {t['name']}({t['ticker']}) | {t['entry_date']}→{t['exit_date']} | {won(t['pnl'])} | {t['pnl_pct']:+.1f}% |")
        L.append("")
        L += ["## 전체 왕복 원장 (진입일순)", "",
              "| 진입일 | 종목 | 진입가 | 청산일 | 청산가 | 수량 | 보유일 | 손익(원) | 손익% |",
              "|---|---|---:|---|---:|---:|---:|---:|---:|"]
        for t in sorted(trips, key=lambda x: (x["entry_date"], x["exit_date"])):
            L.append(f"| {t['entry_date']} | {t['name']}({t['ticker']}) | {t['entry_price']:,.0f} | "
                     f"{t['exit_date']} | {t['exit_price']:,.0f} | {t['qty']:,} | {t['hold']} | "
                     f"{won(t['pnl'])} | {t['pnl_pct']:+.1f}% |")
        L.append("")
    else:
        L += ["_체결 없음 — regime 필터가 전 구간 신규 진입을 차단(현금 보유)._", ""]

    if opens:
        L += ["## 종료시점 보유 중 (미실현)", "", "| 진입일 | 종목 | 진입가 | 수량 |", "|---|---|---:|---:|"]
        for t in sorted(opens, key=lambda x: x["entry_date"]):
            L.append(f"| {t['entry_date']} | {t['name']}({t['ticker']}) | {t['entry_price']:,.0f} | {t['qty']:,} |")
        L.append("")

    L += ["---", "← [이벤트 요약](README.md) · [전체 인덱스](../README.md) · [백테스트 종합](../../README.md)"]
    return "\n".join(L), {"strat": strat, "regime": regime, "n": len(trips), "winrt": winrt, "total": total}


def render_event(window, summ):
    order, label, story, src = WINDOWS[window]
    d = os.path.join(OUT, window)
    os.makedirs(d, exist_ok=True)
    metas = []
    for strat in ("momentum", "mean_reversion"):
        for regime in ("on", "off"):
            res = render_ledger(window, strat, regime, summ)
            if res is None:
                continue
            md, meta = res
            with open(os.path.join(d, f"{strat}_{regime}.md"), "w", encoding="utf-8") as f:
                f.write(md)
            metas.append(meta)

    # 이벤트 요약 README
    L = [f"# {label}", "",
         f"> {story}", f"> 데이터: {src}", ""]
    L += ["| 전략 | regime | 총수익 | MDD | 샤프 | 승률 | α(초과) | 판정 | 원장 |",
          "|---|---|---:|---:|---:|---:|---:|---|---|"]
    for m in metas:
        s = summ.get((window, m["strat"], m["regime"]), {})
        link = f"[원장]({m['strat']}_{m['regime']}.md)"
        L.append(f"| {STRAT_SHORT[m['strat']]} | {m['regime'].upper()} | {s.get('ret_pct','?')}% | "
                 f"{s.get('mdd_pct','?')}% | {s.get('sharpe','?')} | {s.get('win_pct','?')}% | "
                 f"{s.get('alpha_p','?')}%p | {s.get('verdict','')} | {link} |")
    L.append("")
    L.append("← [전체 인덱스](../README.md) · [백테스트 종합](../../README.md)")
    with open(os.path.join(d, "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    return metas


def render_index(all_metas):
    L = ["# 하락장 백테스트 — 이벤트별 매매 원장", "",
         "구간(이벤트)별로 폴더를 나눴다. 각 폴더의 `README.md`는 그 구간 4런(모멘텀/역추세 × regime ON/OFF)의",
         "지표 요약이고, `{전략}_{regime}.md`는 **어떤 종목을 언제 사고팔아 얼마 손익**이었는지 왕복 원장이다.",
         "가격은 KRW. 2011/2018/2020은 수정주가(yfinance).", ""]
    for w in ORDER:
        order, label, story, src = WINDOWS[w]
        L += [f"## {order}. [{label}]({w}/README.md)  ·  _{src}_", "", f"{story}", "",
              "| 전략·regime | 총수익 | 샤프 | 원장 |", "|---|---:|---:|---|"]
        for m in all_metas.get(w, []):
            s = m["_summ"]
            L.append(f"| {STRAT_SHORT[m['strat']]} {m['regime'].upper()} | {s.get('ret_pct','?')}% | "
                     f"{s.get('sharpe','?')} | [열기]({w}/{m['strat']}_{m['regime']}.md) |")
        L.append("")
    L.append("← [백테스트 종합](../README.md)")
    with open(os.path.join(OUT, "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def main():
    os.makedirs(OUT, exist_ok=True)
    summ = load_summaries()
    targets = sys.argv[1:] or ORDER
    all_metas = {}
    for w in targets:
        if w not in WINDOWS:
            print(f"skip(모르는 구간): {w}"); continue
        metas = render_event(w, summ)
        for m in metas:
            m["_summ"] = summ.get((w, m["strat"], m["regime"]), {})
        all_metas[w] = metas
        print(f"ok: events/{w}/ ({len(metas)}런, 실현합 {sum(m['total'] for m in metas):+,.0f})")
    if not sys.argv[1:]:
        render_index(all_metas)
        print("events/README.md (인덱스)")


if __name__ == "__main__":
    main()
