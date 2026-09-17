# 무인 이어달리기 — 긴 작업을 사람 없이, 문맥을 키우지 않고 끝까지 돌린다.
# 대화형 세션은 /clear 를 칠 사람이 없어 자동 압축에 기대야 하고, 압축은 요약이 남아 문맥이 덜 빈다.
# 헤드리스는 사이클마다 프로세스가 새로 뜨므로 문맥이 진짜로 0에서 시작하고, 이어지는 것은 인계 파일 하나뿐이다.
# 그것이 이 스크립트의 전부다 — 긴 한 판을 짧은 여러 판으로 쪼개고, 사이를 인계 파일로 잇는다.
#
#   powershell -ExecutionPolicy Bypass -File scripts/unattended_run.ps1 -Name night-a -PromptFile _private/night_a.md -Until "2026-09-18T09:00:00"
#
# 여럿 동시에: -Name 을 다르게 준다. 인계·로그·완료 신호가 전부 이름별이라 서로 섞이지 않는다.
#   [주의] 같은 파일을 고치는 작업을 둘 이상에게 주면 서로 덮어쓴다. 작업을 파일 단위로 갈라 주거나
#          세션마다 git worktree 를 쓴다(docs/guides/MULTI_SESSION.md).
#   [주의] 무인 표시 _private/UNATTENDED.flag 는 여럿이 함께 쓰므로 러너가 지우지 않는다. -Until 시각이 지나면
#          저절로 풀린다. 먼저 끝난 러너가 지우면 아직 도는 나머지가 무인 모드에서 빠진다.
#
# 절차 정본 docs/AUTOMATION.md. 경로에 한글이 있어 이 파일은 BOM 있는 UTF-8이어야 한다(PS 5.1).

param(
    [Parameter(Mandatory = $true)][string]$Name,   # 인계 파일 이름이 된다 — _private/HANDOFF_<Name>.md
    [string]$PromptFile,                            # 첫 사이클에 줄 작업 지시서(마크다운)
    [string]$Prompt,                                # 지시서 대신 한 줄로 줄 때
    [string]$Until,                                 # 무인 모드가 풀릴 시각(ISO). 보통 기상 시각
    [int]$MaxCycles  = 12,                          # 이만큼 돌면 멈춘다. 무한히 도는 것을 막는다
    [int]$MaxMinutes = 420                          # 7시간. 아침에 깨어 보면 끝나 있어야 한다
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
# PS 5.1은 네이티브 exe로 파이프할 때 $OutputEncoding 기본값이 ASCII라 한글 지시가 물음표로 깨진다.
# 2026-09-17 첫 시험에서 실제로 깨졌다 — 지시서가 한글이면 이 줄이 없으면 아무 일도 제대로 안 된다.
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
Set-Location (Split-Path $PSScriptRoot -Parent)

$handoff = "_private/HANDOFF_$Name.md"
$done    = "_private/DONE_$Name.flag"
$flag    = "_private/UNATTENDED.flag"
$log     = "_private/_unattended_$Name.log"

function Write-Log([string]$msg) {
    $line = "$(Get-Date -Format 'MM-dd HH:mm:ss') [$Name] $msg"
    Write-Output $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}

if (-not $Prompt -and -not $PromptFile) { throw "-Prompt 나 -PromptFile 중 하나는 있어야 한다" }
if ($PromptFile -and -not (Test-Path $PromptFile)) { throw "지시서가 없다: $PromptFile" }
if (Test-Path $done) { Remove-Item $done -Force }
if (-not $Until) { $Until = (Get-Date).AddMinutes($MaxMinutes).ToString('yyyy-MM-ddTHH:mm:ss') }

# 무인 표시. 이미 있고 더 늦은 시각이면 그대로 둔다(다른 러너가 더 오래 돈다는 뜻이다).
$writeFlag = $true
if (Test-Path $flag) {
    try {
        $cur = (Get-Content $flag -TotalCount 1 -Encoding UTF8).Trim()
        if ($cur -and [datetime]$cur -gt [datetime]$Until) { $writeFlag = $false }
    } catch {}
}
if ($writeFlag) { Set-Content -Path $flag -Value $Until -Encoding UTF8 }

# 사이클마다 붙는 규약. 사람이 없으니 되묻지 않고, 다음 사이클이 읽을 것은 인계 파일뿐이다.
$rule = @"

--- 무인 실행 규약 ---
사용자는 자고 있다. 승인을 묻지 말고 판단이 서면 실행한다. 커밋과 푸시는 하지 않는다.
진행 상황은 $handoff 에만 남긴다 — 한 항목을 끝낼 때마다 '한 것'과 '남은 것'을 갱신한다.
다음 사이클은 새 프로세스라 지금 문맥이 하나도 남지 않는다. '남은 것'은 파일 경로와 다음 명령까지 적어,
그것만 읽고도 그대로 이어갈 수 있게 쓴다.
막히면 인계 파일 '주의'에 한 줄로 적고 다음 항목으로 넘어간다.
할 일을 전부 끝냈을 때만 $done 파일을 만든다(빈 파일이면 된다). 그 전에는 만들지 않는다.
"@

$first = if ($PromptFile) { Get-Content $PromptFile -Raw -Encoding UTF8 } else { $Prompt }
$next  = "이전 사이클에서 하던 일을 잇는다. $handoff 를 먼저 읽고 '남은 것'의 첫 항목부터 진행한다. 전체 재탐색은 하지 않는다."

Write-Log "시작 — 최대 $MaxCycles 사이클 / $MaxMinutes 분 / 무인 해제 $Until"
$deadline = (Get-Date).AddMinutes($MaxMinutes)
$cycle = 0

try {
    while ($cycle -lt $MaxCycles) {
        if ((Get-Date) -gt $deadline) { Write-Log '정지 — 시간 한도'; break }
        $cycle++
        $text = if ($cycle -eq 1) { "$first`n$rule" } else { "$next`n$rule" }

        Write-Log "사이클 $cycle 시작"
        $text | & claude -p --permission-mode bypassPermissions 2>&1 |
            ForEach-Object { Add-Content -Path $log -Value $_ -Encoding UTF8 }
        $rc = $LASTEXITCODE
        Write-Log "사이클 $cycle 끝 rc=$rc"

        if (Test-Path $done) { Write-Log '완료 — 모델이 끝냈다고 표시했다'; break }
        if (-not (Test-Path $handoff)) { Write-Log "경고 — $handoff 가 없다. 다음 사이클이 이어받을 것이 없다" }
        if ($rc -ne 0) {
            # 사용량 한도나 네트워크면 조금 쉬었다 다시 온다. 인계 파일이 있으니 잃는 것은 없다.
            Write-Log "경고 — rc=$rc. 10분 쉬고 다시 붙는다"
            Start-Sleep -Seconds 600
        }
    }
    if ($cycle -ge $MaxCycles -and -not (Test-Path $done)) { Write-Log '정지 — 사이클 한도' }
} finally {
    # flag 는 지우지 않는다. 같이 도는 다른 러너가 아직 있을 수 있다($Until 시각이 지나면 저절로 풀린다).
    Write-Log "끝 — $cycle 사이클. 인계 $handoff / 로그 $log"
}
