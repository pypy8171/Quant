#pragma once
// 급등 뒤 되돌림 보유 슬리브(SURGE_HOLD)의 계획 파일 파싱과 판정 — 엔진 없이 시험하는 순수 조각.
//  파일은 research/studies/35_surge_box_breakout/daily_watch.py --emit-plan 이 17:00에 쓴다. 계약(정본 docs/DECISIONS.md D-157):
//   { "schema": 1, "generated_at": "...", "as_of": "YYYY-MM-DD"(신호일), "rule": "c1", "count": N,
//     "rows": [ {"ticker","name","rule","action": "BUY_OPEN","signal_date","d0","stop_basis_low","stop_pct","take_pct",
//                "horizon_days","reference_close","turnover_share"},
//               {"ticker","rule","action": "HOLD|EXIT_CLOSE","entry_date","held_days","horizon_days"} ... ] }
//  BUY_OPEN 은 다음 거래일 장 시작 동시호가 매수, EXIT_CLOSE 는 그날 종가 동시호가 매도(120거래일째), HOLD 는 기록용.
//  BUY_OPEN 은 turnover_share 큰 순으로 적혀 있고, 집행도 그 순서다(상한을 넘으면 앞에서부터 산다).
//  가격은 모두 수정 전 실제 가격(원)이다.
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace surge
{
enum class Action
{
    BuyOpen,
    Hold,
    ExitClose,
};

struct PlanRow
{
    std::string ticker;
    std::string name;
    Action      action          = Action::Hold;
    std::string signal_date;            // YYYY-MM-DD, BUY_OPEN 만
    std::string d0;                     // YYYY-MM-DD, 급등일. 같은 (종목, d0) 신호는 한 번만 산다
    double      stop_basis_low  = 0.0;  // 관찰 중 최저 저가(원)
    double      stop_percent    = 0.0;  // 손절 = 관찰 최저 저가 × (1 − 이 값/100)
    double      take_percent    = 0.0;  // 익절 = 실제 매수가 × (1 + 이 값/100)
    double      reference_close = 0.0;  // 신호일 종가(원) — 수량 계산과 시장가 명목 상한에 쓴다
    double      turnover_share  = 0.0;  // 급등 묶음 최대 거래대금 ÷ 시총(%) — 후보가 상한을 넘을 때 큰 순으로 고른다
    int         held_days       = 0;    // HOLD·EXIT_CLOSE: 다음 거래일 포함 보유 거래일 수
};

struct Plan
{
    int                  schema = 0;
    std::string          as_of;        // YYYY-MM-DD
    std::string          generated_at;
    std::string          rule;
    std::vector<PlanRow> rows;
};

// 검증 실패면 nullopt, reason에 이유. 행 하나라도 틀리면 파일 전체를 버린다(반쪽 계획으로 사지 않는다).
std::optional<Plan> parse_plan(const nlohmann::json& document, std::string& reason);

// 계획이 오늘 쓸 것인가 — 신호일(as_of)이 오늘보다 앞이고 달력으로 5일 안(주말·연휴 하루를 넘긴다).
//  날짜는 YYYYMMDD 숫자.
bool plan_fresh(int as_of_yyyymmdd, int today_yyyymmdd);

// "YYYY-MM-DD" 또는 "YYYYMMDD" → YYYYMMDD 숫자. 틀리면 0.
int date_number(const std::string& text);

// [formula] 손절가 = 관찰 최저 저가 × (1 − p). 매수가와 무관하다 — 시가가 손절선 아래로 갭 하락해 체결되면
//  전략이 09:00 뒤 바로 판다. 백테스트(close_depth.path_exits)는 그날 시가 × 0.999 근처 손절로 친다. [why D-157]
double stop_price(double stop_basis_low, double stop_percent);
double take_price(double entry_price, double take_percent);

enum class Exit
{
    None,
    Stop,
    Take,
};

// 체결 가격 하나로 본 청산 판정. 둘 다 닿을 수는 없다(손절가 < 매수가 < 익절가).
Exit exit_check(double price, double stop, double take);

enum class Skip
{
    None,
    RiskOff,       // 국면 게이트가 이 전략을 껐다(RISK_OFF)
    EntryHalted,   // 계좌 신규매수 차단(일일 손실 한도 등)
    Overlap,       // 다른 슬리브가 이미 보유
    Excluded,      // 바스켓 소유 종목
    MaxPositions,  // 보유 + 오늘 매수 대기가 상한
    MaxDailyBuys,  // 오늘 낸 매수가 상한
    NoPrice,       // 기준가 없음 또는 금액으로 1주도 못 삼
    SameSignal,    // 같은 (종목, 급등일) 신호를 이미 샀다(보유 중이거나 청산 기록에 있다)
};

struct EntryCheck
{
    bool active           = true;
    bool entry_halted     = false;
    bool excluded         = false;
    bool same_signal      = false;
    int  account_position = 0; // 장부의 이 종목 보유(계좌 전체)
    int  account_reserved = 0; // 장부의 이 종목 미체결 선점 순값(매수 − 매도). 양수면 다른 슬리브가 사는 중
    int  open_positions   = 0; // 이 슬리브 보유 + 매수 대기
    int  max_positions    = 0;
    int  bought_today     = 0;
    int  max_daily_buys   = 0;
    int  quantity         = 0;
};

Skip entry_skip(const EntryCheck& check);

const char* skip_text(Skip skip);

// 금액으로 살 수 있는 주 수(내림). 기준가가 0 이하면 0.
int buy_quantity(double amount_krw, double reference_price);

// from 다음 날부터 to 까지(포함) 평일 수. 휴장일은 모른다 — 만기 백스톱이 여유를 두고 쓴다. 날짜가 틀리면 0.
int weekdays_between(int from_yyyymmdd, int to_yyyymmdd);
} // namespace surge
