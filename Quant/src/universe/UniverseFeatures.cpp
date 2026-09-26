// 정배열 프리필터와 지표 산출 — 후보마다 확정 일봉 요약(SMA·ATR·고가·거래량)을 캐시에서 꺼내거나 받아 현재가를
//  접어 넣고, 정배열·이격·거래대금 조건을 통과한 종목의 점수 재료(Features)를 만든다. 일봉 요약 캐시와 그 디스크
//  사본도 여기 있다. 스캔 스레드 전용. [why D-005·D-028]

#include "detail/Pipeline.h"
#include "universe/MarketBoard.h"
#include "utils/ThreadName.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace universe::detail
{
void DailyLookupCache::load_today(const std::string& date_yyyymmdd, symbol::SymbolTable& symbols)
{
    {
        // 비교와 기록을 한 락 안에서 한다 — 같은 날짜로 둘이 동시에 들어와도 파일은 한 번만 읽는다.
        std::lock_guard<std::mutex> lock(mutex_);

        if (loaded_ == date_yyyymmdd)
        {
            return;
        }

        loaded_ = date_yyyymmdd;
    }

    std::ifstream file(cache_path(date_yyyymmdd));

    if (!file)
    {
        return;
    }

    int count = 0;

    try
    {
        nlohmann::json document;
        file >> document;

        if (!document.is_object())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        reserve_locked(symbols.capacity());

        for (auto iterator = document.begin(); iterator != document.end(); ++iterator)
        {
            // 9칸이 지금 형식(D-141에서 60일선 칸을, 재조회 경로를 걷으며 조회 시각 칸을, 읽는 곳이 없던 저항·거래량
            //  네 칸을 뺐다). 다른 칸수는 옛 파일 — 건너뛰면 캐시 미스와 같아서 그 종목만 다시 받는다.
            if (!iterator.value().is_array() || iterator.value().size() != 9)
            {
                continue;
            }

            const symbol::SymbolId symbol = symbols.intern(iterator.key()); // 파일의 문자열 티커 — 여기서 id가 된다

            if (symbol == symbol::kNone)
            {
                continue;
            }

            const auto& value = iterator.value();
            DailyLookup daily_lookup;
            daily_lookup.date_yyyymmdd     = date_yyyymmdd;
            daily_lookup.bars    = value[0].get<int>();
            daily_lookup.average_5      = value[1].get<double>();
            daily_lookup.average_10     = value[2].get<double>();
            daily_lookup.average_20     = value[3].get<double>();
            daily_lookup.close   = value[4].get<double>();
            daily_lookup.r5      = value[5].get<double>();
            daily_lookup.r10     = value[6].get<double>();
            daily_lookup.r20     = value[7].get<double>();
            daily_lookup.atr_percent = value[8].get<double>();

            by_symbol_[symbol] = std::move(daily_lookup);
            ++count;
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[Main] 일봉 캐시 파일 읽기 실패: ") + exception.what());
        return;
    }

    if (count > 0)
    {
        LOG_INFO("[Main] 일봉 캐시 " + std::to_string(count) + "종목을 파일에서 복원했다(" + date_yyyymmdd + ")");
    }
}

void DailyLookupCache::save_today(const std::string& date_yyyymmdd, const symbol::SymbolTable& symbols) const
{
    // [wire] 값 순서: bars, average_5, average_10, average_20, close, r5, r10, r20, atr_percent
    //  — 9칸 고정(읽는 쪽이 칸수로 형식을 가린다)
    nlohmann::json document = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (symbol::SymbolId symbol = 1; symbol < by_symbol_.size(); ++symbol)
        {
            const DailyLookup& daily_lookup = by_symbol_[symbol];

            if (daily_lookup.date_yyyymmdd != date_yyyymmdd)
            {
                continue;
            }

            document[symbols.name(symbol).string()] = nlohmann::json::array({daily_lookup.bars, daily_lookup.average_5, daily_lookup.average_10, daily_lookup.average_20,
                                                 daily_lookup.close, daily_lookup.r5, daily_lookup.r10, daily_lookup.r20,
                                                 daily_lookup.atr_percent});
        }
    }

    const std::string path = cache_path(date_yyyymmdd);
    const std::string temporary  = path + ".tmp";

    try
    {
        {
            std::ofstream file(temporary, std::ios::trunc);

            if (!file)
            {
                return;
            }

            file << document.dump();
        }

        std::error_code error_code;
        std::filesystem::rename(temporary, path, error_code);

        if (error_code)
        {
            std::filesystem::remove(temporary, error_code);
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[Main] 일봉 캐시 파일 쓰기 실패: ") + exception.what());
    }
}

bool DailyLookupCache::get(symbol::SymbolId symbol, const std::string& date_yyyymmdd, DailyLookup& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (symbol >= by_symbol_.size() || by_symbol_[symbol].date_yyyymmdd != date_yyyymmdd)
    {
        return false;
    }

    out = by_symbol_[symbol];   // 복사가 맞다 — 락 밖에서 쓰는 스냅샷이고 재조회가 같은 항목을 덮어쓴다
    return true;
}

void DailyLookupCache::put(symbol::SymbolId symbol, const DailyLookup& daily_lookup)
{
    std::lock_guard<std::mutex> lock(mutex_);
    reserve_locked(static_cast<size_t>(symbol) + 1);
    by_symbol_[symbol] = daily_lookup;
}

std::string DailyLookupCache::cache_path(const std::string& date_yyyymmdd)
{
    return Logger::instance().path_for("daily_lookup_" + date_yyyymmdd + ".json").string();
}

void DailyLookupCache::reserve_locked(size_t size)
{
    if (by_symbol_.size() < size)
    {
        by_symbol_.resize(size);
    }
}

DailyLookupCache g_lookup_cache;

namespace
{
constexpr int kDailyLookupSleepMs      = 150; // 실계좌 — 키 한도 초당 20건
constexpr int kDailyLookupSleepPaperMs = 600; // 모의계좌 — 키 한도 초당 2건
constexpr int kWarmBoardWaitSec        = 120; // 초, 장 전 데우기가 시세판 목록·첫 판을 기다리는 상한
constexpr int kWarmProgressEvery       = 250; // 종목, 장 전 데우기 진행 로그 간격

// 후보 하나의 일봉을 받아 SMA·롤오프·ATR로 요약한다. 20봉 미만이면 bars만 채워 돌려준다.
//  조회 간격은 호출자가 책임진다.
DailyLookup fetch_daily_lookup(KisClient& kis, const DevScanCfg& config, const std::string& ticker,
                       const std::string& date_yyyymmdd)
{
    auto daily_ohlcv = kis.get_daily_ohlcv(ticker, config.align_daily_n);
    DailyLookup daily_lookup;
    daily_lookup.date_yyyymmdd  = date_yyyymmdd;
    daily_lookup.bars = static_cast<int>(daily_ohlcv.size());

    if (daily_lookup.bars < 20)
    {
        return daily_lookup;
    }

    auto simple_moving_average = [&](int count) { double sum = 0.0; for (int index = 0; index < count; ++index) sum += daily_ohlcv[index].close; return sum / count; };
    daily_lookup.average_5 = simple_moving_average(5); daily_lookup.average_10 = simple_moving_average(10); daily_lookup.average_20 = simple_moving_average(20);
    daily_lookup.r5 = daily_ohlcv[4].close; daily_lookup.r10 = daily_ohlcv[9].close;
    daily_lookup.r20 = daily_ohlcv[19].close;
    daily_lookup.close = daily_ohlcv[0].close;
    // [formula] ATR(14) — True Range = max(고−저, |고−전일종가|, |저−전일종가|)의 14봉 평균.
    //  d[0]이 최신이므로 d[i+1]이 i의 전일. 종가로 나눠 종목 간 비교 가능한 비율로 만든다.
    double true_range_sum = 0.0;
    int    true_range_count   = 0;

    for (size_t index = 0; index + 1 < daily_ohlcv.size() && true_range_count < 14; ++index, ++true_range_count)
    {
        const double previous_close = daily_ohlcv[index + 1].close;
        const double high = daily_ohlcv[index].high, low = daily_ohlcv[index].low;
        double true_range = high - low;
        const double high_gap = std::fabs(high - previous_close), low_gap = std::fabs(low - previous_close);

        if (high_gap > true_range)
        {
            true_range = high_gap;
        }

        if (low_gap > true_range)
        {
            true_range = low_gap;
        }

        true_range_sum += true_range;
    }

    daily_lookup.atr_percent = (true_range_count > 0 && daily_lookup.close > 0.0) ? (true_range_sum / true_range_count) / daily_lookup.close : 0.0;
    return daily_lookup;
}
} // namespace

namespace
{
// 장 전 일봉 캐시 데우기. 스레드 하나가 시세 키로 시세판 목록의 종목 일봉을 받아 문자열 티커 → 요약으로 쌓아 두고,
//  스캔 스레드가 drain_daily_warm으로 캐시에 옮긴다. id로 바로 넣지 않는 것은 이 스레드가 엔진이 종목 표를
//  바꿔 끼우기(SymbolTable::adopt) 전에 돌 수 있어서다. [why D-147]
class DailyWarmer
{
public:
    ~DailyWarmer()
    {
        stop_.store(true, std::memory_order_release);

        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    void start(const KisConfig& kis_config, const DevScanCfg& config)
    {
        if (started_.exchange(true))
        {
            return;
        }

        worker_ = std::thread([this, kis_config, config] { run(kis_config, config); });
    }

    int drain(const std::string& date_yyyymmdd, symbol::SymbolTable& symbols)
    {
        std::vector<std::pair<std::string, DailyLookup>> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready.swap(ready_);
        }

        int moved = 0;

        for (const auto& [ticker, daily_lookup] : ready)
        {
            if (daily_lookup.date_yyyymmdd != date_yyyymmdd)
            {
                continue; // 자정을 넘겨 받은 것 — 다른 날 캐시에 넣지 않는다
            }

            const symbol::SymbolId symbol = symbols.intern(ticker);

            if (symbol == symbol::kNone)
            {
                continue; // 종목 표가 가득 참 — 스캔이 필요할 때 다시 받는다
            }

            g_lookup_cache.put(symbol, daily_lookup);
            ++moved;
        }

        return moved;
    }

private:
    static int now_hhmm()
    {
        return kst::hhmmss_int(std::time(nullptr)) / 100;
    }

    // 1초씩 나눠 기다린다 — 멈추라는 신호를 곧 받게.
    template <typename Ready>
    bool wait_for(int seconds, Ready ready)
    {
        for (int waited = 0; waited < seconds; ++waited)
        {
            if (stop_.load(std::memory_order_acquire))
            {
                return false;
            }

            if (ready())
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        return ready();
    }

    // 오늘 캐시 파일에 이미 있는 티커(장 전 재기동). 캐시 파일과 같은 13칸 형식만 센다.
    static std::unordered_set<std::string> cached_tickers(const std::string& date_yyyymmdd)
    {
        std::unordered_set<std::string> cached;
        std::ifstream                   file(DailyLookupCache::cache_path(date_yyyymmdd));

        if (!file)
        {
            return cached;
        }

        const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);

        if (!document.is_object())
        {
            return cached;
        }

        for (auto iterator = document.begin(); iterator != document.end(); ++iterator)
        {
            if (iterator.value().is_array() && iterator.value().size() == 13)
            {
                cached.insert(iterator.key());
            }
        }

        return cached;
    }

    void run(const KisConfig& kis_config, const DevScanCfg& config)
    {
        thread_name::set_current("DailyWarm");
        const int until = config.daily_warm_until_hhmm;

        if (now_hhmm() >= until)
        {
            LOG_INFO("[DailyWarm] 마감 " + std::to_string(until) + " 뒤에 떴다 — 장 전 일봉 캐시 데우기를 건너뛴다");
            return;
        }

        // 시세판이 목록(요청 44건)과 첫 판을 받는 데 몇 초 걸린다.
        MarketBoard& board = MarketBoard::instance();

        if (!wait_for(kWarmBoardWaitSec, [&board] { return board.listing() && board.snapshot(); }))
        {
            LOG_WARN("[DailyWarm] 시세판 목록·시세를 2분 안에 못 받아 장 전 일봉 캐시 데우기를 건너뛴다");
            return;
        }

        const std::shared_ptr<const std::vector<ListedStock>> listing  = board.listing();
        const std::shared_ptr<const BoardSnapshot>            snapshot = board.snapshot();
        const std::string                     date_yyyymmdd = kst::date_yyyymmdd(std::time(nullptr));
        const std::unordered_set<std::string> cached        = cached_tickers(date_yyyymmdd);

        // 대상: 직전 세션 값이 가격·거래대금 조건을 넘는 종목을 거래대금이 큰 순으로. 스캔 후보가 될 수 없는 종목에는
        //  조회를 쓰지 않고, 마감에 걸려 멈춰도 후보가 될 가능성이 큰 종목부터 받혀 있게 한다.
        std::unordered_map<std::string_view, const BoardQuote*> quote_of;
        quote_of.reserve(snapshot->quotes.size() * 2);

        for (const BoardQuote& quote : snapshot->quotes)
        {
            quote_of.emplace(quote.code, &quote);
        }

        std::vector<std::pair<double, const std::string*>> targets;

        for (const ListedStock& listed : *listing)
        {
            const auto found = quote_of.find(listed.code);

            if (found == quote_of.end() || cached.count(listed.code) > 0)
            {
                continue;
            }

            const BoardQuote& quote = *found->second;

            if (quote.price < config.min_price || (config.max_price > 0.0 && quote.price > config.max_price) ||
                (config.min_turnover > 0.0 && quote.value < config.min_turnover))
            {
                continue;
            }

            targets.emplace_back(quote.value, &listed.code);
        }

        std::sort(targets.begin(), targets.end(),
                  [](const auto& left, const auto& right) { return left.first > right.first; });

        KisClient kis(kis_config);

        if (!kis.authenticate())
        {
            LOG_WARN("[DailyWarm] 시세 키 인증 실패 — 장 전 일봉 캐시 데우기를 건너뛴다");
            return;
        }

        LOG_INFO("[DailyWarm] 장 전 일봉 캐시 데우기 시작 — 대상 " + std::to_string(targets.size()) + "종목(이미 받음 " +
                 std::to_string(cached.size()) + "), 마감 " + std::to_string(until));
        const auto started    = std::chrono::steady_clock::now();
        const int  spacing_ms = kis.is_paper() ? kDailyLookupSleepPaperMs : kDailyLookupSleepMs;
        int        fetched    = 0;
        bool       deadline   = false;

        for (const auto& [value, ticker] : targets)
        {
            if (stop_.load(std::memory_order_acquire))
            {
                return;
            }

            if (now_hhmm() >= until)
            {
                deadline = true;
                break;
            }

            if (fetched > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(spacing_ms));
            }

            DailyLookup daily_lookup = fetch_daily_lookup(kis, config, *ticker, date_yyyymmdd);
            ++fetched;

            // 0봉은 대개 조회 실패다. 캐시에 넣으면 그날 내내 데이터부족으로 빠지므로 스캔이 다시 받게 둔다.
            if (daily_lookup.bars > 0)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ready_.emplace_back(*ticker, std::move(daily_lookup));
            }

            if (fetched % kWarmProgressEvery == 0)
            {
                LOG_INFO("[DailyWarm] " + std::to_string(fetched) + "/" + std::to_string(targets.size()) + "종목 받음");
            }
        }

        const long long took_sec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        LOG_INFO("[DailyWarm] 장 전 일봉 캐시 데우기 끝 — " + std::to_string(fetched) + "/" +
                 std::to_string(targets.size()) + "종목, " + std::to_string(took_sec) + "초" +
                 (deadline ? " (마감에 멈춤 — 남은 종목은 스캔이 필요할 때 받는다)" : ""));
    }

    std::atomic<bool>                                started_{false};
    std::atomic<bool>                                stop_{false};
    std::thread                                      worker_;
    std::mutex                                       mutex_; // ready_만 지킨다
    std::vector<std::pair<std::string, DailyLookup>> ready_;
};

// g_lookup_cache보다 뒤에 정의한다 — 같은 번역 단위 안에서는 거꾸로 소멸하므로 스레드가 캐시보다 먼저 멈춘다.
DailyWarmer g_daily_warmer;
} // namespace

int drain_daily_warm(const std::string& date_yyyymmdd, symbol::SymbolTable& symbols)
{
    return g_daily_warmer.drain(date_yyyymmdd, symbols);
}

std::vector<Features> lookup_and_filter(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                                   const CandidateSet& candidates, const QuoteTable& quotes,
                                   const MarketGate& gate, LookupStats& statistics, symbol::SymbolTable& symbols)
{
    std::vector<Features> passed;

    for (const symbol::SymbolId symbol : candidates.symbols)
    {
        if (!gate.allows(candidates.market_of(symbol)))
        {
            continue;   // 시장 risk_off 게이트. 일봉 조회 비용도 여기서 아낀다
        }

        // 유동성 하한 — 거래대금이 받침하지 못하는 종목은 체결이 안 되거나 슬리피지로 손익을
        //  먹는다. 거래대금을 모르는 후보는 통과시킨다(기존 동작 유지).
        if (config.min_turnover > 0.0)
        {
            if (quotes[symbol].value > 0.0 && quotes[symbol].value < config.min_turnover)
            {
                ++statistics.illiquid;
                continue;
            }
        }

        // 스코어링 시엔 max_register 대신 align_lookup_max까지 넓게 모아 랭킹한다(더 나은 상위 N).
        if (config.score_top_n <= 0 && static_cast<int>(passed.size()) >= config.max_register)
        {
            break;
        }

        DailyLookup daily_lookup;
        const bool  cached = g_lookup_cache.get(symbol, date_yyyymmdd, daily_lookup);

        if (!cached)
        {
            if (statistics.fetched >= config.align_lookup_max)
            {
                // 예산은 REST에만 건다. 루프를 끊으면 뒤쪽 후보의 캐시 히트까지 버려
                //  후보 집합을 넓힐수록 뒤쪽이 영구히 미검사로 남는다.
                ++statistics.budget_skipped;
                continue;
            }

            // 하루 첫 스캔은 수백 건이 연속으로 나간다. 60ms에서는 초당한도(EGW00201) 거부가
            //  09-08 하루 149건 났고 CANCEL뿐 아니라 NEW에도 걸려 진입이 4초씩 밀렸다.
            //  같은 날 주문 RTT p50이 09시 381ms에서 10시 1870ms로 단조증가한 것도 계좌 단위
            //  REST 누적 부하로 보여 150ms로 올린다. 캐시 히트 경로에는 걸리지 않는다.
            // 모의계좌는 키 한도가 초당 2건이라 150ms(초당 6.7건)로는 버킷이 계속 밀린다 —
            //  09-22에 초당 한도 재시도 37건이 났다. 모의면 600ms(초당 1.7건)로 벌려 한도 안쪽에서 돈다.
            //  간격 상수는 장 전 데우기와 같이 쓰려고 파일 위쪽에 둔다.

            if (statistics.fetched > 0)
            {
                const int sleep_ms = kis.is_paper() ? kDailyLookupSleepPaperMs : kDailyLookupSleepMs;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                statistics.sleep_ms += sleep_ms;
            }

            const auto fetch_start = std::chrono::steady_clock::now();
            const std::uint64_t wait_before_ns = KisClient::rate_wait_ns_this_thread();
            daily_lookup = fetch_daily_lookup(kis, config, symbols.name(symbol).string(), date_yyyymmdd); // REST는 문자열
            statistics.rest_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - fetch_start).count();
            statistics.wait_ms += static_cast<long long>(
                (KisClient::rate_wait_ns_this_thread() - wait_before_ns) / 1000000ULL);
            ++statistics.fetched;
            g_lookup_cache.put(symbol, daily_lookup);
        }
        else
        {
            ++statistics.cache_hit;
        }

        ++statistics.looked_up;

        if (daily_lookup.bars < 20)
        {
            ++statistics.short_bars;
            continue;
        }

        // 오늘 가격을 최신 봉으로 접어 넣어 SMA를 다시 계산한다. 일봉 캐시는 include_today=false라
        //  전일치에서 멈춰 있고, 그대로 쓰면 정배열 판정이 하루 종일 얼어붙어 재스캔이 같은 종목만
        //  돌려준다. 시세 표의 현재가를 쓰므로 REST 추가 없이 매 재스캔마다 다시 판정한다.
        double price    = daily_lookup.close;
        double turnover = 0.0;

        if (quotes[symbol].price > 0.0)
        {
            price    = quotes[symbol].price;
            turnover = quotes[symbol].value;
        }

        quant::moving_average::SimpleMovingAverages previous;
        previous.average_5 = daily_lookup.average_5; previous.average_10 = daily_lookup.average_10; previous.average_20 = daily_lookup.average_20;
        const quant::moving_average::SimpleMovingAverages moving_average =
            quant::moving_average::fold_today(previous, daily_lookup.r5, daily_lookup.r10, daily_lookup.r20, price);
        // average_10 은 정배열 판정(aligned) 안에서만 쓰여 여기서는 꺼내지 않는다.
        const double average_5  = moving_average.average_5;
        const double average_20 = moving_average.average_20;

        if (!quant::moving_average::aligned(moving_average, config.align_moving_average_tolerance_percent))
        {
            ++statistics.misaligned;
            continue;
        }

        double trend = average_20 > 0.0 ? (average_5 - average_20) / average_20 : 0.0;   // 추세강도(정배열 기울기, D-141부터 20일 기준)
        double pull  = average_20 > 0.0 ? (price - average_20) / average_20 : 0.0;   // 눌림깊이(음수=SMA20 아래)

        // 과확장 컷 — 이격 상한 초과는 존 밴드 진입이 불가한 폭등주라 슬롯만 낭비한다.
        if (config.max_deviation_percent > 0.0 && pull > config.max_deviation_percent)
        {
            ++statistics.overext;
            continue;
        }

        passed.push_back({symbol, trend, pull, daily_lookup.atr_percent, turnover, 0.0});
        ++statistics.aligned;
    }

    if (statistics.budget_skipped > 0)
    {
        LOG_WARN("[Main] DEVSCALE 정배열 프리필터: 일봉 조회 상한(" +
                 std::to_string(config.align_lookup_max) + ") 도달 — 캐시 없는 후보 " +
                 std::to_string(statistics.budget_skipped) + "건은 다음 재스캔에서 채움");
    }

    return passed;
}
} // namespace universe::detail

namespace universe
{
void start_daily_warm(const KisConfig& kis_config, const DevScanCfg& config)
{
    detail::g_daily_warmer.start(kis_config, config);
}
} // namespace universe
