#pragma once
// 체결통보 키 — 거래일·ODNO·체결시각·수량·단가(원 단위 ×100). 통보마다 문자열을 만들어 해시하던 것을 정수
//  다섯 개로 바꿨다. H0STCNI0 전문에는 체결 고유번호가 없어(공식 샘플 ccnl_notice의 26필드표로 2026-09-23
//  확인) 이 조합이 키다(V-4) — 체결시각이 초 단위라 같은 초·같은 수량·같은 단가는 구분하지 못한다.
//  라우터의 seen_fills_·unlinked_fill_keys_가 쓰고, bench_order_path_keys가 옛 문자열 키와 견준다. [why D-112]
#include <cstddef>
#include <cstdint>

namespace fill_key
{

struct FillKey
{
    uint32_t trade_date;   // YYYYMMDD — ODNO는 영업일마다 재사용된다
    uint64_t order_number; // ODNO
    uint32_t fill_time;    // HHMMSS
    int32_t  quantity;
    int64_t  price_cents;
    bool     operator==(const FillKey&) const noexcept = default;
};

struct FillKeyHash
{
    size_t operator()(const FillKey& key) const noexcept;
};

} // namespace fill_key
