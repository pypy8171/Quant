# -*- coding: utf-8 -*-
"""동시보유 상한이 비용인지 필터인지 가른다.

슬롯이 만석이라 거부된 신호와, 같은 날 실제로 진입한 신호를 같은 종가로 맞대본다.

  거부분 평균 > 진입분 평균  →  상한이 수익을 깎고 있다(비용). 슬롯을 늘리거나 교체를 공격적으로.
  거부분 평균 < 진입분 평균  →  상한이 필터로 작동 중. 넓히면 오히려 손해.

거부 시각의 가격은 extract_swap_counterfactual.py가 뽑아둔 CSV에서, 진입 가격은
당일 매매원장의 매수 체결가에서 가져온다. 종가는 FinanceDataReader 일봉.

  py scripts/analyze_slot_cost.py --date 2026-09-03 [--date 2026-09-04 ...]
"""
import argparse
import csv
import io
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import _logdir  # noqa: E402

REJ_DIR = os.path.join(ROOT, 'research', 'runs', 'swap_counterfactual')

# 기동 기동 점검과 수동 TEST 발주는 전략 매매가 아니다. 진입분 모집단에서 뺀다.
#  (TEST는 09-04에 005930을 체결가 75000으로 남겼는데 그날 실제 시세는 25만원대였다.
#   이 한 줄이 진입분 평균을 +26%로 만들어 비교를 통째로 망가뜨렸다.)
EXCLUDE_STRATEGY = ('STARTUP_PROBE', 'TEST')


def fnum(x):
    try:
        return float(x or 0)
    except (TypeError, ValueError):
        return 0.0


def load_rejects(date):
    p = os.path.join(REJ_DIR, '%s_rejects.csv' % date)
    if not os.path.isfile(p):
        return []
    rows = list(csv.DictReader(io.open(p, encoding='utf-8-sig')))
    return [r for r in rows if fnum(r.get('px')) > 0]


def load_entries(date):
    """당일 원장의 매수 체결. 같은 종목 여러 번 체결되면 수량가중 평단으로 접는다."""
    p = _logdir.find_ledger(date)  # 행 수 최대인 원장(같은 날짜가 여러 폴더에 있을 때)
    if p is None:
        return {}
    agg = {}
    for r in csv.DictReader(io.open(p, encoding='utf-8-sig')):
        if r.get('side') != 'BUY' or fnum(r.get('fill_qty')) <= 0:
            continue
        if any(r.get('strategy', '').startswith(x) for x in EXCLUDE_STRATEGY):
            continue
        t = r['ticker']
        q, px = fnum(r['fill_qty']), fnum(r['fill_price'])
        if px <= 0:
            continue
        cur = agg.setdefault(t, [0.0, 0.0])
        cur[0] += q
        cur[1] += q * px
    return {t: v[1] / v[0] for t, v in agg.items() if v[0] > 0}


def closes(tickers, date):
    """그날 종가. 휴장·상장폐지 등으로 못 받으면 그 종목은 뺀다."""
    import FinanceDataReader as fdr
    out = {}
    for t in sorted(tickers):
        try:
            d = fdr.DataReader(t, date, date)
            if len(d) and float(d['Close'].iloc[0]) > 0:
                out[t] = float(d['Close'].iloc[0])
        except Exception as e:
            sys.stderr.write('%s 일봉 실패: %s\n' % (t, e))
    return out


def pct(px0, px1):
    return (px1 / px0 - 1.0) * 100.0


def report(date):
    rejects = load_rejects(date)
    entries = load_entries(date)
    if not rejects and not entries:
        print('%s: 표본 없음' % date)
        return None

    universe = {r['ticker'] for r in rejects} | set(entries)
    cl = closes(universe, date)

    # 같은 종목이 여러 번 거부됐으면 한 번으로 접는다. 발주 재시도 횟수가
    #  모집단 가중치가 되면 안 된다.
    rej_first = {}
    for r in rejects:
        rej_first.setdefault(r['ticker'], r)
    rej_ret = {t: pct(fnum(r['px']), cl[t]) for t, r in rej_first.items() if t in cl}
    ent_ret = {t: pct(px, cl[t]) for t, px in entries.items() if t in cl}

    # 양쪽에 다 걸린 종목은 거부 모집단에서 뺀다(결국 들어갔으므로 기회비용이 아니다).
    both = set(rej_ret) & set(ent_ret)
    for t in both:
        del rej_ret[t]

    print('=== %s ===' % date)
    print('거부 신호 %d종 (중복 제거 전 %d건, 양쪽 중복 %d종 제외)'
          % (len(rej_ret), len(rejects), len(both)))
    print('진입 종목 %d종' % len(ent_ret))
    if not rej_ret or not ent_ret:
        print('한쪽 모집단이 비어 비교 불가')
        return None

    rv, ev = list(rej_ret.values()), list(ent_ret.values())
    mr, me = statistics.mean(rv), statistics.mean(ev)
    print('거부분 진입시각→종가 평균 %+.3f%% (중앙 %+.3f%%)'
          % (mr, statistics.median(rv)))
    print('진입분 체결가→종가 평균 %+.3f%% (중앙 %+.3f%%)'
          % (me, statistics.median(ev)))
    gap = mr - me
    print('격차 %+.3f%%p — %s' % (gap, '상한이 비용을 물리고 있다' if gap > 0
                                  else '상한이 필터로 작동 중'))
    print('거부 상위:', ', '.join('%s %+.2f%%' % (t, v) for t, v in
                                sorted(rej_ret.items(), key=lambda kv: -kv[1])[:5]))
    return (date, len(rv), mr, len(ev), me, gap)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--date', action='append', required=True)
    a = ap.parse_args()
    rows = [r for r in (report(d) for d in a.date) if r]
    if len(rows) > 1:
        print()
        print('=== 종합 ===')
        wr = sum(r[1] * r[2] for r in rows) / sum(r[1] for r in rows)
        we = sum(r[3] * r[4] for r in rows) / sum(r[3] for r in rows)
        print('거부분 가중평균 %+.3f%% (n=%d), 진입분 %+.3f%% (n=%d), 격차 %+.3f%%p'
              % (wr, sum(r[1] for r in rows), we, sum(r[3] for r in rows), wr - we))


if __name__ == '__main__':
    main()
