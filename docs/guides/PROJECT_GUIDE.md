# Quant Trading System — 프로젝트 가이드

> 최종 업데이트: 2026-09-19 (엔진은 데이터→전략 샤드 M→디스패치→주문 파이프라인에 체결 소비·제어 스레드를 더한 구성, D-071. 현행 요약은 [../ENGINE_ARCHITECTURE.md](../ENGINE_ARCHITECTURE.md), 읽는 순서는 [../CODE_FLOW.md](../CODE_FLOW.md). ZMQ IPC·TimescaleDB는 목표 아키텍처이며 현 C++ 엔진 탑재 범위는 그 두 문서 기준)

---

## 목차

1. [전체 아키텍처](#1-전체-아키텍처)
2. [프로젝트 구조](#2-프로젝트-구조)
3. [Docker — 설치 및 운영 명령어](#3-docker--설치-및-운영-명령어)
4. [TimescaleDB — 설치 구조와 PostgreSQL과의 관계](#4-timescaledb--설치-구조와-postgresql과의-관계)
5. [플랫폼별 실행 방법 (Windows / Linux)](#5-플랫폼별-실행-방법-windows--linux)
6. [FEP 레이어 — OrderRouter + OrderGate](#6-fep-레이어--orderrouter--ordergate)
7. [실제 매매 연결 로드맵](#7-실제-매매-연결-로드맵)
8. [디스크 용량 관리](#8-디스크-용량-관리)

---

## 1. 전체 아키텍처

### 데이터 흐름 다이어그램

```
KIS OpenAPI
  REST(봉·현재가·주문) ──▶ [데이터 스레드] ──▶ pipeline_.bars_matrix · pipeline_.trade_matrix(데이터 행 data_row)
  WebSocket(호가·체결) ──▶ [WS 수신 스레드 i = 레인 i] ──▶ pipeline_.order_book_matrix · pipeline_.trade_matrix(행 i)
  WebSocket(체결통보) ──▶ [WS 수신 스레드] ──▶ pipeline_.fill_queue ──▶ [체결 소비 스레드] ──▶ OrderRouter::on_fill

행렬 열 m ──▶ [샤드 스레드 m] ──▶ pipeline_.shard_out(MpscQueue) ──▶ [전략(디스패치) 스레드]
          ──▶ pipeline_.order_queue(RingBuffer 1024) ──▶ [주문 스레드] OrderRouter → OrderGate → IOrderExecutor(KisClient)

[제어 스레드]  잔고 대조 · 토큰 선갱신 · WS 단절 판정  (파이프라인 밖)
ZmqBridge(HAS_ZMQ, 내부 스레드)  PUB :5555 / REP :5556  →  quant-recorder → TimescaleDB  (목표 아키텍처, 3·4절)
OpsServer(내부 스레드)           운영단말 TCP — 조회·수동주문·KILL (D-043)
```

### 스레드 모델 (C++ Engine 내부)

스레드 함수는 `Quant/include/core/Engine.h`의 `data_thread_fn`·`shard_thread_fn`(샤드 M개)·`strategy_thread_fn`·
`order_thread_fn`·`fill_thread_fn`·`control_thread_fn`이고, 큐는 구조체 하나 `pipeline_`(`ShardPipeline`)에 모여 있다.
다섯 스레드 + 샤드 M개(config `strategy_shards`, 기본 1)에 WS 소켓마다 수신 스레드 하나가 더 붙는다.

| 스레드 | 하는 일 | 큐 |
|---|---|---|
| 데이터 | KIS REST 봉·현재가 폴링(`fetch_interval_sec`)·유니버스 재스캔. 매매 창 안에서만 — 정규장 09:00~15:30 + 애프터마켓 16:00~20:00 KST(D-097), 모의계좌(`is_paper`)는 15:30까지(`Quant/src/core/AppConfig.cpp`의 `parse_risk`) | `bars_matrix`(1024)·`trade_matrix` 데이터 행 |
| WS 수신(레인 i, 소켓마다 하나) | 디코드 뒤 행렬 행 i에 push. 체결통보는 `fill_queue`에 push만(가득 차면 드롭 계수, D-056) | `order_book_matrix`(4096)·`trade_matrix`(4096)·`fill_queue`(1024) |
| 샤드 m | 자기 열의 틱을 비우고 열 m의 전략을 부른다(종목 해시로 열을 고른다) | `shard_out`(MpscQueue 4096)에 넣는다 |
| 전략(디스패치) | `SignalDispatcher`가 순번 stamp·신규 차단·교체 진입을 판단하고 주문 큐로 넘긴다 | `order_queue`(RingBuffer 1024, D-073)에 넣는다 |
| 주문 | `OrderRouter::submit` → `OrderGate::check` → `IOrderExecutor::submit_order`. 호출 간격·재시도는 `OrderPacer` | `order_queue` 소비 |
| 체결 소비 | `fill_queue` → `OrderRouter::on_fill`(원장·CSV) → 운영단말 방송 | `fill_queue` 소비 |
| 제어 | 잔고 대조·토큰 선갱신·WS 단절 판정(`feed::Supervisor`)·큐 고수위 로그 | 파이프라인 밖 |

유휴 소비자는 슬립 폴링이 아니라 `Quant/include/core/WakeGate.h`의 `sync::WakeGate`로 잔다 — 생산자가 push 뒤 notify하고
소비자는 큐가 비면 condvar에서 기다린다(전략 스레드는 200µs yield 뒤). Windows 타이머 격자에서 `sleep_for(100µs)`는
실측 중앙값(p50) 15.6ms라 그렇다(D-071). 세부 흐름은 [../CODE_FLOW.md](../CODE_FLOW.md), 모듈 책임은
[../ENGINE_ARCHITECTURE.md](../ENGINE_ARCHITECTURE.md)에 있으니 여기서는 되풀이하지 않는다.

### 사용 기술 스택

| 영역 | 기술 |
|------|------|
| C++ 빌드 | C++23, CMake 3.20+, Ninja, GCC 14(Linux) / MSVC 14.44(Windows, VS 2022 17.14+) — D-070 |
| HTTP (C++) | WinHTTP (Windows) / libcurl (Linux) |
| WebSocket (C++) | KIS WebSocket (`ops.koreainvestment.com`) |
| JSON | nlohmann/json (FetchContent 자동 다운로드) |
| IPC | ZeroMQ (PUB-SUB + REQ-REP) + cppzmq header-only |
| 락-프리 큐 | 자체 구현 SPSC `RingBuffer`·MPSC `MpscQueue`·수신 N×샤드 M `shard::Matrix` (`std::atomic`, cache-line 분리) |
| Python | 3.11, requests, pyzmq, psycopg2-binary |
| 데이터베이스 | TimescaleDB (PostgreSQL 16 확장) |
| 컨테이너 | Docker + Docker Compose |
| OS | Windows 11 (개발), Ubuntu 24.04 (Docker 런타임) |

---

## 2. 프로젝트 구조

### 전체 디렉토리

파일 단위 목록은 손으로 유지하지 않는다 — 정본은 [../FILE_INDEX.md](../FILE_INDEX.md)(파일마다 한 줄 설명). 여기서는 디렉터리의 역할만 둔다.

| 경로 | 역할 |
|---|---|
| `Quant/include/core` · `Quant/src/core` | 엔진 본체 — `Engine.h`(스레드·`pipeline_`), `AppConfig`(config.json을 읽는 유일한 곳), `Types.h`, 큐(`RingBuffer`·`MpscQueue`·`ShardMatrix`), `SignalDispatcher`·`OrderPacer`·`LedgerReconciler`·`FeedSupervisor`·`BarAggregator` 같은 스레드별 지역 객체, `WakeGate`, 틱 캡처·리플레이 |
| `Quant/include/api` · `Quant/src/api` | KIS REST(`KisClient`, 구현은 도메인별 `Kis*.cpp`)·WebSocket(`KisWebSocket`, 소켓은 `WsSocketWin.cpp`/`WsSocketPosix.cpp` 파일 단위 분기)·순수 함수 디코더(`KisRestDecode.h`·`KisWsDecode.h`)·인터페이스(`IOrderExecutor`·`IMarketDataSource`) |
| `Quant/include/risk` · `Quant/src/risk` | `OrderGate`(주문 검증·확정 포지션 원장·kill switch·entry_halt)와 거부 사유 문장 계약 `GateReasons.h`(D-067) |
| `Quant/include/strategy` · `Quant/src/strategy` | `StrategyBase`와 전략 구현, 타입별 로더 `StrategyFactory.cpp`. 가상 함수는 `on_data`·`on_order_book`·`on_order_book_batch`·`on_trade`·`on_trade_batch`·`on_start`·`on_stop`·`get_watch_specifications`·`wants_daily_bars`·`id`·`describe` |
| `Quant/include/ipc` · `Quant/src/ipc` | `OrderRouter`(FEP 층 — 라우팅·이력·통계), 운영단말 TCP `OpsServer`·`OpsProtocol`, `ZmqBridge`(HAS_ZMQ일 때만) |
| `Quant/include/universe` · `Quant/src/universe` | 유니버스 스캔·점수(`UniverseScanner`·`ScoreWeight`·`MaAlign`) |
| `Quant/include/modes` · `Quant/src/modes` | FEED·KR_TEST·US_TEST 모니터 모드(`Monitors`) |
| `Quant/include/utils` · `Quant/src/utils` | 비동기 `Logger`, ETF 이름 판별 `EtfFilter`, json 접근 `JsonNode`, `Utf8` |
| `Quant/src/main.cpp` | 진입점 — 초기화 단계 호출과 FEED / KR_TEST / US_TEST / TRADE 분기 |
| `Quant/tests` | ctest 단위 테스트 `test_*.cpp`(아래 "단위 테스트")와 벤치 `bench_*.cpp` |
| `Quant/tools` | 수동 주문·운영단말 클라이언트 `ops_client`·MFC 운영단말 `ops_terminal`·시세 점검 실행파일과 파이썬 조회 스크립트 |
| `Quant/config` | `config.json`(gitignore — 실KIS 인증정보·계좌번호), 모의용 `config_*_paper.json`, ETF·리츠 이름 목록, 매크로 보조 프로세스가 쓰는 `regime.json`, 유니버스 스캔 결과 |
| `Quant/CMakeLists.txt` · `Quant/Dockerfile` | 빌드 정의(ZMQ 선택, FetchContent), C++ 2-stage 이미지 |
| `PYQuant/` | 파이썬 — `kis/`(REST 클라이언트), `strategy/`, `backtest/`, `live/`, `ipc/`(ZMQ 구독·명령), `db/`(TimescaleDB 스키마·적재), `tools/`(매크로 국면·지수 적재 보조 프로세스), `main.py` |
| `docker-compose.yml` | 4개 서비스(engine/python/recorder/tsdb) — 3절 |
| `docs/` | 설계·운영 문서. 이 파일 외에 [OPS_TERMINAL.md](OPS_TERMINAL.md)·[MFC_TERMINAL.md](MFC_TERMINAL.md)·[CPP20_23_GUIDE.md](CPP20_23_GUIDE.md), 결정 이력 [../DECISIONS.md](../DECISIONS.md) |
| `ARCHITECTURE.md` · `CODE_REVIEW.md` | 저장소 루트의 로컬전용 개인 문서(gitignore) — 저장소에 남는 요약은 `docs/ENGINE_ARCHITECTURE.md` |

부속 가이드: [코드 의존 그래프 가이드](CODE_GRAPH_GUIDE.md) — 모듈·파일 의존 그래프 생성, `--impact` 영향범위 질의, 증분빌드 팬아웃 최적화. 그래프 산출물은 [../CODE_GRAPH.md](../CODE_GRAPH.md).

### 실행 모드 (`config.json` → `"mode"` 또는 CLI 인자)

| 모드 | 동작 |
|------|------|
| `KR_TEST` | KOSPI 상위 20 + 관심종목 실시간 시세. WS 체결 수신 + ZMQ publish. 주문 없음. Docker 기본값. |
| `FEED` | WebSocket 호가+체결 5단계 콘솔 표시. 연결·인증 검증용. |
| `US_TEST` | M7(AAPL·MSFT·NVDA 등) REST 시세 반복 조회. 장 외 시간에도 동작. |
| `TRADE` | Engine 실행(1절 스레드 모델). 전략 신호 → OrderGate → KIS 실주문. |

### 단위 테스트

<!-- gen:test-targets -->
단위 테스트는 `Quant/tests/`에 있고 ctest에 등록돼 있습니다 — 실행 타깃 `34`개.

```bash
cmake --build out/build/x64-release --target test_order_gate test_order_router test_ws_frame test_ws_decode test_kis_decode test_ops_server test_ops_protocol test_market_session test_reconcile_plan test_ledger_reconciler test_data_poller test_signal_dispatcher test_bar_aggregator test_order_pacer test_regime_bridge test_ringbuffer test_ringbuffer_stress test_pipeline_stress test_wake_gate test_symbol_table test_tick_capture test_replay_source test_paper_executor test_feed_mux test_engine test_app_config test_feed_supervisor test_shard_matrix test_strategy_shard test_strategy_router test_latency_trace test_mpsc test_account_ledger test_logger
```
<!-- /gen -->
테스트 이름은 각각 원장·게이트·라우터·큐·WS 디코더·REST 분봉 디코더·정규장 시각·잔고 대조 계산·잔고 대조기·REST 현재가 폴러·신호 디스패처·발주 조절기·운영단말 프로토콜/서버·비동기 로거·매크로 국면 파일 판정기·N분봉 집계기·소비자 깨우기 조각·구간 지연 CSV·종목 id 테이블·틱 캡처·캡처 리플레이 소스·모의 체결기·피드 소스 mux·수신 N×샤드 M 링 행렬·전략 샤드·종목 id 전략 라우터·WS 피드 감독기·시험용 시세로 도는 Engine 한 바퀴(레인 1×샤드 1, 2×2, 캡처 리플레이)를 가리킨다.

```bash
ctest --preset x64-release          # 저장소 루트에서. 스트레스 2종은 3초로 줄여 돈다
ctest --test-dir Quant/build_win    # 수동 Ninja 레이아웃일 때
```
Linux에서는 `-DQUANT_TSAN=ON`으로 Debug를 ThreadSanitizer로 만들 수 있다(ASAN과 배타).

---

## 3. Docker — 설치 및 운영 명령어

### 설치 (Windows)

1. [Docker Desktop](https://www.docker.com/products/docker-desktop/) 다운로드·설치
2. 설치 후 Docker Desktop 앱 실행 (시스템 트레이에 고래 아이콘 확인)
3. 설정 → General → **WSL 2 based engine** 체크 확인

```bash
docker --version
docker compose version
```

### Docker 서비스 구성

```
docker-compose.yml 4개 서비스:

┌──────────────────────────────────────────────────────────────────┐
│  quant-engine    C++ 엔진  (포트 5555 PUB, 5556 REP 노출)         │
│  quant-python    모니터    (ZMQ SUB → stdout)                      │
│  quant-recorder  DB 적재기 (ZMQ SUB → TimescaleDB, healthcheck 대기)│
│  quant-tsdb      TimescaleDB (포트 5432, healthcheck: pg_isready)  │
└──────────────────────────────────────────────────────────────────┘
         모두 quant-net (bridge) 네트워크로 연결

기동 순서:
  quant-tsdb (healthcheck 통과)
      └── quant-recorder (DB ready 대기 + 엔진 started 대기)
  quant-engine (restart: unless-stopped)
      └── quant-python (엔진 started 대기)
```

### 매일 쓰는 명령어

```bash
# ── WSL에서 실행 ──────────────────────────────────────────────────

# 전체 스택 기동 (KR_TEST 모드)
docker compose up -d

# C++ 엔진만 먼저 기동 (DB/Python 없이 검증)
docker compose up -d quant-engine
docker compose logs -f quant-engine

# DB 기동 확인 후 recorder 연결
docker compose up -d timescaledb
docker compose up -d quant-recorder
docker compose logs -f quant-recorder

# ── 로그 확인 ────────────────────────────────────────────────────
docker compose logs -f quant-engine
docker compose logs -f quant-recorder
docker compose ps   # 컨테이너 상태

# ── Python CLI 일회성 실행 ───────────────────────────────────────
# 백테스트
docker compose run --rm quant-python backtest --from 2023-01-01 --to 2024-12-31

# 엔진 상태 조회 (C++ 엔진 실행 중이어야 함)
docker compose run --rm quant-python operate status

# 엔진 종료 명령
docker compose run --rm quant-python operate kill

# ── DB 조회 ──────────────────────────────────────────────────────
docker compose exec timescaledb psql -U quant -d quant
  # psql 접속 후:
  SELECT COUNT(*) FROM ticks;
  SELECT * FROM ticks ORDER BY ts DESC LIMIT 10;
  SELECT * FROM health ORDER BY ts DESC LIMIT 5;
  \q

# ── 이미지 재빌드 (소스 변경 시) ─────────────────────────────────
docker compose build quant-engine
docker compose build quant-python   # recorder도 같은 이미지 사용

# ── 종료 ─────────────────────────────────────────────────────────
docker compose down

# TRADE 모드로 전환 (docker-compose.yml command 변경 또는)
docker compose run --rm quant-engine ./quant_trader config/config.json TRADE
```

### 자주 쓰는 추가 명령어

```bash
# 컨테이너 내부 셸 접속
docker compose exec quant-engine bash
docker compose exec quant-python bash

# 컨테이너 재시작
docker compose restart quant-engine

# 이미지/캐시 정리
docker image prune -f              # dangling 이미지
docker builder prune -f            # 빌드 캐시 (~1.7GB)
docker system prune -f             # 이미지+캐시+네트워크
docker system prune -f --volumes   # ※ TimescaleDB 데이터도 삭제

# TimescaleDB 볼륨 위치 확인
docker volume inspect quant_tsdb-data
```

### 환경변수

```bash
# .env 파일 생성 (TSDB 비밀번호 설정)
echo "TSDB_PASSWORD=mysecretpassword" > .env

# 또는 실행 시 인라인 지정
TSDB_PASSWORD=mysecret docker compose up -d
```

---

## 4. TimescaleDB — 설치 구조와 PostgreSQL과의 관계

### TimescaleDB란?

**PostgreSQL의 확장(Extension)** — PostgreSQL을 교체하는 것이 아니라 그 위에 설치되는 플러그인입니다.

```
┌─────────────────────────────────────────┐
│         timescale/timescaledb 이미지     │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │       PostgreSQL 16 (베이스)    │    │
│  │  - 표준 SQL 완전 지원           │    │
│  │  - psql / psycopg2 그대로 사용  │    │
│  └─────────────────────────────────┘    │
│  ┌─────────────────────────────────┐    │
│  │  TimescaleDB Extension          │    │
│  │  - create_hypertable()          │    │
│  │  - 시간 기반 자동 파티셔닝      │    │
│  │  - continuous aggregate         │    │
│  │  - 자동 압축·보존 정책          │    │
│  └─────────────────────────────────┘    │
└─────────────────────────────────────────┘
```

### 일반 PostgreSQL 대비 장점 (시계열 데이터)

| 항목 | PostgreSQL | TimescaleDB |
|------|-----------|-------------|
| 대용량 시계열 INSERT | 느림 (인덱스 재구성) | 빠름 (시간별 청크 분리) |
| 시간 범위 조회 | 전체 스캔 위험 | 해당 청크만 스캔 |
| 자동 데이터 보존 | 수동 DELETE | 보존 정책 자동 적용 |
| 연속 집계 | 뷰 매번 재계산 | 증분 갱신 (Continuous Aggregate) |
| 기존 SQL 호환 | ✅ | ✅ (동일) |

### 이 프로젝트의 스키마 (`PYQuant/db/schema.sql`)

```sql
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- 실시간 체결 (ZMQ TRADE 이벤트)
CREATE TABLE ticks (
    ts TIMESTAMPTZ NOT NULL, ticker TEXT NOT NULL,
    price NUMERIC(18,4), volume BIGINT, direction SMALLINT, market TEXT DEFAULT 'KR'
);
SELECT create_hypertable('ticks', 'ts');   -- 시계열 파티션 활성화

-- 전략 시그널 (ZMQ SIGNAL)
CREATE TABLE signals ( ts, strategy, ticker, side, qty, price, market );
SELECT create_hypertable('signals', 'ts');

-- 주문 결과 (ZMQ ORDER)
CREATE TABLE orders ( ts, ticker, side, qty, price, ok BOOLEAN, market );
SELECT create_hypertable('orders', 'ts');

-- 엔진 상태 (ZMQ HEALTH)
CREATE TABLE health ( ts, data_cnt, signal_cnt, order_cnt );
SELECT create_hypertable('health', 'ts');

-- KIS REST 일봉 (bars_1d) — ON CONFLICT DO NOTHING (중복 방지)
CREATE TABLE bars_1d ( ts, ticker, open, high, low, close, volume, market );
SELECT create_hypertable('bars_1d', 'ts');
CREATE UNIQUE INDEX bars_1d_ticker_ts ON bars_1d (ticker, ts DESC);
```

스키마는 Docker 최초 기동 시 `/docker-entrypoint-initdb.d/01_schema.sql`로 마운트되어 자동 적용됩니다.

### 접속 방법

```python
# Python (psycopg2) — 일반 PostgreSQL과 완전 동일
conn = psycopg2.connect(
    host="timescaledb", port=5432,
    dbname="quant", user="quant", password="..."
)
```

```bash
# psql CLI
docker compose exec timescaledb psql -U quant -d quant
```

---

## 5. 플랫폼별 실행 방법 (Windows / Linux)

### Windows (개발 환경)

**요구사항:** Docker Desktop (WSL2 백엔드)

```
CMD 열기
  └─ wsl                              WSL(Ubuntu) 진입
       └─ cd /mnt/c/.../Quant
            ├─ docker compose build   이미지 빌드
            ├─ docker compose up -d   4개 컨테이너 시작
            └─ docker compose logs -f quant-engine
```

**Windows 로컬 빌드 (Docker 없이):**
```powershell
# "Developer PowerShell for VS 2022" 에서 실행
# (한글 경로 문제로 vcvars64.bat 방식 필요)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B Quant/build_win -S Quant
cmake --build Quant/build_win

# ZMQ 사용 시 먼저:
# vcpkg install zeromq:x64-windows

# 실행
.\Quant\build_win\quant_trader.exe Quant\config\config.json KR_TEST
```

### Linux (서버 / VPS 배포)

**요구사항:** Docker Engine + Docker Compose Plugin

```bash
# Docker 설치 (Ubuntu 24.04)
sudo apt update && sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | \
  sudo tee /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) \
  signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list
sudo apt update && sudo apt install -y docker-ce docker-compose-plugin
sudo usermod -aG docker $USER  # 재로그인 필요

# 프로젝트 실행
git clone <repo-url>
cd Quant
echo "TSDB_PASSWORD=changeme" > .env
docker compose build
docker compose up -d
```

**Linux 로컬 빌드 (Docker 없이):**
```bash
sudo apt install -y cmake ninja-build g++-14 libcurl4-openssl-dev libzmq3-dev
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -B Quant/build -S Quant
cmake --build Quant/build
./Quant/build/quant_trader Quant/config/config.json KR_TEST
```

### Windows vs Linux 코드 분기

| 항목 | Windows | Linux |
|------|---------|-------|
| HTTP 클라이언트 | WinHTTP (SDK 내장) | libcurl |
| UTF-8 콘솔 | `SetConsoleOutputCP(CP_UTF8)` | 기본 UTF-8 |
| 시간 함수 | `localtime_s()` | `localtime_r()` |
| 컴파일 플래그 | `/utf-8 /D_WIN32_WINNT=0x0A00` | `-Wall -Wextra -fsanitize=address(debug)` |
| ZeroMQ | vcpkg install zeromq | `apt install libzmq3-dev` |
| ZMQ 없을 때 | HAS_ZMQ 미정의 → ZmqBridge 전체 비활성 | 동일 |

---

## 6. FEP 레이어 — OrderRouter + OrderGate

### 증권사 FEP vs 이 프로젝트

| 항목 | 증권사 FEP | 이 프로젝트 (OrderRouter) |
|------|-----------|--------------------------|
| 접속 방식 | KRX 전용선 직접 연결 | KIS REST API 경유 |
| 주문 지연 | 수십 마이크로초 | 수십~수백 밀리초 |
| 프로토콜 | KRX 전용 바이너리 | HTTP/JSON |
| 주문 검증 | ✅ | ✅ (OrderGate) |
| 상태 추적 | ✅ | ✅ (ManagedOrder) |
| Rate Limit | ✅ | ✅ (초당/분당 이중 검사) |
| 접수번호 관리 | ✅ | ✅ (KIS ODNO 저장) |
| 이력 보관 | ✅ | ✅ (최근 500건 메모리) |
| 테스트 격리 | — | ✅ (IOrderExecutor Stub) |

### 주문 흐름 (OrderRouter::submit 내부)

```
OrderSignal (전략 → SignalDispatcher → pipeline_.order_queue → 주문 스레드)
     │
     ▼
 [1] OrderGate::check()  — 검사 순서의 정본은 이 함수 하나
     ├─ kill switch · OrderSide::NONE · entry_halt(신규만)
     ├─ 매매 세션 창 밖(NEW만, D-096·D-097)
     ├─ 1주문 수량·명목 상한(fat-finger, 시장가는 reference_price)
     ├─ 종목당 보유 수량·명목 상한, 동시 보유 종목 상한·교체 진입(D-018·D-019)
     ├─ 총노출 상한 · 일일 손실 한도 · PnL 갱신 정체(신규만)
     ├─ 중복 신호(dedup_window_sec)
     └─ 초당·분당 주문 수(기본 5건/초·20건/분, config risk.max_orders_per_sec/min)
     │
     ▼ PASS
 [2] IOrderExecutor::submit_order()
     ├─ 성공 → ODNO 수신              → ACCEPTED + gate_.on_accept() 포지션 선점
     └─ 실패 → 빈 문자열 반환         → REJECTED
     │
     ▼
 [3] ZMQ publish_order(ok=true/false)  → quant-recorder → TimescaleDB (HAS_ZMQ일 때)
 [4] ManagedOrder → history_ deque 저장 (최대 500건)
```

### OrderGate 뮤텍스 구조

```
뮤텍스 6개(`Quant/include/risk/OrderGate.h`):
  positions_mutex_     — positions_ · reserved_ · avg_prices_ · 전략별 서브원장
  pnl_mutex_           — daily_pnl_
  rate_mutex_          — order_times_min_ / order_times_sec_
  deduplicate_mutex_   — last_signal_
  displace_mutex_      — 교체 진입 후보·쿨다운
  priority_mutex_      — 점수 우선순위 랭크

[lock-order] check()는 positions_mutex_ 안에서 displace_mutex_·priority_mutex_를 잡는다(보유 스냅샷과 같은 시점).
  반대 순서는 없고 pnl·rate·dedup은 독립 스코프에서만 잡는다.
```

### 핵심 타입 (`Types.h`)

```cpp
enum class OrderStatus { PENDING, SUBMITTED, ACCEPTED, REJECTED, FILLED, CANCELLED };

struct ManagedOrder {
    std::string   order_id;       // 내부 순번  "ORD-000001"
    std::string   kis_order_no;   // KIS 접수번호  ODNO
    OrderSignal   signal;
    OrderStatus   status{PENDING};
    std::string   reject_reason;
    time_point    submitted_at;
    time_point    updated_at;
};
```

### OrderGate 검증 항목 + 테스트 현황

`OrderGate::check()`(`Quant/src/risk/OrderGate.cpp`)가 순서대로 검사한다. 검사 목록과 순서의 정본은 그 함수 하나고, 거부 지점 개수는 `docs/facts.json`의 `ordergate_rejects`(생성값)로 센다. 아래 표는 설정 키와 기본값 안내다(`Quant/include/risk/OrderGate.h`의 `Config`).

| 검증 항목 | 설정 키 | 기본값 | 테스트 |
|-----------|---------|--------|--------|
| Kill switch (전방향 하드스톱) | `set_kill_switch(true)` | false | ✅ |
| NONE side | — | — | (OrderRouter에서 검증) |
| Entry halt (신규매수만 차단, 청산 통과) | `set_entry_halt(true)` | false | — |
| 매매 세션 창 (NEW만. 정규장 + 애프터마켓, 모의는 정규장만) | `session_open_hhmm`·`session_close_hhmm`·`after_open_hhmm`·`after_close_hhmm`·`after_market` | 0900·1530·1600·2000 | — |
| 1주문 수량 상한 (fat-finger) | `max_qty_per_order` | 10,000주 | — |
| 1주문 명목 상한 (fat-finger, 시장가는 reference_price) | `max_notional_per_order` | 5,000만원 | — |
| 종목당 최대 보유 (positions_+reserved_) | `max_qty_per_ticker` | 100주 | ✅ |
| 종목당 명목 상한 | `max_notional_per_ticker` | — | — |
| 동시 보유 종목 상한 (신규 진입만) | `max_concurrent_positions` | 0(미적용) | — |
| 총노출 상한 (보유+예약 명목 합 / 자본) | `max_gross_exposure_pct` | 0(미적용) | — |
| 일일 최대 손실 | `daily_loss_limit` | -30만원 | ✅ |
| PnL stale 가드 (신규매수만, control 스레드 감시) | — | — | — |
| 초당 주문 수 | `max_orders_per_sec` | 5건 | ✅ |
| 분당 주문 수 | `max_orders_per_min` | 20건 | (초당으로 커버) |
| 중복 신호 (키에 side 포함, MM 양방 발주) | `dedup_window_sec` | 1.0초 | ✅ |
| 정상 통과 | — | — | ✅ |
| SELL 포지션 무관 | — | — | ✅ |

**테스트 실행:**
```bash
# Docker 빌더 스테이지에서 실행
docker build --target builder -f Quant/Dockerfile -t quant-builder-test .
docker run --rm quant-builder-test sh -c \
  'cmake --build build --target test_order_gate && ./build/test_order_gate'
docker run --rm quant-builder-test sh -c \
  'cmake --build build --target test_order_router && ./build/test_order_router'

# Windows 로컬
cmake --build Quant/build_win --target test_order_gate
.\Quant\build_win\test_order_gate.exe

cmake --build Quant/build_win --target test_order_router
.\Quant\build_win\test_order_router.exe
```

### ZmqBridge IPC 프로토콜

```
PUB tcp://*:5555  멀티파트: [topic bytes][JSON payload bytes]

topic    payload 예시
TRADE    {"ts":1716220800000,"ticker":"005930","price":65000,"volume":1234,"direction":1,"market":"KR"}
SIGNAL   {"ts":...,"strategy":"VALUE_CONTRARY","ticker":"005930","side":"BUY","qty":1,"price":0,"market":"KR"}
ORDER    {"ts":...,"ticker":"005930","side":"BUY","qty":1,"price":0,"ok":true,"market":"KR"}
HEALTH   {"ts":...,"data":123,"signal":5,"order":3}

REP tcp://*:5556  요청/응답
  KILL   → "OK"
  STATUS → {"running":true,"data":123,"signal":5,"order":3}
```

### 관련 파일

| 파일 | 역할 |
|------|------|
| [Quant/include/core/Types.h](../../Quant/include/core/Types.h) | `OrderStatus`, `ManagedOrder`, `WatchSpec` |
| [Quant/include/api/IOrderExecutor.h](../../Quant/include/api/IOrderExecutor.h) | 주문 실행 추상 인터페이스 |
| [Quant/include/ipc/OrderRouter.h](../../Quant/include/ipc/OrderRouter.h) | FEP 라우터 인터페이스 |
| [Quant/src/ipc/OrderRouter.cpp](../../Quant/src/ipc/OrderRouter.cpp) | submit / record / stats |
| [Quant/include/risk/OrderGate.h](../../Quant/include/risk/OrderGate.h) | 주문 검증 게이트·확정 포지션 원장 |
| [Quant/src/risk/OrderGate.cpp](../../Quant/src/risk/OrderGate.cpp) | 검증 로직 (`check()`가 검사 순서의 정본) |
| [Quant/include/risk/GateReasons.h](../../Quant/include/risk/GateReasons.h) | 유량 한도 거부 문장 계약 (D-067) |
| [Quant/include/ipc/ZmqBridge.h](../../Quant/include/ipc/ZmqBridge.h) | ZMQ 브리지 (HAS_ZMQ) |
| [Quant/src/ipc/ZmqBridge.cpp](../../Quant/src/ipc/ZmqBridge.cpp) | 전용 스레드 + 송신 큐 |
| [Quant/tests/test_order_gate.cpp](../../Quant/tests/test_order_gate.cpp) | 단위 테스트 |
| [Quant/tests/test_order_router.cpp](../../Quant/tests/test_order_router.cpp) | 6개 통합 테스트 |

---

## 7. 실제 매매 연결 로드맵

### 현재 상태

이 절에 날짜 스냅샷을 두지 않는다. 엔진이 지금 무엇을 싣고 있는지는 [../ENGINE_ARCHITECTURE.md](../ENGINE_ARCHITECTURE.md),
왜 그렇게 됐는지와 버린 대안은 [../DECISIONS.md](../DECISIONS.md)를 본다.

### 단계별 작업

#### Step 1. C++ TRADE 모드 검증 (모의투자)

`Quant/config/config.json`:
```json
{
  "kis": { "is_paper": true },
  "mode": "TRADE",
  "strategies": [
    { "type": "VALUE_CONTRARY", "market": "KR", "pbr_max": 1.0, "quantity": 1 }
  ]
}
```

```bash
docker compose run --rm quant-engine ./quant_trader config/config.json TRADE
```

#### Step 2. 백테스팅 품질 개선 (우선순위 2 — 내일)

- CostModel 추가 (수수료 0.015%, 거래세 0.18%, 슬리피지 5bp)
- 최대낙폭(MDD): equity 시계열 기반으로 수정 (현재 현금 흐름 기준으로 오류)
- 샤프(위험조정수익): 일별 수익률 기반으로 수정

#### Step 3. DB 캐시 연동

`get_historical_ohlcv()`를 DB 캐시와 병행:
- DB에 있으면 DB에서 읽음 (빠름, KIS API 호출 감소)
- 없으면 KIS REST → DB 저장 후 반환

#### Step 4. Python LiveTrader → C++ Engine 연동

현재 `live/trader.py`는 Python에서 직접 REST를 폴링. 장기적으로 ZMQ를 통해 C++ Engine에 주문 위임:

```
Python LiveTrader → ZmqOperator → C++ Engine → KIS send_order()
```

#### Step 5. 실거래 전환 체크리스트

- [ ] 모의투자(`is_paper: true`)로 2주 이상 무중단 실행
- [ ] 백테스트 결과 vs 페이퍼 결과 괴리 분석
- [ ] `is_paper: false`로 변경 + Docker volume mount만으로 설정 반영
- [ ] KIS 실거래 엔드포인트 (`openapi.koreainvestment.com:9443`) 연결 확인
- [ ] 일일 최대 손실 한도(`daily_loss_limit`) 실거래 기준으로 조정

---

## 8. 디스크 용량 관리

### 현재 용량 현황

수치 스냅샷은 두지 않는다 — `docker system df`와 `du -sh out Quant/build_win`으로 그때그때 본다.
빌드 산출물 위치는 [../ENGINE_ARCHITECTURE.md](../ENGINE_ARCHITECTURE.md), 결정 이력은 [../DECISIONS.md](../DECISIONS.md).

### 용량 확보

```bash
docker builder prune -f            # 빌드 캐시 (~1.4 GB 회수, 다음 빌드 느림)
docker image prune -f              # dangling 이미지
docker system prune -f             # 이미지+캐시+네트워크

# ※ 아래는 TimescaleDB 데이터도 삭제됨
docker system prune -f --volumes
```

### TimescaleDB 데이터 증가 추정

| 기간 | 예상 ticks 행 수 | 예상 크기 |
|------|-----------------|----------|
| 1 거래일 (6.5시간 × 28종목) | ~100만 건 | ~50 MB |
| 1개월 (20 거래일) | ~2,000만 건 | ~1 GB |
| 1년 | ~2.4억 건 | ~12 GB |

TimescaleDB 자동 압축으로 실제 크기는 위의 20~40% 수준.

```sql
-- 3개월 이상 된 ticks 자동 삭제
SELECT add_retention_policy('ticks', INTERVAL '3 months');
```
