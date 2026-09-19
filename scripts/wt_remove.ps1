# 워크트리를 지운다 — 안의 정션(.claude 등)을 먼저 떼고 나서 git worktree remove 를 부른다.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/wt_remove.ps1 ../Quant-wt-<주제> [-Force]
# 규칙:
#   - `git worktree remove --force`는 트리를 재귀 삭제하며 Windows 정션을 타고 들어간다. 워크트리의 `.claude`가 메인
#     `Quant/.claude`로의 정션이면 메인 쪽 hooks·commands·agents·skills·settings.json이 통째로 지워진다(2026-09-18 실제 발생,
#     메모리 project_worktree_junction_removal_hazard). 그래서 워크트리 최상위의 재분석점(정션·심볼릭 링크)은 `rmdir`로
#     링크만 떼고 나서 git에 넘긴다.
#   - 메인 트리 경로를 받으면 아무것도 하지 않는다.
#   - 미커밋 변경이 있으면 git이 거부한다. 버려도 되는 것이 확실할 때만 -Force 를 준다.
param(
    [Parameter(Mandatory = $true)][string]$Worktree,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$target = [IO.Path]::GetFullPath((Join-Path (Get-Location) $Worktree))

if (-not (Test-Path -LiteralPath $target -PathType Container))
{
    Write-Host "[wt_remove] 폴더가 없다: $target"
    exit 1
}

if ($target.TrimEnd('\') -ieq $repo.TrimEnd('\'))
{
    Write-Host "[wt_remove] 메인 트리는 지우지 않는다: $target"
    exit 1
}

# 워크트리인지는 `.git` 이 "gitdir: …" 한 줄짜리 파일인지로 본다(경로 문자열 비교는 한글·슬래시 차이로 어긋난다).
$gitMarker = Join-Path $target '.git'

if (-not (Test-Path -LiteralPath $gitMarker -PathType Leaf) -or -not ((Get-Content -LiteralPath $gitMarker -TotalCount 1) -like 'gitdir:*'))
{
    Write-Host "[wt_remove] git worktree 가 아니다(.git 파일 없음): $target"
    exit 1
}

# 최상위 재분석점(정션·심볼릭 링크)만 뗀다 — rmdir 은 링크 자체만 지우고 가리키는 곳은 건드리지 않는다.
$links = Get-ChildItem -LiteralPath $target -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }

foreach ($link in $links)
{
    & cmd /c rmdir "$($link.FullName)"

    if ($LASTEXITCODE -ne 0)
    {
        Write-Host "[wt_remove] 링크를 못 뗐다, 중단: $($link.FullName)"
        exit 1
    }

    Write-Host "[wt_remove] 링크 뗌: $($link.Name)"
}

# 지우기 전에 메인 .claude/ 를 저장소 밖에 거울로 떠 둔다 — 아래에서 무엇이 잘못돼도 되돌릴 수 있게.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'claude_backup.ps1') -Quiet

$arguments = @('-C', $repo, 'worktree', 'remove')

if ($Force)
{
    $arguments += '--force'
}

$arguments += $target
& git @arguments

if ($LASTEXITCODE -ne 0)
{
    Write-Host "[wt_remove] git worktree remove 실패(미커밋 변경이면 정리하거나 -Force)"
    exit $LASTEXITCODE
}

Write-Host "[wt_remove] 지움: $target"

# 메인 .claude/ 가 정션을 타고 같이 지워졌으면 거울에서 바로 되돌린다.
if (-not (Test-Path -LiteralPath (Join-Path $repo '.claude\hooks') -PathType Container))
{
    Write-Host "[wt_remove] 메인 .claude\hooks 가 사라졌다 — 거울에서 복원"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'claude_restore.ps1')
}
