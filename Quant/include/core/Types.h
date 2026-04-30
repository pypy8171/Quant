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

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 호가 (KIS WebSocket H0STASP0)
// ─────────────────────────────────────────────────────────────────────────────
struct OrderBookLevel {
    double  price    = 0.0;
    int64_t quantity = 0;
};

// asks[0]=ASKP1(최우선매도), asks[4]=ASKP5
// bids[0]=BIDP1(최우선매수), bids[4]=BIDP5
struct OrderBook {
    std::string    ticker;
    std::string    time;        // "HHMMSS"
    OrderBookLevel asks[5];
    OrderBookLevel bids[5];
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 체결 (KIS WebSocket H0STCNT0)
// ─────────────────────────────────────────────────────────────────────────────
struct TradeData {
    std::string ticker;
    std::string time;           // "HHMMSS"
    double      price     = 0.0;
    int64_t     quantity  = 0;  // 체결량
    int         direction = 0;  // 1=매수체결, 5=매도체결
    std::chrono::system_clock::time_point timestamp;
};
