#pragma once
#include <string>
#include <chrono>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// 시장 구분
// ─────────────────────────────────────────────────────────────────────────────
enum class Market { KR, US };

// WebSocket 구독 스펙 — Engine이 전략에서 수집해 WS에 전달
struct WatchSpec {
    std::string ticker;
    Market      market   = Market::KR;
    std::string exchange;  // US only: "NAS", "NYS"
};

// ─────────────────────────────────────────────────────────────────────────────
// 시세 데이터 (KIS API → 수신 스레드 → RingBuffer → 전략)
// ─────────────────────────────────────────────────────────────────────────────
struct MarketData {
    std::string ticker;
    double      close  = 0.0;
    double      open   = 0.0;
    double      high   = 0.0;
    double      low    = 0.0;
    int64_t     volume = 0;
    Market      market = Market::KR;
    std::chrono::system_clock::time_point timestamp;
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
    double      price       = 0.0;
    std::string strategy_id;
    Market      market      = Market::KR;
    std::string exchange;   // US only: "NAS", "NYS"
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 포지션 관리
// ─────────────────────────────────────────────────────────────────────────────
struct Position {
    std::string ticker;
    int         quantity       = 0;
    double      avg_price      = 0.0;
    double      unrealized_pnl = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 호가 (KIS WebSocket H0STASP0) — 국내 전용
// ─────────────────────────────────────────────────────────────────────────────
struct OrderBookLevel {
    double  price    = 0.0;
    int64_t quantity = 0;
};

struct OrderBook {
    std::string    ticker;
    std::string    time;
    OrderBookLevel asks[5];
    OrderBookLevel bids[5];
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 체결 (H0STCNT0 국내 / HDFSCNT0 해외)
// ─────────────────────────────────────────────────────────────────────────────
struct TradeData {
    std::string ticker;
    std::string time;
    double      price     = 0.0;
    int64_t     quantity  = 0;
    int         direction = 0;  // 1=매수, 5=매도
    Market      market    = Market::KR;
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 종목 펀더멘털 + 현재가/호가 (KIS inquire-price 응답 output1)
// ─────────────────────────────────────────────────────────────────────────────
struct Fundamentals {
    std::string ticker;
    double pbr   = 0.0;
    double per   = 0.0;
    double last  = 0.0;   // 현재가
    double open  = 0.0;   // 시가
    double high  = 0.0;   // 고가
    double low   = 0.0;   // 저가
    double pbid  = 0.0;   // 매수호가
    double pask  = 0.0;   // 매도호가
    int64_t vbid = 0;     // 매수잔량
    int64_t vask = 0;     // 매도잔량
    double diff  = 0.0;   // 전일 대비
    double rate  = 0.0;   // 등락율(%)
};
