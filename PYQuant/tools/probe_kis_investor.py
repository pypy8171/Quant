"""
KIS 투자자별 매매동향 TR 과거 데이터 깊이 검증 프로브.

목적: 수급(외인/기관 순매수) 백테스트가 가능한지 판단하려면, KIS
inquire-investor(FHKST01010900)가 '과거 며칠치'를 주는지 알아야 한다.
이 TR은 날짜범위 파라미터가 없어 최근 N거래일만 줄 가능성이 크다.
N이 ~30이면 2년 백테스트엔 부적합 → 다른 수급 데이터 소스 필요.

사용:  python tools/probe_kis_investor.py [종목코드]
출력:  rt_cd / 메시지 / 반환 행수 / 날짜범위(가장 오래된~최신) / 샘플 필드
주문 없음 — 조회(quotations) 전용, 안전.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 를 패키지 루트로

from kis.client import from_config

TICKER = sys.argv[1] if len(sys.argv) > 1 else "005930"  # 기본 삼성전자


def main() -> None:
    kis = from_config()                 # 기본 config.json (실계좌 인증정보)
    if not kis.authenticate():
        print("인증 실패 — config.json app_key/app_secret 확인")
        return

    # FHKST01010900 = 주식현재가 투자자 (일별 투자자 순매수). 날짜범위 파라미터 없음.
    data = kis._get(
        "/uapi/domestic-stock/v1/quotations/inquire-investor",
        {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": TICKER},
        "FHKST01010900",
    )

    print("=" * 60)
    print(f"종목: {TICKER}")
    print(f"rt_cd : {data.get('rt_cd')!r}  (0=정상)")
    print(f"msg1  : {data.get('msg1')!r}")

    rows = data.get("output", []) or data.get("output1", []) or []
    print(f"반환 행수: {len(rows)}")

    if not rows:
        print("\n[!] 빈 응답 — 응답 최상위 키:", list(data.keys()))
        print("    (output/output1 둘 다 비어있음)")
        return

    # 날짜 컬럼 자동 탐지 (KIS 필드명 버전차 대비)
    sample = rows[0]
    date_key = next((k for k in sample if "date" in k.lower() or k == "stck_bsop_date"), None)
    if date_key:
        dates = sorted(r.get(date_key, "") for r in rows if r.get(date_key))
        if dates:
            print(f"날짜범위: {dates[0]} ~ {dates[-1]}  ({len(dates)}일)")
            print(">>> 이 일수가 ~30 수준이면 2년 백테스트 불가 (다른 소스 필요)")
            print(">>> 500+ 수준이면 KIS 수급 백테스트 가능")
    else:
        print("[!] 날짜 컬럼을 못 찾음 — 아래 필드명 확인 필요")

    print("\n── 첫 행 전체 필드 (실제 필드명 확인용) ──")
    for k, v in sample.items():
        print(f"  {k:24s} = {v!r}")


if __name__ == "__main__":
    main()
