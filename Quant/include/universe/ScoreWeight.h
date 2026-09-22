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

ScoreMoments score_moments(const ScoreList& scores);

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
std::vector<double> score_to_mult(const ScoreList& scores, double spread, double target_total_percent,
                                         double base_percent, int slots);

// 종합 점수 → 표준화 점수(z, ±2 클립). 랭크는 "몇 번째"만 알려주므로 교체 판정처럼
//  "얼마나 더 좋은지"를 봐야 하는 곳에서 쓴다. 분산이 없으면 전부 0을 돌려준다.
std::vector<double> score_to_z(const ScoreList& scores);

// 종합 점수 → 진입 우선순위 랭크(1=최고). 동점은 티커 사전순으로 갈라 결정론을 유지한다 — 이름은 여기서만 본다.
std::vector<int> score_to_rank(const ScoreList& scores, const symbol::SymbolTable& symbols);

} // namespace universe
