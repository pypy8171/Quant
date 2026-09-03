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
//  PUB  tcp://*:5555  — 엔진이 발행(publish). 체결/시그널/주문/헬스를 구독자에게 단방향 송신.
//  REP  tcp://*:5556  — Python이 명령 전송(KILL / STATUS / PAUSE / RESUME), 엔진이 응답(reply).
//
//  ZMQ 소켓은 스레드 세이프하지 않아 전용 zmq_thread_에서만 사용한다.
//  다른 스레드는 enqueue()로 메시지를 전달한다.
// ─────────────────────────────────────────────────────────────────────────────
class ZmqBridge
{
public:
    explicit ZmqBridge(int pub_port = 5555, int rep_port = 5556);
    ~ZmqBridge();

    bool start();
    void stop();

    // ── 이벤트 publish (스레드-안전: 내부 큐 경유) ──────────────────────────
    void publish_trade(const TradeData& td);
    void publish_signal(const OrderSignal& sig);
    void publish_order(const OrderSignal& sig, bool ok);
    void publish_health(uint64_t data_cnt, uint64_t sig_cnt, uint64_t ord_cnt);
    void publish_fill(const FillNotification& fn, double commission, double tax,
                      double avg_price, int net_qty, double realized_pnl);

    // ── Python 명령 수신 콜백 설정 ──────────────────────────────────────────
    // cmd  : 수신된 명령 문자열 (KILL / STATUS / PAUSE <id> 등)
    // reply: 명령에 대한 응답 문자열 반환
    using CmdHandler = std::function<std::string(const std::string& cmd)>;
    void set_command_handler(CmdHandler handler)
    {
        cmd_handler_ = std::move(handler);
    }

    uint64_t drop_count() const { return drop_count_.load(); }

private:
    struct Msg
    {
        std::string topic;
        std::string payload;
    };

    void enqueue(std::string topic, std::string payload);
    void thread_fn();

    int pub_port_;
    int rep_port_;

    std::atomic<bool> running_{false};
    std::thread zmq_thread_;

    std::mutex queue_mtx_;
    std::queue<Msg> send_queue_;

    CmdHandler cmd_handler_;
    std::atomic<uint64_t> drop_count_{0};
};

#endif // HAS_ZMQ
