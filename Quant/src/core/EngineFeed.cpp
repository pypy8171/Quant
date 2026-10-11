// 시세 입력 — WebSocket 구독 목록을 만들고 소켓에 걸고, 받은 체결·호가를 전략 샤드 큐나 시세 통로로 보낸다.
//  같은 Engine 의 멤버 함수 본체다. 구독 목록은 EngineFeedWatch.cpp, 구독 칸 배정·시세 쪽 제어 요청은
//  EngineFeedSlots.cpp 로 떼어 냈다. 아래 표는 세 파일을 합쳐 부르는 자리다.
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  websocket_lane_count()             : setup_shards()·start()·configure() 가 소켓 수를 셀 때
//  collect_watch_specifications()      : start() — 전략들이 보는 종목을 구독 목록으로 모은다
//  add_watch_specification()·send_watch_request() : start()·유니버스 재스캔의 전략 등록·떼기(데이터 스레드)·
//                                    apply_feed_control_requests()(감시 스레드)
//  drain_pending_subscriptions()       : 제어 스레드가 주기마다 — 쌓인 구독·해제 요청을 소켓에 건다
//  connect_feed()                      : start() — 소켓을 열고 수신 콜백을 건다. 콜백은 소켓의 수신 스레드에서 돈다
//  push_feed_*()                       : 갈라 띄운 시세 프로세스의 수신 콜백. push_feed_trade()는 넘침 폴러(데이터 스레드)도
//  fan_out_*()                         : 한 프로세스면 수신 콜백, 갈라 띄우면 전략 쪽 feed_lane_thread_fn()
//  feed_lane_thread_fn()               : spawn_threads() 가 전략 역할에서 시세 줄마다 띄운다. [why D-071·D-114]
//  websocket_slot_priority()·rebalance_websocket_slots() : 구독 칸 우선순위 계산과 칸 재배정 [why D-132]
//  apply_feed_control_requests()       : control_thread_fn() 가 — 시세 쪽 요청(구독·해지·칸 우선순위)을 적용한다

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "core/ReconcilePlan.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include "utils/Utf8.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

// 소켓을 쥔 시세 프로세스가 디코드한 체결을 전략 프로세스로 넘긴다. 기다리지 않는다 — 큐가 차면 버리고 센다(원칙 3).
//  [inv] 한 줄의 보내는 쪽은 스레드 하나다 — 소켓 줄은 그 수신 스레드, 마지막 줄(pipeline_.data_row())은 폴러의
//  조회 스레드. 한 줄을 두 스레드가 부르면 SPSC가 깨진다. [why D-114] [why D-138]
void Engine::push_feed_trade(uint32_t lane, const TradeData& trade)
{
    if (!layout_.feed().push_trade(lane, trade) &&
        feed_channel_overflow_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[Engine] 시세 통로 가득 — 체결 버리기 시작 " + trade.ticker.string() + " (전략 프로세스 정체 의심)");
    }
}

size_t Engine::feed_channel_pending_trades(uint32_t lane)
{
    return layout_.feed().pending_trades(lane);
}

uint64_t Engine::feed_channel_sent()
{
    return layout_.feed().sent_trades() + layout_.feed().sent_order_books();
}

uint64_t Engine::feed_channel_received()
{
    return layout_.feed().received_trades() + layout_.feed().received_order_books();
}

uint32_t Engine::feed_channel_lanes()
{
    return layout_.feed().lanes();
}

void Engine::push_feed_order_book(uint32_t lane, const OrderBook& order_book)
{
    if (!layout_.feed().push_order_book(lane, order_book) &&
        feed_channel_overflow_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[Engine] 시세 통로 가득 — 호가 버리기 시작 " + order_book.ticker.string() + " (전략 프로세스 정체 의심)");
    }
}

// 받은 호가를 이 종목을 보는 샤드 전부에 넣는다(아무도 안 보면 해시 열 하나). 버린 수를 세고 넘침이
//  시작될 때 한 번 남긴다. [why D-110]
//  부르는 쪽은 둘이다 — 한 프로세스로 돌면 수신 스레드가, 갈라 띄우면 전략 쪽 줄 스레드가 부른다. [why D-114]
void Engine::fan_out_order_book(uint32_t lane, const OrderBook& order_book)
{
    bool dropped = false;

    shard::for_each_shard(pipeline_.routes.mask(order_book.symbol_id),
                          pipeline_.order_book_matrix.consumer_of(order_book.symbol_id), [&](uint32_t consumer)
    {
        if (!pipeline_.order_book_matrix.push_to(lane, consumer, order_book))
        {
            dropped = true;
            return;
        }

        pipeline_.shards[consumer]->wake().notify();
    });

    if (dropped && order_book_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[WS] 호가 큐 가득 — 호가 폐기 시작 " + order_book.ticker.string() + " (샤드 스레드 정체 의심)");
    }
}

// 체결도 같은 규칙. 전략 스레드가 멈추면 큐가 차고 틱이 여기서 사라진다 — 세어 두고 넘침이 시작될 때
//  한 번 남긴다(09-11 15:15 잔고 조회 정체). [why D-055]
void Engine::fan_out_trade(uint32_t lane, const TradeData& trade)
{
    bool dropped = false;

    shard::for_each_shard(pipeline_.routes.mask(trade.symbol_id), pipeline_.trade_matrix.consumer_of(trade.symbol_id),
                          [&](uint32_t consumer)
    {
        if (!pipeline_.trade_matrix.push_to(lane, consumer, trade))
        {
            dropped = true;
            return;
        }

        pipeline_.shards[consumer]->wake().notify();
    });

    if (dropped && trade_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[WS] 체결 큐 가득 — 틱 폐기 시작 " + trade.ticker.string() + " (샤드 스레드 정체 의심)");
    }
}

// 체결통보 한 건을 갈 길로 넣는다. 갈라 띄운 날의 체결통보는 이 프로세스 것이 아니다 — 소켓이 시세로
//  오면서 체결통보도 같이 따라왔고, 장부를 쥔 쪽은 주문이다. 그래서 시세 역할일 때만 공유 체결 통로로
//  넘긴다. 한 프로세스로 돌면 예전처럼 프로세스 안 큐로 가 홉이 늘지 않는다.
//  부르는 쪽은 둘이다 — WS 수신 콜백(connect_feed), 그리고 갈라 띄운 주문 프로세스의 모의 체결기
//  콜백(start). 어느 쪽이든 한 번에 하나만 넣는다. [why D-114 단계 5]
void Engine::push_fill_notification(const FillNotification& fill_notification)
{
    const bool fill_crosses_boundary = role_ == ProcessRole::Feed;

    // 넣는 쪽은 한 번에 하나다(pipeline_.fill_producing 설명). 겹치면 뒤에 온 쪽이
    //  기다려 한 줄로 서고 센다 — 그대로 넣으면 SPSC 큐의 칸 번호가 어긋나 체결이 사라진다.
    if (pipeline_.fill_producing.exchange(true, std::memory_order_acquire))
    {
        const auto count = pipeline_.fill_producer_overlap.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_ERROR("[Engine] 체결통보 생산자 겹침 — 두 스레드가 같이 넣으려 했다 ODNO=" +
                  fill_notification.kis_order_no + " (누적 " + std::to_string(count) + "건, 기대 0)");

        while (pipeline_.fill_producing.exchange(true, std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }

    struct ProducerTurn
    {
        std::atomic<bool>& producing;

        ~ProducerTurn()
        {
            producing.store(false, std::memory_order_release);
        }
    };

    const ProducerTurn producer_turn{pipeline_.fill_producing};

    // 두 길 모두 문자열 없는 레코드로 옮겨 넣는다 — 큐 칸에 힙 문자열이 있으면 넣고 뺄 때마다
    //  할당·해제가 수신 스레드에서 일어난다. 옮기는 일은 여기서 한 번이다. [why CODE_REVIEW W-7]
    bool           truncated = false;
    const uint64_t sequence  = pipeline_.fill_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto     notice    = ipc::to_notice(fill_notification, sequence, trace::now_ns(), &truncated);

    if (truncated)
    {
        // 잘린 주문번호로는 취소·정정을 증권사에 되돌려 줄 수 없다. 넘기기는 하되 남긴다.
        LOG_ERROR("[Engine] 체결통보 칸이 모자라 글자가 잘렸다 — " + fill_notification.ticker +
                  " ODNO=" + fill_notification.kis_order_no);
    }

    // 수신 스레드는 넣고 바로 돌아간다. 큐가 가득 찼으면(1024건 밀림 = 소비자가 멈춘 것) 기다리지
    //  않고 넘침 목록 뒤에 붙인다 — 여기서 대기하면 전 종목 틱이 같이 선다. 목록에 먼저 온 건이
    //  남아 있으면 이번 건도 그 뒤에 서야 순서가 맞는다. [why D-056]
    if (drain_fill_overflow() && push_fill_notice(notice))
    {
        return;
    }

    pipeline_.fill_overflow.push_back(notice);
    pipeline_.fill_overflow_waiting.store(pipeline_.fill_overflow.size(), std::memory_order_relaxed);
    const auto count = pipeline_.fill_overflowed.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_ERROR(std::string("[Engine] ") + (fill_crosses_boundary ? "체결 통로" : "체결통보 큐") +
              " 가득 참 — 넘침 목록에 보관, 버리지 않는다 " + fill_notification.ticker + " ODNO=" +
              fill_notification.kis_order_no + " (대기 " + std::to_string(pipeline_.fill_overflow.size()) +
              "건, 누적 " + std::to_string(count) + "건)");
}

bool Engine::push_fill_notice(const ipc::FillNotice& notice)
{
    if (role_ == ProcessRole::Feed)
    {
        return layout_.fills().push(notice);
    }

    if (!pipeline_.fill_queue.push(notice))
    {
        return false;
    }

    pipeline_.fill_wake.notify();
    return true;
}

bool Engine::drain_fill_overflow()
{
    auto& overflow = pipeline_.fill_overflow;

    if (overflow.empty())
    {
        return true;
    }

    while (!overflow.empty() && push_fill_notice(overflow.front()))
    {
        overflow.pop_front();
    }

    pipeline_.fill_overflow_waiting.store(overflow.size(), std::memory_order_relaxed);

    if (overflow.empty())
    {
        LOG_INFO("[Engine] 넘침 목록을 다 옮겼다 (누적 " +
                 std::to_string(pipeline_.fill_overflowed.load(std::memory_order_relaxed)) + "건)");
    }

    return overflow.empty();
}

void Engine::flush_fill_overflow()
{
    if (pipeline_.fill_overflow_waiting.load(std::memory_order_relaxed) == 0)
    {
        return;
    }

    // 차례를 다른 쪽이 쥐고 있으면 그쪽이 넣기 전에 비운다 — 기다리지 않고 다음 5초로 넘긴다.
    //  제어 스레드가 차례를 잡는 것은 정해진 일이라 생산자 겹침으로 세지 않는다.
    if (pipeline_.fill_producing.exchange(true, std::memory_order_acquire))
    {
        return;
    }

    drain_fill_overflow();
    pipeline_.fill_producing.store(false, std::memory_order_release);
}

void Engine::connect_feed()
{
    // WebSocket — 동적 구독 스펙으로 연결.
    //  feed_.rest_price_feed 모드에서는 WS를 열지 않는다: KIS는 app_key당 실시간 1세션만
    //  허용하는데, 세션 정리가 서버측에 걸려 rt=9(ALREADY IN USE) 재연결 폭주가 나므로
    //  체결 피드를 REST 현재가 폴링(data_thread_fn)으로 대체하고 WS 의존을 제거한다.
    //  주문은 REST(order_thread_fn)로 나가므로 매매에는 영향 없음(체결통보 on_fill만 없음).
    //  구독할 종목이 없어도 hts_id 가 있으면 열어둔다 — 체결통보(H0STCNI)는 종목 구독과
    //  별개라, 유니버스가 비었다고 닫아버리면 이월 보유분을 청산하는 주문의 체결을 못 듣고
    //  장부가 빈다. 2026-09-23 실계좌 첫날 청산 체결이 이렇게 사라졌다. [why D-097]
    //  갈라 띄울 때도 같다 — 구독 목록은 전략 쪽에서 요청으로 오므로 시세만 맡은 프로세스는
    //  목록이 비어도 소켓을 열어 둔다. 그때 소켓이 없으면 걸 곳이 없다. [why D-114]
    if (feed_.rest_price_feed ||
        (watch_specifications_.empty() && kis_config_.hts_id.empty() && role_ != ProcessRole::Feed))
    {
        // 안 열었다는 것도 남긴다 — 이 줄이 없으면 건강 점검은 체결통보가 끈겼는지를 못 가른다.
        LOG_INFO(std::string("[Engine] 체결통보 세션: 없음(") +
                 (feed_.rest_price_feed ? "REST 시세 모드라 WS를 열지 않는다"
                                        : "구독 종목 0개·hts_id 비었다") +
                 ")");
        return;
    }

    create_feed_source();
    open_tick_capture();
    install_feed_callbacks();

    auto push_fill = [this](const FillNotification& fill_notification)
    {
        push_fill_notification(fill_notification);
    };

    feed_.websocket->set_fill_callback(push_fill);

    if (feed_.paper)
    {
        feed_.paper->set_fill_callback(push_fill);
    }

    if (!feed_.websocket->connect(watch_specifications_))
    {
        // 예전에는 경고만 남기고 넘어갔는데, 그러면 전략이 호가·체결을 하나도 못 받아
        //  매매가 조용히 멈춘다(폴링 경로가 꺼져 있으므로). 폴링으로 낮춰 계속 돈다.
        //  control_thread가 재연결을 계속 시도하고, 붙으면 WS로 되돌린다.
        LOG_ERROR("[Engine] WebSocket 최초 연결 실패");

        if (!activate_rest_fallback("최초 연결 실패"))
        {
            LOG_ERROR("[Engine] 폴링 폴백도 불가(시세 소스 없음) — 호가/체결 이벤트 없이 동작");
        }
    }
}

// connect_feed() 단계 — 피드 소스를 고른다(주입·리플레이·소켓 하나·소켓 여럿 FeedMux). 체결통보를 맡은 소켓 수를 남긴다.
void Engine::create_feed_source()
{
    if (feed_.feed_override)
    {
        feed_.websocket = std::move(feed_.feed_override);
        LOG_INFO("[Engine] 주입된 피드 소스(수신 스레드 " + std::to_string(feed_.websocket->lanes()) + "개)");
    }
    else if (!feed_.replay_file.empty())
    {
        feed_.websocket = std::make_unique<feed::ReplaySource>(utf8::path_from_utf8(feed_.replay_file), feed_.replay_speed);
        LOG_INFO("[Engine] 리플레이 소스: " + feed_.replay_file + " (speed " + std::to_string(feed_.replay_speed) + ")");
    }
    else if (feed_.extra_feed_cfgs.empty())
    {
        feed_.websocket = std::make_unique<KisWebSocket>(kis_config_);
        LOG_INFO(std::string("[Engine] 체결통보 세션: ") + (kis_config_.hts_id.empty() ? "없음(hts_id가 비어 있다)" : "소켓 0"));
    }
    else
    {
        // 소켓 여럿 — 첫 소스가 기본 키다. 체결통보는 hts_id를 가진 소켓 하나가 맡는다(자리가 아니라 설정이 정한다,
        //  D-114 단계 3). 직접 호출 모드라 소켓 i의 수신 스레드가
        //  행렬의 행 i에 직접 넣는다(multiplexer 스레드 없음). 소켓이 하나면 FeedMux를 끼우지 않는다.
        std::vector<std::unique_ptr<feed::IFeedSource>> socks;
        socks.push_back(std::make_unique<KisWebSocket>(kis_config_));

        for (const auto& extra_feed_config : feed_.extra_feed_cfgs)
        {
            socks.push_back(std::make_unique<KisWebSocket>(extra_feed_config));
        }

        auto                      multiplexed  = std::make_unique<feed::FeedMux>(std::move(socks));
        const std::vector<size_t> fill_sources = multiplexed->fill_notice_sources();
        feed_.websocket                        = std::move(multiplexed);
        LOG_INFO("[Engine] WS 소켓 " + std::to_string(feed_.extra_feed_cfgs.size() + 1) + "개를 FeedMux 수신 스레드 " +
                 std::to_string(pipeline_.websocket_lanes) + "개로 묶는다");

        if (fill_sources.size() > 1)
        {
            LOG_ERROR("[Engine] 체결통보 세션: " + std::to_string(fill_sources.size()) +
                      "개 — 하나만 맡아야 한다. KIS는 세션마다 같은 통보를 보내 장부가 체결을 두 번 센다");
        }
        else if (fill_sources.empty())
        {
            LOG_INFO("[Engine] 체결통보 세션: 없음(hts_id가 비어 있다)");
        }
        else
        {
            LOG_INFO("[Engine] 체결통보 세션: 소켓 " + std::to_string(fill_sources.front()));
        }
    }
}

// connect_feed() 단계 — 틱 캡처 파일을 열고 담을 종목 표와 그날 구독 목록을 파일 머리에 남긴다. 리플레이면 열지 않는다.
void Engine::open_tick_capture()
{
    // 리플레이를 다시 캡처하면 같은 틱이 두 파일에 남으므로 캡처는 WS일 때만 연다.
    if (!feed_.capture_directory.empty() && feed_.replay_file.empty())
    {
        // 파일명은 UTC 기동 시각 — 재기동이 같은 파일에 이어 쓰지 않도록.
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const std::string           file_name = "ticks_" + std::to_string(now_s) + ".bin";
        const std::filesystem::path file      = utf8::path_from_utf8(feed_.capture_directory) / file_name;
        feed_.capture = std::make_unique<feed::TickCapture>(file);

        if (feed_.capture->ok())
        {
            LOG_INFO("[Engine] 틱 캡처 시작: " + feed_.capture_directory + "/" + file_name);

            // 담을 종목을 좁혔으면 id 표로 바꿔 둔다 — 수신 스레드는 이 표만 본다. 콜백을 걸기 전이라 경합이 없다.
            for (const auto& ticker : feed_.capture_tickers)
            {
                const symbol::SymbolId symbol = symbols_.table.intern(ticker);

                if (symbol == symbol::kNone)
                {
                    continue;
                }

                if (feed_.capture_wanted.size() <= symbol)
                {
                    feed_.capture_wanted.resize(std::max<size_t>(symbols_.table.capacity() + 1, symbol + 1), 0);
                }

                feed_.capture_wanted[symbol] = 1;
                LOG_INFO("[Engine] 틱 캡처 대상: " + ticker);
            }

            // 그날 무엇을 구독했는지를 파일 머리에 남긴다 — 호가가 비어 있는 종목이 trade_only인지 파일만 보고 알 수 있게.
            for (const auto& specification : watch_specifications_)
            {
                feed_.capture->on_universe(specification.ticker, static_cast<uint8_t>(specification.market),
                                           symbols_.table.intern(specification.ticker), specification.trade_only);
            }
        }
        else
        {
            LOG_WARN("[Engine] 틱 캡처 파일을 열지 못해 캡처 없이 간다: " + feed_.capture_directory + "/" + file_name);
        }
    }
}

// connect_feed() 단계 — 소켓 수신 스레드의 호가·체결 콜백을 건다. 콜백 몸통은 hot path라 람다를 그대로 둔다.
void Engine::install_feed_callbacks()
{
    // 수신 스레드 = 이 콜백을 부르는 수신 스레드 번호 = 행렬의 행. 한 행은 그 스레드만 넣는다(SPSC 셀, 원칙 5).
    //  소스가 준 구조체에 번호·시각을 바로 찍는다(콜백이 비const 참조인 까닭은 Quant/include/core/IFeedSource.h) —
    //  push가 셀에 한 번 베끼는 것이 유일한 복사다.
    feed_.websocket->set_lane_callbacks([this](uint32_t lane, OrderBook& order_book)
                       {
                           order_book.symbol_id = symbols_.table.intern(order_book.ticker);

                           // 번호가 안 붙은 호가는 접는다 — 체결 줄과 같은 규칙이다(아래 체결 콜백). [why D-114 단계 5]
                           if (order_book.symbol_id == symbol::kNone)
                           {
                               return;
                           }

                           // 수신 스레드가 디코드 시점에 찍은 값을 지킨다. 안 찍힌 소스만 여기서 찍는다.
                           if (order_book.received_ns == 0)
                           {
                               order_book.received_ns = trace::now_ns();
                           }

                           if (feed_.capture && capture_wanted(order_book.symbol_id))
                           {
                               feed_.capture->on_book(order_book);
                           }

                           // 갈라 띄우면 소켓을 쥔 쪽은 통로에 넣기까지만 한다 — 샤드도 전략도 저쪽에 있다(원칙 3). [why D-114]
                           if (role_ == ProcessRole::Feed)
                           {
                               push_feed_order_book(lane, order_book);
                               return;
                           }

                           fan_out_order_book(lane, order_book);
                       },
                       [this](uint32_t lane, TradeData& trade)
                       {
                           // 수신 시각은 수신 스레드가 디코드 시점에 찍은 값을 지키고, 안 찍힌 소스만 여기서 찍는다.
                           trade.symbol_id = symbols_.table.intern(trade.ticker);

                           // 번호가 안 붙었으면 이 줄을 접는다. 시세 역할은 표에 없는 티커를 버리고 세고
                           //  (청하지 않은 종목이 세션에 실려 온 것이다), 전략 역할은 주문 쪽이 제때 안 달아
                           //  준 때다. 그대로 보내면 통로 저쪽이 못 알아보고, 샤드로 가면 번호 없는 값이
                           //  남의 샤드를 깨운다. 센 것에도 넣지 않는다 — 못 알아본 줄은 받은 시세가 아니다.
                           //  [why D-114 단계 5]
                           if (trade.symbol_id == symbol::kNone)
                           {
                               return;
                           }

                           // 받은 체결을 센다. 예전엔 REST 폴링 경로(data_thread_fn)에서만 올려서, WS로만 도는
                           //  구성(DevScale 27종목)에서는 HEALTH의 data가 늘 0이었다 — 그라파나 "초당 틱 처리량"이
                           //  항상 0선이고, 이 값이 줄어드는 것으로 재기동을 세는 패널도 영영 0이었다.
                           data_count_.fetch_add(1, std::memory_order_relaxed);

                           if (trade.received_ns == 0)
                           {
                               trade.received_ns = trace::now_ns();
                           }

                           if (feed_.capture && capture_wanted(trade.symbol_id))
                           {
                               feed_.capture->on_trade(trade);
                           }

                           // 모의 체결은 틱 스레드에서 낸다(feed_.paper는 리플레이·피드 주입 전용). 수신 스레드가
                           //  여럿이면 여기가 동시에 불리므로 체결기가 전달을 한 줄로 세운다(PaperExecutor::on_tick, W-6).
                           if (feed_.paper)
                           {
                               feed_.paper->on_tick(trade);
                           }

#ifdef HAS_ZMQ
                           // 발행은 팬아웃보다 먼저 한다 — 샤드 큐가 차서 돌아가던 예전 순서에서는 정체 때
                           //  그라파나까지 같이 멎었다. [why D-114]
                           if (zmq_bridge_)
                           {
                               zmq_bridge_->publish_trade(trade);
                           }
#endif

#ifdef HAS_PQ
                           // DB 적재도 같은 자리 — 큐에 넣기만 하고, 차면 버리고 센다. [why D-148]
                           if (database_)
                           {
                               database_->on_trade(trade);
                           }
#endif

                           if (role_ == ProcessRole::Feed)
                           {
                               push_feed_trade(lane, trade);
                               return;
                           }

                           fan_out_trade(lane, trade);
                       });
}

// ─── 시세 줄 스레드(전략 역할) ────────────────────────────────────────────
void Engine::feed_lane_thread_fn(std::stop_token stop_token, uint32_t lane)
{
    thread_name::set_current("Feed " + std::to_string(lane));
    LOG_INFO("[Feed " + std::to_string(lane) + "] 시작 — 통로에서 꺼내 샤드로 나눈다");

    // 꺼낸 칸이 말이 되는지 보는 기준. 종목 표는 기동 때 다 차고 장중 재스캔이 뒤에 더 붙일 수 있어
    //  줄 한 바퀴마다 다시 읽는다 — 새로 등록된 종목의 시세를 "번호가 표 밖"이라고 버리지 않도록. [why D-114]
    ipc::MarketLimits limits;

    // 유휴 전이: 샤드 스레드와 같은 정책이되 게이트가 없다 — 건너편은 다른 프로세스라 깨울 수 없다. [why D-071]
    //  놀린 뒤에도 빈 채면 짧게 잔다. 이 잠은 타이머 격자를 안 타는 길로 잔다(sleep_precise_unless_stopped) —
    //  격자를 타면 500us 를 부탁해도 2ms 를 자고, 그만큼이 그대로 "시세 수신 -> 전략" 에 실린다.
    //  2026-09-25 부하시험의 조용한 판에서 52us 가 1,086us 가 된 것이 이것이다. [why D-137]
    constexpr auto                        kSpinBudget = std::chrono::microseconds(200);
    constexpr auto                        kIdleSleep  = std::chrono::microseconds(500);
    std::chrono::steady_clock::time_point idle_since{};

    TradeData trade;
    OrderBook order_book;

    // 호가 행렬은 WS 줄 수만큼만 행이 있다 — 마지막 REST 대체 줄(lane == WS 줄 수)은 체결만 흘린다. 그 줄에서 꺼낸
    //  호가는 넘길 행이 없으니 버린다(꺼내기는 한다 — 안 꺼내면 통로 칸이 찬다). [why D-114]
    const bool lane_has_order_book_row = lane < pipeline_.order_book_matrix.producers();

    while (!stop_token.stop_requested())
    {
        limits.symbol_count = static_cast<uint32_t>(symbols_.table.size());

        bool did_work = false;

        // 체결을 먼저 비우고 호가를 비운다 — 둘은 다른 큐라 순서에 걸린 규칙이 없다(종목 안 순서는 큐가 지킨다).
        while (layout_.feed().pop_trade(lane, limits, trade))
        {
            data_count_.fetch_add(1, std::memory_order_relaxed);
            fan_out_trade(lane, trade);
            did_work = true;
        }

        while (layout_.feed().pop_order_book(lane, limits, order_book))
        {
            if (lane_has_order_book_row)
            {
                fan_out_order_book(lane, order_book);
            }

            did_work = true;
        }

        // 말이 안 돼 버린 수는 꺼내는 쪽만 안다 — 감시 스레드가 읽을 자리에 옮겨 둔다. 한 건도 못 건진 바퀴에도
        //  옮긴다 — 번호 표가 반쪽이면 꺼내는 족족 버려 did_work가 계속 거짓이고, 그때가 바로 봐야 할 때다. [why D-114]
        if (const uint64_t discarded_now = layout_.feed().discarded();
            discarded_now != feed_channel_discarded_.load(std::memory_order_relaxed))
        {
            feed_channel_discarded_.store(discarded_now, std::memory_order_relaxed);
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        const auto now_idle = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_idle;
        }

        if (now_idle - idle_since < kSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        wake::sleep_precise_unless_stopped(stop_token, kIdleSleep);
    }

    LOG_INFO("[Feed " + std::to_string(lane) + "] 종료");
}
