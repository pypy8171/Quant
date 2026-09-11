cmake_minimum_required(VERSION 3.16)
project(QuantTrader VERSION 1.0.0 LANGUAGES CXX)
 
set(CMAKE_CXX_STANDARD 17)
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
    add_compile_options(-Wall -Wextra -Wpedantic)
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
find_library(ZMQ_LIB  NAMES zmq libzmq)
find_path   (ZMQ_INC  NAMES zmq.h)

if(ZMQ_LIB AND ZMQ_INC)
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
set(KIS_WS_SOURCES src/api/WebSocketClient.cpp)
if(WIN32)
  list(APPEND KIS_WS_SOURCES src/api/WsSocketWin.cpp)
else()
  list(APPEND KIS_WS_SOURCES src/api/WsSocketPosix.cpp)
endif()

set(SOURCES
  src/main.cpp
  src/modes/Monitors.cpp
  src/universe/UniverseScanner.cpp
  src/strategy/StrategyFactory.cpp
  src/core/Engine.cpp
  src/core/LedgerReconciler.cpp
  src/core/DataPoller.cpp
  src/core/RegimeController.cpp
  src/core/RingBuffer.cpp
  ${KIS_CLIENT_SOURCES}
  ${KIS_WS_SOURCES}
  src/strategy/StrategyBase.cpp
  src/strategy/MACrossStrategy.cpp
  src/strategy/MomentumStrategy.cpp
  src/risk/OrderGate.cpp
  src/utils/Logger.cpp
  src/utils/Config.cpp
  src/utils/Timer.cpp
)

list(APPEND SOURCES src/ipc/OrderRouter.cpp src/ipc/OpsServer.cpp)

if(ZMQ_FOUND)
  list(APPEND SOURCES src/ipc/ZmqBridge.cpp)
endif()
 
# ─── 실행 파일 ────────────────────────────────────────────────────────────
add_executable(quant_trader ${SOURCES})
 
target_include_directories(quant_trader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(ZMQ_FOUND)
  target_include_directories(quant_trader PRIVATE ${ZMQ_INC} ${cppzmq_SOURCE_DIR})
endif()

if(UNIX)
    target_link_libraries(quant_trader PRIVATE
      CURL::libcurl
      OpenSSL::Crypto
      nlohmann_json::nlohmann_json
      pthread
      stdc++fs
    )
else()
    # Windows: WinHTTP/BCrypt는 Windows SDK 내장 (별도 설치 불필요)
    target_link_libraries(quant_trader PRIVATE
      nlohmann_json::nlohmann_json
      winhttp
      bcrypt
      ws2_32
    )
endif()

if(ZMQ_FOUND)
  target_link_libraries(quant_trader PRIVATE ${ZMQ_LIB})
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
add_executable(test_order_gate tests/test_order_gate.cpp src/risk/OrderGate.cpp)
target_include_directories(test_order_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_order_gate PRIVATE pthread)
endif()

# ─── 테스트 타겟: OrderRouter 통합 테스트 (StubOrderExecutor, KIS 불필요) ──
add_executable(test_order_router
    tests/test_order_router.cpp
    src/risk/OrderGate.cpp
    src/ipc/OrderRouter.cpp
    src/utils/Logger.cpp
)
target_include_directories(test_order_router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
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

# 매크로 국면 파일 브리지(core/RegimeFileBridge.h) — 헤더 전용, 파일·로그 없음 (D-060)
add_executable(test_regime_bridge tests/test_regime_bridge.cpp)
target_include_directories(test_regime_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_regime_bridge PRIVATE nlohmann_json::nlohmann_json)

# ─── 테스트 타겟: RegimeController 점수/분류 단위 테스트 (KIS 불필요) ──────
add_executable(test_regime tests/test_regime.cpp)
target_include_directories(test_regime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(test_regime PRIVATE pthread)
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

# ─── 벤치 타겟: 전종목 규모 시세 파이프라인 부하 (market firehose) ────────
add_executable(bench_market_firehose tests/bench_market_firehose.cpp)
target_include_directories(bench_market_firehose PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(bench_market_firehose PRIVATE pthread)
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

# ─── 테스트 타겟: 비동기 Logger MPSC 큐·flush 계약 (D-045) ───────────────
add_executable(test_logger tests/test_logger.cpp)
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
    test_ops_protocol
    test_ops_server
    test_regime
    test_regime_bridge
    test_ringbuffer
    test_ringbuffer_stress
    test_pipeline_stress
    test_mpsc
    test_account_ledger
    test_logger
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
foreach(_qt IN LISTS QUANT_ASSERT_TARGETS)
    if(_qt STREQUAL "test_ringbuffer_stress")
        add_test(NAME ${_qt} COMMAND ${_qt} 3 WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    elseif(_qt STREQUAL "test_pipeline_stress")
        add_test(NAME ${_qt} COMMAND ${_qt} 3 normal WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    else()
        add_test(NAME ${_qt} COMMAND ${_qt} WORKING_DIRECTORY ${QUANT_TEST_RUN_DIR})
    endif()
    set_tests_properties(${_qt} PROPERTIES TIMEOUT 120
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
            tools/ops_terminal/OpsTerminal.rc
        )
        target_include_directories(ops_terminal PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops_terminal)
        # 유니코드 MFC는 진입점이 wWinMainCRTStartup이다 — 빼면 _WinMain@16 미해결로 링크가 실패한다.
        target_compile_definitions(ops_terminal PRIVATE _AFXDLL UNICODE _UNICODE)
        target_link_options(ops_terminal PRIVATE /ENTRY:wWinMainCRTStartup)
        target_link_libraries(ops_terminal PRIVATE nlohmann_json::nlohmann_json ws2_32)
        message(STATUS "ops_terminal(MFC) 빌드 대상에 포함: ${QUANT_MFC_INC}")
    else()
        message(STATUS "ops_terminal(MFC) 건너뜀 — afxwin.h 없음 또는 QUANT_OPS_TERMINAL=OFF")
    endif()
endif()

# ─── 도구: 실 KIS 실시간 시세 수신·처리 지연 프로브 (bench_feed_ingest 자매 실증) ──
add_executable(feed_latency_probe
    tools/feed_latency_probe.cpp
    ${KIS_WS_SOURCES}
    ${KIS_CLIENT_SOURCES}
    src/utils/Logger.cpp
)
target_include_directories(feed_latency_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(feed_latency_probe PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(feed_latency_probe PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
endif()

# ─── 도구: 국내 선물 시세 조회 프로브 (파생 배선 1단계 — REST inquire-price) ──
add_executable(future_quote_probe
    tools/future_quote_probe.cpp
    ${KIS_CLIENT_SOURCES}
    src/utils/Logger.cpp
)
target_include_directories(future_quote_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(future_quote_probe PRIVATE
        CURL::libcurl OpenSSL::Crypto nlohmann_json::nlohmann_json pthread stdc++fs)
else()
    target_link_libraries(future_quote_probe PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt)
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