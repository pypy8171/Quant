<#
.SYNOPSIS
  하루치 자동매매 루프 — 사이드카·유니버스·대시보드를 띄우고 트레이더를 장 마감까지 감시·재기동한다.

.DESCRIPTION
  세 창을 손으로 여는 절차(/intraday-start)를 한 창으로 접었다. 이 스크립트는 사람 판단이
  필요 없는 부분만 맡는다 — 기동, 죽으면 다시 띄우기, 마감에 내리고 사실 문서까지.
  코드 결함 진단·수정·재빌드는 /auto-trade-day 커맨드(클로드)가 이 스크립트 위에서 한다.

  진행 상태는 매 전이마다 _private/_auto_trade_day.json 에 적는다. 클로드가 로그 전체를
  훑지 않고 이 파일만 읽어 지금 몇 번째 세션인지, 왜 죽었는지 알 수 있게 하려는 것이다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -Config Quant\config\config.json -Until 20:05
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -DryRun
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_dev_paper.json",
  [string]$Until = "20:05",          # 이 시각을 넘으면 재기동하지 않는다(애프터마켓 마감 20:00 + 정리 여유, D-097)
  [switch]$NoSidecar,
  [switch]$NoUniverse,
  [switch]$NoDashboard,
  [switch]$NoNotify,                 # 체결·포지션 메신저 알림 창을 띄우지 않는다
  [switch]$NoPrices,                 # 전 종목 시세 파일 전달(네이버 벌크) 창을 띄우지 않는다
  [switch]$NoRecorder,               # ZMQ 체결·주문을 TimescaleDB에 적재하는 창을 띄우지 않는다
  [switch]$NoEod,                    # 마감 뒤 사실 문서·대시보드 갱신을 건너뛴다
  [switch]$NoBuild,                  # 기동 전 재빌드를 건너뛴다(exe를 손으로 바꾼 날). 이때는 소스가 exe보다 새면 중단
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

$Exe     = Join-Path $Repo "Quant\build_win\quant_trader.exe"
$VenvPy  = Join-Path $Repo "PYQuant\.venv-win\Scripts\python.exe"
$Status  = Join-Path $Repo "_private\_auto_trade_day.json"
$LogDir  = Join-Path $Repo "logs"
$RunLog  = Join-Path $LogDir ("auto_trade_day_{0}.log" -f (Get-Date -Format yyyyMMdd))
New-Item -ItemType Directory -Force -Path $LogDir, (Split-Path $Status) | Out-Null

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

$script:Windows = [ordered]@{}   # title -> @{cmd; probe; proc; started} — 되살리기용 원본

function Start-Window([string]$title, [string]$cmd, [string]$probe = "") {
  Say "창 기동: $title"
  if ($DryRun) { Say "  (dry) $cmd"; return }
  $inner = "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; `$Host.UI.RawUI.WindowTitle='$title'; Set-Location '$Repo'; $cmd"
  $proc = Start-Process powershell -ArgumentList @("-NoExit", "-NoProfile", "-Command", $inner) -PassThru
  if ($script:Job -ne [IntPtr]::Zero) {
    # 잡에 들어간 뒤 생기는 손자(py → python)는 자동으로 상속된다.
    if ([WinJob]::Add($script:Job, $proc.Id)) { Say "  pid=$($proc.Id) 잡에 묶음 (워치독과 함께 종료)" }
    else { Say "  pid=$($proc.Id) 잡 편입 실패 — 이 창은 따로 닫아야 한다." "WARN" }
  }
  # 같은 title로 다시 띄우면 항목을 갈아끼운다. 되살릴 때 목록이 불어나지 않게.
  $script:Windows[$title] = @{ cmd = $cmd; probe = $probe; proc = $proc; started = Get-Date }
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

function Restore-Windows {
  # 창(powershell)은 -NoExit라 안의 파이썬이 죽어도 남는다. 빈 창만 보면 살아 있는 줄 안다.
  # 그래서 파이썬 프로세스의 명령줄에서 스크립트 이름을 직접 찾는다.
  if ($DryRun -or $script:Windows.Count -eq 0) { return }
  $procs = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='py.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue)
  foreach ($title in @($script:Windows.Keys)) {
    $w = $script:Windows[$title]
    if (-not $w.probe) {
      # 파이썬 프로세스가 아니라 확인할 probe가 없다(예: quant-wsl-keepalive) — 창(powershell)
      # 자체가 죽었는지만 본다.
      if ($w.proc -and $w.proc.HasExited) {
        Say "부속 창 '$title'이 죽었다 — 다시 띄운다." "WARN"
        Start-Window $title $w.cmd $w.probe
      }
      continue
    }
    # 기동 직후에는 아직 파이썬이 안 뜬 상태일 수 있다. 뜰 시간을 준다.
    if (((Get-Date) - $w.started).TotalSeconds -lt 45) { continue }
    if ($procs | Where-Object { $_.CommandLine -like "*$($w.probe)*" }) { continue }
    Say "부속 창 '$title' 안에서 $($w.probe)가 죽었다 — 다시 띄운다." "WARN"
    if ($w.proc -and -not $w.proc.HasExited) { Stop-Process -Id $w.proc.Id -Force -ErrorAction SilentlyContinue }
    if ($title -eq "quant-recorder") { Wait-Tsdb }
    Start-Window $title $w.cmd $w.probe
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

$dup = Get-Process quant_trader -ErrorAction SilentlyContinue
if ($dup) {
  # 두 프로세스가 같은 계좌에 발주하면 원장이 깨진다. 자동으로 정리하지 않고 멈춘다.
  Say "quant_trader가 이미 $($dup.Count)개 떠 있다. 중복 발주를 막기 위해 중단한다." "ERROR"
  Save-Status "aborted" @{ error = "duplicate_process"; pids = @($dup.Id) }
  exit 2
}
if (-not (Test-Path $Exe))     { Say "실행파일 없음: $Exe — /build 먼저." "ERROR"; Save-Status "aborted" @{ error = "no_exe" }; exit 2 }
if (-not (Test-Path $Config))  { Say "config 없음: $Config" "ERROR"; Save-Status "aborted" @{ error = "no_config" }; exit 2 }

# 낡은 바이너리로 매매하지 않는다. 예전엔 소스가 exe보다 새면 중단하고 사람이 /build를 돌렸는데,
# 그 사이 매매가 안 됐다. 이제 기동 전에 한 번 빌드한다(증분이라 바뀐 게 없으면 몇 초).
# 실패하면 옛 exe가 남아 있어도 매매하지 않는다. 장중 재기동에는 빌드가 없다 — 여기 한 번뿐이다.
$head  = (git rev-parse --short HEAD 2>$null)
$dirty = @(git status --porcelain Quant\src Quant\include 2>$null).Count
if ($dirty -gt 0) { Say "메인 트리에 미커밋 소스 변경 $dirty 건 — 그대로 빌드에 들어간다." "WARN" }
if ($NoBuild) {
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
try { $paper = [bool](Get-Content $Config -Raw | ConvertFrom-Json).kis.is_paper } catch { }
if ($null -eq $paper) { Say "설정 $Config 에서 kis.is_paper 를 읽지 못했다 — 계좌 모드를 모른 채 띄우지 않는다." "ERROR"; exit 2 }
Say ("계좌 모드: {0}" -f $(if ($paper) { "모의(is_paper=true)" } else { "실계좌(is_paper=false)" })) $(if ($paper) { "INFO" } else { "WARN" })

$py = if (Test-Path $VenvPy) { $VenvPy } else { Say "venv 없음 — 보조 프로세스는 FDR 없이 UNKNOWN만 낸다." "WARN"; "py" }

Save-Status "starting" @{ paper = $paper; head = $head; dirty = $dirty }

# ─────────────── 부속 창 ───────────────
if (-not $NoSidecar)   { Start-Window "quant-sidecar"   "& '$py' PYQuant\tools\macro_regime_feed.py --interval 180 --out Quant\config\regime.json" "macro_regime_feed.py" }
if (-not $NoUniverse)  {
  Say "유니버스 스캔(ALL) — 완료까지 기다린다. 이게 없으면 전략이 붙을 종목이 없다."
  if (-not $DryRun) {
    $rc = Run-Native "`"$py`" PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json"
    if ($rc -ne 0) { Say "유니버스 스캔 실패(rc=$rc) — 직전 스캔 파일로 진행한다." "WARN" }
  }
}
if (-not $NoPrices)    { Start-Window "quant-prices"    "& '$py' scripts\live_prices_feed.py" "live_prices_feed.py" }
if (-not $NoDashboard) { Start-Window "quant-dashboard" "py scripts\dashboard_server.py" "dashboard_server.py" }
if (-not $NoNotify)    { Start-Window "quant-notify"    "& '$py' scripts\notify_sidecar.py --config $Config --interval 1800" "notify_sidecar.py" }
# 네이티브 트레이더는 컨테이너가 아니라 ZMQ PUB(127.0.0.1:5555)만 낸다 — docker-compose의
# quant-recorder는 quant-engine 컨테이너를 구독하므로 이 프로세스를 못 본다(D-090 후속).
# 같은 호스트에서 직접 구독해 TimescaleDB에 적재한다.
if (-not $NoRecorder) {
  Say "WSL 배포판을 하루 종일 깨워 둔다(quant-wsl-keepalive) — TimescaleDB 컨테이너가 붙어 있을 곳"
  Start-Window "quant-wsl-keepalive" "while (`$true) { wsl -e sleep infinity; Start-Sleep -Seconds 2 }" ""
  Say "TimescaleDB 사전 점검 — WSL Docker 깨우기"
  Wait-Tsdb
  Start-Window "quant-recorder"  "& '$py' PYQuant\main.py record --host localhost --port 5555" "main.py record"
}

# ─────────────── 감시 루프 ───────────────
$deadline = [datetime]::ParseExact((Get-Date -Format "yyyy-MM-dd") + " " + $Until, "yyyy-MM-dd HH:mm", $null)
if ((Get-Date) -ge $deadline) { Say "이미 $Until 을 지났다. 매매하지 않고 종료." "WARN"; Save-Status "past_deadline" $null; exit 0 }

$shortRuns = 0     # 30초 미만 종료 연속 횟수. 크래시 루프로 계좌를 반복 호출하지 않기 위한 브레이크.
while ((Get-Date) -lt $deadline) {
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
    if (-not [WinJob]::Add($script:Job, $p.Id)) { Say "  트레이더 pid=$($p.Id) 잡 편입 실패 — 워치독이 죽으면 미연결으로 남는다." "WARN" }
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

  if ($secs -lt 30) {
    $shortRuns++
    if ($shortRuns -ge 3) {
      # 세 번 연속 즉사면 원인이 배선(config·인증·바이너리)에 있다. 재기동으로 풀리지 않는다.
      Say "30초 미만 종료 3연속 — 크래시 루프로 보고 멈춘다. 로그를 보고 고쳐야 한다." "ERROR"
      Save-Status "crash_loop" @{ error = "crash_loop"; last_exit = $p.ExitCode }
      exit 3
    }
  } else { $shortRuns = 0 }

  Say "5초 뒤 재기동."
  Start-Sleep -Seconds 5
}

Save-Status "closed" @{ }

# ─────────────── 마감 뒤 사실 정리 ───────────────
# 해석(리뷰 문장·개선안)은 클로드가 채우지만, 사실 문서와 대시보드는 사람 없이도 최신이어야 한다.
if (-not $NoEod -and -not $DryRun) {
  Say "장 마감 정리 — eod_autodoc(일지 사실 구간 + 리뷰 항목 + 대시보드 재생성)"
  py scripts\eod_autodoc.py
  Say "eod_autodoc rc=$LASTEXITCODE"

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
