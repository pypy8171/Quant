#!/usr/bin/env bash
# 컨테이너 로그 실시간 확인
# 사용법: ./scripts/logs.sh [engine|python|all]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

TARGET="${1:-all}"

case "$TARGET" in
  engine)
    echo "==> quant-engine 로그 (Ctrl+C 로 종료)"
    docker compose logs -f --tail=50 quant-engine
    ;;
  python)
    echo "==> quant-python 로그 (Ctrl+C 로 종료)"
    docker compose logs -f --tail=50 quant-python
    ;;
  all|*)
    echo "==> 전체 로그 (Ctrl+C 로 종료)"
    echo "    engine 만 보기: ./scripts/logs.sh engine"
    docker compose logs -f --tail=50
    ;;
esac
