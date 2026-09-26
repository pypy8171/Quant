#include "strategy/TargetBasketStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace
{
constexpr double kSellCostRate = 0.00215; // 거래세 0.20% + 수수료 0.015%(왕복 어림). 실현손익 추정에만 쓴다

std::optional<json> read_json_file(const std::filesystem::path& path)
{
    std::ifstream stream(path);

    if (!stream)
    {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    json document = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);

    if (document.is_discarded())
    {
        return std::nullopt;
    }

    return document;
}

// 임시 파일에 쓰고 이름을 바꾼다 — 쓰는 중에 죽어도 반쪽 파일이 남지 않는다.
bool write_json_atomic(const std::filesystem::path& path, const json& document)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);

        if (!stream)
        {
            return false;
        }

        stream << document.dump(2);
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}
} // namespace

TargetBasketStrategy::TargetBasketStrategy(Params parameters, OwnedSink owned_sink)
    : parameters_(std::move(parameters)), owned_sink_(std::move(owned_sink)), clock_([]
    {
        return std::time(nullptr);
    }),
      id_("BASKET_" + parameters_.label)
{
}

std::string TargetBasketStrategy::describe() const
{
    return std::format("TargetBasket({} 시드 {:.0f}원 밴드 {:.0f}% 창 {}~{} {}) 파일 {}", parameters_.label, parameters_.capital_krw,
                       parameters_.band * 100.0, parameters_.window_start_hhmm, parameters_.window_end_hhmm,
                       parameters_.dry_run ? "dry-run" : "실주문", parameters_.targets_file);
}

int TargetBasketStrategy::kst_hhmm() const
{
    const auto time_of_day = kst::time_of_day(clock_());
    return static_cast<int>(time_of_day.hours().count()) * 100 + static_cast<int>(time_of_day.minutes().count());
}

std::string TargetBasketStrategy::kst_date() const
{
    return kst::date_yyyymmdd(clock_());
}

bool TargetBasketStrategy::load_targets()
{
    const std::filesystem::path path(parameters_.targets_file);
    std::error_code             error;
    const auto                  modified = std::filesystem::last_write_time(path, error);

    if (error)
    {
        if (!targets_)
        {
            LOG_WARN("[" + id_ + "] 목표 비중표 없음 " + parameters_.targets_file + " — 주문 없음");
        }

        return false;
    }

    if (targets_ && modified == targets_mtime_)
    {
        return false;
    }

    targets_mtime_ = modified;
    const auto document = read_json_file(path);

    if (!document)
    {
        LOG_WARN("[" + id_ + "] 목표 비중표 파싱 실패 " + parameters_.targets_file + " — 직전 것을 유지");
        return false;
    }

    std::string reason;
    auto        parsed = basket::parse_targets(*document, reason);

    if (!parsed)
    {
        LOG_WARN("[" + id_ + "] 목표 비중표 검증 실패: " + reason + " — 직전 것을 유지");
        return false;
    }

    targets_ = std::move(*parsed);
    owned_   = targets_->tickers();
    LOG_INFO(std::format("[{}] 목표 비중표 읽음 as_of={} generated_at={} 행 {} 슬리브 {}개 소유 종목 {}개{}", id_, targets_->as_of,
                         targets_->generated_at, targets_->count, targets_->sleeves.size(), owned_.size(),
                         targets_->liquidate_all ? " liquidate_all" : ""));
    publish_owned();
    return true;
}

void TargetBasketStrategy::publish_owned()
{
    if (owned_sink_)
    {
        owned_sink_(owned_);
    }
}

void TargetBasketStrategy::load_state()
{
    const auto document = read_json_file(parameters_.state_file);

    if (!document)
    {
        return;
    }

    realized_pnl_krw_ = document->value("realized_pnl_krw", 0.0);
    const auto day    = document->find("day");

    if (day == document->end() || !day->is_object())
    {
        return;
    }

    day_.date          = day->value("date", std::string());
    day_.targets_key   = day->value("targets_key", std::string());
    day_.sell_leg_done = day->value("sell_leg_done", false);
    day_.buy_leg_done  = day->value("buy_leg_done", false);
    day_.sell_leg_at   = day->value("sell_leg_at", static_cast<std::time_t>(0));
    const auto sent    = day->find("sent");

    if (sent != day->end() && sent->is_array())
    {
        for (const auto& entry : *sent)
        {
            if (entry.is_string())
            {
                day_.sent.insert(entry.get<std::string>());
            }
        }
    }

    LOG_INFO(std::format("[{}] 상태 읽음 date={} 보낸 주문 {}건 매도 레그 {} 매수 레그 {} 실현손익 {:.0f}원", id_, day_.date, day_.sent.size(),
                         day_.sell_leg_done ? "끝" : "전", day_.buy_leg_done ? "끝" : "전", realized_pnl_krw_));
}

void TargetBasketStrategy::save_state() const
{
    json document;
    document["schema"]           = 1;
    document["realized_pnl_krw"] = realized_pnl_krw_;
    json day;
    day["date"]          = day_.date;
    day["targets_key"]   = day_.targets_key;
    day["sell_leg_done"] = day_.sell_leg_done;
    day["buy_leg_done"]  = day_.buy_leg_done;
    day["sell_leg_at"]   = static_cast<long long>(day_.sell_leg_at);
    day["sent"]          = json::array();

    for (const auto& entry : day_.sent)
    {
        day["sent"].push_back(entry);
    }

    document["day"] = std::move(day);

    if (!write_json_atomic(parameters_.state_file, document))
    {
        LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패 " + parameters_.state_file + " — 재기동 시 같은 주문이 다시 나갈 수 있다");
    }
}

void TargetBasketStrategy::on_start()
{
    load_state();
    load_targets();
    publish_owned();
    LOG_INFO("[" + id_ + "] " + describe());
}

void TargetBasketStrategy::on_stop()
{
    save_state();
}

basket::Plan TargetBasketStrategy::current_plan() const
{
    basket::PlanInput input;
    input.capital_krw      = parameters_.capital_krw;
    input.realized_pnl_krw = realized_pnl_krw_;
    input.band             = parameters_.band;
    input.holding          = [this](const std::string& ticker)
    {
        basket::Holding holding;
        holding.quantity = confirmed_position(parameters_.account, ticker);
        const auto sellable = ledger_sellable(parameters_.account, ticker);

        if (sellable)
        {
            holding.sellable      = sellable->sellable;
            holding.average_price = sellable->average_price;
        }
        else
        {
            holding.sellable = holding.quantity;
        }

        return holding;
    };
    return basket::make_plan(*targets_, input);
}

// 한 레그의 주문을 예산만큼 낸다. 낸 것은 상태 파일에 먼저 적는다. 남은 주문이 없으면 true.
bool TargetBasketStrategy::emit_leg(std::vector<basket::PlannedOrder>& orders, std::vector<OrderSignal>& out, int& budget)
{
    bool remaining = false;

    for (auto& order : orders)
    {
        const std::string key = order.ticker + (order.side == OrderSide::BUY ? ":BUY" : ":SELL");
        auto&             sent = parameters_.dry_run ? dry_sent_ : day_.sent;

        if (sent.contains(key))
        {
            continue;
        }

        if (budget <= 0)
        {
            remaining = true;
            break;
        }

        --budget;
        sent.insert(key);
        const std::string line = std::format("{} {} {}주 @기준가 {:.0f} ({}) — {}", order.ticker, order.side == OrderSide::BUY ? "매수" : "매도",
                                             order.quantity, order.reference_price, order.strategy_id, order.reason);

        if (parameters_.dry_run)
        {
            LOG_INFO("[" + id_ + "] dry-run 주문 안 냄: " + line);
            continue;
        }

        if (order.side == OrderSide::SELL)
        {
            const auto sellable = ledger_sellable(parameters_.account, order.ticker);
            const double average_price = sellable ? sellable->average_price : 0.0;

            if (average_price > 0.0)
            {
                realized_pnl_krw_ += order.quantity * (order.reference_price - average_price) - order.quantity * order.reference_price * kSellCostRate;
            }
        }

        save_state(); // 먼저 적고 낸다 — 재기동 뒤 같은 주문이 두 번 나가지 않는다
        OrderSignal signal;
        signal.ticker          = order.ticker;
        signal.symbol_id       = symbol_of(order.ticker);
        signal.side            = order.side;
        signal.type            = OrderType::MARKET;
        signal.quantity        = order.quantity;
        signal.price           = 0.0;
        signal.reference_price = order.reference_price; // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
        signal.strategy_id     = order.strategy_id;
        signal.market          = Market::KR;
        signal.account_id      = parameters_.account;
        signal.reason          = order.reason;
        signal.timestamp       = std::chrono::system_clock::now();
        out.push_back(std::move(signal));
        LOG_INFO("[" + id_ + "] 주문: " + line);
    }

    return !remaining;
}

void TargetBasketStrategy::run_pass(std::vector<OrderSignal>& out)
{
    const auto now = std::chrono::steady_clock::now();

    if (last_reload_.time_since_epoch().count() == 0 || now - last_reload_ >= std::chrono::seconds(parameters_.reload_sec))
    {
        last_reload_ = now;
        load_targets();
    }

    if (!targets_ || parameters_.capital_krw <= 0.0)
    {
        return;
    }

    const int         hhmm  = kst_hhmm();
    const std::string today = kst_date();

    if (hhmm < parameters_.window_start_hhmm)
    {
        return;
    }

    if (targets_->as_of != today)
    {
        return; // 낡은 파일(전날 것) — 오늘 것이 올 때까지 아무것도 안 낸다
    }

    if (day_.date != today || day_.targets_key != targets_->key())
    {
        day_ = DayState{};
        day_.date        = today;
        day_.targets_key = targets_->key();
        save_state();
        LOG_INFO("[" + id_ + "] 오늘 집행 시작 " + today + " 파일 " + day_.targets_key);
    }

    if (day_.buy_leg_done)
    {
        return;
    }

    if (hhmm >= parameters_.window_end_hhmm)
    {
        if (!window_closed_logged_)
        {
            window_closed_logged_ = true;
            const auto plan = current_plan();
            LOG_WARN(std::format("[{}] 집행 창 종료({}) — 남은 매도 {}건 매수 {}건은 다음 날 파일이 다시 낸다", id_, hhmm, plan.sells.size(),
                                 plan.buys.size()));
            day_.sell_leg_done = true;
            day_.buy_leg_done  = true;
            save_state();
        }

        return;
    }

    auto plan   = current_plan();
    int  budget = parameters_.max_signals_per_pass;

    if (!day_.sell_leg_done)
    {
        if (day_.sent.empty() && dry_sent_.empty())
        {
            LOG_INFO(std::format("[{}] 계획 순자산 {:.0f}원 매도 {}건 매수 {}건 유지 {}건", id_, plan.nav, plan.sells.size(), plan.buys.size(),
                                 plan.notes.size()));
        }

        if (emit_leg(plan.sells, out, budget))
        {
            day_.sell_leg_done = true;
            day_.sell_leg_at   = clock_();
            save_state();
        }

        return;
    }

    if (!plan.sells.empty() && clock_() - day_.sell_leg_at < parameters_.buy_leg_delay_sec)
    {
        return; // 매도 체결이 원장·현금에 반영될 시간
    }

    if (entry_halted())
    {
        if (!halt_logged_)
        {
            halt_logged_ = true;
            LOG_WARN("[" + id_ + "] 신규 매수 차단(entry_halt) 중 — 매수 레그 대기");
        }

        return;
    }

    // 국면 매수 비율(OrderGate::entry_scale, 0~1)을 매수 수량에 곱한다 — DevScale과 같은 규칙. 매도는 그대로.
    const double scale = std::clamp(entry_scale(), 0.0, 1.0);

    if (scale < 1.0)
    {
        for (auto& order : plan.buys)
        {
            order.quantity = static_cast<int>(std::floor(order.quantity * scale));
        }

        std::erase_if(plan.buys, [](const basket::PlannedOrder& order)
        {
            return order.quantity <= 0;
        });
        LOG_INFO(std::format("[{}] 국면 매수 비율 {:.1f} 적용 — 매수 {}건", id_, scale, plan.buys.size()));
    }

    if (emit_leg(plan.buys, out, budget))
    {
        day_.buy_leg_done = true;
        save_state();
        LOG_INFO(std::format("[{}] 오늘 집행 끝 — 보낸 주문 {}건", id_, parameters_.dry_run ? dry_sent_.size() : day_.sent.size()));
    }
}

void TargetBasketStrategy::on_trade_batch(const TradeData&, std::vector<OrderSignal>& out)
{
    // 남의 틱을 심장박동으로만 쓴다 — 시각 비교 하나로 돌려보낸다.
    const auto now = std::chrono::steady_clock::now();

    if (last_pass_.time_since_epoch().count() != 0 && now - last_pass_ < std::chrono::milliseconds(parameters_.pass_interval_ms))
    {
        return;
    }

    last_pass_ = now;
    run_pass(out);
}
