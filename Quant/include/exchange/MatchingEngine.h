// 부하시험용 거래소 매칭 엔진 — 종목별 오더북에 주문을 쌓고(동시호가), 단일가로 한 번에 체결하고(개장),
//  그 뒤로는 들어오는 주문을 즉시 맞춘다(연속매매). 실매매 경로와 겹치지 않는다 — `feed::PaperExecutor`와
//  형제이고 서로를 모른다. 이 파일이 있다고 해서 엔진 동작이 달라지지 않는다.
// 스레드: 종목 하나의 오더북은 한 번에 한 스레드만 만진다는 전제다(종목 해시로 샤딩한 뒤 부르거나, 단일 스레드).
//  내부에 락이 없다 — 부하시험에서 재려는 것이 매칭 비용이지 락 경합이 아니라서다. [why D-071 원칙 2]
#pragma once
#include "core/SymbolTable.h"
#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace exchange
{

// 가격은 원 단위 정수다. 국내 주식 호가는 모두 정수 원이라 실수로 들 이유가 없고, 정수라야 격자 인덱스와
//  1:1로 맞아 "같은 가격인데 다른 레벨"이 생기지 않는다.
using PriceKrw = int64_t;

// 시장가 주문의 가격 자리. 지정가 0원은 없으므로 0을 시장가 표식으로 쓴다.
inline constexpr PriceKrw kMarketOrderPrice = 0;

// 오더북 주문 한 건이 놓인 자리 번호. 종목 저장소 안의 첨자다. 없음은 kNoSlot.
using OrderSlot = uint32_t;
inline constexpr OrderSlot kNoSlot = static_cast<OrderSlot>(-1);

// 오더북에 남아 있는 주문 한 건. 가격은 자기가 속한 레벨이 안다.
struct RestingOrder
{
    uint64_t  order_id           = 0;
    int32_t   remaining_quantity = 0;
    OrderSlot next_slot          = kNoSlot; // 같은 레벨의 다음 주문 자리. 16바이트 정렬 자리를 여기에 쓴다
};

// 들어오는 주문 한 건. 인젝터가 보내는 것도, 엔진 전략이 내는 것도 이 모양으로 들어온다.
struct IncomingOrder
{
    uint64_t         order_id  = 0;
    symbol::SymbolId symbol_id = symbol::kNone;
    PriceKrw         price_krw = kMarketOrderPrice;
    int32_t          quantity  = 0;
    OrderSide        side      = OrderSide::NONE;
};

// 맞은 주문 한 쌍. 매칭 엔진이 부르는 콜백으로 나간다.
struct Execution
{
    uint64_t         buy_order_id  = 0;
    uint64_t         sell_order_id = 0;
    symbol::SymbolId symbol_id     = symbol::kNone;
    PriceKrw         price_krw     = 0;
    int32_t          quantity      = 0;
};

// 한 가격에 줄 서 있는 주문들. 먼저 온 것이 먼저 체결된다(시간 우선).
//  주문 자체는 종목 저장소에 있고 여기는 줄의 앞뒤 자리만 안다 — 레벨마다 배열을 따로 들면 레벨이 종목당
//  수백 개라 빈 배열의 관리 비용과 늘어날 때의 복사가 다 든다. 꺼낼 때 앞을 지우지 않고 first_slot만 민다.
struct PriceLevel
{
    OrderSlot first_slot     = kNoSlot; // 다음에 체결될 자리. 이 앞은 이미 다 체결됐다
    OrderSlot last_slot      = kNoSlot; // 새 주문을 붙일 자리
    int64_t   total_quantity = 0;       // [inv] first_slot부터 이어진 주문들의 remaining_quantity 합
};

// 한 종목의 오더북. 가격을 호가단위 격자의 정수 인덱스로 바꿔 배열로 들고 있다 — 가격마다 맵을 찾으면
//  주문 한 건에 해시가 한 번 붙는데, 여기는 초당 수십만 건이 지나는 자리다.
class SymbolBook
{
public:
    // 기준가(전일 종가 자리)를 받아 상하한가 ±30% 구간의 호가 격자를 만든다. 같은 종목에 두 번 부르면 비운다.
    void configure(PriceKrw reference_price_krw);

    // 격자를 만들지 않은 종목은 주문을 받지 않는다 — 어느 가격이 유효한지 모르는 채로 쌓으면 균형가가 틀린다.
    [[nodiscard]] bool ready() const
    {
        return !level_price_.empty();
    }

    [[nodiscard]] PriceKrw reference_price_krw() const
    {
        return reference_price_krw_;
    }

    [[nodiscard]] size_t level_count() const
    {
        return level_price_.size();
    }

    // 레벨 인덱스의 가격. 범위 밖이면 0.
    [[nodiscard]] PriceKrw price_of_level(size_t level_index) const;

    // 가격이 격자의 몇 번째인지. 격자에 없는 가격(호가단위에 안 맞거나 상하한가 밖)이면 kNoLevel.
    [[nodiscard]] size_t level_of_price(PriceKrw price_krw) const;

    static constexpr size_t kNoLevel = static_cast<size_t>(-1);

    // 맞추지 않고 쌓기만 한다(동시호가 접수). 격자 밖 가격이면 버리고 false.
    bool accumulate(const IncomingOrder& order);

    // 쌓인 호가로 단일가를 정한다. 체결 가능 수량이 없으면 0을 돌려준다.
    //  [formula] 순서대로 거른다 — ① 체결 가능 수량 최대 ② 남는 잔량(매수누적-매도누적의 절댓값) 최소
    //  ③ 기준가에 가장 가까운 가격. 유가증권시장 업무규정 제23조(단일가격에 의한 개별경쟁매매)와 같은 순서다.
    [[nodiscard]] PriceKrw find_auction_price() const;

    // 단일가로 한 번에 맞춘다. 그 가격 이상 매수·이하 매도 중 min(누적 매수, 누적 매도)만큼이 그 한 가격에 체결된다.
    //  남는 쪽은 가격·시간 우선으로 자른다.
    //  돌려주는 값은 체결 수량 합. 체결마다 on_execution을 부른다.
    int64_t run_auction(const std::function<void(const Execution&)>& on_execution);

    // 연속매매 — 반대편 최우선호가부터 맞추고, 남으면 오더북에 쌓는다. 돌려주는 값은 이번에 맞은 수량.
    int64_t match(const IncomingOrder& order, const std::function<void(const Execution&)>& on_execution);

    // 지금 최우선 매수호가. 없으면 0.
    [[nodiscard]] PriceKrw best_bid_krw() const;

    // 지금 최우선 매도호가. 없으면 0.
    [[nodiscard]] PriceKrw best_ask_krw() const;

    // 아직 안 맞은 주문 건수(양쪽 합). 부하시험이 "얼마나 쌓였나"를 보는 자리라 세어 둔다.
    [[nodiscard]] int64_t resting_count() const
    {
        return resting_count_;
    }

    // 양쪽에 쌓인 수량 합.
    [[nodiscard]] int64_t resting_quantity() const
    {
        return buy_market_.total_quantity + sell_market_.total_quantity + resting_limit_quantity_;
    }

    // 오더북만 비운다 — 격자는 그대로 두어 다음 회차에 다시 만들지 않는다.
    void clear_orders();

private:
    // 가격 P에 매수 주문을 내면 체결될 수 있는 누적 수량(P 이상 지정가 매수 + 시장가 매수 전부).
    [[nodiscard]] int64_t cumulative_buy_at(size_t level_index) const;

    // 가격 P에 매도 주문을 내면 체결될 수 있는 누적 수량(P 이하 지정가 매도 + 시장가 매도 전부).
    [[nodiscard]] int64_t cumulative_sell_at(size_t level_index) const;

    // 한쪽 레벨에서 수량만큼 꺼내 상대 주문과 맞춘다. 실제로 꺼낸 수량을 돌려준다.
    //  지정가 총잔량(resting_limit_quantity_)은 건드리지 않는다 — 시장가 줄에도 그대로 쓰는 함수라
    //  돌려준 수량을 보고 부른 쪽이 뺀다.
    int64_t take_from_level(PriceLevel& level, int32_t wanted_quantity, uint64_t counterparty_order_id,
                            OrderSide resting_side, PriceKrw execution_price,
                            const std::function<void(const Execution&)>& on_execution);

    // 주문 한 건을 저장소에 놓고 레벨 줄 끝에 붙인다. 저장소가 꽉 찼으면 false.
    bool push_resting(PriceLevel& level, uint64_t order_id, int32_t quantity);

    // 저장소에 빈 자리 하나를 내어 값을 채운다. 자리가 없으면 kNoSlot.
    OrderSlot new_slot(uint64_t order_id, int32_t quantity);

    [[nodiscard]] RestingOrder& slot_at(OrderSlot slot)
    {
        return order_blocks_[slot >> kOrderBlockShift][slot & kOrderBlockMask];
    }

    PriceKrw              reference_price_krw_ = 0;
    std::vector<PriceKrw> level_price_; // 레벨 인덱스 → 가격. 오름차순이라 이진탐색으로 역변환한다

    std::vector<PriceLevel> buy_levels_;  // [레벨] 지정가 매수
    std::vector<PriceLevel> sell_levels_; // [레벨] 지정가 매도

    // 시장가는 격자에 자리가 없다 — 가격을 안 정한 주문이라 따로 줄 세운다. 줄의 생김새는 레벨과 같다.
    PriceLevel buy_market_;
    PriceLevel sell_market_;

    // 주문 자리 저장소. 배열 하나로 들면 다 찰 때마다 두 배로 늘며 쓰던 자리를 통째로 베끼는데, 그 순간
    //  잡는 메모리가 쓰는 양의 두 배다(2.7억 건 4.3GB → 8.6GB, 2026-09-26 10만건 회차가 여기서 멈췄다).
    //  덩어리로 끊어 잡으면 한 번 잡은 덩어리는 다시 옮기지 않는다.
    static constexpr size_t kOrderBlockShift = 13;                        // 덩어리당 8,192건 = 128KB
    static constexpr size_t kOrderBlockSize  = size_t(1) << kOrderBlockShift;
    static constexpr size_t kOrderBlockMask  = kOrderBlockSize - 1;

    std::vector<std::unique_ptr<RestingOrder[]>> order_blocks_;
    size_t                                       order_slot_count_ = 0;

    // 최우선호가를 매번 전 격자에서 찾지 않으려고 들고 있는 경계. 비었으면 kNoLevel.
    size_t highest_buy_level_ = kNoLevel;
    size_t lowest_sell_level_ = kNoLevel;

    int64_t resting_count_         = 0;
    int64_t resting_limit_quantity_ = 0;
};

// 종목 여럿의 오더북을 묶어 들고 있는 것. 종목 id를 배열 인덱스로 쓴다(원칙 6 — hot path에 문자열 없음).
class MatchingEngine
{
public:
    using ExecutionCb = std::function<void(const Execution&)>;

    // 종목 수를 미리 잡아 둔다. id는 1부터라 한 칸 더 잡는다.
    void reserve(size_t symbol_count);

    // 종목의 기준가를 정하고 격자를 만든다. 주문을 받기 전에 종목마다 한 번 불러야 한다.
    void configure_symbol(symbol::SymbolId symbol_id, PriceKrw reference_price_krw);

    void set_execution_callback(ExecutionCb callback)
    {
        on_execution_ = std::move(callback);
    }

    // 접수만 하고 맞추지 않는다(동시호가). 받았으면 true.
    bool accumulate(const IncomingOrder& order);

    // 모든 종목을 단일가로 맞춘다. 돌려주는 값은 체결 수량 합.
    int64_t run_auction_all();

    // 종목 하나를 단일가로 맞춘다.
    int64_t run_auction(symbol::SymbolId symbol_id);

    // 연속매매 — 즉시 맞추고 남으면 쌓는다. 돌려주는 값은 이번에 맞은 수량.
    int64_t match(const IncomingOrder& order);

    [[nodiscard]] SymbolBook* book_of(symbol::SymbolId symbol_id);

    [[nodiscard]] const SymbolBook* book_of(symbol::SymbolId symbol_id) const;

    [[nodiscard]] uint64_t accepted_count() const
    {
        return accepted_count_;
    }

    [[nodiscard]] uint64_t rejected_count() const
    {
        return rejected_count_;
    }

    [[nodiscard]] uint64_t execution_count() const
    {
        return execution_count_;
    }

    [[nodiscard]] int64_t executed_quantity() const
    {
        return executed_quantity_;
    }

    // 쌓여 있는 주문 건수 합.
    [[nodiscard]] int64_t resting_count() const;

private:
    std::vector<SymbolBook> books_; // [symbol_id]. 0번 칸은 symbol::kNone 자리라 안 쓴다
    ExecutionCb             on_execution_;

    uint64_t accepted_count_   = 0;
    uint64_t rejected_count_   = 0;
    uint64_t execution_count_  = 0;
    int64_t  executed_quantity_ = 0;
};

} // namespace exchange
