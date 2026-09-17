<#
  밤에 무인 작업 여러 개를 한 번에 띄운다. _private/jobs/*.txt 하나가 작업 하나다 —
  파일 이름이 곧 -Name 이 되고, 인계·로그·완료표시가 그 이름으로 갈린다(서로 안 섞인다).
  작업 파일은 UTF-8 BOM 으로 써야 한다. 한글이 깨져 들어간 적이 있다.
#>
param(
    [Parameter(Mandatory = $true)][string]$WakeTime,   # 기상 시각 "07:00" 또는 ISO 전체
    [int]$MaxCycles = 12,
    [int]$MaxMinutes = 420
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# "07:00" 처럼 시각만 주면 다음번 그 시각으로 읽는다(지금보다 이르면 내일).
if ($WakeTime -match '^\d{1,2}:\d{2}$')
{
    $until = [datetime]::ParseExact($WakeTime, 'H:mm', $null)
    if ($until -le (Get-Date)) { $until = $until.AddDays(1) }
}
else
{
    $until = [datetime]::Parse($WakeTime)
}
$untilIso = $until.ToString('yyyy-MM-ddTHH:mm:ss')

$jobs = @(Get-ChildItem -Path "_private/jobs" -Filter "*.txt" -File -ErrorAction SilentlyContinue)
if (-not $jobs)
{
    Write-Host "작업 파일이 없다 — _private/jobs/ 에 <이름>.txt 로 지시서를 넣어라."
    exit 1
}

Write-Host "무인 해제 $untilIso / 작업 $($jobs.Count) 개"

foreach ($job in $jobs)
{
    $name = $job.BaseName
    $args = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $PSScriptRoot 'unattended_run.ps1'),
        '-Name', $name,
        '-PromptFile', $job.FullName,
        '-Until', $untilIso,
        '-MaxCycles', $MaxCycles,
        '-MaxMinutes', $MaxMinutes
    )
    Start-Process -FilePath 'powershell.exe' -ArgumentList $args -WindowStyle Hidden -WorkingDirectory $root
    Write-Host "  띄움 $name — 로그 _private/_unattended_$name.log"
}

Write-Host "아침에 볼 것: _private/HANDOFF_<이름>.md 의 '한 것'·'남은 것'·'주의'"
