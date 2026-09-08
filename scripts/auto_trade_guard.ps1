<#
.SYNOPSIS
  하루 루프(auto_trade_day.ps1)가 살아 있는지 주기적으로 보고, 없으면 다시 띄운다.

.DESCRIPTION
  워치독은 트레이더를 되살리지만 워치독 자신이 죽으면 아무도 되살리지 않았다. 잡(Job Object)
  때문에 워치독이 사라지면 트레이더·부속 창도 같이 내려가므로, 그 위에 한 층이 필요하다.
  이 스크립트가 그 층이다 — 예약작업이 몇 분마다 불러, 장중이고 워치독이 없으면 기동한다.

  사람이 일부러 멈춘 상태와 사고를 구분해야 한다. 그래서 오늘 날짜 상태파일
  _private/_auto_trade_day.json 의 phase 가 crash_loop·aborted·done·closed·past_deadline 이면
  건드리지 않는다. 크래시 루프를 예약작업으로 되살리면 계좌만 반복 호출한다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Install
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_guard.ps1 -Uninstall
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_dev_paper.json",
  [string]$Open   = "08:45",   # 이 시각 전에는 기동하지 않는다(장 시작 09:00 전 준비 여유)
  [string]$Until  = "15:35",   # 워치독에 그대로 넘기는 마감 시각
  [switch]$Install,            # 평일 5분 주기 예약작업 등록
  [switch]$Uninstall,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Repo   = Split-Path -Parent $PSScriptRoot
Set-Location $Repo
$Day    = Join-Path $Repo "scripts\auto_trade_day.ps1"
$Status = Join-Path $Repo "_private\_auto_trade_day.json"
$LogDir = Join-Path $Repo "logs"
$RunLog = Join-Path $LogDir ("auto_trade_guard_{0}.log" -f (Get-Date -Format yyyyMMdd))
$TaskName = "QuantAutoTradeGuard"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Say([string]$msg, [string]$level = "INFO") {
  $line = "[{0}] {1,-5} {2}" -f (Get-Date -Format "HH:mm:ss"), $level, $msg
  Write-Host $line
  Add-Content -Path $RunLog -Value $line -Encoding utf8
}

# ─────────────── 예약작업 등록/해제 ───────────────
if ($Uninstall) {
  try { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false; Say "예약작업 '$TaskName' 제거" }
  catch { Say "예약작업이 없거나 제거 실패: $($_.Exception.Message)" "WARN" }
  exit 0
}

if ($Install) {
  # 창이 보여야 사람이 눈으로 확인할 수 있다 → 로그온 세션에서 실행한다.
  $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Config `"$Config`" -Open $Open -Until $Until"
  $act = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arg -WorkingDirectory $Repo
  $trg = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday, Tuesday, Wednesday, Thursday, Friday -At $Open
  # 주간 트리거에는 반복 설정이 없다. 1회 트리거에서 Repetition만 떼어 붙인다.
  $trg.Repetition = (New-ScheduledTaskTrigger -Once -At $Open `
      -RepetitionInterval (New-TimeSpan -Minutes 5) `
      -RepetitionDuration (New-TimeSpan -Hours 7)).Repetition
  $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
  Register-ScheduledTask -TaskName $TaskName -Action $act -Trigger $trg -Settings $set -Force | Out-Null
  Say "예약작업 '$TaskName' 등록 — 평일 $Open 부터 5분마다 7시간, config=$Config"
  exit 0
}

# ─────────────── 기동 조건 ───────────────
$now = Get-Date
if ($now.DayOfWeek -eq [DayOfWeek]::Saturday -or $now.DayOfWeek -eq [DayOfWeek]::Sunday) {
  Say "주말 — 넘어간다."; exit 0
}

$today = $now.ToString("yyyy-MM-dd")
$open = [datetime]::ParseExact("$today $Open", "yyyy-MM-dd HH:mm", $null)
$close = [datetime]::ParseExact("$today $Until", "yyyy-MM-dd HH:mm", $null)
if ($now -lt $open) { Say "$Open 전 — 넘어간다."; exit 0 }
if ($now -ge $close) { Say "$Until 이후 — 넘어간다."; exit 0 }

# 오늘 이미 끝났거나 사람이 멈춘 상태면 되살리지 않는다. 어제 상태파일은 무시한다.
if (Test-Path $Status) {
  try {
    $st = Get-Content $Status -Raw | ConvertFrom-Json
    $stop = @("crash_loop", "aborted", "done", "closed", "past_deadline")
    if (([datetime]$st.updated).Date -eq $now.Date -and $stop -contains $st.phase) {
      Say "오늘 phase=$($st.phase) — 되살리지 않는다(원인을 없앤 뒤 손으로 기동)."
      exit 0
    }
  } catch { Say "상태파일을 읽지 못했다($($_.Exception.Message)) — 없는 셈 치고 진행." "WARN" }
}

# ─────────────── 워치독 생존 확인 ───────────────
# -Command로 감싸 띄운 래퍼 창은 -NoExit라 스크립트가 끝나도 살아 있고, 그 명령줄에도 스크립트
# 이름이 박혀 있다. 그걸 워치독으로 세면 죽은 뒤로도 영영 되살리지 않는다. -File 실행만 센다.
$live = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.CommandLine -like "*auto_trade_day.ps1*" -and $_.CommandLine -notlike "*-Command*" })
if ($live.Count -gt 0) { Say "워치독 생존(pid=$($live.ProcessId -join ',')) — 할 일 없음."; exit 0 }

# 워치독 없이 남은 트레이더는 감시자가 없다. 두면 다음 기동이 duplicate_process로 막힌다.
$orphan = @(Get-Process quant_trader -ErrorAction SilentlyContinue)
if ($orphan.Count -gt 0) {
  Say "워치독 없이 떠 있는 quant_trader $($orphan.Count)개(pid=$($orphan.Id -join ',')) — 내린다." "WARN"
  if (-not $DryRun) { $orphan | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
}

Say "워치독 없음 — 하루 루프를 기동한다(config=$Config until=$Until)."
if ($DryRun) { Say "(dry) 기동 생략"; exit 0 }
Start-Process powershell -ArgumentList @(
  "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Day,
  "-Config", $Config, "-Until", $Until
) -WorkingDirectory $Repo | Out-Null
Say "기동 요청 완료."
