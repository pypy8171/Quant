// 리플레이용 모의 체결기 — OrderRouter가 KIS 대신 주문을 넣는 IOrderExecutor. 주문은 다음 틱에 체결되고
//  체결통보는 라이브와 같은 콜백으로 나간다. 잔고 대조기에는 자기 장부를 돌려준다.
// 스레드: submit/cancel/revise는 주문 스레드, on_tick은 피드 스레드, balance는 제어 스레드. mutex_ 하나로 지킨다.
//  체결통보 콜백은 on_tick(피드 스레드)에서만 부른다 — fill_queue_의 생산자를 하나로 두기 위해.
//  장부·대기 주문은 SymbolId로 인덱스한 배열이다 — on_tick은 수신 스레드에서 틱마다 도니 문자열 생성·해시가
//  없어야 한다(원칙 3·6). 문자열 티커는 주문·취소·잔고처럼 드문 경로에서만 SymbolTable로 푼다. 실측(09-20,
//  2,700종목·대기 주문 100건·무작위 틱): 문자열 키 맵 37 ns/틱 → id 배열 14.5 ns/틱, 남은 건 mutex다. [why D-071]
#pragma once
#include "api/IOrderExecutor.h"
#include "api/KisErrorCodes.h"
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/MarketSession.h"
#include "core/SymbolTable.h"
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace feed
{

class PaperExecutor final : public IOrderExecutor
{
public:
    using FillCb = std::function<void(const FillNotification&)>;

    // symbols는 엔진의 테이블과 같은 것이어야 한다 — 틱에 찍힌 id와 주문 티커를 푼 id가 같은 번호 체계여야 장부가 맞는다.
    PaperExecutor(double initial_cash, symbol::SymbolTable& symbols)
        : symbols_(symbols), cash_(initial_cash), initial_cash_(initial_cash)
    {
    }

    void set_fill_callback(FillCb callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_fill_ = std::move(callback);
    }

    // 접수만 한다. 체결은 그 종목의 다음 틱(on_tick)에서 — 시장가는 틱 가격, 지정가는 가격이 닿을 때.
    //  매도는 보유수량에서 대기 매도를 뺀 만큼만 받고(부족하면 KIS와 같은 40240000), 매수는 현금 한도.
    [[nodiscard]] OrderAck submit_order_acknowledgement(const OrderSignal& signal) override;

    [[nodiscard]] OrderAck cancel_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int quantity, bool all_remaining) override;

    [[nodiscard]] OrderAck revise_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int new_quantity, double new_price) override;

    [[nodiscard]] bool is_paper() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::vector<OpenOrder> get_open_orders() override;

    // 피드 스레드가 틱마다 부른다. 그 종목의 대기 주문을 접수 순서대로 보고 조건이 맞으면 체결·통보한다.
    //  콜백은 락을 놓고 부른다(콜백이 큐 push라 짧지만 락 안에서 남의 코드를 부르지 않는다).
    //  대기 주문이 없는 종목은 배열 한 칸 보고 돌아간다 — 문자열도 해시도 만들지 않는다. id가 안 찍힌 틱
    //  (테스트·옛 경로)만 테이블로 푼다.
    void on_tick(const TradeData& trade);

    // 잔고 대조기(LedgerReconciler::FetchBalance)용 — KIS 잔고 자리에 자기 장부를 준다.
    [[nodiscard]] KisResult<AccountBalance> balance() const;

    [[nodiscard]] double cash() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cash_;
    }

    [[nodiscard]] uint64_t fills() const noexcept
    {
        return fills_;
    }

    [[nodiscard]] size_t open_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_orders_;
    }

private:
    struct Pending
    {
        std::string kis_order_no;
        OrderSignal signal;
    };

    static bool crosses(const OrderSignal& signal, double price);

    void apply_fill_locked(const OrderSignal& signal, double price);

    // 그 종목의 대기 목록. 없으면 배열을 늘려 만든다 — 주문 경로에서만 부른다(락 안).
    std::vector<Pending>& pending_slot_locked(symbol::SymbolId symbol_id);

    int pending_sell_locked(symbol::SymbolId symbol_id) const;

    int sellable_locked(symbol::SymbolId symbol_id) const;

    // 대기 매수의 명목 합. 시장가는 ref_price로 잰다(0이면 한도에 안 잡힌다 — 게이트가 먼저 거른다).
    double reserved_cash_locked() const;

    Pending* find_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no);

    void erase_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no);

    // 모의 ODNO — 실전문처럼 자릿수 10개(라우터가 정수로 바꿔 색인한다, D-112). 앞자리 9는 모의 표시.
    std::string next_odno_locked();

    symbol::SymbolTable& symbols_;
    mutable std::mutex   mutex_;
    FillCb               on_fill_;

    std::vector<std::vector<Pending>>             pending_;    // [symbol_id] → 접수 순서. 빈 칸이 대부분이다
    std::unordered_map<symbol::SymbolId, Holding> book_;       // 보유 종목만
    std::vector<double>                           last_price_; // [symbol_id]. 0이면 아직 틱 없음
    size_t                                        open_orders_ = 0; // [inv] pending_ 안 항목 수의 합
    double                                        cash_;
    double                                        initial_cash_;
    uint64_t                                      next_odno_ = 1;
    std::atomic<uint64_t>                         fills_{0};
};

} // namespace feed
