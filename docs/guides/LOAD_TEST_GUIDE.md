# 부하·지연 테스트 실행 가이드

> 시세 파이프라인의 부하·지연을 **직접 돌려 수치를 확인**하는 실행 가이드다.
> 결과 해석과 측정 배경은 [PIPELINE_LATENCY_REPORT.md](../reports/PIPELINE_LATENCY_REPORT.md)에 있다.
> 회차별 실측값·원자료·그 수치로 내린 판단은 [reports/stresstest/](../reports/stresstest/README.md)에 모은다.
> 지표 약어는 [GLOSSARY.md](../GLOSSARY.md) 성능·지연 섹션 참조: 중앙값(p50)·상위 1%(p99)·상위 0.1%(p999)·전 구간(E2E).

## 하네스 — 무엇을 재나

| 하네스 | 소켓 | 재는 것 | 규모 상한 |
|---|---|---|---|
| `bench_market_firehose` | 없음 | 내부 3단 처리단(링버퍼→전략→주문) 지연·처리량 | 없음(합성, 전종목 규모) |
| `bench_feed_ingest` | 실 TCP(loopback) | 코스콤→서버 소켓 수신 경로(net/proc/e2e 분해) | 없음(합성, 전종목 규모) |
| `feed_latency_measure` | 실 KIS WS | 실데이터로 수신콜백→주문결정 내부 지연 재확인 | **app_key당 ~40종목**(API 하드캡) |
| `bench_engine_load` | 없음 | **진짜 Engine 한 바퀴**(수신 스레드 N → 샤드 M → 전략 → OrderGate → 체결) 처리량·구간별 드롭·지연 | 없음(합성, 종목 2,700개) |
| `bench_latency_path` | 없음 | 09-13 hot path 조각(시각 디코드·현재가 캐시·라우터·상태 키·캡처·FeedMux 홉·연쇄)의 옛/새 A/B | 없음(합성, 종목 2,600개 고정) |

부하테스트의 규모는 앞의 둘(합성)이 담당한다. 라이브 지연 측정은 규모가 아니라 "합성이 낸 처리단 지연이 실데이터에서도 성립하는가"를 확인하는 용도다(장 중에만 틱이 있음).

## 0. 준비 — cmd(PowerShell) 열고 이동

```powershell
Set-Location "$env:USERPROFILE\source\repos\Quant"
```

빌드가 안 돼 있으면(exe 없으면) 개발셸에서 한 번:

```powershell
$env:TEMP="C:\build_tmp"
cmake --build Quant/build_win --target bench_market_firehose bench_feed_ingest feed_latency_measure bench_latency_path
```

## 1. 처리단 부하 (소켓 없음) — bench_market_firehose

```powershell
# 고정부하: 2600종목 20만 msg/s 20초
.\Quant\build_win\bench_market_firehose.exe load --tickers 2600 --rate 200000 --duration 20

# 용량 천장 탐색: rate를 계단식으로 올려 최초 드롭 지점을 찾는다
.\Quant\build_win\bench_market_firehose.exe sweep --tickers 2600 --start 50000 --step 100000 --max 3000000 --dwell 4
```

이 정도 나오면 정상(2026-09-03 실측): load에서 드롭 0·`[PASS]`, E2E 중앙값(p50)=300ns·상위 1%(p99)=32µs대. sweep은 테스트 상한(수백만 msg/s)까지 드롭 0이 이어진다(처리단 천장이 그 위).

## 2. 실 TCP 수신 부하 (코스콤→서버 재현) — bench_feed_ingest

```powershell
# 자체시험: 한 프로세스 안에서 송신(코스콤 emul)+수신(서버)을 loopback으로 연결
.\Quant\build_win\bench_feed_ingest.exe self --tickers 2600 --rate 200000 --duration 20

# 용량 천장 탐색
.\Quant\build_win\bench_feed_ingest.exe sweep --tickers 2600 --start 100000 --step 200000 --max 3000000 --dwell 3
```

두 프로세스로 실제 분리해서 보고 싶으면 (창1 먼저 띄우고, 창2를 이어서):

```powershell
# 창1 — 수신 서버(포트 listen)
.\Quant\build_win\bench_feed_ingest.exe serve --port 47001 --tickers 2600

# 창2 — 코스콤 송신
.\Quant\build_win\bench_feed_ingest.exe send --host 127.0.0.1 --port 47001 --tickers 2600 --rate 200000 --duration 20
```

이 정도 나오면 정상(실측): 드롭 0. offered 20만을 줘도 **achieved ~64k msg/s에서 포화**(메시지당 send() syscall이 천장). net(송신→수신) 중앙값(p50)=16µs대가 e2e를 거의 다 차지하고, proc(수신→주문)는 700ns대. 소켓 수신이 내부 처리보다 지연을 훨씬 많이 차지한다.

## 3. 라이브 실데이터 검증 (규모 아님, 장 중에만)

```powershell
.\Quant\build_win\feed_latency_measure.exe --duration 60 --trade-only 1 --count 40 --universe Quant\config\universe_scan.json Quant\config\config.json
```

`--count 40`은 상한일 뿐 후보가 모자라면 그만큼만 구독한다. 40개를 채우려면 종목 리스트(`--universe`)를 반드시 함께 줘야 한다(안 주면 내장 기본 15종목). 장외(09:00–15:30 KST 밖)에는 틱이 없어 샘플이 0이다.

40종목 하드캡은 app_key당 제약이라, 여러 세션(app_key)을 묶으면 넘을 수 있다. `--sessions creds.json`(JSON 배열 `[{app_key,app_secret,is_paper?,hts_id?}, …]`) 또는 `--configs a.json,b.json`(각 파일의 `"kis"` 블록을 세션 하나로)으로 세션을 여러 개 주면 `--per-session`(세션당 상한, 기본 체결전용 40/호가+체결 20) × 세션 수까지 구독한다:

```powershell
# 전종목 덤프는 커밋되지 않는다 — 먼저 만든다
py PYQuant\tools\full_universe_dump.py --out Quant\config\universe_full.json

# 세션 파일로 300종목(다중 app_key)
.\Quant\build_win\feed_latency_measure.exe --sessions Quant\config\creds.json --universe Quant\config\universe_full.json --count 300
```

이 정도 나오면 정상(실측): drop 0, 관측 msg rate 수십 msg/s, 내부지연 중앙값(p50)=100ns.

## 4. hot path 조각 옛/새 비교 — bench_latency_path

09-13에 들어간 조각들(정수 `hhmmss`·id 배열 현재가 캐시·`strat::Router`·`SymbolId` 키·`TickCapture`·`FeedMux`)을
항목별로 옛 구현과 나란히 잰다. 위 세 하네스는 이 조각들을 지나지 않는다.

```powershell
.\Quant\build_win\bench_latency_path.exe > logs\bench_latency_path.txt
Get-Content logs\bench_latency_path.txt
```

인자는 없다. 7절로 나뉘어 1~4절은 ns/호출, 5~7절은 스레드 경계(캡처 기록 스레드·mux 스레드·전략 스레드)를
100k/s 투입률 맞춤과 최대속도 두 줄로 찍는다. 다음을 지킨다.

- **Release로만**(`build_win`은 Release다). Debug 수치는 비교 대상이 아니다.
- **다른 작업이 없는 머신에서.** 6·7절의 p99 이상은 스케줄러가 정한다. 돌리기 전 `Get-Counter '\Processor(_Total)\% Processor Time'`이
  10% 아래인지 본다. 장중 트레이더와 같이 돌리면 트레이더 꼬리도 넓어진다.
- 5절은 `%TEMP%`에 캡처 파일을 쓰고 끝에 지운다. 디스크가 느리면 "기록 건/s"가 내려간다.
- 이 정도 나오면 정상(09-13 실측, 배경 부하 33%): 시각 디코드 36→4ns, 전략 40개 디스패치 70→10ns, 연쇄 p50 200ns,
  최대속도 2.5M 틱/s, 100k/s 투입률에서 캡처·mux 드롭 0. 결과 표는 리포트 결과 ⑥.

## 5. 함수별 CPU 비중 — perf 핫스팟 (리눅스)

위 네 하네스는 "얼마나 걸리나"를 준다. "어느 함수가 CPU를 먹나"는 안 나온다. 그건 perf로 따로 받는데 윈도우에는 붙일
도구가 없어서 WSL(Ubuntu-24.04) 리눅스 빌드로 받는다. 엔진을 통째로 다시 안 짜도 하네스 타깃 하나만 만들면 된다.

```bash
# 1) 리눅스 빌드 — 한 번만. 빌드 트리는 /root/quant-build 에 이미 잡혀 있다(Release).
wsl -d Ubuntu-24.04 -- cmake --build /root/quant-build --target bench_market_firehose

# 2) 표본 뜨기. /usr/bin/perf 래퍼는 WSL2 커널 버전이 달라 거부하니 linux-tools 바이너리를 직접 부른다.
wsl -d Ubuntu-24.04 -- /usr/lib/linux-tools-6.8.0-139/perf record -q -e cpu-clock -F 499 -o /root/fh.data -- /root/quant-build/bench_market_firehose load --tickers 2600 --rate 200000 --duration 15

# 3) 표 읽기
wsl -d Ubuntu-24.04 -- /usr/lib/linux-tools-6.8.0-139/perf report -i /root/fh.data --stdio -n -q --no-children --sort dso,sym --percent-limit 0.5
```

장중에 돌릴 일이 있으면 `record` 앞에 `taskset -c 8-15`를 붙여 코어를 묶는다. 트레이더가 쓰는 코어를 통째로 가져가지 않는다.
상주 프로세스(트레이더 자신)에 붙일 때는 이 절차를 손으로 하지 않아도 된다 — `PYQuant/core/proc_watch.py`가 같은 perf를
주기로 돌려 `proc_hotspots` 표에 넣는다(그라파나 패널 25). 다만 CPU 5% 아래에서는 안 뜬다.

읽을 때 두 가지를 같이 본다. 안 그러면 병목을 잘못 짚는다.

- **소비 루프(`strategy_fn`·`order_fn`)의 비중은 일한 시간이 아니라 기다린 시간이다.** 이 하네스는 sleep을 안 쓰고
  busy-wait으로 돈다(윈도우 sleep이 부정확해서 그렇게 짰다). 기다리는 동안도 CPU 표본으로 잡히니 두 함수가 30%대로
  나오는 것은 정상이고 병목이 아니다.
- **시계 읽기 비중의 상당 부분은 측정 비용이다.** 하네스가 지연을 재려고 메시지마다 타임스탬프를 찍는다. 이 숫자를
  라이브 엔진의 시계 비용으로 옮겨 적으면 안 된다.

결과는 리포트 결과 ⑦.

## 6. 진짜 Engine 한 바퀴 — bench_engine_load

위 하네스들은 엔진의 조각을 재거나(firehose·latency_path) 실 KIS라 규모가 막힌다(feed_latency_measure).
이것은 **진짜 `Engine`을 띄워** 수신 스레드 N → 샤드 M → 전략 → OrderGate → 체결까지 한 바퀴를 돌린다.

```powershell
# 스레드 수를 쓸어 본다(시세 → 전략 구간만)
.\Quant\build_win\bench_engine_load.exe sweep --tickers 2700 --lane-list 1,2,4,8 --shard-list 1,2,4,8 --seconds 5 --no-orders --rate 0 --out logs\bench_engine_load.csv

# 유량을 올려 가며 천장(드롭 시작점)을 찾는다
.\Quant\build_win\bench_engine_load.exe sweep --tickers 2700 --lanes 4 --shards 4 --seconds 5 --rate-list 100000,200000,400000,800000 --out logs\bench_engine_load.csv
```

### 실제 장만큼만 밀어 분리 전후를 비교할 때

위 두 명령은 천장 탐색이라 실제 장의 10~100배를 민다. 프로세스 분리 전후 비교는 **실측 유량·실전략·발행·DB를 켜고**
재야 비교할 자리가 생긴다. 유량은 엔진이 남긴 체결 캡처에서 뽑는다.

```powershell
# 1) 캡처 → 유량 프로파일(종목별 몫 + 환산 초당 건수)
py scripts\stresstest_flow_profile.py PYQuant\data\ticks_raw\ticks_<epoch>.bin --symbols 2700 --out docs\reports\stresstest\data\<날짜>_flow_profile.json

# 2) 실전략 2,700개 + 발행 켬. 라이브가 127.0.0.1:5555를 쓰므로 바인드 주소를 달리한다
.\Quant\build_win\bench_engine_load.exe run --tickers 2700 --lanes 4 --shards 4 --seconds 20 --profile docs\reports\stresstest\data\<날짜>_flow_profile.json --profile-rate p99 --strategy itb --channel-min 2 --clock-speed 30 --zmq-bind 127.0.0.2 --out docs\reports\stresstest\data\<날짜>_B_pre_split.csv
```

발행을 켜면 리코더를 `TSDB_DB=quant_test`로 띄워 받는다 — 실거래 DB에 시험 주문이 들어가지 않게 가른다. 절차 전체는
[reports/stresstest/README.md](../reports/stresstest/README.md) 6절, 결과는
[B회차 기준선](../reports/stresstest/2026-09-22_B_pre_split_baseline.md).

노브 전체·열 읽는 법·실측값은 [reports/stresstest/README.md](../reports/stresstest/README.md)에 있다.
ctest에 붙이지 않았다 — 수십 초씩 코어를 다 쓴다. **장중에는 돌리지 않는다.**

## 노브 — 수치 바꿔가며 보기

| 노브 | 뜻 | 예 |
|---|---|---|
| `--tickers N` | 종목 수(부하 규모) | `--tickers 5000` |
| `--rate N` | 초당 투입(offered) 메시지 | `--rate 500000` |
| `--duration N` | 측정 시간(초) | `--duration 30` |
| `--zipf S` | 0=균등, 1=대형주 편중(현실 근사) | `--zipf 1.5` |
| `--ob-ratio R` | 호가:체결 비율 | `--ob-ratio 0.8` |
| sweep `--start/--step/--max/--dwell` | 스윕 시작·증가폭·상한·스텝당 초 | `--max 5000000 --dwell 4` |
| `feed_latency_measure` `--count/--per-session/--trade-only` | 구독 종목 상한 / 세션당 상한 / 체결전용 여부 | `--count 40 --trade-only 1` |
| `feed_latency_measure` `--sessions/--configs` | 다중 app_key 세션(40캡 초과) — JSON 배열 / config `"kis"` 블록 쉼표목록 | `--sessions creds.json` |

## 읽는 법

- **drops=0 / lossless=yes가 유지되는 최대 offered rate = 용량 천장**이다. 그 위에서 드롭이 시작된다.
- firehose는 처리단(중앙값 300ns대)이, feed_ingest는 net(송신→수신 16µs대)이 지배하는지 본다. 병목 소재가 계층별로 갈린다.
- tail(상위 0.1%·최악)이 수백µs~ms로 튀는 것은 대개 OS 스케줄러 프리엠션이다(공유 데스크톱). 리포트 "해석: 중앙값과 tail" 참조.

## 트레이더와 동시 실행

장중 모의매매(quant_trader)와 병행하려면 **서로 다른 app_key**여야 WS 세션이 충돌하지 않는다. 트레이더는 모의키(`config_dev_paper.json`)로, 라이브 지연 측정은 실계좌 시세키(`config.json`, 구독만·주문 없음)로 나눠 돌린다. 합성 하네스 2종은 소켓/API를 안 쓰거나 loopback이라 아무 때나 병행 가능하다(같은 머신 CPU를 나눠 쓰므로 tail은 다소 넓어진다).

## 관련 문서

- 결과·해석: [PIPELINE_LATENCY_REPORT.md](../reports/PIPELINE_LATENCY_REPORT.md)
- 지표 약어: [GLOSSARY.md](../GLOSSARY.md)
- 프로젝트 구조: [PROJECT_GUIDE.md](PROJECT_GUIDE.md)
