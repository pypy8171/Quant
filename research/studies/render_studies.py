#!/usr/bin/env python3
"""
폴더형 백테스트 스터디(BT-01/02/03) 원장 렌더러.

입력:
  raw/{run}_trades.csv   (열: date,ticker,name,side,price,quantity,pnl)
출력:
  {study}/README.md      스터디 요약(런 목록 + 실현손익 표 + 원장 링크 + BACKTEST_LOG 링크)
  {study}/{run_key}.md   런별 종목 진입→청산 왕복 원장

지표 수치(수익률·샤프·MDD 전체)는 손으로 옮기지 않고 BACKTEST_LOG 실행 #N을 링크한다.
원장의 실현손익·승률·왕복수는 CSV에서 직접 계산(=진실원천).

사용:
  python3 render_studies.py           # 전부 재생성
  python3 render_studies.py 01        # 특정 스터디만
"""
import csv, sys, os
from collections import defaultdict, deque
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw")

# study_key -> dict(folder, title, log_ref, question, setup, note, runs=[(csv_stem, run_key, run_label), ...])
STUDIES = {
    "01": dict(
        folder="01_momentum_regime",
        title="BT-01 · 모멘텀 × 국면필터, 최근 1~5년 롤링 (기준선)",
        log_ref="실행 #1",
        question="시총 상위 100에 6-1 모멘텀 + 200MA 국면필터를 굴리면 비용 물고 벤치를 이기나? 기간에 견고한가?",
        setup="`CrossMomentumStrategy` lb120/skip20/top10/rb5, regime ON, 동일가중, datagokr 일봉, "
              "유니버스 시총상위100(PIT), END=2026-08-05 고정. 창 길이만 1~5년으로 변화.",
        note="알파가 최근 12개월에 집중되고 3년창에선 엣지가 거의 사라진다. 전 구간 최대낙폭(MDD) 게이트(≤15%) 불합격이라 "
             "1순위 과제는 '수익'이 아니라 '낙폭 통제'로 잡았다(BT-02로 이어짐).",
        runs=[("bt_1y", "1y", "최근 1년"), ("bt_2y", "2y", "최근 2년"),
              ("bt_3y", "3y", "최근 3년"), ("bt_4y", "4y", "최근 4년"),
              ("bt_5y", "5y", "최근 5년")],
    ),
    "02": dict(
        folder="02_vol_target",
        title="BT-02 · 변동성 타게팅(vol_target=0.15) 사이징",
        log_ref="실행 #2",
        question="노출을 변동성으로 조절하면 샤프(위험조정수익) 지키며 최대낙폭(MDD)를 게이트(≤15%)까지 낮추나? (변수: 사이징만)",
        setup="BT-01과 동일하되 동일가중 → 20일 실현변동성 기준 연 15% 목표로 종목별 비중 조절. 창 1~5년.",
        note="MDD 전 구간 8~12%p 개선·샤프 유지, 1년 −11.6%로 게이트 첫 통과. 거래수 급증·초과수익(α) 음전은 "
             "벤치가 풀노출이라 당연(위험조정으론 미달 아님). vol_target 채택.",
        runs=[("bt2_1y", "1y", "최근 1년"), ("bt2_2y", "2y", "최근 2년"),
              ("bt2_3y", "3y", "최근 3년"), ("bt2_4y", "4y", "최근 4년"),
              ("bt2_5y", "5y", "최근 5년")],
    ),
    "03": dict(
        folder="03_2022_ablation",
        title="BT-03 · 2022 약세장 격리 표본외(OOS) + 국면필터 ON/OFF 절제실험(ablation)",
        log_ref="실행 #3",
        question="모멘텀 엣지가 처음 보는 약세장에서 살아남나? 국면필터의 순수 기여는?",
        setup="최근 롤링 → 격리된 2022 약세장(1개년). regime ON vs OFF만 토글(변수 하나). 나머지 BT-01과 동일.",
        note="모멘텀 단독(OFF) −35.4%로 벤치(−20.5%)보다도 나쁨(모멘텀 크래시). regime ON은 1년 내내 "
             "100% 현금화로 큰 낙폭을 피했다. ON의 '체결 없음'은 방어 성과가 아니라 비참여이므로 OFF와 짝으로 해석한다.",
        runs=[("bt_2022bear", "regime_on", "regime ON (200MA 국면필터)"),
              ("bt_2022bear_noregime", "regime_off", "regime OFF (모멘텀 단독)")],
    ),
}
ORDER = ["01", "02", "03"]


def _d(s):
    return datetime.strptime(s[:10], "%Y-%m-%d")


def load_csv(path, delim=","):
    with open(path, encoding="utf-8-sig") as f:
        return list(csv.DictReader(f, delimiter=delim))


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


def render_run(study, csv_stem, run_key, run_label):
    path = os.path.join(RAW, f"{csv_stem}_trades.csv")
    if not os.path.exists(path):
        return None
    trips, opens = round_trips(load_csv(path))
    win = [t for t in trips if t["pnl"] > 0]
    total = sum(t["pnl"] for t in trips)
    winrt = len(win) / len(trips) * 100 if trips else 0.0

    L = [f"# {study['title']}", f"## {run_label}", "",
         f"- 원천 export: `{csv_stem}.json` · 지표 전체(수익률·샤프(위험조정수익)·최대낙폭(MDD)·초과수익(α))는 "
         f"[BACKTEST_LOG {study['log_ref']}](../../BACKTEST_LOG.md)",
         f"- 왕복 **{len(trips)}건** · 승 {len(win)}/패 {len(trips)-len(win)} · 승률 {winrt:.1f}% · "
         f"실현손익 합 **{won(total)}원**", ""]

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
        L += ["_체결 없음 — regime 필터가 전 구간 신규 진입을 차단(100% 현금 보유)._", ""]

    if opens:
        L += ["## 종료시점 보유 중 (미실현)", "", "| 진입일 | 종목 | 진입가 | 수량 |", "|---|---|---:|---:|"]
        for t in sorted(opens, key=lambda x: x["entry_date"]):
            L.append(f"| {t['entry_date']} | {t['name']}({t['ticker']}) | {t['entry_price']:,.0f} | {t['qty']:,} |")
        L.append("")

    L += ["---", "← [스터디 요약](README.md) · [백테스트 카탈로그](../../BACKTESTS.md) · [research 허브](../../README.md)"]
    return "\n".join(L), {"run_key": run_key, "run_label": run_label, "n": len(trips),
                          "winrt": winrt, "total": total, "opens": len(opens)}


def render_study(key):
    study = STUDIES[key]
    d = os.path.join(HERE, study["folder"])
    os.makedirs(d, exist_ok=True)
    metas = []
    for csv_stem, run_key, run_label in study["runs"]:
        res = render_run(study, csv_stem, run_key, run_label)
        if res is None:
            print(f"  skip(CSV 없음): {csv_stem}"); continue
        md, meta = res
        with open(os.path.join(d, f"{run_key}.md"), "w", encoding="utf-8") as f:
            f.write(md)
        metas.append(meta)

    L = [f"# {study['title']}", "",
         f"**질문.** {study['question']}", "",
         f"**설정.** {study['setup']}", "",
         f"**발견.** {study['note']}", "",
         f"> 수익률·샤프(위험조정수익)·최대낙폭(MDD)·초과수익(α) 전체 지표표는 [BACKTEST_LOG {study['log_ref']}](../../BACKTEST_LOG.md), "
         f"카드 요약은 [BACKTESTS.md](../../BACKTESTS.md). 아래는 각 런의 **실현손익 원장**(CSV 직접 계산).", "",
         "| 런 | 왕복수 | 승률 | 실현손익 합(원) | 종료시 보유 | 원장 |",
         "|---|---:|---:|---:|---:|---|"]
    for m in metas:
        L.append(f"| {m['run_label']} | {m['n']} | {m['winrt']:.1f}% | {won(m['total'])} | "
                 f"{m['opens']}종목 | [원장]({m['run_key']}.md) |")
    L += ["", "> 실현손익 합 = 청산 완료된 왕복의 손익만 합산(종료시점 보유분 미실현 제외). "
          "누적 총수익률과 다른 이유는 미실현·현금·복리 때문 — 방향/종목 드릴다운용 지표.", "",
          "← [백테스트 카탈로그](../../BACKTESTS.md) · [research 허브](../../README.md)"]
    with open(os.path.join(d, "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"ok: {study['folder']}/ ({len(metas)}런, 실현합 {sum(m['total'] for m in metas):+,.0f})")


def main():
    targets = sys.argv[1:] or ORDER
    for k in targets:
        if k not in STUDIES:
            print(f"skip(모르는 스터디): {k}"); continue
        render_study(k)


if __name__ == "__main__":
    main()
