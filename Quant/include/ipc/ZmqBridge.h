#pragma once
#ifdef HAS_ZMQ

#include "core/Types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <zmq.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// ZmqBridge  —  C++ 엔진과 Python 레이어 간 프로세스간 통신(IPC, Inter-Process Communication)
//
//  ZMQ(ZeroMQ) 소켓 두 개로 통신한다:
//  PUB  tcp://127.0.0.1:5555  — 엔진이 발행(publish). 체결/시그널/주문/헬스를 구독자에게 단방향 송신.
//  REP  tcp://127.0.0.1:5556  — Python이 명령 전송(KILL / STATUS / PAUSE / RESUME), 엔진이 응답(reply).
//  bind 주소는 set_bind_address로 바꾼다. KILL은 "KILL <token>" 형식이어야 하고 token 미설정이면 거부.
//
//  ZMQ 소켓은 스레드 세이프하지 않아 전용 zmq_thread_에서만 사용한다.
//  다른 스레드는 enqueue()로 메시지를 전달한다.
// ─────────────────────────────────────────────────────────────────────────────
class ZmqBridge
{
public:
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
    // 현재 선택된 국면 라벨(RISK_ON·NEUTRAL·RISK_OFF). Engine::apply_regime_selection이 국면이
    // 바뀔 때마다 갱신 — FILL 페이로드에 그때그때 실어 DB의 regime 열을 채운다.
    void set_regime_label(std::string label) { regime_label_ = std::move(label); }

    // ── 이벤트 publish (스레드-안전: 내부 큐 경유) ──────────────────────────
    void publish_trade(const TradeData& trade);
    void publish_signal(const OrderSignal& signal);
    void publish_order(const OrderSignal& signal, bool ok);
    void publish_health(uint64_t data_count, uint64_t signal_count, uint64_t order_count);
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

    uint64_t drop_count() const { return drop_count_.load(); }

private:
    struct Message
    {
        std::string topic;
        std::string payload;
    };

    void enqueue(std::string topic, std::string payload);
    void thread_fn();

    int pub_port_;
    int rep_port_;
    std::string bind_address_ = "127.0.0.1";
    std::string control_token_;
    std::string account_no_;
    std::string regime_label_;

    std::atomic<bool> running_{false};
    std::thread zmq_thread_;

    std::mutex queue_mutex_;
    std::queue<Message> send_queue_;

    CmdHandler command_handler_;
    std::atomic<uint64_t> drop_count_{0};
};

#endif // HAS_ZMQ
