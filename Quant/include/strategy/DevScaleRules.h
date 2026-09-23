#pragma once
// DevScale 슬리브의 순수 판정 네 개 —전략 본체(DeviationScaleStrategy.h)는 시계·잔고·REST에 묶여 있어
//  단위 테스트가 안 되므로, 값만 받아 답하는 부분을 여기로 뺀다. 넷 다 상태 없음.
//   • peak_trail_triggered: 무장 후 고가 트레일 청산 조건.
//   • tickers_bought_from_ledger: 체결 원장(logs/trades_YYYYMMDD.csv)에서 이 슬리브가 산 종목 집합.
//   • average_true_range: 전일까지 확정 일봉의 ATR(참범위 단순평균).
//   • entry_day_allowed: 하루 단위 진입 필터 — 전일 ATR 비율·개장 이격이 허용 범위 안인가.
// 테스트: Quant/tests/test_devscale_rules.cpp
#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <istream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace devscale_rules
{

inline constexpr int kNoMarketCloseHhmm = 2400;   // market_close_hhmm이 이 값 이상이면 장 마감 청산 없음(하룻밤 넘김, D-111)

// 평단 대비 +arm_percent에 한 번이라도 닿았고(peak 기준) 현재가가 고가 대비 −trail_percent 아래면 true.
//  평단이 없으면(0) 무장 판정을 못 하므로 false. arm_percent 0은 기능 끔.
bool peak_trail_triggered(double peak, double average, double current, double arm_percent, double trail_percent);

// 원장 CSV(ts_kst,event,order_id,odno,strategy,ticker,side,…)에서 event=FILL·side=BUY·strategy가 id_prefix + '_'로
//  시작하는 행의 ticker. 앞 7칸만 본다. 헤더·짧은 행·다른 슬리브 행은 건너뛴다.
std::set<std::string> tickers_bought_from_ledger(std::istream& ledger, const std::string& id_prefix);

// 최신이 앞(daily[0]=전일)인 일봉으로 period일 ATR — 참범위 = max(고-저, |고-전일종가|, |저-전일종가|)의 단순평균.
//  전일 종가가 필요해 period+1개가 없으면 0(호출자가 0을 "판정 불가"로 다룬다).
double average_true_range(const std::vector<MarketData>& daily, int period);

// 하루 단위 진입 필터. atr_percent = 전일 ATR14 / 전일 SMA20 × 100, open_deviation_percent = 개장 봉 종가의 전일 SMA20 이격(%).
//  atr_max_percent 0은 ATR 축 끔. 1년 리플레이(09-21)에서 ATR≤5%·이격≥−3%가 비용 전 +0.10 → +0.26%/건. [why D-111]
bool entry_day_allowed(double atr_percent, double open_deviation_percent, double atr_max_percent,
                              double open_deviation_min_percent, double open_deviation_max_percent);

} // namespace devscale_rules
