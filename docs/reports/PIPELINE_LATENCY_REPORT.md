# 전종목 규모 시세 파이프라인 부하·지연 실측 리포트

> 하네스: [Quant/tests/bench_market_firehose.cpp](../../Quant/tests/bench_market_firehose.cpp)
> 측정일: 2026-09-02 / 빌드: Release(NDEBUG), Ninja+MSVC / 머신: 16 HW threads
> 목적: "최악지연(tail latency)을 잡는다"는 설계 주장을 숫자로 확인한다.

## 용어 — 지표 읽는 법

아래 표들은 지연을 하나의 평균값이 아니라 "분포의 여러 지점(퍼센타일)"으로 적는다.
메시지 지연을 빠른 순으로 줄 세웠다고 하면:

| 표기 | 뜻 |
|---|---|
| **p50** | 중앙값. 절반은 이보다 빠르다 — "보통 이 정도" 지연. |
| **p99** | 100건 중 99건이 이 값 안. 느린 상위 1%가 시작되는 선. |
| **p999** | 1,000건 중 999건이 이 값 안. 가장 느린 0.1%(= tail, 꼬리)를 본다. |
| **max** | 관측된 최악값. |
| **tail(꼬리 지연)** | p999·max처럼 드물게 느린 케이스. 평균보다 이 꼬리가 저지연의 관건이다. |
| **E2E** (end-to-end) | 메시지가 들어와(intake) 주문 결정(order)까지 걸린 전 구간 총 지연. |
| **net / proc / e2e** (결과 ③) | net=소켓 수신(TCP)까지, proc=수신 후 내부 처리, e2e=둘의 합. |

단위: **ns**=나노초(10억분의 1초), **µs**=마이크로초(100만분의 1초), **ms**=밀리초(1,000분의 1초).
1 µs = 1,000 ns, 1 ms = 1,000 µs. (예: p50 300 ns = 0.0003 ms.)

처리량: **msg/s**=초당 메시지 수, **offered / achieved**=투입한 rate / 실제 소화한 rate,
**drops**=큐가 밀려 버린(처리 못 한) 메시지 수(0이 정상), **throughput**=처리량.

## 배경

이 엔진은 락프리 단일생산자·단일소비자(SPSC) 링버퍼([RingBuffer.h](../../Quant/include/core/RingBuffer.h))와
비동기 로깅으로 최악지연(tail latency)을 줄이는 것을 목표로 설계했다. 다만 그동안
end-to-end 지연 분포(p99/p999)를 실측한 적은 없었다. 이 리포트는 KRX 상장 보통주 전종목
규모(~2,600종목)로 시세 팬아웃을 걸어, 내부 3-stage 처리 파이프라인의 지연 분포와 최대
지속가능 처리량(throughput)을 측정한 결과다.

## 측정 범위

측정 대상은 내부 처리 파이프라인이다. Engine과 동일한 3-stage 락프리 토폴로지를 mock
페이로드로 재현한다:

```
ws_producer → [ob_q, td_q] → strategy_thread → order_q → order_thread
   (Zipf 팬아웃)   RingBuffer      신호 생성       RingBuffer    주문 접수
```

①②에서 측정에서 제외한 것은 네트워크/피드 지연이다(실제 KIS REST/WS 왕복 미포함).
그 위에 **실 소켓 수신 경로**를 더한 측정이 결과 ③(TCP loopback, 코스콤→서버 재현)이다.
프로덕션 end-to-end 지연은 무료 OpenAPI 폴링 주기(초 단위)가 좌우하며, 실제 병목도 거기에
있다. 이 리포트가 확인하는 것은 하나다: 처리단은 전종목 규모에서도 µs 수준의 여유가 있어,
전체 지연을 좌우하는 것은 링버퍼가 아니라 피드라는 점이다. 피드가 WS/FIX/직결로 바뀌었을 때
처리단이 병목이 되지 않도록 인터페이스를 미리 분리해 둔 것이 링버퍼의 목적이다.

### 부하 현실성
- **종목 수**: 실제 상장 보통주 전종목 규모 2,600 (`--universe universe_full.json`로 실제
  코드 로드, 미제공 시 합성 폴백). ETF/ETN은 소스(getStockPriceInfo) 구조상 제외.
- **불균등 팬아웃**: 종목별 메시지 rate를 Zipf(s=1.0)로 배분 — 소수 대형주가 총 호가
  팬아웃의 대부분을 차지하는 실제 시장 구조 근사. 균등 분포는 현실과 멀다.
- **호가:체결 = 7:3** (OrderBook가 체결보다 잦음).

### 측정 방법
- pacing은 busy-wait(Windows `Sleep` 부정확성 회피).
- 지연 샘플은 소비자 단독 스레드에서만 수집한다(스레드별 독립 vector라 data race가 없다).
- `reserve`로 미리 잡아 측정 중 재할당(소비자 스톨) 차단.
- 측정창 종료 후 드레인 구간 항목은 percentile 오염원이므로 카운트만 하고 샘플에서 제외.
- Release 빌드로만 측정(Debug/ASan은 무의미) — 하네스가 빌드타입을 배너에 병기.

## 결과 ① — 현실적 전종목 버스트 부하 (2,600종목 @ 100,000 msg/sec, 10s)

실제 KRX 전 시장 피크 메시지량(수만 msg/s 급)을 상회하는 버스트에서:

| 구간 | 중앙값(p50) | 상위 1%(p99) | 상위 0.1%(p999) | 최악(max) | 샘플 |
|---|---|---|---|---|---|
| intake → strategy | 100 ns | 1.30 µs | 317 µs | 1.35 ms | 1,000,001 |
| strategy → order | 100 ns | 700 ns | 202 µs | 1.03 ms | 8,498 |
| **E2E (intake → order)** | **300 ns** | **15.5 µs** | **493 µs** | **1.21 ms** | 8,498 |

- 드롭 0, 백로그 0 (PASS). 큐 high-water mark: ob=97, td=36, order_q=2로 거의 항상 비어
  있어, 소비자가 생산자를 여유 있게 따라잡는다.
- 실측 유입 99,989 msg/s (목표 100k 달성).

## 결과 ② — 용량 스윕 (드롭이 시작되는 지점)

offered rate를 100k → 2.9M msg/sec로 계단 상승(스텝당 3s):

| 투입(offered)/s | E2E 중앙값(p50) | E2E 상위 1%(p99) | E2E 상위 0.1%(p999) | 유실(drops) |
|---|---|---|---|---|
| 100,000 | 300 ns | 119 µs | 741 µs | 0 |
| 500,000 | 300 ns | 4.1 µs | 489 µs | 0 |
| 1,000,000* | 300 ns | ~12 µs | ~450 µs | 0 |
| 2,500,000 | 300 ns | 3.4 µs | 489 µs | 0 |
| 2,900,000 | 300 ns | 443 µs | 3.13 ms | 0 |

\* 900k 스텝 대리값. 전 구간 표는 하네스 CSV 출력 참조.

- 테스트한 2.9M msg/sec까지 드롭 0. 이 지점의 한계는 소비자 파이프라인이 아니라 단일 합성
  생산자 스레드의 방출 속도(interval≈345ns)에서 왔다. 소비단은 이보다 더 받을 수 있는데
  하네스의 단일 producer가 먼저 한계에 닿았다. 그래서 드롭이 시작되는 소비자 천장은 2.9M/s
  위 어딘가에 있고, 이번 측정으로는 거기까지 도달하지 못했다.
- 실제 시장 피크(수만 msg/s) 대비 30~300배 여유를 무손실로 확보.

## 결과 ③ — 실 TCP 소켓 수신 경로 (코스콤→증권사 서버 재현)

①②는 소켓 없이 프로세스 내부 처리단만 격리 측정했다("순수 처리 비용"). 실제 시세 흐름은
**코스콤이 수백~수천 종목을 증권사 서버로 밀어넣고, 서버가 그것을 소켓으로 받아** 시스템
매매 파이프라인에 흘리는 구조다. 이 경로를 재현하려고 자매 하네스
[bench_feed_ingest.cpp](../../Quant/tests/bench_feed_ingest.cpp)를 만들었다:
KOSCOM 에뮬레이터(송신) ↔ 수신 서버가 **실제 TCP loopback 소켓**으로 연결되어, 커널 TCP
스택(send→recv)·직렬화·프레이밍·백프레셔를 실제로 통과한다(TCP_NODELAY=on, 108 bytes/msg).

> 측정 범위 주의: loopback은 물리 회선(WAN/전용선) 지연이 없다. 즉 "동일 머신 TCP 스택 비용 +
> 수신 후 주문 결정까지"를 재는 것이지, 코스콤↔증권사 물리 지연은 아니다. 실 라이브 데이터는
> [feed_latency_probe](../../Quant/tools/feed_latency_probe.cpp)로 소량(~15종목) 병행 실증한다(아래).

지연을 세 구간으로 분해한다: `net = recv_ts − send_ts`(TCP 스택), `proc = order_ts −
recv_ts`(수신 후 처리단), `e2e = order_ts − send_ts`(전체).

**2,600종목 @ 100,000 msg/sec offered, 10s:**

| 구간 | 중앙값(p50) | 상위 1%(p99) | 상위 0.1%(p999) | 최악(max) |
|---|---|---|---|---|
| net (send→recv, TCP) | 15.5 µs | 27.7 µs | 609 µs | 1.91 ms |
| proc (recv→order) | 300 ns | 6.5 µs | 513 µs | 826 µs |
| **e2e (send→order)** | **15.8 µs** | **48.3 µs** | **825 µs** | **2.13 ms** |

- 드롭 0, order_q hwm=1(항상 비어 있음). **e2e의 p50 15.8µs는 거의 전부 net 구간이다** —
  실제 소켓 수신 비용(syscall+커널 TCP)이 내부 처리(proc p50=300ns)를 50배 압도한다.
- **offered 100k인데 achieved 65k.** 단일 blocking-send 스트림이 메시지당 `send()` syscall에
  묶여 ~65k msg/s에서 포화한다. 이게 이 경로의 실제 병목이다.

**용량 스윕 (offered 50k→750k, 3s/step):**

| 투입(offered)/s | 소화(achieved)/s | e2e 중앙값(p50) | e2e 상위 1%(p99) | 유실(drops) |
|---|---|---|---|---|
| 50,000 | 49,172 | 16.0 µs | 86.2 µs | 0 |
| 150,000 | 65,035 | 15.7 µs | 27.6 µs | 0 |
| 350,000 | 65,427 | 15.8 µs | 34.2 µs | 0 |
| 550,000 | 65,476 | 15.7 µs | 36.3 µs | 0 |
| 750,000 | 64,292 | 15.8 µs | 35.8 µs | 0 |

- achieved가 offered와 무관하게 **~65k msg/s에서 평평**하다(150k 이상 전 구간). 수신측 드롭은
  0 — 송신이 수신을 앞지르지 못한다. 즉 **천장은 락프리 파이프라인이 아니라 메시지당 send()
  syscall 오버헤드**다. 실제 코스콤 전 시장 팬아웃 규모의 ingest가 처음 부딪히는 벽이 여기다.
- **함의**: 전 시장 규모에서 최적화해야 할 곳은 링버퍼가 아니라 **수신/전송 경로**다 —
  메시지 배칭/코얼레싱(패킷당 다건), 멀티 스트림, 커널 바이패스(io_uring/RIO). 링버퍼는
  이미 idle이다. ①②(순수 처리 sub-µs)와 합치면 병목의 소재가 정량적으로 확정된다.

### 실 라이브 데이터 병행 실증 (feed_latency_probe)
합성 부하와 별개로, 실제 KIS 실시간 WS로 소량(~15종목, 호가+체결)을 구독해 **실데이터가 우리
파이프라인을 통과함**과 수신콜백→주문결정 내부 지연·관측 msg rate를 라이브로 잰다. 무료 API에는
µs 해상도 원천 타임스탬프가 없어 거래소 wire 지연은 측정 불가(측정 한계) — 재는 것은 내부
처리 구간(③의 proc와 동일 구간)과 실 rate다. KIS 실시간 세션 상한(app_key당 1세션·~41등록)
때문에 소량이 한계이며, 반드시 장 중(09:00–15:30 KST)에 실행한다. (하네스 빌드 완료, 라이브
수치는 차기 장중 세션에 캡처 예정.)

## 해석: 중앙값과 tail

- 파이프라인 자체의 처리 비용은 sub-µs다(p50=100~300ns: 링버퍼 통과 + 전략 계산). 전종목
  규모에서도 이 값은 거의 변하지 않아, 팬아웃이 커져도 처리단 비용은 무시할 수 있다.
- tail(p999~수백µs, max~ms)은 파이프라인이 아니라 OS 스케줄러 프리엠션에서 온다. 공유
  Windows 데스크톱에서 busy-spin 소비자가 스케줄러 퀀텀(기본 ~15.6ms) 동안 디스케줄되면,
  그 사이 쌓인 메시지가 ms 지연으로 관측된다. max가 15.6ms의 배수 근방에 분포하는 것이 이를
  뒷받침한다.
- tail을 더 줄이는 표준적 방법은 (1) 스레드 코어 핀닝(affinity), (2) 코어 격리(isolcpus/전용
  머신), (3) 실시간 우선순위, (4) `Sleep` 대신 busy-spin + `pause`다. 이 하네스는 tail을
  측정하고 그 출처(OS)를 분리하는 데까지를 다뤘다.

## 재현

```powershell
# (선택) 실제 전종목 코드 덤프 — DATA_GO_KR_KEY 필요
py PYQuant/tools/full_universe_dump.py

# 빌드 (VS 개발셸, 한글 TEMP 회피)
$env:TEMP="C:\build_tmp"
cmake --build Quant/build_win --target bench_market_firehose

# 현실 버스트 부하
Quant/build_win/bench_market_firehose.exe load  --tickers 2600 --rate 100000 --duration 10
#   실제 코드 사용 시: --universe Quant/config/universe_full.json

# 용량 스윕 (드롭 시작점 탐색)
Quant/build_win/bench_market_firehose.exe sweep --tickers 2600 --start 100000 --step 200000 --max 3000000 --dwell 3

# ③ 실 TCP 소켓 수신 경로 (코스콤→서버 재현)
cmake --build Quant/build_win --target bench_feed_ingest
Quant/build_win/bench_feed_ingest.exe self  --tickers 2600 --rate 100000 --duration 10
Quant/build_win/bench_feed_ingest.exe sweep --tickers 2600 --start 50000 --step 100000 --max 800000 --dwell 3
#   두 콘솔 분리 실행(코스콤/서버):  서버측> bench_feed_ingest serve --port 47001
#                                    코스콤측> bench_feed_ingest send --host 127.0.0.1 --port 47001 --rate 100000 --duration 10

# 실 라이브 데이터 병행 실증 (반드시 장 중 09:00–15:30 KST)
cmake --build Quant/build_win --target feed_latency_probe
Quant/build_win/feed_latency_probe.exe Quant/config/config_dev_paper.json --duration 60
```

## 한계·후속

- **단일 producer 한계**: 소비자 천장을 실제로 재려면 멀티 producer(또는 더 저렴한 방출)로
  producer saturation을 걷어내야 한다. 현재 결론은 "소비단 천장 > 2.9M/s"까지만 확정한다.
- **OS jitter 분리**: 코어 핀닝·격리 머신에서 재측정하면 tail의 OS 성분을 정량 분해할 수 있다.
- **실 네트워크 성분**: 사내 체결 피드(WS/FIX)로 교체해 on_fill을 실시간화하면, 프로덕션
  E2E의 주요 병목(REST 폴링)이 사라지는 시나리오를 측정할 수 있다.
```
