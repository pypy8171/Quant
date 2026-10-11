#pragma once
// test_engine 공용 준비 — 단언 매크로·시험용 피드 소스·첫 틱 매수 전략 둘과 케이스 함수 선언.
//  케이스 본문은 test_engine_<주제>.cpp에 있고, test_engine.cpp의 main이 원래 순서대로 부른다.

#include "core/Engine.h"
#include "core/IFeedSource.h"
#include "core/ShardMatrix.h"
#include "core/TickCapture.h"
#include "ipc/OrderChannel.h"
#include "strategy/StrategyBase.h"
#include "universe/MarketBoard.h"
#include "universe/ScoreWeight.h"
#include "universe/UniverseScanner.h"
#include "utils/GenerationCopy.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// 실패하면 파일·줄을 찍고 케이스 함수에서 1을 돌려준다. 통과한 단언 수는 g_checks에 센다.
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

namespace engine_test
{

extern int g_checks;

// 콜백을 받아 두고 테스트가 부르는 대로 틱을 내보내는 피드 소스. 연결·구독은 기록만 하고 성공이라 답한다.
//  lanes()를 N으로 답해 Engine이 행렬 행을 N개 잡게 한다 — 실제 소켓 N개(FeedMux 직접 호출 모드)와 같은 자리.
class FakeFeed : public feed::IFeedSource
{
public:
    explicit FakeFeed(uint32_t lanes);
    uint32_t lanes() const override;
    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override;
    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override;
    bool connect(const std::vector<WatchSpec>& specifications) override;
    void disconnect() override;
    bool subscribe_incremental(const WatchSpec& specification) override;
    bool has_specification(const WatchSpec& specification) const override;
    std::vector<WatchSpec> take_overflow_specifications() override;
    bool                   is_connected() const override;
    bool                   is_stale(int) const override;

    // 수신 스레드가 디코드 직후 부르는 자리 — 여기서는 테스트 스레드가 수신 스레드 lane의 역할을 한다.
    void emit_trade(uint32_t lane, const std::string& ticker, double price, int32_t hhmmss);
    std::vector<WatchSpec> specifications() const;

private:
    uint32_t               lanes_;
    mutable std::mutex     mutex_;
    std::vector<WatchSpec> specifications_;
    std::atomic<bool>      connected_{false};
    OrderBookCb            on_order_book_;
    TradeCb                on_trade_;
    LaneOrderBookCb        lane_order_book_;
    LaneTradeCb            lane_trade_;
};

// 자기 종목의 첫 틱에 시장가 1주 매수 한 번. 이후는 침묵. 종목 비교는 id로(원칙 6).
class BuyOnce : public StrategyBase
{
public:
    explicit BuyOnce(std::string ticker);
    const std::string& id() const override;
    std::string describe() const override;
    std::optional<OrderSignal> on_data(const MarketData&) override;
    void on_start() override;
    std::vector<WatchSpec> get_watch_specifications() const override;
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;
    symbol::SymbolId    symbol_id() const;
    std::atomic<int> ticks_seen{0};

private:
    std::string   ticker_;
    std::string   id_;
    symbol::SymbolId symbol_id_{};
    bool          fired_ = false;
};

// 전략 하나가 종목 여럿을 본다 — 종목마다 첫 틱에 1주 매수. 샤드가 둘 이상일 때 이 전략의 종목이 여러 열로 흩어지면
//  옛 설계는 샤드를 1로 내렸다. 지금은 전략이 샤드 하나를 갖고 종목 틱이 그 샤드로 온다. [why D-110]
class BuyEachOnce : public StrategyBase
{
public:
    explicit BuyEachOnce(std::vector<std::string> tickers);
    const std::string& id() const override;
    std::string describe() const override;
    std::optional<OrderSignal> on_data(const MarketData&) override;
    void on_start() override;
    std::vector<WatchSpec> get_watch_specifications() const override;
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;
    const std::vector<symbol::SymbolId>& symbol_ids() const;

private:
    std::vector<std::string>      tickers_;
    std::string                   id_;
    std::vector<symbol::SymbolId> symbol_ids_;
    std::vector<uint8_t>          fired_;
};

// ─── 케이스 — 0이면 통과, 아니면 실패 코드. test_engine.cpp의 main이 부른다 ───
// test_engine_pipeline.cpp
int run_spanning_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers);
int run_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers);
// test_engine_order_path.cpp
int run_stale_entry_case();
int run_protective_claim_case();
int run_replay_case();
// test_engine_roles.cpp
int run_symbol_role_case();
int run_switch_role_case();
int run_manual_order_case();
int run_split_start_case();
// test_engine_universe.cpp
int run_score_spread_case();
int run_score_tie_order_case();
int run_quote_table_reload_case();
// test_engine_generation_copy.cpp
int run_generation_copy_case();

} // namespace engine_test
