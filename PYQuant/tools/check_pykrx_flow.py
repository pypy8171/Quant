"""
STEP B-0 — pykrx 수급 데이터 검증

실행 방법 (PYQuant/ 디렉토리에서):
    pip install pykrx
    python -m tools.check_pykrx_flow

확인 항목:
  (a) 2022-01-01~까지 과거 외인/기관 일별 순매수 시계열이 오는가
  (b) 외국인 / 기관 순매수가 종목별·일별로 분리되어 오는가
  (c) 거래대금(원) 기준인가 수량(주) 기준인가

Pass 기준: (a)(b)(c) 모두 확인 → STEP F를 pykrx 기반으로 구현
Fail 시: C3를 거래량 급증 대체 (20일 평균 거래량 × N배)로 폴백
"""
import json


def _divider(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print('='*60)


def check_a_historical_range():
    """(a) 2022년치 과거 데이터 가용성"""
    _divider("(a) 2022년치 과거 데이터 가용성")
    from pykrx import stock

    df = stock.get_market_trading_value_by_date("20220103", "20220114", "005930")
    print(f"2022-01-03~14 조회 행수: {len(df)}")
    print(f"컬럼: {list(df.columns)}")
    if not df.empty:
        print(df.head(3).to_string())
        return True, df
    else:
        print("[FAIL] 빈 DataFrame")
        return False, df


def check_b_per_stock_daily(df_sample):
    """(b) 외국인/기관 종목별·일별 분리 여부"""
    _divider("(b) 외국인/기관 컬럼 분리 여부")
    cols = list(df_sample.columns) if df_sample is not None else []

    # pykrx 컬럼명 후보: 기관합계, 외국인합계, 외국인, 기관, 개인 등
    frgn_candidates = [c for c in cols if '외국인' in c or 'Foreign' in c.lower()]
    orgn_candidates = [c for c in cols if '기관' in c or 'Institution' in c.lower()]

    print(f"외국인 후보 컬럼: {frgn_candidates}")
    print(f"기관 후보 컬럼: {orgn_candidates}")

    if frgn_candidates and orgn_candidates:
        frgn_col = frgn_candidates[0]
        orgn_col  = orgn_candidates[0]
        print(f"\n사용할 컬럼: 외국인={frgn_col}, 기관={orgn_col}")
        if df_sample is not None and not df_sample.empty:
            print(df_sample[[frgn_col, orgn_col]].head(5).to_string())
        return True, frgn_col, orgn_col
    else:
        print("[FAIL] 외국인 또는 기관 컬럼 없음")
        return False, None, None


def check_c_value_or_volume(df_sample, frgn_col):
    """(c) 거래대금(원) 기준인가 수량(주) 기준인가"""
    _divider("(c) 단위 확인 (거래대금 vs 수량)")
    from pykrx import stock

    # get_market_trading_volume_by_date: 수량 기준
    df_vol = stock.get_market_trading_volume_by_date("20220103", "20220107", "005930")

    if df_sample is not None and not df_sample.empty and frgn_col:
        val_sample = abs(df_sample[frgn_col].iloc[0]) if frgn_col in df_sample.columns else 0
        print(f"get_market_trading_value_by_date 외국인 첫행 절댓값: {val_sample:,.0f}")
        if val_sample > 1_000_000:
            print("  → 거래대금(원) 기준으로 판단 (값이 큼)")
        else:
            print("  → 수량(주) 기준으로 판단 (값이 작음)")

    if not df_vol.empty:
        vol_cols = [c for c in df_vol.columns if '외국인' in c or 'Foreign' in c.lower()]
        if vol_cols:
            vol_sample = abs(df_vol[vol_cols[0]].iloc[0])
            print(f"get_market_trading_volume_by_date 외국인 첫행 절댓값: {vol_sample:,.0f}")

    print("\n권장: C3 판정은 수량(주) 기준 사용 — 주가 수준 무관하게 종목 간 비교 가능")
    print("  → get_market_trading_volume_by_date 사용 예정")
    return True


def check_multi_ticker():
    """종목 2개로 일별·종목별 독립 조회 가능한지 확인"""
    _divider("(추가) 유니버스 종목 샘플 조회 — 005930 / 000660")
    from pykrx import stock

    for ticker, name in [("005930", "삼성전자"), ("000660", "SK하이닉스")]:
        df = stock.get_market_trading_volume_by_date("20220103", "20220107", ticker)
        cols = [c for c in df.columns if '외국인' in c or '기관' in c]
        if not df.empty and cols:
            row = df[cols].iloc[0]
            print(f"  {ticker} {name}: 외국인={row.iloc[0]:+,.0f}주, 기관={row.iloc[1]:+,.0f}주 (수량)")
        else:
            print(f"  {ticker} {name}: 조회 실패 또는 컬럼 없음")


def main():
    print("pykrx 수급 데이터 검증 스크립트")
    print("대상: 005930 삼성전자 (검증용)")

    try:
        import pykrx
    except ImportError:
        print("\n[ERROR] pykrx 미설치. 먼저 실행: pip install pykrx")
        return

    results = {}

    ok_a, df_sample = check_a_historical_range()
    results["(a) 2022년치 과거 데이터"] = ok_a

    ok_b, frgn_col, orgn_col = check_b_per_stock_daily(df_sample if ok_a else None)
    results["(b) 외국인/기관 컬럼 분리"] = ok_b

    ok_c = check_c_value_or_volume(df_sample if ok_a else None, frgn_col)
    results["(c) 단위 확인"] = ok_c

    if ok_a and ok_b:
        check_multi_ticker()

    _divider("종합 결과")
    all_pass = True
    for name, ok in results.items():
        status = "PASS" if ok else "FAIL"
        print(f"  {status}  {name}")
        if not ok:
            all_pass = False

    print()
    if all_pass:
        print("→ PASS: STEP F를 pykrx 기반으로 구현 가능.")
        if frgn_col:
            print(f"  외국인 컬럼명: '{frgn_col}', 기관 컬럼명: '{orgn_col}'")
        print("  수량 기준: get_market_trading_volume_by_date 사용 예정")
    else:
        print("→ FAIL: C3를 거래량 급증 대체 (20일 평균 × N배)로 폴백.")


if __name__ == "__main__":
    main()
