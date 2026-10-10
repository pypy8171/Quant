// tests/test_ledger_snapshot.cpp
// 장부 사본 검증 (D-114 단계 2.5) — 전략 쪽이 OrderGate 대신 읽을 사본이 원본과 같은 약속을 지키는가.
//
//   ① 아무것도 안 실은 사본은 모든 종목이 0이다
//   ② 한 판 실으면 실은 값이 그대로 읽힌다
//   ③ 지난 판에 있던 종목이 이번 판에 없으면 0이 된다(줄마다 찍은 판 번호가 하는 일 — 낡은 값이 남지 않는다)
//   ④ entry()는 보유·선점·여력을 한 판에서 함께 준다(따로 읽어 안 맞는 조합을 보지 않게)
//   ⑤ 상한을 넘은 종목 번호는 사본을 망가뜨리지 않는다
//   ⑥ 보유 전체를 한 판에서 훑는다(강제청산·한도정리가 쓰던 자리)
//   ⑦ 쓰는 중에 읽어도 반쪽 판이 안 나온다 — 쓰는 스레드와 읽는 스레드를 같이 돌려 약속을 검사한다
//   ⑧ 판 번호가 판마다 하나씩 오른다
//   ⑩ 쓰는 쪽이 판을 연 채 멈춰도 읽는 쪽이 끝없이 돌지 않는다 — 기한 뒤 보수값을 돌려주고 횟수를 센다
//   ⑪ 기한(kStableReadDeadline)이 정상 판과 충분히 떨어져 있는가 — 한 판을 쓰는 시간을 재서 표로 찍는다
//
//   사용법: test_ledger_snapshot

#include "ipc/LedgerSnapshot.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cerr << "FAIL: " << name << "\n";
        std::abort();
    }
}

// 한 판을 1..row_count 번 종목으로 채워 내는 시간을 measure_count번 잰 분포(마이크로초).
//  판 번호가 홀수인 구간 전체(begin_publish·줄 채우기·end_publish의 공개 칸 복사)가 읽는 쪽이 기다리는 시간이다.
//  최대는 운영체제가 쓰는 스레드를 판 중간에 내려놓은 회차가 정한다 — 그래서 99번째 백분위를 따로 둔다.
struct PublishTiming
{
    double average_microseconds = 0.0;
    double median_microseconds  = 0.0;
    double p99_microseconds     = 0.0;
    double max_microseconds     = 0.0;
    int    over_limit_count     = 0; // limit_microseconds를 넘은 회차 수
};

PublishTiming measure_publish(ipc::LedgerSnapshot& snapshot, symbol::SymbolId row_count, int measure_count,
                              double limit_microseconds)
{
    using Clock = std::chrono::steady_clock;

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measure_count));

    for (int round = 0; round < measure_count; ++round)
    {
        const auto started = Clock::now();
        snapshot.begin_publish();

        for (symbol::SymbolId id = 1; id <= row_count; ++id)
        {
            ipc::LedgerRow& row = snapshot.row_for_write(id);
            row.position        = round;
            row.sellable        = round;
            row.average_price   = 1000.0 + id;
        }

        snapshot.end_publish();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
    }

    std::sort(samples.begin(), samples.end());

    PublishTiming timing;
    double        total_microseconds = 0.0;

    for (const double sample : samples)
    {
        total_microseconds += sample;

        if (sample >= limit_microseconds)
        {
            ++timing.over_limit_count;
        }
    }

    timing.average_microseconds = total_microseconds / static_cast<double>(samples.size());
    timing.median_microseconds  = samples[samples.size() / 2];
    timing.p99_microseconds     = samples[samples.size() * 99 / 100];
    timing.max_microseconds     = samples.back();
    return timing;
}

// 한 종목만 실은 판을 하나 낸다 — 시험마다 되풀이되는 세 줄을 묶었다.
void publish_one(ipc::LedgerSnapshot& snapshot, symbol::SymbolId id, int position, int reserved, int sellable,
                 double average_price)
{
    snapshot.begin_publish();
    ipc::LedgerRow& row = snapshot.row_for_write(id);
    row.position        = position;
    row.reserved        = reserved;
    row.sellable        = sellable;
    row.average_price   = average_price;
    snapshot.end_publish();
}

} // namespace

int main()
{
    // ── ① 빈 사본 ────────────────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        const ipc::LedgerRow row = snapshot.row(7);
        check(row.position == 0 && row.reserved == 0 && row.sellable == 0, "안 실은 종목은 전부 0");
        check(snapshot.generation() == 0, "판을 안 냈으면 판 번호 0");
    }

    // ── ② 실은 값이 그대로 ───────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        publish_one(snapshot, 3, 10, -4, 6, 71500.0);

        const ipc::LedgerRow row = snapshot.row(3);
        check(row.position == 10, "보유 수량이 그대로");
        check(row.reserved == -4, "미체결 선점은 부호를 지킨다(매도는 음수)");
        check(row.sellable == 6, "매도가능 수량이 그대로");
        check(row.average_price == 71500.0, "평단이 그대로");
        check(snapshot.row(4).position == 0, "안 실은 옆 종목은 0");
    }

    // ── ③ 지난 판 값이 남지 않는다 ────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        publish_one(snapshot, 3, 10, 0, 10, 71500.0);
        check(snapshot.row(3).position == 10, "첫 판에는 보유가 있다");

        // 다음 판에는 3번을 안 싣는다 — 그 사이 전량 매도가 체결돼 장부에서 빠진 경우다.
        snapshot.begin_publish();
        snapshot.end_publish();

        const ipc::LedgerRow row = snapshot.row(3);
        check(row.position == 0, "이번 판에 없으면 보유 0 — 낡은 값을 돌려주지 않는다");
        check(row.average_price == 0.0, "평단도 같이 사라진다");
    }

    // ── ④ entry()는 셋을 함께 ────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        snapshot.begin_publish();
        ipc::LedgerRow& row = snapshot.row_for_write(5);
        row.position        = 2;
        row.reserved        = 1;
        snapshot.globals_for_write().capacity_full = 1;
        snapshot.end_publish();

        const ipc::EntryView view = snapshot.entry(5);
        check(view.position == 2 && view.reserved == 1, "보유·선점을 같이 준다");
        check(view.capacity_full, "여력 없음을 같이 준다");

        const ipc::EntryView unknown = snapshot.entry(9);
        check(unknown.position == 0 && unknown.reserved == 0, "모르는 종목은 보유·선점 0");
        check(unknown.capacity_full, "모르는 종목이어도 여력은 전역값을 본다");
    }

    // ── ⑤ 상한을 넘은 번호 ───────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        publish_one(snapshot, 11, 5, 0, 5, 1000.0);

        snapshot.begin_publish();
        snapshot.row_for_write(ipc::LedgerSnapshot::kMaxSymbols).position     = 999; // 버리는 칸
        snapshot.row_for_write(ipc::LedgerSnapshot::kMaxSymbols + 50).position = 999;
        snapshot.row_for_write(symbol::kNone).position                        = 999;
        snapshot.end_publish();

        check(snapshot.row(ipc::LedgerSnapshot::kMaxSymbols).position == 0, "상한을 넘은 번호는 0으로 읽힌다");
        check(snapshot.row(symbol::kNone).position == 0, "빈 종목 번호는 0으로 읽힌다");
        check(snapshot.row(11).position == 0, "옆 종목이 덮이지 않았다(이번 판에 안 실었으니 0)");
    }

    // ── ⑥ 보유 전체 훑기 ─────────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        snapshot.begin_publish();

        for (symbol::SymbolId id = 1; id <= 3; ++id)
        {
            snapshot.row_for_write(id).position = static_cast<int32_t>(id) * 10;
        }

        snapshot.end_publish();

        symbol::SymbolId ids[8]{};
        ipc::LedgerRow   rows[8]{};
        const size_t     taken = snapshot.collect_rows(ids, rows, 8);
        check(taken == 3, "실은 종목 수만큼 돌려준다");
        check(rows[0].position == 10 && rows[2].position == 30, "실은 값이 그대로 담긴다");

        // 잘릴 때: 실제 수를 돌려줘 호출부가 잘렸는지 안다
        const size_t truncated = snapshot.collect_rows(ids, rows, 2);
        check(truncated == 3, "칸이 모자라도 실제 수를 돌려준다");

        // 다음 판에 하나만 실으면 나머지는 목록에서 빠진다
        snapshot.begin_publish();
        snapshot.row_for_write(2).position = 99;
        snapshot.end_publish();
        check(snapshot.collect_rows(ids, rows, 8) == 1, "이번 판에 실은 것만 나온다");
        check(ids[0] == 2 && rows[0].position == 99, "남은 하나가 이번 판 값이다");
    }

    // ── ⑦ 쓰는 중에 읽어도 반쪽 판이 안 나온다 ─────────────────────────────
    {
        // 약속: 한 판 안에서는 모든 종목의 보유 수량이 같다(판 번호를 그대로 넣는다).
        //  반쪽 판을 읽으면 앞 종목과 뒤 종목의 값이 달라져 이 약속이 깨진다.
        //  한 판에 싣는 줄 수를 실제에 맞춘다 — 값이 실리는 종목은 보유 + 미체결이고 슬롯 상한이 20~30이라
        //  수십 줄이다. 64줄이면 300ms에 읽기가 16만~27만 번 들어간다. 2,000줄로 넓혀 재면 같은 300ms에
        //  44~90번으로 떨어진다 — 아래 읽기 횟수 단언이 그 밀림을 잡는다.
        constexpr symbol::SymbolId kRowsPerRound = 64;

        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        std::atomic<bool>     stop{false};
        std::atomic<uint64_t> reads{0};
        std::atomic<uint64_t> torn{0};

        std::thread writer([&snapshot, &stop]
        {
            for (int32_t round = 1; !stop.load(std::memory_order_relaxed); ++round)
            {
                snapshot.begin_publish();

                for (symbol::SymbolId id = 1; id <= kRowsPerRound; ++id)
                {
                    snapshot.row_for_write(id).position = round;
                }

                snapshot.end_publish();

                // 판 사이에 한 번 양보한다. 쉼 없이 내면 읽는 쪽에 진행 보장이 없다 — 판 번호가 짝수인
                //  순간을 못 잡고 계속 되읽어, 양보가 없던 때는 64줄에서도 읽기가 61번까지 떨어졌다.
                //  실제 발행은 주문·체결 건마다라 판 사이가 이보다 훨씬 넓다. 양보를 넣어도 쓰는 창은
                //  그대로라 반쪽 판을 잡는 힘은 안 줄어든다 — 판 번호 재확인을 없앤 음성 대조가 3번 다
                //  이 시험에서 걸렸고, 울타리가 빠졌을 때 실제로 반쪽 판을 잡아낸 것도 이 시험이다.
                std::this_thread::yield();
            }
        });

        std::thread reader([&snapshot, &stop, &reads, &torn]
        {
            auto ids  = std::make_unique<symbol::SymbolId[]>(kRowsPerRound);
            auto rows = std::make_unique<ipc::LedgerRow[]>(kRowsPerRound);

            while (!stop.load(std::memory_order_relaxed))
            {
                const size_t taken = snapshot.collect_rows(ids.get(), rows.get(), kRowsPerRound);
                reads.fetch_add(1, std::memory_order_relaxed);

                for (size_t slot = 1; slot < taken; ++slot)
                {
                    if (rows[slot].position != rows[0].position)
                    {
                        torn.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        reader.join();

        check(reads.load() > 100, "읽기가 충분히 돌았다(" + std::to_string(reads.load()) + "번)");
        check(torn.load() == 0, "반쪽 판을 한 번도 안 읽었다");
    }

    // ── ⑧ 판 번호 ───────────────────────────────────────────────────────
    {
        // 사본 한 판은 295KB다 — 스택에 얹으면 시험 하나가 여러 판을 쓸 때 프레임이 넘친다.
        //  실제로도 Engine이 make_unique로 잡아 쓴다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        check(snapshot.generation() == 0, "처음은 0");

        snapshot.begin_publish();
        snapshot.end_publish();
        check(snapshot.generation() == 1, "한 판 내면 1");

        snapshot.begin_publish();
        snapshot.end_publish();
        check(snapshot.generation() == 2, "두 판 내면 2");
    }

    // ── ⑨ 판 전체를 벡터로 받아 오는 자리 ───────────────────
    {
        // collect_all_rows는 64칸으로 시작해 모자라면 실제 수만큼 키워 한 번 더 읽는다. 호출부가
        //  kMaxSymbols(295KB)짜리 버퍼를 들고 있지 않아도 전부 받게 하려고 둔 자리다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;

        constexpr symbol::SymbolId kWide = 200; // 첫 짐작(64)보다 많다

        snapshot.begin_publish();

        for (symbol::SymbolId id = 1; id <= kWide; ++id)
        {
            snapshot.row_for_write(id).position = static_cast<int32_t>(id);
        }

        snapshot.end_publish();

        std::vector<symbol::SymbolId> ids;
        std::vector<ipc::LedgerRow>   rows;
        ipc::collect_all_rows(snapshot, ids, rows);
        check(ids.size() == kWide && rows.size() == kWide, "첫 짐작보다 많아도 전부 담는다");
        check(ids[0] == 1 && rows[0].position == 1, "첫 줄이 짝이 맞다");
        check(ids[kWide - 1] == kWide && rows[kWide - 1].position == static_cast<int32_t>(kWide),
              "마지막 줄도 짝이 맞다");

        // 같은 버퍼를 다시 쓴다 — 지난 판의 줄이 남으면 이미 판 종목을 아직 보유로 본다.
        publish_one(snapshot, 7, 5, 0, 5, 1000.0);
        ipc::collect_all_rows(snapshot, ids, rows);
        check(ids.size() == 1 && ids[0] == 7 && rows[0].position == 5, "다음 판은 그 판의 줄만 담는다");
    }

    // ── ⑩ 판을 연 채 멈춘 쓰는 쪽 ───────────────────────────────────────
    {
        // 예전에는 판 번호가 홀수로 남으면 읽는 쪽이 끝없이 돌았다. 지금은 kStableReadDeadline 뒤에
        //  보수값(보유 없음·자리 없음·정지)을 돌려주고 stale_read_count를 올린다.
        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;
        publish_one(snapshot, 3, 10, 0, 10, 5000.0);
        check(snapshot.stale_read_count() == 0, "정상 판에서는 보수값을 안 돌려준다");

        snapshot.begin_publish(); // end_publish 없이 둔다 — 쓰는 쪽이 멈춘 상황
        const auto started = std::chrono::steady_clock::now();

        const ipc::LedgerRow     row     = snapshot.row(3);
        const ipc::EntryView     view    = snapshot.entry(3);
        const ipc::LedgerGlobals globals = snapshot.globals();
        symbol::SymbolId         ids[4]{};
        ipc::LedgerRow           rows[4]{};
        const size_t             taken   = snapshot.collect_rows(ids, rows, 4);

        const auto elapsed = std::chrono::steady_clock::now() - started;
        check(elapsed < std::chrono::seconds(5), "판이 안 닫혀도 읽기가 돌아온다");
        check(row.position == 0 && row.stamp == 0, "row는 빈 줄");
        check(view.capacity_full && view.position == 0, "entry는 자리 없음");
        check(globals.entry_halted == 1 && globals.manual_sell_halted == 1 && globals.capacity_full == 1,
              "globals는 진입·매도 정지와 자리 없음");
        check(taken == 0, "collect_rows는 0줄");
        check(snapshot.stale_read_count() == 4, "보수값을 돌려준 횟수를 센다(" + std::to_string(snapshot.stale_read_count()) + ")");

        snapshot.row_for_write(3).position = 12;
        snapshot.end_publish();
        check(snapshot.row(3).position == 12, "판을 닫으면 다시 제 값을 읽는다");
        check(!snapshot.entry(3).capacity_full, "판을 닫으면 자리 판정도 제 값");
    }

    // ── ⑪ 기한과 정상 판 사이의 거리 ─────────────────────────────────────
    {
        // 기한은 정상 판을 쓰는 시간보다 충분히 길어야 한다 — 짧으면 멀쩡한 판에서 보수값(진입·매도 정지)이 나온다.
        //  실제 판(보유 + 미체결, 수십 줄)과 상한(kNone을 뺀 8,191줄 전부)을 각각 1,000번 재서, 최대가 기한의
        //  1/10보다 작은지 본다. 기한 근처에서 읽기가 실제로 돌아오는지도 잰다.
        constexpr int              kMeasureCount   = 1000;
        constexpr symbol::SymbolId kTypicalRows    = 30;
        constexpr symbol::SymbolId kAllRows        = static_cast<symbol::SymbolId>(ipc::LedgerSnapshot::kMaxSymbols - 1);
        constexpr auto             kHeadroomLimit  = ipc::LedgerSnapshot::kStableReadDeadline / 10;
        constexpr double           kReturnSlackFactor = 4.0; // 기한 뒤 돌아오기까지 허용하는 폭(200ms)
        const double               deadline_milliseconds =
            std::chrono::duration<double, std::milli>(ipc::LedgerSnapshot::kStableReadDeadline).count();
        const double               limit_microseconds =
            std::chrono::duration<double, std::micro>(kHeadroomLimit).count();

        auto                 snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
        ipc::LedgerSnapshot& snapshot        = *snapshot_holder;

        const PublishTiming typical = measure_publish(snapshot, kTypicalRows, kMeasureCount, limit_microseconds);
        const PublishTiming full    = measure_publish(snapshot, kAllRows, kMeasureCount, limit_microseconds);

        std::cout << std::fixed << std::setprecision(1)
                  << "  [사본 한 판 쓰는 시간] " << kMeasureCount << "회씩, 기한 "
                  << ipc::LedgerSnapshot::kStableReadDeadline.count() << "ms\n"
                  << "    줄 수    평균(us)  중앙값(us)     p99(us)    최대(us)  5ms 넘음\n";

        for (const auto& [row_count, timing] : {std::pair{kTypicalRows, typical}, std::pair{kAllRows, full}})
        {
            std::cout << "    " << std::setw(5) << row_count << std::setw(12) << timing.average_microseconds
                      << std::setw(12) << timing.median_microseconds << std::setw(12) << timing.p99_microseconds
                      << std::setw(12) << timing.max_microseconds << std::setw(10) << timing.over_limit_count << "\n";
        }

        // 실제 판(수십 줄)은 최대까지 기한의 1/10 안이어야 한다. 8,191줄은 쓰는 일 자체(p99)만 본다 — 한 판이
        //  수백 마이크로초라 그 사이 스레드가 내려지는 회차가 생기고, 그 최대는 기계 부하가 정한다(표에만 남긴다).
        check(typical.max_microseconds < limit_microseconds, "30줄 판 최대가 기한의 1/10 미만");
        check(full.p99_microseconds < limit_microseconds, "8,191줄 판 p99가 기한의 1/10 미만");

        // 판을 연 채 멈춘 쓰는 쪽 — 읽기 한 번이 기한을 채우고, 기한을 크게 넘기지 않고 돌아오는가.
        snapshot.begin_publish();
        const auto     started = std::chrono::steady_clock::now();
        const uint64_t before  = snapshot.stale_read_count();
        (void)snapshot.row(3);
        const double waited_milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        snapshot.end_publish();

        std::cout << "  [판이 안 닫힌 채 읽기 한 번] " << waited_milliseconds << "ms\n";
        check(snapshot.stale_read_count() == before + 1, "멈춘 판에서는 보수값을 한 번 돌려준다");
        check(waited_milliseconds >= deadline_milliseconds, "기한을 채우고 돌아온다");
        check(waited_milliseconds < deadline_milliseconds * kReturnSlackFactor, "기한을 크게 넘기지 않고 돌아온다(기한의 4배 미만)");
    }

    std::cout << "test_ledger_snapshot: " << g_checks << " checks passed\n";
    return 0;
}
