#!/usr/bin/env bash
# C++ 엔진 + Python 모니터 동시 시작 (백그라운드 데몬 모드)
# 사용법: ./scripts/start.sh
# 엔진은 docker-compose.yml 의 command 대로 TRADE 로 뜬다 — 실행 모드는 TRADE 하나다(D-130).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# config.json 존재 여부 확인
if [[ ! -f "Quant/config/config.json" ]]; then
    echo "[ERROR] Quant/config/config.json 없음."
    echo "        Quant/config/config.json.example 을 복사해서 인증정보를 채워주세요."
    exit 1
fi

echo "==> 엔진 시작 ($(date '+%Y-%m-%d %H:%M:%S'))"
echo "[WARNING] 주문이 나갑니다. 계속하려면 Enter..."
read -r
docker compose up -d quant-engine quant-python
echo "==> 백그라운드 실행 중. 로그 보기: ./scripts/logs.sh"
echo "    중지하려면:              ./scripts/stop.sh"
