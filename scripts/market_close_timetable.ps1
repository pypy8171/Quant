# 마감 자동화 시간표 — 계좌 모드(kis.is_paper)로 두 시간표 중 하나를 고르고, 예약작업·감시견이 그 시간표대로 걸려 있는지
# 보거나(-Check, 기본) 그대로 맞춘다(-Apply).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1                 # 예정 vs 실제, 어긋나면 exit 1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Apply          # schtasks 시각 변경 + 감시견 재등록
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Lines          # "이름<TAB>HH:MM<TAB>대응" (cron-gate 훅이 읽는다)
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Lines -Mode live   # config 안 읽고 그 모드 시간표만 (gen_facts·허브 생성기가 두 모드를 뽑는다)
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Windows        # 창을 띄우는 작업만 "이름<TAB>실행 파일" (cron-gate 훅이 읽는다)
# 규칙:
#   - 모의(is_paper=true): KIS 모의 서버가 15:30 뒤 주문을 거부한다(T-18, 2026-09-18 실측). 매매 끝 15:30, 마감 루틴 16:00대.
#   - 실계좌(is_paper=false): 애프터마켓 16:00~20:00(D-097)까지 매매, 마감 루틴 20:30 시작. 실계좌 애프터마켓 주문은
#     검색으로 파악한 것이고 실증이 없다 — 전환 뒤 첫날 16:00 넘어 체결 1건을 눈으로 확인한다.
#   - 계좌 모드는 감시견 예약작업(QuantAutoTradeGuard)이 넘기는 config의 kis.is_paper 로 읽는다. 최상위·quote_kis 의 is_paper 는
#     시세 계정이라 보지 않는다(scripts/auto_trade_day.ps1 와 같은 이유).
#   - 클로드를 부르는 두 작업(claude_stock_study·claude_dashboard_sync)은 세션 사용량 한도(17시 리셋) 때문에 모의에서도 20:30 뒤다.
#   - 시간표의 작업과 두 감시견은 창 없이 돈다 — conhost --headless 로 감싸거나 pythonw. 그 밖의 실행 방식은 어긋남으로 세고
#     -Apply 가 conhost --headless 로 감싼다. [why 10-30: 매매 아침 5분마다 콘솔 창이 떴다(사용자 지시)]
#   - 순서는 고정: 바스켓 비중표(08:40, 장 전) → 장 마감 AutoDoc → Maintain Daily → Minute Backfill → Daily Bars(바스켓 비중표가 읽는 일봉) → Study35 Watch(그 일봉으로 스터디 35 기록) → stock_study → dashboard_sync → Maintain Weekly(금).
param(
    [string]$Config = $null,
    [switch]$Apply,
    [switch]$Lines,
    [switch]$Windows,
    [ValidateSet('', 'paper', 'live')][string]$Mode = ''
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$repo = Split-Path -Parent $PSScriptRoot

# ── 계좌 모드 ──
if ($Mode -and $Apply)
{
    Write-Host "[market_close_timetable] -Mode 는 보기용이다. 적용은 config 로 모드를 읽는다(-Apply -Config <파일>)"
    exit 1
}

if ($Mode)
{
    $paper = ($Mode -eq 'paper')
}
elseif (-not $Config)
{
    $Config = 'Quant\config\config_dev_paper.json'
    $guardTask = Get-ScheduledTask -TaskName 'QuantAutoTradeGuard' -ErrorAction SilentlyContinue

    if ($guardTask -and $guardTask.Actions[0].Arguments -match '-Config\s+"?([^"\s]+)"?')
    {
        $Config = $Matches[1]
    }
}

if (-not $Mode)
{
    $configPath = if ([IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $repo $Config }
    $paper = [bool](Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json).kis.is_paper
}

$modeLabel = if ($paper) { '모의' } else { '실계좌' }

# ── 두 시간표 ──
if ($paper)
{
    $plan = [ordered]@{
        'Quant Premarket Leaders' = '08:02'
        'Quant Basket Targets'  = '08:40'
        'Quant Premarket Leaders Eval' = '15:45'
        'Quant Market Close AutoDoc'     = '16:05'
        'Quant Maintain Daily'  = '16:20'
        'Quant Minute Backfill' = '16:40'
        'Quant Daily Bars'      = '16:50'
        'Quant Study35 Watch'   = '17:00'
        'Quant Study42 Plan'    = '17:10'
        'Quant Daily Trade Report' = '20:10'
        'claude_stock_study'    = '20:30'
        'claude_dashboard_sync' = '21:10'
        'Quant Maintain Weekly' = '21:20'
    }
    $until = '15:35'      # 감시견이 트레이더를 더 띄우지 않는 시각(매매 끝 15:30 + 여유)
    $guardHours = 8.25    # 07:30 부터 5분마다 → 15:45 까지
}
else
{
    $plan = [ordered]@{
        'Quant Premarket Leaders' = '08:02'
        'Quant Basket Targets'  = '08:40'
        'Quant Premarket Leaders Eval' = '15:45'
        'Quant Market Close AutoDoc'     = '20:30'
        'Quant Daily Trade Report' = '20:35'
        'Quant Maintain Daily'  = '20:45'
        'Quant Minute Backfill' = '21:00'
        'Quant Daily Bars'      = '21:05'
        'Quant Study35 Watch'   = '21:15'
        'Quant Study42 Plan'    = '21:25'
        'claude_stock_study'    = '21:10'
        'claude_dashboard_sync' = '21:40'
        'Quant Maintain Weekly' = '21:50'
    }
    $until = '20:05'      # 애프터마켓 20:00 + 여유
    $guardHours = 12.75   # 07:30 부터 → 20:15 까지
}

# 감시견 시작 시각. 엔진이 떠서 08:00(NXT 개장) 전에 전 종목 일봉을 미리 받아 두게 07:30으로 당긴다(D-147).
$guardOpen = '07:30'

# 실계좌 감시견은 모의 감시견과 따로 평일마다 돈다(config instance=live → 예약작업 이름 QuantAutoTradeGuard_live, D-122).
#  모의 감시견이 07:30에 빌드한 exe를 -NoBuild로 쓰고 10분 뒤 연다 — 빌드 폴더 하나를 둘이 동시에 빌드하지 않게.
#  [why 10-15: 날마다 손으로 만들던 일회성 예약이 빠진 날 실계좌가 뜨지 않았다]
$liveGuardName = 'QuantAutoTradeGuard_live'
$liveGuardConfig = 'Quant\config\config_live.json'
$liveGuardOpen = '07:40'
$liveGuardUntil = '20:05'     # 애프터마켓 20:00 + 여유(D-097)
$liveGuardHours = 12.5        # 07:40 부터 → 20:10 까지

$fix = @{
    'Quant Premarket Leaders' = 'py research\studies\27_premarket_leaders\premarket_leaders.py snapshot (08:57까지 5분마다)'
    'Quant Basket Targets'  = 'py PYQuant\main.py basket --no-momentum --value-share 1.0'
    'Quant Premarket Leaders Eval' = 'py research\studies\27_premarket_leaders\premarket_leaders.py evaluate'
    'Quant Market Close AutoDoc'     = 'py scripts\market_close_autodoc.py'
    'Quant Daily Trade Report' = 'pythonw scripts\daily_trade_report.py (모의·실계좌 하루 매매 리포트 → _private\daily_reports, 애프터마켓 20:00 뒤)'
    'Quant Maintain Daily'  = 'py ..\quant-devtools\maintain.py --daily'
    'Quant Minute Backfill' = 'py scripts\market_close_minute_backfill.py'
    'Quant Daily Bars'      = 'py PYQuant\tools\naver_bars_backfill.py --refresh'
    'Quant Study35 Watch'   = 'py research\studies\35_surge_box_breakout\daily_watch.py --wait-minutes 45 --emit-plan (일봉 갱신을 기다림, 다음 거래일 SURGE_HOLD 계획을 씀)'
    'Quant Study42 Plan'    = 'py research\studies\42_swing_trend\candidates.py --nightly --wait-minutes 45 (일봉 갱신을 기다려 패널을 다시 만들고 다음 거래일 SWING_TREND 매수 계획을 씀)'
    'claude_stock_study'    = '/stock-study'
    'claude_dashboard_sync' = '/dashboard-sync'
    'Quant Maintain Weekly' = 'py ..\quant-devtools\maintain.py --weekly'
}

# ── 창 없이 도는지 ──
# conhost --headless 는 숨은 콘솔을 만들고 그 아래 자식도 같은 콘솔을 물려받는다. pythonw 는 콘솔이 없다.
#  powershell -WindowStyle Hidden 은 숨기기 전에 창이 잠깐 떠서 창을 띄우는 쪽으로 센다.
$windowCheckNames = @($plan.Keys) + @('QuantAutoTradeGuard', $liveGuardName)

function Test-ShowsWindow($task)
{
    $action = $task.Actions[0]
    $exeName = Split-Path -Leaf ([string]$action.Execute).Trim('"')

    if ($exeName -eq 'pythonw.exe')
    {
        return $false
    }

    return -not ($exeName -eq 'conhost.exe' -and ([string]$action.Arguments).TrimStart() -like '--headless*')
}

function Get-WindowTasks
{
    foreach ($name in $windowCheckNames)
    {
        $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue

        if ($task -and (Test-ShowsWindow $task))
        {
            $task
        }
    }
}

if ($Windows)
{
    foreach ($task in Get-WindowTasks)
    {
        "{0}`t{1}" -f $task.TaskName, $task.Actions[0].Execute
    }

    exit 0
}

if ($Lines)
{
    "{0}`t{1}`t{2}" -f 'QuantAutoTradeGuard', ('{0}~ 5분마다, -Until {1}, {2}h' -f $guardOpen, $until, $guardHours), 'powershell -File scripts\market_close_timetable.ps1 -Apply'
    "{0}`t{1}`t{2}" -f $liveGuardName, ('{0}~ 5분마다, -Until {1}, {2}h, -NoBuild' -f $liveGuardOpen, $liveGuardUntil, $liveGuardHours), 'powershell -File scripts\market_close_timetable.ps1 -Apply'

    foreach ($name in $plan.Keys)
    {
        "{0}`t{1}`t{2}" -f $name, $plan[$name], $fix[$name]
    }

    exit 0
}

# ── 실제 등록값 ──
function Get-TaskStart([string]$name)
{
    $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue

    if (-not $task)
    {
        return $null
    }

    return ([datetime]$task.Triggers[0].StartBoundary).ToString('HH:mm')
}

$guard = Get-ScheduledTask -TaskName 'QuantAutoTradeGuard' -ErrorAction SilentlyContinue
$guardUntil = if ($guard -and $guard.Actions[0].Arguments -match '-Until\s+(\S+)') { $Matches[1] } else { $null }
$guardStart = if ($guard) { ([datetime]$guard.Triggers[0].StartBoundary).ToString('HH:mm') } else { $null }
$guardDuration = if ($guard) { [Xml.XmlConvert]::ToTimeSpan($guard.Triggers[0].Repetition.Duration).TotalHours } else { $null }

$liveGuard = Get-ScheduledTask -TaskName $liveGuardName -ErrorAction SilentlyContinue
$liveGuardArgs = if ($liveGuard) { [string]$liveGuard.Actions[0].Arguments } else { '' }
$liveGuardStart = if ($liveGuard) { ([datetime]$liveGuard.Triggers[0].StartBoundary).ToString('HH:mm') } else { $null }
$liveGuardActualUntil = if ($liveGuardArgs -match '-Until\s+(\S+)') { $Matches[1] } else { $null }
$liveGuardDuration = if ($liveGuard) { [Xml.XmlConvert]::ToTimeSpan($liveGuard.Triggers[0].Repetition.Duration).TotalHours } else { $null }

Write-Host ("[market_close_timetable] 계좌 모드 {0} (config={1})" -f $modeLabel, $Config)
$drift = 0

foreach ($name in $plan.Keys)
{
    $actual = Get-TaskStart $name
    $mark = if ($actual -eq $plan[$name]) { '  ' } else { '!!'; $drift++ }
    Write-Host ("  {0} {1,-24} 예정 {2}  실제 {3}" -f $mark, $name, $plan[$name], $(if ($actual) { $actual } else { '(등록 없음)' }))
}

$guardOk = ($guardStart -eq $guardOpen) -and ($guardUntil -eq $until) -and ($guardDuration -eq $guardHours)
$mark = if ($guardOk) { '  ' } else { '!!'; $drift++ }
Write-Host ("  {0} {1,-24} 예정 {2}~ -Until {3} {4}h  실제 {5}~ -Until {6} {7}h" -f $mark, 'QuantAutoTradeGuard', $guardOpen, $until, $guardHours, $guardStart, $guardUntil, $guardDuration)

$liveGuardOk = ($liveGuardStart -eq $liveGuardOpen) -and ($liveGuardActualUntil -eq $liveGuardUntil) -and ($liveGuardDuration -eq $liveGuardHours) -and
    ($liveGuardArgs -like '*config_live.json*') -and ($liveGuardArgs -like '*-NoBuild*')
$mark = if ($liveGuardOk) { '  ' } else { '!!'; $drift++ }
Write-Host ("  {0} {1,-24} 예정 {2}~ -Until {3} {4}h -NoBuild  실제 {5}~ -Until {6} {7}h" -f $mark, $liveGuardName, $liveGuardOpen, $liveGuardUntil, $liveGuardHours, $liveGuardStart, $liveGuardActualUntil, $liveGuardDuration)

foreach ($task in Get-WindowTasks)
{
    $drift++
    Write-Host ("  !! {0,-24} 창을 띄운다 — 실행 {1}" -f $task.TaskName, $task.Actions[0].Execute)
}

if (-not $Apply)
{
    if ($drift -gt 0)
    {
        Write-Host "[market_close_timetable] 어긋남 $drift 건 — -Apply 로 맞춘다"
        exit 1
    }

    Write-Host "[market_close_timetable] 시간표 일치"
    exit 0
}

# ── 맞춘다 ──
foreach ($name in $plan.Keys)
{
    if ((Get-TaskStart $name) -eq $plan[$name])
    {
        continue
    }

    & schtasks /change /tn $name /st $plan[$name] | Out-Null

    if ($LASTEXITCODE -ne 0)
    {
        Write-Host "[market_close_timetable] schtasks /change 실패: $name"
        exit 1
    }

    Write-Host ("  바꿈 {0} → {1}" -f $name, $plan[$name])
}

if (-not $guardOk)
{
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'auto_trade_guard.ps1') -Install -Config $Config -Open $guardOpen -Until $until -Hours $guardHours
    Write-Host ("  감시견 재등록 {0}~ -Until {1} {2}h" -f $guardOpen, $until, $guardHours)
}

if (-not $liveGuardOk)
{
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'auto_trade_guard.ps1') -Install -Config $liveGuardConfig -Open $liveGuardOpen -Until $liveGuardUntil -Hours $liveGuardHours -NoBuild
    Write-Host ("  실계좌 감시견 재등록 {0}~ -Until {1} {2}h -NoBuild" -f $liveGuardOpen, $liveGuardUntil, $liveGuardHours)
}

# 감시견 재등록이 끝난 뒤에 본다 — 재등록이 이미 conhost --headless 로 건다.
foreach ($task in Get-WindowTasks)
{
    $action = $task.Actions[0]
    $exePath = ([string]$action.Execute).Trim('"')
    $headless = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument ("--headless `"{0}`" {1}" -f $exePath, $action.Arguments) -WorkingDirectory $action.WorkingDirectory
    Set-ScheduledTask -TaskName $task.TaskName -Action $headless | Out-Null
    Write-Host ("  창 없이 바꿈 {0} ({1} → conhost --headless)" -f $task.TaskName, (Split-Path -Leaf $exePath))
}

Write-Host "[market_close_timetable] 적용 끝 — .claude/hooks/cron-gate.ps1 는 이 스크립트의 -Lines 를 읽으므로 따로 고칠 것 없다"
exit 0
