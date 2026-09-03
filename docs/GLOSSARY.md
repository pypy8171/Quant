# GLOSSARY — 용어·약어 사전

이 저장소는 전략·인프라·데이터에 영어 약어와 축약 코드명을 많이 쓴다. 처음 보는 사람이 `DevScale`, `ITB`, `dev_buy`, `reconcile` 같은 단어를 코드·로그·일지에서 만났을 때 **무슨 개념인지 한 곳에서 찾을 수 있도록** 모은 사전이다.

- 파일 경로 참조는 백틱(예: `Quant/include/strategy/DeviationScaleStrategy.h:14`)으로 적는다 — 코드가 이동하면 줄번호는 어긋날 수 있으니 **개념 위치의 힌트**로만 본다(정합 검사는 마크다운 링크만 대상).
- 두 허브: 리서치·백테스트는 [research/README.md](../research/README.md), 전략 스펙·실증은 [strategies/README.md](../strategies/README.md).
- 일부 항목은 코드에 명문 정의가 없어 관례상 확장을 적었고, 그런 경우는 "(관례)"로 표시했다.
- **표기 규약**: 문서·표·코드 주석에서 약어를 처음 쓸 때는 **쉬운말(약어)** 로 병기한다(예: `중앙값(p50)`·`최대낙폭(MDD)`·`전 구간(E2E)`). 표 헤더도 병기하고, 같은 문서 안 반복 등장은 약어만 써도 된다. 코드에서는 **주석에만** 병기하고 식별자·필드명은 그대로 둔다. 병기 표준어는 아래 표(특히 [성능·지연](#성능지연-latency--throughput)·[백테스트·리서치](#백테스트리서치-backtest--research))를 따른다.

---

## 전략 (Strategy)

프로젝트에 존재하는 전략은 13종이다(C++ 구체 전략 10 + Python 리서치 3). StrategyBase는 공통 인터페이스라 전략 수에서 제외한다.

| 약어 / 코드명 | 풀네임 | 정의 | 어원·주의 | 대표 위치 |
|---|---|---|---|---|
| **DevScale** / `DEVIATION_SCALE` | DeviationScaleStrategy | 일봉 정배열(SMA5>10>20>60)+눌림 존 게이트 안에서 3분봉 **이격도** 사다리로 지정가 분할매매 | dev = **deviation(이격도)**, development 아님. 시장가가 아니라 "기다리는" 지정가 예약 | `Quant/include/strategy/DeviationScaleStrategy.h:14` |
| **ITB** | IntradayBreakout (v2) | 1분 버킷 채널 돌파 + 당일 시가앵커 기반 장중 자동매매 | 클래스명 `IntradayBreakoutStrategy`, `id()="ITB_"`. 세 글자 확장(In**t**raday **B**reakout)은 관례. 분봉 시점정합 재현 불가 → forward 실증만 | `Quant/include/strategy/IntradayBreakoutStrategy.h:11`, `strategies/README.md` |
| **MM** / MM-1 | MarketMakingStrategy | mid±half_spread_ticks 양방향 지정가를 걸고 시장이 움직이면 취소·재호가하는 미니 시장조성기 | MM = Market Making. CANCEL+NEW 방식(REPLACE 미사용), 재고 미인지(Phase 1) | `Quant/include/strategy/MarketMakingStrategy.h:10` |
| **Momentum** | MomentumStrategy | N일 고점 돌파 매수 / N일 저점 이탈 청산 | 돈치안 채널 브레이크아웃(Donchian) | `Quant/include/strategy/MomentumStrategy.h:7` |
| **SDP** | SupplyDemandPullbackStrategy | 외인·기관 쌍끌이 수급 선별 + 5일선 눌림목 진입(EOD 스윙 / INTRADAY 두 모드) | "쌍끌이" = 외인>0 AND 기관>0. look-ahead 방지로 당일 확정치 제외 | `Quant/include/strategy/SupplyDemandPullbackStrategy.h:18` |
| **ValueContrary** | ValueContraryStrategy | 저PBR 종목 3일 연속 하락 후 반전 매수(4일차 시가 효과) | Contrary = 역발상. KR은 KIS 시총순위, US는 내장 S&P500 | `Quant/include/strategy/ValueContraryStrategy.h:12`, `PYQuant/strategy/value_contrary.py` |
| **Theme** | ThemeStrategy | 업종 5일 모멘텀 상위 → 급등종목 → 외인+기관 순매수 3단 필터 테마 모멘텀 | 업종코드 테이블 내장 | `Quant/include/strategy/ThemeStrategy.h:13` |
| **MACross** | MACrossStrategy | 단기 MA가 장기 MA 상향돌파(골든크로스) 매수 / 하향(데드크로스) 매도 | MA = Moving Average | `Quant/include/strategy/MACrossStrategy.h:7` |
| **FixedInterval** | FixedIntervalStrategy | interval_sec마다 BUY↔SELL 교대 발행 | 지표 무관, 파이프라인 강제 매매 검증용 | `Quant/include/strategy/FixedIntervalStrategy.h:8` |
| **PriceTarget** | PriceTargetStrategy | 가격 도달 시 시장가 + 예약 지정가 주문 | cooldown_sec으로 동일방향 중복 차단 | `Quant/include/strategy/PriceTargetStrategy.h:9` |
| **CrossMomentum** | CrossMomentumStrategy (py) | 유니버스 **횡단면** 12-1 모멘텀 랭킹 상위 top_n 동일가중, 주기 리밸런싱 | cross-sectional = 횡단면(종목 간 상대비교), 시계열 모멘텀과 구분. 12-1 = 최근 skip일 제외 | `PYQuant/strategy/cross_momentum.py` |
| **MeanReversion** | MeanReversionContraryStrategy (py) | SMA 아래로 가장 벌어진 top_n 매수(과매도 반등 가설), CrossMomentum의 부호 반대 미러 | apples-to-apples 비교용 베이스라인 | `PYQuant/strategy/mean_reversion.py` |
| **SupplyDemandRank** | SupplyDemandRankStrategy (py) | 외인+기관 누적순매수/거래대금 정규화 score 상위 top_n 동일가중 | 수급 단독 횡단면, "결과 floor" 측정용 | `PYQuant/strategy/supply_demand_rank.py` |
| **StrategyBase** | StrategyBase | 모든 전략이 상속하는 인터페이스(id/describe/on_data/on_order_book…) | 신규 전략 추가 진입점 | `Quant/include/strategy/StrategyBase.h:14` |

---

## 인프라 (Infrastructure)

| 용어 | 정의 | 어원·주의 | 대표 위치 |
|---|---|---|---|
| **RingBuffer** | SPSC 락-프리 링버퍼 큐(데이터→전략→주문 파이프라인 연결) | SPSC = 단일생산자·단일소비자. `std::atomic` 명시적 메모리순서 | `Quant/include/core/RingBuffer.h` |
| **Engine** | 4-스레드(데이터/전략/주문 파이프라인 + 제어) 오케스트레이터 | rest_price_feed 분기·WS 연결을 여기서 관리 | `Quant/src/core/Engine.cpp` |
| **OrderGate** | 주문 통과 게이트 — 포지션 한도·rate limit·손실컷을 검증하고 확정포지션 원장을 보유 | confirmed_position이 전략 포지션의 진실원천 | `Quant/src/risk/OrderGate.cpp` |
| **OrderRouter** | order 스레드에서 실제 KIS 주문을 실행·라우팅(new_route/on_fill) | 거부(REJECTED) 시 drop, 재큐잉 없음(C++). 체결콜백 on_fill로 원장 갱신 | `Quant/src/ipc/OrderRouter.cpp` |
| **reconcile** (리컨사일) | 로컬 원장 ↔ KIS 실잔고를 재조회로 재동기 | rest 모드처럼 체결콜백이 없을 때 손익 근사 경로 | `Quant/src/core/Engine.cpp` |
| **kill switch** | 신규·청산 양방향 하드스톱 스위치 | ZMQ 수동명령 / WS 연속 실패로 발동(손익기반 자동킬은 미구현) | `Quant/src/risk/OrderGate.cpp` |
| **entry_halt** | 신규 진입(BUY)만 차단, 청산(SELL)은 허용하는 플래그 | **OrderGate 전역 플래그**라 켜지면 모든 전략의 신규진입이 함께 막힌다. 매크로 사이드카 regime.json 파일브리지가 토글(→ 구조 국면 `RegimeController`와 다른 축) | `Quant/src/risk/OrderGate.cpp` |
| **RegimeController** | 장 시작 1회 지수 종가>200MA(±1)+정배열(ma20>ma60>ma120)/역배열(±1)로 `score∈{-2..+2}`를 매겨 BULL/NEUTRAL/BEAR/UNKNOWN 판정 | 매크로 사이드카(regime.json)와 **별개 축**인 구조 국면. config `regime_strategies`로 국면별 전략 집합 자동선택 | `Quant/include/core/RegimeController.h` |
| **Regime / RegimeSnapshot** | 국면 enum(BULL/NEUTRAL/BEAR/UNKNOWN) + 판정 스냅샷 구조체(date·score·200MA·정배열/역배열·지수 이평 분해) | RegimeController가 산출, 학습입력·설명·디버깅용 개별지표 분해 저장 | `Quant/include/core/Types.h` |
| **FORCE_LIQ** | BEAR 등에서 보유 전량을 시장가로 청산하는 강제청산 신호 | `strategy_id="FORCE_LIQ"`. 시장가라 명목 백스톱 우회 방지로 평단을 `ref_price`에 stamp | `Quant/src/core/Engine.cpp` |
| **UniverseScanner** | 시총·거래대금·등락률 필터로 매매 유니버스를 스캔(scan_devscale / scan_itb) | 정배열 프로브·수급 필터 포함 | `Quant/include/universe/UniverseScanner.h:16` |
| **StrategyFactory** | config를 읽어 전략 인스턴스를 생성·등록하는 팩토리 | main.cpp에서 분리된 전략 로딩 계층 | `Quant/src/strategy/StrategyFactory.cpp` |
| **Logger** | 비동기 싱글톤 로거(ms UTC 타임스탬프, 콘솔 + `logs/quant_trader.log`) | hot path는 큐 push만·전용 writer 스레드가 I/O(tail-latency 억제). 백프레셔(kMaxQueue 초과 시 드롭+드롭카운트)·`flush()`. LOG_INFO/WARN/ERROR/DEBUG 매크로 | `Quant/include/utils/Logger.h` |
| **bootstrap_ledger** | 기동 시 실계좌 보유분을 OrderGate 원장에 시드(매도수량·평단·손실한도 정합) | config `bootstrap_ledger_from_balance` | `Quant/src/main.cpp` |
| **manage_holdings** | 스캔 유니버스 밖 잔고 보유분에 "청산 전용" 가디언을 부착(신규진입 영구차단) | config `manage_holdings` 블록 | `Quant/config` 전략 블록 |
| **ZmqBridge / OrderRouter(IPC)** | ZeroMQ 기반 프로세스 간 시세·주문 중계(선택 구성) | Python 오퍼레이터 연동 | `Quant/src/ipc/ZmqBridge.cpp` |
| **OrderSignal / MarketData** | 전략이 산출한 주문신호(side/type/qty/price/ref_price) / OHLCV+bar_index 시세 | 파이프라인 코어 타입 | `Quant/include/core/Types.h` |
| **ref_price** | 시장가(price=0) 주문의 명목 한도(max_notional_per_order/per_ticker) 평가 기준가 | 지정가는 price로 명목 평가, 시장가는 이 값으로 — 시장가의 백스톱 우회 차단. FORCE_LIQ 매도는 평단을 stamp | `Quant/include/core/Types.h` · `Quant/src/risk/OrderGate.cpp` |

---

## 데이터·시장 (Data / Market)

| 용어 | 풀네임 | 정의 | 어원·주의 |
|---|---|---|---|
| **OHLCV** | Open/High/Low/Close/Volume | 시가·고가·저가·종가·거래량 봉 데이터 | 표준 봉 5요소 |
| **SMA** | Simple Moving Average | 단순이동평균(SMA5>10>20>60 정배열 게이트의 기준선) | dev(이격도)의 기준 |
| **PIT** | Point-In-Time | 그 시점에 실제로 알 수 있던 값만 사용(미래참조 방지) | 3분봉 PIT 재현 불가로 DevScale 백테스트 제외 |
| **look-ahead** | look-ahead bias | 미래 정보 누설 편향 | 결정은 당일 종가, 체결은 다음봉 시가로 방지 |
| **survivorship** | survivorship bias | 생존편향 — 살아남은 종목만 유니버스에 남아 성과가 부풀려지는 편향 | 정적 유니버스 백테스트의 상시 주의 |
| **approval key** | WebSocket approval key | KIS 실시간 WS 접속용 승인키(REST로 발급) | OAuth 토큰과 별개. 재연결 폭주 시 재발급 이슈 |
| **OAuth2 / bearer** | — | KIS REST 인증 토큰 발급·캐싱(bearer 헤더) | approval key와 다른 축 |
| **H0STASP0** | KIS 실시간 호가 채널 | 5단계 호가 실시간 스트림 | ASP = 호가 |
| **H0STCNT0** | KIS 실시간 체결 채널 | 실시간 체결(틱) 스트림 | CNT = 체결 |
| **H0STCNI0 / H0STCNI9** | KIS 체결통보 채널 | 내 주문의 체결통보(0=실전, 9=모의) | CNI = 체결통보. 미구독 시 실현손익 원장 공백 |
| **ODNO** | 주문번호(Order Number) | KIS 접수 시 반환되는 주문번호 | 접수 확인용(체결과 무관) |
| **ORGNO** | 원주문번호(Original Order No) | 정정·취소 시 참조하는 원주문 번호 | — |
| **TR id** | Transaction ID | KIS API 거래 식별코드(FHKST·FHPST·FHPUP·TTTC…) | 모의/실전·조회유형별로 다름 |
| **^KS11 / ^KQ11** | KOSPI / KOSDAQ 지수 티커 | yfinance 등에서 쓰는 지수 심볼 | ^KS11=코스피, ^KQ11=코스닥 |
| **KOSPI / KOSDAQ 지수코드** | `0001` / `1001` | KIS 지수·랭킹에서 시장을 가리키는 코드(0000=전체, 2001=코스피200) | 지수 조회는 U-namespace |
| **SOX** | 필라델피아 반도체지수 | 미국 반도체 지수(국내 반도체주 선행지표로 관찰) | 위기·SOX선행 전략 맥락 |
| **VIX / VKOSPI** | 변동성지수 | 미국(VIX)·코스피(VKOSPI) 변동성지수 | 위험국면 관찰 |
| **mrktCtg** | Market Category | data.go.kr 응답의 시장 구분 리터럴("KOSPI"/"KOSDAQ"/"KONEX") | 유니버스 시장 태깅 |

---

## 설정·신호 (Config / Signal)

| 용어 | 정의 | 어원·주의 |
|---|---|---|
| **dev_buy / dev_sell** | 이격도가 아래로/위로 벌어진 층에서 지정가 매수/매도(%) | dev = deviation. −dev_buy%=재진입, +dev_sell%=분할익절 |
| **pullback** (pullback_pct) | SMA ±pullback_pct 이내 눌림목 존 판정 폭 | 눌림목 게이트 |
| **rung / n_rungs** | 이격도 사다리 분할매매의 각 층(가격대) / 층 수 | 물타기 총예산을 n_rungs로 분할 |
| **reprice** (reprice_move_ticks) | 미체결 사다리를 CANCEL+NEW로 재호가 | SMA가 지정 틱 이상 이동 시 |
| **requote** (half_spread_ticks·min_requote_ms) | 시장 이동 시 양방향 견적 재호가·반스프레드·최소간격 | 간격 AND 이동폭 동시조건으로 churn 억제 |
| **churn** | 과잉 재주문(재호가 폭주) | 데드밴드/최소간격 가드로 억제 |
| **notional** | 주문 명목 금액(자본%×총평가 → 수량 산출) | 명목 사이징 백스톱 |
| **TARGET_WEIGHT** | 수량 대신 목표비중을 지정하면 엔진이 동일가중 사이징 | Python 횡단면 전략용 order_type |
| **require_aligned** | 스캔 단계에서 일봉 정배열 종목만 등록 | 신규상장·역배열 제거 |
| **risk_off_index_pct** | 지수 등락률이 이 값 이하면 신규 미등록 | 코스피/코스닥 시장별 이중 게이트 |
| **max_dev_pct** | 정배열이어도 이격 초과면 유니버스 제외(과확장 컷) | 분수 단위(전략 임계는 퍼센트) |
| **avg_loss_pct** | 보유분 손절 임계(%) | 0이면 손절 비활성 |
| **eod_exit_hhmm** | 장 마감 강제청산 시각(HHMM) | EOD 청산 |
| **EOD** | End Of Day, 장 마감(청산·스윙 모드 기준) | — |
| **rest_price_feed** | WS 대신 REST 현재가 폴링을 체결 하트비트로 사용 | true면 reconcile 필수 |
| **is_paper** | 모의(true, openapivts:29443) / 실계좌(false) 스위치 | 시세·주문 도메인 분기 |
| **fetch_interval_sec** | 데이터 폴링 주기(초) | 장외 시간은 스킵 |
| **regime / regime.json** | 매크로 사이드카(`macro_regime_feed.py`)가 쓰는 위험국면 파일브리지 | risk_score 낮으면 entry_halt 토글. ※ 장시작 구조 국면 판정은 별도 축 → `RegimeController` 참조 |
| **regime_strategies / regime_reeval_sec** | 국면(BULL/NEUTRAL/BEAR)별 전략 집합을 자동선택하는 config 맵 / 재평가 주기(기본 300s) | 지정 시 국면이 전략셋을 선택, 미지정 시 전략별 active_regimes 하위호환 |
| **dedup** (dedup_window_sec) | 동일 전략+종목 중복주문 제거 창 | 1초 내 중복 거부 |

---

## 백테스트·리서치 (Backtest / Research)

성과지표·검증 개념. BT 일련번호·계열 프리픽스(M/N/O/C/H 등)의 상세 결론은 리서치 허브 [research/README.md](../research/README.md)와 전략별 폴더에 있다. 일부 계열 라벨은 비공개 실험 문서(로컬전용)에서 관리되어 여기서는 **개념 정의**만 둔다.

| 용어 | 풀네임 | 정의 | 주의 |
|---|---|---|---|
| **Sharpe** | Sharpe ratio | 총변동 대비 위험조정수익 | 채택 게이트 지표 |
| **Sortino** | Sortino ratio | 하방변동만 벌점한 위험조정수익 | — |
| **MDD** | Max Drawdown | 최대 낙폭 | 채택 게이트의 1차 앵커 |
| **PF** | Profit Factor | 총이익 ÷ 총손실 | — |
| **slippage** | 슬리피지 | 체결가와 기대가의 괴리(비용 가정) | 백테스트 비용모델 |
| **OOS** | Out-Of-Sample | 표본외 검증 구간 | — |
| **holdout** | 홀드아웃 | 결론 판정에서 격리해 둔 검증 구간 | 2022 홀드아웃 등 |
| **walk-forward** | walk-forward | 롤링으로 재적합하며 전진 검증 | — |
| **ablation** | 절제실험 | 요소를 하나씩 제거해 기여도 측정 | BT-10 EOD ablation |
| **regime filter** | 국면필터 | 지수 국면으로 신규진입을 게이팅하는 레버 | 지금까지 견고성 확인된 유일 레버 |
| **regime_scorer** | 구조 국면 스코어러 | C++ `RegimeController`를 미러(변형 A)하고 연속화/기울기/오버레이로 확장한 Track A 애블레이션 | `PYQuant/backtest/regime_scorer.py`, study [10](../research/studies/10_regime_scorer/README.md) |
| **index_intraday_logger** | 장중 지수 forward 로거 | 장중 지수(0001/1001/2001) 30s append-only JSONL 적재 — 지수 PIT 히스토리 부재로 백테스트 불가한 Track B의 유일 검증경로 | `PYQuant/tools/index_intraday_logger.py` |
| **BT-NN** | Backtest #NN | 백테스트 일련번호(예: BT-08 위기대응, BT-09 위기전략 10종, BT-10 저점매수) | 상세는 research 허브 |
| **LAB** | STRATEGY_LAB | 전략 실험 가설·판정 카탈로그(로컬전용 문서) | GitHub 미포함 |

> 계열 프리픽스(M=위기 대응법, N=신규 전략, O/C=위기 전략 후보, H=가설)의 개별 결론과 file:line은 리서치 허브·전략 폴더에서 확인한다. 확장 명문 정의가 코드/공개문서에 없는 라벨은 여기서 단정하지 않는다.

---

## 성능·지연 (Latency / Throughput)

파이프라인 지연·처리량 측정에서 쓰는 약어. 지연은 "평균"이 아니라 **분위수(percentile)** 로 본다 — 100건을 느린 순으로 줄 세웠을 때 몇 번째냐다. 상세는 `docs/reports/PIPELINE_LATENCY_REPORT.md`(별도 인프라 커밋 예정).

| 약어 | 쉬운말(병기 표준) | 정의 | 주의 |
|---|---|---|---|
| **p50** | 중앙값(p50) | 절반이 이보다 빠른 값(50분위) | 보통 얼마나 걸리나 |
| **p95** | 상위 5%(p95) | 느린 5%가 시작되는 값(95분위) | — |
| **p99** | 상위 1%(p99) | 느린 1%가 시작되는 값(99분위) | 꼬리 진입 |
| **p999** | 상위 0.1%(p999) | 느린 0.1%가 시작되는 값(99.9분위) | p99.9와 동의 |
| **max** | 최악(max) | 관측된 가장 느린 값 | 1건짜리 극단치라 해석 주의 |
| **tail** | 꼬리 지연(tail) | 가장 느린 소수 구간(p99~max) | 저지연 설계가 실제로 다루는 대상 |
| **E2E** | 전 구간(E2E) | 입력~출력 끝까지 걸린 총 지연(end-to-end) | net/proc 합 |
| **net / proc / e2e** | 수신(net)/처리(proc)/전구간(e2e) | 구간 분해 — 소켓 수신·내부 처리·전체 | 병목 위치 분해용 |
| **throughput** | 처리량(throughput) | 단위시간당 소화한 건수 | — |
| **msg/s** | 초당 건수(msg/s) | 초당 메시지 수 | — |
| **offered / achieved** | 투입(offered)/소화(achieved) | 넣어준 rate / 실제 처리한 rate | 둘이 같아야 무손실 |
| **drops** | 유실(drops) | 버린 메시지 수 | 0이 정상 |
| **ns / µs / ms** | 나노초(ns)/마이크로초(µs)/밀리초(ms) | 10억분의 1 / 100만분의 1 / 1,000분의 1초 | 1 µs=1,000 ns, 1 ms=1,000 µs |
| **SPSC** | 단일생산자·단일소비자(SPSC) | 한 스레드가 넣고 한 스레드가 빼는 락-프리 큐 구조 | RingBuffer의 전제 |

---

*이 사전은 새 전략·개념을 추가할 때 함께 갱신한다. 소스 위치가 바뀌면 백틱 참조의 줄번호는 힌트로만 본다.*
