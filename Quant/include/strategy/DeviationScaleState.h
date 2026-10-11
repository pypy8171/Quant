#pragma once
// strategy/DeviationScaleState.h — 이격 분할 전략(DeviationScaleStrategy)이 체결 사이에 이어 들고 있는 상태 묶음 셋.
//  필드의 뜻과 기본값은 묶기 전 멤버 그대로다. 관련 결정: D-081·D-111·D-115·D-156.
#include "core/Types.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deviation_scale
{
// 스냅샷은 포인터를 바꿔 넘긴다 — 평가마다 일봉 250봉(20KB)을 복사하지 않기 위해서다. 한 번 담은
//  벡터는 const라 아무도 고치지 않고, 읽는 쪽은 자기 shared_ptr로 수명을 붙잡는다. [why D-115]
using BarSnapshot = std::shared_ptr<const std::vector<MarketData>>;

// 분할 주문 재구성과 존 판정이 체결 사이에 기억하는 값. 소유 샤드 스레드만 만진다.
struct TradingState
{
    std::chrono::steady_clock::time_point last_work{};    // 스로틀
    int64_t     last_warm_log_ms = 0;                     // 봉 부족 로그 스로틀(60초)
    double      last_split_buy_reference = 0.0;           // 마지막 재구성의 분할 주문 기준점(SMA 또는 현재가) — 데드밴드 기준
    int         last_position = -1;                       // 마지막 재구성 시 포지션(데드밴드 가드)
    std::string last_split_buy_signal;                    // 마지막 발주 분할 매수 시그니처(no-change 가드)
    std::chrono::steady_clock::time_point last_rebuild{}; // 마지막 분할 매수 전면 재구성
    int         base_target_quantity = 0;                 // 베이스 분할 단계를 처음 깔 때의 목표 수량(부분체결 잔량 기준, 보유 0이면 초기화)
    int         peak_position = 0;                        // 이번 보유 구간의 최대 보유 수량(목표에 닿았으면 베이스 잔량을 더 깔지 않음)
    std::string entry_date;                               // 무포지션에서 베이스 매수를 낸 날(KST YYYYMMDD) — 먼지 정리 면제 판정, 이 프로세스 안에서만 안다
    bool        last_zone = false;                        // 마지막 존 상태(변화 로그용)
    bool        in_zone = false;                          // 존 히스테리시스 상태(진입/청산 임계 전환)
    bool        entry_closed_logged = false;              // 진입 축 닫힘 로그를 냈나(도배 방지)
    bool        entry_cutoff_logged = false;              // 신규 진입 마감 로그를 냈나(하루 한 번)
    std::chrono::steady_clock::time_point zone_log_ts{};  // 마지막 존 판정 로그 시각
    std::string entry_filter_date;                        // 진입 필터를 판정한 날(KST YYYYMMDD) — 하루 한 번
    bool        day_entry_allowed = true;                 // 그날 새 진입 허용 여부(진입 필터 판정 결과)
};

// 청산(손절·트레일·존 이탈·장 마감)이 체결 사이에 기억하는 값. 소유 샤드 스레드만 만진다.
struct LiquidationState
{
    std::chrono::steady_clock::time_point next{};                   // 청산 재시도 백오프 해제 시각
    int    last_position = -1;                                      // 직전 청산시도 position(진행 판정)
    int    fail_streak = 0;                                         // 연속 미진행 횟수(백오프 지수)
    std::chrono::steady_clock::time_point stop_cooldown_until{};    // 스탑·트레일 뒤 분할 매수 재개 시각
    std::chrono::steady_clock::time_point reentry_cooldown_until{}; // 전량 청산 뒤 새 베이스 재개 시각
    double hold_peak_price = 0.0;                                   // 이번 보유 구간의 최고 현재가(무장 후 트레일 기준, 보유 0이면 초기화)
    double last_price = 0.0;                                        // 직전 체결가 — 시장가 청산 reference_price
    double last_average_price = 0.0;                                // 잔고 평단(sellable_qty가 갱신) — reference_price 폴백
    std::chrono::steady_clock::time_point average_query_ts{};       // 평단 직접 조회 스로틀(60초)
    int    average_position_seen = 0;                               // last_average_price를 읽었을 때의 보유 수량(바뀌면 다시 읽음)
};

// 프리페치 스레드가 채우고 샤드 스레드가 읽는 REST 스냅샷. [inv] 전략의 snap_mutex_를 쥐고만 읽고 쓴다.
struct PrefetchSnapshot
{
    BarSnapshot daily;            // 일봉 스냅샷(미수신이면 null)
    std::string daily_date;       // 스냅샷 기준일(KST YYYYMMDD)
    double      equity = 0.0;     // 자본 스냅샷(raw, 폴백 미적용)
    BarSnapshot bars;             // 3분봉 스냅샷(미수신이면 null)
    int         bars_bucket = -1; // 그 스냅샷을 받은 봉 번호(kst_bar_bucket)
    uint64_t    bars_version = 0; // 받을 때마다 +1 — 소유 샤드 스레드가 새 스냅샷만 시드한다
};
} // namespace deviation_scale
