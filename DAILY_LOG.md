# Daily Log — Quant Trading System

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

### 학습 카드 영향
- **P6 반영**: "kill 명령 신뢰성" 약점 해소 → ZMQ EFSM 오염 설명 + 소켓 재생성 패턴 답변 가능
- **D1 반영**: "한 건 실패가 전체를 죽이지 않는 격리" → recorder 루프 생존 보장, 게임서버 패킷 격리 경험과 연결
- **C11 반영**: "UTC 통일, 머신 TZ 무관 설계" → 운영 성숙도 어필 가능
- **C12 반영**: "WS stale 재연결 → 자동 복구" → 장애대응 답변 보강
- **K1+C8 반영**: ODNO/에러코드 수신 → ACCEPTED→FILLED 체결통보(H0STCNI0) 연결의 전제 확보
