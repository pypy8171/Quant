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
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -Config Quant\config\config.json -Until 15:35
  powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -DryRun
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_dev_paper.json",
  [string]$Until = "15:35",          # 이 시각을 넘으면 재기동하지 않는다(장 마감 15:30 + 정리 여유)
  [switch]$NoSidecar,
  [switch]$NoUniverse,
  [switch]$NoDashboard,
  [switch]$NoNotify,                 # 체결·포지션 메신저 알림 창을 띄우지 않는다
  [switch]$NoPrices,                 # 전 종목 시세 파일 전달(네이버 벌크) 창을 띄우지 않는다
  [switch]$NoEod,                    # 마감 뒤 사실 문서·대시보드 갱신을 건너뛴다
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONUTF8 = "1"

# ─────────────── 경로 ───────────────
# quant_trader는 repo 루트에서 떠야 한다. cwd가 다르면 유니버스 파일을 못 찾아 조용히 붕괴한다.
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

$Exe     = Join-Path $Repo "Quant\build_win\quant_trader.exe"
$VenvPy  = Join-Path $Repo "PYQuant\.venv-win\Scripts\python.exe"
$Status  = Join-Path $Repo "_private\_auto_trade_day.json"
$LogDir  = Join-Path $Repo "logs"
$RunLog  = Join-Path $LogDir ("auto_trade_day_{0}.log" -f (Get-Date -Format yyyyMMdd))
New-Item -ItemType Directory -Force -Path $LogDir, (Split-Path $Status) | Out-Null

$script:Sessions = @()
$script:Started  = Get-Date

function Say([string]$msg, [string]$level = "INFO") {
  $line = "[{0}] {1,-5} {2}" -f (Get-Date -Format "HH:mm:ss"), $level, $msg
  Write-Host $line
  Add-Content -Path $RunLog -Value $line -Encoding utf8
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

function Restore-Windows {
  # 창(powershell)은 -NoExit라 안의 파이썬이 죽어도 남는다. 빈 창만 보면 살아 있는 줄 안다.
  # 그래서 파이썬 프로세스의 명령줄에서 스크립트 이름을 직접 찾는다.
  if ($DryRun -or $script:Windows.Count -eq 0) { return }
  $procs = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='py.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue)
  foreach ($title in @($script:Windows.Keys)) {
    $w = $script:Windows[$title]
    if (-not $w.probe) { continue }
    # 기동 직후에는 아직 파이썬이 안 뜬 상태일 수 있다. 뜰 시간을 준다.
    if (((Get-Date) - $w.started).TotalSeconds -lt 45) { continue }
    if ($procs | Where-Object { $_.CommandLine -like "*$($w.probe)*" }) { continue }
    Say "부속 창 '$title' 안에서 $($w.probe)가 죽었다 — 다시 띄운다." "WARN"
    if ($w.proc -and -not $w.proc.HasExited) { Stop-Process -Id $w.proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Window $title $w.cmd $w.probe
  }
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

$src = Get-ChildItem -Recurse -File "Quant\src", "Quant\include" -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($src -and $src.LastWriteTime -gt (Get-Item $Exe).LastWriteTime) {
  Say "소스가 exe보다 새것이다($($src.Name)). 낡은 바이너리로 매매하지 않는다 — /build 후 다시." "ERROR"
  Save-Status "aborted" @{ error = "stale_exe"; newer = $src.Name }
  exit 2
}

# 주문이 나가는 계좌는 kis 블록이다. 최상위나 quote_kis(시세 전용)의 is_paper를 보면
# 모의를 실계좌로 잘못 읽는다 — 실제로 config_dev_paper는 quote_kis.is_paper=false다.
$paper = $true
try { $paper = [bool](Get-Content $Config -Raw | ConvertFrom-Json).kis.is_paper } catch { }
Say ("계좌 모드: {0}" -f $(if ($paper) { "모의(is_paper=true)" } else { "실계좌(is_paper=false)" })) $(if ($paper) { "INFO" } else { "WARN" })

$py = if (Test-Path $VenvPy) { $VenvPy } else { Say "venv 없음 — 보조 프로세스는 FDR 없이 UNKNOWN만 낸다." "WARN"; "py" }

Save-Status "starting" @{ paper = $paper }

# ─────────────── 부속 창 ───────────────
if (-not $NoSidecar)   { Start-Window "quant-sidecar"   "& '$py' PYQuant\tools\macro_regime_feed.py --interval 180 --out Quant\config\regime.json" "macro_regime_feed.py" }
if (-not $NoUniverse)  {
  Say "유니버스 스캔(ALL) — 완료까지 기다린다. 이게 없으면 전략이 붙을 종목이 없다."
  if (-not $DryRun) {
    & $py PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json
    if ($LASTEXITCODE -ne 0) { Say "유니버스 스캔 실패(rc=$LASTEXITCODE) — 직전 스캔 파일로 진행한다." "WARN" }
  }
}
if (-not $NoPrices)    { Start-Window "quant-prices"    "& '$py' scripts\live_prices_feed.py" "live_prices_feed.py" }
if (-not $NoDashboard) { Start-Window "quant-dashboard" "py scripts\dashboard_server.py" "dashboard_server.py" }
if (-not $NoNotify)    { Start-Window "quant-notify"    "& '$py' scripts\notify_sidecar.py --config $Config --interval 1800" "notify_sidecar.py" }

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
    & py (Join-Path $Repo "scripts\check_runtime_health.py") --since $script:LastStart 2>&1 |
      ForEach-Object { Say "  $_" }
  }

  $t0 = Get-Date
  $script:LastStart = $t0.ToString("HH:mm")
  # 이전 세션이 남긴 미체결 주문 목록을 로그에서 복원해 보조 프로세스에 채운다. 엔진이 기동하면서
  # cancel_stale_orders()가 이 파일을 읽어 전부 취소한다 — 유령 지정가가 현금과 매도가능수량을
  # 묶고, 청산 직후 되사서 전략을 뒤집는 것을 막는다(2026-09-08 미체결 85건 실측).
  # 엔진이 스스로 쓰는 보조 프로세스가 정상이면 이 복원은 같은 내용을 다시 쓸 뿐이라 무해하다.
  & py (Join-Path $Repo "scripts\seed_open_orders.py") 2>&1 | ForEach-Object { Say "  $_" }

  # 트레이더도 잡에 넣는다. 워치독이 사라졌는데 엔진만 살아 있으면 아무도 감시하지 않는 채
  # 발주가 계속되고, 다음 기동은 중복 프로세스로 막힌다(duplicate_process). 같이 내리고
  # 감시자(auto_trade_guard.ps1)가 다시 띄우면 잔고 재시드가 포지션을 도로 잡는다.
  $p = Start-Process -FilePath $Exe -ArgumentList $Config -WorkingDirectory $Repo -PassThru -NoNewWindow
  if ($script:Job -ne [IntPtr]::Zero) {
    if (-not [WinJob]::Add($script:Job, $p.Id)) { Say "  트레이더 pid=$($p.Id) 잡 편입 실패 — 워치독이 죽으면 미연결으로 남는다." "WARN" }
  }
  Save-Status "running" @{ pid = $p.Id; session = $n }
  # WaitForExit로 통째로 막지 않는다. 트레이더를 기다리는 동안 부속 창 안의 파이썬이
  # 죽었는지도 같이 본다 — 알림·국면 보조 프로세스가 조용히 사라지는 것을 놓치지 않기 위해서다.
  while (-not $p.HasExited) {
    if ($p.WaitForExit(60000)) { break }
    Restore-Windows
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
