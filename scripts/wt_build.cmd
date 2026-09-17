@echo off
rem worktree build and test. usage: scripts/wt_build.cmd [test]  (ASCII only: cmd reads batch files in the OEM code page)
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "VSEXT=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSEXT%\CMake\bin;%VSEXT%\Ninja;%PATH%"
set "ROOT=%~dp0.."
set "BUILD=%ROOT%\Quant\build_win"

if not exist "%BUILD%\CMakeCache.txt" (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -S "%ROOT%\Quant" -B "%BUILD%"
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD%"
if errorlevel 1 exit /b 1

if "%~1"=="test" ctest --test-dir "%BUILD%" --output-on-failure
