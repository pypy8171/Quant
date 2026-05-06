#!/usr/bin/env bash
# ================================================================
#  Linux 학습 연습 스크립트
#  사용법: bash practice.sh
#  (각 단계를 직접 복붙해서 실행 — 자동 실행 스크립트 아님)
# ================================================================

# ── 빌드 ─────────────────────────────────────────────────────────
gcc -o dummy_server dummy_server.c
# → dummy_server 바이너리 생성됨

# ================================================================
#  단계 1 — 프로세스 관리
# ================================================================

./dummy_server &
# → "[dummy_server] pid=XXXXX  port=9999  log=/tmp/dummy_server.log" 출력
# → 백그라운드 실행, PID 기억해두기

ps -ef | grep dummy_server
# 확인할 것:
#   UID   PID   PPID  ... CMD
#   user  1234  5678  ./dummy_server
#   → PPID = 현재 쉘 PID (부모-자식 관계)
# ps -ef vs ps aux 차이: -ef 는 PPID·SID 표시, aux 는 CPU%·MEM%·VSZ·RSS 표시

ps aux | grep dummy_server
# 확인할 것:
#   %CPU  %MEM  VSZ    RSS
#   0.0   0.1   4096   1024   ← RSS가 실행할수록 10초마다 1MB씩 증가함

# ================================================================
#  단계 2 — 리소스 모니터링
# ================================================================

top -p $(cat /tmp/dummy_server.pid)
# → 이 프로세스만 골라서 보기
# 단축키:
#   q → 종료
#   M → 메모리 정렬 (RSS 컬럼이 10초마다 +1MB 되는 것 관찰)
#   P → CPU 정렬
#   1 → CPU 코어별 사용률 표시
# 확인할 것: RES(RSS) 컬럼이 점점 늘어남 = 메모리 누수 패턴

free -h
# 확인: dummy_server 실행 전후 used 값 비교

watch -n 5 free -h
# 5초마다 자동 갱신 — Ctrl+C 로 종료

# ================================================================
#  단계 3 — 네트워크 진단
# ================================================================

sudo ss -tulpn | grep 9999
# 기대 출력:
#   tcp  LISTEN  0  5  0.0.0.0:9999  0.0.0.0:*  users:(("dummy_server",pid=XXXXX,...))
# 확인: State=LISTEN, 프로세스명 dummy_server

sudo netstat -tulpn | grep 9999
# ss 와 동일 정보, 구형 명령어 (RHEL 8 이하, Ubuntu 20 이하 환경)

sudo lsof -i :9999
# 기대 출력:
#   COMMAND        PID  USER  TYPE  NODE  NAME
#   dummy_ser  XXXXX  user  IPv4  TCP  *:9999 (LISTEN)
# 확인: COMMAND, PID, TYPE=IPv4, NAME에 포트번호

# 외부 접속 테스트 (다른 터미널에서)
nc -zv localhost 9999
# 기대 출력: "Connection to localhost 9999 port [tcp/*] succeeded!"
# → dummy_server 로그에 "accepted connection" 라인도 찍힘

curl localhost:9999
# 기대 출력: "OK"  ← dummy_server가 HTTP 200 응답 줌

# ================================================================
#  단계 4 — 로그 추적
# ================================================================

tail -f /tmp/dummy_server.log
# → 1초마다 새 줄 추가됨. Ctrl+C 로 종료

tail -f /tmp/dummy_server.log | grep ERROR
# 기대: 7초마다 한 줄씩
#   [2026-05-05 21:40:07] ERROR actor_id=42 action=trade FAILED: connection timeout

tail -f /tmp/dummy_server.log | grep "actor_id=42"
# → actor_id 42번만 필터링. 실제 운영에서 특정 유저 트래킹하는 패턴

grep -c ERROR /tmp/dummy_server.log
# → 에러 줄 수만 숫자로 출력

grep "action=trade" /tmp/dummy_server.log | awk '{print $3}' | sort | uniq -c | sort -rn
# $3 = actor_id=xxx 필드
# 기대 출력:
#   8  actor_id=42
#   7  actor_id=101
#   ...
# → 어떤 actor가 trade를 가장 많이 했는지 집계

tail -n 100 /tmp/dummy_server.log | grep WARN
# 마지막 100줄에서 WARN만 추출

# ================================================================
#  단계 5 — vi 연습
# ================================================================
# vi /tmp/dummy_server.log 로 열어서:
#   :set nu        → 줄 번호
#   /ERROR         → 에러 라인 검색 (n=다음, N=이전)
#   /actor_id=42   → 특정 actor 검색
#   gg             → 맨 위
#   G              → 맨 아래
#   :q!            → 저장 없이 종료

# ================================================================
#  단계 6 — SIGTERM vs SIGKILL 직접 비교
# ================================================================

PID=$(cat /tmp/dummy_server.pid)

# --- SIGTERM 테스트 ---
kill $PID
# → dummy_server 로그 마지막에:
#   [INFO]  signal=SIGTERM received — starting graceful shutdown
#   [INFO]  cleaning up resources, removing pid file
#   [INFO]  shutdown complete. uptime=XXs leaked=XXMiB
# → /tmp/dummy_server.pid 파일 자동 삭제됨

ls /tmp/dummy_server.pid   # → 없음 (삭제됨)
tail -5 /tmp/dummy_server.log  # → graceful 종료 메시지 확인

# dummy_server 다시 실행
./dummy_server &
sleep 2

# --- SIGKILL 테스트 ---
PID=$(cat /tmp/dummy_server.pid)
kill -9 $PID
# → 로그에 shutdown 메시지 없음 (핸들러 실행 자체가 안 됨)
# → /tmp/dummy_server.pid 파일 남아있음 (정리 못 함)

ls /tmp/dummy_server.pid   # → 파일 남아있음 (정리 안 됨)
tail -5 /tmp/dummy_server.log  # → shutdown 메시지 없음

# 학습 답변 포인트:
# SIGTERM: 프로세스에 "끝낼 준비해" 신호 → 핸들러에서 DB 커넥션/파일 닫기 가능
# SIGKILL: 커널이 즉시 프로세스 테이블에서 제거 → 자원 회수 불가, graceful shutdown 불가

# ================================================================
#  단계 7 — PID 파일 활용 패턴 (운영 표준)
# ================================================================

cat /tmp/dummy_server.pid      # PID 확인
kill $(cat /tmp/dummy_server.pid)  # PID 파일로 종료 (pkill보다 정확)

# 프로세스가 살아있는지 확인
kill -0 $(cat /tmp/dummy_server.pid) 2>/dev/null && echo "running" || echo "stopped"

# ================================================================
#  AlmaLinux 9 (RHEL 계열) — dnf 연습
# ================================================================
# wsl --install -d AlmaLinux-9 (PowerShell에서)
# 들어간 뒤:

# sudo dnf install -y gcc htop net-tools lsof
# → apt install 과 동일 역할, 패키지명도 거의 같음

# rpm -qa | grep gcc          # 설치 확인 (dpkg -l 과 동일 역할)
# dnf list installed | head   # 전체 목록

# systemctl status sshd
# systemctl list-units --type=service --state=running | head
# journalctl -u sshd --since "10 min ago"
# journalctl -xe | tail -30   # 시스템 에러 빠르게 보기
