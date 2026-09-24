#pragma once
#ifdef HAS_ZMQ

#include "core/MpscQueue.h"
#include "core/WakeGate.h"
#include "core/Types.h"
#include "ipc/RegimeCell.h"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <zmq.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// ZmqBridge  —  C++ 엔진과 Python 레이어 간 프로세스간 통신(IPC, Inter-Process Communication)
//
//  ZMQ(ZeroMQ) 소켓 두 개로 통신한다:
//  PUB  tcp://127.0.0.1:5555  — 엔진이 발행(publish). 포트는 config `zmq_pub_port`·`zmq_rep_port`(기본 5555·5556). 체결/시그널/주문/헬스를 구독자에게 단방향 송신.
//  REP  tcp://127.0.0.1:5556  — Python이 명령 전송(KILL / STATUS / PAUSE / RESUME), 엔진이 응답(reply).
//
//  프로세스를 셋으로 가르면(D-114) 역할마다 발행 포트를 따로 연다 — 주문 5555·시세 `zmq_feed_pub_port`·
//  전략 `zmq_strategy_pub_port`. 명령 소켓은 주문 쪽 하나뿐이라 시세·전략은 rep_port 0으로 만든다.
//  bind 주소는 set_bind_address로 바꾼다. KILL은 "KILL <token>" 형식이어야 하고 token 미설정이면 거부.
//
//  ZMQ 소켓은 스레드 세이프하지 않아 전용 zmq_thread_에서만 사용한다.
//  다른 스레드는 enqueue()로 메시지를 전달한다. TRADE만 예외다 — 수신 스레드(여럿일 수 있다)가 틱마다 부르는
//  자리라 JSON도 뮤텍스도 없이 TradeData를 MpscQueue에 memcpy로 넣고, 문자열은 송신 스레드가 만든다.
//  수신 스레드 비용 ~1,000 ns/틱 → ~26 ns/틱(tests/bench_zmq_publish.cpp, 2026-09-20). [why D-071]
// ─────────────────────────────────────────────────────────────────────────────
class ZmqBridge
{
public:
    // rep_port가 0 이하면 REP 소켓을 아예 열지 않는다 — PUB만 여는 발행 전용 모드다. [why D-129]
    explicit ZmqBridge(int pub_port = 5555, int rep_port = 5556);
    ~ZmqBridge();
    // 스레드·뮤텍스를 소유한다 — 복사는 원본과 사본이 같은 자원을 두 번 닫는 길이라 막는다.
    ZmqBridge(const ZmqBridge&)            = delete;
    ZmqBridge& operator=(const ZmqBridge&) = delete;

    bool start();
    void stop();

    // start() 전에만. 빈 주소는 무시한다.
    void set_bind_address(std::string address) { if (!address.empty()) bind_address_ = std::move(address); }
    // KILL 공유 토큰. 비어 있으면 KILL을 아예 받지 않는다 — 무인증 REQ 한 방으로 매매가 서는 것을 막는다.
    void set_control_token(std::string token) { control_token_ = std::move(token); }
    // 이 프로세스가 물린 브로커 계좌번호. 한 프로세스=한 계좌라 FILL/ORDER 페이로드에 고정으로 실어
    // DB 쪽에서 실계좌·모의계좌 원장이 섞이지 않게 한다.
    void set_account_no(std::string account) { account_no_ = std::move(account); }
    // 현재 선택된 국면(RISK_ON·NEUTRAL·RISK_OFF). Engine::apply_regime_selection이 국면이
    // 바뀔 때마다 갱신 — SIGNAL·FILL 페이로드에 그때그때 실어 DB의 regime 열을 채운다.
    //  쓰는 쪽은 데이터 스레드 하나, 읽는 쪽은 전략·주문·체결 스레드 여럿이다. 예전에는 std::string을
    //  잠금 없이 주고받아 경합이었다 — 정수 하나로 바꿔 원자로 오간다. 라벨 문자열은 읽는 쪽이 만든다.
    void set_regime(Regime regime);
    // 공유 쪽지 위의 국면 칸을 꽂는다. 갈라 띄우면 국면을 고르는 쪽(전략)과 체결을 적는 쪽(주문)이 다른
    //  프로세스라, 프로세스 안 정수만 보면 주문 쪽 값은 기동부터 끝까지 -1이고 체결의 regime 열이
    //  통째로 빈다. 꽂으면 전략이 그 칸에 적고 주문이 그 칸을 읽는다. [why D-129]
    //  [inv] 다리 스레드가 뜨기 전에 부른다. 수명은 Engine 의 자리표가 다시 깔릴 때까지다.
    void set_regime_cell(ipc::RegimeCell* cell) { regime_cell_ = cell; }
    // 이 다리를 연 프로세스의 역할(order·strategy·feed·both). HEALTH 한 건마다 실어, 갈라 띄운 날
    //  세 프로세스가 같은 표에 넣는 행을 읽는 쪽이 가를 수 있게 한다. [why D-129]
    void set_role_label(std::string label) { role_label_ = std::move(label); }

    // ── 이벤트 publish (스레드-안전: 내부 큐 경유) ──────────────────────────
    void publish_trade(const TradeData& trade);
    void publish_signal(const OrderSignal& signal);
    void publish_order(const OrderSignal& signal, bool ok);
    // HEALTH 한 건에 싣는 엔진 내부 수치. 큐 고수위와 지연 분위수는 기동 후 누적이라 줄지 않는다 —
    // 구간 값이 필요하면 읽는 쪽이 직전 행과 뺀다. 표본이 없는 지연은 -1. [why D-071]
    struct HealthSnapshot
    {
        uint64_t data_count             = 0;
        uint64_t signal_count           = 0;
        uint64_t order_count            = 0;
        uint64_t shard_high_water       = 0;   // 샤드 셀 가운데 가장 높았던 값
        uint64_t shard_capacity         = 0;
        uint64_t shard_out_size         = 0;   // 지금 쌓여 있는 깊이(누적 최대가 아니다)
        uint64_t shard_out_capacity     = 0;
        uint64_t order_queue_high_water = 0;
        uint64_t order_queue_capacity   = 0;
        uint64_t fill_queue_high_water  = 0;
        uint64_t fill_queue_capacity    = 0;
        uint64_t shard_dropped          = 0;
        uint64_t order_dropped          = 0;
        uint64_t order_stale            = 0;
        uint64_t fill_dropped           = 0;
        uint64_t latency_samples        = 0;
        int64_t  tick_to_signal_p50_us  = -1;
        int64_t  tick_to_signal_p99_us  = -1;
        int64_t  signal_to_pop_p50_us   = -1;
        int64_t  signal_to_pop_p99_us   = -1;
        int64_t  pop_to_done_p50_us     = -1;
        int64_t  pop_to_done_p99_us     = -1;
        int64_t  total_p50_us           = -1;
        int64_t  total_p99_us           = -1;

        // 직전 HEALTH 이후에 들어온 표본만의 구간 분위수. 누적 분위수는 한 번 튀면 안 내려와
        //  "언제 느려졌나"를 못 본다 — 그래서 같은 구간을 두 벌 싣는다. 표본이 없으면 -1. [why D-071]
        struct IntervalSegment
        {
            std::string_view name;        // health 열 이름 앞머리(<name>_p50_interval_us)
            int64_t          p50_us = -1;
            int64_t          p99_us = -1;
        };

        // [inv] name이 빈 칸은 안 싣는다 — 엔진이 구간 수만큼만 채운다.
        std::array<IntervalSegment, 16> interval_segments{};
        uint64_t                        interval_samples = 0; // 이번 구간에 들어온 주문 표본 수
    };

    void publish_health(const HealthSnapshot& snapshot);
    void publish_fill(const FillNotification& fill_notification, const std::string& strategy_id,
                      double commission, double tax,
                      double average_price, int net_quantity, double realized_pnl);

    // ── Python 명령 수신 콜백 설정 ──────────────────────────────────────────
    // command  : 수신된 명령 문자열 (KILL / STATUS / PAUSE <id> 등)
    // reply: 명령에 대한 응답 문자열 반환
    using CmdHandler = std::function<std::string(const std::string& command)>;
    void set_command_handler(CmdHandler handler)
    {
        command_handler_ = std::move(handler);
    }

    // 버린 건수는 원인별로 나눠 센다 — 한 칸에 뭉치면 고칠 자리를 못 고른다. 소켓이 안 받은 것은
    //  구독자·소켓 상한 쪽이고, 줄서기 큐가 찬 것은 송신 스레드가 못 따라온 것이며, 링이 찬 것은
    //  수신 스레드가 송신보다 빠른 것이라 손댈 곳이 서로 다르다. [why D-125]
    uint64_t drop_count() const;

    uint64_t socket_full_drop_count() const { return socket_full_drop_count_.load(); }
    uint64_t socket_error_drop_count() const { return socket_error_drop_count_.load(); }
    uint64_t send_queue_full_drop_count() const { return send_queue_full_drop_count_.load(); }
    uint64_t trade_ring_full_drop_count() const { return trade_ring_full_drop_count_.load(); }

    // TRADE 전용 봉투 — ts는 부른 시각(수신 스레드)이라 송신이 밀려도 바뀌지 않는다. trivially copyable.
    struct TradeEnvelope
    {
        int64_t   ts_ms = 0;
        TradeData trade;
    };

    // TRADE 페이로드 문자열. 공개인 이유는 bench_zmq_publish가 nlohmann dump()와 글자 단위로 같은지 검사해서다.
    //  account는 주문·체결과 같은 계좌 표식 — 리코더가 남의 엔진(같은 5555에 bind한 테스트·부하 하네스)의 틱을
    //  ticks 표에 섞지 않도록 거르는 근거다(09-22 장중 실측). 하네스는 계좌를 안 주므로 빈 문자열로 나간다.
    static void format_trade(const TradeEnvelope& envelope, std::string_view account, std::string& out);

private:
    // 토픽은 정수로 들고 이름은 송신 직전에 붙인다 — enqueue마다 문자열 비교 세 번을 하지 않으려고.
    enum class Topic : uint8_t
    {
        Trade,
        Signal,
        Order,
        Health,
        Fill
    };

    struct Message
    {
        Topic       topic;
        std::string payload;
    };

    void enqueue(Topic topic, std::string payload);
    // 큐에 넣은 뒤 송신 스레드를 깨운다. queue_mutex_를 놓은 뒤에만 부른다. [lock-order]
    void mark_work_pending();
    static const char* topic_name(Topic topic);
    std::string_view   current_regime_label() const;
    void thread_fn();

    int pub_port_;
    int rep_port_;
    std::string bind_address_ = "127.0.0.1";
    std::string control_token_;
    std::string account_no_;
    std::string role_label_;
    // 아직 판정이 없으면 -1 — 그때는 예전처럼 빈 라벨을 싣는다. 값이 있으면 Regime::Value다.
    //  한 프로세스로 돌 때(both) 쓰는 자리다. 갈라 띄우면 아래 공유 칸이 이 자리를 대신한다.
    std::atomic<int> regime_code_{ipc::kRegimeNone};
    // 꽂혀 있으면 이쪽이 정본이다. nullptr 이면 위 정수만 본다. [why D-129]
    ipc::RegimeCell* regime_cell_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread zmq_thread_;

    std::mutex queue_mutex_;
    std::queue<Message> send_queue_;
    MpscQueue<TradeEnvelope> trade_queue_;
    std::string              trade_payload_; // 송신 스레드 전용 재사용 버퍼 // 생산자 = WS 수신 스레드 수(둘 이상일 수 있다) → MPSC [why D-071 원칙 5]

    // 발행 전용 다리(REP 없음)의 송신 스레드를 생산자가 깨운다 — 폴링할 소켓이 없는데 sleep_for로
    //  쉬면 윈도우 타이머 격자에 걸려 한 바퀴가 길어진다. 깃발은 "비우기 전에 내리고 넣은 뒤에 세운다"
    //  순서라 깨우기가 새지 않는다. [why D-129]
    wake::WakeGate    send_gate_;
    std::atomic<bool> work_pending_{false};

    CmdHandler command_handler_;
    std::atomic<uint64_t> socket_full_drop_count_{0};      // PUB 소켓이 안 받았다(상한·구독자)
    std::atomic<uint64_t> socket_error_drop_count_{0};     // 보내다 예외가 났다
    std::atomic<uint64_t> send_queue_full_drop_count_{0};  // 줄서기 큐가 상한에 닿았다
    std::atomic<uint64_t> trade_ring_full_drop_count_{0};  // 체결 링이 찼다
};

#endif // HAS_ZMQ
