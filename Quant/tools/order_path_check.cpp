// tools/order_path_check.cpp
// 주문 경로 점검 — 프로세스 시작부터 한 종목의 실제 체결 한 건을 받고, 1주 매수를 내고, 접수·체결 통보를 받기까지를
//  단계마다 steady_clock으로 잰다. 엔진 스레드·게이트는 거치지 않고 엔진이 쓰는 KisClient·KisWebSocket만 쓴다.
//
//   흐름: 인증(토큰) → 실시간 접속(접속키·연결·구독 요청) → 체결통보 구독 확인 → 그 종목 첫 체결 수신
//         → 주문 스레드 깨어남 → 매수 접수 응답(주문번호) → 체결통보 도착
//   첫 체결은 수신 스레드가 받고, 주문은 다른 스레드가 낸다 — 엔진처럼 스레드를 한 번 넘긴다.
//
//   안전: 수량은 1주로 고정한다. 모의계좌(is_paper=true)는 그냥 돈다 — 모의 주문은 정규장(09:00~15:30)에만 체결된다.
//         실계좌는 --live를 손으로 적었을 때만 돌고, 그때는 늘 지정가이며 주문 금액이 kLiveNotionalCap 이내여야 한다.
//         실계좌 지정가는 첫 체결가 +1%를 호가 단위로 올린 값이다. 애프터마켓(16:00~20:00)은 시장가가 없고, 엔진처럼
//         시장가를 내면 KisClient가 현재가를 REST로 한 번 더 묻는다 — 그 왕복이 잰 구간에 섞이지 않게 여기서 값을 정한다.
//   앱키 하나로는 실시간 접속을 하나만 연다. 같은 config로 트레이더가 돌고 있으면 먼저 내린다.
//
//   사용법:
//     order_path_check <config> <ticker> [limit] [--live]
//   예)
//     Quant/build_win/order_path_check Quant/config/config_dev_paper.json 005930          (모의 시장가 1주)
//     Quant/build_win/order_path_check Quant/config/config_dev_paper.json 005930 limit    (모의 첫 체결가 지정가 1주)
//     Quant/build_win/order_path_check Quant/config/config_live.json 010140 --live        (실계좌 지정가 1주)

#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/TickSize.h"
#include "core/Types.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// KisWebSocket.h가 windows.h를 끌어오던 때의 조건(LEAN_AND_MEAN·NOMINMAX·ERROR 해제)을 여기서 직접 맞춘다. [why D-049]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR // wingdi.h — LogLevel::ERROR와 부딪힌다
#endif
#endif

using json = nlohmann::json;

namespace
{
constexpr int kOrderQuantity      = 1;
constexpr double kLiveNotionalCap = 50000.0; // 실계좌 1주 주문 금액 상한(원). manual_order와 같은 값
constexpr double kLivePriceStep   = 0.01;    // 실계좌 지정가 = 첫 체결가 × (1 + 이 값). KisOrder.cpp 애프터마켓 변환과 같은 한 걸음
constexpr int kFirstTradeWaitSec  = 60;
constexpr int kFillWaitSec        = 30;
constexpr double kNanosecondsPerMicrosecond = 1000.0;
constexpr double kNanosecondsPerMillisecond = 1000000.0;

// 수신 스레드의 TradeData::received_ns와 같은 시계다(KisWebSocketParse.cpp recv_now_ns).
int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// REST 응답의 주문번호와 체결통보의 주문번호는 앞자리 0 채움이 다를 수 있어 0을 걷고 비교한다.
std::string without_leading_zeros(const std::string& order_number)
{
    const size_t first = order_number.find_first_not_of('0');
    return (first == std::string::npos) ? std::string() : order_number.substr(first);
}

struct Stage
{
    std::string name;
    int64_t     at_ns = 0; // 0 = 오지 않음
};

// 수신 스레드와 주문 스레드(main)가 같이 보는 것. 모두 mutex 아래에서 읽고 쓴다.
struct SharedState
{
    std::mutex              mutex;
    std::condition_variable changed;
    int64_t                 fill_subscribed_ns = 0;
    int64_t                 first_trade_ns     = 0; // TradeData::received_ns
    double                  first_trade_price  = 0.0;
    int32_t                 first_trade_hhmmss = 0;
    std::vector<std::pair<int64_t, FillNotification>> fills; // (도착 시각, 통보). 접수 응답보다 먼저 올 수 있어 모아 둔다
};

void print_clock_resolution()
{
    const auto tick_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(1)).count();
    std::printf("시계: steady_clock 눈금 %lld ns", static_cast<long long>(tick_nanoseconds));
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::printf(", QPC 주파수 %lld Hz → 실제 해상도 %.0f ns", static_cast<long long>(frequency.QuadPart),
                1e9 / static_cast<double>(frequency.QuadPart));
#endif
    std::printf("\n");
}
} // namespace

int main(int argc, char** argv)
{
    const int64_t process_start_ns = now_ns();
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --live · limit 은 위치 인자가 아니다. 먼저 걷어내고 나머지를 순서대로 읽는다.
    bool                     allow_live = false;
    bool                     use_limit  = false;
    std::vector<std::string> arguments;

    for (int index = 0; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--live")
        {
            allow_live = true;
            continue;
        }

        if (argument == "limit")
        {
            use_limit = true;
            continue;
        }

        arguments.emplace_back(argument);
    }

    if (arguments.size() < 3)
    {
        std::cout << "사용법: order_path_check <config> <ticker> [limit] [--live]\n"
                     "예) Quant/build_win/order_path_check Quant/config/config_dev_paper.json 005930\n";
        return 1;
    }

    const std::string config_path = arguments[1];
    const std::string ticker      = arguments[2];

    std::ifstream file(config_path);

    if (!file)
    {
        std::cerr << "[중단] config 못 엶: " << config_path << "\n";
        return 1;
    }

    const json config = json::parse(file);
    const auto kis_node = config.find("kis");

    if (kis_node == config.end())
    {
        std::cerr << "[중단] config에 kis 블록이 없다\n";
        return 1;
    }

    KisConfig kis_config;
    kis_config.app_key      = kis_node->at("app_key").get<std::string>();
    kis_config.app_secret   = kis_node->at("app_secret").get<std::string>();
    kis_config.account_no   = kis_node->at("account_no").get<std::string>();
    kis_config.account_type = kis_node->at("account_type").get<std::string>();
    kis_config.hts_id       = kis_node->value("hts_id", "");
    kis_config.is_paper     = kis_node->at("is_paper").get<bool>();
    // 실시간 채널(H0STCNT0 / 통합 H0UNCNT0)을 엔진과 같게 고른다(WebSocketClient.cpp kis_unified_feed).
    kis_config.exchange     = kis_node->value("exchange", "KRX");

    // [inv] 실계좌는 argv에 "--live"가 있을 때만 돈다. 그때는 늘 지정가다.
    if (!kis_config.is_paper)
    {
        if (!allow_live)
        {
            std::cerr << "[중단] is_paper=false (실계좌 config). 실계좌로 내려면 --live 를 붙이세요.\n";
            return 2;
        }

        use_limit = true;
    }

    if (kis_config.hts_id.empty())
    {
        std::cerr << "[중단] hts_id가 비어 있다 — 체결통보(H0STCNI9) 구독에 필요하다.\n";
        return 2;
    }

    print_clock_resolution();
    std::vector<Stage> stages;
    stages.push_back({"프로세스 시작", process_start_ns});

    // ── 1. 인증 ──────────────────────────────────────────────────────────────
    KisClient kis(kis_config);

    if (!kis.authenticate())
    {
        std::cerr << "[중단] 인증 실패\n";
        return 3;
    }

    stages.push_back({"인증(토큰) 끝", now_ns()});

    // ── 2. 실시간 접속 ───────────────────────────────────────────────────────
    SharedState shared;
    KisWebSocket websocket(kis_config);
    websocket.set_callbacks(
        [](const OrderBook&)
        {
        },
        [&shared, &ticker](const TradeData& trade)
        {
            if (trade.ticker.string() != ticker)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(shared.mutex);

            if (shared.first_trade_ns != 0)
            {
                return;
            }

            shared.first_trade_ns     = trade.received_ns;
            shared.first_trade_price  = trade.price;
            shared.first_trade_hhmmss = trade.hhmmss;
            shared.changed.notify_all();
        });
    websocket.set_fill_callback(
        [&shared](const FillNotification& notification)
        {
            const int64_t arrived_ns = now_ns();
            std::lock_guard<std::mutex> lock(shared.mutex);

            if (notification.kind == FillKind::SessionResumed)
            {
                if (shared.fill_subscribed_ns == 0)
                {
                    shared.fill_subscribed_ns = arrived_ns;
                }
            }
            else
            {
                shared.fills.emplace_back(arrived_ns, notification);
            }

            shared.changed.notify_all();
        });

    WatchSpec watch;
    watch.ticker     = ticker;
    watch.market     = Market::KR;
    watch.trade_only = true;

    if (!websocket.connect({watch}))
    {
        std::cerr << "[중단] 실시간 접속 실패 (같은 앱키로 다른 접속이 열려 있지 않은지 확인)\n";
        return 4;
    }

    stages.push_back({"실시간 접속·구독 요청 끝", now_ns()});

    // ── 3. 첫 체결 대기 (수신 스레드 → 이 스레드) ───────────────────────────
    int64_t order_thread_woke_ns = 0;
    double  first_trade_price    = 0.0;
    int32_t first_trade_hhmmss   = 0;
    int64_t first_trade_ns       = 0;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        const bool arrived = shared.changed.wait_for(lock, std::chrono::seconds(kFirstTradeWaitSec),
                                                     [&shared]
                                                     {
                                                         return shared.first_trade_ns != 0;
                                                     });
        order_thread_woke_ns = now_ns();

        if (!arrived)
        {
            std::cerr << "[중단] " << kFirstTradeWaitSec << "초 안에 " << ticker
                      << " 체결이 오지 않았다 (장 시간·종목 코드 확인)\n";
            websocket.disconnect();
            return 5;
        }

        first_trade_price  = shared.first_trade_price;
        first_trade_hhmmss = shared.first_trade_hhmmss;
        first_trade_ns     = shared.first_trade_ns;

        if (shared.fill_subscribed_ns != 0)
        {
            stages.push_back({"체결통보 구독 확인", shared.fill_subscribed_ns});
        }

        stages.push_back({"첫 체결 수신(수신 스레드)", shared.first_trade_ns});
    }

    stages.push_back({"주문 스레드 깨어남", order_thread_woke_ns});

    // ── 4. 1주 매수 ──────────────────────────────────────────────────────────
    double order_price = use_limit ? first_trade_price : 0.0;

    if (!kis_config.is_paper)
    {
        // 사는 쪽으로 한 걸음 올린 뒤 호가 단위로 올림한다. round_to_tick은 SELL=올림이다.
        order_price = krx::round_to_tick(first_trade_price * (1.0 + kLivePriceStep), OrderSide::SELL);

        if (order_price * kOrderQuantity > kLiveNotionalCap)
        {
            std::cerr << "[중단] 실계좌 주문 금액 " << static_cast<long long>(order_price * kOrderQuantity)
                      << "원이 상한 " << static_cast<long long>(kLiveNotionalCap) << "원을 넘는다. 더 싼 종목을 고른다.\n";
            websocket.disconnect();
            return 2;
        }
    }

    OrderSignal signal;
    signal.ticker          = ticker;
    signal.side            = OrderSide::BUY;
    signal.type            = use_limit ? OrderType::LIMIT : OrderType::MARKET;
    signal.quantity        = kOrderQuantity;
    signal.price           = order_price;
    signal.reference_price = first_trade_price;
    signal.strategy_id     = "ORDER_PATH_CHECK";
    signal.market          = Market::KR;
    signal.account_id      = kis_config.account_no;
    signal.timestamp       = std::chrono::system_clock::now();

    const int64_t submit_start_ns = now_ns();
    const OrderAck acknowledgement = kis.submit_order_acknowledgement(signal);
    const int64_t acknowledged_ns = now_ns();
    const uint64_t bucket_wait_ns = kis.rate_limit_wait_ns_this_thread();
    stages.push_back({"매수 요청 보냄", submit_start_ns});

    if (!acknowledgement.ok())
    {
        std::cerr << "[중단] 접수 실패 [" << acknowledgement.error_code << "]\n";
        websocket.disconnect();
        return 6;
    }

    stages.push_back({"접수 응답(주문번호 " + acknowledgement.kis_order_no + ")", acknowledged_ns});

    // ── 5. 체결통보 대기 ─────────────────────────────────────────────────────
    const std::string wanted_order = without_leading_zeros(acknowledgement.kis_order_no);
    int64_t filled_ns = 0;
    FillNotification fill;
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        shared.changed.wait_for(lock, std::chrono::seconds(kFillWaitSec),
                                [&]
                                {
                                    for (const auto& [arrived_ns, notification] : shared.fills)
                                    {
                                        if (without_leading_zeros(notification.kis_order_no) == wanted_order)
                                        {
                                            filled_ns = arrived_ns;
                                            fill      = notification;
                                            return true;
                                        }
                                    }

                                    return false;
                                });
    }

    if (filled_ns != 0)
    {
        stages.push_back({"체결통보 도착", filled_ns});
    }

    websocket.disconnect();

    // ── 6. 표 ────────────────────────────────────────────────────────────────
    std::printf("\n%s / 종목 %s / %s 1주 %.0f원 / 첫 체결 %06d %.0f원\n", kis_config.is_paper ? "모의계좌" : "실계좌",
                ticker.c_str(), use_limit ? "지정가" : "시장가", order_price, first_trade_hhmmss, first_trade_price);
    std::printf("%-40s %14s %14s\n", "단계", "시작부터(ms)", "앞 단계부터(µs)");
    int64_t previous_ns = process_start_ns;

    for (const Stage& stage : stages)
    {
        std::printf("%-40s %14.3f %14.1f\n", stage.name.c_str(),
                    static_cast<double>(stage.at_ns - process_start_ns) / kNanosecondsPerMillisecond,
                    static_cast<double>(stage.at_ns - previous_ns) / kNanosecondsPerMicrosecond);
        previous_ns = stage.at_ns;
    }

    std::printf("\n첫 체결 수신 → 접수 응답: %.1f µs (이 스레드의 초당 한도 대기 누계 %.1f µs)\n",
                static_cast<double>(acknowledged_ns - first_trade_ns) / kNanosecondsPerMicrosecond,
                static_cast<double>(bucket_wait_ns) / kNanosecondsPerMicrosecond);

    if (filled_ns != 0)
    {
        std::printf("체결: %d주 %.0f원, 증권사 체결시각 %s\n", fill.filled_quantity, fill.filled_price,
                    fill.fill_time.c_str());
    }
    else
    {
        std::printf("체결통보: %d초 안에 오지 않았다 (지정가 미체결이거나 장 밖)\n", kFillWaitSec);
    }

    return 0;
}
