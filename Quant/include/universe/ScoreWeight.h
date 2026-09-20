#pragma once

#include "core/SymbolTable.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace universe
{

// 스캐너가 매긴 종목 하나의 종합 점수. 종목은 id — 문자열 티커는 스캐너가 응답을 받는 자리에서 한 번 번호가 되고,
//  그 뒤 점수·랭크·배수는 전부 이 번호로 다닌다. [why D-112]
struct SymbolScore
{
    symbol::SymbolId symbol;
    double           score;
};

using ScoreList = std::vector<SymbolScore>;

// 아래 세 함수의 결과는 입력 ScoreList와 같은 순서의 배열이다(결과[i]가 scores[i]의 값). 종목 id로 찾고 싶으면
//  호출자가 자기 id 배열에 옮겨 담는다(StrategyFactory의 DevScaleScoreState처럼).

// 평균·표준편차 — 셋이 같은 식을 쓴다.
struct ScoreMoments
{
    double mean               = 0.0;
    double standard_deviation = 0.0;
};

inline ScoreMoments score_moments(const ScoreList& scores)
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
inline std::vector<double> score_to_mult(const ScoreList& scores, double spread, double target_total_percent,
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
            z_score        = std::max(-2.0, std::min(2.0, z_score));
            raw[index]     = 1.0 + spread * z_score / 2.0;
        }
    }

    // 상위 slots개의 raw 합으로 정규화 — 정렬은 사본으로 하고 raw의 순서(입력 순서)는 지킨다.
    std::vector<double> sorted_raw = raw;
    std::ranges::sort(sorted_raw, std::ranges::greater{});
    const size_t take    = std::min(static_cast<size_t>(slots), sorted_raw.size());
    const double sum_top = std::accumulate(sorted_raw.begin(), sorted_raw.begin() + static_cast<std::ptrdiff_t>(take), 0.0);

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

// 종합 점수 → 표준화 점수(z, ±2 클립). 랭크는 "몇 번째"만 알려주므로 교체 판정처럼
//  "얼마나 더 좋은지"를 봐야 하는 곳에서 쓴다. 분산이 없으면 전부 0을 돌려준다.
inline std::vector<double> score_to_z(const ScoreList& scores)
{
    std::vector<double> out(scores.size(), 0.0);
    const ScoreMoments  moments = score_moments(scores);

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

// 종합 점수 → 진입 우선순위 랭크(1=최고). 동점은 티커 사전순으로 갈라 결정론을 유지한다 — 이름은 여기서만 본다.
inline std::vector<int> score_to_rank(const ScoreList& scores, const symbol::SymbolTable& symbols)
{
    std::vector<size_t> order(scores.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t index_a, size_t index_b) {
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
