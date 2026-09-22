#pragma once
// 브로커 잔고(REST inquire-balance) → 원장(OrderGate) 대조기. 기동 시드(bootstrap)·주기 대조(reconcile)·
//  당일 손익 기준선(파일 영속)·잔고조회 서킷브레이커를 한 단위로 든다. Engine의 data_thread가 부르고
//  start()의 단일스레드 구간에서 bootstrap을 한 번 부른다 — 동기화는 없다. 잔고 조회(fetch_)만 std::async로
//  뒤 스레드에서 돌고, 그 결과를 원장에 적용하는 일은 부른 스레드가 한다.
//  브로커 호출·대조 행 기록·종목명 등록은 std::function으로 받아 KIS 없이 시험한다. [why D-061]
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/KstTime.h"
#include "core/ReconcilePlan.h"
#include "risk/OrderGate.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>

namespace ledger
{
// 잔고에 없는 원장 보유를 걷어내기 전에 두는 유예(초). 잔고 조회 왕복(수 초)보다 넉넉히 길게
//  잡아, 방금 체결된 신규 보유가 아직 잔고에 안 보이는 것을 유령으로 오인하지 않게 한다.
inline constexpr int kPrunePositionAgeSec = 90;

// 체결통보 직후 이만큼(초)은 주기 대조를 미룬다. 잔고 스냅샷은 체결보다 몇 초 늦게 따라와, 그 사이에 대조가
//  돌면 방금 산 수량의 매도가능이 0으로 덮이고(refresh_sellable) 방금 판 수량이 "놓친 매도"로 보인다. 체결이
//  이어지면 상한(Max)까지만 미루고 그 뒤엔 돈다 — 손익·총평가 갱신이 끊기면 손실컷 기준이 멈춘다. [why D-074]
inline constexpr int kPostFillDeferSec    = 5;
inline constexpr int kPostFillDeferMaxSec = 30;

// 잔고조회가 이 횟수 연속 실패하면 daily_pnl이 낡은 것으로 보고 BUY NEW를 보수 정지한다(B2).
//  2 = 단발 타임아웃(streak 1)엔 발동 않고 지속 정체만 잡는다.
inline constexpr int kPnlStaleStreak = 2;

// 한 번의 대조 결과가 바깥에 요구하는 것. pnl_stale이 비어 있으면 게이트를 건드리지 않는다.
struct BreakerOutcome
{
    std::optional<bool> pnl_stale;      // OrderGate::set_pnl_stale 호출이 필요할 때만
    int  skip_cycles      = 0;          // 실패 때 다음 조회까지 건너뛸 사이클 수(로그 문구용)
    bool log_recovered    = false;      // 실패 뒤 첫 성공
    bool log_stale_off    = false;      // 정체 창에서 복구 — pnl_stale 해제 알림
    bool log_backoff      = false;      // 실패 — 백오프 안내(매 실패)
    bool log_stale_on     = false;      // 정체 임계 진입 1회
};

// 잔고조회 서킷브레이커 — 모의/실서버 inquire-balance가 연속 타임아웃(12002)하면 GET 3회 재시도로
//  사이클당 ~60s를 태우고 데이터 스레드를 정체시킨다. 실패 누적 시 지수 백오프(1·2·4·8 사이클)로 조회
//  자체를 건너뛰어 핫루프를 보호하고, 성공 시 즉시 복귀한다. 순수 상태기계라 헤더에서 시험한다.
class ReconcileBreaker
{
public:
    // 이번 사이클을 건너뛰어야 하면 true(남은 스킵을 하나 소모한다).
    bool take_skip();

    // responded = 잔고 응답을 실제로 파싱했는가.
    BreakerOutcome on_result(bool responded);

    int fail_streak() const { return fail_streak_; }
    int skip_remaining() const { return skip_remaining_; }

private:
    int fail_streak_    = 0; // 연속 실패 수(성공 시 0)
    int skip_remaining_ = 0; // 남은 스킵 사이클 수(>0이면 조회 생략)
};

// UTC 초 → KST 거래일 YYYYMMDD. 손익 기준선 파일과 날짜별 표식 파일이 같은 기준을 쓴다.
inline std::string kst_ymd(std::time_t now_utc)
{
    return kst::date_yyyymmdd(now_utc);
}

// 기준선 파일명. 계좌번호를 넣어 같은 거래일에 계좌를 갈아끼면(모의계좌 재발급 등) 옛 계좌 기준선을
//  재사용해 당일손익이 오염되는 것을 막는다(계좌 바뀌면 새로 캡처).
std::string baseline_file_name(const std::string& date_yyyymmdd, const std::string& account);
} // namespace ledger

class LedgerReconciler
{
public:
    using FetchBalance  = std::function<KisResult<AccountBalance>()>;
    using NameSink      = std::function<void(const std::string& ticker, const std::string& name)>;
    using ReconcileSink = std::function<void(const reconcile::Row&)>;

    LedgerReconciler(OrderGate& gate, FetchBalance fetch);

    void set_name_sink(NameSink name_sink) { name_sink_ = std::move(name_sink); }
    void set_reconcile_sink(ReconcileSink reconcile_sink) { reconcile_sink_ = std::move(reconcile_sink); }
    void set_account_no(std::string account) { account_no_ = std::move(account); }
    void set_baseline_directory(std::filesystem::path directory) { baseline_directory_ = std::move(directory); }
    void set_prune_age_sec(int prune_age_sec) { prune_age_sec_ = prune_age_sec; }
    void set_post_fill_defer(int seconds, int max_sec) { post_fill_defer_sec_ = seconds; post_fill_defer_max_sec_ = max_sec; }
    // 한 사이클이 잔고 응답을 기다려 주는 상한. 넘기면 조회는 뒤에서 계속 돌고 다음 사이클이 결과를 집는다.
    void set_fetch_wait_budget(std::chrono::milliseconds budget) { fetch_wait_budget_ = budget; }

    // 체결통보 시각. 체결 소비 스레드가 부르고 reconcile(제어 스레드)이 읽는다 — 이 값만 원자적이다.
    void note_fill(std::time_t now_utc) { last_fill_utc_.store(static_cast<long long>(now_utc), std::memory_order_relaxed); }

    // G5: 잔고 보유 행(ticker/quantity/average_price/주문가능)을 OrderGate.seed_position으로 시드. 실패=false → 기동 중단.
    //  기동 직후는 유령주문 취소·유니버스 스캔과 같은 초 안에 겹쳐 한도(초당 5건)에 자주 걸리므로
    //  attempts번 retry_delay 간격으로 다시 묻는다. 끝내 못 읽으면 빈 원장으로 매매하지 않는다(09-11 09:17 사례).
    bool bootstrap(int attempts = 5, std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1500));

    // 주기 대조. resync_positions=true(폴링 모드)면 미체결 선점(reserved_)을 비우고 실보유로 원장을 덮어쓴다.
    //  체결통보가 오는 WS 모드에서는 원장이 이미 체결로 갱신되고 reserved_에는 살아 있는 지정가 주문이
    //  잡혀 있으므로 false로 불러 총평가금·일손익·매도가능수량만 갱신한다. now_utc는 기준선 파일 날짜용.
    //  잔고 조회(fetch_)는 별도 스레드에서 돌리고 이 함수는 fetch_wait_budget_만 기다린다 — 모의 서버가
    //  잔고 응답에 20~100초를 쓴 날(09-18) 이 한 호출이 데이터 사이클(재스캔·시세 보충)을 통째로 세웠다.
    //  응답이 늦으면 다음 사이클이 결과를 집어 적용한다. 원장·게이트 갱신은 여전히 부른 스레드에서만 한다.
    void reconcile(bool resync_positions, std::time_t now_utc);

    bool fetch_in_flight() const { return pending_fetch_.valid(); }

    // 이번 대조를 체결 직후라서 미루는가. reconcile이 먼저 묻고, 미뤘으면 조회를 안 한다(서킷브레이커 집계 밖).
    bool defer_after_fill(std::time_t now_utc);

    // 새 거래일 — 총평가금 기준선을 다음 대조에서 다시 캡처한다.
    void new_trading_day() { have_baseline_ = false; }

    bool   has_baseline() const { return have_baseline_; }
    double baseline() const { return baseline_; }
    const ledger::ReconcileBreaker& breaker() const { return breaker_; }

private:
    void resync_holdings(const AccountBalance& balance, bool resync_positions);
    void capture_baseline(double total_evaluation, std::optional<double> previous_day_total, std::time_t now_utc);

    OrderGate&    gate_;
    FetchBalance  fetch_;
    NameSink      name_sink_;
    ReconcileSink reconcile_sink_;
    std::string   account_no_;
    std::filesystem::path baseline_directory_;      // 비어 있으면 기준선을 영속하지 않는다(시험용)
    int    prune_age_sec_ = ledger::kPrunePositionAgeSec;
    int    post_fill_defer_sec_     = ledger::kPostFillDeferSec;
    int    post_fill_defer_max_sec_ = ledger::kPostFillDeferMaxSec;
    std::atomic<long long> last_fill_utc_{0};
    std::time_t defer_since_ = 0;             // 연속으로 미루기 시작한 시각(0=안 미루는 중)
    bool   have_baseline_ = false;
    double baseline_      = 0.0;              // 전일 총자산(없으면 첫 대조 총평가금)(원) — 손실컷 기준점
    ledger::ReconcileBreaker breaker_;
    // 진행 중인 잔고 조회. 소멸자가 결과를 기다리므로(std::async) 이 객체는 fetch_가 쓰는 클라이언트보다
    //  먼저 사라져야 한다 — Engine에서는 ledger_가 feed_ 뒤에 선언돼 먼저 소멸한다. [inv]
    std::future<KisResult<AccountBalance>> pending_fetch_;
    std::chrono::steady_clock::time_point  pending_since_{};
    int pending_cycles_ = 0;                  // 결과를 못 집고 지나간 사이클 수(로그용)
    std::chrono::milliseconds fetch_wait_budget_{500};
};
