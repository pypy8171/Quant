// 시세 입력 — WebSocket 구독 목록을 만들고 소켓에 걸고, 받은 체결·호가를 전략 샤드 큐나 시세 통로로 보낸다.
//  Engine 클래스는 그대로다. Engine.cpp 가 길어 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
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

uint32_t Engine::websocket_lane_count() const
{
    if (feed_.feed_override)
    {
        return feed_.feed_override->lanes();
    }

    if (feed_.replay_file.empty() && !feed_.extra_feed_cfgs.empty())
    {
        return static_cast<uint32_t>(feed_.extra_feed_cfgs.size() + 1);
    }

    return 1u;
}

// 설정의 종목 목록을 그대로 구독 목록으로 깐다 — 전략이 없는 시세 프로세스에서 종목 순번표를 채우려면
//  이 목록이 필요하다. 스레드가 뜨기 전에만 불러서 자물쇠 없이 쓴다. [why D-114 단계 5]
void Engine::seed_watch_specifications(const std::vector<std::string>& tickers)
{
    watch_specifications_.clear();
    watch_specifications_.reserve(tickers.size());

    for (const auto& ticker : tickers)
    {
        WatchSpec specification;
        specification.ticker = ticker;
        specification.market = Market::KR;
        watch_specifications_.push_back(std::move(specification));
    }

    LOG_INFO("[Engine] 설정에서 깐 WS 구독 종목: " + std::to_string(watch_specifications_.size()) + "개");
}

void Engine::collect_watch_specifications()
{
    // 전략별 구독 스펙 수집 (중복 제거)
    watch_specifications_.clear();
    {
        // 키가 "시장:거래소:티커" 문자열 셋의 조합이고 설정에서 온 스펙을 기동 때 한 번 거르는 자리라 문자열 집합을 쓴다.
        std::unordered_set<std::string> seen;

        for (auto& strategy : strategy_.list)
        {
            for (auto& specification : strategy->get_watch_specifications())
            {
                std::string key = (specification.market == Market::US ? "US:" : "KR:") + specification.exchange + ":" + specification.ticker;

                if (seen.insert(std::move(key)).second)
                {
                    watch_specifications_.push_back(std::move(specification)); // get_watch_specifications()가 준 임시 벡터라 옮겨도 된다
                }
            }
        }
    }

    LOG_INFO("[Engine] WS 구독 종목: " + std::to_string(watch_specifications_.size()) + "개");

    // 재스캔 중복 방지 시드 — 기동 유니버스에 이미 등록된 KR 종목 기록(스펙의 문자열 티커는 여기서 id가 된다).
    universe_rescan_.reset_registered();

    for (auto& specification : watch_specifications_)
    {
        if (specification.market == Market::KR)
        {
            universe_rescan_.set_registered(register_symbol(specification.ticker), true);
        }

        // 거는 자리는 소켓을 쥔 시세 쪽 하나다. Both 로 돌면 connect_feed() 가 이미 이 목록을 통째로
        //  걸어 둔 뒤라 이 요청은 "이미 구독 중"으로 끝난다 — 갈라 띄운 날 처음 도는 코드를 안 만들려고
        //  양쪽이 같은 길을 쓴다. [why D-114]
        send_watch_request(specification);
    }
}

bool Engine::add_watch_specification(const WatchSpec& specification)
{
    std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);

    for (const auto& watch_specification : watch_specifications_)
    {
        if (watch_specification.market == specification.market && watch_specification.exchange == specification.exchange &&
            watch_specification.ticker == specification.ticker && watch_specification.is_future == specification.is_future)
        {
            return false;
        }
    }

    watch_specifications_.push_back(specification);
    return true;
}

void Engine::send_watch_request(const WatchSpec& specification, ipc::ControlKind kind, int32_t priority)
{
    // 칸을 넘는 종목 코드는 잘라 보내지 않는다 — 잘린 코드로 구독하면 엉뚱한 종목의 틱이 이 종목 것으로 온다.
    if (specification.ticker.size() > symbol::Ticker::kMax || specification.exchange.size() >= ipc::kControlExchangeMax)
    {
        LOG_ERROR("[Engine] 구독 스펙이 칸을 넘어 보내지 못했다 — " + specification.ticker + "(" + specification.exchange + ")");
        return;
    }

    ipc::ControlRequest request;
    request.kind       = kind;
    request.rank       = priority;
    request.ticker     = std::string_view(specification.ticker);
    request.market     = static_cast<uint8_t>(specification.market);
    request.trade_only = specification.trade_only ? 1 : 0;
    request.is_future  = specification.is_future ? 1 : 0;
    ipc::set_exchange(request, specification.exchange);

    if (!control_plane_.send(request))
    {
        // 사라지면 그 종목은 틱이 영영 오지 않는다(09-11 실측: 구독 밖 종목 체결 0건). 큰 소리로 남긴다.
        LOG_ERROR("[Engine] 구독 요청을 못 보냈다 — " + specification.ticker + " 는 시세를 받지 못한다");
    }
}

void Engine::drain_pending_subscriptions()
{
    std::vector<WatchSpec> specifications;
    std::vector<WatchSpec> unsubscriptions;
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        specifications.swap(pending_subscriptions_); // 소켓 쓰기는 자물쇠 밖에서 한다
        unsubscriptions.swap(pending_unsubscriptions_);

        // 해지는 목록에서도 뺀다 — 남기면 REST 대체(poll_universe)와 재연결이 다시 건다.
        for (const auto& specification : unsubscriptions)
        {
            std::erase_if(watch_specifications_,
                          [&specification](const WatchSpec& watch) { return same_watch(watch, specification); });
        }
    }

    if ((specifications.empty() && unsubscriptions.empty()) || !feed_.websocket)
    {
        return;
    }

    // 해지를 먼저 푼다 — 돌려받은 칸을 같은 바퀴의 구독이 쓴다. [why D-132]
    for (const auto& specification : unsubscriptions)
    {
        const bool released = feed_.websocket->unsubscribe_incremental(specification);

        if (poller_)
        {
            poller_->remove_overflow(specification);
        }

        const symbol::SymbolId symbol = symbols_.table.lookup(specification.ticker);

        if (symbol < websocket_slots_.size())
        {
            websocket_slots_[symbol] = WebSocketSlotState{};
        }

        LOG_INFO("[WS칸] 구독 해지 — " + specification.ticker + (released ? " (칸 반납)" : " (REST 대체만 멈춤)") +
                 ", 보는 전략·보유·선점 없음");
    }

    for (const auto& specification : specifications)
    {
        if (feed_.capture)
        {
            // 그날 무엇을 구독했는지 캡처 파일 머리에 남긴다 — 호가가 빈 종목이 trade_only 인지 파일만 보고 알 수 있게.
            feed_.capture->on_universe(specification.ticker, static_cast<uint8_t>(specification.market),
                                       symbols_.table.intern(specification.ticker), specification.trade_only);
        }

        // 거짓 자체는 정상일 수 있다(연결 전·이미 구독). 목록에서까지 빠졌으면 구독 상한에 밀린 것이고,
        //  그대로 두면 이 종목은 틱 없이 조용히 매매하지 않는다(09-11 실측: 40 초과 종목 체결 0건).
        if (feed_.websocket->subscribe_incremental(specification) || feed_.websocket->has_specification(specification))
        {
            continue;
        }

        watch_overflow_.fetch_add(1, std::memory_order_relaxed);

        // 넘침 목록에 넣어 데이터 스레드가 REST 로 대신 흘린다. 폴러는 양쪽에 있고, 갈라 띄우면 시세 쪽
        //  폴러가 받아 시세 통로의 마지막 줄로 보낸다(구독을 거는 쪽과 같은 프로세스다). [why D-114]
        if (poller_ && poller_->add_overflow(specification))
        {
            LOG_WARN("[Engine] WS 구독 상한 — " + specification.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                     std::to_string(poller_->overflow_count()) + "종목)");
        }
        else if (!poller_)
        {
            LOG_ERROR("[Engine] WS 구독 상한 — " + specification.ticker + " 는 REST 대체가 아직 없어 틱을 못 받는다");
        }
    }
}

// 소켓을 쥔 시세 프로세스가 디코드한 체결을 전략 프로세스로 넘긴다. 기다리지 않는다 — 큐가 차면 버리고 센다(원칙 3).
//  [inv] 한 줄의 보내는 쪽은 스레드 하나다 — 소켓 줄은 그 수신 스레드, 마지막 줄(pipeline_.data_row)은 넘침 폴러를
//  돌리는 데이터 스레드. 한 줄을 두 스레드가 부르면 SPSC가 깨진다. [why D-114]
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
//  오면서 체결통보도 같이 따라왔고, 원장을 쥔 쪽은 주문이다. 그래서 시세 역할일 때만 공유 체결 통로로
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

    if (fill_crosses_boundary)
    {
        if (!layout_.fills().push(notice))
        {
            const auto count = pipeline_.fill_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_ERROR("[Engine] 체결 통로 가득 참 — 드롭 " + fill_notification.ticker + " ODNO=" +
                      fill_notification.kis_order_no + " (누적 " + std::to_string(count) +
                      "건) 주문 쪽 예약 수량이 안 풀린다");
        }

        return;
    }

    // 수신 스레드는 큐에 넣고 바로 돌아간다. 가득 찼으면(1024건 밀림 = 소비자가 멈춘 것)
    //  기다리지 않고 버린다 — 여기서 대기하면 전 종목 틱이 같이 선다. 버린 건은
    //  잔고 대조(data_thread)가 원장에 메운다. [why D-056]
    if (!pipeline_.fill_queue.push(notice))
    {
        const auto count = pipeline_.fill_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_ERROR("[Engine] 체결통보 큐 가득 참 — 드롭 " + fill_notification.ticker + " ODNO=" + fill_notification.kis_order_no +
                  " (누적 " + std::to_string(count) + "건)");
        return;
    }

    pipeline_.fill_wake.notify();
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
    //  원장이 빈다. 2026-09-23 실계좌 첫날 청산 체결이 이렇게 사라졌다. [why D-097]
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
                      "개 — 하나만 맡아야 한다. KIS는 세션마다 같은 통보를 보내 원장이 체결을 두 번 센다");
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

    // 수신 스레드 = 이 콜백을 부르는 수신 스레드 번호 = 행렬의 행. 한 행은 그 스레드만 넣는다(SPSC 셀, 원칙 5).
    feed_.websocket->set_lane_callbacks([this](uint32_t lane, const OrderBook& in)
                       {
                           OrderBook order_book = in;
                           order_book.symbol_id       = symbols_.table.intern(order_book.ticker);

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

                           if (feed_.capture)
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
                       [this](uint32_t lane, const TradeData& in)
                       {
                           // 트리비얼 복사 타입이라 이동이 곧 복사다 — 여기서 한 번 복사해 id를 찍고 push가 셀에 한 번 더 베낀다. 수신 시각은
                           //  수신 스레드가 디코드 시점에 찍은 값을 지키고, 안 찍힌 소스만 여기서 찍는다.
                           TradeData trade = in;
                           trade.symbol_id       = symbols_.table.intern(trade.ticker);

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

                           if (feed_.capture)
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

                           if (role_ == ProcessRole::Feed)
                           {
                               push_feed_trade(lane, trade);
                               return;
                           }

                           fan_out_trade(lane, trade);
                       });
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

// ─── 구독 칸 배정(제어 스레드) ────────────────────────────────────────────
int32_t Engine::websocket_slot_priority(const WatchSpec& specification) const
{
    if (!websocket_slot::is_managed(specification))
    {
        return websocket_slot::kHeld; // 칸 배정 밖(선물·미국)은 기동 때처럼 먼저 건다
    }

    const symbol::SymbolId symbol = symbols_.table.lookup(specification.ticker);
    return symbol < websocket_slots_.size() ? websocket_slots_[symbol].priority : websocket_slot::kUnranked;
}

void Engine::rebalance_websocket_slots()
{
    // 칸 개념이 없는 소스(리플레이)거나 연결 전이면 하지 않는다 — 연결 전 목록은 connect()가 건다.
    if (!feed_.websocket || !feed_.websocket->is_connected())
    {
        return;
    }

    const int free_slots = feed_.websocket->free_slots();

    if (free_slots < 0)
    {
        return;
    }

    std::vector<WatchSpec> specifications;
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        specifications.reserve(watch_specifications_.size());

        for (const auto& specification : watch_specifications_)
        {
            if (websocket_slot::is_managed(specification))
            {
                specifications.push_back(specification);
            }
        }
    }

    if (websocket_slots_.size() < symbols_.table.capacity() + 1)
    {
        websocket_slots_.resize(symbols_.table.capacity() + 1);
    }

    const auto                    now = std::chrono::steady_clock::now();
    std::vector<websocket_slot::Entry>   entries;
    std::vector<symbol::SymbolId> ids;
    std::vector<size_t>           specification_index;
    int                           used = 0;
    entries.reserve(specifications.size());

    for (size_t index = 0; index < specifications.size(); ++index)
    {
        const WatchSpec&       specification = specifications[index];
        const symbol::SymbolId symbol        = symbols_.table.lookup(specification.ticker);

        if (symbol == symbol::kNone || symbol >= websocket_slots_.size())
        {
            continue;
        }

        WebSocketSlotState&   state = websocket_slots_[symbol];
        websocket_slot::Entry entry;
        entry.channels  = websocket_slot::channels_of(specification);
        entry.priority  = state.priority;
        entry.on_socket = feed_.websocket->has_specification(specification);

        if (entry.on_socket)
        {
            // 처음 칸에 오른 것을 본 때 — 재연결로 다시 걸린 종목이 REST로도 겹쳐 받지 않게 넘침에서 뺀다.
            if (state.on_since == std::chrono::steady_clock::time_point{})
            {
                state.on_since = now;

                if (poller_)
                {
                    poller_->remove_overflow(specification);
                }
            }

            entry.held_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state.on_since).count();
            used          += entry.channels;
        }
        else
        {
            state.on_since = {};
        }

        entries.push_back(entry);
        ids.push_back(symbol);
        specification_index.push_back(index);
    }

    websocket_slot::Rules rules;
    rules.capacity = used + free_slots;
    const websocket_slot::Plan plan = websocket_slot::plan(entries, rules);

    for (size_t chosen : plan.release)
    {
        const WatchSpec& specification = specifications[specification_index[chosen]];

        if (!feed_.websocket->unsubscribe_incremental(specification))
        {
            continue;
        }

        if (poller_)
        {
            poller_->add_overflow(specification);
        }

        websocket_slots_[ids[chosen]].on_since = {};
        LOG_INFO("[WS칸] 칸 내줌 — " + specification.ticker + " 우선순위=" + std::to_string(entries[chosen].priority) +
                 " 쥔 시간=" + std::to_string(entries[chosen].held_sec) + "초, 이제 REST로 받는다");
    }

    std::vector<bool> taken(entries.size(), false);

    for (size_t chosen : plan.take)
    {
        const WatchSpec& specification = specifications[specification_index[chosen]];

        // 상한에 걸려 실패하면 소켓 쪽이 넘침 목록에 다시 올린다 — REST로 계속 받는다.
        if (!feed_.websocket->subscribe_incremental(specification) || !feed_.websocket->has_specification(specification))
        {
            continue;
        }

        if (poller_)
        {
            poller_->remove_overflow(specification);
        }

        taken[chosen]                   = true;
        websocket_slots_[ids[chosen]].on_since = now;
        LOG_INFO("[WS칸] 칸 받음 — " + specification.ticker + " 우선순위=" + std::to_string(entries[chosen].priority));
    }

    // 보유·선점 종목이 칸 밖에 남는 것은 칸 전부를 보유·선점이 쥔 때뿐이다. 바뀔 때만 한 줄 남긴다 — 판정 행이 센다.
    for (size_t index = 0; index < entries.size(); ++index)
    {
        WebSocketSlotState& state       = websocket_slots_[ids[index]];
        const bool   off_socket  = !entries[index].on_socket && !taken[index];
        const bool   must_listen = entries[index].priority <= websocket_slot::kProtected;
        const bool   warn        = must_listen && off_socket;

        if (warn != state.warned_off_socket)
        {
            state.warned_off_socket = warn;
            const std::string& ticker = specifications[specification_index[index]].ticker;

            if (warn)
            {
                LOG_WARN("[WS칸] 보유·선점 종목이 칸 밖 — " + ticker + " 는 REST로만 받는다(칸 전부를 보유·선점이 쥠)");
            }
            else
            {
                LOG_INFO("[WS칸] 보유·선점 종목 칸 밖 풀림 — " + ticker);
            }
        }
    }
}

void Engine::apply_feed_control_requests()
{
    ipc::ControlRequest request;

    while (control_plane_.pop_feed(request))
    {
        switch (request.kind)
        {
        // 목록에만 올리고 소켓에 거는 것은 이어지는 drain_pending_subscriptions()가 한다 — 꺼내는 자리가
        //  소켓 쓰기에 막히면 그동안 제어 줄이 밀린다. [why D-114]
        case ipc::ControlKind::kWatchSubscribe:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break; // 통로 저쪽에서 온 칸은 믿지 않는다 — 길이가 칸을 넘으면 view() 가 칸 밖을 읽는다
            }

            WatchSpec specification = ipc::watch_specification_of(request);

            // 목록에 이미 있어도 구독은 건다 — Both 로 돌면 collect_watch_specifications() 가 채운 목록에 그대로 들어 있다.
            add_watch_specification(specification);

            {
                std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
                pending_subscriptions_.push_back(std::move(specification));
            }

            break;
        }

        // 보는 전략이 없어진 종목 — 소켓에서 푸는 것은 drain_pending_subscriptions()가 한다. 같은 바퀴에 앞서 온
        //  구독이 아직 안 걸렸으면 그것도 거둔다(순서가 뒤집혀 다시 걸리지 않게). [why D-132]
        case ipc::ControlKind::kWatchUnsubscribe:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break;
            }

            WatchSpec specification = ipc::watch_specification_of(request);
            std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
            std::erase_if(pending_subscriptions_,
                          [&specification](const WatchSpec& watch) { return same_watch(watch, specification); });
            pending_unsubscriptions_.push_back(std::move(specification));
            break;
        }

        // 칸 우선순위만 바꾼다. 칸을 실제로 옮기는 것은 이어지는 rebalance_websocket_slots()다. [why D-132]
        case ipc::ControlKind::kWatchPriority:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break;
            }

            const symbol::SymbolId symbol = symbols_.table.lookup(request.ticker.view());

            if (symbol == symbol::kNone)
            {
                break; // 아직 구독 요청이 안 닿은 종목 — 우선순위가 바뀌면 다음 재스캔이 다시 보낸다
            }

            if (websocket_slots_.size() <= symbol)
            {
                websocket_slots_.resize(std::max<size_t>(symbols_.table.capacity() + 1, symbol + 1));
            }

            websocket_slots_[symbol].priority = request.rank;
            break;
        }

        default:
            // 이 줄로는 구독·해지·칸 우선순위만 온다. 다른 낱말이 보이면 가르는 규칙과 보내는 쪽이 어긋난 것이다.
            LOG_ERROR("[Engine] 시세 제어 줄에 엉뚱한 낱말이 왔다 — " +
                      std::to_string(static_cast<int>(request.kind)));
            break;
        }
    }
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
