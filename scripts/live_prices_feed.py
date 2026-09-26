# -*- coding: utf-8 -*-
"""전 종목(코스피+코스닥) 장중 시세 보조 프로세스.

KIS REST는 초당 한도가 좁아 2,700종목을 2분마다 훑을 수 없다. 네이버 벌크 시세는
종목을 묶어 주고 KIS 한도와 무관하므로, 여기서 받아 파일로 떨궈
엔진(UniverseScanner)이 읽게 한다. regime.json·universe_scan.json과 같은 파일 전달다.

한 요청 종목 수는 1,000까지 받아주고 1,500부터 HTTP 400이다(2026-09-26 실측, URL 약 8KB 한도).
900씩 끊으면 전 시장 2,760종목이 요청 3개라, 5초 주기여도 초당 0.6요청이다.
1초 주기 10분 소크(요청 1,800건)도 실패 0이었지만 기본은 5초로 둔다 — 더 줄일 때는
PRICES_PERIOD_SEC 환경변수로.

산출: Quant/config/prices_live.json
  {"ts": <epoch>, "hhmm": "1503", "count": N,
   "prices": {"005930": {"px": 270000.0, "vol": 1234567, "val": 3.3e11, "mcap": 4.5e14}, ...}}
  px = 현재가, nm = 종목명, vol = 누적거래량(주), val = 누적거래대금(원), mcap = 시가총액(원)
  mcap은 universe_feed.py가 재랭킹할 때 이 파일을 재사용하려고 담는다.
"""
import concurrent.futures, io, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNIV = os.path.join(ROOT, 'Quant', 'config', 'universe_scan.json')
OUT  = os.path.join(ROOT, 'Quant', 'config', 'prices_live.json')
CHUNK = 900
PERIOD = int(os.environ.get('PRICES_PERIOD_SEC', '5'))
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


def fetch_safe(codes):
    """fetch()와 같되 실패하면 None — 스레드 풀 안에서 예외로 사이클을 깨지 않게."""
    try:
        return fetch(codes)
    except Exception as error:
        sys.stderr.write('청크(%d종목) 실패: %s\n' % (len(codes), error))
        return None


def cycle(codes):
    out, miss = {}, 0
    parts = [codes[start:start + CHUNK] for start in range(0, len(codes), CHUNK)]
    # 요청 3개를 동시에 보낸다 — 순차 0.5초 안팎이 0.2초로 준다. 실패한 청크만 빠진다.
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(parts)) as pool:
        results = pool.map(lambda part: (part, fetch_safe(part)), parts)

    for part, rows in results:
        if rows is None:
            miss += len(part)
            continue
        for r in rows:
            t = r.get('itemCode')
            price = num(r.get('closePriceRaw') or r.get('closePrice'))
            if not t or price <= 0:
                continue
            # Raw 필드를 쓴다 — 표시용 accumulatedTradingValue는 '5조 5,043억'처럼 접미사가
            #  둘이라 1조 넘는 종목이 전부 0으로 읽혔다(2026-09-26 실측).
            out[t] = {'px': price, 'nm': r.get('stockName') or '',
                      'vol': num(r.get('accumulatedTradingVolumeRaw') or r.get('accumulatedTradingVolume')),
                      'val': num(r.get('accumulatedTradingValueRaw') or r.get('accumulatedTradingValue')),
                      'mcap': num(r.get('marketValueFullRaw'))}
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
        # 하한 0.2초 — PRICES_PERIOD_SEC=1로 내려도 주기가 1.2초로 늘어지지 않게.
        time.sleep(max(0.2, PERIOD - (time.time() - t0)))


if __name__ == '__main__':
    main()
