# 강한 종목 첫 VWAP 눌림 — C++ 엔진 구현 가이드

작성 2026-10-01. 구현 지시서이고, **코드는 백테스트 합격 뒤에 쓴다**. 규칙 숫자(선정 상위 K, 신규 진입 마감 시각, 사이징, 합격선)의 정본은 같은 폴더 `SPEC.md`다 — 이 문서의 숫자와 다르면 SPEC.md를 따른다(예: 선정은 SPEC의 상위 K=10, 신규 무장 마감 13:15).
구현은 워크트리 `../Quant-wt-vwappb`(브랜치 `wt/vwappb`)에서 한다. 줄 번호는 2026-10-01 main(3000c31) 기준이고, 구현을 시작할 때 `grep -n`으로 다시 확인한다.

## 0. 결론

**진단**
- 장부는 (계좌, 종목) 한 칸이고 전략별 서브장부가 없다. 같은 종목을 DevScale·ITB 승계 래퍼와 동시에 들면 손익 귀속·재기동 재인수·마감 청산이 서로의 몫을 건드린다.
- 데이터는 이미 있다. 시세판이 5초마다 전 종목의 등락률과 누적 거래대금을 갖고 있고 1분 단위 csv로 남는다.

**바꿀 것**
1. 순수 함수 `VwapPullbackRules`, 종목별 전략, 슬리브 로더를 새로 만든다. 백테스트와 라이브가 같은 판정 규칙을 쓴다.
2. 소유를 배타로 둔다. 기동할 때는 먼저 도는 로더가 `basket_owned`·`scan_covered`에 표시하고, 장중에는 공유 등록 비트를, 진입 직전에는 보유·예약·청산관리 검사를 쓴다.
3. `scripts/check_runtime_health.py`에 판정 행을 넣는다.

**첫 실행**: 백테스트 합격 → 워크트리 → `Quant/include/strategy/VwapPullbackRules.h`와 `Quant/tests/test_vwap_pullback_rules.cpp`부터. 같은 날 `board_YYYYMMDD.csv`와 1분봉을 넣었을 때 파이썬 백테스트와 같은 신호가 나오게 맞춘다.

**판정**
- 그림자 → 모의: 10거래일 이상, 신호 일치율 90% 이상, FAIL 0.
- 모의 → 실계좌: 20거래일 또는 40건 이상, 비용 뒤 PF 1.1 이상, 손절 슬리피지 평균 0.2%p 이하, 15:10 플랫 100%, 남의 몫 매도 0건, 사용자 승인.

## 1. 현재 상태

| 영역 | 자리 | 상태 |
|---|---|---|
| 전 종목 시세 | `Quant/include/universe/MarketBoard.h`, `Quant/src/universe/MarketBoard.cpp:173`(등락률), `:681`(1분 저장) | 5초 주기, `BoardQuote{code,name,price,volume,value,market_value,change_percent}`, KRX 정규장 누적(NXT 제외). `board_YYYYMMDD.csv`는 2026-10-01부터 쌓임 |
| 1분봉 REST | `Quant/include/api/KisClient.h:116-117` `get_minute_ohlcv` | 호출당 30건 |
| 틱 → 1분봉 | `Quant/include/core/BarAggregator.h:61-86` | interval_min=1 |
| 슬리브 패턴 | `Quant/src/strategy/DevScaleLoader.cpp:200`, `:719-730` | 본뜬다 |
| 공유 등록 | `Quant/src/core/UniverseRescan.cpp:277` | 슬리브끼리 같은 종목을 두 번 등록하지 않음 |
| WS 칸 | `Quant/include/core/WebSocketSlotPlan.h`, `Quant/src/core/EngineUniverse.cpp:164` | 보유 0 / 예약 1 / 1000+순위. 칸 밖은 REST 폴링 |
| 국면 | `Quant/src/core/EngineRegime.cpp:28,35` | 맵에 없는 전략 id는 비활성 |
| 주문 게이트 | `Quant/include/risk/OrderGate.h:40-62` | 거부 지점 19개 |
| 로더 | `Quant/src/strategy/StrategyFactory.cpp:506-514` | 7종 |

## 2. 갭

1. VWAP·오전 고점·눌림 저점을 계산하는 코드가 없다.
2. 09:30에 순위를 한 번만 고정하는 선정 단계가 없다.
3. 재인수 경로(`Quant/src/strategy/DevScaleLoader.cpp:602-673`)는 DevScale과 바스켓만 안다. 실계좌는 `manage_holdings`가 꺼져 `:605-608`에서 잔고를 보지 않으므로, 재기동하면 DevScale 초기 스캔이 VWAP 보유분을 가져갈 수 있다.
4. 이 전략을 위한 판정 행이 없다.

## 3. 데이터 경로

**3.1 선정(하루 한 번)**
- 09:30:00 뒤 처음 바뀐 시세판 세대(`generation()`)를 쓴다.
- 거른다: 등락률 band(SPEC), 상한가·VI 종목 제외 → `value` 내림차순 상위 K.
- 결과를 `logs/vwappb_selection_YYYYMMDD.json`에 쓴다. 재기동하면 이 json을 먼저 읽고, 없으면 `board_YYYYMMDD.csv`의 09:30 행으로 다시 고른다. 현재 시각의 시세판으로는 고르지 않는다(미래 정보가 섞임).
- VI 필드는 MCP `kis-code-assistant`로 확인하고 확인일을 코드에 남긴다(D-120). 우회로: (a) 선정 종목에 `get_current_prices` 2~3회 — **추천**, (b) +10% 초과 제외 — 표본 손실 큼, (c) WS VI 필드 — 칸을 받은 종목만.

**3.2 무장(armed)**
- 시세판 5초 값으로 되밀림 0.30~0.70을 대략 거른 뒤, `get_minute_ohlcv`(프리페치 풀, 시세 키 버킷)로 정확한 고점·저점을 확인한다.
- 되밀림·VWAP 밴드 조건(SPEC)을 만족하면 armed. armed 종목만 재스캔 목록 앞에 둔다(`max_armed`=6, WS는 `trade_only=true`로 1칸, `Quant/include/core/Types.h:23-30`).

**3.3 VWAP**
- 시세판 `value / volume`. 재기동하면 다음 시세판 세대에서 바로 복원된다. 백테스트도 csv의 `turnover / volume`을 쓰므로 정의가 하나다.
- 통합 구독이면 WS가 `H0UNCNT0`(`Quant/src/api/WebSocketClient.cpp:303`)이라 NXT가 섞인다. 그래서 틱 누적 VWAP은 쓰지 않는다.
- 지연은 최대 5초. 그림자 단계에서 `vwappb_vwap_drift`로 잰다.

**3.4 고점·저점 복원**
- 장중에는 틱으로 BarAggregator 1분봉을 만든다. 재기동 때는 `get_minute_ohlcv(ticker, 30*k, 1)`로 09:00 이후 봉을 다시 받는다.
- 시세판 공급원은 `std::function`으로 주입한다(시험·리플레이에서 csv를 넣기 위해).

**3.5 WS 칸 나누기**
- 고정: 체결통보 1, 핀 000660 1. 보유 종목 우선순위 0. VWAP armed 최대 6칸(랭크 없음, 못 받으면 REST 폴링). DevScale은 나머지.
- 엔진은 고치지 않는다. 확보율이 80% 아래면 `set_entry_priority`로 armed 종목에 랭크를 주는 안을 검토한다(연결: DEFERRED D-17).

## 4. 파일 배치

새 파일은 모두 구현 세션이 만든다. 헤더에는 선언만 둔다(D-118).

| 파일 | 내용 |
|---|---|
| `Quant/include/strategy/VwapPullbackRules.h` / `Quant/src/strategy/VwapPullbackRules.cpp` | select_candidates, retrace_ratio, in_vwap_band, breakout_trigger, stop_price, exit_due, traded_today, owns_holding. 엔진에 의존하지 않음 |
| `Quant/include/strategy/VwapPullbackStrategy.h` / `Quant/src/strategy/VwapPullbackStrategy.cpp` | StrategyBase 상속, id `"VWAPPB_"+ticker` |
| `Quant/src/strategy/VwapPullbackLoader.cpp` | `load_vwap_pullback`, `VwapPullbackSleeve`(선정·무장·하루 진입 예산 원자 카운터) |
| `Quant/tests/test_vwap_pullback_rules.cpp`, `Quant/tests/test_vwap_pullback_strategy.cpp` | 6절 |
| `strategies/VWAPPB/SPEC.md` | 규칙·합격선(research 쪽 SPEC.md에서 옮김) |

훅
- `on_start`: `symbol_of`로 id를 받는다. `confirmed_position`을 확인하고, 재인수면 보호 손절을 다시 건다.
- `on_trade`: 1분봉 갱신 → 트리거 → 진입 검사 → 매수.
- `get_watch_specifications`: `trade_only`.

config 기본값(키 이름 초안, 숫자는 SPEC 확정값으로 채운다)
```json
{"type":"VWAP_PULLBACK","enabled":false,"shadow":true,"id_prefix":"VWAPPB",
 "select_hhmm":930,"change_min_pct":6.0,"change_max_pct":20.0,"turnover_top_n":10,
 "retrace_min":0.38,"retrace_max":0.62,"vwap_band_pct":0.5,"stop_below_low_pct":0.3,
 "no_new_entry_hhmm":1315,"exit_hhmm":1510,"max_positions":1,"notional_krw":0,
 "max_armed":6,"entry_timeout_sec":30,"max_entries_per_ticker_per_day":1}
```
- `enabled=false`이면 아무것도 등록하지 않는다. `shadow=true`이면 주문 없이 `logs/vwappb_shadow_YYYYMMDD.csv`에만 쓴다. `notional_krw=0`이면 진입하지 않는다.
- 활성 여부는 국면 맵이 정한다. `"VWAPPB_*"`가 맵에 없으면 `Quant/src/core/SignalDispatcher.cpp:156`이 매수를 막는다. 맵 항목은 모의 config에만 먼저 넣는다.

StrategyFactory
- LOADERS(`Quant/src/strategy/StrategyFactory.cpp:506-514`)에 한 줄.
- `:519-536`의 TARGET_BASKET 판정을 `loads_first(type)`로 바꿔 VWAP_PULLBACK도 DevScale보다 먼저 돌게 한다.

## 5. 사이드 이펙트 차단 (지키는 자리 / 시험)

**5.1 DevScale과 같은 종목 동시 보유 금지**
- 기동: VWAP 로더가 먼저 돌며 재인수 종목을 `pass.basket_owned`·`pass.scan_covered`에 넣는다(`Quant/src/strategy/StrategyLoadPass.h:37,44`). DevScale은 `Quant/src/strategy/DevScaleLoader.cpp:602`·`:657`에서 그 종목을 뺀다. 시험 `load_first_marks_owned_tickers`.
- 장중: `Quant/src/core/UniverseRescan.cpp:277`. 시험 `test_universe_rescan`에 `two_sleeves_do_not_share_ticker`.
- 진입 직전(새로 만듦): `entry_allowed()`에서 `confirmed_position==0`(`Quant/include/strategy/StrategyBase.h:207`), 예약 0, exit_managed 아님, `slot_exempt_symbols()`(`Quant/include/core/Engine.h:505`)에 없음을 확인. 시험 `no_entry_when_other_holds`.
- 반대 방향: DevScale 재스캔이 `held_positions`를 빼므로(`Quant/src/strategy/DevScaleLoader.cpp:356-374`) VWAP 보유 종목은 들어가지 않는다.
- 사후: 판정 행 `vwappb_overlap_devscale`(FAIL).

**5.2 재기동 재인수**
- `trades_YYYYMMDD.csv`에서 strategy가 `"VWAPPB_"`로 시작하는 행의 순매수량으로 판정한다(`Quant/src/strategy/DevScaleRules.cpp:20`, `:75`와 같은 방식).
- `owns_holding`: `net >= held`이면 재인수, `0 < net < held`이면 재인수하지 않고 ITB로 넘김(d2c40d5 규칙), `0`이면 손대지 않음.
- `manage_holdings` 설정과 무관하게 잔고를 본다. 잔고를 못 받으면 재인수 0으로 두고 경고.
- 시험 `owns_full`·`owns_partial_hands_off`·`owns_none`. 회귀 `test_devscale_rules`(DEVSCALE_ 필터가 VWAPPB_ 행을 세지 않는지).

**5.3 슬롯·사이징 백스톱 공유**
- 게이트는 하나이고 전략을 가리지 않는다(`Quant/src/risk/OrderGate.cpp`). 그대로 걸리는 거부 지점: ConcurrentLimit `:515-570`, TickerNotional `:503`, GrossExposure `:646-662`, RatePerSecond `:736`, EntryHalt `:416`, OutsideSession `:432`.
- PriorityBar(`:598-639`)는 rank>0일 때만 걸려 VWAP에는 걸리지 않는다(의도).
- 슬리브 상한 `max_positions`는 원자 카운터로 센다.
- `slot_exempt` 재사용은 기각: `Quant/include/core/Engine.h:504`가 집합을 통째로 바꿔 바스켓 표시를 지우고, `Quant/src/risk/OrderGate.cpp:872` 교체 후보에서도 빠진다.
- 시험 `test_order_gate`에 `unranked_entry_skips_priority_bar`.

**5.4 15:10이 남의 몫을 팔지 않게**
- 매도 수량은 `min(confirmed_position, VWAPPB_ 순매수)`. `Quant/src/strategy/DeviationScaleStrategy.cpp:359-383`(clamp_sellable=false)을 그대로 베끼지 않는다.
- 시장가 매도를 먼저, 미체결 매수 취소는 그 뒤. 재기동 시각이 15:10 뒤면 `on_start`에서 바로 판다.
- 시험 `exit_sells_only_own_quantity`(장부 10, 자기 6 → 6), `exit_after_restart_past_1510`. 판정 행 `vwappb_flat_after_exit`, `vwappb_oversell`.

**5.5 마감 청산·ITB와의 관계**
- `attach_holding_exit_managers`(`Quant/src/strategy/StrategyFactory.cpp:335`, `:385`)는 covered 종목을 건너뛴다. 5.2에서 VWAP 종목이 covered로 표시되므로 ITB가 붙지 않는다.
- ITB 관리 종목은 `Quant/src/core/SignalDispatcher.cpp:188`이 NEW를 막고, 진입 검사에서도 거른다.
- 부분 소유분은 `pass.pending_exit_managers`로 넘긴다(`Quant/src/strategy/DevScaleLoader.cpp:742`와 같은 방식).
- 모의 ITB 마감 1515, VWAP 청산 1510이라 겹치지 않는다.
- 시험 `partial_holding_goes_to_itb`, `no_entry_on_exit_managed`.

**5.6 국면 정지선**
- 게이트 EntryHalt(`Quant/src/risk/OrderGate.cpp:416`, `Quant/include/risk/OrderGate.h:242`)가 걸린다.
- 전략 쪽에서도 `is_active`·`entry_halted`·`entry_scale`(`Quant/include/strategy/StrategyBase.h:132,230,241`)을 본다. `entry_scale==0`이면 진입하지 않는다.
- RISK_OFF 목록에는 넣지 않는다. 청산은 국면과 무관하게 낸다.
- 시험 `test_signal_dispatcher`에 `vwappb_inactive_blocks_buy`. 판정 행 `vwappb_buy_while_halted`(FAIL).

**5.7 주문 초당 한도**
- RatePerSecond 5/s를 같이 쓴다. VWAP 주문은 진입 1·손절 1·15:10 매도 `max_positions`건 이하.
- 15:10은 DevScale 시각(1450 신규 중단)과 다르다. REST는 시세 키 버킷만 쓴다.
- 판정 행 `vwappb_gate_rejects`에서 RatePerSecond가 5%를 넘으면 WARN.

## 6. 시험과 판정 행

**새 시험**
- `test_vwap_pullback_rules`: select_top_k_by_turnover_in_change_band, excludes_over_20_and_vi, selection_uses_0930_row_not_now, retrace_ratio_bounds, vwap_band_half_percent, breakout_trigger_prev_minute_high, stop_price_low_minus_0_3, exit_due_1510, traded_today_blocks_second_entry, owns_full / owns_partial_hands_off / owns_none
- `test_vwap_pullback_strategy`: disabled_registers_nothing, shadow_emits_no_order, no_entry_when_other_holds, no_entry_on_exit_managed, load_first_marks_owned_tickers, exit_sells_only_own_quantity, exit_after_restart_past_1510, partial_holding_goes_to_itb, stop_rearmed_on_restart

**회귀**: test_devscale_rules, test_universe_rescan, test_websocket_slot_plan, test_order_gate, test_signal_dispatcher, test_market_board, test_engine. ctest 전에 `py ../quant-devtools/check_build_ready.py`.

**판정 행**(`(name, ok, level, detail)`, `scripts/check_runtime_health.py` `global_rows` `:2392-2427`, 원장 읽기 `live_ledger_records` `:739`)

| 행 | 기준 | 시작 단계 |
|---|---|---|
| vwappb_selection_present | 09:35까지 json 없으면 FAIL | 그림자 |
| vwappb_preempted_ratio | 30% 초과 WARN | 그림자 |
| vwappb_shadow_match | 90% 미만 WARN | 그림자 |
| vwappb_ws_slot_coverage | 80% 미만 WARN | 그림자 |
| vwappb_vwap_drift | 중앙값 0.2% 초과 WARN | 그림자 |
| vwappb_overlap_devscale | 있으면 FAIL | 모의 |
| vwappb_flat_after_exit | 15:15 뒤 순보유 ≠ 0이면 FAIL | 모의 |
| vwappb_oversell | 매도 > 매수면 FAIL | 모의 |
| vwappb_buy_while_halted | 있으면 FAIL | 모의 |
| vwappb_stop_slippage | 평균 0.2%p 초과 WARN | 모의 |
| vwappb_gate_rejects | 정보 | 모의 |
| vwappb_restart_orphan | 있으면 FAIL | 모의 |

## 7. 단계

**Phase 1 — 그림자**: 4절 파일, 6절 시험, 그림자 판정 행 5개. 모의 프로세스에서 `enabled=true, shadow=true`. 그림자도 WS 칸을 쓰므로 DevScale 칸이 줄어드는지 같이 본다. 넘어가는 조건: 10거래일 이상, 일치율 90% 이상, FAIL 0, 칸 확보율 80% 이상(못 미치면 REST 폴링 결과의 일치율로 판단).

**Phase 2 — 모의 주문**: `shadow=false`, `max_positions=1`, `notional_krw`를 정하고 RISK_ON·NEUTRAL에 `"VWAPPB_*"`를 넣는다. 판정 행을 전부 켠다. 손절 주문 구분 코드는 MCP로 확인(D-120). 넘어가는 조건: 20거래일 또는 40건 이상, 비용 뒤 PF 1.1 이상, 슬리피지 0.2%p 이하, 15:10 플랫 100%, oversell·overlap·orphan 0건, **사용자 승인**(실계좌 한도·자본은 리스크 한도 변경).

**Phase 3 — 실계좌 소액**: `Quant/config/config_live.json`에 블록(`max_positions=1`, 소액). 4종목 자리를 나눠 쓰므로 20거래일 동안 "VWAPPB_ 때문에 거부된 DEVSCALE_ 진입 수"를 재서 슬리브 예산을 만들지 정한다.

**공용 파일 줄 단위 편집**

| 파일 | 자리 | 편집 |
|---|---|---|
| `Quant/include/core/Types.h` | :355-356 | enum 1줄 |
| `Quant/src/core/Types.cpp` | :63-64 | from_string 1줄 |
| `Quant/src/strategy/StrategyLoadPass.h` | :70 근처 | 선언 1줄 |
| `Quant/src/strategy/StrategyFactory.cpp` | :506-514, :519-536 | LOADERS 1줄, `loads_first(type)` |
| `Quant/CMakeLists.txt` | :184·:212, :524 근처, :967·:986 | 소스 3줄, 시험 타깃 2개, QUANT_ASSERT_TARGETS |
| `scripts/check_runtime_health.py` | :2392-2427 | 행 12개 |
| `Quant/config/config_dev_paper.json` | strategies·regime_strategies | 블록 1개(Phase 1), 맵 항목(Phase 2) |
| `Quant/config/config_live.json` | 같은 자리 | Phase 3, 승인 뒤 |
| `docs/FILE_INDEX.md` | 새 파일 줄 | 게이트 대응 |

바꾸지 않는 파일: `Quant/src/core/Engine.cpp`·`Quant/include/core/Engine.h`, `Quant/src/strategy/DevScaleLoader.cpp`, `Quant/src/risk/OrderGate.cpp`, `Quant/src/core/UniverseRescan.cpp`. `basket_owned` → `sleeve_owned` 이름 바꾸기는 별도 리팩터 커밋으로 미룬다.

## 8. 결정 기록 초안 (넣을 때 번호 재확인, 작성 시점 최신 D-153)

**결정 초안 ① VWAP 원천을 시세판 누적값으로 둔다** — VWAP = 시세판 누적 대금 / 누적 거래량. 버린 대안: A 틱 누적(재기동 때 처음부터 못 쌓음, NXT 섞임, 칸 없는 종목 계산 불가), B TradeData에 누적대금 필드(hot path 구조체·공용 디코더 변경), C 1분봉 typical price(REST 증가, 근사). 대가: 최대 5초 지연, 네이버 가용성 의존 — 시세판이 끊기면 진입 중지·청산 계속.

**결정 초안 ② 슬리브 간 소유를 배타로 둔다** — 한 종목은 한 시점에 한 전략만 보유(먼저 도는 로더 + 공유 등록 비트 + 진입 직전 검사). 버린 대안: A 전략별 서브장부(원장·게이트·재시드 전부 수정, 범위 큼), B slot_exempt 재사용(바스켓 표시 지움, 교체·청산 후보에서 빠짐), C 동시 보유 + 사후 귀속(청산·재인수가 남의 몫을 팜). 대가: DevScale이 먼저 잡은 종목을 놓쳐 표본이 편향된다(`vwappb_preempted_ratio`로 잰다). 고객 계좌 배분(D-128)과 같은 구조의 문제다.

## 9. DEFERRED 신규 문장 초안

1. **슬리브별 슬롯 예산** — 공유 동시 보유 상한(실계좌 4)을 슬리브가 나눠 쓰는 예산이 없다. 재개 조건: 실계좌 20거래일 중 VWAPPB_ 보유 때문에 거부된 DEVSCALE_ 진입이 5건 이상.
2. **전략별 포지션 서브장부** — 장부가 (계좌, 종목) 한 칸이라 동시 보유 시 귀속·청산 수량을 정할 수 없다. 지금은 결정 초안 ② 배타로 피한다. 재개 조건: 동시 보유가 필요한 전략이 합격하거나 D-128 계좌 샤딩 원장을 착수할 때.
3. **슬리브 간 선점 우선권** — DevScale이 먼저 등록한 종목을 VWAP 눌림이 넘겨받을 규칙이 없다. 재개 조건: vwappb_preempted_ratio가 10거래일 평균 30% 초과.

기존 항목은 `docs/DEFERRED_ISSUES.md` D-17(WS 칸 상한·REST 대체)만 걸린다.

## 10. 우회로

| 막히는 곳 | 1 | 2 | 3 | 추천 |
|---|---|---|---|---|
| armed 종목이 WS 칸을 못 받음 | REST 폴링 | armed에 랭크 부여(엔진 편집) | max_armed 3 | 1. 일치율 90% 이상이면 그대로 |
| 시세판 끊김 | 진입 중지·청산 계속 | 틱 VWAP | 분봉 typical | 1. 정의를 바꾸면 백테스트와 어긋남 |
| 선점으로 표본 부족 | VWAP 로더 우선 | DevScale 재스캔에서 VWAP 후보 제외 | 그대로 두고 재계산 | 3부터, 30% 넘으면 2 |
