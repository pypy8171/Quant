// 전 종목 시세판 구현 — 목록 받기·시세 한 바퀴·재랭킹·파일 쓰기. 왜 있는지는 universe/MarketBoard.h 머리말.
//  응답 읽기(parse_listing_page·parse_polling)는 MarketBoardParse.cpp.
#include "universe/MarketBoard.h"

#include "api/HttpGet.h"
#include "core/KstTime.h"
#include "utils/AtomicFile.h"
#include "utils/Logger.h"
#include "utils/ThreadGuard.h"
#include "utils/ThreadName.h"
#include "utils/Utf8.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace universe
{
namespace
{

// 받는 곳 두 군데. 목록은 시총순 쪽 목록(한 쪽 100종목이 상한 — 500·1000은 JSON이 아닌 응답, 09-26 실측),
//  시세는 폴링 주소에 코드를 쉼표로 이어 붙인다.
constexpr std::string_view kListingEndpoint = "https://m.stock.naver.com/api/stocks/marketValue/";
constexpr std::string_view kPollingEndpoint = "https://polling.finance.naver.com/api/realtime/domestic/stock/";
constexpr int              kListingPageSize = 100;
constexpr int              kListingMaxPages = 60; // 코스피 25쪽·코스닥 19쪽(09-26). 응답이 어긋나도 끝없이 돌지 않게
constexpr std::int64_t     kSecondsPerMinute = 60;
constexpr int              kDefaultCodesPerCall = 900; // 폴링 한 번에 묻는 종목 수(config 0일 때)
constexpr size_t           kCsvBytesPerRow = 96;      // 시세판 CSV 한 줄 80바이트 안팎 — 예약 크기
// 루프가 예외로 끝났을 때 다시 돌기 전에 쉬는 시간. 같은 원인으로 곧바로 다시 던져 로그만 쌓이는 것을 막는다.
constexpr std::chrono::milliseconds kRestartPause{1000};

// 웹페이지가 보내는 것과 같은 헤더. 없으면 폴링 주소가 빈 본문으로 200을 준다.
//  [wire] 2026-09-27 실측에서는 헤더 없이도 같은 본문(3종목 7,275바이트)이 왔다(2026-09-27 확인으로 고침 — 빈 본문은 재현되지 않음).
//  응답이 다시 바뀔 수 있어 헤더는 그대로 보낸다. 공식 문서 없음.
const std::vector<std::string>& naver_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0", "Referer: https://finance.naver.com/"};
    return headers;
}

} // namespace

std::vector<RankedStock> rank_universe(const std::vector<ListedStock>& listing, const BoardSnapshot& board,
                                       int n_market_value, int n_turnover, double min_turnover)
{
    // 코드로 시세를 찾는다. 판은 한 바퀴에 한 번 바뀌고 재랭킹은 1분에 한 번이라 여기서 색인을 세운다.
    std::unordered_map<std::string_view, const BoardQuote*> quote_of;
    quote_of.reserve(board.quotes.size() * 2);

    for (const BoardQuote& quote : board.quotes)
    {
        quote_of.emplace(quote.code, &quote);
    }

    std::vector<RankedStock>        ranked;
    std::unordered_set<std::string> taken;

    for (const char* market : {"KOSPI", "KOSDAQ"})
    {
        std::vector<std::pair<const ListedStock*, const BoardQuote*>> pool;

        for (const ListedStock& listed : listing)
        {
            if (listed.market != market)
            {
                continue;
            }

            const auto found = quote_of.find(listed.code);

            if (found == quote_of.end())
            {
                continue;
            }

            const BoardQuote& quote = *found->second;

            if (quote.value >= min_turnover && quote.market_value > 0.0)
            {
                pool.emplace_back(&listed, &quote);
            }
        }

        // 같은 값이면 코드순으로 — 실행마다 순서가 바뀌지 않게.
        auto take_top = [&](int count, auto key)
        {
            std::vector<std::pair<const ListedStock*, const BoardQuote*>> sorted = pool;
            std::sort(sorted.begin(), sorted.end(),
                      [&](const auto& left, const auto& right)
                      {
                          const double left_key  = key(*left.second);
                          const double right_key = key(*right.second);
                          return left_key != right_key ? left_key > right_key : left.first->code < right.first->code;
                      });

            if (count >= 0 && sorted.size() > static_cast<size_t>(count))
            {
                sorted.resize(static_cast<size_t>(count));
            }

            for (const auto& [listed, quote] : sorted)
            {
                if (!taken.insert(listed->code).second)
                {
                    continue;
                }

                // 이름은 목록 쪽을 먼저 쓴다 — 시세 쪽 이름이 비는 경우가 있다.
                ranked.push_back({listed->code, listed->name.empty() ? quote->name : listed->name,
                                  std::round(quote->price), market});
            }
        };

        take_top(n_market_value, [](const BoardQuote& quote)
        {
            return quote.market_value;
        });
        take_top(n_turnover, [](const BoardQuote& quote)
        {
            return quote.value;
        });
    }

    return ranked;
}

bool turnover_filled(const BoardSnapshot& board)
{
    size_t filled = 0;

    for (const BoardQuote& quote : board.quotes)
    {
        if (quote.value > 0.0)
        {
            ++filled;
        }
    }

    return !board.quotes.empty() && filled * 2 >= board.quotes.size();
}

std::string universe_file_text(const RankedUniverse& ranked, const std::string& source_label)
{
    nlohmann::json universe = nlohmann::json::array();

    for (const RankedStock& stock : ranked.stocks)
    {
        universe.push_back({{"ticker", stock.code},
                            {"name", stock.name},
                            {"close", static_cast<long long>(stock.close)},
                            {"market", stock.market}});
    }

    nlohmann::json market_map = nlohmann::json::object();
    nlohmann::json name_map   = nlohmann::json::object();

    if (ranked.listing)
    {
        for (const ListedStock& listed : *ranked.listing)
        {
            market_map[listed.code] = listed.market;

            if (!listed.name.empty())
            {
                name_map[listed.code] = listed.name;
            }
        }
    }

    const nlohmann::json document = {
        {"schema", 1},
        {"source", "engine:MarketBoard(naver)"},
        {"market", "ALL"},
        {"basDt", kst::date_yyyymmdd(ranked.ranked_at)},
        {"requested_date", kst::date_yyyymmdd(ranked.ranked_at)},
        {"mktcap_source", source_label},
        {"turnover_source", source_label},
        {"scanned_at", kst::datetime(ranked.ranked_at)},
        {"count", ranked.stocks.size()},
        {"universe", std::move(universe)},
        {"market_map", std::move(market_map)},
        {"name_map", std::move(name_map)},
    };
    return document.dump(1);
}

bool board_minute_due(std::time_t received_at, std::int64_t last_saved_minute)
{
    const std::int64_t minute = static_cast<std::int64_t>(received_at) / kSecondsPerMinute;

    if (minute == last_saved_minute)
    {
        return false;
    }

    const int minute_of_day = kst::sec_of_day(received_at) / static_cast<int>(kSecondsPerMinute);
    return minute_of_day >= kst::kKrMarketOpenMinute && minute_of_day <= kst::kKrAfterMarketCloseMinute;
}

std::string board_minute_file_name(std::time_t received_at)
{
    return "board_" + kst::date_yyyymmdd(received_at) + ".csv";
}

std::string board_minute_header()
{
    return "# 시세판 1분 표본 - 네이버 전 종목 5초 폴링 판 중 분마다 한 판(09:00~20:00, 엔진 매매 시간). 시각은 판을 다 받은 KST.\n"
           "# 16:00 뒤 값이 애프터마켓 시세를 담는지는 확인 전이다 - check_runtime_health.py 시세판 행이 15:30 대비 바뀐 종목 수로 판정한다.\n"
           "# 5초 표본이라 고가·저가는 없다. board_count=그 판의 종목 수, failed_batches/batches=그 판의 실패 요청 묶음/전체 묶음.\n"
           "kst_time,code,price,volume,turnover,change_pct,market_cap,board_count,failed_batches,batches\n";
}

std::string board_minute_rows(const BoardSnapshot& board)
{
    const std::string      digits = kst::hhmmss(board.received_at); // "HHMMSS"
    const std::string_view digit_view(digits);
    const std::string      time_text =
        std::format("{}:{}:{}", digit_view.substr(0, 2), digit_view.substr(2, 2), digit_view.substr(4, 2));
    const std::string board_tail =
        std::format(",{},{},{}\n", board.quotes.size(), board.failed_requests, board.request_count);
    std::string rows;
    rows.reserve(board.quotes.size() * kCsvBytesPerRow);

    for (const BoardQuote& quote : board.quotes)
    {
        std::format_to(std::back_inserter(rows), "{},{},{:.0f},{:.0f},{:.0f},{:.2f},{:.0f}", time_text, quote.code,
                       quote.price, quote.volume, quote.value, quote.change_percent, quote.market_value);
        rows.append(board_tail);
    }

    return rows;
}

MarketBoard::~MarketBoard()
{
    stop();
}

void MarketBoard::start(const Config& config)
{
    if (running_.exchange(true))
    {
        return;
    }

    config_ = config;

    if (config_.codes_per_call == 0)
    {
        config_.codes_per_call = kDefaultCodesPerCall;
    }

    config_.period_sec = std::max(1, config_.period_sec);
    config_.rerank_sec = std::max(config_.period_sec, config_.rerank_sec);
    // 수집 루프가 던지면 로그를 남기고 잠깐 쉰 뒤 다시 돈다. 시세판은 장 내내 갱신돼야 하고, 한 번 받기가 실패한
    //  상태는 다음 주기 받기에 이어지지 않아 다시 도는 쪽이 낫다. 끝내면 시세판이 그 시각 값에 멈춘다.
    worker_            = std::thread([this]
    {
        thread_guard::run_restarting("MarketBoard",
                                     [this]
                                     {
                                         return running_.load(std::memory_order_acquire);
                                     },
                                     kRestartPause,
                                     [this]
                                     {
                                         run();
                                     });
    });
    LOG_INFO("[MarketBoard] 시작 — 시세 " + std::to_string(config_.period_sec) + "초·재랭킹 " +
             std::to_string(config_.rerank_sec) + "초 주기, 시장별 시총 top" + std::to_string(config_.n_market_value) +
             " ∪ 거래대금 top" + std::to_string(config_.n_turnover) + (config_.universe_out.empty() ? "" : " → " + config_.universe_out) +
             (config_.minute_directory.empty() ? ", 1분 표본 끔" : ", 1분 표본 → " + config_.minute_directory));
}

void MarketBoard::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }
}

std::shared_ptr<const BoardSnapshot> MarketBoard::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::shared_ptr<const RankedUniverse> MarketBoard::ranked() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ranked_;
}

std::shared_ptr<const std::vector<ListedStock>> MarketBoard::listing() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return listing_;
}

bool MarketBoard::wait_new_board(std::uint64_t seen, std::chrono::milliseconds timeout, std::stop_token stop_token) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return board_arrived_.wait_for(lock, stop_token, timeout, [this, seen]
    {
        return generation_.load(std::memory_order_acquire) != seen;
    });
}

void MarketBoard::run()
{
    thread_name::set_current("MarketBoard");
    const auto period = std::chrono::seconds(config_.period_sec);
    const auto rerank_every = std::chrono::seconds(config_.rerank_sec);
    auto       next_sweep_at = std::chrono::steady_clock::now();
    auto       next_rerank_at = next_sweep_at;

    while (running_.load(std::memory_order_acquire))
    {
        // 목록은 KST 날짜가 바뀔 때 다시 받는다. 못 받았으면 시세도 못 받으니 다음 주기에 다시 한다.
        const std::string today = kst::date_yyyymmdd(std::time(nullptr));

        const bool have_listing = listing_ && !listing_->empty();

        if ((listing_date_ != today || !have_listing) && refresh_listing())
        {
            listing_date_ = today;
        }

        if (listing_ && !listing_->empty() && sweep())
        {
            if (!config_.minute_directory.empty())
            {
                const std::shared_ptr<const BoardSnapshot> board = snapshot();

                if (board && board_minute_due(board->received_at, last_minute_saved_))
                {
                    last_minute_saved_ = static_cast<std::int64_t>(board->received_at) / kSecondsPerMinute;
                    save_minute(*board);
                }
            }

            const auto now = std::chrono::steady_clock::now();

            if (now >= next_rerank_at)
            {
                rerank();
                next_rerank_at = now + rerank_every;
            }
        }

        next_sweep_at += period;
        const auto now = std::chrono::steady_clock::now();

        if (now >= next_sweep_at)
        {
            next_sweep_at = now; // 밀린 바퀴를 몰아 보내지 않는다
            continue;
        }

        // 멈추라는 신호를 곧 받도록 잘게 나눠 잔다.
        while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < next_sweep_at)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

bool MarketBoard::refresh_listing()
{
    std::vector<ListedStock> listing;

    for (const char* market : {"KOSPI", "KOSDAQ"})
    {
        int pages_needed = 1;

        for (int page = 1; page <= pages_needed && page <= kListingMaxPages; ++page)
        {
            if (!running_.load(std::memory_order_acquire))
            {
                return false;
            }

            const std::string url = std::string(kListingEndpoint) + market + "?page=" + std::to_string(page) +
                                    "&pageSize=" + std::to_string(kListingPageSize);
            int               total_count = -1;
            std::vector<ListedStock> rows = parse_listing_page(http::get(url, naver_headers()), market, total_count);

            if (total_count < 0)
            {
                LOG_WARN(std::string("[MarketBoard] 종목 목록 ") + market + " " + std::to_string(page) +
                         "쪽을 못 받았다 — 다음 주기에 목록 전체를 다시 받는다");
                return false;
            }

            pages_needed = (total_count + kListingPageSize - 1) / kListingPageSize;
            listing.insert(listing.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        }
    }

    request_urls_.clear();

    for (size_t begin = 0; begin < listing.size(); begin += config_.codes_per_call)
    {
        const size_t end = std::min(begin + config_.codes_per_call, listing.size());
        std::string  url(kPollingEndpoint);

        for (size_t index = begin; index < end; ++index)
        {
            if (index > begin)
            {
                url.push_back(',');
            }

            url.append(listing[index].code);
        }

        request_urls_.push_back(std::move(url));
    }

    // 목록 객체는 여기서 한 번 만들고, 재랭킹 결과·일봉 데우기는 같은 객체를 포인터로 나눠 쥔다.
    auto published = std::make_shared<const std::vector<ListedStock>>(std::move(listing));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        listing_ = published;
    }

    LOG_INFO("[MarketBoard] 종목 목록 " + std::to_string(published->size()) + "종목(개별주) — 시세 요청 " +
             std::to_string(request_urls_.size()) + "개로 나눈다");
    return true;
}

bool MarketBoard::sweep()
{
    const auto started = std::chrono::steady_clock::now();
    // 요청을 동시에 보낸다 — 차례로 보내면 0.5초 안팎, 동시면 0.2초 안팎이다(파이썬 피드 09-26 실측).
    std::vector<std::future<std::string>> bodies;
    bodies.reserve(request_urls_.size());

    for (const std::string& url : request_urls_)
    {
        bodies.push_back(std::async(std::launch::async, [&url]
        {
            return http::get(url, naver_headers());
        }));
    }

    auto   board  = std::make_shared<BoardSnapshot>();
    size_t failed = 0;
    board->quotes.reserve(listing_ ? listing_->size() : 0);

    for (std::future<std::string>& body : bodies)
    {
        std::vector<BoardQuote> quotes = parse_polling(body.get());

        if (quotes.empty())
        {
            ++failed; // 빈 본문·깨진 본문 — 그 묶음만 이번 판에서 빠진다
            continue;
        }

        board->quotes.insert(board->quotes.end(), std::make_move_iterator(quotes.begin()),
                             std::make_move_iterator(quotes.end()));
    }

    if (board->quotes.empty())
    {
        LOG_WARN("[MarketBoard] 시세 한 바퀴 전부 실패(요청 " + std::to_string(request_urls_.size()) +
                 "개) — 직전 판을 그대로 둔다");
        return false;
    }

    board->received_at     = std::time(nullptr);
    board->request_count   = request_urls_.size();
    board->failed_requests = failed;
    log_before_open(*board);
    const auto took_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    if (failed > 0)
    {
        LOG_WARN("[MarketBoard] 시세 요청 " + std::to_string(failed) + "/" + std::to_string(request_urls_.size()) +
                 "개 실패 — 그 묶음 종목은 이번 판에서 빠진다(" + std::to_string(board->quotes.size()) + "종목, " +
                 std::to_string(took_ms) + "ms)");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(board);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    board_arrived_.notify_all();
    return true;
}

// 09:00 전 판의 거래대금 상태를 분에 한 줄 남긴다. 09:00 첫 재스캔이 이 판으로 거래대금 상위 축을 뽑으므로,
//  여기서 0이면 그 축이 비어 시작한다. 문구는 scripts/check_runtime_health.py "정규장 전 거래대금 축" 행이 읽는다.
void MarketBoard::log_before_open(const BoardSnapshot& board)
{
    const std::int64_t minute        = static_cast<std::int64_t>(board.received_at) / kSecondsPerMinute;
    const int          minute_of_day = kst::sec_of_day(board.received_at) / static_cast<int>(kSecondsPerMinute);

    if (minute_of_day >= kst::kKrMarketOpenMinute || minute == last_before_open_logged_)
    {
        return;
    }

    last_before_open_logged_ = minute;
    size_t valued    = 0;
    size_t premarket = 0;

    for (const BoardQuote& quote : board.quotes)
    {
        valued += quote.value > 0.0 ? 1 : 0;
        premarket += quote.premarket ? 1 : 0;
    }

    LOG_INFO("[MarketBoard] 정규장 전 판 - 거래대금 있는 종목(" + std::to_string(valued) + "/" +
             std::to_string(board.quotes.size()) + ") 프리마켓 값(" + std::to_string(premarket) + ")");
}

void MarketBoard::rerank()
{
    const std::shared_ptr<const BoardSnapshot> board = snapshot();

    if (!board || !listing_)
    {
        return;
    }

    // 장이 막 열려 누적 거래대금이 비어 가는 동안(정규장 시작 직후)에는 거래대금 축이 몇 종목만으로 정해진다.
    //  옛 파이썬 피드도 이때는 직전 값을 썼다 — 여기서는 직전 재랭킹을 그대로 둔다.
    if (!turnover_filled(*board))
    {
        LOG_INFO("[MarketBoard] 재랭킹 보류 — 거래대금이 잡힌 종목이 절반 미만이라 직전 유니버스를 유지한다");
        return;
    }

    auto ranked       = std::make_shared<RankedUniverse>();
    ranked->ranked_at = board->received_at;
    ranked->stocks    = rank_universe(*listing_, *board, config_.n_market_value, config_.n_turnover, config_.min_turnover);
    ranked->listing   = listing_; // 포인터만 나눠 쥔다

    if (ranked->stocks.empty())
    {
        LOG_WARN("[MarketBoard] 재랭킹 결과 0종목 — 직전 유니버스를 유지한다");
        return;
    }

    size_t kospi = 0;

    for (const RankedStock& stock : ranked->stocks)
    {
        kospi += stock.market == "KOSPI" ? 1 : 0;
    }

    LOG_INFO("[MarketBoard] 재랭킹 " + std::to_string(ranked->stocks.size()) + "종목(KOSPI=" + std::to_string(kospi) +
             " KOSDAQ=" + std::to_string(ranked->stocks.size() - kospi) + ", 시세 " +
             std::to_string(board->quotes.size()) + "종목 기준)");

    if (!config_.universe_out.empty())
    {
        const std::string label = "naver-live " + kst::hhmmss(ranked->ranked_at).substr(0, 4);

        if (!file_io::write_atomically(config_.universe_out, universe_file_text(*ranked, label)))
        {
            LOG_WARN("[MarketBoard] 유니버스 파일 쓰기 실패(" + config_.universe_out + ")");
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ranked_ = std::move(ranked);
}

void MarketBoard::save_minute(const BoardSnapshot& board)
{
    // 파일은 분마다 열고 닫는다 — 날짜가 바뀌면 이름이 바뀌고, 재기동해도 같은 날 파일에 이어 붙는다.
    //  실패해도 예외를 밖으로 내지 않는다: 시세판 스레드가 죽으면 스캐너가 판을 못 받는다.
    try
    {
        const std::filesystem::path directory = utf8::path_from_utf8(config_.minute_directory);
        const std::filesystem::path file      = directory / board_minute_file_name(board.received_at);
        std::error_code             error;
        std::filesystem::create_directories(directory, error);
        const bool is_new = !std::filesystem::exists(file, error) || std::filesystem::file_size(file, error) == 0;
        std::ofstream out(file, std::ios::binary | std::ios::app);

        if (is_new)
        {
            out << board_minute_header();
        }

        out << board_minute_rows(board);
        out.flush();

        if (!out)
        {
            LOG_WARN("[MarketBoard] 1분 표본 쓰기 실패 - 파일(" + config_.minute_directory + "/" +
                     board_minute_file_name(board.received_at) + ")");
        }
    }
    catch (const std::exception& error)
    {
        LOG_WARN(std::string("[MarketBoard] 1분 표본 쓰기 예외 - 내용(") + error.what() + ")");
    }
}

} // namespace universe
