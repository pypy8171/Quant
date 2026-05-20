#!/usr/bin/env bash
# C++ 엔진 + Python 모니터 동시 시작 (백그라운드 데몬 모드)
# 사용법: ./scripts/start.sh [kr_test|feed|trade]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

MODE="${1:-kr_test}"

# config.json 존재 여부 확인
if [[ ! -f "Quant/config/config.json" ]]; then
    echo "[ERROR] Quant/config/config.json 없음."
    echo "        Quant/config/config.json.example 을 복사해서 인증정보를 채워주세요."
    exit 1
fi

echo "==> 엔진 시작 (모드: $MODE, $(date '+%Y-%m-%d %H:%M:%S'))"

case "$MODE" in
  feed)
    # FEED 모드: WebSocket 체결/호가만 수신, 주문 없음
    docker compose run --rm quant-engine \
        ./quant_trader config/config.json FEED
    ;;
  trade)
    # TRADE 모드: 실제 주문 발생 — 주의!
    echo "[WARNING] TRADE 모드는 실제 주문이 나갑니다. 계속하려면 Enter..."
    read -r
    docker compose up -d quant-engine quant-python
    echo "==> 로그 확인: ./scripts/logs.sh"
    ;;
  kr_test|*)
    # KR_TEST 모드 (기본): 지수+관심종목 모니터링, 주문 없음
    docker compose up -d quant-engine quant-python
    echo "==> 백그라운드 실행 중. 로그 보기: ./scripts/logs.sh"
    echo "    중지하려면:              ./scripts/stop.sh"
    ;;
esac
