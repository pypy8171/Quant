#!/usr/bin/env bash
# ThreadSanitizer 회차 — Debug+TSAN 으로 짓고 단위 테스트를 한 판 돌려 스레드 경합을 찾는다.
#
# 왜 따로 도는가: -DQUANT_TSAN=ON 은 Quant/CMakeLists.txt:15 에 있는데 부르는 자리가 없었다.
# 큐·표 교체(D-116)·샤드 같은 동시성 변경은 경합을 넣어도 평소 Release 테스트를 그냥 통과한다 —
# 다음 장중에 값이 어긋나서야 안다. 커밋 시점에는 못 돈다(윈도우 MSVC 에 TSAN 이 없고, 계측하면
# 5~15배 느리다). 그래서 스레드가 여럿 붙는 코드를 고친 워크트리가 main 에 머지하기 전에 한 판
# 부른다 — docs/guides/MULTI_SESSION.md 머지 절차(D-116 후속).
#
# 사용 (WSL2 **Ubuntu-24.04**, 저장소 루트에서):
#   bash scripts/tsan_round.sh                   # 증분 — 빌드 폴더가 있으면 바뀐 것만 다시 짓는다
#   bash scripts/tsan_round.sh --clean           # 빌드 폴더를 지우고 처음부터
#   bash scripts/tsan_round.sh --jobs 4          # 동시 컴파일 수(기본 nproc)
#
# 배포판을 골라야 한다 — 기본(Ubuntu-22.04)은 g++ 11 이라 C++23 <format>·<expected> 가 없어 못 짓는다.
# 윈도우에서 부를 때: wsl.exe -d Ubuntu-24.04 -e bash -c "cd '/mnt/c/.../Quant' && bash scripts/tsan_round.sh"
# 잘못 고르면 아래 컴파일러 검사가 먼저 막는다.
#
# 빌드 폴더는 저장소 트리마다 따로 쓴다 — $HOME/quant-build-tsan-<트리 폴더 이름>.
# QUANT_TSAN_BUILD 로 덮어쓸 수 있다.
#
# 남기는 것:
#   _private/state/tsan_last.json                   마지막 회차 한 줄 요약 — scripts/check_runtime_health.py
#                                                   "TSAN 회차" 행이 이것을 읽는다. 사람이 열어 볼 파일이 아니다
#   logs/tsan/tsan_YYYYMMDD_HHMMSS.log              그 회차 전체 출력(경합 보고 원문)
#
# 종료코드: 0 = 경합·실패 없음, 1 = 경합 또는 테스트 실패, 2 = 빌드·설정이 안 됨
set -u

clean=0
jobs=""

while [ $# -gt 0 ]; do
  case "$1" in
    --clean) clean=1; shift ;;
    --jobs)  jobs="$2"; shift 2 ;;
    *) echo "모르는 인자: $1" >&2; exit 2 ;;
  esac
done

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo" || exit 2

# 빌드 폴더는 트리마다 갈라 쓴다. 폴더가 하나뿐이면 워크트리에서 불러도 그 폴더에 적힌 원본 폴더(메인 트리)를
# 그대로 다시 지어 놓고 "41/41 통과"를 적는다 — 워크트리 코드는 한 줄도 시험하지 않은 채로다
# (2026-09-22 에 실제로 그렇게 거짓 통과했다). 폴더 이름에 트리 폴더 이름을 붙여 서로 안 겹치게 한다.
build_dir="${QUANT_TSAN_BUILD:-$HOME/quant-build-tsan-$(basename "$repo")}"

export TZ=Asia/Seoul
started="$(date '+%Y-%m-%dT%H:%M:%S')"
started_epoch="$(date +%s)"
stamp="$(date '+%Y%m%d_%H%M%S')"
run_log="$repo/logs/tsan/tsan_$stamp.log"
state_file="$repo/_private/state/tsan_last.json"
mkdir -p "$repo/logs/tsan" "$repo/_private/state"

[ -n "$jobs" ] || jobs="$(nproc 2>/dev/null || echo 4)"

# 컴파일러가 C++23 <format>·<expected> 를 갖췄는가. 배포판을 잘못 고르면 272개를 짓다가 첫 파일에서 죽는데,
# 메시지가 "format: No such file or directory" 라 코드가 깨진 것처럼 보인다(2026-09-23 에 세 회차를 그렇게 버렸다).
compiler_ok() {
  local cxx
  cxx="${CXX:-c++}"
  "$cxx" -std=gnu++23 -fsyntax-only -x c++ - 2>/dev/null <<'CHECK_EOF'
#include <format>
#include <expected>
int main() { return 0; }
CHECK_EOF
}

# 커밋 해시. 이게 unknown 이면 판정기가 "그 회차 뒤로 동시성 코드가 몇 번 바뀌었나"를 못 센다.
# 걸리는 것 둘: /mnt/c 아래 저장소는 소유자가 달라 git 이 거절하고(dubious ownership),
# 워크트리는 .git 이 "gitdir: C:/..." 라는 윈도우 절대 경로라 WSL 에서 그대로는 못 따라간다.
resolve_commit() {
  local short gitdir drive
  short="$(git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null)"
  if [ -n "$short" ]; then
    echo "$short"
    return
  fi

  [ -f "$repo/.git" ] || return
  gitdir="$(sed -n 's/^gitdir: //p' "$repo/.git" | tr -d '\r')"

  # git 은 윈도우에서도 이 파일에 슬래시로 적는다 — C:/... 를 /mnt/c/... 로만 바꾸면 된다.
  case "$gitdir" in
    [A-Za-z]:/*)
      drive="$(echo "${gitdir%%:*}" | tr 'A-Z' 'a-z')"
      gitdir="/mnt/$drive${gitdir#*:}"
      ;;
  esac

  [ -d "$gitdir" ] && git -c safe.directory='*' --git-dir="$gitdir" rev-parse --short HEAD 2>/dev/null
}

commit="$(resolve_commit)"
[ -n "$commit" ] || commit="unknown"

say() {
  local line
  line="$(date '+%Y-%m-%d %H:%M:%S') $1"
  echo "$line"
  echo "$line" >> "$run_log"
}

# 결과 한 줄을 적고 끝낸다. 판정기가 읽는 파일이라 회차가 어떻게 끝나든 반드시 쓴다 —
# 파일이 갱신되지 않으면 판정기는 "회차가 밀렸다"로 본다(실패와 밀림을 구분하려면 이쪽이 남아야 한다).
write_state() {
  local stage="$1" tests_total="$2" tests_failed="$3" races="$4" failed_names="$5"
  local finished elapsed
  finished="$(date '+%Y-%m-%dT%H:%M:%S')"
  elapsed=$(( $(date +%s) - started_epoch ))
  cat > "$state_file" <<JSON
{
  "schema_version": 1,
  "started": "$started",
  "finished": "$finished",
  "elapsed_sec": $elapsed,
  "commit": "$commit",
  "stage": "$stage",
  "tests_total": $tests_total,
  "tests_failed": $tests_failed,
  "races": $races,
  "failed_names": [$failed_names],
  "log": "logs/tsan/tsan_$stamp.log"
}
JSON
}

say "[TSAN] 회차 시작 — HEAD $commit, 빌드 폴더 $build_dir, 동시 $jobs"

if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
  say "[TSAN] cmake 또는 ninja 없음 — apt install cmake ninja-build"
  write_state "no_toolchain" 0 0 0 ""
  exit 2
fi

if ! compiler_ok; then
  say "[TSAN] $(${CXX:-c++} --version 2>/dev/null | head -1) 에 C++23 <format>·<expected> 가 없다 — 배포판을 잘못 골랐다"
  say "[TSAN] wsl.exe -d Ubuntu-24.04 -e bash -c \"cd '$repo' && bash scripts/tsan_round.sh\""
  write_state "no_toolchain" 0 0 0 ""
  exit 2
fi

if [ "$clean" -eq 1 ] && [ -d "$build_dir" ]; then
  say "[TSAN] 빌드 폴더 비움"
  rm -rf "$build_dir"
fi

# QUANT_TSAN_BUILD 로 폴더를 직접 준 경우까지 막는다 — 그 폴더에 다른 트리 설정이 남아 있으면 비운다.
# CMake 는 한 번 정한 원본 폴더를 바꾸지 못해(재설정해도 "does not match the source directory"),
# 그냥 두면 엉뚱한 트리를 지어 놓고 통과를 적는다.
cache_home=""

if [ -f "$build_dir/CMakeCache.txt" ]; then
  cache_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build_dir/CMakeCache.txt" | tr -d '\r')"
fi

if [ -n "$cache_home" ] && [ "$cache_home" != "$repo/Quant" ]; then
  say "[TSAN] 빌드 폴더가 다른 트리를 가리킨다($cache_home) — 비우고 다시 설정한다"
  rm -rf "$build_dir"
fi

# 설정은 빌드 폴더가 없을 때만. 한 번 TSAN 으로 만든 폴더는 그 설정을 유지한다 —
# Release 폴더에 섞어 쓰면 계측 없이 런타임만 링크돼 "TSAN 통과"로 오인한다(CMakeLists.txt:20 주석).
if [ ! -f "$build_dir/build.ninja" ]; then
  say "[TSAN] 설정 — cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug -DQUANT_TSAN=ON"
  if ! cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug -DQUANT_TSAN=ON -S Quant -B "$build_dir" >> "$run_log" 2>&1; then
    say "[TSAN] 설정 실패 — 끝을 본다: $run_log"
    write_state "configure_failed" 0 0 0 ""
    exit 2
  fi
fi

say "[TSAN] 빌드 — ninja -C $build_dir -j $jobs"
if ! ninja -C "$build_dir" -j "$jobs" >> "$run_log" 2>&1; then
  say "[TSAN] 빌드 실패 — 끝을 본다: $run_log"
  write_state "build_failed" 0 0 0 ""
  exit 2
fi

# 벤치는 뺀다 — 처리량을 재는 것이라 TSAN(5~15배 느림) 아래서는 숫자에 뜻이 없고,
# TIMEOUT 60 에 걸려 경합과 무관한 실패만 만든다(Quant/CMakeLists.txt 225·234·409).
suppressions="$repo/Quant/tests/tsan.supp"
tsan_options="halt_on_error=0 second_deadlock_stack=1 history_size=4"
if [ -f "$suppressions" ]; then
  tsan_options="$tsan_options suppressions=$suppressions"
fi
export TSAN_OPTIONS="$tsan_options"

# 주소 무작위화를 끄고 부른다. 요즘 커널은 실행 파일을 TSAN 이 예상하지 않는 자리에 올리고,
# 그러면 테스트가 첫 줄에서 "FATAL: ThreadSanitizer: unexpected memory mapping" 으로 죽는다
# (2026-09-22 첫 회차에 41개 중 33개가 그렇게 떨어졌다 — 경합이 아니라 못 뜬 것이다).
# personality 는 자식이 물려받으므로 ctest 한 번만 감싸면 테스트 전부에 걸린다.
aslr_off=""
if command -v setarch >/dev/null 2>&1 && setarch "$(uname -m)" -R true >/dev/null 2>&1; then
  aslr_off="setarch $(uname -m) -R"
else
  say "[TSAN] setarch 없음 — 주소 무작위화를 못 끈다. unexpected memory mapping 이 나면 sysctl vm.mmap_rnd_bits=28"
fi

say "[TSAN] 테스트 — ctest -E ^bench_ (벤치 제외)"
ctest_output="$repo/logs/tsan/ctest_$stamp.txt"
$aslr_off ctest --test-dir "$build_dir" --output-on-failure -E '^bench_' > "$ctest_output" 2>&1
ctest_rc=$?
cat "$ctest_output" >> "$run_log"
rm -f "$ctest_output"

# "97% tests passed, 1 tests failed out of 44"
summary="$(grep -E '^[0-9]+% tests passed' "$run_log" | tail -1)"
tests_failed="$(echo "$summary" | sed -n 's/.*, \([0-9]\+\) tests\? failed out of \([0-9]\+\).*/\1/p')"
tests_total="$(echo "$summary" | sed -n 's/.*, \([0-9]\+\) tests\? failed out of \([0-9]\+\).*/\2/p')"
[ -n "$tests_failed" ] || tests_failed=0
[ -n "$tests_total" ] || tests_total=0

# 떨어진 테스트 이름 — ctest 의 "The following tests FAILED:" 아래 "\t 12 - 이름 (Failed)" 줄
failed_names="$(sed -n '/The following tests FAILED:/,$p' "$run_log" \
  | sed -n 's/^[[:space:]]*[0-9]\+ - \([A-Za-z0-9_]\+\) .*/"\1"/p' | sort -u | paste -sd, -)"

# 경합·교착 보고 수. TSAN 은 테스트를 통과시키면서도 보고를 찍을 수 있어(halt_on_error=0) 따로 센다.
# grep -c 는 0건일 때도 0 을 찍고 1로 끝난다 — || echo 0 을 붙이면 0 이 두 줄 나와
# 상태 파일 JSON 이 깨진다. 끝 상태만 보지 말고 출력이 비었는지로 판단한다.
races="$(grep -c 'WARNING: ThreadSanitizer' "$run_log" 2>/dev/null)"
[ -n "$races" ] || races=0

write_state "done" "$tests_total" "$tests_failed" "$races" "$failed_names"

say "[TSAN] 끝 — 테스트 $((tests_total - tests_failed))/$tests_total, 경합 보고 $races건 (ctest rc=$ctest_rc)"
say "[TSAN] 전체 출력 $run_log"

if [ "$races" -gt 0 ] || [ "$tests_failed" -gt 0 ]; then
  say "[TSAN] 경합 보고 또는 실패가 있다 — 위 로그에서 'WARNING: ThreadSanitizer' 를 찾는다"
  exit 1
fi

exit 0
