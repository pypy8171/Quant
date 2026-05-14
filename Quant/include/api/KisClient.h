#pragma once
#include "core/Types.h"
#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

struct KisConfig {
    std::string app_key;
    std::string app_secret;
    std::string account_no;
    std::string account_type;  // "01"
    bool        is_paper = false;
};

class KisClient {
public:
    explicit KisClient(const KisConfig& cfg);
    ~KisClient();

    bool authenticate();
    bool is_authenticated() const { return !access_token_.empty(); }

    // ── 국내 (KR) ──────────────────────────────────────────────────────────
    std::vector<MarketData> get_daily_ohlcv(const std::string& ticker, int count);
    double                  get_current_price(const std::string& ticker);
    Fundamentals            get_fundamentals(const std::string& ticker);
    bool                    send_order(const OrderSignal& signal);
    nlohmann::json          get_balance();

    // 시가총액 순위 — 현재가·등락률 포함 전체 데이터
    struct RankingStock {
        int         rank        = 0;
        std::string ticker;
        std::string name;
        double      price       = 0.0;
        double      change      = 0.0;   // 전일 대비
        double      change_rate = 0.0;   // 등락률(%)
        int64_t     volume      = 0;     // 누적 거래량
        double      pbr         = 0.0;
        double      per         = 0.0;
    };
    std::vector<RankingStock> fetch_kr_ranking(int count = 200,
                                               const std::string& market_div = "J");

    // 전체 시장 PBR 기반 Universe 조회 (ticker만 반환)
    std::vector<std::string> fetch_universe_by_pbr(double max_pbr,
                                                    const std::string& market_div = "J");

    // ── 해외 (US) ──────────────────────────────────────────────────────────
    // exchange: "NAS"(NASDAQ), "NYS"(NYSE)
    std::vector<MarketData> get_us_daily_ohlcv(const std::string& ticker,
                                                int count,
                                                const std::string& exchange = "NAS");
    Fundamentals            get_us_fundamentals(const std::string& ticker,
                                                const std::string& exchange = "NAS");
    bool                    send_us_order(const OrderSignal& signal);

    // S&P 500 주요 종목 내장 리스트 — PBR 필터 적용 후 반환
    std::vector<std::string> fetch_us_universe_by_pbr(double max_pbr,
                                                       const std::string& exchange = "NAS");

private:
    std::string http_get (const std::string& url,
                          const std::vector<std::string>& headers);
    std::string http_post(const std::string& url,
                          const std::vector<std::string>& headers,
                          const std::string& body);

    // 토큰 만료 5분 전이면 자동 재발급
    void ensure_authenticated();

    std::string base_url() const {
        return cfg_.is_paper
            ? "https://openapivts.koreainvestment.com:29443"
            : "https://openapi.koreainvestment.com:9443";
    }

    KisConfig   cfg_;
    std::string access_token_;
    std::chrono::system_clock::time_point token_expires_at_;
};
