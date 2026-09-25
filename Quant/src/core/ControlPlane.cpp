// 제어 요청 통로 구현 — 싣기·옮기기·적용과 보호 주문 창구. 스레드 규칙은 Quant/include/core/ControlPlane.h 머리 주석.
//  관련 결정: D-114(프로세스 분리와 제어 면), D-112(종목 번호), 원칙 4(주문 쪽 표는 주문 스레드만 고친다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  send()·send_switch() : 요청을 내는 쪽 전부(종목·전략 등록, 스위치 다섯, 슬롯 면제·진입 우선순위 표, 구독)
//  relay()              : strategy_thread_fn() 가 한 바퀴마다, 번호를 기다리는 쪽(request_*_registration)이 직접
//  apply()              : order_thread_fn() 가 한 바퀴마다
//  pop_feed()           : Engine::apply_feed_control_requests() — 시세 쪽 감시 스레드

#include "core/ControlPlane.h"
#include "core/LatencyTrace.h"
#include "risk/OrderGate.h"
#include "risk/ProtectiveOrders.h"
#include "utils/Logger.h"
#include <utility>
#include <vector>

ControlPlane::ControlPlane(OrderGate& order_gate, risk::ProtectiveOrderBook& protective_book, symbol::SymbolTable& table,
                           wake::WakeGate& strategy_wake, wake::WakeGate& order_wake, std::function<void()> reset_daily)
    : order_gate_(order_gate), protective_book_(protective_book), table_(table), strategy_wake_(strategy_wake),
      order_wake_(order_wake), reset_daily_(std::move(reset_daily))
{
}

void ControlPlane::bind(Lane* order_lane, Lane* feed_lane)
{
    order_lane_ = order_lane;
    feed_lane_  = feed_lane;
}

bool ControlPlane::send(ipc::ControlRequest& request)
{
    request.sequence   = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    request.sent_at_ns = trace::now_ns();

    if (outbox_.push(request))
    {
        // 깨울 쪽은 옮겨 줄 전략 스레드다. 주문 스레드는 옮기는 쪽이 옮긴 뒤 깨운다. [why D-114]
        strategy_wake_.notify();
        return true;
    }

    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void ControlPlane::send_switch(ipc::ControlRequest& request, std::string_view what)
{
    if (send(request))
    {
        return;
    }

    // 제어 큐가 가득 찼다. 사라진 것이 kill switch 일 수 있어 조용히 넘기지 않는다.
    LOG_ERROR("[Engine] 주문 쪽에 " + std::string(what) + " 요청을 못 실었다 — 제어 큐가 가득 찼다");
}

void ControlPlane::relay()
{
    // 꺼내는 쪽을 하나로 묶는다 — 평소에는 전략 스레드가, 번호를 기다리는 동안에는 기다리는 쪽이 부른다.
    std::lock_guard<std::mutex> relay_lock(relay_mutex_);

    bool moved = false;

    while (auto option = outbox_.pop())
    {
        // 낱말로 줄을 가른다 — 구독·해지는 소켓을 쥔 시세 쪽으로, 나머지 표 고치기는 원장을 쥔 주문 쪽으로.
        //  역할이 아니라 낱말로 가르는 것은 한 프로세스로 돌 때도 같은 길을 타야 갈라 띄운 날과 동작이
        //  같기 때문이다(줄이 둘 다 이 프로세스 안에 있을 뿐이다). [why D-114 단계 5]
        const bool to_feed = ipc::routes_to_feed(option->kind);
        Lane&      lane    = to_feed ? *feed_lane_ : *order_lane_;

        if (!lane.push(*option))
        {
            // 보낸 쪽은 이미 성공을 받아 갔다 — 여기서 조용히 버리면 사라진 표를 아무도 모른다.
            relay_dropped_.fetch_add(1, std::memory_order_relaxed);
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
        order_wake_.notify(); // 자고 있으면 깨운다 — 잠드는 조건이 이 면도 본다
    }
}

void ControlPlane::apply()
{
    auto& ledger = order_gate_.ledger();

    ipc::ControlRequest request;

    while (order_lane_->pop(request))
    {
        switch (request.kind)
        {
        case ipc::ControlKind::kSlotExemptBegin:
            slot_exempt_.begin(request.sequence);
            break;

        case ipc::ControlKind::kSlotExemptEntry:
            slot_exempt_.add(request);
            break;

        case ipc::ControlKind::kSlotExemptCommit:
            if (slot_exempt_.commit(request.batch, request.row_count))
            {
                std::vector<symbol::SymbolId> symbols;
                symbols.reserve(slot_exempt_.rows().size());

                for (const ipc::ControlRequest& row : slot_exempt_.rows())
                {
                    symbols.push_back(row.symbol_id);
                }

                ledger.set_slot_exempt_by_id(symbols);
            }

            break;

        case ipc::ControlKind::kEntryPriorityBegin:
            entry_priority_.begin(request.sequence);
            break;

        case ipc::ControlKind::kEntryPriorityEntry:
            entry_priority_.add(request);
            break;

        case ipc::ControlKind::kEntryPriorityCommit:
            if (entry_priority_.commit(request.batch, request.row_count))
            {
                std::vector<OrderGate::PriorityEntry> entries;
                entries.reserve(entry_priority_.rows().size());

                for (const ipc::ControlRequest& row : entry_priority_.rows())
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
            rule.ticker            = table_.name(request.symbol_id).string();
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
        //  표가 차면 intern이 0을 돌려주고 표에는 아무것도 안 남는다. 묻는 쪽은 번호가 뜨기를 5분 기다리다
        //  빈손으로 돌아가므로(EngineSymbols.cpp의 wait_for_shared_id) 여기서 말하지 않으면 왜 멎었는지가
        //  로그 어디에도 없다 — 2026-09-25 부하시험에서 이 침묵으로 원인을 찾는 데 시간을 썼다.
        case ipc::ControlKind::kRegisterSymbol:
            if (table_.intern(request.ticker.view()) == symbol::kNone)
            {
                LOG_ERROR("[ControlPlane] 종목 표에 못 넣었다 — 표가 찼거나 코드가 칸을 넘는다 종목=" +
                          std::string(request.ticker.view()) + " 든 것=" + std::to_string(table_.size()) +
                          " 상한=" + std::to_string(table_.capacity()));
            }

            break;

        // 전략 이름표에 넣는 자리도 여기 하나다 — 이름과 번호가 갈리면 서브원장 귀속이 남의 전략에 붙는다.
        //  못 넣으면 위와 같은 까닭으로 말한다.
        case ipc::ControlKind::kRegisterStrategy:
            if (ledger.strategy_index_of(request.strategy_name.view()) == strategy_table::kNone)
            {
                LOG_ERROR("[ControlPlane] 전략 이름표에 못 넣었다 — 표가 찼거나 이름이 칸을 넘는다 전략=" +
                          std::string(request.strategy_name.view()) + " 든 것=" +
                          std::to_string(ledger.strategy_table().size()) + " 상한=" +
                          std::to_string(ledger.strategy_table().capacity()));
            }

            break;

        // 주문 쪽 스위치를 고치는 자리도 여기 하나다 — 전략 쪽은 요청만 보낸다.
        case ipc::ControlKind::kResetDaily:
            reset_daily_();
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
        //  (ipc::routes_to_feed). 받는 자리는 Engine::apply_feed_control_requests 하나다. [why D-114 단계 5]

        default:
            break;
        }
    }

    discarded_.store(slot_exempt_.discarded() + entry_priority_.discarded(), std::memory_order_relaxed);
}

bool ControlPlane::pop_feed(ipc::ControlRequest& request)
{
    return feed_lane_->pop(request);
}

bool ControlPlane::order_lane_empty() const
{
    return order_lane_->readable() == 0;
}

ControlPlane::ProtectiveRegistry::ProtectiveRegistry(ControlPlane& plane) : plane_(plane)
{
}

void ControlPlane::ProtectiveRegistry::arm(const risk::ProtectiveRule& rule)
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

    if (!plane_.send(request))
    {
        LOG_WARN("[Engine] 보호 주문 등록을 못 보냈다 — " + rule.ticker + " 는 표가 지키지 않는다");
    }
}

void ControlPlane::ProtectiveRegistry::disarm(const std::string& account, symbol::SymbolId symbol)
{
    if (symbol == symbol::kNone)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kDisarmProtective;
    request.symbol_id = symbol;
    ipc::set_account(request, account);

    if (!plane_.send(request))
    {
        LOG_WARN("[Engine] 보호 주문 해제를 못 보냈다 — 떨어진 전략의 규칙이 표에 남는다");
    }
}

bool ControlPlane::ProtectiveRegistry::owns(const std::string& account, symbol::SymbolId symbol) const
{
    return plane_.protective_book_.owns(account, symbol);
}

bool ControlPlane::ProtectiveRegistry::consume_fired(const std::string& account, symbol::SymbolId symbol)
{
    return plane_.protective_book_.consume_fired(account, symbol);
}
