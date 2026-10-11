// bench_engine_load — 입력 준비 — 종목 목록·실측 유량 프로파일·뽑기표·숫자 목록 읽기.
//  공용 준비는 bench_engine_load_support.h, 구성 순회는 bench_engine_load.cpp의 main.
#include "bench_engine_load_support.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace engine_load_bench
{

// ── 종목 목록 ────────────────────────────────────────────────────────────────
// universe_full.json의 "codes" 배열에서 6자리 코드만 뽑는다. 파일이 없으면 합성 코드로 채운다.
//  JSON 파서를 끌어오지 않는 것은 bench_market_firehose와 같은 이유다(하네스는 가볍게).
std::vector<std::string> load_universe(const std::string& path, size_t wanted)
{
    std::vector<std::string> codes;
    std::ifstream            file(path);

    if (file)
    {
        const std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const size_t      codes_at = body.find("\"codes\"");
        const std::string scan     = codes_at == std::string::npos ? body : body.substr(codes_at);

        for (size_t index = 0; index + 1 < scan.size(); ++index)
        {
            if (scan[index] != '"')
            {
                continue;
            }

            size_t tail = index + 1;

            while (tail < scan.size() && scan[tail] >= '0' && scan[tail] <= '9')
            {
                ++tail;
            }

            if (tail - index - 1 == 6 && tail < scan.size() && scan[tail] == '"')
            {
                codes.push_back(scan.substr(index + 1, 6));
                index = tail;
            }
        }
    }

    // 실제 종목 수가 모자라면 합성 코드로 채운다 — 부하 규모가 목적이라 코드의 진짜 여부는 중요하지 않다.
    for (size_t index = codes.size(); index < wanted; ++index)
    {
        std::string synthetic = std::to_string(900000 + index);
        codes.push_back(synthetic.substr(synthetic.size() - 6));
    }

    codes.resize(wanted);
    return codes;
}

// ── 실측 유량 프로파일 ───────────────────────────────────────────────────────
// scripts/stresstest_flow_profile.py가 캡처한 실체결에서 뽑은 JSON이다. 여기서 두 가지를 읽는다.
//  rank_shares: 순위별 체결 몫(대형주 편중의 실제 모양) → 뽑기표. scaled_trades_per_second: 전 종목 환산 초당 건수.
// 합성 zipf 계수 대신 이것을 쓰는 이유는, 프로세스 분리 전후 비교는 실제 장이 주는 만큼에서 재야 뜻이 있기 때문이다.
std::vector<double> load_rank_shares(const std::string& path)
{
    std::vector<double> shares;
    std::ifstream       file(path);

    if (!file)
    {
        return shares;
    }

    const std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t      key_at = body.find("\"rank_shares\"");

    if (key_at == std::string::npos)
    {
        return shares;
    }

    const size_t open_at  = body.find('[', key_at);
    const size_t close_at = open_at == std::string::npos ? std::string::npos : body.find(']', open_at);

    if (close_at == std::string::npos)
    {
        return shares;
    }

    std::stringstream stream(body.substr(open_at + 1, close_at - open_at - 1));
    std::string       token;

    while (std::getline(stream, token, ','))
    {
        try
        {
            shares.push_back(std::stod(token));
        }
        catch (const std::exception&)
        {
            // 공백·줄바꿈만 있는 토큰은 건너뛴다
        }
    }

    return shares;
}

// scaled_trades_per_second의 p50·p90·p99·max 중 하나를 읽는다. 없으면 0(=최대 속도).
uint64_t load_profile_rate(const std::string& path, const std::string& key)
{
    std::ifstream file(path);

    if (!file)
    {
        return 0;
    }

    const std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t      block_at = body.find("\"scaled_trades_per_second\"");

    if (block_at == std::string::npos)
    {
        return 0;
    }

    const size_t key_at = body.find("\"" + key + "\"", block_at);

    if (key_at == std::string::npos)
    {
        return 0;
    }

    const size_t colon_at = body.find(':', key_at);

    if (colon_at == std::string::npos)
    {
        return 0;
    }

    return static_cast<uint64_t>(std::stoull(body.substr(colon_at + 1, 32)));
}

// 몫 벡터(합이 1일 필요는 없다)로 뽑기표를 만든다. 아래 Zipf 판과 같은 방식 — 누적합에 균등 격자를 쏘고 한 번 섞는다.
std::vector<uint32_t> build_pick_table_from_weights(const std::vector<double>& weights, size_t table_size)
{
    std::vector<uint32_t> table;

    if (weights.empty())
    {
        return table;
    }

    std::vector<double> cumulative(weights.size());
    double              total = 0.0;

    for (size_t rank = 0; rank < weights.size(); ++rank)
    {
        total += weights[rank] > 0.0 ? weights[rank] : 0.0;
        cumulative[rank] = total;
    }

    if (total <= 0.0)
    {
        return table;
    }

    table.reserve(table_size);

    for (size_t slot = 0; slot < table_size; ++slot)
    {
        const double target = total * (static_cast<double>(slot) + 0.5) / static_cast<double>(table_size);
        const auto   found  = std::lower_bound(cumulative.begin(), cumulative.end(), target);
        table.push_back(static_cast<uint32_t>(std::min<size_t>(std::distance(cumulative.begin(), found), weights.size() - 1)));
    }

    std::mt19937 generator(20260922u);
    std::shuffle(table.begin(), table.end(), generator);
    return table;
}

// ── 거래량 쏠림(Zipf) ────────────────────────────────────────────────────────
// 실제 장은 대형주 몇 개가 체결 대부분을 차지한다. 종목을 균등하게 도는 루프로는 큐가 실제로 터지는
//  국면(한 종목·한 샤드에 몰리는 순간)을 재현하지 못한다. 뽑기표를 미리 만들고 한 번 섞어 두었다가
//  순서대로 읽는다 — hot loop에 난수와 나눗셈을 두지 않으려고. 난수 시드 고정이라 구성 사이 비교가 된다.
std::vector<uint32_t> build_pick_table(size_t symbol_count, double skew, size_t table_size)
{
    std::vector<uint32_t> table;

    if (symbol_count == 0)
    {
        return table;
    }

    table.reserve(table_size);

    if (skew <= 0.0)
    {
        for (size_t slot = 0; slot < table_size; ++slot)
        {
            table.push_back(static_cast<uint32_t>(slot % symbol_count));
        }

        return table;
    }

    std::vector<double> cumulative(symbol_count);
    double              total = 0.0;

    for (size_t rank = 0; rank < symbol_count; ++rank)
    {
        total += 1.0 / std::pow(static_cast<double>(rank + 1), skew);
        cumulative[rank] = total;
    }

    for (size_t slot = 0; slot < table_size; ++slot)
    {
        const double target = total * (static_cast<double>(slot) + 0.5) / static_cast<double>(table_size);
        const auto   found  = std::lower_bound(cumulative.begin(), cumulative.end(), target);
        table.push_back(static_cast<uint32_t>(std::distance(cumulative.begin(), found)));
    }

    std::mt19937 generator(20260922u);
    std::shuffle(table.begin(), table.end(), generator);
    return table;
}

std::vector<uint32_t> parse_number_list(const std::string& text)
{
    std::vector<uint32_t> numbers;
    std::stringstream     stream(text);
    std::string           token;

    while (std::getline(stream, token, ','))
    {
        if (!token.empty())
        {
            numbers.push_back(static_cast<uint32_t>(std::stoul(token)));
        }
    }

    return numbers;
}

} // namespace engine_load_bench
