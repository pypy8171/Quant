# 부하·지연 테스트 실행 가이드

> 시세 파이프라인의 부하·지연을 **직접 돌려 수치를 확인**하는 실행 가이드다.
> 결과 해석과 측정 배경은 [PIPELINE_LATENCY_REPORT.md](../reports/PIPELINE_LATENCY_REPORT.md)에 있다.
> 지표 약어는 [GLOSSARY.md](../GLOSSARY.md) 성능·지연 섹션 참조: 중앙값(p50)·상위 1%(p99)·상위 0.1%(p999)·전 구간(E2E).

## 하네스 — 무엇을 재나

| 하네스 | 소켓 | 재는 것 | 규모 상한 |
|---|---|---|---|
| `bench_market_firehose` | 없음 | 내부 3단 처리단(링버퍼→전략→주문) 지연·처리량 | 없음(합성, 전종목 규모) |
| `bench_feed_ingest` | 실 TCP(loopback) | 코스콤→서버 소켓 수신 경로(net/proc/e2e 분해) | 없음(합성, 전종목 규모) |
| `feed_latency_probe` | 실 KIS WS | 실데이터로 수신콜백→주문결정 내부 지연 재확인 | **app_key당 ~40종목**(API 하드캡) |
| `bench_hot_path` | 없음 | 09-13 hot path 조각(시각 디코드·현재가 캐시·라우터·상태 키·캡처·FeedMux 홉·연쇄)의 옛/새 A/B | 없음(합성, 종목 2,600개 고정) |

부하테스트의 규모는 앞의 둘(합성)이 담당한다. 라이브 지연 측정은 규모가 아니라 "합성이 낸 처리단 지연이 실데이터에서도 성립하는가"를 확인하는 용도다(장 중에만 틱이 있음).

## 0. 준비 — cmd(PowerShell) 열고 이동

```powershell
Set-Location "$env:USERPROFILE\source\repos\Quant"
```

빌드가 안 돼 있으면(exe 없으면) 개발셸에서 한 번:

```powershell
$env:TEMP="C:\build_tmp"
cmake --build Quant/build_win --target bench_market_firehose bench_feed_ingest feed_latency_probe bench_hot_path
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
.\Quant\build_win\feed_latency_probe.exe --duration 60 --trade-only 1 --count 40 --universe Quant\config\universe_scan.json Quant\config\config.json
```

`--count 40`은 상한일 뿐 후보가 모자라면 그만큼만 구독한다. 40개를 채우려면 종목 리스트(`--universe`)를 반드시 함께 줘야 한다(안 주면 내장 기본 15종목). 장외(09:00–15:30 KST 밖)에는 틱이 없어 샘플이 0이다.

40종목 하드캡은 app_key당 제약이라, 여러 세션(app_key)을 묶으면 넘을 수 있다. `--sessions creds.json`(JSON 배열 `[{app_key,app_secret,is_paper?,hts_id?}, …]`) 또는 `--configs a.json,b.json`(각 파일의 `"kis"` 블록을 세션 하나로)으로 세션을 여러 개 주면 `--per-session`(세션당 상한, 기본 체결전용 40/호가+체결 20) × 세션 수까지 구독한다:

```powershell
# 전종목 덤프는 커밋되지 않는다 — 먼저 만든다
py PYQuant\tools\full_universe_dump.py --out Quant\config\universe_full.json

# 세션 파일로 300종목(다중 app_key)
.\Quant\build_win\feed_latency_probe.exe --sessions Quant\config\creds.json --universe Quant\config\universe_full.json --count 300
```

이 정도 나오면 정상(실측): drop 0, 관측 msg rate 수십 msg/s, 내부지연 중앙값(p50)=100ns.

## 4. hot path 조각 옛/새 비교 — bench_hot_path

09-13에 들어간 조각들(정수 `hhmmss`·id 배열 현재가 캐시·`strat::Router`·`SymbolId` 키·`TickCapture`·`FeedMux`)을
항목별로 옛 구현과 나란히 잰다. 위 세 하네스는 이 조각들을 지나지 않는다.

```powershell
.\Quant\build_win\bench_hot_path.exe > logs\bench_hot_path.txt
Get-Content logs\bench_hot_path.txt
```

인자는 없다. 7절로 나뉘어 1~4절은 ns/호출, 5~7절은 스레드 경계(캡처 기록 스레드·mux 스레드·전략 스레드)를
100k/s 투입률 맞춤과 최대속도 두 줄로 찍는다. 다음을 지킨다.

- **Release로만**(`build_win`은 Release다). Debug 수치는 비교 대상이 아니다.
- **다른 작업이 없는 머신에서.** 6·7절의 p99 이상은 스케줄러가 정한다. 돌리기 전 `Get-Counter '\Processor(_Total)\% Processor Time'`이
  10% 아래인지 본다. 장중 트레이더와 같이 돌리면 트레이더 꼬리도 넓어진다.
- 5절은 `%TEMP%`에 캡처 파일을 쓰고 끝에 지운다. 디스크가 느리면 "기록 건/s"가 내려간다.
- 이 정도 나오면 정상(09-13 실측, 배경 부하 33%): 시각 디코드 36→4ns, 전략 40개 디스패치 70→10ns, 연쇄 p50 200ns,
  최대속도 2.5M 틱/s, 100k/s 투입률에서 캡처·mux 드롭 0. 결과 표는 리포트 결과 ⑥.

## 노브 — 수치 바꿔가며 보기

| 노브 | 뜻 | 예 |
|---|---|---|
| `--tickers N` | 종목 수(부하 규모) | `--tickers 5000` |
| `--rate N` | 초당 투입(offered) 메시지 | `--rate 500000` |
| `--duration N` | 측정 시간(초) | `--duration 30` |
| `--zipf S` | 0=균등, 1=대형주 편중(현실 근사) | `--zipf 1.5` |
| `--ob-ratio R` | 호가:체결 비율 | `--ob-ratio 0.8` |
| sweep `--start/--step/--max/--dwell` | 스윕 시작·증가폭·상한·스텝당 초 | `--max 5000000 --dwell 4` |
| `feed_latency_probe` `--count/--per-session/--trade-only` | 구독 종목 상한 / 세션당 상한 / 체결전용 여부 | `--count 40 --trade-only 1` |
| `feed_latency_probe` `--sessions/--configs` | 다중 app_key 세션(40캡 초과) — JSON 배열 / config `"kis"` 블록 쉼표목록 | `--sessions creds.json` |

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
