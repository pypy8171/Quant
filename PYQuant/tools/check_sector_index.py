"""업종 지수 TR 라이브 점검 (2026-09-11).

대시보드 섹터 변동성 카드에 쓸 두 TR을 실전 시세키(quote_kis)로 확인한다.
- FHPUP02110000 업종 구분별 전체시세: 시장(K/Q/K2) 하나로 전 업종의 현재가·등락률을 한 번에 받는지
- FHKUP03500100 업종 일봉: 업종 하나의 일봉이 몇 봉 오는지(변동성 계산은 20봉이면 된다)
주문 없음 — 시세 조회 전용.

사용 (저장소 루트에서):
    py -X utf8 PYQuant/tools/check_sector_index.py Quant/config/config_dev_paper.json
"""
import json
import sys
import time
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from kis.client import KisClient  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def main() -> None:
    cfg = json.load(open(sys.argv[1], encoding="utf-8"))
    k = cfg.get("quote_kis") or cfg["kis"]
    c = KisClient(app_key=k["app_key"], app_secret=k["app_secret"],
                  account_no=k.get("account_no", cfg["kis"].get("account_no", "")),
                  is_paper=k.get("is_paper", False))
    c.authenticate()

    print("== 업종 구분별 전체시세 (FHPUP02110000) ==")
    for mkt, iscd in [("K", "0001"), ("Q", "1001"), ("K2", "2001")]:
        for blng in ("0", "1"):
            d = c._get("/uapi/domestic-stock/v1/quotations/inquire-index-category-price",
                       {"FID_COND_MRKT_DIV_CODE": "U", "FID_INPUT_ISCD": iscd,
                        "FID_COND_SCR_DIV_CODE": "20214", "FID_MRKT_CLS_CODE": mkt,
                        "FID_BLNG_CLS_CODE": blng}, "FHPUP02110000")
            rows = d.get("output2") or []
            print(f"  mkt={mkt} blng={blng} rt={d.get('rt_cd')} msg={d.get('msg1')} rows={len(rows)}")
            for r in rows[:40]:
                print(f"    {r.get('bstp_cls_code')} {r.get('hts_kor_isnm'):<14} "
                      f"prpr={r.get('bstp_nmix_prpr')} ctrt={r.get('bstp_nmix_prdy_ctrt')} "
                      f"vol={r.get('acml_vol')} pbmn={r.get('acml_tr_pbmn')}")
            if rows:
                print("    keys:", sorted(rows[0].keys()))
            time.sleep(0.2)

    print("== 업종 일봉 (FHKUP03500100) 0013 전기전자 ==")
    d2 = date.today().strftime("%Y%m%d")
    d1 = (date.today() - timedelta(days=60)).strftime("%Y%m%d")
    d = c._get("/uapi/domestic-stock/v1/quotations/inquire-daily-indexchartprice",
               {"FID_COND_MRKT_DIV_CODE": "U", "FID_INPUT_ISCD": "0013",
                "FID_INPUT_DATE_1": d1, "FID_INPUT_DATE_2": d2, "FID_PERIOD_DIV_CODE": "D"},
               "FHKUP03500100")
    rows = d.get("output2") or []
    print(f"  rt={d.get('rt_cd')} msg={d.get('msg1')} rows={len(rows)} output1={d.get('output1')}")
    for r in rows[:3]:
        print("   ", {kk: r.get(kk) for kk in ("stck_bsop_date", "bstp_nmix_prpr", "bstp_nmix_oprc",
                                              "bstp_nmix_hgpr", "bstp_nmix_lwpr", "acml_vol")})
    if rows:
        print("  keys:", sorted(rows[0].keys()))


if __name__ == "__main__":
    main()
