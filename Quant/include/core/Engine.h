#pragma once
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "strategy/StrategyBase.h"
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Engine  —  퀀트 트레이딩 엔진
//
//  [데이터 스레드]  KIS REST 일봉 폴링   → market_queue_
//  [전략 스레드]    ob_queue_ + td_queue_ + market_queue_ → order_queue_
//  [주문 스레드]    order_queue_ → KIS REST 주문 (KR/US 자동 분기)
//
//  WS 구독 목록은 on_start() 이후 전략의 get_watch_specs()로 동적 수집
// ─────────────────────────────────────────────────────────────────────────────
class Engine
{
public:
    Engine(KisConfig kis_cfg, int fetch_interval_sec = 60);
    ~Engine();

    void add_strategy(std::unique_ptr<StrategyBase> strategy);
    void start();
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

private:
    void data_thread_fn();
    void strategy_thread_fn();
    void order_thread_fn();
    void control_thread_fn(); // ZMQ REP 명령 처리 (HAS_ZMQ 시 활성)

    bool is_kr_market_open() const;
    bool is_us_market_open() const;
    bool is_any_market_open() const;
    void print_stats() const;

    KisConfig kis_cfg_;
    int fetch_interval_sec_;

    std::unique_ptr<KisClient> kis_;
    std::unique_ptr<KisWebSocket> ws_;

    std::vector<std::unique_ptr<StrategyBase>> strategies_;

    RingBuffer<MarketData> market_queue_{1024};
    RingBuffer<OrderSignal> order_queue_{256};
    RingBuffer<OrderBook> ob_queue_{4096}; // 호가 (국내)
    RingBuffer<TradeData> td_queue_{4096}; // 체결 (미국 + 국내)

    std::thread data_thread_;
    std::thread strategy_thread_;
    std::thread order_thread_;
    std::thread control_thread_;

    std::atomic<bool> running_{false};

#ifdef HAS_ZMQ
    std::unique_ptr<ZmqBridge> zmq_bridge_;
#endif

    std::atomic<uint64_t> data_count_{0};
    std::atomic<uint64_t> signal_count_{0};
    std::atomic<uint64_t> order_count_{0};

    OrderGate order_gate_;

    // 전략에서 수집한 구독 스펙 (on_start 이후 확정)
    std::vector<WatchSpec> watch_specs_;
};
