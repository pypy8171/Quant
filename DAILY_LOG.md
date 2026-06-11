# Daily Log — Quant Trading System

---

## 2026-06-08 (월)

### 한 일
- **모의 WS(:31000) 회복 + 전체 파이프라인 검증** — WS 시세→전략→모의주문(체결)→OrderRouter end-to-end 정상.
- **리뷰 G-1/W-1 반영** — 국면 게이트 `active_` atomic화(data race 제거), Linux WS disconnect `shutdown()`로 종료 교착 해소.
- **DB 원장 트랙 완성 (오늘의 핵심)**:
  - 스키마 드리프트 해결 — fills/positions/regime 테이블이 옛 볼륨에 없던 것 `schema.sql` 재적용으로 생성.
  - orders/signals 적재는 원래 정상이었음(멀티터미널 인스턴스 혼선이 원인 → 단일 스택으로 해소).
  - **체결통보(H0STCNI9) AES-256-CBC 복호화 구현** — KIS 시세는 평문이나 체결통보는 암호화 전문임을 모의 실측으로 발견. 구독응답 key/iv 확보 → base64+AES 복호. Linux=OpenSSL/Windows=BCrypt.
  - **체결구분 파싱 버그 수정** — `f[13]`을 "Y"로 검사하던 것을 "2"(체결)로. 기존엔 모든 체결통보가 조용히 드롭돼 fills/positions가 영구 공백이었음 → 수정 후 실체결가·수수료·세금 적재 + at-least-once 멱등 동작 확인.
- **로컬 @reviewer + 웹 Claude 리뷰 반영** — C-1~4(key/iv 길이검증·재연결 clear·평문 sanity), C-2/W-4(토큰 캐시 atomic write·chmod 0600). C-1/C-3는 오탐·검증완료로 플래그(번들 축약 탓).
- **KIS 토큰 공유 캐시 + `balance --paper`** — docker 컨테이너 간 공유 볼륨으로 엔진 발급 토큰을 balance가 재사용(중복발급 403 해소). 모의계좌 잔고 터미널 조회 + 한글 폭 보정 정렬.
- **strategist 방향 설정** — 병목이 인프라→엣지로 이동. 다음 한 수 = VALUE_CONTRARY 백테스트로 엣지 흑백 판정. 지수 페이지네이션/국면게이트 정교화는 "보호할 엣지 0개"라 보류.

### 변경 파일
- `Quant/src/api/WebSocketClient.cpp` → 체결통보 AES-256-CBC 복호화(base64_decode/aes_cbc_decrypt 플랫폼분기) + 파싱 `f[13]=="2"` 수정 + 견고화(C-1~4)
- `Quant/include/api/KisWebSocket.h` → aes_key_/aes_iv_ 멤버 + 복호화 선언
- `Quant/src/api/KisClient.cpp` → 토큰 캐시 경로 env(KIS_TOKEN_CACHE_DIR) + atomic write + chmod 0600
- `PYQuant/kis/client.py` → 토큰 캐시 공유(C++ 동일 파일·포맷) + atomic os.replace
- `PYQuant/main.py` → balance `--paper`, `_resolve_config`(/.dockerenv), 한글 정렬 헬퍼
- `docker-compose.yml` → token-cache 공유 볼륨 + KIS_TOKEN_CACHE_DIR
- `Quant/CMakeLists.txt`·`Dockerfile` → OpenSSL(Linux)/bcrypt(Windows) 링크, libssl

### 막힌 지점 / 미해결
- RegimeController 지수 일봉이 50봉만 수집(페이지네이션 1콜)돼 ma200 못 만들고 NEUTRAL 폴백 → 국면 게이트 실질 무력. (엣지 검증 후로 보류)
- 백로그: W-1(복호 실패 로깅), W-2(base64 strict), OrderRouter `seen_fills_` 무한 증가(EOD clear 경로 없음).
- AES 견고화·토큰공유 변경의 **전체 체결 fills 회귀**는 장 마감으로 다음 장중 재확인 필요(happy-path 불변).

### 내일 할 일
- **VALUE_CONTRARY 백테스트로 엣지 흑백 판정** (사전 합격기준 박고) — strategist의 다음 한 수.
- (장중) AES/토큰 변경분 전체 체결 fills 회귀 확인.

### 학습 카드 영향
- **체결통보 AES-256-CBC 복호화 직접 구현** — "왜 OS crypto(EVP/CNG) 직접? / 체결통보가 암호화인 걸 실측으로 발견 / CBC는 MAC 없어 복호 후 필드검증으로 garbage 거부" 등 강한 카드 다수 확보.
- **토큰 공유 캐시(atomic write)** — "프로세스 2개가 1토큰 공유, 비원자적 쓰기→temp+rename" IPC/동시성 카드.
- **"조용히 틀리는 버그" 규명 사례** — `f[13]` "Y"vs"2"로 체결통보 전량 드롭되던 것을 모의 실측 23필드로 규명·수정.

---

## 2026-06-04 (목)

### 한 일
- **웹 Claude 리뷰(C1~C10) 코드 반영 (`/review-apply`)** — 1·2·3순위 전부 처리
  - 🔴 C1 중복 체결통보 멱등 처리 (at-least-once 방어)
  - 🔴 C2·C4 OrderGate 선점(reserved)/실체결(positions) 원장 분리 — 부분체결 평단 왜곡·net_qty 불일치 동시 해결
  - 🔴 C3 WS 재연결 시 joinable 스레드 재대입 → std::terminate 크래시 가드
  - 🟡 C6 TOCTOU 단일호출자 가정 주석 / C7 backtest total_return equity 기반 / C8 ZMQ FILL 토픽 drop 차등
  - 🟢 C9 큐 스핀(검토완료·변경불필요) / C10 손실한도 BUY-only 설계의도 확정
  - 🟦 C5 미체결 타임아웃/취소 — reserved 일일만료만 반영, KIS 취소 API 선행 필요로 부분반영
- 회귀 테스트 추가/갱신: `test_duplicate_fill_ignored`(C1), `test_partial_fill_avg_price`(C2/C4), `test_sell_clamps_position_at_zero` 재작성 → **test_order_gate 10/10, test_order_router 7/7 통과**, quant_trader.exe 빌드 OK
- `@review-recorder` 에이전트 신설 — review-apply 반영분을 git diff 기준으로 검증·정리 (AGENTS.md 등록)
- 리뷰용 번들 `REVIEW_PROMPT.md`(full) 재생성 — FEP 레이어/체결통보 파이프라인 반영, secret 미포함 확인
- `config_paper.json` 검증용 구성 정비 — SDP(INTRADAY) 제거하고 FixedInterval **BUY/SELL 왕복**(402340·097230, interval 120/180)으로 교체 (파이프라인 SELL 경로까지 검증 목적)
- 보안 점검 — config_paper.json/config.json이 git 이력에 단 한 번도 없음 확인 (.gitignore 정상, secret 노출 0)
- 1M 컨텍스트 크레딧 에러 해결 — `CLAUDE_CODE_DISABLE_1M_CONTEXT=1` 사용자 환경변수 설정

### 변경 파일
- `Quant/include/risk/OrderGate.h` / `src/risk/OrderGate.cpp` → reserved_/positions_ 분리, reserved() 게터, check()=합산, on_fill_confirmed 실체결 평단, reset_daily reserved 만료, C6/C10 주석
- `Quant/include/ipc/OrderRouter.h` / `src/ipc/OrderRouter.cpp` → seen_fills_ 멱등 set + on_fill dedup
- `Quant/src/api/WebSocketClient.cpp` → connect()(Win/Linux) recv_thread_ join 가드
- `Quant/src/ipc/ZmqBridge.cpp` → enqueue 토픽별 drop 차등(FILL/ORDER/SIGNAL 보존)
- `PYQuant/backtest/engine.py` → total_return을 equity[-1] 기반으로
- `Quant/tests/test_order_gate.cpp` / `test_order_router.cpp` → 회귀 테스트 추가/갱신
- `CODE_REVIEW.md` → C1~C10 ✅/🟦 + 반영 내역 상세 기록
- `.claude/agents/review-recorder.md` / `.claude/AGENTS.md` → 신규 에이전트 + 등록
- `Quant/config/config_paper.json` → 검증용 왕복 전략 구성 (gitignored)

### 막힌 지점 / 미해결
- 🟦 C5 미체결 주문 타임아웃/취소: **KisClient에 주문 취소 API(order-rvsecncl)가 없어** 풀구현 보류 — 취소 API 구현이 선행돼야 함 (실거래 전 필수)
- WS 재연결 이중 메커니즘(recv_loop 내부 + control_thread) 일원화 미해결 — C5와 함께 재검토
- C8 FILL drop 차등은 HAS_ZMQ 빌드 전용 → Docker/Linux에서 컴파일·동작 미검증

### 내일 할 일
- Docker로 모의 왕복 파이프라인 검증 — BUY/SELL 체결이 fills/positions/orders DB 원장에 정상 기록되는지 확인 (이게 되면 그간 작업분 커밋)
- 검증 통과 후 SDP를 entry_mode=EOD + 완화 파라미터로 재투입
- (여유 시) KIS 주문취소 API 구현 → C5 풀구현

### 학습 카드 영향
- **체결원장 도메인 버그 4종을 코딩으로 발견·보완** → 학습 카드 다수 확보:
  - 외부 이벤트 at-least-once 멱등성(C1), 예약잔고 vs 확정잔고 분리·평단 회계(C2/C4), joinable std::thread 재대입=terminate·스레드 RAII(C3)
- "토이라 안 했다"가 아니라 "약점을 정확히 짚고 보완 중(C5)"으로 프레이밍 가능 — 거래소 연동 운영 이해도 어필

---

## 2026-05-26 (월)

### 한 일
- VSCode C++ 빌드 환경 구성 — tasks.json, c_cpp_properties.json 추가, vcvarsall.bat 연동
- launch.json 실전/모의투자 실행 구성 분리 (config.json / config_paper.json)
- `kis/client.py` — `get_kr_balance()` 추가 (BalanceItem, AccountSummary 데이터클래스 포함)
- `main.py` — `balance --watch` 서브커맨드 추가 (국내주식 잔고 폴링)
- KIS 모의투자 계좌 발급 — config_paper.json 분리, .gitignore 추가
- `Quant/src/main.cpp` — KR_WATCH에 제주반도체(080220) 추가

### 변경 파일
- `.gitignore` → `config_paper.json` 추가 (인증정보 git 제외)
- `.vscode/launch.json` → C++ 실전/모의투자 실행 구성 분리
- `.vscode/tasks.json` → 신규: vcvarsall.bat 경유 cmake 빌드 태스크
- `.vscode/c_cpp_properties.json` → 신규: MSVC IntelliSense 설정
- `PYQuant/kis/client.py` → BalanceItem, AccountSummary, get_kr_balance() 추가
- `PYQuant/main.py` → cmd_balance(), balance 서브커맨드 추가
- `Quant/src/main.cpp` → KR_WATCH 관심종목에 제주반도체 추가

### 막힌 지점 / 미해결
- 모의투자 APP Key 갱신 필요 (내일 apiportal에서 처리)
- KIS 모의투자 가상 자금 신청 미완료 (잔고 0원 상태)

### 내일 할 일
- 모의투자 APP Key/Secret 갱신 → config_paper.json 업데이트
- 모의투자 가상 자금 신청 후 `python main.py balance` 정상 확인
- 장 중 모의투자 주문 발생 여부 확인 (09:00~15:30)

---

## 2026-05-21 (목)

### 한 일
- `/review-apply` 슬래시 커맨드로 CODE_REVIEW.md 전체 항목(1순위~3순위) 일괄 반영
- **1순위 🔴**
  - C5: OrderGate dedup 검사를 rate limit 앞으로 이동 (중복 신호가 rate slot 소모 방지)
  - C6: on_accept() SELL 시 포지션 0 미만 클램핑 (현물 공매도 불가)
  - C7: OrderGate 멤버명 snake_case 통일 (order_times_min_, order_times_sec_, last_signal_)
  - P1: BacktestEngine equity 자기상쇄 버그 수정 (_positions dict + 일별 포트폴리오 재계산)
  - P2: MDD를 cash-flow 기준에서 equity 시계열 기준으로 변경
  - P6: ZmqOperator TIMEOUT 시 REQ 소켓 재생성 (EFSM 오염 방지, kill 명령 신뢰성 확보)
  - D1: DbClient insert 필수필드 검증(_require) + try/except 격리 (단건 실패가 루프 중단 방지)
- **2순위 🟡**
  - P3: CostModel dataclass 추가 (수수료 0.015% + 거래세 0.18% + 슬리피지 5bp)
  - P4: Sharpe → 일별 equity 수익률 연환산 (252 거래일 기준)
  - P5: subscriber.py print → logger 전환
  - D2: insert_trade_batch() 추가 (executemany 배치 insert)
  - K1: send_order 반환형 bool → OrderResult(ok, odno, rt_cd, msg_cd, msg1)
  - K2: _mask() 정적 헬퍼 + _headers() appsecret 로깅 금지 docstring
  - C8: OrderRouter reject_reason에 e.what() 포함, 빈 ODNO 구분
  - C9+C10: ZmqBridge drop_count_ atomic 추가 + HEALTH에 drop 필드 + catch 로그
  - C11: Engine 장 시간 체크 localtime → utc_plus_hours(9) gmtime (머신 TZ 무관)
- **3순위 🟢**
  - C12: WS stale 감지 → disconnect → connect 재시도 → 실패 시만 kill switch
  - C13: StrategyBase KisClient* 소유권 계약 주석 명시
  - C1: FEED 모드 SetConsoleOutputCP 중복 제거
  - C3: fmt_price/fmt_qty → fmt_int_comma 공통 헬퍼로 통합
  - C4: utils/Utf8.h 신규 생성 (utf8::display_width/pad_right/trunc) + KR_TEST 람다 위임
  - K3: FID_ORG_ADJ_PRC 수정주가 실측 검증 주석
  - K4: logger 모듈레벨 정의 확인 (이미 반영됨)
- C++ 빌드 13/13 PASS, OrderGate 단위 테스트 9/9 PASS
- 커밋 5개 + push 완료

### 변경 파일
- `Quant/src/risk/OrderGate.cpp` → C5(dedup/rate 순서) + C6(SELL 클램핑) + C7(멤버명 정리)
- `Quant/include/risk/OrderGate.h` → C7 멤버명 snake_case 통일
- `Quant/tests/test_order_gate.cpp` → C5/C6 회귀 테스트 2개 추가 (9/9 PASS)
- `Quant/src/ipc/OrderRouter.cpp` → C8 try/catch + reject_reason 개선
- `Quant/include/ipc/OrderRouter.h` → 카운터명 통일
- `Quant/src/ipc/ZmqBridge.cpp` → C9 drop_count_ increment + C10 catch 로그 + HEALTH drop 필드
- `Quant/include/ipc/ZmqBridge.h` → drop_count_ atomic 멤버 + drop_count() 접근자
- `Quant/src/core/Engine.cpp` → C11 UTC+9 장시간 체크 + C12 WS 재연결 + stop 시퀀스 개선
- `Quant/include/strategy/StrategyBase.h` → C13 포인터 소유권 주석
- `Quant/src/main.cpp` → C1 중복 제거 + C3 fmt_int_comma + C4 Utf8.h 위임
- `Quant/include/utils/Utf8.h` → 신규 생성 (UTF-8 터미널 유틸)
- `PYQuant/backtest/engine.py` → P1 equity 재계산 + P2 MDD + P3 CostModel + P4 Sharpe
- `PYQuant/ipc/operator.py` → P6 소켓 재생성
- `PYQuant/ipc/subscriber.py` → P5 logger 전환
- `PYQuant/db/client.py` → D1 필드 검증/격리 + D2 insert_trade_batch
- `PYQuant/kis/client.py` → K1 OrderResult + K2 _mask + K3 주석 + logger 정비
- `.gitignore` → REVIEW_PROMPT.md / CODE_REVIEW.md / ARCHITECTURE.md 커밋 제외 추가

### 막힌 지점 / 미해결
- C2: main.cpp 990줄 → 모드별 파일 분리 (src/modes/) — 대규모 구조 변경으로 별도 작업 연기
- Python pytest 0개 — BacktestEngine / ZmqOperator / DbClient 단위 테스트 미작성

### 내일 할 일
- 전체 프로젝트 구조 파악 (아키텍처 흐름, 각 모듈 역할 정리)
- C2 main.cpp 모드별 파일 분리 검토
- Python pytest 도입 (BacktestEngine CostModel / DbClient 필드검증 / ZmqOperator TIMEOUT)
