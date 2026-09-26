// strategy/StrategyLoadPass.h — 전략 로더 파일들이 함께 쓰는 로드 상태와 도우미. 공개 헤더가 아니다.
//  구현은 StrategyFactory.cpp(디스패치·공용 도우미·다른 전략 로더)와 DevScaleLoader.cpp(DEVIATION_SCALE)로 나뉜다.
//  스레드: 전부 메인 스레드(load_strategies 안)에서 부른다.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Types.h"
#include "strategy/StrategyBase.h"
#include "strategy/StrategyFactory.h"

namespace strategy_load
{
struct EntryPriorityMerger;

// ─── 로드 한 번의 공용 상태 ─────────────────────────────────────────────────
//  로더끼리 넘겨야 하는 값을 파일 전역 대신 여기에 둔다. load_strategies가 만들어 로더에 넘기고,
//  돌아가면 사라진다 — 재스캔 람다가 나중에 쓰는 것(priority_merger)만 shared_ptr로 따로 산다.
//  스레드: 메인 스레드(load_strategies 안)만 쓴다.
struct LoadPass : StrategyLoadCtx
{
    explicit LoadPass(const StrategyLoadCtx& base) : StrategyLoadCtx(base) {}

    // 활성 국면 부착 — 한 config 항목이 N개를 등록하는 로더(유니버스·보유 전 종목)에서 하나도 빠지지 않도록,
    //  전략을 추가하는 지점마다 붙인다.
    //  로더를 부르는 동안만 값이 있다(그 밖에서는 nullptr).
    const std::vector<Regime>* pending_regimes = nullptr;

    // 보유분 청산 관리 부착은 슬리브 하나가 아니라 전 슬리브가 등록된 뒤에 한 번만 한다.
    //  scan_covered는 모든 DEVIATION_SCALE 슬리브가 담당하는 종목(id 인덱스 비트)의 합집합이고, 청산 관리 설정은
    //  마지막으로 manage_holdings.enabled를 켠 슬리브의 것을 쓴다(현재 구성은 하나만 켠다).
    std::vector<bool>     scan_covered;
    const nlohmann::json* pending_exit_managers = nullptr; // config 노드를 가리킨다 — load_strategies 안에서만 유효
    bool                  guard_gated = false;
    std::vector<Regime>   guard_regimes;

    // 바스켓 슬리브(TARGET_BASKET)가 소유한 종목(id 인덱스 비트) — DEVSCALE 초기 유니버스와 청산 관리 부착에서 뺀다.
    //  바스켓 로더가 먼저 돌아 채운다 [why D-109].
    std::vector<bool> basket_owned;

    // 전 슬리브의 진입 우선순위 점수표. 재스캔 람다(데이터 스레드)가 들고 가 로드가 끝난 뒤에도 쓴다.
    std::shared_ptr<EntryPriorityMerger> priority_merger;
};

using StrategyMaker = std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)>;

// 종목 id 인덱스 비트를 켠다 — 배열은 종목 테이블 용량만큼 한 번만 늘린다.
void mark_symbol(std::vector<bool>& bits, symbol::SymbolId symbol, size_t capacity);

bool has_symbol(const std::vector<bool>& bits, symbol::SymbolId symbol);

// 활성 국면을 붙여 엔진에 등록한다.
void add_gated(const LoadPass& context, std::unique_ptr<StrategyBase> strategy);

// 재스캔처럼 Engine이 나중에 factory를 직접 부르는 경로용 — 국면을 factory 안에 묶는다.
StrategyMaker gate_factory(const LoadPass& context, StrategyMaker factory);

// 필수 "ticker" 키. 없으면 예외로 기동이 죽는 대신 경고 후 그 항목만 건너뛴다.
bool require_ticker(const nlohmann::json& node, const char* type, std::string& out);

// 전 슬리브의 진입 우선순위 점수표를 만든다(정의는 DevScaleLoader.cpp).
std::shared_ptr<EntryPriorityMerger> make_entry_priority_merger();

// DEVIATION_SCALE 항목 하나를 읽어 등록한다(DevScaleLoader.cpp).
void load_deviation_scale(LoadPass& context, const nlohmann::json& node);

// 키가 있으면 field에 읽고, 없으면 field의 현재 값(구조체 멤버 기본값)을 그대로 둔다.
//  기본값을 로더와 구조체 두 곳에 적지 않으려고 쓴다.
template <typename T>
void read_or_keep(const nlohmann::json& node, const char* key, T& field)
{
    field = node.value(key, field);
}
} // namespace strategy_load
