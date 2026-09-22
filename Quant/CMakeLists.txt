cmake_minimum_required(VERSION 3.20)
project(QuantTrader VERSION 1.0.0 LANGUAGES CXX)
 
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
 
# ─── 빌드 타입 기본값 ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
 
# ─── 컴파일 옵션 ──────────────────────────────────────────────────────────
# QUANT_TSAN=ON: Debug를 ThreadSanitizer로 만든다(ASAN과 같이 못 켜므로 ASAN을 뺀다).
# 큐·RCU·락 순서 검증용. -O1은 TSAN 권장 최저 최적화. Linux 전용, MSVC는 지원 없음.
option(QUANT_TSAN "Debug 빌드를 ThreadSanitizer로 (Linux)" OFF)
if(UNIX)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
    if(QUANT_TSAN)
        set(CMAKE_CXX_FLAGS_DEBUG "-g -O1 -fsanitize=thread")
        # 링크 옵션을 Debug에만 건다. 구성 무관으로 걸면 Release에서 계측 없이 런타임만
        # 링크돼 "TSAN 통과"로 오인한다.
        add_link_options($<$<CONFIG:Debug>:-fsanitize=thread>)
    else()
        set(CMAKE_CXX_FLAGS_DEBUG "-g -fsanitize=address")
    endif()
    # GCC 12+는 hardware_destructive_interference_size 사용에 경고를 낸다. 값은 큐 헤더가 64로 고정 분기한다.
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-interference-size)
else()
    # MSVC: UTF-8 소스 인식, deprecated 함수 경고 억제, WinHTTP 최소 대상 버전
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)  # Windows 10+
endif()
 
# ─── 플랫폼 감지 ──────────────────────────────────────────────────────────
if(UNIX AND NOT APPLE)
  message(STATUS "Platform: Linux")
  set(PLATFORM_LINUX TRUE)
else()
  message(STATUS "Platform: Windows")
endif()
 
# ─── 의존성: libcurl (Linux만), WinHTTP는 Windows SDK 내장 ───────────────
# OpenSSL: 체결통보(H0STCNI) AES-256-CBC 복호화 (Windows는 BCrypt/CNG 사용)
if(UNIX)
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)
endif()
 
# ─── 의존성: nlohmann/json (JSON 파싱) ────────────────────────────────────
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found via find_package, using FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(json)
endif()
 
# ─── 의존성: ZeroMQ (선택적) ──────────────────────────────────────────────
# Windows: vcpkg install zeromq  /  Linux: sudo apt install libzmq3-dev
# vcpkg가 만드는 라이브러리 파일명은 libzmq-mt-<버전>.lib처럼 버전이 박혀 있어
# find_library(NAMES zmq libzmq)로는 못 찾는다 — vcpkg 쪽은 CONFIG 패키지로 먼저 찾는다.
find_package(ZeroMQ CONFIG QUIET)
if(ZeroMQ_FOUND)
  set(ZMQ_LIB libzmq)
  set(ZMQ_INC "")
else()
  find_library(ZMQ_LIB  NAMES zmq libzmq)
  find_path   (ZMQ_INC  NAMES zmq.h)
endif()

if(ZeroMQ_FOUND OR (ZMQ_LIB AND ZMQ_INC))
  message(STATUS "ZeroMQ found: ${ZMQ_LIB}")
  add_compile_definitions(HAS_ZMQ)

  # cppzmq — C++ header-only wrapper (zmq.hpp)
  include(FetchContent)
  FetchContent_Declare(cppzmq
    URL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
  )
  set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(cppzmq)

  set(ZMQ_FOUND TRUE)
else()
  message(STATUS "ZeroMQ not found — ZMQ bridge disabled")
  message(STATUS "  Linux  : sudo apt install libzmq3-dev")
  message(STATUS "  Windows: vcpkg install zeromq")
  set(ZMQ_FOUND FALSE)
endif()

# ─── 소스 파일 ────────────────────────────────────────────────────────────
# KisClient 구현은 도메인별 7파일이다(D-048). 도구·벤치 타깃도 이 목록을 그대로 쓴다.
set(KIS_CLIENT_SOURCES
  src/api/KisTransport.cpp
  src/api/KisAuth.cpp
  src/api/KisMarket.cpp
  src/api/KisIndex.cpp
  src/api/KisOrder.cpp
  src/api/KisAccount.cpp
  src/api/KisUniverse.cpp
)

# 실시간 WS 클라이언트. 소켓 계층은 플랫폼당 한 파일만 링크한다(D-049).
set(KIS_WS_SOURCES src/api/WebSocketClient.cpp src/api/KisWebSocket.cpp)
if(WIN32)
  list(APPEND KIS_WS_SOURCES src/api/WsSocketWin.cpp)
else()
  list(APPEND KIS_WS_SOURCES src/api/WsSocketPosix.cpp)
endif()

# 헤더에 선언만 두고 구현은 여기로 내린 단위들(template·constexpr·짧은 접근자만 헤더에 남는다).
#   정적 라이브러리 하나로 묶어 모든 실행 파일이 건다 — 정적 링크라 실제로 부르는 오브젝트만 끌어간다.
set(HEADER_IMPL_SOURCES
  src/strategy/StrategyBase.cpp
  src/api/KisClient.cpp
  src/api/KisRestDecode.cpp
  src/api/KisWsDecode.cpp
  src/core/FeedMux.cpp
  src/core/FeedSupervisor.cpp
  src/core/IFeedSource.cpp
  src/core/KstTime.cpp
  src/core/LatencyTrace.cpp
  src/core/MarketSession.cpp
  src/core/PaperExecutor.cpp
  src/core/ReconcilePlan.cpp
  src/core/RegimeFileJudge.cpp
  src/core/ReplaySource.cpp
  src/core/SessionEndJudge.cpp
  src/core/ShardRoutes.cpp
  src/core/StrategyShard.cpp
  src/core/StrategyTable.cpp
  src/core/SymbolTable.cpp
  src/core/TickCapture.cpp
  src/core/TickSize.cpp
  src/core/Types.cpp
  src/core/UniverseExit.cpp
  src/core/WakeGate.cpp
  src/ipc/FillKey.cpp
  src/ipc/Heartbeat.cpp
  src/ipc/OpsProtocol.cpp
  src/ipc/OrderChannel.cpp
  src/ipc/LedgerSnapshot.cpp
  src/risk/GateReasons.cpp
  src/risk/LedgerJournal.cpp
  src/risk/ProtectiveOrders.cpp
  src/risk/ProtectiveRule.cpp
  src/strategy/DevScaleRules.cpp
  src/strategy/DeviationScaleStrategy.cpp
  src/strategy/FixedIntervalStrategy.cpp
  src/strategy/IntradayBreakoutStrategy.cpp
  src/strategy/MarketMakingStrategy.cpp
  src/strategy/PriceTargetStrategy.cpp
  src/strategy/SeedPeakStore.cpp
  src/strategy/SupplyDemandPullbackStrategy.cpp
  src/strategy/ThemeStrategy.cpp
  src/strategy/ValueContraryStrategy.cpp
  src/universe/MaAlign.cpp
  src/universe/ScoreWeight.cpp
  src/utils/EtfFilter.cpp
  src/utils/JsonNode.cpp
  src/utils/ThreadName.cpp
  src/utils/Utf8.cpp
)

# main.cpp를 뺀 전부가 엔진 라이브러리다 — quant_trader와 test_engine이 같은 오브젝트를 링크한다(D-071).
set(ENGINE_SOURCES
  src/modes/Monitors.cpp
  src/universe/UniverseScanner.cpp
  src/strategy/StrategyFactory.cpp
  src/core/AppConfig.cpp
  src/core/Engine.cpp
  src/core/EngineConfigure.cpp
  src/core/LedgerReconciler.cpp
  src/core/DataPoller.cpp
  src/core/HttpQuoteFeed.cpp
  src/core/BarAggregator.cpp
  src/core/SignalDispatcher.cpp
  src/core/OrderRateLimiter.cpp
  src/core/RingBuffer.cpp
  ${KIS_CLIENT_SOURCES}
  ${KIS_WS_SOURCES}
  src/strategy/MACrossStrategy.cpp
  src/strategy/MomentumStrategy.cpp
  src/strategy/TargetBasketPlan.cpp
  src/strategy/TargetBasketStrategy.cpp
  src/risk/OrderGate.cpp
  src/utils/Logger.cpp
  src/utils/Timer.cpp
)

list(APPEND ENGINE_SOURCES src/ipc/OrderRouter.cpp src/ipc/OpsServer.cpp)

if(ZMQ_FOUND)
  list(APPEND ENGINE_SOURCES src/ipc/ZmqBridge.cpp)
endif()

# ─── 헤더에서 내려온 구현 ────────────────────────────
add_library(quant_header_impl STATIC ${HEADER_IMPL_SOURCES})
target_include_directories(quant_header_impl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quant_header_impl PUBLIC nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(quant_header_impl PUBLIC pthread stdc++fs)
endif()

# ─── 엔진 라이브러리 ────────────────────────────────
add_library(quant_engine STATIC ${ENGINE_SOURCES})

target_include_directories(quant_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(quant_engine PUBLIC quant_header_impl)

# 두 아카이브는 서로를 부른다 — 헤더에서 내려온 DeviationScaleStrategy 몸통이 엔진 소스의 bars::BarAggregator 를
# 쓰고, 엔진은 그 구현들을 쓴다. 순환을 적어 두면 CMake 가 링크 줄에 두 아카이브를 반복해 적는다.
# GNU ld 는 아카이브를 한 번만 훑어 이것이 없으면 뒤에 온 아카이브가 앞 것을 못 본다(MSVC 는 그냥 지나간다). [why D-118]
target_link_libraries(quant_header_impl PUBLIC quant_engine)

if(ZMQ_FOUND)
  target_include_directories(quant_engine PUBLIC ${ZMQ_INC} ${cppzmq_SOURCE_DIR})
endif()

if(UNIX)
    target_link_libraries(quant_engine PUBLIC
      CURL::libcurl
      OpenSSL::Crypto
      nlohmann_json::nlohmann_json
      pthread
      stdc++fs
    )
else()
    # Windows: WinHTTP/BCrypt는 Windows SDK 내장 (별도 설치 불필요)
    target_link_libraries(quant_engine PUBLIC
      nlohmann_json::nlohmann_json
      winhttp
      bcrypt
      ws2_32
      winmm
    )
endif()

if(ZMQ_FOUND)
  target_link_libraries(quant_engine PUBLIC ${ZMQ_LIB})
endif()

# ─── 실행 파일 ──────────────────────────────────────
add_executable(quant_trader src/main.cpp)
target_link_libraries(quant_trader PRIVATE quant_engine)
set_target_properties(quant_trader PROPERTIES
  VS_DEBUGGER_COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}/config/config_dev_paper.json"
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
  target_link_libraries(quant_trader PRIVATE dbghelp)   # 비정상 종료 미니덤프(main.cpp on_seh)
endif()
 
# ─── config 파일 처리 (파일이 없으면 스킵 — Docker 빌드 시 volume으로 주입) ──
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json)
  # IDE(솔루션 탐색기)에 표시
  target_sources(quant_trader PRIVATE config/config.json)
  set_source_files_properties(config/config.json PROPERTIES HEADER_FILE_ONLY TRUE)
  source_group("config" FILES config/config.json)

  # 빌드 후 바이너리 옆에 자동 복사
  add_custom_command(TARGET quant_trader POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:quant_trader>/config
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json
          $<TARGET_FILE_DIR:quant_trader>/config/config.json
  )
endif()

# ─── 설치 ──────────────────────────────────────────────────────────────────
install(TARGETS quant_trader DESTINATION bin)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json)
  install(FILES config/config.json DESTINATION etc/quant_trader)
endif()

# ─── 테스트 타겟: OrderGate 단위 테스트 ──────────────────────────────────
add_executable(test_order_gate tests/test_order_gate.cpp src/risk/OrderGate.cpp src/ipc/LedgerSnapshot.cpp)
target_include_directories(test_order_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_order_gate PRIVATE pthread)
endif()

# ─── 벤치: OrderGate 원장 조회 — 문자열 키 대 정수 키 (D-105 결정 3) ──────────
add_executable(bench_order_gate_position tests/bench_order_gate_position.cpp src/risk/OrderGate.cpp)
target_include_directories(bench_order_gate_position PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_order_gate_position PRIVATE pthread)
endif()
add_test(NAME bench_order_gate_position COMMAND bench_order_gate_position)
set_tests_properties(bench_order_gate_position PROPERTIES TIMEOUT 60)

# ─── 벤치: 주문 경로 문자열 키 — check() 중복 키·진입 우선순위 표·전략 서브원장, 정수 키 전환 전후 ──
add_executable(bench_order_path_keys tests/bench_order_path_keys.cpp src/risk/OrderGate.cpp)
target_include_directories(bench_order_path_keys PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_order_path_keys PRIVATE pthread)
endif()
add_test(NAME bench_order_path_keys COMMAND bench_order_path_keys)
set_tests_properties(bench_order_path_keys PROPERTIES TIMEOUT 120)

# ─── 테스트 타겟: OrderRouter 통합 테스트 (StubOrderExecutor, KIS 불필요) ──
add_executable(test_order_router
    tests/test_order_router.cpp
    src/risk/OrderGate.cpp
    src/ipc/OrderRouter.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_order_router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(ZMQ_FOUND)
  # OrderRouter.cpp가 ZmqBridge.h를 포함하고 publish_order/publish_fill을 호출한다 —
  # quant_engine과 링크하지 않고 소스를 직접 컴파일하므로 ZmqBridge.cpp 본체·include 경로·링크를
  # 별도로 받아야 한다. vcpkg CONFIG 경로는 ZMQ_INC가 비고 zmq.h 경로를 libzmq 타깃이 전이적으로
  # 실어주므로 링크까지 해야 zmq.h를 찾는다.
  target_sources(test_order_router PRIVATE src/ipc/ZmqBridge.cpp)
  target_include_directories(test_order_router PRIVATE ${ZMQ_INC} ${cppzmq_SOURCE_DIR})
  target_link_libraries(test_order_router PRIVATE ${ZMQ_LIB} nlohmann_json::nlohmann_json)
endif()
if(UNIX)
    target_link_libraries(test_order_router PRIVATE pthread)
endif()

# ─── 테스트 타겟: WS 다건 프레임 분리·분봉 커서 시각 산술 (헤더만, KIS 불필요) ──
add_executable(test_ws_frame tests/test_ws_frame.cpp)
target_include_directories(test_ws_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_ws_frame PRIVATE nlohmann_json::nlohmann_json)

# WS 채널 디코더(api/KisWsDecode.h)와 정규장 시각 판정 — 헤더 전용이라 링크 없음
add_executable(test_ws_decode tests/test_ws_decode.cpp)
target_include_directories(test_ws_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# KIS REST 분봉 디코더(api/KisRestDecode.h) — 헤더 전용, nlohmann만 (D-051)
add_executable(test_kis_decode tests/test_kis_decode.cpp)
target_include_directories(test_kis_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_kis_decode PRIVATE nlohmann_json::nlohmann_json)

# 운영단말 서버 소켓 왕복 — 실제 TCP 루프백(17100), Logger 링크
add_executable(test_ops_server tests/test_ops_server.cpp src/ipc/OpsServer.cpp src/utils/Logger.cpp)
target_include_directories(test_ops_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_ops_server PRIVATE nlohmann_json::nlohmann_json pthread)
else()
    target_link_libraries(test_ops_server PRIVATE nlohmann_json::nlohmann_json ws2_32)
endif()

# 운영단말 프레이밍(ipc/OpsProtocol.h) — 헤더 전용, 부분 수신·규약 위반 케이스
add_executable(test_ops_protocol tests/test_ops_protocol.cpp)
target_include_directories(test_ops_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(test_market_session tests/test_market_session.cpp)
target_include_directories(test_market_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 잔고 대조 차이 계산(core/ReconcilePlan.h) — 헤더 전용, 링크 없음 (D-038)
add_executable(test_reconcile_plan tests/test_reconcile_plan.cpp)
target_include_directories(test_reconcile_plan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 잔고 → 원장 대조기(core/LedgerReconciler) — 브로커는 std::function 대역, OrderGate·Logger 링크 (D-061)
add_executable(test_ledger_reconciler
    tests/test_ledger_reconciler.cpp
    src/core/LedgerReconciler.cpp
    src/risk/OrderGate.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_ledger_reconciler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_ledger_reconciler PRIVATE pthread)
endif()

# REST 현재가 폴러(core/DataPoller) — 브로커·큐·WS 재구독은 std::function 대역, Logger만 링크 (D-062)
add_executable(test_data_poller
    tests/test_data_poller.cpp
    src/core/DataPoller.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_data_poller PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_data_poller PRIVATE pthread)
endif()

# 신호 디스패처(core/SignalDispatcher) — 주문 큐·ZMQ·종목 표기는 std::function 대역, OrderGate·Logger 링크 (D-063)
add_executable(test_signal_dispatcher
    tests/test_signal_dispatcher.cpp
    src/core/SignalDispatcher.cpp
    src/risk/OrderGate.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_signal_dispatcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_signal_dispatcher PRIVATE pthread)
endif()

# N분봉 집계기(core/BarAggregator) — 틱을 봉으로 모으는 순수 조각, 의존 없음 (D-068)
add_executable(test_bar_aggregator
    tests/test_bar_aggregator.cpp
    src/core/BarAggregator.cpp
)
target_include_directories(test_bar_aggregator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 발주 조절기(core/OrderRateLimiter) — 보유 수량 조회는 std::function 대역, Logger만 링크 (D-065)
add_executable(test_order_rate_limiter
    tests/test_order_rate_limiter.cpp
    src/core/OrderRateLimiter.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_order_rate_limiter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_order_rate_limiter PRIVATE pthread)
endif()

# 매크로 국면 파일 전달(core/RegimeFileJudge.h) — 헤더 전용, 파일·로그 없음 (D-060)
add_executable(test_devscale_rules tests/test_devscale_rules.cpp)
target_include_directories(test_devscale_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(test_regime_file_judge tests/test_regime_file_judge.cpp)
target_include_directories(test_regime_file_judge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_regime_file_judge PRIVATE nlohmann_json::nlohmann_json)

# 목표 비중표 바스켓(strategy/TargetBasketPlan) — 파일 파싱·검증과 "목표 − 보유 = 주문" 계산, 순수 조각 (D-109)
add_executable(test_target_basket_plan tests/test_target_basket_plan.cpp src/strategy/TargetBasketPlan.cpp)
target_include_directories(test_target_basket_plan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_target_basket_plan PRIVATE nlohmann_json::nlohmann_json)

# 바스켓 전략 본체 — 원장·시각을 주입해 집행 창·두 레그·하루 한 번 규칙을 본다. Logger를 링크한다 (D-109)
add_executable(test_target_basket_strategy
    tests/test_target_basket_strategy.cpp
    src/strategy/TargetBasketStrategy.cpp
    src/strategy/TargetBasketPlan.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_target_basket_strategy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_target_basket_strategy PRIVATE nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(test_target_basket_strategy PRIVATE pthread)
endif()

# ─── 테스트 타겟: SPSC RingBuffer 검증 ───────────────────────────────────
add_executable(test_ringbuffer tests/test_ringbuffer.cpp)
target_include_directories(test_ringbuffer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_ringbuffer PRIVATE pthread)
endif()

# ─── 테스트 타겟: RingBuffer 부하/안정성 시뮬레이션 ──────────────────────
add_executable(test_ringbuffer_stress tests/test_ringbuffer_stress.cpp)
target_include_directories(test_ringbuffer_stress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_ringbuffer_stress PRIVATE pthread)
endif()

# ─── 테스트 타겟: E2E 파이프라인 부하 시뮬레이션 ─────────────────────────
add_executable(test_pipeline_stress tests/test_pipeline_stress.cpp)
target_include_directories(test_pipeline_stress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_pipeline_stress PRIVATE pthread)
endif()

# ─── 벤치 타겟: 티커 조회 방식별 성능 비교 (map vs unordered_map vs flat array) ──
add_executable(bench_ticker_lookup tests/test_ticker.cpp)
target_include_directories(bench_ticker_lookup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 벤치 타겟: ZmqBridge::publish_trade가 수신 스레드에 얹는 비용 (원칙 3 전후 측정) ──
if(ZMQ_FOUND)
  add_executable(bench_zmq_publish tests/bench_zmq_publish.cpp src/ipc/ZmqBridge.cpp src/utils/Logger.cpp)
  target_include_directories(bench_zmq_publish PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${ZMQ_INC} ${cppzmq_SOURCE_DIR})
  target_link_libraries(bench_zmq_publish PRIVATE ${ZMQ_LIB} nlohmann_json::nlohmann_json)
  # 벤치이지만 와이어 포맷 검사(예전 dump()와 같은 문자열)를 겸해 ctest에도 건다. 아래 픽스처 블록보다 앞이라
  #  픽스처 없이 등록한다 — 로그 폴더를 쓰지 않는다.
  add_test(NAME bench_zmq_publish COMMAND bench_zmq_publish)
  set_tests_properties(bench_zmq_publish PROPERTIES TIMEOUT 60)
endif()

# ─── 벤치 타겟: 전종목 규모 시세 파이프라인 부하 (market firehose) ────────
add_executable(bench_market_firehose tests/bench_market_firehose.cpp)
target_include_directories(bench_market_firehose PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_market_firehose PRIVATE pthread)
endif()

# ─── 벤치 타겟: 평가 1회가 스냅샷을 잡는 비용 — 벡터 복사 vs 포인터 교체 (D-114) ──
add_executable(bench_snapshot_swap tests/bench_snapshot_swap.cpp)
target_include_directories(bench_snapshot_swap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 벤치 타겟: 프리페치 풀이 작업 N개를 주기 T로 버티는가 — 스레드 수 스윕 (D-115) ──
add_executable(bench_prefetch_pool tests/bench_prefetch_pool.cpp)
target_include_directories(bench_prefetch_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_prefetch_pool PRIVATE pthread)
endif()

# ─── 벤치 타겟: 시세 피드 수신·처리 부하 (TCP loopback, KOSCOM→broker) ────
add_executable(bench_feed_ingest tests/bench_feed_ingest.cpp)
target_include_directories(bench_feed_ingest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_feed_ingest PRIVATE pthread)
else()
    target_link_libraries(bench_feed_ingest PRIVATE ws2_32)
endif()

# ─── 벤치 타겟: 프로세스 경계 고장 실측 (공유메모리 전달·죽음 감지, D-071 큐 34) ────
add_executable(bench_peer_failure tests/bench_peer_failure.cpp)
target_include_directories(bench_peer_failure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_peer_failure PRIVATE pthread rt)
endif()

# ─── 테스트 타겟: WakeGate 깨우기 유실·만기 (헤더 전용, D-071) ──
add_executable(test_wake_gate tests/test_wake_gate.cpp)
target_include_directories(test_wake_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_wake_gate PRIVATE pthread)
endif()

# ─── 테스트 타겟: SymbolTable id 부여·상한·동시 등록 (헤더 전용, D-071) ──
add_executable(test_symbol_table tests/test_symbol_table.cpp)
target_include_directories(test_symbol_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_symbol_table PRIVATE pthread)
endif()

# ─── 테스트 타겟: TickCapture 왕복·이어 쓰기·잘린 꼬리·큐 넘침 (헤더 전용, D-071) ──
# ─── 테스트 타겟: prefetch::Pool 고정 스레드·등록·해제·정지 (헤더 전용, D-071) ──
add_executable(test_prefetch_pool tests/test_prefetch_pool.cpp)
target_include_directories(test_prefetch_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_prefetch_pool PRIVATE pthread)
endif()

add_executable(test_tick_capture tests/test_tick_capture.cpp)
target_include_directories(test_tick_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_tick_capture PRIVATE pthread)
endif()

# ─── 테스트 타겟: ReplaySource 순서 보존·종목 필터·속도·도중 정지 (헤더 전용, D-071) ──
add_executable(test_replay_source tests/test_replay_source.cpp)
target_include_directories(test_replay_source PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_replay_source PRIVATE pthread)
endif()

# ─── 테스트 타겟: PaperExecutor 다음 틱 체결·지정가 대기·한도 거부·취소/정정 (헤더 전용, D-071) ──
add_executable(test_paper_executor tests/test_paper_executor.cpp)
target_include_directories(test_paper_executor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 테스트 타겟: FeedMux 종목 배정·소스별 스레드 → mux 스레드 하나·증분/넘침·체결통보 첫 소스 (헤더 전용, D-071) ──
add_executable(test_feed_mux tests/test_feed_mux.cpp)
target_include_directories(test_feed_mux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_feed_mux PRIVATE pthread)
endif()

# ─── 테스트 타깃: Engine 한 바퀴 — 시험용 시세를 주입해 KIS·소켓 없이 틱→샤드→전략→주문→모의 체결→원장을 돈다, 수신 스레드 1×샤드 1과 2×2 (D-071) ──
add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE quant_engine)

# ─── 벤치 타깃: 전 종목 부하 — 합성 체결 2,700종목을 진짜 Engine에 밀어넣고 수신 스레드 N × 전략 샤드 M을 쓸어 잰다 (D-071 원칙 7) ──
#     ctest에 붙이지 않는다 — 수십 초씩 코어를 다 쓴다. 부하 하네스의 관례다(bench_market_firehose와 같다).
add_executable(bench_engine_load tests/bench_engine_load.cpp)
target_link_libraries(bench_engine_load PRIVATE quant_engine)

# ─── 테스트 타깃: config.json → AppConfig 경계 — 키 이름·기본값·kis.exchange 검증·매매 창 hhmm→분 (json 읽기는 이 한 곳) ──
add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE quant_engine)

# ─── 테스트 타깃: WS 피드 감독기 — stale→재연결 백오프(step×n, 상한)→폴백 요구 한 번·복귀 재무장 (헤더 전용, D-071) ──
add_executable(test_feed_supervisor tests/test_feed_supervisor.cpp)
target_include_directories(test_feed_supervisor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 테스트 타깃: 마감 자기 종료 판정 — 창 닫힘→유예→큐 비면 종료, 배출 한도 강제, 늦은 기동 무판정 (헤더 전용, D-098) ──
add_executable(test_session_end tests/test_session_end.cpp)
target_include_directories(test_session_end PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 테스트 타겟: 수신 N × 전략 샤드 M SPSC 행렬(종목 해시 샤딩·라운드로빈·종목 안 순서) + 처리량 측정 (헤더 전용, D-071) ──
add_executable(test_shard_matrix tests/test_shard_matrix.cpp)
target_include_directories(test_shard_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_shard_matrix PRIVATE pthread)
endif()

# ─── 테스트 타겟: StrategyShard 열 비우기·샤드별 전략 집합 라우팅·샤드 1/2/4 측정 (헤더 전용, D-071) ──
add_executable(test_strategy_shard tests/test_strategy_shard.cpp)
target_include_directories(test_strategy_shard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_strategy_shard PRIVATE pthread)
endif()

# ─── 테스트 타겟: 종목 id → 전략 라우팅(전부 받는 전략·테이블 가득 참·재구성) + 틱당 시간 측정 (헤더 전용, D-071) ──
add_executable(test_strategy_router tests/test_strategy_router.cpp)
target_include_directories(test_strategy_router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 테스트 타겟: LatencyTrace 구간 계산·CSV 행·머리글 1회 (헤더 전용, D-071) ──
add_executable(test_latency_trace tests/test_latency_trace.cpp)
target_include_directories(test_latency_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── 테스트 타겟: MPSC/Mutex 큐 정확성 검증 (멀티클라이언트 인테이크 D1) ──
add_executable(test_mpsc tests/test_mpsc.cpp)
target_include_directories(test_mpsc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_mpsc PRIVATE pthread)
endif()

# ─── 벤치 타겟: 멀티클라이언트 주문 인테이크 부하 (D2) ────────────────────
add_executable(bench_intake tests/bench_intake.cpp)
target_include_directories(bench_intake PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_intake PRIVATE pthread)
endif()

# ─── 벤치 타겟: 유휴 대기 해상도 (sleep_for·condvar, Windows 타이머 격자, D-071 Phase 0) ────
add_executable(bench_sleep_res tests/bench_sleep_res.cpp)
if(WIN32)
    target_link_libraries(bench_sleep_res PRIVATE winmm)
elseif(UNIX)
    target_link_libraries(bench_sleep_res PRIVATE pthread)
endif()

# ─── 벤치 타겟: 소비자 깨우기 — WakeGate condvar vs atomic::wait (D-070 7단계) ────
add_executable(bench_wake_gate tests/bench_wake_gate.cpp)
target_include_directories(bench_wake_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_wake_gate PRIVATE pthread)
endif()

# ─── 벤치 타겟: 09-13 hot path 조각 항목별 옛/새 비교 (D-071 측정 근거, 헤더 전용) ────
add_executable(bench_latency_path tests/bench_latency_path.cpp)
target_include_directories(bench_latency_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_latency_path PRIVATE pthread)
endif()

# ─── 벤치 타겟: OrderGate 원장 읽기 vs 쓰기 스레드 경합 (감사 L4, D-058) ────
add_executable(bench_gate_contention tests/bench_gate_contention.cpp src/risk/OrderGate.cpp)
target_include_directories(bench_gate_contention PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_gate_contention PRIVATE pthread)
endif()

# ─── 테스트 타겟: 계좌별 원장 파티셔닝 검증 (D3) ──────────────────────────
add_executable(test_account_ledger tests/test_account_ledger.cpp src/risk/OrderGate.cpp)
target_include_directories(test_account_ledger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_account_ledger PRIVATE pthread)
endif()

# ─── 테스트 타겟: 보호 주문 표 검증 (D-114 단계 1) ─────────────
add_executable(test_protective_orders tests/test_protective_orders.cpp src/risk/OrderGate.cpp src/utils/Logger.cpp)
target_include_directories(test_protective_orders PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_protective_orders PRIVATE pthread)
endif()

# ─── 테스트 타겟: 전략↔주문 요청·응답 통로 (D-114 단계 2) ─────────────
add_executable(test_order_channel tests/test_order_channel.cpp src/ipc/OrderChannel.cpp src/core/Types.cpp)
target_include_directories(test_order_channel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_order_channel PRIVATE pthread)
endif()

# ─── 테스트 타겟: 장부 사본 (D-114 단계 2.5) ──────────────────────
add_executable(test_ledger_snapshot tests/test_ledger_snapshot.cpp src/ipc/LedgerSnapshot.cpp)
target_include_directories(test_ledger_snapshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_ledger_snapshot PRIVATE pthread)
endif()

# ─── 테스트 타겟: 심장박동 전이 (D-114 단계 2) ───────────────────
add_executable(test_heartbeat tests/test_heartbeat.cpp src/ipc/Heartbeat.cpp)
target_include_directories(test_heartbeat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_heartbeat PRIVATE pthread)
endif()

# ─── 테스트 타겟: 비동기 Logger MPSC 큐·flush 계약 (D-045) ───────────────
add_executable(test_logger tests/test_logger.cpp src/utils/Logger.cpp)
target_include_directories(test_logger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_logger PRIVATE pthread)
endif()

# ─── 테스트 타겟 공통: Release에서도 assert를 살린다 ──────────────────────
#  Release 구성은 /DNDEBUG를 주므로 assert가 전부 사라진다. 그러면 테스트 바이너리가
#  검증 대상을 호출하지도 않고 PASS만 찍는다(2026-09-08 리뷰에서 test_order_gate 18건이
#  그 상태였다). 테스트 타깃에 한해 NDEBUG를 되돌린다 — 컴파일러가 /D 뒤에 오는 /U를
#  나중에 처리하므로, 구성 플래그보다 뒤에 붙는 이 옵션이 이긴다.
set(QUANT_ASSERT_TARGETS
    test_order_gate
    test_order_router
    test_ws_frame
    test_ws_decode
    test_kis_decode
    test_market_session
    test_reconcile_plan
    test_ledger_reconciler
    test_data_poller
    test_signal_dispatcher
    test_order_rate_limiter
    test_bar_aggregator
    test_ops_protocol
    test_ops_server
    test_regime_file_judge
    test_devscale_rules
    test_target_basket_plan
    test_target_basket_strategy
    test_ringbuffer
    test_ringbuffer_stress
    test_pipeline_stress
    test_mpsc
    test_wake_gate
    test_latency_trace
    test_symbol_table
    test_tick_capture
    test_prefetch_pool
    test_replay_source
    test_paper_executor
    test_feed_mux
    test_feed_supervisor
    test_session_end
    test_engine
    test_app_config
    test_shard_matrix
    test_strategy_shard
    test_strategy_router
    test_account_ledger
    test_logger
    test_protective_orders
    test_order_channel
    test_heartbeat
    test_ledger_snapshot
)
foreach(_qt IN LISTS QUANT_ASSERT_TARGETS)
    if(MSVC)
        target_compile_options(${_qt} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${_qt} PRIVATE -UNDEBUG)
    endif()
endforeach()

# ─── ctest 등록 ───────────────────────────────────────────────────────────
# `ctest --preset x64-release`(저장소 루트) 또는 `ctest --test-dir Quant/build_win`.
# 스트레스 두 개는 3초로 줄여 돌린다(운영 검증은 실행파일을 직접 길게). 벤치 3종은 측정용이라
# 등록하지 않는다. 산출물(logs/·trades_*.csv)은 실행파일 옆이 아니라 QUANT_LOG_DIR을 따르므로
# 빌드 폴더 아래 test_run/logs로 고정한다 — 셸에 운영 QUANT_LOG_DIR이 잡혀 있으면 테스트가
# 라이브 원장에 행을 섞는다(09-09 오염 재현 경로).
enable_testing()
set(QUANT_TEST_RUN_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_run)
file(MAKE_DIRECTORY ${QUANT_TEST_RUN_DIR})
# 산출물 자기 정리 — test_run/logs에 며칠치 latency_trace.csv·order_reasons_*.txt가 쌓이면
#  test_latency_trace·test_tick_capture·test_replay_source·test_engine이 ctest에서만 실패한다(단독 실행은
#  통과). 픽스처 하나가 매 ctest 시작 때 그 폴더를 비우고, 아래 모든 테스트가 그 픽스처를 요구한다.
add_test(NAME test_run_clean_logs
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${QUANT_TEST_RUN_DIR}/logs)
set_tests_properties(test_run_clean_logs PROPERTIES FIXTURES_SETUP quant_test_run_logs)
foreach(_qt IN LISTS QUANT_ASSERT_TARGETS)
    if(_qt STREQUAL "test_ringbuffer_stress")
        add_test(NAME ${_qt} COMMAND ${_qt} 3 WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    elseif(_qt STREQUAL "test_pipeline_stress")
        add_test(NAME ${_qt} COMMAND ${_qt} 3 normal WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    else()
        add_test(NAME ${_qt} COMMAND ${_qt} WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    endif()
    set_tests_properties(${_qt} PROPERTIES TIMEOUT 120
        FIXTURES_REQUIRED quant_test_run_logs
        ENVIRONMENT "QUANT_LOG_DIR=${QUANT_TEST_RUN_DIR}/logs")
endforeach()

# ─── 도구: 수동 주문(모의계좌) — 접수→체결 실증 ───────────────────────────
add_executable(manual_order
    tools/manual_order.cpp
    ${KIS_CLIENT_SOURCES}
    src/risk/OrderGate.cpp
    src/utils/Logger.cpp
)
target_include_directories(manual_order PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(manual_order PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(manual_order PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
endif()

# ─── 도구: 운영단말 콘솔 클라이언트 — OpsServer 왕복 검증·최소 단말 (D-043) ──
add_executable(ops_client tools/ops_client.cpp)
target_include_directories(ops_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(ops_client PRIVATE nlohmann_json::nlohmann_json)
else()
    target_link_libraries(ops_client PRIVATE nlohmann_json::nlohmann_json ws2_32)
endif()

# ─── 도구: 운영단말 MFC 씬 클라이언트 (D-043 3단계) ───────────────────────
# MFC가 설치된 MSVC에서만 만든다. VCToolsInstallDir는 VS 개발셸에서 잡히고, 아니면 알려진 경로를 뒤진다.
if(MSVC)
    find_path(QUANT_MFC_INC afxwin.h
        PATHS "$ENV{VCToolsInstallDir}/atlmfc/include"
              "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/atlmfc/include"
        NO_DEFAULT_PATH)
    if(QUANT_MFC_INC)
        set(QUANT_OPS_TERMINAL_DEFAULT ON)
    else()
        set(QUANT_OPS_TERMINAL_DEFAULT OFF)
    endif()
    option(QUANT_OPS_TERMINAL "MFC 운영단말(ops_terminal) 빌드" ${QUANT_OPS_TERMINAL_DEFAULT})
    if(QUANT_OPS_TERMINAL AND QUANT_MFC_INC)
        set(CMAKE_MFC_FLAG 2)
        add_executable(ops_terminal WIN32
            tools/ops_terminal/OpsTerminal.cpp
            tools/ops_terminal/OpsTerminalDlg.cpp
            tools/ops_terminal/OpsLink.cpp
            src/ipc/OpsProtocol.cpp
            tools/ops_terminal/OpsTerminal.rc
        )
        target_include_directories(ops_terminal PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops_terminal)
        # 유니코드 MFC는 진입점이 wWinMainCRTStartup이다 — 빼면 _WinMain@16 미해결로 링크가 실패한다.
        target_compile_definitions(ops_terminal PRIVATE _AFXDLL UNICODE _UNICODE)
        target_link_options(ops_terminal PRIVATE /ENTRY:wWinMainCRTStartup)
        target_link_libraries(ops_terminal PRIVATE nlohmann_json::nlohmann_json ws2_32)
        # 링크 뒤 바탕화면 `운영단말.lnk`를 이 exe로 맞춘다(메인 트리 빌드만, worktree는 스크립트가 건너뛴다).
        #  사용자가 더블클릭으로 띄우는 길이라 빌드마다 따라가야 한다. 실패해도 빌드는 깨지 않는다.
        add_custom_command(TARGET ops_terminal POST_BUILD
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ops_terminal_shortcut.ps1" "$<TARGET_FILE:ops_terminal>"
            VERBATIM)
        message(STATUS "ops_terminal(MFC) 빌드 대상에 포함: ${QUANT_MFC_INC}")
    else()
        message(STATUS "ops_terminal(MFC) 건너뜀 — afxwin.h 없음 또는 QUANT_OPS_TERMINAL=OFF")
    endif()
endif()

# ─── 도구: 실 KIS 실시간 시세 수신·처리 지연 측정 (bench_feed_ingest 자매 실증) ──
add_executable(feed_latency_measure
    tools/feed_latency_measure.cpp
    ${KIS_WS_SOURCES}
    ${KIS_CLIENT_SOURCES}
    src/utils/Logger.cpp
)
target_include_directories(feed_latency_measure PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(feed_latency_measure PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(feed_latency_measure PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
endif()

# ─── 도구: 국내 선물 시세 조회 점검 (파생 배선 1단계 — REST inquire-price) ──
add_executable(future_quote_check
    tools/future_quote_check.cpp
    ${KIS_CLIENT_SOURCES}
    src/utils/Logger.cpp
)
target_include_directories(future_quote_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(future_quote_check PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(future_quote_check PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
endif()

# ─── 벤치: REST 커넥션 풀링(P-2) 효과 — 조회 왕복 지연 분포, QUANT_HTTP_NOPOOL로 A/B ──
add_executable(bench_rest_pool
    tools/bench_rest_pool.cpp
    ${KIS_CLIENT_SOURCES}
    src/utils/Logger.cpp
)
target_include_directories(bench_rest_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(bench_rest_pool PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(bench_rest_pool PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
endif()

# 헤더에서 내려온 구현은 시험·벤치 타깃도 쓴다 — 타깃마다 소스를 나열하는 대신 한 번에 건다.
get_property(QUANT_ALL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(one_target ${QUANT_ALL_TARGETS})
    get_target_property(target_kind ${one_target} TYPE)
    if(target_kind STREQUAL "EXECUTABLE" AND NOT one_target STREQUAL "ops_terminal")
        target_link_libraries(${one_target} PRIVATE quant_header_impl)
    endif()
endforeach()
