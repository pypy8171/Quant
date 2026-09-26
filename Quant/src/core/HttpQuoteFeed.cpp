// HTTP 시세 피드 구현 — 레인별 한 바퀴·본문 훑기·증분 판정. 왜 있는지는 core/HttpQuoteFeed.h 머리말.
#include "core/HttpQuoteFeed.h"

#include "api/HttpGet.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <algorithm>
#include <unordered_map>

namespace feed
{

namespace
{

// 받는 곳 — 네이버 금융의 시세 폴링 주소. 바꿀 자리를 한군데로 모으려고 여기 상수 하나로
//  둔다. 다른 곳으로 옮길 때 고칠 자리는 이 줄·아래 헤더 두 줄·아래 파싱 키다.
constexpr std::string_view kEndpoint = "https://polling.finance.naver.com/api/realtime/domestic/stock/";

// 응답 한 종목이 시작하는 표지. 이 뒤로 다음 표지 전까지가 한 종목의 구간이다.
constexpr std::string_view kCodeKey   = "\"itemCode\":\"";
constexpr std::string_view kPriceKey  = "\"closePrice\":\"";
constexpr std::string_view kVolumeKey = "\"accumulatedTradingVolume\":\"";
constexpr std::string_view kTimeKey   = "\"localTradedAt\":\"";

// "276,500" 처럼 쉼표가 끼어 있는 숫자를 읽는다. 닫는 따옴표에서 멈춘다.
int64_t read_grouped_number(std::string_view segment, size_t from)
{
    int64_t value = 0;

    for (size_t index = from; index < segment.size() && segment[index] != '"'; ++index)
    {
        const char letter = segment[index];

        if (letter >= '0' && letter <= '9')
        {
            value = value * 10 + (letter - '0');
        }
        else if (letter != ',')
        {
            break; // 소수점·부호는 국내 현물 가격에 안 나온다 — 나오면 거기서 끊는다
        }
    }

    return value;
}

// "2026-09-22T14:35:56.444725+09:00" 에서 143556을 만든다. 모양이 다르면 0.
int32_t read_hhmmss(std::string_view segment, size_t from)
{
    if (from + 19 > segment.size() || segment[from + 10] != 'T')
    {
        return 0;
    }

    const auto    digit  = [&](size_t offset)
    {
        return static_cast<int32_t>(segment[from + offset] - '0');
    };
    const int32_t hour   = digit(11) * 10 + digit(12);
    const int32_t minute = digit(14) * 10 + digit(15);
    const int32_t second = digit(17) * 10 + digit(18);
    return hour * 10000 + minute * 100 + second;
}

// 한 종목 구간에서 열쇠 뒤 값의 시작 자리를 찾는다. 없으면 npos.
size_t value_start(std::string_view segment, std::string_view key)
{
    const size_t found = segment.find(key);
    return found == std::string_view::npos ? std::string_view::npos : found + key.size();
}

} // namespace

std::vector<ParsedQuote> parse_quotes(std::string_view body)
{
    std::vector<ParsedQuote> quotes;
    size_t                   cursor = body.find(kCodeKey);

    while (cursor != std::string_view::npos)
    {
        const size_t code_at = cursor + kCodeKey.size();
        const size_t next    = body.find(kCodeKey, code_at);
        // 이 종목 구간만 본다 — 구간을 안 자르면 뒤 종목의 값을 읽는다. 마지막 종목은 본문 끝까지.
        const std::string_view segment =
            body.substr(code_at, (next == std::string_view::npos ? body.size() : next) - code_at);
        const size_t code_end = segment.find('"');

        if (code_end == std::string_view::npos)
        {
            break;
        }

        ParsedQuote quote;
        quote.code = segment.substr(0, code_end);

        const size_t price_at = value_start(segment, kPriceKey);

        if (price_at != std::string_view::npos)
        {
            quote.price = static_cast<double>(read_grouped_number(segment, price_at));
        }

        const size_t volume_at = value_start(segment, kVolumeKey);

        if (volume_at != std::string_view::npos)
        {
            quote.accumulated_volume = read_grouped_number(segment, volume_at);
        }

        const size_t time_at = value_start(segment, kTimeKey);

        if (time_at != std::string_view::npos)
        {
            quote.hhmmss = read_hhmmss(segment, time_at);
        }

        if (quote.price > 0.0)
        {
            quotes.push_back(quote);
        }

        cursor = next;
    }

    return quotes;
}

HttpQuoteFeed::HttpQuoteFeed(Config config, TickSink sink) : config_(std::move(config)), sink_(std::move(sink))
{
    if (config_.lane_count == 0)
    {
        config_.lane_count = 1;
    }

    if (config_.max_codes_per_call == 0)
    {
        config_.max_codes_per_call = 900;
    }
}

HttpQuoteFeed::~HttpQuoteFeed()
{
    stop();
}

void HttpQuoteFeed::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    for (size_t lane = 0; lane < config_.lane_count; ++lane)
    {
        lanes_.emplace_back([this, lane]
        {
            run_lane(lane);
        });
    }

    LOG_INFO("[HttpQuoteFeed] 시작 — 종목 " + std::to_string(config_.codes.size()) + "개, 수신 스레드 " +
             std::to_string(config_.lane_count) + "개, 주기 " + std::to_string(config_.sweep_period.count()) +
             "ms, 묶음 " + std::to_string(config_.max_codes_per_call) + "개");
}

void HttpQuoteFeed::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    for (std::thread& lane : lanes_)
    {
        if (lane.joinable())
        {
            lane.join();
        }
    }

    lanes_.clear();
}

void HttpQuoteFeed::run_lane(size_t lane)
{
    thread_name::set_current("HttpLane " + std::to_string(lane));

    // [inv] 이 레인이 맡은 종목은 여기서 정해지고 끝까지 바뀌지 않는다 — 한 종목은 한 스레드만 내보낸다.
    std::vector<std::string> slice;

    for (size_t index = lane; index < config_.codes.size(); index += config_.lane_count)
    {
        slice.push_back(config_.codes[index]);
    }

    if (slice.empty())
    {
        return;
    }

    // 직전 바퀴 값. 코드로 찾는다 — 응답 순서가 요청 순서와 같다는 보장이 없다.
    struct LastSeen
    {
        double  price              = 0.0;
        int64_t accumulated_volume = 0;
    };

    std::unordered_map<std::string, LastSeen> last_seen;
    last_seen.reserve(slice.size() * 2);

    // 한 바퀴에 보낼 요청의 URL을 미리 만든다 — 종목 목록은 안 바뀌므로 매 바퀴 문자열을 잇는 것은 낭비다.
    std::vector<std::string> request_urls;

    for (size_t begin = 0; begin < slice.size(); begin += config_.max_codes_per_call)
    {
        const size_t end = std::min(begin + config_.max_codes_per_call, slice.size());
        std::string  url(kEndpoint);

        for (size_t index = begin; index < end; ++index)
        {
            if (index > begin)
            {
                url.push_back(',');
            }

            url.append(slice[index]);
        }

        request_urls.push_back(std::move(url));
    }

    // 웹페이지가 보내는 것과 같은 헤더. 이 둘이 없으면 본문이 빈 채로 200이 온다.
    const std::vector<std::string> headers{"User-Agent: Mozilla/5.0", "Referer: https://finance.naver.com/"};
    auto                           next_sweep_at = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire))
    {
        const auto sweep_started = std::chrono::steady_clock::now();

        for (const std::string& url : request_urls)
        {
            if (!running_.load(std::memory_order_acquire))
            {
                break;
            }

            const std::string body = http::get(url, headers);
            counters_.calls.fetch_add(1, std::memory_order_relaxed);
            counters_.bytes_received.fetch_add(body.size(), std::memory_order_relaxed);

            if (body.empty())
            {
                counters_.call_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const std::vector<ParsedQuote> quotes = parse_quotes(body);

            if (quotes.empty())
            {
                // 200인데 종목이 하나도 없으면 실패로 센다 — 조용히 넘기면 시세가 끊긴 줄 모른다.
                counters_.call_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            counters_.quotes_received.fetch_add(quotes.size(), std::memory_order_relaxed);
            const int64_t received_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
            const auto received_at = std::chrono::system_clock::now();

            for (const ParsedQuote& quote : quotes)
            {
                LastSeen& last = last_seen[std::string(quote.code)];
                // 주기 사이에 늘어난 누적 거래량이 이번 건의 수량이다. 값이 그대로면 그 종목은 이 주기에
                //  체결이 없었다는 뜻이라 흘리지 않는다 — 안 흘려야 전략이 안 움직인 종목을 움직였다고 안 본다.
                const int64_t traded = quote.accumulated_volume - last.accumulated_volume;
                const bool    first  = last.accumulated_volume == 0 && last.price == 0.0;

                if (!first && traded <= 0 && quote.price == last.price)
                {
                    continue;
                }

                last.price              = quote.price;
                last.accumulated_volume = quote.accumulated_volume;

                if (first)
                {
                    continue; // 첫 바퀴는 기준만 잡는다 — 그날 누적 전부를 한 건으로 흘리면 안 된다
                }

                TradeData trade;
                trade.ticker.assign(quote.code);
                trade.hhmmss             = quote.hhmmss;
                trade.price              = quote.price;
                trade.quantity           = traded > 0 ? traded : 0;
                trade.market             = Market::KR;
                trade.timestamp          = received_at;
                trade.accumulated_volume = quote.accumulated_volume;
                trade.received_ns        = received_ns;
                counters_.ticks_emitted.fetch_add(1, std::memory_order_relaxed);
                sink_(lane, trade);
            }
        }

        const auto sweep_micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - sweep_started)
                .count();
        counters_.sweeps.fetch_add(1, std::memory_order_relaxed);
        counters_.sweep_micros_total.fetch_add(static_cast<uint64_t>(sweep_micros), std::memory_order_relaxed);
        uint64_t previous_max = counters_.sweep_micros_max.load(std::memory_order_relaxed);

        while (static_cast<uint64_t>(sweep_micros) > previous_max &&
               !counters_.sweep_micros_max.compare_exchange_weak(previous_max, static_cast<uint64_t>(sweep_micros),
                                                                 std::memory_order_relaxed))
        {
        }

        next_sweep_at += config_.sweep_period;
        const auto now = std::chrono::steady_clock::now();

        if (now >= next_sweep_at)
        {
            // 한 바퀴가 주기를 넘겼다. 밀린 만큼을 몰아 보내지 않고 지금을 새 기준으로 잡는다 — 밀린 것을
            //  따라잡으려 요청을 몰아도 받는 속도는 그대로다.
            counters_.sweep_overruns.fetch_add(1, std::memory_order_relaxed);
            next_sweep_at = now;
            continue;
        }

        // 멈추라는 신호를 바로 받도록 잘게 나눠 잔다.
        while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < next_sweep_at)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

} // namespace feed
