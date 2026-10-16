#pragma once
// strategy/SwingTrendRules.h — 스윙 추세(SWING_TREND) 슬리브의 순수 판정: 계획 파일 검증, 매수 여부·수량, 확정 저점과 손절선 올리기,
//  매도 여부, 계획이 오래됐는지. 파일·로그·시각 없이 돈다(시험 tests/test_swing_trend_rules.cpp).
//  스레드: 상태가 없어 어느 스레드에서 불러도 된다. 규칙 원본은 research/studies/42_swing_trend/swing_trend.py. [why D-160]
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace swing_trend
{
// 계획 한 행. 가격은 원 단위 정수다(파이썬이 호가 단위로 내려 적는다).
struct PlanEntry
{
    std::string ticker;
    std::string name;
    int64_t     entry_price  = 0; // 지정가 매수가(되돌림 기준선)
    int64_t     cancel_below = 0; // 첫 손절선
    std::string signal_date;      // YYYY-MM-DD
};

struct Plan
{
    std::string            as_of; // YYYY-MM-DD
    std::string            generated_at;
    std::vector<PlanEntry> entries;
};

// 검증 실패면 nullopt, reason에 이유. 행 하나라도 틀리면 파일 전체를 버린다(반쪽 계획으로 사지 않는다).
std::optional<Plan> parse_plan(const nlohmann::json& document, std::string& reason);

// 매수 여부 — 그날 시가가 매수가보다 위에서 열려야 되돌림을 기다린다. 시가가 매수가 이하면 그날은 사지 않는다.
//  시가를 모르면(0 이하) 사지 않는다.
bool should_buy(double open_price, int64_t entry_price);

// [formula] 근접 발주선 = 매수가 × (1 + trigger_percent/100). 현재가가 이 선 이하로 내려오면 매수가 지정가 주문을 낸다.
//  현재가나 매수가를 모르면(0 이하) 내지 않는다.
bool near_entry(double price, int64_t entry_price, double trigger_percent);

// [formula] 취소선 = 매수가 × (1 + disarm_percent/100). 체결 0주 매수 주문은 현재가가 이 선 위로 멀어지면 취소한다.
//  현재가나 매수가를 모르면(0 이하) 취소하지 않는다.
bool far_from_entry(double price, int64_t entry_price, double disarm_percent);

// [formula] 수량 = floor(종목당 금액 / 매수가). 매수가가 0 이하면 0.
int buy_quantity(int64_t notional_krw, int64_t entry_price);

// day가 좌우 window일 저가 중 가장 낮은가(같은 값도 인정). 0 이하 저가(거래정지 봉)는 없는 값으로 건너뛰고,
//  day 자체가 0 이하면 저점이 아니다. 창이 배열 밖으로 나가는 부분은 보지 않는다(부른 쪽이 범위를 맞춘다).
bool is_extreme_low(const std::vector<int64_t>& lows, size_t day, int window);

// [formula] 손절선 = max(current_stop, 매수 뒤 확정된 저점들). lows는 오늘 앞까지 끝난 일봉(0..T−1),
//  entry_index는 매수일 봉의 위치(매수일이 오늘이면 T). 날 t(> 매수일)마다 저점 후보 t−1−window를 확정하므로
//  후보 범위는 [entry_index − window, T − 1 − window]이고, 왼쪽 창이 다 있어야 한다(후보 ≥ window).
//  손절선은 내려가지 않는다.
int64_t raised_stop(const std::vector<int64_t>& lows, size_t entry_index, int window, int64_t current_stop);

// session_dates(YYYYMMDD, 오래된 것부터)에서 entry_date 이상인 첫 봉의 위치. 없으면 size()(매수일이 오늘이다).
size_t entry_bar_index(const std::vector<int32_t>& session_dates, int entry_date);

// 매도 여부 — 종가 동시호가 창(sell_start_hhmm–sell_end_hhmm, 둘 다 포함) 안이고 현재가가 손절선 아래면 판다.
//  현재가나 손절선을 모르면(0 이하) 팔지 않는다.
bool should_sell(int hhmm, double price, int64_t stop, int sell_start_hhmm, int sell_end_hhmm);

// 계획이 오래됐는가 — as_of가 오늘보다 달력으로 max_days일 넘게 앞이면 참. as_of를 못 읽었으면(0) 참.
//  날짜는 YYYYMMDD 숫자.
bool plan_stale(int as_of_yyyymmdd, int today_yyyymmdd, int max_days);
} // namespace swing_trend
