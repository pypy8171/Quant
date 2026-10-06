#include "strategy/SurgeHoldStrategy.h"
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
constexpr size_t kClosedKeep = 100; // 상태 파일에 남기는 청산 기록 수

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

        if (!stream.flush())
        {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::optional<SurgeHoldStrategy::Status> status_from(const std::string& text)
{
    if (text == "PENDING")
    {
        return SurgeHoldStrategy::Status::Pending;
    }

    if (text == "OPEN")
    {
        return SurgeHoldStrategy::Status::Open;
    }

    if (text == "EXITING")
    {
        return SurgeHoldStrategy::Status::Exiting;
    }

    return std::nullopt;
}

json position_json(const SurgeHoldStrategy::Position& position)
{
    return json{{"ticker", position.ticker},
                {"d0", position.d0},
                {"entry_date", position.entry_date},
                {"quantity", position.quantity},
                {"ordered", position.ordered},
                {"entry_price", position.entry_price},
                {"stop_basis_low", position.stop_basis_low},
                {"stop_pct", position.stop_percent},
                {"take_pct", position.take_percent},
                {"stop_price", position.stop},
                {"take_price", position.take},
                {"status", SurgeHoldStrategy::status_text(position.status)},
                {"exit_reason", position.exit_reason},
                {"exit_due", position.exit_due},
                {"exit_attempts", position.exit_attempts},
                {"last_exit_at", static_cast<int64_t>(position.last_exit_at)},
                {"order_number", position.order_number},
                {"cancel_sent", position.cancel_sent}};
}

// 형이 틀린 값은 json이 예외를 던진다 — 부른 쪽이 파일 전체를 깨진 것으로 본다.
std::optional<SurgeHoldStrategy::Position> position_from(const json& node)
{
    if (!node.is_object())
    {
        return std::nullopt;
    }

    SurgeHoldStrategy::Position position;
    position.ticker         = node.value("ticker", std::string());
    position.d0             = node.value("d0", std::string());
    position.entry_date     = node.value("entry_date", std::string());
    position.quantity       = node.value("quantity", 0);
    position.ordered        = node.value("ordered", 0);
    position.entry_price    = node.value("entry_price", 0.0);
    position.stop_basis_low = node.value("stop_basis_low", 0.0);
    position.stop_percent   = node.value("stop_pct", 0.0);
    position.take_percent   = node.value("take_pct", 0.0);
    position.stop           = node.value("stop_price", 0.0);
    position.take           = node.value("take_price", 0.0);
    position.exit_reason    = node.value("exit_reason", std::string());
    position.exit_due       = node.value("exit_due", std::string());
    position.exit_attempts  = node.value("exit_attempts", 0);
    position.last_exit_at   = static_cast<std::time_t>(node.value("last_exit_at", int64_t{0}));
    position.order_number   = node.value("order_number", uint64_t{0});
    position.cancel_sent    = node.value("cancel_sent", false);
    const auto status       = status_from(node.value("status", std::string()));

    if (position.ticker.empty() || !status)
    {
        return std::nullopt;
    }

    position.status = *status;
    return position;
}
} // namespace

SurgeHoldStrategy::SurgeHoldStrategy(Params parameters, OwnedSink owned_sink)
    : parameters_(std::move(parameters)), owned_sink_(std::move(owned_sink)), clock_([]
    {
        return std::time(nullptr);
    }),
      id_("SURGE_" + parameters_.label)
{
}

const char* SurgeHoldStrategy::status_text(Status status)
{
    switch (status)
    {
    case Status::Pending:
        return "PENDING";
    case Status::Open:
        return "OPEN";
    case Status::Exiting:
        return "EXITING";
    }

    return "?";
}

std::string SurgeHoldStrategy::describe() const
{
    return std::format("SurgeHold({} 종목당 {:.0f}원 상한 {}종목 하루 {}건 {}) 계획 {}", parameters_.label, parameters_.amount_krw,
                       parameters_.max_positions, parameters_.max_daily_buys, parameters_.dry_run ? "dry-run" : "실주문",
                       parameters_.plan_file);
}

std::optional<OrderSignal> SurgeHoldStrategy::on_data(const MarketData&)
{
    return std::nullopt;
}

int SurgeHoldStrategy::kst_hhmm() const
{
    const auto time_of_day = kst::time_of_day(clock_());
    return static_cast<int>(time_of_day.hours().count()) * 100 + static_cast<int>(time_of_day.minutes().count());
}

std::string SurgeHoldStrategy::kst_date() const
{
    return kst::date_yyyymmdd(clock_());
}

// 손절·익절 주문을 내는 창 — 09:00–15:19. 장 마감 뒤 NXT 체결이나 장 시작 전에 닿은 판정은 여기서 걸러 다음 창으로 미룬다.
bool SurgeHoldStrategy::in_exit_window() const
{
    const int hhmm = kst_hhmm();
    return hhmm >= parameters_.exit_start_hhmm && hhmm < parameters_.close_start_hhmm;
}

bool SurgeHoldStrategy::load_plan()
{
    const std::filesystem::path path(parameters_.plan_file);
    std::error_code             error;
    const auto                  modified = std::filesystem::last_write_time(path, error);

    if (error)
    {
        if (!plan_)
        {
            LOG_WARN("[" + id_ + "] 계획 파일 없음 " + parameters_.plan_file + " — 새 매수 없음, 보유 감시만");
        }

        return false;
    }

    if (plan_ && modified == plan_modified_)
    {
        return false;
    }

    plan_modified_      = modified;
    const auto document = read_json_file(path);

    if (!document)
    {
        LOG_WARN("[" + id_ + "] 계획 파일 파싱 실패 " + parameters_.plan_file + " — 직전 것을 유지");
        return false;
    }

    std::string reason;
    auto        parsed = surge::parse_plan(*document, reason);

    if (!parsed)
    {
        LOG_WARN("[" + id_ + "] 계획 파일 검증 실패: " + reason + " — 직전 것을 유지");
        return false;
    }

    plan_ = std::move(*parsed);
    int buys  = 0;
    int exits = 0;

    for (const auto& row : plan_->rows)
    {
        buys += row.action == surge::Action::BuyOpen ? 1 : 0;
        exits += row.action == surge::Action::ExitClose ? 1 : 0;
    }

    plan_stale_logged_ = false;
    plan_done_logged_  = false;
    LOG_INFO(std::format("[{}] 계획 읽음 as_of={} generated_at={} rule={} 매수 {} 만기 청산 {} 행 {}", id_, plan_->as_of,
                         plan_->generated_at, plan_->rule, buys, exits, plan_->rows.size()));
    return true;
}

void SurgeHoldStrategy::load_state()
{
    const std::filesystem::path path(parameters_.state_file);
    std::error_code             error;

    if (!std::filesystem::exists(path, error) && !error)
    {
        LOG_INFO("[" + id_ + "] 상태 파일 없음 " + parameters_.state_file + " — 보유 없이 시작");
        return;
    }

    // 파일이 있는데 못 읽으면 보유를 모른다 — 빈 보유로 시작하면 같은 종목을 또 사고 손절 감시도 빠진다.
    //  매수를 막고, 사람이 고칠 수 있게 그 파일을 덮어쓰지 않는다.
    const auto broken = [this](const std::string& why)
    {
        state_broken_ = true;
        positions_.clear();
        LOG_ERROR("[" + id_ + "] 상태 파일 읽기 실패 " + parameters_.state_file + " — " + why +
                  ". 보유를 모르므로 매수를 막고 파일을 덮어쓰지 않는다");
    };

    const auto document = read_json_file(path);

    if (!document || !document->is_object())
    {
        broken("JSON이 아님");
        return;
    }

    std::vector<Position> loaded;

    try
    {
        const auto positions = document->find("positions");

        if (positions != document->end())
        {
            if (!positions->is_array())
            {
                broken("positions가 배열이 아님");
                return;
            }

            for (const auto& node : *positions)
            {
                auto position = position_from(node);

                if (!position)
                {
                    broken("종목·상태가 틀린 보유 행 " + node.dump());
                    return;
                }

                loaded.push_back(std::move(*position));
            }
        }

        day_                 = document->value("day", std::string());
        bought_today_        = document->value("bought_today", 0);
        last_executed_as_of_ = document->value("last_executed_as_of", 0);
        executed_on_         = document->value("executed_on", std::string());
        attempted_today_.clear();
        const auto attempted = document->find("attempted");

        if (attempted != document->end() && attempted->is_array())
        {
            for (const auto& ticker : *attempted)
            {
                if (ticker.is_string())
                {
                    attempted_today_.insert(ticker.get<std::string>());
                }
            }
        }

        const auto closed = document->find("closed");
        closed_           = closed != document->end() && closed->is_array() ? *closed : json::array();
    }
    catch (const json::exception& exception)
    {
        broken(std::string("값의 형이 틀림: ") + exception.what());
        return;
    }

    positions_ = std::move(loaded);

    for (const auto& position : positions_)
    {
        LOG_INFO(std::format("[{}] 보유 복원 {} {}주 매수가 {:.0f} 손절 {:.0f} 익절 {:.0f} 상태 {} 매수일 {}", id_, position.ticker,
                             position.quantity, position.entry_price, position.stop, position.take, status_text(position.status),
                             position.entry_date));
    }
}

bool SurgeHoldStrategy::save_state()
{
    if (state_broken_)
    {
        return false; // 못 읽은 파일을 덮어쓰지 않는다
    }

    json document;
    document["schema"]              = 1;
    document["strategy"]            = id_;
    document["day"]                 = day_;
    document["bought_today"]        = bought_today_;
    document["last_executed_as_of"] = last_executed_as_of_;
    document["executed_on"]         = executed_on_;
    document["attempted"]           = json(std::vector<std::string>(attempted_today_.begin(), attempted_today_.end()));
    document["positions"]           = json::array();

    for (const auto& position : positions_)
    {
        document["positions"].push_back(position_json(position));
    }

    document["closed"] = closed_;

    if (!write_json_atomic(parameters_.state_file, document))
    {
        LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패 " + parameters_.state_file);
        dirty_ = true;
        return false;
    }

    dirty_ = false;
    return true;
}

std::vector<std::string> SurgeHoldStrategy::held_tickers() const
{
    std::vector<std::string> tickers;

    for (const auto& position : positions_)
    {
        tickers.push_back(position.ticker);
    }

    return tickers;
}

std::vector<std::string> SurgeHoldStrategy::buy_candidates() const
{
    std::vector<std::string> tickers;

    if (!plan_)
    {
        return tickers;
    }

    for (const auto& row : plan_->rows)
    {
        const bool excluded = std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) !=
                              parameters_.excluded_tickers.end();

        if (row.action == surge::Action::BuyOpen && !excluded)
        {
            tickers.push_back(row.ticker);
        }
    }

    return tickers;
}

std::vector<WatchSpec> SurgeHoldStrategy::get_watch_specifications() const
{
    // 손절·익절은 체결 가격으로 보므로 보유와 오늘 매수 후보를 체결만 구독한다(호가 칸을 아낀다).
    std::set<std::string> tickers;

    for (const auto& ticker : held_tickers())
    {
        tickers.insert(ticker);
    }

    for (const auto& ticker : buy_candidates())
    {
        tickers.insert(ticker);
    }

    std::vector<WatchSpec> specifications;

    for (const auto& ticker : tickers)
    {
        WatchSpec specification;
        specification.ticker     = ticker;
        specification.market     = Market::KR;
        specification.trade_only = true;
        specifications.push_back(std::move(specification));
    }

    return specifications;
}

void SurgeHoldStrategy::rebuild_index()
{
    index_of_symbol_.clear();

    for (size_t index = 0; index < positions_.size(); ++index)
    {
        const symbol::SymbolId symbol = symbol_of(positions_[index].ticker);

        if (symbol != symbol::kNone)
        {
            index_of_symbol_[symbol] = index;
        }
    }
}

void SurgeHoldStrategy::publish_owned()
{
    // dry_run은 주문을 내지 않으니 다른 슬리브의 슬롯 계산을 건드리지 않는다.
    if (parameters_.dry_run)
    {
        return;
    }

    owned_scratch_.clear();

    for (const auto& position : positions_)
    {
        owned_scratch_.push_back(position.ticker);
    }

    // 오늘 매수할 후보도 넣는다 — 체결되는 순간부터 슬롯 계산 밖이어야 장중 전략의 강제청산 대상이 되지 않는다.
    //  다른 슬리브가 이미 든 후보는 넣지 않는다(겹침이라 사지 않고, 넣으면 그 슬리브 보유가 슬롯 밖으로 빠진다).
    if (plan_ && surge::plan_fresh(surge::date_number(plan_->as_of), surge::date_number(kst_date())))
    {
        for (const auto& row : plan_->rows)
        {
            const bool candidate = row.action == surge::Action::BuyOpen &&
                                   std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) ==
                                       parameters_.excluded_tickers.end();

            if (!candidate || attempted_today_.count(row.ticker) != 0)
            {
                continue;
            }

            const bool ours = std::any_of(positions_.begin(), positions_.end(), [&row](const Position& position)
            {
                return position.ticker == row.ticker;
            });

            if (!ours && confirmed_position(parameters_.account, row.ticker) > 0)
            {
                continue;
            }

            owned_scratch_.push_back(row.ticker);
        }
    }

    std::sort(owned_scratch_.begin(), owned_scratch_.end());
    owned_scratch_.erase(std::unique(owned_scratch_.begin(), owned_scratch_.end()), owned_scratch_.end());

    if (owned_scratch_ == owned_published_)
    {
        return;
    }

    // 못 보냈으면 지난 목록을 그대로 두어 다음 on_clock에 다시 보낸다.
    if (owned_sink_ && !owned_sink_(owned_scratch_))
    {
        return;
    }

    owned_published_.swap(owned_scratch_);
}

void SurgeHoldStrategy::on_start()
{
    load_plan();
    roll_day();
    rebuild_index();
    publish_owned();
    LOG_INFO("[" + id_ + "] 시작 " + describe() + " 보유 " + std::to_string(positions_.size()) + "종목");
}

void SurgeHoldStrategy::on_stop()
{
    save_state();
}

void SurgeHoldStrategy::roll_day()
{
    const std::string today = kst_date();

    if (today == day_)
    {
        return;
    }

    day_                   = today;
    bought_today_          = 0;
    attempted_today_.clear();
    dry_sent_.clear();
    buys_stopped_today_    = false;
    plan_stale_logged_     = false;
    plan_done_logged_      = false;
    regime_unknown_logged_ = false;

    // 어제 다 못 판 매도 — 주문은 밤사이 사라지므로 시도 횟수를 0으로 되돌리고 오늘 창에서 다시 판다.
    for (auto& position : positions_)
    {
        if (position.status != Status::Exiting)
        {
            continue;
        }

        const auto sellable = ledger_sellable(parameters_.account, position.ticker);

        if (sellable && sellable->sellable > 0)
        {
            position.status        = Status::Open;
            position.exit_attempts = 0;
            position.exit_due      = position.exit_reason;
            LOG_INFO(std::format("[{}] 어제 남은 매도 {} {}주 — 오늘 창에서 다시 판다({})", id_, position.ticker, sellable->sellable,
                                 position.exit_reason));
        }
    }

    save_state();
}

bool SurgeHoldStrategy::wants_clock() const
{
    return true;
}

OrderSignal SurgeHoldStrategy::make_signal(const std::string& ticker, OrderSide side, int quantity, double reference_price,
                                           const std::string& reason) const
{
    OrderSignal signal;
    signal.ticker          = ticker;
    signal.symbol_id       = symbol_of(ticker);
    signal.side            = side;
    signal.type            = OrderType::MARKET;
    signal.quantity        = quantity;
    signal.price           = 0.0;
    signal.reference_price = reference_price; // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
    signal.strategy_id     = id_;
    signal.market          = Market::KR;
    signal.account_id      = parameters_.account;
    signal.reason          = reason;
    signal.timestamp       = std::chrono::system_clock::now();
    // 계획 주문은 몇 초 늦어도 판단이 낡지 않는다 — 큐 나이 제한에서 뺀다(D-155와 같은 이유).
    signal.exempt_from_age_limit = true;
    return signal;
}

void SurgeHoldStrategy::on_clock(std::vector<OrderSignal>& out)
{
    roll_day();
    const auto steady_now = std::chrono::steady_clock::now();

    if (steady_now - last_reload_ >= std::chrono::seconds(parameters_.reload_sec))
    {
        last_reload_ = steady_now;
        load_plan();
    }

    track_fills();
    run_buys(out);
    run_unfilled_cancels(out);

    if (in_exit_window())
    {
        run_due_exits(out);
        run_exit_retries(out);
    }

    run_close_exits(out);
    publish_owned();

    if (dirty_)
    {
        save_state();
    }
}

std::optional<bool> SurgeHoldStrategy::regime_allows_buy() const
{
    // 엔진의 국면 선택은 09:00 첫 장중 사이클에 적용된다 — 08:50의 is_active()는 어제 값이라 파일을 직접 본다.
    if (regime_check_)
    {
        return regime_check_();
    }

    return is_active();
}

bool SurgeHoldStrategy::bought_same_signal(const std::string& ticker, const std::string& d0) const
{
    if (d0.empty())
    {
        return false;
    }

    for (const auto& position : positions_)
    {
        if (position.ticker == ticker && position.d0 == d0)
        {
            return true;
        }
    }

    for (const auto& record : closed_)
    {
        const auto record_ticker = record.find("ticker");
        const auto record_d0     = record.find("d0");

        if (record_ticker != record.end() && record_d0 != record.end() && record_ticker->is_string() && record_d0->is_string() &&
            record_ticker->get_ref<const std::string&>() == ticker && record_d0->get_ref<const std::string&>() == d0)
        {
            return true;
        }
    }

    return false;
}

void SurgeHoldStrategy::run_buys(std::vector<OrderSignal>& out)
{
    const int hhmm = kst_hhmm();

    if (!plan_ || hhmm < parameters_.buy_start_hhmm || hhmm > parameters_.buy_end_hhmm || buys_stopped_today_)
    {
        return;
    }

    if (state_broken_)
    {
        if (!broken_logged_)
        {
            broken_logged_ = true;
            LOG_ERROR("[" + id_ + "] 상태 파일을 못 읽어 매수 없음 — " + parameters_.state_file + " 을 고친 뒤 재기동");
        }

        return;
    }

    const int as_of = surge::date_number(plan_->as_of);

    if (!surge::plan_fresh(as_of, surge::date_number(day_)))
    {
        if (!plan_stale_logged_)
        {
            plan_stale_logged_ = true;
            LOG_WARN("[" + id_ + "] 계획 as_of=" + plan_->as_of + " 가 오늘 쓸 것이 아님 — 매수 없음");
        }

        return;
    }

    // 계획 하나는 한 번만 집행한다 — 17:00 계획 생성이 실패한 날 어제 계획으로 또 사지 않는다.
    if (as_of < last_executed_as_of_ || (as_of == last_executed_as_of_ && executed_on_ != day_))
    {
        if (!plan_done_logged_)
        {
            plan_done_logged_ = true;
            LOG_WARN(std::format("[{}] 계획 as_of={} 는 {} 에 이미 집행함 — 새 계획이 없어 매수 없음", id_, plan_->as_of, executed_on_));
        }

        return;
    }

    const bool pending_rows = std::any_of(plan_->rows.begin(), plan_->rows.end(), [this](const surge::PlanRow& row)
    {
        return row.action == surge::Action::BuyOpen && attempted_today_.count(row.ticker) == 0;
    });

    if (!pending_rows)
    {
        return;
    }

    const std::optional<bool> allowed = regime_allows_buy();

    if (!allowed)
    {
        if (!regime_unknown_logged_)
        {
            regime_unknown_logged_ = true;
            LOG_WARN("[" + id_ + "] 국면 파일을 못 읽음(없음·오래됨·판정 보류) — 매수 보류, 매수 창 안에서 다시 본다");
        }

        return;
    }

    bool changed = false;

    if (as_of != last_executed_as_of_)
    {
        last_executed_as_of_ = as_of;
        executed_on_         = day_;
        changed              = true;
    }

    // 행은 거래대금 비중 큰 순으로 정렬돼 있다(parse_plan) — 상한에 걸리면 앞의 것을 산다.
    for (const auto& row : plan_->rows)
    {
        if (row.action != surge::Action::BuyOpen || attempted_today_.count(row.ticker) != 0)
        {
            continue;
        }

        int open_positions = 0;

        for (const auto& position : positions_)
        {
            open_positions += position.status != Status::Exiting ? 1 : 0;
        }

        const auto        ledger = ledger_sellable(parameters_.account, row.ticker);
        surge::EntryCheck check;
        check.active           = *allowed;
        check.entry_halted     = entry_halted();
        check.excluded         = std::find(parameters_.excluded_tickers.begin(), parameters_.excluded_tickers.end(), row.ticker) !=
                         parameters_.excluded_tickers.end();
        check.same_signal      = bought_same_signal(row.ticker, row.d0);
        check.account_position = confirmed_position(parameters_.account, row.ticker);
        check.account_reserved = ledger ? ledger->reserved : 0;
        check.open_positions   = open_positions;
        check.max_positions    = parameters_.max_positions;
        check.bought_today     = bought_today_;
        check.max_daily_buys   = parameters_.max_daily_buys;
        check.quantity         = surge::buy_quantity(parameters_.amount_krw, row.reference_close);
        const surge::Skip skip = surge::entry_skip(check);
        attempted_today_.insert(row.ticker);
        changed = true;

        if (skip != surge::Skip::None)
        {
            LOG_INFO(std::format("[{}] 매수 건너뜀 {} — {} (신호일 {} 급등일 {})", id_, row.ticker, surge::skip_text(skip), row.signal_date,
                                 row.d0));
            continue;
        }

        const std::string reason = std::format("SURGE 시가 매수 신호일 {} 기준 저가 {:.0f} 손절 {:.0f}% 익절 {:.0f}%", row.signal_date,
                                               row.stop_basis_low, row.stop_percent, row.take_percent);

        if (parameters_.dry_run)
        {
            if (dry_sent_.insert(row.ticker).second)
            {
                LOG_INFO(std::format("[{}] [dry-run] 동시호가 매수 {} {}주 기준가 {:.0f} — {}", id_, row.ticker, check.quantity,
                                     row.reference_close, reason));
            }

            continue;
        }

        // 상태 파일에 먼저 적고 낸다 — 내고 죽으면 재기동이 같은 매수를 다시 내지 않는다.
        Position position;
        position.ticker         = row.ticker;
        position.d0             = row.d0;
        position.entry_date     = day_;
        position.ordered        = check.quantity;
        position.stop_basis_low = row.stop_basis_low;
        position.stop_percent   = row.stop_percent;
        position.take_percent   = row.take_percent;
        position.stop           = surge::stop_price(row.stop_basis_low, row.stop_percent);
        position.take           = surge::take_price(row.reference_close, row.take_percent);
        position.status         = Status::Pending;
        position.order_number   = next_client_order_number();
        positions_.push_back(std::move(position));
        ++bought_today_;

        if (!save_state())
        {
            // 적지 못한 매수는 내지 않는다 — 재기동이 이 주문을 모르게 된다. 오늘은 더 사지 않는다.
            positions_.pop_back();
            --bought_today_;
            buys_stopped_today_ = true;
            LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패로 " + row.ticker + " 매수를 내지 않고 오늘 매수를 멈춘다");
            rebuild_index();
            return;
        }

        rebuild_index();
        OrderSignal signal         = make_signal(row.ticker, OrderSide::BUY, check.quantity, row.reference_close, reason);
        signal.opening_auction     = true;
        signal.client_order_number = positions_.back().order_number;
        out.push_back(std::move(signal));
        LOG_INFO(std::format("[{}] 동시호가 매수 {} {}주 기준가 {:.0f} — {}", id_, row.ticker, check.quantity, row.reference_close, reason));
    }

    if (changed)
    {
        save_state();
    }
}

// 09:30까지 안 잡힌 매수 잔량은 취소 주문을 낸다. 취소 확인 신호는 전략에 오지 않으므로, 장부의 미체결 선점이
//  0이 되는 것을 보고 track_fills가 PENDING을 지운다.
void SurgeHoldStrategy::run_unfilled_cancels(std::vector<OrderSignal>& out)
{
    if (parameters_.dry_run || kst_hhmm() < parameters_.fill_wait_hhmm)
    {
        return;
    }

    for (auto& position : positions_)
    {
        if (position.entry_date != day_ || position.order_number == 0 || position.cancel_sent || position.status == Status::Exiting)
        {
            continue;
        }

        const auto ledger = ledger_sellable(parameters_.account, position.ticker);

        if (!ledger || ledger->reserved <= 0)
        {
            continue;
        }

        OrderSignal signal                  = make_signal(position.ticker, OrderSide::BUY, 0, 0.0, "SURGE 매수 미체결 취소");
        signal.action                       = OrderAction::CANCEL;
        signal.original_client_order_number = position.order_number;
        out.push_back(std::move(signal));
        position.cancel_sent = true;
        dirty_               = true;
        LOG_WARN(std::format("[{}] 매수 미체결 {} — {} 까지 선점 {}주가 남아 취소 주문을 낸다", id_, position.ticker,
                             parameters_.fill_wait_hhmm, ledger->reserved));
    }
}

void SurgeHoldStrategy::track_fills()
{
    bool      changed = false;
    const int hhmm    = kst_hhmm();

    for (auto iterator = positions_.begin(); iterator != positions_.end();)
    {
        Position&  position         = *iterator;
        const int  account_position = confirmed_position(parameters_.account, position.ticker);
        const bool entry_day        = position.entry_date == day_;

        // 매수 날에는 장부 보유·평단을 따라간다(부분 체결이 이어질 수 있다). 실제 매수가로 익절을 다시 잡는다.
        if ((position.status == Status::Pending || (position.status == Status::Open && entry_day)) && account_position > position.quantity)
        {
            const auto   sellable = ledger_sellable(parameters_.account, position.ticker);
            const double average  = sellable && sellable->average_price > 0.0 ? sellable->average_price : position.entry_price;

            if (average > 0.0)
            {
                position.quantity    = account_position;
                position.entry_price = average;
                position.stop        = surge::stop_price(position.stop_basis_low, position.stop_percent);
                position.take        = surge::take_price(average, position.take_percent);
                position.status      = Status::Open;
                changed              = true;
                LOG_INFO(std::format("[{}] 매수 체결 {} {}주 매수가 {:.0f} → 손절 {:.0f} 익절 {:.0f}", id_, position.ticker,
                                     position.quantity, average, position.stop, position.take));

                // 시가가 손절선 아래로 갭 하락해 체결됐다 — 백테스트는 그날 손절로 친다. 창이 열리면 바로 판다.
                if (position.stop > 0.0 && average <= position.stop && position.exit_due.empty())
                {
                    position.exit_due = std::format("손절 시가 체결가 {:.0f} ≤ {:.0f}", average, position.stop);
                    LOG_WARN(std::format("[{}] {} 매수가 {:.0f} 가 손절선 {:.0f} 아래 — 09:00 뒤 바로 판다", id_, position.ticker, average,
                                         position.stop));
                }
            }
        }

        // 매수가 끝내 안 잡혔다 — 미체결 선점까지 0이면 지운다(취소 확인 전에는 남긴다).
        if (position.status == Status::Pending && account_position == 0 && (!entry_day || hhmm >= parameters_.fill_wait_hhmm))
        {
            const auto ledger = ledger_sellable(parameters_.account, position.ticker);

            if (!ledger || ledger->reserved <= 0)
            {
                LOG_WARN(std::format("[{}] 매수 미체결 {} — {} 까지 장부 보유 0, 계획에서 지운다", id_, position.ticker,
                                     parameters_.fill_wait_hhmm));
                iterator = positions_.erase(iterator);
                changed  = true;
                continue;
            }
        }

        // 매도를 냈고 장부 보유가 0이 됐다 — 청산 끝.
        if (position.status == Status::Exiting && account_position == 0)
        {
            LOG_INFO(std::format("[{}] 청산 완료 {} 사유 {} 매수가 {:.0f} 매수일 {}", id_, position.ticker, position.exit_reason,
                                 position.entry_price, position.entry_date));
            json record           = position_json(position);
            record["closed_date"] = day_;
            closed_.push_back(std::move(record));

            while (closed_.size() > kClosedKeep)
            {
                closed_.erase(closed_.begin());
            }

            iterator = positions_.erase(iterator);
            changed  = true;
            continue;
        }

        ++iterator;
    }

    if (changed)
    {
        dirty_ = true;
        rebuild_index();
    }
}

// 매도를 낸다. 저장은 on_clock이 한다(체결 틱 경로에서 파일을 쓰지 않는다). 팔 수량이 없으면 false.
bool SurgeHoldStrategy::emit_sell(Position& position, const std::string& reason, std::vector<OrderSignal>& out)
{
    const auto sellable = ledger_sellable(parameters_.account, position.ticker);
    const int  quantity = std::min(position.quantity, sellable ? sellable->sellable : position.quantity);

    if (quantity <= 0)
    {
        return false;
    }

    position.status       = Status::Exiting;
    position.exit_reason  = reason;
    position.exit_due.clear();
    position.last_exit_at = clock_();
    ++position.exit_attempts;
    dirty_ = true;

    if (parameters_.dry_run)
    {
        LOG_INFO(std::format("[{}] [dry-run] 매도 {} {}주 — {}", id_, position.ticker, quantity, reason));
        return true;
    }

    out.push_back(make_signal(position.ticker, OrderSide::SELL, quantity, position.entry_price, reason));
    LOG_INFO(std::format("[{}] 매도 {} {}주 — {} (시도 {})", id_, position.ticker, quantity, reason, position.exit_attempts));
    return true;
}

// 창 밖에서 닿은 손절·익절, 갭 하락 체결, 어제 못 판 매도를 창이 열리면 낸다.
void SurgeHoldStrategy::run_due_exits(std::vector<OrderSignal>& out)
{
    for (auto& position : positions_)
    {
        if (position.status == Status::Open && !position.exit_due.empty())
        {
            const std::string reason = position.exit_due;
            emit_sell(position, reason, out);
        }
    }
}

void SurgeHoldStrategy::run_close_exits(std::vector<OrderSignal>& out)
{
    const int hhmm = kst_hhmm();

    if (hhmm < parameters_.close_start_hhmm || hhmm > parameters_.close_end_hhmm)
    {
        return;
    }

    const std::time_t now = clock_();

    // 처음이면 내고, 이미 냈는데 남았으면 간격·횟수 안에서 다시 낸다.
    const auto close_sell = [this, now, &out](Position& position, const std::string& reason)
    {
        if (position.entry_date == day_)
        {
            return;
        }

        if (position.status == Status::Open)
        {
            emit_sell(position, reason, out);
            return;
        }

        if (position.status == Status::Exiting && position.exit_attempts < parameters_.max_exit_attempts &&
            now - position.last_exit_at >= parameters_.exit_retry_sec)
        {
            emit_sell(position, position.exit_reason, out);
        }
    };

    const int  today      = surge::date_number(day_);
    const bool plan_today = plan_ && surge::plan_fresh(surge::date_number(plan_->as_of), today);

    if (plan_ && surge::date_number(plan_->as_of) < today)
    {
        for (const auto& row : plan_->rows)
        {
            if (row.action != surge::Action::ExitClose)
            {
                continue;
            }

            for (auto& position : positions_)
            {
                if (position.ticker == row.ticker)
                {
                    close_sell(position, std::format("만기 {}거래일 종가 동시호가", row.held_days));
                }
            }
        }
    }

    // 계획이 없는 날의 백스톱 — 17:00 계획 생성이 며칠 멈춰도 보유가 끝없이 남지 않게 평일 수로 판다.
    if (!plan_today)
    {
        for (auto& position : positions_)
        {
            const int weekdays = surge::weekdays_between(surge::date_number(position.entry_date), today);

            if (weekdays >= parameters_.backstop_weekdays)
            {
                close_sell(position, std::format("만기 백스톱 평일 {}일(계획 없음) 종가 동시호가", weekdays));
            }
        }
    }
}

void SurgeHoldStrategy::run_exit_retries(std::vector<OrderSignal>& out)
{
    const std::time_t now = clock_();

    for (auto& position : positions_)
    {
        if (position.status != Status::Exiting || position.exit_attempts >= parameters_.max_exit_attempts ||
            now - position.last_exit_at < parameters_.exit_retry_sec)
        {
            continue;
        }

        const auto sellable = ledger_sellable(parameters_.account, position.ticker);

        if (sellable && sellable->sellable > 0)
        {
            emit_sell(position, position.exit_reason, out);
        }
    }
}

void SurgeHoldStrategy::on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out)
{
    const auto found = index_of_symbol_.find(trade.symbol_id);

    if (found == index_of_symbol_.end() || found->second >= positions_.size())
    {
        return;
    }

    Position& position = positions_[found->second];

    if (position.status != Status::Open || position.entry_price <= 0.0)
    {
        return;
    }

    const surge::Exit exit = surge::exit_check(trade.price, position.stop, position.take);

    if (exit == surge::Exit::None)
    {
        return;
    }

    std::string reason = exit == surge::Exit::Stop ? std::format("손절 체결가 {:.0f} ≤ {:.0f}", trade.price, position.stop)
                                                   : std::format("익절 체결가 {:.0f} ≥ {:.0f}", trade.price, position.take);

    if (in_exit_window())
    {
        emit_sell(position, reason, out);
        return;
    }

    // 장 마감 뒤 NXT 체결이나 장 시작 전 — 주문은 내지 않고 다음 창으로 넘긴다.
    if (position.exit_due.empty())
    {
        LOG_INFO(std::format("[{}] {} {} — 매도 창 밖이라 다음 창에서 판다", id_, position.ticker, reason));
        position.exit_due = std::move(reason);
        dirty_            = true;
    }
}
