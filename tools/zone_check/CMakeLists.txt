cmake_minimum_required(VERSION 3.20)
project(zone_check CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(zone_check main.cpp)

if(MSVC)
    target_compile_options(zone_check PRIVATE /utf-8)
    target_compile_definitions(zone_check PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(zone_check PRIVATE winhttp)
endif()
