#!/usr/bin/env bash
# 도커 이미지 빌드 (소스 변경 후 실행)
# 사용법: ./scripts/build.sh [--no-cache]
set -euo pipefail

# 스크립트 위치 기준으로 프로젝트 루트를 찾음
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "==> 도커 이미지 빌드 시작 ($(date '+%Y-%m-%d %H:%M:%S'))"

if [[ "${1:-}" == "--no-cache" ]]; then
    echo "    캐시 없이 전체 재빌드"
    docker compose build --no-cache
else
    echo "    캐시 사용 (변경된 레이어만 재빌드)"
    docker compose build
fi

echo "==> 빌드 완료"
echo "    실행하려면: ./scripts/start.sh"
