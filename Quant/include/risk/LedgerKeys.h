#pragma once
#include "core/SymbolTable.h"
#include "core/Types.h"
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// LedgerKeys — 원장 키 (계좌 번호, 종목 id) 를 만들고 되찾는 표
//
//  OrderGate 원장의 모든 맵이 이 키를 쓴다. 종목은 SymbolTable(Engine이 넘긴 것, 없으면 자체 테이블),
//  계좌는 account_names_ 인덱스라 키가 정수 두 개다. 문자열은 로그·계획·스냅샷에서 ticker_of()·account_of()로
//  되찾는다. OrderGate에서 떼어 냈다 — 키 규칙이 원장 계산과 섞여 있으면 둘 중 하나를 고칠 때 다른 쪽을 같이 읽어야 했다.
//
// [thread] 종목 테이블은 자체 락을 든다. account_names_는 락이 없다 — 소유자(OrderGate)가 positions_mutex_를 잡고
//   부른다. 계좌를 등록하는 make·register_signal·account_index(create)만이 아니라 읽는 lookup·account_of도 같다.
// [why D-057] 두 필드를 따로 들어 "A"+"B:C"와 "A:B"+"C"가 섞이지 않고, 키에서 (계좌, 종목)을 되찾는 파싱이 없다.
// [why D-105] 문자열 두 개이던 때는 조회 한 번이 34~40 ns였고 대부분이 문자열 해시였다.
// ─────────────────────────────────────────────────────────────────────────────
class LedgerKeys
{
public:
    static constexpr uint32_t kUnknownAccount = UINT32_MAX;

    struct Key
    {
        uint32_t         account = kUnknownAccount;
        symbol::SymbolId symbol  = symbol::kNone;

        bool operator==(const Key&) const = default;
    };

    struct KeyHash
    {
        // 두 32비트를 64비트 하나로 붙여 곱셈으로 섞는다. xor만 하면 (a,b)와 (b,a)가 같은 버킷에 간다.
        //  원장 맵 조회마다 불려 헤더에 둔다 — .cpp로 내리면 인라인이 안 돼 체결 한 건이 8 ns 늘었다.
        size_t operator()(const Key& key) const noexcept
        {
            const uint64_t mixed = ((static_cast<uint64_t>(key.account) << 32) | key.symbol) * 0x9e3779b97f4a7c15ull;
            return static_cast<size_t>(mixed ^ (mixed >> 29));
        }
    };

    template <class V>
    using Map = std::unordered_map<Key, V, KeyHash>;

    LedgerKeys()                             = default;
    LedgerKeys(const LedgerKeys&)            = delete;
    LedgerKeys& operator=(const LedgerKeys&) = delete;

    // nullptr이면 자체 테이블. [inv] 첫 키가 생기기 전에 부른다 — 뒤에 바꾸면 이미 든 키의 번호가 다른 테이블의 것이 된다.
    void set_symbol_table(symbol::SymbolTable* table) noexcept
    {
        symbols_ = table ? table : &own_symbols_;
    }

    [[nodiscard]] symbol::SymbolTable& symbols() noexcept { return *symbols_; }
    [[nodiscard]] const symbol::SymbolTable& symbols() const noexcept { return *symbols_; }

    // 쓰기 경로(체결·시드·선점) — 처음 보는 계좌·종목을 등록한다. 종목 테이블이 가득 차면 던진다.
    Key make(std::string_view account, std::string_view ticker);
    // 읽기 경로(조회·정리·게이트) — 등록하지 않는다. 모르는 계좌·종목이면 원장에 없는 키가 나와 find가 빈다.
    [[nodiscard]] Key lookup(std::string_view account, std::string_view ticker) const;
    [[nodiscard]] Key lookup(std::string_view account, symbol::SymbolId symbol) const;
    // 신호는 수신 스레드가 찍은 symbol_id를 이미 들고 있다 — Engine 테이블을 쓸 때만 그 id를 믿는다
    //  (자체 테이블이면 다른 테이블의 id라 문자열로 찾는다).
    [[nodiscard]] Key lookup(const OrderSignal& signal) const;
    // check() 전용 — 처음 보는 계좌·종목을 등록해 중복 신호 키가 모르는 계좌끼리 겹치지 않게 한다.
    [[nodiscard]] Key register_signal(const OrderSignal& signal);
    // 계좌 문자열 → 계좌 번호. create면 처음 보는 계좌를 등록한다.
    [[nodiscard]] uint32_t account_index(std::string_view account, bool create);
    // 16바이트 값이라 힙 할당이 없다 — std::string이 필요한 자리(계획·스냅샷)만 .string()으로 만든다.
    [[nodiscard]] symbol::Ticker ticker_of(const Key& key) const;
    [[nodiscard]] const std::string& account_of(const Key& key) const { return account_name(key.account); }
    // 계좌 번호 → 문자열. 모르는 번호(kUnknownAccount 포함)면 0번 = ""을 준다.
    [[nodiscard]] const std::string& account_name(uint32_t account) const;
    // 종목 문자열 목록 → 종목 id 비트(테이블 용량 크기). 유령 정리 두 곳이 쓴다.
    [[nodiscard]] std::vector<bool> live_symbols(const std::vector<std::string>& live_tickers) const;

private:
    symbol::SymbolTable  own_symbols_;
    symbol::SymbolTable* symbols_ = &own_symbols_;

    std::vector<std::string> account_names_{std::string()}; // 계좌 번호 → 문자열. [0]은 ""(단일 계좌 하위호환)
};

// ─── 인라인 정의 ─────────────────────────────────────────────────────────────
//  [why D-118] 주문 검사·체결 반영이 신호·체결마다 여러 번 부른다. .cpp로 내리면 번역 단위를 넘는 호출이라 인라인이
//  안 돼 bench_order_path_keys가 SELL 검사 153→160 ns, 체결 반영 73→82 ns로 늘었다(2026-09-24, x64 Release,
//  세 번씩 번갈아 잼). 여기 두면 전과 같다. Release LTO는 오히려 172 ns로 나빠 쓰지 않는다. D-118의 예외다.

// 계좌는 기동 중 몇 개뿐이라 벡터를 앞에서부터 비교한다(해시보다 싸다).
inline uint32_t LedgerKeys::account_index(std::string_view account, bool create)
{
    for (size_t index = 0; index < account_names_.size(); ++index)
    {
        if (account_names_[index] == account)
        {
            return static_cast<uint32_t>(index);
        }
    }

    if (!create)
    {
        return kUnknownAccount;
    }

    account_names_.emplace_back(account);
    return static_cast<uint32_t>(account_names_.size() - 1);
}

inline LedgerKeys::Key LedgerKeys::make(std::string_view account, std::string_view ticker)
{
    const uint32_t         account_id = account_index(account, true);
    const symbol::SymbolId symbol     = symbols_->intern(ticker);

    if (symbol == symbol::kNone)
    {
        throw std::runtime_error(std::format("OrderGate: 종목 테이블이 가득 차 원장 키를 못 만든다 ticker={} capacity={}",
                                             ticker, symbols_->capacity()));
    }

    return Key{account_id, symbol};
}

inline LedgerKeys::Key LedgerKeys::lookup(std::string_view account, symbol::SymbolId symbol) const
{
    // const 경로라 등록하지 않는다 — account_index(create=false)와 같은 탐색이지만 const 멤버로 둔다.
    for (size_t index = 0; index < account_names_.size(); ++index)
    {
        if (account_names_[index] == account)
        {
            return Key{static_cast<uint32_t>(index), symbol};
        }
    }

    return Key{kUnknownAccount, symbol};
}

inline LedgerKeys::Key LedgerKeys::lookup(std::string_view account, std::string_view ticker) const
{
    return lookup(account, symbols_->lookup(ticker));
}

inline LedgerKeys::Key LedgerKeys::lookup(const OrderSignal& signal) const
{
    if (symbols_ != &own_symbols_ && signal.symbol_id != symbol::kNone)
    {
        return lookup(signal.account_id, signal.symbol_id);
    }

    return lookup(signal.account_id, signal.ticker);
}

inline LedgerKeys::Key LedgerKeys::register_signal(const OrderSignal& signal)
{
    if (symbols_ != &own_symbols_ && signal.symbol_id != symbol::kNone)
    {
        return Key{account_index(signal.account_id, true), signal.symbol_id};
    }

    return make(signal.account_id, signal.ticker);
}

inline symbol::Ticker LedgerKeys::ticker_of(const Key& key) const
{
    return symbols_->name(key.symbol);
}

inline const std::string& LedgerKeys::account_name(uint32_t account) const
{
    return (account < account_names_.size()) ? account_names_[account] : account_names_[0];
}
