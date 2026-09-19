# 메인 트리의 .claude/ 를 저장소 밖 거울(scripts/claude_backup.ps1 이 만든 것)에서 되돌린다.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/claude_restore.ps1 [-From <폴더>] [-Mirror]
# 기본은 %USERPROFILE%\.claude\backups\Quant\latest 에서 빠진 파일·낡은 파일만 채운다(있는 파일은 안 건드림).
# -From 으로 daily\yyyy-MM-dd 스냅샷이나 다른 사본을 고를 수 있다. -Mirror 는 거울과 완전히 같게 만든다(사본에 없는 파일 삭제).
# 세션이 스스로 돌릴 수 있게 .claude/settings.json 의 permissions.allow 에 이 스크립트 호출이 열려 있다.
param(
    [string]$From = '',
    [switch]$Mirror
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$target = Join-Path $repo '.claude'

if ($From -eq '')
{
    $From = Join-Path $env:USERPROFILE '.claude\backups\Quant\latest'
}

if (-not (Test-Path -LiteralPath (Join-Path $From 'hooks') -PathType Container))
{
    Write-Host "[claude_restore] 사본에 hooks\ 가 없다, 중단: $From"
    exit 1
}

$before = 0

if (Test-Path -LiteralPath $target -PathType Container)
{
    $before = (Get-ChildItem -LiteralPath $target -Recurse -File -Force | Measure-Object).Count
}

$options = @('/E')

if ($Mirror)
{
    $options = @('/MIR')
}

& robocopy $From $target @options /XD worktrees /XF *.state *.lock /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
$code = $LASTEXITCODE

if ($code -ge 8)
{
    Write-Host "[claude_restore] robocopy 실패 (exit $code)"
    exit 1
}

$after = (Get-ChildItem -LiteralPath $target -Recurse -File -Force | Measure-Object).Count
$hooks = (Get-ChildItem -LiteralPath (Join-Path $target 'hooks') -File | Measure-Object).Count
Write-Host "[claude_restore] $From → $target : 파일 $before → $after, hooks $hooks 개"
exit 0
