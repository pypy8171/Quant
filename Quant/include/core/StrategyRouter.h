// 종목 id → 그 종목을 보는 전략 목록. 전략 스레드가 틱마다 전략 전부를 돌며 각자 문자열을 비교하던 것을
//  id 배열 인덱스 한 번으로 바꾼다(원칙 6). 구독 종목을 안 밝힌 전략(get_watch_specs가 빈 것)은 전부 받는다 —
//  오늘과 같은 동작이다.
// 스레드: 전략 스레드 전용. 전략 목록이 바뀔 때(strat_version_)만 다시 만든다 — hot path에서 문자열을 보지 않는다.
//  [why D-071]
#pragma once

#include "core/SymbolTable.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace strat
{

class Router
{
public:
    // sym_of: 종목 문자열 → id. 처음 보는 종목은 여기서 id를 받는다(기동·재스캔 시점이라 hot path가 아니다).
    //  kNone을 돌려주면(테이블이 찼다) 그 전략은 전부 받는 쪽으로 보낸다 — 틱을 놓치는 것보다 낫다.
    template <class SymOf>
    void rebuild(const std::vector<StrategyBase*>& strategies, SymOf&& sym_of)
    {
        all_.clear();
        by_sym_.clear();
        routes_ = 0;

        std::vector<sym::SymbolId> ids;

        for (StrategyBase* s : strategies)
        {
            const auto specs = s->get_watch_specs();
            ids.clear();
            bool unresolved = specs.empty();

            for (const auto& sp : specs)
            {
                const sym::SymbolId id = sym_of(sp.ticker);

                if (id == sym::kNone)
                {
                    unresolved = true;
                    break;
                }

                ids.push_back(id);
            }

            if (unresolved)
            {
                all_.push_back(s);
                continue;
            }

            for (const sym::SymbolId id : ids)
            {
                if (id >= by_sym_.size())
                {
                    by_sym_.resize(static_cast<size_t>(id) + 1);
                }

                auto& v = by_sym_[id];

                if (std::find(v.begin(), v.end(), s) == v.end())
                {
                    v.push_back(s);
                    ++routes_;
                }
            }
        }
    }

    // id를 보는 전략들, 그 다음 전부 받는 전략들을 방문한다. id가 kNone이거나 모르는 id면 후자만.
    template <class Fn>
    void for_each(sym::SymbolId id, Fn&& fn) const
    {
        if (id < by_sym_.size())
        {
            for (StrategyBase* s : by_sym_[id])
            {
                fn(s);
            }
        }

        for (StrategyBase* s : all_)
        {
            fn(s);
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

    [[nodiscard]] size_t watchers(sym::SymbolId id) const noexcept
    {
        return id < by_sym_.size() ? by_sym_[id].size() : 0;
    }

private:
    std::vector<std::vector<StrategyBase*>> by_sym_; // index = SymbolId
    std::vector<StrategyBase*>              all_;    // 구독 종목을 안 밝힌 전략
    size_t                                  routes_ = 0;
};

} // namespace strat
