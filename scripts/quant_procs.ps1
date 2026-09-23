<#
.SYNOPSIS
  이 저장소가 띄운 프로세스만 골라 상태를 보이고, 남으면 안 되는 것을 정리한다.

.DESCRIPTION
  Get-Process로 보면 powershell·python이 여러 줄 나오는데 어느 것이 매매용이고 어느 것이
  죽었어야 할 남은 프로세스인지 구분되지 않는다. 여기서는 명령줄로 역할을 붙이고, 부모-자식을 한
  인스턴스로 묶어(py → python → python은 하나다) 역할별로 몇 개가 떠 있는지만 본다.

  판정은 넷이다.
    정상    역할당 인스턴스 1개
    중복    역할당 2개 이상 — 가장 최근에 뜬 것을 남기고 옛 것을 내린다(고치고 재기동한 흔적)
    빈 창  -NoExit 창은 살아 있는데 그 안의 역할 프로세스가 죽은 것. 창만 남는다
    없음    역할 프로세스가 하나도 없다

  트레이더(quant_trader)는 중복이어도 자동으로 죽이지 않는다. 두 프로세스가 같은 계좌에
  발주하면 원장이 깨지는데 어느 쪽이 진짜인지 스크립트가 알 수 없다. 보고만 한다.

  -KillAll 은 판정과 무관하게 이 저장소가 띄운 것을 전부 내린다. 하루를 끝낼 때 쓴다. 이때는 어느
  트레이더가 진짜인지 가릴 필요가 없으므로 트레이더도 같이 내리고, 오늘 상태파일의 phase 를
  closed 로 적어 5분 주기 감시자가 되살리지 않게 한다(이 표시가 없으면 재부팅해도 장중에는
  몇 분 안에 다시 떠 있다).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1            # 현황만
  powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1 -Reap      # 중복·빈 창 정리
  powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1 -KillAll   # 전부 내리고 하루 종료
#>
[CmdletBinding()]
param(
  [switch]$Reap,            # 중복·빈 창을 실제로 내린다(없으면 현황만)
  [switch]$KillAll,         # 역할 프로세스·quant 창을 전부 내리고 감시자 재기동을 막는다
  [switch]$IncludeTrader,   # 트레이더 중복도 정리 대상에 넣는다(기본 제외 — 원장 위험)
  [switch]$Quiet,           # 표를 찍지 않고 정리 결과만
  # 한 기계에서 계좌를 둘 돌리는 날(모의 비교군 + 실계좌)에 쓴다. -KillAll 은 원래 이 기계의 quant
  #  프로세스를 전부 내린다 — 그러면 15:35에 마감하는 모의 감시견이 20:00까지 도는 실계좌 트레이더와
  #  그 부속 창까지 같이 내린다. OwnerPid 를 주면 그 감시견의 자손만 내린다. [why D-122]
  [int]$OwnerPid = 0,
  [string]$Instance = ""    # 상태 파일 이름 접미(_auto_trade_day_live.json). 비면 예전 이름
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 역할 판정표 — 명령줄에 이 조각이 있으면 그 역할이다. 위에서부터 먼저 맞는 것.
$Roles = @(
  @{ role = "trader";    match = "quant_trader.exe" },
  @{ role = "watchdog";  match = "auto_trade_day.ps1" },
  @{ role = "guard";     match = "auto_trade_guard.ps1" },
  @{ role = "regime_feed";   match = "macro_regime_feed.py" },
  @{ role = "dashboard"; match = "dashboard_server.py" },
  @{ role = "notify";    match = "notify_trades.py" },
  @{ role = "universe";  match = "universe_feed.py" },
  @{ role = "prices";    match = "live_prices_feed.py" },
  @{ role = "recorder";  match = "main.py record" }   # ZMQ 틱 기록기(auto_trade_day가 띄움)
)

$all  = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine })
$byId = @{}
foreach ($p in $all) { $byId[[int]$p.ProcessId] = $p }

function Get-QRole([string]$cmd) {
  foreach ($r in $Roles) { if ($cmd -like ("*" + $r.match + "*")) { return $r.role } }
  return $null
}

# 갈라 띄운 날에는 quant_trader.exe 가 둘이다 — 주문 쪽과 전략 쪽. 명령줄의 --role 로 가른다.
#  안 가르면 성한 짝 하나가 "중복"으로 찍히고, -Reap -IncludeTrader 가 그걸 내린다. [why D-114]
function Get-QVariant([string]$cmd) {
  if ($cmd -match '--role[=\s]+(order|strategy|both)') { return $Matches[1].ToLower() }
  return ""
}

function Get-Ancestors([int]$start) {
  # 자기 자신은 빼고 위로 올라간 조상 PID 목록. 사슬이 끊기거나 20홉이면 멈춘다.
  $out = @(); $cur = $start; $hop = 0
  while ($hop -lt 20) {
    if (-not $byId.ContainsKey($cur)) { break }
    $cur = [int]$byId[$cur].ParentProcessId
    if (-not $cur) { break }
    $out += $cur; $hop++
  }
  return $out
}

$selfChain = @($PID) + (Get-Ancestors $PID)

$tagged = @()
foreach ($p in $all) {
  $role = Get-QRole $p.CommandLine
  if (-not $role) { continue }
  $pid_ = [int]$p.ProcessId
  # 이 스크립트 자신과 그 조상은 건드리지 않는다.
  if ($selfChain -contains $pid_) { continue }
  $anc = Get-Ancestors $pid_
  if (($anc | Where-Object { $selfChain -contains $_ }).Count -gt 0 -and $role -ne "trader") { }
  # 묶고 세는 열쇠는 RoleKey 다(trader(order)·trader(strategy)). 내리는 판정은 Role 그대로 본다.
  $variant = Get-QVariant $p.CommandLine
  $tagged += [pscustomobject]@{
    QPid = $pid_; QPpid = [int]$p.ParentProcessId; Name = $p.Name
    Role = $role; Variant = $variant
    RoleKey = $(if ($variant -and $variant -ne "both") { "$role($variant)" } else { $role })
    Start = $p.CreationDate; Anc = $anc
  }
}

# 인스턴스 묶기 — 조상 중에 같은 역할이 이미 있으면 그건 자식이다(py → python → python은 하나).
$sameRole = @{}
foreach ($t in $tagged) { $sameRole[$t.QPid] = $t.RoleKey }
$roots = @()
foreach ($t in $tagged) {
  $isChild = $false
  foreach ($a in $t.Anc) { if ($sameRole.ContainsKey($a) -and $sameRole[$a] -eq $t.RoleKey) { $isChild = $true; break } }
  if (-not $isChild) { $roots += $t }
}

# 빈 창 창 — Start-Window로 띄운 quant-* 제목의 -NoExit 셸인데 안의 역할 프로세스가 없다.
$shells = @()
$allShells = @()   # 안이 살아 있든 아니든 quant-* 창 전부 — KillAll 이 쓴다
foreach ($p in $all) {
  if ($p.Name -ne "powershell.exe" -and $p.Name -ne "pwsh.exe") { continue }
  $pid_ = [int]$p.ProcessId
  if ($selfChain -contains $pid_) { continue }
  if ($p.CommandLine -notlike "*RawUI.WindowTitle='quant-*") { continue }
  if (Get-QRole $p.CommandLine) { continue }        # 역할이 직접 붙은 셸은 위에서 이미 잡혔다
  $shellTitle = if ($p.CommandLine -match "WindowTitle='([^']+)'") { $Matches[1] } else { "quant-?" }
  $allShells += [pscustomobject]@{ QPid = $pid_; Title = $shellTitle; Start = $p.CreationDate; Anc = (Get-Ancestors $pid_) }
  $alive = @($tagged | Where-Object { $_.Anc -contains $pid_ })
  if ($alive.Count -eq 0) {
    $title = if ($p.CommandLine -match "WindowTitle='([^']+)'") { $Matches[1] } else { "quant-?" }
    $shells += [pscustomobject]@{ QPid = $pid_; Title = $title; Start = $p.CreationDate }
  }
}

# ─────────────── 판정 ───────────────
$rows = @()
$dupRoots = @()
foreach ($g in @($roots | Group-Object RoleKey)) {
  $ordered = @($g.Group | Sort-Object Start)
  $keep = $ordered[-1]
  foreach ($i in $ordered) {
    $v = if ($g.Count -le 1) { "정상" } elseif ($i.QPid -eq $keep.QPid) { "정상(최신)" } else { "중복"; }
    if ($g.Count -gt 1 -and $i.QPid -ne $keep.QPid) { $dupRoots += $i }
    $kids = @($tagged | Where-Object { $_.RoleKey -eq $i.RoleKey -and $_.Anc -contains $i.QPid }).Count
    $rows += [pscustomobject]@{
      역할 = $i.RoleKey; PID = $i.QPid; 자식 = $kids
      기동 = $i.Start.ToString("HH:mm:ss"); 판정 = $v
    }
  }
}
foreach ($s in $shells) {
  $rows += [pscustomobject]@{ 역할 = "shell:$($s.Title)"; PID = $s.QPid; 자식 = 0; 기동 = $s.Start.ToString("HH:mm:ss"); 판정 = "빈 창" }
}
foreach ($r in $Roles) {
  if (@($roots | Where-Object { $_.Role -eq $r.role }).Count -eq 0) {
    $rows += [pscustomobject]@{ 역할 = $r.role; PID = "-"; 자식 = "-"; 기동 = "-"; 판정 = "없음" }
  }
}

if (-not $Quiet) {
  Write-Host ""
  Write-Host ("  Quant 프로세스 현황 " + (Get-Date -Format 'HH:mm:ss'))
  ($rows | Sort-Object 역할, 기동 | Format-Table -AutoSize | Out-String -Width 120) | Write-Host
}

# ─────────────── 전부 내리기 ───────────────
if ($KillAll) {
  # OwnerPid 가 있으면 그 감시견이 띄운 것만 내린다. 트레이더도 창도 감시견의 자손이라 이 하나로
  #  갈린다 — 다른 계좌의 감시견·트레이더·부속 창은 건드리지 않는다. [why D-122]
  $mine = { param($item) $OwnerPid -eq 0 -or $item.Anc -contains $OwnerPid }

  $victims = @()
  foreach ($t in $tagged)    { if (& $mine $t) { $victims += [pscustomobject]@{ Pid = $t.QPid; What = $t.Role } } }
  foreach ($s in $allShells) { if (& $mine $s) { $victims += [pscustomobject]@{ Pid = $s.QPid; What = "창 $($s.Title)" } } }

  $killed = 0
  # 자식이 먼저 죽어야 부모 셸이 재기동 로직을 타지 않는다. 나중에 뜬 것부터 내린다.
  foreach ($v in ($victims | Sort-Object Pid -Unique | Sort-Object { -$_.Pid })) {
    try { Stop-Process -Id $v.Pid -Force -ErrorAction Stop; $killed++; Write-Host "  [종료] $($v.What) pid=$($v.Pid)" }
    catch { }
  }

  # 감시자가 5분 뒤 되살리지 않도록 오늘 상태에 종료를 남긴다.
  $repo   = Split-Path -Parent $PSScriptRoot
  $suffix = if ($Instance) { "_$Instance" } else { "" }
  $status = Join-Path $repo ("_private\_auto_trade_day{0}.json" -f $suffix)
  if (Test-Path $status) {
    try {
      $st = Get-Content $status -Raw -Encoding UTF8 | ConvertFrom-Json
      $st.phase   = "closed"
      $st.updated = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
      $st | ConvertTo-Json -Depth 6 | Set-Content $status -Encoding UTF8
      Write-Host "  [표시] phase=closed — 오늘은 감시자가 되살리지 않는다."
    } catch {
      Write-Host "  [경고] 상태파일을 고치지 못했다: $($_.Exception.Message)" -ForegroundColor Yellow
      Write-Host "         장중이면 5분 안에 다시 뜬다. 손으로 phase 를 closed 로 바꾼다: $status" -ForegroundColor Yellow
    }
  }

  Write-Host "  종료 $killed 개."
  exit 0
}

if (-not $Reap) { exit 0 }

# ─────────────── 정리 ───────────────
$killed = 0
foreach ($i in $dupRoots) {
  if ($i.Role -eq "trader" -and -not $IncludeTrader) {
    Write-Host "  [보류] trader pid=$($i.QPid) 중복 — 원장이 깨질 수 있어 자동으로 내리지 않는다." -ForegroundColor Yellow
    continue
  }
  $tree = @($i.QPid) + @($tagged | Where-Object { $_.Anc -contains $i.QPid } | ForEach-Object { $_.QPid })
  foreach ($k in ($tree | Select-Object -Unique)) {
    try { Stop-Process -Id $k -Force -ErrorAction Stop; $killed++; Write-Host "  [정리] 중복 $($i.Role) pid=$k" } catch { }
  }
}
foreach ($s in $shells) {
  try { Stop-Process -Id $s.QPid -Force -ErrorAction Stop; $killed++; Write-Host "  [정리] 빈 창 창 $($s.Title) pid=$($s.QPid)" } catch { }
}
Write-Host "  정리 $killed 개."
exit 0
