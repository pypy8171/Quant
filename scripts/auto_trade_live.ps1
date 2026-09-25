<#
.SYNOPSIS
  실계좌 하루 루프 진입점 — 계좌·한도·이중기동을 기동 전에 확인하고 auto_trade_day.ps1 에 넘긴다.

.DESCRIPTION
  auto_trade_day.ps1 은 -Config 만 바꾸면 실계좌도 돌린다. 기능이 모자라서 이 스크립트를 두는 것이
  아니라, 그 한 줄을 손으로 칠 때마다 틀릴 수 있는 것이 셋이어서다.

    1. 모의 설정을 실계좌로 착각한다 — 파일 이름만 보고는 안 갈린다. is_paper 를 읽어 가른다.
    2. -Until 을 기본값 15:35 로 둔다 — 실계좌는 애프터마켓 20:00 까지라(D-097) 4시간 반을 놓친다.
    3. 이미 떠 있는 엔진 위에 하나 더 띄운다 — 같은 계좌에 엔진 둘이면 같은 신호로 주문이 두 번 간다.

  확인만 하고 나머지는 그대로 넘긴다. 부속 창·재기동·마감 정리는 전부 auto_trade_day.ps1 의 일이다.
  모르는 인자는 그대로 전달하므로 -NoBuild·-NoDashboard 같은 스위치를 그대로 쓸 수 있다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_live.ps1
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_live.ps1 -Yes       # 확인 입력 없이 바로
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_live.ps1 -DryRun    # 무엇을 할지만 찍는다
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_live.ps1 -NoBuild   # 재빌드 건너뛰기(그대로 전달)
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_live.json",
  [string]$Until = "20:05",          # 애프터마켓 20:00 + 여유 5분 (D-097)
  [switch]$Yes,                      # 확인 입력을 건너뛴다(예약 실행처럼 사람이 없는 자리)
  [switch]$DryRun,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$Passthrough
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONUTF8 = "1"

# 저장소 루트로. 경로 리터럴을 박지 않고 스크립트 위치에서 유도한다 — 사용자 폴더에 한글이 들어 있다.
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

function Line([string]$label, $value)
{
  "  {0,-14} {1}" -f $label, $value
}

# ─────────────── 1. 설정을 읽는다 ───────────────
if (-not (Test-Path $Config))
{
  Write-Host "[중단] 설정 파일이 없다: $Config" -ForegroundColor Red
  exit 2
}

# -Encoding UTF8 은 빼면 안 된다. PowerShell 5.1 의 기본값은 시스템 ANSI(cp949)라, BOM 없는
#  UTF-8 인 config 의 한글 주석이 깨지면서 닫는 따옴표를 먹고 ConvertFrom-Json 이 통째로 실패한다.
try
{
  $configJson = Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json
}
catch
{
  Write-Host "[중단] $Config 을 JSON 으로 읽지 못했다 — $($_.Exception.Message)" -ForegroundColor Red
  Write-Host "       (본문에 앱키가 있으므로 오류 전문을 그대로 남에게 붙여넣지 않는다)" -ForegroundColor DarkYellow
  exit 2
}

$isPaper = $configJson.kis.is_paper

if ($null -eq $isPaper)
{
  Write-Host "[중단] $Config 에서 kis.is_paper 를 읽지 못했다 — 계좌 모드를 모른 채 띄우지 않는다." -ForegroundColor Red
  exit 2
}

if ([bool]$isPaper)
{
  Write-Host "[중단] 이 스크립트는 실계좌 전용인데 $Config 은 모의 설정이다(is_paper=true)." -ForegroundColor Red
  Write-Host "       모의는 scripts\auto_trade_day.ps1 을 인자 없이 띄운다." -ForegroundColor DarkYellow
  exit 2
}

# 실계좌 중지 표시가 있으면 띄우지 않는다. 표시 파일에 이유와 재개 조건을 적고, 조건을 채운 뒤 사람이
#  손으로 지운다 — 이 스크립트는 지우지 않는다. [why 전수조사 09-25: 매수·매도 선점이 한 칸이라 DevScale 이 실계좌에서 새다]
$haltFile = Join-Path $Repo "_private\state\live_halt.txt"

if (Test-Path $haltFile)
{
  Write-Host "[중단] 실계좌 중지 표시가 있다: $haltFile" -ForegroundColor Red
  Get-Content $haltFile -Encoding UTF8 | ForEach-Object { Write-Host "       $_" -ForegroundColor DarkYellow }
  exit 3
}

# ─────────────── 2. 무엇으로 띄우는지 보여 준다 ───────────────
$risk = $configJson.risk
$strategy = $configJson.strategies[0]

Write-Host ""
Write-Host "═══ 실계좌 자동매매 ═══" -ForegroundColor Yellow
Line "설정" $Config
Line "인스턴스" "$($configJson.instance)  (로그·상태 파일에 이 이름이 붙는다)"
Line "계좌" "$($configJson.kis.account_no)-$($configJson.kis.account_type)  거래소 $($configJson.kis.exchange)"
Line "앱키" "$($configJson.kis.app_key.Substring(0,8))…  (앞 8자만 — 전문은 화면에 찍지 않는다)"
Line "운영단말" "포트 $($configJson.ops_port)"
Write-Host ""
Line "정규장" ("{0:D4} ~ {1:D4}" -f $risk.session_open_hhmm, $risk.session_close_hhmm)

if ($risk.after_market)
{
  Line "애프터" ("{0:D4} ~ {1:D4}   (주문구분 41 지정가 + 거래소 KRX, 시장가는 1% 물러선 지정가로, D-097)" -f $risk.after_open_hhmm, $risk.after_close_hhmm)
}

Line "감시 종료" $Until
Write-Host ""
Line "주문당 상한" ("{0:N0}원 · {1}주" -f $risk.max_notional_per_order, $risk.max_qty_per_order)
Line "종목당 상한" ("{0:N0}원 · {1}주" -f $risk.max_notional_per_ticker, $risk.max_qty_per_ticker)
Line "동시 보유" ("{0}종목   (이월 보유도 이 수를 먹는다)" -f $risk.max_concurrent_positions)
Line "총노출 한도" ("자본의 {0}배" -f $risk.max_gross_exposure_pct)
Line "일손실 한도" ("{0:N0}원   (넘으면 그날 신규 진입이 멈춘다)" -f $risk.daily_loss_limit)

if ($strategy)
{
  Line "전략" "$($strategy.type)  가격대 $('{0:N0}' -f $strategy.min_price) ~ $('{0:N0}' -f $strategy.max_price)원"
  Line "한 건 규모" ("{0:N0} ~ {1:N0}원" -f $strategy.notional_floor_krw, $strategy.notional_cap_krw)
  Line "청산선" ("손절 {0}% · 익절 평단 +{1}%" -f $strategy.stop_loss_pct, $strategy.dev_sell_pct)
}

Write-Host ""

# ─────────────── 3. 이미 도는 것이 있는지 ───────────────
# 같은 계좌에 엔진이 둘이면 같은 신호로 주문이 두 번 나간다. 트레이더도 감시견도 본다 —
#  감시견이 살아 있으면 트레이더가 잠깐 죽어 있어도 곧 다시 띄우기 때문이다.
$blocked = $false

foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='quant_trader.exe'" -ErrorAction SilentlyContinue))
{
  $sameConfig = $process.CommandLine -and $process.CommandLine.Contains((Split-Path $Config -Leaf))

  if ($sameConfig)
  {
    Write-Host "[중단] 같은 설정으로 트레이더가 이미 돈다 — pid=$($process.ProcessId)" -ForegroundColor Red
    Write-Host "       엔진이 둘이면 같은 신호로 주문이 두 번 간다. 먼저 그 감시견 창을 닫는다." -ForegroundColor DarkYellow
    $blocked = $true
  }
  else
  {
    Write-Host "[알림] 다른 설정의 트레이더가 돈다 — pid=$($process.ProcessId) (계좌가 다르면 같이 돌아도 된다)" -ForegroundColor DarkYellow
  }
}

foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue))
{
  if (-not $process.CommandLine -or $process.ProcessId -eq $PID)
  {
    continue
  }

  if ($process.CommandLine -notmatch 'auto_trade_day\.ps1|auto_trade_live\.ps1')
  {
    continue
  }

  $sameConfig = $process.CommandLine.Contains((Split-Path $Config -Leaf))
  $noTrader = $process.CommandLine -match '-NoTrader'

  if ($sameConfig)
  {
    Write-Host "[중단] 같은 설정의 감시견이 이미 돈다 — pid=$($process.ProcessId)" -ForegroundColor Red
    $blocked = $true
  }
  elseif (-not $noTrader)
  {
    # 부속 창(대시보드·시세·리코더)을 두 감시견이 나눠 가지며 서로 죽인다. 계좌가 달라도 정리하는 편이 낫다.
    Write-Host "[알림] 다른 설정의 감시견이 돈다 — pid=$($process.ProcessId)" -ForegroundColor DarkYellow
    Write-Host "       부속 창을 두고 서로 다툰다. 그 창을 먼저 닫기를 권한다:  Stop-Process -Id $($process.ProcessId) -Force" -ForegroundColor DarkYellow
  }
}

if ($blocked)
{
  exit 3
}

# ─────────────── 4. 사람 확인 ───────────────
if (-not $Yes -and -not $DryRun)
{
  Write-Host "실제 돈이 나간다. 위 계좌·한도가 맞으면 " -NoNewline
  Write-Host "live" -ForegroundColor Yellow -NoNewline
  Write-Host " 를 치고 Enter (그 밖에는 취소): " -NoNewline
  $answer = Read-Host

  if ($answer -ne "live")
  {
    Write-Host "[취소] 아무것도 띄우지 않았다." -ForegroundColor DarkYellow
    exit 1
  }
}

# ─────────────── 5. 넘긴다 ───────────────
# 해시테이블로 넘긴다. 배열을 펼치면(@arguments) "-Config" 가 파라미터 이름이 아니라 위치 인자로
#  붙어 바인딩이 깨진다. 남은 인자(-NoBuild 같은 스위치)만 배열로 뒤에 붙인다.
$parameters = @{
  Config = $Config
  Until  = $Until
}

if ($DryRun)
{
  $parameters["DryRun"] = $true
}

# 남은 인자(-NoBuild 같은 스위치, -Until 15:35 같은 값 인자)도 같은 해시테이블에 넣는다.
#  배열로 펼치면(@Passthrough) PowerShell 이 "-NoBuild" 를 파라미터 이름이 아니라 위치 인자
#  문자열로 넘겨 바인딩이 깨진다 — 2026-09-23 첫 DryRun 이 이걸로 막혔다.
for ($index = 0; $index -lt $Passthrough.Count; $index++)
{
  $token = $Passthrough[$index]

  if ($token -notmatch '^-{1,2}(\w+)$')
  {
    Write-Host "[중단] 넘길 인자를 알아보지 못했다: $token" -ForegroundColor Red
    exit 2
  }

  $name = $Matches[1]
  $next = if ($index + 1 -lt $Passthrough.Count) { $Passthrough[$index + 1] } else { $null }

  # 다음 토큰이 또 다른 인자 이름이면 이건 스위치다. 아니면 그 토큰이 이 인자의 값이다.
  if ($null -ne $next -and $next -notmatch '^-{1,2}\w+$')
  {
    $parameters[$name] = $next
    $index++
  }
  else
  {
    $parameters[$name] = $true
  }
}

$shown = "-Config $Config -Until $Until" + $(if ($DryRun) { " -DryRun" } else { "" }) +
         $(if ($Passthrough) { " " + ($Passthrough -join " ") } else { "" })
Write-Host ""
Write-Host "→ scripts\auto_trade_day.ps1 $shown" -ForegroundColor Cyan
Write-Host ""

& (Join-Path $PSScriptRoot "auto_trade_day.ps1") @parameters
