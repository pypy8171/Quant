# 개장 DB 부하 벤치마크 가이드 (bench_market_open.py)

08:50~09:00 KRX 동시호가(콜옥션)를 압축 재현해 TimescaleDB 쓰기 부하·Grafana 대시보드를 관찰하는
`PYQuant/tools/bench_market_open.py --call-auction`의 실행 절차. 값을 바꿔가며 반복 실행하는 용도라
전 단계를 PowerShell 명령으로 남긴다. C++ 엔진 자체의 지연·처리량 측정은 별도
[LOAD_TEST_GUIDE.md](LOAD_TEST_GUIDE.md) 대상이고, 이 문서는 그것과 무관한 Python/DB 부하 시나리오다.

## 0. 터미널 열고 이동

```powershell
cd C:\Users\PYH\source\repos\Quant\PYQuant
```

## 1. DB·Grafana 컨테이너 상태 확인

이 환경은 Docker Desktop이 WSL2 위에서 돈다 — PowerShell 네이티브에 `docker` 명령이 없다.
반드시 `wsl -e docker ...`로 부른다. 컨테이너는 보통 이미 떠 있으므로 먼저 상태만 본다.

```powershell
wsl -e docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
```

`quant-tsdb`(5432)·`quant-grafana`(3000)가 `Up`이면 2단계로 건너뛴다. 둘 중 하나라도 없으면 저장소
루트에서 올린다.

```powershell
cd C:\Users\PYH\source\repos\Quant
wsl -e docker compose up -d timescaledb grafana
cd PYQuant
```

## 2. DB 접속 환경변수

`TSDB_HOST`는 기본값이 `localhost`라 안 넣어도 되지만, 비밀번호는 반드시 넣어야 한다(미설정 시
DbClient가 기동 실패한다).

```powershell
$env:TSDB_PASSWORD = "changeme"
```

## 3. 파라미터 조정 — config 파일 수정

모든 `--call-auction` 튜닝값은 [PYQuant/config/bench_market_open.json](../../PYQuant/config/bench_market_open.json)
에서 관리한다. 이 파일을 열어 값을 고치고 저장한 뒤 재실행하면 된다(코드 수정 불필요).

| 키 | 뜻 | 현재값 |
|---|---|---|
| `symbols` | 유니버스 종목 수(부하테스트용, 실데이터 아님) | 2700 |
| `preopen_real_minutes` / `preopen_seconds` | 압축 대상 실제 시간(10분) / 압축 후 걸리는 시간(70초) | 10.0 / 70.0 |
| `preopen_ticks` / `preopen_orders` | 08:50~09:00 동안 쌓는 틱·주문 건수(DB row 수 축, 금액과 무관) | 40000 / 1500 |
| `symbol_skew` | 상위 2종목 제외 나머지 종목 간 쏠림(Zipf skew) | 1.2 |
| `top2_share` / `top2_split` | 상위 2종목(삼성전자·SK하이닉스 역할) 합산 비중 / 그 둘 사이 분배비 | 0.45 / 0.55 |
| `auction_value_krw` | 09:00 일괄 체결 목표 총 대금(원) | 700,000,000,000 |
| `auction_first_chunk_ratio` | 09:00 체결 중 첫 청크(거의 동시 체결)에 몰리는 비율 | 0.85 |
| `dead_zone_seconds` | 첫 청크 직후 이벤트 없는 소강 구간(초) | 2.0 |
| `post_open_seconds` / `post_open_multiplier` | 소강 이후 재진입 연속매매 구간 길이(초) / 시작 배율 | 90.0 / 4.0 |
| `auction_seconds` | 09:00 체결 전체(첫 청크+꼬리)가 걸리는 시간(초) | 4.0 |
| `flush_interval` | DB로 배치 insert하는 주기(초) | 0.5 |

## 4. 실행

```powershell
py -m tools.bench_market_open --call-auction --truncate
```

- `--call-auction`: 동시호가 재현 모드(기본 모드는 09:00 이후 단순 연속매매만 도는 별도 시나리오).
- `--truncate`: 실행 전 `bench_*` 테이블을 비운다. 이전 실행 데이터와 섞이지 않게 항상 붙이는 것을 권장.
- config를 코드 수정 없이 일회성으로만 다르게 돌리고 싶으면 같은 이름의 CLI 플래그로 덮어쓴다. 예:
  ```powershell
cd PYQuant
py -m tools.bench_market_open --call-auction --truncate --top2-share 0.5 --auction-value-krw 900000000000
  ```
  CLI 플래그가 config 파일 값보다 항상 우선한다.

### 스트레스(파괴 테스트) 변형

`bench_market_open.json`은 오늘 실측(상위2종목 체결대금 쏠림 45%)에 맞춘 베이스라인이라 그대로 둔다.
DB insert 경로 자체의 한계(병목 지점)를 보고 싶을 때는 `--config`로 별도 파일을 지정한다.

```powershell
py -m tools.bench_market_open --call-auction --truncate --config config/bench_market_open_stress.json
```

[bench_market_open_stress.json](../../PYQuant/config/bench_market_open_stress.json)은 베이스라인과 같은
`top2_share`(0.45, 실측 유지)를 쓰되 09:00 체결을 첫 청크에 100% 몰아넣고(`auction_first_chunk_ratio: 1.0`,
`auction_seconds: 0.5`) 소강 구간을 0.5초로 줄이고 재진입 배율을 8배로 올린 변형이다. `preopen_orders`는
그대로 1,500건으로 뒀다 — 크게 키우면 `_continuous_phase`의 파이썬 단일 루프(`step=0.2`초 while)가 먼저
병목이 돼서 DB 병목을 가리기 때문. 이 값을 더 키워 보려면 파이썬 루프 자체를 먼저 측정해 병목이 어디인지
가른 뒤에 결정한다.

실행 시간은 `preopen_seconds`(기본 70초) + `auction_seconds`(4초) + `dead_zone_seconds`(2초) +
`post_open_seconds`(90초) 순서로 흘러 총 3분 안팎이다. 콘솔에 5초 간격 진행 로그가 찍힌다.

## 5. 결과 확인

### Grafana 대시보드
- http://localhost:3000 (계정 `admin` / `changeme`)
- `quant-ops` 대시보드 — DB부하 패널(초당 insert: ticks/signals/orders/fills)이 실행 구간에 맞춰
  스파이크 → 급락 → 완만한 재상승 곡선을 그리는지 본다. 30초 자동 리로딩이라 기다리면 갱신된다.

### SQL로 직접 검증
`quant-tsdb` 컨테이너에 접속해 초 단위 체결 분포·상위 종목 쏠림을 직접 볼 수 있다.

```powershell
py -c "
import os, psycopg2
os.environ.setdefault('TSDB_PASSWORD', 'changeme')
conn = psycopg2.connect(host='localhost', port=5432, dbname='quant', user='quant', password=os.environ['TSDB_PASSWORD'])
cur = conn.cursor()
cur.execute('''
    SELECT time_bucket('1 second', ts) AS bucket, count(*), sum(filled_qty*filled_price)
    FROM bench_fills GROUP BY bucket ORDER BY bucket LIMIT 20
''')
for row in cur.fetchall():
    print(row)
"
```

상위 2종목 쏠림을 보려면 `GROUP BY ticker ORDER BY sum(filled_qty*filled_price) DESC LIMIT 5`로 바꿔서
같은 방식으로 조회한다.

## 관련 문서
- [PYQuant/tools/bench_market_open.py](../../PYQuant/tools/bench_market_open.py) — 스크립트 본체
- [PYQuant/config/bench_market_open.json](../../PYQuant/config/bench_market_open.json) — 파라미터
- [LOAD_TEST_GUIDE.md](LOAD_TEST_GUIDE.md) — C++ 엔진 자체의 지연·처리량 벤치마크(이 문서와 무관한 별도 대상)
