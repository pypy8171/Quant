#pragma once
// DevScale 슬리브의 순수 판정 —전략 본체(DeviationScaleStrategy.h)는 시계·잔고·REST에 묶여 있어
//  단위 테스트가 안 되므로, 값만 받아 답하는 부분을 여기로 뺀다. 모두 상태 없음.
//   • peak_trail_triggered: 무장 후 고가 트레일 청산 조건.
//   • add_net_quantity_from_ledger: 체결 장부(logs/trades_YYYYMMDD.csv)에서 이 슬리브의 종목별 순수량(매수 − 매도).
//   • devscale_owns_holding: 재기동 때 그 순수량으로 잔고를 DevScale이 다시 맡을지.
//   • average_true_range: 전일까지 확정 일봉의 ATR(참범위 단순평균).
//   • entry_day_allowed: 하루 단위 진입 필터 — 전일 ATR 비율·개장 이격이 허용 범위 안인가.
//   • entry_time_closed: 그날 신규 진입 마감 시각을 지났나.
//   • is_dust: 먼지 정리 대상인가(평가금 기준 + 당일 분할 진입 면제).
//   • zone_band: 일봉 SMA20 이격 존의 하단·상단(%) — 진입 폭과 유지 폭.
// 테스트: Quant/tests/test_devscale_rules.cpp
#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <istream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace devscale_rules
{

inline constexpr int kNoMarketCloseHhmm = 2400;   // market_close_hhmm이 이 값 이상이면 장 마감 청산 없음(하룻밤 넘김, D-111)

// 평단 대비 +arm_percent에 한 번이라도 닿았고(peak 기준) 현재가가 고가 대비 −trail_percent 아래면 true.
//  평단이 없으면(0) 무장 판정을 못 하므로 false. arm_percent 0은 기능 끔.
bool peak_trail_triggered(double peak, double average, double current, double arm_percent, double trail_percent);

// 장부 CSV(ts_kst,event,order_id,odno,strategy,ticker,side,type,order_qty,order_price,fill_qty,…)에서 event=FILL이고
//  strategy가 id_prefix + '_'로 시작하는 행의 fill_qty를 종목별로 net_quantity에 더한다 — 매수는 +, 매도는 −.
//  앞 11칸만 본다. 헤더·짧은 행·다른 전략(MANUAL·ITB 등) 행·접수·취소·거부 행은 건너뛴다. 여러 날 장부를 같은
//  map에 이어 더할 수 있게 0 이하 종목도 지우지 않는다(날을 다 더한 뒤 호출자가 거른다).
void add_net_quantity_from_ledger(std::istream& ledger, const std::string& id_prefix,
                                  std::map<std::string, long long>& net_quantity);

// 재기동 때 DevScale이 이 종목 잔고를 다시 맡는가 — 장부 순수량이 0보다 크고 잔고 이상일 때만 true.
//  순수량이 잔고보다 적으면 나머지는 수동·다른 전략 몫이다. 포지션 장부가 (계좌, 종목) 한 칸이라 DevScale과
//  청산 관리(ITB)가 수량을 나눠 들 수 없으므로, 그때는 잔고 전부를 청산 관리에 둔다.
bool devscale_owns_holding(long long net_quantity, int held_quantity);

// 최신이 앞(daily[0]=전일)인 일봉으로 period일 ATR — 참범위 = max(고-저, |고-전일종가|, |저-전일종가|)의 단순평균.
//  전일 종가가 필요해 period+1개가 없으면 0(호출자가 0을 "판정 불가"로 다룬다).
double average_true_range(const std::vector<MarketData>& daily, int period);

// 하루 단위 진입 필터. atr_percent = 전일 ATR14 / 전일 SMA20 × 100, open_deviation_percent = 개장 봉 종가의 전일 SMA20 이격(%).
//  atr_max_percent 0은 ATR 축 끔. 1년 리플레이(09-21)에서 ATR≤5%·이격≥−3%가 비용 전 +0.10 → +0.26%/건. [why D-111]
bool entry_day_allowed(double atr_percent, double open_deviation_percent, double atr_max_percent,
                              double open_deviation_min_percent, double open_deviation_max_percent);

// no_new_entry_hhmm(KST HHMM) 이후면 true — 새 베이스와 매수 분할 단계를 깔지 않는다. 0 이하는 끔.
//  hhmm은 같은 날 안에서만 비교한다(DevScale은 자정을 넘겨 돌지 않는다).
bool entry_time_closed(int hhmm, int no_new_entry_hhmm);

// 보유 평가금(position × current_price)이 dust_krw 아래면 먼지다. 단 오늘 이 전략이 무포지션에서 진입했고
//  (entered_today) 아직 줄인 적이 없으면(position >= peak_position) 분할 매수 진행 중이라 먼지로 보지 않는다 —
//  첫 회차 체결액(약 12만원)이 dust_krw(25만원) 아래라 사고 10~30초 뒤 되팔던 문제(09-29 신규 11건 중 8건).
//  익절로 줄어든 잔량과 전날부터 넘어온 자투리는 그대로 먼지다. dust_krw 0 이하는 끔. [why D-081]
bool is_dust(int position, double current_price, double dust_krw, bool entered_today, int peak_position);

struct ZoneBand
{
    double low_percent = 0.0; // SMA20 대비 하단(음수)
    double up_percent  = 0.0; // SMA20 대비 상단
};

// 진입 폭은 −pullback ~ +entry_upper, 유지 폭은 양쪽에 hysteresis를 더한다. widened는 직전 판정이 존 안이었거나
//  보유가 있을 때 true다 — 보유를 넣지 않으면 재기동 직후(직전 판정 기억 없음) 이격 5~9%인 보유분이 좁은 폭으로
//  판정돼 시장가로 팔린다(09-28~30 5건).
ZoneBand zone_band(double entry_upper_percent, double pullback_percent, double hysteresis_percent, bool widened);

} // namespace devscale_rules
