"""
data.go.kr 가격이 '수정주가'인지 '원주가'인지 검증.

원리: 한국 주식 가격제한폭은 ±30%다. 따라서 정상 거래로는 시가가 전일종가 대비
±30%를 넘을 수 없다. 그 이상의 '하룻밤 갭'이 있으면 = 액면분할/권리락/병합 등
기업행위(corporate action)이며, 데이터가 **원주가(비수정)** 라는 뜻이다.
수정주가라면 이런 갭이 사라진다(과거가 비율 보정됨).

→ 갭이 발견되면 C-1(수정주가 미처리)이 실재 → 모멘텀 점수 오염 → 보정 필요.
→ 갭이 없으면 data.go.kr이 이미 수정주가 → C-1 기우, 추가 작업 불필요.

사용 (키 환경변수):
  export DATA_GO_KR_KEY='...'
  python3 PYQuant/tools/probe_adjusted.py            # 기본 종목셋
  python3 PYQuant/tools/probe_adjusted.py 005930 035720 247540   # 종목 지정
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from data.datagokr_source import DataGoKrSource

GAP_THRESHOLD = 0.35   # 전일종가 대비 시가 갭 35%↑ → 기업행위(가격제한 ±30% 초과)
START, END = "2020-01-01", "2026-06-10"

# 기본 검사 종목 — 분할/권리락 이력 가능성 있는 대형·중형 + KOSDAQ 변동주
DEFAULT_TICKERS = ["005930", "035720", "035420", "000660", "207940",
                   "247540", "086520", "196170", "112040", "028300"]


def main() -> None:
    tickers = sys.argv[1:] or DEFAULT_TICKERS
    src = DataGoKrSource(market="KOSPI")
    if not src.authenticate():
        print("DATA_GO_KR_KEY 환경변수 필요. 중단.")
        return

    print(f"검사: {START}~{END}, 갭 임계 {GAP_THRESHOLD:.0%} (가격제한 ±30% 초과 = 기업행위)")
    print("=" * 70)
    total_gaps = 0
    for tk in tickers:
        bars = src.get_historical_ohlcv(tk, START, END)
        name = src.ticker_name(tk) or tk
        gaps = []
        for i in range(1, len(bars)):
            pc = bars[i - 1].close
            op = bars[i].open
            if pc > 0 and op > 0:
                gap = op / pc - 1.0
                if abs(gap) >= GAP_THRESHOLD:
                    gaps.append((bars[i].date, pc, op, gap))
        total_gaps += len(gaps)
        flag = f"⚠ 갭 {len(gaps)}건 (원주가 의심)" if gaps else "OK (갭 없음)"
        print(f"  {tk} {name:<16} 봉 {len(bars):>4}  {flag}")
        for d, pc, op, g in gaps[:5]:
            print(f"      {d}  전일종가 {pc:,.0f} → 시가 {op:,.0f}  ({g:+.1%})")

    print("=" * 70)
    if total_gaps:
        print(f"❌ 총 {total_gaps}건 갭 발견 → data.go.kr은 원주가(비수정). "
              "C-1 실재 → 수정주가 보정 필요.")
    else:
        print("✅ 갭 0건 → data.go.kr이 이미 수정주가일 가능성 높음. C-1 기우 — 추가작업 불필요.")
    print("   (단, 검사 종목에 기간 내 분할이 없었을 수도 있으니 종목 더 넣어 재확인 권장)")


if __name__ == "__main__":
    main()
