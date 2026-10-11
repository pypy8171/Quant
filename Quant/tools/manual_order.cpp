// tools/manual_order.cpp
// 수동 주문 도구 — 사람이 직접 주문을 넣어 접수→체결을 확인한다.
//
//   전략 자동주문이 아니라 DMA 클라이언트 한 명(=사람)이 주문을 인테이크에 넣는
//   실제(비벤치) 경로. OrderGate(리스크) → KIS submit_order_acknowledgement(접수, ODNO) →
//   get_balance 폴링(체결=보유수량 변화)까지 한 흐름으로 확인한다.
//
//   안전: 모의계좌(is_paper=true)는 그냥 돈다. 실계좌는 --live 를 손으로 적었을 때만 돌고,
//         그때도 지정가 · 5만원 이내만 받는다. 엔진을 띄우기 전에 "이 계좌로 주문이 접수되는가"를
//         한 건으로 재는 것이 실계좌를 여는 이유다.
//
//   --cancel 은 접수 직후 그 주문을 거둔다. 체결될 수 없는 가격과 함께 쓰면 "이 계좌로 주문이
//   접수되는가"만 재고 미체결을 남기지 않는다.
//
//   사용법:
//     manual_order <config> <buy|sell> <ticker> <quantity> [price] [market|limit] [--live] [--cancel]
//   예)
//     manual_order config/config_paper.json buy  005930 1            (시장가 매수 1주)
//     manual_order config/config_paper.json buy  005930 1 70000 limit (지정가 70000 매수)
//     manual_order config/config_paper.json sell 005930 1            (시장가 매도 1주)
//     manual_order Quant/config/config_live.json buy 201490 1 2610 limit --live  (실계좌 지정가 1주)

#include "api/KisClient.h"
#include "core/AppConfig.h"
#include "core/Types.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static std::string mask(const std::string& text)
{
    return (text.size() > 4) ? text.substr(0, 4) + std::string(text.size() - 4, '*') : "****";
}

// 실계좌 주문 금액 상한(원). 이 도구는 경로 확인용이라 그보다 큰 주문은 엔진으로 낸다.
constexpr double kLiveNotionalCap = 50000.0;

// 명령행 인자. 뜻은 파일 머리의 "사용법".
struct ManualOrderArguments
{
    bool        allow_live                   = false; // [inv] argv 에 "--live" 가 있을 때만 true
    bool        cancel_after_acknowledgement = false;
    std::string config_path;
    std::string side_text;
    std::string ticker;
    int         quantity = 0;
    double      price    = 0.0;
    std::string type_text;
};

// --live · --cancel 은 위치 인자가 아니다. 먼저 걷어내고 나머지를 순서대로 읽는다. 인자가 모자라면 사용법을 찍고 false.
static bool read_arguments(int argc, char** argv, ManualOrderArguments& parsed)
{
    std::vector<std::string> arguments;

    for (int index = 0; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--live")
        {
            parsed.allow_live = true;
            continue;
        }

        if (argument == "--cancel")
        {
            parsed.cancel_after_acknowledgement = true;
            continue;
        }

        arguments.emplace_back(argument);
    }

    if (arguments.size() < 5)
    {
        std::cout << "사용법: manual_order <config> <buy|sell> <ticker> <qty> [price] [market|limit] [--live]\n"
                     "예) manual_order config/config_paper.json buy 005930 1\n"
                     "    manual_order config/config_paper.json buy 005930 1 70000 limit\n"
                     "    manual_order config/config_live.json  buy 201490 1 2610 limit --live --cancel  (실계좌 경로 확인)\n";
        return false;
    }

    parsed.config_path = arguments[1];
    parsed.side_text   = arguments[2];
    parsed.ticker      = arguments[3];
    parsed.quantity    = std::atoi(arguments[4].c_str());
    parsed.price       = (arguments.size() > 5) ? std::atof(arguments[5].c_str()) : 0.0;
    parsed.type_text   = (arguments.size() > 6) ? arguments[6] : (parsed.price > 0 ? "limit" : "market");
    return true;
}

// ── ★ 안전 게이트: 실계좌는 --live 를 손으로 적었을 때만 ──────────────────
//   엔진을 띄우기 전에 "이 계좌로 주문이 접수되는가"를 한 건으로 재는 자리다. 실계좌를 아예
//   막아 두면 그 확인을 할 수 없어, 거부 사유를 장중에야 알게 된다. 대신 실수로 큰 돈이 나가지
//   않도록 세 가지를 건다 — 플래그를 손으로 적을 것, 지정가일 것, 금액이 kLiveNotionalCap 이내일 것.
//   통과하면 0, 막으면 종료코드 2.
static int check_live_guard(const KisConfig& kis_config, bool allow_live, OrderType type, double price, int quantity)
{
    if (kis_config.is_paper)
    {
        return 0;
    }

    if (!allow_live)
    {
        std::cerr << "[중단] is_paper=false (실계좌 config). 실계좌로 내려면 --live 를 붙이세요.\n"
                     "       예) manual_order Quant/config/config_live.json buy 201490 1 2610 limit --live\n";
        return 2;
    }

    if (type != OrderType::LIMIT)
    {
        std::cerr << "[중단] 실계좌에서는 지정가만 받습니다 — 가격과 limit 을 적으세요.\n"
                     "       사람이 내는 단발 주문에 시장가를 허용하면 오타 한 번이 그대로 체결됩니다.\n";
        return 2;
    }

    const double notional = price * quantity;

    if (notional > kLiveNotionalCap)
    {
        std::cerr << "[중단] 실계좌 주문 금액 " << static_cast<long long>(notional) << "원이 이 도구의 상한 "
                  << static_cast<long long>(kLiveNotionalCap) << "원을 넘습니다.\n"
                     "       이 도구는 경로 확인용입니다. 그보다 큰 주문은 엔진으로 내세요.\n";
        return 2;
    }

    return 0;
}

// ── 주문 양식 출력 (KIS 요청 본문) ───────────────────────────────────────
static void print_order_form(const KisConfig& kis_config, const OrderSignal& signal)
{
    const OrderSide side  = signal.side;
    const OrderType type  = signal.type;
    const double    price = signal.price;

    std::cout << "=== 수동 주문 (" << (kis_config.is_paper ? "모의계좌 " : "실계좌 ")
              << mask(kis_config.account_no) << ") ===\n";
    std::cout << "종목=" << signal.ticker << "  " << (side == OrderSide::BUY ? "매수" : "매도")
              << "  수량=" << signal.quantity << "  유형=" << (type == OrderType::MARKET ? "시장가" : "지정가")
              << "  가격=" << (type == OrderType::LIMIT ? std::to_string(static_cast<int>(price)) : "-") << "\n";
    std::cout << "KIS 주문 본문(양식):\n"
              << "  CANO=" << mask(kis_config.account_no) << "  ACNT_PRDT_CD=" << kis_config.account_type
              << "  PDNO=" << signal.ticker << "\n"
              << "  ORD_DVSN=" << (type == OrderType::MARKET ? "01(시장가)" : "00(지정가)")
              << "  ORD_QTY=" << signal.quantity << "  ORD_UNPR=" << (type == OrderType::LIMIT ? static_cast<int>(price) : 0) << "\n"
              << "  tr_id=" << (kis_config.is_paper ? (side == OrderSide::BUY ? "VTTC0012U(모의매수)" : "VTTC0011U(모의매도)")
                                                    : (side == OrderSide::BUY ? "TTTC0012U(실매수)" : "TTTC0011U(실매도)"))
              << "\n\n";
}

// ── [3-1] 접수만 확인하고 거두기(--cancel) ───────────────────────────────
//   "이 계좌로 주문이 접수되는가"만 재는 쓰임이다. 체결될 수 없는 가격으로 내고 곧바로
//   거두므로 미체결이 남지 않는다. 남기면 엔진이 기동할 때 open_orders.txt 에 없는 주문이라
//   아무도 취소하지 않는다. 돌려주는 값은 종료코드(성공 0, 취소 실패 6).
static int cancel_and_report(KisClient& kis, const OrderAck& acknowledgement, const std::string& ticker, int quantity)
{
    const std::string& kis_order_no = acknowledgement.kis_order_no;

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const OrderAck cancel = kis.cancel_order(ticker, kis_order_no, acknowledgement.krx_forwarding_org_no,
                                             quantity, true);

    if (!cancel.ok())
    {
        std::cerr << "[경고] 취소 실패 [" << cancel.error_code << "] — 미체결 주문 " << kis_order_no
                  << " 가 남아 있다. HTS 나 scripts 로 즉시 거둘 것.\n";
        return 6;
    }

    std::cout << "[4] 취소 완료 — 접수번호 " << cancel.kis_order_no << "\n"
              << "[판정] 이 계좌로 주문이 접수되고 취소된다. 엔진을 띄워도 된다.\n";
    return 0;
}

// ── [4] 체결 확인 (잔고 폴링) ────────────────────────────────────────────
//   시장가 주문은 장중이면 곧 체결된다. 지정가/장외 시간이면 미체결일 수 있음.
static void poll_fill(KisClient& kis, const std::string& ticker)
{
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
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    ManualOrderArguments parsed;

    if (!read_arguments(argc, argv, parsed))
    {
        return 1;
    }

    const std::string& ticker   = parsed.ticker;
    const int          quantity = parsed.quantity;
    const double       price    = parsed.price;

    // ── config 로드 ──────────────────────────────────────────────────────────
    // 엔진과 같은 parse_config로 읽는다(규약 8.3). 이 도구만 json을 따로 읽으면 risk 칸이 빠졌을 때
    //  엔진과 다른 기본값(예: 종목당 4,000주)으로 주문이 나간다.
    AppConfig app;

    try
    {
        app = parse_config(load_config_file(parsed.config_path));
    }
    catch (const std::exception& error)
    {
        std::cerr << "[중단] config 읽기 실패: " << parsed.config_path << "\n"
                  << "       - error(" << error.what() << ")\n";
        return 1;
    }

    const KisConfig& kis_config = app.kis;

    const OrderSide side = (parsed.side_text == "sell" || parsed.side_text == "SELL") ? OrderSide::SELL : OrderSide::BUY;
    const OrderType type = (parsed.type_text == "limit") ? OrderType::LIMIT : OrderType::MARKET;

    const int guard_exit_code = check_live_guard(kis_config, parsed.allow_live, type, price, quantity);

    if (guard_exit_code != 0)
    {
        return guard_exit_code;
    }

    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.type        = type;
    signal.quantity    = quantity;
    signal.price       = price;
    signal.strategy_id = "MANUAL";
    signal.market      = Market::KR;
    signal.account_id  = kis_config.account_no; // 계좌별 장부에 실제 계좌로 파티션

    print_order_form(kis_config, signal);

    // ── [1] 인증 ─────────────────────────────────────────────────────────────
    KisClient kis(kis_config);

    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패 (앱키/시크릿 확인)\n";
        return 3;
    }

    std::cout << "[1] 인증 완료\n";

    // ── [2] 리스크 게이트 (FEP 경로) ─────────────────────────────────────────
    // 한도는 엔진과 같은 값을 쓴다 — parse_config가 채운 app.risk 전체(주문 단위·종목 보유·일손실·
    //  세션 창 포함)를 그대로 싣는다. 09-09 15:07 강제청산에서 기본 생성 게이트가 22주 매도를
    //  "1주문 수량 한도 초과 (22 > 0)"으로 막은 일이 있어 기본 생성으로 두지 않는다.
    OrderGate gate;
    gate.set_config(app.risk);

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

    gate.ledger().on_accept(signal.account_id, ticker, side, quantity, price); // 미체결 선점(장부)
    std::cout << "[3] 접수 완료 — ODNO=" << acknowledgement.kis_order_no << "\n";

    if (parsed.cancel_after_acknowledgement)
    {
        return cancel_and_report(kis, acknowledgement, ticker, quantity);
    }

    poll_fill(kis, ticker);
    std::cout << "=== 완료 ===\n";
    return 0;
}
