// 체결을 TimescaleDB ticks 표에 넣는 전용 워커 스레드들. 엔진 기동 때 맨 먼저 만들어 끝날 때까지 둔다. [why D-148]
//  수신 스레드는 on_trade로 큐에 넣기만 하고 바로 돌아온다. DB 호출(블로킹)은 적재 워커 스레드에서만 돈다 —
//  DB가 느리거나 멈추면 큐가 차고, 넘친 행은 버리고 센다. 수신·전략·주문 스레드는 DB를 기다리지 않는다.
//  적재 워커 tick_workers개가 symbol_id % 개수로 나눈 큐를 묶어 COPY로 넣는다(원칙 2). 파이썬 적재기
//  (PYQuant/main.py record)와 같은 표·같은 값이다. 워커마다 연결 하나.
//  접속 실패는 워커가 나눠 보는 상태표에 적혀, 다음 시도 시각 전에는 아무도 다시 붙지 않고 그 뒤에도 한 워커만 붙어 본다.
//  끝낼 때 stop()은 stop_grace_ms 뒤 연결을 끊고, 그래도 안 돌아오는 워커는 떼어 둔다.
//  DB에 묻고 답을 받는 요청 경로는 두지 않는다 — 받을 곳(D-128 원장)이 생길 때 그 요구에 맞춰 만든다.
//  libpq 호출은 DbManager.cpp에만 있고, 그 파일은 HAS_PQ일 때만 빌드된다. 이 헤더는 설정 구조체 때문에 늘 포함된다.
#pragma once

#include "core/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace db
{

// config.json "database" 블록. 비밀번호는 config에 적지 않고 TSDB_PASSWORD 환경변수로 받는다(파이썬 적재기와 같다).
struct DbConfig
{
    bool        enabled = false;
    std::string host = "localhost";
    int         port = 5432;
    std::string dbname = "quant";
    std::string user = "quant";
    unsigned    tick_workers = 2;              // 체결 적재 워커 수(= DB 연결 수)
    size_t      tick_queue_capacity = 262144;  // 적재 워커 하나의 큐 칸 수. 2의 거듭제곱으로 올림된다
    size_t      batch_rows = 5000;
    int         flush_ms = 200;
    int         stop_grace_ms = 3000;          // stop()이 남은 일을 끝내며 기다리는 한도. 넘기면 연결을 끊는다
};

struct DbStatistics
{
    uint64_t ticks_offered = 0;
    uint64_t ticks_written = 0;
    uint64_t ticks_dropped = 0;    // 적재 큐가 차서 버린 행
    uint64_t ticks_failed = 0;     // 서버가 거절했거나 끝낼 때 DB가 없어 못 넣은 행
    uint64_t ticks_ambiguous = 0;  // 끝 신호 뒤 답 전에 끊긴 행 — 표에 중복 방지 장치가 없어 다시 넣지 않는다
};

// ── COPY 글자 만들기(DB 없이 시험한다) ──────────────────────────────────────────
// COPY text 형식의 이스케이프 — 역슬래시·탭·줄바꿈·CR. 파이썬 _COPY_ESCAPES와 같다.
void append_copy_text(std::string& out, std::string_view text);

// epoch 밀리초를 "YYYY-MM-DDTHH:MM:SS.mmm+00:00"으로. 파이썬 datetime.isoformat()과 같은 시각을 뜻한다.
void append_iso_utc(std::string& out, int64_t epoch_ms);

// ticks(ts,ticker,price,volume,direction,market) 한 줄. ts는 큐에 넣은 시각 — ZmqBridge 발행 시각과 같은 뜻이다.
void append_trade_row(std::string& out, const TradeData& trade, int64_t enqueue_ms);

// 윈도우에서 host가 localhost면 WSL eth0 주소를 돌려준다(wslrelay를 거치지 않으려고). 못 구하면 host 그대로.
//  끄는 스위치·배포판 이름은 파이썬과 같은 TSDB_WSL_DIRECT·TSDB_WSL_DISTRO. 접속 때만 부른다(약 150ms). [why D-144]
std::string resolve_host(const std::string& host);

class DbManager
{
public:
    // 적재 워커를 모두 띄운다. 비밀번호가 없거나 워커가 0개면 ok()가 false고 on_trade는 아무것도 하지 않는다.
    explicit DbManager(DbConfig config);

    ~DbManager();

    DbManager(const DbManager&)            = delete;
    DbManager& operator=(const DbManager&) = delete;

    [[nodiscard]] bool ok() const noexcept;

    // 체결 적재(수신 스레드가 부른다). 큐가 차면 버리고 센다 — DB가 느리거나 멈춰도 틱 경로가 서지 않는다.
    void on_trade(const TradeData& trade) noexcept;

    // 남은 일을 끝내고 워커를 거둔다. 생산자가 모두 멈춘 뒤 부른다. 두 번 불러도 된다.
    //  DB가 멈춰 있어도 stop_grace_ms + 연결 제한(약 6초) 안에 돌아온다 — 못 넣은 행은 거절로 센다.
    void stop();

    // 체결 적재 큐가 빌 때까지 기다린다. 시험용 — hot path에서 부르지 않는다.
    void flush_ticks();

    [[nodiscard]] DbStatistics statistics() const noexcept;

    // 워커와 나눠 갖는 상태. 정의는 DbManager.cpp 안에만 있다. stop()이 돌아오지 않는 워커를 떼어 둘 때도
    //  그 워커가 이것을 붙들고 있어 DbManager가 먼저 사라져도 댕글링이 없다.
    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace db
