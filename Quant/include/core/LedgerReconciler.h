#pragma once
// 브로커 잔고(REST inquire-balance) → 원장(OrderGate) 대조기. 기동 시드(bootstrap)·주기 대조(reconcile)·
//  당일 손익 기준선(파일 영속)·잔고조회 서킷브레이커를 한 단위로 든다. Engine의 data_thread가 부르고
//  start()의 단일스레드 구간에서 bootstrap을 한 번 부른다 — 동기화는 없다.
//  브로커 호출·대조 행 기록·종목명 등록은 std::function으로 받아 KIS 없이 시험한다. [why D-061]
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/ReconcilePlan.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace ledger
{
// 잔고에 없는 원장 보유를 걷어내기 전에 두는 유예(초). 잔고 조회 왕복(수 초)보다 넉넉히 길게
//  잡아, 방금 체결된 신규 보유가 아직 잔고에 안 보이는 것을 유령으로 오인하지 않게 한다.
inline constexpr int kPrunePositionAgeSec = 90;

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
    bool take_skip()
    {
        if (skip_remaining_ <= 0)
        {
            return false;
        }

        --skip_remaining_;
        return true;
    }

    // responded = 잔고 응답을 실제로 파싱했는가.
    BreakerOutcome on_result(bool responded)
    {
        BreakerOutcome out;

        if (responded)
        {
            out.log_recovered = fail_streak_ > 0;
            out.log_stale_off = fail_streak_ >= kPnlStaleStreak;
            fail_streak_      = 0;
            skip_remaining_   = 0;
            out.pnl_stale     = false;
            return out;
        }

        ++fail_streak_;
        int cap = fail_streak_ - 1;

        if (cap > 3)
        {
            cap = 3; // 백오프 상한: 2^3 = 8 사이클
        }

        skip_remaining_  = 1 << cap;
        out.skip_cycles  = skip_remaining_;
        out.log_backoff  = true;
        out.log_stale_on = fail_streak_ == kPnlStaleStreak;

        if (fail_streak_ >= kPnlStaleStreak)
        {
            out.pnl_stale = true;
        }

        return out;
    }

    int fail_streak() const { return fail_streak_; }
    int skip_remaining() const { return skip_remaining_; }

private:
    int fail_streak_    = 0; // 연속 실패 수(성공 시 0)
    int skip_remaining_ = 0; // 남은 스킵 사이클 수(>0이면 조회 생략)
};

// UTC 초 → KST 거래일 YYYYMMDD. 손익 기준선 파일과 날짜별 표식 파일이 같은 기준을 쓴다.
inline std::string kst_ymd(std::time_t now_utc)
{
    constexpr int kKstOffsetSec = 9 * 3600;
    std::time_t   kt = now_utc + kKstOffsetSec;
    struct tm     ktm{};
#ifdef _WIN32
    gmtime_s(&ktm, &kt);
#else
    gmtime_r(&kt, &ktm);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &ktm);
    return std::string(buf);
}

// 기준선 파일명. 계좌번호를 넣어 같은 거래일에 계좌를 갈아끼면(모의계좌 재발급 등) 옛 계좌 기준선을
//  재사용해 당일손익이 오염되는 것을 막는다(계좌 바뀌면 새로 캡처).
inline std::string baseline_file_name(const std::string& ymd, const std::string& account)
{
    std::string name = "pnl_baseline_" + ymd;

    if (!account.empty())
    {
        name += "_" + account;
    }

    return name + ".txt";
}
} // namespace ledger

class LedgerReconciler
{
public:
    using FetchBalance  = std::function<KisResult<AccountBalance>()>;
    using NameSink      = std::function<void(const std::string& ticker, const std::string& name)>;
    using ReconcileSink = std::function<void(const reconcile::Row&)>;

    LedgerReconciler(OrderGate& gate, FetchBalance fetch);

    void set_name_sink(NameSink s) { name_sink_ = std::move(s); }
    void set_reconcile_sink(ReconcileSink s) { reconcile_sink_ = std::move(s); }
    void set_account_no(std::string acct) { account_no_ = std::move(acct); }
    void set_baseline_dir(std::filesystem::path dir) { baseline_dir_ = std::move(dir); }
    void set_prune_age_sec(int s) { prune_age_sec_ = s; }

    // G5: 잔고 보유 행(ticker/qty/avg_price/주문가능)을 OrderGate.seed_position으로 시드. 실패=false → 기동 중단.
    //  기동 직후는 유령주문 취소·유니버스 스캔과 같은 초 안에 겹쳐 한도(초당 5건)에 자주 걸리므로
    //  attempts번 retry_delay 간격으로 다시 묻는다. 끝내 못 읽으면 빈 원장으로 매매하지 않는다(09-11 09:17 사례).
    bool bootstrap(int attempts = 5, std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1500));

    // 주기 대조. resync_positions=true(폴링 모드)면 미체결 선점(reserved_)을 비우고 실보유로 원장을 덮어쓴다.
    //  체결통보가 오는 WS 모드에서는 원장이 이미 체결로 갱신되고 reserved_에는 살아 있는 지정가 주문이
    //  잡혀 있으므로 false로 불러 총평가금·일손익·매도가능수량만 갱신한다. now_utc는 기준선 파일 날짜용.
    void reconcile(bool resync_positions, std::time_t now_utc);

    // 새 거래일 — 총평가금 기준선을 다음 대조에서 다시 캡처한다.
    void new_trading_day() { have_baseline_ = false; }

    bool   has_baseline() const { return have_baseline_; }
    double baseline() const { return baseline_; }
    const ledger::ReconcileBreaker& breaker() const { return breaker_; }

private:
    void resync_holdings(const AccountBalance& bal, bool resync_positions);
    void capture_baseline(double tot_eval, std::time_t now_utc);

    OrderGate&    gate_;
    FetchBalance  fetch_;
    NameSink      name_sink_;
    ReconcileSink reconcile_sink_;
    std::string   account_no_;
    std::filesystem::path baseline_dir_;      // 비어 있으면 기준선을 영속하지 않는다(시험용)
    int    prune_age_sec_ = ledger::kPrunePositionAgeSec;
    bool   have_baseline_ = false;
    double baseline_      = 0.0;              // 당일 첫 대조 시 캡처한 총평가금(원) — 손실컷 세션 앵커
    ledger::ReconcileBreaker breaker_;
};
