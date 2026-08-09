# 하락장 백테스트 — 이벤트별 매매 원장

구간(이벤트)별로 폴더를 나눴다. 각 폴더의 `README.md`는 그 구간 4런(모멘텀/역추세 × regime ON/OFF)의
지표 요약이고, `{전략}_{regime}.md`는 **어떤 종목을 언제 사고팔아 얼마 손익**이었는지 왕복 원장이다.
가격은 KRW. 2011/2018/2020은 수정주가(yfinance).

## 1. [2011 유럽재정위기](2011euro/README.md)  ·  _yfinance_

그리스發 유로존 위기로 완만·장기 하락. 8월 미국 신용강등 급락 포함.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | 24.10% | 1.17 | [열기](2011euro/momentum_on.md) |
| 모멘텀 OFF | 11.51% | 0.54 | [열기](2011euro/momentum_off.md) |
| 역추세 ON | -5.67% | -0.28 | [열기](2011euro/mean_reversion_on.md) |
| 역추세 OFF | -2.87% | 0.12 | [열기](2011euro/mean_reversion_off.md) |

## 2. [2018 반도체·미중무역](2018semi/README.md)  ·  _yfinance_

미중 무역전쟁 + 반도체 고점 논쟁. 방향이 자주 뒤집힌 박스+급락장.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | -5.63% | -0.38 | [열기](2018semi/momentum_on.md) |
| 모멘텀 OFF | -22.13% | -0.90 | [열기](2018semi/momentum_off.md) |
| 역추세 ON | -6.42% | -0.63 | [열기](2018semi/mean_reversion_on.md) |
| 역추세 OFF | 10.94% | 0.61 | [열기](2018semi/mean_reversion_off.md) |

## 3. [2020 COVID 팬데믹](2020covid/README.md)  ·  _yfinance_

3월 코로나 급락 후 유동성 장세로 V자 반등. 연말 기준 플러스.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | 13.68% | 0.82 | [열기](2020covid/momentum_on.md) |
| 모멘텀 OFF | 58.00% | 1.48 | [열기](2020covid/momentum_off.md) |
| 역추세 ON | 5.52% | 0.43 | [열기](2020covid/mean_reversion_on.md) |
| 역추세 OFF | 36.39% | 0.98 | [열기](2020covid/mean_reversion_off.md) |

## 4. [2022 금리인상 약세장](2022bear/README.md)  ·  _datagokr(PIT)_

연준 급격한 긴축으로 연중 내내 순수 하락 추세.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | 0.00% | 0.00 | [열기](2022bear/momentum_on.md) |
| 모멘텀 OFF | -24.34% | -1.25 | [열기](2022bear/momentum_off.md) |
| 역추세 ON | 0.00% | 0.00 | [열기](2022bear/mean_reversion_on.md) |
| 역추세 OFF | -11.89% | -0.29 | [열기](2022bear/mean_reversion_off.md) |

## 5. [2024 블랙먼데이](2024blackmon/README.md)  ·  _datagokr(PIT)_

8월 엔캐리 청산發 급락 후 회복.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | 9.78% | 0.47 | [열기](2024blackmon/momentum_on.md) |
| 모멘텀 OFF | 36.66% | 1.19 | [열기](2024blackmon/momentum_off.md) |
| 역추세 ON | -21.99% | -0.96 | [열기](2024blackmon/mean_reversion_on.md) |
| 역추세 OFF | -17.49% | -0.60 | [열기](2024blackmon/mean_reversion_off.md) |

## 6. [2026 현재 변동장](2026now/README.md)  ·  _datagokr(PIT)_

AI·반도체 테마 랠리와 급락이 공존한 고변동 국면.

| 전략·regime | 총수익 | 샤프 | 원장 |
|---|---:|---:|---|
| 모멘텀 ON | 99.94% | 2.30 | [열기](2026now/momentum_on.md) |
| 모멘텀 OFF | 37.10% | 1.09 | [열기](2026now/momentum_off.md) |
| 역추세 ON | 12.17% | 0.81 | [열기](2026now/mean_reversion_on.md) |
| 역추세 OFF | 2.30% | 0.30 | [열기](2026now/mean_reversion_off.md) |

← [백테스트 종합](../README.md)