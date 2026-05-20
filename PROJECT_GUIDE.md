# Quant Trading System — 프로젝트 가이드

> 최종 업데이트: 2026-05-20 (FEP 레이어 추가)

---

## 목차

1. [전체 아키텍처](#1-전체-아키텍처)
2. [프로젝트 구조](#2-프로젝트-구조)
3. [Docker — 설치 및 운영 명령어](#3-docker--설치-및-운영-명령어)
4. [TimescaleDB — 설치 구조와 PostgreSQL과의 관계](#4-timescaledb--설치-구조와-postgresql과의-관계)
5. [플랫폼별 실행 방법 (Windows / Linux)](#5-플랫폼별-실행-방법-windows--linux)
6. [FEP 레이어 — OrderRouter](#6-fep-레이어--orderrouter)
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
│  ┌─────────────┐   market_queue_   ┌──────────────┐   order_queue_       │
│  │ Data Thread │ ──RingBuffer──▶   │Strategy Thread│ ──RingBuffer──▶     │
│  │ REST 폴링   │                   │ 전략 순회     │                      │
│  │ WS 수신     │                   │ → OrderSignal │   ┌──────────────┐  │
│  └─────────────┘                   └──────┬────────┘   │ Order Thread │  │
│                                           │            │ send_order() │  │
│                                           ▼            └──────┬───────┘  │
│                                    ┌────────────┐             │          │
│                                    │ ZmqBridge  │◀────────────┘          │
│                                    │ PUB :5555  │  TRADE/SIGNAL/ORDER    │
│                                    │ REP :5556  │  HEALTH (1초마다)      │
│                                    └─────┬──────┘                        │
└──────────────────────────────────────────┼───────────────────────────────┘
                                           │ ZMQ TCP (Docker 내부 네트워크)
           ┌───────────────────────────────┼──────────────────┐
           ▼                               ▼                  ▼
┌─────────────────┐             ┌─────────────────┐  ┌──────────────────┐
│  quant-recorder │             │  quant-python   │  │  Python Operator │
│  ZMQ SUB :5555  │             │  ZMQ SUB :5555  │  │  ZMQ REQ :5556   │
│  → TimescaleDB  │             │  콘솔 모니터    │  │  KILL / STATUS   │
└────────┬────────┘             └─────────────────┘  └──────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────┐
│              TimescaleDB  (quant-tsdb 컨테이너)            │
│                                                          │
│   ticks      signals     orders     health    bars_1d    │
│  (체결)      (시그널)    (주문)     (상태)    (일봉)     │
│  hypertable  hypertable  hypertable hypertable hypertable │
└──────────────────────────────────────────────────────────┘
```

### 스레드 모델 (C++ Engine 내부)

```
[Data Thread]  →  RingBuffer<MarketData>  →  [Strategy Thread]  →  RingBuffer<OrderSignal>  →  [Order Thread]
  KIS REST                                    등록된 전략들                                       KIS 주문 API
  WebSocket                                   → OrderSignal 생성                                  send_order()
  60초 폴링                                   → ZMQ publish_signal                               → ZMQ publish_order

[Control Thread]  ←  ZMQ REP :5556  ←  Python Operator (KILL / STATUS 명령)
```

### 사용 기술 스택

| 영역 | 기술 |
|------|------|
| C++ 빌드 | CMake 3.16+, Ninja, GCC(Linux) / MSVC(Windows) |
| HTTP (C++) | WinHTTP (Windows) / libcurl (Linux) |
| WebSocket (C++) | KIS WebSocket (`ops.koreainvestment.com`) |
| JSON | nlohmann/json (FetchContent 자동 다운로드) |
| IPC | ZeroMQ (PUB-SUB + REQ-REP) + cppzmq header-only wrapper |
| 락-프리 큐 | 자체 구현 SPSC RingBuffer (`std::atomic`) |
| Python | 3.11, requests, pyzmq, psycopg2-binary |
| 데이터베이스 | TimescaleDB (PostgreSQL 16 확장) |
| 컨테이너 | Docker + Docker Compose |
| OS | Windows 11 (개발), Ubuntu 22.04 (Docker 런타임) |

---

## 2. 프로젝트 구조

### 전체 디렉토리

```
Quant/                          ← 저장소 루트
├── Quant/                      ← C++ 프로젝트
│   ├── include/
│   │   ├── api/
│   │   │   ├── KisClient.h         REST API (인증·OHLCV·주문)
│   │   │   └── KisWebSocket.h      실시간 체결·호가 WebSocket
│   │   ├── core/
│   │   │   ├── Engine.h            3-스레드 트레이딩 엔진
│   │   │   ├── RingBuffer.h        SPSC 락-프리 큐
│   │   │   └── Types.h             MarketData, OrderSignal, TradeData 등
│   │   ├── ipc/
│   │   │   └── ZmqBridge.h         ZMQ PUB/REP 브리지
│   │   ├── risk/
│   │   │   └── OrderGate.h         주문 수량·빈도 제한, Kill Switch
│   │   ├── strategy/
│   │   │   ├── StrategyBase.h      전략 추상 인터페이스
│   │   │   ├── MACrossStrategy.h   이동평균 교차 전략
│   │   │   ├── MomentumStrategy.h  모멘텀 전략
│   │   │   └── ValueContraryStrategy.h  저PBR 역추세 전략
│   │   └── utils/
│   │       ├── Logger.h            싱글톤 로거 (ms UTC 타임스탬프)
│   │       ├── Config.h            JSON 설정 파서
│   │       └── Timer.h             고분해능 타이머
│   ├── src/
│   │   ├── main.cpp                진입점 + KR_TEST / FEED / TRADE 모드
│   │   ├── api/
│   │   │   ├── KisClient.cpp       플랫폼별 HTTP (WinHTTP↔libcurl)
│   │   │   └── WebSocketClient.cpp WebSocket 연결·파싱
│   │   ├── core/
│   │   │   ├── Engine.cpp          스레드 시작·중지·파이프라인
│   │   │   └── RingBuffer.cpp
│   │   ├── ipc/
│   │   │   └── ZmqBridge.cpp       ZMQ 소켓 스레드
│   │   ├── risk/
│   │   │   └── OrderGate.cpp
│   │   ├── strategy/
│   │   │   ├── StrategyBase.cpp
│   │   │   ├── MACrossStrategy.cpp
│   │   │   └── MomentumStrategy.cpp
│   │   └── utils/
│   │       ├── Logger.cpp
│   │       ├── Config.cpp
│   │       └── Timer.cpp
│   ├── config/
│   │   ├── config.json             ← gitignore (실제 KIS 인증정보)
│   │   └── config.json.example     ← 템플릿 (커밋됨)
│   ├── tests/
│   │   ├── test_ringbuffer.cpp
│   │   ├── test_ringbuffer_stress.cpp
│   │   └── test_pipeline_stress.cpp
│   ├── CMakeLists.txt
│   └── Dockerfile                  ← C++ 빌드 + 런타임 (2-stage)
│
├── PYQuant/                    ← Python 프로젝트
│   ├── backtest/
│   │   ├── engine.py               날짜별 시뮬레이션 (look-ahead bias 방지)
│   │   └── report.py               수익률·MDD·Sharpe 출력
│   ├── db/
│   │   ├── client.py               DbClient (psycopg2, insert 메서드)
│   │   └── schema.sql              TimescaleDB hypertable DDL
│   ├── ipc/
│   │   ├── subscriber.py           ZmqSubscriber + EngineMonitor
│   │   └── operator.py             ZmqOperator (KILL·STATUS 명령)
│   ├── kis/
│   │   └── client.py               KIS REST API Python 래퍼
│   ├── live/
│   │   └── trader.py               실시간 REST 폴링 트레이더
│   ├── strategy/
│   │   ├── base.py                 StrategyBase 추상 클래스
│   │   └── value_contrary.py       저PBR 역추세 전략 (Python 버전)
│   ├── main.py                     CLI 진입점 (backtest·live·monitor·record·operate)
│   ├── requirements.txt
│   └── Dockerfile
│
├── scripts/                    ← Docker 자동화 쉘스크립트
│   ├── build.sh                이미지 빌드
│   ├── start.sh                전체 스택 시작 (kr_test / feed / trade)
│   ├── stop.sh                 컨테이너 중지
│   └── logs.sh                 실시간 로그 확인
│
├── docker-compose.yml          ← 4개 서비스 정의
├── .env.example                ← TSDB_PASSWORD 템플릿
├── .dockerignore
├── .gitignore
├── CMakeLists.txt              ← 루트 (Windows 프리셋용)
├── CMakePresets.json
└── CLAUDE.md                   ← AI 어시스턴트용 가이드
```

### 실행 모드 (`config.json` → `"mode"` 또는 CLI 인자)

| 모드 | 동작 |
|------|------|
| `KR_TEST` | KOSPI 상위 20 + 관심종목 실시간 시세 모니터링. WebSocket 체결 수신 후 ZMQ publish. 주문 없음. |
| `FEED` | WebSocket 체결·호가 원시 데이터 콘솔 출력. 연결·인증 검증용. |
| `TRADE` | 3-스레드 Engine 실행. 전략 신호 → 실제 KIS 주문. |

---

## 3. Docker — 설치 및 운영 명령어

### 설치 (Windows)

1. [Docker Desktop](https://www.docker.com/products/docker-desktop/) 다운로드·설치
2. 설치 후 Docker Desktop 앱 실행 (시스템 트레이에 고래 아이콘 확인)
3. 설정 → General → **WSL 2 based engine** 체크 확인

설치 확인:
```bash
# WSL 또는 CMD에서
docker --version
docker compose version
```

### Docker 구성 (이 프로젝트)

```
docker-compose.yml 정의 서비스:

┌──────────────────────────────────────────────────────────────────┐
│  quant-engine    C++ 트레이딩 엔진  (포트 5555, 5556 노출)        │
│  quant-python    Python 콘솔 모니터 (ZMQ SUB → stdout)            │
│  quant-recorder  Python DB 적재기  (ZMQ SUB → TimescaleDB)        │
│  timescaledb     시계열 DB          (포트 5432 노출)              │
└──────────────────────────────────────────────────────────────────┘
         모두 quant-net (bridge) 네트워크로 연결
```

### 매일 쓰는 명령어 순서

```bash
# 1. WSL 진입 (CMD에서)
wsl

# 2. 프로젝트 경로 이동
cd /mnt/c/Users/PYH/source/repos/Quant

# 3. 이미지 빌드 (최초 또는 소스 변경 시만)
./scripts/build.sh            # 캐시 활용 (빠름)
./scripts/build.sh --no-cache # 전체 재빌드

# 4. 전체 스택 시작
./scripts/start.sh            # KR_TEST 모드 (기본)
./scripts/start.sh trade      # TRADE 모드 (실주문, 확인 프롬프트)

# 5. 로그 확인
./scripts/logs.sh             # 전체
./scripts/logs.sh engine      # C++ 엔진만
docker compose logs -f quant-recorder  # DB 적재 현황

# 6. DB 조회
docker compose exec timescaledb psql -U quant -d quant
  # psql 접속 후:
  SELECT COUNT(*) FROM ticks;
  SELECT * FROM ticks ORDER BY ts DESC LIMIT 10;
  SELECT * FROM health ORDER BY ts DESC LIMIT 5;
  \q  # 종료

# 7. 컨테이너 상태 확인
docker compose ps

# 8. 종료
./scripts/stop.sh
exit   # WSL 종료
```

### 자주 쓰는 추가 명령어

```bash
# 특정 컨테이너만 재시작
docker compose restart quant-engine

# 컨테이너 내부 셸 접속
docker compose exec quant-engine bash
docker compose exec quant-python bash

# 이미지 목록
docker images

# 안 쓰는 이미지·캐시 정리 (용량 확보)
docker image prune -f              # dangling 이미지만
docker builder prune -f            # 빌드 캐시 전체 (약 1.7GB 회수)
docker system prune -f             # 이미지+캐시+미사용 네트워크
docker system prune -f --volumes   # 위 + 볼륨 (DB 데이터 삭제 주의!)

# TimescaleDB 볼륨 (DB 데이터) 위치
docker volume inspect quant_tsdb-data
```

---

## 4. TimescaleDB — 설치 구조와 PostgreSQL과의 관계

### TimescaleDB란?

**PostgreSQL의 확장(Extension)** 입니다. PostgreSQL을 교체하는 것이 아니라 그 위에 설치되는 플러그인입니다.

```
┌─────────────────────────────────────────┐
│         timescale/timescaledb 이미지     │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │       PostgreSQL 16 (베이스)    │    │
│  │  - 표준 SQL 완전 지원           │    │
│  │  - psql 클라이언트 그대로 사용  │    │
│  │  - psycopg2 드라이버 그대로 사용│    │
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

### 이 프로젝트에서 설치 방식

**Docker 이미지**로 설치 — 별도 설치 과정 없음:

```yaml
# docker-compose.yml
timescaledb:
  image: timescale/timescaledb:latest-pg16   # ← 이 이미지가 TimescaleDB 포함
  environment:
    POSTGRES_DB:       quant
    POSTGRES_USER:     quant
    POSTGRES_PASSWORD: ${TSDB_PASSWORD:-changeme}
  volumes:
    - tsdb-data:/var/lib/postgresql/data          # DB 데이터 영속화
    - ./PYQuant/db/schema.sql:/docker-entrypoint-initdb.d/01_schema.sql:ro
    #                          ↑ 컨테이너 최초 기동 시 이 SQL이 자동 실행됨
```

### 스키마 (`PYQuant/db/schema.sql`)

```sql
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- 실시간 체결 (ZMQ TRADE 이벤트)
CREATE TABLE ticks (ts TIMESTAMPTZ, ticker TEXT, price NUMERIC, volume BIGINT, direction SMALLINT, market TEXT);
SELECT create_hypertable('ticks', 'ts');   -- ← 이 한 줄로 시계열 파티션 활성화

-- 전략 시그널, 주문 결과, 엔진 상태, KIS 일봉 — 동일 패턴
```

### 접속 방법 (psycopg2 / psql 모두 동일)

```python
# Python (psycopg2)
conn = psycopg2.connect(host="timescaledb", port=5432, dbname="quant", user="quant", password="...")
# TimescaleDB라서 특별한 설정 필요 없음 — 일반 PostgreSQL과 동일
```

```bash
# psql CLI
docker compose exec timescaledb psql -U quant -d quant
```

---

## 5. 플랫폼별 실행 방법 (Windows / Linux)

### Windows (개발 환경)

**요구사항:** Docker Desktop (WSL2 백엔드), Git Bash 또는 WSL

```
CMD 열기
  └─ wsl                              WSL(Ubuntu) 진입
       └─ cd /mnt/c/.../Quant         프로젝트 경로
            ├─ ./scripts/build.sh     이미지 빌드 (Linux 컨테이너)
            ├─ docker compose up -d   4개 컨테이너 시작
            └─ ./scripts/logs.sh      로그 확인
```

C++ 로컬 빌드 (Docker 없이 Windows에서 직접):
```powershell
# Visual Studio 2022 + CMake 필요, vcpkg로 zeromq 설치
cmake --preset x64-release
cmake --build out/build/x64-release
.\out\build\x64-release\quant_trader.exe Quant\config\config.json KR_TEST
```

### Linux (서버 / VPS 배포)

**요구사항:** Docker Engine + Docker Compose Plugin

```bash
# Docker 설치 (Ubuntu)
sudo apt update
sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo tee /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# sudo 없이 docker 실행 (재로그인 필요)
sudo usermod -aG docker $USER

# 프로젝트 실행 (Windows와 동일)
git clone https://github.com/pypy8171/Quant.git
cd Quant
cp .env.example .env
./scripts/build.sh
./scripts/start.sh
```

C++ 로컬 빌드 (Docker 없이 Linux에서 직접):
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
| 컴파일 플래그 | `/utf-8 /D_WIN32_WINNT=0x0A00` | `-Wall -Wextra` |
| AddressSanitizer | 미지원 | 디버그 빌드에서 활성 |
| ZeroMQ | vcpkg 설치 | `apt install libzmq3-dev` |

---

## 6. FEP 레이어 — OrderRouter

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

### 주문 흐름 (OrderRouter 내부)

```
OrderSignal (전략에서 생성)
     │
     ▼
 [1] OrderGate::check()
     ├─ Kill switch 활성?         → REJECTED
     ├─ 포지션 한도 초과?         → REJECTED
     ├─ 일일 손실 한도 초과?      → REJECTED
     ├─ 초당 주문 수 초과?        → REJECTED  ← KIS 5건/초 안전 한도
     ├─ 분당 주문 수 초과?        → REJECTED  ← KIS 20건/분 권장
     └─ 중복 신호 (1초 내)?       → REJECTED
     │
     ▼ PASS
 [2] KisClient::submit_order()
     ├─ KIS API 전송
     ├─ 성공 → ODNO(접수번호) 수신   → ACCEPTED
     └─ 실패 → HTTP 오류 / rt_cd≠0  → REJECTED
     │
     ▼
 [3] ZMQ publish_order(ok=true/false)   → quant-recorder → TimescaleDB
 [4] ManagedOrder → 이력 deque에 저장
```

### 주요 타입

```cpp
// ManagedOrder — 주문 1건의 전체 생명주기
struct ManagedOrder {
    std::string   order_id;      // 내부 순번  "ORD-000001"
    std::string   kis_order_no;  // KIS 접수번호  ODNO
    OrderSignal   signal;        // 원본 주문 신호
    OrderStatus   status;        // PENDING → SUBMITTED → ACCEPTED/REJECTED
    std::string   reject_reason; // 거부 사유 (REJECTED 시)
    time_point    submitted_at;
    time_point    updated_at;
};

// OrderStatus
enum class OrderStatus { PENDING, SUBMITTED, ACCEPTED, REJECTED, FILLED, CANCELLED };
```

### OrderGate 검증 체계 (단위 테스트 완료)

| 검증 항목 | 설정 키 | 기본값 | 테스트 |
|-----------|---------|--------|--------|
| Kill switch | `set_kill_switch(true)` | false | ✅ `test_kill_switch` |
| 종목당 최대 보유 | `max_qty_per_ticker` | 100주 | ✅ `test_position_limit` |
| 일일 최대 손실 | `daily_loss_limit` | -30만원 | ✅ `test_daily_loss_limit` |
| 초당 주문 수 | `max_orders_per_sec` | 5건 | ✅ `test_rate_limit_per_sec` |
| 분당 주문 수 | `max_orders_per_min` | 20건 | (초당으로 커버) |
| 중복 신호 | `dedup_window_sec` | 1.0초 | ✅ `test_dedup` |
| 정상 통과 | — | — | ✅ `test_normal_pass` |
| SELL 포지션 무관 | — | — | ✅ `test_sell_bypasses_position_check` |

테스트 실행:
```bash
# Docker 빌더 스테이지에서 실행
docker build --target builder -f Quant/Dockerfile -t quant-builder-test .
docker run --rm quant-builder-test sh -c \
  'cmake --build build --target test_order_gate && ./build/test_order_gate'
```

### 관련 파일

| 파일 | 역할 |
|------|------|
| [Quant/include/core/Types.h](Quant/include/core/Types.h) | `OrderStatus`, `ManagedOrder` 타입 정의 |
| [Quant/include/ipc/OrderRouter.h](Quant/include/ipc/OrderRouter.h) | OrderRouter 인터페이스 |
| [Quant/src/ipc/OrderRouter.cpp](Quant/src/ipc/OrderRouter.cpp) | submit / record / stats 구현 |
| [Quant/include/risk/OrderGate.h](Quant/include/risk/OrderGate.h) | 검증 게이트 (초당/분당 이중 Rate Limit) |
| [Quant/src/risk/OrderGate.cpp](Quant/src/risk/OrderGate.cpp) | 검증 로직 구현 |
| [Quant/tests/test_order_gate.cpp](Quant/tests/test_order_gate.cpp) | 7개 단위 테스트 |

---

## 7. 실제 매매 연결 로드맵

### 현재 상태

```
[Python 백테스팅]  ← KIS REST API (과거 OHLCV)
[C++ KR_TEST]     ← KIS REST + WebSocket (실시간 시세) → ZMQ → TimescaleDB
                     주문 없음
```

### 목표 상태

```
[Python 백테스팅]  ← TimescaleDB (과거 데이터, KIS 호출 불필요)
[C++ TRADE]       ← KIS REST + WebSocket → 전략 → 실제 주문 → ZMQ → TimescaleDB
```

### 단계별 작업

#### Step 1. C++ TRADE 모드 검증 (모의투자)

`Quant/config/config.json`:
```json
{
  "is_paper": true,           ← 모의투자 엔드포인트로 전환
  "mode": "TRADE",
  "strategies": [
    { "type": "VALUE_CONTRARY", "market": "KR", "pbr_max": 1.0, "quantity": 1 }
  ]
}
```

`docker-compose.yml`에서 커맨드 변경:
```yaml
command: ["./quant_trader", "config/config.json", "TRADE"]
```

#### Step 2. 백테스팅 → DB 연동

`PYQuant/kis/client.py`의 `get_daily_ohlcv()`를 DB 캐시와 병행:
- DB에 있으면 DB에서 읽음 (빠름)
- 없으면 KIS REST → DB에 저장 후 반환

#### Step 3. Python LiveTrader → C++ Engine 연동

현재 `live/trader.py`는 Python에서 직접 REST를 폴링하고 주문합니다.
장기적으로는 `ZmqOperator`를 통해 C++ Engine에 주문을 위임하는 구조로 전환합니다:

```
Python LiveTrader          C++ Engine
  시그널 판단           →  ZMQ REQ "BUY 005930 1"
                        ←  ZMQ REP "OK"
                              ↓
                          KIS send_order()
```

#### Step 4. Kill Switch 연동

비정상 상황 시 Python에서 즉시 전체 청산:
```bash
python main.py operate kill   # C++ 엔진 종료 명령 (ZMQ REP)
```

#### Step 5. 실거래 전환 체크리스트

- [ ] 모의투자(`is_paper: true`)로 2주 이상 무중단 실행
- [ ] 백테스트 수익률 vs 페이퍼 결과 괴리 분석
- [ ] `config.json`에서 `is_paper: false`로 변경
- [ ] Docker 재빌드 없이 volume mount만으로 설정 변경 확인
- [ ] KIS 실거래 엔드포인트 (`openapi.koreainvestment.com:9443`) 연결 확인

---

## 8. 디스크 용량 관리

### 현재 용량 현황 (2026-05-20 기준)

| 항목 | 크기 | 비고 |
|------|------|------|
| Docker 이미지 | 3.7 GB | 비활성 이미지 2.0 GB 회수 가능 |
| Docker 빌드 캐시 | 1.7 GB | 1.4 GB 회수 가능 |
| TimescaleDB 볼륨 | 70 MB | 데이터 쌓일수록 증가 |
| `out/` (VS 빌드) | 96 MB | 로컬 Windows 빌드 산출물 |
| `build_test/` | 26 MB | 테스트 빌드 산출물 |
| `PYQuant/.venv/` | 26 MB | Python 가상환경 |
| `Quant/build_win/` | 18 MB | Ninja 빌드 산출물 |

### 용량 확보 명령어

```bash
# Docker 빌드 캐시 정리 (약 1.4 GB 회수, 다음 빌드가 느려짐)
docker builder prune -f

# 사용하지 않는 이미지 정리
docker image prune -f

# 중지된 컨테이너 + dangling 이미지 + 미사용 네트워크 일괄 정리
docker system prune -f

# ※ 주의: 아래는 TimescaleDB 데이터까지 삭제됨
docker system prune -f --volumes
```

### 로컬 빌드 산출물 정리

```powershell
# Windows PowerShell (프로젝트 루트에서)
Remove-Item -Recurse -Force out\, build_test\   # VS / CMake 빌드 결과물
```

### TimescaleDB 데이터 증가 추정

| 기간 | 예상 ticks 행 수 | 예상 크기 |
|------|-----------------|----------|
| 1 거래일 (6.5시간 × 28종목) | ~100만 건 | ~50 MB |
| 1개월 (20 거래일) | ~2,000만 건 | ~1 GB |
| 1년 | ~2.4억 건 | ~12 GB |

TimescaleDB의 자동 압축 정책으로 실제 저장 크기는 위의 20~40% 수준으로 줄어듭니다.

보존 정책 설정 예시 (3개월 이상 된 ticks 자동 삭제):
```sql
SELECT add_retention_policy('ticks', INTERVAL '3 months');
```
