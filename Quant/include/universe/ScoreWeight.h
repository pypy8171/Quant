#pragma once

#include <algorithm>
#include <functional>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace universe
{

// ─────────────────────────────────────────────────────────────────────────────
// 종합 점수 → 종목별 비중 배수
//
//  스캐너가 낸 종합 점수 S(추세·눌림·변동성의 횡단면 z 가중합)를 사이징 배수로 옮긴다.
//  같은 점수 하나가 진입 우선순위(등록 순서)와 비중을 모두 정하므로, 여기서는 비중만 만든다.
//
//    z_i    = clamp((S_i − μ)/σ, −2, +2)        점수 자체를 다시 정규화 — 가중치를 바꿔도
//                                               배수 범위가 예측 가능하게 유지된다
//    raw_i  = 1 + spread × z_i / 2              spread=0.6 → raw ∈ [0.4, 1.6] (최대:최소 4:1)
//    scale  = target_total_percent / (base_percent × Σ_{상위 slots} raw)
//    mult_i = raw_i × scale
//
//  정규화 기준을 "등록 전체"가 아니라 "상위 slots개"로 잡는 이유: 동시 보유 상한이 slots이라
//  실제로 자본을 물고 있는 건 최대 slots종목이다. 등록 수(오늘 57)로 나누면 예산을 절반도
//  못 쓰고, 정규화를 아예 안 하면 반대로 넘친다(2026-09-07 실측: 25슬롯 × base_percent 5% =
//  125% > 총노출 캡 95% → 캡이 신규 매수를 통째로 리젝).
//
//  σ가 사실상 0이거나 입력이 비면 전부 1.0을 돌려준다(균등 폴백 — 배선 전 동작과 동일).
// ─────────────────────────────────────────────────────────────────────────────
inline std::unordered_map<std::string, double>
score_to_mult(const std::unordered_map<std::string, double>& scores,
              double spread, double target_total_percent, double base_percent, int slots)
{
    std::unordered_map<std::string, double> multiplier;

    if (scores.empty())
    {
        return multiplier;
    }

    for (const auto& entry : scores)
    {
        multiplier[entry.first] = 1.0;
    }

    if (!(spread > 0.0) || !(target_total_percent > 0.0) || !(base_percent > 0.0) || slots <= 0)
    {
        return multiplier;
    }

    const size_t count = scores.size();
    double mean = 0.0;

    for (const auto& entry : scores)
    {
        mean += entry.second;
    }

    mean /= static_cast<double>(count);
    double variance = 0.0;

    for (const auto& entry : scores) { const double deviation = entry.second - mean; variance += deviation * deviation; }
    variance /= static_cast<double>(count);
    const double standard_deviation = std::sqrt(variance);

    // 분산이 없으면(전 종목 동점) 차등이 의미 없다. 총합 정규화만 걸고 배수는 균등하게 둔다.
    std::vector<std::pair<std::string, double>> raw;
    raw.reserve(count);

    for (const auto& entry : scores)
    {
        double ratio = 1.0;

        if (standard_deviation > 1e-12)
        {
            double z_score = (entry.second - mean) / standard_deviation;
            z_score = std::max(-2.0, std::min(2.0, z_score));
            ratio = 1.0 + spread * z_score / 2.0;
        }

        raw.emplace_back(entry.first, ratio);
    }

    // 상위 slots개의 raw 합으로 정규화(내림차순 정렬 후 앞에서 slots개).
    std::ranges::sort(raw, std::ranges::greater{}, &std::pair<std::string, double>::second);
    const size_t take = std::min(static_cast<size_t>(slots), raw.size());
    double sum_top = 0.0;

    for (size_t take_index = 0; take_index < take; ++take_index)
    {
        sum_top += raw[take_index].second;
    }

    if (!(sum_top > 1e-12))
    {
        return multiplier;
    }

    const double scale = target_total_percent / (base_percent * sum_top);

    for (const auto& entry : raw)
    {
        multiplier[entry.first] = entry.second * scale;
    }

    return multiplier;
}

// 종합 점수 → 표준화 점수(z, ±2 클립). 랭크는 "몇 번째"만 알려주므로 교체 판정처럼
//  "얼마나 더 좋은지"를 봐야 하는 곳에서 쓴다. 분산이 없으면 전부 0을 돌려준다.
inline std::unordered_map<std::string, double>
score_to_z(const std::unordered_map<std::string, double>& scores)
{
    std::unordered_map<std::string, double> out;

    if (scores.empty())
    {
        return out;
    }

    double mean = 0.0;

    for (const auto& entry : scores)
    {
        mean += entry.second;
    }

    mean /= static_cast<double>(scores.size());
    double variance = 0.0;

    for (const auto& entry : scores) { const double deviation = entry.second - mean; variance += deviation * deviation; }
    variance /= static_cast<double>(scores.size());
    const double standard_deviation = std::sqrt(variance);

    for (const auto& entry : scores)
    {
        double value = 0.0;

        if (standard_deviation > 1e-12)
        {
            value = std::max(-2.0, std::min(2.0, (entry.second - mean) / standard_deviation));
        }

        out[entry.first] = value;
    }

    return out;
}

// 종합 점수 → 진입 우선순위 랭크(1=최고). 동점은 티커 사전순으로 갈라 결정론을 유지한다.
inline std::unordered_map<std::string, int>
score_to_rank(const std::unordered_map<std::string, double>& scores)
{
    std::vector<std::pair<std::string, double>> values(scores.begin(), scores.end());
    std::sort(values.begin(), values.end(),
              [](const std::pair<std::string, double>& pair_a,
                 const std::pair<std::string, double>& pair_b)
              { return pair_a.second != pair_b.second ? pair_a.second > pair_b.second : pair_a.first < pair_b.first; });
    std::unordered_map<std::string, int> rank;

    for (size_t index = 0; index < values.size(); ++index)
    {
        rank[values[index].first] = static_cast<int>(index) + 1;
    }

    return rank;
}

} // namespace universe
