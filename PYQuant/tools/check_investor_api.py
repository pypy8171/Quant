"""
PRE 검증 스크립트 — 수급 API + 일봉 과거 데이터 가용성 확인

실행 방법 (PYQuant/ 디렉토리에서):
    python -m tools.check_investor_api

확인 항목:
  1. FHKST01010900: 과거 일별 수급 시계열 반환 여부 (vs 당일치만)
  2. 일봉(FHKST03010100): 2022-01-01~까지 조회 가능한지 확인
  3. 수급도 2022년치를 반환하는지 확인

Pass 기준:
  - 항목 1: output2에 복수의 날짜 행이 오고, 날짜 필드 + 외인/기관 순매수량 포함
  - 항목 2: 2022-01-01~10 조회 시 bars 5~7개 반환
  - 항목 3: 2022년 수급 조회 시 비어 있지 않음

Fail 시 조치:
  - 항목 1 Fail → DEV_GUIDE_STRATEGY_A.md 0-3절의 대안 TR 탐색
  - 항목 2/3 Fail → STEP G의 start_date를 조회 가능한 최초 날짜로 조정
"""
import sys
import json
import os

# PYQuant 루트를 sys.path에 추가 (python -m 실행 시 자동이지만 직접 실행 보험)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from kis.client import from_config


def _divider(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print('='*60)


def check_investor_flow_api(kis):
    """검증 1: FHKST01010900이 과거 일별 시계열을 반환하는지"""
    _divider("검증 1 — 수급 API 일별 시계열 (FHKST01010900)")

    resp = kis._get(
        "/uapi/domestic-stock/v1/quotations/inquire-investor",
        {
            "FID_COND_MRKT_DIV_CODE": "J",
            "FID_INPUT_ISCD":         "005930",  # 삼성전자
            "FID_INPUT_DATE_1":       "20250401",
            "FID_INPUT_DATE_2":       "20250430",
        },
        "FHKST01010900",
    )

    output2 = resp.get("output2", [])
    print(f"output2 행 수: {len(output2)}")

    if not output2:
        print("[FAIL] output2가 비어 있음 → 당일치만 반환하거나 TR_ID 불일치")
        print(f"전체 응답 키: {list(resp.keys())}")
        print(f"output1: {resp.get('output1', '없음')}")
        return False

    # 첫 행 필드 확인
    row0 = output2[0]
    print(f"\n첫 행 키: {list(row0.keys())}")
    print(f"첫 행 값: {json.dumps(row0, ensure_ascii=False, indent=2)}")

    # 날짜 필드 탐색 (bsop_date, stck_bsop_date 등 후보)
    date_keys = [k for k in row0 if 'date' in k.lower() or 'bsop' in k.lower()]
    frgn_keys = [k for k in row0 if 'frgn' in k.lower()]
    orgn_keys  = [k for k in row0 if 'orgn' in k.lower()]

    print(f"\n날짜 후보 필드: {date_keys}")
    print(f"외인 순매수 후보 필드: {frgn_keys}")
    print(f"기관 순매수 후보 필드: {orgn_keys}")

    has_multirow  = len(output2) > 1
    has_date      = bool(date_keys)
    has_flow      = bool(frgn_keys) and bool(orgn_keys)

    if has_multirow and has_date and has_flow:
        print(f"\n[PASS] 일별 시계열 {len(output2)}행, 날짜/수급 필드 확인됨")
        print(f"  → C3 구현 진행 가능")
        print(f"  → 날짜 필드명: {date_keys[0]}, 외인: {frgn_keys[0]}, 기관: {orgn_keys[0]}")
        return True
    else:
        reasons = []
        if not has_multirow: reasons.append("단일 행 또는 0행")
        if not has_date:     reasons.append("날짜 필드 없음")
        if not has_flow:     reasons.append("외인/기관 필드 없음")
        print(f"\n[FAIL] {', '.join(reasons)}")
        print("  → DEV_GUIDE_STRATEGY_A.md 0-3절 대안 TR 탐색 필요")
        return False


def check_historical_bars(kis):
    """검증 2: 일봉이 2022-01-01~까지 조회되는지"""
    _divider("검증 2 — 일봉 과거 데이터 가용성 (2022-01-01~)")

    bars = kis.get_historical_ohlcv("005930", "2022-01-01", "2022-01-14")
    print(f"2022-01-01~14 조회 결과: {len(bars)}개 (기대: 5~8개)")

    if bars:
        print(f"  가장 오래된 봉: {bars[-1].date}")
        print(f"  가장 최근 봉:   {bars[0].date}")

    if len(bars) >= 4:
        print("[PASS] 2022년 일봉 조회 가능")
        print("  → STEP G start_date = '2022-01-01' 사용 가능")
        return True
    else:
        print("[FAIL] 2022년 일봉 부족 (API 제공 범위 초과 가능성)")
        # 조회 가능한 최초 날짜 탐색
        print("\n  → 최근 5년치 시작일 탐색 중...")
        for year in [2023, 2024]:
            probe = kis.get_historical_ohlcv("005930", f"{year}-01-01", f"{year}-01-14")
            if len(probe) >= 4:
                print(f"  → {year}년부터 조회 가능: STEP G start_date = '{year}-01-01'로 조정")
                return False
        print("  → 2024년도 부족 — KIS API 제공 범위 재확인 필요")
        return False


def check_investor_flow_2022(kis):
    """검증 3: 수급 API가 2022년치를 반환하는지"""
    _divider("검증 3 — 수급 API 2022년치 가용성")

    resp = kis._get(
        "/uapi/domestic-stock/v1/quotations/inquire-investor",
        {
            "FID_COND_MRKT_DIV_CODE": "J",
            "FID_INPUT_ISCD":         "005930",
            "FID_INPUT_DATE_1":       "20220101",
            "FID_INPUT_DATE_2":       "20220131",
        },
        "FHKST01010900",
    )

    output2 = resp.get("output2", [])
    print(f"2022년 1월 수급 조회 결과: {len(output2)}행")

    if output2:
        print("[PASS] 2022년 수급 데이터 가용")
        return True
    else:
        print("[FAIL] 2022년 수급 없음 → C3 백테스트 기간을 가용 수급 시작일로 조정 필요")
        return False


def main():
    print("KIS 수급 API + 일봉 가용성 검증 스크립트")
    print("대상 종목: 005930 (삼성전자) — 검증용, 실제 유니버스 아님")

    kis = from_config()
    kis.authenticate()

    results = {}
    results["investor_flow_api"]  = check_investor_flow_api(kis)
    results["historical_bars"]    = check_historical_bars(kis)
    results["investor_flow_2022"] = check_investor_flow_2022(kis)

    _divider("종합 결과")
    all_pass = True
    for name, ok in results.items():
        status = "PASS" if ok else "FAIL"
        print(f"  {status}  {name}")
        if not ok:
            all_pass = False

    print()
    if all_pass:
        print("→ 모든 검증 통과. DEV_GUIDE_STRATEGY_A.md STEP A로 진행 가능.")
    else:
        print("→ 일부 실패. 위 결과에 따라 start_date 조정 또는 대안 TR 탐색 후 재실행.")


if __name__ == "__main__":
    main()
