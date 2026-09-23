#pragma once
#include "core/StrategyTable.h"
#include "core/SymbolTable.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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
    // true: 국내 선물 채널(H0IFCNT0 체결·H0IFASP0 호가)로 구독. market은 KR로 두되
    // 이 플래그로 선물 transaction_id를 고른다(주문/엔진 경로의 Market enum은 건드리지 않음).
    bool is_future = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// 시세 데이터 (KIS API → 수신 스레드 → RingBuffer → 전략)
// ─────────────────────────────────────────────────────────────────────────────
struct MarketData
{
    symbol::Ticker ticker; // 고정 배열 — 링을 memcpy로 지난다. 문자열은 ticker.string(). [why D-071]
    double close = 0.0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    int64_t volume = 0;
    Market market = Market::KR;
    std::chrono::system_clock::time_point timestamp; // 수신 시각 (KIS REST 응답 처리 시점, 거래소 체결 시각과 다를 수 있음)
    int bar_index = 0;
    symbol::SymbolId symbol_id = symbol::kNone; // 데이터 스레드가 SymbolTable로 찍는다. 0이면 배선이 빠진 경로. [why D-071]
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 신호 (전략 → RingBuffer → 주문 실행 스레드)
// ─────────────────────────────────────────────────────────────────────────────
// 주문 매수/매도 — enum class는 멤버 함수를 못 가져 문자열 매칭을 못 둔다. StrategyType과 같은
//  스마트enum idiom(Value 감싸기)으로 OrderSide::from_string(파싱)을 붙인다. [why D-071]
class OrderSide
{
public:
    enum Value
    {
        BUY,
        SELL,
        NONE
    };

    OrderSide() = default;
    constexpr OrderSide(Value value) : value_(value) {}
    constexpr operator Value() const { return value_; }

    // 체결·주문 로그의 "BUY"/"SELL" 문자열 → OrderSide. SELL이 아니면 BUY로 본다
    //  (기존 (s=="SELL")?SELL:BUY 관례 유지 — 오탈자·미지정도 BUY).
    static OrderSide from_string(const std::string& text)
    {
        return text == "SELL" ? OrderSide(SELL) : OrderSide(BUY);
    }

private:
    Value value_ = NONE;
};
enum class OrderType
{
    MARKET,
    LIMIT
};

// 주문 생명주기 액션 (MM-1) — 기본 NEW로 기존 전략 무변경.
//   NEW     : 신규 주문 (기존 경로)
//   CANCEL  : original_client_order_id 대상 미체결 취소 (order-rvsecncl, 취소)
//   REPLACE : original_client_order_id 대상 정정 (order-rvsecncl, 정정 — cancel-replace 단일 콜)
enum class OrderAction
{
    NEW,
    CANCEL,
    REPLACE
};

struct OrderSignal
{
    std::string ticker;
    // 종목 id. 전략 스레드가 신호를 큐에 넣기 전에 ticker로 찍는다(emit_from). 0이면 배선이 빠진 경로.
    //  남은 문자열(ticker·strategy_id·client_order_id·reason)은 신호가 틱보다 훨씬 드물고 KIS 전문·원장 CSV가
    //  문자열을 요구해 그대로 둔다 — 링 복사 비용은 test_strategy_router 5번이 잰다. [why D-071]
    symbol::SymbolId symbol_id = symbol::kNone;
    OrderSide side = OrderSide::NONE;
    OrderType type = OrderType::MARKET;
    int quantity = 0;
    double price = 0.0;
    // 시장가(price=0) 주문의 명목 한도 평가용 참조가(직전 현재가/최우선호가). 기본 0=미지정.
    // 지정가는 price로 명목을 평가하지만 시장가는 price가 0이라, 이 값이 없으면 명목 백스톱이
    // 우회된다(특히 급락장 강제청산의 시장가 전량매도). 발주 측이 마지막 체결가를 stamp한다.
    double reference_price = 0.0;
    std::string strategy_id; // 로그·원장 CSV·ZMQ용 이름. 키로는 쓰지 않는다 — 아래 strategy_index가 키다.
    // 전략 번호(StrategyTable). 엔진이 전략 등록 때 매기고 emit에서 찍는다. 게이트 서브원장·중복 신호 키는 이 번호로
    //  찾는다 — 신호마다 "계좌:전략:종목:방향" 문자열을 만들어 해시하던 것을 정수 4개로 바꿨다. [why D-112]
    strategy_table::StrategyId strategy_index = strategy_table::kNone;
    Market market = Market::KR;
    std::string exchange; // US only: "NAS", "NYS"
    std::chrono::system_clock::time_point timestamp;
    std::string account_id; // 법인/직접시장접속(DMA, Direct Market Access) 다계좌 구분 — 계좌별 원장 분리 키 (빈값=단일 계좌)

    // ── 주문 생명주기 관리 (MM-1) — 전부 기본값, 비파괴 확장 ─────────────────
    OrderAction action = OrderAction::NEW; // 기본 NEW라 기존 전략은 이 필드를 몰라도 동일 동작
    std::string client_order_id;                // 전략이 부여하는 주문 이름 — 로그·원장 CSV·운영단말 응답용. 키가 아니다
    std::string original_client_order_id;           // CANCEL/REPLACE 대상 원주문 이름(로그용)
    // 주문 번호 — 신호를 만들 때 next_client_order_number()로 한 번 받는다. 라우터는 취소·정정 대상을 이 번호로
    //  찾는다(문자열 이름을 이력 전체와 비교하던 것을 정수 색인으로 바꿨다). 0=없음. [why D-112]
    uint64_t client_order_number          = 0;
    uint64_t original_client_order_number = 0;

    // ── 판단 근거 (G4) — 비파괴 확장, 기본 빈값 ──────────────────────────────
    // 전략이 이 신호를 낸 "이유"(충족된 지표·조건 요약). 신호와 한 레코드로 영속되어
    // 로그 타임라인 재구성 없이 "왜 샀나"를 조인 가능. reject_reason(거부사유)과 별개.
    std::string reason;

    // ── 구간 시각 (steady_clock nanoseconds, 0=안 찍음) — 틱 수신·신호 생성 시각. 주문 스레드가 pop·완료 시각을 더해
    //  logs/latency_trace.csv 한 줄로 남긴다(core/LatencyTrace.h). [why D-071]
    int64_t tick_at_ns   = 0;
    int64_t signal_at_ns = 0;

    // ── 신호 순번 (C-2) — 전략 스레드가 신호를 만들 때 단조 증가로 stamp. 0=미부여 ────────
    // 게이트 거부·라우터 접수·체결·원장 CSV(`sequence` 열)가 이 번호를 그대로 물고 가므로
    // 한 신호의 경로를 ODNO 없이도 잇는다(재기동 전 접수된 주문의 체결은 ODNO만 있어 0).
    uint64_t sequence = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 포지션 관리
// ─────────────────────────────────────────────────────────────────────────────
struct Position
{
    std::string ticker;
    int quantity = 0;
    double average_price = 0.0;
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
    symbol::Ticker   ticker;           // 고정 배열 — 링을 memcpy로 지난다. 문자열은 ticker.string(). [why D-071]
    symbol::SymbolId symbol_id = symbol::kNone; // 수신 스레드가 SymbolTable로 찍는다. 0이면 배선이 빠진 경로. [why D-071]
    int32_t       hhmmss = 0;       // KST 호가 시각 정수(093001 → 93001). 0이면 모름. 디코더가 한 번 파싱한다. [why D-071]
    OrderBookLevel asks[5];
    OrderBookLevel bids[5];
    std::chrono::system_clock::time_point timestamp;
    // 수신 스레드가 디코드 직후 찍는 steady_clock nanoseconds. 체결(TradeData.received_ns)과 같은 시계라 채널이 달라도 도착 순서를
    //  하나로 되돌릴 수 있다. 0은 "안 찍음". [why D-071]
    int64_t received_ns = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 실시간 체결 (H0STCNT0 국내 / HDFSCNT0 해외)
// ─────────────────────────────────────────────────────────────────────────────
struct TradeData
{
    symbol::Ticker   ticker;           // 고정 배열 — 링을 memcpy로 지난다. 문자열은 ticker.string(). [why D-071]
    symbol::SymbolId symbol_id = symbol::kNone; // 수신·폴러 스레드가 SymbolTable로 찍는다. 0이면 배선이 빠진 경로. [why D-071]
    int32_t       hhmmss = 0;       // KST 체결 시각 정수(093001 → 93001). 0이면 모름. 디코더가 한 번 파싱한다. [why D-071]
    double price = 0.0;
    int64_t quantity = 0;
    int direction = 0; // 1=매수, 5=매도
    Market market = Market::KR;
    std::chrono::system_clock::time_point timestamp;
    // 아래 둘은 국내 현물 체결(H0STCNT0)에만 있다. REST 폴링·선물·미국 틱은 0.
    double  strength = 0.0;   // 체결강도(CTTR, %) — 100 위면 매수 체결이 우세
    int64_t accumulated_volume = 0;  // 당일 누적 거래량
    // 수신 스레드가 디코드 직후 찍는 steady_clock nanoseconds(호가 OrderBook.received_ns와 같은 시계). 구간 지연 측정의 출발점이고
    //  0은 "안 찍음"(REST 대체 틱). [why D-071]
    int64_t received_ns = 0;
};

// [inv] 틱·호가·봉은 trivially copyable — 링 push/pop이 memcpy고 문자열 할당이 hot path에 없다. [why D-071]
static_assert(std::is_trivially_copyable_v<TradeData> && std::is_trivially_copyable_v<OrderBook> &&
              std::is_trivially_copyable_v<MarketData>);

// ─────────────────────────────────────────────────────────────────────────────
// 체결통보 (H0STCNI0 실거래 / H0STCNI9 모의투자)
// ─────────────────────────────────────────────────────────────────────────────
// [inv] 전문에 체결 건별 고유번호가 없다 — 식별자는 주문번호(ODER_NO)와 원주문번호(OODER_NO) 둘뿐이다.
//  그래서 체결 한 건을 가리키려면 라우터가 (거래일:주문번호:체결시각:수량:단가) 조합키를 만든다(ipc/FillKey.h).
struct FillNotification
{
    std::string kis_order_no;                        // KIS 주문번호 (ODER_NO)
    std::string original_order_no;                   // 원주문번호 (OODER_NO). 정정·취소면 고친 대상, 신규는 0 채움
    std::string ticker;                              // 단축종목코드
    OrderSide   side            = OrderSide::NONE;
    int         filled_quantity = 0;                 // 체결수량 (CNTG_QTY)
    double      filled_price    = 0.0;               // 체결단가 (CNTG_UNPR)
    std::string fill_time;                           // 체결시간 HHMMSS
    // 주문수량 (ODER_QTY). 전문 뒤쪽 필드라 짧은 전문에서는 0 — 0이면 "모른다"는 뜻이다.
    //  미연결 체결의 잔량 상한이 이 값이다(OrderRouter::on_fill).
    int         order_quantity  = 0;
    std::string exchange;                            // 주문거래소 구분 (ORD_EXG_GB, KRX/NXT). 짧은 전문에서는 빈 값
    std::chrono::system_clock::time_point timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 상태 머신 (주문 전처리·중계 레이어, FEP=Front-End Processor)
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

// 자릿수 문자열 → 정수. KIS 주문번호(ODNO "0000014893")·체결시각("110707")처럼 전문이 자릿수로 주는 값을
//  받는 자리에서 한 번 바꾼다. 빈 문자열이나 숫자 아닌 글자가 섞이면 0.
uint64_t digits_to_number(std::string_view digits) noexcept;

// 주문 번호 발급 — 프로세스 안에서 단조 증가. 전략·수동주문이 신호를 만들 때 한 번 부른다. [why D-112]
uint64_t next_client_order_number() noexcept;

// 주문 하나가 OrderRouter 안에서 쓴 시간(us). -1은 그 구간을 안 지났다 — 게이트 거부는 원장·전송이 없다.
// 주문 스레드가 이 값을 구간 분포에 넣는다. pop→반환을 한 덩이로 두면 게이트·이력 훑기·원장 디스크·초당한도
// 줄서기·망 왕복·파일 쓰기 중 누구 탓인지 못 가른다. gate·이력가드·원장·버킷·왕복·마무리 여섯을 더하면
// pop→반환에 거의 닿고, 나머지 칸은 그 여섯을 다시 가른 몫이다(합에 두 번 넣지 않는다). [why D-071] [wire] Quant/include/core/LatencyTrace.h PipelineLatency::add
struct OrderStageTiming
{
    int64_t gate_us          = -1; // 라우터 진입 → 게이트 판정 끝(한도 클램프·예약매도 정리·check). history_guard_us를 뺀 몫
    int64_t history_guard_us = -1; // 주문 이력 잠금·중복 가드(취소누락 보류 조회 + 같은 시장가 매도 선형 탐색)
    int64_t history_lock_wait_us = -1; // 그중 잠금을 기다린 몫. 나머지가 잠금 안에서 훑은 몫이다 [why D-126]
    int64_t journal_us       = -1; // 원장 선기록(take_intent — 디스크에 닿는다) [why D-113]
    int64_t bucket_wait_us   = -1; // 증권사 초당한도 버킷에서 줄 선 시간
    int64_t transport_us     = -1; // 증권사 REST 왕복(버킷 대기 뺀 몫)
    int64_t record_us        = -1; // 전송 뒤 마무리 — 접수 확정(원장 ACCEPT)·발행·이력 저장·원장 CSV·미결주문 파일
    // record_us를 셋으로 가른 몫. 셋을 더하면 record_us에 거의 닿는다(남는 건 구간 사이 잔돈). [why D-126]
    int64_t accept_us        = -1; // 접수 확정 — 원장 ACCEPT/REJECT 기록(드물게 청산차단 자가정리 왕복도 여기 든다)
    int64_t publish_us       = -1; // ZMQ 발행
    int64_t history_store_us = -1; // 이력 저장 — 이력 잠금·미결주문 스냅숏·파일 넘기기(open_orders_us를 품는다)
    int64_t open_orders_us   = -1; // 그중 미결주문 파일 다시쓰기 몫(record_us 안에 포함된다 — 더할 때 빼야 한다)
};

struct ManagedOrder
{
    std::string   order_id;       // 내부 순번 ID  "ORD-000001"
    std::string   kis_order_no;   // KIS 접수번호  ODNO (전문·로그용 문자열)
    uint64_t      kis_order_number = 0; // 같은 값의 정수 — 체결통보 매칭 색인 키. 0=미접수 [why D-112]
    std::string   krx_forwarding_org_no;      // KRX_FWDG_ORD_ORGNO — 정정/취소 필수 입력 (원주문 조직번호). 빈값=미보존
    OrderSignal   signal;
    OrderStatus   status{OrderStatus::PENDING};
    std::string   reject_reason;
    int           confirmed_quantity = 0; // 누적 체결 수량 (부분체결 추적)
    OrderStageTiming stages;      // 라우터 안 구간 시간 — 관측용, 매매 판단에는 안 쓴다
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
    int64_t     institution_net    = 0;  // 기관 순매수 수량   (orgn_ntby_qty)
    int64_t     individual_net   = 0;  // 개인 순매수 수량   (prsn_ntby_qty)
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
    double bid_price = 0.0; // 매수호가
    double ask_price = 0.0; // 매도호가
    int64_t bid_quantity = 0;  // 매수잔량
    int64_t ask_quantity = 0;  // 매도잔량
    double difference = 0.0;         // 전일 대비
    double rate = 0.0;         // 등락율(%)
    double market_cap = 0.0;   // 시가총액 (억원)
    double week52_high = 0.0;          // 52주 최고가(원). 0=미제공
    double week52_high_distance_percent = 0.0; // 현재가의 52주고가 대비 등락률(%, 고가 아래면 음수)
    std::string sector_name;        // 업종명(KIS bstp_kor_isnm). 업종 분산·상관 캡용
};

// ─────────────────────────────────────────────────────────────────────────────
// 시장 국면 — regime.json 라벨을 옮긴 전략 선택 입력(RISK_ON→BULL, RISK_OFF→BEAR) [why D-084]
// ─────────────────────────────────────────────────────────────────────────────
// 시장 국면 — StrategyType과 같은 스마트enum idiom. Regime::from_string으로 config
//  "active_regimes" 문자열을 파싱한다(regime.json 라벨 파싱은 RegimeFileJudge::selection_of,
//  어휘가 달라 여기 합치지 않는다).
class Regime
{
public:
    enum Value
    {
        BULL,     // 강세장
        NEUTRAL,  // 중립(방향성 약함)
        BEAR,     // 약세장
        UNKNOWN   // 판정 불가(데이터 부족 등)
    };

    Regime() = default;
    constexpr Regime(Value value) : value_(value) {}
    constexpr operator Value() const { return value_; }

    // 매칭 실패는 UNKNOWN — 호출자(parse_active_regimes)가 경고 로그로 판단한다.
    //  이름 셋을 컴파일 시점 표로 훑는다 — 해시 맵은 첫 호출에 힙을 잡고 호출마다 문자열 해시를 도는데, 항목 셋에는 비교가 더 싸다.
    static Regime from_string(std::string_view text);

private:
    Value value_ = UNKNOWN;
};

// 전략 타입 — enum class는 멤버 함수를 못 가져 문자열 매칭 로직을 이 안에 못 둔다. Value를 감싸
//  StrategyType::MA_CROSS(값)와 StrategyType::from_string(파싱)을 같은 이름 밑에 둔다(스마트enum idiom).
//  Value로의 암묵 변환이 있어 map key·switch·비교는 기존 enum class와 동일하게 쓴다.
class StrategyType
{
public:
    enum Value
    {
        UNKNOWN,
        MA_CROSS,
        INTRADAY_BREAKOUT,
        MOMENTUM,
        VALUE_CONTRARY,
        FIXED_INTERVAL,
        PRICE_TARGET,
        SUPPLY_DEMAND_PULLBACK,
        MARKET_MAKING,
        DEVIATION_SCALE,
        THEME,
        TARGET_BASKET // 목표 비중표(파일)를 원장과 맞추는 바스켓 슬리브 [why D-109]
    };

    StrategyType() = default;
    constexpr StrategyType(Value value) : value_(value) {}
    constexpr operator Value() const { return value_; }

    // config "type" 문자열 → StrategyType. 디스패치·로그 비교를 문자열이 아닌 enum값으로 하기 위함
    //  (hot path는 아니지만 오탈자 비교·string 해시를 매 로드마다 반복할 이유가 없다). 매칭 실패는 UNKNOWN.
    //  입력이 설정 파일의 문자열이라 문자열 비교 자체는 남는다. 열 항목을 컴파일 시점 표로 훑는다 — 해시 맵은 첫 호출에
    //  힙을 잡고 호출마다 해시를 도는데, 이 크기에는 비교가 더 싸고 정적 초기화 순서 문제도 없다.
    static StrategyType from_string(std::string_view text);

private:
    Value value_ = UNKNOWN;
};

// 실행 모드 — StrategyType과 같은 스마트enum idiom. main.cpp의 인자·config "mode" 파싱이 쓴다.
class Mode
{
public:
    enum Value
    {
        FEED,
        KR_TEST,
        US_TEST,
        TRADE
    };

    Mode() = default;
    constexpr Mode(Value value) : value_(value)
    {
    }

    constexpr operator Value() const
    {
        return value_;
    }

    // config/argv "mode" 문자열 → Mode. 모르는 값(과거 "TRADE" 포함)은 TRADE로 낙하 —
    //  기존 if/else 체인이 FEED/KR_TEST/US_TEST만 걸러내고 나머지를 TRADE 경로로 흘리던 것과 동일하다.
    static Mode from_string(const std::string& text);

private:
    Value value_ = TRADE;
};
