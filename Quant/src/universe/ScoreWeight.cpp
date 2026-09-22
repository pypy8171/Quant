#include "universe/ScoreWeight.h"

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

std::vector<double> score_to_mult(const ScoreList& scores, double spread, double target_total_percent,
                                  double base_percent, int slots)
{
    std::vector<double> multiplier(scores.size(), 1.0);

    if (scores.empty() || !(spread > 0.0) || !(target_total_percent > 0.0) || !(base_percent > 0.0) || slots <= 0)
    {
        return multiplier;
    }

    const ScoreMoments moments = score_moments(scores);

    // 분산이 없으면(전 종목 동점) 차등이 의미 없다. 총합 정규화만 걸고 배수는 균등하게 둔다.
    std::vector<double> raw(scores.size(), 1.0);

    if (moments.standard_deviation > 1e-12)
    {
        for (size_t index = 0; index < scores.size(); ++index)
        {
            double z_score = (scores[index].score - moments.mean) / moments.standard_deviation;
            z_score = std::max(-2.0, std::min(2.0, z_score));
            raw[index] = 1.0 + spread * z_score / 2.0;
        }
    }

    // 상위 slots개의 raw 합으로 정규화 — 정렬은 사본으로 하고 raw의 순서(입력 순서)는 지킨다.
    std::vector<double> sorted_raw = raw;
    std::ranges::sort(sorted_raw, std::ranges::greater{});
    const size_t take = std::min(static_cast<size_t>(slots), sorted_raw.size());
    const double sum_top =
        std::accumulate(sorted_raw.begin(), sorted_raw.begin() + static_cast<std::ptrdiff_t>(take), 0.0);

    if (!(sum_top > 1e-12))
    {
        return multiplier;
    }

    const double scale = target_total_percent / (base_percent * sum_top);

    for (size_t index = 0; index < raw.size(); ++index)
    {
        multiplier[index] = raw[index] * scale;
    }

    return multiplier;
}

std::vector<double> score_to_z(const ScoreList& scores)
{
    std::vector<double> out(scores.size(), 0.0);
    const ScoreMoments moments = score_moments(scores);

    if (moments.standard_deviation <= 1e-12)
    {
        return out;
    }

    for (size_t index = 0; index < scores.size(); ++index)
    {
        out[index] = std::max(-2.0, std::min(2.0, (scores[index].score - moments.mean) / moments.standard_deviation));
    }

    return out;
}

std::vector<int> score_to_rank(const ScoreList& scores, const symbol::SymbolTable& symbols)
{
    std::vector<size_t> order(scores.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(),
              [&](size_t index_a, size_t index_b)
              {
                  const SymbolScore& entry_a = scores[index_a];
                  const SymbolScore& entry_b = scores[index_b];

                  if (entry_a.score != entry_b.score)
                  {
                      return entry_a.score > entry_b.score;
                  }

                  return symbols.name(entry_a.symbol).view() < symbols.name(entry_b.symbol).view();
              });
    std::vector<int> rank(scores.size(), 0);

    for (size_t position = 0; position < order.size(); ++position)
    {
        rank[order[position]] = static_cast<int>(position) + 1;
    }

    return rank;
}

} // namespace universe
