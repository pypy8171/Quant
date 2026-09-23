# 부하테스트 결과 모음

여기는 **부하를 걸어 얻은 수치와 그 수치로 내린 판단**을 회차별로 모아 두는 곳이다.
하네스가 어떤 것들이 있고 무엇을 재는지는 [docs/guides/LOAD_TEST_GUIDE.md](../../guides/LOAD_TEST_GUIDE.md)에 있다.
여기 문서들은 "그 하네스로 언제 무엇을 재서 무슨 결론을 냈는가"를 남긴다.

## 왜 모으는가

D-071 설계 원칙 7은 "성능을 이유로 고칠 때만 먼저 잰다"이다. 그러면 잰 값이 판단의 근거로 남아야 하는데,
지금까지는 세션이 끝나면 콘솔 출력과 함께 사라졌다. 남기지 않으면 세 가지가 안 된다.

1. **전후 비교** — 프로세스 분리(시세/전략/주문) 뒤에 다시 재려면 분리 전 기준선이 있어야 한다.
2. **재현** — 같은 명령을 다시 돌려 다른 사람이 같은 자릿수를 얻는지 확인해야 한다.
3. **번복 추적** — 어떤 수치를 보고 어떤 선택을 했는지가 남아야, 나중에 그 선택을 뒤집을 때 근거를 대조할 수 있다.

## 회차 색인

이 폴더의 회차뿐 아니라 저장소 전체의 부하·성능 측정 스물여섯 번을 그래프로 모은 장이 따로 있다 — 저장소 `README.md`의 "결과 보기" 표에서 열 수 있다. 숫자는 아래 회차 문서와 `data/`의 CSV, 그리고 `docs/reports/PIPELINE_LATENCY_REPORT.md`·`docs/reports/FEED_MEASURE.md`가 정본이고, 그 장은 보기만 쉽게 한 것이다.

| 날짜 | 문서 | 하네스 | 무엇을 바꿔가며 쟀나 | 결론 한 줄 |
|---|---|---|---|---|
| 2026-09-22 | [엔진 전 구간 부하](2026-09-22_engine_full_path.md) | `bench_engine_load` | 수신 스레드 1·2·4·8 × 전략 샤드 1·2·4·8, 투입 유량 100~800,000건/초 | 천장은 수신부가 아니라 **샤드→전략 스레드 큐(약 40만/초)**와 **주문 경로(초당 200건대)**다 |
| 2026-09-22 | [A회차 — CPU·스레드 표본을 붙여서](2026-09-22_A_cpu_sampled.md) | `bench_engine_load` + `procwatch` | 위와 같은 39구성, 프로세스 CPU·메모리·스레드를 2초마다 | 8레인은 **16코어 중 10개만 쓰고도 느려진다** → 코어 부족이 아니라 스레드 다툼. 주문 경로는 CPU 2코어 밑 → I/O 대기 |
| 2026-09-22 | [B회차 — 프로세스 분리 전 기준선](2026-09-22_B_pre_split_baseline.md) | `bench_engine_load` + `procwatch` + 리코더 | 실측 캡처에서 뽑은 유량 4구간(초당 1,592~73,315) × 실전략(ITB)·카운터, 발행·DB 켬 | 단일 프로세스는 **실제 장 최대 유량을 0.2코어로 드롭 없이** 받는다. 좁은 곳은 발행→DB(신호 5.8%만 적재)와 주문 경로(초당 300건대) |
| 2026-09-22 | [C회차 — 전 종목 1초 피드로 전략·주문까지](2026-09-22_C_http_1hz_feed.md) | `bench_engine_load --source http` | 실제 장에서 2,700종목을 주기마다 통째로 받아 전략·주문까지. 주기 1초 vs 7초 | **된다** — 한 바퀴 107ms, 주문 지연 p99 243ms로 1초 예산 안. 다만 시세는 초당 184건으로 실제 체결의 1/8이라 체결 하나하나를 보는 전략은 못 태운다 |
| 2026-09-23 | [D~I회차 — 주문 한 건의 2밀리초를 구간으로](2026-09-23_order_stage_breakdown.md) | `load_injector.py` + `config_load_test.json` | 주문 구간 9개 → 12개로 가른 뒤, 주문 스레드가 하던 파일 쓰기 둘을 차례로 전담 스레드로 빼고 몇 분 간격으로 앞뒤 대조 | 2밀리초는 후보 셋(증권사 왕복·중복 가드·원장 CSV) 어디도 아닌 **미결주문 파일 다시쓰기가 절반**이었다. 그것을 빼서 `pop→반환` 1.81배, 남은 원장 CSV·사유 줄까지 빼서 1.45배 — 누적 대략 2.6배. 드러난 다음 자리는 이력 잠금 713µs |
| 2026-09-23 | [J~L회차 — DB 적재기까지 붙여서](2026-09-23_db_ingest_cost.md) | `load_injector.py` + `config_load_test.json` + 적재기 | 종목당 3만·5만·7만을 적재기 없이·붙여서 두 번씩. 투입기 메모리를 종목 묶음으로 나눠 큰 회차를 돌 수 있게 고친 뒤 | 적재기를 붙이면 `꺼냄 → 반환`이 **1.5~1.7배** 느려지고, 늘어난 몫은 거의 전부 이력 저장이다. 적재는 입력을 2.3배 올려도 **초당 272~372행에서 평평**하다 — 09-22의 5.8%, 회차 B의 0.013%와 같은 천장 |
| 2026-09-23 | [M회차 — 버린 건수를 원인별로](2026-09-23_drop_breakdown.md) | `load_injector.py` + `config_load_test.json` + 적재기 | J회차와 같은 입력(종목당 3만)을 적재기 없이·붙여서, 버린 건수를 원인 넷으로 갈라 요청·응답 소켓으로 읽어가며 | 버린 2,300만 건이 **전부 체결 링 만석**이고 소켓 쪽은 0이다. 주문은 엔진이 한 건도 안 버렸는데 DB에는 56.5%만 남아, 구멍은 엔진 밖에 있다. 버린 건수가 `health` 로 안 남던 이유는 그 메시지가 **가장 먼저 버려지는 쪽**이었기 때문이다 |

1차·A회차는 **단일 엔진 안쪽 경로의 천장 탐색**이다 — 발행·DB를 끄고 실제 장보다 10~100배 위 유량을 밀었다. 프로세스 분리
전후 비교의 **분리 전 기준선은 [B회차](2026-09-22_B_pre_split_baseline.md)**(실측 유량 프로파일·발행·DB 켬·실전략, 코드 해시 명기)가
맡는다. 두 문서의 한계 절에 같은 말이 있다.

09-23 회차는 B부터 M까지 열둘이고, 문서로 남은 것은 D~I와 J~L, M이다. 기준선 회차 B와 되민 회차 C는 아직 위에서 말한 그래프 장에만 있다.

원자료는 [data/](data/)에 `<날짜>_<하네스>.csv` 로 둔다. 자원 표본을 같이 받은 회차는 `<날짜>_<회차>_procwatch_samples.csv`(수집기 원자료)와
`<날짜>_<회차>_joined.csv`(하네스 행과 시각으로 맞춘 표)를 옆에 둔다. 실측 유량 프로파일은 `<날짜>_flow_profile.json`이다.

## 회차를 적는 규칙

회차는 앞 회차에서 조건 하나를 바꿔 다시 잰 것이다. 그래서 **회차 문서와 대시보드의 회차 탭은 맨 위에 "이전 회차 대비"를
적는다** — 바꾼 것, 같게 둔 것, 달라진 숫자. 첫 회차는 "첫 회차 — 견줄 앞이 없다"로 적는다. 이것을 빼면 숫자 차이가
고친 효과인지 조건 차이인지 읽는 쪽에서 가를 수 없다. 대시보드는 날짜마다 회차끼리 견주는 통합 요약 탭을 하나 둔다.

## 직접 돌려 보기

### 1. 빌드

**cmd 창을 연다**(시작 → `cmd` 입력 → Enter). 그다음 그대로 붙여넣는다.

```cmd
cd /d C:\Users\...\source\repos\Quant
..\quant-devtools\wt_build.cmd
```

워크트리에서 돌린다면 `cd /d` 뒤를 그 워크트리 경로로 바꾼다. 빌드가 끝나면
`Quant\build_win\bench_engine_load.exe` 가 생긴다. 중간에 LNK1104가 나면 실행 중인 프로세스가
exe를 잡고 있는 것이니 그 프로세스를 먼저 내린다.

### 2. 한 번만 돌려 보기

```cmd
Quant\build_win\bench_engine_load.exe run --tickers 2700 --lanes 4 --shards 4 --seconds 10
```

CSV 한 줄이 화면에 찍힌다. 파일로도 남기려면 `--out logs\bench_engine_load.csv` 를 붙인다
(폴더가 없으면 하네스가 만든다).

### 3. 스레드 수를 쓸어 보기

```cmd
Quant\build_win\bench_engine_load.exe sweep --tickers 2700 --lane-list 1,2,4,8 --shard-list 1,2,4,8 ^
    --seconds 5 --no-orders --rate 0 --out logs\bench_engine_load.csv
```

`^` 는 cmd에서 줄을 잇는 기호다. 한 줄로 붙여 써도 된다. 16개 구성을 도느라 2분쯤 걸린다.

### 4. 유량을 올려 가며 천장 찾기

```cmd
Quant\build_win\bench_engine_load.exe sweep --tickers 2700 --lanes 4 --shards 4 ^
    --seconds 5 --rate-list 100000,200000,400000,800000 --out logs\bench_engine_load.csv
```

### 5. CPU·스레드 표본을 같이 받기(A회차 방식)

"느려졌다"만으로는 코어가 모자란 것인지 스레드가 서로 기다리는 것인지 못 가린다. 하네스를 돌리는 동안
다른 창에서 자원 수집기를 붙인다. **창 둘**이 필요하다.

```cmd
:: 창 1 — 수집기를 먼저 띄운다(하네스 이름은 반드시 .exe까지, --wsl-distro는 주지 않는다)
cd /d C:\Users\...\source\repos\Quant
PYQuant\.venv-win\Scripts\python.exe PYQuant\main.py procwatch --process-name bench_engine_load.exe --interval 2
```

수집기는 `.env`의 `TSDB_PASSWORD`가 필요하다(저장소 루트, 커밋되지 않는다). 없다고 나오면 그 창에서
`set TSDB_PASSWORD=...`를 먼저 친다. `psutil이 설치되지 않았습니다`가 나오면
`PYQuant\.venv-win\Scripts\python.exe -m pip install "psutil>=5.9.0"`.

```cmd
:: 창 2 — 하네스. 3·4절 명령 그대로, --out만 data/ 쪽으로
Quant\build_win\bench_engine_load.exe sweep --tickers 2700 --lane-list 1,2,4,8 --shard-list 1,2,4,8 ^
    --seconds 5 --no-orders --rate 0 --out docs\reports\stresstest\data\<날짜>_A_cpu_sampled.csv
```

하네스가 끝나면 창 1을 Ctrl+C로 내리고, 두 자료를 시각으로 맞춘다.

```cmd
py scripts\stresstest_join_procwatch.py docs\reports\stresstest\data\<날짜>_A_cpu_sampled.csv ^
    logs\procwatch_bench_engine_load.log --samples-out docs\reports\stresstest\data\<날짜>_A_procwatch_samples.csv ^
    > docs\reports\stresstest\data\<날짜>_A_joined.csv
```

맞춘 표의 `cpu_avg_cores`·`cpu_max_cores`는 psutil %를 100으로 나눈 **코어 개수**다. `cpu_samples`가 0인 행은 빈칸으로
남는다(0으로 채우면 "CPU를 안 썼다"로 읽힌다). 구성당 5초·2초 주기면 표본이 2~3개라, 표본을 더 원하면 `--seconds 10`.
수집기는 프로세스 CPU만 받는다 — 머신 전체를 같이 보려면 창 3에서
`powershell Get-Counter '\Processor(_Total)\% Processor Time' -Continuous -SampleInterval 2`를 켠다.

### 6. 실측 유량·발행·DB를 켜고 재기(B회차 방식)

프로세스 분리 전후를 비교하려면 실제 장에서 오는 만큼의 유량을 밀고, 발행·DB까지 켜야 한다.

```cmd
:: (1) 캡처에서 유량 프로파일을 뽑는다. 입력은 엔진이 capture_dir에 남긴 ticks_<epoch>.bin
py scripts\stresstest_flow_profile.py PYQuant\data\ticks_raw\ticks_<epoch>.bin ^
    --symbols 2700 --out docs\reports\stresstest\data\<날짜>_flow_profile.json
```

```cmd
:: (2) 시험 DB를 쓰는 리코더를 띄운다. 실거래 DB(quant)에 시험 주문이 들어가지 않게 DB를 가른다
set TSDB_DB=quant_test
PYQuant\.venv-win\Scripts\python.exe PYQuant\main.py record --host 127.0.0.2 --port 5555
```

```cmd
:: (3) 하네스. 라이브 트레이더가 127.0.0.1:5555를 쓰므로 바인드 주소를 달리한다
Quant\build_win\bench_engine_load.exe run --tickers 2700 --lanes 4 --shards 4 --seconds 20 ^
    --profile docs\reports\stresstest\data\<날짜>_flow_profile.json --profile-rate p99 ^
    --strategy itb --channel-min 2 --clock-speed 30 --zmq-bind 127.0.0.2 ^
    --out docs\reports\stresstest\data\<날짜>_B_pre_split.csv
```

`--strategy itb`는 종목마다 `IntradayBreakoutStrategy` 하나를 붙인다(2,700개, 라이브와 같은 배치). 이 전략은 분봉이 쌓여야
채널이 생기므로 `--clock-speed`로 합성 장시계를 빠르게 돌리고 `--channel-min`으로 채널을 짧게 잡는다. 둘을 빼면 신호가 0이다.

### 7. 실제 장 시세를 주기마다 통째로 받아 재기(C회차 방식)

합성 유량은 "엔진이 이만큼을 견디는가"에 답하고, "이만큼을 실제로 받아올 수 있는가"에는 답하지 못한다. 후자는
**장중에** 실제 장 시세를 붙여 잰다. 유량을 우리가 정하지 않으므로 `--rate`·`--profile`·`--clock-speed`는 안 쓴다.

```powershell
.\Quant\build_win\bench_engine_load.exe run --tickers 2700 --lanes 4 --shards 4 --seconds 300 `
    --source http --sweep-ms 1000 --strategy itb --channel-min 2 `
    --out docs\reports\stresstest\data\<날짜>_C_http_1hz.csv
```

결과 열 중 `feed_`로 시작하는 것이 피드 쪽 값이다 — `feed_sweeps`(돈 바퀴) · `feed_calls`/`feed_failures`(요청과
빈 응답) · `feed_quotes`(응답에 실려 온 종목 수 합) · `feed_megabytes` · `feed_sweep_average_ms`/`feed_sweep_max_ms` ·
`feed_overruns`(한 바퀴가 주기를 넘긴 횟수).

**`feed_failures`가 0이 아니면 값을 못 받은 것이다.** 빈 본문도 실패로 센다. 같은 주소를 라이브 트레이더의
유니버스 스캔(`scripts/live_prices_feed.py`)이 같은 IP로 부르므로, 여기가 실패하면 실매매 시세가 같이 먼다.
주기·묶음을 줄여도 실패가 이어지면 그날은 그만 돌린다. 결과와 이 엔드포인트를 두고 확인한 사실은
[C회차](2026-09-22_C_http_1hz_feed.md) 7절.

### 주의

- **장중에는 돌리지 않는다.** 코어를 다 쓴다. 트레이더가 떠 있으면 그쪽에 돌아갈 CPU가 없다.
- ctest에 붙어 있지 않다. 부하 하네스의 관례다(`bench_market_firehose`와 같다).
- 산출물(`latency_trace.csv`·`open_orders.txt`·합성 체결 원장)은 실행파일 옆 `Quant/build_win/logs_bench/`에 남고, 구성 시작마다
  `open_orders.txt`를 지운다. 기본 `logs/`를 쓰면 다음에 뜨는 트레이더·`test_engine`이 그 수만 건을 이전 세션 미체결로 읽어 취소하러 간다.
- 다른 세션이 같은 PC에서 빌드 중이면 같은 구성이 ±50%까지 흔들린다. 절대값보다 모양을 읽는다.

## 노브

| 노브 | 뜻 | 기본값 |
|---|---|---|
| `--tickers N` | 종목 수. `Quant/config/universe_full.json`에서 앞에서부터 N개를 쓴다 | 2700 |
| `--lanes N` / `--lane-list a,b,c` | 수신(WebSocket) 스레드 수. 한 종목은 한 레인만 내보낸다 | 4 |
| `--shards N` / `--shard-list a,b,c` | 전략 샤드 수. 샤드 하나가 전략 하나를 소유한다(D-110) | 4 |
| `--seconds N` | 한 구성당 측정 시간 | 20 |
| `--rate N` / `--rate-list a,b,c` | 초당 투입 건수(offered). 0이면 최대 속도 | 0 |
| `--order-every N` | N건마다 주문 하나. 1이면 체결마다 | 1 |
| `--no-orders` | 주문을 아예 안 낸다(= `--order-every 0`). 시세→전략 구간만 본다 | — |
| `--zipf S` | 거래량 쏠림. 0이면 종목 균등, 1이면 대형주 편중(현실 근사) | 1.0 |
| `--universe <경로>` | 종목 목록 파일 | `Quant/config/universe_full.json` |
| `--out <경로>` | CSV 이어쓰기 | 없음(화면만) |
| `--log-level debug\|info\|warn\|error` | 로그 하한. 기본 error — 건마다 찍는 로그의 파일 쓰기가 재려는 구간보다 길다 | error |
| `--profile <경로>` | 실측 유량 프로파일 JSON. 종목별 몫(`rank_shares`)이 `--zipf`를 대신한다 | 없음(합성 zipf) |
| `--profile-rate p50\|p90\|p99\|max` | 프로파일의 환산 초당 건수 중 어느 구간을 쓸지. `--rate` 대신 쓴다 | 없음 |
| `--strategy counter\|itb` | `counter`는 체결마다 주문(파이프라인 천장용), `itb`는 종목마다 실전략 하나 | counter |
| `--channel-min N` | `itb` 전략의 채널 길이(분). 짧은 구간을 잴 때 줄인다 | 10 |
| `--clock-speed X` | 합성 장시계 배속. 09:00에서 시작해 실제 1초가 장 X초로 흐른다 | 1.0 |
| `--zmq-bind <주소>` | 발행(ZMQ) 켬. 라이브가 쓰는 `127.0.0.1`과 겹치면 안 된다 | 없음(발행 끔) |
| `--source synthetic\|http` | `synthetic`은 하네스가 유량을 만든다. `http`는 실제 장 시세를 주기마다 통째로 받아 유량을 시장이 정한다(장중에만 뜻이 있다) | synthetic |
| `--sweep-ms N` | `http` 한 바퀴 주기(ms). 1000과 7000 두 값으로 쟀다 | 1000 |
| `--codes-per-call N` | `http` 한 요청에 묶을 종목 수. 레인 하나가 맡은 종목이 이보다 많으면 여러 번 나눠 부른다 | 900 |

## 결과 열 읽는 법

| 열 | 뜻 |
|---|---|
| `offered_per_sec` | 발생기가 **내보낸** 초당 건수 |
| `accepted_per_sec` | 샤드까지 **들어간** 초당 건수. 이 둘이 갈라지는 지점이 수신부 천장이다 |
| `drop_pct` | 수신 → 샤드 셀에서 버린 비율 |
| `signals` | 전략이 내서 전략 스레드 큐에 **들어간** 신호 수 |
| `orders` | OrderGate를 통과해 실제로 나간 주문 수 |
| `shard_high_water` | 샤드 셀 최고 수위. 상한 4096에 붙어 있으면 그 구간이 포화다 |
| `trade_dropped` | 수신 → 샤드 셀에서 버린 건수 |
| `shard_dropped` | 샤드 → 전략 스레드 큐에서 버린 신호 수 |
| `order_dropped` | 전략 스레드 → 주문 큐에서 버린 신호 수 |
| `p50_us` / `p99_us` / `max_us` | 주문 한 건의 전 구간 지연(마이크로초). 엔진이 쓰는 `latency_trace.csv`에서 이번 실행분만 읽는다 |

`p50_us`가 커진 회차는 그 `latency_trace.csv`를 직접 연다. `total_us` 뒤 여섯 열(`gate_us`·`history_guard_us`·
`journal_us`·`bucket_wait_us`·`transport_us`·`record_us`)이 `pop_to_done_us` 한 덩이를 가른 몫이고, 합이 그 덩이에
거의 닿는다(D-117 후속). 이 여섯이 없던 회차 B·C는 2.07ms·3.31ms가 어디서 온 것인지 표만 보고는 말할 수 없다.
09-23 짧은 회차 둘에서 답이 `record_us` 하나로 나왔다(`pop_to_done_us`의 99%). 그 안에서도 미결주문 파일
다시쓰기가 절반이다 — `open_orders_us` 열이 그 몫이고, `record_us` 안에 든 값이라 더할 때는 뺀다.
| `strategy` | 그 구성이 쓴 전략(`counter` 또는 `itb`) |
| `zmq` | 발행 바인드 주소, 껐으면 `off` |
| `started_at` | 구성 시작 벽시계(HH:MM:SS). 5절의 수집기 표본과 이 행을 맞추는 열쇠 |

회계가 맞는지는 `ticks_emitted == signals + shard_dropped + trade_dropped` 로 확인한다(주문 켠 구성 기준).
안 맞으면 세지 않는 드롭 경로가 새로 생긴 것이다.

## 새 회차를 추가할 때

1. `data/<날짜>_<하네스>.csv` 에 원자료를 둔다.
2. `<날짜>_<주제>.md` 에 **왜 쟀는지 · 무엇을 바꿨는지 · 수치 · 그래서 무엇을 정했는지 · 이 측정의 한계**를 적는다.
   한계를 빼면 다음 사람이 숫자를 과신한다.
3. 위 회차 색인표에 줄을 추가한다.
4. [docs/FILE_INDEX.md](../../FILE_INDEX.md)에 새 파일 설명을 채운다.
