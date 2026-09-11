#pragma once
// KIS 실시간 채널 레코드(`^`로 나눈 필드 벡터) → 구조체 디코더.
// 헤더 전용·순수 함수. 로그·소켓·암호 의존이 없어 테스트가 플랫폼 링크 없이 직접 부른다.
// 호출 스레드: WS 수신 스레드(KisWebSocket::parse_*)와 테스트. 관련 결정: D-037.
//
// [wire] 필드 위치는 KIS 실시간 전문 순서를 그대로 따른다. 채널별 위치는 각 함수 위 주석이 정본이고
// 실데이터 확인은 parse_*의 `첫 수신` 로그로 한다. 숫자 변환 실패는 0으로 남기고 kBadNumber를 돌려준다 —
// 버릴지 흘릴지는 호출자가 정한다(현재 호가·체결은 흘리고, 체결통보는 버린다).

#include "core/Types.h"

#include <chrono>
#include <string>
#include <vector>

namespace kis_ws
{

enum class Decode
{
    kOk,        // 전 필드 정상
    kShort,     // 필드 수 부족 — 구조체를 채우지 않았다
    kSkip,      // 이 채널이 다루지 않는 레코드(체결통보의 접수/취소 통보 등)
    kBadSide,   // 체결통보 매매구분이 01/02 밖 — 원장에 못 넣는다
    kBadNumber, // 숫자 필드 하나 이상이 변환 실패. 나머지 필드는 채워져 있다
};

// 채널별로 있어야 하는 최소 필드 수. 마지막으로 읽는 인덱스 + 1.
constexpr size_t kMinFieldsOrderbook    = 38; // BIDP_RSQN5 = f[37]
constexpr size_t kMinFieldsKrTrade      = 22; // 체결구분 = f[21]
constexpr size_t kMinFieldsUsTrade      = 9;  // 체결량 = f[8]
constexpr size_t kMinFieldsFutTrade     = 19; // 미결제약정 = f[18]
constexpr size_t kMinFieldsFutOrderbook = 32; // 매수잔량5 = f[31]
constexpr size_t kMinFieldsFill         = 14; // CNTG_YN = f[13]

namespace detail
{

// stod/stoll/stoi를 예외 없는 형태로 감싼다. 실패면 out을 건드리지 않고 false.
// C-4에서 std::from_chars로 바꿀 자리 — 호출자는 이 함수만 본다.
inline bool to_double(const std::string& s, double& out)
{
    try
    {
        out = std::stod(s);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

inline bool to_i64(const std::string& s, int64_t& out)
{
    try
    {
        out = std::stoll(s);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

inline bool to_int(const std::string& s, int& out)
{
    try
    {
        out = std::stoi(s);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 5단계 호가 블록. 현물·선물이 시작 위치만 다르고 배열 규칙은 같다.
inline bool fill_levels(const std::vector<std::string>& f, size_t ask_p, size_t ask_q, size_t bid_p,
                        size_t bid_q, OrderBook& ob)
{
    bool ok = true;

    for (size_t i = 0; i < 5; ++i)
    {
        ok &= to_double(f[ask_p + i], ob.asks[i].price);
        ok &= to_i64(f[ask_q + i], ob.asks[i].quantity);
        ok &= to_double(f[bid_p + i], ob.bids[i].price);
        ok &= to_i64(f[bid_q + i], ob.bids[i].quantity);
    }

    return ok;
}

} // namespace detail

// ─── 국내 현물 호가 (H0STASP0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]시각 [3-7]매도호가1-5 [13-17]매수호가1-5 [23-27]매도잔량1-5 [33-37]매수잔량1-5
inline Decode decode_orderbook(const std::vector<std::string>& f, OrderBook& ob)
{
    if (f.size() < kMinFieldsOrderbook)
    {
        return Decode::kShort;
    }

    ob.ticker = f[0];
    ob.time = f[1];
    ob.timestamp = std::chrono::system_clock::now();
    return detail::fill_levels(f, 3, 23, 13, 33, ob) ? Decode::kOk : Decode::kBadNumber;
}

// ─── 국내 현물 체결 (H0STCNT0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시간 [2]현재가 [12]체결량 [21]체결구분(1=매수,5=매도)
inline Decode decode_kr_trade(const std::vector<std::string>& f, TradeData& td)
{
    if (f.size() < kMinFieldsKrTrade)
    {
        return Decode::kShort;
    }

    td.ticker = f[0];
    td.time = f[1];
    td.market = Market::KR;
    td.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_double(f[2], td.price);
    ok &= detail::to_i64(f[12], td.quantity);
    ok &= detail::to_int(f[21], td.direction);
    return ok ? Decode::kOk : Decode::kBadNumber;
}

// ─── 미국 체결 (HDFSCNT0) ────────────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시간(KST) [2]현재가 [8]체결량 [20]방향(미검증 — 20필드 이하면 0)
inline Decode decode_us_trade(const std::vector<std::string>& f, TradeData& td)
{
    if (f.size() < kMinFieldsUsTrade)
    {
        return Decode::kShort;
    }

    td.ticker = f[0];
    td.time = f[1];
    td.market = Market::US;
    td.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_double(f[2], td.price);
    ok &= detail::to_i64(f[8], td.quantity);
    td.direction = 0;

    if (f.size() > 20)
    {
        ok &= detail::to_int(f[20], td.direction);
    }

    return ok ? Decode::kOk : Decode::kBadNumber;
}

// ─── 국내 선물 체결 (H0IFCNT0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시각 [5]현재가 [9]단위체결량. 방향 코드가 없어 direction=0.
inline Decode decode_fut_trade(const std::vector<std::string>& f, TradeData& td)
{
    if (f.size() < kMinFieldsFutTrade)
    {
        return Decode::kShort;
    }

    td.ticker = f[0];
    td.time = f[1];
    td.market = Market::KR; // 선물도 국내 세션. 소비 측은 종목코드로 현·선을 구분한다.
    td.direction = 0;
    td.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_double(f[5], td.price);
    ok &= detail::to_i64(f[9], td.quantity);
    return ok ? Decode::kOk : Decode::kBadNumber;
}

// ─── 국내 선물 호가 (H0IFASP0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]시각 [2-6]매도호가 [7-11]매수호가 [12-21]호가건수(건너뜀) [22-26]매도잔량 [27-31]매수잔량
inline Decode decode_fut_orderbook(const std::vector<std::string>& f, OrderBook& ob)
{
    if (f.size() < kMinFieldsFutOrderbook)
    {
        return Decode::kShort;
    }

    ob.ticker = f[0];
    ob.time = f[1];
    ob.timestamp = std::chrono::system_clock::now();
    return detail::fill_levels(f, 2, 22, 7, 27, ob) ? Decode::kOk : Decode::kBadNumber;
}

// ─── 체결통보 (H0STCNI0 실거래 / H0STCNI9 모의) ──────────────────────────
// [wire] [2]ODER_NO [4]SELN_BYOV_CLS(01=매도,02=매수) [8]종목코드 [9]체결수량 [10]체결단가
//        [11]체결시각(HHMMSS) [13]CNTG_YN(1=접수/정정/취소/거부 통보, 2=체결통보)
// 체결통보(2)만 kOk. 원장에 들어가는 값이라 매매구분·수량·단가 어느 하나라도 못 읽으면 채우지 않는다.
inline Decode decode_fill(const std::vector<std::string>& f, FillNotification& fn)
{
    if (f.size() < kMinFieldsFill)
    {
        return Decode::kShort;
    }

    if (f[13] != "2")
    {
        return Decode::kSkip;
    }

    if (f[4] == "02")
    {
        fn.side = OrderSide::BUY;
    }
    else if (f[4] == "01")
    {
        fn.side = OrderSide::SELL;
    }
    else
    {
        return Decode::kBadSide;
    }

    fn.odno = f[2];
    fn.ticker = f[8];
    fn.fill_time = f[11];
    fn.timestamp = std::chrono::system_clock::now();
    bool ok = detail::to_int(f[9], fn.filled_qty);
    ok &= detail::to_double(f[10], fn.filled_price);
    return ok ? Decode::kOk : Decode::kBadNumber;
}

} // namespace kis_ws
