// 종목 id → 그 종목을 보는 전략 목록. 전략 스레드가 틱마다 전략 전부를 돌며 각자 문자열을 비교하던 것을
//  id 배열 인덱스 한 번으로 바꾼다(원칙 6). 구독 종목을 안 밝힌 전략(get_watch_specifications가 빈 것)은 전부 받는다 —
//  오늘과 같은 동작이다.
// 스레드: 소유 샤드 스레드 전용. 전략 목록이 바뀔 때(strategy_.version)만 다시 만든다 — hot path에서 문자열을 보지 않는다.
//  [why D-071]
#pragma once

#include "core/SymbolTable.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace strategy
{

class Router
{
public:
    // symbol_id_of: 종목 문자열 → id. 처음 보는 종목은 여기서 id를 받는다(기동·재스캔 시점이라 hot path가 아니다).
    //  kNone을 돌려주면(테이블이 찼다) 그 전략은 전부 받는 쪽으로 보낸다 — 틱을 놓치는 것보다 낫다.
    template <class SymbolIdOf>
    void rebuild(const std::vector<StrategyBase*>& strategies, SymbolIdOf&& symbol_id_of)
    {
        all_.clear();
        by_symbol_.clear();
        routes_ = 0;

        std::vector<symbol::SymbolId> ids;

        for (StrategyBase* strategy : strategies)
        {
            const auto specifications = strategy->get_watch_specifications();
            ids.clear();
            bool unresolved = specifications.empty();

            for (const auto& watch_specification : specifications)
            {
                const symbol::SymbolId id = symbol_id_of(watch_specification.ticker);

                if (id == symbol::kNone)
                {
                    unresolved = true;
                    break;
                }

                ids.push_back(id);
            }

            if (unresolved)
            {
                all_.push_back(strategy);
                continue;
            }

            for (const symbol::SymbolId id : ids)
            {
                if (id >= by_symbol_.size())
                {
                    by_symbol_.resize(static_cast<size_t>(id) + 1);
                }

                auto& subscribers = by_symbol_[id];

                if (std::find(subscribers.begin(), subscribers.end(), strategy) == subscribers.end())
                {
                    subscribers.push_back(strategy);
                    ++routes_;
                }
            }
        }
    }

    // id를 보는 전략들, 그 다음 전부 받는 전략들을 방문한다. id가 kNone이거나 모르는 id면 후자만.
    template <class Fn>
    void for_each(symbol::SymbolId id, Fn&& callback) const
    {
        if (id < by_symbol_.size())
        {
            for (StrategyBase* strategy : by_symbol_[id])
            {
                callback(strategy);
            }
        }

        for (StrategyBase* strategy : all_)
        {
            callback(strategy);
        }
    }

    // 진단용. routes는 (종목, 전략) 쌍의 수, all은 전부 받는 전략 수.
    [[nodiscard]] size_t routes() const noexcept
    {
        return routes_;
    }

    [[nodiscard]] size_t all_count() const noexcept
    {
        return all_.size();
    }

    [[nodiscard]] size_t watchers(symbol::SymbolId id) const noexcept
    {
        return id < by_symbol_.size() ? by_symbol_[id].size() : 0;
    }

private:
    std::vector<std::vector<StrategyBase*>> by_symbol_; // index = SymbolId
    std::vector<StrategyBase*>              all_;    // 구독 종목을 안 밝힌 전략
    size_t                                  routes_ = 0;
};

} // namespace strategy
