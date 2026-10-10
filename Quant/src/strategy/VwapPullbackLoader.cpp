// strategy/VwapPullbackLoader.cpp — VWAP_PULLBACK(강한 종목 첫 VWAP 눌림) 로더.
//  책임: config 항목을 읽어 (1) 재기동 재인수 종목을 다른 로더보다 먼저 예약하고, (2) 09:30 선정 → 종목별 전략 등록을
//   재스캔 작업으로 엔진에 건다. 선정 결과는 logs/vwappb_selection_YYYYMMDD.json에 한 번 적고, 재기동하면 그 파일을 쓴다.
//  Phase 1(그림자)이라 주문 경로가 없다 — shadow=false를 줘도 그림자로 돈다.
//  스레드: reserve·load는 메인 스레드(load_strategies 안). 선정 함수와 factory는 데이터 스레드(재스캔)에서만 돈다.
//  관련 결정: D-109(슬리브 소유권), D-153(시세판을 따라 도는 재스캔).
//  정본 숫자: research/studies/30_strong_stock_strategies/SPEC.md 2.2~2.4절.
#include "StrategyLoadPass.h"

#include "api/KisClient.h"
#include "core/Engine.h"
#include "core/KstTime.h"
#include "strategy/DevScaleRules.h"
#include "strategy/VwapPullbackRules.h"
#include "strategy/VwapPullbackStrategy.h"
#include "universe/MarketBoard.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include "utils/Utf8.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace strategy_load
{
namespace
{
constexpr const char* kIdPrefix   = "VWAPPB";
constexpr int         kHhmmShift  = 100;

struct Settings
{
    std::string               account;
    vwap_pullback::RuleParams rules;
    bool                      shadow                 = true;
    int                       rescan_interval_sec    = 20; // 시세판이 끊겼을 때의 예비 주기
    int                       snapshot_stale_minutes = 3;  // 1분 파일이 없을 때 시세판 판을 선정 시각 뒤 이 분 안에서만 쓴다
    std::string               board_minute_directory;      // board_YYYYMMDD.csv 폴더. 비면 엔진 틱 캡처 폴더
};

vwap_pullback::RuleParams parse_rules(const json& node)
{
    vwap_pullback::RuleParams rules;
    read_or_keep(node, "select_hhmm", rules.select_hhmm);
    read_or_keep(node, "change_min_pct", rules.change_min_percent);
    read_or_keep(node, "change_max_pct", rules.change_max_percent);
    read_or_keep(node, "turnover_top_n", rules.turnover_top_n);
    read_or_keep(node, "min_prev_close_krw", rules.min_previous_close_krw);
    read_or_keep(node, "min_turnover_krw", rules.min_turnover_krw);
    read_or_keep(node, "first_arm_hhmm", rules.first_arm_hhmm);
    read_or_keep(node, "no_new_entry_hhmm", rules.no_new_entry_hhmm);
    read_or_keep(node, "retrace_min", rules.retrace_min);
    read_or_keep(node, "retrace_max", rules.retrace_max);
    read_or_keep(node, "vwap_band_pct", rules.vwap_band_percent);
    read_or_keep(node, "max_run_pct", rules.max_run_percent);
    read_or_keep(node, "disarm_below_vwap_pct", rules.disarm_below_vwap_percent);
    read_or_keep(node, "disarm_retrace", rules.disarm_retrace);
    read_or_keep(node, "arm_timeout_bars", rules.arm_timeout_bars);
    read_or_keep(node, "entry_ticks", rules.entry_ticks);
    read_or_keep(node, "vi_jump_pct", rules.vi_jump_percent);
    read_or_keep(node, "vi_zero_volume_bars", rules.vi_zero_volume_bars);
    read_or_keep(node, "stop_below_low_pct", rules.stop_below_low_percent);
    read_or_keep(node, "min_stop_width_pct", rules.min_stop_width_percent);
    read_or_keep(node, "max_stop_width_pct", rules.max_stop_width_percent);
    read_or_keep(node, "exit_hhmm", rules.exit_hhmm);
    return rules;
}

Settings parse_settings(const LoadPass& context, const json& node)
{
    Settings settings;
    settings.rules                  = parse_rules(node);
    settings.board_minute_directory = context.engine.capture_directory();
    read_or_keep(node, "account", settings.account);
    read_or_keep(node, "shadow", settings.shadow);
    read_or_keep(node, "rescan_interval_sec", settings.rescan_interval_sec);
    read_or_keep(node, "snapshot_stale_minutes", settings.snapshot_stale_minutes);
    read_or_keep(node, "board_minute_dir", settings.board_minute_directory);

    if (!settings.shadow)
    {
        LOG_WARN("[Main] VWAP_PULLBACK shadow=false — 주문 경로가 아직 없어 그림자로 돈다(Phase 1)");
        settings.shadow = true;
    }

    return settings;
}

// ETF·ETN·리츠·우선주·스팩은 선정에서 뺀다(SPEC 2.2). 우선주는 코드 끝자리가 0이 아닌 것으로 본다.
bool excluded_stock(const std::string& code, const std::string& name)
{
    static const std::vector<std::string> kEtfPrefixes =
        etf_filter::load_list("etf_prefixes.json", etf_filter::default_prefixes());
    static const std::vector<std::string> kEtfTokens =
        etf_filter::load_list("etf_name_tokens.json", etf_filter::default_tokens());
    static const std::vector<std::string> kReitSuffixes =
        etf_filter::load_list("reit_name_suffixes.json", etf_filter::default_reit_suffixes());
    static const std::vector<std::string> kReitExacts =
        etf_filter::load_list("reit_names.json", etf_filter::default_reit_exacts());

    if (code.empty() || code.back() != '0')
    {
        return true;
    }

    if (name.find("스팩") != std::string::npos)
    {
        return true;
    }

    return etf_filter::is_etf_like(name, kEtfPrefixes, kEtfTokens) ||
           etf_filter::is_reit_like(name, kReitSuffixes, kReitExacts);
}

std::filesystem::path selection_path(const std::string& date)
{
    return Logger::instance().path_for("vwappb_selection_" + date + ".json");
}

// 임시 파일에 다 쓴 뒤 이름을 바꿔 넣는다 — 점검 스크립트가 반쯤 쓰인 파일을 읽지 않게.
bool write_file_atomically(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

        if (!file || !(file << text))
        {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

// 선정 장부 — 데이터 스레드(재스캔)만 쓴다. 하루에 한 번 고르고, 그날은 같은 목록을 돌려준다.
class Sleeve
{
public:
    Sleeve(Engine& engine, Settings settings)
        : engine_(engine)
        , settings_(std::move(settings))
    {
    }

    const Settings& settings() const
    {
        return settings_;
    }

    std::vector<symbol::SymbolId> universe();
    std::unique_ptr<StrategyBase> make(symbol::SymbolId symbol);

private:
    bool load_selection_file();
    bool select_from_board(std::time_t now);
    void write_selection_file() const;

    Engine&                              engine_;
    Settings                             settings_;
    std::string                          date_;
    std::vector<vwap_pullback::Selected> selected_;
    std::string                          source_;
    std::string                          selected_at_;
    bool                                 ready_        = false;
    bool                                 file_pending_ = false; // 다음 재스캔에서 선점 여부를 붙여 적는다
    bool                                 empty_warned_ = false;
    std::set<std::string>                attached_; // factory가 실제로 만든 종목 — 나머지는 다른 슬리브가 선점
    std::set<std::string>                preempted_from_file_;
};

std::vector<symbol::SymbolId> Sleeve::universe()
{
    const std::time_t now  = std::time(nullptr);
    const std::string date = kst::date_yyyymmdd(now);

    if (date != date_)
    {
        date_ = date;
        selected_.clear();
        attached_.clear();
        preempted_from_file_.clear();
        ready_        = false;
        file_pending_ = false;
        empty_warned_ = false;
    }

    if (kst::hhmmss_int(now) / kHhmmShift < settings_.rules.select_hhmm)
    {
        return {};
    }

    if (!ready_)
    {
        if (load_selection_file())
        {
            ready_ = true;
        }
        else if (select_from_board(now))
        {
            ready_        = true;
            file_pending_ = true;
        }
        else
        {
            if (!empty_warned_)
            {
                LOG_WARN("[VWAPPB] 09:30 시세판을 못 읽어 아직 고르지 못했다 — 다음 판에서 다시 본다");
                empty_warned_ = true;
            }

            return {};
        }
    }
    else if (file_pending_)
    {
        write_selection_file(); // 직전 재스캔에서 factory가 붙인 종목이 정해졌다 — 선점 여부를 같이 적는다
        file_pending_ = false;
    }

    std::vector<symbol::SymbolId> symbols;
    symbols.reserve(selected_.size());

    for (const vwap_pullback::Selected& selected : selected_)
    {
        symbols.push_back(engine_.symbols().intern(selected.code));
    }

    return symbols;
}

std::unique_ptr<StrategyBase> Sleeve::make(symbol::SymbolId symbol)
{
    const std::string code = engine_.symbols().name(symbol).string();
    const auto        found = std::find_if(selected_.begin(), selected_.end(), [&code](const vwap_pullback::Selected& selected)
    {
        return selected.code == code;
    });

    if (found == selected_.end())
    {
        return nullptr;
    }

    attached_.insert(code);

    VwapPullbackStrategy::Params parameters;
    parameters.account        = settings_.account;
    parameters.ticker         = code;
    parameters.name           = found->name;
    parameters.previous_close = found->previous_close;
    parameters.rules          = settings_.rules;
    parameters.shadow         = settings_.shadow;

    auto    strategy = std::make_unique<VwapPullbackStrategy>(std::move(parameters));
    Engine& engine   = engine_;
    strategy->set_owner_block([&engine](symbol::SymbolId owned_symbol)
    {
        if (engine.is_exit_managed(owned_symbol))
        {
            return std::string("exit_managed");
        }

        const std::vector<symbol::SymbolId> exempt = engine.slot_exempt_symbols();

        if (std::find(exempt.begin(), exempt.end(), owned_symbol) != exempt.end())
        {
            return std::string("slot_exempt");
        }

        return std::string();
    });
    return strategy;
}

bool Sleeve::load_selection_file()
{
    std::ifstream file(selection_path(date_), std::ios::binary);

    if (!file)
    {
        return false;
    }

    const json document = json::parse(file, nullptr, false);

    if (document.is_discarded() || document.value("date", std::string()) != date_)
    {
        return false;
    }

    const auto selected = document.find("selected");

    if (selected == document.end() || !selected->is_array())
    {
        return false;
    }

    selected_.clear();

    for (const json& entry : *selected)
    {
        vwap_pullback::Selected row;
        row.code           = entry.value("code", std::string());
        row.name           = entry.value("name", std::string());
        row.previous_close = entry.value("previous_close", 0.0);
        row.change_percent = entry.value("change_percent", 0.0);
        row.value          = entry.value("value", 0.0);

        if (!row.code.empty())
        {
            selected_.push_back(std::move(row));
        }
    }

    source_      = document.value("source", std::string());
    selected_at_ = document.value("selected_at", std::string());
    LOG_INFO("[VWAPPB] 선정 파일을 다시 읽었다 — " + std::to_string(selected_.size()) + "종목(" + source_ + ")");
    return true;
}

bool Sleeve::select_from_board(std::time_t now)
{
    std::vector<vwap_pullback::BoardRow> rows;

    // 1순위: 1분 저장 파일의 09:30 뒤 첫 판 — 재기동이 늦어도 같은 판으로 고른다.
    const std::filesystem::path board_file =
        utf8::path_from_utf8(settings_.board_minute_directory) / universe::board_minute_file_name(now);
    {
        std::ifstream file(board_file, std::ios::binary);

        if (file)
        {
            rows    = vwap_pullback::rows_at_selection(file, settings_.rules.select_hhmm);
            source_ = "board_csv";
        }
    }

    const std::shared_ptr<const universe::BoardSnapshot> board = universe::MarketBoard::instance().snapshot();

    // 2순위: 지금 시세판 — 선정 시각 뒤 snapshot_stale_minutes 안에 받은 판일 때만.
    if (rows.empty() && board && kst::date_yyyymmdd(board->received_at) == date_)
    {
        const int received_hhmm = kst::hhmmss_int(board->received_at) / kHhmmShift;

        if (received_hhmm >= settings_.rules.select_hhmm &&
            received_hhmm < settings_.rules.select_hhmm + settings_.snapshot_stale_minutes)
        {
            for (const universe::BoardQuote& quote : board->quotes)
            {
                vwap_pullback::BoardRow row;
                row.code           = quote.code;
                row.name           = quote.name;
                row.price          = quote.price;
                row.value          = quote.value;
                row.volume         = quote.volume;
                row.change_percent = quote.change_percent;
                rows.push_back(std::move(row));
            }

            source_ = "snapshot";
        }
    }

    if (rows.empty())
    {
        return false;
    }

    // 1분 파일에는 이름이 없다 — 판의 이름, 없으면 종목 목록에서 채운다.
    std::map<std::string, std::string> names;

    if (board)
    {
        for (const universe::BoardQuote& quote : board->quotes)
        {
            names.emplace(quote.code, quote.name);
        }
    }

    if (const auto listing = universe::MarketBoard::instance().listing())
    {
        for (const universe::ListedStock& stock : *listing)
        {
            names.emplace(stock.code, stock.name);
        }
    }

    for (vwap_pullback::BoardRow& row : rows)
    {
        if (row.name.empty())
        {
            const auto found = names.find(row.code);
            row.name         = found != names.end() ? found->second : std::string();
        }

        row.excluded = excluded_stock(row.code, row.name);
    }

    selected_    = vwap_pullback::select_candidates(rows, settings_.rules);
    selected_at_ = kst::hhmmss(now);
    std::string listed;

    for (const vwap_pullback::Selected& selected : selected_)
    {
        listed += " " + selected.code;
    }

    LOG_INFO("[VWAPPB] 09:30 선정 " + std::to_string(selected_.size()) + "종목(" + source_ + "):" + listed);
    return true;
}

void Sleeve::write_selection_file() const
{
    json document;
    document["date"]        = date_;
    document["selected_at"] = selected_at_;
    document["source"]      = source_;
    document["shadow"]      = settings_.shadow;
    json selected           = json::array();
    size_t preempted_count  = 0;

    for (const vwap_pullback::Selected& row : selected_)
    {
        const bool preempted = attached_.count(row.code) == 0; // 다른 슬리브·청산 관리가 이미 붙어 있었다
        preempted_count += preempted ? 1 : 0;
        selected.push_back({{"code", row.code},
                            {"name", row.name},
                            {"previous_close", row.previous_close},
                            {"change_percent", row.change_percent},
                            {"value", row.value},
                            {"preempted", preempted}});
    }

    document["selected"]        = std::move(selected);
    document["preempted_count"] = preempted_count;

    if (!write_file_atomically(selection_path(date_), document.dump(2)))
    {
        LOG_WARN("[VWAPPB] 선정 파일 쓰기 실패 — vwappb_selection_" + date_ + ".json");
    }
}

// 오늘 장부(logs/trades_YYYYMMDD.csv)의 VWAPPB_ 종목별 순수량. 0 이하는 뺀다.
std::map<std::string, long long> net_quantity_today()
{
    std::map<std::string, long long> net_quantity;
    std::ifstream                    ledger(Logger::instance().path_for("trades_" + kst::date_yyyymmdd(std::time(nullptr)) + ".csv"));
    devscale_rules::add_net_quantity_from_ledger(ledger, kIdPrefix, net_quantity);
    std::erase_if(net_quantity, [](const auto& entry)
    {
        return entry.second <= 0;
    });
    return net_quantity;
}
} // namespace

void apply_vwap_pullback_holdings(LoadPass& context, const std::map<std::string, long long>& net_quantity,
                                  const std::map<std::string, int>& held_quantity)
{
    Engine&      engine          = context.engine;
    const size_t symbol_capacity = engine.symbols().capacity();

    for (const auto& [ticker, net] : net_quantity)
    {
        const auto held  = held_quantity.find(ticker);
        const int  owned = held != held_quantity.end() ? held->second : 0;

        switch (vwap_pullback::classify_holding(net, owned))
        {
        case vwap_pullback::Ownership::None:
            break;

        case vwap_pullback::Ownership::Partial:
            LOG_WARN("[Main] VWAPPB 재인수 안 함 " + ticker + " — 장부 순수량 " + std::to_string(net) + "주 < 잔고 " +
                     std::to_string(owned) + "주(남의 몫이 섞임), 청산 관리가 맡는다");
            break;

        case vwap_pullback::Ownership::Full:
        {
            const symbol::SymbolId symbol = engine.symbols().intern(ticker); // 장부 CSV의 문자열 티커 — 여기서 id가 된다
            mark_symbol(context.basket_owned, symbol, symbol_capacity);
            mark_symbol(context.scan_covered, symbol, symbol_capacity);
            context.vwap_pullback_owned[ticker] = net;
            break;
        }
        }
    }
}

void reserve_vwap_pullback_holdings(LoadPass& context, const json& node)
{
    if (!node.value("enabled", false))
    {
        return;
    }

    const std::map<std::string, long long> net_quantity = net_quantity_today();

    if (net_quantity.empty())
    {
        return; // 오늘 VWAPPB가 산 것이 없다 — 잔고를 부르지 않는다
    }

    KisClient held_kis(context.kis_config);

    if (!held_kis.authenticate())
    {
        LOG_WARN("[Main] VWAPPB 재인수: 잔고 조회 인증 실패 — 보유분은 청산 관리가 맡는다");
        return;
    }

    const KisResult<AccountBalance> balance = held_kis.get_balance();

    if (!balance)
    {
        LOG_WARN("[Main] VWAPPB 재인수: 잔고 조회 실패(" + error_text(balance) + ") — 보유분은 청산 관리가 맡는다");
        return;
    }

    std::map<std::string, int> held_quantity;

    for (const Holding& holding : balance->holdings)
    {
        held_quantity[holding.ticker] += holding.quantity;
    }

    apply_vwap_pullback_holdings(context, net_quantity, held_quantity);
}

void load_vwap_pullback(LoadPass& context, const json& node)
{
    if (!node.value("enabled", false))
    {
        LOG_INFO("[Main] VWAP_PULLBACK 꺼짐(enabled=false) — 등록하지 않는다");
        return;
    }

    Settings settings = parse_settings(context, node);
    Engine&  engine   = context.engine;

    if (!universe::MarketBoard::instance().running())
    {
        LOG_WARN("[Main] VWAP_PULLBACK: 시세판이 꺼져 있다 — 1분 저장 파일이 있을 때만 고른다(DEVIATION_SCALE의 market_board를 켠다)");
    }

    // 재인수한 자기 보유분 — 15:10 시간 청산을 맡을 전략을 지금 붙인다.
    for (const auto& [ticker, net] : context.vwap_pullback_owned)
    {
        VwapPullbackStrategy::Params parameters;
        parameters.account          = settings.account;
        parameters.ticker           = ticker;
        parameters.rules            = settings.rules;
        parameters.shadow           = settings.shadow;
        parameters.own_net_quantity = net;
        LOG_INFO("[Main] VWAPPB 재인수 " + ticker + " 순수량 " + std::to_string(net) + "주");
        add_gated(context, std::make_unique<VwapPullbackStrategy>(std::move(parameters)));
    }

    const size_t top_n    = static_cast<size_t>(std::max(settings.rules.turnover_top_n, 0));
    const int    interval = settings.rescan_interval_sec;
    auto         sleeve   = std::make_shared<Sleeve>(engine, std::move(settings));

    engine.set_universe_rescan(
        [sleeve](KisClient&)
        {
            return sleeve->universe();
        },
        gate_factory(context,
                     [sleeve](symbol::SymbolId symbol)
                     {
                         return sleeve->make(symbol);
                     }),
        interval, top_n, 0, 0, 2, true);

    LOG_INFO("[Main] VWAP_PULLBACK 등록 — 그림자 모드, " + std::to_string(sleeve->settings().rules.select_hhmm) +
             " 선정 상위 " + std::to_string(top_n) + "종목");
}
} // namespace strategy_load
