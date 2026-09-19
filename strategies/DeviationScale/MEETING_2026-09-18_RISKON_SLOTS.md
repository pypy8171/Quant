# 2026-09-18 회의 — 강세장에서 점수 상위가 저변동 금융주로 채워지는 문제

참석: strategist(제안) → reviewer(반박, 코드·로그 실측) → 메인 중재. 장중 10:15~10:40.
입력: `Quant/build_win/logs/quant_trader.log`, `Quant/build_win/logs/trades_20260918.csv`, `Quant/config/config_dev_paper.json`.

## 0. 회의 전에 확정된 사실

| 항목 | 사실 | 근거 |
|---|---|---|
| 장 | 코스피 +2.1%, 국면 RISK_ON(score 8). 주도주는 전선·전기(삼화콘덴서 이격 26%, 가온전선 32%, 일진전기 8.8%) | 대시보드·로그 존 판정 |
| 슬리브별 손익(10:07) | DEVSCALE 매수 27/매도 0, 평가 −293K(086790 −103K·005830 −69K·055550 −65K = 금융 3종 −237K). TRENDX 매수 90/매도 44, 실현 +124K. DISPLACE 매도 31건 +5K | `trades_20260918.csv` FILL 집계 |
| 자본 배분 | DEVSCALE 종목당 캡 1,300만(005830 명목 1,143만), TRENDX 400만 | config |
| 점수식 | S = z(추세)+z(−이격)−0.5·z(ATR%)+0.7·z(log 거래대금). 이격 낮을수록·ATR 낮을수록 가점 → 주도주는 뒤로 밀림 | `Quant/src/universe/UniverseScanner.cpp:1294` |
| 슬롯 병목 | 두 슬리브가 25슬롯을 공유. 랭크는 슬리브별 z를 한 풀에 합친 근사(`StrategyFactory.cpp:540`). 가온전선 "점수 우선순위 미달(랭크 21, 슬롯 24/25)". 우선순위 거부 848건 | 로그 |
| 회전 | 익절 지정가 체결 3초 뒤 같은 값에 베이스 재매수(010170 17,720 매도→17,710 매수, 375500 87,600→87,700). 삼화콘덴서 144,900 매수→09:11 140,800 손절(−52K)→09:26 143,000·10:06 148,100 재매수 | CSV |
| 물타기 재적층 | 005830 베이스 24주 뒤 물타기 12주가 09:06·09:30·09:37 세 번(36주=684만). 재구성마다 분할 단계를 다시 깔아 소진 기억이 없고, DISPLACE가 24주 판 3분 뒤 되삼 | CSV·`DeviationScaleStrategy.h:783` |
| 재기동 부작용 | 재기동하면 잔고 보유가 DEVSCALE 스캔에서 빠지고(`StrategyFactory.cpp:621-648`) TRENDX 상위 25 밖이면 ITB 청산 관리 전략(장 마감 1600)으로 넘어가 15:05 청산이 없어진다. 시드는 `opened_at=now−24h`·z=−2.5라 즉시 교체 대상. 오늘 10:02·10:08·10:17·10:38 재기동으로 오늘 산 DEVSCALE 종목이 이미 청산 관리 전략 아래 있다 | `OrderGate.cpp:994`, 로그 "청산 관리(ITB)" 29건 |
| `daily_basis_warmup` | 효과 없음 — 09:00:03 REST 분봉 시드가 전일 63봉을 돌려줘(`BarAggregator.cpp:310` 날짜 필터 없음) 개장부터 워밍업이 아니다. "09:00 일봉 기준 맹목 매수"가 아니라 "전일 마감 60분 SMA 기준 매수" | 로그 415116·415154 |

## 1. 안건별 관점과 결론

| 안건 | strategist | reviewer | 중재 |
|---|---|---|---|
| ① 국면별 슬롯 분리(TRENDX 전용 상한) | 엣지 근거 약함(D-064 −0.015R). 기계적 병목은 실재. `risk.sleeve_caps` 코드 필요 | 서브원장이 DISPLACE/미연결 매도를 안 빼서 접두 카운트가 유령을 셈(`OrderGate.cpp:1014-1050`). displace 수혜 BUY가 슬리브 캡에 걸리면 피해자만 팔고 끝. 시드분은 슬리브 없음 | **보류 → 설계 뒤.** 서브원장 정리가 선행 |
| ② TRENDX 점수식(ATR 감점 제거·당일 등락 추가) | 반대. IC 검정 통과분은 3항이고 항을 바꾸면 무효. 당일 등락은 1일 반전 항 | 동의 | **백테스트 뒤.** `ic_summary`로 (i) ATR 제거 (ii) 당일 등락 z 두 셀, 홀드아웃 2022 통과 시만 |
| ③ 익절 후 재진입 쿨다운 | 찬성. `reentry_after_profit_sec` 600 | `peak_position_` 있음, 전량 청산 뒤에만 의미. 자기 매도와 교체·장마감 매도를 구분 못 하나 구분할 이유도 약함 | **적용(코드).** `reentry_cooldown_sec` 600, 전량 청산이면 원인 불문 |
| ④ TRENDX 캡 상향 | 반대. 손절 1건 16만, daily_loss 6건이면 정지 | 동의 | **기각.** 자본 이동은 DEVSCALE 캡 축소로 |
| ⑤ DEVSCALE 손실 구조 | 진짜 원인. 익절 +1.5% 캡/손실은 당일 하락폭 전부, 물타기 재적층, 캡 1,300만 | 물타기 끔·캡 400만은 안전(사이징만, 강제매도 경로 없음). `daily_basis_warmup`은 무효, `score_top_n` 축소는 무의미(정배열 통과 5~19개) | **적용(config).** `buy_split_steps` 0, `notional_cap_krw` 400만 |
| ⑥ `stop_cooldown_sec` 21600 | 손절 종목 당일 재매수 금지 | 근거는 `16_trendx_execution/README.md:114` 한 줄뿐, 길이 셀 없음. `devscale_replay.py:81 stop_cooldown_bars`로 잴 수 있음. 유니버스 이탈 600초 뒤 retire되면 쿨다운도 사라져 우회됨(W-1) | **백테스트 뒤.** 원칙 7 |

## 2. 적용한 것 (2026-09-18 10:40, 재기동 포함)

- 코드: `Quant/include/strategy/DeviationScaleStrategy.h` `reentry_cooldown_sec`(전량 청산 뒤 새 베이스 금지, 기본 600초), `Quant/src/strategy/StrategyFactory.cpp` 파싱. ctest 33/33.
- config(`Quant/config/config_dev_paper.json`, 비추적): DEVSCALE `buy_split_steps` 0, `notional_cap_krw` 1,300만→400만, 두 슬리브 `reentry_cooldown_sec` 600.
- 안 한 것: 슬롯 분리·점수식·캡 상향·손절 쿨다운 연장(위 표).

## 3. 남은 결정 (오너)

1. 재기동 시 오늘 산 종목이 ITB 청산 관리 전략(장 마감 1600)으로 넘어가 15:05 청산이 사라지는 문제. `manage_holdings.market_close_exit_hhmm` 1600→1505로 두면 이월 보유분(흥구석유·이글루·LG에너지솔루션·HD현대마린솔루션)도 함께 청산된다 — 보유분 강제청산이라 오너 결정.
2. ~~3분봉 시드의 전일 봉 제거(날짜 필터)~~ — 15:00 코드로 고침(`Quant/src/api/KisMarket.cpp` `get_minute_ohlcv`가 당일 KST 날짜 행만 받고, 당일 행이 없는 페이지에서 멈춘다). 리플레이(`PYQuant/backtest/devscale_replay.py`)는 REST 시드를 안 쓰고 분봉 파일을 직접 읽어 영향 없음. 별도 커밋, exe 재링크는 장 마감 뒤(재기동 부작용 때문에 장중엔 안 함) — 내일 09:00부터 DEVSCALE 개장 60분 워밍업이 실제로 걸린다.

## 4. 검증 필요 가정 (STRATEGIES.md 반영)

| # | 가정 | 검증 수단 | 시점 |
|---|---|---|---|
| 9 | RISK_ON(지수 전일 +1% 이상) 날 TRENDX가 DEVSCALE보다 초과R이 높다 | `research/studies/13_trendx_gate/results.tsv`를 지수 등락으로 조건화한 제거 비교 | 백테스트 |
| 10 | 점수식에서 ATR 감점을 빼거나 당일 등락 z를 더해도 IC 기준(t>2 ∧ IC>0 월 ≥60%)과 홀드아웃 2022를 통과한다 | `ic_summary` 두 셀 | 백테스트 |
| 11 | 손절 뒤 재진입 금지 길이(15분 vs 당일)가 기대값을 바꾼다 | `PYQuant/backtest/devscale_replay.py stop_cooldown_bars` 5/130봉 | 리플레이 |
| 12 | 전량 청산 뒤 10분 재진입 금지가 왕복 비용만 줄이고 익절 기회를 줄이지 않는다 | 09-19~ 라이브 원장: 청산→재매수 간격 분포와 재매수분 R | forward 2주 |
