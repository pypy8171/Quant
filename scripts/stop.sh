#!/usr/bin/env bash
# 실행 중인 컨테이너 중지 및 제거
# 사용법: ./scripts/stop.sh [--volumes]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "==> 컨테이너 중지 ($(date '+%Y-%m-%d %H:%M:%S'))"

if [[ "${1:-}" == "--volumes" ]]; then
    echo "    볼륨도 함께 삭제"
    docker compose down --volumes
else
    docker compose down
fi

echo "==> 완료"
