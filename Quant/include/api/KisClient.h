#pragma once
#include "api/IOrderExecutor.h"
#include "core/Types.h"
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct KisConfig
{
    std::string app_key;
    std::string app_secret;
    std::string account_no;
    std::string account_type; // "01"
    std::string hts_id;       // H0STCNI0 구독 키 (미설정 시 account_no 사용)
    bool is_paper = false;
};

class KisClient : public IOrderExecutor
{
public:
    explicit KisClient(const KisConfig& cfg);
    ~KisClient() override;

    bool authenticate();
    bool is_authenticated() const
    {
        return !access_token_.empty();
    }

    // ── 국내 (KR) ──────────────────────────────────────────────────────────
    std::vector<MarketData> get_daily_ohlcv(const std::string& ticker, int count);
    double get_current_price(const std::string& ticker);
    Fundamentals get_fundamentals(const std::string& ticker);
    bool send_order(const OrderSignal& signal);
    // FEP: 주문 제출 — KIS 접수번호(ODNO) 반환, 실패 시 빈 문자열
    std::string submit_order(const OrderSignal& signal) override;
    // MM-1: 신규 주문 + KRX 조직번호(정정/취소용) 캡처
    OrderAck submit_order_ack(const OrderSignal& signal) override;
    // MM-1: 국내 미체결 취소 (order-rvsecncl). 성공 시 취소접수 ODNO, 실패 시 ""
    std::string cancel_order(const std::string& ticker, const std::string& orig_odno,
                             const std::string& krx_orgno, int qty, bool all_remaining) override;
    // MM-1: 국내 정정 (order-rvsecncl). 성공 시 새 ODNO(정정접수번호), 실패 시 ""
    std::string revise_order(const std::string& ticker, const std::string& orig_odno,
                             const std::string& krx_orgno, int new_qty, double new_price) override;
    nlohmann::json get_balance();

    // 지수 현재값 (코스피 "0001", 코스닥 "1001", KOSPI200 "2001")
    struct IndexPrice
    {
        std::string ticker;
        double price = 0.0;
        double change = 0.0;
        double change_rate = 0.0;
        int sign = 3; // 1=상한 2=상승 3=보합 4=하한 5=하락
    };
    IndexPrice get_index_price(const std::string& ticker);

    // 시가총액 순위 — 현재가·등락률 포함 전체 데이터
    struct RankingStock
    {
        int rank = 0;
        std::string ticker;
        std::string name;
        double price = 0.0;
        double change = 0.0;      // 전일 대비
        double change_rate = 0.0; // 등락률(%)
        int64_t volume = 0;       // 누적 거래량
        double pbr = 0.0;
        double per = 0.0;
    };
    std::vector<RankingStock> fetch_kr_ranking(int count = 200, const std::string& market_div = "J");

    // 전체 시장 PBR 기반 Universe 조회 (ticker만 반환)
    std::vector<std::string> fetch_universe_by_pbr(double max_pbr, const std::string& market_div = "J");

    // 업종 지수 일봉 (sector_code: 코스피 업종 "0001"~"0026" 등)
    std::vector<MarketData> get_index_daily_ohlcv(const std::string& sector_code, int count = 6);

    // 업종별 등락률 순위 — 업종 내 상승 종목 스캔
    std::vector<RankingStock> fetch_sector_ranking(const std::string& sector_code, int count = 30);

    // 투자자별 매매동향 — 외국인·기관 순매수 확인 (단일 최신값)
    struct InvestorTrend
    {
        std::string ticker;
        int64_t foreign_net = 0; // 외국인 순매수 수량 (양수=순매수)
        int64_t inst_net    = 0; // 기관 순매수 수량
    };
    InvestorTrend get_investor_trend(const std::string& ticker);

    // 투자자별 매매동향 일자별 시계열 (수급 전략용 — 최대 30거래일)
    // flows[0] = 가장 최근 거래일, look-ahead 방지는 호출측 책임
    std::vector<InvestorFlow> get_investor_flow(const std::string& ticker,
                                                const std::string& market_div = "J");

    // ── 해외 (US) ──────────────────────────────────────────────────────────
    // exchange: "NAS"(NASDAQ), "NYS"(NYSE)
    std::vector<MarketData> get_us_daily_ohlcv(const std::string& ticker, int count,
                                               const std::string& exchange = "NAS");
    Fundamentals get_us_fundamentals(const std::string& ticker, const std::string& exchange = "NAS");
    bool send_us_order(const OrderSignal& signal);

    // S&P 500 주요 종목 내장 리스트 — PBR 필터 적용 후 반환
    std::vector<std::string> fetch_us_universe_by_pbr(double max_pbr, const std::string& exchange = "NAS");

private:
    std::string http_get(const std::string& url, const std::vector<std::string>& headers);
    std::string http_post(const std::string& url, const std::vector<std::string>& headers, const std::string& body);

    // 토큰 만료 5분 전이면 자동 재발급
    void ensure_authenticated();

    std::string base_url() const
    {
        return cfg_.is_paper ? "https://openapivts.koreainvestment.com:29443"
                             : "https://openapi.koreainvestment.com:9443";
    }

    KisConfig cfg_;
    std::string access_token_;
    std::chrono::system_clock::time_point token_expires_at_;
};
