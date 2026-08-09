cmake_minimum_required(VERSION 3.16)
project(QuantTrader VERSION 1.0.0 LANGUAGES CXX)
 
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
 
# ─── 빌드 타입 기본값 ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
 
# ─── 컴파일 옵션 ──────────────────────────────────────────────────────────
if(UNIX)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG   "-g -fsanitize=address")
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
set(SOURCES
  src/main.cpp
  src/modes/Monitors.cpp
  src/universe/UniverseScanner.cpp
  src/strategy/StrategyFactory.cpp
  src/core/Engine.cpp
  src/core/RegimeController.cpp
  src/core/RingBuffer.cpp
  src/api/KisClient.cpp
  src/api/WebSocketClient.cpp
  src/strategy/StrategyBase.cpp
  src/strategy/MACrossStrategy.cpp
  src/strategy/MomentumStrategy.cpp
  src/risk/OrderGate.cpp
  src/utils/Logger.cpp
  src/utils/Config.cpp
  src/utils/Timer.cpp
)

list(APPEND SOURCES src/ipc/OrderRouter.cpp)

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

# ─── 테스트 타겟: 계좌별 원장 파티셔닝 검증 (D3) ──────────────────────────
add_executable(test_account_ledger tests/test_account_ledger.cpp src/risk/OrderGate.cpp)
target_include_directories(test_account_ledger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX)
    target_link_libraries(test_account_ledger PRIVATE pthread)
endif()

# ─── 도구: 수동 주문(모의계좌) — 접수→체결 실증 ───────────────────────────
add_executable(manual_order
    tools/manual_order.cpp
    src/api/KisClient.cpp
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