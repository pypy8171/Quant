# -*- coding: utf-8 -*-
"""전 종목(코스피+코스닥) 장중 시세 보조 프로세스.

KIS REST는 초당 한도가 좁아 2,700종목을 2분마다 훑을 수 없다. 네이버 벌크 시세는
한 번에 100종목을 묶어 주고 KIS 한도와 무관하므로, 여기서 받아 파일로 떨궈
엔진(UniverseScanner)이 읽게 한다. regime.json·universe_scan.json과 같은 파일 전달다.

산출: Quant/config/prices_live.json
  {"ts": <epoch>, "hhmm": "1503", "count": N,
   "prices": {"005930": {"px": 270000.0, "vol": 1234567, "val": 3.3e11}, ...}}
  px = 현재가, nm = 종목명, vol = 누적거래량(주), val = 누적거래대금(원)
"""
import io, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNIV = os.path.join(ROOT, 'Quant', 'config', 'universe_scan.json')
OUT  = os.path.join(ROOT, 'Quant', 'config', 'prices_live.json')
CHUNK = 100
PERIOD = int(os.environ.get('PRICES_PERIOD_SEC', '20'))
UA = {'User-Agent': 'Mozilla/5.0', 'Referer': 'https://finance.naver.com/'}


def num(v):
    """'1,201' / 1201 / '5.98억' / '-' 을 float로. 못 읽으면 0.0."""
    if v is None:
        return 0.0
    if isinstance(v, (int, float)):
        return float(v)
    s = str(v).strip().replace(',', '')
    if not s or s == '-':
        return 0.0
    mult = 1.0
    for suf, m in (('조', 1e12), ('억', 1e8), ('만', 1e4)):
        if s.endswith(suf):
            s, mult = s[:-1], m
            break
    try:
        return float(s) * mult
    except ValueError:
        return 0.0


def load_codes():
    with io.open(UNIV, encoding='utf-8') as f:
        j = json.load(f)
    mm = j.get('market_map') or {}
    codes = [c for c in mm if len(c) == 6 and c.isdigit()]
    if not codes:  # market_map이 없으면 universe 배열로 폴백
        codes = [e.get('ticker') for e in j.get('universe', []) if e.get('ticker')]
    return sorted(set(codes)), mm


def fetch(codes):
    url = 'https://polling.finance.naver.com/api/realtime/domestic/stock/' + ','.join(codes)
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode('utf-8')).get('datas', [])


def cycle(codes):
    out, miss = {}, 0
    for i in range(0, len(codes), CHUNK):
        part = codes[i:i + CHUNK]
        try:
            rows = fetch(part)
        except Exception as e:
            miss += len(part)
            sys.stderr.write('chunk %d 실패: %s\n' % (i // CHUNK, e))
            continue
        for r in rows:
            t = r.get('itemCode')
            px = num(r.get('closePrice'))
            if not t or px <= 0:
                continue
            out[t] = {'px': px, 'nm': r.get('stockName') or '',
                      'vol': num(r.get('accumulatedTradingVolume')),
                      'val': num(r.get('accumulatedTradingValue'))}
    return out, miss


def main():
    codes, _ = load_codes()
    print('전 종목 시세 보조 프로세스 시작: %d종목, %d초 주기 → %s' % (len(codes), PERIOD, OUT), flush=True)
    while True:
        t0 = time.time()
        prices, miss = cycle(codes)
        # 원자적 교체 — 엔진이 반쪽 파일을 읽지 않게 한다.
        tmp = OUT + '.tmp'
        with io.open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'ts': int(time.time()),
                       'hhmm': time.strftime('%H%M'),
                       'count': len(prices),
                       'prices': prices}, f, ensure_ascii=False)
        os.replace(tmp, OUT)
        print('%s  %d종목 갱신 (실패 %d, %.1fs)' % (time.strftime('%H:%M:%S'), len(prices), miss, time.time() - t0), flush=True)
        time.sleep(max(1.0, PERIOD - (time.time() - t0)))


if __name__ == '__main__':
    main()
