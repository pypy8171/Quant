// tools/manual_order.cpp
// 수동 주문 도구 — 사람이 직접 주문을 넣어 접수→체결을 확인한다.
//
//   전략 자동주문이 아니라 DMA 클라이언트 한 명(=사람)이 주문을 인테이크에 넣는
//   실제(비벤치) 경로. OrderGate(리스크) → KIS submit_order(접수, ODNO) →
//   get_balance 폴링(체결=보유수량 변화)까지 한 흐름으로 확인한다.
//
//   안전: is_paper=true(모의계좌)에서만 실행된다. 실거래 config면 즉시 중단.
//
//   사용법:
//     manual_order <config> <buy|sell> <ticker> <qty> [price] [market|limit]
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

static std::string mask(const std::string& s)
{
    return (s.size() > 4) ? s.substr(0, 4) + std::string(s.size() - 4, '*') : "****";
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
    const int qty = std::atoi(argv[4]);
    const double price = (argc > 5) ? std::atof(argv[5]) : 0.0;
    const std::string type_s = (argc > 6) ? argv[6] : (price > 0 ? "limit" : "market");

    // ── config 로드 ──────────────────────────────────────────────────────────
    std::ifstream f(config_path);
    if (!f)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }
    json cfg = json::parse(f);

    KisConfig kc;
    kc.app_key      = cfg["kis"]["app_key"];
    kc.app_secret   = cfg["kis"]["app_secret"];
    kc.account_no   = cfg["kis"]["account_no"];
    kc.account_type = cfg["kis"]["account_type"].get<std::string>();
    kc.hts_id       = cfg["kis"].value("hts_id", "");
    kc.is_paper     = cfg["kis"]["is_paper"].get<bool>();

    // ── ★ 안전 게이트: 모의계좌 아니면 거부 ──────────────────────────────────
    if (!kc.is_paper)
    {
        std::cerr << "[중단] is_paper=false (실거래 config). 이 도구는 모의계좌 전용입니다.\n"
                     "       config/config_paper.json 을 쓰거나 is_paper=true로 설정하세요.\n";
        return 2;
    }

    const OrderSide side = (side_s == "sell" || side_s == "SELL") ? OrderSide::SELL : OrderSide::BUY;
    const OrderType type = (type_s == "limit") ? OrderType::LIMIT : OrderType::MARKET;

    OrderSignal sig;
    sig.ticker      = ticker;
    sig.side        = side;
    sig.type        = type;
    sig.quantity    = qty;
    sig.price       = price;
    sig.strategy_id = "MANUAL";
    sig.market      = Market::KR;
    sig.account_id  = kc.account_no; // 계좌별 원장에 실제 계좌로 파티션

    // ── 주문 양식 출력 (KIS 요청 본문) ───────────────────────────────────────
    std::cout << "=== 수동 주문 (모의계좌 " << mask(kc.account_no) << ") ===\n";
    std::cout << "종목=" << ticker << "  " << (side == OrderSide::BUY ? "매수" : "매도")
              << "  수량=" << qty << "  유형=" << (type == OrderType::MARKET ? "시장가" : "지정가")
              << "  가격=" << (type == OrderType::LIMIT ? std::to_string((int)price) : "-") << "\n";
    std::cout << "KIS 주문 본문(양식):\n"
              << "  CANO=" << mask(kc.account_no) << "  ACNT_PRDT_CD=" << kc.account_type
              << "  PDNO=" << ticker << "\n"
              << "  ORD_DVSN=" << (type == OrderType::MARKET ? "01(시장가)" : "00(지정가)")
              << "  ORD_QTY=" << qty << "  ORD_UNPR=" << (type == OrderType::LIMIT ? (int)price : 0) << "\n"
              << "  tr_id=" << (side == OrderSide::BUY ? "VTTC0802U(모의매수)" : "VTTC0801U(모의매도)") << "\n\n";

    // ── [1] 인증 ─────────────────────────────────────────────────────────────
    KisClient kis(kc);
    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }
    std::cout << "[1] 인증 완료\n";

    // ── [2] 리스크 게이트 (FEP 경로) ─────────────────────────────────────────
    OrderGate gate; // 기본 config
    std::string reason;
    if (!gate.check(sig, reason))
    {
        std::cerr << "[중단] OrderGate 거부: " << reason << "\n";
        return 4;
    }
    std::cout << "[2] OrderGate 통과\n";

    // ── [3] 접수 (submit_order → ODNO) ───────────────────────────────────────
    std::string odno = kis.submit_order(sig);
    if (odno.empty())
    {
        std::cerr << "[중단] 주문 접수 실패 (ODNO 없음 — 로그의 KIS msg 확인)\n";
        return 5;
    }
    gate.on_accept(sig.account_id, ticker, side, qty, price); // 미체결 선점(원장)
    std::cout << "[3] 접수 완료 — ODNO=" << odno << "\n";

    // ── [4] 체결 확인 (잔고 폴링) ────────────────────────────────────────────
    //   시장가 주문은 장중이면 곧 체결된다. 지정가/장외 시간이면 미체결일 수 있음.
    std::cout << "[4] 체결 확인 (잔고 2초 간격 폴링, 최대 20초)...\n";
    bool seen = false;
    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        json bal = kis.get_balance();
        if (!bal.contains("output1"))
            continue;
        for (auto& h : bal["output1"])
        {
            if (h.value("pdno", "") == ticker)
            {
                std::cout << "    [" << (i + 1) * 2 << "s] 보유수량=" << h.value("hldg_qty", "0")
                          << "  매입평균=" << h.value("pchs_avg_pric", "0")
                          << "  평가손익=" << h.value("evlu_pfls_amt", "0") << "\n";
                seen = true;
            }
        }
        if (seen)
            break;
    }
    if (!seen)
        std::cout << "    (아직 보유수량에 안 잡힘 — 장외 시간/지정가 미체결이거나 매도로 청산됐을 수 있음)\n";

    std::cout << "=== 완료 ===\n";
    return 0;
}
