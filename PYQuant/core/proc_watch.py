"""
엔진 프로세스(quant_trader.exe) CPU/메모리 표본 수집 — ZMQ HEALTH에는 처리건수만 있고
자원 사용량이 없어 psutil로 별도로 떠서 TimescaleDB(proc_stats)에 쌓는다.
"""
import time

from core.logger import setup_logger

logger = setup_logger("quant.procwatch")

try:
    import psutil
    _PSUTIL_AVAILABLE = True
except ImportError:
    _PSUTIL_AVAILABLE = False


def find_process(process_name: str):
    """이름이 일치하는 프로세스 1개를 찾는다. 여러 개면 먼저 찾은 것(재기동 중 중복 기동은
    감시견이 막는 영역이라 여기서는 판단하지 않는다)."""
    for process in psutil.process_iter(["pid", "name"]):
        try:
            if process.info["name"] and process.info["name"].lower() == process_name.lower():
                return process
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return None


def run(db, process_name: str = "quant_trader.exe", interval: float = 5.0):
    """무한 루프 — process_name을 매 주기 다시 찾는다(재기동으로 pid가 바뀌어도 이어서 수집).
    프로세스가 없으면 조용히 대기(엔진 기동 전·장 마감 후가 정상 상태)."""
    if not _PSUTIL_AVAILABLE:
        raise RuntimeError("psutil이 설치되지 않았습니다: pip install psutil")

    logger.info(f"프로세스 자원 수집 시작: name={process_name} interval={interval}초 (Ctrl+C로 종료)")
    was_missing = False
    while True:
        process = find_process(process_name)
        if process is None:
            if not was_missing:
                logger.warning(f"프로세스 미기동(name={process_name}) — 뜰 때까지 대기")
                was_missing = True
            time.sleep(interval)
            continue
        was_missing = False
        try:
            cpu_percent = process.cpu_percent(interval=interval)  # 이 구간 동안 대기하며 측정
            memory_mb = process.memory_info().rss / (1024 * 1024)
            thread_count = process.num_threads()
            db.insert_proc_stat({
                "process_name": process_name,
                "pid": process.pid,
                "cpu_percent": cpu_percent,
                "memory_mb": memory_mb,
                "thread_count": thread_count,
            })
            logger.info(f"pid={process.pid} cpu={cpu_percent:.1f}% mem={memory_mb:.0f}MB "
                        f"threads={thread_count}")
        except (psutil.NoSuchProcess, psutil.AccessDenied) as error:
            logger.warning(f"표본 수집 중 프로세스 사라짐(name={process_name}): {error}")
            time.sleep(interval)
