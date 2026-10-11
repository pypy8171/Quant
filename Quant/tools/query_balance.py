#!/usr/bin/env python3
# 모의계좌 잔고 조회(연속조회 포함). 사용: python3 Quant/tools/query_balance.py [config경로]
# 기본 config: Quant/config/config_itb_paper.json (kis 블록의 모의 키/계좌 사용)
import json, sys, urllib.request, urllib.parse
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # 저장소 루트 — PYQuant 패키지
from PYQuant.kis.endpoints import rest_base_url

path = sys.argv[1] if len(sys.argv) > 1 else "Quant/config/config_itb_paper.json"
config = json.load(open(path, encoding="utf-8"))["kis"]
BASE = rest_base_url(bool(config.get("is_paper")))

token = json.load(urllib.request.urlopen(urllib.request.Request(
    BASE + "/oauth2/tokenP",
    data=json.dumps({"grant_type": "client_credentials",
                     "appkey": config["app_key"], "appsecret": config["app_secret"]}).encode(),
    headers={"content-type": "application/json"})))["access_token"]

def call(search_key, next_key, continuation):
    query = {"CANO": config["account_no"], "ACNT_PRDT_CD": config["account_type"],
             "AFHR_FLPR_YN": "N", "OFL_YN": "", "INQR_DVSN": "02", "UNPR_DVSN": "01",
             "FUND_STTL_ICLD_YN": "N", "FNCG_AMT_AUTO_RDPT_YN": "N", "PRCS_DVSN": "00",
             "CTX_AREA_FK100": search_key, "CTX_AREA_NK100": next_key}
    transaction_id = "VTTC8434R" if config.get("is_paper") else "TTTC8434R"
    headers = {"authorization": "Bearer " + token, "appkey": config["app_key"],
               "appsecret": config["app_secret"], "tr_id": transaction_id, "tr_cont": continuation}
    url = BASE + "/uapi/domestic-stock/v1/trading/inquire-balance?" + urllib.parse.urlencode(query)
    return json.load(urllib.request.urlopen(urllib.request.Request(url, headers=headers)))

rows, search_key, next_key, continuation, last = [], "", "", "", {}
while True:
    document = call(search_key, next_key, continuation); last = document
    rows += document.get("output1", [])
    next_key = document.get("ctx_area_nk100", "").strip()
    if not next_key:
        break
    search_key = document.get("ctx_area_fk100", "").strip(); continuation = "N"

print(f"보유 {len(rows)}종목")
print(f"{'코드':6} {'종목명':12} {'수량':>6} {'평단':>10} {'현재가':>10} {'평가손익':>13}")
for row in sorted(rows, key=lambda holding: -int(holding.get("evlu_pfls_amt", "0") or 0)):
    print(f"{row['pdno']:6} {row['prdt_name'][:12]:12} {row['hldg_qty']:>6} "
          f"{row['pchs_avg_pric']:>10} {row['prpr']:>10} {row['evlu_pfls_amt']:>13}")
summary = (last.get("output2") or [{}])[0]
print("-" * 60)
print(f"예수금(D+2): {summary.get('prvs_rcdl_excc_amt','?')}  "
      f"총평가금액: {summary.get('tot_evlu_amt','?')}  "
      f"평가손익합: {summary.get('evlu_pfls_smtl_amt','?')}")
