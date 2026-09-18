# 운영단말(ops_terminal.exe) 바탕화면 바로가기를 만든다 — 사용자가 더블클릭으로 띄우는 길.
# CMake가 ops_terminal 링크 뒤(POST_BUILD)에 부르고, 손으로도 부를 수 있다.
#   py 아님 — powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ops_terminal_shortcut.ps1 <exe 전체 경로>
# 규칙:
#   - 메인 트리 빌드만 바로가기를 가리키게 한다. worktree(../Quant-wt-*) 빌드는 지워질 수 있어 건너뛴다.
#   - 토큰은 사용자 환경변수 QUANT_OPS_TOKEN(단말이 읽는다). 비어 있으면 Quant/config/config_dev_paper.json의 ops_token으로 채운다.
#   - 실패해도 빌드는 깨지 않는다(exit 0). 결과는 한 줄로 알린다.
param([Parameter(Mandatory = $true)][string]$Exe)

$ErrorActionPreference = 'Stop'
try {
    $repo = Split-Path -Parent $PSScriptRoot
    Set-Location $repo
    # 링크된 worktree는 git-dir(.git/worktrees/<이름>)과 common-dir(.git)가 다르다.
    $gitDir    = (git rev-parse --path-format=absolute --git-dir 2>$null)
    $commonDir = (git rev-parse --path-format=absolute --git-common-dir 2>$null)
    if ($gitDir -and $commonDir -and ($gitDir -ne $commonDir)) {
        Write-Host "[ops_terminal_shortcut] worktree 빌드 — 바탕화면 바로가기는 메인 트리 빌드만 갱신한다."
        exit 0
    }

    if (-not (Test-Path $Exe)) { Write-Host "[ops_terminal_shortcut] exe 없음: $Exe"; exit 0 }
    $exeFull = (Resolve-Path $Exe).Path

    if (-not [Environment]::GetEnvironmentVariable('QUANT_OPS_TOKEN', 'User')) {
        $configPath = Join-Path $repo 'Quant\config\config_dev_paper.json'
        if (Test-Path $configPath) {
            $token = (Get-Content $configPath -Raw | ConvertFrom-Json).ops_token
            if ($token) {
                [Environment]::SetEnvironmentVariable('QUANT_OPS_TOKEN', $token, 'User')
                Write-Host "[ops_terminal_shortcut] 사용자 환경변수 QUANT_OPS_TOKEN 설정(길이 $($token.Length))."
            }
        }
    }

    $desktop  = [Environment]::GetFolderPath('Desktop')
    $linkPath = Join-Path $desktop '운영단말.lnk'
    $shell    = New-Object -ComObject WScript.Shell
    $link     = $shell.CreateShortcut($linkPath)
    $link.TargetPath       = $exeFull
    $link.WorkingDirectory = Split-Path -Parent $exeFull
    $link.Description      = 'quant_trader 운영단말 (토큰은 사용자 환경변수 QUANT_OPS_TOKEN)'
    $link.Save()
    Write-Host "[ops_terminal_shortcut] $linkPath -> $exeFull"
}
catch {
    Write-Host "[ops_terminal_shortcut] 건너뜀 — $($_.Exception.Message)"
}
exit 0
