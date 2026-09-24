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

// 아래 세 함수는 입력 ScoreList와 같은 순서의 배열을 호출자 버퍼에 채운다(결과[i]가 scores[i]의 값). 버퍼는 재스캔마다
//  새로 잡지 않도록 호출자가 들고 있다가 넘긴다. 종목 id로 찾고 싶으면 호출자가 자기 id 배열에 옮겨 담는다
//  (StrategyFactory의 DevScaleScoreState처럼).

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
//  σ가 사실상 0이면 모두 같은 값(target_total_percent/(base_percent×min(slots,n)))이다.
//  입력이 비었거나 파라미터가 무효일 때만 1.0이다.
//  전제: spread ∈ [0, 1]. 1을 넘으면 z=−2 쪽 raw가 음수가 되어 호출자가 기본 배수로 되돌리므로
//  여기서 1로 자른다(설정 적재에서 경고와 함께 먼저 자른다).
//  sorted_raw는 상위 slots개 합을 구하는 작업 버퍼다 — 내용은 쓰지 않고 크기만 재사용한다.
// ─────────────────────────────────────────────────────────────────────────────
void score_to_mult(const ScoreList& scores, double spread, double target_total_percent, double base_percent,
                   int slots, std::vector<double>& multiplier, std::vector<double>& sorted_raw);

// 종합 점수 → 표준화 점수(z, ±2 클립). 랭크는 "몇 번째"만 알려주므로 교체 판정처럼
//  "얼마나 더 좋은지"를 봐야 하는 곳에서 쓴다. 분산이 없으면 전부 0이다.
void score_to_z(const ScoreList& scores, std::vector<double>& z_score);

// 점수 내림차순, 같은 점수는 티커 사전순. 순위 계산과 스캐너의 등록 상한 자르기가 같은 규칙을 써서
//  같은 입력이면 늘 같은 순서가 나온다.
bool ranks_before(double score_a, symbol::SymbolId symbol_a, double score_b, symbol::SymbolId symbol_b,
                  const symbol::SymbolTable& symbols);

// 종합 점수 → 진입 우선순위 랭크(1=최고). 순서는 ranks_before다 — 이름은 동점일 때만 본다.
//  order는 정렬용 작업 버퍼다.
void score_to_rank(const ScoreList& scores, const symbol::SymbolTable& symbols, std::vector<int>& rank,
                   std::vector<size_t>& order);

} // namespace universe
