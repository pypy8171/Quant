# -*- coding: utf-8 -*-
"""교체·슬롯거부 가정 비교(counterfactual) 추출기.

엔진 로그만 읽어 두 가지 자연실험 표본을 만든다. 코드 재빌드가 필요 없고,
이미 지나간 날짜도 로그가 남아 있으면 소급해서 뽑을 수 있다.

  1) 교체(Displace) — 축출한 종목과 들여온 종목의 이후 수익률을 나란히 둔다.
     축출한 쪽이 더 올랐으면 교체 규칙이 틀린 것이다.
  2) 슬롯초과 거부 — 동시보유 상한이 비용인지 필터인지 가른다. 거부된 신호들의
     평균 수익률이 보유분보다 높으면 상한이 비용을 물리고 있다는 뜻이다.

가격은 로그에 흩어진 "현재가=" 관측으로 티커별 타임라인을 만들어 이벤트 시각에
가장 가까운 값을 붙인다(TOL_SEC 이내). 사후 가격은 네이버 벌크 시세로 받는다.

  py scripts/extract_swap_counterfactual.py [--date 2026-09-08] [--no-fetch]

산출: research/runs/swap_counterfactual/<date>_displace.csv
      research/runs/swap_counterfactual/<date>_rejects.csv
"""
import argparse
import bisect
import csv
import io
import json
import os
import re
import sys
import urllib.request
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOG = os.path.join(ROOT, 'Quant', 'build_win', 'logs', 'quant_trader.log')
OUTDIR = os.path.join(ROOT, 'research', 'runs', 'swap_counterfactual')
TOL_SEC = 180          # 이벤트 시각과 가격 관측 사이 허용 간격
CHUNK = 100
UA = {'User-Agent': 'Mozilla/5.0', 'Referer': 'https://finance.naver.com/'}

TS = r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.\d{3}'
RE_PRICE = re.compile(TS + r'.*?\b(\d{6})\([^)]*\).*?현재가=([\d.]+)')
RE_DISPLACE = re.compile(
    TS + r' \[INFO \] \[Displace\] (\d{6})\(([^)]*)\) 전량 매도 (\d+)주'
         r' — 교체 진입 — '
         r'(\d{6})\(z=(-?[\d.]+)\)가 \d{6}\(z=(-?[\d.]+)\)보다 ([\d.]+)')
RE_REJECT = re.compile(
    TS + r' \[WARN \] \[OrderRouter\] 거부 \[(ORD-\d+)\] (\d{6}) → '
         r'동시 보유 종목 한도 초과 \((\d+) >= (\d+)\)')


def secs(hhmmss):
    h, m, s = hhmmss.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def scan(log_path, date):
    """한 번만 훑어 가격 타임라인과 두 종류 이벤트를 모은다."""
    prices = {}      # ticker -> [(sec, px), ...] 시간순
    displaces, rejects = [], []
    with io.open(log_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith(date):
                continue
            m = RE_DISPLACE.match(line)
            if m:
                displaces.append({
                    'ts': m.group(2), 'sec': secs(m.group(2)),
                    'evicted': m.group(3), 'evicted_name': m.group(4),
                    'qty': int(m.group(5)), 'entered': m.group(6),
                    'entered_z': float(m.group(7)), 'evicted_z': float(m.group(8)),
                    'z_gap': float(m.group(9)),
                })
                continue
            m = RE_REJECT.match(line)
            if m:
                rejects.append({
                    'ts': m.group(2), 'sec': secs(m.group(2)), 'ord': m.group(3),
                    'ticker': m.group(4), 'held': int(m.group(5)), 'cap': int(m.group(6)),
                })
                continue
            m = RE_PRICE.search(line)
            if m:
                prices.setdefault(m.group(3), []).append((secs(m.group(2)), float(m.group(4))))
    for v in prices.values():
        v.sort()
    return prices, displaces, rejects


def price_at(prices, ticker, sec):
    """이벤트 시각에 가장 가까운 관측가. TOL_SEC를 넘으면 빈값."""
    arr = prices.get(ticker)
    if not arr:
        return ''
    keys = [a[0] for a in arr]
    i = bisect.bisect_left(keys, sec)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(arr):
            d = abs(arr[j][0] - sec)
            if best is None or d < best[0]:
                best = (d, arr[j][1])
    if best is None or best[0] > TOL_SEC:
        return ''
    return best[1]


def fetch_close(tickers):
    """네이버 벌크로 현재가(장 마감 후면 종가)를 받는다. 실패분은 비워 둔다."""
    out = {}
    codes = sorted(t for t in tickers if t)
    for i in range(0, len(codes), CHUNK):
        part = codes[i:i + CHUNK]
        url = ('https://polling.finance.naver.com/api/realtime/domestic/stock/'
               + ','.join(part))
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=10) as r:
                rows = json.loads(r.read().decode('utf-8')).get('datas', [])
        except Exception as e:
            sys.stderr.write('시세 조회 실패(%d~): %s\n' % (i, e))
            continue
        for row in rows:
            t = row.get('itemCode')
            px = str(row.get('closePrice') or '').replace(',', '')
            try:
                if t and float(px) > 0:
                    out[t] = float(px)
            except ValueError:
                pass
    return out


def ret_pct(px0, px1):
    try:
        if float(px0) > 0 and float(px1) > 0:
            return round((float(px1) / float(px0) - 1.0) * 100.0, 3)
    except (TypeError, ValueError):
        pass
    return ''


def write_csv(path, cols, rows):
    with io.open(path, 'w', encoding='utf-8-sig', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, '') for c in cols})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--date', default=datetime.now().strftime('%Y-%m-%d'))
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--no-fetch', action='store_true', help='사후 가격 조회 생략')
    a = ap.parse_args()

    # 네이버 벌크는 "지금 가격"만 준다. 과거 날짜에 그대로 붙이면 그날 진입가 대비
    #  오늘 가격이 되어 수익률이 통째로 틀린다. 과거분은 사후가격 없이 뽑고,
    #  종가는 나중에 일봉 소스로 채운다.
    if not a.no_fetch and a.date != datetime.now().strftime('%Y-%m-%d'):
        print('%s는 과거 날짜다 — 사후 가격 조회를 건너뛴다(그날 종가가 아니라 현재가가 붙는다). '
              'close/ret 열은 비워 두고 일봉으로 따로 채운다.' % a.date)
        a.no_fetch = True

    prices, displaces, rejects = scan(a.log, a.date)
    print('%s: 가격관측 %d종목, 교체 %d건, 슬롯초과 거부 %d건'
          % (a.date, len(prices), len(displaces), len(rejects)))
    if not displaces and not rejects:
        print('추출할 이벤트가 없다.')
        return

    for d in displaces:
        d['evicted_px'] = price_at(prices, d['evicted'], d['sec'])
        d['entered_px'] = price_at(prices, d['entered'], d['sec'])
    for r in rejects:
        r['px'] = price_at(prices, r['ticker'], r['sec'])

    universe = ({d['evicted'] for d in displaces} | {d['entered'] for d in displaces}
                | {r['ticker'] for r in rejects})
    close = {} if a.no_fetch else fetch_close(universe)
    if close:
        print('사후 가격 %d종목 확보' % len(close))

    for d in displaces:
        d['evicted_close'] = close.get(d['evicted'], '')
        d['entered_close'] = close.get(d['entered'], '')
        d['evicted_ret_pct'] = ret_pct(d['evicted_px'], d['evicted_close'])
        d['entered_ret_pct'] = ret_pct(d['entered_px'], d['entered_close'])
        # 양수면 교체가 이득. 음수면 축출한 쪽이 더 올랐다 = 규칙이 틀렸다.
        if d['entered_ret_pct'] != '' and d['evicted_ret_pct'] != '':
            d['swap_edge_pct'] = round(d['entered_ret_pct'] - d['evicted_ret_pct'], 3)
        else:
            d['swap_edge_pct'] = ''
    for r in rejects:
        r['close'] = close.get(r['ticker'], '')
        r['ret_pct'] = ret_pct(r['px'], r['close'])

    if not os.path.isdir(OUTDIR):
        os.makedirs(OUTDIR)
    p1 = os.path.join(OUTDIR, '%s_displace.csv' % a.date)
    p2 = os.path.join(OUTDIR, '%s_rejects.csv' % a.date)
    write_csv(p1, ['ts', 'evicted', 'evicted_name', 'qty', 'evicted_z', 'evicted_px',
                   'evicted_close', 'evicted_ret_pct', 'entered', 'entered_z',
                   'entered_px', 'entered_close', 'entered_ret_pct', 'z_gap',
                   'swap_edge_pct'], displaces)
    write_csv(p2, ['ts', 'ord', 'ticker', 'held', 'cap', 'px', 'close', 'ret_pct'], rejects)
    print('기록: %s' % p1)
    print('기록: %s' % p2)

    edges = [d['swap_edge_pct'] for d in displaces if d['swap_edge_pct'] != '']
    if edges:
        print('교체 우위 평균 %+.3f%% (표본 %d) — 음수면 축출한 쪽이 더 올랐다'
              % (sum(edges) / len(edges), len(edges)))
    rr = [r['ret_pct'] for r in rejects if r['ret_pct'] != '']
    if rr:
        print('거부 신호 사후수익 평균 %+.3f%% (표본 %d, 티커 %d종)'
              % (sum(rr) / len(rr), len(rr), len({r['ticker'] for r in rejects})))


if __name__ == '__main__':
    main()
