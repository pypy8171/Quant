# run_claude_task.ps1 — 예약작업이 헤드리스 클로드(`claude -p`)를 부를 때 쓰는 얇은 래퍼 (D-101 리셋 회의, 2026-09-19)
#
# 왜 있나: 예약작업 액션이 `powershell -Command "... *>> 로그"`였을 때, 클로드가 stderr에 쓴 권한 경고 한 줄을
# PowerShell 5.1이 NativeCommandError로 감싸 exit 1로 만들었다(작업은 끝났는데 rc=1, 로그는 UTF-16). 여기서는
# cmd.exe에 리다이렉션을 맡겨 stderr를 있는 그대로 UTF-8 로그에 붙이고, exit code는 클로드 것을 그대로 돌려준다.
#
# 사용: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_claude_task.ps1 -Command "/dashboard-sync" -Log "_private\_cron_dashboard.log"
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [Parameter(Mandatory = $true)][string]$Log
)

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$claude = Join-Path $env:APPDATA 'npm\claude.cmd'

if (-not (Test-Path $claude))
{
    Add-Content -Path $Log -Value ("[{0}] claude.cmd 없음: {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $claude) -Encoding UTF8
    exit 2
}

Add-Content -Path $Log -Value ("[{0}] 시작 {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Command) -Encoding UTF8

# chcp 65001 로 콘솔을 UTF-8 로 두고, 표준출력·오류를 같은 로그에 덧붙인다. 따옴표는 cmd 규칙(겉 큰따옴표 한 겹).
$inner = ('chcp 65001 >nul & "{0}" -p "{1}" --permission-mode bypassPermissions >> "{2}" 2>&1' -f $claude, $Command, $Log)
& cmd.exe /d /c $inner
$code = $LASTEXITCODE

Add-Content -Path $Log -Value ("[{0}] 끝 rc={1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $code) -Encoding UTF8
exit $code
