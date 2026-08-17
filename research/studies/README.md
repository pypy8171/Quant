# studies/ — 폴더형 백테스트 스터디

> [BACKTESTS.md](../BACKTESTS.md) 카탈로그 항목 중 **trade CSV가 남아 종목별 매매 원장까지 드릴다운되는** 스터디를 폴더로 승격한 곳.
> 각 폴더의 `README.md`는 질문·설정·발견 요약 + 런별 실현손익 표, `{run}.md`는 **어떤 종목을 언제 사고팔아 얼마 손익**이었는지 왕복 원장(FIFO 매칭).

## 계열 A — 종목레벨 모멘텀/레짐

| 스터디 | 무엇 | 런 | 원장 |
|---|---|---|---|
| [01_momentum_regime/](01_momentum_regime/README.md) | 모멘텀×국면필터 기준선 | 최근 1~5년 | 5런 |
| [02_vol_target/](02_vol_target/README.md) | 변동성 타게팅 사이징 | 최근 1~5년 | 5런 |
| [03_2022_ablation/](03_2022_ablation/README.md) | 2022 약세장 regime ON/OFF | ON·OFF | 2런 |
| [06_bear_market/](06_bear_market/README.md) | 하락장 6구간 이벤트 스터디 | 6구간×4전략 | [events/](06_bear_market/events/README.md) |

> 지표 전체(수익률·샤프·MDD·α)는 [BACKTEST_LOG](../BACKTEST_LOG.md) 실행 #1~#3(폴더 01~03), 6구간은 각 이벤트 README.
> BT-04/05는 trade CSV 없이 summary JSON만이라 폴더 미승격 — [카탈로그](../BACKTESTS.md) 카드로만(의도적 공백).

## 계열 B — 지수레벨 위기대응

> 계열 A와 **엔진·데이터가 다른 독립 계열**이다. 엔진 무관 단독 실행 스크립트로 yfinance 무키 **대표 지수**를 굴려 위기 국면의 익스포저 토글을 검증한다(개별종목·수급·survivorship 미반영, 룩어헤드 차단). 엣지 발견이 아니라 **인과적 스트레스테스트**로 읽는다.

| 스터디 | 무엇 | 한 줄 요지 |
|---|---|---|
| [07_crisis_regimes/](07_crisis_regimes/README.md) | 위기 17건 지수레벨 특성화 | speed(fast/slow)×shape(V/U/L) 거동 그리드로 분류 — 표본 n≈17·셀당 1~3개라 셀단위 우열비교는 무의미, hindsight 라벨은 신호 아님(기술통계만). |
| [08_crisis_response/](08_crisis_response/README.md) | 위기 인과적 대응 5종(M1~M5) | t-1 정보로만 익스포저 산출 → 종가-종가 구조상 **당일 급락 몸통은 못 막고 꼬리만** 자름. 낙폭이 아니라 전 구간(위기+정상+회복) 순효과로 평가. |
| [09_crisis_strategies/](09_crisis_strategies/README.md) | 위기 전략 10종(방어5+공세5) | 동일 규율로 확장·홀드아웃(2022) 잠금. Buy&Hold 대비 결정적 우위는 없고 진짜 유효 신호는 소수(O3 SOX 선행 등) — 순위 요약은 [SUMMARY_RANKING.md](09_crisis_strategies/SUMMARY_RANKING.md). |

> 04/05 공백은 계열 A와 동일하게 **의도적**(summary만 남아 폴더 미승격).

## 재생성
```bash
# 01~03 (raw/bt_*.csv → 각 스터디 폴더)
python3 render_studies.py
# 06_bear_market (raw/*_trades.csv → events/ 트리)
cd 06_bear_market && python3 render_ledger.py
```
`raw/`(원본 trade CSV)는 gitignore.

← [research 허브](../README.md) · [백테스트 카탈로그](../BACKTESTS.md)
