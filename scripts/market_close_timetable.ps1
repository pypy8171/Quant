# 마감 자동화 시간표 — 계좌 모드(kis.is_paper)로 두 시간표 중 하나를 고르고, 예약작업·감시견이 그 시간표대로 걸려 있는지
# 보거나(-Check, 기본) 그대로 맞춘다(-Apply).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1                 # 예정 vs 실제, 어긋나면 exit 1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Apply          # schtasks 시각 변경 + 감시견 재등록
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Lines          # "이름<TAB>HH:MM<TAB>대응" (cron-gate 훅이 읽는다)
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/market_close_timetable.ps1 -Lines -Mode live   # config 안 읽고 그 모드 시간표만 (gen_facts·허브 생성기가 두 모드를 뽑는다)
# 규칙:
#   - 모의(is_paper=true): KIS 모의 서버가 15:30 뒤 주문을 거부한다(T-18, 2026-09-18 실측). 매매 끝 15:30, 마감 루틴 16:00대.
#   - 실계좌(is_paper=false): 애프터마켓 16:00~20:00(D-097)까지 매매, 마감 루틴 20:30 시작. 실계좌 애프터마켓 주문은
#     검색으로 파악한 것이고 실증이 없다 — 전환 뒤 첫날 16:00 넘어 체결 1건을 눈으로 확인한다.
#   - 계좌 모드는 감시견 예약작업(QuantAutoTradeGuard)이 넘기는 config의 kis.is_paper 로 읽는다. 최상위·quote_kis 의 is_paper 는
#     시세 계정이라 보지 않는다(scripts/auto_trade_day.ps1 와 같은 이유).
#   - 클로드를 부르는 두 작업(claude_stock_study·claude_dashboard_sync)은 세션 사용량 한도(17시 리셋) 때문에 모의에서도 20:30 뒤다.
#   - 순서는 고정: 바스켓 비중표(08:40, 장 전) → 장 마감 AutoDoc → Maintain Daily → Minute Backfill → stock_study → dashboard_sync → Maintain Weekly(금).
param(
    [string]$Config = $null,
    [switch]$Apply,
    [switch]$Lines,
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
    $paper = [bool](Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json).kis.is_paper
}

$modeLabel = if ($paper) { '모의' } else { '실계좌' }

# ── 두 시간표 ──
if ($paper)
{
    $plan = [ordered]@{
        'Quant Basket Targets'  = '08:40'
        'Quant Market Close AutoDoc'     = '16:05'
        'Quant Maintain Daily'  = '16:20'
        'Quant Minute Backfill' = '16:40'
        'claude_stock_study'    = '20:30'
        'claude_dashboard_sync' = '21:10'
        'Quant Maintain Weekly' = '21:20'
    }
    $until = '15:35'      # 감시견이 트레이더를 더 띄우지 않는 시각(매매 끝 15:30 + 여유)
    $guardHours = 7       # 08:45 부터 5분마다 → 15:45 까지
}
else
{
    $plan = [ordered]@{
        'Quant Basket Targets'  = '08:40'
        'Quant Market Close AutoDoc'     = '20:30'
        'Quant Maintain Daily'  = '20:45'
        'Quant Minute Backfill' = '21:00'
        'claude_stock_study'    = '21:10'
        'claude_dashboard_sync' = '21:40'
        'Quant Maintain Weekly' = '21:50'
    }
    $until = '20:05'      # 애프터마켓 20:00 + 여유
    $guardHours = 11.5    # 08:45 부터 → 20:15 까지
}

$fix = @{
    'Quant Basket Targets'  = 'py PYQuant\main.py basket'
    'Quant Market Close AutoDoc'     = 'py scripts\market_close_autodoc.py'
    'Quant Maintain Daily'  = 'py scripts\maintain.py --daily'
    'Quant Minute Backfill' = 'py scripts\market_close_minute_backfill.py'
    'claude_stock_study'    = '/stock-study'
    'claude_dashboard_sync' = '/dashboard-sync'
    'Quant Maintain Weekly' = 'py scripts\maintain.py --weekly'
}

if ($Lines)
{
    "{0}`t{1}`t{2}" -f 'QuantAutoTradeGuard', ('08:45~ 5분마다, -Until {0}, {1}h' -f $until, $guardHours), 'powershell -File scripts\market_close_timetable.ps1 -Apply'

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
$guardDuration = if ($guard) { [Xml.XmlConvert]::ToTimeSpan($guard.Triggers[0].Repetition.Duration).TotalHours } else { $null }

Write-Host ("[market_close_timetable] 계좌 모드 {0} (config={1})" -f $modeLabel, $Config)
$drift = 0

foreach ($name in $plan.Keys)
{
    $actual = Get-TaskStart $name
    $mark = if ($actual -eq $plan[$name]) { '  ' } else { '!!'; $drift++ }
    Write-Host ("  {0} {1,-24} 예정 {2}  실제 {3}" -f $mark, $name, $plan[$name], $(if ($actual) { $actual } else { '(등록 없음)' }))
}

$guardOk = ($guardUntil -eq $until) -and ($guardDuration -eq $guardHours)
$mark = if ($guardOk) { '  ' } else { '!!'; $drift++ }
Write-Host ("  {0} {1,-24} 예정 -Until {2} {3}h  실제 -Until {4} {5}h" -f $mark, 'QuantAutoTradeGuard', $until, $guardHours, $guardUntil, $guardDuration)

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
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'auto_trade_guard.ps1') -Install -Config $Config -Until $until -Hours $guardHours
    Write-Host ("  감시견 재등록 -Until {0} {1}h" -f $until, $guardHours)
}

Write-Host "[market_close_timetable] 적용 끝 — .claude/hooks/cron-gate.ps1 는 이 스크립트의 -Lines 를 읽으므로 따로 고칠 것 없다"
exit 0
