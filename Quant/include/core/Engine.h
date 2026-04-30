#pragma once
#include "core/RingBuffer.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"
#include "api/KisClient.h"
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Engine  —  퀀트 트레이딩 엔진
//
// 스레드 구조:
//   [데이터 수집 스레드]  →  market_queue (RingBuffer)
//   [전략 처리 스레드]   →  order_queue  (RingBuffer)
//   [주문 실행 스레드]   →  KisClient.send_order()
//
// 게임서버의 패킷 처리 분산 구조와 동일한 패턴
// ─────────────────────────────────────────────────────────────────────────────
class Engine {
public:
    Engine(KisConfig kis_cfg,
           std::vector<std::string> tickers,
           int fetch_interval_sec = 60);
    ~Engine();

    // 전략 등록 (플러그인 구조)
    void add_strategy(std::unique_ptr<StrategyBase> strategy);

    // 엔진 시작/중지
    void start();
    void stop();

    bool is_running() const { return running_.load(); }

private:
    // 스레드 함수들
    void data_thread_fn();      // KIS API → market_queue
    void strategy_thread_fn();  // market_queue → order_queue
    void order_thread_fn();     // order_queue → KIS 주문

    // 장 시간 체크 (09:00 ~ 15:30 KST)
    bool is_market_open() const;

    // 통계 출력
    void print_stats() const;

    KisConfig                               kis_cfg_;
    std::vector<std::string>                tickers_;
    int                                     fetch_interval_sec_;

    std::unique_ptr<KisClient>              kis_;

    // 전략 목록
    std::vector<std::unique_ptr<StrategyBase>> strategies_;

    // 스레드 간 통신 큐
    RingBuffer<MarketData>                  market_queue_{1024};
    RingBuffer<OrderSignal>                 order_queue_{256};

    // 스레드
    std::thread data_thread_;
    std::thread strategy_thread_;
    std::thread order_thread_;

    std::atomic<bool> running_{false};

    // 통계
    std::atomic<uint64_t> data_count_{0};
    std::atomic<uint64_t> signal_count_{0};
    std::atomic<uint64_t> order_count_{0};
};
