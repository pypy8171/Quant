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
if(UNIX)
    find_package(CURL REQUIRED)
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
 
# ─── 소스 파일 ────────────────────────────────────────────────────────────
set(SOURCES
  src/main.cpp
  src/core/Engine.cpp
  src/core/RingBuffer.cpp
  src/api/KisClient.cpp
  src/api/WebSocketClient.cpp
  src/strategy/StrategyBase.cpp
  src/strategy/MACrossStrategy.cpp
  src/strategy/MomentumStrategy.cpp
  src/utils/Logger.cpp
  src/utils/Config.cpp
  src/utils/Timer.cpp
)
 
# ─── 실행 파일 ────────────────────────────────────────────────────────────
add_executable(quant_trader ${SOURCES})
 
target_include_directories(quant_trader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(UNIX)
    target_link_libraries(quant_trader PRIVATE
      CURL::libcurl
      nlohmann_json::nlohmann_json
      pthread
      stdc++fs
    )
else()
    # Windows: WinHTTP는 Windows SDK 내장 (별도 설치 불필요)
    target_link_libraries(quant_trader PRIVATE
      nlohmann_json::nlohmann_json
      winhttp
    )
endif()
 
# ─── config 파일을 IDE(솔루션 탐색기)에 표시 ─────────────────────────────
target_sources(quant_trader PRIVATE config/config.json)
set_source_files_properties(config/config.json PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("config" FILES config/config.json)

# ─── 빌드 후 config 자동 복사 ─────────────────────────────────────────────
add_custom_command(TARGET quant_trader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:quant_trader>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json
        $<TARGET_FILE_DIR:quant_trader>/config/config.json
)

# ─── 설치 ──────────────────────────────────────────────────────────────────
install(TARGETS quant_trader DESTINATION bin)
install(FILES config/config.json DESTINATION etc/quant_trader)