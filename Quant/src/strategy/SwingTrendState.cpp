// strategy/SwingTrendState.cpp — 스윙 추세 슬리브의 계획 파일 읽기, 상태 파일 복원·저장, 기동 잔고 대조, 장부 체결 반영(track_fills).
//  스레드: 샤드 스레드(load_state·reconcile_with_balance는 로더가 메인 스레드에서 부른다). 관련 결정: D-160(상태 파일을 먼저 적고 낸다).
#include "strategy/SwingTrendStrategy.h"
#include "utils/AtomicFile.h"
#include "utils/Logger.h"
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace
{
constexpr int kMissingChecksBeforeRelease = 60; // 장부 0주가 이만큼(on_clock 약 1초 간격) 이어져야 보유를 놓는다

std::optional<json> read_json_file(const std::filesystem::path& path)
{
    std::ifstream stream(path);

    if (!stream)
    {
        return std::nullopt;
    }

    json document = json::parse(stream, nullptr, /*allow_exceptions=*/false);

    if (document.is_discarded())
    {
        return std::nullopt;
    }

    return document;
}

std::optional<SwingTrendStrategy::Status> status_from(const std::string& text)
{
    if (text == "PENDING")
    {
        return SwingTrendStrategy::Status::Pending;
    }

    if (text == "OPEN")
    {
        return SwingTrendStrategy::Status::Open;
    }

    if (text == "EXITING")
    {
        return SwingTrendStrategy::Status::Exiting;
    }

    return std::nullopt;
}

json position_json(const SwingTrendStrategy::Position& position)
{
    return json{{"ticker", position.ticker},
                {"entry_date", position.entry_date},
                {"entry_price", position.entry_price},
                {"initial_stop", position.initial_stop},
                {"stop", position.stop},
                {"ordered", position.ordered},
                {"quantity", position.quantity},
                {"status", SwingTrendStrategy::status_text(position.status)},
                {"order_number", position.order_number},
                {"cancel_sent", position.cancel_sent},
                {"exit_attempts", position.exit_attempts},
                {"last_exit_at", static_cast<int64_t>(position.last_exit_at)},
                {"exit_reason", position.exit_reason}};
}

// 형이 틀린 값은 json이 예외를 던진다 — 부른 쪽이 파일 전체를 깨진 것으로 본다.
std::optional<SwingTrendStrategy::Position> position_from(const json& node)
{
    if (!node.is_object())
    {
        return std::nullopt;
    }

    SwingTrendStrategy::Position position;
    position.ticker        = node.value("ticker", std::string());
    position.entry_date    = node.value("entry_date", std::string());
    position.entry_price   = node.value("entry_price", int64_t{0});
    position.initial_stop  = node.value("initial_stop", int64_t{0});
    position.stop          = node.value("stop", position.initial_stop);
    position.ordered       = node.value("ordered", 0);
    position.quantity      = node.value("quantity", 0);
    position.order_number  = node.value("order_number", uint64_t{0});
    position.cancel_sent   = node.value("cancel_sent", false);
    position.exit_attempts = node.value("exit_attempts", 0);
    position.last_exit_at  = static_cast<std::time_t>(node.value("last_exit_at", int64_t{0}));
    position.exit_reason   = node.value("exit_reason", std::string());
    const auto status      = status_from(node.value("status", std::string()));

    if (position.ticker.empty() || position.initial_stop <= 0 || !status)
    {
        return std::nullopt;
    }

    position.status = *status;
    return position;
}
} // namespace

bool SwingTrendStrategy::load_plan()
{
    const std::filesystem::path path(parameters_.plan_file);
    std::error_code             error;
    const auto                  modified = std::filesystem::last_write_time(path, error);

    if (error)
    {
        if (!plan_missing_logged_)
        {
            plan_missing_logged_ = true;
            LOG_WARN("[" + id_ + "] 계획 파일 없음 " + parameters_.plan_file + " — 새 매수 없음, 보유 감시만");
        }

        return false;
    }

    plan_missing_logged_ = false;

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
    auto        parsed = swing_trend::parse_plan(*document, reason);

    if (!parsed)
    {
        LOG_WARN("[" + id_ + "] 계획 파일 검증 실패: " + reason + " — 직전 것을 유지");
        return false;
    }

    plan_              = std::move(*parsed);
    plan_stale_logged_ = false;
    LOG_INFO(std::format("[{}] 계획 읽음 as_of={} generated_at={} 후보 {}", id_, plan_->as_of, plan_->generated_at, plan_->entries.size()));
    return true;
}

void SwingTrendStrategy::load_state()
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
                    broken("종목·손절선·상태가 틀린 보유 행 " + node.dump());
                    return;
                }

                loaded.push_back(std::move(*position));
            }
        }

        day_ = document->value("day", std::string());
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
    }
    catch (const json::exception& exception)
    {
        broken(std::string("값의 형이 틀림: ") + exception.what());
        return;
    }

    positions_ = std::move(loaded);

    for (const auto& position : positions_)
    {
        LOG_INFO(std::format("[{}] 보유 복원 {} {}주 매수가 {} 손절선 {} 상태 {} 매수일 {}", id_, position.ticker, position.quantity,
                             position.entry_price, position.stop, status_text(position.status), position.entry_date));
    }
}

int SwingTrendStrategy::reconcile_with_balance(const std::map<std::string, int>& held_quantity)
{
    const std::string today   = kst_date();
    int               dropped = 0;

    for (auto iterator = positions_.begin(); iterator != positions_.end();)
    {
        const auto held     = held_quantity.find(iterator->ticker);
        const int  quantity = held != held_quantity.end() ? held->second : 0;

        if (quantity > 0)
        {
            if (iterator->status == Status::Pending)
            {
                iterator->status = Status::Open;
            }

            iterator->quantity = quantity;
            ++iterator;
            continue;
        }

        // 오늘 낸 매수는 아직 안 잡혔을 수 있다 — 남겨 두고 15:19 취소·정리에 맡긴다.
        if (iterator->status == Status::Pending && iterator->entry_date == today)
        {
            ++iterator;
            continue;
        }

        LOG_WARN(std::format("[{}] 잔고에 없는 보유 {} (상태 {} 매수일 {}) — 이 전략 소유에서 뺀다", id_, iterator->ticker,
                             status_text(iterator->status), iterator->entry_date));
        iterator = positions_.erase(iterator);
        ++dropped;
    }

    if (dropped > 0)
    {
        dirty_ = true;
    }

    return dropped;
}

bool SwingTrendStrategy::save_state()
{
    if (state_broken_)
    {
        return false; // 못 읽은 파일을 덮어쓰지 않는다
    }

    json document;
    document["schema"]    = 1;
    document["strategy"]  = id_;
    document["day"]       = day_;
    document["attempted"] = json(std::vector<std::string>(attempted_today_.begin(), attempted_today_.end()));
    document["positions"] = json::array();

    for (const auto& position : positions_)
    {
        document["positions"].push_back(position_json(position));
    }

    const std::filesystem::path path(parameters_.state_file);
    std::error_code             error;

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), error);
    }

    if (!file_io::write_atomically(parameters_.state_file, document.dump(2)))
    {
        LOG_ERROR("[" + id_ + "] 상태 파일 쓰기 실패 " + parameters_.state_file);
        dirty_ = true;
        return false;
    }

    dirty_ = false;
    return true;
}

void SwingTrendStrategy::track_fills()
{
    bool      changed = false;
    const int hhmm    = kst_hhmm(clock_());

    for (auto iterator = positions_.begin(); iterator != positions_.end();)
    {
        Position&  position         = *iterator;
        const int  account_position = confirmed_position(parameters_.account, position.ticker);
        const bool entry_day        = position.entry_date == day_;

        // 매수 날에는 장부 보유를 따라간다(부분 체결이 이어질 수 있다).
        if ((position.status == Status::Pending || (position.status == Status::Open && entry_day)) && account_position > position.quantity)
        {
            position.quantity       = account_position;
            position.status         = Status::Open;
            position.missing_checks = 0;
            changed                 = true;
            LOG_INFO(std::format("[{}] 매수 체결 {} {}주 매수가 {} 손절선 {}", id_, position.ticker, position.quantity, position.entry_price,
                                 position.stop));
        }

        // 매수가 끝내 안 잡혔다 — 취소 시각이 지났고 미체결 선점까지 0이면 지운다.
        if (position.status == Status::Pending && account_position == 0 && (!entry_day || hhmm >= parameters_.buy_cancel_hhmm))
        {
            const auto ledger = ledger_sellable(parameters_.account, position.ticker);

            if (!ledger || ledger->reserved <= 0)
            {
                LOG_INFO(std::format("[{}] 매수 미체결 정리 {} — 장부 보유 0", id_, position.ticker));
                iterator = positions_.erase(iterator);
                changed  = true;
                continue;
            }
        }

        // 보유가 0이 됐다 — 매도 체결이면 청산 끝, 아니면 손으로 판 것이다. 일시적인 0(장부 재시드)으로 놓지 않게 이어서 볼 때만 놓는다.
        if (position.status != Status::Pending && account_position == 0)
        {
            if (++position.missing_checks >= kMissingChecksBeforeRelease || position.status == Status::Exiting)
            {
                if (position.status == Status::Exiting)
                {
                    LOG_INFO(std::format("[{}] 청산 완료 {} 사유 {} 매수일 {}", id_, position.ticker, position.exit_reason, position.entry_date));
                }
                else
                {
                    LOG_WARN(std::format("[{}] 장부 보유 0이 이어짐 {} — 이 전략 소유에서 뺀다(손으로 판 것으로 본다)", id_, position.ticker));
                }

                iterator = positions_.erase(iterator);
                changed  = true;
                continue;
            }
        }
        else
        {
            position.missing_checks = 0;
        }

        ++iterator;
    }

    if (changed)
    {
        dirty_ = true;
    }
}
