<#
.SYNOPSIS
  하루치 자동매매 루프 — 보조 프로세스·유니버스·대시보드를 띄우고 트레이더를 장 마감까지 감시·재기동한다.

.DESCRIPTION
  세 창을 손으로 여는 절차(/intraday-start)를 한 창으로 접었다. 이 스크립트는 사람 판단이
  필요 없는 부분만 맡는다 — 기동, 죽으면 다시 띄우기, 마감에 내리고 사실 문서까지.
  코드 결함 진단·수정·재빌드는 /auto-trade-day 커맨드(클로드)가 이 스크립트 위에서 한다.

  진행 상태는 매 전이마다 _private/_auto_trade_day.json 에 적는다. 클로드가 로그 전체를
  훑지 않고 이 파일만 읽어 지금 몇 번째 세션인지, 왜 죽었는지 알 수 있게 하려는 것이다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -Config Quant\config\config.json -Until 15:35
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -DryRun
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -NoTrader   # 트레이더는 리눅스(WSL)에서 손으로 띄우는 날
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_dev_paper.json",
  [string]$Until = "15:35",          # 이 시각을 넘으면 재기동하지 않는다. 모의는 15:30이 매매 끝. 실계좌 전환 때 20:05(애프터마켓 20:00 + 여유, D-097·T-18)
  [switch]$NoRegimeFeed,
  [switch]$NoUniverse,
  [switch]$NoDashboard,
  [switch]$NoNotify,                 # 체결·포지션 메신저 알림 창을 띄우지 않는다
  [switch]$NoPrices,                 # 전 종목 시세 파일 전달(네이버 벌크) 창을 띄우지 않는다
  [switch]$NoRecorder,               # ZMQ 체결·주문을 TimescaleDB에 적재하는 창을 띄우지 않는다
  [switch]$NoMarketClose,                    # 마감 뒤 사실 문서·대시보드 갱신을 건너뛴다
  [switch]$NoBuild,                  # 기동 전 재빌드를 건너뛴다(exe를 손으로 바꾼 날). 이때는 소스가 exe보다 새면 중단
  [switch]$NoTrader,                 # 트레이더를 이 창이 띄우지 않는다(리눅스 등 다른 곳이 띄우는 날). 부속 창·유니버스 갱신·마감 정리는 그대로
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONUTF8 = "1"

# ─────────────── 경로 ───────────────
# quant_trader는 repo 루트에서 떠야 한다. cwd가 다르면 유니버스 파일을 못 찾아 조용히 붕괴한다.
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo
# KIS 토큰 캐시를 C++ 트레이더와 파이썬 부속 프로세스가 한 파일로 쓰게 한다. 기본값이 서로 달라
# (C++는 cwd, 파이썬은 Quant\config) 부속 프로세스가 먼저 토큰을 받으면 10초 뒤 트레이더의 발급이
# 1분 1회 제한에 걸려 403 → 크래시 루프가 됐다(09-18 08:30 실측). 자식 창·트레이더가 모두 물려받는다.
$env:KIS_TOKEN_CACHE_DIR = Join-Path $Repo "Quant\config"

# 한 기계에서 계좌를 둘 돌리는 날(모의 비교군 + 실계좌)에는 두 감시견이 상태 파일·실행 로그·
#  엔진 로그 폴더를 공유해 서로를 덮어썼다. config `instance`가 있으면 그 이름을 전부에 붙인다.
#  없으면 예전 이름 그대로 — 계좌 하나만 돌리는 날은 아무것도 안 바뀐다. [why D-122]
# config 를 읽을 때는 -Encoding UTF8 을 반드시 붙인다. PowerShell 5.1 의 Get-Content 기본값은
#  시스템 ANSI(한국어 Windows 는 cp949) 라, BOM 없는 UTF-8 인 config 의 한글 주석이 깨지면서
#  닫는 따옴표가 사라져 ConvertFrom-Json 이 통째로 실패한다. 2026-09-23 실계좌 첫 기동이
#  config_live.json 의 "//risk_100만" 키에서 이걸로 막혔다. 이 파일의 JSON 읽기 전부가 같다.
$Instance = ""
try { $Instance = [string](Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).instance } catch { }
$Suffix = if ($Instance) { "_$Instance" } else { "" }

$Exe     = Join-Path $Repo "Quant\build_win\quant_trader.exe"
$VenvPy  = Join-Path $Repo "PYQuant\.venv-win\Scripts\python.exe"
$Status  = Join-Path $Repo ("_private\_auto_trade_day{0}.json" -f $Suffix)
$LogDir  = Join-Path $Repo "logs"
$RunLog  = Join-Path $LogDir ("auto_trade_day{0}_{1}.log" -f $Suffix, (Get-Date -Format yyyyMMdd))
New-Item -ItemType Directory -Force -Path $LogDir, (Split-Path $Status) | Out-Null

# 엔진 로그 폴더. 엔진은 기본으로 exe 옆 logs\ 를 쓰는데 exe가 하나뿐이라 두 계좌가 같은
#  quant_trader.log·trades_*.csv·open_orders.txt 를 쓴다. 그러면 재기동 때 남의 계좌 미체결을
#  취소하려 들고, 알림·대시보드·건전성 판정이 두 계좌를 합산한다. 부속 파이썬 창도 이 값을
#  물려받아 같은 폴더를 본다(scripts/_logdir.py 가 QUANT_LOG_DIR 를 가장 먼저 본다).
if ($Instance) { $env:QUANT_LOG_DIR = Join-Path $Repo ("Quant\build_win\logs_{0}" -f $Instance) }

# ─────────────── .env 로드 ───────────────
# TimescaleDB 비밀번호 등은 소스에 심지 않는다 — 리포 루트 .env(gitignore)에서 읽어
# 이 프로세스 환경에 실으면 Start-Window가 띄우는 자식 창에 자동으로 상속된다.
$EnvFile = Join-Path $Repo ".env"
if (Test-Path $EnvFile) {
  Get-Content $EnvFile | ForEach-Object {
    if ($_ -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
  }
}

$script:Sessions = @()
$script:Started  = Get-Date

# RunLog 한 줄 쓰기. Add-Content는 다른 프로세스가 파일을 열어두기만 해도(다른 세션의 `tail -f`) IOException을 내고,
# $ErrorActionPreference=Stop 아래서는 그 한 줄이 감시견 전체를 끊는다(2026-09-18 10:26 재빌드 직후 중단, 트레이더
# 미기동). .NET FileStream을 공유 모드 ReadWrite로 열면 같은 상황에서도 붙는다. 그래도 안 되면 짧게 재시도하고
# 마지막엔 화면에만 남긴다 — 로그 한 줄은 잃어도 되지만 감시견은 죽으면 안 된다.
function Write-RunLog([string]$line) {
  for ($attempt = 1; $attempt -le 5; $attempt++) {
    try {
      $stream = [System.IO.File]::Open($RunLog, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
      $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
      $writer.WriteLine($line); $writer.Close(); return
    } catch [System.IO.IOException] { Start-Sleep -Milliseconds 200 }
  }
  Write-Host "  (RunLog 잠김 — 파일에 못 남긴 줄) $line" -ForegroundColor DarkYellow
}

function Say([string]$msg, [string]$level = "INFO") {
  $line = "[{0}] {1,-5} {2}" -f (Get-Date -Format "HH:mm:ss"), $level, $msg
  Write-Host $line
  Write-RunLog $line
}

# 네이티브 프로그램(py·wsl)의 stdout·stderr를 한 줄씩 RunLog에 남기고 rc를 돌려준다.
# PowerShell 5.1은 $ErrorActionPreference=Stop 아래서 네이티브 stderr 한 줄을 `2>&1`로 받는 순간
# 스크립트 전체를 끊는다 — 그래서 합치기는 cmd 안에서 하고 PowerShell은 문자열만 받는다.
# 2026-09-16 rc=1의 사유(재수집 "이틀 전 랭킹")가 어디에도 안 남은 것이 이 함수를 만든 이유다.
function Run-Native([string]$cmdline, [string]$prefix = "    ") {
  $out = cmd /c "$cmdline 2>&1"
  $rc = $LASTEXITCODE
  $out | ForEach-Object { Write-RunLog "$prefix$_" }
  return $rc
}

function Save-Status([string]$phase, [hashtable]$extra) {
  # 클로드가 읽는 단일 진실 파일. 로그 tail보다 싸고, 파싱이 흔들리지 않는다.
  $o = [ordered]@{
    phase      = $phase
    config     = $Config
    started    = $script:Started.ToString("s")
    updated    = (Get-Date).ToString("s")
    until      = $Until
    sessions   = $script:Sessions.Count
    history    = $script:Sessions
    run_log    = (Resolve-Path -Relative $RunLog) -replace '\\', '/'
  }
  if ($extra) { foreach ($k in $extra.Keys) { $o[$k] = $extra[$k] } }
  if (-not $DryRun) { $o | ConvertTo-Json -Depth 6 | Set-Content -Path $Status -Encoding utf8 }
}

# ─────────────── 프로세스 수명 묶기 (Job Object) ───────────────
# Windows는 부모가 죽어도 자식을 안 죽인다(리눅스의 프로세스 그룹 cascade가 없다). 그래서
# 워치독이 죽으면 -NoExit로 띄운 창들이 유휴 쉘로 남고, 트레이더는 감시 없이 계속 발주한다.
# JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 잡에 넣어두면 잡 핸들을 쥔 워치독이 사라지는 순간
# (정상 종료든 강제 종료든) 커널이 부속 창과 트레이더를 같이 내린다. 되살리는 쪽은
# scripts\auto_trade_guard.ps1(예약작업)이 맡는다 — 사람 손을 요구하지 않기 위한 분담이다.
$JobSrc = @'
using System;
using System.Runtime.InteropServices;
public static class WinJob {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr CreateJobObject(IntPtr attr, string name);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint cb);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool CloseHandle(IntPtr h);

  [StructLayout(LayoutKind.Sequential)]
  struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
  [StructLayout(LayoutKind.Sequential)]
  struct BASIC_LIMIT {
    public long PerProcessUserTime; public long PerJobUserTime; public uint LimitFlags;
    public UIntPtr MinWorkingSet; public UIntPtr MaxWorkingSet; public uint ActiveProcessLimit;
    public UIntPtr Affinity; public uint PriorityClass; public uint SchedulingClass;
  }
  [StructLayout(LayoutKind.Sequential)]
  struct EXTENDED_LIMIT {
    public BASIC_LIMIT Basic; public IO_COUNTERS Io;
    public UIntPtr ProcessMemoryLimit; public UIntPtr JobMemoryLimit;
    public UIntPtr PeakProcessMemory; public UIntPtr PeakJobMemory;
  }

  public static IntPtr CreateKillOnClose() {
    IntPtr job = CreateJobObject(IntPtr.Zero, null);
    if (job == IntPtr.Zero) return IntPtr.Zero;
    var ext = new EXTENDED_LIMIT();
    ext.Basic.LimitFlags = 0x2000;              // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    int cb = Marshal.SizeOf(typeof(EXTENDED_LIMIT));
    IntPtr buf = Marshal.AllocHGlobal(cb);
    try {
      Marshal.StructureToPtr(ext, buf, false);
      if (!SetInformationJobObject(job, 9, buf, (uint)cb)) { CloseHandle(job); return IntPtr.Zero; }
    } finally { Marshal.FreeHGlobal(buf); }
    return job;
  }

  public static bool Add(IntPtr job, int pid) {
    IntPtr h = OpenProcess(0x0100 | 0x0001, false, pid);   // SET_QUOTA | TERMINATE
    if (h == IntPtr.Zero) return false;
    try { return AssignProcessToJobObject(job, h); } finally { CloseHandle(h); }
  }
}
'@

$script:Job = [IntPtr]::Zero
if (-not $DryRun) {
  try {
    Add-Type -TypeDefinition $JobSrc
    $script:Job = [WinJob]::CreateKillOnClose()
    if ($script:Job -eq [IntPtr]::Zero) { Say "Job Object 생성 실패 — 부속 창이 워치독보다 오래 남을 수 있다." "WARN" }
  } catch {
    Say "Job Object를 쓸 수 없다($($_.Exception.Message)) — 부속 창은 손으로 정리해야 한다." "WARN"
  }
}

$script:Windows = [ordered]@{}   # title -> @{cmd; marker; proc; started} — 되살리기용 원본

function Start-Window([string]$title, [string]$cmd, [string]$marker = "") {
  Say "창 기동: $title"
  if ($DryRun) { Say "  (dry) $cmd"; return }
  # 창 제목에만 인스턴스를 붙인다 — 목록 키($script:Windows)는 원래 제목 그대로 둬야 아래
  #  Restore-Windows 의 제목 비교가 안 깨진다. [why D-122]
  $windowTitle = "$title$Suffix"
  $inner = "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; `$Host.UI.RawUI.WindowTitle='$windowTitle'; Set-Location '$Repo'; $cmd"
  $proc = Start-Process powershell -ArgumentList @("-NoExit", "-NoProfile", "-Command", $inner) -PassThru
  if ($script:Job -ne [IntPtr]::Zero) {
    # 잡에 들어간 뒤 생기는 손자(py → python)는 자동으로 상속된다.
    if ([WinJob]::Add($script:Job, $proc.Id)) { Say "  pid=$($proc.Id) 잡에 묶음 (워치독과 함께 종료)" }
    else { Say "  pid=$($proc.Id) 잡 편입 실패 — 이 창은 따로 닫아야 한다." "WARN" }
  }
  # 같은 title로 다시 띄우면 항목을 갈아끼운다. 되살릴 때 목록이 불어나지 않게.
  $script:Windows[$title] = @{ cmd = $cmd; marker = $marker; proc = $proc; started = Get-Date }
}

# TimescaleDB는 Docker Desktop이 아니라 WSL2(Ubuntu-22.04) 안의 Docker가 낸다. .wslconfig에
# vmIdleTimeout=-1(무제한)을 걸어 놔도 `wsl -e <명령>`처럼 한 번 실행하고 끝나는 호출은 명령이
# 끝나자마자 그 배포판 인스턴스가 곧바로 내려간다(09-16 실측: postgres가 정상 shutdown 로그를
# 남기며 1~2분 간격으로 뜨고 죽길 반복, docker events·crontab·systemd 타이머 어디에도 이걸
# 만드는 주체가 없었다 — VM 유휴 타임아웃이 아니라 배포판 인스턴스 자체의 동작이다). 그래서
# 찔러 깨우는 것만으로는 못 고치고, 하루 종일 배포판에 붙어 있는 프로세스(quant-wsl-keepalive
# 창의 `wsl -e sleep infinity`)를 따로 유지한다 — 이 함수는 기동 직후·재시도 때 healthy까지
# 대기하는 보조 확인일 뿐이다.
function Wait-Tsdb {
  if ($DryRun) { return }
  $tsdb = (cmd /c 'wsl -e docker inspect quant-tsdb --format "{{.State.Health.Status}}" 2>nul')
  if ($LASTEXITCODE -ne 0 -or $tsdb -notmatch "healthy") {
    Say "  quant-tsdb 미기동(status=$tsdb) — 최대 30초 대기"
    for ($i = 0; $i -lt 6; $i++) {
      Start-Sleep -Seconds 5
      $tsdb = (cmd /c 'wsl -e docker inspect quant-tsdb --format "{{.State.Health.Status}}" 2>nul')
      if ($tsdb -match "healthy") { break }
    }
  }
  if ($tsdb -match "healthy") { Say "  quant-tsdb healthy" }
  else { Say "  quant-tsdb 여전히 미기동(status=$tsdb) — recorder는 재시도 루프로 뜬다, DB 적재는 못 할 수 있다." "WARN" }
}

# candidate 의 부모 사슬을 따라 올라가 ancestor 를 만나는지 본다. ancestor 가 0이면(창 핸들이 없는
#  경우) 판정을 포기하고 참으로 둔다 — 멀쩡한 창을 다시 띄우는 쪽이 더 나쁘다. [why D-122]
function Test-Descendant([int]$candidate, [int]$ancestor, [hashtable]$parentOf)
{
  if ($ancestor -eq 0) { return $true }

  $current = $candidate
  $hop = 0

  while ($hop -lt 20)
  {
    if (-not $parentOf.ContainsKey($current)) { return $false }

    $current = $parentOf[$current]

    if (-not $current) { return $false }

    if ($current -eq $ancestor) { return $true }

    $hop++
  }

  return $false
}

function Restore-Windows {
  # 창(powershell)은 -NoExit라 안의 파이썬이 죽어도 남는다. 빈 창만 보면 살아 있는 줄 안다.
  # 그래서 파이썬 프로세스의 명령줄에서 스크립트 이름을 직접 찾는다.
  if ($DryRun -or $script:Windows.Count -eq 0) { return }
  # 이름만 보면 다른 계좌의 감시견이 띄운 같은 스크립트를 자기 것으로 오인해, 죽은 자기 자식을
  #  영영 안 살린다. 그래서 부모 사슬을 따라 자기 창의 자손인지까지 본다. [why D-122]
  $allProcesses = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
  $parentOf = @{}
  foreach ($process in $allProcesses) { $parentOf[[int]$process.ProcessId] = [int]$process.ParentProcessId }
  $procs = @($allProcesses | Where-Object { $_.Name -eq "python.exe" -or $_.Name -eq "py.exe" -or $_.Name -eq "pythonw.exe" })
  foreach ($title in @($script:Windows.Keys)) {
    $w = $script:Windows[$title]
    if (-not $w.marker) {
      # 파이썬 프로세스가 아니라 확인할 marker가 없다(예: quant-wsl-keepalive) — 창(powershell)
      # 자체가 죽었는지만 본다.
      if ($w.proc -and $w.proc.HasExited) {
        Say "부속 창 '$title'이 죽었다 — 다시 띄운다." "WARN"
        Start-Window $title $w.cmd $w.marker
      }
      continue
    }
    # 기동 직후에는 아직 파이썬이 안 뜬 상태일 수 있다. 뜰 시간을 준다.
    if (((Get-Date) - $w.started).TotalSeconds -lt 45) { continue }
    $ownerPid = if ($w.proc) { [int]$w.proc.Id } else { 0 }
    if ($procs | Where-Object { $_.CommandLine -like "*$($w.marker)*" -and (Test-Descendant ([int]$_.ProcessId) $ownerPid $parentOf) }) { continue }
    Say "부속 창 '$title' 안에서 $($w.marker)가 죽었다 — 다시 띄운다." "WARN"
    if ($w.proc -and -not $w.proc.HasExited) { Stop-Process -Id $w.proc.Id -Force -ErrorAction SilentlyContinue }
    if ($title -eq "quant-recorder") { Wait-Tsdb }
    Start-Window $title $w.cmd $w.marker
  }
}

# 유니버스 스캔은 시총·거래대금을 네이버 실행 시점 값으로 받는다(universe_feed.py). 장중에는 당일 누적
#  거래대금이 그날 강한 종목을 가장 잘 가리키므로 15:30 까지 30분마다 다시 돌려 파일만 바꿔 둔다 — 엔진은
#  union_refresh_sec마다 파일을 다시 읽는다. 스캔이 실패하면(장 전에 누적치가 없고 data.go.kr 목록이 이틀
#  전이면 rc=1) 직전 파일을 그대로 두고 30분 뒤 다시 본다(09-14 실측: 이틀 전 기준으로 하루를 보내
#  보안주 3종·라온시큐어가 풀에 없었다).
# 09-16 실측: 08시대(사전장) 스캔은 거래대금이 전 종목 0이라 이 가드에 거의 매번 걸려 rc=1이고,
#  30분 카운터는 스크립트 기동 시각 기준이라 09:00과 우연히 맞지 않으면 장 시작 뒤에도 한참(최대
#  30분) 전날 파일로 매매한다 — 주도주는 매일 바뀌므로 이 창이 위험하다. 카운터와 별개로 09:00~09:04
#  구간에 한 번 강제 재확인해 그 창을 최대 5분으로 줄인다.
# 09-18 실측: 09:00 재확인은 거래대금 35초치라 1,471종목만 값이 있어 104종목에 그쳤고, 09:30에야 277종목이
#  됐다. 첫 한 시간은 거래대금 순위가 가장 빠르게 바뀌는 구간이라 30분 간격은 너무 길다 — 10:00 전에는 3분,
#  그 뒤는 10분으로 간격을 시간대별로 둔다(스캔 1회 ~3초, 네이버 29요청).
function Get-UnivIntervalMin {
  if ((Get-Date).ToString("HHmm") -lt "1000") { return 3 }
  return 10
}
$script:UnivNext = (Get-Date).AddMinutes((Get-UnivIntervalMin))   # 장중 재기동이면 첫 카운터도 같은 규칙
$script:UnivOpenRetryDone = $false
function Refresh-Universe {
  if ($DryRun -or $NoUniverse) { return }
  $now = Get-Date
  if ($now.ToString("HHmm") -ge "1530") { return }
  if (-not $script:UnivOpenRetryDone -and $now.ToString("HHmm") -ge "0900" -and $now.ToString("HHmm") -lt "0905") {
    $script:UnivOpenRetryDone = $true
    Say "장 시작 직후 유니버스 재확인 — 정기 카운터와 별개(사전장 rc=1 대비)."
    $rc = Run-Native "`"$py`" PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json"
    if ($rc -ne 0) { Say "유니버스 재스캔 실패(rc=$rc) — 직전 파일 유지." "WARN" }
    else { $script:UnivNext = $now.AddMinutes((Get-UnivIntervalMin)) }
    return
  }
  if ($now -lt $script:UnivNext) { return }
  $interval = Get-UnivIntervalMin
  $script:UnivNext = $now.AddMinutes($interval)
  Say "유니버스 스캔을 다시 돌린다(시총·거래대금 현재 값, ${interval}분 뒤 재확인)."
  $rc = Run-Native "`"$py`" PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json"
  if ($rc -ne 0) { Say "유니버스 재스캔 실패(rc=$rc) — 직전 파일 유지." "WARN" }
}

# ─────────────── 사전 점검 ───────────────
Say "자동매매 하루 루프 시작 — config=$Config until=$Until$(if($DryRun){' (dry-run)'})"

# 지난 회차의 남은 프로세스를 먼저 치운다. Job Object는 워치독이 정상적으로 사라질 때만 자식을 내리는데,
# 강제 종료·리부트·워치독 없이 손으로 띄운 창은 그 경로를 타지 않는다. 그렇게 남은 보조 프로세스·
# 대시보드가 계속 폴링하면 REST 초당 한도를 같이 갉아먹고, 창만 남은 빈 창은 화면을 먹는다.
# 트레이더는 여기서 죽이지 않는다 — 바로 아래 duplicate_process 게이트가 사람 판단으로 처리한다.
$reaper = Join-Path $PSScriptRoot "quant_procs.ps1"
if ((Test-Path $reaper) -and -not $DryRun) {
  try {
    $out = & powershell -ExecutionPolicy Bypass -NoProfile -File $reaper -Reap -Quiet 2>&1
    foreach ($l in $out) { if ("$l".Trim()) { Say "  $l" } }
  } catch { Say "남은 프로세스 정리 실패($($_.Exception.Message)) — 손으로 확인할 것." "WARN" }
}

# 이름이 quant_trader라고 다 발주하는 것은 아니다. 리플레이 측정용 프로세스는 config에 replay_file이
#  있어 저장된 체결을 다시 먹일 뿐 증권사에 주문을 내지 않는다(워크트리 세션이 장중에도 돌린다).
#  그것까지 중복으로 보면 부속 워치독이 못 뜬다 — 09-22 실측. 판정이 안 서면 막는 쪽으로 남긴다.
#  모의와 실계좌를 같이 돌리는 날에는 계좌까지 봐야 한다 — 다른 계좌면 같이 떠도 원장이 안 섞인다.
#  그래서 "발주하는 계좌"를 열쇠로 돌려준다: $null=리플레이 전용(발주 안 함), "?"=판정 불가(막는다). [why D-122]
function Get-TraderAccountKey([System.Diagnostics.Process]$traderProcess)
{
  $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$($traderProcess.Id)" -ErrorAction SilentlyContinue).CommandLine

  if (-not $commandLine) { return "?" }

  $configArgument = [regex]::Matches($commandLine, '"[^"]+"|\S+') |
                    ForEach-Object { $_.Value.Trim('"') } |
                    Where-Object { $_ -like "*.json" } | Select-Object -Last 1

  if (-not $configArgument) { return "?" }

  # exe는 <저장소>\Quant\build_win\quant_trader.exe — 세 단계 올라가면 그 트리의 루트다.
  $repoRoot = Split-Path (Split-Path (Split-Path $traderProcess.Path -Parent) -Parent) -Parent

  foreach ($candidatePath in @($configArgument, (Join-Path $repoRoot $configArgument)))
  {
    if (Test-Path $candidatePath)
    {
      try
      {
        $json = Get-Content $candidatePath -Raw -Encoding UTF8 | ConvertFrom-Json

        if ($json.replay_file) { return $null }

        if (-not $json.kis.account_no) { return "?" }

        return (Get-AccountKey $json)
      }
      catch { return "?" }
    }
  }

  return "?"
}

# 계좌를 가르는 열쇠. 모의와 실계좌는 계좌번호가 다르지만, 번호가 같고 is_paper만 다른 경우도
#  섞이지 않게 둘을 함께 쓴다.
function Get-AccountKey($configJson)
{
  return ("{0}/{1}" -f $configJson.kis.account_no, [bool]$configJson.kis.is_paper)
}

$myAccountKey = "?"
try
{
  $myConfigJson = Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json

  if ($myConfigJson.kis.account_no) { $myAccountKey = Get-AccountKey $myConfigJson }
}
catch { }

$dup = @(Get-Process quant_trader -ErrorAction SilentlyContinue | Where-Object {
  $otherKey = Get-TraderAccountKey $_
  # 리플레이 전용은 통과. 한쪽이라도 판정이 안 서면 막는 쪽으로 남긴다.
  $null -ne $otherKey -and ($otherKey -eq "?" -or $myAccountKey -eq "?" -or $otherKey -eq $myAccountKey)
})
if ($dup) {
  # 두 프로세스가 같은 계좌에 발주하면 원장이 깨진다. 자동으로 정리하지 않고 멈춘다.
  Say "같은 계좌($myAccountKey)에 발주하는 quant_trader가 이미 $($dup.Count)개 떠 있다. 중복 발주를 막기 위해 중단한다." "ERROR"
  Save-Status "aborted" @{ error = "duplicate_process"; pids = @($dup.Id); account = $myAccountKey }
  exit 2
}
if (-not $NoTrader -and -not (Test-Path $Exe)) { Say "실행파일 없음: $Exe — /build 먼저." "ERROR"; Save-Status "aborted" @{ error = "no_exe" }; exit 2 }
if (-not (Test-Path $Config))  { Say "config 없음: $Config" "ERROR"; Save-Status "aborted" @{ error = "no_config" }; exit 2 }

# 낡은 바이너리로 매매하지 않는다. 예전엔 소스가 exe보다 새면 중단하고 사람이 /build를 돌렸는데,
# 그 사이 매매가 안 됐다. 이제 기동 전에 한 번 빌드한다(증분이라 바뀐 게 없으면 몇 초).
# 실패하면 옛 exe가 남아 있어도 매매하지 않는다. 장중 재기동에는 빌드가 없다 — 여기 한 번뿐이다.
$head  = (git rev-parse --short HEAD 2>$null)
$dirty = @(git status --porcelain Quant\src Quant\include 2>$null).Count
if ($dirty -gt 0) { Say "메인 트리에 미커밋 소스 변경 $dirty 건 — 그대로 빌드에 들어간다." "WARN" }
if ($NoTrader) {
  Say "-NoTrader — Windows exe를 쓰지 않으므로 재빌드 생략"
} elseif ($NoBuild) {
  $src = Get-ChildItem -Recurse -File "Quant\src", "Quant\include" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($src -and $src.LastWriteTime -gt (Get-Item $Exe).LastWriteTime) {
    Say "소스가 exe보다 새것이다($($src.Name)). -NoBuild라 빌드하지 않는다 — /build 후 다시." "ERROR"
    Save-Status "aborted" @{ error = "stale_exe"; newer = $src.Name; head = $head }
    exit 2
  }
} elseif ($DryRun) {
  Say "  (dry) 재빌드 생략"
} else {
  # /build 커맨드와 같은 배선 — 한글 %TEMP%의 LNK1104 회피, vcvars64로 MSVC 환경, cmake는 절대경로.
  if (-not (Test-Path C:\build_tmp)) { New-Item -ItemType Directory -Force C:\build_tmp | Out-Null }
  $env:TEMP = 'C:\build_tmp'; $env:TMP = 'C:\build_tmp'
  # 헤더 의존 추적 — cl의 /showIncludes 한글 접두어는 콘솔 코드페이지 바이트로 나오고 ninja는 configure 때 적은 바이트와 비교한다.
  #  Quant\build_win은 UTF-8(65001)로 configure돼 있고 이 창도 34행에서 UTF-8이다. 코드페이지가 다르면 deps 0 → 헤더가 바뀌어도
  #  재컴파일이 안 된다(09-21 08:20 크래시 루프, 낡은 오브젝트). 여기서 한 번 더 못박는다.
  cmd /c 'chcp 65001 >nul'; [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
  $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  $cmake  = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  Say "재빌드 시작 — HEAD $head, 타깃 quant_trader"
  $t0 = Get-Date
  $out = cmd /c "`"$vcvars`" >nul 2>&1 && `"$cmake`" --build Quant\build_win --target quant_trader 2>&1"
  $rc = $LASTEXITCODE
  $out | ForEach-Object { Write-RunLog "    $_" }
  $secs = [int]((Get-Date) - $t0).TotalSeconds
  if ($rc -ne 0) {
    $why = ($out | Where-Object { $_ -match 'error|FAILED|LNK' } | Select-Object -First 3) -join ' | '
    Say "재빌드 실패(rc=$rc, ${secs}s) — $why" "ERROR"
    Save-Status "aborted" @{ error = "build_failed"; head = $head; rc = $rc; detail = $why }
    exit 2
  }
  Say "재빌드 완료(${secs}s) — exe $((Get-Item $Exe).LastWriteTime.ToString('MM-dd HH:mm'))"
}

# 주문이 나가는 계좌는 kis 블록이다. 최상위나 quote_kis(시세 전용)의 is_paper를 보면
# 모의를 실계좌로 잘못 읽는다 — 실제로 config_dev_paper는 quote_kis.is_paper=false다.
# 설정을 못 읽으면 '모의'로 가정하지 않는다 — 실계좌 설정을 모의로 잘못 알고 띄우는 쪽이 더 위험하다.
$paper = $null
try { $paper = [bool](Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).kis.is_paper } catch { }
if ($null -eq $paper) { Say "설정 $Config 에서 kis.is_paper 를 읽지 못했다 — 계좌 모드를 모른 채 띄우지 않는다." "ERROR"; exit 2 }
Say ("계좌 모드: {0}" -f $(if ($paper) { "모의(is_paper=true)" } else { "실계좌(is_paper=false)" })) $(if ($paper) { "INFO" } else { "WARN" })

$py = if (Test-Path $VenvPy) { $VenvPy } else { Say "venv 없음 — 보조 프로세스는 FDR 없이 UNKNOWN만 낸다." "WARN"; "py" }

# 휴장일 관문 — 여태 주말 판정조차 없어서 토·일·공휴일에도 트레이더와 부속 창이 다 떴다.
# 시세가 안 오고 주문도 안 나가니 손해는 없지만 토큰을 새로 받고 WS 재접속을 되풀이한다.
# 부속 창보다 앞에 둔다 — 뒤에 두면 이미 띄운 창을 도로 내려야 한다.
# 종료코드 0=개장 1=휴장 2=모름. 2는 휴장으로 보지 않는다 — 조회 한 번 실패한 날 매매를 통째로
# 거르는 쪽이 휴장일에 헛도는 것보다 비싸다. -DryRun·-NoTrader도 같은 관문을 지난다. [why D-120]
$marketOpenOutput = & cmd /c "`"$py`" scripts\check_market_open.py --config `"$Config`" 2>&1"
$marketOpenCode   = $LASTEXITCODE
foreach ($line in $marketOpenOutput) { if ("$line".Trim()) { Write-RunLog "    $line" } }
$marketOpenVerdict = "$(($marketOpenOutput | Where-Object { "$_" -match '^\[' } | Select-Object -First 1))"

if ($marketOpenCode -eq 1) {
  Say "휴장일 — 트레이더도 부속 창도 띄우지 않는다. $marketOpenVerdict"
  Save-Status "market_closed" @{ paper = $paper; head = $head; detail = $marketOpenVerdict }
  exit 0
}

if ($marketOpenCode -ne 0) {
  Say "개장 여부를 확인하지 못했다(rc=$marketOpenCode) — 휴장으로 단정하지 않고 그대로 진행한다. $marketOpenVerdict" "WARN"
}

Save-Status "starting" @{ paper = $paper; head = $head; dirty = $dirty }

# 수치·주기 시트(_private/TUNING_SHEET.md)를 오늘 띄우는 config 기준으로 다시 쓴다. 실패해도 매매와 무관하다.
if (-not $DryRun) { [void](Run-Native "py scripts\gen_tuning_sheet.py --config $Config") }

# ─────────────── 부속 창 ───────────────
if (-not $NoRegimeFeed)   { Start-Window "quant-regime"   "& '$py' PYQuant\tools\macro_regime_feed.py --interval 180 --out Quant\config\regime.json" "macro_regime_feed.py" }
if (-not $NoUniverse)  {
  Say "유니버스 스캔(ALL) — 완료까지 기다린다. 이게 없으면 전략이 붙을 종목이 없다."
  if (-not $DryRun) {
    $rc = Run-Native "`"$py`" PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json"
    if ($rc -ne 0) { Say "유니버스 스캔 실패(rc=$rc) — 직전 스캔 파일로 진행한다." "WARN" }
  }
}
if (-not $NoPrices)    { Start-Window "quant-prices"    "& '$py' scripts\live_prices_feed.py" "live_prices_feed.py" }
if (-not $NoDashboard)
{
  # --config·--port 를 안 넘기면 dashboard_server.py 는 기본값(config_dev_paper.json·8787)을 읽는다.
  #  실계좌 감시견이 띄워도 화면에는 모의 계좌가 뿌려지고, 두 감시견이 같은 8787 을 다투다(2026-09-23).
  $dashboardPort = 0
  try { $dashboardPort = [int](Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).dashboard_port } catch { }
  if (-not $dashboardPort) { $dashboardPort = 8787 }
  # --logs 도 같이 넘긴다. 이게 없으면 대시보드는 같은 날짜 원장 중 행 수가 많은 쪽을 고른다
  #  (_logdir.find_ledger). 모의가 하루 종일 돌아 원장이 훨씬 크므로, 실계좌 화면에 모의의
  #  실현손익·체결 건수가 뜨고 계좌 잔고만 실계좌이다(2026-09-23 실측).
  $ledgerDirectory = ""
  try { $ledgerDirectory = (Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).ledger_journal_dir } catch { }
  $logsClause = if ($ledgerDirectory) { " --logs $ledgerDirectory" } else { "" }
  Start-Window "quant-dashboard" "py scripts\dashboard_server.py --config $Config --port $dashboardPort$logsClause" "dashboard_server.py"
  Say "  대시보드 http://127.0.0.1:$dashboardPort  ($Config)"
}
if (-not $NoNotify)    { Start-Window "quant-notify"    "& '$py' scripts\notify_trades.py --config $Config --interval 1800" "notify_trades.py" }
# 네이티브 트레이더는 컨테이너가 아니라 ZMQ PUB(127.0.0.1:5555)만 낸다 — docker-compose의
# quant-recorder는 quant-engine 컨테이너를 구독하므로 이 프로세스를 못 본다(D-090 후속).
# 같은 호스트에서 직접 구독해 TimescaleDB에 적재한다.
if (-not $NoRecorder) {
  Say "WSL 배포판을 하루 종일 깨워 둔다(quant-wsl-keepalive) — TimescaleDB 컨테이너가 붙어 있을 곳"
  Start-Window "quant-wsl-keepalive" "while (`$true) { wsl -e sleep infinity; Start-Sleep -Seconds 2 }" ""
  Say "TimescaleDB 사전 점검 — WSL Docker 깨우기"
  Wait-Tsdb
  # --record-ticks: 체결 틱을 ticks 표에 모아 넣는다(배치). 그라파나 "피드 지연"·"초당 틱 유입" 패널이
  #  이 표를 읽는다 — 없으면 피드가 멀쩡해도 패널이 며칠 전 시각을 가리킨다.
  # --account: 이 계좌의 주문·체결만 받는다. Engine 을 그대로 띄우는 테스트·부하 하네스가 같은 5555에
  #  bind 하면 리코더가 그쪽을 잡아 합성 데이터가 운영 표에 섞인다(09-22 장중 실측).
  $recorderAccount = ""
  try { $recorderAccount = (Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).kis.account_no } catch { }
  if (-not $recorderAccount) { Say "config 에서 kis.account_no 를 못 읽었다 — 리코더 계좌 거르기 없이 띄운다." "WARN" }
  $recorderArgs = if ($recorderAccount) { "--account $recorderAccount" } else { "" }
  # --port: 엔진이 PUB 을 여는 포트다. 실계좌는 모의 엔진과 bind 가 겹치지 않게 5565 로 옮겨 놓았으므로
  #  (config_live.json 의 zmq_pub_port) 5555 를 박아 두면 실계좌 체결·틱이 DB 에 한 건도 안 들어간다.
  $recorderPort = 0
  try { $recorderPort = [int](Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).zmq_pub_port } catch { }
  if (-not $recorderPort) { $recorderPort = 5555; Say "config 에서 zmq_pub_port 를 못 읽었다 — 기본 $recorderPort 로 띄운다." "WARN" }
  Start-Window "quant-recorder"  "& '$py' PYQuant\main.py record --host localhost --port $recorderPort --record-ticks $recorderArgs" "main.py record"
  # 엔진 자원(CPU·메모리·스레드별 CPU·perf 함수 핫스팟) → 그라파나 ops. -NoTrader 날은 엔진이 WSL(Ubuntu-24.04)에
  # 있어 /proc를 그 배포판에서 읽고, Windows exe 날은 psutil로 본다.
  $procwatchArgs = if ($NoTrader) { "--wsl-distro Ubuntu-24.04" } else { "" }
  Start-Window "quant-procwatch" "& '$py' PYQuant\main.py procwatch $procwatchArgs" "main.py procwatch"
  # 원장 저널 파일(D-113) → TimescaleDB 복제. 2초 주기로 꼬리를 따라가고, DB가 없으면 그냥 죽는다 —
  #  되살릴 때 안 읽은 구간부터 따라잡으므로 잃는 것이 없다. 폴더는 config의 ledger_journal_dir(엔진과 같은 곳).
  $ledgerDir = ""
  try { $ledgerDir = (Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json).ledger_journal_dir } catch { }
  if (-not $ledgerDir) { $ledgerDir = "Quant\build_win\logs"; Say "config 에서 ledger_journal_dir 을 못 읽었다 — $ledgerDir 로 띄운다." "WARN" }
  Start-Window "quant-ledger"    "& '$py' PYQuant\tools\ledger_recorder.py --dir '$ledgerDir'" "ledger_recorder.py"
}

# ─────────────── 감시 루프 ───────────────
$deadline = [datetime]::ParseExact((Get-Date -Format "yyyy-MM-dd") + " " + $Until, "yyyy-MM-dd HH:mm", $null)
if ((Get-Date) -ge $deadline) { Say "이미 $Until 을 지났다. 매매하지 않고 종료." "WARN"; Save-Status "past_deadline" $null; exit 0 }

# 크래시 루프 브레이크 — 최근 30분 안에 세션 종료가 3번이면 멈춘다(T-13-7). 예전 "30초 미만 3연속"은 2~3분 살다 죽는
#  루프(토큰 만료·WS 재접속 실패 뒤 종료)를 못 잡았다. 종료 시각은 상태 파일 history의 end로도 남는다.
$crashWindow   = [timespan]::FromMinutes(30)
$crashMaxExits = 3
$exitTimes     = @()
if ($NoTrader) {
  # 트레이더는 다른 곳(리눅스)이 띄운다 — 같은 계좌에 Windows 트레이더까지 띄우면 이중 발주다(09-11).
  # 이 창은 부속 창 생존·유니버스 갱신만 하며 마감까지 기다리고, 마감 뒤 사실 정리는 평소와 같이 한다.
  Say "-NoTrader — 트레이더를 띄우지 않는다. 부속 창만 지키며 $Until 까지 기다린다."
  Save-Status "running" @{ trader = "external" }
  if ($DryRun) { Say "  (dry) $Until 까지 60초마다 부속 창 생존·유니버스 갱신만 한다"; exit 0 }
  while ((Get-Date) -lt $deadline) { Start-Sleep -Seconds 60; Restore-Windows; Refresh-Universe }
}
while (-not $NoTrader -and (Get-Date) -lt $deadline) {
  $n = $script:Sessions.Count + 1
  Say "세션 #$n 기동 — $Exe $Config"
  if ($DryRun) { Say "  (dry) 트레이더 기동 생략, 루프 종료"; break }

  # 직전 세션이 '이미 한 번 당한' 실패 유형을 다시 냈는지 본다. 재기동마다 확인하지 않으면
  # 같은 결함으로 하루를 다 태운다(2026-09-08: 유령주문 재부활 85건, 재기동 투매 64건).
  if ($script:LastStart) {
    $null = Run-Native ("py `"{0}`" --since {1}" -f (Join-Path $Repo "scripts\check_runtime_health.py"), $script:LastStart) "  "
  }

  $t0 = Get-Date
  $script:LastStart = $t0.ToString("HH:mm")
  # 이전 세션이 남긴 미체결 주문 목록을 로그에서 복원해 보조 프로세스에 채운다. 엔진이 기동하면서
  # cancel_stale_orders()가 이 파일을 읽어 전부 취소한다 — 유령 지정가가 현금과 매도가능수량을
  # 묶고, 청산 직후 되사서 전략을 뒤집는 것을 막는다(2026-09-08 미체결 85건 실측).
  # 엔진이 스스로 쓰는 보조 프로세스가 정상이면 이 복원은 같은 내용을 다시 쓸 뿐이라 무해하다.
  $null = Run-Native ("py `"{0}`"" -f (Join-Path $Repo "scripts\seed_open_orders.py")) "  "

  # 트레이더도 잡에 넣는다. 워치독이 사라졌는데 엔진만 살아 있으면 아무도 감시하지 않는 채
  # 발주가 계속되고, 다음 기동은 중복 프로세스로 막힌다(duplicate_process). 같이 내리고
  # 감시자(auto_trade_guard.ps1)가 다시 띄우면 잔고 재시드가 포지션을 도로 잡는다.
  $p = Start-Process -FilePath $Exe -ArgumentList $Config -WorkingDirectory $Repo -PassThru -NoNewWindow
  # Process.ExitCode는 종료 전에 Handle을 한 번 만져 둔 객체에서만 채워진다. 안 만지면 아래
  # 세션 기록·크래시 루프 판정(last_exit)이 전부 null을 본다.
  $null = $p.Handle
  if ($script:Job -ne [IntPtr]::Zero) {
    if (-not [WinJob]::Add($script:Job, $p.Id)) { Say "  트레이더 pid=$($p.Id) 잡 편입 실패 — 워치독이 죽으면 미연결로 남는다." "WARN" }
  }
  Save-Status "running" @{ pid = $p.Id; session = $n }
  # WaitForExit로 통째로 막지 않는다. 트레이더를 기다리는 동안 부속 창 안의 파이썬이
  # 죽었는지도 같이 본다 — 알림·국면 보조 프로세스가 조용히 사라지는 것을 놓치지 않기 위해서다.
  while (-not $p.HasExited) {
    if ($p.WaitForExit(60000)) { break }
    Restore-Windows
    Refresh-Universe
  }
  $secs = [int]((Get-Date) - $t0).TotalSeconds

  $script:Sessions += [ordered]@{ n = $n; start = $t0.ToString("HH:mm:ss"); end = (Get-Date).ToString("HH:mm:ss")
                                  seconds = $secs; exit = $p.ExitCode }
  Say "세션 #$n 종료 — exit=$($p.ExitCode) 지속 ${secs}s" $(if ($p.ExitCode -eq 0) { "INFO" } else { "WARN" })

  if ((Get-Date) -ge $deadline) { Say "마감 시각 도달 — 재기동하지 않는다."; break }

  # 엔진이 스스로 내려간 날은 재기동하지 않는다 — exit 코드로는 자기 종료와 크래시를 가를 수 없어 표지 파일로 본다(D-098).
  #  session_done_<날짜>: 마지막 매매 창 + 유예가 지나 엔진이 큐를 비우고 종료. kill_today_<날짜>: 운영단말·ZMQ KILL.
  #  KILL을 풀고 다시 띄우려면 scripts/kill_release.ps1(표지 파일을 지운다) — 이 창을 닫으면 가드가 5분 안에 다시 띄운다.
  $today = Get-Date -Format yyyy-MM-dd
  # 표지 파일 이름에도 인스턴스가 붙는다(Engine::write_state_marker) — 모의가 15:30에 남긴 마감
  #  표지로 20:00까지 도는 실계좌 감시견이 멈추지 않게. [why D-122]
  if (Test-Path "_private\state\session_done$Suffix`_$today") { Say "엔진이 마감 자기 종료 — 재기동하지 않는다."; break }
  if (Test-Path "_private\state\kill_today$Suffix`_$today")   { Say "운영자 KILL — 오늘은 재기동하지 않는다(풀려면 scripts/kill_release.ps1)." "WARN"; break }

  $now = Get-Date
  # 배포가 일부러 내린 것(scripts/deploy_trader.py 가 pid 표지를 남긴다)은 크래시로 세지 않는다 — 30분 안에 세 번
  #  배포하면 감시견이 크래시 루프로 보고 멈췄다.
  $planned = "_private\state\planned_restart_$($p.Id)"
  if (Test-Path $planned) {
    Say "배포 재기동($(Get-Content $planned -Raw)) — 크래시 계산에서 뺀다."
    Remove-Item $planned -ErrorAction SilentlyContinue
  }
  else {
    $exitTimes = @($exitTimes | Where-Object { ($now - $_) -lt $crashWindow }) + $now
  }
  if ($exitTimes.Count -ge $crashMaxExits) {
    # 30분 안에 세 번 내려갔으면 원인이 배선(config·인증·바이너리·브로커)에 있다. 재기동으로 풀리지 않는다.
    Say "최근 $($crashWindow.TotalMinutes)분 안 종료 $($exitTimes.Count)회 — 크래시 루프로 보고 멈춘다. 로그를 보고 고쳐야 한다." "ERROR"
    Save-Status "crash_loop" @{ error = "crash_loop"; last_exit = $p.ExitCode; exits = @($exitTimes | ForEach-Object { $_.ToString("HH:mm:ss") }) }
    exit 3
  }

  Say "5초 뒤 재기동."
  Start-Sleep -Seconds 5
}

Save-Status "closed" @{ }

# ─────────────── 마감 뒤 사실 정리 ───────────────
# 해석(리뷰 문장·개선안)은 클로드가 채우지만, 사실 문서와 대시보드는 사람 없이도 최신이어야 한다.
if (-not $NoMarketClose -and -not $DryRun) {
  Say "장 마감 정리 — market_close_autodoc(일지 사실 구간 + 리뷰 항목 + 대시보드 재생성)"
  py scripts\market_close_autodoc.py
  Say "market_close_autodoc rc=$LASTEXITCODE"

  # 하루 전체 건전성 점검. FAIL이 남았으면 그날 사후검토에서 먼저 다룰 항목이다.
  Say "실행 건전성 점검(하루 전체)"
  & py (Join-Path $Repo "scripts\check_runtime_health.py") 2>&1 | ForEach-Object { Say "  $_" }
}

# 잡 핸들이 닫히며 부속 창은 커널이 같이 내린다. 그래도 한 번 훑는 것은, 잡 편입에 실패했거나
# 워치독 밖에서 띄운 것이 남을 수 있어서다. 남은 게 없으면 아무 줄도 찍히지 않는다.
if ((Test-Path $reaper) -and -not $DryRun) {
  try {
    $out = & powershell -ExecutionPolicy Bypass -NoProfile -File $reaper -Reap -Quiet 2>&1
    foreach ($l in $out) { if ("$l".Trim()) { Say "  $l" } }
  } catch { }
}

Say "하루 루프 종료 — 세션 $($script:Sessions.Count)회, 로그 $RunLog"
Save-Status "done" @{ }

# 부속 창은 Job Object로 이 프로세스에 묶여 있지만, 이 창은 -NoExit로 떠 있어 잡 핸들이 안 닫힌다 — 시세·국면·알림
#  폴러가 밤새 REST를 부르고 창 5개가 남는다(09-21 16:20 사용자 확인). 기동 때 쓰는 정리기를 -KillAll로 한 번 더 불러
#  역할 프로세스·창을 전부 내린다. 이 창(watchdog 역할)도 같이 닫히므로 이 줄이 마지막이어야 한다.
if ((Test-Path $reaper) -and -not $DryRun) {
  Say "부속 프로세스 정리 — quant_procs.ps1 -KillAll"
  # 자기 PID를 넘겨 자기가 띄운 것만 내린다 — 계좌를 둘 돌리는 날 15:35에 마감하는 모의 감시견이
  #  20:00까지 도는 실계좌 트레이더까지 내리던 것을 막는다. [why D-122]
  & powershell -ExecutionPolicy Bypass -NoProfile -File $reaper -KillAll -Quiet -OwnerPid $PID -Instance $Instance 2>&1 | ForEach-Object { if ("$_".Trim()) { Write-RunLog "    $_" } }
}
