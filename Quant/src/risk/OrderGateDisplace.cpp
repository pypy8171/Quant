// 주문 게이트 — 교체 진입(자리·여력 판정, 최약체 고르기)과 진입 우선순위 표 조회. [why D-112]
#include "risk/OrderGate.h"

#include <format>

// ─── 교체 진입 ──────────────────────────────────────────────────────────────
//  슬롯이 꽉 찼을 때 "먼저 온 순서"가 하루 종일 자리를 지키는 것을 막는다.
//  여기서는 positions·priority·displace를 겹치지 않고 하나씩 잡는다. check()는 positions_mutex_ 안에서
//  EntryPriority의 priority_mutex_·displace_mutex_(둘 다 잎)를 잡는다(한도 거부 문구, 3c-1 쿨다운·슬롯 예약).
//  (헤더의 중첩 금지 규약 유지 — 각 구간에서 필요한 값만 복사해 나온다).
bool OrderGate::slots_full() const
{
    if (config_.max_concurrent_positions <= 0)
    {
        return false;
    }

    return ledger_.open_slot_count() >= static_cast<size_t>(config_.max_concurrent_positions);
}

// 신규 종목을 열 여력이 없는가. 교체 진입이 슬롯만 보면, 슬롯은 남았는데 총노출 상한에
//  닿아 매수가 전부 거부되는 상태에서 더 좋은 종목이 와도 최약체를 비우지 못한다.
//  2026-09-08 실측: 보유 12/25종목(슬롯 여유), 총노출 97.4M > 상한 96.0M, 교체 0회.
bool OrderGate::capacity_full() const
{
    if (slots_full())
    {
        return true;
    }

    const double equity = ledger_.equity();

    if (config_.max_gross_exposure_percent <= 0.0 || equity <= 0.0)
    {
        return false;
    }

    double gross = 0.0;
    {
        const PositionLedger::Reader ledger = ledger_.read();
        gross = gross_exposure(ledger); // check() §3d와 같은 셈 — 한 곳이라도 원가로 재면 판단이 갈린다
    }

    // 상한의 95%를 넘으면 여력 없음으로 본다. 정확히 상한에 닿기를 기다리면 한 종목분
    //  명목이 애매하게 남아 교체도 매수도 안 되는 구간이 생긴다.
    return gross >= config_.max_gross_exposure_percent * equity * 0.95;
}

OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account, symbol::SymbolId new_symbol) const
{
    DisplacePlan plan;

    // 거절 사유는 한 곳에서 기록한다(뒤의 거부 문구가 읽는다). 락 안에서는 부르지 않는다.
    auto decline = [&](std::string why) -> DisplacePlan
    {
        entry_priority_.note_decline(new_symbol, why);
        plan.reason = std::move(why);
        return plan;
    };

    if (!config_.displace_enabled || config_.max_concurrent_positions <= 0)
    {
        return plan;
    }

    // (0) 한도를 넘겨 들고 있으면(한도를 내린 날·이월 보유가 많은 날) 최약체 하나를 비워도 자리가 안 난다.
    //  그래도 비우면 수혜 종목의 보류 매수는 만료되고 매도만 남는다(09-14 실측: 한도 40→30 뒤 204270·388050
    //  이월분이 z=-2.5로 밀려 나갔는데 105560·067290은 끝내 못 들어옴). 보유가 한도 밑으로 내려갈 때까지 교체는 쉰다.
    {
        const auto open = ledger_.open_slot_count();
        const auto capture  = static_cast<size_t>(config_.max_concurrent_positions);

        if (open > capture)
        {
            return decline(std::format("보유 {} > 한도 {} — 하나 비워도 자리가 안 나 교체 보류", open, capture));
        }
    }

    // (1) 신규 종목의 점수. 점수를 모르면 교체 근거가 없다.
    //  표는 불변 스냅샷이라 포인터만 들고 락 밖에서 읽는다 — 맵을 통째로 복사해 들고 나오던 비용이 없다.
    const std::shared_ptr<const PriorityTable> table = entry_priority_.snapshot();

    if (!table || !EntryPriority::z_of(*table, new_symbol, plan.new_z))
    {
        return decline("신규 종목 점수 없음");
    }

    // (2) 당일 교체 횟수·슬롯 예약 상태. 이미 비워 둔 슬롯이 있으면 또 비우지 않는다.
    const auto  now = Clock::now();
    std::string why = entry_priority_.refusal(new_symbol, now, config_.displace_max_per_day, ledger_.symbols());

    if (!why.empty())
    {
        return decline(why);
    }

    // (3) 보유분 중 최약체. 거절 문구도 같은 장부 잠금 안에서 만든다.
    WeakestHolding weakest;
    {
        const PositionLedger::Reader ledger = ledger_.read();
        weakest = find_weakest_holding(ledger, *table, new_symbol, now);

        if (weakest.key.symbol == symbol::kNone)
        {
            why = "내보낼 보유 종목 없음(전부 최소 보유 미달·매도 중·매도 불가)";
        }
        else if (plan.new_z - weakest.z_score < config_.displace_min_z_gap)
        {
            // 격차가 잡음 수준이면 비용만 나간다
            why = std::format("점수 격차 {:.2f}σ < {:.2f}σ (최약체 z={:.2f})",
                              plan.new_z - weakest.z_score, config_.displace_min_z_gap, weakest.z_score);
        }
    }

    if (!why.empty())
    {
        return decline(why);
    }

    // (4) 내보낼 종목의 계좌·종목·수량·평단.
    fill_victim(weakest.key, plan);

    if (plan.quantity <= 0)
    {
        return decline("최약체 " + plan.ticker + " 매도 가능 수량 0");
    }

    (void)account; // 계좌는 피교체 종목 쪽에서 복원한다(신호 계좌와 다를 수 있음)
    plan.victim_z = weakest.z_score;
    plan.reason = std::format("교체 진입 — {}(z={:.2f})가 {}(z={:.2f})보다 {:.2f}σ 높아 슬롯을 넘긴다",
                              ledger_.symbols().name(new_symbol).view(), plan.new_z, plan.ticker, plan.victim_z,
                              plan.new_z - plan.victim_z);
    plan.ok = true;
    entry_priority_.clear_decline(new_symbol);
    return plan;
}

// 교체 (3) — 보유분 중 최약체. 점수를 아는 종목만 대상 — 스캔 유니버스 밖 보유분(청산 관리,
//  전일 물린 물량)은 이 판정의 모집단이 아니다. 점수가 없는 것과 낮은 것은 다르다.
//  못 찾으면 key.symbol이 kNone으로 남는다. [inv] ledger(positions_mutex_)를 쥔 채 부른다.
OrderGate::WeakestHolding OrderGate::find_weakest_holding(const PositionLedger::Reader& ledger,
                                                          const PriorityTable& table, symbol::SymbolId new_symbol,
                                                          TimePoint now) const
{
    WeakestHolding weakest;

    for (const auto& entry : ledger.positions())
    {
        if (entry.second <= 0)
        {
            continue;
        }

        const PosKey& key = entry.first;

        if (key.symbol == new_symbol || ledger.slot_exempt().contains(key.symbol)) // 바스켓 소유 종목은 교체 후보가 아니다 [why D-109]
        {
            continue;
        }

        double cand_z = 0.0;

        if (EntryPriority::z_of(table, key.symbol, cand_z))
        {
            // 점수를 안다
        }
        else if (config_.displace_unscored_z != 0.0)
        {
            // 오늘 어느 슬리브의 스캔에도 안 잡힌 보유분. 점수가 없는 게 아니라 오늘 필터를
            //  통과하지 못한 것이라, 통과한 종목들 아래로 내려 교체 후보에 넣는다.
            //  이 경로가 없으면 전일 이월분이 슬롯을 영구히 점유한다.
            cand_z = config_.displace_unscored_z;
        }
        else
        {
            continue; // 점수 미상 — 교체 대상 아님(기존 동작)
        }

        // 미체결 매도가 이미 걸린 종목은 건드리지 않는다(중복 매도).
        auto reserved_found = ledger.reserved().find(key);
        const int sell_pending = (reserved_found != ledger.reserved().end()) ? reserved_found->second.sell : 0;

        if (sell_pending >= entry.second)
        {
            continue;
        }

        // 이전 세션이 낸 미체결 매도는 reserved_에 없다 — 프로세스 메모리라 재기동으로
        //  사라졌다. 그 수량은 잔고 시드(ord_psbl_qty)를 거쳐 sellable_에만 남으므로
        //  여기서도 같이 본다. 안 보면 팔 수 없는 종목을 매번 최약체로 골라 교체가 헛돈다
        //  (09-09: 000215 4회, 001120 1회. 그동안 진짜 팔 수 있는 하위 종목은 그대로 있었다).
        //  상한 계산은 clamp_buy_quantity의 SELL 분기와 같은 규칙을 쓴다.
        int cand_cap = entry.second;
        auto sellable_iterator = ledger.sellable().find(key);

        if (sellable_iterator != ledger.sellable().end() && sellable_iterator->second < cand_cap)
        {
            cand_cap = sellable_iterator->second;
        }

        if (cand_cap - sell_pending <= 0)
        {
            continue;
        }

        // 방금 산 종목은 빼지 않는다. 사자마자 파는 왕복은 비용만 남는다.
        auto opened_iterator = ledger.opened_at().find(key);

        if (opened_iterator != ledger.opened_at().end() && config_.displace_min_hold_sec > 0 &&
            now - opened_iterator->second < std::chrono::seconds(config_.displace_min_hold_sec))
        {
            continue;
        }

        if (weakest.key.symbol == symbol::kNone || cand_z < weakest.z_score)
        {
            weakest.key = key;
            weakest.z_score   = cand_z;
        }
    }

    return weakest;
}

// 교체 (4) — 내보낼 종목의 계좌·종목·매도 가능 수량(보유와 KIS 상한 중 작은 쪽 - 미체결 매도)·평단을 plan에 채운다.
//  장부 잠금을 새로 쥔다 — (3)과 (4) 사이에 잠금을 놓는 것은 원래 순서 그대로다.
void OrderGate::fill_victim(const PosKey& victim, DisplacePlan& plan) const
{
    const PositionLedger::Reader ledger = ledger_.read();

    plan.account = ledger.keys().account_of(victim);
    plan.ticker  = ledger.keys().ticker_of(victim).string();
    plan.symbol  = victim.symbol;
    auto position_iterator = ledger.positions().find(victim);
    auto reserved_found  = ledger.reserved().find(victim);
    const int sell_pending = (reserved_found != ledger.reserved().end()) ? reserved_found->second.sell : 0;
    int capture = (position_iterator != ledger.positions().end() ? position_iterator->second : 0);
    auto sellable_iterator = ledger.sellable().find(victim);

    if (sellable_iterator != ledger.sellable().end() && sellable_iterator->second < capture)
    {
        capture = sellable_iterator->second;
    }

    plan.quantity = capture - sell_pending;
    auto average_price_iterator = ledger.average_prices().find(victim);
    plan.average_price = (average_price_iterator != ledger.average_prices().end()) ? average_price_iterator->second : 0.0;
}

void OrderGate::note_displacement(const DisplacePlan& plan, symbol::SymbolId beneficiary)
{
    if (!plan.ok)
    {
        return;
    }

    entry_priority_.note_displacement(plan.symbol, beneficiary, Clock::now(), config_.displace_cooldown_sec,
                                      config_.displace_slot_hold_sec, ledger_.symbols().capacity());
}

// ─── 진입 우선순위 표 ─────────────────────────────────────────────────────────
void OrderGate::set_entry_priority(const std::vector<PriorityEntry>& entries, int total)
{
    entry_priority_.set(entries, total, ledger_.symbols().capacity());
}

OrderGate::EntrySnapshot OrderGate::entry_snapshot(const std::string& account, const std::string& ticker) const
{
    EntrySnapshot snapshot;
    const PositionLedger::Reader ledger = ledger_.read();

    const auto key = ledger.keys().lookup(account, ticker);
    auto position_iterator = ledger.positions().find(key);
    snapshot.position = (position_iterator != ledger.positions().end()) ? position_iterator->second : 0;

    auto reserved_it = ledger.reserved().find(key);
    snapshot.reserved = (reserved_it != ledger.reserved().end()) ? reserved_it->second.net() : 0;

    if (config_.max_concurrent_positions > 0)
    {
        // 바스켓 슬리브 소유 종목은 자리를 안 먹는다 — check()·slots_full()·plan_displacement()와 같은 규칙이다.
        //  여기만 빼먹고 세던 때는, 교체를 켠 날 바스켓을 든 계좌에서 "자리 꽉 참"이 잘못 나고
        //  plan_displacement가 자리 남음을 따로 안 보기 때문에 멀쩡한 최약체를 팔아 이미 있던 자리를 만들었다.
        //  [why D-109]
        size_t open = 0;

        for (const auto& entry : ledger.positions())
        {
            if (entry.second > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
            {
                ++open;
            }
        }

        for (const auto& entry : ledger.reserved())
        {
            if (entry.second.buy > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
            {
                auto iterator = ledger.positions().find(entry.first);

                if (iterator == ledger.positions().end() || iterator->second <= 0)
                {
                    ++open;
                }
            }
        }

        snapshot.slots_full = open >= static_cast<size_t>(config_.max_concurrent_positions);
    }

    return snapshot;
}
