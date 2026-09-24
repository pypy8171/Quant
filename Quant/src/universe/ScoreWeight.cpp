#include "universe/ScoreWeight.h"

#include <functional>

namespace universe
{
ScoreMoments score_moments(const ScoreList& scores)
{
    ScoreMoments moments;

    if (scores.empty())
    {
        return moments;
    }

    for (const SymbolScore& entry : scores)
    {
        moments.mean += entry.score;
    }

    moments.mean /= static_cast<double>(scores.size());
    double variance = 0.0;

    for (const SymbolScore& entry : scores)
    {
        const double deviation = entry.score - moments.mean;
        variance += deviation * deviation;
    }

    variance /= static_cast<double>(scores.size());
    moments.standard_deviation = std::sqrt(variance);
    return moments;
}

void score_to_mult(const ScoreList& scores, double spread, double target_total_percent, double base_percent,
                   int slots, std::vector<double>& multiplier, std::vector<double>& sorted_raw)
{
    multiplier.assign(scores.size(), 1.0);

    if (scores.empty() || !(spread > 0.0) || !(target_total_percent > 0.0) || !(base_percent > 0.0) || slots <= 0)
    {
        return;
    }

    spread = std::min(spread, 1.0);
    const ScoreMoments moments = score_moments(scores);

    // 분산이 없으면(전 종목 동점) 차등이 의미 없다. 총합 정규화만 걸고 배수는 균등하게 둔다.
    //  raw는 multiplier 칸에 먼저 담고, 끝에서 scale을 곱해 배수로 바꾼다.
    if (moments.standard_deviation > 1e-12)
    {
        for (size_t index = 0; index < scores.size(); ++index)
        {
            double z_score = (scores[index].score - moments.mean) / moments.standard_deviation;
            z_score = std::max(-2.0, std::min(2.0, z_score));
            multiplier[index] = 1.0 + spread * z_score / 2.0;
        }
    }

    // 상위 slots개의 raw 합으로 정규화 — 정렬은 작업 버퍼에서 하고 raw의 순서(입력 순서)는 지킨다.
    sorted_raw.assign(multiplier.begin(), multiplier.end());
    const size_t take = std::min(static_cast<size_t>(slots), sorted_raw.size());
    const auto take_end = sorted_raw.begin() + static_cast<std::ptrdiff_t>(take);
    std::partial_sort(sorted_raw.begin(), take_end, sorted_raw.end(), std::greater<>{});
    const double sum_top = std::accumulate(sorted_raw.begin(), take_end, 0.0);

    if (!(sum_top > 1e-12))
    {
        multiplier.assign(scores.size(), 1.0);
        return;
    }

    const double scale = target_total_percent / (base_percent * sum_top);

    for (double& value : multiplier)
    {
        value *= scale;
    }
}

void score_to_z(const ScoreList& scores, std::vector<double>& z_score)
{
    z_score.assign(scores.size(), 0.0);
    const ScoreMoments moments = score_moments(scores);

    if (moments.standard_deviation <= 1e-12)
    {
        return;
    }

    for (size_t index = 0; index < scores.size(); ++index)
    {
        z_score[index] =
            std::max(-2.0, std::min(2.0, (scores[index].score - moments.mean) / moments.standard_deviation));
    }
}

bool ranks_before(double score_a, symbol::SymbolId symbol_a, double score_b, symbol::SymbolId symbol_b,
                  const symbol::SymbolTable& symbols)
{
    if (score_a != score_b)
    {
        return score_a > score_b;
    }

    return symbols.name(symbol_a).view() < symbols.name(symbol_b).view();
}

void score_to_rank(const ScoreList& scores, const symbol::SymbolTable& symbols, std::vector<int>& rank,
                   std::vector<size_t>& order)
{
    order.resize(scores.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(),
              [&](size_t index_a, size_t index_b)
              {
                  const SymbolScore& entry_a = scores[index_a];
                  const SymbolScore& entry_b = scores[index_b];
                  return ranks_before(entry_a.score, entry_a.symbol, entry_b.score, entry_b.symbol, symbols);
              });
    rank.assign(scores.size(), 0);

    for (size_t position = 0; position < order.size(); ++position)
    {
        rank[order[position]] = static_cast<int>(position) + 1;
    }
}

} // namespace universe
