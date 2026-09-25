// 유니버스 재스캔 장부 — 슬리브마다 스캔 함수를 주기적으로 불러 새 종목에 전략을 붙이고, 스캔에서 연속으로 빠진
//  소유 종목은 신규매수를 막았다가 뗀다. 전략 목록·라우팅·구독은 Engine 몫이라 콜백 둘(등록·떼기)로만 닿는다.
//  Engine 안에서만 돌려 볼 수 있던 교체·차단·복귀 판정을 KIS 없이 시험하려고 뗐다. [why D-077][why D-087]
//
//  스레드: data_thread 전용이다. 예외는 기동 때 스레드 시작 전(add_job·seed·reset_registered)과,
//  register 콜백 안에서 Engine이 다시 부르는 set_registered다. 동기화는 없다.
//  [inv] run이 register 콜백을 부르는 동안 Engine이 set_registered로 되돌아 들어온다 — 반복자를 쥔 채 부르지 않는다.
#pragma once

#include "core/SymbolTable.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class KisClient;
class StrategyBase;

namespace ipc
{
class LedgerSnapshot;
}

class UniverseRescan
{
public:
    // 슬리브 하나당 한 건. 상한(max_registered)은 그 슬리브가 등록한 수로만 센다
    //  — 공유 카운트로 세면 한 슬리브가 다른 슬리브의 자리를 먹는다.
    struct Job
    {
        std::function<std::vector<symbol::SymbolId>(KisClient&)> universe_fn;
        std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)> factory;
        int    interval_sec   = 0;
        size_t max_registered = 0;
        size_t registered     = 0;
        int    drop_after_sec = 0;
        int    block_after_sec = 0;
        int    return_confirm  = 2;
        bool   empty_scan_warned = false; // 빈 스캔 결과 WARN은 연속 구간당 한 번
        std::chrono::steady_clock::time_point last_run{};
        std::vector<symbol::SymbolId> last_scanned; // 마지막 스캔 결과(점수 순) — 구독 칸 우선순위가 순위로 읽는다 [why D-132]
        // 이 슬리브가 소유한 종목(차단·해제 대상) — 종목 id 인덱스. strategy가 nullptr이면 소유가 아니다.
        struct Owned
        {
            StrategyBase* strategy = nullptr;
            // 연속 부재 시계 — 마지막으로 보인 스캔 시각. 첫 부재 스캔에서 직전 스캔 시각으로 놓는다. 0=부재 아님.
            std::chrono::steady_clock::time_point absent_since{};
            int present_streak = 0; // 차단 중 연속 present 스캔 수(복귀 확인)
        };
        std::vector<Owned>            owned;     // add_job이 table.capacity() 크기로 채운다
        std::vector<symbol::SymbolId> owned_ids; // 순회용 소유 id 목록(owned[id].strategy != nullptr인 id 전부)
    };

    // 전략을 전략 목록에 올린다. 받아들였으면 Engine이 그 종목에 set_registered(true)를 부른다.
    using RegisterStrategy = std::function<void(std::unique_ptr<StrategyBase>)>;
    // 전략을 전략 목록에서 뗀다. make_line은 해제 로그 문장.
    using RetireStrategy = std::function<void(StrategyBase*, const std::function<std::string(const StrategyBase&)>&)>;

    UniverseRescan(const symbol::SymbolTable& table, RegisterStrategy register_strategy, RetireStrategy retire_strategy);

    // 슬리브마다 한 번씩 부른다 — 덮어쓰지 않고 쌓는다. owned는 여기서 table.capacity() 크기가 된다.
    void add_job(Job job);
    bool empty() const { return jobs_.empty(); }

    // 기동 유니버스를 마지막 슬리브의 소유로 잡는다. started는 기동 때 올라간 (KR 종목, 그 종목의 전략) 쌍.
    void seed(const std::vector<std::pair<symbol::SymbolId, StrategyBase*>>& started,
              const std::vector<symbol::SymbolId>& symbols);

    // 등록 표시(슬리브 공유, 중복 등록 방지). reset_registered는 기동 때 한 번 — Engine::collect_watch_specifications()가
    //  구독 목록을 모은 뒤, 그 목록의 KR 종목을 set_registered로 표시하기 직전에 부른다.
    void reset_registered();
    void set_registered(symbol::SymbolId symbol, bool on);
    bool is_registered(symbol::SymbolId symbol) const
    {
        return symbol < registered_.size() && registered_[symbol];
    }

    // 주기가 가장 짧은 슬리브의 주기(초). ceiling보다 짧은 것이 없으면 ceiling.
    int shortest_interval_sec(int ceiling) const;

    // 주기가 된 슬리브를 모두 돈다. 한 슬리브라도 스캔 함수를 불렀으면 true — 부른 쪽이 칸 우선순위를 다시 보낸다.
    bool run(KisClient& scan_client, const ipc::LedgerSnapshot& snapshot, std::chrono::steady_clock::time_point now);

    // 종목 id → 가장 앞선 스캔 순위(없으면 -1). 슬리브가 여럿이면 가장 앞선 순위를 쓴다.
    std::vector<int32_t> scan_rank(size_t extent) const;

private:
    // 소유 종목 하나를 떼어 retire 콜백으로 넘긴다 — 점수 교체·이탈 해제가 같은 절차를 쓴다.
    void retire_owned(Job& job, symbol::SymbolId symbol,
                      const std::function<std::string(const StrategyBase&)>& make_line);
    // 슬리브 하나. 주기가 안 됐거나 설정이 비었거나 스캔 함수가 던졌으면 false.
    bool run_job(Job& job, KisClient& scan_client, const ipc::LedgerSnapshot& snapshot,
                 std::chrono::steady_clock::time_point now);

    const symbol::SymbolTable& table_;
    RegisterStrategy           register_strategy_;
    RetireStrategy             retire_strategy_;
    std::vector<Job>           jobs_;
    std::vector<bool>          registered_;           // 등록된 KR 종목(id 인덱스, 중복 방지, 슬리브 공유)
    size_t                     registered_count_ = 0; // registered_의 true 수
};
