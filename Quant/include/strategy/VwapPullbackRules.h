#pragma once
// 강한 종목 첫 VWAP 눌림(VWAPPB) 슬리브의 순수 판정 — 시계·잔고·REST 없이 값만 받아 답한다. 라이브 전략과 백테스트가
//  같은 규칙을 쓰도록 숫자 규칙은 전부 여기 둔다. 정본은 research/studies/30_strong_stock_strategies/SPEC.md 2.2~2.4절이다.
//   • select_candidates: 09:30 시세판에서 등락률 띠 안 종목을 거래대금 순으로 K개 고른다.
//   • rows_at_selection: 1분 저장 파일(board_YYYYMMDD.csv)에서 선정 시각 뒤 첫 판만 읽는다(재기동 복원용).
//   • retrace_ratio·in_vwap_band·breakout_trigger·stop_price·exit_due: 상태 기계가 쓰는 낱개 판정.
//   • PullbackMachine: 1분봉을 하나씩 받아 대기 → 무장 → 신호(또는 그날 끝)로 간다.
//   • classify_holding·own_exit_quantity: 재기동 재인수와 15:10 매도 수량.
//  스레드: 상태 없는 함수는 어디서나, PullbackMachine은 그것을 가진 전략의 샤드 스레드에서만 부른다.
//  관련 결정: D-109(슬리브 소유권), D-071(종목 단위 순서 보장).
// 테스트: Quant/tests/test_vwap_pullback_rules.cpp
#include <cstdint>
#include <istream>
#include <set>
#include <string>
#include <vector>

namespace vwap_pullback
{
// 규칙 숫자. 기본값은 SPEC.md 확정값이고, config 키 이름은 VwapPullbackLoader.cpp가 읽는다.
struct RuleParams
{
    // 2.2 선정
    int    select_hhmm             = 930;   // 이 시각(HHMM) 뒤 첫 시세판으로 고른다
    double change_min_percent      = 6.0;   // 등락률 하한(%)
    double change_max_percent      = 20.0;  // 등락률 상한(%) — 상한가(30%) 근처는 여기서 빠진다
    int    turnover_top_n          = 10;    // 거래대금 상위 K
    double min_previous_close_krw  = 2'000.0;
    double min_turnover_krw        = 3e9;   // 09:30 누적 거래대금 하한(30억)

    // 2.3 진입
    int    first_arm_hhmm          = 931;   // 이 봉부터 무장 조건을 본다
    int    no_new_entry_hhmm       = 1315;  // 무장 시각 상한
    double retrace_min             = 0.38;
    double retrace_max             = 0.62;
    double vwap_band_percent       = 0.5;   // |저가/VWAP − 1| 상한(%)
    double max_run_percent         = 25.0;  // 고점/전일종가 − 1 상한(%)
    double disarm_below_vwap_percent = 1.0; // 종가 < VWAP × (1 − 이 값%)면 그날 끝
    double disarm_retrace          = 0.75;  // 되밀림이 이 값을 넘으면 그날 끝
    int    arm_timeout_bars        = 15;    // 무장 뒤 이 봉 수 안에 신호가 없으면 그날 끝
    int    entry_ticks             = 2;     // 진입 지정가 = 종가 + 이 틱 수
    double vi_jump_percent         = 6.0;   // 한 봉의 직전 종가 대비 변동이 이 값 이상이면 VI로 보고 제외
    int    vi_zero_volume_bars     = 2;     // 거래량 0 봉이 이 수만큼 이어지면 VI로 보고 제외

    // 2.4 청산
    double stop_below_low_percent  = 0.3;   // 손절 = 눌림 저점 × (1 − 이 값%)
    double min_stop_width_percent  = 0.6;   // 손절 폭이 이보다 좁으면 진입 × (1 − 이 값%)로 넓힌다
    double max_stop_width_percent  = 3.0;   // 손절 폭이 이보다 넓으면 진입하지 않는다
    int    exit_hhmm               = 1510;  // 시간 청산
};

// 시세판 한 줄 중 선정에 쓰는 값. excluded는 호출자가 채운다(ETF·리츠·우선주·스팩·VI).
struct BoardRow
{
    std::string code;
    std::string name;
    double      price          = 0.0;
    double      value          = 0.0; // 원, 누적 거래대금
    double      volume         = 0.0; // 주, 누적 거래량
    double      change_percent = 0.0;
    bool        excluded       = false;
};

struct Selected
{
    std::string code;
    std::string name;
    double      previous_close = 0.0;
    double      change_percent = 0.0;
    double      value          = 0.0;
};

// 현재가와 등락률로 전일 종가를 되돌린다. 등락률이 −100% 이하면 0.
double previous_close(double price, double change_percent);

// 등락률이 [min, max]이고 전일 종가·거래대금 하한을 넘는 종목을 거래대금 내림차순으로 turnover_top_n개.
//  거래대금이 같으면 코드 순 — 같은 판이면 같은 집합이 나온다.
std::vector<Selected> select_candidates(const std::vector<BoardRow>& rows, const RuleParams& rules);

// board_YYYYMMDD.csv(kst_time,code,price,volume,turnover,change_pct,…)에서 kst_time이 select_hhmm:00 이상인 첫 판의
//  줄만 돌려준다. 그 뒤 판은 읽지 않는다 — 재기동이 늦어도 09:30 값으로 고른다(미래 정보 차단).
std::vector<BoardRow> rows_at_selection(std::istream& board_csv, int select_hhmm);

// 되밀림 비율 (H − low) / (H − P0). H ≤ P0이면 정의가 없어 −1.
double retrace_ratio(double session_high, double low, double previous_close_price);

// |low / vwap − 1| ≤ band_percent%.
bool in_vwap_band(double low, double vwap, double band_percent);

// 신호: 종가가 직전 봉 고가와 VWAP을 모두 넘는다.
bool breakout_trigger(double close, double previous_high, double vwap);

// 호가단위 격자로 내린다(가격 자신의 구간 호가단위).
double floor_to_tick(double price);

// 진입가 기준 손절가. 너무 넓으면(max_stop_width_percent 초과) 0 — 진입하지 않는다.
double stop_price(double pullback_low, double entry_price, const RuleParams& rules);

// 시간 청산 시각을 지났나(같은 날 HHMM 비교).
bool exit_due(int hhmm, int exit_hhmm);

// 같은 종목은 하루 한 번만 — 오늘 진입한 종목 집합에 있으면 true.
bool traded_today(const std::set<std::string>& entered_today, const std::string& ticker);

// 재기동 재인수 판정. 장부 순수량(VWAPPB_ 매수 − 매도)과 잔고로 정한다.
enum class Ownership
{
    None,    // 순수량 0 이하 — 손대지 않는다
    Full,    // 순수량 ≥ 잔고 — VWAPPB가 다시 맡는다
    Partial, // 0 < 순수량 < 잔고 — 남의 몫이 섞여 청산 관리(ITB)에 넘긴다
};

Ownership classify_holding(long long net_quantity, int held_quantity);

// 15:10 매도 수량 = min(확정 잔고, 자기 순수량). 남의 몫은 팔지 않는다.
int own_exit_quantity(int confirmed_quantity, long long own_net_quantity);

// 1분봉 하나. hhmm은 봉 시작 분(KST).
struct MinuteBar
{
    int     hhmm   = 0;
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    int64_t volume = 0;
};

enum class Phase
{
    Waiting,
    Armed,
    Done, // 그날 끝(신호를 냈거나 무장이 풀렸거나 제외)
};

enum class Event
{
    None,
    Armed,
    Disarmed,
    Signal,
};

struct StepResult
{
    Event       event = Event::None;
    std::string reason;              // Disarmed 사유(below_vwap·deep_retrace·timeout·new_high·vi·stop_too_wide·late)
    double      vwap         = 0.0;  // 이 봉까지의 VWAP
    double      session_high = 0.0;  // 이 봉까지의 고가 최댓값
    double      retrace      = 0.0;
    double      entry_price  = 0.0;  // Signal일 때 지정가
    double      stop         = 0.0;  // Signal일 때 손절가
    double      pullback_low = 0.0;  // 무장 뒤 저가 최솟값
};

// SPEC 2.3 상태 기계 — 09:00부터 1분봉을 순서대로 받는다. VWAP = Σ((고+저+종)/3 × 거래량) / Σ거래량.
//  [inv] 봉은 hhmm 오름차순으로 한 번씩만 넣는다. 같은 분이나 이른 분은 무시한다.
class PullbackMachine
{
public:
    PullbackMachine(double previous_close_price, RuleParams rules);

    StepResult on_bar(const MinuteBar& bar);

    Phase phase() const
    {
        return phase_;
    }

    int last_hhmm() const
    {
        return last_hhmm_;
    }

    double vwap() const;

private:
    StepResult disarm(StepResult result, const char* reason);

    double     previous_close_;
    RuleParams rules_;
    Phase      phase_            = Phase::Waiting;
    int        last_hhmm_        = -1;
    double     price_volume_sum_ = 0.0;
    double     volume_sum_       = 0.0;
    double     session_high_     = 0.0;
    double     armed_high_       = 0.0;
    double     pullback_low_     = 0.0;
    double     previous_high_    = 0.0;
    double     previous_close_bar_ = 0.0;
    int        bars_since_arm_   = 0;
    int        zero_volume_run_  = 0;
};
} // namespace vwap_pullback
