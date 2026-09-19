# 세션용 워크트리를 만든다 — git worktree add + .claude 정션 + gitignore 로컬 파일 복사까지 한 번에.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/wt_add.ps1 <주제> [-Branch wt/<이름>]
# 규칙:
#   - `.claude` 는 gitignore 라 워크트리에 안 따라온다. 복사하면 두 벌이 갈라지니 메인 트리로 정션을 건다.
#   - 정션이 이미 있으면 `cmd /c rmdir` 로 링크만 뗀 뒤 다시 건다. `Remove-Item -Recurse` 는 쓰지 않는다 —
#     정션을 타고 들어가 메인 `.claude/` 를 지운다(2026-09-18·09-19 두 번 발생,
#     메모리 project_worktree_junction_removal_hazard). `.claude/hooks/secret-gate.ps1` 이 그 명령을 막는다.
#   - 검사기들이 저장소 루트 상대 경로로 읽는 로컬 전용 파일을 복사한다. 없으면 commit_gate 가 내 변경과
#     무관하게 [차단] 을 낸다(메모리 project_worktree_gate_local_files).
param(
    [Parameter(Mandatory = $true)][string]$Topic,
    [string]$Branch
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# repo 루트는 한글 경로 리터럴을 피해 $PSScriptRoot 에서 유도한다.
$repo = Split-Path -Parent $PSScriptRoot
$parent = Split-Path -Parent $repo
$target = Join-Path $parent ("Quant-wt-" + $Topic)

if (-not $Branch)
{
    $Branch = "wt/" + $Topic
}

if (Test-Path -LiteralPath $target)
{
    Write-Host "[wt_add] 이미 있다: $target"
    exit 1
}

# ── 워크트리 ──────────────────────────────────────────────────────────────
$exists = & git -C $repo rev-parse --verify --quiet ("refs/heads/" + $Branch)

if ($exists)
{
    & git -C $repo worktree add $target $Branch
}
else
{
    & git -C $repo worktree add $target -b $Branch
}

if ($LASTEXITCODE -ne 0)
{
    Write-Host "[wt_add] git worktree add 실패"
    exit $LASTEXITCODE
}

# ── .claude 정션 ──────────────────────────────────────────────────────────
$link = Join-Path $target '.claude'

if (Test-Path -LiteralPath $link)
{
    $item = Get-Item -LiteralPath $link -Force

    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
    {
        & cmd /c rmdir "$link"      # 링크만 뗀다 — 가리키는 메인 .claude/ 는 그대로
    }
    else
    {
        Write-Host "[wt_add] .claude 가 정션이 아닌 실제 폴더다 — 손대지 않고 멈춘다: $link"
        exit 1
    }
}

& cmd /c mklink /J "$link" "$(Join-Path $repo '.claude')" | Out-Null

if ($LASTEXITCODE -ne 0)
{
    Write-Host "[wt_add] 정션을 못 걸었다: $link"
    exit 1
}

Write-Host "[wt_add] 정션: $link -> $(Join-Path $repo '.claude')"

# ── gitignore 로컬 전용 파일 ──────────────────────────────────────────────
$locals = @(
    '_private\gate_words.txt',
    'research\STRATEGY_LAB.md',
    'Quant\config\config_dev_paper.json',
    # 아래 셋은 docs/RUNBOOK.md 코드 블록이 가리켜서, 없으면 워크트리의 check_docs.py 가 막힌다
    'Quant\config\config_mm_paper.json',
    'Quant\config\regime.json',
    'Quant\config\universe_scan.json'
)

foreach ($relative in $locals)
{
    $source = Join-Path $repo $relative

    if (-not (Test-Path -LiteralPath $source -PathType Leaf))
    {
        Write-Host "[wt_add] 메인 트리에 없다, 건너뜀: $relative"
        continue
    }

    $destination = Join-Path $target $relative
    $directory = Split-Path -Parent $destination

    if (-not (Test-Path -LiteralPath $directory))
    {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    Copy-Item -LiteralPath $source -Destination $destination -Force
    Write-Host "[wt_add] 복사: $relative"
}

Write-Host ""
Write-Host "[wt_add] 됐다: $target (브랜치 $Branch)"
Write-Host "         지울 때는 scripts/wt_remove.ps1 을 쓴다 — 손으로 지우면 정션을 타고 메인 .claude/ 가 같이 지워진다."
