// 정배열 프리필터와 지표 산출 — 후보마다 확정 일봉 요약(SMA·ATR·고가·거래량)을 캐시에서 꺼내거나 받아 현재가를
//  접어 넣고, 정배열·이격·거래대금 조건을 통과한 종목의 점수 재료(Features)를 만든다. 일봉 요약 캐시와 그 디스크
//  사본도 여기 있다. 스캔 스레드 전용. [why D-005·D-028]

#include "detail/Pipeline.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
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
            // 13칸이 지금 형식(D-141에서 60일선 칸을, 재조회 경로를 걷으며 조회 시각 칸을 뺐다). 다른 칸수는 옛 파일 —
            //  건너뛰면 캐시 미스와 같아서 그 종목만 다시 받는다.
            if (!iterator.value().is_array() || iterator.value().size() != 13)
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
            daily_lookup.hi250     = value[9].get<double>();
            daily_lookup.pivot_high  = value[10].get<double>();
            daily_lookup.average_vol20 = value[11].get<double>();
            daily_lookup.close21   = value[12].get<double>();

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
    // [wire] 값 순서: bars, average_5, average_10, average_20, close, r5, r10, r20, atr_percent,
    //  hi250, pivot_high, average_vol20, close21 — 13칸 고정(읽는 쪽이 칸수로 형식을 가린다)
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
                                                 daily_lookup.atr_percent,
                                                 daily_lookup.hi250, daily_lookup.pivot_high, daily_lookup.average_vol20, daily_lookup.close21});
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
// 후보 하나의 일봉을 받아 SMA·롤오프·ATR로 요약한다. 60봉 미만이면 bars만 채워 돌려준다.
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

    // 저항·거래량 축. 일봉은 전일까지(include_today=false)라 d[0]이 전일이다.
    for (const auto& bar : daily_ohlcv)
    {
        if (bar.high > daily_lookup.hi250)
        {
            daily_lookup.hi250 = bar.high;
        }
    }

    // [formula] 스윙 고점 = 좌우 5봉의 고가보다 모두 높은 봉. 가장 최근 것 하나만 쓴다.
    for (size_t index = 5; index + 5 < daily_ohlcv.size(); ++index)
    {
        bool peak = true;

        for (size_t innermost_index = 1; innermost_index <= 5 && peak; ++innermost_index)
        {
            peak = daily_ohlcv[index].high > daily_ohlcv[index - innermost_index].high && daily_ohlcv[index].high > daily_ohlcv[index + innermost_index].high;
        }

        if (peak)
        {
            daily_lookup.pivot_high = daily_ohlcv[index].high;
            break;
        }
    }

    double volume_sum = 0.0;

    for (int index = 0; index < 20; ++index)
    {
        volume_sum += static_cast<double>(daily_ohlcv[index].volume);
    }

    daily_lookup.average_vol20 = volume_sum / 20.0;
    daily_lookup.close21   = daily_ohlcv.size() > 21 ? daily_ohlcv[21].close : 0.0;
    return daily_lookup;
}
} // namespace

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
            constexpr int kDailyLookupSleepMs      = 150; // 실계좌 — 키 한도 초당 20건
            constexpr int kDailyLookupSleepPaperMs = 600; // 모의계좌 — 키 한도 초당 2건

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

        // 과확장 하한 — 밴드 아래(덜 벌어진 종목)는 눌림 슬리브 몫이다.
        if (config.min_deviation_percent > 0.0 && pull < config.min_deviation_percent)
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
