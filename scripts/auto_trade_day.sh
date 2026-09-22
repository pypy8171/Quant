#!/usr/bin/env bash
# 리눅스(WSL2) 쪽 하루 루프 — 트레이더를 띄우고 마감까지 죽으면 다시 띄운다.
#
# Windows 쪽 scripts/auto_trade_day.ps1 -NoTrader 가 부속 창(국면·유니버스·시세·대시보드·알림·리코더)과 마감 정리를 맡고,
# 이 스크립트는 트레이더 하나만 맡는다. 같은 계좌에 엔진은 하나라 Windows 쪽은 그날 트레이더를 띄우지 않는다
# (상태파일 trader=external, scripts/auto_trade_guard.ps1 도 그 값을 본다). 절차 정본 docs/RUNBOOK.md 1.1절.
#
# 사용 (cmd → wsl 로 들어가서, 저장소 루트에서):
#   wsl -d Ubuntu-24.04 -u root
#   cd /mnt/c/Users/<사용자>/source/repos/Quant
#   bash scripts/auto_trade_day.sh                       # 모의계좌 Quant/config/config_dev_paper.json, 15:35 까지
#   bash scripts/auto_trade_day.sh --dry-run             # 무엇을 할지만 찍는다
#   bash scripts/auto_trade_day.sh --config <json> --until 20:05 --no-build
#
# 상태는 _private/_auto_trade_linux.json (phase·pid·sessions), 실행 로그는 logs/auto_trade_linux_YYYYMMDD.log.
# 로그·토큰 캐시는 Windows 쪽과 같은 자리에 쓴다(QUANT_LOG_DIR=Quant/build_win/logs, KIS_TOKEN_CACHE_DIR=Quant/config) —
# parse_quant_log.py·check_runtime_health.py·notify_trades.py 가 평소 경로로 읽는다.
set -u

config="Quant/config/config_dev_paper.json"
until_hhmm="15:35"
dry_run=0
no_build=0
build_dir="${QUANT_LINUX_BUILD:-$HOME/quant-build}"

while [ $# -gt 0 ]; do
  case "$1" in
    --config)   config="$2"; shift 2 ;;
    --until)    until_hhmm="$2"; shift 2 ;;
    --dry-run)  dry_run=1; shift ;;
    --no-build) no_build=1; shift ;;
    *) echo "모르는 인자: $1" >&2; exit 2 ;;
  esac
done

# 저장소 루트로 — 표지 파일(_private/state)·캡처 경로가 전부 상대 경로다.
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo" || exit 2

export TZ=Asia/Seoul
export KIS_TOKEN_CACHE_DIR="$repo/Quant/config"
export QUANT_LOG_DIR="$repo/Quant/build_win/logs"

exe="$build_dir/quant_trader"
status_file="$repo/_private/_auto_trade_linux.json"
run_log="$repo/logs/auto_trade_linux_$(date +%Y%m%d).log"
mkdir -p "$repo/logs" "$repo/_private/state" "$QUANT_LOG_DIR"

say() {
  local level="${2:-INFO}"
  local line
  line="$(date '+%Y-%m-%d %H:%M:%S') [$level] $1"
  echo "$line"
  [ "$dry_run" -eq 1 ] || echo "$line" >> "$run_log"
}

sessions=()   # "HH:MM:SS→HH:MM:SS rc" 기록
save_status() {
  # Windows 쪽과 같은 모양의 단일 진실 파일 — 클로드가 로그 tail 대신 이걸 읽는다.
  local phase="$1"; shift
  [ "$dry_run" -eq 1 ] && return
  {
    echo "{"
    echo "  \"phase\": \"$phase\","
    echo "  \"platform\": \"linux\","
    echo "  \"config\": \"$config\","
    echo "  \"exe\": \"$exe\","
    echo "  \"updated\": \"$(date +%Y-%m-%dT%H:%M:%S)\","
    echo "  \"until\": \"$until_hhmm\","
    echo "  \"sessions\": ${#sessions[@]},"
    printf '  "history": ['
    local first=1 entry
    for entry in "${sessions[@]}"; do
      [ $first -eq 1 ] || printf ', '
      printf '"%s"' "$entry"; first=0
    done
    echo "],"
    while [ $# -gt 0 ]; do echo "  \"$1\": $2,"; shift 2; done
    echo "  \"run_log\": \"logs/auto_trade_linux_$(date +%Y%m%d).log\""
    echo "}"
  } > "$status_file"
}

say "리눅스 하루 루프 시작 — config=$config until=$until_hhmm exe=$exe$([ $dry_run -eq 1 ] && echo ' (dry-run)')"

# ─────────────── 기동 전 검사 ───────────────
if pgrep -x quant_trader >/dev/null; then
  say "quant_trader 가 이미 떠 있다(pid $(pgrep -x quant_trader | tr '\n' ' ')) — 이중 발주. 띄우지 않는다." ERROR
  save_status aborted error '"duplicate_process"'; exit 2
fi
# Windows 쪽 트레이더도 같은 계좌다. tasklist 는 WSL 상호운용으로 부른다(없으면 검사 생략).
if command -v tasklist.exe >/dev/null 2>&1 && tasklist.exe /FI "IMAGENAME eq quant_trader.exe" 2>/dev/null | grep -q quant_trader.exe; then
  say "Windows 쪽 quant_trader.exe 가 떠 있다 — 같은 계좌에 엔진 둘. 띄우지 않는다." ERROR
  save_status aborted error '"duplicate_process_windows"'; exit 2
fi
if [ ! -f "$config" ]; then say "config 없음: $config" ERROR; save_status aborted error '"no_config"'; exit 2; fi

# 모의/실계좌 확인 — 실계좌면 그냥 띄우지 않는다(계좌 전환은 사용자 승인 사항).
paper="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1], encoding='utf-8'))['kis']['is_paper'])" "$config" 2>/dev/null)"
if [ "$paper" != "True" ]; then
  say "설정 $config 의 kis.is_paper=$paper — 모의계좌가 아니면 이 루프로 띄우지 않는다." ERROR
  save_status aborted error '"not_paper"'; exit 2
fi
say "계좌: 모의(is_paper=true)"

# ─────────────── 재빌드 — 소스가 실행파일보다 새면 ───────────────
newest_source="$(find Quant/src Quant/include -type f \( -name '*.cpp' -o -name '*.h' \) -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1)"
if [ $no_build -eq 1 ]; then
  if [ -f "$exe" ] && [ "${newest_source%% *}" != "" ] && [ "$(stat -c %Y "$exe")" -lt "${newest_source%%.*}" ]; then
    say "--no-build 인데 소스(${newest_source#* })가 실행파일보다 새것 — 옛 바이너리로 매매하지 않는다." ERROR
    save_status aborted error '"stale_exe"'; exit 2
  fi
  say "--no-build — 재빌드 생략"
elif [ ! -f "$build_dir/build.ninja" ]; then
  say "빌드 폴더 없음: $build_dir — cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S Quant -B $build_dir 먼저 (docs/RUNBOOK.md 1.1절)" ERROR
  save_status aborted error '"no_build_dir"'; exit 2
else
  say "재빌드 — ninja -C $build_dir quant_trader (HEAD $(git rev-parse --short HEAD 2>/dev/null))"
  if [ $dry_run -eq 1 ]; then
    say "  (dry) ninja -C $build_dir quant_trader"
  else
    t0=$(date +%s)
    if ! build_out="$(ninja -C "$build_dir" quant_trader 2>&1)"; then
      first_error="$(echo "$build_out" | grep -m1 -E 'error|오류')"
      say "재빌드 실패 — $first_error" ERROR
      save_status aborted error '"build_failed"' detail "\"$(echo "$first_error" | tr '"' "'")\""; exit 2
    fi
    say "재빌드 끝 — $(( $(date +%s) - t0 ))초"
  fi
fi
if [ $dry_run -eq 0 ] && [ ! -x "$exe" ]; then say "실행파일 없음: $exe" ERROR; save_status aborted error '"no_exe"'; exit 2; fi

# ─────────────── 감시 루프 ───────────────
today="$(date +%Y-%m-%d)"
deadline=$(date -d "$today $until_hhmm" +%s)
if [ "$(date +%s)" -ge "$deadline" ]; then
  say "이미 $until_hhmm 을 지났다. 매매하지 않고 종료." WARN; save_status past_deadline; exit 0
fi

crash_window=1800   # 30분 안에
crash_max_exits=3   # 세 번 내려가면 배선 문제 — 재기동으로 풀리지 않는다
exit_times=()
save_status starting

while [ "$(date +%s)" -lt "$deadline" ]; do
  session_number=$(( ${#sessions[@]} + 1 ))
  # 이전 세션이 남긴 미체결을 로그에서 복원해 보조 파일에 채운다 — 엔진이 기동하며 전부 취소한다(09-08 미체결 85건).
  say "세션 #$session_number 기동 — $exe $config TRADE"
  if [ $dry_run -eq 1 ]; then
    say "  (dry) python3 scripts/seed_open_orders.py"
    say "  (dry) $exe $config TRADE"
    save_status dry_run; exit 0
  fi
  python3 scripts/seed_open_orders.py 2>&1 | sed 's/^/  /' | tee -a "$run_log"

  started="$(date +%H:%M:%S)"
  "$exe" "$config" TRADE &
  trader_pid=$!
  save_status running pid "$trader_pid" session "$session_number"
  wait "$trader_pid"; rc=$?
  ended="$(date +%H:%M:%S)"
  sessions+=("$started→$ended rc=$rc")
  say "세션 #$session_number 종료 — rc=$rc ($started→$ended)"
  python3 scripts/check_runtime_health.py --since "${started%:*}" 2>&1 | sed 's/^/  /' | tee -a "$run_log"

  if [ "$(date +%s)" -ge "$deadline" ]; then say "마감 시각 도달 — 재기동하지 않는다."; break; fi
  # 엔진이 스스로 내려간 날은 재기동하지 않는다 — 표지 파일로 본다(D-098). KILL은 kill_release.ps1 로 푼다.
  if [ -f "_private/state/session_done_$today" ]; then say "엔진이 마감 자기 종료 — 재기동하지 않는다."; break; fi
  if [ -f "_private/state/kill_today_$today" ]; then say "운영자 KILL — 오늘은 재기동하지 않는다(풀려면 scripts/kill_release.ps1)." WARN; break; fi

  now=$(date +%s)
  kept=()
  for t in "${exit_times[@]}"; do [ $(( now - t )) -lt $crash_window ] && kept+=("$t"); done
  exit_times=("${kept[@]}" "$now")
  if [ ${#exit_times[@]} -ge $crash_max_exits ]; then
    say "최근 30분 안 종료 ${#exit_times[@]}회 — 크래시 루프로 보고 멈춘다. 로그를 보고 고쳐야 한다." ERROR
    save_status crash_loop error '"crash_loop"' last_exit "$rc"; exit 3
  fi
  say "5초 뒤 재기동."; sleep 5
done

save_status closed
# 마감 정리(일지 사실 구간·리뷰·대시보드·건전성 점검)는 Windows 쪽 auto_trade_day.ps1 -NoTrader 창이 한다.
say "리눅스 하루 루프 종료 — 세션 ${#sessions[@]}회, 로그 $run_log. 마감 정리는 Windows 창(-NoTrader)이 맡는다."
save_status done
