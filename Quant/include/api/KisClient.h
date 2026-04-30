#pragma once
#include "core/Types.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// KisClient  —  한국투자증권 REST API 클라이언트 (libcurl 기반)
//   Windows의 WinHTTP → Linux에서는 libcurl로 대체
//   인터페이스는 동일하게 유지
// ─────────────────────────────────────────────────────────────────────────────
struct KisConfig {
    std::string app_key;
    std::string app_secret;
    std::string account_no;     // 계좌번호 (앞 8자리)
    std::string account_type;   // 계좌 유형 (e.g. "01")
    bool        is_paper;       // true: 모의투자, false: 실거래
};

class KisClient {
public:
    explicit KisClient(const KisConfig& cfg);
    ~KisClient();

    // 토큰 발급 (앱 시작 시 1회 호출)
    bool authenticate();

    // 일봉 데이터 조회 (최근 N개)
    std::vector<MarketData> get_daily_ohlcv(const std::string& ticker, int count);

    // 현재가 조회
    double get_current_price(const std::string& ticker);

    // 시장가 주문
    bool send_order(const OrderSignal& signal);

    // 잔고 조회
    nlohmann::json get_balance();

    bool is_authenticated() const { return !access_token_.empty(); }

private:
    // libcurl GET/POST 래퍼
    std::string http_get (const std::string& url,
                          const std::vector<std::string>& headers);
    std::string http_post(const std::string& url,
                          const std::vector<std::string>& headers,
                          const std::string& body);

    std::string base_url() const {
        return cfg_.is_paper
            ? "https://openapivts.koreainvestment.com:29443"
            : "https://openapi.koreainvestment.com:9443";
    }

    KisConfig   cfg_;
    std::string access_token_;
};
