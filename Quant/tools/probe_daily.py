#!/usr/bin/env python3
# 일봉(FHKST03010100) 확보 가능성 관문 검증 + G1 수정방향(유한 날짜창) 검증.
#   기존 get_daily_ohlcv는 날짜를 19000101~99991231로 하드코딩 → 모의 500(G1).
#   여기선 오늘-Ndays ~ 오늘 유한창으로 찍어 (a)모의 지원 여부 (b)1콜 반환 봉수를 본다.
# 사용: python3 Quant/tools/probe_daily.py [config경로] [종목코드] [일수N]
#   기본: config_itb_paper.json  005930  120
import json, sys, urllib.request, urllib.parse, urllib.error
from datetime import datetime, timezone, timedelta

path   = sys.argv[1] if len(sys.argv) > 1 else "Quant/config/config_itb_paper.json"
ticker = sys.argv[2] if len(sys.argv) > 2 else "005930"
days   = int(sys.argv[3]) if len(sys.argv) > 3 else 120

cfg   = json.load(open(path, encoding="utf-8"))["kis"]
paper = cfg.get("is_paper")
BASE  = "https://openapivts.koreainvestment.com:29443" if paper \
        else "https://openapi.koreainvestment.com:9443"

now_kst = datetime.now(timezone(timedelta(hours=9)))
d2 = now_kst.strftime("%Y%m%d")
d1 = (now_kst - timedelta(days=days)).strftime("%Y%m%d")
print(f"[probe] config={path}  domain={'모의' if paper else '실계좌'}  "
      f"ticker={ticker}  창={d1}~{d2}")

tok = json.load(urllib.request.urlopen(urllib.request.Request(
    BASE + "/oauth2/tokenP",
    data=json.dumps({"grant_type": "client_credentials",
                     "appkey": cfg["app_key"], "appsecret": cfg["app_secret"]}).encode(),
    headers={"content-type": "application/json"})))["access_token"]

q = {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": ticker,
     "FID_INPUT_DATE_1": d1, "FID_INPUT_DATE_2": d2,
     "FID_PERIOD_DIV_CODE": "D", "FID_ORG_ADJ_PRC": "0"}  # 0=수정주가
h = {"authorization": "Bearer " + tok, "appkey": cfg["app_key"],
     "appsecret": cfg["app_secret"], "tr_id": "FHKST03010100", "tr_cont": ""}
url = (BASE + "/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice?"
       + urllib.parse.urlencode(q))
try:
    j = json.load(urllib.request.urlopen(urllib.request.Request(url, headers=h)))
except urllib.error.HTTPError as e:
    body = e.read().decode("utf-8", "replace")[:400]
    print(f"\n실패  HTTP {e.code}: {body}")
    print("판정: 유한 날짜창으로도 이 도메인에서 일봉 불가. "
          "모의라면 시세는 실계좌 도메인으로 분리 필요.")
    sys.exit(1)

out2 = j.get("output2") or []
print(f"rt_cd={j.get('rt_cd')} msg={j.get('msg1','').strip()} 봉수={len(out2)}")
if not out2:
    print("판정: 응답 정상이나 봉 0개 — 날짜창/종목 확인.")
    sys.exit(1)

# output2는 최신→과거 순. 필드: stck_bsop_date, stck_oprc/hgpr/lwpr/clpr, acml_vol
sample = out2[0]
print("[샘플 봉 필드]", {k: sample.get(k) for k in
      ("stck_bsop_date", "stck_oprc", "stck_hgpr", "stck_lwpr", "stck_clpr", "acml_vol")})
dates = sorted(b.get("stck_bsop_date", "") for b in out2 if b.get("stck_bsop_date"))
print(f"\n날짜 범위: {dates[0]} ~ {dates[-1]}  (거래일 {len(dates)}개)")
print(f"판정: 유한창 일봉 확보 {'가능' if len(out2) >= 20 else '봉 부족'}. "
      f"1콜 반환 {len(out2)}봉{'(≈100 상한 근처면 >100봉엔 페이지네이션 필요)' if len(out2) >= 95 else ''}. "
      f"→ get_daily_ohlcv의 G1(날짜 하드코딩)을 이 유한창 방식으로 고치면 모의에서 동작.")
