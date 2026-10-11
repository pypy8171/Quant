#pragma once
// DevScale 슬리브의 순수 판정 —전략 본체(DeviationScaleStrategy.h)는 시계·잔고·REST에 묶여 있어
//  단위 테스트가 안 되므로, 값만 받아 답하는 부분을 여기로 뺀다. 모두 상태 없음.
//   • peak_trail_triggered: 무장 후 고가 트레일 청산 조건.
//   • add_net_quantity_from_ledger: 체결 장부(logs/trades_YYYYMMDD.csv)에서 이 슬리브의 종목별 순수량(매수 − 매도).
//   • add_holding_from_ledger: 같은 장부를 날짜 순으로 읽어 종목별 순수량과 이번 보유를 시작한 날(매수일).
//   • judge_trend_exit: 일봉 SMA 추세 이탈 청산 — 매수 뒤 종가가 SMA 위에 한 번 선 뒤, 전일 종가가 SMA 아래면 매도.
//   • devscale_owns_holding: 재기동 때 그 순수량으로 잔고를 DevScale이 다시 맡을지.
//   • average_true_range: 전일까지 확정 일봉의 ATR(참범위 단순평균).
//   • entry_day_allowed: 하루 단위 진입 필터 — 전일 ATR 비율·개장 이격이 허용 범위 안인가.
//   • entry_time_closed: 그날 신규 진입 마감 시각을 지났나.
//   • is_dust: 먼지 정리 대상인가(평가금 기준 + 당일 분할 진입 면제).
//   • zone_band: 일봉 SMA20 이격 존의 하단·상단(%) — 진입 폭과 유지 폭.
//   • sell_room_after_cancel: 같은 처리에서 자기 매도를 취소한 뒤 다시 낼 수 있는 매도 수량.
//   • sell_cover_missing: 익절 매도가 보유를 덮지 못한 채 남아 있어 재구성을 다시 열어야 하나.
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

struct LedgerHolding
{
    long long net_quantity = 0; // 지금까지 읽은 장부의 순수량(매수 − 매도)
    int       entry_date   = 0; // 순수량이 0 이하에서 양수로 바뀐 날(YYYYMMDD) — 이번 보유를 시작한 날
};

// add_net_quantity_from_ledger와 같은 행을 읽되 매수일도 남긴다. session_date는 이 장부 파일의 날짜(YYYYMMDD)다.
//  여러 날을 오래된 날부터 차례로 넣어야 매수일이 맞다. 다 판 뒤 다시 사면 매수일이 새 날로 바뀐다.
void add_holding_from_ledger(std::istream& ledger, const std::string& id_prefix, int session_date,
                             std::map<std::string, LedgerHolding>& holdings);

struct TrendExitJudgement
{
    bool   sell    = false; // 오늘 장 시작부터 판다
    bool   armed   = false; // 매수 뒤(전일 이전) 종가가 SMA 위에 한 번이라도 섰다
    double close   = 0.0;   // 전일 종가
    double average = 0.0;   // 전일 기준 SMA(period)
};

// 최신이 앞(daily[0]=전일)인 일봉으로 추세 이탈 청산을 판정한다. 전일 종가 < 전일 SMA이고, 매수일부터 그 전날까지
//  종가가 그날 SMA 위에 선 날이 하루라도 있으면 sell이다. 매수일 당일(entry_date)도 센다. 매수 당일 장중이면
//  (daily[0]이 매수일 이전) 판정하지 않는다. 일봉에 날짜가 없거나(0) period+1개가 안 되면 sell=false.
//  2010–2026 전 종목 시험(스터디 38, 매수 130,649건)에서 SMA60이 SMA20보다 건당 +0.58%p, 칸 25개 계좌 씨앗 20개 중
//  19개에서 앞섰다. 위에 선 적 없이 바로 깨지는 것까지 팔면(arm 없음) 씨앗 17/20에서 뒤졌다. [why D-158]
TrendExitJudgement judge_trend_exit(const std::vector<MarketData>& daily, int period, int entry_date);

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

// 같은 처리에서 자기 매도 cancelled_sell_quantity주를 취소하고 곧바로 새 매도를 낼 때 쓸 수 있는 수량.
//  장부 사본의 매도가능(sellable)은 취소 답이 오기 전까지 그 매도가 잡은 수량을 빼고 보여 준다 — 그대로 쓰면
//  0이 나와 익절 매도를 건너뛰거나(10-02 실계좌 138930 31주), 취소한 몫을 뺀 나머지만 덮는다(10-02 모의 017860
//  148주를 37·111주로 번갈아 덮음). 주문 스레드가 같은 종목의 다음 주문을 앞 주문 답 뒤에 판정하므로 새 매도가
//  게이트에 닿을 때는 취소가 이미 풀려 있다. 취소가 실패해도(이미 체결) 게이트 클램프가 넘친 몫을 깎는다.
//  결과는 0 이상 position 이하. [why D-156]
int sell_room_after_cancel(int sellable, int cancelled_sell_quantity, int position);

// 직전 재구성과 계획·보유가 같아 재구성을 건너뛰려는 자리에서, 익절 매도가 보유를 덮지 못하고 있으면 true.
//  free_sellable은 장부 사본의 매도가능(자기 미체결 매도를 뺀 값)이다. 계획이 덮으려는 수량(planned_sell_quantity)이
//  남기려는 몫(position − planned)보다 더 비어 있으면 낸 매도가 거부됐거나 빠진 것이다. 막 낸 주문은 답이 오기
//  전이라 아직 장부에 안 잡히므로 grace_sec 동안은 보지 않는다. 10-02 모의 003490·016360·005930은 08:30 장전
//  매도가 세션 창 밖으로 거부된 뒤 같은 계획이라 하루 종일 다시 내지 않았다. [why D-156]
bool sell_cover_missing(int position, int planned_sell_quantity, int free_sellable, long long seconds_since_rebuild,
                        int grace_sec);

} // namespace devscale_rules
