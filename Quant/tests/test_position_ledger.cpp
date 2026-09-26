// PositionLedger 단위 테스트 — 게이트 판정 없이 원장만 본다
// 빌드: cmake --build <directory> --target test_position_ledger
// 실행: ./test_position_ledger
//
// 테스트 항목:
//   9. SELL 체결이 실보유를 0 미만으로 내리지 않는다 (C6 fix)
//  10. 부분체결 평단은 선점 수량이 아니라 실체결 수량으로 계산한다 (C2/C4 fix)
//  19. 원장 저널 — 재기동 리플레이가 보유·평단·선점·매도가능·당일손익·현금을 되살린다 (D-113)
//  20. 원장 저널 — 쓰다 만 꼬리(전원 장애)는 리플레이가 거기서 멈추고 다음 기동이 잘라 낸다
//  21. 원장 저널 — 한 레코드가 깨지면(CRC 불일치) 그 앞까지만 적용하고 뒤는 버린다
//  22. 원장 저널 — 코드페이지에 없는 글자가 든 폴더에서도 연다
//  23. 원장 저널 — 두 스레드가 같이 적어도 호출이 끝나면 전부 파일에 있고, 순번이 빠짐없이 이어진다 (CODE_REVIEW W-2)
//  24. 전략별 손익 — 전략이 가진 것보다 많이 팔면 가진 만큼만 그 전략 몫으로 잡고, 수수료·세금도 같은 비율로 나눈다 (CODE_REVIEW W-4)
//  25. 놓친 매수 체결 — 미체결 매수 이내의 부족분이 두 번 연속 보이면 잔고로 맞추고 예약을 푼다 (CODE_REVIEW W-1)

#include "risk/PositionLedger.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static void PASS(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl; // 뒤 테스트가 assert로 죽어도 여기까지는 남게 flush
}

// ─── 테스트 9: SELL 체결이 실보유를 0 미만으로 내리지 않음 (C6 + C2/C4) ──
//   원장 분리 후: on_accept는 reserved_만, 실보유 positions_는 on_fill_confirmed가 갱신
void test_sell_clamps_position_at_zero()
{
    PositionLedger ledger;

    // BUY 2주 접수→체결 → 실보유 2, 선점 해제
    ledger.on_accept("005930", OrderSide::BUY, 2, 100000);
    ledger.on_fill_confirmed("005930", OrderSide::BUY, 2, 100000);
    assert(ledger.position("005930") == 2);
    assert(ledger.reserved("005930") == 0);

    // SELL 5주 접수→체결 (보유 2 초과) → 실보유 0으로 클램프 (공매도 미지원)
    ledger.on_accept("005930", OrderSide::SELL, 5, 100000);
    ledger.on_fill_confirmed("005930", OrderSide::SELL, 5, 100000);
    assert(ledger.position("005930") == 0);
    PASS("sell_clamps_position_at_zero");
}

// ─── 테스트 10: 부분체결 평단 정확성 (C2/C4 fix) ───────────────────────────
//   on_accept 선점값이 아니라 실체결 수량으로 평단을 계산해야 함
void test_partial_fill_average_price()
{
    PositionLedger ledger;

    ledger.on_accept("005930", OrderSide::BUY, 10, 1000.0); // 선점 10
    assert(ledger.reserved("005930") == 10);

    // 5주 부분체결 @1000 → 평단 1000 (구버그라면 분모=선점10 → 500)
    auto fill_a = ledger.on_fill_confirmed("005930", OrderSide::BUY, 5, 1000.0);
    assert(fill_a.net_quantity == 5);
    assert(fill_a.average_price > 999.9 && fill_a.average_price < 1000.1);
    assert(ledger.reserved("005930") == 5); // 체결분만큼 선점 해제

    // 나머지 5주 체결 @1100 → 평단 (5*1000 + 5*1100)/10 = 1050
    auto fill_b = ledger.on_fill_confirmed("005930", OrderSide::BUY, 5, 1100.0);
    assert(fill_b.net_quantity == 10);
    assert(fill_b.average_price > 1049.9 && fill_b.average_price < 1050.1);
    assert(ledger.position("005930") == 10);
    assert(ledger.reserved("005930") == 0); // 선점 전부 해제
    PASS("partial_fill_avg_price");
}

// ─── 원장 저널 공통 ────────────────────────────────────────────────────────
//  테스트마다 빈 폴더 하나 — 남아 있던 파일을 리플레이해 앞 테스트가 뒤 테스트를 오염시키지 않게.
static std::filesystem::path make_journal_directory(const std::string& name)
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("quant_ledger_test_" + name);
    std::error_code             error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);
    return directory;
}

// 한 거래일치 사건을 저널에 적고 그 파일 경로를 돌려준다 — 리플레이 테스트 3개가 같은 장면을 쓴다.
static std::filesystem::path write_sample_day(const std::filesystem::path& directory, const std::string& date)
{
    PositionLedger ledger;
    assert(ledger.set_journal(directory, date, false));
    assert(ledger.journal_open());

    ledger.seed_position("ACC1", "005930", 10, 70000.0, 10);                                                 // 1 SEED
    assert(ledger.on_intent("ACC1", "005930", OrderSide::BUY, 5, 71000.0,
                          PositionLedger::OrderRef{11, 0, OrderType::LIMIT}));                                  // 2 INTENT
    ledger.on_accepted("ACC1", "005930", OrderSide::BUY, 5, PositionLedger::OrderRef{11, 991, OrderType::LIMIT}); // 3 ACCEPT
    ledger.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 3, 71000.0, strategy_table::kNone,
                           PositionLedger::OrderRef{11, 991, OrderType::LIMIT});                                // 4 FILL
    // 보내고 거부당한 주문 — 선점이 잡혔다 풀린다
    assert(ledger.on_intent("ACC1", "000660", OrderSide::BUY, 7, 200000.0,
                          PositionLedger::OrderRef{12, 0, OrderType::MARKET}));                                 // 5 INTENT
    ledger.on_reject("ACC1", "000660", OrderSide::BUY, 7, PositionLedger::OrderRef{12, 0, OrderType::MARKET},
                   "KIS 거부");                                                                            // 6 REJECT
    ledger.set_daily_pnl(-12345.0);                                                                          // 7 DAILY_PNL
    ledger.set_available_cash(4500000.0);                                                                    // 8 CASH
    ledger.set_equity(9900000.0);                                                                            // 9 CASH

    assert(ledger.position("ACC1", "005930") == 13);
    assert(ledger.reserved("ACC1", "005930") == 2);
    assert(ledger.reserved("ACC1", "000660") == 0);
    assert(ledger.journal_failures() == 0);
    return ledger.journal_path();
}

// 저널 파일 끝에서 몇 바이트를 잘라 낸다 — 쓰다 만 마지막 레코드(전원 장애) 흉내.
static void truncate_tail_bytes(const std::filesystem::path& file, uintmax_t bytes)
{
    std::error_code error_code;
    const uintmax_t size = std::filesystem::file_size(file, error_code);
    assert(size > bytes);
    std::filesystem::resize_file(file, size - bytes, error_code);
    assert(!error_code);
}

// ─── 테스트 19: 재기동 리플레이가 원장을 되살린다 ──────────────────────────
void test_journal_replay_rebuilds_ledger()
{
    const std::filesystem::path directory = make_journal_directory("replay");
    const std::string           date      = "20260922";
    write_sample_day(directory, date);

    PositionLedger restarted;
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.header_ok);
    assert(replayed.applied == 9);
    assert(!replayed.truncated_tail);
    assert(replayed.last_sequence == 9);

    assert(restarted.position("ACC1", "005930") == 13);
    assert(restarted.reserved("ACC1", "005930") == 2);   // 5주 중 3주 체결 → 잔량 2주 선점
    assert(restarted.reserved("ACC1", "000660") == 0);   // 거부된 주문의 선점은 안 남는다
    const double average = restarted.average_price("ACC1", "005930");
    assert(average > 70229.0 && average < 70232.0);      // (10*70000 + 3*71000)/13 = 70230.77
    assert(restarted.sellable_view("ACC1", "005930").possible_quantity_cap == 13); // 시드 10 + 매수체결 3
    assert(restarted.daily_pnl() < -12344.0 && restarted.daily_pnl() > -12346.0);
    assert(restarted.available_cash() > 4499999.0 && restarted.available_cash() < 4500001.0);
    assert(restarted.equity() > 9899999.0 && restarted.equity() < 9900001.0);
    assert(restarted.journal_failures() == 0);

    // FILL은 DB 적재기용으로 체결 결과(수수료·세금·평단·보유)를 reason 칸에 싣는다.
    ledger_journal::FillDetail detail;
    ledger_journal::LedgerJournal::replay(restarted.journal_path(),
                                          [&](const ledger_journal::Record& record)
                                          {
                                              if (record.kind == static_cast<uint16_t>(ledger_journal::Kind::FILL))
                                              {
                                                  std::memcpy(&detail, record.reason, sizeof(detail));
                                              }
                                          });
    assert(detail.present == 1);
    assert(detail.net_quantity == 13);
    assert(detail.average_price > 70229.0 && detail.average_price < 70232.0);
    assert(detail.commission > 31.94 && detail.commission < 31.96); // 3 * 71000 * 0.015%
    assert(detail.tax == 0.0);                                       // 매수는 거래세 없음

    // 미결 주문 — 5주 중 3주만 체결돼 결말을 못 본 주문 하나가 남는다(거부된 000660은 닫혔다).
    const auto intents = restarted.open_intents();
    assert(intents.size() == 1);
    assert(intents[0].order_id == 11 && intents[0].kis_order_number == 991);
    assert(intents[0].ticker == "005930" && intents[0].remaining == 2 && intents[0].accepted);

    // 이어 적기 — 리플레이한 만큼 건너뛴 번호부터 붙는다(같은 번호를 두 번 쓰면 뒤 기동이 헷갈린다).
    assert(restarted.on_intent("ACC1", "005930", OrderSide::SELL, 4, 72000.0,
                               PositionLedger::OrderRef{13, 0, OrderType::LIMIT}));
    assert(restarted.reserved("ACC1", "005930") == -2); // 매수 잔량 2 - 매도 선점 4

    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    PASS("journal_replay_rebuilds_ledger");
}

// ─── 테스트 20: 쓰다 만 꼬리 ───────────────────────────────────────────────
void test_journal_truncates_broken_tail()
{
    const std::filesystem::path directory = make_journal_directory("tail");
    const std::string           date      = "20260922";
    const std::filesystem::path file      = write_sample_day(directory, date);

    // 마지막 CASH 레코드를 절반만 남긴다 — fwrite 도중 전원이 나간 모습.
    truncate_tail_bytes(file, sizeof(ledger_journal::Record) / 2);

    PositionLedger restarted;
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.truncated_tail);
    assert(replayed.applied == 8);                        // 온전한 8건까지만
    assert(restarted.position("ACC1", "005930") == 13);   // 보유는 그대로
    assert(restarted.equity() == 0.0);                    // 잘린 CASH는 안 적용된다

    // 잘린 꼬리는 파일에서도 떨어져 나가 다음 레코드가 경계에 맞게 붙는다 — 파이썬 판독기가 같은 자리를 본다.
    std::error_code error_code;
    assert(std::filesystem::file_size(restarted.journal_path(), error_code) ==
           sizeof(ledger_journal::FileHeader) + 8 * sizeof(ledger_journal::Record));

    std::filesystem::remove_all(directory, error_code);
    PASS("journal_truncates_broken_tail");
}

// ─── 테스트 21: 깨진 레코드 ────────────────────────────────────────────────
void test_journal_stops_at_corrupt_record()
{
    const std::filesystem::path directory = make_journal_directory("crc");
    const std::string           date      = "20260922";
    const std::filesystem::path file      = write_sample_day(directory, date);

    // 4번째 레코드(FILL) 한 바이트를 뒤집는다 — 디스크가 조용히 썩은 경우.
    const long offset = static_cast<long>(sizeof(ledger_journal::FileHeader) + 3 * sizeof(ledger_journal::Record)) + 32;
    std::FILE* handle = std::fopen(file.string().c_str(), "r+b");
    assert(handle != nullptr);
    assert(std::fseek(handle, offset, SEEK_SET) == 0);
    unsigned char byte = 0;
    assert(std::fread(&byte, 1, 1, handle) == 1);
    byte ^= 0xFFu;
    assert(std::fseek(handle, offset, SEEK_SET) == 0);
    assert(std::fwrite(&byte, 1, 1, handle) == 1);
    std::fclose(handle);

    PositionLedger restarted;
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.truncated_tail);
    assert(replayed.applied == 3);                       // SEED·INTENT·ACCEPT까지
    assert(restarted.position("ACC1", "005930") == 10);  // 체결 전 보유
    assert(restarted.reserved("ACC1", "005930") == 5);   // 선점은 아직 안 풀렸다
    assert(restarted.daily_pnl() == 0.0);

    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    PASS("journal_stops_at_corrupt_record");
}

// ─── 테스트 22: 코드페이지에 없는 글자가 든 폴더 ──────────────────────────
void test_journal_opens_on_non_codepage_path()
{
    // U+3400은 Windows 한국어 코드페이지(CP949)에 매핑이 없다. 경로를 좁은 문자열로 되돌려 열면
    //  여기서 예외가 나고, 저널을 못 열면 엔진이 기동을 거부한다 — 실제로 리플레이 기동이 그렇게 죽었다.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::wstring(L"quant_ledger_test_㐀");
    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);

    const std::string date = "20260922";
    PositionLedger ledger;
    assert(ledger.set_journal(directory, date, false));
    ledger.seed_position("ACC1", "005930", 10, 70000.0);
    assert(ledger.journal_failures() == 0);

    PositionLedger restarted;
    assert(restarted.set_journal(directory, date, false));
    assert(restarted.journal_replay().header_ok);
    assert(restarted.position("ACC1", "005930") == 10);

    std::filesystem::remove_all(directory, error_code);
    PASS("journal_opens_on_non_codepage_path");
}

// ─── 테스트 23: 두 스레드가 같이 적어도 빠짐없이 순서대로 남는다 ──────────────
//  원장 잠금 안에서는 버퍼에 쌓기만 하고 잠금 밖에서 모아 쓴다(W-2). 체결 스레드와 잔고 대조 스레드가 겹쳐 적어도
//  각 호출이 끝나면 제 레코드가 이미 파일에 있어야 하고(버퍼에 남은 채 끝나면 재기동이 그 변경을 잃는다),
//  파일의 순번은 1부터 하나씩 늘어야 한다(파일 순서 = 원장 갱신 순서).
void test_journal_concurrent_writers_flush_in_order()
{
    const std::filesystem::path directory = make_journal_directory("concurrent");
    const std::string           date      = "20260925";
    constexpr int               kPerThread = 500;
    std::filesystem::path       file;
    {
        PositionLedger ledger;
        assert(ledger.set_journal(directory, date, false));
        file = ledger.journal_path();

        std::thread filler([&]
                           {
                               for (int index = 0; index < kPerThread; ++index)
                               {
                                   ledger.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 1, 70000.0,
                                                            strategy_table::kNone,
                                                            PositionLedger::OrderRef{static_cast<uint64_t>(index + 1), 0,
                                                                                     OrderType::LIMIT});
                               }
                           });
        std::thread reconciler([&]
                               {
                                   for (int index = 0; index < kPerThread; ++index)
                                   {
                                       ledger.set_available_cash(1000000.0 + index);
                                   }
                               });
        filler.join();
        reconciler.join();

        // 원장이 살아 있는 채로 읽는다 — 소멸자의 마지막 쓰기에 기대지 않고 호출마다 썼는지 본다.
        std::vector<uint64_t> sequences;
        const auto result = ledger_journal::LedgerJournal::replay(
            file, [&](const ledger_journal::Record& record)
            {
                sequences.push_back(record.sequence);
            });
        assert(result.header_ok && !result.truncated_tail);
        assert(sequences.size() == 2 * kPerThread);

        for (size_t index = 0; index < sequences.size(); ++index)
        {
            assert(sequences[index] == index + 1);
        }

        assert(ledger.journal_failures() == 0);
    }

    PositionLedger restarted;
    assert(restarted.set_journal(directory, date, false));
    assert(restarted.position("ACC1", "005930") == kPerThread);

    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    PASS("journal_concurrent_writers_flush_in_order");
}

void test_strategy_sell_beyond_holding_attributes_only_held_part()
{
    PositionLedger                   ledger;
    constexpr strategy_table::StrategyId kStrategy = 7;

    // 기동 시드 10주(전략 없음) 위에 전략 7이 3주를 산다. 그 뒤 13주를 한 번에 판다.
    ledger.seed_position("ACC1", "005930", 10, 900.0);
    ledger.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 3, 1000.0, kStrategy);
    const auto sell = ledger.on_fill_confirmed("ACC1", "005930", OrderSide::SELL, 13, 1100.0, kStrategy);

    // 전략 7 몫은 3주뿐이다 — 3 × (1100 − 1000)에서 비용의 3/13만 뺀다.
    const double expected = 3 * 100.0 - (sell.commission + sell.tax) * 3.0 / 13.0;
    assert(!sell.strategy_basis_unknown);
    assert(std::abs(sell.strategy_realized_pnl - expected) < 1e-6);
    PASS("strategy_sell_beyond_holding_attributes_only_held_part");
}

void test_absorb_missed_buy_after_two_observations()
{
    PositionLedger ledger;

    // 매수 10주를 접수했는데 체결통보가 큐에서 버려졌다 — 원장 0, 예약 10, 잔고는 10주.
    ledger.on_accept("005930", OrderSide::BUY, 10, 1000.0);
    assert(ledger.absorb_missed_buy(std::string(), "005930", 10, 1005.0) == 0); // 첫 관측은 기다린다
    assert(ledger.position("005930") == 0);
    assert(ledger.absorb_missed_buy(std::string(), "005930", 10, 1005.0) == 10);
    assert(ledger.position("005930") == 10);
    assert(ledger.reserved("005930") == 0);
    assert(ledger.average_price("005930") == 1005.0);

    // 예약보다 큰 부족분(밖에서 산 것)은 놓친 체결로 보지 않는다.
    ledger.on_accept("000660", OrderSide::BUY, 3, 2000.0);
    assert(ledger.absorb_missed_buy(std::string(), "000660", 5, 2000.0) == 0);
    assert(ledger.absorb_missed_buy(std::string(), "000660", 5, 2000.0) == 0);
    assert(ledger.position("000660") == 0);
    PASS("absorb_missed_buy_after_two_observations");
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== PositionLedger Unit Tests ===\n";
    test_sell_clamps_position_at_zero();
    test_partial_fill_average_price();
    test_journal_replay_rebuilds_ledger();
    test_journal_truncates_broken_tail();
    test_journal_stops_at_corrupt_record();
    test_journal_opens_on_non_codepage_path();
    test_journal_concurrent_writers_flush_in_order();
    test_strategy_sell_beyond_holding_attributes_only_held_part();
    test_absorb_missed_buy_after_two_observations();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
