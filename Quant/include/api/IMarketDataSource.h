#pragma once
// 시세·봉 읽기 인터페이스 — 국면 판정기·전략·스캔이 "어디서 받는지" 모른 채 시세를 읽게 한다. KisClient가 구현하고
//  테스트는 가짜 소스를 꽂는다. 주문·잔고·순위 조회는 여기 없다(주문은 IOrderExecutor). [why D-066]
#include "core/Types.h"

#include <string>
#include <vector>

// 지수 현재값 (코스피 "0001", 코스닥 "1001", KOSPI200 "2001")
struct IndexPrice
{
    std::string ticker;
    double      price       = 0.0;
    double      change      = 0.0;
    double      change_rate = 0.0;
    int         sign        = 3; // 1=상한 2=상승 3=보합 4=하한 5=하락
};

// 봉 목록은 전부 최신→과거(result[0]=최신)다. 조회 실패는 예외가 아니라 빈 목록·0 가격으로 온다.
class IMarketDataSource
{
public:
    virtual ~IMarketDataSource() = default;

    virtual double get_current_price(const std::string& ticker) = 0;

    // 일봉. include_today=false면 당일 미완성봉을 뺀 '어제까지'.
    virtual std::vector<MarketData> get_daily_ohlcv(const std::string& ticker, int count,
                                                    bool include_today = false) = 0;

    // 당일 분봉을 interval_min 분으로 묶은 봉.
    virtual std::vector<MarketData> get_minute_ohlcv(const std::string& ticker, int count, int interval_min = 3) = 0;

    // 업종·지수 일봉 (sector_code: 코스피 "0001", 업종 "0002"~"0026" 등)
    virtual std::vector<MarketData> get_index_daily_ohlcv(const std::string& sector_code, int count = 6) = 0;

    virtual IndexPrice get_index_price(const std::string& ticker) = 0;

    // 해외 일봉. exchange: "NAS"(NASDAQ), "NYS"(NYSE)
    virtual std::vector<MarketData> get_us_daily_ohlcv(const std::string& ticker, int count,
                                                       const std::string& exchange = "NAS") = 0;
};
