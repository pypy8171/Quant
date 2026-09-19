# 편향 감사 2라운드 — 백테스트 구조와 장중 전략 판정 기준 (bias-auditor, 2026-09-20)

판정: **기각**. 지금 트리의 백테스트 숫자(스터디 11·13·15·17, `PYQuant/tools/walkforward.py` 산출)는 어느 것도 결론으로 승격하지 않는다.
근거 넷. 2022 홀드아웃을 스터디 15개 중 10개가 평가에 썼다(다중 사용). 비용 상수가 아직 네 갈래다(0.31 / 0.0018 / 0.0021 / 0.215가 각 스터디에 남아 있다).
일봉 패널의 상폐 포함이 연도별로 고르지 않아 시점 고정 등급 B다. 장중 리플레이는 라이브와 체결 건수가 매수 1.5배·매도 3.2배 어긋난 채 성과를 냈다.
승격된 결과가 없어서 부풀린 숫자가 운용에 들어간 흔적은 없다. 이 문서는 그 상태를 그대로 두지 않기 위한 수정 순서다.

읽은 것: `research/COUNCIL_CHARTER.md`, `research/RESET_2026-09-19.md` §1-2·§1-3·§B·§3, `research/GUARDRAILS.md`, `research/BACKTESTS.md`, `research/BACKTEST_FLOW.md`,
`PYQuant/backtest/engine.py`·`ledger.py`·`metrics.py`·`costs.py`·`devscale_replay.py`, `PYQuant/tools/walkforward.py`, `PYQuant/data/datagokr_source.py`,
`Quant/include/core/PaperExecutor.h`, 스터디 11·12·13·15·17의 실행 스크립트와 README, `scripts/exit_ev.py`, `scripts/check_backtest.py`.
데이터는 `PYQuant/data/bars_all_pit.parquet`를 직접 읽어 셌다.

## 0. 과거 함정 대조

| 과거 함정 | 이번에 재발? | 근거 |
|---|---|---|
| 홀드아웃 2022 재사용(리셋 §1-3) | 재발 | `research/studies/03·08·09·10·11·12·13·14·15·16`이 2022를 홀드아웃 또는 평가 구간으로 쓴다(README·스크립트 grep, 10/15). 스터디 13은 24셀 전부에 2022 행이 있다(`research/studies/13_trendx_gate/run_trendx_gate.py:282`) |
| 비용 정의 네 곳(리셋 §1-3) | 부분 재발 | `costs.LIVE`가 생겼지만 스터디 13·11·15·17은 자기 상수를 쓴다(§1 결함 2) |
| SELL 손익이 마지막 매수가 기준(리셋 §1-3) | 해소 | `PYQuant/backtest/engine.py:420` `PositionLedger.on_fill`로 평단 계산, `PYQuant/tests/test_costs_golden.py` 10케이스 |
| 벤치 비용 0·상폐 전방채움(리셋 §1-3) | 절반 해소 | `engine.py:344` 매수 비용 반영, `engine.py:355` 상폐 0원. 전략 쪽은 마지막 종가로 청산해 비대칭(§1 결함 3) |
| O(일수²) 전수 스캔(리셋 §1-3) | 해소 | `engine.py:250-263` 커서 |
| D-064 게이트 거래가중·풀 일가중 혼합 | 해소 | `run_trendx_gate.py:181-186` 일 정합 |
| 사전등록이 산출물보다 늦음 | 재발 | 스터디 17 README §1 "회의 자리에서 기술통계 일부를 먼저 보았다". 스펙 해시·커밋 선행을 확인하는 장치가 없다(`scripts/check_backtest.py`는 07·08·09 결정론만 본다) |
| 이벤트 윈도우 밖 복리(09 감사) | 해당 없음 | 이번 대상은 종목 패널이라 같은 구조가 없다 |

## 1. 결함 목록 (반증 목표)

방향은 "결과를 어느 쪽으로 밀었나"다. 숫자가 없는 칸은 추정이라고 적었다.

| # | 결함 | 위치 | 방향과 규모 |
|---|---|---|---|
| 1 | 2022 홀드아웃 다중 사용 | `research/studies/13_trendx_gate/run_trendx_gate.py:49-53` HOLDOUT 상수를 24셀 전부에 적용, `research/studies/15_impulse_pullback/run_impulse_pullback.py:48`, `research/studies/11_signal_axes/run_channel_breakout.py:176`, 그 외 7개 스터디 README | 통과 확률 과대. 스터디 10개 × 평균 6셀이면 2022 한 해를 60번 넘게 봤다. 명목 5%로도 우연 통과 1개 이상이 나올 확률이 95%를 넘는다(추정). 이번엔 통과가 0이라 실해는 없었지만 앞으로 2022 단독 성과는 승격 근거가 될 수 없다 |
| 2 | 비용 상수가 한 벌이 아니다 | `run_trendx_gate.py:58` COST_PCT=0.31 / `run_channel_breakout.py:56` 세율 0.0018 하드코딩(`run_cross_momentum.py`·`run_mean_reversion.py` 같음) / `run_impulse_pullback.py:44` 0.0021 / `scripts/exit_ev.py:40` `LIVE.sell_cost_rate`(0.215%)로 세율 0.18% 시절 원장을 역산 | 13은 왕복 0.08%p 과소(보수적), 15는 0.02%p 과대, 17은 평단을 명목의 0.02% 낮게 잡아 수익률 0.02%p 과대. 크기는 작지만 `PYQuant/backtest/costs.py:119-123` 주석이 요구한 `LEDGER_UNTIL_2026_09_21`을 17이 안 쓴다 — 골든 테스트가 그 상수로 통과했으니 스크립트만 틀렸다 |
| 3 | 상폐·자료 종료 처리 비대칭 | `engine.py:396-399` 전략 SELL은 마지막 종가로 강제 청산 / `engine.py:355` 벤치는 0원 / `run_trendx_gate.py:152` trunc도 마지막 종가 | 전략 유리. 스터디 11 C3에서 강제 청산 4건(100종목 풀), 슬롯 1/10이면 초과수익 상한 +4%p 안쪽(추정). 자료 종료가 상폐가 아니라 캐시 절단·합병일 때는 반대로 벤치가 손해(0원). 13은 풀·게이트가 같은 규칙이라 상대 비교엔 영향이 작다 |
| 4 | 유니버스의 시점 고정 등급 | `PYQuant/data/bars_all_pit.parquet`: 3,081종목, delisted 340, 자료가 끝난 종목 464. 연도별 종료 2019: 8 / 2020: 22 / 2021: 38 / 2022: 40 / 2023: 53 / 2024: 64 / 2025: 101 / 2026: 138 | 2019~2021 상폐가 실제(KIND 기준 연 60~90건, 추정)보다 훨씬 적다. 초기 연도일수록 생존 종목 위주라 하락 구간 손실을 덜 본다. 등급 B. 스터디 12 `raw/universe.parquet`는 2,685종 전부 09-09 생존이라 등급 C. `PYQuant/data/datagokr_source.py:339-357` 단면은 시작일 하나로 4년을 고정한다(`run_channel_breakout.py:152`) — 편입 종목이 빠져 벤치와 전략 모두 누락, 방향 불명 |
| 5 | 거래정지 종목의 현금·평가 시점 | `engine.py:392-395` 다음 봉이 몇 달 뒤여도 `Trade.date`는 신호일이고 현금이 그날 들어온다. `engine.py:299-303` 봉이 없는 날은 보유 평가 0 | 현금이 이르게 들어와 재투자 수익이 앞당겨진다(전략 유리, 규모 작음). 평가 0은 최대낙폭(MDD)을 과대(전략 불리). 두 방향이 섞여 부호는 모르지만 곡선이 흔들린다 |
| 6 | 사이징이 다음 봉 시가를 본다 | `engine.py:489-492` `_peek_next_open`으로 수량 계산 | 수익률 영향은 0에 가깝지만 라이브(종가 t 기준 수량)와 다르다. 재현 가능성 문제 |
| 7 | 유동성·참여율 상한 없음, 슬리피지 0 | `engine.py` 전체, `run_trendx_gate.py`, `run_impulse_pullback.py`. `costs.LIVE` slippage_ticks=0·impact 0 | 라이브 슬롯 20 × 400만원 = 8천만원. 스터디 15 하한(20일 평균 거래대금 5억)에 걸친 종목이면 하루 거래대금의 16%다. 원장은 비신호 매도가 건당 −5,899원(리셋 §1-2)으로 시장가 비용을 이미 내고 있다. 백테스트가 이 비용을 0으로 두면 부호가 바뀌는 셀이 생긴다(추정, 3단 감도 필요) |
| 8 | 리플레이 체결 규칙이 낙관적 | `PYQuant/backtest/devscale_replay.py:255-263` 지정가는 저가·고가가 닿으면 체결(대기열 없음), 부분체결 없음, 같은 봉에서 매수·매도 양쪽 체결. `Quant/include/core/PaperExecutor.h:139-168` 틱 `crosses`도 대기열 없음 | 분할 매수·익절형(TRENDX·DEVSCALE)에 유리. 라이브 대조(스터디 13 README "라이브 대조")에서 매수 1.5배·매도 3.2배. 이 상태의 리플레이 성과는 방향 관찰도 어렵다 |
| 9 | 표본과 유효 표본 | `PYQuant/backtest/metrics.py:141` n_eff = min(n, 날짜 수)/max_hold(하한). 13 홀드아웃 월 12개, 17 거래일 9, 12 사건 10 | 부풀림은 아니고 반대로 판정 불능이다. 월 12개로 t≥2를 넘으려면 월 초과 0.6R(σ 1R 가정) 이상이어야 한다 — 어떤 게이트도 그만큼 강하지 않다. 표본 설계 없이 돌린 것 자체가 결함이다 |
| 10 | 파라미터가 검증 창을 봤는가 | 11: 단일 사전등록(양호). 13: 12셀 격자 × 익절 2 = 24셀, 벤저미니-호흐버그 보정 q(양호). 12: KOSPI 임계 2회, 암묵 자유도 12 이상(README 자인). 15: 밴드 3. `PYQuant/tools/walkforward.py:33-38` 확장창 4폴드는 좋지만 기본값(top30·rb20·lb120·skip20)이 2020~2026 전 구간 스윕에서 왔는지 확인 불가 | 12는 이미 기각. walkforward 기본값은 "확인 불가"로 남긴다 — `PYQuant/tools/sweep.py` 실행 이력과 기본값 확정일이 없다 |
| 11 | 엔진 골든 회귀 부재 | `PYQuant/tests/test_backtest_engine.py` 2케이스, `scripts/check_backtest.py`는 07·08·09만. 09-19 엔진 수술(원장·커서·상폐 0원) 뒤 스터디 11 재생성 여부 미확인 — git status에 `research/studies/11_signal_axes/metrics.json` 수정본이 미커밋 | README 표(커밋 d6df019 각인)와 지금 엔진이 내는 숫자가 다를 수 있다. 어느 쪽이 정본인지 지금은 모른다 |
| 12 | 사전등록 증거 없음 | 스펙 파일·해시·커밋 선행이 없다. 스터디 README §1이 유일한 흔적 | 사후 조정을 감사할 수 없다. 17 README가 솔직히 적었듯 이미 한 번 순서가 뒤집혔다 |
| 13 | 벤치 정의 | `engine.py:326-359` 시작 단면 동일가중 매수 후 보유(리밸런싱 없음, 매수 비용만) | 전략은 매달 비용을 내고 벤치는 안 내니 초과수익이 보수적(전략 불리). 반대로 2021~2024 벤치 −11%(스터디 11) 구간에선 초과 부호가 국면에 지배된다 — 벤치를 KODEX200과 동일가중 둘 다 두고 월 초과로 검정해야 한다 |

편향 판정표(감사 정본 형식):

- ① Look-ahead: 없음 — `PYQuant/strategy/channel_breakout.py:36` 신호봉 제외, `run_impulse_pullback.py:94` `vol_ma20.shift(1)`, `run_trendx_gate.py:120-124` 진입 `o[1:]`, 전역 통계 없음. 잔여는 결함 5·6(현금 시점·사이징)과 `datagokr_source.py:182-187` 분할 감지가 갭 뒤 10거래일 주식수를 보는 것 — 수익률 영향 없음, 가격 하한 필터(`MIN_CLOSE`)만 조정가 기준(규모 작음).
- ② Survivorship: 있음 — 결함 3·4.
- ③ Hindsight/선택: 있음 — 결함 1·10·12.
- ④ 과최적화/재적합: 확인 불가 — 결함 10(walkforward 기본값 출처).
- ⑤ 표본부족: 있음 — 결함 9.
- ⑥ 체결현실성/비용: 있음 — 결함 2·7·8.

## 2. 버릴 것 / 고칠 것 / 새로 지을 것

추천: **엔진은 새로 짓고 `PYQuant/backtest/engine.py`는 골든 대조용으로 동결한다.** 이유 셋. 첫째, 결함 3·5·7은 종목별 `list[Bar]`와 날짜 루프라는 뼈대에서 나온 것이라 패치로는 참여율·상폐 종결가·정지 처리가 계속 예외 분기로 쌓인다. 둘째, 리셋 §1-3 실측대로 3,000종목 × 7년이 7분이면 walk-forward 5창 × 격자 50셀은 하루 일이다 — 판정 기준 ②·③을 돌릴 수 없는 엔진은 기준을 못 만든다. 셋째, `costs.py`·`ledger.py`는 이미 라이브와 0원 골든이 있으니 그 둘만 가져가면 새 엔진의 비용 정합은 처음부터 확보된다.

| 버릴 것 | 고칠 것 | 새로 지을 것 |
|---|---|---|
| 2022 단일 홀드아웃 규칙(`research/GUARDRAILS.md` ④ "홀드아웃 2022 잠금") — walk-forward 5창으로 교체 | `scripts/exit_ev.py:40` 세율 상수를 `LEDGER_UNTIL_2026_09_21`로(09-21까지 원장), 그 뒤 날짜별 분기 | `PYQuant/backtest/panel.py` 와이드 패널 빌더(리셋 §B-3 그대로) |
| `engine.py:15` `CostModel` 호환 래퍼와 스터디 11 `cost()`(세율 0.0018) | `engine.py:396-399` 상폐 종결가 규칙을 벤치와 같게(종결가 없으면 0원) | `PYQuant/backtest/sim.py` 벡터 체결기 — 아래 설계 |
| `run_trendx_gate.py:107-162` `simulate_code`, `metrics.py:52-118` `simulate` (엔진 셋을 하나로) | `PYQuant/data/bars_all_pit_v2.parquet` 상폐 커버를 KIND 목록과 대조해 채움(리셋 §3 데이터 행 90% 이상) | `PYQuant/backtest/run_spec.py` 사전등록 실행기(스펙 해시·산출 해시) |
| 스터디 12 `raw/universe.parquet`(등급 C, 결론 근거로 재사용 금지) | `devscale_replay.py:255-263` 체결 규칙(지정가는 1틱 지나쳐야, 참여율 상한) | `PYQuant/data/universe/membership.parquet` 발효일 있는 멤버십 |

### 최소 설계 — `PYQuant/backtest/sim.py`

입력 parquet(전부 `PYQuant/data/cache/wide/`, Date × code_id, float32, code_id는 정수):

| 파일 | 내용 |
|---|---|
| `open/high/low/close/volume/turnover.parquet` | 수정주가, 거래정지일은 NaN(0 아님) |
| `../universe/membership.parquet` | `code_id, effective_from, effective_to, market, end_reason(listed/delisted/merged), terminal_price` — 시점 고정 등급 A의 정의 그대로 |
| `../features/<name>.parquet` | `code_id, effective_date, published_at, value, grade` — 조인은 published_at이 t 이전인 값만 |
| `../calendar.parquet` | 거래일, 동시호가 규칙 변경일 |

이벤트 순서(날짜 t 하나에 대해):

1. t 종가 확정 뒤 피처·신호 계산. 수급·재무는 published_at이 t 이전인 값만.
2. 유니버스 = effective_from 이후, effective_to 이전인 종목. 시작 단면 고정을 쓰지 않는다.
3. 주문 목록 = 목표 − 보유. 수량은 close[t]로 계산(라이브와 같다).
4. t+1 시가 체결. 가격 = open[t+1]에 slippage_ticks(기본 1) 불리하게, 수량 상한 = 참여율(기본 5%) × turnover20[t] / open[t+1]. 남은 수량은 취소(이월 여부는 스펙에 적는다). open[t+1]이 NaN(정지)이면 주문 보류.
5. effective_to가 t+1인 종목은 terminal_price로 강제 청산, 없으면 0원. 벤치도 같은 규칙.
6. 비용은 `costs.LIVE`에 슬리피지 3단(0·1·2틱)을 얹은 세 벌. 원장은 `ledger.PositionLedger`.
7. 평가는 close[t+1], 정지일은 마지막 종가 유지(0 아님).

결과 스키마 `metrics.json` 한 벌:

```
spec_sha256, spec_commit, data_fingerprint(파일별 mtime+행수), git_commit, run_utc
grade: A|B|C (시점 고정 등급)
windows: [{train, test, params, n_months, n_roundtrips, n_stock_months,
           excess_monthly_mean, excess_t, sharpe, mdd, bench_mdd, turnover_yr}]
robustness: {neighbors: [{params, excess_t}], cost_ladder: {0.23, 0.35, 0.55}, slippage_ticks: {0, 1, 2}}
verdict: {layer1, layer2, layer3, final}
```

사전등록 절차: ① `research/studies/NN/spec.yaml`(가설·유니버스·신호·파라미터와 격자·창·비용·임계·표본 계산)을 먼저 커밋한다. ② `py PYQuant/backtest/run_spec.py research/studies/NN/spec.yaml`이 스펙의 커밋 blob 해시와 작업 트리 해시가 다르면 거부한다. ③ `metrics.json`에 스펙 해시·데이터 지문·git 커밋을 적고, 같은 입력으로 두 번 돌려 sha256이 같아야 산출물로 인정한다. ④ 스펙을 고치면 새 번호(NN-2)로 다시 등록하고 이전 결과는 남긴다.

## 3. 판정 기준 (숫자)

헌장 3절 세 층을 이 저장소에 맞게 적는다. 세 층 모두 통과해야 채택이고, 시점 고정 등급이 B면 "조건부(슬리브 5종목·100왕복)", C면 "방향 관찰"까지만이다.

| 층 | 기준 | 최소 표본 |
|---|---|---|
| ① 부호와 t | 비용 후(LIVE + 슬리피지 1틱) 월 초과수익(벤치: 같은 유니버스 동일가중 매수 후 보유, KODEX200 둘 다) 1표본 t 2.0 이상. 격자 20셀 이상이면 t 3.0 이상 또는 벤저미니-호흐버그 보정 q 0.1 미만 | 검증 창 합계 월 36 이상, 왕복 300 이상, 종목×월 6,000 이상(예: 250종목 × 24개월). 이벤트형은 사건 30 이상, 유효 일 블록 30 이상 |
| ② walk-forward | 창 5개 고정: 2019-21→22, 2020-22→23, 2021-23→24, 2022-24→25, 2023-25→26(9월까지). 학습 창에서 고른 파라미터를 검증 창에 한 번만 적용. 부호 유지 3/5 이상, 이어붙인 검증 곡선 t 2.0 이상, 최악 창 초과 −5%p 이상, 검증 창 최대낙폭이 벤치 + 5%p 이하 | 창마다 월 12, 왕복 60 이상 |
| ③ 강건성 | 각 파라미터 축 ±1 이웃 전부 초과 양수이고 중심 셀 Sharpe의 70% 이상. 비용 0.23/0.35/0.55% 중 0.35%까지 양수. 슬리피지 2틱에서도 부호 유지. Deflated Sharpe p 0.05 미만(시도 셀 수 전체를 분모로) | — |

2022는 다섯 창 중 하나로만 들어간다. 2022 단독 성과를 문장에 쓸 때는 "5창 중 2022 창"이라고 적고 단독 승격 근거로 쓰지 않는다.

### 장중 전략(DEVIATION_SCALE·ITB)을 리플레이로 판정할 때

리플레이 성과를 읽기 전에 패리티 게이트를 먼저 통과해야 한다. 지금은 통과하지 못했다(매수 1.5배·매도 3.2배).

| 단계 | 기준 |
|---|---|
| 패리티(같은 날 라이브 원장 대조, 겹치는 거래일 5 이상) | 종목·일별 매수 건수 비 0.8~1.2, 매도 건수 비 0.8~1.2, 실현손익 부호 일치 90% 이상, 체결가 차이 중앙값 1틱 이하, 이월 포지션을 전날 잔고로 시드해 대조 |
| 체결 규칙 | 지정가는 가격이 지정가를 1틱 지나쳐야 체결(닿기만 하면 미체결), 또는 그 봉 거래량이 주문량 × 2 이상. 시장가는 다음 틱(봉 리플레이면 다음 봉 시가)에 1틱 불리. 봉 하나에서 같은 종목 매수·매도 양쪽 체결 금지(경로 불명). 참여율은 봉 거래량 10% 이하 |
| 성과 | 비용 후 왕복당 +10bp 이상, 일 블록 부트스트랩 95% CI 하한 양수, 거래일 40 이상, 왕복 300 이상, 비용/비용전 0.5 이하, 비신호 청산 30% 이하. 파라미터 변경 전후 구간(스터디 17의 A·B) 둘 다 부호 양수 |
| 킬 | 20거래일 뒤 CI 상한이 음수면 슬리브 정지. 재등록은 새 스펙 번호로 |
| 표본 계산 | 스펙에 N = (2.5·sd/μ)²를 원장 sd로 미리 적는다. 리셋 §1-2 수치(비용 후 −1,949원/건, t 1.23)로 역산하면 왕복 약 2,300 — 현재 슬롯·회전으로 80거래일 |

## 4. 첫 실행 단계

| 순서 | 누가 | 파일·명령 | 완료 판정 |
|---|---|---|---|
| 1 (오늘) | bias-auditor | `scripts/exit_ev.py:40` 상수를 `LEDGER_UNTIL_2026_09_21.sell_cost_rate`로, `py scripts/exit_ev.py` 재실행 | 총 손익 −1,155,413원 유지, 평단 미상 13행 유지, 수익률 열만 바뀜 |
| 2 (오늘) | backtest-runner | 스터디 11 세 스크립트를 지금 엔진으로 재실행해 `metrics.json`을 다시 만들고 커밋(README 표와 차이를 §8에 적는다) | 미커밋 `metrics.json` 해소, 차이 목록 |
| 3 (주말) | data-sourcer | KIND 상장폐지 목록과 `PYQuant/data/bars_all_pit_v2.parquet`의 delisted 대조, `PYQuant/data/universe/membership.parquet` 생성 | 2019~2021 상폐 커버 90% 이상, terminal_price 결측률 보고 |
| 4 (W1) | backtest-runner | `py PYQuant/backtest/panel.py --build` | 스터디 13 주검정 셀(`results.tsv` main=True, all4) 소수 4자리 재현, 50셀 60초 이하 |
| 5 (W1) | backtest-runner | `PYQuant/backtest/sim.py` + `run_spec.py`. 골든 셋: (a) 스터디 11 C3를 `engine.py`와 `sim.py`로 각각 돌려 총수익 ±0.5%p (b) 무작위 신호 1,000회 순위상관(IC) 0 ± 0.01 (c) 두 번 실행 sha256 동일 | 셋 다 통과 |
| 6 (W1) | bias-auditor | `research/studies/18_factor_harness/spec.yaml` 검토·커밋 뒤 실행, §3 기준으로 감사 | 첫 감사표 |
| 7 (09-22~26) | strategist·backtest-runner | capture_dir 틱 5일 → ReplaySource + PaperExecutor 재생 → 라이브 원장과 패리티 표 | §3 패리티 게이트 통과 여부 |

## 5. 영역 밖에서 본 것

- git status에 `research/studies/11_signal_axes/metrics.json`이 수정 미커밋이다. 엔진 수술 뒤 재생성본인지 확인하고 커밋하지 않으면 README와 대시보드가 서로 다른 숫자를 든다.
- 백테스트가 라이브 리스크 규칙(슬롯 20·교체 매도 5건·일 손실 한도·재기동 리셋)을 시뮬하지 않는다. 리셋 §1-1이 "손익은 운영이 갉아먹었다"고 진단했는데, 그 운영 규칙을 백테스트에 넣지 않으면 전략 엣지와 운영 손실을 앞으로도 분리할 수 없다. `sim.py`에 슬롯·교체 제한을 파라미터로 둔다.
- 세율 0.18%에서 0.20%로 바뀌는 09-21 경계를 `scripts/trade_costs.py`와 매매일지 집계가 날짜별로 구분하는지 확인이 필요하다. `costs.py`는 상수를 둘로 나눴지만 호출부 분기는 못 봤다.
- `devscale_replay.py`는 존 게이트를 전일 SMA로 판정하고 엔진은 당일 접기(fold_today)로 판정한다. 라이브 코드의 look-ahead는 아니지만 두 규칙이 다르면 패리티가 안 맞는 원인이 하나 더 생긴다. 어느 쪽이 정본인지 스펙에 적어야 한다.
- 헌장 2절대로 데이터는 등급을 붙여 쓰되, 결론 문장에 등급이 안 붙은 스터디(11·13·15 README)가 있다. 등급 표기를 `metrics.json`의 grade 필드로 강제하는 편이 문서 수기보다 낫다.
