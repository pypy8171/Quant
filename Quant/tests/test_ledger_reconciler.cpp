// 잔고 → 원장 대조기(core/LedgerReconciler.h) 단위 테스트. 브로커는 std::function으로 대신해 KIS 없이
//  기동 시드 재시도, REST/WS 모드의 덮어쓰기·유지, 유령 정리와 빈 잔고 가드, 당일 기준선의 파일 재사용·
//  새 거래일 재캡처, 서킷브레이커 백오프와 pnl_stale 전이를 고정한다. OrderGate·Logger를 링크한다.
//  관련 결정: D-038(대조 행), D-061(분리).
// 빌드: cmake --build <directory> --target test_ledger_reconciler
#include "core/LedgerReconciler.h"
#include "utils/Logger.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 2027-01-15 08:00 UTC = KST 17:00 같은 날. 기준선 파일 날짜를 고정한다.
constexpr std::time_t kT0 = 1800000000;

Holding hold(const char* ticker, int quantity, double average_value, std::optional<int> psbl = std::nullopt)
{
    Holding holding;
    holding.ticker       = ticker;
    holding.name         = std::string("N-") + ticker;
    holding.quantity          = quantity;
    holding.average_price    = average_value;
    holding.sellable_quantity = psbl;
    return holding;
}

KisResult<AccountBalance> ok_balance(std::vector<Holding> hs, double total_evaluation, double cash = 500000.0,
                                     std::optional<double> previous = std::nullopt)
{
    AccountBalance balance;
    balance.holdings             = std::move(hs);
    balance.total_evaluation_amount       = total_evaluation;
    balance.available_cash       = cash;
    balance.previous_day_total_asset = previous;
    return balance;
}

KisResult<AccountBalance> fail_balance()
{
    return kis_fail("EGW00201", "초당 거래건수 초과");
}

std::filesystem::path baseline_directory()
{
    return Logger::instance().base_directory() / "ledger_test";
}

void wipe_baselines()
{
    std::error_code error_code;
    std::filesystem::remove_all(baseline_directory(), error_code);
    std::filesystem::create_directories(baseline_directory(), error_code);
}

int test_breaker()
{
    ledger::ReconcileBreaker reconcile_breaker;
    CHECK(!reconcile_breaker.take_skip());

    // 실패 1: 백오프 1사이클, 아직 pnl_stale 아님.
    ledger::BreakerOutcome breaker_outcome = reconcile_breaker.on_result(false);
    CHECK(breaker_outcome.log_backoff && breaker_outcome.skip_cycles == 1 && !breaker_outcome.pnl_stale && !breaker_outcome.log_stale_on && reconcile_breaker.fail_streak() == 1);
    CHECK(reconcile_breaker.take_skip() && !reconcile_breaker.take_skip());

    // 실패 2: 임계 진입 1회 로그 + pnl_stale=true, 백오프 2.
    breaker_outcome = reconcile_breaker.on_result(false);
    CHECK(breaker_outcome.log_stale_on && breaker_outcome.pnl_stale && *breaker_outcome.pnl_stale && breaker_outcome.skip_cycles == 2);
    breaker_outcome = reconcile_breaker.on_result(false);
    CHECK(!breaker_outcome.log_stale_on && breaker_outcome.pnl_stale && *breaker_outcome.pnl_stale && breaker_outcome.skip_cycles == 4);
    breaker_outcome = reconcile_breaker.on_result(false);
    CHECK(breaker_outcome.skip_cycles == 8);
    breaker_outcome = reconcile_breaker.on_result(false);
    CHECK(breaker_outcome.skip_cycles == 8 && reconcile_breaker.fail_streak() == 5); // 상한 2^3

    // 복구: 즉시 스킵 0, 두 로그 모두, pnl_stale=false.
    breaker_outcome = reconcile_breaker.on_result(true);
    CHECK(breaker_outcome.log_recovered && breaker_outcome.log_stale_off && breaker_outcome.pnl_stale && !*breaker_outcome.pnl_stale && reconcile_breaker.skip_remaining() == 0 &&
          reconcile_breaker.fail_streak() == 0);

    // 단발 실패 뒤 성공: 복구 로그는 찍되 정체 해제 로그는 없다.
    (void) reconcile_breaker.on_result(false);
    breaker_outcome = reconcile_breaker.on_result(true);
    CHECK(breaker_outcome.log_recovered && !breaker_outcome.log_stale_off);
    breaker_outcome = reconcile_breaker.on_result(true);
    CHECK(!breaker_outcome.log_recovered && breaker_outcome.pnl_stale && !*breaker_outcome.pnl_stale);
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
    LedgerReconciler reconciler(gate, [&] {
        ++calls;
        return fail_balance();
    });
    CHECK(!reconciler.bootstrap(5, std::chrono::milliseconds(0)) && calls == 5);
    CHECK(gate.snapshot_positions().empty());

    // 두 번 실패 뒤 성공 — 원장·주문가능·종목명이 시드된다.
    calls = 0;
    std::vector<std::pair<std::string, std::string>> names;
    LedgerReconciler second_reconciler(gate, [&] {
        ++calls;
        return calls < 3 ? fail_balance()
                         : ok_balance({hold("005930", 10, 70000.0, 7), hold("000660", 3, 150000.0)}, 1000000.0);
    });
    second_reconciler.set_name_sink([&](const std::string& ticker, const std::string& name) { names.emplace_back(ticker, name); });
    CHECK(second_reconciler.bootstrap(5, std::chrono::milliseconds(0)) && calls == 3);
    CHECK(gate.position("005930") == 10 && gate.average_price("005930") == 70000.0);
    CHECK(gate.sellable_view(std::string(), "005930").possible_quantity_cap == 7);
    CHECK(gate.sellable_view(std::string(), "000660").possible_quantity_cap == 3); // 모름(-1) → 보유수량
    CHECK(names.size() == 2 && names[0].first == "005930" && names[0].second == "N-005930");
    return 0;
}

int test_reconcile_rest()
{
    wipe_baselines();
    OrderGate gate;
    KisResult<AccountBalance> next = ok_balance({hold("A", 10, 100.0, 10)}, 1000000.0, 500000.0, 990000.0);
    std::vector<reconcile::Row> rows;
    LedgerReconciler reconciler(gate, [&] { return next; });
    reconciler.set_baseline_directory(baseline_directory());
    reconciler.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });

    // 첫 대조: 원장 덮어쓰기, 기준선은 전일 총자산(990000) — 시초 갭 +10000이 당일손익에 든다. 파일 저장.
    reconciler.reconcile(true, kT0);
    CHECK(gate.position("A") == 10 && gate.average_price("A") == 100.0);
    CHECK(gate.daily_pnl() == 10000.0 && gate.equity() == 1000000.0 && gate.available_cash() == 500000.0);
    CHECK(reconciler.has_baseline() && reconciler.baseline() == 990000.0);
    {
        std::ifstream file(baseline_directory() / "pnl_baseline_20270115.txt");
        long long     cash_value = 0;
        CHECK(file.is_open() && (file >> cash_value) && cash_value == 990000);
    }

    // 원장이 비어 있었고 A가 새로 들어왔으니 OVERWRITE 행 하나.
    CHECK(rows.size() == 1 && rows[0].ticker == "A" && rows[0].action == "OVERWRITE");

    // 평가금이 오르면 델타만 움직인다.
    rows.clear();
    next = ok_balance({hold("A", 10, 100.0, 10)}, 1020000.0);
    reconciler.reconcile(true, kT0 + 60);
    CHECK(gate.daily_pnl() == 30000.0 && rows.empty());

    // 재시작(새 인스턴스, 같은 날): 파일 기준선을 재사용해 손실컷이 이어진다.
    LedgerReconciler second_reconciler(gate, [&] { return next; });
    second_reconciler.set_baseline_directory(baseline_directory());
    second_reconciler.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });
    next = ok_balance({hold("A", 10, 100.0, 10)}, 900000.0);
    second_reconciler.reconcile(true, kT0 + 120);
    CHECK(second_reconciler.baseline() == 990000.0 && gate.daily_pnl() == -90000.0);

    // 새 거래일: 다시 캡처(다른 파일). 전일 총자산이 없으면 첫 대조 총평가금으로 떨어진다.
    second_reconciler.new_trading_day();
    second_reconciler.reconcile(true, kT0 + 24 * 3600);
    CHECK(second_reconciler.baseline() == 900000.0 && gate.daily_pnl() == 0.0);
    CHECK(std::filesystem::exists(baseline_directory() / "pnl_baseline_20270116.txt"));

    // 유령 정리: 원장에만 있는 B(시드는 24시간 전 열린 것으로 잡힌다)는 걷어내고 PRUNE 행을 남긴다.
    gate.seed_position(std::string(), "B", 5, 50.0);
    rows.clear();
    second_reconciler.reconcile(true, kT0 + 24 * 3600 + 60);
    CHECK(gate.position("B") == 0 && gate.position("A") == 10);
    CHECK(rows.size() == 1 && rows[0].ticker == "B" && rows[0].action == "PRUNE");

    // [inv] 빈 output1은 잔고 실패로 보고 한 종목도 걷어내지 않는다 — 대조 행도 없다.
    gate.seed_position(std::string(), "B", 5, 50.0);
    rows.clear();
    next = ok_balance({}, 900000.0);
    second_reconciler.reconcile(true, kT0 + 24 * 3600 + 120);
    CHECK(gate.position("B") == 5 && gate.position("A") == 10 && rows.empty());
    return 0;
}

int test_reconcile_websocket()
{
    OrderGate gate;
    gate.seed_position(std::string(), "A", 10, 100.0);
    std::vector<reconcile::Row> rows;
    LedgerReconciler reconciler(gate, [&] { return ok_balance({hold("A", 8, 100.0, 8)}, 1000000.0); });
    reconciler.set_reconcile_sink([&](const reconcile::Row& row) { rows.push_back(row); });

    // WS 모드는 원장을 덮어쓰지 않는다(첫 관측만으로는 놓친 매도로도 안 본다). 매도가능은 매번 맞춘다.
    reconciler.reconcile(false, kT0);
    CHECK(gate.position("A") == 10);
    CHECK(gate.sellable_view(std::string(), "A").possible_quantity_cap == 8);
    CHECK(rows.size() == 1 && rows[0].ticker == "A" && rows[0].action == "KEEP" && rows[0].ledger_quantity == 10 &&
          rows[0].broker_quantity == 8);
    CHECK(gate.equity() == 1000000.0 && reconciler.has_baseline()); // 기준선 디렉터리 없음 → 파일 없이 캡처
    return 0;
}

// 체결 직후 유예: 체결 뒤 defer초 안의 대조는 조회 없이 미루고, 체결이 이어지면 max마다 한 번은 돈다. [D-074]
int test_reconcile_post_fill_defer()
{
    OrderGate gate;
    int       calls = 0;
    LedgerReconciler reconciler(gate, [&] {
        ++calls;
        return ok_balance({hold("A", 8, 100.0, 8)}, 1000000.0);
    });
    reconciler.set_post_fill_defer(5, 30);

    reconciler.reconcile(false, kT0); // 체결 이력 없음 — 돈다
    CHECK(calls == 1);
    reconciler.note_fill(kT0 + 10);
    reconciler.reconcile(false, kT0 + 11); // 체결 1초 뒤 — 미룬다
    reconciler.reconcile(false, kT0 + 14); // 4초 뒤 — 아직
    CHECK(calls == 1);
    reconciler.reconcile(false, kT0 + 15); // 5초 — 돈다
    CHECK(calls == 2);

    // 체결이 3초마다 이어진다: 30초 상한마다 한 번은 돈다.
    std::time_t time_value = kT0 + 100;

    for (int index = 0; index < 20; ++index, time_value += 3)
    {
        reconciler.note_fill(time_value);
        reconciler.reconcile(false, time_value + 1);
    }

    CHECK(calls == 3); // 101에서 유예 시작 → 131에서 한 번(다음은 161인데 체결은 157에서 끝난다)
    reconciler.reconcile(false, time_value + 10); // 마지막 체결에서 5초 넘음 — 돈다
    CHECK(calls == 4);
    // 유예 0이면 끈 것과 같다.
    reconciler.set_post_fill_defer(0, 30);
    reconciler.note_fill(time_value + 20);
    reconciler.reconcile(false, time_value + 21);
    CHECK(calls == 5);
    return 0;
}

int test_reconcile_failure()
{
    OrderGate gate;
    int       calls = 0;
    bool      good  = false;
    LedgerReconciler reconciler(gate, [&] {
        ++calls;
        return good ? ok_balance({hold("A", 1, 1.0)}, 100.0) : fail_balance();
    });

    reconciler.reconcile(true, kT0);
    CHECK(calls == 1 && !gate.is_pnl_stale() && reconciler.breaker().skip_remaining() == 1);
    reconciler.reconcile(true, kT0); // 백오프 — 조회 안 함
    CHECK(calls == 1);
    reconciler.reconcile(true, kT0); // 실패 2 → pnl_stale
    CHECK(calls == 2 && gate.is_pnl_stale() && reconciler.breaker().skip_remaining() == 2);
    reconciler.reconcile(true, kT0);
    reconciler.reconcile(true, kT0);
    CHECK(calls == 2);

    good = true;
    reconciler.reconcile(true, kT0); // 복구 — 즉시 해제
    CHECK(calls == 3 && !gate.is_pnl_stale() && reconciler.breaker().fail_streak() == 0 && gate.position("A") == 1);
    return 0;
}

// 8) 느린 잔고 응답: 조회가 대기 상한을 넘기면 reconcile은 곧 돌아오고(사이클을 안 붙잡음) 원장은 그대로,
//    응답이 도착한 뒤의 다음 reconcile이 결과를 적용한다. 09-18 모의 서버가 잔고에 20~100초를 쓴 사례.
int test_reconcile_slow_fetch()
{
    wipe_baselines();
    OrderGate gate;
    std::atomic<int> calls{0};
    LedgerReconciler reconciler(gate, [&]
    {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        return ok_balance({hold("A", 3, 100.0, 3)}, 1000000.0, 500000.0, 990000.0);
    });
    reconciler.set_baseline_directory(baseline_directory());
    reconciler.set_fetch_wait_budget(std::chrono::milliseconds(50));

    const auto first_call = std::chrono::steady_clock::now();
    reconciler.reconcile(true, kT0);
    const auto first_elapsed = std::chrono::steady_clock::now() - first_call;
    CHECK(first_elapsed < std::chrono::milliseconds(400) && reconciler.fetch_in_flight());
    CHECK(gate.position("A") == 0 && calls == 1);

    reconciler.reconcile(true, kT0); // 아직 응답 전 — 새 조회를 띄우지 않는다
    CHECK(calls == 1 && reconciler.fetch_in_flight() && gate.position("A") == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    reconciler.reconcile(true, kT0); // 응답이 와 있다 — 적용하고 future를 비운다
    CHECK(gate.position("A") == 3 && gate.equity() == 1000000.0 && !reconciler.fetch_in_flight());
    CHECK(reconciler.breaker().fail_streak() == 0 && calls == 1);
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    if (test_breaker() || test_names() || test_bootstrap() || test_reconcile_rest() || test_reconcile_websocket() ||
        test_reconcile_post_fill_defer() || test_reconcile_failure() || test_reconcile_slow_fetch())
    {
        return 1;
    }

    std::cout << "test_ledger_reconciler: " << g_checks << " checks passed\n";
    return 0;
}
