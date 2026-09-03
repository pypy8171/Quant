# PIPELINE A-to-Z — 코드 레벨 파이프라인 흐름 문서

이 문서는 C++ 퀀트 트레이딩 엔진(`Quant/`)의 **TRADE 모드 실행 경로**를 파일·함수·라인 단위로 추적한다. 모든 주장에는 `파일:라인` 근거가 달려 있으며, 데이터가 큐/콜백을 넘을 때 "무엇이 무엇으로 변환되는가"를 명시한다.

> 시크릿(app_key/app_secret/access_token/account_no)은 `Quant/config/*.json`에 평문으로 존재하나(예: `config.json:3-5`), 본 문서에는 값을 옮기지 않는다. 존재 사실만 언급한다.

---

## 0. 개요 — 3+1 스레드 락프리 파이프라인

TRADE 모드의 데이터 흐름:

```
                          get_daily_ohlcv (REST, 60s 폴링)
                                    │  MarketData
   [DataThread] ───────────────────┼──────────────▶ market_queue_(RingBuffer<MarketData>, 1024)
                                    │
   KIS WebSocket 수신 스레드         │
   (Engine 소유 아님, ws_ 내부 recv_thread_)
     H0STASP0 ─▶ OrderBook  ───────────────────────▶ ob_queue_(RingBuffer<OrderBook>, 4096)
     H0STCNT0 ─▶ TradeData  ───────────────────────▶ td_queue_(RingBuffer<TradeData>, 4096)
     H0STCNI9 ─▶ FillNotification ─(콜백)──────────▶ OrderRouter::on_fill (원장 갱신)

   [StrategyThread]  ob_queue_ → td_queue_ → market_queue_ 우선순위로 pop
        │  각 전략의 on_order_book / on_trade / on_data 호출
        │  OrderSignal 생성
        ▼
   order_queue_(RingBuffer<OrderSignal>, 256)
        │
   [OrderThread]  order_queue_ pop → OrderRouter::submit
        │  OrderGate::check(11검사) → KisClient::submit_order_ack(REST)
        ▼
   KIS 주문 API → ODNO 수신 → gate_.on_accept (reserved_ 선점)

   [ControlThread]  5초 주기로 running_ 감시 + WS stale 재연결 백오프
```

- 스레드 4개는 `Engine::start()`에서 기동된다: `data_thread_`, `strategy_thread_`, `order_thread_`, `control_thread_` (`Engine.cpp:138-141`).
- 큐는 전부 `RingBuffer<T>` — **SPSC(단일 생산자/단일 소비자) 락프리** 큐. `head_`(producer 전용)와 `tail_`(consumer 전용)을 별도 캐시라인에 배치해 false sharing을 회피한다 (`RingBuffer.h:95-98`). `push`는 release, `pop`은 acquire 메모리 순서를 쓴다 (`RingBuffer.h:43,65`). 슬롯 1개를 full/empty 구분용으로 비워둔다 (`RingBuffer.h:27`).
- SPSC 계약상 각 큐는 **생산자 1 / 소비자 1**이어야 한다. 실제로 `market_queue_`는 DataThread가 유일 생산자, `ob_queue_`/`td_queue_`는 WS 수신 스레드가 유일 생산자, 이 셋 모두 StrategyThread가 유일 소비자다. `order_queue_`는 StrategyThread 생산 / OrderThread 소비 → 계약 성립.

---

## 1. 진입(main) → 설정 로드 → 전략 등록

### 1.1 진입점과 인자 파싱
- `main()` 진입: `main.cpp:184`. Windows 콘솔 UTF-8/ANSI 설정 후 (`main.cpp:186-192`) `Logger::instance().init("logs/quant_trader.log", INFO)` (`main.cpp:194`, logs/ 하위 고정·부모폴더 자동생성).
- 인자 파싱: `quant_trader [config] [MODE]`. `KR_TEST/US_TEST/FEED/TRADE`는 `mode_override`로, 그 외 토큰은 `config_path`로 해석 (`main.cpp:202-209`).
- 설정 로드는 `load_config()` — `std::ifstream` + `json::parse` (`main.cpp:43-49`, 호출 `main.cpp:214`). 즉 설정 파서는 nlohmann/json 직접 사용이며, `Quant/src/utils/Config.cpp`는 **빈 placeholder**(`Config.cpp:1`)다 — 파싱 로직이 여기에 없다.

### 1.2 config.json 스키마
`config.json` 최상위 키:
- `kis` 객체: `app_key`, `app_secret`, `account_no`, `account_type`("01"), `hts_id`, `is_paper`(bool) — 파싱 위치 `main.cpp:230-236`. `hts_id`는 체결통보 채널 구독 키로 쓰이며 미설정 시 빈 문자열 (`main.cpp:235`).
- `mode`: "FEED"/"TRADE"/"KR_TEST"/"US_TEST" (`main.cpp:246`).
- `fetch_interval_sec`: DataThread 폴링 주기, 기본 60 (`main.cpp:864`).
- `tickers`: FEED 모드 전용 배열, TRADE 모드에서는 전략이 동적으로 종목을 구성하므로 불필요 (`main.cpp:239-241`).
- `strategies`: 전략 배열. 각 원소는 `type` + 전략별 파라미터.
- `is_paper=true`이면 모의투자 엔드포인트를 쓴다 (`config_mm_paper.json:8`). `is_paper=false`는 실거래 (`config.json:8`).

### 1.3 전략 등록 루프
`for (auto& s : cfg["strategies"])` (`main.cpp:868`)에서 `type` 분기로 전략 객체를 생성해 `engine.add_strategy()`로 등록. 지원 타입:
- `MA_CROSS` (`main.cpp:874-916`) — `short_period`, `long_period`, `quantity`. **`universe_from_balance=true`** 모드가 특수(`main.cpp:879-910`): 별도 `KisClient bal_kis`로 인증 후 `get_balance()`로 모의계좌 보유종목을 읽어(`main.cpp:890`), `output1` 배열의 `pdno`(종목)·`hldg_qty`(보유수량)마다 `MACrossStrategy(code, sp, lp, hq, start_in_position=true)`를 등록한다 (`main.cpp:894-905`). 즉 보유분을 "이미 진입한 상태"로 시드해 데드크로스에 실제 보유수량을 매도할 수 있게 한다.
- `MOMENTUM` (`main.cpp:917-921`), `VALUE_CONTRARY` (`main.cpp:922-930`), `FIXED_INTERVAL` (`main.cpp:931-938`), `PRICE_TARGET` (`main.cpp:939-970`), `SUPPLY_DEMAND_PULLBACK` (`main.cpp:971-991`), `MARKET_MAKING` (`main.cpp:992-1001`), `THEME` (`main.cpp:1002-1013`). 미지원 타입은 경고 (`main.cpp:1016`).
- **전략-국면 매핑**: 방금 추가된 전략에 `active_regimes` 배열(BULL/NEUTRAL/BEAR)을 파싱해 `engine.set_last_active_regimes()`로 주입 (`main.cpp:1020-1039`). 키가 있는데 파싱 결과가 비면 "게이트 무효" 경고 (`main.cpp:1037-1038`).

전략 등록이 끝나면 `engine.start()` 후 `is_running()`이 false가 될 때까지 1초 슬립 루프로 메인 스레드가 대기한다 (`main.cpp:1042-1044`).

---

## 2. 인증 / 로그인 — OAuth2 + 토큰 캐시

`KisClient::authenticate()` (`KisClient.cpp:236-359`)의 흐름:

1. **캐시 재사용 시도**: 토큰 캐시 경로는 `token_cache_path()` — `kis_token_<appkey앞8자>.json` (`KisClient.cpp:217-222`). 환경변수 `KIS_TOKEN_CACHE_DIR`이 있으면 그 디렉터리에 저장(도커 공유 볼륨 → Python balance가 재사용). 캐시 파일을 열어 `access_token`/`expires_at`을 읽고, 만료 **10분 전**까지 남았으면(`exp_t - now_t > 600`) 인메모리로 로드하고 즉시 반환 (`KisClient.cpp:239-283`).
2. **신규 발급**: 캐시 미스 시 `POST {base}/oauth2/tokenP`에 `{grant_type:client_credentials, appkey, appsecret}` 전송 (`KisClient.cpp:286-289`). 응답에서 `access_token`, 만료 필드 `access_token_token_expired` 파싱 (`KisClient.cpp:300-303`).
3. **캐시 저장**: `.tmp`에 쓴 뒤 atomic rename — Windows `MoveFileExA`(`KisClient.cpp:339`), Linux는 `chmod 0600` + `fsync` + `rename`(`KisClient.cpp:341-348`). 읽는 쪽(Python)이 truncated JSON을 보지 않게 함.
4. **자동 갱신**: `ensure_authenticated()` (`KisClient.cpp:224-234`)는 만료 **5분 전**이면 `authenticate()`를 다시 부른다. 모든 `http_get`/`http_post`가 URL에 "oauth2"가 없으면 진입 시 이걸 호출한다(재귀 방지 가드 포함) (`KisClient.cpp:800-801, 824-825`).

**base_url / tr_id 분기**:
- `base_url()`: `is_paper ? openapivts...:29443 : openapi...:9443` (`KisClient.h:115-119`).
- 국내 주문 tr_id: 매수 `VTTC0802U`(모의)/`TTTC0802U`(실), 매도 `VTTC0801U`/`TTTC0801U` (`KisClient.cpp:497-500`). 정정/취소는 `VTTC0803U`/`TTTC0803U` (`KisClient.cpp:684, 729`). 잔고조회 `VTTC8434R`/`TTTC8434R` (`KisClient.cpp:771`).
- 조회계 tr_id: 일봉 `FHKST03010100`(`KisClient.cpp:368`), 현재가/펀더멘털 `FHKST01010100`(`KisClient.cpp:412,435`), 시총랭킹 `FHPST01720000`(`KisClient.cpp:847`), 지수일봉 `FHKUP03500100`(`KisClient.cpp:1309`), 지수현재값 `FHPUP02100000`(`KisClient.cpp:1576`), 투자자동향 `FHKST01010900`(`KisClient.cpp:1473,1521`).

**헤더 구성** (공통 4종): `authorization: Bearer <token>`, `appkey`, `appsecret`, `tr_id` (예: `KisClient.cpp:367-368`). GET에도 KIS는 `Content-Type: application/json`을 요구하므로 `http_get`이 없으면 자동 추가 (`KisClient.cpp:803-813`). HTTP 구현은 플랫폼 분기: Windows `winhttp_request`(`KisClient.cpp:69-151`), Linux `curl_request`(`KisClient.cpp:163-194`).

---

## 3. 엔진 기동 — Engine::start()

`Engine::start()` (`Engine.cpp:29-144`) 순서:

1. `running_` 이미 true면 반환 (`Engine.cpp:31-32`).
2. (HAS_ZMQ 시) ZmqBridge 생성 + KILL/STATUS 명령 핸들러 등록 (`Engine.cpp:36-58`). 기본 빌드는 미포함.
3. **KisClient 생성 + 인증**: `kis_ = make_unique<KisClient>(kis_cfg_)`; `authenticate()` 실패 시 조기 반환 (`Engine.cpp:60-65`).
4. **OrderRouter(FEP) 초기화**: `order_router_ = make_unique<OrderRouter>(order_gate_, *kis_)` (`Engine.cpp:68-73`). `order_gate_`는 Engine 멤버(값 소유, `Engine.h:88`), `kis_`는 참조 주입.
5. **RegimeController 초기화**: `regime_ = make_unique<RegimeController>()`, `regime_->set_kis(kis_.get())` (`Engine.cpp:76-78`). 실제 판정은 장 시작 시 1회.
6. **전략 초기화**: 각 전략에 `set_kis()` 주입 후 `on_start()` 호출(예외는 잡아 해당 전략만 건너뜀) (`Engine.cpp:81-96`). 전략은 `on_start`에서 KIS로 universe를 조회할 수 있다.
7. **WS 구독 스펙 수집(중복 제거)**: 모든 전략의 `get_watch_specs()`를 순회, 키 `"KR:<exch>:<ticker>"`로 중복 제거해 `watch_specs_`에 적재 (`Engine.cpp:99-112`).
8. `running_ = true` (`Engine.cpp:114`).
9. **WebSocket 연결·구독**: `watch_specs_`가 비어있지 않으면 `ws_ = make_unique<KisWebSocket>()` 생성 후 콜백 3종 등록 (`Engine.cpp:117-136`):
   - OrderBook 콜백 → `ob_queue_.push(ob)` (`Engine.cpp:120`)
   - TradeData 콜백 → `td_queue_.push(td)` (`Engine.cpp:121-128`)
   - Fill 콜백 → `order_router_->on_fill(fn)` (`Engine.cpp:129-133`)
   - `ws_->connect(watch_specs_)` 실패 시 경고만 하고 계속(호가/체결 없이 REST만으로 동작) (`Engine.cpp:134-135`).
10. **스레드 4개 기동** (`Engine.cpp:138-141`).

`stop()` (`Engine.cpp:146-172`)은 `running_.exchange(false)`로 중복 방지, control→order→strategy→data 역순 join, WS disconnect, 전략 `on_stop()`, 통계 출력.

---

## 4. 시세 수신 경로 A — DataThread (REST 일봉 폴링)

`Engine::data_thread_fn()` (`Engine.cpp:175-237`):

1. **장중 판정 + 장 시작 감지**: `is_any_market_open()`(`Engine.cpp:383-386`) = KR(09:00~15:30 KST, `Engine.cpp:362-370`) 또는 US(KST 22:30~05:00, `Engine.cpp:373-381`). 판정은 `gmtime + 9h`로 머신 TZ 무관하게 KST를 계산 (`Engine.cpp:348-359`).
2. **장 시작 엣지**(`market_now && !was_market_open`): `order_gate_.reset_daily()` + `order_router_->reset_daily()` + `regime_->evaluate()` 후 전략별 `set_active(regime_->is_active_for(...))` (`Engine.cpp:184-198`).
3. 장 외 시간이면 60초 슬립 후 continue (`Engine.cpp:201-205`).
4. **폴링**: `watch_specs_`의 각 종목에 대해 KR이면 `kis_->get_daily_ohlcv(spec.ticker, 1)`, US면 `get_us_daily_ohlcv(spec.ticker, 1, exchange)` (`Engine.cpp:211-215`). 반환 `bars[0]`에 `bar_index = data_count_`를 심고 `market_queue_.push(md)` (`Engine.cpp:219-223`). 큐가 full이면 1ms 슬립하며 재시도. push 후 `data_count_++`.
5. 루프 말미 `fetch_interval_sec_`초(기본 60s) 슬립 (`Engine.cpp:230`).

**데이터 변환**: KIS REST JSON `output2[i]`의 `stck_clpr/oprc/hgpr/lwpr/acml_vol`(문자열) → `std::stod/stoll` → `MarketData{close,open,high,low,volume, timestamp=now, bar_index}` (`KisClient.cpp:387-395`). `timestamp`는 거래소 체결시각이 아니라 **REST 응답 처리 시각**임에 주의 (`Types.h:36`).

### 4.1 ★버그: get_daily_ohlcv 날짜 하드코딩 → 모의서버 HTTP 500 → 전략 굶음
`get_daily_ohlcv()`의 URL이 `FID_INPUT_DATE_1=19000101` … `FID_INPUT_DATE_2=99991231`로 **하드코딩**되어 있다 (`KisClient.cpp:363-365`):

```cpp
"?FID_COND_MRKT_DIV_CODE=J" + "&FID_INPUT_ISCD=" + ticker + "&FID_INPUT_DATE_1=19000101" +
"&FID_INPUT_DATE_2=99991231" + "&FID_PERIOD_DIV_CODE=D" + "&FID_ORG_ADJ_PRC=0";
```

- **왜 문제인가**: `inquire-daily-itemchartprice`(TR `FHKST03010100`)는 조회 구간을 합리적 범위(보통 ~100일 이내)로 기대한다. 1900~9999년 전 구간을 요청하면 서버가 처리하지 못해 HTTP 500 또는 빈 `output2`를 반환한다. `http_get`이 빈 문자열/에러를 돌려주면 `get_daily_ohlcv`는 빈 벡터를 반환하고(`KisClient.cpp:372-376`), DataThread는 `bars.empty()`에서 continue → **market_queue_에 아무것도 push되지 않는다** (`Engine.cpp:217-218`). 결과적으로 StrategyThread의 `on_data`가 호출되지 않아 MACross 같은 일봉 기반 전략이 **신호를 전혀 내지 못한다**(입력이 굶음).
- **올바른 값**: 같은 파일의 `get_index_daily_ohlcv`가 쓰는 방식처럼 `DATE_2 = 오늘(KST)`, `DATE_1 = 오늘 - N일`로 유한 구간을 넣어야 한다 (참조 패턴: `KisClient.cpp:1312-1328`의 `fmt_date(end_t)` / `end_t - kWindowDays*86400`). count봉을 채우려면 페이지네이션도 함께 필요.

### 4.2 부차 문제: count=1 폴링과 일봉 반복
DataThread는 `get_daily_ohlcv(ticker, 1)`로 **최신 1봉만** 가져온다 (`Engine.cpp:213`). 이 봉은 "오늘의(미완성) 일봉"이라, 60초마다 폴링할 때마다 사실상 같은 날짜의 종가가 반복 push된다. MACross의 `prices_` deque(`MACrossStrategy.h:112`)는 서로 거의 같은 값으로 채워져 골든/데드크로스가 잘 발생하지 않는다. 설령 4.1 버그가 고쳐져도, 장중 일봉 크로스 전략이 의미 있게 동작하려면 과거 N봉을 시드하는 로직이 필요하다. (KR_TEST 경로는 `get_daily_ohlcv(code, 65)`로 여러 봉을 받아 MA를 계산하므로 대조적 — `main.cpp:428`.)

---

## 5. 시세 수신 경로 B — WebSocket 수신 스레드

WS 연결/구독은 `KisWebSocket::connect()` (Windows `WebSocketClient.cpp:224-313`, Linux `:766-818`), 수신 파싱은 공통 `parse_message()` (`WebSocketClient.cpp:1024-1125`).

### 5.1 연결·구독
- approval key 발급: `POST /oauth2/Approval` with `{grant_type, appkey, secretkey}` → `approval_key_` (`WebSocketClient.cpp:987-1013`). REST OAuth 토큰과 별개 키.
- WS 엔드포인트: `ops.koreainvestment.com`, 포트 `is_paper ? 31000 : 21000` (`WebSocketClient.cpp:230-231`).
- 구독: KR 종목은 `H0STASP0`(호가, `trade_only=false`일 때만) + `H0STCNT0`(체결) (`WebSocketClient.cpp:284-286`). US는 `HDFSCNT0`, tr_key=`"EXCH|SYMBOL"` (`WebSocketClient.cpp:291-292`). 체결통보 `H0STCNI9`(모의)/`H0STCNI0`(실)은 `on_fill_` 등록 + `hts_id` 비어있지 않을 때만 구독 (`WebSocketClient.cpp:296-305`). `send_subscribe`는 approval_key를 header에 담은 JSON을 보낸다 (`WebSocketClient.cpp:1015-1022`).

### 5.2 프레임 파싱 (`parse_message`)
- JSON 프레임(`msg[0]=='{'`): `PINGPONG`이면 그대로 echo (`WebSocketClient.cpp:1038-1042`). 구독 응답이면 rt_cd/msg1 로그. 체결통보 구독 응답이면 `output.key/iv`를 확보해 **AES-256-CBC key(32B)/iv(16B)**를 저장(길이 검증 후) (`WebSocketClient.cpp:1052-1075`).
- 데이터 프레임: `TYPE|TR_ID|COUNT|DATA`로 `|` 분리 (`WebSocketClient.cpp:1085-1090`). `parts[0]=="1"`이면 암호화 프레임(체결통보) → `base64_decode` + `aes_cbc_decrypt` (`WebSocketClient.cpp:1093-1113`). 그 후 `data`를 `^`로 분리해 tr_id별 파서 호출 (`WebSocketClient.cpp:1115-1124`).

### 5.3 H0STCNT0 체결 필드 인덱스 (parse_kr_trade, `WebSocketClient.cpp:1181-1212`)
| 인덱스 | 의미 | 파싱 → 필드 |
|---|---|---|
| `f[0]` | 종목코드 | `td.ticker` |
| `f[1]` | 체결시간 | `td.time` |
| `f[2]` | **현재가** | `td.price = stod(f[2])` |
| `f[12]` | 체결량 | `td.quantity = stoll(f[12])` |
| `f[21]` | 체결구분(1=매수,5=매도) | `td.direction = stoi(f[21])` |

필드 22개 미만이면 무시 (`WebSocketClient.cpp:1183`). 결과 `TradeData`를 `on_trade_(td)`로 콜백 → Engine의 콜백이 `td_queue_.push(td)` (`Engine.cpp:121`).

### 5.4 H0STASP0 호가 필드 인덱스 (parse_orderbook, `WebSocketClient.cpp:1135-1177`)
5단계만 사용: `asks[i].price=f[3+i]`, `asks[i].quantity=f[23+i]`, `bids[i].price=f[13+i]`, `bids[i].quantity=f[33+i]` (`WebSocketClient.cpp:1160-1168`). 38필드 미만이면 무시. `OrderBook`을 `on_orderbook_(ob)` → `ob_queue_.push(ob)` (`Engine.cpp:120`).

### 5.5 US 체결 HDFSCNT0 (parse_us_trade, `WebSocketClient.cpp:1217-1249`)
`f[2]`=현재가, `f[8]`=체결량, 방향은 `f[20]`(추정, 필드 확인 주석) (`WebSocketClient.cpp:1239-1242`).

### 5.6 ★경로 A vs 경로 B가 어떤 콜백으로 흐르는가 (중요)
- **경로 A (REST 일봉 → market_queue_)** → StrategyThread가 `on_data()` 호출 (`Engine.cpp:299-308`).
- **경로 B-호가 (H0STASP0 → ob_queue_)** → `on_order_book()` + `on_order_book_batch()` (`Engine.cpp:267-284`).
- **경로 B-체결 (H0STCNT0/HDFSCNT0 → td_queue_)** → `on_trade()` (`Engine.cpp:287-296`).
- **MACrossStrategy는 `on_data`만 구현**하며(`MACrossStrategy.h:50`), `on_order_book`/`on_trade`는 StrategyBase 기본(nullopt 반환, `StrategyBase.h:25-41`)을 그대로 쓴다. 즉 **MACross는 경로 A(REST 일봉)만 소비하고 WS 체결/호가는 무시한다.** WS 실시간 현재가가 전략에 활용되지 않는 것이 현재 설계 상태다. (MACross의 `get_watch_specs`가 `trade_only=true`로 H0STCNT0만 구독하나, 그 체결 데이터는 전략이 읽지 않는다 — `MACrossStrategy.h:29-34`.)

---

## 6. 전략 분석 — StrategyThread

`Engine::strategy_thread_fn()` (`Engine.cpp:242-319`)는 우선순위 순으로 큐를 비운다:

1. **ob_queue_** (호가, 고주파) — while 루프로 소진, 각 전략 `on_order_book` + `on_order_book_batch` (`Engine.cpp:267-284`). batch 경로는 MM 등 다건 발주용이며 CANCEL/REPLACE는 side==NONE이어도 통과 (`Engine.cpp:280`).
2. **td_queue_** (체결) — while 루프, `on_trade` (`Engine.cpp:287-296`).
3. **market_queue_** (일봉) — if 1건, `on_data` (`Engine.cpp:299-308`).
4. 아무 일도 없으면 100µs 슬립(저지연 유지) (`Engine.cpp:315-316`).

신호가 나오면 `push_signal` 람다: `signal_count_++`, 로그, `order_queue_.push(sig)`(full이면 100µs 슬립 재시도) (`Engine.cpp:246-257`).

### 6.1 MACross on_data 로직 (`MACrossStrategy.h:50-83`)
1. `data.ticker != ticker_`면 무시 (`MACrossStrategy.h:52-53`).
2. `prices_.push_back(data.close)`, deque 크기를 `long_period_`로 유지(`pop_front`) (`MACrossStrategy.h:55-57`). 봉이 `long_period_` 미만이면 nullopt (`MACrossStrategy.h:59-60`).
3. `calc_ma(short)`, `calc_ma(long)` — deque 뒤에서 period개 합산/평균 (`MACrossStrategy.h:86-93`).
4. **골든크로스(매수)**: `is_active() && !in_position_ && prev_short_ma_ <= prev_long_ma_ && short_ma > long_ma` (`MACrossStrategy.h:68-72`) → `make_signal(BUY)`, `in_position_=true`.
5. **데드크로스(매도)**: `in_position_ && prev_short_ma_ >= prev_long_ma_ && short_ma < long_ma` (`MACrossStrategy.h:74-78`) → `make_signal(SELL)`, `in_position_=false`. **매도에는 `is_active()` 게이트가 없다** — 비활성 국면에서도 청산은 허용(진입만 차단).
6. `prev_*_ma_` 갱신 (`MACrossStrategy.h:80-81`).

`in_position_`은 `on_start`에서 `start_in_position_`으로 시드 (`MACrossStrategy.h:47`). `universe_from_balance` 모드에서 보유분을 true로 시드하면 첫 신호가 데드크로스 매도가 될 수 있어 실보유분을 지표로 청산 가능 (`MACrossStrategy.h:14-21`).

### 6.2 국면 게이트 (is_active)
`set_active(bool)`은 Engine이 장 시작 국면 판정 후 호출 (`Engine.cpp:196`, `StrategyBase.h:64`). `active_regimes()`(기본 전 국면, `StrategyBase.h:77`)와 현재 국면을 비교해 세팅. 기본값 true라 국면을 모를 때는 통과 (`StrategyBase.h:76`).

---

## 7. 매매 로직 — OrderSignal 생성

`OrderSignal` 구조 (`Types.h:66-83`): `ticker, side(BUY/SELL/NONE), type(MARKET/LIMIT), quantity, price, strategy_id, market, exchange, timestamp, account_id`, 그리고 MM-1 확장 `action(NEW/CANCEL/REPLACE), client_oid, orig_client_oid`.

MACross의 `make_signal` (`MACrossStrategy.h:95-105`)은 `type=MARKET`, `quantity=quantity_`, `strategy_id=id()`("MA_CROSS_<ticker>"), `timestamp=d.timestamp`를 채운다. `account_id`/`action`은 기본값(빈 문자열/NEW). StrategyThread가 `side != NONE`인 신호만 push한다 (`Engine.cpp:304-305`).

---

## 8. 리스크 게이트 — OrderGate::check 11개 검사

`OrderGate::check()` (`OrderGate.cpp:11-199`)는 순서대로:

1. **Kill switch** — `kill_switch_` 활성 시 전방향(BUY·SELL) 거부 (`OrderGate.cpp:13-18`).
2. **NONE side** — `sig.side==NONE`이면 거부 (`OrderGate.cpp:20-25`).
3. **Entry halt** — 신규 진입(BUY NEW)만 차단, SELL 청산·CANCEL/REPLACE는 통과 (`OrderGate.cpp:27-34`). kill_switch와 분리된 국면 리스크 플래그.
4. **1주문 fat-finger 백스톱** — NEW에만. 수량≤0·`max_qty_per_order` 초과·명목(`max_notional_per_order`) 초과 거부. 명목 평가는 지정가=price, 시장가(price=0)=ref_price (`OrderGate.cpp:36-64`).
5. **종목당 포지션 수량 한도** — BUY에만. 파티션 키 `make_key(account, ticker)`로 `positions_[k]`(실체결) + `reserved_[k]`(미체결 선점) 합산이 `max_qty_per_ticker`(기본 100) 초과면 거부 (`OrderGate.cpp:66-81`). **매도에는 한도 미적용** — 청산은 막지 않는 설계.
6. **종목당 명목 한도** — BUY에만. 보유·예약 합산 평가가 `max_notional_per_ticker` 초과면 거부(자본% 사이징 상한 백스톱) (`OrderGate.cpp:83-95`).
7. **동시 보유 종목 상한** — 새 종목을 여는 BUY NEW에만. 실보유∪예약 종목 수가 `max_concurrent_positions` 이상이면 신규 진입 차단(기존 보유·예약 종목은 예외) (`OrderGate.cpp:97-120`).
8. **일일 손실 한도** — BUY에만. `daily_pnl_ <= daily_loss_limit`(기본 -30만원)이면 신규 매수 거부 (`OrderGate.cpp:123-136`). 보유분 추가 하락은 막지 않음(주석 C10).
9. **PnL stale 가드(B2)** — BUY NEW에만. `daily_pnl_`이 낡으면(잔고 리컨사일 정체) §8 손실컷을 신뢰할 수 없어 신규 진입만 보수 정지. SELL·취소/정정은 통과 (`OrderGate.cpp:138-145`).
10. **중복 신호(dedup)** — rate 소비 전에 검사. 키 = `account:strategy:ticker:side` (side 포함 이유: MM이 같은 틱에 BUY+SELL 동시 발주 시 오거부 방지). `dedup_window_sec`(기본 1.0초) 이내 동일 키면 거부 (`OrderGate.cpp:147-167`).
11. **Rate limit** — 초당(`max_orders_per_sec`=5)/분당(`max_orders_per_min`=20) 슬라이딩 윈도우. 초과 시 거부, 통과 시에만 `order_times_sec_/min_`에 push (`OrderGate.cpp:169-196`).

**뮤텍스 규칙**: 각 항목은 독립 스코프에서만 락(중첩 없음). check() 자체는 원자적이지 않으나 **단일 order_thread만 호출**하므로 check()+on_accept가 직렬 실행돼 TOCTOU가 없다 (`OrderGate.cpp:6-10`, C6 주석).

원장 파티션 키 `make_key`는 `account.size() + ":" + account + ticker`로 구성해 `"A"+"B:C"`와 `"A:B"+"C"` 충돌을 방지(외부 계좌ID/US 티커에 `:` 가능) (`OrderGate.h:121-124`).

---

## 9. 주문 실행 — OrderThread → OrderRouter → KIS

`Engine::order_thread_fn()` (`Engine.cpp:322-345`): `order_queue_.pop()` → `order_router_->submit(*opt)` → ACCEPTED면 `order_count_++`.

`OrderRouter::submit()` (`OrderRouter.cpp:20-29`)은 `sig.action`으로 라우팅: NEW→`new_route`, CANCEL→`cancel_route`, REPLACE→`replace_route`. 전 경로가 단일 order_thread에서만 실행돼 OrderGate의 단일소비자 전제를 보존 (`OrderRouter.cpp:17-19`).

**new_route** (`OrderRouter.cpp:32-132`):
1. `ManagedOrder` 생성, `order_id="ORD-000001"` 형식(`next_id`, `OrderRouter.cpp:9-15`), status=PENDING, `total_count_++`.
2. `gate_.check()` 실패 → REJECTED + `rejected_count_++` + record (`OrderRouter.cpp:46-60`).
3. 통과 → status=SUBMITTED. **RTT 계측**: `t_send` 기록 후 `kis_.submit_order_ack(sig)` 호출, `steady_clock` 차이로 `rtt_ms` 산출 (`OrderRouter.cpp:64-76`). 예외 시 REJECTED (`OrderRouter.cpp:77-89`).
4. `ack.odno`가 비어있지 않으면 → status=ACCEPTED, `kis_order_no=odno`, `krx_orgno=ack.krx_orgno`(정정/취소용), `accepted_count_++` (`OrderRouter.cpp:93-98`).
5. **`gate_.on_accept(account, ticker, side, qty, price)`로 reserved_ 선점** (`OrderRouter.cpp:100`) — 실체결 전까지 재주문 차단.
6. `client_oid` 있으면 `oid_index_`에 매핑 (`OrderRouter.cpp:103-107`). 접수 로그에 RTT 포함 (`OrderRouter.cpp:108-112`).
7. odno 비었으면 REJECTED("빈 ODNO") (`OrderRouter.cpp:118-128`).

`KisClient::submit_order_ack()` (`KisClient.cpp:612-670`): tr_id 분기(§2 참조), body 구성(국내는 `ORD_DVSN` MARKET="01"/LIMIT="00", `ORD_QTY`, `ORD_UNPR`) (`KisClient.cpp:636-641`), `http_post` 후 `rt_cd=="0"` 확인, `output.ODNO`와 `output.KRX_FWDG_ORD_ORGNO` 추출해 `OrderAck` 반환 (`KisClient.cpp:661-669`).

**on_accept** (`OrderGate.cpp:201-214`): `reserved_[k] += (BUY? +qty : -qty)`, 0이면 erase. positions_/avg_price는 불변(접수는 체결이 아님).

---

## 10. 원장 / 체결 — 체결통보 AES 복호화 → on_fill

### 10.1 체결통보 수신·복호화
`parse_fill_notification` (`WebSocketClient.cpp:1257-1293`)는 암호 프레임 복호 후 호출된다. 복호는 `parse_message`에서 `parts[0]=="1"` → `aes_cbc_decrypt(base64_decode(data), aes_key_, aes_iv_)` (`WebSocketClient.cpp:1093-1113`). AES 구현은 Windows BCrypt(`WebSocketClient.cpp:71-107`), Linux OpenSSL EVP(`:110-144`). key/iv는 구독 응답에서 확보(§5.2).

체결통보 필드 (`WebSocketClient.cpp:1257-1293`): `f[2]`=ODNO, `f[4]`=매도/매수구분(01=매도,02=매수), `f[8]`=종목코드, `f[9]`=체결수량, `f[10]`=체결단가, `f[11]`=체결시각, `f[13]`=CNTG_YN(1=접수통보,2=체결통보). **`f[13]!="2"`면 반환** — 체결(2)만 처리 (`WebSocketClient.cpp:1268-1269`). 결과 `FillNotification`을 `on_fill_(fn)` → Engine 콜백 → `OrderRouter::on_fill(fn)` (`Engine.cpp:129-133`).

### 10.2 OrderRouter::on_fill (`OrderRouter.cpp:376-440`)
1. **멱등 처리**: 체결고유번호가 없어 `수신일(YYYYMMDD):ODNO:fill_time:qty:price*100`를 조합 키로 `seen_fills_`에 삽입 시도, 중복이면 무시 (`OrderRouter.cpp:382-399`). 거래일 prefix로 cross-day ODNO 재사용 충돌 방지(V-4).
2. `history_`에서 `kis_order_no==fn.odno`이고 ACCEPTED/FILLED이며 미체결 잔량이 있는 주문을 찾아 (`OrderRouter.cpp:400-409`) `confirmed_qty += filled_qty`, 전량이면 status=FILLED (`OrderRouter.cpp:411-414`).
3. **원장 갱신**: `gate_.on_fill_confirmed(account, ticker, side, qty, price)` (`OrderRouter.cpp:428-429`).

### 10.3 on_fill_confirmed (`OrderGate.cpp:271-325`)
- 수수료 `price*qty*0.00015`, 거래세(매도만) `price*qty*0.0018` (`OrderGate.cpp:160-161`).
- **BUY**: `new_qty=pre_qty+qty`, `avg_prices_[k] = (pre_qty*cur_avg + qty*price)/new_qty` (부분체결도 정확), `positions_[k]=new_qty`, reserved_ 선점 -qty 해제 (`OrderGate.cpp:169-183`).
- **SELL**: `new_qty=pre_qty-qty`(음수는 0 클램프—공매도 미지원), `realized_pnl=(price-cur_avg)*qty - 수수료 - 세금`, avg_price 불변, new_qty==0이면 positions_/avg_prices_ erase, reserved_ +qty 해제 (`OrderGate.cpp:184-203`). SELL이면 `add_realized_pnl`로 `daily_pnl_` 적립 (`OrderGate.cpp:206-207`).

파티션 키는 `mo.signal.account_id` — 현재 단일 CANO 전제라 ODNO가 유일해 매핑이 정확. 진짜 다계좌 라우팅 시 (odno+account) 키 확장 필요(TODO 주석 `OrderRouter.cpp:424-427`).

### 10.4 취소/정정 원장 정합 (MM-1)
`cancel_route` (`OrderRouter.cpp:170-265`): orig_client_oid로 live 주문 스냅샷 → lock 밖에서 `kis_.cancel_order` → 성공 시 그 시점 `quantity - confirmed_qty`를 재계산해 `gate_.on_cancel`로 reserved 해제(이중해제 방지) + 원주문 CANCELLED 표기. `replace_route` (`OrderRouter.cpp:273-373`): `kis_.revise_order` 성공 시 원 잔량 해제 후 new_qty 재선점, 정정본을 새 ManagedOrder(ACCEPTED)로 추적.

---

## 11. 로깅 / 적재

`Logger`는 헤더온리 싱글톤 (`Logger.h:18-100`). `Logger.cpp`는 placeholder(`Logger.cpp:1`) — 구현이 전부 헤더 inline. `log()`는 `localtime` 기반 `YYYY-MM-DD HH:MM:SS.mmm` 타임스탬프 + 레벨 + msg를 콘솔(`console_enabled_`일 때)과 `quant_trader.log`에 동시 기록 (`Logger.h:41-59`). 매크로 `LOG_INFO/WARN/ERROR/DEBUG` (`Logger.h:102-105`).

> 주석에 "ms UTC"라 적혀 있으나 실제 코드는 `std::localtime`(로컬 TZ)을 쓴다 (`Logger.h:51`). 파일 기록은 append 모드(`Logger.h:30`)이며 명시적 flush가 없어 `std::ofstream` 기본 버퍼링을 탄다. TRADE 모드는 신호가 드물어 로그가 희소한데, 파이프로 stdout을 캡처하면 블록 버퍼링 때문에 실시간으로 안 보일 수 있다(콘솔 직접 출력은 라인 단위라 보임).

FEED/KR_TEST/US_TEST 모드는 `set_console_enabled(false)`로 콘솔 로그를 끄고 화면을 직접 그린다 (`main.cpp:254, 457, 706`).

---

## 12. 종료 흐름

- SIGINT/SIGTERM → `signal_handler`가 `g_running=false` + `g_engine->stop()` (`main.cpp:35-40`, 등록 `main.cpp:243-244`).
- `Engine::stop()` (`Engine.cpp:146-172`): `running_.exchange(false)`로 1회성 보장 → control→order→strategy→data 역순 join → `ws_->disconnect()` → 전략 `on_stop()` → `print_stats()`.
- `KisWebSocket::disconnect()` (`WebSocketClient.cpp:497-526` Win / `:941-961` Linux): `connected_.exchange(false)`, 소켓 close, recv_thread_ join. Linux는 `shutdown(SHUT_RDWR)`로 블로킹 recv를 깨워 join 무한대기 방지(W-1).
- 메인 스레드는 `engine.is_running()`이 false가 되면 루프 탈출 후 종료 로그 (`main.cpp:1043-1047`). 로그 flush는 `std::ofstream` 소멸자에 의존(명시적 flush 없음).

---

## 13. ★빈틈 / 미완성 목록

| # | 항목 | 근거(파일:라인) | 영향 | 개선 방향 |
|---|---|---|---|---|
| G1 | **get_daily_ohlcv 날짜 하드코딩 → 500/빈응답** | `KisClient.cpp:363-365` | 모의서버에서 일봉 응답 실패 → market_queue_ 미적재 → MACross 등 일봉 전략 **신호 0건**. TRADE 모드 핵심 경로가 사실상 무동작. | `get_index_daily_ohlcv`처럼 KST 기준 유한 날짜구간(오늘, 오늘-N일) + 페이지네이션으로 교체 (`KisClient.cpp:1312-1373` 패턴 재사용). |
| G2 | **count=1 폴링 + 일봉 반복** | `Engine.cpp:213`, `MACrossStrategy.h:55-60` | 최신 1봉만 반복 수신 → deque가 동일 종가로 채워져 크로스 미발생. 과거봉 시드 부재. | on_start에서 과거 N봉 시드(seed) 또는 DataThread에서 `count=long_period+α` 요청 + 신규봉만 push. |
| G3 | **WS 실시간 체결/호가가 전략에 미활용** | `MACrossStrategy.h`(on_trade/on_order_book 미구현), `StrategyBase.h:38-41` | H0STCNT0 현재가가 들어와도 MACross는 무시. 실시간성 없음. 구독은 하되 소비 안 함. | 실시간 가격 기반 전략(예: 밴드/스탑) 도입 또는 MACross를 WS 가격으로 교차 판정하도록 확장. |
| G4 | **RegimeController 지수일봉 조회 실패 가능(500)** | `RegimeController.cpp:91-93`, `KisClient.cpp:1263-1384`(TR `FHKUP03500100`, 모의서버 제약) | 조회 실패 시 연속 fail이면 NEUTRAL fallback (`RegimeController.cpp:57-85`). 국면 게이트가 사실상 NEUTRAL 고정될 수 있음 → 진입 게이트가 의도대로 안 걸림. | 모의/실전 지수 TR 가용성 확인, 실패 시 캐시/보수 fallback 정책 명문화. |
| G5 | **포지션 원장이 실제 계좌잔고와 분리되어 시작** | `OrderGate.cpp:235`(positions_ 영속·리셋 안 함), `OrderGate` 초기 상태 = 빈 맵 | 엔진 기동 시 `positions_`는 비어 있어, 실제 계좌에 보유분이 있어도 게이트는 0으로 인식 → 매도 가능수량 오판/평단 부정확. universe_from_balance는 전략 시드만 하고 게이트 원장은 시드 안 함(`main.cpp:900-905`). | 기동 시 `get_balance()`로 positions_/avg_prices_ 시드하는 원장 부트스트랩 추가. |
| G6 | **US 체결 방향 필드 인덱스 추정** | `WebSocketClient.cpp:1241-1242`("방향 필드 위치 확인 후 조정" 주석) | 미국 체결 direction이 부정확할 수 있음(현재 US 전략 미사용이라 저위험). | 실측 로그로 인덱스 확정. |
| G7 | **정정(REPLACE) 부분체결·조직번호 재캡처 미완** | `OrderRouter.cpp:271-272, 364`(TODO) | 부분체결 상태 정정은 수량 정합 미보장 → MM은 CANCEL+NEW만 사용. 정정 응답의 새 조직번호 미파싱(원 조직번호 승계). | 정정 응답 파싱 강화 + 부분체결 정정 로직(Phase 2). |
| G8 | **해외 정정/취소 미구현** | `KisClient.cpp:675`(주석 "해외 정정/취소 별도 tr_id — 미구현 TODO") | US 주문 취소/정정 불가. | overseas order-rvsecncl tr_id/URL 추가. |
| G9 | **Config.cpp / Logger.cpp placeholder** | `Config.cpp:1`, `Logger.cpp:1` | 설정 파서/로거가 헤더·main에 inline. 모듈 경계가 흐림(유지보수 시 혼란). | 실제 파싱/로깅 구현을 .cpp로 이전하거나 placeholder 제거. |
| G10 | **Logger 타임스탬프 로컬 TZ + flush 없음** | `Logger.h:51-59` | 주석은 "UTC"인데 localtime 사용. 파이프 캡처 시 블록버퍼링으로 실시간 미표시(§11). | flush 정책 명시(줄마다 `<< std::flush` 또는 파일 라인버퍼), TZ 주석 정정. |
| G11 | **OrderGate 원자성은 단일 소비자 전제에 의존** | `OrderGate.cpp:6-10`(C6), `OrderRouter.cpp:17-19` | 멀티 프로듀서로 확장 시 check()+on_accept TOCTOU 발생. 현재는 안전. | 다계좌/멀티스레드 발주 확장 시 check+reserve를 단일 임계구역으로 묶기. |
| G12 | **hts_id 미설정 시 체결통보 구독 스킵** | `WebSocketClient.cpp:296-305` | hts_id 없으면 H0STCNI 구독 안 함 → **체결통보 미수신 → 원장(positions_/pnl) 미갱신**. 주문은 나가나 체결 반영이 안 됨. | config에 hts_id 필수화 또는 잔고 폴링 폴백. (현재 config엔 설정됨: `config.json:7`.) |

---

## 검증한 파일 목록

- `Quant/src/main.cpp` (전체, 1048줄)
- `Quant/include/core/Types.h` (전체)
- `Quant/src/core/Engine.cpp` (전체) / `Quant/include/core/Engine.h` (전체)
- `Quant/include/core/RingBuffer.h` (전체)
- `Quant/src/api/KisClient.cpp` (전체, 1620줄 — 2페이지 분할 확인) / `Quant/include/api/KisClient.h` (전체)
- `Quant/src/api/WebSocketClient.cpp` (전체, 1294줄) / `Quant/include/api/KisWebSocket.h` (전체)
- `Quant/include/risk/OrderGate.h` / `Quant/src/risk/OrderGate.cpp` (전체)
- `Quant/src/ipc/OrderRouter.cpp` / `Quant/include/ipc/OrderRouter.h` (전체)
- `Quant/include/api/IOrderExecutor.h` (전체)
- `Quant/src/core/RegimeController.cpp` / `Quant/include/core/RegimeController.h` (전체)
- `Quant/include/strategy/StrategyBase.h` / `Quant/include/strategy/MACrossStrategy.h` (전체)
- `Quant/include/utils/Logger.h` (전체), `Quant/src/utils/Logger.cpp`(placeholder), `Quant/src/utils/Config.cpp`(placeholder)
- `Quant/config/config.json`, `Quant/config/config_mm_paper.json`

## 확인 못 한 부분 (본 문서 범위 밖)

- 다른 전략 헤더의 내부 로직 상세: `MomentumStrategy`, `ValueContraryStrategy`, `FixedIntervalStrategy`, `PriceTargetStrategy`, `SupplyDemandPullbackStrategy`, `MarketMakingStrategy`, `ThemeStrategy` (main.cpp의 등록·파라미터 파싱만 확인, on_data/on_order_book 내부 미정독). 특히 MM의 `on_order_book_batch` 다건 발주 실제 로직은 미확인.
- `ZmqBridge`(HAS_ZMQ 경로) 구현 — 기본 빌드 비활성이라 미정독.
- `KisClient::get_index_daily_ohlcv`의 실제 모의서버 500 여부는 코드상 페이지네이션·정렬만 확인했고 런타임 실측 로그로는 검증 못 함(G4는 코드 구조와 모의서버 제약 정황 기반 추정).
- 빌드 산출물(`build_win/`, `build/`)과 CMake 설정 파일은 미검토.
- 로그 실물(`quant_trader.log`) 및 `BACKTEST_LOG.md`, `bt_*.json` 백테스트 결과는 본 파이프라인 문서 범위 밖.
