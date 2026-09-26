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
  [string]$Open   = "07:30",   # 이 시각 전에는 기동하지 않는다(08:00 NXT 개장 전 일봉 캐시 데우기 여유, D-147)
  [string]$Until  = "15:35",   # 워치독에 그대로 넘기는 마감 시각. 실계좌 전환 때 20:05(애프터마켓 20:00 + 여유, D-097·T-18)
  [double]$Hours  = 7,         # -Open 부터 5분마다 몇 시간 도는지. 모의 8.25(15:45까지), 실계좌 12.75(20:15까지). scripts\market_close_timetable.ps1 -Apply 가 넘긴다
  [switch]$Install,            # 평일 5분 주기 예약작업 등록
  [switch]$Uninstall,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Repo   = Split-Path -Parent $PSScriptRoot
Set-Location $Repo
$Day    = Join-Path $Repo "scripts\auto_trade_day.ps1"

# 한 기계에서 계좌를 둘 돌리는 날(모의 비교군 + 실계좌)에는 가드도 계좌마다 따로 돈다. config
#  `instance` 가 있으면 상태 파일·실행 로그·예약작업 이름에 붙인다 — 이게 없으면 두 번째 계좌용
#  가드를 -Install 할 때 첫 번째 예약작업을 덮어쓰고, 한쪽 상태를 보고 다른 쪽을 판정한다. [why D-122]
$Instance = ""
try { $Instance = [string](Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).instance } catch { }
$Suffix = if ($Instance) { "_$Instance" } else { "" }

$Status = Join-Path $Repo ("_private\_auto_trade_day{0}.json" -f $Suffix)
$LogDir = Join-Path $Repo "logs"
$RunLog = Join-Path $LogDir ("auto_trade_guard{0}_{1}.log" -f $Suffix, (Get-Date -Format yyyyMMdd))
$TaskName = "QuantAutoTradeGuard$Suffix"
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
  $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Config `"$Config`" -Open $Open -Until $Until -Hours $Hours"
  $act = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arg -WorkingDirectory $Repo
  $trg = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday, Tuesday, Wednesday, Thursday, Friday -At $Open
  # 주간 트리거에는 반복 설정이 없다. 1회 트리거에서 Repetition만 떼어 붙인다.
  $trg.Repetition = (New-ScheduledTaskTrigger -Once -At $Open `
      -RepetitionInterval (New-TimeSpan -Minutes 5) `
      -RepetitionDuration (New-TimeSpan -Minutes ([int]($Hours * 60)))).Repetition   # -Hours 는 정수만 받아 8.25 가 8 로 잘린다
  $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
  Register-ScheduledTask -TaskName $TaskName -Action $act -Trigger $trg -Settings $set -Force | Out-Null
  Say "예약작업 '$TaskName' 등록 — 평일 $Open 부터 5분마다 ${Hours}시간, -Until $Until, config=$Config"
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
    $st = Get-Content $Status -Raw -Encoding UTF8 | ConvertFrom-Json
    $stop = @("crash_loop", "aborted", "done", "closed", "past_deadline")
    if (([datetime]$st.updated).Date -eq $now.Date -and $stop -contains $st.phase) {
      Say "오늘 phase=$($st.phase) — 되살리지 않는다(원인을 없앤 뒤 손으로 기동)."
      exit 0
    }
    # 트레이더를 다른 곳(리눅스)이 띄운 날(-NoTrader, trader=external)은 Windows 트레이더를 절대 띄우지 않는다.
    # 감시견 창이 죽었다고 평소 루프를 되살리면 같은 계좌에 엔진이 둘이 된다(09-11 이중 발주).
    if (([datetime]$st.updated).Date -eq $now.Date -and "$($st.trader)" -eq "external") {
      Say "오늘 trader=external(리눅스가 띄움) — Windows 트레이더를 띄우지 않는다."
      exit 0
    }
  } catch { Say "상태파일을 읽지 못했다($($_.Exception.Message)) — 없는 셈 치고 진행." "WARN" }
}

# ─────────────── 워치독 생존 확인 ───────────────
# -Command로 감싸 띄운 래퍼 창은 -NoExit라 스크립트가 끝나도 살아 있고, 그 명령줄에도 스크립트
# 이름이 박혀 있다. 그걸 워치독으로 세면 죽은 뒤로도 영영 되살리지 않는다. -File 실행만 센다.
# 계좌를 둘 돌리는 날에는 config 까지 봐야 한다 — 실계좌 감시견이 떠 있다고 죽은 모의 감시견을
#  안 살리면, 모의 쪽은 아무도 안 지킨다. config 이름이 명령줄에 그대로 박혀 있다. [why D-122]
$configLeaf = Split-Path $Config -Leaf
$live = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.CommandLine -like "*auto_trade_day.ps1*" -and $_.CommandLine -notlike "*-Command*" -and $_.CommandLine -like "*$configLeaf*" })
if ($live.Count -gt 0) { Say "워치독 생존(pid=$($live.ProcessId -join ',')) — 할 일 없음."; exit 0 }

# 워치독 없이 남은 트레이더는 감시자가 없다. 두면 다음 기동이 duplicate_process로 막힌다.
# 이 config 로 뜬 트레이더만 내린다. 예전에는 이름만 보고 전부 내려, 다른 계좌의 멀쩡한 엔진까지
#  같이 죽었다. 명령줄을 못 읽으면 건드리지 않는다 — 남의 것일 수 있다. [why D-122]
$leftover_trader = @(Get-Process quant_trader -ErrorAction SilentlyContinue | Where-Object {
  $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
  $commandLine -and $commandLine -like "*$configLeaf*"
})
if ($leftover_trader.Count -gt 0) {
  Say "워치독 없이 떠 있는 quant_trader $($leftover_trader.Count)개(pid=$($leftover_trader.Id -join ',')) — 내린다." "WARN"
  if (-not $DryRun) { $leftover_trader | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
}

Say "워치독 없음 — 하루 루프를 기동한다(config=$Config until=$Until)."
if ($DryRun) { Say "(dry) 기동 생략"; exit 0 }
Start-Process powershell -ArgumentList @(
  "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Day,
  "-Config", $Config, "-Until", $Until
) -WorkingDirectory $Repo | Out-Null
Say "기동 요청 완료."
