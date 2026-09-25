// 유니버스 재스캔 장부(UniverseRescan) 단위 테스트. 스캔 함수·전략 공장·등록/떼기 콜백을 가짜로 끼워 KIS 없이
//  신규 등록·주기 대기·상한 교체·빈 스캔 무시·차단→해제·복귀 확인을 시각을 손으로 넘기며 고정한다. 관련 결정: D-077·D-087.
#include "api/KisClient.h"
#include "core/UniverseRescan.h"
#include "ipc/LedgerSnapshot.h"
#include "strategy/StrategyBase.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

class FakeStrategy : public StrategyBase
{
public:
    explicit FakeStrategy(symbol::SymbolId symbol) : symbol_(symbol), id_("fake_" + std::to_string(symbol)) {}

    const std::string& id() const override { return id_; }
    std::string describe() const override { return id_; }
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    symbol::SymbolId symbol() const { return symbol_; }

private:
    symbol::SymbolId symbol_;
    std::string      id_;
};

// Engine 대신 전략 목록을 쥐는 자리. 등록을 받으면 Engine처럼 set_registered(true)를 되부른다.
struct Harness
{
    symbol::SymbolTable                        table{64};
    std::vector<std::unique_ptr<StrategyBase>> live;
    std::vector<std::string>                   retired_lines;
    std::vector<symbol::SymbolId>              next_scan;
    std::unique_ptr<UniverseRescan>            rescan;

    Harness()
    {
        rescan = std::make_unique<UniverseRescan>(
            table,
            [this](std::unique_ptr<StrategyBase> strategy)
            {
                rescan->set_registered(static_cast<FakeStrategy&>(*strategy).symbol(), true);
                live.push_back(std::move(strategy));
            },
            [this](StrategyBase* pointer, const std::function<std::string(const StrategyBase&)>& make_line)
            {
                retired_lines.push_back(make_line(*pointer));
                std::erase_if(live, [pointer](const std::unique_ptr<StrategyBase>& strategy) { return strategy.get() == pointer; });
            });
        rescan->reset_registered();
    }

    void add_job(size_t max_registered, int block_after_sec, int drop_after_sec, int return_confirm = 2)
    {
        UniverseRescan::Job job;
        job.universe_fn     = [this](KisClient&) { return next_scan; };
        job.factory         = [](symbol::SymbolId symbol) { return std::make_unique<FakeStrategy>(symbol); };
        job.interval_sec    = 10;
        job.max_registered  = max_registered;
        job.block_after_sec = block_after_sec;
        job.drop_after_sec  = drop_after_sec;
        job.return_confirm  = return_confirm;
        rescan->add_job(std::move(job));
    }

    StrategyBase* strategy_of(symbol::SymbolId symbol) const
    {
        for (const auto& strategy : live)
        {
            if (static_cast<const FakeStrategy&>(*strategy).symbol() == symbol)
            {
                return strategy.get();
            }
        }

        return nullptr;
    }
};

// 스캔 결과의 새 종목에 전략이 붙고, 주기 안에서 다시 부르면 스캔 함수를 부르지 않는다.
void check_register_and_interval(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(0, 0, 0);
    const auto start = Clock::now();

    harness.next_scan = {3, 5};
    assert(harness.rescan->run(client, snapshot, start));
    assert(harness.rescan->is_registered(3) && harness.rescan->is_registered(5));
    assert(harness.live.size() == 2);

    harness.next_scan = {7};
    assert(!harness.rescan->run(client, snapshot, start + std::chrono::seconds(5)));
    assert(!harness.rescan->is_registered(7));

    assert(harness.rescan->run(client, snapshot, start + std::chrono::seconds(10)));
    assert(harness.rescan->is_registered(7));
    assert(harness.rescan->shortest_interval_sec(30) == 10);
    assert(harness.rescan->shortest_interval_sec(5) == 5);
}

// 상한이 찼으면 오늘 스캔에 없는 미보유 종목 하나(부재가 같으면 id가 작은 쪽)가 자리를 내준다.
void check_cap_eviction(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(2, 0, 0);
    const auto start = Clock::now();

    harness.next_scan = {3, 5};
    harness.rescan->run(client, snapshot, start);
    harness.next_scan = {9};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(10));

    assert(harness.retired_lines.size() == 1);
    assert(harness.retired_lines[0].find("fake_3") != std::string::npos);
    assert(!harness.rescan->is_registered(3));
    assert(harness.rescan->is_registered(5) && harness.rescan->is_registered(9));
    assert(harness.live.size() == 2);
}

// 빈 스캔은 이탈 근거가 아니다 — 오래 비어도 차단·해제하지 않는다. 순위표도 직전 것을 둔다.
void check_empty_scan_ignored(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(0, 20, 40);
    const auto start = Clock::now();

    harness.next_scan = {4, 6};
    harness.rescan->run(client, snapshot, start);
    harness.next_scan = {};

    for (int round = 1; round <= 10; ++round)
    {
        harness.rescan->run(client, snapshot, start + std::chrono::seconds(10 * round));
    }

    assert(harness.retired_lines.empty());
    assert(harness.strategy_of(4)->in_universe() && harness.strategy_of(6)->in_universe());

    const std::vector<int32_t> rank = harness.rescan->scan_rank(16);
    assert(rank[4] == 0 && rank[6] == 1 && rank[5] == -1);
}

// 연속 부재가 block_after_sec에 닿으면 신규매수를 막고, drop_after_sec에 닿으면 뗀다.
//  부재 시계는 마지막으로 보인 스캔 시각부터 잰다.
void check_block_then_drop(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(0, 20, 40);
    const auto start = Clock::now();

    harness.next_scan = {4};
    harness.rescan->run(client, snapshot, start);
    StrategyBase* owned = harness.strategy_of(4);

    harness.next_scan = {8};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(10)); // 부재 10초
    assert(owned->in_universe());

    harness.rescan->run(client, snapshot, start + std::chrono::seconds(20)); // 부재 20초 → 차단
    assert(!owned->in_universe());
    assert(harness.rescan->is_registered(4));

    harness.rescan->run(client, snapshot, start + std::chrono::seconds(40)); // 부재 40초 → 해제
    assert(harness.retired_lines.size() == 1);
    assert(harness.retired_lines[0].find("재스캔 이탈 해제") != std::string::npos);
    assert(!harness.rescan->is_registered(4));
}

// 차단된 종목은 return_confirm회 연속으로 보여야 풀린다 — 한 번 보였다가 빠지면 다시 센다.
void check_return_confirm(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(0, 20, 1000, 2);
    const auto start = Clock::now();

    harness.next_scan = {4};
    harness.rescan->run(client, snapshot, start);
    StrategyBase* owned = harness.strategy_of(4);

    harness.next_scan = {8};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(20));
    assert(!owned->in_universe());

    harness.next_scan = {4};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(30));
    assert(!owned->in_universe());

    harness.next_scan = {8};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(40));
    harness.next_scan = {4};
    harness.rescan->run(client, snapshot, start + std::chrono::seconds(50));
    assert(!owned->in_universe());

    harness.rescan->run(client, snapshot, start + std::chrono::seconds(60));
    assert(owned->in_universe());
}

// 기동 유니버스는 마지막 슬리브의 소유가 되고 상한 셈에 든다.
void check_seed(KisClient& client, const ipc::LedgerSnapshot& snapshot)
{
    Harness harness;
    harness.add_job(1, 0, 0);

    auto started = std::make_unique<FakeStrategy>(2);
    harness.rescan->set_registered(2, true);
    harness.rescan->seed({{2, started.get()}}, {2});
    harness.live.push_back(std::move(started));

    harness.next_scan = {6};
    harness.rescan->run(client, snapshot, Clock::now());

    // 상한 1이 기동 종목으로 차 있어 2가 자리를 내주고 6이 들어간다.
    assert(harness.retired_lines.size() == 1);
    assert(!harness.rescan->is_registered(2) && harness.rescan->is_registered(6));
}
} // namespace

int main()
{
    KisClient client(KisConfig{});
    // 판이 한 번도 발행되지 않은 사본 — 보유·선점이 모두 0이다. 크기가 커서 힙에 둔다.
    const auto snapshot_holder = std::make_unique<ipc::LedgerSnapshot>();
    const ipc::LedgerSnapshot& snapshot = *snapshot_holder;

    check_register_and_interval(client, snapshot);
    check_cap_eviction(client, snapshot);
    check_empty_scan_ignored(client, snapshot);
    check_block_then_drop(client, snapshot);
    check_return_confirm(client, snapshot);
    check_seed(client, snapshot);

    std::cout << "test_universe_rescan: 전부 통과\n";
    return 0;
}
