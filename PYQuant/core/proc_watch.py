"""
엔진 프로세스 자원 표본 수집 — ZMQ HEALTH에는 처리건수만 있고 자원 사용량이 없어 따로 떠서
TimescaleDB에 쌓는다(proc_stats·proc_thread_stats·proc_hotspots).

- Windows 네이티브 엔진(quant_trader.exe): psutil로 프로세스 CPU/메모리만.
- 리눅스 엔진(quant_trader, WSL 포함): /proc를 읽어 프로세스 합계 + 스레드별 CPU까지. Windows에서 WSL 안의
  엔진을 볼 때는 `wsl -d <배포판>`으로 같은 셸 조각을 돌린다(psutil은 WSL 프로세스를 못 본다).
  perf가 있으면 일정 주기로 몇 초 표본을 떠서 함수별 자기 시간 비율(proc_hotspots)도 적재한다.
"""
import platform
import re
import subprocess
import time

from core.logger import attach_file_handler, setup_logger

logger = setup_logger("quant.procwatch")

try:
    import psutil
    _PSUTIL_AVAILABLE = True
except ImportError:
    _PSUTIL_AVAILABLE = False


# ── 리눅스(/proc) 표본 ────────────────────────────────────────────────────────
# /proc/<pid>/stat은 ')' 뒤를 공백으로 나눠 f[1]=state 기준 — f[12]=utime f[13]=stime f[18]=스레드 수 f[22]=rss(페이지).
# 스레드 이름(comm)은 공백을 품을 수 있어("Shard 0") '|'로 구분해 따로 찍는다.
_PROC_SNAPSHOT_SCRIPT = r'''
pid=$(pgrep -x "$1" | head -1); [ -z "$pid" ] && exit 0
awk -v pid="$pid" -v cores="$(nproc)" -v clk="$(getconf CLK_TCK)" -v page="$(getconf PAGESIZE)" \
  '{i=index($0,")"); split(substr($0,i+2),f," "); print "P|" pid "|" cores "|" clk "|" f[12]+f[13] "|" f[22]*page "|" f[18]}' /proc/$pid/stat
for t in /proc/$pid/task/*; do
  c=$(cat "$t/comm" 2>/dev/null) || continue
  awk -v thread_id="$(basename "$t")" -v c="$c" '{i=index($0,")"); split(substr($0,i+2),f," "); print "T|" thread_id "|" c "|" f[12]+f[13]}' "$t/stat" 2>/dev/null
done
'''

# perf 표본: cpu-clock(소프트웨어 이벤트)이라 WSL2에서도 돈다. 499Hz × 몇 초면 엔진 부하는 1% 안쪽이다.
_PERF_SCRIPT = r'''
pid=$(pgrep -x "$1" | head -1); [ -z "$pid" ] && exit 0
# /usr/bin/perf 래퍼는 커널 버전이 다르면(WSL2) 거부하므로 linux-tools의 실제 바이너리를 먼저 찾는다
perf_bin=$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | tail -1); [ -z "$perf_bin" ] && perf_bin=$(command -v perf); [ -z "$perf_bin" ] && { echo "NOPERF"; exit 0; }
"$perf_bin" record -q -e cpu-clock -F 499 -p "$pid" -o /tmp/quant_procwatch_perf.data -- sleep "$2" >/dev/null 2>&1
# --percent-limit은 낮게 둔다 — 0.3%로 자르면 표본이 적은 회차에서 비율 합이 90%대로 빠져 표를 못 믿는다
"$perf_bin" report -i /tmp/quant_procwatch_perf.data --stdio -n -q --no-children --sort dso,sym --percent-limit 0.05 2>/dev/null | head -150
'''
# perf report -n 한 줄:  "    12.34%       123  quant_trader  [.] Engine::foo(...)"
_PERF_LINE = re.compile(r"^\s*([\d.]+)%\s+(\d+)\s+(\S+)\s+\[[.k]\]\s+(.+?)\s*$")


class LinuxProcSampler:
    """/proc 기반 표본. wsl_distro가 있으면 Windows에서 그 배포판 안의 프로세스를 본다."""

    def __init__(self, process_name: str, wsl_distro: str = ""):
        self.process_name = process_name
        self.wsl_distro = wsl_distro
        self._previous = None      # (monotonic, pid, proc_ticks, {thread_id: (name, ticks)})

    def reset(self):
        """다음 표본을 기준점부터 다시 잡는다 — 수집이 한 번 실패한 뒤 이어서 차분을 내면
        그 사이 시간이 통째로 한 표본에 몰려 CPU%가 실제보다 크게 나온다."""
        self._previous = None

    def _shell(self, script: str, *arguments: str, timeout: float) -> str:
        command = ["bash", "-c", script, "procwatch", *arguments]

        if self.wsl_distro:
            command = ["wsl", "-d", self.wsl_distro, "-u", "root", "-e", *command]

        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout, encoding="utf-8",
                                   errors="replace")
        return completed.stdout

    def sample(self):
        """한 번 읽어 직전 표본과의 차분으로 CPU%를 낸다. 첫 호출·프로세스 교체 직후는 None(기준점만 잡는다)."""
        output = self._shell(_PROC_SNAPSHOT_SCRIPT, self.process_name, timeout=15)
        now = time.monotonic()
        process_line = None
        threads = {}

        for line in output.splitlines():
            parts = line.split("|")

            if parts[0] == "P":
                process_line = parts
            elif parts[0] == "T" and len(parts) == 4:
                threads[int(parts[1])] = (parts[2], int(parts[3]))

        if process_line is None:
            self._previous = None
            return None

        pid, core_count, clock_ticks = int(process_line[1]), int(process_line[2]), int(process_line[3])
        process_ticks, rss_bytes, thread_count = int(process_line[4]), int(process_line[5]), int(process_line[6])
        previous = self._previous
        self._previous = (now, pid, process_ticks, threads)

        if previous is None or previous[1] != pid:
            return None

        elapsed = max(now - previous[0], 1e-3)
        to_percent = 100.0 / clock_ticks / elapsed
        previous_threads = previous[3]
        thread_rows = []

        for thread_id, (name, ticks) in threads.items():
            if thread_id not in previous_threads:
                continue

            thread_rows.append({
                "process_name": self.process_name, "pid": pid, "tid": thread_id, "thread_name": name,
                "cpu_percent": (ticks - previous_threads[thread_id][1]) * to_percent,
            })

        return {
            "process_name": self.process_name, "pid": pid,
            "cpu_percent": (process_ticks - previous[2]) * to_percent,
            "memory_mb": rss_bytes / (1024 * 1024), "thread_count": thread_count, "core_count": core_count,
            "threads": thread_rows,
        }

    def hotspots(self, seconds: float):
        """perf로 seconds 동안 표본을 떠서 함수별 자기 시간 비율 행을 돌려준다. perf가 없으면 None."""
        output = self._shell(_PERF_SCRIPT, self.process_name, str(seconds), timeout=seconds + 60)

        if "NOPERF" in output:
            return None

        pid = self._previous[1] if self._previous else None
        rows = []

        for line in output.splitlines():
            match = _PERF_LINE.match(line)

            if match:
                rows.append({
                    "process_name": self.process_name, "pid": pid, "sample_seconds": seconds,
                    "symbol": match.group(4)[:300], "shared_object": match.group(3),
                    "self_percent": float(match.group(1)), "samples": int(match.group(2)),
                })

        return rows


# ── Windows(psutil) 표본 ──────────────────────────────────────────────────────
def find_processes(process_name: str) -> list:
    """이름이 일치하는 프로세스를 전부 찾는다.

    하나만 돌려주면 역할을 나눠 띄운 판(시세·전략·주문 세 프로세스가 다 quant_trader.exe)에서 먼저
    찾은 하나만 재게 되고, 그 숫자를 한 프로세스 판과 나란히 놓으면 "나눴더니 CPU가 줄었다"는
    거짓 결론이 나온다 — 셋 중 하나만 본 것이다(2026-09-26 부하시험 준비 중 발견). proc_stats 에
    pid 칸이 있으니 여럿을 그대로 넣고, 보는 쪽에서 합치거나 나눠 본다."""
    found = []

    for process in psutil.process_iter(["pid", "name"]):
        try:
            if process.info["name"] and process.info["name"].lower() == process_name.lower():
                found.append(process)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue

    return found


MISSING_REPEAT_SEC = 300.0


def warn_missing(process_name: str, missing_since: float, last_warning: float) -> tuple:
    """찾는 프로세스가 없을 때 경고를 한 번만 찍고 마는 대신 5분마다 기다린 시간과 함께 다시 찍는다.
    한 번만 찍으면 이름을 잘못 준 것과 아직 안 뜬 것을 영영 구분할 수 없다 — 창은 멀쩡해 보이는데
    표가 비어 있게 된다(2026-09-22 부하 회차 준비 중 발견). 돌려주는 값은 (처음 없어진 시각, 마지막 경고 시각)."""
    now = time.monotonic()

    if not missing_since:
        logger.warning(f"프로세스 미기동(name={process_name}) — 뜰 때까지 대기")
        return now, now

    if now - last_warning >= MISSING_REPEAT_SEC:
        logger.warning(f"프로세스 미기동(name={process_name}) — {(now - missing_since) / 60:.0f}분째 대기. "
                       f"이름이 맞는지 본다(윈도우는 .exe까지, 리눅스는 pgrep -x 기준)")
        return missing_since, now

    return missing_since, last_warning


def _run_psutil(db, process_name: str, interval: float):
    if not _PSUTIL_AVAILABLE:
        raise RuntimeError("psutil이 설치되지 않았습니다: pip install psutil")

    core_count = psutil.cpu_count(logical=True)
    missing_since = 0.0
    last_missing_warning = 0.0
    primed = set()   # cpu_percent()를 한 번 불러 기준점을 잡아 둔 pid

    while True:
        processes = find_processes(process_name)

        if not processes:
            missing_since, last_missing_warning = warn_missing(
                process_name, missing_since, last_missing_warning)
            time.sleep(interval)
            continue

        missing_since = 0.0

        # 재는 방식: cpu_percent()에 간격을 주면 그 시간만큼 멈춰 서므로 프로세스가 셋이면 한 바퀴가
        #  세 배로 늘어난다. 대신 기준점만 잡아 두고 한 번만 자고 일어나 셋을 읽는다 — 세 값이 같은
        #  구간을 가리켜야 역할끼리 더한 값이 말이 된다.
        for process in processes:
            if process.pid not in primed:
                try:
                    process.cpu_percent()
                    primed.add(process.pid)
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue

        time.sleep(interval)
        primed &= {process.pid for process in processes}

        for process in processes:
            try:
                cpu_percent = process.cpu_percent()   # 위에서 잡은 기준점 이후 구간
                memory_mb = process.memory_info().rss / (1024 * 1024)
                thread_count = process.num_threads()
                db.insert_proc_stat({
                    "process_name": process_name,
                    "pid": process.pid,
                    "cpu_percent": cpu_percent,
                    "memory_mb": memory_mb,
                    "thread_count": thread_count,
                    "core_count": core_count,
                })
                logger.info(f"pid={process.pid} cpu={cpu_percent:.1f}% mem={memory_mb:.0f}MB "
                            f"threads={thread_count}")
            except (psutil.NoSuchProcess, psutil.AccessDenied) as error:
                logger.warning(f"표본 수집 중 프로세스 사라짐(name={process_name}): {error}")
            except Exception as error:
                # [inv] 리눅스 경로와 같은 약속 — 어떤 예외로도 이 루프를 나가지 않는다.
                logger.warning(f"표본 수집 실패 — 이어서 간다: {error}")


def _run_linux(db, process_name: str, interval: float, wsl_distro: str, perf_interval: float, perf_seconds: float,
               perf_cpu_floor: float):
    sampler = LinuxProcSampler(process_name, wsl_distro)
    where = f"wsl:{wsl_distro}" if wsl_distro else "local"
    logger.info(f"/proc 표본({where}) — perf 핫스팟 {'매 %.0f초 %.0f초 표본(CPU %.1f%% 이상일 때만)' % (perf_interval, perf_seconds, perf_cpu_floor) if perf_interval > 0 else '끔'}")
    missing_since = 0.0
    last_missing_warning = 0.0
    perf_supported = perf_interval > 0
    last_perf = 0.0
    consecutive_failures = 0

    while True:
        # [inv] 이 루프는 어떤 예외로도 빠져나가지 않는다 — 여기서 나가면 수집이 끝나고 그 뒤 하루가 통째로 빈다.
        #  WSL 호출은 배포판이 잠들거나 perf 직후면 몇 초씩 밀려 subprocess timeout이 실제로 난다
        #  (2026-09-22 09:32~09:44 12분 공백). 한 번 늦은 것과 영영 죽은 것을 구분해 늦은 쪽은 이어서 간다.
        try:
            sample = sampler.sample()
        except Exception as error:
            consecutive_failures += 1
            backoff = min(interval * consecutive_failures, 60.0)
            logger.warning(f"표본 수집 실패 {consecutive_failures}회째 — {backoff:.0f}초 뒤 다시 시도: {error}")
            sampler.reset()
            time.sleep(backoff)
            continue

        consecutive_failures = 0

        if sample is None:
            if sampler._previous is None:
                missing_since, last_missing_warning = warn_missing(
                    process_name, missing_since, last_missing_warning)

            time.sleep(interval)
            continue

        missing_since = 0.0
        threads = sample.pop("threads")
        db.insert_proc_stat(sample)
        db.insert_proc_thread_statistics(threads)
        busiest = sorted(threads, key=lambda row: -row["cpu_percent"])[:3]
        logger.info(f"pid={sample['pid']} cpu={sample['cpu_percent']:.1f}%/{sample['core_count']}코어 "
                    f"mem={sample['memory_mb']:.0f}MB threads={sample['thread_count']} "
                    f"top={', '.join('%s %.1f%%' % (row['thread_name'], row['cpu_percent']) for row in busiest)}")

        # 노는 프로세스에서 뜬 표본은 읽을 수 없다 — 499Hz로 10초를 떠도 CPU 1%면 표본이 50개뿐이라
        #  상위 함수 비율이 표본 몇 개로 정해진다. 바쁠 때만 떠서 표본이 모이는 회차만 남긴다.
        if perf_supported and time.monotonic() - last_perf >= perf_interval:
            if sample["cpu_percent"] < perf_cpu_floor:
                logger.info(f"핫스팟 건너뜀 — CPU {sample['cpu_percent']:.1f}% < 기준 {perf_cpu_floor:.1f}%")
            else:
                last_perf = time.monotonic()

                try:
                    rows = sampler.hotspots(perf_seconds)
                except Exception as error:
                    rows = []
                    logger.warning(f"핫스팟 표본 실패 — 이번 회차만 건너뛴다: {error}")

                if rows is None:
                    perf_supported = False
                    logger.warning("perf가 없어 함수별 핫스팟은 건너뛴다(apt install linux-tools-generic)")
                elif rows:
                    db.insert_proc_hotspots(rows)
                    total_samples = sum(row["samples"] for row in rows)
                    logger.info(f"핫스팟 {len(rows)}행 표본 {total_samples}개: "
                                + ", ".join(f"{row['symbol'][:40]} {row['self_percent']:.1f}%" for row in rows[:3]))

                sampler.reset()   # perf 동안의 CPU를 표본 하나로 몰지 않도록 기준점을 다시 잡는다

        time.sleep(interval)


def run(db, process_name: str = "", interval: float = 5.0, wsl_distro: str = "",
        perf_interval: float = 300.0, perf_seconds: float = 30.0, perf_cpu_floor: float = 5.0,
        log_file: str = ""):
    """무한 루프 — process_name을 매 주기 다시 찾는다(재기동으로 pid가 바뀌어도 이어서 수집).
    프로세스가 없으면 조용히 대기(엔진 기동 전·장 마감 후가 정상 상태).

    perf_seconds가 30초인 이유: 09-22 회차는 10초에 표본이 27개뿐이라 함수 순위가 우연이었다(1개 = 3.7%).
    엔진이 거의 놀고 있어서 생긴 일이라 근본 해결은 부하를 올리는 것이고, 이 값은 그때까지의 완화다."""
    on_linux = platform.system() == "Linux" or bool(wsl_distro)

    if not process_name:
        process_name = "quant_trader" if on_linux else "quant_trader.exe"

    if not log_file:
        # 엔진 말고 다른 프로세스(부하 하네스 bench_engine_load 등)를 볼 때는 로그를 따로 쓴다 —
        #  두 수집기가 한 파일을 회전시키면 Windows에서 잠금으로 깨진다.
        engine_names = ("quant_trader", "quant_trader.exe")
        log_file = "logs/procwatch.log" if process_name in engine_names             else f"logs/procwatch_{process_name.removesuffix('.exe')}.log"

    attach_file_handler(logger, log_file)

    logger.info(f"프로세스 자원 수집 시작: name={process_name} interval={interval}초 (Ctrl+C로 종료)")

    if on_linux:
        _run_linux(db, process_name, interval, wsl_distro, perf_interval, perf_seconds, perf_cpu_floor)
    else:
        _run_psutil(db, process_name, interval)
