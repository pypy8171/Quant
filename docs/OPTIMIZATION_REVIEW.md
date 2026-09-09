# 전체 코드 평가 및 수정 가이드 (2026-09-07)

기존 리뷰·에이전트 결과를 참조하지 않고 트리를 처음부터 다시 읽어 정리했다.
범위: `Quant/`(C++ 18,675줄) + `PYQuant/`·`scripts/`(Python 12,355줄).

판정 축은 세 가지다 — 최적화(O), 아키텍처(A), 가독성(R). 각 항목에 근거 위치를 붙였다.

---

## 요약

지금 이 코드베이스의 실제 지연 병목은 큐나 자료구조가 아니라 **네트워크 호출을 스레드 위에서 동기로 돌리는 지점**이다.
`bench_market_firehose` 실측이 E2E p50 300ns인데(파이프라인은 이미 충분히 빠르다), 라이브에서 발주가 늦는 이유는
전략 스레드가 REST 응답을 기다리기 때문이다. 따라서 O1·O2가 최우선이고, 링버퍼·문자열 최적화(O4·O5)는 후순위다.

구조 쪽에서는 `data_thread_fn()` 하나가 시세·잔고 대조·관측 3종을 같은 `try{}` 안에서 돌리는 것이 가장 위험하다.
관측 코드의 예외가 그 사이클의 시세 수집을 통째로 건너뛴다.

---

## 최적화 (O)

### O1. 전략 스레드에서 동기 REST — 가장 큰 지연 원인
`Quant/include/strategy/DeviationScaleStrategy.h` `sellable_qty()`가 공유 전략 스레드에서
`akis->get_balance()`를 동기로 호출한다. 소스 주석도 이 위험을 이미 인정하고 있다
("한 종목의 잔고 조회가 다른 종목 전부의 발주를 수십 초 막는다"). `FastFailScope`로 재시도는 껐지만
왕복 자체는 남는다. 같은 클래스에 이미 `prefetch_thread_` + `snap_mtx_` 스냅샷 패턴이 있다.

- 수정: 매도가능수량도 프리페치 스냅샷에 포함시키고, 스냅샷이 낡았으면(예: 5초 초과) 보수적으로 0을 반환한다.
- 검증: 전략 스레드 사이클 시간을 로깅해 REST 왕복이 사라졌는지 본다.

### O2. 데이터 스레드의 직렬 sleep — 유니버스 크기에 비례해 시세가 낡는다
`Quant/src/core/Engine.cpp` `data_thread_fn()`:
- 종목당 `sleep_for(150ms)` 후 `get_current_price()` — 40종목이면 한 바퀴에 6초 이상.
- 섹터 10개에 `sleep_for(120ms)`, 수급추정에 `sleep_for(150ms)`가 추가로 붙는다.

`KisClient`에는 이미 토큰버킷(`rate_limit_acquire`/`note_rate_limited`)이 있다. 고정 sleep은 그 위에 얹힌 이중 호출 간격 조절이다.

- 수정: 고정 sleep을 걷어내고 토큰버킷에만 맡긴다. 관측용 호출(섹터·수급·매크로)은 A1에서 분리한 스레드로 옮겨
  시세 경로와 예산을 나눈다.

### O3. 전략 팬아웃이 O(이벤트 × 전략)
`strategy_thread_fn()`이 호가/체결 이벤트마다 등록된 전 전략을 순회한다. 대부분의 전략은 자기 종목이 아니면
즉시 반환하지만, 종목 수 × 전략 수만큼 가상 호출이 발생한다.

- 수정: 전략 스냅샷을 뜰 때 `ticker -> vector<StrategyBase*>` 인덱스를 같이 만들어 해당 종목 구독자만 호출한다.

### O4. RingBuffer 폴리시 (후순위)
`Quant/include/core/RingBuffer.h`:
- `(head + 1) % capacity_` — capacity를 2의 거듭제곱으로 강제하고 마스크 연산으로 바꾼다.
- push/pop마다 상대편 인덱스를 `acquire`로 다시 읽어 다른 코어의 캐시라인을 건드린다. `cached_tail_`/`cached_head_`를
  두고 여유가 없을 때만 재적재한다.
- `pop_batch(span)`이 없어 소비 측이 원소마다 한 번씩 원자 연산을 한다.

이미 무손실 2.9M msg/s를 내는 구조라 체감 이득은 크지 않다. **지연 개선으로 주장하지 말고, 측정 후 수치가 나올 때만 반영한다.**

### O5. 큐 원소가 문자열을 들고 다닌다
`Quant/include/core/Types.h`의 `OrderSignal`은 `std::string` 7개(ticker, strategy_id, exchange, account_id,
client_oid, orig_client_oid, reason)를 갖는다. push마다 복사 대입이므로 할당이 따라온다.

- 수정: 최소한 `ticker`/`strategy_id`는 고정 길이 배열이나 인턴된 id로 바꾼다. `reason`은 코드 + 파라미터로 축약한다.
- 이것도 O4와 같은 성격이라 O1·O2 이후에 본다.

### O6. OrderGate 지연에 민감한 경로의 선형 스캔
`Quant/src/risk/OrderGate.cpp` `check()`의 §3c(동시 보유 종목 수)·§3d(총노출)가 매 BUY마다
`positions_`와 `reserved_` 전체를 `positions_mtx_` 아래서 훑는다. 거절 사유는 매번 `std::ostringstream`으로 만든다.
`positions_.count(k) ? positions_[k] : 0`은 조회를 두 번 하고 비-const `operator[]`로 없는 키를 삽입할 여지도 있다.

- 수정: 보유 종목 수와 총노출을 갱신 시점에 증분 유지한다. 거절 문자열은 문자열 연결로 바꾸고, 조회는 `find()` 한 번으로 끝낸다.

### O7. 빌드 플래그
`Quant/CMakeLists.txt`:
- `-Wall -Wextra -Wpedantic`이 `if(UNIX)` 안에만 있다. 주 개발 플랫폼인 MSVC는 경고가 사실상 꺼져 있다.
- LTO/IPO가 어디에도 없다.

- 수정: MSVC에 `/W4`를 켜고, Release에 `INTERPROCEDURAL_OPTIMIZATION ON`을 건다. 경고를 켜면 처음엔 많이 나올 테니
  한 번에 `/WX`까지 가지 말고 경고 목록을 먼저 본다.

---

## 아키텍처 (A)

### A1. `data_thread_fn()`이 한 함수에 너무 많은 일을 담았다 — 관측 예외가 시세 수집을 멈춘다
약 560줄 한 함수 안에 국면 폴링, 유니버스 재스캔, 잔고 대조, 관측 3종(수급추정·섹터·매크로),
REST 시세 폴링, 일봉 폴링이 모두 들어 있고 **하나의 `try{}`로 감싸여 있다**. 섹터 조회에서 예외가 나면
그 사이클의 시세 수집이 통째로 건너뛰어진다.

- 수정: 두 스레드로 나눈다.
  - `data_thread` — 시세·일봉만. 실패는 즉시 로깅하고 다음 종목으로.
  - `ops_thread` — 잔고 대조·국면·재스캔·관측. 각 블록이 자기 `try{}`를 갖는다.
- 이건 이 목록에서 **라이브 안정성 기여가 가장 큰 항목**이다.

### A2. Engine 설정 주입 세터가 11개다
`Quant/include/core/Engine.h`에 설정 주입 세터가 11개, `Quant/src/main.cpp`에는 그걸 채우는 명령형 배선이 약 120줄 있다.
config는 `cfg["kis"]["app_key"]`처럼 직접 인덱싱해 검증 계층이 없다.

- 수정: `EngineConfig` 구조체 하나로 모으고, 파싱·검증을 `ConfigLoader`에 몰아 넣는다. 필수 키 누락은 기동 시점에 죽는다.

### A3. `KisClient.cpp` 2,588줄 단일 파일
전송(WinHTTP/libcurl), OAuth, 레이트리밋, 캐시, 시세·주문·랭킹·미국장·파생이 한 파일에 있다.

- 수정: `HttpTransport`(플랫폼 분기만) / `KisAuth` / `KisMarketData` / `KisOrders` / `KisDerivatives`로 나눈다.
  `#ifdef _WIN32` 분기가 전송 계층 한 곳에만 남는 것이 목표다.

### A4. 전략이 헤더 온리
`DeviationScaleStrategy.h`가 715줄 전체 구현이다. 전략 한 줄만 고쳐도 이 헤더를 포함한 모든 TU가 다시 컴파일된다.

- 수정: 헤더는 선언만 남기고 구현을 `.cpp`로 옮긴다. `MACross`/`Momentum`은 이미 이 형태라 선례가 있다.

### A5. 중단 불가능한 sleep
`stop()`이 스레드를 join하는데 데이터 스레드는 `sleep_for(60s)` 안에 있을 수 있다. 종료가 느리다.

- 수정: `std::condition_variable` + stop 플래그로 `wait_for`를 쓴다. sleep 지점 전부에 적용한다.

### A6. 시그널 핸들러가 async-signal-unsafe
`Quant/src/main.cpp`의 `signal_handler`가 `g_engine->stop()`을 직접 부른다 — 뮤텍스 획득·로깅·스레드 join이 들어 있다.

- 수정: 핸들러는 `volatile std::sig_atomic_t g_stop = 1`만 세우고, 메인 루프가 그걸 보고 `stop()`을 호출한다.

### A7. 죽은 빌드 항목
`Quant/src/core/RingBuffer.cpp`는 0바이트, `Quant/src/strategy/StrategyBase.cpp`는 `// placeholder` 한 줄인데
둘 다 SOURCES에 있다.

- 수정: 목록에서 뺀다.

### A8. 테스트가 빌드는 되는데 실행 배선이 없다
테스트·벤치 실행파일이 11개 있는데 `enable_testing()`/`add_test()`가 없어 `ctest`로 돌릴 수 없다. CI 설정도 없다.
CLAUDE.md는 아직 "테스트 스위트는 없습니다"라고 적혀 있는데, 이는 현재 트리와 맞지 않는다.

- 수정: 결정론적인 것들(`test_order_router`, `test_pipeline_stress` 등)을 `add_test()`로 등록하고 CLAUDE.md를 고친다.
  벤치는 테스트로 등록하지 않는다(수치가 머신마다 달라 게이트로 못 쓴다).

### A9. 테스트 타깃마다 소스를 다시 나열
공유 라이브러리 타깃이 없어 각 테스트가 필요한 `.cpp`를 개별로 다시 컴파일한다.

- 수정: `quant_core` STATIC 라이브러리를 만들고 `quant_trader`와 테스트가 모두 링크한다.

### A10. 국면 축이 두 개인데 조정자가 없다
`RegimeController`(지수 이평 기반 전략 집합 선택)와 `regime.json` 파일 전달(`set_entry_halt`)가 서로 모른다.
문서에는 분리 의도가 적혀 있지만 코드에는 둘의 우선순위를 정하는 지점이 없다.

- 수정: `RegimeState` 하나가 두 입력을 받아 최종 상태를 내도록 좁힌다. 지금 당장은 아니어도, 세 번째 축이 생기기 전에 한다.

### A11. KIS 클라이언트 이중 구현
C++ 2,588줄과 Python 657줄이 같은 API를 각자 구현한다. tr_id·필드명이 따로 흘러 드리프트한다.

- 수정: 지금 합칠 필요는 없다. 대신 tr_id와 엔드포인트 경로만 한 곳(JSON/YAML)에서 읽도록 빼서 둘이 같은 표를 본다.

---

## 가독성 (R)

### R1. 헤더가 설계 에세이다
`Engine.h`는 코드보다 주석 산문이 많다. 근거는 `docs/`로 옮기고 헤더에는 결정 지점에 한 줄씩만 남긴다.

### R2. KST 시간 헬퍼 중복
`DeviationScaleStrategy.h`의 `kst_tm`/`kst_hhmm`/`kst_bar_bucket`/`kst_ymd`와 `Engine.cpp`의 `utc_plus_hours()`가
같은 일을 따로 한다. `utils/KstTime.h`(신설) 하나로 모은다.

### R3. 섹터 테이블 중복
`Engine.cpp`의 인라인 `kSectors`가 `ThemeStrategy.h`의 `KOSPI_SECTORS`와 겹친다. 소스 주석도 중복임을 적어 두었다.
한쪽을 지운다.

### R4. 멤버 함수 안의 `static` 카운터
`data_thread_fn()` 안의 `static int est_flow_tick / sector_tick / macro_tick`. 밖에서 안 보이고, 테스트에서 초기화할 수 없고,
Engine을 두 개 만들면 공유된다. 멤버 필드로 옮긴다.

### R5. 파일 중간 전방 선언
`utc_plus_hours()`가 662줄에서 선언되고 1332줄에서 정의된다. 헤더나 파일 상단으로 올린다.

### R6. 락 규약이 일관되지 않다
`apply_regime_selection()`은 `strategies_`를 락 없이 순회하고 `daily_bars_needed()`는 `strat_mutex_`를 잡는다.
어느 쪽이 맞는지 정하고 클래스 주석에 불변식을 적는다.

### R7. `scripts/dashboard_server.py` (916줄)
- HTML/CSS/JS 약 370줄이 파이썬 소스 문자열에 박혀 있다. 별도 템플릿 파일로 뺀다.
- `sys.path.insert(0, REPO/"PYQuant")` 경로 조작은 패키지 설치나 `PYTHONPATH`로 바꾼다.
- `_candidate_log_dirs()`가 mtime이 가장 최신인 디렉터리를 로그 폴더로 추정한다. 이미 `QUANT_LOG_DIR` 규약이 있으니
  그걸 우선 읽고 추정은 폴백으로만 둔다.

### R8. `.clang-format`은 있는데 게이트가 없다
포맷 검사를 `scripts/check_docs.py`와 같은 위치(커밋 전)에 붙일지 정한다.

---

## 진행 순서

| 단계 | 항목 | 성격 | 예상 |
|---|---|---|---|
| 0 | A7, A8, R3, R4, R5, O7 | 위험 없음, 즉시 | 반나절 |
| 1 | **A1, O1, A5, A6** | 라이브 안정성 — 최우선 | 1~2일 |
| 2 | O2, O3, O6 | 지연·처리량, 측정 동반 | 1~2일 |
| 3 | A2, A9, A4 | 구조 정리, 컴파일 시간 | 2~3일 |
| 4 | A3, R1, R2, R7 | 대수술·문서 | 이후 |
| 보류 | O4, O5, A10, A11, R6, R8 | 근거 생기면 | — |

단계 1을 먼저 하는 이유는 성능이 아니라 **장중에 조용히 실패하는 경로를 없애는 것**이 먼저이기 때문이다.
A1은 관측 코드 예외가 시세를 멈추는 것을 막고, O1은 한 종목의 잔고 조회가 전 종목 발주를 막는 것을 막는다.
둘 다 로그에는 "매매가 멈췄다"로만 보이는 종류의 문제다.

단계 2 이후는 항목마다 전후 수치를 남긴다. 수치가 없으면 반영하지 않는다.
