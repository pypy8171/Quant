# 자동화 스크립트 작성 가이드 — PowerShell·Python

`scripts/auto_trade_day.ps1` 같은 운영 스크립트와 `scripts/eod_autodoc.py`·`scripts/dashboard_server.py` 같은 파이썬 자동화를
**직접 읽고, 고치고, 처음부터 쓸 수 있게** 되는 것이 이 문서의 목적이다. 저장소 `scripts/`의 `.ps1` 8개·`.py` 42개에 실제로
쓰인 문법·API·설계 패턴만 다룬다. 쓰이지 않은 기능은 다루지 않는다.

예제는 전부 저장소 코드에서 그대로 가져왔고, 옆에 어느 파일 어느 함수인지 적었다. 절마다 **무엇 → 왜 → 코드 → 어디서 쓰였나** 순서다.
17절은 이 지식으로 작은 감시견을 처음부터 짜 보는 연습이다.

| 부 | 절 | 내용 |
|---|---|---|
| 1부 PowerShell | 1–8 | 실행 환경, 뼈대, 문법 사전, 파일·로그, 네이티브 호출, 프로세스·Job Object, 시간, 예약작업 |
| 2부 Python | 9–14 | 뼈대와 공용 모듈, 로그·원장 파싱, 마크다운 생성·gen 블록, 대시보드 HTTP 서버, 외부 호출·알림, 검사 스크립트 |
| 3부 공통 | 15–18 | 설계 패턴(상태 파일·세 층·브레이크), 디버깅, 연습(감시견 60줄), 저장소에 넣을 때 |

### 어느 스크립트가 어떤 기법을 쓰는가

| 스크립트 | 역할 | 여기서 배울 기법(절) |
|---|---|---|
| `scripts/auto_trade_day.ps1` | 하루 루프 워치독 | 뼈대(2) · FileStream 로그(4.2) · Run-Native(5.3) · Start-Process·Job Object(6) · 상태 파일(15.1) |
| `scripts/auto_trade_guard.ps1` | 5분 감시자 | 예약작업 등록(8) · Win32_Process 명령줄 판정(6.4) · 날짜 검사(15.1) |
| `scripts/quant_procs.ps1` | 프로세스 현황·정리 | 역할 판정표·부모-자식 묶기(6.4) · pscustomobject 표(3.2) |
| `scripts/eod_timetable.ps1` | 시간표 예정 vs 실제 | 예약작업 조회·schtasks(8) · `-Lines` 기계용 출력(13.2) |
| `scripts/unattended_run.ps1` | 무인 이어달리기 | 인코딩(1.3) · 히어스트링(3.3) · 플래그 파일(15.1) |
| `scripts/_logdir.py` | 로그·원장 폴더 규칙 | 공용 모듈·후보 탐색(9.3) |
| `scripts/eod_autodoc.py` | 마감 일지·대시보드 | 로그·CSV 파싱(10) · AUTO 마커 병합(11.1) · subprocess 위임(13.1) |
| `scripts/dashboard_server.py` | 실시간 대시보드 | ThreadingHTTPServer·데몬 스레드·잠금(12) |
| `scripts/gen_facts.py`·`scripts/gen_automation_hub.py` | 생성물 | gen 블록 치환(11.2) · PowerShell·schtasks 호출(13.2) |
| `scripts/check_runtime_health.py` | 건전성 점검 | 정규식 검사·종료코드(14) |
| `scripts/notify_sidecar.py` | 체결 알림 | CSV 증분 읽기(10.3) · 웹훅(13.3) |

---

# 1부 — PowerShell

## 1. 실행 환경 — 스크립트가 도는 바닥

### 1.1 PowerShell 5.1과 7의 차이

이 저장소 스크립트는 **Windows PowerShell 5.1**(`powershell.exe`, Windows에 내장)에서 돈다. 예약작업·바탕화면 바로가기가
전부 `powershell.exe`를 부르기 때문이다. 5.1에는 없는 것:

| 없는 것 | 대신 쓰는 것 |
|---|---|
| `&&` `\|\|` 체인 | `A; if ($?) { B }` 또는 `cmd /c "A && B"` |
| 삼항 `?:`, `??`, `?.` | `if/else`, `$null -eq $x` |
| `ConvertFrom-Json -AsHashtable` | 결과는 `PSCustomObject`, 점으로 접근(`$st.phase`) |
| 기본 UTF-8 | 직접 지정(1.3절) |

### 1.2 실행 정책과 호출 방식

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -Config Quant\config\config.json
```

| 인자 | 뜻 |
|---|---|
| `-ExecutionPolicy Bypass` | 서명 없는 `.ps1`을 막는 기본 정책을 이 호출에만 푼다 |
| `-NoProfile` | 사용자 프로필(`$PROFILE`)을 읽지 않는다. 예약작업에서 프로필이 뜨는 시간·부작용을 없앤다 |
| `-File <경로> <인자…>` | 파일을 실행한다. `param()` 인자를 그대로 넘길 수 있다 |
| `-Command "<문장>"` | 문자열을 실행한다. 부속 창처럼 짧은 한 줄을 띄울 때(4.2절) |
| `-NoExit` | 명령이 끝나도 창을 닫지 않는다. 안의 파이썬이 죽어도 창은 남는다(6.4절 함정) |

`-File`과 `-Command`의 구분은 감시자가 워치독을 세는 데 쓰인다(`scripts/auto_trade_guard.ps1` "워치독 생존 확인"):
`-Command`로 띄운 래퍼 창의 명령줄에도 스크립트 이름이 들어 있어, 그것까지 세면 죽은 워치독을 살아 있다고 오판한다.

### 1.3 인코딩 — 한글 경로·한글 출력 세 줄

스크립트 첫머리에 반드시 이 셋이 있다.

```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8   # 콘솔에 찍는 글자
$env:PYTHONUTF8 = "1"                                       # 자식 파이썬의 stdout
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)  # 네이티브 exe로 파이프할 때(unattended_run.ps1)
```

그리고 **파일 자체는 BOM 있는 UTF-8**로 저장한다(각 `.ps1` 첫 바이트가 `﻿`). 5.1은 BOM이 없으면 파일을 ANSI로 읽어
한글 문자열 리터럴이 깨진다. `scripts/unattended_run.ps1` 머리 주석에 "2026-09-17 첫 시험에서 실제로 깨졌다"가 이 이유다.

파일에 쓸 때도 `-Encoding utf8`을 명시한다(`Set-Content`·`Add-Content`는 기본이 ANSI).

---

## 2. 스크립트 뼈대 — 모든 스크립트가 같은 순서로 시작한다

```powershell
<#
.SYNOPSIS
  한 줄 요약
.DESCRIPTION
  왜 있는지, 무엇을 맡고 무엇을 안 맡는지
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\xxx.ps1 -DryRun
#>
[CmdletBinding()]
param(
  [string]$Config = "Quant\config\config_dev_paper.json",
  [string]$Until  = "15:35",
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo
```

| 조각 | 역할 |
|---|---|
| `<# .SYNOPSIS … #>` | 주석 기반 도움말. `Get-Help scripts\auto_trade_day.ps1`이 읽는다. **왜**를 여기 쓴다 |
| `[CmdletBinding()]` | 스크립트를 cmdlet처럼 만든다. `-Verbose`·`-ErrorAction` 같은 공통 인자가 붙는다 |
| `param(...)` | 반드시 실행문보다 앞. `[string]`은 기본값 있는 선택 인자, `[switch]`는 있으면 `$true` |
| `[Parameter(Mandatory = $true)]` | 필수 인자(`scripts/unattended_run.ps1`의 `-Name`) |
| `[ValidateSet('', 'paper', 'live')]` | 허용값 제한(`scripts/eod_timetable.ps1`의 `-Mode`) |
| `$ErrorActionPreference = "Stop"` | cmdlet 오류를 예외로 올린다. 조용히 넘어가서 반쯤 된 상태로 계속 도는 것을 막는다. 대신 5.4·6.2절 함정이 생긴다 |
| `$PSScriptRoot` | 이 `.ps1`이 있는 폴더. `Split-Path -Parent`로 한 단계 올라가면 저장소 루트 |
| `Set-Location $Repo` | 이후 모든 상대경로의 기준. `quant_trader`는 루트가 아니면 유니버스 파일을 못 찾는다 |

`$PSCommandPath`는 스크립트 파일 자기 경로다(감시자가 예약작업에 자기를 등록할 때 씀).

### 2.1 `-DryRun` 관례

부작용이 있는 모든 지점 앞에 `if ($DryRun) { Say "  (dry) …"; return }`을 둔다. 로직은 다 타고 실행만 건너뛰어,
장 밖에서도 흐름을 검증할 수 있다. 새 스크립트를 쓸 때 처음부터 넣는다.

---

## 3. 문법 사전 — 코드에 쓰인 것만

### 3.1 변수와 스코프

```powershell
$script:Sessions = @()          # 스크립트 전체에서 하나. 함수 안에서 바꾸려면 반드시 $script: 접두
$script:Sessions += [ordered]@{ n = 1; exit = 0 }
$local = 5                      # 함수 안에서 만들면 함수 안에서만
$env:KIS_TOKEN_CACHE_DIR = "…"  # 환경변수. 이 프로세스와 그 자식이 물려받는다
$null = $p.Handle               # 반환값 버리기. `| Out-Null`과 같다
```

함수 안에서 `$Sessions += …`라고 쓰면 **로컬 복사본**이 생기고 바깥은 안 바뀐다. 이 저장소에서 상태를 갖는 변수는 전부 `$script:`다.

### 3.2 컬렉션

```powershell
$a = @()                       # 배열. += 는 매번 새 배열을 만든다(수십 개까지는 무방)
$a = @(Get-Process quant_trader -ErrorAction SilentlyContinue)   # 결과가 0개·1개·N개여도 항상 배열
$a.Count; $a[-1]               # 개수, 마지막 원소
$h = @{ cmd = "…"; proc = $p } # 해시. 순서 없음
$o = [ordered]@{ phase = "…" } # 순서 있는 해시. JSON으로 쓸 때 키 순서가 유지된다
$o["extra"] = 1; $o.Keys       # 접근
[pscustomobject]@{ 역할 = "trader"; PID = 1 }   # 표(Format-Table)로 찍을 객체
```

`@( … )`로 감싸는 습관이 중요하다. 감싸지 않으면 결과가 하나일 때 `.Count`가 없거나(5.1) 스칼라로 풀려 `foreach`가 이상해진다.

### 3.3 문자열

```powershell
"세션 #$n 기동 — $Exe"                      # 큰따옴표: 변수 치환
"pid=$($proc.Id)"                          # 속성·식은 $( )로
'literal $not_expanded'                    # 작은따옴표: 치환 없음
"[{0}] {1,-5} {2}" -f (Get-Date -Format "HH:mm:ss"), $level, $msg   # 서식. {1,-5}는 왼쪽정렬 5칸
"auto_trade_day_{0}.log" -f (Get-Date -Format yyyyMMdd)
"`"$py`" tools\x.py"                       # 백틱(`)이 이스케이프. 큰따옴표 안의 큰따옴표
"…`$Host.UI.RawUI.WindowTitle='$title'…"   # `$ 는 치환 막기 — 자식 창에서 풀리게 하려고
$s -replace '\\', '/'                      # 정규식 치환
($out | Where-Object { … } | Select-Object -First 3) -join ' | '
@'
여러 줄
그대로
'@                                          # 히어스트링. 닫는 '@는 열 0에
```

### 3.4 비교·논리

`-eq -ne -lt -le -gt -ge`, `-and -or -not`, `-like "*조각*"`(와일드카드), `-match '정규식'`, `-contains`(배열 ∋ 값), `-in`.
`==`·`!=`·`&&`는 없다.

```powershell
if ($now.ToString("HHmm") -ge "0900")      # 시각을 "HHmm" 문자열로 비교 — 날짜 없이 오늘 시각만 볼 때
if ($stop -contains $st.phase)             # 배열에 값이 있나
if ($null -eq $paper)                      # $null 비교는 왼쪽에 $null
```

### 3.5 정규식과 `$Matches`

```powershell
if ($_ -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$') {
  [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
}
if ($guard.Actions[0].Arguments -match '-Until\s+(\S+)') { $Matches[1] }
```

`-match`가 참이면 자동변수 `$Matches`에 그룹이 들어간다(`[0]` 전체, `[1]`부터 괄호). 거짓이면 `$Matches`는 이전 값 그대로라
반드시 `if` 안에서만 읽는다.

### 3.6 제어문·파이프라인

```powershell
foreach ($title in @($script:Windows.Keys)) { … }   # 순회 중 해시를 바꾸려면 Keys를 @()로 복사
for ($attempt = 1; $attempt -le 5; $attempt++) { … }
while (-not $p.HasExited) { … }
$out | ForEach-Object { Write-RunLog "$prefix$_" }   # $_ 는 현재 원소
$procs | Where-Object { $_.CommandLine -like "*$probe*" }
$rows | Sort-Object 역할, 기동 | Format-Table -AutoSize | Out-String -Width 120
$g.Group | Sort-Object Start ; $ordered[-1]          # 가장 최근 것
$roots | Group-Object Role                          # 역할별 묶음 → .Name .Count .Group
$victims | Sort-Object Pid -Unique | Sort-Object { -$_.Pid }   # 스크립트블록으로 정렬 키
$py = if (Test-Path $VenvPy) { $VenvPy } else { "py" }         # if 는 식이다 — 값을 돌려준다
$v = if ($g.Count -le 1) { "정상" } elseif (…) { … } else { "중복" }
break; continue; return; exit 2                     # exit 코드는 호출자(예약작업·감시자)가 읽는다
```

### 3.7 함수

```powershell
function Say([string]$msg, [string]$level = "INFO") { … }
Say "창 기동: $title"                 # 호출은 공백 구분. Say("a","b")는 틀리다(배열 하나로 들어간다)
function Run-Native([string]$cmdline, [string]$prefix = "    ") { … ; return $rc }
$rc = Run-Native "…"
```

함수 안에서 파이프에 흘려보낸 값은 **전부** 반환값이 된다. `Stop-Process` 같은 것이 뭔가 출력하면 반환값에 섞이므로
`| Out-Null`이나 `$null =`로 버린다.

### 3.8 예외 처리

```powershell
try { Add-Type -TypeDefinition $JobSrc } catch { Say "…($($_.Exception.Message))" "WARN" }
try { … } catch [System.IO.IOException] { Start-Sleep -Milliseconds 200 }   # 종류별
try { … } finally { Marshal.FreeHGlobal(buf) }
Get-Process quant_trader -ErrorAction SilentlyContinue     # 이 cmdlet만 오류 무시
Stop-Process -Id $k -Force -ErrorAction Stop               # 이 cmdlet만 예외로 승격(catch 하려고)
```

`$ErrorActionPreference = "Stop"` 아래서 `-ErrorAction SilentlyContinue`는 "없어도 되는 조회"에만 쓴다.

---

## 4. 파일·로그·설정

### 4.1 경로

```powershell
Join-Path $Repo "Quant\config"          # 구분자를 알아서 붙인다. 문자열 결합보다 이걸 쓴다
Split-Path -Parent $path                # 상위 폴더
Test-Path $Exe                          # 있나
New-Item -ItemType Directory -Force -Path $LogDir, (Split-Path $Status) | Out-Null   # mkdir -p. 여러 개 한 번에
Get-Item $Exe).LastWriteTime            # 수정 시각 — 소스보다 exe가 낡았는지 비교에 씀
Get-ChildItem -Recurse -File "Quant\src" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Resolve-Path -Relative $RunLog          # 현재 위치 기준 상대경로
[IO.Path]::IsPathRooted($Config)        # 절대경로인가
Get-Content $f -Raw                     # 통째로 한 문자열(JSON 파싱용). -Raw 없으면 줄 배열
Get-Content $f -TotalCount 1            # 첫 줄만
```

### 4.2 로그 쓰기 — `Add-Content`의 함정과 `FileStream`

단순 스크립트는 `Add-Content -Path $log -Value $line -Encoding utf8`이면 된다(`scripts/auto_trade_guard.ps1`의 `Say`).
그러나 워치독은 다르다. **다른 프로세스가 그 파일을 열어 두면**(예: 다른 세션의 `tail -f`) `Add-Content`가 `IOException`을 내고,
`$ErrorActionPreference = Stop` 아래서는 그 한 줄이 감시견 전체를 끊는다(2026-09-18 10:26 실제로 트레이더 미기동).

그래서 `scripts/auto_trade_day.ps1`의 `Write-RunLog`는 .NET을 직접 쓴다:

```powershell
$stream = [System.IO.File]::Open($RunLog, [System.IO.FileMode]::Append,
                                 [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
$writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
$writer.WriteLine($line); $writer.Close()
```

핵심은 `FileShare.ReadWrite` — 남이 읽고 있어도 붙는다. 그래도 실패하면 5번 재시도, 끝내 안 되면 화면에만 찍고 넘어간다.
**원칙: 로그 한 줄은 잃어도 되지만 감시견은 죽으면 안 된다.**

`[System.IO.File]`·`New-Object System.IO.StreamWriter`처럼 PowerShell은 .NET 클래스를 그대로 부른다. cmdlet에 없는 기능은
.NET에서 찾는다.

### 4.3 JSON — 상태 파일

```powershell
# 쓰기
$o = [ordered]@{ phase = $phase; updated = (Get-Date).ToString("s"); history = $script:Sessions }
$o | ConvertTo-Json -Depth 6 | Set-Content -Path $Status -Encoding utf8
# 읽기
$st = Get-Content $Status -Raw | ConvertFrom-Json
if (([datetime]$st.updated).Date -eq $now.Date -and $stop -contains $st.phase) { … }
$paper = [bool](Get-Content $Config -Raw | ConvertFrom-Json).kis.is_paper
```

`-Depth`를 안 주면 기본 2라 중첩된 `history` 배열이 문자열로 뭉개진다. 읽은 결과는 `PSCustomObject`라 `$st.phase = "closed"`처럼
고쳐서 다시 `ConvertTo-Json`할 수 있다(`scripts/quant_procs.ps1` `-KillAll`).

설정에서 어느 키를 읽을지가 곧 안전 규칙이다: 주문 계좌는 `kis.is_paper`이고 최상위·`quote_kis`는 시세 계정이라 보지 않는다.
못 읽으면 "모의로 가정"하지 않고 멈춘다(실계좌를 모의로 착각하는 쪽이 더 위험하다).

### 4.4 `.env` 로드

비밀번호는 소스에 없다. 저장소 루트 `.env`(gitignore)를 줄 단위 정규식으로 읽어 프로세스 환경변수로 싣는다(3.5절 예제).
`"Process"` 범위라 이 프로세스와 **그 자식**(부속 창·트레이더)만 본다. 레지스트리에는 남지 않는다.

---

## 5. 네이티브 프로그램 호출 — `py`·`git`·`cmake`·`wsl`

### 5.1 세 가지 호출법

```powershell
py scripts\eod_autodoc.py                       # 그냥 쓴다. 인자에 변수가 없을 때
& $py PYQuant\tools\universe_feed.py --market ALL   # & 호출 연산자: 경로가 변수·공백 포함일 때
& "C:\Program Files\…\cmake.exe" --build …
cmd /c "`"$vcvars`" >nul 2>&1 && `"$cmake`" --build Quant\build_win --target quant_trader 2>&1"
```

세 번째가 중요하다. `&&`·`2>&1`·`>nul`은 **cmd 문법**이라 5.1에서는 `cmd /c "…"`로 감싸서 cmd가 처리하게 한다.
`vcvars64.bat`처럼 환경변수를 바꾸는 배치는 같은 cmd 안에서 이어 붙여야 그 환경으로 다음 명령이 돈다.

### 5.2 종료 코드

```powershell
$out = cmd /c "$cmdline 2>&1"
$rc = $LASTEXITCODE                 # 네이티브 exe의 종료 코드. cmdlet은 $? (성공 여부만)
```

`$LASTEXITCODE`는 다음 네이티브 호출에 덮어써지므로 **바로 다음 줄에서** 변수에 받는다.

### 5.3 stderr 합치기의 함정 → `Run-Native`

5.1은 `$ErrorActionPreference = Stop` 아래서 네이티브 stderr 한 줄을 `2>&1`로 받는 순간 `NativeCommandError`로 스크립트를 끊는다.
그래서 `scripts/auto_trade_day.ps1`은 합치기를 cmd 안에서 하고 PowerShell은 문자열만 받는 `Run-Native`를 둔다:

```powershell
function Run-Native([string]$cmdline, [string]$prefix = "    ") {
  $out = cmd /c "$cmdline 2>&1"
  $rc = $LASTEXITCODE
  $out | ForEach-Object { Write-RunLog "$prefix$_" }   # 출력 전부 로그에
  return $rc
}
$rc = Run-Native "`"$py`" PYQuant\tools\universe_feed.py --market ALL --out Quant\config\universe_scan.json"
if ($rc -ne 0) { Say "유니버스 재스캔 실패(rc=$rc) — 직전 파일 유지." "WARN" }
```

이 함수가 생긴 이유는 "2026-09-16 rc=1의 사유가 어디에도 안 남아서"다. 자식 출력은 **반드시 로그에 남긴다**.

### 5.4 인용 부호 조립

cmd 한 줄 안에 공백 있는 경로를 넣을 때는 `` `"$py`" ``로 큰따옴표를 안에 넣는다. `-f` 서식으로 조립하면 읽기 쉽다:

```powershell
Run-Native ("py `"{0}`" --since {1}" -f (Join-Path $Repo "scripts\check_runtime_health.py"), $script:LastStart)
```

### 5.5 git 조회

```powershell
$head  = (git rev-parse --short HEAD 2>$null)
$dirty = @(git status --porcelain Quant\src Quant\include 2>$null).Count
```

`2>$null`은 stderr를 버린다(git이 없거나 저장소가 아니어도 스크립트가 안 죽게).

### 5.6 WSL·Docker

```powershell
$tsdb = (cmd /c 'wsl -e docker inspect quant-tsdb --format "{{.State.Health.Status}}" 2>nul')
if ($LASTEXITCODE -ne 0 -or $tsdb -notmatch "healthy") { … 5초 간격 6회 대기 … }
Start-Window "quant-wsl-keepalive" "while (`$true) { wsl -e sleep infinity; Start-Sleep -Seconds 2 }" ""
```

`wsl -e <명령>`은 명령이 끝나면 배포판 인스턴스가 곧 내려간다(09-16 실측, `.wslconfig`로 못 막는다). 그래서 `sleep infinity`를
하루 종일 붙여 두는 창을 하나 둔다. 외부 시스템의 실제 동작은 **문서가 아니라 실측**으로 확인하고 그 실측을 주석에 남긴다.

---

## 6. 프로세스 — 띄우기·기다리기·찾기·내리기

### 6.1 `Start-Process`

```powershell
# 트레이더: 같은 창 안에서, 핸들 받아서
$p = Start-Process -FilePath $Exe -ArgumentList $Config -WorkingDirectory $Repo -PassThru -NoNewWindow
# 부속 창: 새 창으로, 제목 붙여서
$inner = "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; `$Host.UI.RawUI.WindowTitle='$title'; Set-Location '$Repo'; $cmd"
$proc = Start-Process powershell -ArgumentList @("-NoExit", "-NoProfile", "-Command", $inner) -PassThru
# 다른 스크립트: 결과는 안 봄
Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Day, "-Config", $Config) -WorkingDirectory $Repo | Out-Null
```

| 인자 | 뜻 |
|---|---|
| `-PassThru` | `Process` 객체를 돌려준다. 없으면 pid도 못 받는다 |
| `-NoNewWindow` | 현재 콘솔에 출력. 트레이더 로그를 워치독 창에서 본다 |
| `-WorkingDirectory` | 자식의 cwd. 트레이더는 저장소 루트여야 한다 |
| `-ArgumentList @(…)` | 배열로 주면 인용을 알아서 한다 |

**창 제목 규약**: `$Host.UI.RawUI.WindowTitle='quant-…'`. 이 문자열이 자식 명령줄에 남아서 나중에 `quant_procs.ps1`이 "우리 창"을
가려낸다(6.4절). 제목 앞 접두 `quant-`가 곧 식별자다.

### 6.2 `ExitCode` 함정

```powershell
$null = $p.Handle     # 종료 전에 한 번 만져 둔다
…
$p.ExitCode           # 이제 채워진다
```

.NET `Process` 객체는 종료 전에 `Handle`을 읽어 둔 것만 `ExitCode`를 준다. 안 만지면 세션 기록·크래시 루프 판정이 전부 `null`을 본다.

### 6.3 기다리기 — 통째로 막지 않는다

```powershell
while (-not $p.HasExited) {
  if ($p.WaitForExit(60000)) { break }   # 60초 안에 끝나면 true
  Restore-Windows                        # 매 60초마다 부속 창 점검
  Refresh-Universe                       # 유니버스 재스캔 카운터
}
```

`$p.WaitForExit()`(인자 없음)로 막으면 트레이더가 살아 있는 동안 아무것도 못 한다. 타임아웃을 주고 루프를 돌리면
그 틈에 다른 점검을 끼워 넣을 수 있다. **주기 작업은 전부 이 루프 한 곳에 모은다.**

### 6.4 찾기 — `Get-Process`로는 부족하다

```powershell
Get-Process quant_trader -ErrorAction SilentlyContinue                 # 이름으로. exe 하나짜리는 이걸로 충분
$procs = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='py.exe'")
$procs | Where-Object { $_.CommandLine -like "*macro_regime_feed.py*" }  # 명령줄로 역할 판정
```

`powershell.exe`·`python.exe`는 여러 개 떠 있어 이름으로는 구분이 안 된다. `Win32_Process`는 `CommandLine`·`ParentProcessId`·
`CreationDate`를 준다. 이 저장소의 **역할 판정표**(`scripts/quant_procs.ps1` `$Roles`)는 명령줄에 들어 있는 스크립트 이름
조각으로 역할을 붙인다. 새 부속 프로세스를 추가하면 이 표에도 한 줄 넣는다.

**빈 창 문제**: `-NoExit` 창은 안의 파이썬이 죽어도 남는다. 창이 살았는지가 아니라 **파이썬 명령줄에 스크립트 이름이 있는지**를
본다(`Restore-Windows`가 창마다 기억해 둔 스크립트 이름 조각). 기동 직후 45초는 아직 안 떴을 수 있어 건너뛴다.

**부모-자식 묶기**: `py → python → python`은 하나의 인스턴스다. `ParentProcessId`를 따라 올라가 조상 중 같은 역할이 있으면
자식으로 친다(`Get-Ancestors`, 20홉 상한). 자기 자신과 자기 조상은 제외(`$selfChain`).

### 6.5 내리기

```powershell
Stop-Process -Id $pid_ -Force -ErrorAction SilentlyContinue
$orphan | Stop-Process -Force
```

순서 규칙: **자식을 먼저**(pid 큰 것부터). 부모 창을 먼저 죽이면 그 창의 재기동 로직이 돌 수 있다. 트레이더 중복은 자동으로
안 죽인다 — 어느 쪽이 진짜인지 스크립트는 모르고, 잘못 죽이면 원장이 깨진다. 보고만 하고 사람에게 넘긴다.

### 6.6 Job Object — 부모가 죽으면 자식도 같이

Windows는 부모가 죽어도 자식이 산다. 워치독이 죽으면 트레이더가 감시 없이 발주를 계속하고, 다음 기동은 `duplicate_process`로 막힌다.
해결은 커널의 **Job Object**에 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`를 걸고 자식을 전부 넣는 것 — 잡 핸들을 쥔 워치독이 사라지는 순간
커널이 자식을 내린다.

PowerShell cmdlet에는 없어서 C#을 `Add-Type`으로 컴파일해 Win32 API를 부른다(P/Invoke):

```powershell
$JobSrc = @'
using System; using System.Runtime.InteropServices;
public static class WinJob {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attr, string name);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint cb);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  … 구조체(EXTENDED_LIMIT) 정의 …
  public static IntPtr CreateKillOnClose() { … LimitFlags = 0x2000; SetInformationJobObject(job, 9, buf, cb) … }
  public static bool Add(IntPtr job, int pid) { OpenProcess(0x0100|0x0001 /*SET_QUOTA|TERMINATE*/) → AssignProcessToJobObject }
}
'@
Add-Type -TypeDefinition $JobSrc
$script:Job = [WinJob]::CreateKillOnClose()
[WinJob]::Add($script:Job, $proc.Id)
```

읽을 때 알아야 할 것:
- `[DllImport("kernel32.dll")]` — Windows API를 C#에서 부르는 선언. 함수 이름·인자 형은 MS 문서(`CreateJobObjectW` 등)와 맞춘다.
- `[StructLayout(LayoutKind.Sequential)]` — 구조체 메모리 배치를 C와 같게. `Marshal.SizeOf`·`StructureToPtr`로 포인터에 복사.
- `0x2000`·`9`·`0x0100` 같은 상수는 Windows SDK 헤더 값. 주석에 이름을 반드시 적는다.
- 잡에 들어간 프로세스의 **손자**(py → python)는 자동으로 상속된다.
- 실패해도 스크립트는 계속 간다(WARN만). 잡은 안전장치이지 필수 경로가 아니다.

이 계층 덕에 "워치독이 죽으면 → 전부 내려감 → 5분 안에 감시자가 다시 띄움 → 잔고 재시드"라는 **자동 복구 경로**가 성립한다.

---

## 7. 시간

```powershell
Get-Date                                                    # DateTime
(Get-Date).ToString("HHmm") -lt "1000"                      # 오늘 시각 비교는 문자열로(간단·자릿수 고정)
(Get-Date).ToString("s")                                    # ISO 2026-09-19T15:20:00
[datetime]::ParseExact("$today $Until", "yyyy-MM-dd HH:mm", $null)   # "15:35" → 오늘 15:35 DateTime
((Get-Date) - $t0).TotalSeconds                             # TimeSpan
$now.AddMinutes(10); $now.DayOfWeek -eq [DayOfWeek]::Saturday
Start-Sleep -Seconds 5; Start-Sleep -Milliseconds 200
[Xml.XmlConvert]::ToTimeSpan("PT7H").TotalHours             # 예약작업 XML의 ISO 8601 기간
```

**시각을 인자로 받을 때** `-Until "15:35"`처럼 문자열로 받고 스크립트 안에서 `ParseExact`로 오늘 날짜에 붙인다. 카운터(`$script:UnivNext`)는
"다음 실행 시각"을 DateTime으로 들고 `if ($now -lt $script:UnivNext) { return }`로 건너뛴다. 시간대별 간격은 함수 하나(`Get-UnivIntervalMin`)에 모은다.

---

## 8. Windows 예약작업

두 가지 방법이 있고 둘 다 쓴다.

**cmdlet(등록)** — `scripts/auto_trade_guard.ps1 -Install`:

```powershell
$act = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arg -WorkingDirectory $Repo
$trg = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday, Tuesday, Wednesday, Thursday, Friday -At $Open
$trg.Repetition = (New-ScheduledTaskTrigger -Once -At $Open -RepetitionInterval (New-TimeSpan -Minutes 5) `
                                            -RepetitionDuration (New-TimeSpan -Hours $Hours)).Repetition
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
                                    -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
Register-ScheduledTask -TaskName $TaskName -Action $act -Trigger $trg -Settings $set -Force | Out-Null
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
```

주간 트리거에는 반복 설정이 없어서 1회 트리거의 `.Repetition`만 떼어 붙인다(이 꼼수는 주석에 남아 있다).
`-MultipleInstances IgnoreNew`는 앞 회차가 아직 돌면 새 회차를 건너뛴다. `-ExecutionTimeLimit`은 멈춘 회차를 강제 종료한다.
로그온 세션에서 실행해야 창이 보인다(`-LogonType`을 지정하지 않으면 현재 사용자·대화형).

**조회·변경** — `scripts/eod_timetable.ps1`:

```powershell
$task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
([datetime]$task.Triggers[0].StartBoundary).ToString('HH:mm')        # 시작 시각
$task.Actions[0].Arguments -match '-Until\s+(\S+)'                     # 등록된 인자에서 값 꺼내기
& schtasks /change /tn $name /st $plan[$name] | Out-Null               # 시각만 바꾸기(cmdlet보다 짧다)
```

등록된 인자 문자열을 정규식으로 다시 읽는 것이 "지금 어느 config로 걸려 있나"를 아는 유일한 방법이다.
등록 후에는 반드시 `-Check`(기본)로 예정 vs 실제를 비교해 exit 1이 나는지 본다. 어긋남을 사람이 눈으로 찾지 않게 한다.

# 2부 — Python

파이썬 스크립트는 두 부류다. **마감 뒤 결정론 경로**(`eod_autodoc`·`gen_facts`·`maintain`·`check_*`: 입력 파일을 읽어 문서·JSON을 만들고
종료코드를 낸다)와 **장중 상주 프로세스**(`dashboard_server`·`notify_sidecar`·`live_prices_feed`: PowerShell 부속 창 안에서 하루 종일 돈다).
둘 다 표준 라이브러리만으로 짜는 것이 원칙이고(`requests` 하나 예외), 파이썬은 반드시 `py` 런처로 부른다(`python`은 스토어 스텁).

## 9. 뼈대 — 모든 파이썬 스크립트가 같은 머리를 가진다

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""한 줄 요약.

    py scripts/eod_autodoc.py                # 오늘
    py scripts/eod_autodoc.py --date 2026-09-07
    py scripts/eod_autodoc.py --dry-run      # 파일을 쓰지 않고 결과만 출력

왜 있는지, 무엇을 안 하는지(예: "LLM 없이 도는 결정론 경로다. 해석이 필요한 자리는 빈 칸으로 남긴다").
"""
from __future__ import annotations          # 타입 힌트에 list[str]·Path | None 을 3.9 이하에서도 쓰게

import argparse, json, re, subprocess, sys
from pathlib import Path

# 콘솔이 cp949(한글 Windows 기본)면 이모지·한글 출력에서 UnicodeEncodeError로 죽는다(예약작업 rc=267009의 원인).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parents[1]    # scripts/의 부모 = 저장소 루트
sys.path.insert(0, str(REPO / "scripts"))     # 옆 모듈(_logdir 등) import 용
import _logdir  # noqa: E402                  # E402 = "import가 맨 위가 아님" 경고를 일부러 끈다


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="YYYY-MM-DD (기본: 오늘)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    ...
    return 0                                   # 0 = 정상. 호출자(PowerShell $LASTEXITCODE·예약작업)가 읽는다


if __name__ == "__main__":
    raise SystemExit(main())
```

| 조각 | 역할 |
|---|---|
| 모듈 독스트링 | 실행 예·목적·안 하는 것. `.ps1`의 `.SYNOPSIS`와 같은 자리 |
| `reconfigure(encoding="utf-8")` | PowerShell의 `[Console]::OutputEncoding` 세 줄과 같은 역할. **없으면 예약작업에서 조용히 죽는다** |
| `REPO = Path(__file__).resolve().parents[1]` | `$Repo = Split-Path -Parent $PSScriptRoot`와 같다. 이후 경로는 전부 `REPO / "…"` |
| `sys.path.insert(0, …)` | 옆 파일을 모듈로 쓴다. `PYQuant/`도 같은 식으로 넣어 `from kis.client import KisClient` |
| `argparse` | `param()`과 같다. `action="store_true"`가 `[switch]` |
| `raise SystemExit(main())` | 종료코드 규약. `main()`이 int를 돌려준다 |

### 9.1 `--dry-run` 관례

PowerShell과 같다. 파일을 쓰는 함수마다 `dry` 인자를 받아 `if not dry: path.write_text(...)`. 결과 문장은 dry여도 그대로 찍어
무엇을 했을지 보이게 한다(`write_journal`이 `"AUTO 구간 갱신 …"`을 돌려주는 식).

### 9.2 pathlib — 경로는 문자열이 아니라 `Path`

```python
REPO / "strategies" / "DeviationScale" / "live"      # 결합은 /
path.exists(); path.is_dir(); path.stat().st_mtime    # 있나·폴더인가·수정 시각
path.parent / "quant_trader.log"                      # 옆 파일
path.parent.mkdir(parents=True, exist_ok=True)        # mkdir -p
path.read_text(encoding="utf-8"); path.write_text(s, encoding="utf-8")
with path.open("a", encoding="utf-8") as f: f.write(line + "\n")    # 이어 쓰기
path.open(encoding="utf-8-sig")                       # BOM 있는 CSV(엔진 원장)는 -sig로 BOM을 벗긴다
```

`encoding="utf-8"`을 **매번 명시**한다. Windows 기본은 cp949라 빼먹으면 한글 파일에서 깨진다.

### 9.3 공용 모듈 — `scripts/_logdir.py`

같은 날짜 원장이 두 폴더에 생길 수 있어(엔진은 exe 옆 `logs/`, 테스트는 cwd 기준) 소비자마다 고르는 규칙이 다르면 같은 날을 두고
다른 숫자를 낸다(2026-09-08: 561체결 원장을 7체결 시험 원장이 mtime으로 이겼다). 그래서 규칙을 **한 모듈**에 두고 9곳이 import한다:

```python
_logdir.log_dir()          # QUANT_LOG_DIR > quant_trader.log가 가장 최근인 후보 > 실재하는 첫 후보
_logdir.find_ledger(date)  # 행 수 최대, 동률이면 mtime 최신
_logdir.find_log(date)     # 고른 원장 옆의 로그
```

후보 목록을 순서대로 두고 `seen` 집합으로 중복을 거르는 꼴(`candidate_dirs`)이 이런 탐색의 기본형이다.
**규칙: 둘 이상의 스크립트가 같은 것을 찾으면 그 찾는 법은 모듈 하나로 뺀다.** 정규식도 같다 — `scripts/log_patterns.py`의 `PNL_RE`·`GUARD_ATTACH_RE`를
`eod_autodoc`·`check_runtime_health`가 같이 쓴다.

---

## 10. 로그·원장 파싱 — 사실을 뽑는 곳

### 10.1 엔진 로그는 정규식 한 벌로

```python
LINE_RE    = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.\d+ \[(\w+)\s*\] (.*)$")   # 날짜 시각 [레벨] 본문
SESSION_RE = re.compile(r"=== Quant Trader")                                                    # 세션 경계
SIZING_RE  = re.compile(r"사이징 백스톱: 종목당 명목 (\d+)원, 동시보유 (\d+)종목")
NAME_RE    = re.compile(r"(\d{6})\(([^)]{1,24})\)")                                             # 005930(삼성전자)

for line in log.open(encoding="utf-8", errors="replace"):
    m = LINE_RE.match(line)
    if not m:
        continue
    date, hms, level, body = m.groups()
    if SESSION_RE.search(body):
        sessions.append({"start": hms, ...})            # 세션마다 갈라 둔다 — 장중 재기동이 있으면 설정이 다르다
```

- `re.compile`은 모듈 상단에 상수로. 이름은 `_RE` 접미.
- `errors="replace"`: 엔진이 섞어 쓴 깨진 바이트 한 줄 때문에 하루치 파싱이 죽지 않게.
- 세션 경계(`=== Quant Trader`)로 잘라야 "몇 번째 기동의 설정인가"를 안다. PowerShell 상태 파일의 `history`와 같은 정보를 로그에서 복원하는 셈이다.
- 사유 문자열은 숫자를 `N`으로 바꿔(`re.sub(r"\d", "N", reason)[:44]`) 묶는다 — 그래야 "한도 1,234원 초과"와 "한도 5,678원 초과"가 한 종류로 센다.

### 10.2 CSV 원장은 `DictReader` + `Counter` + `defaultdict`

```python
rows = list(csv.DictReader(path.open(encoding="utf-8-sig", errors="replace")))
events = Counter(r["event"] for r in rows)                       # {"FILL": 561, "REJECTED": 12}
per = defaultdict(lambda: {"B": 0, "Bq": 0, "Bn": 0.0, "breason": Counter(), ...})   # 종목별 누적
for r in rows:
    if r["event"] != "FILL":
        continue
    notional = float(r.get("fill_qty") or 0) * float(r.get("fill_price") or 0)   # 빈 칸은 or 0
    per[r["ticker"]]["Bn" if r["side"] == "BUY" else "Sn"] += notional
```

`r.get("x") or 0` — 열이 없거나 빈 문자열이어도 `float("")`로 죽지 않는다. 비용 요율 같은 상수(`COMMISSION_RATE = 0.00015`)는
**C++ 쪽 상수와 같은 값임을 주석에 파일명까지 적어** 둔다(`Quant/src/risk/OrderGate.cpp kCommissionRate`).

### 10.3 증분 읽기 — 상주 프로세스가 파일을 따라가는 법

`scripts/notify_sidecar.py`는 원장 CSV를 폴링하며 **읽은 위치(offset)** 뒤만 읽어 새 체결을 알린다. 핵심 모양:

```python
pos = 0
while True:
    with path.open(encoding="utf-8-sig") as f:
        f.seek(pos)
        for line in f:
            handle(line)
        pos = f.tell()
    time.sleep(interval)
```

파일이 날짜별이라 자정에 이름이 바뀌면 `pos`를 0으로 되돌린다. 종목명 캐시처럼 매번 API를 부르면 비싼 것은 JSON 파일에 저장(`save_name_cache`)하고 기동 때 읽는다.

---

## 11. 마크다운·JSON 생성 — 사람 문서를 기계가 갱신하는 두 방식

### 11.1 AUTO 마커 — 파일 일부만 기계 소유

매매일지는 사람이 해석을 쓰고 기계가 사실을 쓴다. 한 파일에 둘이 공존하려면 **기계 구간을 마커로 경계**한다:

```python
AUTO_BEGIN = "<!-- AUTO:BEGIN -->"
AUTO_END   = "<!-- AUTO:END -->"

def write_journal(path: Path, body: str, dry: bool) -> str:
    if path.exists():
        old = path.read_text(encoding="utf-8")
        if AUTO_BEGIN not in old:
            return f"보존(수기 일지) {path.name}"                 # 마커 없는 파일 = 사람이 쓴 것. 건드리지 않는다
        new_auto = body.split(AUTO_BEGIN, 1)[1].rsplit(AUTO_END, 1)[0]
        head, rest = old.split(AUTO_BEGIN, 1)
        _, tail = rest.split(AUTO_END, 1)
        merged = head + AUTO_BEGIN + new_auto + AUTO_END + tail    # 마커 밖(head·tail)은 그대로
        if not dry:
            path.write_text(merged, encoding="utf-8")
        return f"AUTO 구간 갱신 {path.name}"
    ...신규 생성...
```

본문은 `lines: list[str]`에 `add(...)`로 모아 마지막에 `"\n".join(lines)` — 문자열 `+=` 반복보다 빠르고 읽기 쉽다.
숫자 서식은 작은 함수로(`won(x)`·`man(x)`·`signed(x)`) 한 곳에 모은다.

### 11.2 gen 블록 — 문서 안의 표를 사실 파일에서 채운다

`docs/AUTOMATION.md`의 시간표 표처럼 **손으로 적으면 반드시 낡는 숫자·표**는 `<!-- gen:이름 --> … <!-- /gen -->` 블록으로 두고
`scripts/gen_facts.py`가 `docs/facts.json`의 값으로 치환한다:

```python
GEN_RE = re.compile(r"(<!--\s*gen:([A-Za-z0-9_-]+)[^>]*-->)(.*?)(<!--\s*/gen(?::[A-Za-z0-9_-]+)?\s*-->)", re.S)
# 그룹: (여는 태그)(이름)(현재 본문)(닫는 태그). re.S 로 여러 줄. (.*?) 는 최소 일치 — 첫 닫는 태그에서 멈춘다

def apply(doc: Path, renderers: dict[str, Callable[[], str]]) -> list[str]:
    text = doc.read_text(encoding="utf-8")
    def repl(m):
        name = m.group(2)
        return m.group(1) + "\n" + renderers[name]() + "\n" + m.group(4)
    new = GEN_RE.sub(repl, text)
    if new != text: doc.write_text(new, encoding="utf-8")
```

기본 모드는 **검사**(낡은 블록이 있으면 exit 1, Stop 훅 `sync-gate.ps1`이 이걸 본다), `--apply`가 치환이다. 전체 파일이 생성물이면
(`_private/AUTOMATION_HUB.md`) 마커 없이 통째로 다시 쓴다(`gen_automation_hub.render()`) — 손으로 고치는 원본이 어디인지 파일 머리에 적는다.

### 11.3 JSON 사실 파일

```python
json.dumps(obj, ensure_ascii=False, indent=2)     # ensure_ascii=False 없으면 한글이 \\uXXXX
json.loads(path.read_text(encoding="utf-8"))
with open(cfg_path, encoding="utf-8") as f: cfg = json.load(f)
cfg.get("regime_file", "Quant/config/regime.json")   # 기본값은 get 으로
next((s.get("universe_file") for s in strat if isinstance(s, dict) and s.get("universe_file")), "기본값")   # 첫 일치
```

`docs/facts.json`이 PowerShell 상태 파일과 같은 자리다 — 세는 것은 여기서 세고, 문서는 이 값을 보여만 준다.

---

## 12. 대시보드 — 상주 HTTP 서버의 뼈대

`scripts/dashboard_server.py`(1,775줄)는 크지만 뼈대는 넷이다. 이 넷만 알면 새 대시보드를 만들 수 있다.

### 12.1 서버 — 표준 라이브러리 `ThreadingHTTPServer`

```python
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    kis = None                      # 클래스 속성에 공유 객체를 건다 — 핸들러는 요청마다 새로 만들어지므로
    cfg_path = None; cfg_mtime = 0.0

    def log_message(self, *a): pass                     # 요청마다 콘솔에 찍는 기본 로그를 끈다

    def _send(self, code, ctype, body: bytes):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
            pass                                        # 브라우저가 먼저 끊은 것. 서버는 계속 산다

    def do_GET(self):
        if self.path.startswith("/api/state"):
            try:
                body = json.dumps(build_state(...), ensure_ascii=False).encode("utf-8")
            except Exception as e:
                body = json.dumps({"__error__": str(e)}, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif self.path == "/":
            self._send(200, "text/html; charset=utf-8", PAGE_HTML.encode("utf-8"))

ThreadingHTTPServer.allow_reuse_address = False   # Windows: 기본 True면 같은 포트에 둘이 떠서 응답이 섞인다(09-11 실측)
srv = ThreadingHTTPServer((host, port), Handler)
srv.serve_forever()
```

브라우저 쪽은 HTML 문자열 상수 하나에 `fetch("/api/state")`를 3초마다 도는 JS를 넣는다. 파일을 따로 두지 않아 스크립트 하나로 끝난다.
오류도 200으로 `{"__error__": …}`를 보내 화면에서 보이게 한다 — 폴링 중 500이 나면 브라우저 콘솔에만 남고 사람은 못 본다.

### 12.2 백그라운드 수집 — 요청 스레드는 절대 기다리지 않는다

KIS 잔고 조회가 타임아웃(20초×3)이면 `/api/state`가 그만큼 멈춰 대시보드 전체가 죽는다. 그래서 **느린 것은 데몬 스레드가 주기 수집**하고
요청은 마지막 스냅샷만 읽는다:

```python
LIVE = {}
LIVE_LOCK = threading.Lock()

def _live_set(key, val):
    with LIVE_LOCK:
        LIVE[key] = {"val": val, "ts": time.time(), "err": None}

def _live_err(key, msg):
    with LIVE_LOCK:
        cur = LIVE.get(key)
        if cur and cur.get("val") is not None:
            cur["err"] = msg                 # 마지막 정상값은 유지, 지연 표식만
        else:
            LIVE[key] = {"val": None, "ts": time.time(), "err": msg}

def _warm_loop(kis, quote, interval=5.0, flow_every=6):
    tick = 0
    while True:
        try:
            _live_set("balance", _fetch_balance(kis))
        except Exception as e:
            _live_err("balance", str(e))
        if tick % flow_every == 0:           # 비싼 것(REST 6회)은 6주기에 한 번
            ...
        tick += 1
        time.sleep(interval)

threading.Thread(target=_warm_loop, args=(kis, quote), daemon=True).start()
```

- `daemon=True`: 메인(서버)이 끝나면 같이 죽는다. PowerShell의 Job Object가 하는 일을 프로세스 안에서 하는 셈.
- 공유 dict는 `Lock`으로 감싼다. 항목마다 `val·ts·err` 셋을 두면 화면이 "값 + 몇 초 전 + 지연 중"을 다 보여줄 수 있다.
- 실패해도 **마지막 정상값을 지우지 않는다**. 빈 화면보다 "5분 전 값 + 경고"가 낫다.

### 12.3 설정 다시 읽기 — mtime

```python
@classmethod
def current_cfg(cls):
    try:
        m = os.path.getmtime(cls.cfg_path)
        if m != cls.cfg_mtime:
            with open(cls.cfg_path, encoding="utf-8") as f:
                cls.cfg = json.load(f)
            cls.cfg_mtime = m
    except Exception:
        pass                                 # 파싱 실패면 마지막 정상본 유지
    return cls.cfg
```

기동 때 한 번만 읽으면 config를 고쳐도 재기동 전까지 옛 값이 화면에 남는다(09-11 실측). 수정 시각이 바뀐 때만 다시 읽는다.
인증 정보는 예외 — 기동 시점 것을 그대로 쓴다.

### 12.4 프로세스 안 환경 조정

```python
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")   # numpy import 전에. 안 하면 16코어에서 프로세스당 500MB
```

import 순서가 의미를 갖는 드문 경우라 주석 없이는 지워진다. 이런 줄은 왜 여기 있어야 하는지를 반드시 옆에 쓴다.

---

## 13. 외부 호출 — subprocess·PowerShell·웹훅

### 13.1 다른 스크립트에 위임

```python
r = subprocess.run([sys.executable, "scripts/refresh_dashboard.py", "--live", ymd], cwd=REPO,
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
lines = [l.strip() for l in (r.stdout or r.stderr or "").strip().splitlines() if l.strip()]
return lines or [f"refresh_dashboard rc={r.returncode}"]
```

- 인자는 **리스트**로(셸 인용 문제 없음). `sys.executable`은 지금 도는 파이썬 — `py`를 다시 찾지 않는다.
- `cwd=REPO`: PowerShell의 `-WorkingDirectory`. 상대경로를 쓰는 자식은 이게 없으면 조용히 실패한다.
- `capture_output=True, text=True, encoding="utf-8", errors="replace"` 넷을 항상 같이. 하나라도 빠지면 한글 출력에서 예외.
- 절차를 두 곳에 적지 않는다: `eod_autodoc`이 대시보드 재생성 순서를 자기 안에 갖지 않고 `refresh_dashboard.py`에 넘기는 이유가 독스트링에 있다("한쪽만 고쳐져 갈라진다").

### 13.2 PowerShell·OS 명령을 파이썬에서

```python
run = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-Lines", "-Mode", mode],
                     capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
for line in run.stdout.splitlines():
    parts = line.rstrip("\r").split("\t")          # PowerShell 쪽이 "이름<TAB>시각<TAB>명령" 으로 찍어 준다
```

경계 규약: **PowerShell 스크립트에 `-Lines` 같은 기계용 출력 모드를 두고** 파이썬은 탭으로 자른다. 사람용 표를 파싱하지 않는다.
`schtasks /query /fo csv /v`처럼 코드페이지가 셸마다 다른 출력은 바이트로 받아 utf-8 → cp949 순으로 풀어 본다(`gen_automation_hub.scheduled_tasks`).
`timeout=`과 `(OSError, subprocess.TimeoutExpired)` 처리를 두어 Linux나 멈춘 명령에서 스크립트가 통째로 죽지 않게 한다 — 실패하면
**지난 값을 그대로 둔다**(`facts.json`의 이전 시간표).

### 13.3 웹훅 알림

```python
r = requests.post(webhook_url, json={"content": text}, timeout=10)
r.raise_for_status()
```

수신처는 `_private/notify.json`(gitignore) 또는 환경변수 — `os.environ.get("DISCORD_WEBHOOK_URL") or conf.get("discord_webhook")` 순으로,
소스에는 없다. 채널이 여럿이면 용도별 키(`_fill`·`_position`)를 두고 없으면 기본 채널로 떨어지게 한다.

---

## 14. 검사 스크립트 — 종료코드가 곧 인터페이스

`check_runtime_health.py`·`check_docs.py`·`check_code_conventions.py`·`commit_gate.py`는 모두 같은 꼴이다:

```python
"""… 종료코드: 0 = FAIL 없음, 1 = FAIL 있음"""
CHECKS = [
    # (이름, 정규식, 임계값, 왜 넣었나 — 실제로 하루를 망친 날짜)
    ("유령주문 재부활", re.compile(r"…"), 0, "2026-09-08 85건"),
]

def main() -> int:
    bad = []
    for name, pattern, limit, why in CHECKS:
        count = sum(1 for line in log_lines if pattern.search(line))
        mark = "FAIL" if count > limit else "ok"
        print(f"  {mark:4} {name:24} {count:4}  ({why})")
        if count > limit: bad.append(name)
    return 1 if bad else 0
```

- 검사 항목은 **표 하나**(리스트의 튜플)에 모으고, 각 항목에 **왜 넣었는지 실측 날짜**를 붙인다. 독스트링의 규칙: "가정으로 만든 체크는 경보만 늘리고 신뢰를 깎는다".
- 출력은 항목마다 한 줄, 정렬된 열. PowerShell `Run-Native`가 이 줄들을 그대로 로그에 옮긴다.
- 호출자는 종료코드만 본다. 워치독은 재기동 전마다 `--since HH:MM`으로 직전 세션 구간만 검사한다.


---

# 3부 — 공통: 설계·디버깅·연습

## 15. 설계 패턴 — 문법 위에 얹힌 것

### 15.1 상태 파일 = 단일 진실

`_private/_auto_trade_day.json`의 `phase`가 오늘 상태다. 전이마다 `Save-Status`로 통째로 다시 쓴다(부분 수정 없음).

```
starting → running → (재기동 반복) → closed → done
                  ↘ crash_loop / aborted / past_deadline
```

읽는 쪽은 둘: 감시자(`stop` 집합이면 되살리지 않음)와 클로드(`/auto-trade-day`가 로그 대신 이 파일을 읽음).
**날짜 검사**(`updated.Date -eq 오늘`)를 빼먹으면 어제의 `done`이 오늘 기동을 막는다.

### 15.2 세 층 — 누가 누구를 되살리는가

| 층 | 무엇 | 되살리는 대상 | 자신이 죽으면 |
|---|---|---|---|
| 예약작업 `QuantAutoTradeGuard` | 5분마다 `auto_trade_guard.ps1` | 워치독 | OS가 5분 뒤 또 부른다 |
| 워치독 `auto_trade_day.ps1` | 하루 한 프로세스 | 트레이더·부속 창 | Job Object가 자식을 내리고 감시자가 되살린다 |
| 트레이더·부속 창 | 실제 일 | — | 워치독이 되살린다 |

각 층은 **바로 아래 층만** 본다. 층을 건너뛰어 트레이더를 손으로 띄우면 엔진이 둘이 된다.

### 15.3 브레이크 — 되살리기에도 한계를 둔다

- **크래시 루프**: 30초 미만 종료 3연속이면 `exit 3`. 원인이 배선(config·인증·바이너리)에 있으면 재기동으로 안 풀리고 계좌만 반복 호출한다.
- **중복 프로세스**: 트레이더가 이미 있으면 `exit 2`. 자동 정리하지 않는다.
- **마감 시각**: `-Until`을 넘기면 재기동하지 않는다.
- **낡은 바이너리**: 기동 전에 한 번 빌드. 실패하면 옛 exe가 있어도 매매하지 않는다.
- **계좌 모드 불명**: `kis.is_paper`를 못 읽으면 멈춘다.

각 브레이크는 `Save-Status "aborted" @{ error = "…" }`로 이유를 남기고 **다른 exit 코드**로 나간다. 호출자가 코드만 보고 구분한다.

### 15.4 사람·클로드·스크립트의 분담

스크립트는 **판단이 필요 없는 것**만 한다 — 띄우기, 죽으면 다시, 마감에 내리기, 사실 문서. 코드 결함 진단·수정·재빌드는 클로드(`/auto-trade-day`),
리스크 한도·계좌 전환·강제청산은 사람. 새 기능을 스크립트에 넣기 전에 "이게 판단인가 절차인가"를 먼저 묻는다.

### 15.5 실측 주석

이 스크립트들의 주석 절반은 "09-16 실측: …", "2026-09-08: 유령주문 85건"이다. 코드가 왜 이렇게 꼬였는지는 그 사고를 모르면 이해가 안 된다.
**규칙: 우회로를 넣을 때는 날짜·현상·수치를 주석에 같이 적는다.** 나중에 그 우회로를 지워도 되는지 판단하는 유일한 근거다.

### 15.6 처음부터 `-DryRun`, 처음부터 로그

부작용마다 dry 분기(2.1절), 자식 출력은 전부 로그(5.3절), 전이마다 상태 파일(15.1절). 이 셋이 없는 스크립트는 문제가 나면 원인을 못 찾는다.

---

## 16. 디버깅·검증

```powershell
powershell -ExecutionPolicy Bypass -File scripts\auto_trade_day.ps1 -DryRun          # 흐름만
powershell -ExecutionPolicy Bypass -File scripts\quant_procs.ps1                     # 지금 뭐가 떠 있나(표)
powershell -ExecutionPolicy Bypass -File scripts\eod_timetable.ps1                   # 예약작업 예정 vs 실제
Get-Content logs\auto_trade_day_20260919.log -Tail 30                                # 로그 꼬리(tail -f 금지 — 4.2절)
Get-Content _private\_auto_trade_day.json                                            # 상태
Get-ScheduledTask QuantAutoTradeGuard | Get-ScheduledTaskInfo                        # 마지막 실행·결과 코드
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Select-Object ProcessId, CommandLine
Set-PSDebug -Trace 1                                                                 # 줄마다 찍기(끝나면 -Trace 0)
```

흔한 오류와 원인:

| 증상 | 원인 |
|---|---|
| 한글이 `????` | BOM 없는 파일, 또는 1.3절 세 줄 누락 |
| `NativeCommandError`로 스크립트 중단 | `Stop` 아래서 네이티브 stderr를 `2>&1`로 직접 받음 → `Run-Native` |
| `ExitCode`가 비어 있음 | `$p.Handle`을 안 만짐 |
| 함수 안에서 바꾼 변수가 밖에서 안 바뀜 | `$script:` 누락 |
| `.Count`가 없음 / foreach가 한 번만 | 결과를 `@()`로 안 감쌈 |
| `Say("a","b")`가 이상 | 함수 호출은 공백 구분 |
| 예약작업이 안 돔 | `Get-ScheduledTaskInfo`의 `LastTaskResult`. `0x1`이면 스크립트 오류, `0x41303`은 아직 안 돎 |
| 워치독을 두 번 세거나 못 셈 | `-File` vs `-Command` 구분(1.2절) |
| `Add-Content` IOException | 다른 프로세스가 열어 둠 → FileStream 공유 모드 |

---

## 17. 연습 — 작은 감시견을 처음부터

`scripts/auto_trade_day.ps1`의 축소판을 60줄로 짜 본다. 목표: 아무 exe를 띄우고, 죽으면 다시 띄우고, 세 번 즉사면 멈추고,
마감에 끝내고, 상태 파일을 남긴다. 아래 순서대로 한 단계씩 실행해 보면서 늘린다.

```powershell
<#
.SYNOPSIS  연습용 감시견 — 프로세스 하나를 마감까지 지킨다.
.EXAMPLE   powershell -ExecutionPolicy Bypass -File scripts\practice_watchdog.ps1 -Exe notepad.exe -Until 23:59 -DryRun
#>
[CmdletBinding()]
param(
  [string]$Exe   = "notepad.exe",
  [string]$Until = "23:59",
  [switch]$DryRun
)
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 1단계: 경로·로그·상태 파일
$Repo   = Split-Path -Parent $PSScriptRoot
Set-Location $Repo
$Status = Join-Path $Repo "_private\_practice_watchdog.json"
$RunLog = Join-Path $Repo ("logs\practice_watchdog_{0}.log" -f (Get-Date -Format yyyyMMdd))
New-Item -ItemType Directory -Force -Path (Split-Path $Status), (Split-Path $RunLog) | Out-Null
$script:Sessions = @()

function Say([string]$msg, [string]$level = "INFO") {
  $line = "[{0}] {1,-5} {2}" -f (Get-Date -Format "HH:mm:ss"), $level, $msg
  Write-Host $line
  Add-Content -Path $RunLog -Value $line -Encoding utf8
}
function Save-Status([string]$phase, [hashtable]$extra) {
  $o = [ordered]@{ phase = $phase; updated = (Get-Date).ToString("s"); sessions = $script:Sessions.Count; history = $script:Sessions }
  if ($extra) { foreach ($key in $extra.Keys) { $o[$key] = $extra[$key] } }
  if (-not $DryRun) { $o | ConvertTo-Json -Depth 6 | Set-Content -Path $Status -Encoding utf8 }
}

# 2단계: 사전 점검 — 중복이면 멈춘다
$name = [IO.Path]::GetFileNameWithoutExtension($Exe)
if (Get-Process $name -ErrorAction SilentlyContinue) { Say "$name 이미 떠 있음 — 중단" "ERROR"; Save-Status "aborted" @{ error = "duplicate_process" }; exit 2 }

# 3단계: 마감 시각
$deadline = [datetime]::ParseExact((Get-Date -Format "yyyy-MM-dd") + " " + $Until, "yyyy-MM-dd HH:mm", $null)
if ((Get-Date) -ge $deadline) { Say "이미 $Until 지남" "WARN"; Save-Status "past_deadline" $null; exit 0 }

# 4단계: 감시 루프 + 크래시 루프 브레이크
$shortRuns = 0
while ((Get-Date) -lt $deadline) {
  $n = $script:Sessions.Count + 1
  Say "세션 #$n 기동 — $Exe"
  if ($DryRun) { Say "  (dry) 기동 생략"; break }
  $t0 = Get-Date
  $p = Start-Process -FilePath $Exe -PassThru
  $null = $p.Handle
  Save-Status "running" @{ pid = $p.Id; session = $n }
  while (-not $p.HasExited) {
    if ($p.WaitForExit(10000)) { break }
    Say "  살아 있음(pid=$($p.Id))"        # 5단계에서 여기에 부속 점검을 넣는다
  }
  $secs = [int]((Get-Date) - $t0).TotalSeconds
  $script:Sessions += [ordered]@{ n = $n; seconds = $secs; exit = $p.ExitCode }
  Say "세션 #$n 종료 — exit=$($p.ExitCode) ${secs}s"
  if ((Get-Date) -ge $deadline) { break }
  if ($secs -lt 30) { $shortRuns++; if ($shortRuns -ge 3) { Say "3연속 즉사 — 멈춤" "ERROR"; Save-Status "crash_loop" $null; exit 3 } }
  else { $shortRuns = 0 }
  Start-Sleep -Seconds 5
}
Save-Status "done" @{ }
Say "끝 — 세션 $($script:Sessions.Count)회"
```

다음 단계로 늘릴 것(각각 본문 절 참고): ⑤ 부속 창 `Start-Window`·`Restore-Windows`(6.1·6.4) → ⑥ `Run-Native`로 자식 출력 로그(5.3) →
⑦ Job Object(6.6) → ⑧ 감시자 스크립트와 예약작업 등록(8·15.2) → ⑨ `Write-RunLog`를 FileStream으로(4.2).

각 단계에서 **일부러 깨뜨려 본다** — 메모장을 손으로 닫아 재기동을 보고, 없는 exe를 줘서 크래시 루프를 보고, `-Until`을 1분 뒤로 줘서
마감 종료를 본다. 상태 파일과 로그가 그 셋을 구분해 적고 있으면 된 것이다.

---

## 18. 새 스크립트를 저장소에 넣을 때

1. 파일은 BOM 있는 UTF-8, 머리 주석에 `.SYNOPSIS/.DESCRIPTION/.EXAMPLE`.
2. 파이썬은 9절 머리(독스트링·reconfigure·REPO·argparse·SystemExit(main()))를 그대로 쓰고, 옆 스크립트가 이미 찾는 것은 `_logdir`·`log_patterns`를 import한다.
3. 이름에 약어를 쓰지 않는다(`$cfg`·`$i` 대신 `$config`·`$index`, `CLAUDE.md` 코드 작업 규약). 기존 스크립트의 `$p`·`$n`은 규약 이전 것이다.
4. 중괄호는 Allman이 규약이나 `.ps1`은 `py scripts/brace_style.py` 대상이 아니다 — 새로 쓰는 것은 `scripts/eod_timetable.ps1` 꼴로 맞춘다.
5. `docs/AUTOMATION.md` 표에 한 줄(스스로 도는 것이면), `docs/FILE_INDEX.md`에 한 줄.
6. 부속 프로세스를 새로 띄우면 `scripts/quant_procs.ps1`의 `$Roles`에 역할 조각을 추가한다.
7. 문구는 [docs/STYLE_GUIDE.md](../STYLE_GUIDE.md) — 로그·주석도 게이트 대상이다.

관련 문서: 운영 절차 [docs/AUTOMATION.md](../AUTOMATION.md), 훅·하네스 [docs/HARNESS.md](../HARNESS.md),
주석 규약 [docs/guides/MAINTENANCE_AUTOMATION.md](MAINTENANCE_AUTOMATION.md) 4절.
