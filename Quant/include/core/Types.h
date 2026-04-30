#pragma once
#include <string>
#include <chrono>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// 시세 데이터 (KIS API → 수신 스레드 → RingBuffer → 전략)
// ─────────────────────────────────────────────────────────────────────────────
struct MarketData {
    std::string ticker;         // 종목 코드 (e.g. "005930")
    double      close;          // 현재가/종가
    double      open;           // 시가
    double      high;           // 고가
    double      low;            // 저가
    int64_t     volume;         // 거래량
    std::chrono::system_clock::time_point timestamp;

    // 이동평균 계산용 인덱스 (엔진이 채움)
    int         bar_index = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 신호 (전략 → RingBuffer → 주문 실행 스레드)
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderSide { BUY, SELL, NONE };
enum class OrderType { MARKET, LIMIT };

struct OrderSignal {
    std::string ticker;
    OrderSide   side        = OrderSide::NONE;
    OrderType   type        = OrderType::MARKET;
    int         quantity    = 0;
    double      price       = 0.0;      // LIMIT 주문 시 사용
    std::string strategy_id;            // 어느 전략에서 발생했는지
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 포지션 관리
// ─────────────────────────────────────────────────────────────────────────────
struct Position {
    std::string ticker;
    int         quantity    = 0;
    double      avg_price   = 0.0;
    double      unrealized_pnl = 0.0;
};
