# 전략 카탈로그 (Strategy Catalog)

> 자동매매로 **코드화된 전략의 단일 소스**. @strategist 가 피드백 시 이 문서를 참조한다.
> 새 전략을 코드화하거나 파라미터/로직을 바꾸면 **반드시 이 문서를 갱신**한다.
> 최종 갱신: 2026-06-04

## 요약 테이블
| 전략 | type(config) | 파일 | 데이터훅 | 시장 | 등록 상태 |
|---|---|---|---|---|---|
| MACrossStrategy | `MA_CROSS` | `Quant/include/strategy/MACrossStrategy.h` | on_data(일봉) | KR | 미등록 |
| MomentumStrategy | `MOMENTUM` | `Quant/include/strategy/MomentumStrategy.h` | on_data(일봉) | KR | 미등록 |
| ValueContraryStrategy | `VALUE_CONTRARY` | `Quant/include/strategy/ValueContraryStrategy.h` | on_order_book(KR)+on_trade(US) | KR/US | config.json(실계좌) ✅ |
| FixedIntervalStrategy | `FIXED_INTERVAL` | `Quant/include/strategy/FixedIntervalStrategy.h` | on_trade(체결) | KR | config_paper.json(2개) ✅ |
| PriceTargetStrategy | `PRICE_TARGET` | `Quant/include/strategy/PriceTargetStrategy.h` | on_order_book+on_trade | KR | 미등록 |
| SupplyDemandPullbackStrategy | `SUPPLY_DEMAND_PULLBACK` | `Quant/include/strategy/SupplyDemandPullbackStrategy.h` | on_data(EOD)/on_trade(INTRADAY) | KR | 미등록 |
| ThemeStrategy | `THEME` | `Quant/include/strategy/ThemeStrategy.h` | on_order_book+on_trade | KR | 미등록 |

> `StrategyBase`(`Quant/include/strategy/StrategyBase.h`): 추상 인터페이스. `id()`/`describe()`/`on_data()` 순수가상, `on_order_book()`/`on_trade()`/`on_start()`/`on_stop()`/`get_watch_specs()` 기본 no-op. `set_kis()`로 KisClient 주입(non-owning, Engine이 수명 관리).

**활성 등록 현황**
- `config.json`(실계좌, is_paper:false, mode TRADE) → VALUE_CONTRARY 1개
- `config_paper.json`(모의, is_paper:true, mode TRADE) → FIXED_INTERVAL 2개 (402340 buy/sell 1·120s, 097230 buy/sell 2·180s)
- `config.json.example` → mode KR_TEST(샘플, 전략 미실행)

---

## 전략별 상세 (C++)

### MACrossStrategy (`MA_CROSS`)
- **로직**: 단/장기 SMA 골든·데드크로스. 진입(BUY): 미보유 + 직전봉 short≤long → 당봉 short>long(상향돌파). 청산(SELL): 보유 + 직전봉 short≥long → 당봉 short<long. 종가 SMA, `prices_` deque는 long_period개 유지.
- **데이터 훅**: on_data(일봉)
- **config**: `ticker`(필수), `short_period`(필수), `long_period`(필수), `quantity`=1
- **리스크**: MARKET, 1포지션(in_position_), 손절 없음(데드크로스까지 보유).

### MomentumStrategy (`MOMENTUM`)
- **로직**: 돈치안 채널 브레이크아웃. 진입: 미보유 + `close≥최근 period일 최고가`. 청산: 보유 + `close≤최근 period일 최저가`.
- **데이터 훅**: on_data(일봉)
- **config**: `ticker`(필수), `period`(필수), `quantity`=1
- **리스크**: MARKET, 1포지션, 저점 이탈이 곧 청산.

### ValueContraryStrategy (`VALUE_CONTRARY`)  — 실계좌 등록
- **로직**: 저PBR 3일 연속 하락 반전 매수. on_start 스크리닝: KR=KOSPI("J")+KOSDAQ("W") PBR≤pbr_max 유니버스, 5일 일봉(호출당 200ms 슬립), volume=0 미완성봉 제거 후 `bars[0]<bars[1]<bars[2]<bars[3]` → candidates_. 진입: 세션 내 후보 첫 이벤트 시장가 BUY(종목당 1회). 청산: `hhmm≥eod_exit_hhmm` 시장가 SELL. 세션 KR 0900–1530 / US 2230–0500(KST).
- **데이터 훅**: KR=on_order_book, US=on_trade
- **config**: `market`="KR"(KR/US), `exchange`=""(US NAS/NYS), `pbr_max`=1.0(0=미적용), `quantity`=1, `eod_exit_hhmm`=1520
- **현재 등록**: ✅ config.json(실계좌) KR/pbr 1.0/qty 1/EOD 1520
- **리스크**: 인트라데이(EOD 강제청산, 익일 이월 X). 유니버스 크면 장전 스캔 시간 김.

### FixedIntervalStrategy (`FIXED_INTERVAL`)  — 모의 등록 (파이프라인 테스트용)
- **로직**: 체결 이벤트마다 interval_sec 경과 체크 → BUY(buy_qty)/SELL(sell_qty) 교대 발행. on_start에서 첫 신호 즉시 허용. sell_qty=0이면 BUY만. 시간=steady_clock, 세션 판정=td.time.
- **데이터 훅**: on_trade(체결)
- **config**: `ticker`(필수), `buy_qty`=1, `sell_qty`=1, `interval_sec`=300
- **현재 등록**: ✅ config_paper.json 2개(검증용 왕복)
- **리스크**: 실전략 아님 — 연결/주문/체결/원장 파이프라인 검증용. 무한 반복이라 수수료·세금 누적, **실계좌 사용 금지**.

### PriceTargetStrategy (`PRICE_TARGET`)
- **로직**: (1) price_targets — `price≤buy_price`→BUY, `price≥sell_price`→SELL, 동일 방향 cooldown_sec 내 재주문 차단. (2) limit_orders — 장 시작 후 종목 첫 이벤트에 지정가 1회 제출. 호가에선 asks[0](없으면 bids[0]) 현재가 대리, 체결에선 td.price.
- **데이터 훅**: on_order_book + on_trade
- **config**: `price_targets[]`(`ticker`,`buy_price`=0,`sell_price`=0,`quantity`=1,`cooldown_sec`=60), `limit_orders[]`(`ticker`,`side`="BUY",`price`=0,`quantity`=1)
- **리스크**: ⚠️ 포지션 추적 없음(보유량 무관 SELL 가능), 1일 주문 횟수 상한 없음(cooldown만).

### SupplyDemandPullbackStrategy (`SUPPLY_DEMAND_PULLBACK`)
- **로직**: 수급(쌍끌이) 선별 + 5일선 눌림목. on_start: 시총 상위 universe_size(`fetch_kr_ranking`) → 종목별 `get_investor_flow` 시계열, 당일 제외(look-ahead 방지) lookback_days 집계. 통과: dual_days(외인>0 AND 기관>0)≥min_dual_days AND (min_consec_days=0 또는 consec≥) AND 누적외인>threshold AND 누적기관>threshold. **EOD**(on_data): closes_ SMA, prev_above & in_band(ma±pullback_band) & supported(price≥ma)면 BUY, 보유 중 stop_below_ma 손절. **INTRADAY**(on_trade): 전일 확정 ref_ma5 고정, 장중 밴드 터치 시 BUY, eod_exit_hhmm 또는 ma 이탈 손절 시 SELL.
- **데이터 훅**: EOD=on_data, INTRADAY=on_trade(trade_only)
- **config**: `market_div`="J", `universe_size`=50, `lookback_days`=5, `min_dual_days`=3, `min_consec_days`=0, `net_buy_threshold`=0, `ma_period`=5, `pullback_band`=0.01, `require_prev_above`=true, `quantity`=10, `eod_exit_hhmm`="1500", `stop_below_ma`=0.0, `entry_mode`="EOD"(INTRADAY 가능)
- **✅ 버그 수정완료(2026-06-04)**: 이전엔 `is_candidate()`가 `cand_set_`를 조회하는데 `on_start()`가 `rebuild_set()`을 호출하지 않아 cand_set_가 항상 비어 매매가 일어나지 않았음. 후보 확정 직후 `rebuild_set()` 호출 추가(헤더 line 119 뒤). quant_trader 빌드 확인. 이제 등록 시 정상 동작.

- **🔬 검증 필요 가정 (strategy-debate 2026-06-04, @strategist↔@reviewer)** — INTRADAY 모드 투입 전 반드시 확인:
  1. **수급 데이터 잠정/확정**: `get_investor_flow`는 `get_investor_trend`와 **동일 엔드포인트(FHKST01010900)**. 후자는 `output[0]`을 "당일 잠정치"로 사용(KisClient.cpp 주석). 따라서 `flows[0]`이 "전일 확정치"라는 가정은 **거짓일 가능성 높음** → 장 시작 전/직후 실제 응답의 `stck_bsop_date`·값 안정성을 찍어 확인. (수량 vs 금액 단위 정합성도)
  2. **수급 신선도 공백**: INTRADAY 수급 선별은 `on_start` **1회 고정**, 장중 `on_trade`는 가격만 봄 → 외인·기관이 장중 매도 전환해도 전략은 모르고 매수 지속. (로직 한계, 데이터 문제 아님)
  3. **WebSocket 구독 한계**: `get_watch_specs`가 후보 전체를 무제한 구독, Engine/connect에 개수 캡 없음 → 후보 >~41이면 일부 종목 체결/체결통보 **조용히 누락 → 진입 누락**. 구독 거부 응답 미처리.
  4. **손실 한도 무력화**: INTRADAY는 15:00까지 미청산 → daily_pnl(SELL 체결로만 갱신)이 장중 0 → daily_loss_limit 사실상 미작동 + `stop_below_ma=0.0` 기본 비활성. **INTRADAY는 stop_below_ma 양수 강제 필요**.
  5. **OrderGate 거부 시 held_ 롤백 없음**: BUY가 rate/dedup으로 거부돼도 `held_`는 이미 set → 그 종목 그날 진입 영구 차단. 체결통보로만 held_ set하도록 변경 검토.
  6. **백테스트 불가**: INTRADAY 진입은 틱(on_trade) 기반인데 백테스트 엔진은 일봉 → 검증 수단 부재. EOD 모드부터 검증 권장.
  > 메모: reviewer의 "KisClient.cpp:1277 백슬래시 빌드 깨짐"은 **오탐**(정상 `//` 주석, 메인 검증 완료).

### ThemeStrategy (`THEME`)
- **로직**: 3단 필터 테마 모멘텀. Step1: 업종(sector_codes 또는 내장 KOSPI 10개) 6일 지수일봉 5일 수익률 정렬 → 상위 top_n_sectors. Step2: 업종별 `fetch_sector_ranking`(30) 중 20일 평균거래량 대비 surge≥volume_surge_mult → surge_candidates(≤50). Step3: inst_filter true면 `get_investor_trend` 외인>0 AND 기관>0만. 진입: 세션 내 후보 첫 이벤트 시장가 BUY(종목당 1회). 청산: hhmm≥eod_exit_hhmm 시장가 SELL.
- **데이터 훅**: on_order_book + on_trade
- **config**: `sector_codes[]`=[](빈배열=내장 10업종), `top_n_sectors`=2, `volume_surge_mult`=2.0, `inst_filter`=true, `quantity`=1, `eod_exit_hhmm`=1520
- **리스크**: MARKET, 인트라데이 EOD 청산. on_start 다단계 REST(업종 500ms+종목 200ms 슬립 다수) → 장전 준비 매우 김.

---

## Python 전략 (백테스트/리서치)
위치 `PYQuant/strategy/`. 베이스 `base.py`(StrategyBase). 지표 `indicators.py`(sma, is_aligned=SMA5>10>20>60 정배열, deviation_from_sma 이격도%).

### StrategyA (`strategy_a_v1`) — `PYQuant/strategy/strategy_a.py`
- 강세 테마주 5일선 눌림목 추종(백테스트+라이브 겸용). 진입(BUY): 유니버스 ∩ 60봉+ ∩ 정배열 ∩ close≥SMA5 ∩ 이격도 dev≤z. 청산(SELL): dev≥a(과익) 또는 close<SMA5*(1-b/100)(손절). 백테스트는 on_data 내 visible bars로 필터(look-ahead X), 라이브는 on_start에서 get_historical_ohlcv(0.3s 슬립) pre-screening.
- 파라미터: `z`=1.0, `a`=7.0, `b`=2.0, `quantity`=1. **수급 필터(C3)는 미구현(STEP F TODO)**.

### ValueContraryStrategy (`VALUE_CONTRARY_KR`) — `PYQuant/strategy/value_contrary.py`
- C++ VALUE_CONTRARY의 Python 포팅(백테스트/리서치). on_start 5일 일봉(0.2s 슬립) 3일 연속 하락 → candidates. on_data: 후보+미보유 시장가 BUY, 보유 중 진입 익일(1봉 보유) 시장가 SELL. 파라미터 `pbr_max`=1.0, `quantity`=1. C++판과 청산 차이: Python=익일 1봉 보유, C++=당일 EOD 인트라데이.

### 백테스트 엔진 — `PYQuant/backtest/engine.py`
- `BacktestEngine` + `CostModel`(수수료 0.015%/거래세 0.18%/슬리피지 5bp). look-ahead 차단: 시그널 봉의 **다음 봉 시가**로 체결. MDD/Sharpe는 일별 equity 시계열 기반(연환산 252), total_return은 equity[-1] 기반(C7 반영). 스크리닝: start_date 직전 4봉 3일 연속 하락.

---

## 미등록·실험 / TODO
- **미등록(코드만 존재)**: MA_CROSS, MOMENTUM, FIXED_INTERVAL(테스트용), PRICE_TARGET, SUPPLY_DEMAND_PULLBACK, THEME.
- **SDP `cand_set_` 버그**: ✅ 수정완료(2026-06-04, rebuild_set() 호출 추가).
- **StrategyA 수급 필터(C3)**: pykrx 검증 후 추가 예정, 현재 C2(정배열)+C3.5(SMA5 위)까지만.
- **검증 진행 중**: config_paper.json FIXED_INTERVAL 왕복으로 주문→체결→원장 파이프라인 검증 중. 통과 후 SDP(버그 수정 + EOD 완화 파라미터) 재투입 예정.
