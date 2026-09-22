-- TimescaleDB 익스텐션 활성화
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- ── 실시간 체결 (ZMQ TRADE) ───────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ticks (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    price      NUMERIC(18,4),
    volume     BIGINT,
    direction  SMALLINT,        -- 1=매수체결, 5=매도체결
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('ticks', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS ticks_ticker_ts ON ticks (ticker, ts DESC);

-- ── 전략 시그널 (ZMQ SIGNAL) ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS signals (
    ts         TIMESTAMPTZ  NOT NULL,
    strategy   TEXT,
    ticker     TEXT         NOT NULL,
    side       TEXT,            -- BUY / SELL
    qty        INTEGER,
    price      NUMERIC(18,4),
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('signals', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS signals_ticker_ts ON signals (ticker, ts DESC);

-- ── 주문 결과 (ZMQ ORDER) ─────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS orders (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    side       TEXT,
    qty        INTEGER,
    price      NUMERIC(18,4),
    ok         BOOLEAN,
    market     TEXT DEFAULT 'KR',
    account    TEXT            -- 브로커 계좌번호 (실계좌·모의계좌 원장 분리, D-090)
);
SELECT create_hypertable('orders', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS orders_ticker_ts ON orders (ticker, ts DESC);

-- ── 엔진 상태 (ZMQ HEALTH) ───────────────────────────────────────────────────
-- 큐 고수위·지연 분위수는 엔진 기동 후 누적이라 줄지 않는다. 구간 값은 읽는 쪽이 직전 행과 뺀다.
-- 지연 열은 표본이 없으면 -1, 옛 엔진이 보낸 행은 NULL(그 필드를 아예 안 싣는다).
CREATE TABLE IF NOT EXISTS health (
    ts         TIMESTAMPTZ  NOT NULL,
    data_cnt   BIGINT       DEFAULT 0,
    signal_cnt BIGINT       DEFAULT 0,
    order_cnt  BIGINT       DEFAULT 0,
    drop_cnt   BIGINT,      -- ZMQ 발행이 버린 건수(구독자가 못 따라오거나 소켓이 막힐 때)
    queue_shard_high_water   BIGINT,   -- 샤드 셀 가운데 가장 높았던 깊이
    queue_shard_capacity     BIGINT,
    queue_shard_out_size     BIGINT,   -- 샤드 → 전략 큐의 지금 깊이(누적 최대가 아니다)
    queue_shard_out_capacity BIGINT,
    queue_order_high_water   BIGINT,
    queue_order_capacity     BIGINT,
    queue_fill_high_water    BIGINT,
    queue_fill_capacity      BIGINT,
    dropped_shard            BIGINT,   -- 큐가 가득 차 버린 건수(누적)
    dropped_order            BIGINT,
    dropped_fill             BIGINT,
    latency_samples          BIGINT,   -- 분위수의 표본 수(누적 주문 건수)
    tick_to_signal_p50_us    BIGINT,
    tick_to_signal_p99_us    BIGINT,
    signal_to_pop_p50_us     BIGINT,
    signal_to_pop_p99_us     BIGINT,
    pop_to_done_p50_us       BIGINT,
    pop_to_done_p99_us       BIGINT,
    total_p50_us             BIGINT,
    total_p99_us             BIGINT
);
SELECT create_hypertable('health', 'ts', if_not_exists => TRUE);

-- ── 체결 원장 (H0STCNI0 체결통보) ───────────────────────────────────────────
CREATE TABLE IF NOT EXISTS fills (
    ts           TIMESTAMPTZ   NOT NULL,
    odno         TEXT          NOT NULL,   -- KIS 주문번호
    ticker       TEXT          NOT NULL,
    side         TEXT          NOT NULL,   -- BUY / SELL
    filled_qty   INTEGER       NOT NULL,
    filled_price NUMERIC(18,4) NOT NULL,
    commission   NUMERIC(18,4),            -- 수수료 (매수·매도 0.015%)
    tax          NUMERIC(18,4),            -- 거래세 (매도 0.18%)
    market       TEXT DEFAULT 'KR',
    regime       TEXT,
    strategy     TEXT,
    account      TEXT          -- 브로커 계좌번호 (실계좌·모의계좌 원장 분리, D-090)
);
SELECT create_hypertable('fills', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS fills_odno        ON fills (odno);
CREATE INDEX IF NOT EXISTS fills_ticker_ts   ON fills (ticker, ts DESC);
CREATE INDEX IF NOT EXISTS fills_ts_idx      ON fills (ts DESC);

-- fills.filled_qty·filled_price·commission·tax는 이미 체결통보(H0STCNI0)에서 채워지므로
-- C++/Python 쪽 계산 추가 없이 생성 컬럼으로 총액을 뽑는다.
ALTER TABLE fills ADD COLUMN IF NOT EXISTS total_amount NUMERIC(18,4)
    GENERATED ALWAYS AS (filled_qty * filled_price) STORED;  -- 총 거래대금(수수료·세금 제외)
ALTER TABLE fills ADD COLUMN IF NOT EXISTS net_amount NUMERIC(18,4)
    GENERATED ALWAYS AS (
        CASE WHEN side = 'SELL'
             THEN filled_qty * filled_price - COALESCE(commission, 0) - COALESCE(tax, 0)
             ELSE filled_qty * filled_price + COALESCE(commission, 0)
        END
    ) STORED;  -- 정산 반영 대금: 매수=지불액, 매도=실수령액

-- ── 포지션 원장 (계좌 현재 상태) ─────────────────────────────────────────────
-- 체결 발생 시 UPSERT, 장 마감 배치에서도 갱신
CREATE TABLE IF NOT EXISTS positions (
    account      TEXT          NOT NULL DEFAULT 'unknown',  -- 브로커 계좌번호 (D-090, PK의 일부)
    ticker       TEXT          NOT NULL,
    quantity     INTEGER       NOT NULL DEFAULT 0,
    avg_price    NUMERIC(18,4) NOT NULL DEFAULT 0,  -- 매수 평균단가
    realized_pnl NUMERIC(18,4) NOT NULL DEFAULT 0,  -- 당일 실현손익
    updated_at   TIMESTAMPTZ   NOT NULL DEFAULT NOW(),
    PRIMARY KEY (account, ticker)
);

-- ── 계좌 일별 스냅샷 (원금추적/기간수익률용) ────────────────────────────────
-- 장 마감 1회 get_kr_balance() 결과를 적재. account='paper'|'real' 로 모의/실전 분리.
CREATE TABLE IF NOT EXISTS account_snapshots (
    ts             TIMESTAMPTZ   NOT NULL,
    account        TEXT          NOT NULL,   -- 'paper' | 'real'
    cash           NUMERIC(18,4),            -- 예수금
    total_eval     NUMERIC(18,4),            -- 총평가금액
    total_pnl      NUMERIC(18,4),            -- 총평가손익
    total_pnl_rate NUMERIC(10,4)             -- 총수익률(%)
);
SELECT create_hypertable('account_snapshots', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS account_snapshots_acct_ts ON account_snapshots (account, ts DESC);

-- ── 입출금 내역 (원금 추적) ─────────────────────────────────────────────────
-- 실전은 입출금이 의미 있음(현재 수동 기록 — KIS 입출금 TR 확정 시 자동화).
-- 모의는 입출금 개념이 없어 비워 둠.
CREATE TABLE IF NOT EXISTS cash_flows (
    ts         TIMESTAMPTZ   NOT NULL,
    account    TEXT          NOT NULL,   -- 'paper' | 'real'
    flow_type  TEXT          NOT NULL,   -- DEPOSIT | WITHDRAW
    amount     NUMERIC(18,4) NOT NULL,   -- 양수 (방향은 flow_type)
    memo       TEXT
);
CREATE INDEX IF NOT EXISTS cash_flows_acct_ts ON cash_flows (account, ts DESC);

-- ── KIS REST 일봉 (bars_1d) ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS bars_1d (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    open       NUMERIC(18,4),
    high       NUMERIC(18,4),
    low        NUMERIC(18,4),
    close      NUMERIC(18,4),
    volume     BIGINT,
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('bars_1d', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS bars_1d_ticker_ts ON bars_1d (ticker, ts DESC);

-- ── 시장 국면 (RegimeController, 장 시작 1회 판정) ───────────────────────────
-- 학습 데이터 축적 시작점. 개별 지표 분해 저장 (어느 지표가 국면을 갈랐나).
CREATE TABLE IF NOT EXISTS regime (
    date          DATE PRIMARY KEY,
    regime        TEXT NOT NULL,        -- BULL / NEUTRAL / BEAR
    score         INT  NOT NULL,        -- v0: -2..+2
    above_ma200   BOOLEAN,
    aligned_bull  BOOLEAN,
    aligned_bear  BOOLEAN,
    index_close   DOUBLE PRECISION,
    ts            TIMESTAMPTZ NOT NULL
);

-- ── 거래 라벨: 그때의 국면을 행동(signals)·결과(fills)에 stamp ────────────────
-- (상황→행동→결과) 삼각형 완성. 지금 안 붙이면 소급 불가.
ALTER TABLE signals ADD COLUMN IF NOT EXISTS regime TEXT;
ALTER TABLE fills   ADD COLUMN IF NOT EXISTS regime TEXT;

-- ── 엔진 프로세스 자원 표본 (procwatch, 그라파나 서버자원 패널) ──────────────
-- ZMQ HEALTH에는 처리건수만 있고 CPU/메모리가 없어 별도로 표본을 뜬다.
CREATE TABLE IF NOT EXISTS proc_stats (
    ts            TIMESTAMPTZ  NOT NULL,
    process_name  TEXT         NOT NULL,
    pid           INTEGER,
    cpu_percent   DOUBLE PRECISION,   -- psutil Process.cpu_percent(interval) — 코어 100%=1개 코어 풀가동
    memory_mb     DOUBLE PRECISION,   -- RSS
    thread_count  INTEGER,
    core_count    INTEGER             -- 그 기계의 논리 코어 수 — cpu_percent/100 이 몇 코어를 쓰는지 읽을 때 분모
);
SELECT create_hypertable('proc_stats', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS proc_stats_name_ts ON proc_stats (process_name, ts DESC);

-- 스레드별 CPU (리눅스 /proc/<pid>/task/*/stat 차분). thread_name은 엔진이 붙인 이름(DataThread·Shard 0 …)
CREATE TABLE IF NOT EXISTS proc_thread_stats (
    ts            TIMESTAMPTZ  NOT NULL,
    process_name  TEXT         NOT NULL,
    pid           INTEGER,
    tid           INTEGER,
    thread_name   TEXT,
    cpu_percent   DOUBLE PRECISION
);
SELECT create_hypertable('proc_thread_stats', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS proc_thread_stats_name_ts ON proc_thread_stats (thread_name, ts DESC);

-- 함수별 자기 시간 비율 (perf record -e cpu-clock 표본, 한 회차 = 같은 ts)
CREATE TABLE IF NOT EXISTS proc_hotspots (
    ts             TIMESTAMPTZ  NOT NULL,
    process_name   TEXT         NOT NULL,
    pid            INTEGER,
    sample_seconds DOUBLE PRECISION,
    symbol         TEXT         NOT NULL,
    shared_object  TEXT,
    self_percent   DOUBLE PRECISION,
    samples        BIGINT
);
SELECT create_hypertable('proc_hotspots', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS proc_hotspots_ts ON proc_hotspots (ts DESC);

-- 관측 표 압축·보존 (2026-09-22 실측). 5초 주기 시계열이라 ts·process_name·thread_name이 반복돼 압축비가 잘 나온다.
-- 최근 일주일은 장중에 그대로 읽어야 해서 그 앞은 건드리지 않는다. 삭제는 스레드별 CPU만 — 나머지 둘은
-- 1년에 합쳐 1.1 GB라 지우는 값어치가 없고, proc_stats는 "작년 이맘때 대비"를 볼 수 있는 유일한 표다.
ALTER TABLE proc_stats SET (timescaledb.compress,
    timescaledb.compress_segmentby = 'process_name', timescaledb.compress_orderby = 'ts DESC');
SELECT add_compression_policy('proc_stats', INTERVAL '7 days', if_not_exists => TRUE);

ALTER TABLE proc_thread_stats SET (timescaledb.compress,
    timescaledb.compress_segmentby = 'process_name,thread_name', timescaledb.compress_orderby = 'ts DESC');
SELECT add_compression_policy('proc_thread_stats', INTERVAL '7 days', if_not_exists => TRUE);
SELECT add_retention_policy('proc_thread_stats', INTERVAL '90 days', if_not_exists => TRUE);

ALTER TABLE proc_hotspots SET (timescaledb.compress,
    timescaledb.compress_segmentby = 'process_name', timescaledb.compress_orderby = 'ts DESC');
SELECT add_compression_policy('proc_hotspots', INTERVAL '7 days', if_not_exists => TRUE);

-- ── 벤치마크용 테스트 테이블 (bench_market_open.py) ───────────────────────────
-- 실거래 테이블(ticks/signals/orders/fills/positions)과 같은 모양으로 별도 유지 —
-- 개장 폭주를 재현해 DB 부하를 측정할 때 실계좌/모의계좌 원장을 건드리지 않으려고 분리했다.
CREATE TABLE IF NOT EXISTS bench_ticks (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    price      NUMERIC(18,4),
    volume     BIGINT,
    direction  SMALLINT,
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('bench_ticks', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS bench_ticks_ticker_ts ON bench_ticks (ticker, ts DESC);

CREATE TABLE IF NOT EXISTS bench_signals (
    ts         TIMESTAMPTZ  NOT NULL,
    strategy   TEXT,
    ticker     TEXT         NOT NULL,
    side       TEXT,
    qty        INTEGER,
    price      NUMERIC(18,4),
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('bench_signals', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS bench_signals_ticker_ts ON bench_signals (ticker, ts DESC);

CREATE TABLE IF NOT EXISTS bench_orders (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    side       TEXT,
    qty        INTEGER,
    price      NUMERIC(18,4),
    ok         BOOLEAN,
    market     TEXT DEFAULT 'KR',
    account    TEXT
);
SELECT create_hypertable('bench_orders', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS bench_orders_ticker_ts ON bench_orders (ticker, ts DESC);

CREATE TABLE IF NOT EXISTS bench_fills (
    ts           TIMESTAMPTZ   NOT NULL,
    odno         TEXT          NOT NULL,
    ticker       TEXT          NOT NULL,
    side         TEXT          NOT NULL,
    filled_qty   INTEGER       NOT NULL,
    filled_price NUMERIC(18,4) NOT NULL,
    commission   NUMERIC(18,4),
    tax          NUMERIC(18,4),
    market       TEXT DEFAULT 'KR',
    account      TEXT
);
SELECT create_hypertable('bench_fills', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS bench_fills_ticker_ts ON bench_fills (ticker, ts DESC);

CREATE TABLE IF NOT EXISTS bench_positions (
    account      TEXT          NOT NULL DEFAULT 'bench',
    ticker       TEXT          NOT NULL,
    quantity     INTEGER       NOT NULL DEFAULT 0,
    avg_price    NUMERIC(18,4) NOT NULL DEFAULT 0,
    realized_pnl NUMERIC(18,4) NOT NULL DEFAULT 0,
    updated_at   TIMESTAMPTZ   NOT NULL DEFAULT NOW(),
    PRIMARY KEY (account, ticker)
);

-- ── 원장 이벤트 (D-113) ─────────────────────────────────────────────────────
-- 정본은 엔진이 주문 직전에 적는 파일 ledger_YYYYMMDD.bin이고, 이 테이블은 그 파일을 따라 적는
-- 복제본이다. 적재기(PYQuant/tools/ledger_recorder.py)가 파일을 꼬리부터 읽어 넣는다.
-- 같은 줄을 두 번 넣어도 원장이 부풀지 않도록 (trade_date, seq)가 PK다 — seq는 파일 안에서만
-- 증가하므로 날짜를 같이 잡아야 유일해진다.
CREATE TABLE IF NOT EXISTS ledger_events (
    trade_date   DATE          NOT NULL,   -- 저널 파일 헤더의 YYYYMMDD
    seq          BIGINT        NOT NULL,   -- 파일 안 1부터 증가
    ts           TIMESTAMPTZ   NOT NULL,   -- 엔진이 그 줄을 적은 시각
    kind         TEXT          NOT NULL,   -- SEED/INTENT/ACCEPT/REJECT/FILL/CANCEL/ADJUST/RESET_RESERVED/CASH/DAILY_PNL
    account      TEXT,
    ticker       TEXT,
    side         TEXT,                     -- BUY / SELL / NONE
    order_type   TEXT,                     -- MARKET / LIMIT
    order_id     BIGINT,                   -- 엔진 내부 주문번호(ORD-NNNNNN)
    odno         BIGINT,                   -- KIS 주문번호
    quantity     INTEGER,
    reserved_qty INTEGER,                  -- ADJUST — 맞춘 뒤 선점(BUY +, SELL -)
    sellable     INTEGER,
    price        NUMERIC(18,4),
    cash         NUMERIC(18,4),
    equity       NUMERIC(18,4),
    pnl          NUMERIC(18,4),
    strategy     TEXT,
    reason       TEXT,
    PRIMARY KEY (trade_date, seq)
);
CREATE INDEX IF NOT EXISTS ledger_events_ts        ON ledger_events (ts DESC);
CREATE INDEX IF NOT EXISTS ledger_events_ticker_ts ON ledger_events (ticker, ts DESC);
CREATE INDEX IF NOT EXISTS ledger_events_order     ON ledger_events (trade_date, order_id);

-- 적재기가 어디까지 읽었는지 — 파일 바이트 위치. 내렸다 올리면 여기서부터 이어 읽는다.
CREATE TABLE IF NOT EXISTS ledger_offsets (
    journal_file TEXT          PRIMARY KEY,   -- 파일 이름만(경로 제외) — 기계를 옮겨도 이어진다
    trade_date   DATE          NOT NULL,
    byte_offset  BIGINT        NOT NULL DEFAULT 0,
    last_seq     BIGINT        NOT NULL DEFAULT 0,
    updated_at   TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);
