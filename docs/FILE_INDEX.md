# 파일 색인

저장소의 추적 파일 전부를 폴더별로 한 줄씩 적은 색인이다. 찾을 때는 Ctrl+F로 파일명이나 낱말을 검색한다. 링크는 이 문서 기준 상대 경로다. 개인 파일(`_private/`)은 `_private/FILE_INDEX.md`에 따로 있다.

이 문서는 `py ../quant-devtools/file_index.py`가 다시 쓴다 — 설명은 이 문서의 줄이 정본이고, 새 파일은 `(설명 필요)`로 들어오니 그 자리에서 채운다. 턴 끝 Stop 훅과 커밋 게이트가 빠진 파일·남은 자리표시자를 잡는다.

## 목차

- [(루트)](#루트) — 10개
- [.vscode](#vscode) — 4개
- [PYQuant](#pyquant) — 101개
- [Quant](#quant) — 172개
- [docs](#docs) — 60개
- [linux_practice](#linux_practice) — 2개
- [research](#research) — 216개
- [scripts](#scripts) — 38개
- [strategies](#strategies) — 31개
- [tools](#tools) — 3개

## (루트)

- [.clang-format](../.clang-format) — 클랭포맷 스타일 설정(Allman 중괄호 강제)
- [.dockerignore](../.dockerignore) — Docker 빌드 제외 목록
- [.env.example](../.env.example) — Docker Compose 환경변수 예시 파일
- [.gitattributes](../.gitattributes) — GitHub 언어 통계에서 대시보드 생성 HTML 제외(linguist-generated)
- [.gitignore](../.gitignore) — 빌드 산출물·시크릿·로그 제외 목록
- [CLAUDE.md](../CLAUDE.md) — 저장소 빌드·설계 가이드 문서
- [CMakeLists.txt](../CMakeLists.txt) — 최상위 CMake 프로젝트 정의
- [CMakePresets.json](../CMakePresets.json) — Windows Ninja/MSVC 빌드 프리셋
- [README.md](../README.md) — 프로젝트 개요·아키텍처 설명
- [docker-compose.yml](../docker-compose.yml) — 엔진·파이썬 서비스 컴포즈 정의

## .vscode

### .vscode/

- [c_cpp_properties.json](../.vscode/c_cpp_properties.json) — VS Code C++ 인텔리센스 설정
- [launch.json](../.vscode/launch.json) — VS Code 디버그 실행 구성
- [settings.json](../.vscode/settings.json) — VS Code 작업공간 설정
- [tasks.json](../.vscode/tasks.json) — VS Code 빌드 태스크 정의

## PYQuant

### PYQuant/

- [Dockerfile](../PYQuant/Dockerfile) — 파이썬 컨테이너 빌드 정의
- [main.py](../PYQuant/main.py) — 파이썬 퀀트 시스템 진입점 CLI
- [requirements.txt](../PYQuant/requirements.txt) — 파이썬 의존 패키지 목록

### PYQuant/.vscode/

- [launch.json](../PYQuant/.vscode/launch.json) — VSCode pytest 디버그 실행 설정

### PYQuant/backtest/

- [__init__.py](../PYQuant/backtest/__init__.py) — 빈 패키지 초기화 파일
- [costs.py](../PYQuant/backtest/costs.py) — 체결 비용 정의 한 소스. CostSpec(수수료·매도세·호가 슬리피지·충격)·fill_result·tick_size·LIVE(원장 OrderGate.cpp와 같은 요율)
- [devscale_replay.py](../PYQuant/backtest/devscale_replay.py) — DevScale 3분봉 리플레이 백테스트 도구
- [engine.py](../PYQuant/backtest/engine.py) — 일봉 백테스트 엔진과 비용모델
- [ledger.py](../PYQuant/backtest/ledger.py) — 백테스트 평단 원장 PositionLedger. 매수 평단·부분 매도 평단 유지·전량 매도 리셋·실현손익을 OrderGate::on_fill_confirmed와 같은 규칙으로
- [metrics.py](../PYQuant/backtest/metrics.py) — 손절·트레일 경로 시뮬레이션과 R배수 지표
- [regime_scorer.py](../PYQuant/backtest/regime_scorer.py) — 국면 스코어러 C++ 미러와 연속화 실험
- [report.py](../PYQuant/backtest/report.py) — 백테스트 결과 콘솔 출력
- [stats.py](../PYQuant/backtest/stats.py) — 판정 통계 한 벌(scipy 없음): 1표본·뉴이-웨스트 t, 블록·일 블록 부트스트랩, Benjamini-Hochberg q, Deflated Sharpe, 순위 IC·분위 스프레드, walk-forward 창, ±1 격자, 필요 표본 수, 성과 요약(metrics.json v2 열 이름). 스터디 13 stats_util 정본

### PYQuant/config/

- [default_universe.json](../PYQuant/config/default_universe.json) — 기본 코스피·코스닥 유니버스 목록
- [strategy_a.json](../PYQuant/config/strategy_a.json) — Strategy A 테마·종목·백테스트 파라미터

### PYQuant/core/

- [__init__.py](../PYQuant/core/__init__.py) — 빈 패키지 초기화 파일
- [logger.py](../PYQuant/core/logger.py) — 콘솔 로거 설정 헬퍼

### PYQuant/dashboard/

- [backfill_live.py](../PYQuant/dashboard/backfill_live.py) — 라이브 매매 기록 대시보드용 요약 생성
- [backfill_series_a.py](../PYQuant/dashboard/backfill_series_a.py) — 계열 A 백테스트 결과 metrics.json 백필
- [build_dashboard.py](../PYQuant/dashboard/build_dashboard.py) — 정적 매매·백테스트 대시보드 HTML 생성기

### PYQuant/data/

- [__init__.py](../PYQuant/data/__init__.py) — 빈 패키지 초기화 파일
- [asof.py](../PYQuant/data/asof.py) — 백테스트 기준일 계산 헬퍼
- [datagokr_source.py](../PYQuant/data/datagokr_source.py) — 공공데이터포털 기반 시세·유니버스 소스
- [index_source.py](../PYQuant/data/index_source.py) — yfinance 기반 지수 일봉 소스
- [keys.py](../PYQuant/data/keys.py) — 외부 API 키 로더 `load_key("dart"|"fred"|"ecos"|"datagokr")`, `_private/keys.json` 한 곳에서만 읽고 값은 어디에도 찍지 않는다(D-103)
- [krx_source.py](../PYQuant/data/krx_source.py) — pykrx 기반 시세·수급·유니버스 소스
- [point_in_time.py](../PYQuant/data/point_in_time.py) — 시점 고정 조인 `as_of_join`: 기준일에 이미 발효된 최신 행만 붙인다(merge_asof 래퍼, 정정 공시는 늦은 published_at 우선, max_age로 옛 값 차단)
- [trend7_codes.json](../PYQuant/data/trend7_codes.json) — 종목명-코드-시장 매핑 목록
- [universe_kospi.py](../PYQuant/data/universe_kospi.py) — 정적 코스피 대형·중형주 유니버스 목록
- [yfinance_source.py](../PYQuant/data/yfinance_source.py) — yfinance 기반 과거 구간 일봉 소스

### PYQuant/db/

- [__init__.py](../PYQuant/db/__init__.py) — 빈 패키지 초기화 파일
- [client.py](../PYQuant/db/client.py) — TimescaleDB 저장 클라이언트
- [schema.sql](../PYQuant/db/schema.sql) — TimescaleDB 테이블 스키마 정의

### PYQuant/features/

- [__init__.py](../PYQuant/features/__init__.py) — 시점 고정 피처 패키지 표시(백테스트·라이브 공용)
- [fundamental.py](../PYQuant/features/fundamental.py) — 재무 팩터 `compute(as_of)`: 유니버스 필터(보통주·유동성·시총·금융 제외) 뒤 PBR·ROE(TTM)·z 복합 점수. 스터디 19(저PBR×고ROE)와 장전 잡이 같은 함수를 쓴다
- [regime_axes.py](../PYQuant/features/regime_axes.py) — 성장·물가·유동성·위험선호 네 축 국면 점수와 노출 배수 `score(as_of)` — 백테스트와 라이브가 같은 함수, `published_at < 결정일` 행만 본다

### PYQuant/ipc/

- [__init__.py](../PYQuant/ipc/__init__.py) — 빈 패키지 초기화 파일
- [operator.py](../PYQuant/ipc/operator.py) — ZMQ로 엔진에 명령 전송하는 클라이언트
- [subscriber.py](../PYQuant/ipc/subscriber.py) — ZMQ로 엔진 이벤트 구독하는 클라이언트

### PYQuant/kis/

- [__init__.py](../PYQuant/kis/__init__.py) — 빈 패키지 초기화 파일
- [client.py](../PYQuant/kis/client.py) — KIS REST API 파이썬 클라이언트
- [endpoints.py](../PYQuant/kis/endpoints.py) — KIS 접속점(모의·실계좌 REST URL, WebSocket 포트) 파이썬 정본, C++ KisEndpoints.h와 짝

### PYQuant/live/

- [__init__.py](../PYQuant/live/__init__.py) — 빈 패키지 초기화 파일
- [forward_trader.py](../PYQuant/live/forward_trader.py) — 검증 전략의 모의계좌 forward 실행기
- [trader.py](../PYQuant/live/trader.py) — REST 폴링 방식 실전 매매 실행기

### PYQuant/naver/

- [__init__.py](../PYQuant/naver/__init__.py) — 빈 패키지 초기화 파일
- [theme.py](../PYQuant/naver/theme.py) — 네이버 증권 테마 조회 모듈

### PYQuant/report/

- [__init__.py](../PYQuant/report/__init__.py) — 빈 패키지 초기화 파일
- [account.py](../PYQuant/report/account.py) — 계좌 원금·수익률 리포트 생성

### PYQuant/strategy/

- [__init__.py](../PYQuant/strategy/__init__.py) — 빈 패키지 초기화 파일
- [base.py](../PYQuant/strategy/base.py) — 전략 베이스 클래스 정의
- [channel_breakout.py](../PYQuant/strategy/channel_breakout.py) — 종목별 채널 돌파 전략
- [cross_momentum.py](../PYQuant/strategy/cross_momentum.py) — 횡단면 모멘텀 전략
- [indicators.py](../PYQuant/strategy/indicators.py) — 정배열·이격도 계산 지표 헬퍼
- [mean_reversion.py](../PYQuant/strategy/mean_reversion.py) — 횡단면 이격도 역추세 전략
- [strategy_a.py](../PYQuant/strategy/strategy_a.py) — 테마주 5일선 눌림목 추종 전략
- [supply_demand_rank.py](../PYQuant/strategy/supply_demand_rank.py) — 수급 순매수 기반 횡단면 랭킹 전략
- [value_contrary.py](../PYQuant/strategy/value_contrary.py) — 저PBR 3일 하락 반전 매수 전략

### PYQuant/tests/

- [__init__.py](../PYQuant/tests/__init__.py) — 빈 패키지 초기화 파일
- [test_adjust_splits.py](../PYQuant/tests/test_adjust_splits.py) — 수정주가 분할 보정 회귀 테스트
- [test_backtest_engine.py](../PYQuant/tests/test_backtest_engine.py) — 백테스트 엔진 리팩터 회귀 테스트
- [test_costs_golden.py](../PYQuant/tests/test_costs_golden.py) — costs·ledger 골든 테스트 10케이스. C++ OrderGate 수식·원장 CSV 실제 행과 0원 오차, 상수는 C++ 소스에서 다시 읽어 대조
- [test_indicators.py](../PYQuant/tests/test_indicators.py) — 지표 함수 pytest 검증
- [test_metrics.py](../PYQuant/tests/test_metrics.py) — 경로 시뮬레이션 손계산 검증
- [test_point_in_time.py](../PYQuant/tests/test_point_in_time.py) — `as_of_join` 테스트 5건: 미래 행 차단·정정 우선·첫 공시 전 결측·max_age·왼쪽 순서 보존
- [test_regime_axes.py](../PYQuant/tests/test_regime_axes.py) — regime_axes 시점 고정 테스트 — as_of 뒤 발표 행을 바꾸거나 지워도 점수가 같은지, 데드밴드·배수 반올림 검증
- [test_regime_scorer.py](../PYQuant/tests/test_regime_scorer.py) — 국면 스코어러 패리티·성질 검증
- [test_stats.py](../PYQuant/tests/test_stats.py) — backtest.stats 검산 14건: 뉴이-웨스트 lag=0 동치, 스터디 17 TRENDX CI·스터디 11 샤프·MDD·스터디 13 t·q 골든 재현, walk-forward 9창/5창, 무작위 점수 IC≈0
- [test_strategy_a.py](../PYQuant/tests/test_strategy_a.py) — Strategy A 필터 로직 pytest 검증

### PYQuant/tools/

- [__init__.py](../PYQuant/tools/__init__.py) — 빈 패키지 초기화 파일
- [check_adjusted.py](../PYQuant/tools/check_adjusted.py) — data.go.kr 수정주가 여부 검증 점검
- [check_datagokr.py](../PYQuant/tools/check_datagokr.py) — DataGoKrSource 인증·조회 확인용 점검
- [check_investor_api.py](../PYQuant/tools/check_investor_api.py) — 수급·일봉 API 가용성 검증 스크립트
- [check_kis_investor.py](../PYQuant/tools/check_kis_investor.py) — KIS 투자자매매동향 TR 깊이 점검
- [check_market_flow.py](../PYQuant/tools/check_market_flow.py) — 시장 수급·프로그램·선물 TR 점검
- [check_pykrx.py](../PYQuant/tools/check_pykrx.py) — pykrx 런타임 데이터 가용성 확인
- [check_pykrx_flow.py](../PYQuant/tools/check_pykrx_flow.py) — pykrx 수급 데이터 검증 스크립트
- [check_sector_index.py](../PYQuant/tools/check_sector_index.py) — 업종 지수 TR 라이브 점검
- [compare_ws_bars.py](../PYQuant/tools/compare_ws_bars.py) — WS 1분봉과 REST 분봉 비교표 생성
- [dart_fin_history_fill.py](../PYQuant/tools/dart_fin_history_fill.py) — DART 주요계정(fnlttMultiAcnt) 2015~ 전 상장사를 100종목 묶음으로 받아 원본·정리본 parquet(PYQuant/data/fin/)에 append-only 적재, 발효일은 rcept_no 앞 8자리
- [dart_shares_history_fill.py](../PYQuant/tools/dart_shares_history_fill.py) — 상장주식수 시점 고정 표 적재: 2015~2019 DART stockTotqySttus 사업보고서(B) + 2020~ data.go.kr 월말 스냅샷(A) → `PYQuant/data/fin/shares_point_in_time.parquet`
- [fetch_naver_themes.py](../PYQuant/tools/fetch_naver_themes.py) — 네이버 테마 스냅샷 수집 도구
- [full_universe_dump.py](../PYQuant/tools/full_universe_dump.py) — KRX 상장 전종목 코드 덤프 도구
- [fullperiod_validate.py](../PYQuant/tools/fullperiod_validate.py) — 시작월 스윕 결론 전기간 재검증
- [index_intraday_logger.py](../PYQuant/tools/index_intraday_logger.py) — 장중 지수 스냅샷 forward 적재 로거
- [investor_flow_logger.py](../PYQuant/tools/investor_flow_logger.py) — 수급 장마감 확정치 forward 적재 로거
- [kind_delisted_fill.py](../PYQuant/tools/kind_delisted_fill.py) — KIND 상장폐지 목록(2000~) 적재·종목코드 붙이기·패널에 없는 상폐사 일봉 보강(--fill-bars)
- [log_report.py](../PYQuant/tools/log_report.py) — quant_trader 로그 운용 리포트 생성기
- [macro_ingest.py](../PYQuant/tools/macro_ingest.py) — FRED(ALFRED 판본, A)·ECOS(B)·관세청 10일 잠정치(B) 거시 시계열을 시점 고정 스키마로 PYQuant/data/macro/<source>_<series>.parquet에 append-only 적재
- [macro_regime_feed.py](../PYQuant/tools/macro_regime_feed.py) — 매크로 지표 기반 국면 게이트 발행기
- [minute_backfill.py](../PYQuant/tools/minute_backfill.py) — 거래일별 1분봉 백필 도구
- [month_start_sweep.py](../PYQuant/tools/month_start_sweep.py) — 매매 시작월 민감도 스윕 도구
- [naver_bars_backfill.py](../PYQuant/tools/naver_bars_backfill.py) — 네이버 siseJson 일봉 1990~ 전량 백필 → bars_all_pit_v2.parquet(v1 스키마 + 외인보유율), 끝에 005930 종가 v1 일치 검사
- [naver_flow_backfill.py](../PYQuant/tools/naver_flow_backfill.py) — 네이버 모바일 trend API 수급 이력(외인·기관·개인 순매수 주식수·외인보유율) 백필 → investor_flow_pit.parquet, 종목별 캐시로 재실행 안전
- [naver_research_fetch.py](../PYQuant/tools/naver_research_fetch.py) — 네이버 증권 리서치 목록·PDF 본문·컨센서스를 받아 PYQuant/data/research/·consensus/에 저장(장중 대시보드 리서치 패널 입력, 등급 B·C)
- [nxt_divergence_check.py](../PYQuant/tools/nxt_divergence_check.py) — KRX·NXT 시세 괴리 측정 도구
- [pit_universe_backfill.py](../PYQuant/tools/pit_universe_backfill.py) — 거래일별 PIT 유니버스 재구성 도구
- [regime_removal_test_2022.py](../PYQuant/tools/regime_removal_test_2022.py) — 2022 폭락장 국면필터 제거실험
- [sweep.py](../PYQuant/tools/sweep.py) — 전략 파라미터 강건성 스윕 도구
- [universe_feed.py](../PYQuant/tools/universe_feed.py) — 시총·거래대금 유니버스 피드 생성기
- [walkforward.py](../PYQuant/tools/walkforward.py) — 전진검증 표본외 성과 검증 도구

## Quant

### Quant/

- [CMakeLists.txt](../Quant/CMakeLists.txt) — 빌드 설정 — C++23, 플랫폼별 컴파일 옵션(ASan/TSan), 타깃 정의
- [Dockerfile](../Quant/Dockerfile) — 리눅스 컨테이너 빌드(2단계: 빌드+런타임) 설정
- [quant_trader.pid](../Quant/quant_trader.pid) — 실행 중 프로세스 PID 파일

### Quant/config/

- [config.json.example](../Quant/config/config.json.example) — 설정 파일 예시(KIS 인증·모드·전략 스펙)
- [etf_name_tokens.json](../Quant/config/etf_name_tokens.json) — 종목명 ETF·ETN 판별용 토큰 목록
- [etf_prefixes.json](../Quant/config/etf_prefixes.json) — ETF 브랜드 접두사 목록(유니버스 필터용)
- [reit_name_suffixes.json](../Quant/config/reit_name_suffixes.json) — 리츠 종목명 접미사 목록
- [reit_names.json](../Quant/config/reit_names.json) — 접미사로 안 걸리는 리츠 종목명 예외 목록
- [ticker_names.json](../Quant/config/ticker_names.json) — bench_ticker_lookup이 쓰는 티커→종목명 표본 20종목
- [us_universe.json](../Quant/config/us_universe.json) — 해외주식(나스닥·뉴욕) PBR 후보 유니버스 폴백 목록

### Quant/include/api/

- [IMarketDataSource.h](../Quant/include/api/IMarketDataSource.h) — 시세·봉 읽기 인터페이스 — 전략·스캔이 KIS 의존 없이 시세 접근(D-066)
- [IOrderExecutor.h](../Quant/include/api/IOrderExecutor.h) — 주문 실행 인터페이스 — OrderAck·OpenOrder 정의(D-039)
- [KisClient.h](../Quant/include/api/KisClient.h) — KIS REST 클라이언트 선언과 KisConfig
- [KisEndpoints.h](../Quant/include/api/KisEndpoints.h) — KIS 접속점(모의·실계좌 REST URL, WebSocket 호스트·포트) 한 곳
- [KisErrorCodes.h](../Quant/include/api/KisErrorCodes.h) — KIS 주문 거부 오류코드 문자열 상수
- [KisRestDecode.h](../Quant/include/api/KisRestDecode.h) — KIS REST JSON 응답 디코드 순수 함수(D-051·D-059)
- [KisResult.h](../Quant/include/api/KisResult.h) — KIS REST 결과 봉투 — 값과 실패를 구분(D-059·D-070)
- [KisTypes.h](../Quant/include/api/KisTypes.h) — KIS 잔고·선물 전광판 응답 값 타입(D-059)
- [KisWebSocket.h](../Quant/include/api/KisWebSocket.h) — KIS 실시간 WebSocket 클라이언트 선언(D-049)
- [KisWsDecode.h](../Quant/include/api/KisWsDecode.h) — KIS 실시간 채널 레코드 디코더 순수 함수(D-037·D-042)

### Quant/include/core/

- [AppConfig.h](../Quant/include/core/AppConfig.h) — config.json을 typed 값으로 옮긴 프로세스 설정 한 벌(`AppConfig`)과 `parse_config` 선언
- [BarAggregator.h](../Quant/include/core/BarAggregator.h) — 체결 틱 → 종목별 N분봉 집계기(D-068·D-072)
- [DataPoller.h](../Quant/include/core/DataPoller.h) — REST 현재가 폴러 — 폴링 모드·WS 폴백(D-062)
- [Engine.h](../Quant/include/core/Engine.h) — 엔진 클래스 선언 — 파이프라인 스레드 배선
- [FeedMux.h](../Quant/include/core/FeedMux.h) — 피드 소스 여러 개를 한 소스로 묶는 mux(D-071)
- [FeedSupervisor.h](../Quant/include/core/FeedSupervisor.h) — WS stale→재연결 백오프→폴백 요구 판정 상태기계(D-071)
- [IFeedSource.h](../Quant/include/core/IFeedSource.h) — 실시간 피드 소스 인터페이스(D-071)
- [KstTime.h](../Quant/include/core/KstTime.h) — UTC → KST 시각 분해 변환 유틸
- [LatencyTrace.h](../Quant/include/core/LatencyTrace.h) — 신호 구간 지연을 CSV로 남기는 기록기
- [LedgerReconciler.h](../Quant/include/core/LedgerReconciler.h) — 브로커 잔고 ↔ 원장 대조기(D-061)
- [MarketSession.h](../Quant/include/core/MarketSession.h) — KRX 정규장 세션 시각 판정
- [MpscQueue.h](../Quant/include/core/MpscQueue.h) — Vyukov MPSC 락프리 큐
- [MutexQueue.h](../Quant/include/core/MutexQueue.h) — 뮤텍스+deque 큐 — MpscQueue 벤치 대조군
- [OrderRateLimiter.h](../Quant/include/core/OrderRateLimiter.h) — 발주 조절기 — 간격·재시도 분류(D-065)
- [PaperExecutor.h](../Quant/include/core/PaperExecutor.h) — 리플레이용 모의 체결기(D-071)
- [ReconcilePlan.h](../Quant/include/core/ReconcilePlan.h) — 잔고 대조 차이 계산 순수 함수(D-038)
- [RegimeFileJudge.h](../Quant/include/core/RegimeFileJudge.h) — 매크로 국면 파일 → 진입정지·강제청산 상태기계(D-060)
- [ReplaySource.h](../Quant/include/core/ReplaySource.h) — 캡처 파일 리플레이 피드 소스(D-071)
- [RingBuffer.h](../Quant/include/core/RingBuffer.h) — SPSC 락프리 링버퍼
- [SessionEndJudge.h](../Quant/include/core/SessionEndJudge.h) — 마감 자기 종료 판정(창 닫힘→유예→큐 비면 종료, D-098)
- [ShardMatrix.h](../Quant/include/core/ShardMatrix.h) — 수신 N×전략 샤드 M SPSC 링 행렬(D-071)
- [ShardRoutes.h](../Quant/include/core/ShardRoutes.h) — 종목 id → 그 종목을 보는 샤드 비트마스크 표(D-110)
- [SignalDispatcher.h](../Quant/include/core/SignalDispatcher.h) — 신호 디스패처 — 순번 stamp·슬롯 교체 판단(D-063)
- [StrategyRouter.h](../Quant/include/core/StrategyRouter.h) — 종목 id → 구독 전략 목록 라우터
- [StrategyShard.h](../Quant/include/core/StrategyShard.h) — 전략 샤드 — 링 행렬 열 하나 소비(D-071)
- [SymbolTable.h](../Quant/include/core/SymbolTable.h) — 종목 문자열 ↔ 정수 id 테이블(D-071)
- [TickCapture.h](../Quant/include/core/TickCapture.h) — 틱·호가 append-only 이진 캡처와 리더(D-071)
- [TickSize.h](../Quant/include/core/TickSize.h) — KRX 호가단위 표
- [Types.h](../Quant/include/core/Types.h) — 핵심 타입 정의 — MarketData·WatchSpec 등
- [UniverseExit.h](../Quant/include/core/UniverseExit.h) — 유니버스 이탈·복귀 판정 순수 함수(D-077)
- [WakeGate.h](../Quant/include/core/WakeGate.h) — 생산자가 소비자를 깨우는 대기 조각(D-071)

### Quant/include/ipc/

- [OpsProtocol.h](../Quant/include/ipc/OpsProtocol.h) — 운영단말 ↔ 엔진 TCP 프레이밍 프로토콜(D-043)
- [OpsServer.h](../Quant/include/ipc/OpsServer.h) — 운영단말 TCP 서버 선언(D-043)
- [OrderRouter.h](../Quant/include/ipc/OrderRouter.h) — 주문 전처리·중계(FEP) 라우팅 레이어 선언
- [ZmqBridge.h](../Quant/include/ipc/ZmqBridge.h) — C++ 엔진 ↔ Python ZMQ IPC 브릿지

### Quant/include/modes/

- [Monitors.h](../Quant/include/modes/Monitors.h) — 관찰용 모니터 모드(FEED 등) 함수 선언

### Quant/include/risk/

- [GateReasons.h](../Quant/include/risk/GateReasons.h) — 게이트 거부 사유 문자열 계약(D-067)
- [OrderGate.h](../Quant/include/risk/OrderGate.h) — 주문 전 위험 검증 게이트

### Quant/include/strategy/

- [DeviationScaleStrategy.h](../Quant/include/strategy/DeviationScaleStrategy.h) — 일봉 정배열+3분봉 이격도 분할매매 전략
- [FixedIntervalStrategy.h](../Quant/include/strategy/FixedIntervalStrategy.h) — 고정 종목 주기 매수/매도 테스트용 전략
- [IntradayBreakoutStrategy.h](../Quant/include/strategy/IntradayBreakoutStrategy.h) — 장중 채널 돌파 전략(ITB v2)
- [MACrossStrategy.h](../Quant/include/strategy/MACrossStrategy.h) — 골든/데드크로스 이평 전략
- [MarketMakingStrategy.h](../Quant/include/strategy/MarketMakingStrategy.h) — 미니 시장조성기(MM-1) 전략
- [MomentumStrategy.h](../Quant/include/strategy/MomentumStrategy.h) — 돈치안 채널 브레이크아웃 전략
- [PriceTargetStrategy.h](../Quant/include/strategy/PriceTargetStrategy.h) — 가격 도달 시장가+예약 지정가 주문 전략
- [SeedPeakStore.h](../Quant/include/strategy/SeedPeakStore.h) — 청산관리 시드분 당일 고점 재기동 간 보존(D-052)
- [StrategyBase.h](../Quant/include/strategy/StrategyBase.h) — 전략 기반 인터페이스
- [StrategyFactory.h](../Quant/include/strategy/StrategyFactory.h) — config strategies 배열 파싱·등록 로더
- [SupplyDemandPullbackStrategy.h](../Quant/include/strategy/SupplyDemandPullbackStrategy.h) — 수급 선별+5일선 눌림목 진입 전략
- [ThemeStrategy.h](../Quant/include/strategy/ThemeStrategy.h) — 3단 필터 테마 모멘텀 전략
- [ValueContraryStrategy.h](../Quant/include/strategy/ValueContraryStrategy.h) — 저PBR 3일 연속 하락 반전 매수 전략

### Quant/include/universe/

- [MaAlign.h](../Quant/include/universe/MaAlign.h) — 일봉 이동평균 정배열 판정 공용 함수(D-005)
- [ScoreWeight.h](../Quant/include/universe/ScoreWeight.h) — 종합 점수 → 종목별 비중 배수 변환
- [UniverseScanner.h](../Quant/include/universe/UniverseScanner.h) — 유니버스 스캐너 — 국면 게이트·랭킹·필터 선언

### Quant/include/utils/

- [EtfFilter.h](../Quant/include/utils/EtfFilter.h) — ETF·ETN 종목명 판별 필터
- [JsonNode.h](../Quant/include/utils/JsonNode.h) — json 하위 노드를 복사 없이 참조로 집어 오는 헬퍼(jsonx::array_or_empty·object_or_empty)
- [Logger.h](../Quant/include/utils/Logger.h) — 비동기 로거 — MPSC 큐+writer 스레드(D-045)
- [Utf8.h](../Quant/include/utils/Utf8.h) — UTF-8 터미널 표시폭 계산·패딩 유틸

### Quant/src/

- [main.cpp](../Quant/src/main.cpp) — 프로그램 진입점 — `main()` 호출 목록이 초기화 순서(콘솔·로거·인자·설정·크래시 핸들러·모드 분기), TRADE는 `run_trade`

### Quant/src/api/

- [KisAccount.cpp](../Quant/src/api/KisAccount.cpp) — 잔고·미체결 조회 구현(연속조회 tr_cont)
- [KisAuth.cpp](../Quant/src/api/KisAuth.cpp) — OAuth2 토큰 발급·캐시·만료 전 재발급 구현(D-073)
- [KisClientInternal.h](../Quant/src/api/KisClientInternal.h) — KisClient 구현 파일 공유 include·상수(D-048)
- [KisIndex.cpp](../Quant/src/api/KisIndex.cpp) — 지수·업종 일봉, 수급, 선물 시세·전광판 구현
- [KisMarket.cpp](../Quant/src/api/KisMarket.cpp) — 국내·해외 주식 일봉·분봉·현재가 구현
- [KisOrder.cpp](../Quant/src/api/KisOrder.cpp) — 주문 발주·정정·취소 및 응답 파서 구현(D-039)
- [KisTransport.cpp](../Quant/src/api/KisTransport.cpp) — 플랫폼별 HTTP 전송·재시도·한도·인증헤더 구현(D-048)
- [KisUniverse.cpp](../Quant/src/api/KisUniverse.cpp) — 유니버스 후보(시총·거래대금·수급) 조회 구현
- [WebSocketClient.cpp](../Quant/src/api/WebSocketClient.cpp) — KIS WS 클라이언트 플랫폼독립부(연결·재연결·백오프)(D-049)
- [WsSocket.h](../Quant/src/api/WsSocket.h) — 플랫폼 소켓 인터페이스 경계(D-049)
- [WsSocketPosix.cpp](../Quant/src/api/WsSocketPosix.cpp) — POSIX 소켓+RFC 6455 프레이밍, libcurl·OpenSSL 구현(D-049)
- [WsSocketWin.cpp](../Quant/src/api/WsSocketWin.cpp) — WinHTTP 소켓 및 Windows 전용 HTTP·AES 구현(D-049)

### Quant/src/core/

- [AppConfig.cpp](../Quant/src/core/AppConfig.cpp) — config.json 읽기의 유일한 자리 — 키 이름·기본값·kis.exchange 검증·매매 창 hhmm→분
- [BarAggregator.cpp](../Quant/src/core/BarAggregator.cpp) — N분봉 집계기 구현(D-068·D-074)
- [DataPoller.cpp](../Quant/src/core/DataPoller.cpp) — REST 현재가 폴러 구현 — 호출 간격·넘침 목록(D-062)
- [Engine.cpp](../Quant/src/core/Engine.cpp) — 엔진 본체 구현 — 생성자·전략 등록·파이프라인
- [EngineConfigure.cpp](../Quant/src/core/EngineConfigure.cpp) — `Engine::configure(const AppConfig&)` — AppConfig 값을 Engine 세터에 옮기는 배선 4단계(채널·국면맵·시세 키·위험 한도)
- [LedgerReconciler.cpp](../Quant/src/core/LedgerReconciler.cpp) — 잔고 대조기 구현 — 원장 부트스트랩(D-061)
- [OrderRateLimiter.cpp](../Quant/src/core/OrderRateLimiter.cpp) — 발주 조절기 구현 — 재시도 분류(D-065)
- [RingBuffer.cpp](../Quant/src/core/RingBuffer.cpp) — 빈 구현 파일 — 템플릿 헤더 전용
- [SignalDispatcher.cpp](../Quant/src/core/SignalDispatcher.cpp) — 신호 디스패처 구현 — 강제청산·한도 정리 신호 생성(D-063)

### Quant/src/ipc/

- [OpsServer.cpp](../Quant/src/ipc/OpsServer.cpp) — 운영단말 TCP 서버 구현(D-043)
- [OrderRouter.cpp](../Quant/src/ipc/OrderRouter.cpp) — 주문 라우터 구현 — 제출·순번·거부코드 처리
- [ZmqBridge.cpp](../Quant/src/ipc/ZmqBridge.cpp) — ZMQ IPC 브릿지 구현 — PUB/REP 소켓

### Quant/src/modes/

- [Monitors.cpp](../Quant/src/modes/Monitors.cpp) — FEED 등 시세 표시 모니터 모드 구현

### Quant/src/risk/

- [OrderGate.cpp](../Quant/src/risk/OrderGate.cpp) — 주문 위험 게이트 구현 — 수수료율·우선순위 바

### Quant/src/strategy/

- [MACrossStrategy.cpp](../Quant/src/strategy/MACrossStrategy.cpp) — 빈 placeholder 파일
- [MomentumStrategy.cpp](../Quant/src/strategy/MomentumStrategy.cpp) — 빈 placeholder 파일
- [StrategyBase.cpp](../Quant/src/strategy/StrategyBase.cpp) — 빈 placeholder 파일
- [StrategyFactory.cpp](../Quant/src/strategy/StrategyFactory.cpp) — 전략 로더 구현 — config 파싱·국면 부착

### Quant/src/universe/

- [UniverseScanner.cpp](../Quant/src/universe/UniverseScanner.cpp) — 유니버스 스캐너 구현 — 정배열 판정 캐시

### Quant/src/utils/

- [Logger.cpp](../Quant/src/utils/Logger.cpp) — 빈 placeholder 파일
- [Timer.cpp](../Quant/src/utils/Timer.cpp) — 빈 placeholder 파일

### Quant/tests/

- [bench_feed_ingest.cpp](../Quant/tests/bench_feed_ingest.cpp) — 시세 피드 수신 부하테스트, TCP loopback 네트워크·처리 구간 분해
- [bench_gate_contention.cpp](../Quant/tests/bench_gate_contention.cpp) — OrderGate 락 경합 벤치(읽기 지연 분포)
- [bench_intake.cpp](../Quant/tests/bench_intake.cpp) — 멀티생산자 주문 인테이크 큐 부하 벤치(MPSC 대 Mutex)
- [bench_latency_path.cpp](../Quant/tests/bench_latency_path.cpp) — 지연에 민감한 경로 리팩터 전후 비교 벤치(D-071)
- [bench_market_firehose.cpp](../Quant/tests/bench_market_firehose.cpp) — 전종목 규모 시세 파이프라인 부하테스트(E2E 지연·처리량)
- [bench_order_gate_position.cpp](../Quant/tests/bench_order_gate_position.cpp) — OrderGate 원장 조회 벤치: 키가 (계좌, 종목) 문자열일 때와 정수 id일 때의 position() 비용(D-105 결정 3)
- [bench_sleep_res.cpp](../Quant/tests/bench_sleep_res.cpp) — sleep_for·condvar 대기 해상도 실측 도구
- [bench_wake_gate.cpp](../Quant/tests/bench_wake_gate.cpp) — WakeGate 대 atomic::wait 깨우기 지연 비교 벤치(D-070)
- [bench_zmq_publish.cpp](../Quant/tests/bench_zmq_publish.cpp) — ZmqBridge::publish_trade가 수신 스레드에 얹는 비용 벤치 + TRADE 와이어 포맷이 예전 dump()와 같은지 검사(ctest)
- [test_account_ledger.cpp](../Quant/tests/test_account_ledger.cpp) — 계좌별 원장 파티셔닝(다계좌 독립성) 단위 테스트
- [test_app_config.cpp](../Quant/tests/test_app_config.cpp) — config.json → AppConfig 경계 단위 테스트(기본값·오버라이드·feed_keys 상속·risk·regime_strategies)
- [test_bar_aggregator.cpp](../Quant/tests/test_bar_aggregator.cpp) — N분봉 집계기 단위 테스트(D-068·D-072)
- [test_data_poller.cpp](../Quant/tests/test_data_poller.cpp) — REST 현재가 폴러 단위 테스트(D-053·D-062)
- [test_engine.cpp](../Quant/tests/test_engine.cpp) — Engine 한 바퀴 단위 테스트(시험용 시세 주입, KIS·소켓 없이 틱→주문→모의 체결→원장, 수신 스레드 1×샤드 1과 2×2, 캡처 파일 리플레이는 KIS 없이)
- [test_feed_mux.cpp](../Quant/tests/test_feed_mux.cpp) — 다중 소켓 피드 묶음(FeedMux) 단위 테스트
- [test_feed_supervisor.cpp](../Quant/tests/test_feed_supervisor.cpp) — WS 피드 감독기 단위 테스트
- [test_kis_decode.cpp](../Quant/tests/test_kis_decode.cpp) — KIS REST 응답 디코더 단위 테스트(분봉·잔고·전광판, D-051·D-059)
- [test_latency_trace.cpp](../Quant/tests/test_latency_trace.cpp) — 구간 지연 CSV 기록기 단위 테스트
- [test_ledger_reconciler.cpp](../Quant/tests/test_ledger_reconciler.cpp) — 잔고-원장 대조기 단위 테스트(D-038·D-061)
- [test_logger.cpp](../Quant/tests/test_logger.cpp) — 비동기 Logger 무손실·flush·드롭 계수 검증(D-045)
- [test_market_session.cpp](../Quant/tests/test_market_session.cpp) — 정규장 시각 판정·KST 시각 분해 단위 테스트(D-037·D-070)
- [test_mpsc.cpp](../Quant/tests/test_mpsc.cpp) — MpscQueue·MutexQueue 정확성 검증
- [test_ops_protocol.cpp](../Quant/tests/test_ops_protocol.cpp) — 운영단말 프레이밍 단위 테스트(D-043)
- [test_ops_server.cpp](../Quant/tests/test_ops_server.cpp) — 운영단말 서버 TCP 왕복 테스트(D-043)
- [test_order_gate.cpp](../Quant/tests/test_order_gate.cpp) — OrderGate 한도·거부 사유 단위 테스트
- [test_order_rate_limiter.cpp](../Quant/tests/test_order_rate_limiter.cpp) — 발주 조절기 단위 테스트(재시도 분류·만기, D-065)
- [test_order_router.cpp](../Quant/tests/test_order_router.cpp) — OrderRouter 통합 테스트(접수·체결·이력)
- [test_paper_executor.cpp](../Quant/tests/test_paper_executor.cpp) — 모의 체결기 단위 테스트(다음틱 체결·취소·정정)
- [test_pipeline_stress.cpp](../Quant/tests/test_pipeline_stress.cpp) — 파이프라인 E2E 부하 테스트(WS수신-전략-주문-체결)
- [test_reconcile_plan.cpp](../Quant/tests/test_reconcile_plan.cpp) — 잔고 대조 차이 계산 순수 함수 단위 테스트(D-038)
- [test_regime_file_judge.cpp](../Quant/tests/test_regime_file_judge.cpp) — 매크로 국면 파일 판정기 단위 테스트(D-033·D-060)
- [test_replay_source.cpp](../Quant/tests/test_replay_source.cpp) — 캡처 리플레이 소스 단위 테스트
- [test_ringbuffer.cpp](../Quant/tests/test_ringbuffer.cpp) — SPSC RingBuffer 정확성·처리량 테스트
- [test_ringbuffer_stress.cpp](../Quant/tests/test_ringbuffer_stress.cpp) — SPSC RingBuffer 실환경 부하 시뮬레이션(버스트·가변지연)
- [test_session_end.cpp](../Quant/tests/test_session_end.cpp) — 마감 자기 종료 판정 단위 테스트(D-098)
- [test_shard_matrix.cpp](../Quant/tests/test_shard_matrix.cpp) — 수신 N×전략 샤드 M 링 행렬 단위 테스트
- [test_signal_dispatcher.cpp](../Quant/tests/test_signal_dispatcher.cpp) — 신호 디스패처 단위 테스트(교체진입·강제청산·유니버스 이탈)
- [test_strategy_router.cpp](../Quant/tests/test_strategy_router.cpp) — 종목 id 전략 라우터 단위 테스트, 틱당 시간 측정
- [test_strategy_shard.cpp](../Quant/tests/test_strategy_shard.cpp) — 전략 샤드 단위 테스트(열 소비 순서·다건 발주)
- [test_symbol_table.cpp](../Quant/tests/test_symbol_table.cpp) — 종목 id 테이블 단위 테스트(부여 순서·동시성)
- [test_tick_capture.cpp](../Quant/tests/test_tick_capture.cpp) — 틱 캡처·리더 왕복·이어쓰기 단위 테스트
- [test_ticker.cpp](../Quant/tests/test_ticker.cpp) — 티커 조회 방식 7가지(std::map·unordered_map·SymbolTable::intern·정수 id 배열·숫자 파싱 희소 배열·틱당 소비자 4곳 모델)를 2,700종목·1천만 회로 재는 벤치(체크섬 출력으로 데드코드 제거를 막는다)
- [test_wake_gate.cpp](../Quant/tests/test_wake_gate.cpp) — WakeGate 소비자 깨우기 단위 테스트
- [test_ws_decode.cpp](../Quant/tests/test_ws_decode.cpp) — KIS 실시간 채널 디코더 단위 테스트(D-037)
- [test_ws_frame.cpp](../Quant/tests/test_ws_frame.cpp) — WS 다건 프레임 분리·분봉 커서 시각 산술 단위 테스트

### Quant/tools/

- [bench_rest_pool.cpp](../Quant/tools/bench_rest_pool.cpp) — REST 커넥션 풀링 효과 측정 벤치
- [check_daily_truncation.py](../Quant/tools/check_daily_truncation.py) — 일봉 당일봉 절단 전후 이동평균 비교 검증 스크립트(D-005)
- [feed_latency_measure.cpp](../Quant/tools/feed_latency_measure.cpp) — 실 KIS WS 다세션 시세 수신 지연 측정 도구
- [future_quote_check.cpp](../Quant/tools/future_quote_check.cpp) — 국내 선물 시세 조회 점검 도구(필드명 확정용)
- [manual_order.cpp](../Quant/tools/manual_order.cpp) — 수동 주문 도구(모의계좌 접수-체결 확인)
- [ops_client.cpp](../Quant/tools/ops_client.cpp) — 운영단말 콘솔 클라이언트(상태·보유 조회·수동주문, D-043)
- [query_balance.py](../Quant/tools/query_balance.py) — 모의계좌 잔고 조회 스크립트(연속조회 포함)

### Quant/tools/ops_terminal/

- [OpsLink.cpp](../Quant/tools/ops_terminal/OpsLink.cpp) — 운영단말 소켓 작업자 스레드 구현(D-043)
- [OpsLink.h](../Quant/tools/ops_terminal/OpsLink.h) — 운영단말 소켓 작업자 헤더(접속·재접속·프레임 송수신)
- [OpsTerminal.cpp](../Quant/tools/ops_terminal/OpsTerminal.cpp) — 운영단말 MFC 앱 진입점(명령행 파싱)
- [OpsTerminal.h](../Quant/tools/ops_terminal/OpsTerminal.h) — 운영단말 MFC 앱 객체 선언
- [OpsTerminal.rc](../Quant/tools/ops_terminal/OpsTerminal.rc) — 운영단말 대화상자 리소스 정의
- [OpsTerminalDlg.cpp](../Quant/tools/ops_terminal/OpsTerminalDlg.cpp) — 운영단말 메인 대화상자 구현(포지션·주문폼·로그)
- [OpsTerminalDlg.h](../Quant/tools/ops_terminal/OpsTerminalDlg.h) — 운영단말 메인 대화상자 선언
- [pch.h](../Quant/tools/ops_terminal/pch.h) — 운영단말 공용 선행 헤더(winsock·MFC)
- [resource.h](../Quant/tools/ops_terminal/resource.h) — 운영단말 리소스 ID 정의

## docs

### docs/

- [AUTOMATION.md](AUTOMATION.md) — 예약 자동화 작업 목록
- [CODE_FLOW.md](CODE_FLOW.md) — 실시간 매매 코드 흐름 읽는 순서(생성물, 심볼 줄 링크·시그니처·테스트, D-078)
- [CODE_GRAPH.md](CODE_GRAPH.md) — 모듈 의존 그래프 자동생성 문서
- [DATA_SOURCES.md](DATA_SOURCES.md) — 데이터 출처 표(data.go.kr·네이버·KIS REST/WS·FDR이 각각 무엇을 얼마나 주는지, 유니버스가 만들어지는 순서, 키)
- [DECISIONS.md](DECISIONS.md) — 설계 결정 원장(D-NNN)
- [DEFERRED_ISSUES.md](DEFERRED_ISSUES.md) — 보류된 코드 이슈 목록
- [ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md) — 엔진 아키텍처 요약(스레드 모델·핵심 타입·국면·KIS·WebSocket·로깅), CLAUDE.md에서 옮김, sync 도장 보유
- [FILE_INDEX.md](FILE_INDEX.md) — 이 파일 — 저장소 전체 파일 한 줄 색인(`../quant-devtools/file_index.py`가 생성)
- [GLOSSARY.md](GLOSSARY.md) — 전략 약어 용어집
- [HARNESS.md](HARNESS.md) — 하네스·루프 엔지니어링 문서
- [OPTIMIZATION_REVIEW.md](OPTIMIZATION_REVIEW.md) — 코드 전수 최적화 리뷰
- [REALTIME_READINESS_REVIEW.md](REALTIME_READINESS_REVIEW.md) — 외부 리뷰 항목 검증 문서
- [RUNBOOK.md](RUNBOOK.md) — 운영 명령 복붙용 정본. gen_runbook.py 가 RUNBOOK.html(gitignore)로 렌더, 절 머리 도장으로 인용 스크립트 변경을 잡는다
- [STYLE_GUIDE.md](STYLE_GUIDE.md) — 문서 문체 규칙집
- [SYNC_MAP.md](SYNC_MAP.md) — 문서 동기화·드리프트 방지 지도
- [code_flow.toml](code_flow.toml) — CODE_FLOW.md의 정본 명세 — 단계·걸음·심볼·볼 것(줄 번호 없음)
- [code_graph.dot](code_graph.dot) — 모듈 의존 그래프(Graphviz)
- [code_graph.json](code_graph.json) — 모듈 의존 그래프(JSON)
- [facts.json](facts.json) — 저장소 사실 자동집계 DB
- [sync_map.json](sync_map.json) — 문서 동기화 대상 매핑(JSON)
- [sync_map.toml](sync_map.toml) — 문서 동기화 대상 매핑(TOML 정본)
- [tuning_sheet.toml](tuning_sheet.toml) — 장중 매매 수치·주기 시트(`_private/TUNING_SHEET.md`)의 정본 명세 — config 키 묶음·단위와 코드 수치(파일·찾기 패턴·뜻), 요약판 `[[cycle]]` 문장. 값은 안 적고 생성기가 소스에서 읽는다

### docs/design/

- [DASHBOARD_SPEC.md](design/DASHBOARD_SPEC.md) — 대시보드 설계 스펙

### docs/guides/

- [AUTOMATION_SCRIPTING_GUIDE.md](guides/AUTOMATION_SCRIPTING_GUIDE.md) — PowerShell·Python 자동화 스크립트를 직접 쓰기 위한 문법·API·설계 패턴 가이드(auto_trade_day·market_close_autodoc·dashboard_server 해부)
- [CODE_GRAPH_GUIDE.md](guides/CODE_GRAPH_GUIDE.md) — 코드 그래프 생성기 사용법
- [CPP20_23_GUIDE.md](guides/CPP20_23_GUIDE.md) — C++20/23 기능 사용 가이드
- [LINUX_SETUP.md](guides/LINUX_SETUP.md) — 리눅스 빌드·실행 설정 가이드
- [LOAD_TEST_GUIDE.md](guides/LOAD_TEST_GUIDE.md) — 부하·지연 테스트 가이드
- [MAINTENANCE_AUTOMATION.md](guides/MAINTENANCE_AUTOMATION.md) — 유지보수 자동화 원칙 문서
- [MFC_TERMINAL.md](guides/MFC_TERMINAL.md) — MFC 운영단말 가이드
- [MULTI_SESSION.md](guides/MULTI_SESSION.md) — 다중 세션 운영 절차 정본(worktree·현황판·머지 큐·교통정리), CLAUDE.md 다중 세션 절의 원본
- [OPS_TERMINAL.md](guides/OPS_TERMINAL.md) — 운영단말 TCP 채널 가이드
- [PIPELINE_A_to_Z.md](guides/PIPELINE_A_to_Z.md) — 코드 파이프라인 추적 문서
- [PROJECT_GUIDE.md](guides/PROJECT_GUIDE.md) — 프로젝트 전반 가이드
- [REGIME_DRILL_GUIDE.md](guides/REGIME_DRILL_GUIDE.md) — 국면 드릴 절차 가이드

### docs/market_close/

- [2026-09-03.md](market_close/2026-09-03.md) — 09-03 매매 사후검토
- [2026-09-04.md](market_close/2026-09-04.md) — 09-04 매매 사후검토
- [2026-09-07.md](market_close/2026-09-07.md) — 09-07 매매 사후검토
- [2026-09-08.md](market_close/2026-09-08.md) — 09-08 매매 사후검토
- [2026-09-09.md](market_close/2026-09-09.md) — 09-09 매매 사후검토
- [2026-09-10.md](market_close/2026-09-10.md) — 09-10 매매 사후검토
- [2026-09-11.md](market_close/2026-09-11.md) — 09-11 매매 사후검토
- [2026-09-14.md](market_close/2026-09-14.md) — 09-14 매매 사후검토
- [2026-09-15.md](market_close/2026-09-15.md) — 09-15 매매 사후검토
- [2026-09-16.md](market_close/2026-09-16.md) — 09-16 매매 사후검토
- [2026-09-17.md](market_close/2026-09-17.md) — 09-17 매매 사후검토
- [2026-09-18.md](market_close/2026-09-18.md) — 09-18 매매 사후검토
- [README.md](market_close/README.md) — 장 마감 리뷰 색인

### docs/premarket/

- [2026-09-04.md](premarket/2026-09-04.md) — 09-04 장전 시황 브리핑
- [2026-09-07.md](premarket/2026-09-07.md) — 09-07 장전 시황 브리핑
- [2026-09-08.md](premarket/2026-09-08.md) — 09-08 장전 시황 브리핑
- [2026-09-09.md](premarket/2026-09-09.md) — 09-09 장전 시황 브리핑
- [2026-09-10.md](premarket/2026-09-10.md) — 09-10 장전 시황 브리핑
- [2026-09-11.md](premarket/2026-09-11.md) — 09-11 장전 시황 브리핑
- [README.md](premarket/README.md) — 장전 브리핑 색인(날짜 결함 설명)
- [ROUTINE_PROMPT.md](premarket/ROUTINE_PROMPT.md) — 장전 시황 브리핑 클라우드 루틴 프롬프트 정본. 국면 모델 표는 gen:regime-model, 올린 해시는 premarket_routine.py --mark

### docs/reports/

- [MAINTENANCE_WEEKLY.md](reports/MAINTENANCE_WEEKLY.md) — 주간 유지보수 현황 보고서
- [MDC_BLOCK_REPORT.md](reports/MDC_BLOCK_REPORT.md) — KRX 데이터 차단 진단 보고서
- [PIPELINE_LATENCY_REPORT.md](reports/PIPELINE_LATENCY_REPORT.md) — 파이프라인 지연 벤치마크 보고서
- [TOKEN_AUDIT.md](reports/TOKEN_AUDIT.md) — 최근 7일 세션 기록의 토큰 사용 감사(절차·도구·하네스 주입·압축·훅별 표), `py scripts/token_audit.py --md`로 다시 만든다

## linux_practice

### linux_practice/

- [dummy_server.c](../linux_practice/dummy_server.c) — 리눅스 연습용 더미 TCP 서버
- [practice.sh](../linux_practice/practice.sh) — 리눅스 프로세스 관리 연습 스크립트

## research

### research/

- [BACKTESTS.md](../research/BACKTESTS.md) — 계열 A/B 백테스트 목록 한눈에 보기 표
- [BACKTEST_FLOW.md](../research/BACKTEST_FLOW.md) — 장중 매매 실증 흐름도(트랙 A/B) 정리
- [BACKTEST_LOG.md](../research/BACKTEST_LOG.md) — 백테스트 실행 저널 누적 기록
- [COUNCIL_CHARTER.md](../research/COUNCIL_CHARTER.md) — 리서치 회의 헌장(D-101): 시니어 기준·PIT 등급제·walk-forward 3층 게이트·기각 전략 라이브 제거
- [GUARDRAILS.md](../research/GUARDRAILS.md) — 백테스트 규율(엔진·데이터 단일소스 등) 문서
- [README.md](../research/README.md) — research 허브(계열 A/B 개요) 문서
- [RESEARCH_COUNCIL.md](../research/RESEARCH_COUNCIL.md) — 리서치 회의 프로토콜과 멤버 역할표
- [RESET_2026-09-19.md](../research/RESET_2026-09-19.md) — 2026-09-19 리셋 회의 결론(D-101): 진단·월요일 config·백테스트 재건·데이터 마트·판정 기준·오너 결정 목록
- [RESET_2026-09-19_BRIEF.md](../research/RESET_2026-09-19_BRIEF.md) — 2026-09-19 리셋 회의 브리핑(오너 지시·백테스트·장중·데이터 현황·외부 진단)

### research/RESET_2026-09-19_R2/

- [README.md](../research/RESET_2026-09-19_R2/README.md) — 리셋 2라운드 종합 색인: 결과 한눈에·데이터 적재 현황·스터디 19/20 판정·공용 코드·D-103·보고 7장·오너 결정 6건·남은 일·명령 모음
- [bias-auditor.md](../research/RESET_2026-09-19_R2/bias-auditor.md) — 리셋 2라운드: 스터디 11·13·15·17 기각 근거(2022 재사용·비용 상수 네 갈래·상폐 커버 B등급·리플레이 체결 낙관)와 새 sim.py·run_spec.py 설계
- [fundamental-quant.md](../research/RESET_2026-09-19_R2/fundamental-quant.md) — 리셋 2라운드: 재무 팩터 후보 3개(저PBR×고ROE·PEAD·GP/A×발생액) 사전등록 스펙과 fin_point_in_time 스키마·features/fundamental.py 계약
- [harness-engineer.md](../research/RESET_2026-09-19_R2/harness-engineer.md) — 리셋 2라운드: 회의가 코드를 못 낸 구조 원인 3개(쓰기 권한·산출물 형식·절차)와 에이전트별 tools·문단 개정안(D-103)
- [macro-quant.md](../research/RESET_2026-09-19_R2/macro-quant.md) — 리셋 2라운드: 거시 네 축 27시리즈 국면 오버레이 사전등록 스펙(z·데드밴드·배수), 발표 이벤트 정지, 스터디 20 파일 계획
- [quant-analyst.md](../research/RESET_2026-09-19_R2/quant-analyst.md) — 리셋 2라운드: 스터디 17개 재판정(살아남은 엣지 0)·라이브 9거래일 왕복 t값·채택 표준 5층(뉴이-웨스트·walk-forward·격자·FDR·부트스트랩)·metrics v2 열
- [risk-behavior.md](../research/RESET_2026-09-19_R2/risk-behavior.md) — 리셋 2라운드: 계좌 손실 5단(D1~M1)·결합 사이징식·심리 편향을 엔진 규칙으로 옮긴 표, A등급 후보(ODNO 미매핑 체결 재기록)
- [strategist.md](../research/RESET_2026-09-19_R2/strategist.md) — 리셋 2라운드: 전략 슬리브 6개(S1 저변동성·S2 수급·S3 PEAD·S4 거시·S5 DEVSCALE 소액·S6 공시 배제)와 TARGET_BASKET 로더·자본 배분 오너 결정 3건

### research/dashboard/

- [live.json](../research/dashboard/live.json) — 라이브 매매일지 링크 모음 데이터
- [reviews.json](../research/dashboard/reviews.json) — 실증 사후검토 데이터

### research/runs/

- [2026-08-07_month-start-sweep.md](../research/runs/2026-08-07_month-start-sweep.md) — 월별 시작시점 스윕 실행 원자료 아카이브

### research/studies/

- [.gitignore](../research/studies/.gitignore) — 스터디 폴더 gitignore
- [READING_NUMBERS.md](../research/studies/READING_NUMBERS.md) — 스터디 숫자 읽는 법: t·walk-forward 창·격자·IC·Calmar·용량의 뜻과 문턱(2.0·3/5·70%)의 출처(계산·관행·고른 값 구분), 가짜 전략 통과 확률 표
- [README.md](../research/studies/README.md) — 폴더형 백테스트 스터디 인덱스 문서
- [_TEMPLATE.md](../research/studies/_TEMPLATE.md) — 백테스트 결과 표준 템플릿
- [index.json](../research/studies/index.json) — 스터디 23건 색인(번호·질문·방법·데이터·결과·판정·왜·후속·파일). 대시보드 "스터디 · 백테스트 결과" 카드의 원천
- [render_studies.py](../research/studies/render_studies.py) — 모멘텀·국면필터 롤링검증(1~5년)/02/03 매매 원장 렌더러 스크립트
- [threshold_check.py](../research/studies/threshold_check.py) — 합격선 검증: 효과 0인 가짜 전략 40만 개로 t ≥ 2.0 통과 비율·격자 최고 칸 문제·창 동전 던지기 확률을 센다(READING_NUMBERS.md의 표)

### research/studies/01_momentum_regime/

- [1y.md](../research/studies/01_momentum_regime/1y.md) — 모멘텀×국면필터 최근 1년 매매 원장
- [2y.md](../research/studies/01_momentum_regime/2y.md) — 모멘텀×국면필터 최근 2년 매매 원장
- [3y.md](../research/studies/01_momentum_regime/3y.md) — 모멘텀×국면필터 최근 3년 매매 원장
- [4y.md](../research/studies/01_momentum_regime/4y.md) — 모멘텀×국면필터 최근 4년 매매 원장
- [5y.md](../research/studies/01_momentum_regime/5y.md) — 모멘텀×국면필터 최근 5년 매매 원장
- [README.md](../research/studies/01_momentum_regime/README.md) — 모멘텀×국면필터 기준선 스터디 요약
- [metrics.json](../research/studies/01_momentum_regime/metrics.json) — 모멘텀·국면필터 롤링검증(1~5년) 지표 데이터

### research/studies/02_vol_target/

- [1y.md](../research/studies/02_vol_target/1y.md) — 변동성 타게팅 사이징 최근 1년 매매 원장
- [2y.md](../research/studies/02_vol_target/2y.md) — 변동성 타게팅 사이징 최근 2년 매매 원장
- [3y.md](../research/studies/02_vol_target/3y.md) — 변동성 타게팅 사이징 최근 3년 매매 원장
- [4y.md](../research/studies/02_vol_target/4y.md) — 변동성 타게팅 사이징 최근 4년 매매 원장
- [5y.md](../research/studies/02_vol_target/5y.md) — 변동성 타게팅 사이징 최근 5년 매매 원장
- [README.md](../research/studies/02_vol_target/README.md) — 변동성 타게팅 사이징 스터디 요약
- [metrics.json](../research/studies/02_vol_target/metrics.json) — 변동성 타게팅 사이징 지표 데이터

### research/studies/03_2022_removal_test/

- [README.md](../research/studies/03_2022_removal_test/README.md) — 2022 약세장 국면필터 ON/OFF 제거실험 요약
- [metrics.json](../research/studies/03_2022_removal_test/metrics.json) — 2022 약세장 국면필터 제거실험 지표 데이터
- [regime_off.md](../research/studies/03_2022_removal_test/regime_off.md) — 국면필터 OFF(모멘텀 단독) 매매 원장
- [regime_on.md](../research/studies/03_2022_removal_test/regime_on.md) — 국면필터 ON 매매 원장(체결 없음)

### research/studies/06_bear_market/

- [.gitignore](../research/studies/06_bear_market/.gitignore) — 하락장 스터디 폴더 gitignore
- [README.md](../research/studies/06_bear_market/README.md) — 하락장 6구간×4전략 백테스트 종합표
- [metrics.json](../research/studies/06_bear_market/metrics.json) — 하락장 유사구간 6구간 비교 지표 데이터
- [render_ledger.py](../research/studies/06_bear_market/render_ledger.py) — trades CSV를 이벤트별 매매 원장으로 만드는 렌더러
- [summary_datagokr.tsv](../research/studies/06_bear_market/summary_datagokr.tsv) — datagokr 소스 window×전략×regime 지표 요약
- [summary_yf.tsv](../research/studies/06_bear_market/summary_yf.tsv) — yfinance 소스 window×전략×regime 지표 요약

### research/studies/06_bear_market/events/

- [README.md](../research/studies/06_bear_market/events/README.md) — 하락장 이벤트별 매매 원장 전체 인덱스

### research/studies/06_bear_market/events/2011euro/

- [README.md](../research/studies/06_bear_market/events/2011euro/README.md) — 2011 유럽재정위기 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2011euro/mean_reversion_off.md) — 2011 유럽위기 역추세 regime OFF 매매 원장
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2011euro/mean_reversion_on.md) — 2011 유럽위기 역추세 regime ON 매매 원장
- [momentum_off.md](../research/studies/06_bear_market/events/2011euro/momentum_off.md) — 2011 유럽위기 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2011euro/momentum_on.md) — 2011 유럽위기 모멘텀 regime ON 매매 원장

### research/studies/06_bear_market/events/2018semi/

- [README.md](../research/studies/06_bear_market/events/2018semi/README.md) — 2018 반도체·미중무역 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2018semi/mean_reversion_off.md) — 2018 반도체 역추세 regime OFF 매매 원장(유일 승자)
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2018semi/mean_reversion_on.md) — 2018 반도체 역추세 regime ON 매매 원장
- [momentum_off.md](../research/studies/06_bear_market/events/2018semi/momentum_off.md) — 2018 반도체 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2018semi/momentum_on.md) — 2018 반도체 모멘텀 regime ON 매매 원장

### research/studies/06_bear_market/events/2020covid/

- [README.md](../research/studies/06_bear_market/events/2020covid/README.md) — 2020 COVID 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2020covid/mean_reversion_off.md) — 2020 COVID 역추세 regime OFF 매매 원장
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2020covid/mean_reversion_on.md) — 2020 COVID 역추세 regime ON 매매 원장
- [momentum_off.md](../research/studies/06_bear_market/events/2020covid/momentum_off.md) — 2020 COVID 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2020covid/momentum_on.md) — 2020 COVID 모멘텀 regime ON 매매 원장

### research/studies/06_bear_market/events/2022bear/

- [README.md](../research/studies/06_bear_market/events/2022bear/README.md) — 2022 금리인상 약세장 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2022bear/mean_reversion_off.md) — 2022 약세장 역추세 regime OFF 매매 원장
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2022bear/mean_reversion_on.md) — 2022 약세장 역추세 regime ON 매매 원장(체결 없음)
- [momentum_off.md](../research/studies/06_bear_market/events/2022bear/momentum_off.md) — 2022 약세장 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2022bear/momentum_on.md) — 2022 약세장 모멘텀 regime ON 매매 원장(체결 없음)

### research/studies/06_bear_market/events/2024blackmon/

- [README.md](../research/studies/06_bear_market/events/2024blackmon/README.md) — 2024 블랙먼데이 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2024blackmon/mean_reversion_off.md) — 2024 블랙먼데이 역추세 regime OFF 매매 원장
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2024blackmon/mean_reversion_on.md) — 2024 블랙먼데이 역추세 regime ON 매매 원장
- [momentum_off.md](../research/studies/06_bear_market/events/2024blackmon/momentum_off.md) — 2024 블랙먼데이 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2024blackmon/momentum_on.md) — 2024 블랙먼데이 모멘텀 regime ON 매매 원장

### research/studies/06_bear_market/events/2026now/

- [README.md](../research/studies/06_bear_market/events/2026now/README.md) — 2026 현재 변동장 4전략 성과표
- [mean_reversion_off.md](../research/studies/06_bear_market/events/2026now/mean_reversion_off.md) — 2026 현재장 역추세 regime OFF 매매 원장
- [mean_reversion_on.md](../research/studies/06_bear_market/events/2026now/mean_reversion_on.md) — 2026 현재장 역추세 regime ON 매매 원장
- [momentum_off.md](../research/studies/06_bear_market/events/2026now/momentum_off.md) — 2026 현재장 모멘텀 regime OFF 매매 원장
- [momentum_on.md](../research/studies/06_bear_market/events/2026now/momentum_on.md) — 2026 현재장 모멘텀 regime ON 매매 원장

### research/studies/07_crisis_regimes/

- [README.md](../research/studies/07_crisis_regimes/README.md) — 위기 레짐 지수레벨 특성화 스터디 요약
- [analyze_crisis_regimes.py](../research/studies/07_crisis_regimes/analyze_crisis_regimes.py) — 위기 레짐 특성화 분석 스크립트
- [summary.tsv](../research/studies/07_crisis_regimes/summary.tsv) — 위기 이벤트 17건 특성화 데이터

### research/studies/08_crisis_response/

- [README.md](../research/studies/08_crisis_response/README.md) — 위기 대응 방어법 인과 백테스트 요약
- [backtest_crisis_response.py](../research/studies/08_crisis_response/backtest_crisis_response.py) — 위기 대응 방어법 5종 백테스트 스크립트
- [metrics.json](../research/studies/08_crisis_response/metrics.json) — 위기 인과 대응 5법 지표 데이터

### research/studies/09_crisis_strategies/

- [.gitignore](../research/studies/09_crisis_strategies/.gitignore) — 위기 전략 스터디 폴더 gitignore
- [README.md](../research/studies/09_crisis_strategies/README.md) — 위기 대응·공세 전략 10종 인과 백테스트 요약
- [SUMMARY_RANKING.md](../research/studies/09_crisis_strategies/SUMMARY_RANKING.md) — 위기 전략 10종 순위 요약본
- [backtest_crisis_strategies.py](../research/studies/09_crisis_strategies/backtest_crisis_strategies.py) — 위기 대응·수익추구 전략 10종 백테스트 오케스트레이션 스크립트
- [bt09_report.py](../research/studies/09_crisis_strategies/bt09_report.py) — 위기 대응·수익추구 전략 10종 평가결과 README 직렬화 스크립트
- [bt09_signals.py](../research/studies/09_crisis_strategies/bt09_signals.py) — 위기 대응·수익추구 전략 10종 정렬·지표·신호 계산 모듈
- [bt09_strategies.py](../research/studies/09_crisis_strategies/bt09_strategies.py) — 위기 대응·수익추구 전략 10종 방어·공세 전략 익스포저 함수 모음
- [gen_trade_journal.py](../research/studies/09_crisis_strategies/gen_trade_journal.py) — 전략×이벤트 매매일지 생성 스크립트
- [journal_by_event.md](../research/studies/09_crisis_strategies/journal_by_event.md) — 대표지수 기준 이벤트별 매매일지
- [journal_by_event_kr.md](../research/studies/09_crisis_strategies/journal_by_event_kr.md) — KODEX 200 기준 이벤트별 매매일지
- [metrics.json](../research/studies/09_crisis_strategies/metrics.json) — 위기 대응·수익추구 전략 10종 지표 데이터

### research/studies/10_regime_scorer/

- [A1_REPORT.md](../research/studies/10_regime_scorer/A1_REPORT.md) — 국면라벨 순열검정·라벨품질 리포트
- [README.md](../research/studies/10_regime_scorer/README.md) — 구조 국면 스코어러 4변형 제거실험 요약
- [a1_permutation.py](../research/studies/10_regime_scorer/a1_permutation.py) — 국면라벨 순열검정 스크립트
- [a1_results.tsv](../research/studies/10_regime_scorer/a1_results.tsv) — 국면라벨 순열검정 결과 데이터
- [ablate.py](../research/studies/10_regime_scorer/ablate.py) — 국면 스코어러 4변형 제거실험 하네스
- [summary.tsv](../research/studies/10_regime_scorer/summary.tsv) — 국면 스코어러 제거실험 결과 요약

### research/studies/11_signal_axes/

- [README.md](../research/studies/11_signal_axes/README.md) — 신호 3축(횡단면·시계열·역추세) 비교 요약
- [channel_breakout_run_meta.json](../research/studies/11_signal_axes/channel_breakout_run_meta.json) — 채널 돌파 실행 메타데이터
- [cross_momentum_run_meta.json](../research/studies/11_signal_axes/cross_momentum_run_meta.json) — 횡단면모멘텀 실행 메타데이터
- [mean_reversion_run_meta.json](../research/studies/11_signal_axes/mean_reversion_run_meta.json) — 단기역추세 실행 메타데이터
- [metrics.json](../research/studies/11_signal_axes/metrics.json) — 신호 3축 나란히 비교 지표 데이터
- [run_channel_breakout.py](../research/studies/11_signal_axes/run_channel_breakout.py) — 채널 돌파 재현 하네스 스크립트
- [run_cross_momentum.py](../research/studies/11_signal_axes/run_cross_momentum.py) — 횡단면모멘텀 재현 하네스 스크립트
- [run_mean_reversion.py](../research/studies/11_signal_axes/run_mean_reversion.py) — 단기역추세 재현 하네스 스크립트

### research/studies/12_base_breakout/

- [HANDOFF.md](../research/studies/12_base_breakout/HANDOFF.md) — 바닥권 진입 타점 스터디 중단 인계 문서
- [README.md](../research/studies/12_base_breakout/README.md) — 급락 후 바닥권 진입 타점 스터디 요약(음성 결과)
- [characterize.py](../research/studies/12_base_breakout/characterize.py) — 18종목 사후선택 전제 검증 스크립트
- [fetch.py](../research/studies/12_base_breakout/fetch.py) — 18종목·지수 데이터 수집 스크립트
- [fetch_universe.py](../research/studies/12_base_breakout/fetch_universe.py) — 전종목 유니버스 데이터 수집 스크립트
- [fetch_universe_long.py](../research/studies/12_base_breakout/fetch_universe_long.py) — 전종목 장기 유니버스 데이터 수집 스크립트
- [forward_record.py](../research/studies/12_base_breakout/forward_record.py) — 바닥 사건 전방 기록기(무매매 로그)
- [run_18.py](../research/studies/12_base_breakout/run_18.py) — 18종목 대상 신호 분석 스크립트
- [run_apply_2026.py](../research/studies/12_base_breakout/run_apply_2026.py) — 2026 신호 적용 분석 스크립트
- [run_combo.py](../research/studies/12_base_breakout/run_combo.py) — 신호 조합 분석 스크립트
- [run_crosssec.py](../research/studies/12_base_breakout/run_crosssec.py) — 횡단면 신호 분석 스크립트
- [run_crosssec26.py](../research/studies/12_base_breakout/run_crosssec26.py) — 2026년 횡단면 신호 분석 스크립트
- [run_crosssec_pit.py](../research/studies/12_base_breakout/run_crosssec_pit.py) — 시점정합 횡단면 신호 분석 스크립트
- [run_excess.py](../research/studies/12_base_breakout/run_excess.py) — 초과수익 분석 스크립트
- [run_filters.py](../research/studies/12_base_breakout/run_filters.py) — 신호 필터 분석 스크립트
- [run_index_timing.py](../research/studies/12_base_breakout/run_index_timing.py) — 지수 타이밍 축 과거 재현성 검정 스크립트
- [run_replicate.py](../research/studies/12_base_breakout/run_replicate.py) — 과거 폭락구간 재현성 검정 스크립트
- [run_replicate2.py](../research/studies/12_base_breakout/run_replicate2.py) — 과거 폭락구간 재현성 검정 스크립트(변형)
- [run_sizing.py](../research/studies/12_base_breakout/run_sizing.py) — 실측 분포 기반 사이징 상한 계산 스크립트
- [run_universe.py](../research/studies/12_base_breakout/run_universe.py) — 전종목 트리거 기저율 측정 스크립트
- [signals.py](../research/studies/12_base_breakout/signals.py) — 바닥다지기 진입 신호 피처 정의 모듈

### research/studies/13_trendx_gate/

- [README.md](../research/studies/13_trendx_gate/README.md) — TRENDX 게이트 하락장 일봉 근사 백테스트 요약
- [gate_pairs.jsonl](../research/studies/13_trendx_gate/gate_pairs.jsonl) — 일자별 게이트 통과 종목 목록 데이터
- [ic_summary.tsv](../research/studies/13_trendx_gate/ic_summary.tsv) — 슬리브별 순위상관(IC) 요약 데이터
- [live_pairs.jsonl](../research/studies/13_trendx_gate/live_pairs.jsonl) — 라이브 게이트 통과 종목 목록 데이터
- [live_pairs_all.jsonl](../research/studies/13_trendx_gate/live_pairs_all.jsonl) — 라이브 전체 후보 종목 목록 데이터
- [replay_live_check.tsv](../research/studies/13_trendx_gate/replay_live_check.tsv) — 리플레이 변형별 월간 체결 검증 데이터
- [replay_live_check_days.tsv](../research/studies/13_trendx_gate/replay_live_check_days.tsv) — 리플레이 변형별 일별 체결 검증 데이터
- [replay_vs_live.tsv](../research/studies/13_trendx_gate/replay_vs_live.tsv) — 리플레이·라이브 체결 대조 데이터
- [results.tsv](../research/studies/13_trendx_gate/results.tsv) — TRENDX 게이트 탐색 격자 결과 데이터
- [results_smaprev.tsv](../research/studies/13_trendx_gate/results_smaprev.tsv) — 전일 확정 SMA 감도 결과 데이터
- [run_score_ic.py](../research/studies/13_trendx_gate/run_score_ic.py) — 스캐너 점수 순위상관 검정 스크립트
- [run_trendx_gate.py](../research/studies/13_trendx_gate/run_trendx_gate.py) — TRENDX 게이트 일봉 근사 백테스트 스크립트
- [stats_util.py](../research/studies/13_trendx_gate/stats_util.py) — `PYQuant/backtest/stats.py`로 넘기는 얇은 층(옛 튜플 인터페이스 유지). 스터디 13·14·16이 쓴다

### research/studies/14_hold_axis/

- [README.md](../research/studies/14_hold_axis/README.md) — DevScale 유지 게이트 정배열 축 완화 스터디 요약
- [paired.tsv](../research/studies/14_hold_axis/paired.tsv) — 1차(현행 브래킷) 짝비교 일별 데이터
- [paired_nobracket.tsv](../research/studies/14_hold_axis/paired_nobracket.tsv) — 축 분리(브래킷 제거) 짝비교 일별 데이터
- [paired_notop_nobracket.tsv](../research/studies/14_hold_axis/paired_notop_nobracket.tsv) — 점수필터 제거 감도 짝비교 일별 데이터
- [paired_tpfirst.tsv](../research/studies/14_hold_axis/paired_tpfirst.tsv) — 익절 우선 가정 짝비교 일별 데이터
- [results.tsv](../research/studies/14_hold_axis/results.tsv) — 1차(현행 브래킷) 구간별 결과 데이터
- [results_nobracket.tsv](../research/studies/14_hold_axis/results_nobracket.tsv) — 축 분리(브래킷 제거) 구간별 결과 데이터
- [results_notop_nobracket.tsv](../research/studies/14_hold_axis/results_notop_nobracket.tsv) — 점수필터 제거 감도 구간별 결과 데이터
- [results_tpfirst.tsv](../research/studies/14_hold_axis/results_tpfirst.tsv) — 익절 우선 가정 구간별 결과 데이터
- [run_hold_axis.py](../research/studies/14_hold_axis/run_hold_axis.py) — 유지 게이트 4변형 짝비교 백테스트 스크립트
- [run_meta.json](../research/studies/14_hold_axis/run_meta.json) — 1차(현행 브래킷) 실행 메타데이터
- [run_meta_nobracket.json](../research/studies/14_hold_axis/run_meta_nobracket.json) — 축 분리(브래킷 제거) 실행 메타데이터
- [run_meta_notop_nobracket.json](../research/studies/14_hold_axis/run_meta_notop_nobracket.json) — 점수필터 제거 감도 실행 메타데이터
- [run_meta_tpfirst.json](../research/studies/14_hold_axis/run_meta_tpfirst.json) — 익절 우선 가정 실행 메타데이터
- [trades_V1_current.csv.gz](../research/studies/14_hold_axis/trades_V1_current.csv.gz) — 1차 V1(현행) 매매 원장 데이터
- [trades_V1_current_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V1_current_nobracket.csv.gz) — 축 분리 V1(현행) 매매 원장 데이터
- [trades_V1_current_notop_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V1_current_notop_nobracket.csv.gz) — 점수필터 제거 V1(현행) 매매 원장 데이터
- [trades_V1_current_tpfirst.csv.gz](../research/studies/14_hold_axis/trades_V1_current_tpfirst.csv.gz) — 익절 우선 V1(현행) 매매 원장 데이터
- [trades_V2_or_today.csv.gz](../research/studies/14_hold_axis/trades_V2_or_today.csv.gz) — 1차 V2(당일봉도 허용) 매매 원장 데이터
- [trades_V2_or_today_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V2_or_today_nobracket.csv.gz) — 축 분리 V2(당일봉도 허용) 매매 원장 데이터
- [trades_V2_or_today_notop_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V2_or_today_notop_nobracket.csv.gz) — 점수필터 제거 V2(당일봉도 허용) 매매 원장 데이터
- [trades_V2_or_today_tpfirst.csv.gz](../research/studies/14_hold_axis/trades_V2_or_today_tpfirst.csv.gz) — 익절 우선 V2(당일봉도 허용) 매매 원장 데이터
- [trades_V3_band_only.csv.gz](../research/studies/14_hold_axis/trades_V3_band_only.csv.gz) — 1차 V3(이격 단독) 매매 원장 데이터
- [trades_V3_band_only_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V3_band_only_nobracket.csv.gz) — 축 분리 V3(이격 단독) 매매 원장 데이터
- [trades_V3_band_only_notop_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V3_band_only_notop_nobracket.csv.gz) — 점수필터 제거 V3(이격 단독) 매매 원장 데이터
- [trades_V3_band_only_tpfirst.csv.gz](../research/studies/14_hold_axis/trades_V3_band_only_tpfirst.csv.gz) — 익절 우선 V3(이격 단독) 매매 원장 데이터
- [trades_V4_today.csv.gz](../research/studies/14_hold_axis/trades_V4_today.csv.gz) — 1차 V4(D-033 되돌림) 매매 원장 데이터
- [trades_V4_today_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V4_today_nobracket.csv.gz) — 축 분리 V4(D-033 되돌림) 매매 원장 데이터
- [trades_V4_today_notop_nobracket.csv.gz](../research/studies/14_hold_axis/trades_V4_today_notop_nobracket.csv.gz) — 점수필터 제거 V4(D-033 되돌림) 매매 원장 데이터
- [trades_V4_today_tpfirst.csv.gz](../research/studies/14_hold_axis/trades_V4_today_tpfirst.csv.gz) — 익절 우선 V4(D-033 되돌림) 매매 원장 데이터

### research/studies/15_impulse_pullback/

- [README.md](../research/studies/15_impulse_pullback/README.md) — 15번 스터디 리포트(임펄스 후 눌림 추격, 사전등록·방법·결과·스윕·진단·재현정보)
- [check_no_lookahead.py](../research/studies/15_impulse_pullback/check_no_lookahead.py) — 15번 하네스 자체 점검(절단 동치·손계산·쿨다운)
- [fetch_pit_panel.py](../research/studies/15_impulse_pullback/fetch_pit_panel.py) — 15번 입력 준비(PIT 유니버스 단면·일봉 패널·지수)
- [run_baseline_control.py](../research/studies/15_impulse_pullback/run_baseline_control.py) — 15번 날짜맞춤 대조군(사건 알파 = 사건 − 같은 날 유니버스 평균)
- [run_impulse_pullback.py](../research/studies/15_impulse_pullback/run_impulse_pullback.py) — 15번 사건 백테스트 하네스(사건 스캔·익일 시가 체결·밴드 3종 스윕)

### research/studies/16_trendx_execution/

- [README.md](../research/studies/16_trendx_execution/README.md) — 16번 요약(ATR 손절·진입 지연 검증 결과, STRATEGIES.md #7·#8 판정)
- [analyze_exec.py](../research/studies/16_trendx_execution/analyze_exec.py) — 16번 3분봉 리플레이 결과 집계(손절 발동률·R분포)
- [atr_stop_results.tsv](../research/studies/16_trendx_execution/atr_stop_results.tsv) — 16번 일봉 근사 ATR 배수별 손절 결과
- [exec_paired.tsv](../research/studies/16_trendx_execution/exec_paired.tsv) — 16번 진입 지연(즉시 vs 2봉 확인) 짝비교 결과
- [exec_replay.tsv](../research/studies/16_trendx_execution/exec_replay.tsv) — 16번 3분봉 손절 리플레이 원장
- [exec_replay_days.tsv](../research/studies/16_trendx_execution/exec_replay_days.tsv) — 16번 3분봉 리플레이 입력 원본(일별)
- [run_atr_stop.py](../research/studies/16_trendx_execution/run_atr_stop.py) — 16번 ATR 손절 백테스트 하네스

### research/studies/17_exit_ev/

- [README.md](../research/studies/17_exit_ev/README.md) — study 17 청산 사유별 조건부 기대값: 사전등록·편향·결과·재현·수정 이력
- [RESULT.md](../research/studies/17_exit_ev/RESULT.md) — exit_ev.py 가 만든 결과 표(묶음 합계·전체·구간 A/B·비용 감도)
- [exit_ev_all.tsv](../research/studies/17_exit_ev/exit_ev_all.tsv) — 전 구간 셀별 통계·CI(생성물)
- [exit_ev_segment_a.tsv](../research/studies/17_exit_ev/exit_ev_segment_a.tsv) — 구간 A(09-08~10) 셀별 통계·CI(생성물)
- [exit_ev_segment_b.tsv](../research/studies/17_exit_ev/exit_ev_segment_b.tsv) — 구간 B(09-11~18) 셀별 통계·CI(생성물)

### research/studies/19_fundamental_factors/

- [PREREG.md](../research/studies/19_fundamental_factors/PREREG.md) — 스터디 19 사전등록: 신호·유니버스·비용 세 벌·walk-forward 5창·합격 숫자·격자 18칸(결과 전 확정)
- [README.md](../research/studies/19_fundamental_factors/README.md) — 스터디 19 결과 표(세 층 판정·비용 감도·walk-forward·격자)와 판정 한 줄. run_pbr_roe.py가 생성
- [metrics.json](../research/studies/19_fundamental_factors/metrics.json) — 스터디 19 판정 숫자(sample_n·holdout·sharpe·mdd·t_stat·walk_forward_windows_positive·trials_prior·cost_level·grade + 격자·IC·데이터 지문)
- [run_pbr_roe.py](../research/studies/19_fundamental_factors/run_pbr_roe.py) — 스터디 19 하네스: 월 리밸 상위 N 동일가중(버퍼 2N)·틱 환산 비용·walk-forward·격자·5분위 IC → metrics.json·csv·README

### research/studies/20_macro_overlay/

- [PREREG.md](../research/studies/20_macro_overlay/PREREG.md) — 스터디 20 사전등록 — 가설·고정 설계·합격 7항목·격자 27셀·스펙과 다르게 한 곳
- [README.md](../research/studies/20_macro_overlay/README.md) — 스터디 20 거시 국면 오버레이 결과 — 코스피 매수 후 보유 대비 낙폭·CAGR·판정표, `macro_apply=false`
- [build_axes.py](../research/studies/20_macro_overlay/build_axes.py) — 스터디 20 네 축 국면 표 생성 — `regime_axes`를 전 기간 평일에 돌려 `out/axes_<cell>.parquet`로 저장
- [metrics.json](../research/studies/20_macro_overlay/metrics.json) — 스터디 20 숫자 정본 — 셀별 CAGR·MDD·Calmar·판정 7항목·연도 창·격자 54런·입력 해시
- [overlay_backtest.py](../research/studies/20_macro_overlay/overlay_backtest.py) — 스터디 20 오버레이 백테스트 — 축 표에서 노출을 만들어 코스피 시가 수익에 곱하고 사전등록 7항목을 판정, `metrics.json` 기록

### research/studies/21_macro_overlay_hold/

- [PREREG.md](../research/studies/21_macro_overlay_hold/PREREG.md) — 스터디 21 사전등록 — 스터디 20 재실행, 배수를 축 구간 변경 시에만 갱신(오너 결정 6), 합격 7항목은 20 그대로
- [README.md](../research/studies/21_macro_overlay_hold/README.md) — 스터디 21 결과 — 비용 12.2%→5.7%인데 기각(같은 2항목만 통과), 원인은 국면 라벨 지연
- [metrics.json](../research/studies/21_macro_overlay_hold/metrics.json) — 스터디 21 숫자 정본 — 셀별 CAGR·MDD·판정 7항목·격자 54런·`scale_update`·입력 해시

### research/studies/22_pbr_roe_value_tilt/

- [PREREG.md](../research/studies/22_pbr_roe_value_tilt/PREREG.md) — 스터디 22 사전등록: 19와 다른 점(중심 N=30·w=0.7·분기 리밸, 격자 27칸)·추가 표 4개 정의·합격 숫자(결과 전 확정)
- [README.md](../research/studies/22_pbr_roe_value_tilt/README.md) — 스터디 22 결과 표(세 층 판정·비용 감도·walk-forward·격자·시총가중·크기 3분위·용량·상폐 보유 기록). run_value_tilt.py가 생성
- [metrics.json](../research/studies/22_pbr_roe_value_tilt/metrics.json) — 스터디 22 판정 숫자(19와 같은 키 + cap_weighted·size_buckets·capacity·delisted_holdings·observation_window)
- [run_value_tilt.py](../research/studies/22_pbr_roe_value_tilt/run_value_tilt.py) — 스터디 22 하네스: 19의 달력·신호·통계를 빌려 쓰고 중심 셀·격자 27칸·walk-forward(리밸 축)·시총가중·크기 분위·용량·상폐 기록을 더한다

### research/studies/23_value_tilt_liquidity_floor/

- [PREREG.md](../research/studies/23_value_tilt_liquidity_floor/PREREG.md) — 스터디 23 사전등록(2026-09-20 16:05, 결과 보기 전) — 22와 다른 점(하한 30억·50억, 7창 2019~2025, 층 ② 6/7), 합격 숫자, 판정 규칙, 숫자의 뜻
- [README.md](../research/studies/23_value_tilt_liquidity_floor/README.md) — 스터디 23 결과(생성) — 하한 30억·50억 두 유니버스 판정 표, 창별 검증, 이웃, 시총가중·크기 3분위·용량, 관찰창. 둘 다 세 층 통과 → 채택 후보
- [metrics.json](../research/studies/23_value_tilt_liquidity_floor/metrics.json) — 스터디 23 합본 지표(생성) — verdict(overall·floor_30·floor_50), 유니버스별 판정·창·이웃·용량 요약, 22 참조값. 대시보드 원천
- [run_liquidity_floor.py](../research/studies/23_value_tilt_liquidity_floor/run_liquidity_floor.py) — 스터디 23 하네스 — 22 하네스를 불러 거래대금 하한·검증 연도·판정 시작·층 ② 문턱만 바꿔 30억·50억을 따로 돌리고 합본 metrics.json·README.md를 쓴다

### research/studies/23_value_tilt_liquidity_floor/floor_30/

- [metrics.json](../research/studies/23_value_tilt_liquidity_floor/floor_30/metrics.json) — 하한 30억 유니버스의 22 형식 전체 지표(생성)

### research/studies/23_value_tilt_liquidity_floor/floor_50/

- [metrics.json](../research/studies/23_value_tilt_liquidity_floor/floor_50/metrics.json) — 하한 50억 유니버스의 22 형식 전체 지표(생성)

## scripts

### scripts/

- [_logdir.py](../scripts/_logdir.py) — 로그·원장 경로 탐색 헬퍼
- [analyze_slot_cost.py](../scripts/analyze_slot_cost.py) — 보유 슬롯 한도 비용 분석 스크립트
- [auto_trade_day.ps1](../scripts/auto_trade_day.ps1) — 일일 자동매매 기동 스크립트
- [auto_trade_guard.ps1](../scripts/auto_trade_guard.ps1) — 자동매매 감시견 스크립트
- [backfill_fills_db.py](../scripts/backfill_fills_db.py) — 과거 체결 원장 CSV를 TimescaleDB fills 테이블에 적재하는 스크립트
- [backfill_studies.py](../scripts/backfill_studies.py) — 스터디 결과 메트릭 백필 스크립트
- [build.sh](../scripts/build.sh) — Docker 이미지 빌드 스크립트
- [build_review_entry.py](../scripts/build_review_entry.py) — 장 마감 리뷰 항목 생성 스크립트
- [build_study_site.py](../scripts/build_study_site.py) — 주식 스터디 리더 사이트 생성 스크립트
- [check_backtest.py](../scripts/check_backtest.py) — 백테스트 재현성 검사 스크립트
- [check_runtime_health.py](../scripts/check_runtime_health.py) — 실행 로그 장애 패턴 검사 스크립트
- [dashboard_server.py](../scripts/dashboard_server.py) — 장중 매매 대시보드 서버(계좌·보유·국면·유니버스·차트·테마·종목 뉴스·증권사 리서치)
- [deploy_guard.py](../scripts/deploy_guard.py) — 매매 창 안 트레이더 exe 교체를 막는 가드(A등급 결함은 --hotfix-a로 통과, D-101 결정 1)
- [exit_ev.py](../scripts/exit_ev.py) — 원장 매도 체결을 odno 레그로 합쳐 청산 사유별 승률·기대값·일블록 부트스트랩 CI·판정을 낸다(study 17)
- [exit_ev_dashboard.py](../scripts/exit_ev_dashboard.py) — study 17 표를 셀별 근거까지 펼치는 정적 HTML 생성기(exit_ev.py 재사용) — 종목명 맵, 규칙 탭(실행 config 값), 백테스트 탭(metrics.json). refresh_dashboard.py 가 매매일 마감 뒤 부른다
- [exit_ev_dashboard_template.html](../scripts/exit_ev_dashboard_template.html) — exit_ev_dashboard.py 가 JSON을 박아 넣는 화면 템플릿
- [extract_swap_what_if.py](../scripts/extract_swap_what_if.py) — 슬롯 교체 가정 비교 표본 추출 스크립트
- [gen_tuning_sheet.py](../scripts/gen_tuning_sheet.py) — 실행 중 config(`_private/_auto_trade_day.json` 의 config)와 `docs/tuning_sheet.toml` 코드 수치로 `_private/TUNING_SHEET.md`(상세)·`_private/TUNING_CYCLE.md`(요약)를 만든다. 주기 표(초 환산 정렬)·시각 표·묶음별 전체 표. `--check`는 낡음·코드 수치 실패면 exit 1, sync-gate가 매 턴 돌리고 감시견 기동·`maintain --daily`도 부른다
- [kill_release.ps1](../scripts/kill_release.ps1) — 킬스위치 해제: `_private/state/kill_today_<날짜>` 표지 파일을 지우고 감시견 상태파일을 옆으로 치워 가드가 5분 안에 감시견을 다시 띄우게 한다(D-098)
- [live_prices_feed.py](../scripts/live_prices_feed.py) — 전종목 실시간 시세 보조 프로세스
- [log_patterns.py](../scripts/log_patterns.py) — 로그 파싱 공용 정규식 모듈
- [logs.sh](../scripts/logs.sh) — Docker 컨테이너 로그 확인 스크립트
- [market_close_autodoc.py](../scripts/market_close_autodoc.py) — 장 마감 매매일지 자동생성 스크립트
- [market_close_collect.py](../scripts/market_close_collect.py) — 장 마감 사실 수집 스크립트
- [market_close_minute_backfill.py](../scripts/market_close_minute_backfill.py) — 장 마감 후 분봉 백필 스크립트
- [market_close_timetable.ps1](../scripts/market_close_timetable.ps1) — 마감 자동화 시간표 정본. 감시견 config의 `kis.is_paper`로 모의(매매 끝 15:30·루틴 16:00대)/실계좌(20:00·루틴 20:30) 시간표를 고르고, 예약작업·감시견이 그대로인지 보거나(`-Apply`로) 맞춘다. cron-gate 훅이 `-Lines`를 읽는다 · `-Mode paper|live`로 config 없이 한 모드 시간표만
- [notify_trades.py](../scripts/notify_trades.py) — 매매 알림 발송 프로세스
- [ops_terminal_shortcut.ps1](../scripts/ops_terminal_shortcut.ps1) — 운영단말 바탕화면 바로가기 `운영단말.lnk`와 사용자 환경변수 `QUANT_OPS_TOKEN`을 만든다. `ops_terminal` 링크 뒤 CMake POST_BUILD가 부르고, worktree 빌드는 건너뛴다
- [parse_quant_log.py](../scripts/parse_quant_log.py) — 매매 로그 파서 스크립트
- [premarket_routine.py](../scripts/premarket_routine.py) — 루틴 프롬프트 본문 출력(--render)·올린 해시 기록(--mark)·정본과 비교(--check, check_docs 가 부른다)
- [quant_procs.ps1](../scripts/quant_procs.ps1) — 실행 프로세스 점검·정리 스크립트
- [refresh_dashboard.py](../scripts/refresh_dashboard.py) — 대시보드·리뷰 재생성 스크립트
- [run_claude_task.ps1](../scripts/run_claude_task.ps1) — 예약작업이 헤드리스 클로드를 부르는 래퍼(cmd 리다이렉션으로 stderr 경고를 rc=1로 만들지 않고 UTF-8 로그에 붙인다)
- [seed_open_orders.py](../scripts/seed_open_orders.py) — 미체결 주문 상태 복구 스크립트
- [start.sh](../scripts/start.sh) — Docker Compose 기동 스크립트
- [stop.sh](../scripts/stop.sh) — Docker Compose 종료 스크립트
- [summarize_trading_day.py](../scripts/summarize_trading_day.py) — 일일 매매 사실 요약 스크립트
- [trade_costs.py](../scripts/trade_costs.py) — 체결 원장(trades_YYYYMMDD.csv)의 날짜별·종목별 매매 비용(수수료·거래세)과 실현손익을 누적 JSON(logs/trade_costs.json)과 표로 낸다(--days, --symbol)

## strategies

### strategies/

- [README.md](../strategies/README.md) — 전략 폴더 구조·현황 색인

### strategies/DeviationScale/

- [EVOLUTION.md](../strategies/DeviationScale/EVOLUTION.md) — DevScale 전략 진화 기록
- [MEETING_2026-09-11_TRENDX.md](../strategies/DeviationScale/MEETING_2026-09-11_TRENDX.md) — TRENDX 전략 검토 회의록
- [MEETING_2026-09-18_RISKON_SLOTS.md](../strategies/DeviationScale/MEETING_2026-09-18_RISKON_SLOTS.md) — 강세장 슬롯·점수·재진입 회의록(재진입 쿨다운·DEVSCALE 물타기 끔·캡 400만)
- [MEETING_2026-09-18_TURNOVER_COST.md](../strategies/DeviationScale/MEETING_2026-09-18_TURNOVER_COST.md) — 잦은 매매 원인 회의록(매도 76%가 비신호 경로, 교체 상한 복원→하루 1회 진입 게이트)

### strategies/DeviationScale/evidence/

- [exit_ev_table.md](../strategies/DeviationScale/evidence/exit_ev_table.md) — 청산 규칙 config 키(실행값) ↔ study 17 표 행 대응과 결론

### strategies/DeviationScale/live/

- [2026-08-10.md](../strategies/DeviationScale/live/2026-08-10.md) — DevScale 08-10 모의매매 일지(체결 0건)
- [2026-08-11.md](../strategies/DeviationScale/live/2026-08-11.md) — DevScale 08-11 모의매매 일지(빈 ODNO 오류)
- [2026-08-12.md](../strategies/DeviationScale/live/2026-08-12.md) — DevScale 08-12 모의매매 일지(진입 0건)
- [2026-08-13.md](../strategies/DeviationScale/live/2026-08-13.md) — DevScale 08-13 모의매매 일지(HTTP 500 재시도)
- [2026-08-14.md](../strategies/DeviationScale/live/2026-08-14.md) — DevScale 08-14 모의매매 일지(토큰 경쟁 크래시)
- [2026-08-18.md](../strategies/DeviationScale/live/2026-08-18.md) — DevScale 08-18 모의매매 일지(유니버스 ETF 편중 수정)
- [2026-08-19.md](../strategies/DeviationScale/live/2026-08-19.md) — DevScale 08-19 모의매매 일지(위험회피 진입차단)
- [2026-08-20.md](../strategies/DeviationScale/live/2026-08-20.md) — DevScale 08-20 모의매매 일지(장마감 청산 실패)
- [2026-08-21.md](../strategies/DeviationScale/live/2026-08-21.md) — DevScale 08-21 모의매매 일지(프로세스 크래시)
- [2026-08-25.md](../strategies/DeviationScale/live/2026-08-25.md) — DevScale 08-25 모의매매 일지(국면 자동선택 전환)
- [2026-09-03.md](../strategies/DeviationScale/live/2026-09-03.md) — DevScale 09-03 모의매매 일지(계좌설정 오류 교정)
- [2026-09-04.md](../strategies/DeviationScale/live/2026-09-04.md) — DevScale 09-04 모의매매 일지(체결통보 WS 연결)
- [2026-09-07.md](../strategies/DeviationScale/live/2026-09-07.md) — DevScale 09-07 모의매매 일지(유니버스 전면개방)
- [2026-09-08.md](../strategies/DeviationScale/live/2026-09-08.md) — DevScale 09-08 모의매매 일지(자동생성)
- [2026-09-09.md](../strategies/DeviationScale/live/2026-09-09.md) — DevScale 09-09 모의매매 일지(자동생성)
- [2026-09-10.md](../strategies/DeviationScale/live/2026-09-10.md) — DevScale 09-10 모의매매 일지(자동생성)
- [2026-09-11.md](../strategies/DeviationScale/live/2026-09-11.md) — DevScale 09-11 모의매매 일지(자동생성)
- [2026-09-14.md](../strategies/DeviationScale/live/2026-09-14.md) — 09-14 라이브 매매일지
- [2026-09-15.md](../strategies/DeviationScale/live/2026-09-15.md) — 09-15 라이브 매매일지
- [2026-09-16.md](../strategies/DeviationScale/live/2026-09-16.md) — 09-16 라이브 매매일지
- [2026-09-17.md](../strategies/DeviationScale/live/2026-09-17.md) — 09-17 라이브 매매일지
- [2026-09-18.md](../strategies/DeviationScale/live/2026-09-18.md) — 09-18 라이브 매매일지

### strategies/DeviationScale/reviews/

- [2026-08-21_postmortem_08-20_08-21.md](../strategies/DeviationScale/reviews/2026-08-21_postmortem_08-20_08-21.md) — 08-20·08-21 크래시·청산실패 사후검토 회의록

### strategies/ITB/

- [SPEC.md](../strategies/ITB/SPEC.md) — ITB v2 전략 확정 스펙 문서

### strategies/ITB/live/

- [2026-08-07.md](../strategies/ITB/live/2026-08-07.md) — ITB 08-07 모의매매 일지(5회 재기동)

## tools

### tools/zone_check/

- [CMakeLists.txt](../tools/zone_check/CMakeLists.txt) — zone_check 도구 빌드 정의
- [README.md](../tools/zone_check/README.md) — zone_check 도구 사용법 문서
- [main.cpp](../tools/zone_check/main.cpp) — 존 판정 재현 단독 실행 파일

