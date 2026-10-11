#pragma once
// test_order_router 공용 준비 — KIS 응답을 흉내 내는 스텁, 신호·설정·원장 CSV 읽기 도우미, 주제별 시험 묶음 선언.
//  시험 본문은 test_order_router_<주제>.cpp에 있고, test_order_router.cpp의 main이 묶음을 차례로 부른다.

#include "api/IOrderExecutor.h"
#include "api/KisErrorCodes.h"
#include "core/TransportPool.h"
#include "ipc/OrderRouter.h"
#include "risk/OrderGate.h"
#include "utils/Logger.h"
#include <atomic>
#include <filesystem>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// ─── 잠금으로 지키는 시험 값 ─────────────────────────────────────────────────
//  스텁의 문자열·목록은 시험 본문(메인 스레드)이 바꾸고 라우터 스레드(재확인·기동 취소·전송 풀)가 읽는다.
//  대입·추가·비우기와 읽기(사본)를 같은 잠금 아래에서 한다. 시험 본문은 대입 문법을 그대로 쓴다.
template <typename Value>
class GuardedValue
{
public:
    GuardedValue() = default;

    // 중괄호 목록 대입(stub.open_orders = {first, second})이 여기로 온다.
    GuardedValue& operator=(Value value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::move(value);
        return *this;
    }

    // 문자열 상수 대입은 Value의 대입 연산자를 그대로 쓴다.
    template <typename Source>
    GuardedValue& operator=(const Source& source)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = source;
        return *this;
    }

    template <typename Item>
    void push_back(Item&& item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.push_back(std::forward<Item>(item));
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.clear();
    }

    [[nodiscard]] Value copy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

private:
    mutable std::mutex mutex_;
    Value              value_;
};

// ─── KIS 응답 시뮬레이션 Stub ─────────────────────────────────────────────────
//  [inv] 라우터 스레드(재확인·기동 취소·전송 풀)도 이 스텁을 부른다. 수와 참거짓은 atomic, 문자열·목록은 GuardedValue로 둔다.
struct StubOrderExecutor : IOrderExecutor
{
    std::atomic<bool>         succeed;
    GuardedValue<std::string> kis_order_no;
    std::atomic<int>          call_count{0};
    // MM-1 확장 — 취소/정정 경로 추적
    GuardedValue<std::string> orgno;                   // submit_order_ack가 반환할 조직번호(생성자에서 ORG000001)
    std::atomic<bool>         cancel_ok{true};         // cancel_order 성공 여부
    std::atomic<bool>         revise_ok{true};         // revise_order 성공 여부
    std::atomic<int>          cancel_calls{0};         // 재확인 스레드도 올린다
    std::atomic<int>          revise_calls{0};
    std::atomic<int>          last_cancel_quantity{-1}; // 마지막 취소에 전달된 quantity(잔량 재계산 검증)
    // C-2 청산차단 경로 — 다음 fail_next건은 err_code로 실패, 그 뒤 성공
    std::atomic<bool>         paper{false};
    std::atomic<int>          fail_next{0};
    GuardedValue<std::string> error_code;
    // 재기동 대조 — get_open_orders가 돌려줄 KIS 미체결
    GuardedValue<std::vector<OpenOrder>> open_orders;
    std::atomic<int>                     open_order_calls{0};     // 재확인 스레드도 올린다
    std::atomic<bool>                    open_orders_fail{false}; // 미체결 조회 실패(한 쪽이라도 못 받음)
    // 놓친 체결 되찾기 — get_daily_order_fills가 돌려줄 당일 체결 누적
    GuardedValue<std::vector<DailyOrderFill>> daily_fills;
    std::atomic<int>                          daily_fill_calls{0};
    std::atomic<bool>                         daily_fills_fail{false};

    explicit StubOrderExecutor(bool flag, std::string output = "0000000042");

    bool is_paper() const noexcept override;
    KisResult<std::vector<OpenOrder>> get_open_orders() override;
    KisResult<std::vector<DailyOrderFill>> get_daily_order_fills() override;
    OrderAck submit_order_acknowledgement(const OrderSignal&) override;
    OrderAck cancel_order(const std::string&, const std::string&, const std::string&,
                          int quantity, bool) override;
    OrderAck revise_order(const std::string&, const std::string&, const std::string&,
                          int, double) override;
};

// ─── 헬퍼 ─────────────────────────────────────────────────────────────────────
OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity = 1);
OrderGate::Config relaxed_config();

// 오늘 원장 CSV의 마지막 n줄(헤더 제외). 행 검증용.
std::vector<std::string> tail_trade_rows(size_t count);
std::vector<std::string> split_csv(const std::string& line);
void PASS(const std::string& name);

// KIS 미체결 한 건과, 리플레이가 되쌓는 모양의 INTENT 하나(선점 포함).
OpenOrder make_open_order(const std::string& ticker, OrderSide side, int quantity, double price,
                          const std::string& kis_order_no);
OrderGate::OpenIntent reserve_intent(OrderGate& gate, uint64_t order_id, uint64_t kis_order_number, int quantity);

// ─── 주제별 시험 묶음 — test_order_router.cpp의 main이 아래 순서로 부른다 ───────────────
void run_reconcile_tests();
void run_submit_tests();
void run_fill_tests();
void run_send_tests();
void run_cancel_tests();
void run_journal_tests();
void run_blocked_sell_tests();
void run_restart_tests();
void run_recover_tests();
void run_session_tests();
