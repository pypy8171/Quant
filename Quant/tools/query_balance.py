#!/usr/bin/env python3
# 모의계좌 잔고 조회(연속조회 포함). 사용: python3 Quant/tools/query_balance.py [config경로]
# 기본 config: Quant/config/config_itb_paper.json (kis 블록의 모의 키/계좌 사용)
import json, sys, urllib.request, urllib.parse

path = sys.argv[1] if len(sys.argv) > 1 else "Quant/config/config_itb_paper.json"
cfg = json.load(open(path, encoding="utf-8"))["kis"]
BASE = "https://openapivts.koreainvestment.com:29443" if cfg.get("is_paper") \
       else "https://openapi.koreainvestment.com:9443"

tok = json.load(urllib.request.urlopen(urllib.request.Request(
    BASE + "/oauth2/tokenP",
    data=json.dumps({"grant_type": "client_credentials",
                     "appkey": cfg["app_key"], "appsecret": cfg["app_secret"]}).encode(),
    headers={"content-type": "application/json"})))["access_token"]

def call(fk, nk, cont):
    q = {"CANO": cfg["account_no"], "ACNT_PRDT_CD": cfg["account_type"],
         "AFHR_FLPR_YN": "N", "OFL_YN": "", "INQR_DVSN": "02", "UNPR_DVSN": "01",
         "FUND_STTL_ICLD_YN": "N", "FNCG_AMT_AUTO_RDPT_YN": "N", "PRCS_DVSN": "00",
         "CTX_AREA_FK100": fk, "CTX_AREA_NK100": nk}
    tr = "VTTC8434R" if cfg.get("is_paper") else "TTTC8434R"
    h = {"authorization": "Bearer " + tok, "appkey": cfg["app_key"],
         "appsecret": cfg["app_secret"], "tr_id": tr, "tr_cont": cont}
    url = BASE + "/uapi/domestic-stock/v1/trading/inquire-balance?" + urllib.parse.urlencode(q)
    return json.load(urllib.request.urlopen(urllib.request.Request(url, headers=h)))

rows, fk, nk, cont, last = [], "", "", "", {}
while True:
    j = call(fk, nk, cont); last = j
    rows += j.get("output1", [])
    nk = j.get("ctx_area_nk100", "").strip()
    if not nk:
        break
    fk = j.get("ctx_area_fk100", "").strip(); cont = "N"

print(f"보유 {len(rows)}종목")
print(f"{'코드':6} {'종목명':12} {'수량':>6} {'평단':>10} {'현재가':>10} {'평가손익':>13}")
for r in sorted(rows, key=lambda x: -int(x.get("evlu_pfls_amt", "0") or 0)):
    print(f"{r['pdno']:6} {r['prdt_name'][:12]:12} {r['hldg_qty']:>6} "
          f"{r['pchs_avg_pric']:>10} {r['prpr']:>10} {r['evlu_pfls_amt']:>13}")
o2 = (last.get("output2") or [{}])[0]
print("-" * 60)
print(f"예수금(D+2): {o2.get('prvs_rcdl_excc_amt','?')}  "
      f"총평가금액: {o2.get('tot_evlu_amt','?')}  "
      f"평가손익합: {o2.get('evlu_pfls_smtl_amt','?')}")
