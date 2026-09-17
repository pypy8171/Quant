// tools/manual_order.cpp
// 수동 주문 도구 — 사람이 직접 주문을 넣어 접수→체결을 확인한다.
//
//   전략 자동주문이 아니라 DMA 클라이언트 한 명(=사람)이 주문을 인테이크에 넣는
//   실제(비벤치) 경로. OrderGate(리스크) → KIS submit_order_acknowledgement(접수, ODNO) →
//   get_balance 폴링(체결=보유수량 변화)까지 한 흐름으로 확인한다.
//
//   안전: is_paper=true(모의계좌)에서만 실행된다. 실거래 config면 즉시 중단.
//
//   사용법:
//     manual_order <config> <buy|sell> <ticker> <quantity> [price] [market|limit]
//   예)
//     manual_order config/config_paper.json buy  005930 1            (시장가 매수 1주)
//     manual_order config/config_paper.json buy  005930 1 70000 limit (지정가 70000 매수)
//     manual_order config/config_paper.json sell 005930 1            (시장가 매도 1주)

#include "api/KisClient.h"
#include "core/Types.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

static std::string mask(const std::string& text)
{
    return (text.size() > 4) ? text.substr(0, 4) + std::string(text.size() - 4, '*') : "****";
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 5)
    {
        std::cout << "사용법: manual_order <config> <buy|sell> <ticker> <qty> [price] [market|limit]\n"
                     "예) manual_order config/config_paper.json buy 005930 1\n"
                     "    manual_order config/config_paper.json buy 005930 1 70000 limit\n";
        return 1;
    }

    const std::string config_path = argv[1];
    const std::string side_s = argv[2];
    const std::string ticker = argv[3];
    const int quantity = std::atoi(argv[4]);
    const double price = (argc > 5) ? std::atof(argv[5]) : 0.0;
    const std::string type_s = (argc > 6) ? argv[6] : (price > 0 ? "limit" : "market");

    // ── config 로드 ──────────────────────────────────────────────────────────
    std::ifstream file(config_path);

    if (!file)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }

    json config = json::parse(file);

    KisConfig kis_config;
    kis_config.app_key      = config["kis"]["app_key"];
    kis_config.app_secret   = config["kis"]["app_secret"];
    kis_config.account_no   = config["kis"]["account_no"];
    kis_config.account_type = config["kis"]["account_type"].get<std::string>();
    kis_config.hts_id       = config["kis"].value("hts_id", "");
    kis_config.is_paper     = config["kis"]["is_paper"].get<bool>();

    // ── ★ 안전 게이트: 모의계좌 아니면 거부 ──────────────────────────────────
    if (!kis_config.is_paper)
    {
        std::cerr << "[중단] is_paper=false (실거래 config). 이 도구는 모의계좌 전용입니다.\n"
                     "       config/config_paper.json 을 쓰거나 is_paper=true로 설정하세요.\n";
        return 2;
    }

    const OrderSide side = (side_s == "sell" || side_s == "SELL") ? OrderSide::SELL : OrderSide::BUY;
    const OrderType type = (type_s == "limit") ? OrderType::LIMIT : OrderType::MARKET;

    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.type        = type;
    signal.quantity    = quantity;
    signal.price       = price;
    signal.strategy_id = "MANUAL";
    signal.market      = Market::KR;
    signal.account_id  = kis_config.account_no; // 계좌별 원장에 실제 계좌로 파티션

    // ── 주문 양식 출력 (KIS 요청 본문) ───────────────────────────────────────
    std::cout << "=== 수동 주문 (모의계좌 " << mask(kis_config.account_no) << ") ===\n";
    std::cout << "종목=" << ticker << "  " << (side == OrderSide::BUY ? "매수" : "매도")
              << "  수량=" << quantity << "  유형=" << (type == OrderType::MARKET ? "시장가" : "지정가")
              << "  가격=" << (type == OrderType::LIMIT ? std::to_string(static_cast<int>(price)) : "-") << "\n";
    std::cout << "KIS 주문 본문(양식):\n"
              << "  CANO=" << mask(kis_config.account_no) << "  ACNT_PRDT_CD=" << kis_config.account_type
              << "  PDNO=" << ticker << "\n"
              << "  ORD_DVSN=" << (type == OrderType::MARKET ? "01(시장가)" : "00(지정가)")
              << "  ORD_QTY=" << quantity << "  ORD_UNPR=" << (type == OrderType::LIMIT ? static_cast<int>(price) : 0) << "\n"
              << "  tr_id=" << (side == OrderSide::BUY ? "VTTC0802U(모의매수)" : "VTTC0801U(모의매도)") << "\n\n";

    // ── [1] 인증 ─────────────────────────────────────────────────────────────
    KisClient kis(kis_config);

    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }

    std::cout << "[1] 인증 완료\n";

    // ── [2] 리스크 게이트 (FEP 경로) ─────────────────────────────────────────
    // 게이트를 기본 생성만 하면 이 도구가 링크한 시점의 기본값을 그대로 쓴다. 그 값이
    //  config와 어긋나면 정상 주문이 막힌다(09-09 15:07 강제청산에서 22주 매도가
    //  "1주문 수량 한도 초과 (22 > 0)"으로 거부됐다). 운영자가 직접 내는 단발 주문이므로
    //  주문 단위 한도만 config에서 실어 준다 — 보유·노출 한도는 엔진이 따로 본다.
    OrderGate gate;
    {
        const json risk_json = config.value("risk", json::object());
        OrderGate::Config gate_config;
        gate_config.max_quantity_per_order      = risk_json.value("max_qty_per_order", 10000);
        gate_config.max_notional_per_order = risk_json.value("max_notional_per_order", 50000000.0);
        gate_config.max_quantity_per_ticker     = risk_json.value("max_qty_per_ticker", 4000);
        gate_config.max_orders_per_min     = risk_json.value("max_orders_per_min", 20);
        gate_config.max_orders_per_sec     = risk_json.value("max_orders_per_sec", 5);
        gate.set_config(gate_config);
    }

    std::string reason;

    if (!gate.check(signal, reason))
    {
        std::cerr << "[중단] OrderGate 거부: " << reason << "\n";
        return 4;
    }

    std::cout << "[2] OrderGate 통과\n";

    // ── [3] 접수 (submit_order_acknowledgement → ODNO) ───────────────────────────────────────
    const OrderAck acknowledgement = kis.submit_order_acknowledgement(signal);

    if (!acknowledgement.ok())
    {
        std::cerr << "[중단] 주문 접수 실패 [" << acknowledgement.error_code << "] (로그의 KIS msg 확인)\n";
        return 5;
    }

    const std::string& kis_order_no = acknowledgement.kis_order_no;

    gate.on_accept(signal.account_id, ticker, side, quantity, price); // 미체결 선점(원장)
    std::cout << "[3] 접수 완료 — ODNO=" << kis_order_no << "\n";

    // ── [4] 체결 확인 (잔고 폴링) ────────────────────────────────────────────
    //   시장가 주문은 장중이면 곧 체결된다. 지정가/장외 시간이면 미체결일 수 있음.
    std::cout << "[4] 체결 확인 (잔고 2초 간격 폴링, 최대 20초)...\n";
    bool seen = false;

    for (int index = 0; index < 10; ++index)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const KisResult<AccountBalance> balance = kis.get_balance();

        if (!balance)
        {
            std::cout << "    [" << (index + 1) * 2 << "s] 잔고 조회 실패(" << error_text(balance) << ")\n";
            continue;
        }

        for (const Holding& holding : balance->holdings)
        {
            if (holding.ticker == ticker)
            {
                std::cout << "    [" << (index + 1) * 2 << "s] 보유수량=" << holding.quantity << "  매입평균=" << holding.average_price
                          << "  평가손익=" << holding.evaluation_pnl << "\n";
                seen = true;
            }
        }

        if (seen)
        {
            break;
        }
    }

    if (!seen)
    {
        std::cout << "    (아직 보유수량에 안 잡힘 — 장외 시간/지정가 미체결이거나 매도로 청산됐을 수 있음)\n";
    }

    std::cout << "=== 완료 ===\n";
    return 0;
}
