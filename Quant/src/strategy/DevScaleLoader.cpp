// strategy/DevScaleLoader.cpp — config의 DEVIATION_SCALE 항목을 읽어 슬리브를 등록한다(단일 종목 또는 스캔 유니버스).
//  스레드: 로더는 메인 스레드(load_strategies 안), DevScaleSleeve의 scan·rescan·make는 초기 등록 때 메인 스레드,
//  그 뒤로는 데이터 스레드(재스캔)가 부른다.
//  관련 결정: D-077(재스캔 차단·해제), D-109(바스켓 소유 종목 제외), D-112(종목 id), D-147(시세판·일봉 데우기).
#include "StrategyLoadPass.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "api/KisClient.h"
#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/UniverseExit.h"
#include "strategy/DevScaleRules.h"
#include "strategy/DeviationScaleStrategy.h"
#include "universe/MarketBoard.h"
#include "universe/ScoreWeight.h"
#include "universe/UniverseScanner.h"
#include "utils/JsonNode.h"
#include "utils/Logger.h"

using json = nlohmann::json;

namespace strategy_load
{
// 진입 우선순위 랭크는 슬리브 하나가 아니라 전 슬리브를 합쳐서 매겨야 한다. 슬리브마다
//  set_entry_priority를 부르면 나중에 스캔한 쪽이 앞 슬리브의 랭크 맵을 통째로 덮어쓰고,
//  랭크를 잃은 종목은 rank=0이 되어 우선순위 바를 건너뛴다. 두 슬리브가 20초 간격으로
//  번갈아 스캔하는 지금 구성에서는 바가 절반만 작동하는 셈이었다.
//  슬리브별 z는 각자의 풀 안에서 정규화된 값이라 슬리브를 넘는 비교는 근사다. 그래도
//  랭크가 통째로 사라지는 것보다는 낫다.
//  슬리브 키는 설정의 id_prefix 문자열 그대로(둘뿐, 스캔당 한 번 찾는다). 종목은 id로 든다.
//  합치기부터 엔진에 넣기까지 한 락 안에서 한다. 두 슬리브가 겹쳐 부르면 먼저 합친 쪽이 늦게 넣어
//  최신 표를 옛 표로 덮을 수 있어서다. 아래 버퍼는 그 락 아래에서만 쓰고 매번 새로 잡지 않는다.
struct EntryPriorityMerger
{
    std::mutex                                 mutex;
    std::map<std::string, universe::ScoreList> by_sleeve;
    std::vector<int32_t>                       slot_by_symbol; // 합칠 때 종목 id → merged 위치(-1=아직 없음)
    universe::ScoreList                        merged;
    std::vector<int>                           rank;
    std::vector<size_t>                        rank_order;     // score_to_rank 작업 버퍼
    std::vector<double>                        z_score;
    std::vector<OrderGate::PriorityEntry>      entries;
};

std::shared_ptr<EntryPriorityMerger> make_entry_priority_merger()
{
    return std::make_shared<EntryPriorityMerger>();
}
} // namespace strategy_load

using namespace strategy_load;

namespace
{
using DevScaleParams = DeviationScaleStrategy::Params;

// 한 슬리브의 스캔이 재스캔마다 다시 쓰는 버퍼. 매번 새로 잡지 않으려고 슬리브가 들고 있다.
//  락이 없다 — 한 슬리브의 스캔은 한 번에 하나씩만 돈다(초기 스캔은 엔진 시작 전 메인 스레드, 재스캔은
//  그 뒤 데이터 스레드). [inv] DevScaleSleeve의 scan·rescan 밖에서는 건드리지 않는다.
struct DevScaleScanBuffers
{
    universe::QuoteTable quotes;     // 전 종목 시세 표(종목 id 인덱스). 칸 비우기·다시 채우기는 load_quote_table
    std::vector<double>  multiplier; // scores 순서의 비중 배수
    std::vector<double>  sorted_raw; // score_to_mult 작업 버퍼
    std::vector<double>  krw;        // scores 순서의 종목당 명목(원)
    std::vector<bool>    held;       // 재스캔 때 보유·바스켓 종목 표시(종목 id 인덱스)
};

// 점수를 얼마를 사는가(배수·원)로 바꾸는 설정.
//  spread는 최상위/최하위 배수 폭, target은 베이스 명목 총합 목표. 베이스 총합(base_percent x 슬롯)이 총노출 상한을
//  넘으면 매수가 무더기로 거부되므로 스캔이 매회 슬롯 수 기준으로 배수를 재정규화한다.
//  원 단위 사이징은 floor·cap이 둘 다 0보다 크면 자본%·정규화 배수 대신 이 구간을 쓴다(D-036).
struct DevScaleSizing
{
    double weight_spread = 0.6;  // [0, 1]로 자른 값
    double weight_target = 0.80;
    double weight_base   = 0.05; // 슬리브의 base_percent
    double krw_floor     = 0.0;  // 원, 종목당 바닥
    double krw_cap       = 0.0;  // 원, 종목당 천장
    double krw_cap_z     = 1.5;  // 천장에 닿는 점수 z

    bool krw_on() const
    {
        return krw_floor > 0.0 && krw_cap >= krw_floor;
    }
};

// 주기적 재스캔의 시계. 판정은 core/UniverseExit.h [why D-077].
struct RescanPolicy
{
    int rescan_sec      = 600;  // 재스캔 간격
    int drop_after_sec  = 1800; // 이만큼 연속으로 빠진 종목의 전략을 뗀다(보유·선점 없을 때만). 0=안 뗌
    int block_after_sec = 0;    // 같은 시계로 이만큼 빠지면 떼기 전에 신규매수부터 막는다. 0=안 막음
    int return_confirm  = 2;    // 막힌 종목은 present 스캔이 이 횟수 연속일 때만 푼다
};

// 기동 때 잔고로 가린 스캔 제외 종목.
struct HeldSnapshot
{
    std::vector<bool>             held;       // 스캔에서 뺄 종목(보유분·바스켓 소유, 종목 id 인덱스)
    std::vector<symbol::SymbolId> reinstated; // 보유 중인 최근 매수분 — 스캔에 없어도 등록하고 청산 관리는 안 붙인다
};

// 점수 z → 종목당 명목(원). z≤0은 바닥, z≥cap_z는 천장, 사이는 직선. 결과는 scores와 같은 순서.
//  [formula] krw = floor + (capture − floor) × clamp(z / cap_z, 0, 1)
//  천장은 "풀 안에서 확실히 강하다"(z)만 본다. 절대 산포 조건(모두 비슷한 장에서는 천장을 닫는
//  것)은 점수 이력이 쌓인 뒤 붙인다 — 지금은 이력이 없어 임계를 정할 근거가 없다.
void score_to_krw(const universe::ScoreList& scores, double floor_krw, double cap_krw, double cap_z,
                  std::vector<double>& krw)
{
    universe::score_to_z(scores, krw);

    for (double& value : krw)
    {
        const double factor = cap_z > 0.0 ? (std::max)(0.0, (std::min)(1.0, value / cap_z)) : 0.0;
        value               = floor_krw + (cap_krw - floor_krw) * factor;
    }
}

// 한 슬리브의 점수를 갱신하고, 전 슬리브를 합친 랭크를 엔진에 넣는다.
//  scores는 sink — 슬리브 표에 옮겨 넣는다.
//  [lock-order] merger.mutex를 쥔 채 engine.set_entry_priority를 부른다. 그 안의 잠금은 파일 쓰기용 하나뿐이고
//  그쪽에서 merger를 다시 부르지 않으므로 잠금 순서가 뒤집히지 않는다.
void publish_entry_priority(Engine& engine, EntryPriorityMerger& merger, const std::string& sleeve,
                            universe::ScoreList scores)
{
    std::lock_guard<std::mutex> lock(merger.mutex);
    universe::ScoreList&        merged = merger.merged;
    merged.clear();
    merger.by_sleeve[sleeve] = std::move(scores);

    if (merger.slot_by_symbol.size() < engine.symbols().capacity())
    {
        merger.slot_by_symbol.assign(engine.symbols().capacity(), -1);
    }

    for (const auto& sleeve_entry : merger.by_sleeve)
    {
        for (const universe::SymbolScore& entry : sleeve_entry.second)
        {
            if (entry.symbol == symbol::kNone || entry.symbol >= merger.slot_by_symbol.size())
            {
                continue;
            }

            int32_t& slot = merger.slot_by_symbol[entry.symbol];

            // 같은 종목이 두 슬리브에 올라오면 높은 점수를 남긴다.
            if (slot < 0)
            {
                slot = static_cast<int32_t>(merged.size());
                merged.push_back(entry);
            }
            else if (entry.score > merged[static_cast<size_t>(slot)].score)
            {
                merged[static_cast<size_t>(slot)].score = entry.score;
            }
        }
    }

    for (const universe::SymbolScore& entry : merged) // 다음 호출을 위해 건드린 칸만 되돌린다
    {
        merger.slot_by_symbol[entry.symbol] = -1;
    }

    universe::score_to_rank(merged, engine.symbols(), merger.rank, merger.rank_order);
    universe::score_to_z(merged, merger.z_score);
    merger.entries.clear();

    for (size_t index = 0; index < merged.size(); ++index)
    {
        merger.entries.push_back({merged[index].symbol, merger.rank[index], merger.z_score[index]});
    }

    engine.set_entry_priority(merger.entries, static_cast<int>(merged.size()));
}

void drop_marked(std::vector<symbol::SymbolId>& scanned, const std::vector<bool>& marks)
{
    std::erase_if(scanned, [&marks](symbol::SymbolId symbol)
    {
        return has_symbol(marks, symbol);
    });
}

// ─── 슬리브 ─────────────────────────────────────────────────────────────────
//  한 DEVIATION_SCALE 항목의 설정과 재스캔 사이에 이어지는 상태를 한 객체에 둔다. 엔진에 넘기는 재스캔 함수와
//  전략 생성 함수는 이 객체의 shared_ptr 하나만 붙잡는다.
//  engine_ 참조의 수명: 슬리브를 붙잡는 함수는 engine에 저장되어(set_universe_rescan) engine 생존 중에만 불린다.
class DevScaleSleeve
{
public:
    DevScaleSleeve(Engine& engine, DevScaleParams parameters, universe::DevScanCfg scan_config, DevScaleSizing sizing,
                   std::shared_ptr<EntryPriorityMerger> merger);

    const DevScaleParams&       parameters() const
    {
        return parameters_;
    }

    const universe::DevScanCfg& scan_config() const
    {
        return scan_config_;
    }

    // 종목 하나의 전략을 만든다(초기 등록·재스캔 공용).
    std::unique_ptr<StrategyBase> make(symbol::SymbolId symbol) const;

    // 기동 등록용 — load_strategies는 engine.start()(bootstrap_ledger 포함) 전에 돌아 OrderGate 원장이 아직
    //  비어 있다. 기동 때 직접 조회한 잔고 스냅샷으로 거른다.
    std::vector<symbol::SymbolId> scan_initial(KisClient& kis, const HeldSnapshot& snapshot);

    // 주기적 재스캔용 — 매회 OrderGate 원장에서 현재 보유를 다시 읽는다. 청산 관리가 청산한
    //  종목은 그 시점부터 다시 후보가 된다(기동 스냅샷 고정이 유니버스를 굳히던 문제).
    //  이미 등록된 종목은 재스캔이 추가만 하므로 자기 보유분으로 등록이 풀리진 않는다.
    std::vector<symbol::SymbolId> rescan(KisClient& kis);

private:
    std::vector<symbol::SymbolId> scan(KisClient& kis);
    void                          store_sizes(const universe::ScoreList& scores);

    Engine&                              engine_;
    const DevScaleParams                 parameters_;
    const universe::DevScanCfg           scan_config_;
    const DevScaleSizing                 sizing_;
    std::shared_ptr<EntryPriorityMerger> merger_;

    // 스캔 점수로 정한 종목별 비중 배수·명목(원). 전략을 만들 때 한 번만 읽는다 — 분할 매수 도중에 예산이
    //  바뀌면 남은 층 예산과 평단이 어긋나서, 이미 등록된 전략의 배수는 갱신하지 않는다. 재스캔으로 새로 붙는
    //  종목만 최신 배수를 받는다. 두 배열 다 종목 id 인덱스(0=값 없음), 크기는 종목 테이블 용량. [why D-112]
    mutable std::mutex  size_mutex_;
    std::vector<double> multiplier_by_symbol_;
    std::vector<double> krw_by_symbol_; // 원 사이징이 켜진 슬리브만 채운다

    DevScaleScanBuffers buffers_;
};

DevScaleSleeve::DevScaleSleeve(Engine& engine, DevScaleParams parameters, universe::DevScanCfg scan_config,
                               DevScaleSizing sizing, std::shared_ptr<EntryPriorityMerger> merger)
    : engine_(engine),
      parameters_(std::move(parameters)),
      scan_config_(std::move(scan_config)),
      sizing_(sizing),
      merger_(std::move(merger)),
      multiplier_by_symbol_(engine.symbols().capacity(), 0.0),
      krw_by_symbol_(engine.symbols().capacity(), 0.0)
{
}

// 스캔이 register_ticker_name으로 이름을 먼저 등록하므로 여기서 조회해 전략에 주입한다 → 로그에 "티커(종목명)"
//  노출(id()·데이터키는 티커 그대로).
std::unique_ptr<StrategyBase> DevScaleSleeve::make(symbol::SymbolId symbol) const
{
    DevScaleParams deviation_parameters = parameters_;
    deviation_parameters.ticker = engine_.symbols().name(symbol).string(); // 전략 파라미터·id()는 아직 문자열
    deviation_parameters.name   = engine_.ticker_name(symbol);

    if (symbol < multiplier_by_symbol_.size())
    {
        std::lock_guard<std::mutex> lock(size_mutex_);

        if (multiplier_by_symbol_[symbol] > 0.0)
        {
            deviation_parameters.size_mult = multiplier_by_symbol_[symbol];
        }

        if (krw_by_symbol_[symbol] > 0.0)
        {
            deviation_parameters.notional_krw = krw_by_symbol_[symbol];
        }
    }

    return std::make_unique<DeviationScaleStrategy>(std::move(deviation_parameters));
}

std::vector<symbol::SymbolId> DevScaleSleeve::scan(KisClient& kis)
{
    // 스캐너가 응답의 문자열 티커를 종목 테이블에 한 번 넣고 id·이름·점수를 준다.
    universe::ScanResult scan = universe::scan_devscale(kis, scan_config_, engine_.symbols(), buffers_.quotes);

    for (size_t index = 0; index < scan.symbols.size(); ++index)
    {
        engine_.register_ticker_name(scan.symbols[index], scan.names[index]);
    }

    store_sizes(scan.scores);
    publish_entry_priority(engine_, *merger_, parameters_.id_prefix, std::move(scan.scores));
    return std::move(scan.symbols);
}

// 점수의 두 가지 용도 — (a) 누가 먼저 슬롯을 차지하는가(랭크, publish_entry_priority), (b) 얼마를 사는가(여기).
void DevScaleSleeve::store_sizes(const universe::ScoreList& scores)
{
    universe::score_to_mult(scores, sizing_.weight_spread, sizing_.weight_target, sizing_.weight_base,
                            engine_.risk_max_positions(), buffers_.multiplier, buffers_.sorted_raw);
    const std::vector<double>&  multiplier = buffers_.multiplier;
    std::lock_guard<std::mutex> lock(size_mutex_);

    // make()는 전략 생성 시점에 한 번만 읽으므로, 여기서 값을 덮어써도
    //  이미 분할 매수를 타는 전략의 예산은 흔들리지 않는다(신규 등록분에만 반영).
    for (size_t index = 0; index < scores.size(); ++index)
    {
        const symbol::SymbolId symbol = scores[index].symbol;

        if (symbol < multiplier_by_symbol_.size())
        {
            multiplier_by_symbol_[symbol] = multiplier[index];
        }
    }

    if (!sizing_.krw_on())
    {
        return;
    }

    std::vector<double>& krw = buffers_.krw;
    score_to_krw(scores, sizing_.krw_floor, sizing_.krw_cap, sizing_.krw_cap_z, krw);

    for (size_t index = 0; index < scores.size(); ++index)
    {
        const symbol::SymbolId symbol = scores[index].symbol;

        if (symbol < krw_by_symbol_.size())
        {
            krw_by_symbol_[symbol] = krw[index];
        }
    }
}

std::vector<symbol::SymbolId> DevScaleSleeve::scan_initial(KisClient& kis, const HeldSnapshot& snapshot)
{
    std::vector<symbol::SymbolId> scanned = scan(kis);
    drop_marked(scanned, snapshot.held);

    for (const symbol::SymbolId symbol : snapshot.reinstated) // 오늘 스캔에서 빠졌어도 보유분이라 맡는다
    {
        if (std::find(scanned.begin(), scanned.end(), symbol) == scanned.end())
        {
            scanned.push_back(symbol);
        }
    }

    return scanned;
}

std::vector<symbol::SymbolId> DevScaleSleeve::rescan(KisClient& kis)
{
    std::vector<symbol::SymbolId> scanned = scan(kis);
    std::vector<bool>&            current = buffers_.held;
    current.assign(engine_.symbols().capacity(), false);

    for (const auto& held_position : engine_.held_positions())
    {
        mark_symbol(current, held_position.symbol, current.size());
    }

    for (const symbol::SymbolId basket_symbol : engine_.slot_exempt_symbols()) // 바스켓 소유 종목(파일이 바뀌면 여기서 따라온다) [why D-109]
    {
        mark_symbol(current, basket_symbol, current.size());
    }

    drop_marked(scanned, current);
    return scanned;
}

// ─── 설정 읽기 ──────────────────────────────────────────────────────────────
//  기본값은 DeviationScaleStrategy::Params·universe::DevScanCfg의 멤버 기본값 한 곳에 둔다(read_or_keep).

// 공통 파라미터(티커 제외) — 스캔 유니버스/단일 종목이 함께 쓴다.
DevScaleParams parse_devscale_parameters(const json& node)
{
    DevScaleParams parameters;
    read_or_keep(node, "base_pct", parameters.base_percent);
    read_or_keep(node, "max_pct", parameters.max_percent);
    read_or_keep(node, "fallback_equity", parameters.fallback_equity);
    read_or_keep(node, "base_qty", parameters.base_quantity);
    read_or_keep(node, "step_qty", parameters.step_quantity);
    read_or_keep(node, "sma_period", parameters.simple_moving_average_period);
    read_or_keep(node, "dev_sell_pct", parameters.deviation_sell);
    read_or_keep(node, "dev_buy_pct", parameters.deviation_buy);
    read_or_keep(node, "split_step_count", parameters.split_step_count);
    read_or_keep(node, "add_below_sma_only", parameters.add_below_simple_moving_average_only);
    read_or_keep(node, "daily_basis_warmup", parameters.daily_basis_warmup);
    read_or_keep(node, "split_buy_cross_guard", parameters.cross_guard);
    read_or_keep(node, "pullback_pct", parameters.pullback_percent);
    read_or_keep(node, "entry_upper_pct", parameters.entry_upper_percent);
    read_or_keep(node, "zone_hyst_pct", parameters.zone_hysteresis_percent);
    // 정배열 허용오차는 스캐너와 같은 값을 써야 등록·활성이 어긋나지 않는다 — parse_scan_config가 이 값을 넘겨받는다.
    read_or_keep(node, "align_ma_tol_pct", parameters.align_moving_average_tolerance_percent);
    read_or_keep(node, "reprice_move_ticks", parameters.reprice_move_ticks);
    read_or_keep(node, "min_rebuild_sec", parameters.min_rebuild_sec);
    read_or_keep(node, "id_prefix", parameters.id_prefix);
    read_or_keep(node, "buy_split_steps", parameters.buy_split_steps);
    read_or_keep(node, "stop_loss_pct", parameters.stop_loss_percent);
    read_or_keep(node, "stop_cooldown_sec", parameters.stop_cooldown_sec);
    read_or_keep(node, "reentry_cooldown_sec", parameters.reentry_cooldown_sec);
    read_or_keep(node, "dust_krw", parameters.dust_krw);
    read_or_keep(node, "sell_base_average", parameters.sell_base_average);
    read_or_keep(node, "trail_arm_pct", parameters.trail_arm_percent);
    read_or_keep(node, "trail_pct", parameters.trail_percent);
    read_or_keep(node, "entry_atr_max_pct", parameters.entry_atr_max_percent);
    read_or_keep(node, "entry_open_dev_min_pct", parameters.entry_open_deviation_min_percent);
    read_or_keep(node, "entry_open_dev_max_pct", parameters.entry_open_deviation_max_percent);
    read_or_keep(node, "prefetch_jitter_pct", parameters.prefetch_jitter_percent);
    read_or_keep(node, "bar_source", parameters.bar_source); // "ws"|"rest" (D-069·D-072)
    read_or_keep(node, "market_close_exit_hhmm", parameters.market_close_hhmm);
    read_or_keep(node, "interval_min", parameters.interval_min);
    read_or_keep(node, "min_action_ms", parameters.min_action_ms);
    read_or_keep(node, "daily_lookback", parameters.daily_lookback);
    read_or_keep(node, "account", parameters.account);
    return parameters;
}

// 스캔 유니버스 설정. 1단(스캐너): 시세 표 거래대금 상위 + KIS 거래대금·거래증가율 상위 + 업종 축의 합집합을
//  최소·최대가 필터로 압축 + 정배열 프리필터. 시총 축은 D-146에서 뺐다.
//  2단(전략): 등록된 각 DeviationScale이 자기 일봉으로 정배열+눌림 존을 판정해 자격 종목만 실제로 매매한다.
universe::DevScanCfg parse_scan_config(const json& node, const DevScaleParams& parameters)
{
    universe::DevScanCfg config;
    // 거래대금 상위 스캔 수(장중 급변). 30을 넘기면 가격 구간을 갈라 두 번 부르므로 REST 호출이
    //  하나 는다 — 그 대신 ETF를 뺀 개별주를 60종목까지 볼 수 있다.
    read_or_keep(node, "value_top_n", config.value_top_n);
    read_or_keep(node, "turnover_top_n", config.turnover_top_n);
    read_or_keep(node, "min_price", config.min_price);
    read_or_keep(node, "max_price", config.max_price);
    read_or_keep(node, "max_universe", config.max_register);
    read_or_keep(node, "risk_off_index_pct", config.risk_off_index);
    read_or_keep(node, "kosdaq_enabled", config.kosdaq_enabled); // 백테스트 통과 후 개방
    read_or_keep(node, "risk_off_index_pct_kosdaq", config.risk_off_index_kosdaq);
    // 재개 임계와 최소 체류 — 차단 임계와 갈라 두어 경계 근처 토글을 없앤다. [why D-033]
    read_or_keep(node, "risk_off_resume_pct", config.risk_off_index_resume);
    read_or_keep(node, "risk_off_resume_pct_kosdaq", config.risk_off_index_kosdaq_resume);
    read_or_keep(node, "risk_off_dwell_sec", config.risk_off_dwell_sec);
    read_or_keep(node, "require_aligned", config.require_aligned);
    read_or_keep(node, "align_lookup_max", config.align_lookup_max);

    // 업종 순위 축 — 업종 코드, 업종마다 상위 몇 행, 최소 등락률.
    for (const auto& element : jsonx::array_or_empty(node, "sector_codes"))
    {
        if (element.is_string())
        {
            config.sector_codes.push_back(element.get<std::string>());
        }
    }

    read_or_keep(node, "sector_top_n", config.sector_top_n);
    read_or_keep(node, "sector_min_chg", config.sector_min_change);
    // 후보 합집합(KIS 랭킹·업종 REST) 갱신 주기. 미지정이면 재스캔 주기와 같아
    //  기존 동작(재스캔마다 새로 수집)이 유지된다.
    read_or_keep(node, "union_refresh_sec", config.union_refresh_sec);
    read_or_keep(node, "max_dev_pct", config.max_deviation_percent);
    read_or_keep(node, "universe_file", config.universe_file); // 거래대금 상위 종목 파일(시세판이 씀), 비면 KIS 랭킹만
    read_or_keep(node, "market_board", config.market_board);
    read_or_keep(node, "min_turnover", config.min_turnover);
    read_or_keep(node, "full_market", config.full_market);
    read_or_keep(node, "daily_warm_until_hhmm", config.daily_warm_until_hhmm);
    // 횡단면 스코어러(2026-08-09 회의 Task 4) — score_top_n>0이면 정배열 통과분을 점수 랭킹해 상위 N만 등록. 0=전체 등록.
    read_or_keep(node, "score_top_n", config.score_top_n);
    read_or_keep(node, "score_w_trend", config.score_weight_trend);
    read_or_keep(node, "score_w_pullback", config.score_weight_pullback);
    read_or_keep(node, "score_w_vol", config.score_weight_volume);
    read_or_keep(node, "score_w_liquidity", config.score_weight_liquidity);
    config.align_daily_n                          = parameters.daily_lookback;
    config.align_moving_average_tolerance_percent = parameters.align_moving_average_tolerance_percent;
    return config;
}

// 엔진 안 시세판과 장 전 일봉 데우기를 띄운다. 둘 다 슬리브가 여럿이어도 프로세스에 한 번만 뜬다. [why D-147]
void start_scan_services(const LoadPass& context, const json& node, const universe::DevScanCfg& scan_config)
{
    if (!scan_config.market_board)
    {
        return;
    }

    universe::MarketBoard::Config board_config;
    read_or_keep(node, "market_board_period_sec", board_config.period_sec);
    read_or_keep(node, "market_board_rerank_sec", board_config.rerank_sec);
    read_or_keep(node, "market_board_n_market_value", board_config.n_market_value);
    read_or_keep(node, "market_board_n_turnover", board_config.n_turnover);
    read_or_keep(node, "market_board_min_turnover", board_config.min_turnover);
    board_config.universe_out = scan_config.universe_file; // 알림·대시보드·백필 스크립트가 이 파일을 읽는다
    universe::MarketBoard::instance().start(board_config);

    if (scan_config.daily_warm_until_hhmm > 0 && context.has_quote_kis)
    {
        universe::start_daily_warm(context.quote_kis_config, scan_config);
    }
}

RescanPolicy parse_rescan_policy(const json& node)
{
    RescanPolicy policy;
    read_or_keep(node, "rescan_interval_sec", policy.rescan_sec);
    read_or_keep(node, "rescan_drop_after_sec", policy.drop_after_sec);
    read_or_keep(node, "rescan_return_confirm", policy.return_confirm);

    const int block_config = node.value("rescan_block_after_sec", policy.block_after_sec);
    policy.block_after_sec = universe_exit::clamp_block(block_config, policy.drop_after_sec);

    if (policy.block_after_sec != block_config)
    {
        LOG_WARN("[Main] rescan_block_after_sec " + std::to_string(block_config) + "이 rescan_drop_after_sec " +
                 std::to_string(policy.drop_after_sec) + "보다 커서 해제 시각에 맞춘다");
    }

    return policy;
}

DevScaleSizing parse_sizing(const json& node, const DevScaleParams& parameters)
{
    DevScaleSizing sizing;
    sizing.weight_base = parameters.base_percent;

    // spread는 [0, 1]로 자른다. 1을 넘으면 점수 하위 쪽 배수가 음수가 되고, make()는 0 이하 배수를 버리고
    //  기본 배수 1.0으로 되돌아가 하위 종목을 도리어 크게 산다.
    const double weight_spread_config = node.value("weight_spread", sizing.weight_spread);
    sizing.weight_spread              = weight_spread_config;

    if (sizing.weight_spread > 1.0)
    {
        sizing.weight_spread = 1.0;
    }
    else if (!(sizing.weight_spread >= 0.0)) // 음수와 NaN
    {
        sizing.weight_spread = 0.0;
    }

    if (sizing.weight_spread != weight_spread_config)
    {
        LOG_WARN("[Main] " + parameters.id_prefix + " weight_spread " + std::to_string(weight_spread_config) +
                 "는 0~1 밖이라 " + std::to_string(sizing.weight_spread) + "로 자른다");
    }

    read_or_keep(node, "weight_target_pct", sizing.weight_target);
    read_or_keep(node, "notional_floor_krw", sizing.krw_floor);
    read_or_keep(node, "notional_cap_krw", sizing.krw_cap);
    read_or_keep(node, "notional_cap_z", sizing.krw_cap_z);

    if (sizing.krw_on())
    {
        LOG_INFO("[Main] " + parameters.id_prefix + " 원 사이징: 종목당 " +
                 std::to_string(static_cast<long long>(sizing.krw_floor)) + "~" +
                 std::to_string(static_cast<long long>(sizing.krw_cap)) + "원, 천장 z>=" +
                 std::to_string(sizing.krw_cap_z));
    }

    return sizing;
}

// 이 슬리브(id_prefix)가 오늘부터 lookback_days일 전(달력일)까지 산 종목 — 체결 원장 logs/trades_YYYYMMDD.csv
//  (OrderRouter가 쓴다)의 FILL·BUY 행. lookback_days 0이면 오늘 원장 하나만 본다.
//  재기동 때 보유분을 전부 청산 관리(ITB)로 넘기면 당일 매수분도 익절선 없이 트레일에만 걸린다(09-04~18 승계 매도
//  725체결 −190만). 분할 매수가 없으면(buy_split_steps 0) 명목 상한 초과 위험이 없어 DevScale이 그대로 맡는다.
//  파일이 없거나(첫 기동) 못 읽으면 빈 집합 — 그때는 기존대로 청산 관리가 맡는다.
//  넘김 모드(market_close_exit_hhmm 2400)는 전날 산 것도 DevScale 보유라 호출자가 lookback_days 20을 준다.
std::set<std::string> tickers_bought_recently(const std::string& id_prefix, int lookback_days)
{
    std::set<std::string> bought;
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    constexpr std::time_t kSecondsPerDay = 86400;

    for (int day_offset = 0; day_offset <= lookback_days; ++day_offset)
    {
        const std::string date = kst::date_yyyymmdd(now - static_cast<std::time_t>(day_offset) * kSecondsPerDay);
        std::ifstream ledger(Logger::instance().path_for("trades_" + date + ".csv"));
        bought.merge(devscale_rules::tickers_bought_from_ledger(ledger, id_prefix));
    }

    return bought;
}

bool manage_holdings_enabled(const json& node)
{
    const auto iterator = node.find("manage_holdings");
    return iterator != node.end() && iterator->value("enabled", false);
}

// ─── 등록 ───────────────────────────────────────────────────────────────────

// 이미 보유 중인 종목은 신규 스캔에서 제외 → 청산 관리가 전담(윈드다운).
//  시드/전일 물린 보유분에 DevScale 분할 매수가 겹치면 종목당 명목상한(max_percent)을
//  초과해 CANCEL 거부·과주문이 난다(073240 사례). 보유분=청산 관리, 신규만=DevScale로 분리.
//  manage_holdings.enabled일 때만 잔고를 본다(청산 관리가 있어야 보유분을 인수하므로).
//  바스켓 소유 종목은 그와 무관하게 처음부터 이 슬리브의 후보가 아니다 [why D-109].
HeldSnapshot snapshot_holdings(const LoadPass& context, const DevScaleParams& parameters, bool manage_holdings)
{
    Engine&      engine          = context.engine;
    const size_t symbol_capacity = engine.symbols().capacity();
    HeldSnapshot snapshot;
    snapshot.held = context.basket_owned;
    snapshot.held.resize(std::max(snapshot.held.size(), symbol_capacity), false);

    if (!manage_holdings)
    {
        return snapshot;
    }

    KisClient held_kis(context.kis_config);

    if (!held_kis.authenticate())
    {
        LOG_WARN("[Main] DEVSCALE: 보유분 조회 인증 실패 — 스캔 제외 미적용(중복 위험)");
        return snapshot;
    }

    size_t                          held_count = 0;
    const KisResult<AccountBalance> balance    = held_kis.get_balance();

    if (!balance)
    {
        LOG_WARN("[Main] DEVSCALE: 보유분 조회 실패(" + error_text(balance) + ") — 스캔 제외 미적용(중복 위험)");
    }
    else
    {
        for (const Holding& holding : balance->holdings)
        {
            mark_symbol(snapshot.held, engine.symbols().intern(holding.ticker), symbol_capacity); // 잔고 티커는 문자열
            ++held_count;
        }
    }

    // 당일 매수분은 DevScale이 다시 맡는다(재인수). 분할 매수가 있으면 기존대로 청산 관리에 넘긴다.
    //  넘김 모드면 최근 20일 원장까지 봐서 전날 넘긴 보유도 되찾는다.
    const bool carry_over = parameters.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm;

    if (parameters.buy_split_steps == 0)
    {
        const int lookback_days = carry_over ? 20 : 0;

        for (const std::string& ticker : tickers_bought_recently(parameters.id_prefix, lookback_days))
        {
            const symbol::SymbolId symbol = engine.symbols().intern(ticker); // 원장 CSV의 문자열 티커 — 여기서 id가 된다

            if (has_symbol(snapshot.held, symbol) && !has_symbol(context.basket_owned, symbol)) // 바스켓 것은 바스켓이 인수한다 [why D-109]
            {
                snapshot.held[symbol] = false;
                --held_count;
                snapshot.reinstated.push_back(symbol);
            }
        }
    }

    LOG_INFO("[Main] DEVSCALE: 보유분 " + std::to_string(held_count) + "종목 스캔 제외(청산 관리 전담), " +
             (carry_over ? "최근 매수분 " : "당일 매수분 ") + std::to_string(snapshot.reinstated.size()) + "종목 재인수");
    return snapshot;
}

// 스캔으로 초기 유니버스를 등록하고 주기적 재스캔을 엔진에 건다. 등록한 종목을 covered에 켠다.
//  data_thread가 rescan_sec마다 재스캔해 신규 티커를 런타임 add하고, drop_after_sec 이상 빠져 있는 티커는 뗀다.
//  인증에 실패해도 재스캔은 엔진 내부 시세 클라이언트로 시도한다.
void register_scan_universe(LoadPass& context, const std::shared_ptr<DevScaleSleeve>& sleeve,
                            const HeldSnapshot& snapshot, const RescanPolicy& policy, std::vector<bool>& covered)
{
    Engine&               engine    = context.engine;
    const DevScaleParams& parameters    = sleeve->parameters();
    std::vector<symbol::SymbolId> seeded; // 기동 등록 종목 — 재스캔 슬리브의 소유로 넘겨 차단·해제 대상에 넣는다

    if (!context.has_quote_kis)
    {
        LOG_ERROR("[Main] DEVSCALE universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
        return;
    }

    KisClient scan_kis(context.quote_kis_config);

    if (!scan_kis.authenticate())
    {
        LOG_ERROR("[Main] DEVSCALE universe_from_scan: 시세 키 인증 실패 — 건너뜀");
    }
    else
    {
        for (const symbol::SymbolId symbol : sleeve->scan_initial(scan_kis, snapshot))
        {
            add_gated(context, sleeve->make(symbol));
            mark_symbol(covered, symbol, covered.size()); // 청산 관리 중복 부착 방지용
            LOG_INFO("[Main]   + " + parameters.id_prefix + " 초기 " + engine.symbols().name(symbol).string());
            seeded.push_back(symbol);
        }

        LOG_INFO("[Main] " + parameters.id_prefix + " universe_from_scan: 초기 " + std::to_string(seeded.size()) +
                 "종목 등록 (각자 정배열+눌림 존 게이트로 자체 선별)");
    }

    // 등록 총수 상한은 스캔 1회 상한(max_universe)과 같게 둔다 — 해제가 느리게 따라오므로
    //  상한이 없으면 총수가 그 값을 넘어 는다.
    engine.set_universe_rescan([sleeve](KisClient& kis)
    {
        return sleeve->rescan(kis);
    },
                               gate_factory(context, [sleeve](symbol::SymbolId symbol)
                               {
                                   return sleeve->make(symbol);
                               }),
                               policy.rescan_sec, static_cast<size_t>(sleeve->scan_config().max_register),
                               policy.drop_after_sec, policy.block_after_sec, policy.return_confirm);
    engine.seed_universe_rescan(seeded);
    LOG_INFO("[Main] " + parameters.id_prefix + " 주기적 재스캔 활성: " + std::to_string(policy.rescan_sec) +
             "초 간격, 이탈 차단 " + std::to_string(policy.block_after_sec) + "초(복귀 확인 " +
             std::to_string(policy.return_confirm) + "회), 해제 " + std::to_string(policy.drop_after_sec) + "초");
}

// 보유분 청산 관리 — 스캔에 안 잡힌 잔고 보유분에 청산 전용 ITB 부착(옵션).
//  여기서 바로 붙이지 않고 전 슬리브 로드가 끝난 뒤에 붙인다. 이 슬리브의 covered만 보면
//  뒤에 로드되는 슬리브가 방금 산 종목이 "스캔 밖 보유분"으로 보여 청산 관리가
//  겹쳐 붙고, 그 청산 관리의 seed-trail 매도가 슬리브의 잔여 매도와 같은 주식을 두고
//  경합한다(09-11 09:26~ ITB_112610·267250·014530 매도가능수량 0 거부 반복).
void hand_off_exit_managers(LoadPass& context, const json& node, const std::vector<bool>& covered,
                            bool manage_holdings)
{
    if (context.scan_covered.size() < covered.size())
    {
        context.scan_covered.resize(covered.size(), false);
    }

    for (size_t symbol = 0; symbol < covered.size(); ++symbol)
    {
        if (covered[symbol])
        {
            context.scan_covered[symbol] = true;
        }
    }

    if (manage_holdings)
    {
        context.pending_exit_managers = &node["manage_holdings"];
        context.guard_gated           = (context.pending_regimes != nullptr);
        context.guard_regimes         = context.guard_gated ? *context.pending_regimes : std::vector<Regime>{};
    }
}
} // namespace

namespace strategy_load
{
void load_deviation_scale(LoadPass& context, const json& node)
{
    Engine&              engine          = context.engine;
    const DevScaleParams parameters          = parse_devscale_parameters(node);
    const bool           manage_holdings = manage_holdings_enabled(node);
    const HeldSnapshot   snapshot        = snapshot_holdings(context, parameters, manage_holdings);
    // 스캔/단일로 실제 DeviationScale이 담당하는 종목(id 인덱스 비트) — 보유분 청산 관리 중복 부착 방지.
    std::vector<bool> covered(engine.symbols().capacity(), false);

    if (node.value("universe_from_scan", false))
    {
        universe::DevScanCfg scan_config = parse_scan_config(node, parameters);
        start_scan_services(context, node, scan_config);
        const RescanPolicy   policy = parse_rescan_policy(node);
        const DevScaleSizing sizing = parse_sizing(node, parameters);
        auto sleeve = std::make_shared<DevScaleSleeve>(engine, parameters, std::move(scan_config), sizing,
                                                       context.priority_merger);
        register_scan_universe(context, sleeve, snapshot, policy, covered);
    }
    else
    {
        std::string ticker;

        if (!require_ticker(node, "DEVSCALE", ticker))
        {
            return;
        }

        const DevScaleSleeve   sleeve(engine, parameters, universe::DevScanCfg{}, DevScaleSizing{}, context.priority_merger);
        const symbol::SymbolId symbol = engine.symbols().intern(ticker); // 설정의 문자열 티커는 여기서 id가 된다
        add_gated(context, sleeve.make(symbol));
        mark_symbol(covered, symbol, covered.size());
    }

    hand_off_exit_managers(context, node, covered, manage_holdings);
}
} // namespace strategy_load
