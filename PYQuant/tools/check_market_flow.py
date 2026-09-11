"""시장 수급·프로그램·선물 TR 라이브 점검 (2026-09-11).

대시보드 국면 카드에 붙일 세 TR의 인자 의미와 응답 키를 실전 시세키(quote_kis)로 확인한다.
KIS 문서에 인자 값 설명이 없어(예제 999·S001뿐) 조합을 돌려 어떤 시장이 나오는지 본다.
주문 없음 — 시세 조회 전용.

사용 (저장소 루트에서):
    py PYQuant/tools/check_market_flow.py Quant/config/config_dev_paper.json
"""
import json
import sys
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

    print("== 투자자매매동향(시세) 조합별 첫 행 ==")
    for iscd, iscd2 in [("999", "S001"), ("999", "S002"), ("0001", "S001"), ("1001", "S001"),
                        ("KSP", "S001"), ("KSQ", "S001"), ("999", "F001"), ("999", "S003")]:
        rows = c.get_investor_time_by_market(iscd, iscd2)
        head = rows[0] if rows else {}
        keys = ("frgn_ntby_tr_pbmn", "orgn_ntby_tr_pbmn", "prsn_ntby_tr_pbmn")
        print(f"  {iscd}/{iscd2}: rows={len(rows)} " +
              " ".join(f"{kk}={head.get(kk)}" for kk in keys) +
              (f" 기타키={sorted(head.keys())[:6]}" if head else ""))

    print("== 프로그램매매 종합현황(시간) ==")
    for cls in ("K", "Q"):
        rows = c.get_program_trade_today(cls)
        print(f"  {cls}: rows={len(rows)} first={json.dumps(rows[0], ensure_ascii=False) if rows else None}")

    print("== 선물 전광판(MKI) ==")
    board = c.get_future_board()
    for r in board[:4]:
        print("  " + json.dumps({kk: r.get(kk) for kk in list(r.keys())[:8]}, ensure_ascii=False))
    print(f"  전체 키: {sorted(board[0].keys()) if board else None}")

    if board:
        code = board[0].get("futs_shrn_iscd") or board[0].get("shrn_iscd")
        print(f"== 선물 현재가({code}) ==")
        fp = c.get_future_price(code)
        for key, val in fp.items():
            v = val[0] if isinstance(val, list) and val else val
            if isinstance(v, dict):
                print(f"  {key}: " + json.dumps({kk: v.get(kk) for kk in (
                    "futs_prpr", "futs_prdy_ctrt", "basis", "hts_otst_stpl_qty", "futs_last_tr_date",
                    "bstp_nmix_prpr", "hts_kor_isnm", "futs_shrn_iscd")}, ensure_ascii=False))
                print(f"     키: {sorted(v.keys())}")


if __name__ == "__main__":
    main()
