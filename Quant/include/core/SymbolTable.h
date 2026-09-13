// 종목 문자열 ↔ 정수 id(SymbolId) 테이블. 수신 스레드가 틱·호가에 id를 찍고, hot path의 캐시·디스패치는
//  id 배열 인덱스로 간다. 등록은 기동·재스캔·처음 보는 종목에서만 일어나고 id는 재사용하지 않는다. [why D-071]
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sym
{

using SymbolId = uint32_t;

// 0은 "아직 id를 안 받았다". 기본 초기화된 TradeData·OrderBook이 이 값이라 배선이 빠진 경로가 드러난다.
constexpr SymbolId kNone = 0;

class SymbolTable
{
public:
    // capacity는 id 상한(0 제외). 코스콤 전 종목이 2,500여 개라 기본 8,192면 재스캔 누적분까지 든다.
    explicit SymbolTable(size_t capacity = 8192) : capacity_(capacity)
    {
        names_.reserve(64);
        names_.emplace_back(); // id 0 자리
    }

    // 있으면 그 id, 없으면 새 id. 가득 차면 kNone — 호출 쪽은 문자열 경로로 돌아간다.
    //  읽기는 shared 락(수신 스레드가 틱마다 한 번), 삽입만 배타 락.
    SymbolId intern(std::string_view ticker)
    {
        {
            std::shared_lock<std::shared_mutex> rl(mu_);
            auto                                it = ids_.find(ticker);

            if (it != ids_.end())
            {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> wl(mu_);
        auto                                it = ids_.find(ticker);

        if (it != ids_.end())
        {
            return it->second;
        }

        if (names_.size() >= capacity_)
        {
            return kNone;
        }

        const SymbolId id = static_cast<SymbolId>(names_.size());
        names_.emplace_back(ticker);
        ids_.emplace(names_.back(), id);
        return id;
    }

    [[nodiscard]] SymbolId lookup(std::string_view ticker) const
    {
        std::shared_lock<std::shared_mutex> rl(mu_);
        auto                                it = ids_.find(ticker);
        return it == ids_.end() ? kNone : it->second;
    }

    // 모르는 id면 빈 문자열. 복사해 돌려준다 — 참조를 내주면 재스캔의 벡터 재할당과 경쟁한다.
    [[nodiscard]] std::string name(SymbolId id) const
    {
        std::shared_lock<std::shared_mutex> rl(mu_);
        return id < names_.size() ? names_[id] : std::string{};
    }

    // 등록된 종목 수(id 0 제외).
    [[nodiscard]] size_t size() const
    {
        std::shared_lock<std::shared_mutex> rl(mu_);
        return names_.size() - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    struct SvHash
    {
        using is_transparent = void;

        size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };

    mutable std::shared_mutex                                      mu_;
    std::unordered_map<std::string, SymbolId, SvHash, std::equal_to<>> ids_;
    std::vector<std::string>                                       names_; // [inv] names_[ids_[t]] == t
    const size_t                                                   capacity_;
};

} // namespace sym
