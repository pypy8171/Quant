import logging
import logging.handlers
import os
import sys


def setup_logger(name: str = "quant", level: int = logging.INFO) -> logging.Logger:
    logger = logging.getLogger(name)
    if logger.handlers:  # 중복 핸들러 방지
        return logger
    logger.setLevel(level)
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        except ValueError:
            pass  # 이미 리다이렉트된 스트림 등 reconfigure 불가한 경우는 무시
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    ))
    logger.addHandler(handler)
    return logger


def attach_file_handler(logger: logging.Logger, log_file: str) -> logging.Logger:
    """콘솔에 더해 파일에도 남긴다. 창으로만 띄우는 상주 프로세스(procwatch·record)는 창이 닫히면
    로그가 통째로 사라져 사후에 원인을 못 밝힌다 — 2026-09-22 자원 수집 12분 공백이 그랬다.
    같은 파일을 두 번 붙이지 않는다(재호출해도 줄이 겹치지 않게).

    회전을 거는 것은 수집기가 밀리는 날 5초마다 실패 줄이 쌓이기 때문이다 — 평소엔 하루 몇 줄뿐이라
    안 걸어도 되지만, 정작 기록이 필요한 날에만 커진다."""
    if not log_file:
        return logger

    resolved_path = os.path.abspath(log_file)

    for existing in logger.handlers:
        if isinstance(existing, logging.FileHandler) and os.path.abspath(existing.baseFilename) == resolved_path:
            return logger

    os.makedirs(os.path.dirname(resolved_path) or ".", exist_ok=True)
    handler = logging.handlers.RotatingFileHandler(
        resolved_path, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
    handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    ))
    logger.addHandler(handler)
    logger.info(f"로그 파일: {resolved_path}")
    return logger
