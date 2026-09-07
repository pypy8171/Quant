#!/usr/bin/env python3
# D-005 검증 — 일봉 응답에 당일 봉이 섞이는지, 섞이면 이동평균을 얼마나 움직이는지 잰다.
#   get_daily_ohlcv는 이제 응답에서 오늘(KST) 날짜 행을 버린다(include_today=false 기본).
#   이 스크립트는 같은 응답으로 절단 전/후 SMA를 나란히 계산해 차이를 보여준다.
# 사용: py Quant/tools/probe_daily_truncation.py [config경로] [종목,종목,...]
#   기본: config_dev_paper.json  005930,000660,161890
import json, sys, urllib.request, urllib.parse, urllib.error
from datetime import datetime, timezone, timedelta

path    = sys.argv[1] if len(sys.argv) > 1 else "Quant/config/config_dev_paper.json"
tickers = (sys.argv[2] if len(sys.argv) > 2 else "005930,000660,161890").split(",")

cfg   = json.load(open(path, encoding="utf-8"))["kis"]
BASE  = "https://openapivts.koreainvestment.com:29443" if cfg.get("is_paper") \
        else "https://openapi.koreainvestment.com:9443"
now_kst = datetime.now(timezone(timedelta(hours=9)))
d2 = now_kst.strftime("%Y%m%d")
d1 = (now_kst - timedelta(days=120)).strftime("%Y%m%d")

tok = json.load(urllib.request.urlopen(urllib.request.Request(
    BASE + "/oauth2/tokenP",
    data=json.dumps({"grant_type": "client_credentials",
                     "appkey": cfg["app_key"], "appsecret": cfg["app_secret"]}).encode(),
    headers={"content-type": "application/json"})))["access_token"]

def sma(rows, n):
    if len(rows) < n:
        return None
    return sum(float(r["stck_clpr"]) for r in rows[:n]) / n

print(f"[D-005] domain={'모의' if cfg.get('is_paper') else '실계좌'}  KST오늘={d2}  "
      f"현재={now_kst.strftime('%H:%M')}")
print(f"{'종목':>8} {'최신봉':>9} {'당일포함':>4} "
      f"{'SMA5(포함)':>12} {'SMA5(절단)':>12} {'Δ%':>7} "
      f"{'SMA20(포함)':>12} {'SMA20(절단)':>12} {'Δ%':>7}")

for t in tickers:
    q = {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": t,
         "FID_INPUT_DATE_1": d1, "FID_INPUT_DATE_2": d2,
         "FID_PERIOD_DIV_CODE": "D", "FID_ORG_ADJ_PRC": "0"}
    h = {"authorization": "Bearer " + tok, "appkey": cfg["app_key"],
         "appsecret": cfg["app_secret"], "tr_id": "FHKST03010100", "tr_cont": ""}
    url = (BASE + "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice?"
           + urllib.parse.urlencode(q))
    try:
        rows = [r for r in json.load(urllib.request.urlopen(
            urllib.request.Request(url, headers=h)))["output2"] if r.get("stck_clpr")]
    except urllib.error.HTTPError as e:
        print(f"{t:>8}  HTTP {e.code}")
        continue
    if not rows:
        print(f"{t:>8}  빈 응답")
        continue

    head = rows[0].get("stck_bsop_date", "")
    has_today = (head == d2)
    cut = [r for r in rows if r.get("stck_bsop_date") != d2]   # 엔진과 같은 규칙

    def line(n):
        a, b = sma(rows, n), sma(cut, n)
        if a is None or b is None:
            return "        -", "        -", "      -"
        return f"{a:12,.1f}", f"{b:12,.1f}", f"{(a - b) / b * 100:7.3f}"

    a5, b5, p5    = line(5)
    a20, b20, p20 = line(20)
    print(f"{t:>8} {head:>9} {('예' if has_today else '아니오'):>4} "
          f"{a5} {b5} {p5} {a20} {b20} {p20}")

print("\n판정: '당일포함=예'인 종목은 절단 전 지표가 오늘 가격을 물고 있었다는 뜻이다.")
print("      장중이면 그 값은 현재가를 따라 움직이고, 스냅샷을 하루 1회만 갱신하는")
print("      호출자에게는 페치 시각에 동결된다. Δ%가 dev_buy(0.8%)와 비교할 크기다.")
