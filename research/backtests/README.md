# backtests/ — 폴더형 백테스트 스터디

> [BACKTESTS.md](../BACKTESTS.md) 카탈로그 항목 중 **trade CSV가 남아 종목별 매매 원장까지 드릴다운되는** 스터디를 폴더로 승격한 곳.
> 각 폴더의 `README.md`는 질문·설정·발견 요약 + 런별 실현손익 표, `{run}.md`는 **어떤 종목을 언제 사고팔아 얼마 손익**이었는지 왕복 원장(FIFO 매칭).

| 스터디 | 무엇 | 런 | 원장 |
|---|---|---|---|
| [01_momentum_regime/](01_momentum_regime/README.md) | 모멘텀×국면필터 기준선 | 최근 1~5년 | 5런 |
| [02_vol_target/](02_vol_target/README.md) | 변동성 타게팅 사이징 | 최근 1~5년 | 5런 |
| [03_2022_ablation/](03_2022_ablation/README.md) | 2022 약세장 regime ON/OFF | ON·OFF | 2런 |
| [06_bear_market/](06_bear_market/README.md) | 하락장 6구간 이벤트 스터디 | 6구간×4전략 | [events/](06_bear_market/events/README.md) |

> 지표 전체(수익률·샤프·MDD·α)는 [BACKTEST_LOG](../BACKTEST_LOG.md) 실행 #1~#3(폴더 01~03), 6구간은 각 이벤트 README.
> BT-04/05는 trade CSV 없이 summary JSON만이라 폴더 미승격 — 카탈로그 카드로만.

## 재생성
```bash
# 01~03 (raw/bt_*.csv → 각 스터디 폴더)
python3 render_studies.py
# 06_bear_market (raw/*_trades.csv → events/ 트리)
cd 06_bear_market && python3 render_ledger.py
```
`raw/`(원본 trade CSV)는 gitignore.

← [research 허브](../README.md) · [백테스트 카탈로그](../BACKTESTS.md)
