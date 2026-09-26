// 전 종목 시세판 구현 — 목록 받기·시세 한 바퀴·재랭킹·파일 쓰기. 왜 있는지는 universe/MarketBoard.h 머리말.
#include "universe/MarketBoard.h"

#include "api/HttpGet.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// 웹페이지가 보내는 것과 같은 헤더. 없으면 폴링 주소가 빈 본문으로 200을 준다.
const std::vector<std::string>& naver_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0", "Referer: https://finance.naver.com/"};
    return headers;
}

// Raw 필드는 "5504265000000"처럼 숫자만 든 문자열이다. 숫자로 오는 경우도 받아 둔다. 못 읽으면 0.
double raw_number(const nlohmann::json& row, const char* key)
{
    const auto found = row.find(key);

    if (found == row.end())
    {
        return 0.0;
    }

    if (found->is_number())
    {
        return found->get<double>();
    }

    if (!found->is_string())
    {
        return 0.0;
    }

    const std::string& text = found->get_ref<const std::string&>();
    char*              end  = nullptr;
    const double       value = std::strtod(text.c_str(), &end);
    return (end != text.c_str() && std::isfinite(value)) ? value : 0.0;
}

std::string text_field(const nlohmann::json& row, const char* key)
{
    const auto found = row.find(key);
    return (found != row.end() && found->is_string()) ? found->get<std::string>() : std::string();
}

// 임시 파일에 다 쓴 뒤 바꿔 넣는다 — 읽는 쪽이 반쯤 쓰인 파일을 보지 않게. 임시 이름에 시각을 붙이는 것은
//  모의·실계좌 엔진 둘이 같은 경로에 쓸 때 서로의 임시 파일을 덮지 않게 하려는 것이다.
bool write_atomically(const std::string& path, const std::string& text)
{
    const std::string temporary =
        path + ".tmp" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

        if (!file)
        {
            return false;
        }

        file.write(text.data(), static_cast<std::streamsize>(text.size()));

        if (!file)
        {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);

    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }

    return true;
}

} // namespace

std::vector<ListedStock> parse_listing_page(std::string_view body, const std::string& market, int& total_count)
{
    std::vector<ListedStock> listed;
    total_count = -1;
    const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);

    if (!document.is_object())
    {
        return listed;
    }

    const auto total = document.find("totalCount");

    if (total != document.end() && total->is_number())
    {
        total_count = total->get<int>();
    }

    const auto stocks = document.find("stocks");

    if (stocks == document.end() || !stocks->is_array())
    {
        return listed;
    }

    listed.reserve(stocks->size());

    for (const nlohmann::json& row : *stocks)
    {
        // ETF·ETN은 여기서 뺀다 — 옛 파이썬 피드가 data.go.kr 주식 시세(개별주만)를 목록으로 쓴 것과 같은 범위다.
        if (!row.is_object() || text_field(row, "stockEndType") != "stock")
        {
            continue;
        }

        std::string code = text_field(row, "itemCode");

        if (code.size() != 6)
        {
            continue;
        }

        listed.push_back({std::move(code), text_field(row, "stockName"), market});
    }

    return listed;
}

std::vector<BoardQuote> parse_polling(std::string_view body)
{
    std::vector<BoardQuote> quotes;
    const nlohmann::json    document = nlohmann::json::parse(body, nullptr, false);

    if (!document.is_object())
    {
        return quotes;
    }

    const auto rows = document.find("datas");

    if (rows == document.end() || !rows->is_array())
    {
        return quotes;
    }

    quotes.reserve(rows->size());

    for (const nlohmann::json& row : *rows)
    {
        if (!row.is_object())
        {
            continue;
        }

        // 맨 위 칸만 읽는다. 같은 이름의 Raw 필드가 시간외(overMarketPriceInfo)·통합(integratedPriceInfo)
        //  안에도 있어 본문을 문자열로 훑으면 그쪽 값을 잡는다.
        BoardQuote quote;
        quote.code  = text_field(row, "itemCode");
        quote.price = raw_number(row, "closePriceRaw");

        if (quote.code.empty() || quote.price <= 0.0)
        {
            continue;
        }

        quote.name         = text_field(row, "stockName");
        quote.volume       = raw_number(row, "accumulatedTradingVolumeRaw");
        quote.value        = raw_number(row, "accumulatedTradingValueRaw");
        quote.market_value = raw_number(row, "marketValueFullRaw");
        quotes.push_back(std::move(quote));
    }

    return quotes;
}

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

        take_top(n_market_value, [](const BoardQuote& quote) { return quote.market_value; });
        take_top(n_turnover, [](const BoardQuote& quote) { return quote.value; });
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

    for (const ListedStock& listed : ranked.listing)
    {
        market_map[listed.code] = listed.market;

        if (!listed.name.empty())
        {
            name_map[listed.code] = listed.name;
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

MarketBoard& MarketBoard::instance()
{
    static MarketBoard board;
    return board;
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
        config_.codes_per_call = 900;
    }

    config_.period_sec = std::max(1, config_.period_sec);
    config_.rerank_sec = std::max(config_.period_sec, config_.rerank_sec);
    worker_            = std::thread([this] { run(); });
    LOG_INFO("[MarketBoard] 시작 — 시세 " + std::to_string(config_.period_sec) + "초·재랭킹 " +
             std::to_string(config_.rerank_sec) + "초 주기, 시장별 시총 top" + std::to_string(config_.n_market_value) +
             " ∪ 거래대금 top" + std::to_string(config_.n_turnover) + (config_.universe_out.empty() ? "" : " → " + config_.universe_out));
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
    return published_listing_;
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

        if ((listing_date_ != today || listing_.empty()) && refresh_listing())
        {
            listing_date_ = today;
        }

        if (!listing_.empty() && sweep())
        {
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

    listing_ = std::move(listing);
    request_urls_.clear();

    for (size_t begin = 0; begin < listing_.size(); begin += config_.codes_per_call)
    {
        const size_t end = std::min(begin + config_.codes_per_call, listing_.size());
        std::string  url(kPollingEndpoint);

        for (size_t index = begin; index < end; ++index)
        {
            if (index > begin)
            {
                url.push_back(',');
            }

            url.append(listing_[index].code);
        }

        request_urls_.push_back(std::move(url));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        published_listing_ = std::make_shared<const std::vector<ListedStock>>(listing_);
    }

    LOG_INFO("[MarketBoard] 종목 목록 " + std::to_string(listing_.size()) + "종목(개별주) — 시세 요청 " +
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
        bodies.push_back(std::async(std::launch::async, [&url] { return http::get(url, naver_headers()); }));
    }

    auto   board  = std::make_shared<BoardSnapshot>();
    size_t failed = 0;
    board->quotes.reserve(listing_.size());

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

    board->received_at = std::time(nullptr);
    const auto took_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    if (failed > 0)
    {
        LOG_WARN("[MarketBoard] 시세 요청 " + std::to_string(failed) + "/" + std::to_string(request_urls_.size()) +
                 "개 실패 — 그 묶음 종목은 이번 판에서 빠진다(" + std::to_string(board->quotes.size()) + "종목, " +
                 std::to_string(took_ms) + "ms)");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(board);
    return true;
}

void MarketBoard::rerank()
{
    const std::shared_ptr<const BoardSnapshot> board = snapshot();

    if (!board)
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
    ranked->stocks    = rank_universe(listing_, *board, config_.n_market_value, config_.n_turnover, config_.min_turnover);
    ranked->listing   = listing_;

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

        if (!write_atomically(config_.universe_out, universe_file_text(*ranked, label)))
        {
            LOG_WARN("[MarketBoard] 유니버스 파일 쓰기 실패(" + config_.universe_out + ")");
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ranked_ = std::move(ranked);
}

} // namespace universe
