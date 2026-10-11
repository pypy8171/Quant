// 국면 판정 피드 스레드 — 지표 받기(네이버·Yahoo·FRED HTTP)·사이클·파일 쓰기. 왜 있는지는 regime/RegimeFeed.h 머리말.
//  응답 읽기는 RegimeFeedParse.cpp, 점수·regime.json 문서는 RegimeFeedScore.cpp, 둘 사이 공용 표는 detail/RegimeTables.h.
//  스레드: start·stop은 엔진 기동·종료 스레드, 나머지는 피드 스레드 하나(run_once는 도구가 직접 부른다). [why D-147]
#include "regime/RegimeFeed.h"

#include "detail/RegimeTables.h"

#include "api/HttpGet.h"
#include "core/KstTime.h"
#include "utils/AtomicFile.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace regime_feed
{

using detail::InfoSymbol;
using detail::kInfoSymbols;
using detail::kVotingSymbols;
using detail::VotingSymbol;

namespace
{

constexpr int kInfoRefreshSec     = 1800; // FRED는 하루 한 번 바뀌어 30분에 한 번만 받는다
constexpr int kFredLookbackDays   = 15;   // 휴일이 이어져도 값 두 개가 남는 폭
constexpr int kSecondsPerDay      = 86400;
constexpr int kStopPollMs         = 200;  // 멈추라는 신호를 기다리는 한 번의 잠
constexpr int kOwnerGraceSec      = 30;   // 다른 엔진의 사이클이 조금 늦어도 넘겨받지 않는 여유

constexpr std::string_view kNaverIndexUrl = "https://polling.finance.naver.com/api/realtime/domestic/index/KOSPI,KOSDAQ";

const std::vector<std::string>& naver_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0", "Referer: https://finance.naver.com/"};
    return headers;
}

// 기본 UA는 Yahoo가 429·403을 준다.
const std::vector<std::string>& browser_headers()
{
    static const std::vector<std::string> headers{"User-Agent: Mozilla/5.0"};
    return headers;
}

// FRED 앞단은 헤더 조합으로 요청을 거른다 — 걸리면 응답 없이 연결을 붙잡는다(12002 시간 초과). WinHTTP 기본(UA·
//  Connection만)과 브라우저 UA는 걸리고, 파이썬 requests가 보내는 조합(Accept·Accept-Encoding)은 통과한다(09-26 실측).
//  CSV가 작아 압축하지 않고 보낸다.
const std::vector<std::string>& fred_headers()
{
    static const std::vector<std::string> headers{"Accept: */*", "Accept-Encoding: gzip, deflate"};
    return headers;
}

// 차트 주소에 넣는 기호(^·=)는 퍼센트로 바꾼다.
std::string encode_symbol(std::string_view symbol)
{
    std::string encoded;

    for (const char character : symbol)
    {
        if (character == '^')
        {
            encoded += "%5E";
        }
        else if (character == '=')
        {
            encoded += "%3D";
        }
        else
        {
            encoded += character;
        }
    }

    return encoded;
}

void append_line(const std::string& path, const std::string& line)
{
    std::error_code error;
    const auto      parent = std::filesystem::path(path).parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
    }

    std::ofstream file(path, std::ios::binary | std::ios::app);

    if (file)
    {
        file << line << '\n';
    }
}

std::string read_whole(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_with_parent(const std::string& path, const std::string& text)
{
    std::error_code error;
    const auto      parent = std::filesystem::path(path).parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
    }

    return file_io::write_atomically(path, text);
}

} // namespace

// ── 스레드 ──

RegimeFeed::~RegimeFeed()
{
    stop();
}

void RegimeFeed::start(const FeedConfig& config)
{
    if (config.out_path.empty() || running_.exchange(true))
    {
        return;
    }

    config_ = config;
    worker_ = std::thread([this]
    {
        run();
    });
}

void RegimeFeed::stop()
{
    running_.store(false, std::memory_order_release);

    if (worker_.joinable())
    {
        worker_.join();
    }
}

void RegimeFeed::run_once(const FeedConfig& config)
{
    config_ = config;
    cycle();
}

void RegimeFeed::run()
{
    thread_name::set_current("RegimeFeed");
    LOG_INFO("[국면] 국면 판정 피드 시작 — 출력 " + config_.out_path + ", 주기 " + std::to_string(config_.interval_sec) +
             "초, 정지 " + std::to_string(config_.thresholds.halt_score) + "·청산 " +
             std::to_string(config_.thresholds.liquidate_score));

    while (running_.load(std::memory_order_acquire))
    {
        if (!another_writer_owns_file())
        {
            try
            {
                cycle();
            }
            catch (const std::exception& exception)
            {
                LOG_WARN(std::string("[국면] 사이클 실패 — 다음 주기에 다시: ") + exception.what());
            }
        }

        // 멈추라는 신호를 곧 받게 0.2초씩 나눠 잔다.
        const auto wake_at = std::chrono::steady_clock::now() + std::chrono::seconds(config_.interval_sec);

        while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < wake_at)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStopPollMs));
        }
    }
}

// 같은 출력 파일을 다른 엔진(모의·실계좌)이나 옛 파이썬 피드가 쓰고 있으면 쉰다. 파일이 주기+여유보다 오래
//  안 바뀌면 그쪽이 멈춘 것이라 넘겨받는다.
bool RegimeFeed::another_writer_owns_file()
{
    std::error_code error;
    const auto      written = std::filesystem::last_write_time(config_.out_path, error);

    if (error || (last_written_ && *last_written_ == written))
    {
        return false;
    }

    const auto age = std::filesystem::file_time_type::clock::now() - written;

    if (age < std::chrono::seconds(config_.interval_sec + kOwnerGraceSec))
    {
        if (!resting_logged_)
        {
            LOG_INFO("[국면] " + config_.out_path + "를 다른 프로세스가 쓰고 있어 이 엔진은 쉰다(멈추면 넘겨받는다)");
            resting_logged_ = true;
        }

        return true;
    }

    if (resting_logged_)
    {
        LOG_INFO("[국면] " + config_.out_path + "가 " + std::to_string(config_.interval_sec + kOwnerGraceSec) +
                 "초 넘게 안 바뀌어 이 엔진이 넘겨받는다");
        resting_logged_ = false;
    }

    return false;
}

Changes RegimeFeed::fetch_changes()
{
    Changes                             changes;
    const std::time_t                   now     = std::time(nullptr);
    const std::map<std::string, Change> indexes =
        parse_naver_index(http::get(std::string(kNaverIndexUrl), naver_headers()), now);

    for (const VotingSymbol& symbol : kVotingSymbols)
    {
        const std::string key(symbol.key);

        if (!symbol.naver_code.empty())
        {
            const auto found = indexes.find(std::string(symbol.naver_code));

            if (found != indexes.end())
            {
                changes[key] = found->second;
                continue;
            }

            LOG_WARN("[국면] " + key + " 네이버 지수 실패 — Yahoo로 받는다");
        }

        const std::string body = http::get("https://query1.finance.yahoo.com/v8/finance/chart/" +
                                               encode_symbol(symbol.yahoo) + "?range=1d&interval=5m",
                                           browser_headers());
        Change change;

        if (body.empty())
        {
            change.source = "yahoo";
            change.error  = "yahoo:no_response";
        }
        else
        {
            change = parse_yahoo_chart(body, now);
        }

        // 미국 지표는 새 세션이 열리기 전까지 직전 세션 등락으로 표를 낸다. Yahoo는 미국 자정(한국 13~14시)에
        //  거래 시간대를 다음 날로 넘겨, 그때부터 개장까지 0으로 빠졌다(10-01 14:00 VIX·TNX10 0.000%, 그 전에는
        //  같은 직전 세션 등락 1.87%·0.72%로 표를 냈다). 코스피·코스닥의 개장 전 0은 그대로 둔다(09-15).
        if (change.premarket && symbol.naver_code.empty() && symbol.cash_reference.empty())
        {
            const std::optional<double> last_session = parse_last_session_percent(
                http::get("https://query1.finance.yahoo.com/v8/finance/chart/" + encode_symbol(symbol.yahoo) +
                              "?range=10d&interval=1d",
                          browser_headers()),
                now);

            if (last_session)
            {
                change.percent          = *last_session;
                change.previous_percent = last_session;
            }
        }

        // 현물 직전 세션 + 선물 정산 뒤 변동. 현물을 못 받으면 선물 변동만으로 표를 낸다.
        if (change.percent && !symbol.cash_reference.empty())
        {
            const std::optional<double> cash_percent = parse_last_session_percent(
                http::get("https://query1.finance.yahoo.com/v8/finance/chart/" + encode_symbol(symbol.cash_reference) +
                              "?range=10d&interval=1d",
                          browser_headers()),
                now);
            change.since_settle_percent = change.percent;
            change.previous_percent     = cash_percent;

            if (cash_percent)
            {
                change.percent = *cash_percent + *change.since_settle_percent;
            }
            else
            {
                LOG_WARN("[국면] " + key + " 현물 " + std::string(symbol.cash_reference) +
                         " 직전 세션을 못 받아 선물 변동만 센다");
            }
        }

        if (!change.percent)
        {
            LOG_WARN("[국면] " + key + " 받기 실패(" + change.error + ") — 이번 사이클 표결에서 빠진다");
        }

        changes[key] = change;
    }

    return changes;
}

void RegimeFeed::fetch_info_changes(Changes& changes)
{
    const std::time_t now = std::time(nullptr);

    if (now - info_fetched_at_ >= kInfoRefreshSec)
    {
        const std::string since = kst::datetime(now - static_cast<std::time_t>(kFredLookbackDays) * kSecondsPerDay).substr(0, 10);
        bool              all_ok = true;

        for (const InfoSymbol& symbol : kInfoSymbols)
        {
            const std::string body = http::get("https://fred.stlouisfed.org/graph/fredgraph.csv?id=" +
                                                   std::string(symbol.fred_id) + "&cosd=" + since,
                                               fred_headers());
            Change change = body.empty() ? Change{} : parse_fred_csv(body);

            if (body.empty())
            {
                change.source = "fred";
                change.error  = "fred:no_response";
            }

            all_ok = all_ok && change.percent.has_value();
            info_cache_[std::string(symbol.key)] = change;
        }

        if (all_ok)
        {
            info_fetched_at_ = now; // 하나라도 실패하면 다음 사이클에 다시 받는다
        }
    }

    for (const auto& [key, change] : info_cache_)
    {
        changes[key] = change;
    }
}

void RegimeFeed::cycle()
{
    Changes changes = fetch_changes();
    fetch_info_changes(changes);

    const std::time_t now         = std::time(nullptr);
    bool              should_save = false;
    const OpenReference open_reference =
        choose_open_reference(read_whole(config_.open_reference_path), changes, now, should_save);

    if (should_save && !config_.open_reference_path.empty())
    {
        nlohmann::ordered_json document;
        document["date"]   = open_reference.date;
        document["ts"]     = open_reference.timestamp;
        document["prices"] = open_reference.prices;

        if (!write_with_parent(config_.open_reference_path, document.dump(1)))
        {
            LOG_WARN("[국면] 장초 기준점 저장 실패: " + config_.open_reference_path);
        }
    }

    const nlohmann::ordered_json regime = build_regime(changes, open_reference, config_.thresholds, now);

    if (!write_with_parent(config_.out_path, regime.dump(2)))
    {
        LOG_WARN("[국면] " + config_.out_path + " 쓰기 실패 — 다음 주기에 다시");
        return;
    }

    std::error_code error;
    const auto      written = std::filesystem::last_write_time(config_.out_path, error);

    if (!error)
    {
        last_written_ = written;
    }

    if (!config_.history_path.empty())
    {
        append_line(config_.history_path, history_line(regime, config_.thresholds));
    }

    LOG_INFO(summary_line(regime));
}

} // namespace regime_feed
