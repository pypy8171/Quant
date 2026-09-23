# 킬스위치 해제 — 운영단말·ZMQ KILL이 남긴 표지 파일(_private/state/kill_today_<날짜>)을 지운다.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/kill_release.ps1 [-Date 2026-09-19] [-Instance live] [-DryRun]
#   -Instance는 config의 "instance" 값이다(D-122). 주면 표지·상태파일 이름에 _<instance>가 붙은 쪽을 다룬다 — 엔진
#   write_state_marker·감시견 $Suffix와 같은 규칙.
# 동작 원리(D-098):
#   - KILL을 받은 엔진은 kill_today_<KST 날짜> 파일을 쓰고 내려간다. 감시견(scripts/auto_trade_day.ps1)은 재기동 전에
#     이 파일을 보고 그날은 다시 띄우지 않는다("오늘은 끝"). 가드(auto_trade_guard.ps1)는 감시견 상태 closed를 존중한다.
#   - 원인을 고친 뒤 다시 매매하려면 이 스크립트로 파일을 지우고, 감시견 창이 닫혀 있으면 가드가 5분 안에 다시 띄운다
#     (기다리기 싫으면 auto_trade_guard.ps1을 손으로 한 번 돌린다; 상태파일은 이 스크립트가 옆으로 치운다). 엔진을 손으로 띄우지 않는다 — 엔진이 둘이 된다.
#   - session_done_<날짜>(마감 자기 종료)는 지우지 않는다. 그건 KILL이 아니라 하루가 끝난 것이다.
param(
    [string]$Date = (Get-Date -Format yyyy-MM-dd),
    [string]$Instance = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repo   = Split-Path -Parent $PSScriptRoot
$suffix = if ($Instance) { "_$Instance" } else { "" }
$marker = Join-Path $repo "_private\state\kill_today$($suffix)_$Date"

if (-not (Test-Path $marker))
{
    Write-Host "KILL 표지 파일이 없다: $marker — 지울 것이 없다."
    exit 0
}

Write-Host "KILL 표지 파일:"
Get-Content $marker | ForEach-Object { Write-Host "  $_" }

if ($DryRun)
{
    Write-Host "[DryRun] 지우지 않는다."
    exit 0
}

Remove-Item $marker -Force
Write-Host "지웠다: $marker"

# 감시견은 KILL을 보고 phase=closed로 끝났고, 가드는 오늘 phase가 closed면 되살리지 않는다. 상태파일을 옆으로 치워
#  가드가 "상태 없음"으로 보게 한다 — 다음 5분 주기에 감시견을 다시 띄운다.
$statusPath = Join-Path $repo "_private\_auto_trade_day$suffix.json"

if (Test-Path $statusPath)
{
    $phase = (Get-Content $statusPath -Raw -Encoding UTF8 | ConvertFrom-Json).phase

    if ($phase -ne "running")
    {
        $aside = "$statusPath.killed_$(Get-Date -Format HHmmss)"
        Move-Item $statusPath $aside -Force
        Write-Host "감시견 상태 $phase → 상태파일을 $aside 로 치웠다. 가드(auto_trade_guard.ps1)가 5분 안에 감시견을 다시 띄운다."
    }
    else
    {
        Write-Host "감시견이 running 상태다 — 감시견 루프가 다음 재기동에서 그대로 엔진을 띄운다."
    }
}
