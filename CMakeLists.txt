# CMakeList.txt : 최상위 CMake 프로젝트 파일, 전역 구성을 수행하고
# 여기에 하위 프로젝트를 포함합니다.
#
cmake_minimum_required (VERSION 3.8)

# 지원되는 경우 MSVC 컴파일러에 대해 핫 다시 로드 사용하도록 설정합니다.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("Quant")

if(MSVC)
    # 하위 모든 타깃이 MSVC 빌드 시 자동으로 /MD (또는 /MDd)를 사용하도록 전역 설정합니다.
    # add_subdirectory보다 먼저 설정해야 한다 — 타깃은 생성 시점의 값을 쓴다(늦게 설정하면 정적 런타임
    # 기본값으로 만들어져 ops_terminal의 _AFXDLL과 충돌한다: C1189 "/MD 스위치를 사용하십시오").
    # cmake_minimum_required가 3.8이라 CMP0091이 기본 OLD다 — NEW로 올리지 않으면 이 변수 자체가 무시된다.
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# CMP0141의 자동 링커 /DEBUG 주입이 Ninja + Open-Folder 조합에서 안 먹어
# quant_trader.pdb가 재빌드해도 안 갱신되는 문제가 있었다 — 명시로 강제한다.
add_link_options($<$<CONFIG:Debug,RelWithDebInfo>:/DEBUG>)

# ctest가 루트 빌드 폴더에서 하위 테스트를 찾게 한다(`ctest --preset x64-release`).
enable_testing()

# 하위 프로젝트를 포함합니다.
add_subdirectory ("Quant")
