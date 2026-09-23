// 운영단말 서버 — 사람이 단말에서 보낸 조회·수동 주문을 받는 자리.
//  Engine 클래스는 그대로다. Engine.cpp 가 4,400줄을 넘겨 열기 어려워져 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  수동 주문의 생산자는 여기 서버 스레드, 소비자는 order_thread_fn 이다. 둘을 잇는 ops_.manual_inbox 는
//  Engine 의 멤버로 그대로 두었다 — 주문을 내는 주체가 갈리지 않게 한다. [why D-043][why D-114]

#include "core/Engine.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

// ─── 운영단말(OpsServer) 배선 ─────────────────────────────────────────────
//  서버 스레드에서 불리는 콜백은 큐에 넣거나 스냅샷을 읽기만 한다. 주문은 order_thread가
//  take_manual_order에서 OrderSignal로 바꿔 그 자리에서 발주 사슬에 올린다. 단말은 주문 쪽에만 뜬다
//  (initialize_order_router가 연다) — 전략 프로세스가 멎어도 사람이 손으로 낼 수 있어야 한다. [why D-043][why D-114]

void Engine::start_ops_server()
{
    if (ops_.port <= 0)
    {
        return;
    }

    ops_.server = std::make_unique<OpsServer>();
    ops_.server->set_bind(ops_.bind_address, ops_.port);
    ops_.server->set_token(ops_.token);
    ops_.server->set_paper(kis_config_.is_paper);
    ops_.server->set_status_provider([this] { return ops_status_json(); });
    ops_.server->set_positions_provider([this] { return ops_positions_json(); });
    ops_.server->set_kill_handler(
        [this]
        {
            LOG_WARN("[Ops] KILL — 신규 주문 차단 + 엔진 종료");
            request_kill_switch(true);
            write_state_marker("kill_today", "운영단말 KILL");
            request_shutdown("운영단말 KILL");
        });
    ops_.server->set_shutdown_handler(
        [this](const std::string& who)
        {
            // 킬과 다른 길이다 — 킬스위치도 표지 파일도 건드리지 않는다. 표지를 남기면 감시견이
            //  그날 내내 재기동을 거부해(scripts/auto_trade_day.ps1) 배포가 매매를 하루 멈춘다. [why D-114]
            LOG_WARN("[Ops] SHUTDOWN — 곱게 내린다(배포 교체) " + who);
            request_shutdown("운영단말 SHUTDOWN — " + who, ipc::SharedShutdownReason::kOperator);
        });
    ops_.server->set_halt_handler(
        [this](const std::string& side, bool on)
        {
            LOG_WARN(std::string("[Ops] HALT_REQ — 수동 ") + (side == "SELL" ? "전략 매도 정지 " : "진입 정지 ") + (on ? "ON" : "OFF"));
            request_manual_halt(side == "SELL" ? OrderSide::SELL : OrderSide::BUY, on);
        });
    ops_.server->set_halt_provider(
        [this] { return std::make_pair(order_gate_.is_manual_buy_halted(), order_gate_.is_manual_sell_halted()); });
    ops_.server->set_order_handler([this](const OpsOrderReq& ops_order_request) { return accept_manual_order(ops_order_request); });

    if (!ops_.server->start())
    {
        ops_.server.reset();
    }
}

std::string Engine::ops_status_json() const
{
    // 계좌 요약. equity·cash·daily_pnl은 잔고 대조 주기(브로커 값)로만 바뀌고, position_value·unrealized_pnl은
    //  보유분 × 최근 체결가라 틱마다 움직인다 — 단말이 1초마다 물어도 셋은 그대로일 수 있다.
    double position_value = 0.0;
    double unrealized_pnl = 0.0;

    // 사본 한 판을 훑는다 — 줄마다 주문 쪽 잠금을 잡던 것이 없어진다. 단말이 1초마다 물어도
    //  주문·체결 스레드를 세우지 않는다. [why D-114]
    std::vector<symbol::SymbolId> ledger_ids;
    std::vector<ipc::LedgerRow>   ledger_rows;
    ipc::collect_all_rows(*ledger_snapshot_, ledger_ids, ledger_rows);

    for (size_t index = 0; index < ledger_rows.size(); ++index)
    {
        const ipc::LedgerRow& row  = ledger_rows[index];
        const double          last = last_price(ledger_ids[index]);

        if (row.position <= 0 || last <= 0.0)
        {
            continue;
        }

        position_value += row.position * last;
        unrealized_pnl += row.position * (last - row.average_price);
    }

    return nlohmann::json{{"running", running_.load()},
                          {"data", data_count_.load()},
                          {"signal", signal_count_.load()},
                          {"order", order_count_.load()},
                          {"kill", order_gate_.is_killed()},
                          {"entry_halt", ledger_snapshot_->globals().entry_halted != 0},
                          {"manual_buy_halt", order_gate_.is_manual_buy_halted()},
                          {"manual_sell_halt", order_gate_.is_manual_sell_halted()},
                          {"force_liq", force_liquidate_.load(std::memory_order_relaxed)},
                          {"paper", kis_config_.is_paper},
                          // 전략을 올리지 않는 주문 전용 프로세스에서는 제 목록이 항상 비어 있다 — 그때는 공유 이름표의
                          //  등록 수를 대신 싣는다. 전략 쪽이 올린 수에 고정 이름(MANUAL·FORCE_LIQ·LIMIT_TRIM·UNLINKED·
                          //  DISPLACE) 몇이 더해진 값이라 딱 맞아떨어지지는 않고, "전략이 올라왔나"를 보는 데 쓴다. [why D-114]
                          {"strategies", runs_strategy_side() ? strategy_.list.size() : order_gate_.strategy_table().size()},
                          {"equity", order_gate_.equity()},
                          {"cash", order_gate_.available_cash()},
                          {"daily_pnl", order_gate_.daily_pnl()},
                          {"position_value", position_value},
                          {"unrealized_pnl", unrealized_pnl}}
        .dump();
}

std::string Engine::ops_positions_json() const
{
    nlohmann::json array = nlohmann::json::array();

    // 보유와 미체결 선점을 한 판에서 함께 읽는다 — 따로 읽으면 "보유 10, 선점 -10"처럼 서로 다른 순간의
    //  값이 한 줄에 실려 단말이 없는 상태를 본다. [why D-114]
    std::vector<symbol::SymbolId> ledger_ids;
    std::vector<ipc::LedgerRow>   ledger_rows;
    ipc::collect_all_rows(*ledger_snapshot_, ledger_ids, ledger_rows);

    const std::string account = ledger_snapshot_->globals().account;

    for (size_t index = 0; index < ledger_rows.size(); ++index)
    {
        const ipc::LedgerRow& row = ledger_rows[index];

        if (row.position <= 0)
        {
            continue; // 선점만 있는 줄은 보유 목록이 아니다(구 snapshot_positions()와 같은 규칙)
        }

        const std::string ticker = symbols_.table.name(ledger_ids[index]).string();
        array.push_back({{"account", account},
                       {"ticker", ticker},
                       {"name", ticker_name(ticker)},
                       {"qty", row.position},
                       {"avg_price", row.average_price},
                       {"reserved", row.reserved},
                       {"last", last_price(ledger_ids[index])}});
    }

    return nlohmann::json{{"positions", array}}.dump();
}

std::string Engine::accept_manual_order(const OpsOrderReq& ops_order_request)
{
    // 수동주문 입력 상한 — 운영 중 바꿀 값이 아니라 config가 아닌 상수다. cid는 중복 방지 set의 키라 길이를 막고,
    //  수량은 오타를 거르는 선일 뿐 실제 한도는 OrderGate가 본다.
    constexpr size_t kManualOrderClientIdMaxLength = 64;
    constexpr int    kManualOrderMaxQuantity       = 100000;

    if (ops_order_request.client_id.empty() || ops_order_request.client_id.size() > kManualOrderClientIdMaxLength)
    {
        return "cid는 1~64자";
    }

    if (!symbol::is_korean_ticker(ops_order_request.ticker))
    {
        return "ticker는 6자리 숫자";
    }

    if (ops_order_request.side != "SELL" && ops_order_request.side != "BUY")
    {
        return "side는 SELL|BUY";
    }

    if (ops_order_request.quantity <= 0 || ops_order_request.quantity > kManualOrderMaxQuantity)
    {
        return "qty 범위 1~100000";
    }

    if (ops_order_request.price < 0.0 || ops_order_request.reference_price < 0.0)
    {
        return "가격은 0 이상";
    }

    // 재전송 차단: 있나 확인 → 큐에 넣기 → 들어갔을 때만 cid 기록. 세 줄이 한 락 안이라 서버 스레드가
    //  늘어도 같은 cid가 두 번 큐에 들어가지 않는다. [inv] 지금은 OpsServer 스레드 하나만 부른다.
    std::lock_guard<std::mutex> lock(ops_.manual_client_id_mutex);

    if (ops_.manual_cids.count(ops_order_request.client_id) != 0)
    {
        return "중복 cid — 이미 접수";
    }

    if (!ops_.manual_inbox.push(ops_order_request))
    {
        return "수동주문 인테이크 가득 참";
    }

    ops_.manual_cids.insert(ops_order_request.client_id);

    // 꺼내 가는 쪽은 주문 스레드다 — 자고 있으면 여기서 깨운다. [why D-114]
    pipeline_.order_wake.notify();
    return std::string();
}

bool Engine::take_manual_order(OrderSignal& signal)
{
    while (auto request = ops_.manual_inbox.pop())
    {
        const OpsOrderReq& ops_order_request = *request;
        std::string reject;
        double      reference = ops_order_request.reference_price;

        // 단말이 기준가를 안 찍었으면 엔진의 최근 체결가로 채운다 — 시장가 명목 한도가 0으로 새지 않게.
        if (reference <= 0.0)
        {
            reference = last_price(ops_order_request.ticker);
        }

        if (ops_order_request.side == "SELL")
        {
            // 보유 범위 안에서만 — 보유가 없거나 보유를 넘는 요청은 여기서 끊는다. 매도가능(보유−미체결매도)이 0인
            //  것은 거부하지 않고 라우터로 보낸다 — 라우터가 그 종목의 예약매도를 취소해 수량을 풀고 다시 낸다
            //  (청산차단 자가정리). 예전엔 여기서 "매도가능 0"으로 끊어 그 길에 닿지 못했다(09-14 15:00 먼지 정리
            //  3건 중 2건이 예약 익절 때문에 거부). [why D-082]
            int held = 0;
            double average = 0.0;

            // 보유와 평단을 사본 한 줄에서 함께 읽는다. 계좌는 한 판이 한 계좌라 판의 계좌와 맞는지만 본다.
            if (ledger_snapshot_->globals().account == ops_order_request.account)
            {
                const ipc::LedgerRow row = ledger_snapshot_->row(symbols_.table.lookup(ops_order_request.ticker));
                held    = row.position;
                average = row.average_price;
            }

            if (held <= 0)
            {
                reject = "보유 없음";
            }
            else if (ops_order_request.quantity > held)
            {
                reject = "보유 " + std::to_string(held) + " 초과 요청 " + std::to_string(ops_order_request.quantity);
            }

            if (reference <= 0.0)
            {
                reference = average;
            }
        }

        if (!reject.empty())
        {
            LOG_WARN("[Ops] 수동주문 거부 cid=" + ops_order_request.client_id + " " + ops_order_request.ticker + " " + ops_order_request.side + " " + std::to_string(ops_order_request.quantity) +
                     " — " + reject);

            if (ops_.server)
            {
                ops_.server->broadcast(ops::OpsMsg::ORDER_RESULT_NTF,
                                       nlohmann::json{{"cid", ops_order_request.client_id},
                                                      {"order_id", ""},
                                                      {"odno", ""},
                                                      {"strategy", "MANUAL"},
                                                      {"ticker", ops_order_request.ticker},
                                                      {"side", ops_order_request.side},
                                                      {"qty", ops_order_request.quantity},
                                                      {"price", ops_order_request.price},
                                                      {"ok", false},
                                                      {"msg", reject}}
                                           .dump());
            }

            continue;
        }

        OrderSignal built;
        built.ticker      = ops_order_request.ticker;
        built.account_id  = ops_order_request.account;
        built.side        = OrderSide::from_string(ops_order_request.side);
        built.type        = ops_order_request.price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
        built.quantity    = ops_order_request.quantity;
        built.price       = ops_order_request.price;
        built.reference_price   = reference;
        built.strategy_id    = "MANUAL";
        built.strategy_index = manual_strategy_index_;
        built.client_order_id  = ops_order_request.client_id;
        built.client_order_number = next_client_order_number();
        built.reason      = "운영단말 수동주문 cid=" + ops_order_request.client_id;
        built.timestamp   = std::chrono::system_clock::now();
        LOG_INFO("[Ops] 수동주문 → 게이트 cid=" + ops_order_request.client_id + " " + ops_order_request.ticker + " " + ops_order_request.side + " " + std::to_string(ops_order_request.quantity) +
                 (ops_order_request.price > 0.0 ? " @" + std::to_string(static_cast<long long>(ops_order_request.price)) : " 시장가"));
        signal = std::move(built);
        return true;
    }

    return false;
}
