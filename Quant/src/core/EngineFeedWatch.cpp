// 구독 목록 — 소켓 수 세기, 전략들이 보는 종목을 구독 목록으로 모으기, 구독·해제 요청을 소켓에 걸기.
//  EngineFeed.cpp 에서 떼어 낸 것이다(같은 Engine 의 멤버 함수 본체). 부르는 자리는 EngineFeed.cpp 머리 주석. 관련 결정: D-071·D-114.

#include "core/Engine.h"
#include "utils/Logger.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

    return ShardPipeline::kDefaultWebsocketLanes;
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

    watch_specifications_generation_.fetch_add(1, std::memory_order_relaxed);
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

    // 고정 종목은 목록 맨 앞에 체결만으로 넣는다 — 연결 때 앞에서부터 칸을 채우므로 상한에 밀리지 않는다. [why D-138]
    for (auto pin = websocket_pin_tickers_.rbegin(); pin != websocket_pin_tickers_.rend(); ++pin)
    {
        const bool already = std::any_of(watch_specifications_.begin(), watch_specifications_.end(),
                                         [&pin](const WatchSpec& watch)
                                         {
                                             return watch.market == Market::KR && watch.ticker == *pin;
                                         });

        if (!already)
        {
            WatchSpec specification;
            specification.ticker     = *pin;
            specification.market     = Market::KR;
            specification.trade_only = true;
            watch_specifications_.insert(watch_specifications_.begin(), std::move(specification));
        }

        LOG_INFO("[Engine] WS 고정 종목: " + *pin);
    }

    // 스레드가 뜨기 전이라 자물쇠는 없지만, 데이터 스레드 사본이 첫 바퀴에 이 목록을 뜨도록 판 번호는 올린다.
    watch_specifications_generation_.fetch_add(1, std::memory_order_relaxed);
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
            watch_specification.ticker == specification.ticker)
        {
            return false;
        }
    }

    watch_specifications_.push_back(specification);
    watch_specifications_generation_.fetch_add(1, std::memory_order_relaxed); // 자물쇠 안 — 데이터 스레드 사본이 다음 바퀴에 다시 뜬다
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
                          [&specification](const WatchSpec& watch)
                          {
                              return same_watch(watch, specification);
                          });
        }

        if (!unsubscriptions.empty())
        {
            watch_specifications_generation_.fetch_add(1, std::memory_order_relaxed);
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
