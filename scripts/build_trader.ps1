<#
.SYNOPSIS
  quant_trader(또는 지정 타깃)를 MSVC 로 빌드한다 — 한글 임시폴더·콘솔 인코딩 함정을 미리 막는다.

.DESCRIPTION
  손으로 cmake 를 부르면 이 저장소에서는 두 가지에 걸린다.

    1. LNK1104 — 임시폴더 경로에 한글이 들어 있어 link.exe 가 임시파일을 못 연다. 컴파일은 통과하고
       링크만 깨지므로 코드 문제로 오해하기 쉽다. TEMP 를 ASCII 경로로 바꿔 막는다.
    2. 헤더 의존이 하나도 안 적힌다 — Quant/build_win 은 UTF-8 콘솔에서 만들어졌다. 다른 코드페이지로
       빌드하면 cl 이 내는 머리말 바이트가 어긋나 ninja 가 의존을 못 적고, 헤더만 바뀐 날 옛 오브젝트가
       그대로 링크된다. 같은 구조체를 두 크기로 아는 실행 파일이 나와 기동하자마자 죽는다
       (2026-09-21·09-23 두 번). 콘솔을 UTF-8 로 고정해 막는다.

  빌드 뒤에 실행 파일과 소스 시각을 견줘, 지금 띄울 수 있는 상태인지까지 알려 준다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build_trader.ps1
  powershell -ExecutionPolicy Bypass -File scripts\build_trader.ps1 -Target ops_terminal
  powershell -ExecutionPolicy Bypass -File scripts\build_trader.ps1 -CleanFirst   # 헤더 의존이 깨졌을 때
#>
[CmdletBinding()]
param(
  [string]$Target = "quant_trader",
  [switch]$CleanFirst                # 전부 지우고 다시. 의존 기록이 비었을 때만 쓴다(몇 분 걸린다)
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 저장소 루트로. 경로 리터럴을 박지 않고 스크립트 위치에서 유도한다 — 사용자 폴더에 한글이 들어 있다.
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

$BuildDirectory = "Quant\build_win"
$ExePath = Join-Path $Repo "$BuildDirectory\quant_trader.exe"

function Say([string]$message, [string]$color = "Gray")
{
  Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $message) -ForegroundColor $color
}

# ─────────────── 1. 함정 두 개를 미리 막는다 ───────────────
if (-not (Test-Path "C:\build_tmp"))
{
  New-Item -ItemType Directory -Force "C:\build_tmp" | Out-Null
}

$env:TEMP = "C:\build_tmp"
$env:TMP = "C:\build_tmp"

# ─────────────── 2. 실행 중인 트레이더가 exe 를 잠그고 있나 ───────────────
# 링크 단계에서 LNK1104 가 나는 또 하나의 원인이다. 감시견이 띄운 것을 여기서 죽이지는 않는다.
# 잠금은 이름이 아니라 경로에 걸린다 — 돌고 있는 프로세스가 지금 링크할 그 자리를 잡고 있을 때만 막는다.
#  Windows 는 실행 중인 파일의 rename 은 허용한다 — quant_trader.exe 를
#  quant_trader_old_<HHmmss>.exe 로 옆에 옮겨 놓고 부르면 링크가 통하고, 그 뒤 프로세스를
#  내리면 감시견이 새 exe 로 다시 띄운다. 2026-09-23 장중 배포 세 번을 이 길로 했다.
$targetExe = Join-Path (Get-Location).Path "Quantuild_win\$Target.exe"
$locking   = @(Get-Process quant_trader -ErrorAction SilentlyContinue |
               Where-Object { $_.Path -and ($_.Path -eq $targetExe) })

if ($locking.Count -gt 0)
{
  Say "트레이더가 돌고 있다(pid=$($locking.Id -join ', ')) — 링크할 그 자리를 프로세스가 잡고 있다." "Yellow"
  Say "  옆으로 옮기고 다시 부른다 — Move-Item Quantuild_win\$Target.exe Quantuild_win\${Target}_old_170000.exe" "DarkYellow"
  exit 3
}

$swapped = @(Get-Process quant_trader -ErrorAction SilentlyContinue)

if ($swapped.Count -gt 0)
{
  Say "트레이더 $($swapped.Count)개가 옆으로 옮겨놓은 exe 로 돌고 있다(pid=$($swapped.Id -join ', ')) — 링크는 진행한다." "DarkGray"
}

# ─────────────── 3. 도구 경로 ───────────────
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

foreach ($tool in @($vcvars, $cmake))
{
  if (-not (Test-Path $tool))
  {
    Write-Host "[중단] 빌드 도구를 못 찾았다: $tool" -ForegroundColor Red
    exit 2
  }
}

if (-not (Test-Path (Join-Path $Repo "$BuildDirectory\CMakeCache.txt")))
{
  Write-Host "[중단] $BuildDirectory 가 아직 구성되지 않았다 — .claude/commands/build.md 2절의 generate 부터." -ForegroundColor Red
  exit 2
}

# ─────────────── 4. 빌드 ───────────────
$before = if (Test-Path $ExePath) { (Get-Item $ExePath).LastWriteTime } else { [datetime]::MinValue }
$cleanClause = if ($CleanFirst) { " --clean-first" } else { "" }

Say "빌드 시작 — 타깃 $Target$(if ($CleanFirst) { ' (전부 다시)' } else { '' })"
cmd /c "`"$vcvars`" >nul 2>&1 && `"$cmake`" --build $BuildDirectory --target $Target$cleanClause" 2>&1 |
  Where-Object { $_ -match 'error|FAILED|LNK\d|Linking CXX executable|Building CXX' } |
  Select-Object -Last 6

$buildFailed = $LASTEXITCODE -ne 0

if ($buildFailed)
{
  Write-Host ""
  Say "빌드 실패 — 위 줄에서 error·FAILED·LNK 를 찾는다." "Red"
  exit 1
}

# ─────────────── 5. 띄울 수 있는 상태인가 ───────────────
Write-Host ""

if ($Target -ne "quant_trader")
{
  Say "빌드 성공 — 타깃 $Target" "Green"
  exit 0
}

$exe = Get-Item $ExePath
Say "빌드 성공 — 실행 파일 $($exe.LastWriteTime.ToString('HH:mm:ss'))" "Green"

# 감시견이 -NoBuild 로 막는 조건과 같은 검사다. 여기서 미리 걸러야 기동하다 되돌아오지 않는다.
$newer = @(Get-ChildItem Quant\src, Quant\include -Recurse -Include *.cpp, *.h |
  Where-Object { $_.LastWriteTime -gt $exe.LastWriteTime })

if ($newer.Count -gt 0)
{
  Say "실행 파일보다 새 소스 $($newer.Count)개 — 빌드 도중 누가 또 고쳤다. 다시 부른다." "Yellow"
  $newer | Select-Object -First 5 | ForEach-Object { Write-Host ("    {0}  {1}" -f $_.LastWriteTime.ToString("HH:mm:ss"), $_.Name) -ForegroundColor DarkYellow }
  exit 4
}

if ($exe.LastWriteTime -eq $before)
{
  Say "바뀐 것이 없어 다시 링크하지 않았다 — 이미 최신이다." "DarkGray"
}

Write-Host ""
Write-Host "  띄우기:  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_live.ps1 -NoBuild" -ForegroundColor Cyan
