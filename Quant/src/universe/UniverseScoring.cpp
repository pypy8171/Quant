// 점수와 등록 순서 — 통과 집합 안에서 지표를 z-score로 맞춰 종합 점수를 매기고(score_cross_section, 입력만 보는
//  순수 함수), 점수 순으로 잘라 스캔 결과를 만든다. 등록 순서가 곧 진입 우선순위다.
//  스캔 스레드 전용. [why D-018]

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
void score_cross_section(const DevScanCfg& config, std::vector<Features>& passed)
{
    auto zscore = [&](double Features::*field, bool invert, std::vector<double>& values)
    {
        const size_t count = passed.size();
        values.assign(count, 0.0);

        if (count < 2)
        {
            return;
        }

        double mean = 0.0;

        for (const auto& passed_entry : passed)
        {
            mean += passed_entry.*field;
        }

        mean /= static_cast<double>(count);
        double variance = 0.0;

        for (const auto& passed_entry : passed)
        {
            const double d0 = passed_entry.*field - mean;
            variance += d0 * d0;
        }

        variance /= static_cast<double>(count);
        const double standard_deviation = std::sqrt(variance);

        // 분산이 사실상 0이면(전 종목 동일) 정규화가 무의미하다. 전부 0으로 두어 균등 폴백.
        if (!(standard_deviation > 1e-12))
        {
            return;
        }

        for (size_t index = 0; index < count; ++index)
        {
            double value = (passed[index].*field - mean) / standard_deviation;

            if (value > 2.0)
            {
                value = 2.0;
            }

            if (value < -2.0)
            {
                value = -2.0;
            }

            values[index] = invert ? -value : value;
        }
    };
    std::vector<double> z_trend, z_pull, z_atr_percent, z_liquidity;
    zscore(&Features::trend, false, z_trend);
    zscore(&Features::pull,  true,  z_pull);   // 눌림은 음수(SMA20 아래)일수록 좋아 부호를 뒤집는다.
    zscore(&Features::atr_percent, false, z_atr_percent);

    if (config.score_weight_liquidity != 0.0)
    {
        // 거래대금은 자릿수 분포라 로그를 취해 z를 낸다. 원값 그대로면 대형주 한둘이 표준편차를
        //  다 먹어 나머지가 한 점에 뭉친다.
        std::vector<double> known;

        for (const auto& passed_entry : passed)
        {
            if (passed_entry.turnover > 0.0)
            {
                known.push_back(std::log(passed_entry.turnover));
            }
        }

        // 시세 파일이 거래대금을 안 준 종목은 중앙값으로 받쳐 중립(z≈0)에 둔다. 0을 그대로
        //  로그로 넘기면 데이터 결측이 최하위 점수로 둔갑한다.
        double fill = 0.0;

        if (!known.empty())
        {
            std::sort(known.begin(), known.end());
            fill = known[known.size() / 2];
        }

        for (auto& passed_entry : passed)
        {
            passed_entry.turnover = passed_entry.turnover > 0.0 ? std::log(passed_entry.turnover) : fill;
        }

        zscore(&Features::turnover, false, z_liquidity);
    }
    else
    {
        z_liquidity.assign(passed.size(), 0.0);
    }

    for (size_t passed_index = 0; passed_index < passed.size(); ++passed_index)
    {
        passed[passed_index].score = config.score_weight_trend * z_trend[passed_index] + config.score_weight_pullback * z_pull[passed_index]
                        - config.score_weight_volume * z_atr_percent[passed_index] + config.score_weight_liquidity * z_liquidity[passed_index];
    }
}

ScanResult rank_and_truncate(const DevScanCfg& config, std::vector<Features>& passed, const CandidateSet& candidates,
                             const symbol::SymbolTable& symbols)
{
    std::ranges::sort(passed,
                      [&symbols](const Features& feature_a, const Features& feature_b)
                      {
                          return ranks_before(feature_a.score, feature_a.symbol, feature_b.score, feature_b.symbol,
                                              symbols);
                      });
    std::size_t take_n = passed.size();

    if (config.score_top_n > 0 && static_cast<std::size_t>(config.score_top_n) < take_n)
    {
        take_n = static_cast<std::size_t>(config.score_top_n);
    }

    // max_universe(=max_register)는 스코어 경로에도 상한이다 — 0이면 등록 없음(전략 정지용).
    //  이 줄이 없으면 max_universe 0에 score_top_n 25가 25종목을 그대로 등록한다.
    if (config.max_register >= 0 && static_cast<std::size_t>(config.max_register) < take_n)
    {
        take_n = static_cast<std::size_t>(config.max_register);
    }

    ScanResult out;
    out.symbols.reserve(take_n);
    out.names.reserve(take_n);
    out.scores.reserve(take_n);

    for (std::size_t take_index = 0; take_index < take_n; ++take_index)
    {
        const Features& feature = passed[take_index];
        out.symbols.push_back(feature.symbol);
        out.names.emplace_back(candidates.name_of(feature.symbol));
        out.scores.push_back({feature.symbol, feature.score});
    }

    if (config.score_top_n > 0)
    {
        LOG_INFO("[Main] DEVSCALE 횡단면 스코어: 정배열통과=" + std::to_string(passed.size()) +
                 " → 상위 " + std::to_string(take_n) + " 선정 (w_trend=" +
                 std::to_string(config.score_weight_trend) + " w_pull=" + std::to_string(config.score_weight_pullback) +
                 " w_liq=" + std::to_string(config.score_weight_liquidity) +
                 " w_vol=" + std::to_string(config.score_weight_volume) + ")");
    }

    return out;
}

ScanResult take_first_n(const DevScanCfg& config, const CandidateSet& candidates, const MarketGate& gate)
{
    ScanResult out;

    for (const symbol::SymbolId symbol : candidates.symbols)
    {
        if (!gate.allows(candidates.market_of(symbol)))
        {
            continue;
        }

        if (static_cast<int>(out.symbols.size()) >= config.max_register)
        {
            break;
        }

        out.symbols.push_back(symbol);
        out.names.emplace_back(candidates.name_of(symbol));
    }

    return out;
}
} // namespace universe::detail
