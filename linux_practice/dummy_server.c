/*
 * dummy_server.c  — Linux 학습 연습용 프로세스
 *
 * 빌드:  gcc -o dummy_server dummy_server.c
 * 실행:  ./dummy_server &
 *
 * 이 서버가 제공하는 연습 포인트:
 *   ps / top / htop  — 프로세스 확인
 *   netstat/ss/lsof  — 9999 포트 점유 확인
 *   tail -f / grep   — 실시간 로그 추적
 *   kill / pkill     — SIGTERM(정상) vs SIGKILL(강제) 차이 체험
 *   free / top RSS   — 10초마다 1MB씩 증가하는 메모리 누수 패턴
 *   PID 파일         — /tmp/dummy_server.pid
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define LOG_PATH  "/tmp/dummy_server.log"
#define PID_PATH  "/tmp/dummy_server.pid"
#define PORT      9999
#define MB        (1024 * 1024)

static volatile int g_running = 1;
static FILE*        g_log     = NULL;

/* ── 타임스탬프 문자열 ──────────────────────────────────────────────── */
static void now_str(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm);
}

/* ── SIGTERM: graceful shutdown ─────────────────────────────────────
 * kill PID        → 여기로 들어옴 → 정리 후 종료
 * kill -9 PID     → 여기 안 들어옴 → 즉시 강제 종료 (정리 불가)
 * ------------------------------------------------------------------ */
static void on_sigterm(int sig) {
    (void)sig;
    char tbuf[32];
    now_str(tbuf, sizeof(tbuf));
    if (g_log) {
        fprintf(g_log, "[%s] INFO  signal=SIGTERM received — starting graceful shutdown\n", tbuf);
        fprintf(g_log, "[%s] INFO  cleaning up resources, removing pid file\n", tbuf);
        fflush(g_log);
    }
    g_running = 0;
}

/* ── SIGINT: Ctrl+C도 graceful ──────────────────────────────────── */
static void on_sigint(int sig) { on_sigterm(sig); }

int main(void) {
    /* PID 파일 기록 (lsof / ps 연습용) */
    {
        FILE* pf = fopen(PID_PATH, "w");
        if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    }

    /* 로그 파일 열기 */
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) { perror("fopen log"); return 1; }

    /* SIGTERM / SIGINT 핸들러 등록 */
    signal(SIGTERM, on_sigterm);
    signal(SIGINT,  on_sigint);

    /* ── TCP 소켓 9999 포트 LISTEN ─────────────────────────────────
     * 확인 명령어:
     *   sudo netstat -tulpn | grep 9999
     *   sudo ss -tulpn | grep 9999
     *   sudo lsof -i :9999
     *   nc -zv localhost 9999
     * ------------------------------------------------------------ */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* non-blocking: select()로 accept 시도하면서 메인 루프 유지 */
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(server_fd, 5);

    /* ── 시작 로그 ──────────────────────────────────────────────── */
    char tbuf[32];
    now_str(tbuf, sizeof(tbuf));
    fprintf(g_log,
        "[%s] INFO  dummy_server started pid=%d port=%d log=%s\n",
        tbuf, (int)getpid(), PORT, LOG_PATH);
    fflush(g_log);

    printf("[dummy_server] pid=%d  port=%d  log=%s\n",
           (int)getpid(), PORT, LOG_PATH);
    printf("  tail -f %s          # 실시간 로그\n", LOG_PATH);
    printf("  sudo ss -tulpn | grep %d   # 포트 확인\n", PORT);
    printf("  kill %d             # SIGTERM (graceful)\n", (int)getpid());
    printf("  kill -9 %d          # SIGKILL  (즉시 강제)\n\n", (int)getpid());
    fflush(stdout);

    /* ── 로그 패턴용 데이터 ────────────────────────────────────── */
    const char* actors[]  = {"42", "101", "7", "256", "999", "1024", "31337"};
    const char* actions[] = {"trade", "buy", "sell", "login", "logout", "query"};
    const char* levels[]  = {"INFO ", "INFO ", "INFO ", "INFO ", "WARN ", "ERROR"};
    int n_actors  = 7, n_actions = 6, n_levels = 6;

    /* ── 메모리 누수 시뮬레이션 ─────────────────────────────────
     * 10초마다 1MB 할당 후 절대 해제 안 함
     * top 에서 RSS 컬럼이 점점 올라가는 걸 직접 볼 수 있음
     * --------------------------------------------------------- */
    int   leak_mb    = 0;
    int   tick       = 0;

    /* ── 메인 루프 ───────────────────────────────────────────── */
    while (g_running) {
        sleep(1);
        ++tick;
        now_str(tbuf, sizeof(tbuf));

        /* 접속 시도가 있으면 수락 후 바로 닫기 (연결 테스트용) */
        int client = accept(server_fd, NULL, NULL);
        if (client >= 0) {
            const char* msg = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nOK";
            write(client, msg, strlen(msg));
            close(client);
            fprintf(g_log, "[%s] INFO  accepted connection from client fd=%d\n", tbuf, client);
        }

        /* 1초마다 일반 로그 */
        int ai = tick % n_actors;
        int ac = tick % n_actions;
        int lv = tick % n_levels;
        fprintf(g_log, "[%s] %s actor_id=%s action=%s seq=%d\n",
                tbuf, levels[lv], actors[ai], actions[ac], tick);

        /* 7틱마다 ERROR 추가 (grep ERROR 연습용) */
        if (tick % 7 == 0) {
            fprintf(g_log,
                "[%s] ERROR actor_id=%s action=trade FAILED: connection timeout (seq=%d)\n",
                tbuf, actors[ai], tick);
        }

        /* 13틱마다 WARN */
        if (tick % 13 == 0) {
            fprintf(g_log,
                "[%s] WARN  actor_id=%s queue_depth=high latency_ms=%d\n",
                tbuf, actors[(ai + 2) % n_actors], 450 + (tick % 200));
        }

        /* 10틱마다 1MB 누수 */
        if (tick % 10 == 0) {
            void* p = malloc(MB);
            if (p) {
                memset(p, 0xAB, MB);   /* 실제로 물리 페이지에 쓰기 → RSS 증가 */
                ++leak_mb;
                /* 해제 안 함 — 의도적 누수 */
            }
            fprintf(g_log,
                "[%s] INFO  mem_leak_simulated total_leaked=%dMB (watch: top -p %d)\n",
                tbuf, leak_mb, (int)getpid());
        }

        /* 30틱마다 통계 요약 */
        if (tick % 30 == 0) {
            fprintf(g_log,
                "[%s] INFO  stats uptime=%ds leaked=%dMB pid=%d port=%d\n",
                tbuf, tick, leak_mb, (int)getpid(), PORT);
        }

        fflush(g_log);
    }

    /* ── 종료 정리 (SIGTERM 경로만 도달) ─────────────────────── */
    now_str(tbuf, sizeof(tbuf));
    fprintf(g_log,
        "[%s] INFO  shutdown complete. uptime=%ds leaked=%dMB\n",
        tbuf, tick, leak_mb);
    fclose(g_log);
    close(server_fd);
    remove(PID_PATH);
    return 0;
}
