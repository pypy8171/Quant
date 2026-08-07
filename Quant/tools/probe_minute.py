#!/usr/bin/env python3
# 당일 분봉(FHKST03010200) 확보 가능성 관문 검증.
#   - 이 계좌/도메인에서 분봉 TR이 되는가 (모의 500 여부)
#   - 1콜당 봉 개수, 봉 간격(1분 네이티브 확인)
#   - 기준시각 역페이지네이션으로 3분봉 집계에 쓸 만큼 쌓이는가
# 사용: python Quant/tools/probe_minute.py [config경로] [종목코드] [기준HHMMSS] [페이지수]
#   기본: config_itb_paper.json  005930(삼성전자)  자동(현재 KST, 장전이면 133000)  4페이지
# 모의/실계좌 각각 config를 바꿔 두 번 돌려 비교하세요.
import json, sys, urllib.request, urllib.parse, urllib.error
from datetime import datetime, timezone, timedelta

path   = sys.argv[1] if len(sys.argv) > 1 else "Quant/config/config_itb_paper.json"
ticker = sys.argv[2] if len(sys.argv) > 2 else "005930"
pages  = int(sys.argv[4]) if len(sys.argv) > 4 else 4

cfg  = json.load(open(path, encoding="utf-8"))["kis"]
paper = cfg.get("is_paper")
BASE = "https://openapivts.koreainvestment.com:29443" if paper \
       else "https://openapi.koreainvestment.com:9443"

# 기준시각: 인자 우선, 아니면 현재 KST. 장 시작(0900) 전이면 직전 마감 시각으로.
if len(sys.argv) > 3:
    base_hhmmss = sys.argv[3]
else:
    now_kst = datetime.now(timezone(timedelta(hours=9)))
    hhmmss = now_kst.strftime("%H%M%S")
    base_hhmmss = hhmmss if "090000" <= hhmmss <= "153000" else "153000"

print(f"[probe] config={path}  domain={'모의' if paper else '실계좌'}  "
      f"ticker={ticker}  base={base_hhmmss}  pages={pages}")

tok = json.load(urllib.request.urlopen(urllib.request.Request(
    BASE + "/oauth2/tokenP",
    data=json.dumps({"grant_type": "client_credentials",
                     "appkey": cfg["app_key"], "appsecret": cfg["app_secret"]}).encode(),
    headers={"content-type": "application/json"})))["access_token"]

def call(hour):
    q = {"FID_ETC_CLS_CODE": "", "FID_COND_MRKT_DIV_CODE": "J",
         "FID_INPUT_ISCD": ticker, "FID_INPUT_HOUR_1": hour,
         "FID_PW_DATA_INCU_YN": "N"}
    h = {"authorization": "Bearer " + tok, "appkey": cfg["app_key"],
         "appsecret": cfg["app_secret"], "tr_id": "FHKST03010200", "tr_cont": ""}
    url = (BASE + "/uapi/domestic-stock/v1/quotations/inquire-time-itemchartprice?"
           + urllib.parse.urlencode(q))
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, headers=h))
        return json.load(r), None
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")[:300]
        return None, f"HTTP {e.code}: {body}"

# 역페이지네이션: 각 콜의 가장 이른 봉 시각 - 1분을 다음 기준으로.
bars, hour = [], base_hhmmss
for p in range(pages):
    j, err = call(hour)
    if err:
        print(f"  page{p} 기준{hour} → 실패  {err}")
        if p == 0:
            print("\n판정: 이 계좌/도메인에서 분봉 TR 사용 불가(위 오류). "
                  "모의라면 실계좌 config로 재시도 필요.")
            sys.exit(1)
        break
    rc = j.get("rt_cd")
    out2 = j.get("output2") or []
    print(f"  page{p} 기준{hour} → rt_cd={rc} msg={j.get('msg1','').strip()} 봉수={len(out2)}")
    if not out2:
        break
    bars += out2
    earliest = out2[-1].get("stck_cntg_hour", "")
    if not earliest or len(earliest) < 6:
        break
    prev = int(earliest) - 100          # 1분(=HHMMSS 100) 이전
    if prev < 90000:
        break
    hour = f"{prev:06d}"

if not bars:
    print("\n판정: 응답은 왔으나 봉 0개 — 장중 시각으로 재시도하거나 파라미터 확인.")
    sys.exit(1)

# 필드/간격 진단
sample = bars[0]
print("\n[샘플 봉 필드]", {k: sample.get(k) for k in
      ("stck_bsop_date", "stck_cntg_hour", "stck_oprc", "stck_hgpr",
       "stck_lwpr", "stck_prpr", "cntg_vol")})

times = [b.get("stck_cntg_hour", "") for b in bars if b.get("stck_cntg_hour")]
uniq = sorted(set(times))
gaps = [int(uniq[i+1]) - int(uniq[i]) for i in range(len(uniq)-1)]
one_min = sum(1 for g in gaps if g == 100)
print(f"\n총 수집 봉수: {len(bars)}  (고유 시각 {len(uniq)}개)")
print(f"봉 간격 분포: 1분(100)={one_min} / 그 외={len(gaps)-one_min}  "
      f"→ {'1분봉 네이티브 확인' if one_min >= len(gaps)*0.8 else '간격 불규칙 — 확인 필요'}")
print(f"시각 범위: {uniq[0]} ~ {uniq[-1]}")

need_1min_for_3min_sma20 = 20 * 3
print(f"\n판정: 3분봉 SMA20에 1분봉 {need_1min_for_3min_sma20}개 필요 → "
      f"현재 {pages}콜로 {len(uniq)}개 확보 "
      f"({'충분' if len(uniq) >= need_1min_for_3min_sma20 else f'부족, 약 {-(-need_1min_for_3min_sma20//max(1,len(uniq)//pages))}콜 필요'}). "
      f"3분봉 = 이 1분봉을 3개씩 OHLC 집계.")
