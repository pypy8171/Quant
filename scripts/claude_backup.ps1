# 메인 트리의 .claude/(훅·명령·에이전트·스킬·settings) 를 저장소 밖에 그대로 복사해 둔다.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/claude_backup.ps1 [-Quiet]
# 어디에: %USERPROFILE%\.claude\backups\Quant\latest (항상 최신 거울) + daily\yyyy-MM-dd (하루 한 번 스냅샷, 14일 보관)
# 왜: .claude/ 는 gitignore 라 git 에도 휴지통에도 없다. 워크트리 정션을 타고 통째로 지워진 일이 두 번 있었다(09-18·09-19,
#     메모리 project_worktree_junction_removal_hazard). 되돌리는 쪽은 scripts/claude_restore.ps1.
# 부르는 곳: Stop 훅 .claude/hooks/sync-gate.ps1(매 턴), scripts/maintain.py --daily, scripts/wt_remove.ps1(지우기 전).
# 안전장치: 원본에 hooks\ 가 없거나 파일이 20개 아래면 이미 지워진 상태로 보고 거울을 덮어쓰지 않는다.
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo '.claude'
$root = Join-Path $env:USERPROFILE '.claude\backups\Quant'
$latest = Join-Path $root 'latest'
$daily = Join-Path $root ('daily\' + (Get-Date -Format 'yyyy-MM-dd'))

function Say($text)
{
    if (-not $Quiet)
    {
        Write-Host $text
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $source 'hooks') -PathType Container))
{
    Say "[claude_backup] 원본에 hooks\ 가 없다 — 지워진 상태로 보고 건너뜀: $source"
    exit 2
}

$fileCount = (Get-ChildItem -LiteralPath $source -Recurse -File -Force | Measure-Object).Count

if ($fileCount -lt 20)
{
    Say "[claude_backup] 원본 파일이 $fileCount 개뿐 — 지워진 상태로 보고 건너뜀"
    exit 2
}

New-Item -ItemType Directory -Force -Path $latest | Out-Null
# /MIR 거울, worktrees\ 는 세션 임시물이라 뺀다. robocopy 는 0~7 이 성공(8 이상이 실패).
& robocopy $source $latest /MIR /XD worktrees /XF *.state *.lock /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
$code = $LASTEXITCODE

if ($code -ge 8)
{
    Say "[claude_backup] robocopy 실패 (exit $code)"
    exit 1
}

if (-not (Test-Path -LiteralPath $daily -PathType Container))
{
    & robocopy $latest $daily /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    # 14일 넘은 스냅샷은 지운다.
    Get-ChildItem -LiteralPath (Join-Path $root 'daily') -Directory |
        Where-Object { $_.Name -match '^\d{4}-\d{2}-\d{2}$' -and [datetime]::ParseExact($_.Name, 'yyyy-MM-dd', $null) -lt (Get-Date).AddDays(-14) } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
}

Say "[claude_backup] $fileCount 파일 → $latest (변경분만 복사)"
exit 0
