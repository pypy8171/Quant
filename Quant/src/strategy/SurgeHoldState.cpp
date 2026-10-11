// strategy/SurgeHoldState.cpp — 급등 보유 슬리브의 계획 파일 읽기, 상태 파일 복원·저장, 장부 체결 반영(track_fills).
//  관련 결정: D-157(상태 파일을 먼저 적고 낸다, 못 읽으면 매수 금지).
#include "strategy/SurgeHoldStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
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

void SurgeHoldStrategy::track_fills()
{
    bool      changed = false;
    const int hhmm    = kst_hhmm(clock_());

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
