#include "strategy/VwapPullbackStrategy.h"

#include "api/KisClient.h"
#include "core/KstTime.h"
#include "core/PrefetchPool.h"
#include "universe/MarketBoard.h"
#include "utils/Logger.h"
#include "utils/Utf8.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
constexpr int kHhmmShift        = 100;
constexpr int kSessionOpenHhmm  = 900;
constexpr int kSessionCloseHhmm = 1530;
constexpr int kMaxMinuteBars    = 400; // 09:00~15:30은 391분 — 하루치를 다 담는다

const char* const kShadowHeader =
    "kst_time,ticker,event,bar_hhmm,reason,price,vwap,board_vwap,retrace,entry,stop,quantity,blocked,ws_ticks,rest_ticks,"
    "match\n";

// 그림자 파일은 슬리브의 종목 K개가 서로 다른 샤드 스레드에서 같이 쓴다. 락 안에서는 행을 대기열에 넣기만 하고,
//  파일 열기·쓰기는 락을 놓은 뒤 한 스레드만 한다 — 그동안 다른 스레드는 넣고 바로 돌아간다(코드 규약 4.5).
//  쓰는 스레드는 대기열이 빌 때까지 이어서 쓰므로 행 순서는 넣은 순서 그대로다.
struct PendingRow
{
    std::filesystem::path path;
    std::string           line;
    std::string           strategy_id; // 못 열었을 때 로그용
};

struct ShadowWriter
{
    std::mutex              mutex;
    std::vector<PendingRow> pending;
    bool                    writing = false; // [inv] true인 동안 파일에 쓰는 스레드는 그 하나뿐이다
};

ShadowWriter g_shadow_writer;

// 락 밖에서 부른다. 같은 파일 행은 한 번 열어 이어 쓴다.
void append_rows(const std::vector<PendingRow>& rows)
{
    size_t start = 0;

    while (start < rows.size())
    {
        const std::filesystem::path& path = rows[start].path;
        size_t                       end  = start;

        while (end < rows.size() && rows[end].path == path)
        {
            ++end;
        }

        std::error_code error;
        const bool      fresh = !std::filesystem::exists(path, error) || std::filesystem::file_size(path, error) == 0;
        std::ofstream   file(path, std::ios::app | std::ios::binary);

        if (!file)
        {
            for (size_t row_index = start; row_index < end; ++row_index)
            {
                LOG_WARN("[" + rows[row_index].strategy_id + "] 그림자 파일을 못 열었다 — 행을 버린다: " +
                         rows[row_index].line);
            }

            start = end;
            continue;
        }

        if (fresh)
        {
            file << kShadowHeader;
        }

        for (size_t row_index = start; row_index < end; ++row_index)
        {
            file << rows[row_index].line;
        }

        start = end;
    }
}

void enqueue_shadow_row(PendingRow row)
{
    std::vector<PendingRow> batch;

    {
        std::lock_guard<std::mutex> lock(g_shadow_writer.mutex);
        g_shadow_writer.pending.push_back(std::move(row));

        if (g_shadow_writer.writing)
        {
            return; // 쓰는 스레드가 이 행까지 이어서 쓴다
        }

        g_shadow_writer.writing = true;
        batch.swap(g_shadow_writer.pending);
    }

    while (true)
    {
        append_rows(batch);
        batch.clear();

        std::lock_guard<std::mutex> lock(g_shadow_writer.mutex);

        if (g_shadow_writer.pending.empty())
        {
            g_shadow_writer.writing = false;
            return;
        }

        batch.swap(g_shadow_writer.pending);
    }
}

std::string format_number(double value, int digits)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}

int bar_hhmm(const MarketData& bar)
{
    return kst::hhmmss_int(std::chrono::system_clock::to_time_t(bar.timestamp)) / kHhmmShift;
}

vwap_pullback::MinuteBar to_minute_bar(const MarketData& bar)
{
    vwap_pullback::MinuteBar minute_bar;
    minute_bar.hhmm   = bar_hhmm(bar);
    minute_bar.open   = bar.open;
    minute_bar.high   = bar.high;
    minute_bar.low    = bar.low;
    minute_bar.close  = bar.close;
    minute_bar.volume = bar.volume;
    return minute_bar;
}

// 09:00부터 now_hhmm 앞까지의 분 수(진행 중 분 포함 한 칸 여유).
int minutes_since_open(int now_hhmm)
{
    const int minutes = (now_hhmm / kHhmmShift - kSessionOpenHhmm / kHhmmShift) * 60 + now_hhmm % kHhmmShift + 1;
    return std::clamp(minutes, 1, kMaxMinuteBars);
}

bars::BarAggregator::Config minute_config()
{
    bars::BarAggregator::Config config;
    config.interval_min  = 1;
    config.keep          = kMaxMinuteBars;
    config.session_open  = kSessionOpenHhmm;
    config.session_close = kSessionCloseHhmm;
    return config;
}

std::vector<std::string_view> split_csv(std::string_view line)
{
    std::vector<std::string_view> fields;

    while (true)
    {
        const size_t comma = line.find(',');
        fields.push_back(line.substr(0, comma));

        if (comma == std::string_view::npos)
        {
            break;
        }

        line.remove_prefix(comma + 1);
    }

    return fields;
}
} // namespace

VwapPullbackStrategy::VwapPullbackStrategy(Params parameters)
    : parameters_(std::move(parameters))
    , id_("VWAPPB_" + parameters_.ticker)
    , machine_(parameters_.previous_close, parameters_.rules)
    , aggregator_(minute_config())
{
}

VwapPullbackStrategy::~VwapPullbackStrategy()
{
    stop_prefetch();
}

std::string VwapPullbackStrategy::describe() const
{
    return id_ + " " + parameters_.name + " 전일종가 " + format_number(parameters_.previous_close, 0) +
           (parameters_.shadow ? " (그림자 — 주문 없음)" : "");
}

void VwapPullbackStrategy::set_clock(Clock clock)
{
    clock_ = std::move(clock);
}

void VwapPullbackStrategy::set_minute_fetcher(MinuteFetcher fetcher)
{
    minute_fetcher_ = std::move(fetcher);
}

void VwapPullbackStrategy::set_board_vwap(BoardVwap board_vwap)
{
    board_vwap_ = std::move(board_vwap);
}

void VwapPullbackStrategy::set_owner_block(OwnerBlock owner_block)
{
    owner_block_ = std::move(owner_block);
}

std::time_t VwapPullbackStrategy::now() const
{
    return clock_ ? clock_() : std::time(nullptr);
}

int VwapPullbackStrategy::kst_hhmm() const
{
    return kst::hhmmss_int(now()) / kHhmmShift;
}

std::vector<WatchSpec> VwapPullbackStrategy::get_watch_specifications() const
{
    WatchSpec specification;
    specification.ticker     = parameters_.ticker;
    specification.trade_only = true; // 봉만 만든다 — 호가는 안 쓴다
    return {specification};
}

void VwapPullbackStrategy::on_start()
{
    symbol_id_ = symbol_of(parameters_.ticker);
    restore_from_shadow_file();
    LOG_INFO("[" + id_ + "] 시작 — " + describe() + (restored_done_ ? ", 오늘 신호는 이미 냄(그림자 파일)" : ""));

    stop_prefetch(); // 재등록 경로 대비

    if (prefetch_pool_ != nullptr)
    {
        prefetch_task_ = prefetch_pool_->add([this]
        {
            run_prefetch_once();
        });
    }
}

void VwapPullbackStrategy::on_stop()
{
    stop_prefetch();
    LOG_INFO("[" + id_ + "] 종료");
}

void VwapPullbackStrategy::stop_prefetch()
{
    if (prefetch_pool_ != nullptr && prefetch_task_ != 0)
    {
        prefetch_pool_->remove(prefetch_task_);
    }

    prefetch_task_ = 0;
}

void VwapPullbackStrategy::run_prefetch_once()
{
    bool need_bootstrap = false;
    bool need_recheck   = false;
    {
        std::lock_guard<std::mutex> lock(fetch_mutex_);
        need_bootstrap = !bootstrap_bars_;
        need_recheck   = recheck_wanted_ && !recheck_bars_;
    }

    const int hhmm = kst_hhmm();

    if ((!need_bootstrap && !need_recheck) || hhmm < kSessionOpenHhmm)
    {
        return;
    }

    const int               count = minutes_since_open(std::min(hhmm, kSessionCloseHhmm));
    std::vector<MarketData> fetched;

    if (minute_fetcher_)
    {
        fetched = minute_fetcher_(parameters_.ticker, count);
    }
    else if (kis_ != nullptr)
    {
        fetched = kis_->get_minute_ohlcv(parameters_.ticker, count, 1);
    }

    if (fetched.empty())
    {
        return; // 다음 주기에 다시 받는다
    }

    // 진행 중인 분(지금 분)은 아직 덜 찬 봉이라 뺀다 — 시드하면 닫힌 봉으로 굳는다.
    fetched.erase(std::remove_if(fetched.begin(), fetched.end(), [hhmm](const MarketData& bar)
    {
        return bar_hhmm(bar) >= hhmm;
    }), fetched.end());

    auto shared = std::make_shared<const std::vector<MarketData>>(std::move(fetched));
    std::lock_guard<std::mutex> lock(fetch_mutex_);

    if (need_bootstrap)
    {
        bootstrap_bars_ = std::move(shared);
    }
    else
    {
        recheck_bars_ = std::move(shared);
    }
}

void VwapPullbackStrategy::on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out)
{
    (void)out; // 그림자 모드 — 주문을 내지 않는다(Phase 1). [inv] out은 늘 빈 채로 돌아간다.

    if (!same_symbol(symbol_id_, parameters_.ticker, trade.symbol_id, trade.ticker.view()))
    {
        return;
    }

    if (trade.received_ns == 0)
    {
        ++rest_ticks_;
    }
    else
    {
        ++websocket_ticks_;
    }

    if (trade.price > 0.0)
    {
        last_price_ = trade.price;
    }

    aggregator_.on_tick(trade);
    aggregator_.close_stale(symbol_id_, now());

    if (!seeded_)
    {
        std::shared_ptr<const std::vector<MarketData>> bootstrap;
        {
            std::lock_guard<std::mutex> lock(fetch_mutex_);
            bootstrap = bootstrap_bars_;
        }

        if (bootstrap)
        {
            const int added = aggregator_.seed(symbol_id_, *bootstrap);
            seeded_         = true;
            LOG_INFO("[" + id_ + "] REST 1분봉 시드 " + std::to_string(bootstrap->size()) + "봉, 새 " +
                     std::to_string(added));
        }
    }

    if (seeded_)
    {
        feed_closed_bars();
    }

    run_exit_checks(last_price_);
    run_recheck();
}

void VwapPullbackStrategy::feed_closed_bars()
{
    const int now_hhmm     = kst_hhmm();
    const int closed_count = aggregator_.closed_count(symbol_id_);

    if (closed_count == fed_closed_count_ && now_hhmm == last_feed_hhmm_)
    {
        return;
    }

    fed_closed_count_ = closed_count;
    last_feed_hhmm_   = now_hhmm;

    const std::vector<MarketData> bars = aggregator_.snapshot(symbol_id_, 0);
    const size_t first_closed          = aggregator_.current_slot(symbol_id_).valid() ? 1 : 0;

    // snapshot은 [0]=최신이라 뒤에서부터 넘긴다(오래된 분 → 새 분).
    for (size_t index = bars.size(); index > first_closed; --index)
    {
        const vwap_pullback::MinuteBar minute_bar = to_minute_bar(bars[index - 1]);

        if (minute_bar.hhmm <= machine_.last_hhmm() || minute_bar.hhmm >= now_hhmm)
        {
            continue; // 이미 넘긴 분, 아직 안 끝난 분
        }

        track_shadow_position(minute_bar);

        if (restored_done_)
        {
            continue; // 오늘 신호는 재기동 전에 냈다 — 같은 종목은 하루 한 번
        }

        handle_step(minute_bar, machine_.on_bar(minute_bar));
    }
}

void VwapPullbackStrategy::handle_step(const vwap_pullback::MinuteBar& bar, const vwap_pullback::StepResult& step)
{
    ShadowRow row;
    row.bar_hhmm   = bar.hhmm;
    row.price      = bar.close;
    row.vwap       = step.vwap;
    row.retrace    = step.retrace;

    switch (step.event)
    {
    case vwap_pullback::Event::None:
        return;

    case vwap_pullback::Event::Armed:
        row.event      = "ARM";
        row.stop       = step.pullback_low; // 무장 시점 눌림 저점
        row.board_vwap = board_vwap();
        break;

    case vwap_pullback::Event::Disarmed:
        row.event  = "DISARM";
        row.reason = step.reason;
        break;

    case vwap_pullback::Event::Signal:
        row.event         = "SIGNAL";
        row.entry         = step.entry_price;
        row.stop          = step.stop;
        row.board_vwap    = board_vwap();
        row.blocked       = entry_block();
        live_signal_hhmm_ = bar.hhmm;

        if (row.blocked.empty())
        {
            shadow_entry_ = step.entry_price;
            shadow_stop_  = step.stop;
        }

        LOG_INFO("[" + id_ + "] 그림자 신호 " + std::to_string(bar.hhmm) + " 진입 " + format_number(step.entry_price, 0) +
                 " 손절 " + format_number(step.stop, 0) + (row.blocked.empty() ? "" : " (막힘: " + row.blocked + ")"));
        break;
    }

    write_row(row);
}

void VwapPullbackStrategy::track_shadow_position(const vwap_pullback::MinuteBar& bar)
{
    if (shadow_entry_ <= 0.0 || shadow_exited_ || bar.hhmm <= live_signal_hhmm_)
    {
        return;
    }

    if (bar.low <= shadow_stop_)
    {
        ShadowRow row;
        row.event      = "EXIT";
        row.bar_hhmm   = bar.hhmm;
        row.reason     = "stop";
        row.price      = shadow_stop_;
        row.entry      = shadow_entry_;
        row.stop       = shadow_stop_;
        shadow_exited_ = true;
        write_row(row);
    }
}

int VwapPullbackStrategy::own_exit_quantity() const
{
    const int confirmed = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);
    return vwap_pullback::own_exit_quantity(confirmed, parameters_.own_net_quantity);
}

void VwapPullbackStrategy::run_exit_checks(double last_price)
{
    const int hhmm = kst_hhmm();

    if (!vwap_pullback::exit_due(hhmm, parameters_.rules.exit_hhmm))
    {
        return;
    }

    if (shadow_entry_ > 0.0 && !shadow_exited_)
    {
        ShadowRow row;
        row.event      = "EXIT";
        row.bar_hhmm   = hhmm;
        row.reason     = "time";
        row.price      = last_price;
        row.entry      = shadow_entry_;
        row.stop       = shadow_stop_;
        shadow_exited_ = true;
        write_row(row);
    }

    // 재기동으로 다시 맡은 자기 보유분 — Phase 2에서 이 수량으로 매도를 낸다. 지금은 행만 남긴다.
    if (parameters_.own_net_quantity > 0 && !own_exit_written_)
    {
        ShadowRow row;
        row.event         = "EXIT";
        row.bar_hhmm      = hhmm;
        row.reason        = "own_holding";
        row.price         = last_price;
        row.quantity      = own_exit_quantity();
        own_exit_written_ = true;
        write_row(row);
    }

    if (!summary_written_)
    {
        ShadowRow row;
        row.event        = "SUMMARY";
        row.bar_hhmm     = hhmm;
        summary_written_ = true;
        write_row(row);
    }

    std::lock_guard<std::mutex> lock(fetch_mutex_);
    recheck_wanted_ = !recheck_written_;
}

void VwapPullbackStrategy::run_recheck()
{
    if (recheck_written_)
    {
        return;
    }

    std::shared_ptr<const std::vector<MarketData>> rest_bars;
    {
        std::lock_guard<std::mutex> lock(fetch_mutex_);
        rest_bars = recheck_bars_;
    }

    if (!rest_bars)
    {
        return;
    }

    // REST 봉으로 같은 규칙을 처음부터 다시 돌린다 — 실시간 봉(틱 집계)과 신호 분이 같은가.
    vwap_pullback::PullbackMachine replay(parameters_.previous_close, parameters_.rules);
    int                            rest_signal_hhmm = 0;

    for (auto iterator = rest_bars->rbegin(); iterator != rest_bars->rend(); ++iterator)
    {
        const vwap_pullback::MinuteBar minute_bar = to_minute_bar(*iterator);

        if (minute_bar.hhmm >= parameters_.rules.exit_hhmm)
        {
            continue;
        }

        if (replay.on_bar(minute_bar).event == vwap_pullback::Event::Signal)
        {
            rest_signal_hhmm = minute_bar.hhmm;
            break;
        }
    }

    ShadowRow row;
    row.event        = "RECHECK";
    row.bar_hhmm     = rest_signal_hhmm;
    row.reason       = "live=" + std::to_string(live_signal_hhmm_) + " rest=" + std::to_string(rest_signal_hhmm);
    row.match        = rest_signal_hhmm == live_signal_hhmm_ ? "1" : "0";
    recheck_written_ = true;
    write_row(row);
}

std::string VwapPullbackStrategy::entry_block() const
{
    if (!is_active())
    {
        return "inactive";
    }

    if (entry_halted())
    {
        return "entry_halt";
    }

    if (entry_scale() <= 0.0)
    {
        return "entry_scale_zero";
    }

    if (confirmed_position(parameters_.account, symbol_id_, parameters_.ticker) > 0)
    {
        return "held"; // 다른 슬리브(또는 손) 보유 — 섞지 않는다
    }

    if (owner_block_)
    {
        return owner_block_(symbol_id_);
    }

    return std::string();
}

double VwapPullbackStrategy::board_vwap() const
{
    if (board_vwap_)
    {
        return board_vwap_(parameters_.ticker);
    }

    const std::shared_ptr<const universe::BoardSnapshot> board = universe::MarketBoard::instance().snapshot();

    if (!board)
    {
        return 0.0;
    }

    for (const universe::BoardQuote& quote : board->quotes)
    {
        if (quote.code == parameters_.ticker)
        {
            return quote.volume > 0.0 ? quote.value / quote.volume : 0.0;
        }
    }

    return 0.0;
}

std::filesystem::path VwapPullbackStrategy::shadow_path() const
{
    if (!parameters_.shadow_file.empty())
    {
        return utf8::path_from_utf8(parameters_.shadow_file);
    }

    return Logger::instance().path_for("vwappb_shadow_" + kst::date_yyyymmdd(now()) + ".csv");
}

void VwapPullbackStrategy::write_row(const ShadowRow& row) const
{
    constexpr int kPriceDigits   = 2;
    constexpr int kRetraceDigits = 4;

    std::ostringstream line;
    line << kst::hhmmss(now()) << ',' << parameters_.ticker << ',' << row.event << ',' << row.bar_hhmm << ','
         << row.reason << ',' << format_number(row.price, kPriceDigits) << ','
         << format_number(row.vwap, kPriceDigits) << ',' << format_number(row.board_vwap, kPriceDigits) << ','
         << format_number(row.retrace, kRetraceDigits) << ',' << format_number(row.entry, kPriceDigits) << ','
         << format_number(row.stop, kPriceDigits) << ',' << row.quantity << ',' << row.blocked << ','
         << websocket_ticks_ << ',' << rest_ticks_ << ',' << row.match << '\n';

    enqueue_shadow_row(PendingRow{shadow_path(), line.str(), id_});
}

void VwapPullbackStrategy::restore_from_shadow_file()
{
    constexpr size_t kTickerColumn = 1;
    constexpr size_t kEventColumn  = 2;
    constexpr size_t kHhmmColumn   = 3;
    constexpr size_t kReasonColumn = 4;

    std::ifstream file(shadow_path(), std::ios::binary);
    std::string   line;

    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        const std::vector<std::string_view> fields = split_csv(line);

        if (fields.size() <= kReasonColumn || fields[kTickerColumn] != parameters_.ticker)
        {
            continue;
        }

        const std::string_view event = fields[kEventColumn];

        if (event == "SIGNAL")
        {
            restored_done_    = true;
            const std::string_view hhmm_text = fields[kHhmmColumn];
            std::from_chars(hhmm_text.data(), hhmm_text.data() + hhmm_text.size(), live_signal_hhmm_);
        }
        else if (event == "SUMMARY")
        {
            summary_written_ = true;
        }
        else if (event == "RECHECK")
        {
            recheck_written_ = true;
        }
        else if (event == "EXIT" && fields[kReasonColumn] == "own_holding")
        {
            own_exit_written_ = true;
        }
    }
}
