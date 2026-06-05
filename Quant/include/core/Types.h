#pragma once
#include <chrono>
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// 시장 구분
// ─────────────────────────────────────────────────────────────────────────────
enum class Market
{
    KR,
    US
};

// WebSocket 구독 스펙 — Engine이 전략에서 수집해 WS에 전달
struct WatchSpec
{
    std::string ticker;
    Market market = Market::KR;
    std::string exchange;      // US only: "NAS", "NYS"
    bool trade_only = false;   // true: H0STCNT0만 구독 (호가 제외, 구독 한도 절약)
};

// ─────────────────────────────────────────────────────────────────────────────
// 시세 데이터 (KIS API → 수신 스레드 → RingBuffer → 전략)
// ─────────────────────────────────────────────────────────────────────────────
struct MarketData
{
    std::string ticker;
    double close = 0.0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    int64_t volume = 0;
    Market market = Market::KR;
    std::chrono::system_clock::time_point timestamp; // 수신 시각 (KIS REST 응답 처리 시점, 거래소 체결 시각과 다를 수 있음)
    int bar_index = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 신호 (전략 → RingBuffer → 주문 실행 스레드)
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderSide
{
    BUY,
    SELL,
    NONE
};
enum class OrderType
{
    MARKET,
    LIMIT
};

struct OrderSignal
{
    std::string ticker;
    OrderSide side = OrderSide::NONE;
    OrderType type = OrderType::MARKET;
    int quantity = 0;
    double price = 0.0;
    std::string strategy_id;
    Market market = Market::KR;
    std::string exchange; // US only: "NAS", "NYS"
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 포지션 관리
// ─────────────────────────────────────────────────────────────────────────────
struct Position
{
    std::string ticker;
    int quantity = 0;
    double avg_price = 0.0;
    double unrealized_pnl = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 호가 (KIS WebSocket H0STASP0) — 국내 전용
// ─────────────────────────────────────────────────────────────────────────────
struct OrderBookLevel
{
    double price = 0.0;
    int64_t quantity = 0;
};

struct OrderBook
{
    std::string ticker;
    std::string time;
    OrderBookLevel asks[5];
    OrderBookLevel bids[5];
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 체결 (H0STCNT0 국내 / HDFSCNT0 해외)
// ─────────────────────────────────────────────────────────────────────────────
struct TradeData
{
    std::string ticker;
    std::string time;
    double price = 0.0;
    int64_t quantity = 0;
    int direction = 0; // 1=매수, 5=매도
    Market market = Market::KR;
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 체결통보 (H0STCNI0 실거래 / H0STCNI9 모의투자)
// ─────────────────────────────────────────────────────────────────────────────
struct FillNotification
{
    std::string odno;                              // KIS 주문번호 (ODNO)
    std::string ticker;                            // 단축종목코드
    OrderSide   side        = OrderSide::NONE;
    int         filled_qty  = 0;                   // 체결수량 (CNTG_QTY)
    double      filled_price = 0.0;                // 체결단가 (CNTG_UNPR)
    std::string fill_time;                         // 체결시간 HHMMSS
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 상태 머신 (FEP 레이어)
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderStatus
{
    PENDING,    // OrderGate 검증 대기
    SUBMITTED,  // KIS API 전송 완료, 거래소 접수 대기
    ACCEPTED,   // KIS rt_cd=="0" 접수 성공
    REJECTED,   // OrderGate 거부 또는 KIS 오류
    FILLED,     // 체결 확인 (WebSocket 또는 조회)
    CANCELLED   // 취소
};

struct ManagedOrder
{
    std::string   order_id;       // 내부 순번 ID  "ORD-000001"
    std::string   kis_order_no;   // KIS 접수번호  ODNO
    OrderSignal   signal;
    OrderStatus   status{OrderStatus::PENDING};
    std::string   reject_reason;
    int           confirmed_qty = 0; // 누적 체결 수량 (부분체결 추적)
    std::chrono::system_clock::time_point submitted_at;
    std::chrono::system_clock::time_point updated_at;
};

// ─────────────────────────────────────────────────────────────────────────────
// 투자자별 매매동향 일자별 시계열 (KIS inquire-investor, FHKST01010900)
// 부호 규약: 양수=순매수, 음수=순매도
// 주의: 당일 데이터는 장 종료 후 제공 — on_start에서 조회 시 자연히 전일까지만 유효
// ─────────────────────────────────────────────────────────────────────────────
struct InvestorFlow
{
    std::string date;             // "YYYYMMDD" (stck_bsop_date)
    int64_t     foreign_net = 0;  // 외국인 순매수 수량 (frgn_ntby_qty)
    int64_t     inst_net    = 0;  // 기관 순매수 수량   (orgn_ntby_qty)
    int64_t     indiv_net   = 0;  // 개인 순매수 수량   (prsn_ntby_qty)
    double      close       = 0.0;// 해당일 종가 (stck_clpr)
};

// ─────────────────────────────────────────────────────────────────────────────
// 종목 펀더멘털 + 현재가/호가 (KIS inquire-price 응답 output1)
// ─────────────────────────────────────────────────────────────────────────────
struct Fundamentals
{
    std::string ticker;
    double pbr = 0.0;
    double per = 0.0;
    double last = 0.0; // 현재가
    double open = 0.0; // 시가
    double high = 0.0; // 고가
    double low = 0.0;  // 저가
    double pbid = 0.0; // 매수호가
    double pask = 0.0; // 매도호가
    int64_t vbid = 0;  // 매수잔량
    int64_t vask = 0;  // 매도잔량
    double diff = 0.0;         // 전일 대비
    double rate = 0.0;         // 등락율(%)
    double market_cap = 0.0;   // 시가총액 (억원)
};

// ─────────────────────────────────────────────────────────────────────────────
// 시장 국면 (RegimeController, 장 시작 1회 판정)
// 개별 지표를 분해 저장 — 학습 입력 + 학습 설명 + 디버깅
// ─────────────────────────────────────────────────────────────────────────────
enum class Regime
{
    BULL,
    NEUTRAL,
    BEAR,
    UNKNOWN
};

struct RegimeSnapshot
{
    std::string date;            // "YYYYMMDD" — 국면이 적용되는 거래일(KST)
    Regime      regime = Regime::UNKNOWN;
    int         score  = 0;      // v0: -2..+2
    bool        above_ma200  = false;  // 지수 종가 > 200일선
    bool        aligned_bull  = false; // ma20 > ma60 > ma120
    bool        aligned_bear  = false; // ma20 < ma60 < ma120
    double      index_close = 0.0;
    double      ma200 = 0.0, ma20 = 0.0, ma60 = 0.0, ma120 = 0.0;
    std::chrono::system_clock::time_point timestamp;
};
