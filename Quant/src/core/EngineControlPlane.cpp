// 제어 요청 통로 — 딴 스레드가 주문 쪽(OrderGate·원장)에 부탁하는 요청을 보내고, 옮기고, 적용한다.
//  Engine 클래스는 그대로다. Engine.cpp 가 2,300줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  send_control()                 : 요청을 내는 쪽 전부(종목·전략 등록, 스위치 다섯, 슬롯 면제, 보호 주문 등록)
//  relay_control_requests()       : strategy_thread_fn() 가 한 바퀴마다 — 전략 쪽 요청을 주문 쪽 통로로 옮긴다
//  apply_control_requests()       : order_thread_fn() 가 한 바퀴마다 — 주문 스레드가 적용한다
//  apply_feed_control_requests()  : control_thread_fn() 가 — 시세 쪽 요청(구독 등)을 적용한다
//  request_*() · apply_reset_daily() : 매크로 국면 폴링·ZMQ·운영단말·개장 전이

#include "core/Engine.h"
#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include <mutex>
#include <string>

// ─── 주문 쪽 스위치 다섯 ──────────────────────────────────────────────────
//  OrderGate·원장은 주문 프로세스 것이다. 고치는 일은 주문 스레드가 하고(D-071 원칙 4), 딴 스레드는
//  제어 요청 한 줄로 부탁한다. 통로를 탈지 그 자리에서 고칠지는 **부르는 자리가 어느 프로세스 것인가**로
//  갈린다 — 역할 이름이 아니다. [why D-114]
//  - 신규진입 정지·매수 비율: 매크로 국면 폴링(전략 쪽 일감)만 부른다 → 통로
//  - 하루치 새로 열기·전방향 차단·수동 정지: 개장 전이(order_side 가드)·ZMQ·시세 감시·운영단말만 부르고
//    그 넷은 모두 주문 프로세스 것이다 → 그 자리에서 고친다. 전략 역할 분기는 남의 손이 닿을 때의 안전망이다

void Engine::send_control_switch(ipc::ControlRequest& request, std::string_view what)
{
    if (send_control(request))
    {
        return;
    }

    // 제어 큐가 가득 찼다. 사라진 것이 kill switch 일 수 있어 조용히 넘기지 않는다.
    LOG_ERROR("[Engine] 주문 쪽에 " + std::string(what) + " 요청을 못 실었다 — 제어 큐가 가득 찼다");
}

void Engine::apply_reset_daily()
{
    order_gate_.reset_daily();

    if (order_router_)
    {
        order_router_->reset_daily(); // V-4: 중복방지 키 일별 정리(거래일 prefix와 함께 cross-day 충돌 차단)
    }

    ledger_->new_trading_day(); // C-1: 새 거래일 → 총평가금 기준선 재캡처
}

void Engine::request_reset_daily()
{
    // 부르는 자리는 데이터 스레드의 장 시작 감지라 역할 셋이 다 지난다. 원장·게이트를 든 쪽만 그 자리서
    //  고치고, 전략은 통로로 청하고, 시세는 고칠 것이 없어 지나간다. [why D-114 단계 5]
    if (runs_order_side())
    {
        apply_reset_daily();
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind = ipc::ControlKind::kResetDaily;
    send_control_switch(request, "하루치 새로 열기");
}

void Engine::request_entry_halt(bool on)
{
    // 부르는 자리는 매크로 국면 폴링 하나이고 그것은 전략 쪽 일감이다 — Both 로 돌아도 같은 통로를 태운다.
    //  여기서 Both 만 질러가게 두면 통로가 갈라 띄운 날 처음 돈다. [why D-114]
    //  [inv] 주문 프로세스에는 옮겨 줄 전략 스레드가 없다 — 그 역할이면 넣지 않고 그 자리에서 고친다.
    if (role_ == ProcessRole::Order)
    {
        order_gate_.set_entry_halt(on);
        return;
    }

    // 시세 프로세스는 제어 줄의 보내는 쪽이 아니다 — 여기서 보내면 SPSC가 깨진다. [why D-114 단계 5]
    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kEntryHalt;
    request.toggle_on = on ? 1 : 0;
    send_control_switch(request, "신규 진입 정지");
}

void Engine::request_entry_scale(double entry_scale)
{
    // 신규진입 정지와 같은 자리에서 같은 판정으로 나온다 — 통로도 같다. [why D-114]
    if (role_ == ProcessRole::Order)
    {
        order_gate_.set_entry_scale(entry_scale);
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind        = ipc::ControlKind::kEntryScale;
    request.entry_scale = entry_scale;
    send_control_switch(request, "매수 비율");
}

void Engine::request_kill_switch(bool on)
{
    if (runs_order_side())
    {
        order_gate_.set_kill_switch(on);
        return;
    }

    // 시세 프로세스에는 게이트가 없다 — 주문을 내지 않으므로 막을 것도 없다. [why D-114 단계 5]
    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kKillSwitch;
    request.toggle_on = on ? 1 : 0;
    send_control_switch(request, "전방향 주문 차단");
}

void Engine::request_manual_halt(OrderSide side, bool on)
{
    if (runs_order_side())
    {
        order_gate_.set_manual_halt(side, on);
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kManualHalt;
    request.halt_side = static_cast<uint8_t>(static_cast<OrderSide::Value>(side));
    request.toggle_on = on ? 1 : 0;
    send_control_switch(request, "수동 정지");
}

bool Engine::send_control(ipc::ControlRequest& request)
{
    request.sequence   = pipeline_.control_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    request.sent_at_ns = trace::now_ns();

    if (pipeline_.strategy_control_outbox.push(request))
    {
        // 깨울 쪽은 옮겨 줄 전략 스레드다. 주문 스레드는 옮기는 쪽이 옮긴 뒤 깨운다. [why D-114]
        pipeline_.strategy_wake.notify();
        return true;
    }

    pipeline_.control_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Engine::set_slot_exempt_tickers(const std::vector<std::string>& tickers)
{
    ipc::ControlRequest open;
    open.kind = ipc::ControlKind::kSlotExemptBegin;

    if (!send_control(open))
    {
        LOG_WARN("[Engine] 슬롯 면제 " + std::to_string(tickers.size()) + "종목 — 제어 통로가 가득 차 이번 판을 접는다");
        return;
    }

    uint32_t sent = 0;

    for (const auto& ticker : tickers)
    {
        ipc::ControlRequest row;
        row.kind      = ipc::ControlKind::kSlotExemptEntry;
        row.batch     = open.sequence;
        row.symbol_id = register_symbol(ticker); // 티커 문자열이 번호가 되는 경계 [why D-112]

        if (row.symbol_id == symbol::kNone)
        {
            continue;
        }

        // 한 줄이라도 못 보내면 commit 을 안 보내고 접는다 — 받는 쪽은 commit 없는 표를 걸지 않는다.
        if (!send_control(row))
        {
            LOG_WARN("[Engine] 슬롯 면제 집합을 보내다 통로가 가득 찼다 — 이번 판을 접는다(보낸 " +
                     std::to_string(sent) + "줄)");
            return;
        }

        ++sent;
    }

    ipc::ControlRequest close;
    close.kind      = ipc::ControlKind::kSlotExemptCommit;
    close.batch     = open.sequence;
    close.rank      = static_cast<int32_t>(sent);
    close.row_count = sent;

    if (!send_control(close))
    {
        LOG_WARN("[Engine] 슬롯 면제 집합 마무리를 못 보냈다 — 이번 판은 걸리지 않는다");
    }
}

Engine::ControlProtectiveRegistry::ControlProtectiveRegistry(Engine& engine) : engine_(engine)
{
}

void Engine::ControlProtectiveRegistry::arm(const risk::ProtectiveRule& rule)
{
    if (rule.symbol == symbol::kNone)
    {
        return; // 번호가 없으면 표가 어차피 받지 않는다(ProtectiveOrderBook::arm) — 요청도 안 보낸다
    }

    ipc::ControlRequest request;
    request.kind              = ipc::ControlKind::kArmProtective;
    request.symbol_id         = rule.symbol;
    request.owner_index       = rule.owner_index;
    request.stop_loss_percent = rule.stop_loss_percent;
    request.trail_arm_percent = rule.trail_arm_percent;
    request.trail_percent     = rule.trail_percent;
    ipc::set_account(request, rule.account);

    if (!engine_.send_control(request))
    {
        LOG_WARN("[Engine] 보호 주문 등록을 못 보냈다 — " + rule.ticker + " 는 표가 지키지 않는다");
    }
}

void Engine::ControlProtectiveRegistry::disarm(const std::string& account, symbol::SymbolId symbol)
{
    if (symbol == symbol::kNone)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kDisarmProtective;
    request.symbol_id = symbol;
    ipc::set_account(request, account);

    if (!engine_.send_control(request))
    {
        LOG_WARN("[Engine] 보호 주문 해제를 못 보냈다 — 떨어진 전략의 규칙이 표에 남는다");
    }
}

bool Engine::ControlProtectiveRegistry::owns(const std::string& account, symbol::SymbolId symbol) const
{
    return engine_.protective_book_.owns(account, symbol);
}

bool Engine::ControlProtectiveRegistry::consume_fired(const std::string& account, symbol::SymbolId symbol)
{
    return engine_.protective_book_.consume_fired(account, symbol);
}

void Engine::relay_control_requests()
{
    // 꺼내는 쪽을 하나로 묶는다 — 평소에는 전략 스레드가, 번호를 기다리는 동안에는 기다리는 쪽이 부른다.
    std::lock_guard<std::mutex> relay_lock(pipeline_.control_relay_mutex);

    bool moved = false;

    while (auto option = pipeline_.strategy_control_outbox.pop())
    {
        // 낱말로 줄을 가른다 — 구독·해지는 소켓을 쥔 시세 쪽으로, 나머지 표 고치기는 원장을 쥔 주문 쪽으로.
        //  역할이 아니라 낱말로 가르는 것은 한 프로세스로 돌 때도 같은 길을 타야 갈라 띄운 날과 동작이
        //  같기 때문이다(줄이 둘 다 이 프로세스 안에 있을 뿐이다). [why D-114 단계 5]
        const bool to_feed = ipc::routes_to_feed(option->kind);
        auto&      lane    = to_feed ? *pipeline_.feed_controls : *pipeline_.controls;

        if (!lane.push(*option))
        {
            // 보낸 쪽은 이미 성공을 받아 갔다 — 여기서 조용히 버리면 사라진 표를 아무도 모른다.
            pipeline_.control_relay_dropped.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR(std::string("[Engine] 제어 요청을 ") + (to_feed ? "시세" : "주문") +
                      " 쪽에 못 옮겼다 — 경계 너머 제어 면이 가득 찼다");
            continue;
        }

        if (!to_feed)
        {
            moved = true; // 깨울 쪽은 주문 스레드다. 시세 쪽은 건너편 프로세스라 깨울 수 없다 — 제 바퀴에서 본다
        }
    }

    if (moved)
    {
        pipeline_.order_wake.notify(); // 자고 있으면 깨운다 — 잠드는 조건이 이 면도 본다
    }
}

void Engine::apply_control_requests(ControlInbox& inbox)
{
    auto& ledger = order_gate_.ledger();

    ipc::ControlRequest request;

    while (pipeline_.controls->pop(request))
    {
        switch (request.kind)
        {
        case ipc::ControlKind::kSlotExemptBegin:
            inbox.slot_exempt.begin(request.sequence);
            break;

        case ipc::ControlKind::kSlotExemptEntry:
            inbox.slot_exempt.add(request);
            break;

        case ipc::ControlKind::kSlotExemptCommit:
            if (inbox.slot_exempt.commit(request.batch, request.row_count))
            {
                std::vector<symbol::SymbolId> symbols;
                symbols.reserve(inbox.slot_exempt.rows().size());

                for (const ipc::ControlRequest& row : inbox.slot_exempt.rows())
                {
                    symbols.push_back(row.symbol_id);
                }

                ledger.set_slot_exempt_by_id(symbols);
            }

            break;

        case ipc::ControlKind::kEntryPriorityBegin:
            inbox.entry_priority.begin(request.sequence);
            break;

        case ipc::ControlKind::kEntryPriorityEntry:
            inbox.entry_priority.add(request);
            break;

        case ipc::ControlKind::kEntryPriorityCommit:
            if (inbox.entry_priority.commit(request.batch, request.row_count))
            {
                std::vector<OrderGate::PriorityEntry> entries;
                entries.reserve(inbox.entry_priority.rows().size());

                for (const ipc::ControlRequest& row : inbox.entry_priority.rows())
                {
                    entries.push_back({row.symbol_id, row.rank, row.score_z});
                }

                order_gate_.set_entry_priority(entries, request.rank);
            }

            break;

        case ipc::ControlKind::kArmProtective:
        {
            // 레코드에 문자열을 안 실으므로 티커·전략 이름은 번호로 되찾는다(로그·신호 strategy_id 용도다).
            risk::ProtectiveRule rule;
            rule.account           = std::string(ipc::account_of(request));
            rule.ticker            = symbols_.table.name(request.symbol_id).string();
            rule.symbol            = request.symbol_id;
            rule.stop_loss_percent = request.stop_loss_percent;
            rule.trail_arm_percent = request.trail_arm_percent;
            rule.trail_percent     = request.trail_percent;
            rule.owner             = ledger.strategy_table().name(request.owner_index).string();
            rule.owner_index       = request.owner_index;
            protective_book_.arm(rule);
            break;
        }

        case ipc::ControlKind::kDisarmProtective:
            protective_book_.disarm(std::string(ipc::account_of(request)), request.symbol_id);
            break;

        // 종목 표에 넣는 자리는 여기 하나다 — 전략 쪽은 이 요청을 보내고 번호는 같은 표에서 읽어 간다.
        case ipc::ControlKind::kRegisterSymbol:
            (void)symbols_.table.intern(request.ticker.view());
            break;

        // 전략 이름표에 넣는 자리도 여기 하나다 — 이름과 번호가 갈리면 서브원장 귀속이 남의 전략에 붙는다.
        case ipc::ControlKind::kRegisterStrategy:
            (void)ledger.strategy_index_of(request.strategy_name.view());
            break;

        // 주문 쪽 스위치를 고치는 자리도 여기 하나다 — 전략 쪽은 요청만 보낸다.
        case ipc::ControlKind::kResetDaily:
            apply_reset_daily();
            break;

        case ipc::ControlKind::kEntryHalt:
            order_gate_.set_entry_halt(request.toggle_on != 0);
            break;

        case ipc::ControlKind::kEntryScale:
            order_gate_.set_entry_scale(request.entry_scale);
            break;

        case ipc::ControlKind::kKillSwitch:
            order_gate_.set_kill_switch(request.toggle_on != 0);
            break;

        case ipc::ControlKind::kManualHalt:
            order_gate_.set_manual_halt(request.halt_side == OrderSide::SELL ? OrderSide::SELL : OrderSide::BUY,
                                        request.toggle_on != 0);
            break;

        // 구독 낱말은 이 줄로 오지 않는다 — 소켓이 시세로 옮겨 가면서 구독 요청도 전략 → 시세 줄로 갈렸다
        //  (ipc::routes_to_feed). 받는 자리는 apply_feed_control_requests 하나다. [why D-114 단계 5]

        default:
            break;
        }
    }

    const uint64_t discarded = inbox.slot_exempt.discarded() + inbox.entry_priority.discarded();
    pipeline_.control_discarded.store(discarded, std::memory_order_relaxed);
}

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

    while (pipeline_.feed_controls->pop(request))
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

            // 목록에 이미 있어도 구독은 건다 — Both 로 돌면 connect_feed() 가 채운 목록에 그대로 들어 있다.
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
            // 이 줄로는 구독·해지만 온다. 다른 낱말이 보이면 가르는 규칙과 보내는 쪽이 어긋난 것이다.
            LOG_ERROR("[Engine] 시세 제어 줄에 엉뚱한 낱말이 왔다 — " +
                      std::to_string(static_cast<int>(request.kind)));
            break;
        }
    }
}
