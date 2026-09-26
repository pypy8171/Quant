# 데이터 출처 — 어느 플랫폼에서 무엇을 받는가

2026-09-14 기준, 코드와 당일 로그로 확인한 것만 적는다. 숫자는 그날 실측이라 다음 날 달라질 수 있다.
바뀐 이유는 [DECISIONS.md](DECISIONS.md)(D-054 테마, D-062 폴러, D-071 피드 구조)와 메모리 `data_source_constraints`.

## 한눈에

| 플랫폼 | 받는 것 | 규모·주기 | 만드는 파일 / 쓰는 코드 |
|---|---|---|---|
| **data.go.kr** (금융위 `getStockPriceInfo`) | 전 종목 T-1 시세 스냅샷 — 시총·거래대금·종가. ETF/ETN은 구조적으로 없음 | 코스피+코스닥 2,765종목 스냅샷에서 시장별 시총 top100 ∪ 거래대금 top100 → 오늘 277종목. 장 전 하루 1회(`scripts/auto_trade_day.ps1`가 돌림). 공시가 1~3영업일 늦어 08시 스캔은 T-2를 받기도 한다 | `PYQuant/tools/universe_feed.py` → `Quant/config/universe_scan.json`(`universe` 277 + `market_map` 2,765). 전 종목 코드 덤프는 `PYQuant/tools/full_universe_dump.py` → `Quant/config/universe_full.json` |
| **네이버 증권** (비공식, 키 없음) | ① 전 종목 장중 시세 — 현재가·누적거래량·누적거래대금 ② 테마(인포스탁 분류) 목록·구성 종목 | ① `market_map`의 2,683종목을 900개씩 요청 3개(병렬)로 5초마다 — 한 요청 1,000종목까지 받고 1,500부터 HTTP 400(09-26 실측, D-142) ② 하루 1회 | ① `scripts/live_prices_feed.py` → `Quant/config/prices_live.json`(엔진 재스캔의 현재가, 09:30 뒤 `universe_feed.py --live-prices`가 거래대금 축을 이 값으로 바꿈) ② `PYQuant/tools/fetch_naver_themes.py`·`PYQuant/naver/theme.py` → `PYQuant/data/themes/latest.json` |
| **KIS REST** | 순위 3축(시총 `FHPST01720000`·거래대금 `FHPST01710000`·업종별 등락률 `FHPST01700000`, 축마다 30행 상한), 종목 일봉·분봉·현재가, 지수 일봉·현재값, 잔고·미체결·주문 | 순위는 기동·재스캔마다, 일봉은 종목·일 1회 캐시(`align_lookup_max` 800), 현재가 폴링은 WS에서 밀린 종목만 | `Quant/src/api/KisUniverse.cpp`(순위·업종), `KisMarket.cpp`(봉·현재가), `KisIndex.cpp`(지수), `KisAccount.cpp`·`KisOrder.cpp`. 폴링은 `Quant/include/core/DataPoller.h` |
| **KIS WebSocket** | 실시간 체결(`H0STCNT0`, 통합 `H0UNCNT0`)·호가(`H0STASP0`, 통합 `H0UNASP0`)·체결통보 | 세션당 구독 40건(`kMaxWsSubs`, 문서상 41). 오늘: 구독 대상 57종목 중 체결통보 1 + 시세 39, 나머지 18종목은 REST 폴링으로 대체 | `Quant/src/api/WebSocketClient.cpp`. 소켓을 더 달면(`feed_keys`) `FeedMux`가 상한을 소켓 수만큼 늘린다 |
| **Yahoo chart** (무료, FDR 폴백) | 매크로 국면 입력 — 코스피·코스닥·나스닥 선물·S&P 선물·VIX·10년물·USD/KRW·WTI 현재가(표), FRED 30Y·2Y·하이일드(참고) | 3분마다 갱신, `regime.json` 파일 전달 | `PYQuant/tools/macro_regime_feed.py` → `Quant/config/regime.json`(엔진 `entry_scale` 매수 비율·`entry_halt`·`force_liquidate`) |

## 기억과 다른 점

- **yfinance는 "제거"가 아니라 "라이브 경로에서 빠짐"이다.** 이 환경에서 야후 크럼 SSL이 막혀 전 심볼 실패라 `macro_regime_feed.py`는 FDR로 바꿨다.
  파일은 남아 있다 — `PYQuant/data/yfinance_source.py`(백테스트 `PYQuant/main.py`가 import), `PYQuant/data/index_source.py`(백테스트 `PYQuant/backtest/engine.py`의 지수 국면).
  백테스트 어댑터는 FDR로 옮기지 않았다(메모리 `project_yfinance_dead_fdr`). 라이브 매매에는 yfinance 호출이 없다.
- **네이버 2,700종목**은 맞다(오늘 2,683). 다만 "섹터군별 종목"은 네이버가 아니라 **KIS 업종별 등락률 순위**(`sector_codes` 28업종, 축당 30행)가 준다. 네이버가 주는 분류는 **테마**(인포스탁)이고 유니버스 후보 축이 아니라 조회·리포트용이다.
- **data.go.kr**는 "거래대금 상위 100 ∪ 시총 100"이 맞고, 시장별로 따로 뽑아 합친다(`--market ALL`). 그래서 후보는 최대 400이 아니라 오늘 277이다.
  ETF가 없는 게 이 축을 넣은 이유다(KIS 순위는 ETF가 절반을 먹었다).
- **KIS 구독은 "40종목"이 아니라 "구독 40건"**이다. 체결통보가 1건을 먹고, 호가까지 받는 종목은 2건을 쓴다. 지금은 시세를 체결만 받아(`trade_only`) 39종목이 실시간이고 넘친 종목은 REST 현재가 폴링(`DataPoller`)으로 간다.
- 실시간 지수는 KIS만 준다. 장중 지수 히스토리는 어디에도 없어 `PYQuant/tools/index_intraday_logger.py`가 KIS로 직접 쌓는다.

## 유니버스가 만들어지는 순서

1. 장 전 `universe_feed.py` — data.go.kr 스냅샷 → 시장별 시총∪거래대금 → `universe_scan.json`(축 4).
2. 기동 시 엔진 `UniverseScanner` — KIS 순위 3축(시총·거래대금·업종 등락률) + 파일 축을 합쳐 후보를 만들고, KIS 일봉으로 정배열·이격을 판정해 `max_universe`(100)까지 등록한다.
3. 장중 `live_prices_feed.py`가 5초마다 네이버 시세를 갈아 끼우고, 엔진은 `rescan_interval_sec`마다 캐시된 일봉 + 이 파일 현재가로 재판정한다(REST 0). 감시견이 1~2분마다 `universe_feed.py`를 다시 돌려 시총·거래대금 축을 당일 값으로 재랭킹한다 — 시세는 `prices_live.json` 재사용이라 외부 조회 0(D-142).
4. 등록 종목 중 40건까지 KIS WS 실시간, 나머지는 REST 폴링.

## 키·환경

- data.go.kr: 환경변수 `DATA_GO_KR_KEY`(없으면 즉시 에러).
- KIS: `Quant/config/config_dev_paper.json`(모의)·`Quant/config/config.json`(실계좌)의 `app_key`·`app_secret`. 토큰 캐시 `Quant/config/kis_token_*.json`은 파이썬 `PYQuant/kis/client.py`와 엔진이 공유한다.
- 네이버·FDR: 키 없음. 비공식 경로라 형식이 바뀌면 끊긴다 — 호출자는 마지막 성공 파일로 버틴다.

## 리셋 회의(D-101, 2026-09-19) 뒤 새로 확인한 경로

백테스트 데이터 마트(`research/RESET_2026-09-19.md` §2-C)에 쓰려고 실호출로 확인했다. 어느 것도 키가 없다.

| 경로 | 주는 것 | 깊이·PIT | 상태 |
|---|---|---|---|
| `m.stock.naver.com/api/stock/{code}/trend?bizdate=YYYYMMDD&pageSize=60` | 일별 외국인·기관 순매수, 외국인 보유 주식수·비율 | 2009-10~, 보유 주식수는 그날 값이라 PIT-A | 살아 있음 → `PYQuant/tools/naver_flow_backfill.py`(R-2) |
| `fchart.stock.naver.com/siseJson.naver` | 일봉 OHLCV | 1990~ | 살아 있음 → `PYQuant/tools/naver_bars_backfill.py`(R-2, 2020 이전 일봉 보강) |
| `finance.naver.com/item/frgn.naver`(구 HTML) | (예전 외국인 보유비율 경로) | — | **2026-09 302로 죽음**. 위 trend API로 바꿨다 |
| KIND `kind.krx.co.kr/corpgeneral/delcompany.do`(POST) | 상장폐지 종목·일자 | 전 기간 | 살아 있음. FDR 상폐 목록(995행)과 교차 확인용 |
| `m.stock.naver.com/api/research/company`, `…/api/stock/{code}/integration`(`consensusInfo`) | 증권사 리서치 목록·목표가 컨센서스 | 게시일 기준 PIT-A | 살아 있음. 대시보드 리서치 뷰어(R-7) 입력 후보 |

- TimescaleDB(5432)는 이 PC에서 닫혀 있어 마트 저장은 parquet(`PYQuant/data/`)이 1순위다.
- 아직 키를 못 받은 것 4건(FRED·ECOS·DART·관세청)은 §5 오너 결정에 올라가 있다.
