# Quant Trading System — 프로젝트 가이드

> 최종 업데이트: 2026-05-20 (4-스레드 파이프라인 + ZMQ IPC + FEP 레이어 완성)

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
┌─────────────────────────────────────────────────────────────────────────┐
│                        KIS OpenAPI (한국투자증권)                         │
│   REST API (OHLCV · 종목정보 · 주문)      WebSocket (실시간 체결 · 호가)  │
└──────────┬──────────────────────────────────────┬────────────────────────┘
           │                                      │
           ▼                                      ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     C++ Engine  (quant-engine 컨테이너)                   │
│                                                                          │
│  ┌─────────────┐  market_queue_  ┌──────────────┐  order_queue_         │
│  │ Data Thread │ ──RingBuffer──▶ │Strategy Thread│ ──RingBuffer──▶      │
│  │ REST 폴링   │                 │ on_data()     │                       │
│  │ (fetch_     │  ob_queue_      │ on_order_book │  ┌──────────────────┐ │
│  │  interval_  │ ──RingBuffer──▶ │ on_trade()    │  │  Order Thread    │ │
│  │  sec 주기)  │  td_queue_      │ → OrderSignal │  │  OrderRouter     │ │
│  └─────────────┘ ──RingBuffer──▶ └──────────────┘  │  .submit()       │ │
│                                                     │  → OrderGate     │ │
│  KisWebSocket                                       │  → KisClient     │ │
│  H0STASP0 → ob_queue_                              └──────┬───────────┘ │
│  H0STCNT0 → td_queue_ + ZMQ publish_trade                 │             │
│  HDFSCNT0 → td_queue_ + ZMQ publish_trade                 │             │
│                                                            ▼             │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  ZmqBridge (전용 zmq_thread_)                                    │   │
│  │  PUB tcp://*:5555  TRADE / SIGNAL / ORDER / HEALTH               │   │
│  │  REP tcp://*:5556  KILL / STATUS (Python Operator 수신)          │   │
│  └──────────────────────┬───────────────────────────────────────────┘   │
│                          │                                               │
│  ┌──────────────────┐    │ (장 중 5초 주기)                               │
│  │ Control Thread   │    │                                               │
│  │ WS stale 감지    │    │                                               │
│  │ → kill switch    │    │                                               │
│  └──────────────────┘    │                                               │
└─────────────────────────┼────────────────────────────────────────────────┘
                           │ ZMQ TCP (Docker 내부 quant-net)
           ┌───────────────┼──────────────────┐
           ▼               ▼                  ▼
┌─────────────────┐ ┌─────────────────┐ ┌──────────────────┐
│  quant-recorder │ │  quant-python   │ │  Python Operator │
│  ZMQ SUB :5555  │ │  ZMQ SUB :5555  │ │  ZMQ REQ :5556   │
│  → TimescaleDB  │ │  콘솔 모니터    │ │  KILL / STATUS   │
└────────┬────────┘ └─────────────────┘ └──────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│              TimescaleDB  (quant-tsdb 컨테이너)          │
│  ticks · signals · orders · health · bars_1d            │
│  (모두 hypertable — 시간별 자동 파티셔닝)                 │
└─────────────────────────────────────────────────────────┘
```

### 스레드 모델 (C++ Engine 내부 — 4-스레드)

```
[Data Thread]
  KIS REST 일봉 폴링 (fetch_interval_sec, 기본 60초)
  장 중에만 동작 (KR: 09:00~15:30, US: 22:30~05:00 KST)
  장 시작 감지 → OrderGate.reset_daily()
       │
       ▼ RingBuffer<MarketData>[1024]

[WebSocket recv_thread_ — KisWebSocket 내부]
  H0STASP0 → ob_queue_[4096]
  H0STCNT0 / HDFSCNT0 → td_queue_[4096] + ZMQ publish_trade

[Strategy Thread]
  우선순위: ob_queue_ > td_queue_ > market_queue_ (고주파 → 저주파)
  아이들 시 100µs 슬립
  신호 발생 → ZMQ publish_signal → order_queue_ push
       │
       ▼ RingBuffer<OrderSignal>[256]

[Order Thread]
  OrderRouter::submit()
    → OrderGate::check() (6단계 검증)
    → IOrderExecutor::submit_order() (KisClient 또는 Stub)
    → ZMQ publish_order

[Control Thread]
  5초 주기: 장 중에서만 WS stale 감지 (30초 미수신 → kill switch)
  ZmqBridge 전용 zmq_thread_가 REP 소켓 처리 (KILL/STATUS 명령)
```

### 사용 기술 스택

| 영역 | 기술 |
|------|------|
| C++ 빌드 | CMake 3.16+, Ninja, GCC(Linux) / MSVC(Windows) |
| HTTP (C++) | WinHTTP (Windows) / libcurl (Linux) |
| WebSocket (C++) | KIS WebSocket (`ops.koreainvestment.com`) |
| JSON | nlohmann/json (FetchContent 자동 다운로드) |
| IPC | ZeroMQ (PUB-SUB + REQ-REP) + cppzmq header-only |
| 락-프리 큐 | 자체 구현 SPSC RingBuffer (`std::atomic`, cache-line 분리) |
| Python | 3.11, requests, pyzmq, psycopg2-binary |
| 데이터베이스 | TimescaleDB (PostgreSQL 16 확장) |
| 컨테이너 | Docker + Docker Compose |
| OS | Windows 11 (개발), Ubuntu 22.04 (Docker 런타임) |

---

## 2. 프로젝트 구조

### 전체 디렉토리

```
Quant/                              ← 저장소 루트
├── Quant/                          ← C++ 프로젝트
│   ├── include/
│   │   ├── api/
│   │   │   ├── IOrderExecutor.h    주문 실행 추상 인터페이스 (테스트 격리용)
│   │   │   ├── KisClient.h         REST API (인증·OHLCV·주문·지수)
│   │   │   └── KisWebSocket.h      실시간 체결·호가 WebSocket + stale 감지
│   │   ├── core/
│   │   │   ├── Engine.h            4-스레드 트레이딩 엔진
│   │   │   ├── RingBuffer.h        SPSC 락-프리 큐 (cache-line 분리)
│   │   │   └── Types.h             MarketData, OrderSignal, ManagedOrder 등
│   │   ├── ipc/
│   │   │   ├── OrderRouter.h       FEP 레이어 (주문 라우팅·이력·통계)
│   │   │   └── ZmqBridge.h         ZMQ PUB/REP 브리지 (HAS_ZMQ 시 활성)
│   │   ├── risk/
│   │   │   └── OrderGate.h         6단계 주문 검증 게이트 + Kill Switch
│   │   ├── strategy/
│   │   │   ├── StrategyBase.h      전략 인터페이스 (on_data/on_order_book/on_trade)
│   │   │   ├── MACrossStrategy.h   이동평균 교차 전략
│   │   │   ├── MomentumStrategy.h  모멘텀 전략
│   │   │   └── ValueContraryStrategy.h  저PBR 역추세 전략
│   │   └── utils/
│   │       ├── Logger.h            싱글톤 로거 (ms UTC 타임스탬프)
│   │       ├── Config.h            JSON 설정 파서
│   │       └── Timer.h             고분해능 타이머
│   ├── src/
│   │   ├── main.cpp                진입점 + FEED / KR_TEST / US_TEST / TRADE 모드
│   │   ├── api/
│   │   │   ├── KisClient.cpp       플랫폼별 HTTP (WinHTTP↔libcurl)
│   │   │   └── WebSocketClient.cpp WebSocket 연결·파싱 (국내/해외)
│   │   ├── core/
│   │   │   ├── Engine.cpp          4-스레드 라이프사이클
│   │   │   └── RingBuffer.cpp
│   │   ├── ipc/
│   │   │   ├── OrderRouter.cpp     submit / record / stats 구현
│   │   │   └── ZmqBridge.cpp       전용 zmq_thread_ + 송신 큐 (HAS_ZMQ)
│   │   ├── risk/
│   │   │   └── OrderGate.cpp       6단계 검증 + 뮤텍스 4개 독립 스코프
│   │   ├── strategy/
│   │   │   ├── StrategyBase.cpp
│   │   │   ├── MACrossStrategy.cpp
│   │   │   └── MomentumStrategy.cpp
│   │   └── utils/
│   │       ├── Logger.cpp
│   │       ├── Config.cpp
│   │       └── Timer.cpp
│   ├── config/
│   │   └── config.json             ← gitignore (실KIS 인증정보+계좌번호)
│   ├── tests/
│   │   ├── test_order_gate.cpp     OrderGate 단위 테스트 (7/7 PASS)
│   │   ├── test_order_router.cpp   OrderRouter 통합 테스트 (6/6 PASS, StubExecutor)
│   │   ├── test_ringbuffer.cpp     RingBuffer 기본 동작 검증
│   │   ├── test_ringbuffer_stress.cpp  SPSC 부하 테스트
│   │   └── test_pipeline_stress.cpp    E2E 파이프라인 부하 테스트
│   ├── CMakeLists.txt              빌드 정의 (ZMQ 선택적, FetchContent)
│   └── Dockerfile                  C++ 2-stage 빌드 (builder/runtime)
│
├── PYQuant/                        ← Python 프로젝트
│   ├── core/
│   │   ├── __init__.py
│   │   └── logger.py               setup_logger(name) → 구조화 로깅
│   ├── kis/
│   │   ├── __init__.py
│   │   └── client.py               KisClient (토큰 캐시·KisAuthError·예외 분리)
│   ├── strategy/
│   │   ├── __init__.py
│   │   ├── base.py                 StrategyBase (Python)
│   │   └── value_contrary.py       저PBR 역추세 전략 (3일 연속 하락 스크리닝)
│   ├── backtest/
│   │   ├── __init__.py
│   │   ├── engine.py               날짜별 시뮬레이션 (look-ahead bias 방지)
│   │   └── report.py               수익률·MDD·Sharpe·승률 출력
│   ├── live/
│   │   ├── __init__.py
│   │   └── trader.py               REST 폴링 기반 실시간 트레이더
│   ├── ipc/
│   │   ├── __init__.py
│   │   ├── subscriber.py           ZmqSubscriber + EngineMonitor (콜백 기반)
│   │   └── operator.py             ZmqOperator (KILL·STATUS 명령 전송)
│   ├── db/
│   │   ├── __init__.py
│   │   ├── client.py               DbClient (psycopg2, 재시도, 5개 insert)
│   │   └── schema.sql              TimescaleDB hypertable DDL (자동 적용)
│   ├── main.py                     CLI 진입점 (5개 서브커맨드)
│   ├── requirements.txt
│   └── Dockerfile                  python:3.11-slim (ENTRYPOINT + CMD 분리)
│
├── docker-compose.yml              4개 서비스 (engine/python/recorder/tsdb)
├── ARCHITECTURE.md                 전체 아키텍처 상세 리뷰
├── CODE_REVIEW.md                  코드 리뷰 (버그·설계·개선 항목)
├── PROJECT_GUIDE.md                이 파일
└── CLAUDE.md                       AI 어시스턴트용 빌드·실행 가이드
```

### 실행 모드 (`config.json` → `"mode"` 또는 CLI 인자)

| 모드 | 동작 |
|------|------|
| `KR_TEST` | KOSPI 상위 20 + 관심종목 실시간 시세. WS 체결 수신 + ZMQ publish. 주문 없음. Docker 기본값. |
| `FEED` | WebSocket 호가+체결 5단계 콘솔 표시. 연결·인증 검증용. |
| `US_TEST` | M7(AAPL·MSFT·NVDA 등) REST 시세 반복 조회. 장 외 시간에도 동작. |
| `TRADE` | 4-스레드 Engine 실행. 전략 신호 → OrderGate → KIS 실주문. |

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
# Docker 설치 (Ubuntu 22.04)
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
sudo apt install -y cmake ninja-build g++ libcurl4-openssl-dev libzmq3-dev
cmake -DCMAKE_BUILD_TYPE=Release -B Quant/build -S Quant
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
OrderSignal (전략에서 생성)
     │
     ▼
 [1] OrderGate::check()
     ├─ Kill switch 활성?             → REJECTED
     ├─ OrderSide::NONE?              → REJECTED
     ├─ 포지션 한도 초과? (BUY만)    → REJECTED
     ├─ 일일 손실 한도 초과? (BUY만) → REJECTED
     ├─ 초당 주문 수 초과?            → REJECTED  (KIS 5건/초)
     ├─ 분당 주문 수 초과?            → REJECTED  (KIS 20건/분)
     └─ 중복 신호 (1초 내)?           → REJECTED
     │
     ▼ PASS
 [2] IOrderExecutor::submit_order()
     ├─ 성공 → ODNO 수신              → ACCEPTED + gate_.on_accept() 포지션 선점
     └─ 실패 → 빈 문자열 반환         → REJECTED
     │
     ▼
 [3] ZMQ publish_order(ok=true/false)  → quant-recorder → TimescaleDB
 [4] ManagedOrder → history_ deque 저장 (최대 500건)
```

### OrderGate 뮤텍스 구조

```
4개 뮤텍스, 각각 독립 스코프 (중첩 락 없음):
  positions_mtx_  — positions_ map
  pnl_mtx_        — daily_pnl_
  rate_mtx_       — deqOrder_times / deqOrder_times_sec
  dedup_mtx_      — mapLast_signal

중첩 필요 시 반드시 선언 순서대로:
  positions → pnl → rate → dedup
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

| 검증 항목 | 설정 키 | 기본값 | 테스트 |
|-----------|---------|--------|--------|
| Kill switch | `set_kill_switch(true)` | false | ✅ |
| NONE side | — | — | (OrderRouter에서 검증) |
| 종목당 최대 보유 | `max_qty_per_ticker` | 100주 | ✅ |
| 일일 최대 손실 | `daily_loss_limit` | -30만원 | ✅ |
| 초당 주문 수 | `max_orders_per_sec` | 5건 | ✅ |
| 분당 주문 수 | `max_orders_per_min` | 20건 | (초당으로 커버) |
| 중복 신호 | `dedup_window_sec` | 1.0초 | ✅ |
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
| [Quant/include/core/Types.h](Quant/include/core/Types.h) | `OrderStatus`, `ManagedOrder`, `WatchSpec` |
| [Quant/include/api/IOrderExecutor.h](Quant/include/api/IOrderExecutor.h) | 주문 실행 추상 인터페이스 |
| [Quant/include/ipc/OrderRouter.h](Quant/include/ipc/OrderRouter.h) | FEP 라우터 인터페이스 |
| [Quant/src/ipc/OrderRouter.cpp](Quant/src/ipc/OrderRouter.cpp) | submit / record / stats |
| [Quant/include/risk/OrderGate.h](Quant/include/risk/OrderGate.h) | 6단계 검증 게이트 |
| [Quant/src/risk/OrderGate.cpp](Quant/src/risk/OrderGate.cpp) | 검증 로직 |
| [Quant/include/ipc/ZmqBridge.h](Quant/include/ipc/ZmqBridge.h) | ZMQ 브리지 (HAS_ZMQ) |
| [Quant/src/ipc/ZmqBridge.cpp](Quant/src/ipc/ZmqBridge.cpp) | 전용 스레드 + 송신 큐 |
| [Quant/tests/test_order_gate.cpp](Quant/tests/test_order_gate.cpp) | 7개 단위 테스트 |
| [Quant/tests/test_order_router.cpp](Quant/tests/test_order_router.cpp) | 6개 통합 테스트 |

---

## 7. 실제 매매 연결 로드맵

### 현재 상태 (2026-05-20)

```
C++:
  KR_TEST ✅  — REST+WebSocket 시세 → ZMQ publish → TimescaleDB
  TRADE   ⚠️  — 코드 준비됨, 모의투자 검증 필요

Python:
  백테스팅 ✅  — KIS REST API (look-ahead bias 방지)
  모니터   ✅  — ZMQ SUB → 콘솔 출력
  DB 적재  ✅  — ZMQ SUB → TimescaleDB
  운영     ✅  — ZMQ REQ → C++ 엔진 (KILL/STATUS)
  LiveTrader ⚠️ — 독립 REST 폴링 방식 (C++ 엔진 미연동)
```

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
- MDD: equity 시계열 기반으로 수정 (현재 현금 흐름 기준으로 오류)
- Sharpe: 일별 수익률 기반으로 수정

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

### 현재 용량 현황 (2026-05-20 기준)

| 항목 | 크기 | 비고 |
|------|------|------|
| Docker 이미지 | ~3.7 GB | 비활성 이미지 포함 |
| Docker 빌드 캐시 | ~1.7 GB | builder prune으로 회수 가능 |
| TimescaleDB 볼륨 | 70 MB~ | 데이터 쌓일수록 증가 |
| `out/` (VS 빌드) | 96 MB | 로컬 Windows 빌드 산출물 |
| `Quant/build_win/` | 18 MB | Ninja 빌드 산출물 |

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
