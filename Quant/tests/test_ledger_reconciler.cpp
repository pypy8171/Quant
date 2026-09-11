// 잔고 → 원장 대조기(core/LedgerReconciler.h) 단위 테스트. 브로커는 std::function으로 대신해 KIS 없이
//  기동 시드 재시도, REST/WS 모드의 덮어쓰기·유지, 유령 정리와 빈 잔고 가드, 당일 기준선의 파일 재사용·
//  새 거래일 재캡처, 서킷브레이커 백오프와 pnl_stale 전이를 고정한다. OrderGate·Logger를 링크한다.
//  관련 결정: D-038(대조 행), D-061(분리).
// 빌드: cmake --build <dir> --target test_ledger_reconciler
#include "core/LedgerReconciler.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 2027-01-15 08:00 UTC = KST 17:00 같은 날. 기준선 파일 날짜를 고정한다.
constexpr std::time_t kT0 = 1800000000;

Holding hold(const char* t, int q, double av, std::optional<int> psbl = std::nullopt)
{
    Holding h;
    h.ticker       = t;
    h.name         = std::string("N-") + t;
    h.qty          = q;
    h.avg_price    = av;
    h.sellable_qty = psbl;
    return h;
}

KisResult<AccountBalance> ok_balance(std::vector<Holding> hs, double tot_eval, double cash = 500000.0,
                                     std::optional<double> prev = std::nullopt)
{
    AccountBalance b;
    b.holdings             = std::move(hs);
    b.total_eval_amt       = tot_eval;
    b.available_cash       = cash;
    b.prev_day_total_asset = prev;
    return KisResult<AccountBalance>::ok(std::move(b));
}

KisResult<AccountBalance> fail_balance()
{
    return KisResult<AccountBalance>::fail("EGW00201", "초당 거래건수 초과");
}

std::filesystem::path baseline_dir()
{
    return Logger::instance().base_dir() / "ledger_test";
}

void wipe_baselines()
{
    std::error_code ec;
    std::filesystem::remove_all(baseline_dir(), ec);
    std::filesystem::create_directories(baseline_dir(), ec);
}

int test_breaker()
{
    ledger::ReconcileBreaker b;
    CHECK(!b.take_skip());

    // 실패 1: 백오프 1사이클, 아직 pnl_stale 아님.
    ledger::BreakerOutcome o = b.on_result(false);
    CHECK(o.log_backoff && o.skip_cycles == 1 && !o.pnl_stale && !o.log_stale_on && b.fail_streak() == 1);
    CHECK(b.take_skip() && !b.take_skip());

    // 실패 2: 임계 진입 1회 로그 + pnl_stale=true, 백오프 2.
    o = b.on_result(false);
    CHECK(o.log_stale_on && o.pnl_stale && *o.pnl_stale && o.skip_cycles == 2);
    o = b.on_result(false);
    CHECK(!o.log_stale_on && o.pnl_stale && *o.pnl_stale && o.skip_cycles == 4);
    o = b.on_result(false);
    CHECK(o.skip_cycles == 8);
    o = b.on_result(false);
    CHECK(o.skip_cycles == 8 && b.fail_streak() == 5); // 상한 2^3

    // 복구: 즉시 스킵 0, 두 로그 모두, pnl_stale=false.
    o = b.on_result(true);
    CHECK(o.log_recovered && o.log_stale_off && o.pnl_stale && !*o.pnl_stale && b.skip_remaining() == 0 &&
          b.fail_streak() == 0);

    // 단발 실패 뒤 성공: 복구 로그는 찍되 정체 해제 로그는 없다.
    (void) b.on_result(false);
    o = b.on_result(true);
    CHECK(o.log_recovered && !o.log_stale_off);
    o = b.on_result(true);
    CHECK(!o.log_recovered && o.pnl_stale && !*o.pnl_stale);
    return 0;
}

int test_names()
{
    CHECK(ledger::kst_ymd(0) == "19700101");
    CHECK(ledger::kst_ymd(kT0) == "20270115");
    CHECK(ledger::kst_ymd(kT0 + 15 * 3600) == "20270116"); // 23:00 UTC = KST 다음날 08:00
    CHECK(ledger::baseline_file_name("20270115", "") == "pnl_baseline_20270115.txt");
    CHECK(ledger::baseline_file_name("20270115", "12345678-01") == "pnl_baseline_20270115_12345678-01.txt");
    return 0;
}

int test_bootstrap()
{
    OrderGate gate;
    int       calls = 0;
    LedgerReconciler r(gate, [&] {
        ++calls;
        return fail_balance();
    });
    CHECK(!r.bootstrap(5, std::chrono::milliseconds(0)) && calls == 5);
    CHECK(gate.snapshot_positions().empty());

    // 두 번 실패 뒤 성공 — 원장·주문가능·종목명이 시드된다.
    calls = 0;
    std::vector<std::pair<std::string, std::string>> names;
    LedgerReconciler r2(gate, [&] {
        ++calls;
        return calls < 3 ? fail_balance()
                         : ok_balance({hold("005930", 10, 70000.0, 7), hold("000660", 3, 150000.0)}, 1000000.0);
    });
    r2.set_name_sink([&](const std::string& t, const std::string& n) { names.emplace_back(t, n); });
    CHECK(r2.bootstrap(5, std::chrono::milliseconds(0)) && calls == 3);
    CHECK(gate.position("005930") == 10 && gate.avg_price("005930") == 70000.0);
    CHECK(gate.sellable_view(std::string(), "005930").psbl_cap == 7);
    CHECK(gate.sellable_view(std::string(), "000660").psbl_cap == 3); // 모름(-1) → 보유수량
    CHECK(names.size() == 2 && names[0].first == "005930" && names[0].second == "N-005930");
    return 0;
}

int test_reconcile_rest()
{
    wipe_baselines();
    OrderGate gate;
    KisResult<AccountBalance> next = ok_balance({hold("A", 10, 100.0, 10)}, 1000000.0, 500000.0, 990000.0);
    std::vector<reconcile::Row> rows;
    LedgerReconciler r(gate, [&] { return next; });
    r.set_baseline_dir(baseline_dir());
    r.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });

    // 첫 대조: 원장 덮어쓰기, 기준선 캡처(델타 0), 파일 저장.
    r.reconcile(true, kT0);
    CHECK(gate.position("A") == 10 && gate.avg_price("A") == 100.0);
    CHECK(gate.daily_pnl() == 0.0 && gate.equity() == 1000000.0 && gate.available_cash() == 500000.0);
    CHECK(r.has_baseline() && r.baseline() == 1000000.0);
    {
        std::ifstream f(baseline_dir() / "pnl_baseline_20270115.txt");
        long long     v = 0;
        CHECK(f.is_open() && (f >> v) && v == 1000000);
    }

    // 원장이 비어 있었고 A가 새로 들어왔으니 OVERWRITE 행 하나.
    CHECK(rows.size() == 1 && rows[0].ticker == "A" && rows[0].action == "OVERWRITE");

    // 평가금이 오르면 델타만 움직인다.
    rows.clear();
    next = ok_balance({hold("A", 10, 100.0, 10)}, 1020000.0);
    r.reconcile(true, kT0 + 60);
    CHECK(gate.daily_pnl() == 20000.0 && rows.empty());

    // 재시작(새 인스턴스, 같은 날): 파일 기준선을 재사용해 손실컷이 이어진다.
    LedgerReconciler r2(gate, [&] { return next; });
    r2.set_baseline_dir(baseline_dir());
    r2.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });
    next = ok_balance({hold("A", 10, 100.0, 10)}, 900000.0);
    r2.reconcile(true, kT0 + 120);
    CHECK(r2.baseline() == 1000000.0 && gate.daily_pnl() == -100000.0);

    // 새 거래일: 다시 캡처(다른 파일).
    r2.new_trading_day();
    r2.reconcile(true, kT0 + 24 * 3600);
    CHECK(r2.baseline() == 900000.0 && gate.daily_pnl() == 0.0);
    CHECK(std::filesystem::exists(baseline_dir() / "pnl_baseline_20270116.txt"));

    // 유령 정리: 원장에만 있는 B(시드는 24시간 전 열린 것으로 잡힌다)는 걷어내고 PRUNE 행을 남긴다.
    gate.seed_position(std::string(), "B", 5, 50.0);
    rows.clear();
    r2.reconcile(true, kT0 + 24 * 3600 + 60);
    CHECK(gate.position("B") == 0 && gate.position("A") == 10);
    CHECK(rows.size() == 1 && rows[0].ticker == "B" && rows[0].action == "PRUNE");

    // [inv] 빈 output1은 잔고 실패로 보고 한 종목도 걷어내지 않는다 — 대조 행도 없다.
    gate.seed_position(std::string(), "B", 5, 50.0);
    rows.clear();
    next = ok_balance({}, 900000.0);
    r2.reconcile(true, kT0 + 24 * 3600 + 120);
    CHECK(gate.position("B") == 5 && gate.position("A") == 10 && rows.empty());
    return 0;
}

int test_reconcile_ws()
{
    OrderGate gate;
    gate.seed_position(std::string(), "A", 10, 100.0);
    std::vector<reconcile::Row> rows;
    LedgerReconciler r(gate, [&] { return ok_balance({hold("A", 8, 100.0, 8)}, 1000000.0); });
    r.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });

    // WS 모드는 원장을 덮어쓰지 않는다(첫 관측만으로는 놓친 매도로도 안 본다). 매도가능은 매번 맞춘다.
    r.reconcile(false, kT0);
    CHECK(gate.position("A") == 10);
    CHECK(gate.sellable_view(std::string(), "A").psbl_cap == 8);
    CHECK(rows.size() == 1 && rows[0].ticker == "A" && rows[0].action == "KEEP" && rows[0].ledger_qty == 10 &&
          rows[0].broker_qty == 8);
    CHECK(gate.equity() == 1000000.0 && r.has_baseline()); // 기준선 디렉터리 없음 → 파일 없이 캡처
    return 0;
}

int test_reconcile_failure()
{
    OrderGate gate;
    int       calls = 0;
    bool      good  = false;
    LedgerReconciler r(gate, [&] {
        ++calls;
        return good ? ok_balance({hold("A", 1, 1.0)}, 100.0) : fail_balance();
    });

    r.reconcile(true, kT0);
    CHECK(calls == 1 && !gate.is_pnl_stale() && r.breaker().skip_remaining() == 1);
    r.reconcile(true, kT0); // 백오프 — 조회 안 함
    CHECK(calls == 1);
    r.reconcile(true, kT0); // 실패 2 → pnl_stale
    CHECK(calls == 2 && gate.is_pnl_stale() && r.breaker().skip_remaining() == 2);
    r.reconcile(true, kT0);
    r.reconcile(true, kT0);
    CHECK(calls == 2);

    good = true;
    r.reconcile(true, kT0); // 복구 — 즉시 해제
    CHECK(calls == 3 && !gate.is_pnl_stale() && r.breaker().fail_streak() == 0 && gate.position("A") == 1);
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* env = std::getenv("QUANT_LOG_DIR"); !env || !*env)
    {
        Logger::instance().set_base_dir(Logger::executable_dir() / "logs_test");
    }

    if (test_breaker() || test_names() || test_bootstrap() || test_reconcile_rest() || test_reconcile_ws() ||
        test_reconcile_failure())
    {
        return 1;
    }

    std::cout << "test_ledger_reconciler: " << g_checks << " checks passed\n";
    return 0;
}
