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

| 날짜 | 문서 | 하네스 | 무엇을 바꿔가며 쟀나 | 결론 한 줄 |
|---|---|---|---|---|
| 2026-09-22 | [엔진 전 구간 부하](2026-09-22_engine_full_path.md) | `bench_engine_load` | 수신 스레드 1·2·4·8 × 전략 샤드 1·2·4·8, 투입 유량 100~800,000건/초 | 천장은 수신부가 아니라 **샤드→전략 스레드 큐(약 40만/초)**와 **주문 경로(초당 200건대)**다 |
| 2026-09-22 | [A회차 — CPU·스레드 표본을 붙여서](2026-09-22_A_cpu_sampled.md) | `bench_engine_load` + `procwatch` | 위와 같은 39구성, 프로세스 CPU·메모리·스레드를 2초마다 | 8레인은 **16코어 중 10개만 쓰고도 느려진다** → 코어 부족이 아니라 스레드 다툼. 주문 경로는 CPU 2코어 밑 → I/O 대기 |

1차·A회차는 **단일 엔진 안쪽 경로의 천장 탐색**이다 — 발행·DB를 끄고 실제 장보다 10~100배 위 유량을 밀었다. 프로세스 분리
전후 비교의 **분리 전 기준선은 B회차**(실측 유량 프로파일·발행·DB 켬·실전략, 코드 해시 명기)가 맡는다. 두 문서의 한계 절에 같은 말이 있다.

원자료는 [data/](data/)에 `<날짜>_<하네스>.csv` 로 둔다. 자원 표본을 같이 받은 회차는 `<날짜>_<회차>_procwatch_samples.csv`(수집기 원자료)와
`<날짜>_<회차>_joined.csv`(하네스 행과 시각으로 맞춘 표)를 옆에 둔다.

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
| `started_at` | 구성 시작 벽시계(HH:MM:SS). 5절의 수집기 표본과 이 행을 맞추는 열쇠 |

회계가 맞는지는 `ticks_emitted == signals + shard_dropped + trade_dropped` 로 확인한다(주문 켠 구성 기준).
안 맞으면 세지 않는 드롭 경로가 새로 생긴 것이다.

## 새 회차를 추가할 때

1. `data/<날짜>_<하네스>.csv` 에 원자료를 둔다.
2. `<날짜>_<주제>.md` 에 **왜 쟀는지 · 무엇을 바꿨는지 · 수치 · 그래서 무엇을 정했는지 · 이 측정의 한계**를 적는다.
   한계를 빼면 다음 사람이 숫자를 과신한다.
3. 위 회차 색인표에 줄을 추가한다.
4. [docs/FILE_INDEX.md](../../FILE_INDEX.md)에 새 파일 설명을 채운다.
