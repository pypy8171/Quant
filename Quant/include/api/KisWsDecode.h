#pragma once
// KIS 실시간 채널 레코드(`^`로 나눈 필드 뷰) → 구조체 디코더.
// 헤더 전용·순수 함수. 로그·소켓·암호 의존이 없어 테스트가 플랫폼 링크 없이 직접 부른다.
// 호출 스레드: WS 수신 스레드(KisWebSocket::parse_*)와 테스트. 관련 결정: D-037, D-042.
//
// 필드는 원문 버퍼를 가리키는 string_view다 — 프레임 한 장에 std::string 40~50개를 만들던 것을
//  뷰 벡터 하나(용량 재사용)로 바꿨다. 뷰는 원문이 살아 있는 동안만 유효하다.
//
// [wire] 필드 위치는 KIS 실시간 전문 순서를 그대로 따른다. 채널별 위치는 각 함수 위 주석이 정본이고
// 실데이터 확인은 parse_*의 `첫 수신` 로그로 한다. 숫자 변환 실패는 0으로 남기고 kBadNumber를 돌려준다 —
// 버릴지 흘릴지는 호출자가 정한다(현재 호가·체결은 흘리고, 체결통보는 버린다).

#include "core/MarketSession.h"
#include "core/Types.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kis_websocket
{

// 레코드 한 건의 필드 목록. 뷰 벡터의 구간을 가리키기만 한다(소유 없음, 16바이트, 값 전달).
using Fields = std::span<const std::string_view>;

// delim으로 나눠 out에 뷰를 채운다. out은 비운 뒤 재사용하므로 용량이 잡힌 뒤로는 할당이 없다.
//  빈 토큰도 자리로 남기고 마지막 토큰은 delim 없이 끝나도 넣는다("a^^b" → {"a","","b"}).
void split_fields(std::string_view text, char delim, std::vector<std::string_view>& out);

// 다건 프레임의 데이터부(^-구분 필드 전체)를 레코드 단위로 본다. 순수 함수 — 단위 테스트 대상.
//  KIS 원문 스펙이 리포에 없어 채널별 절대 폭을 하드코딩하지 않고, 레코드 폭이 채널마다 고정이라는
//  성질만 써서 "총 필드 수 / count"로 폭을 복원한다. count<=1이거나 나누어떨어지지 않거나 폭이
//  min_fields 미만이면 비어 있다 — 호출부는 기존 1건 경로로 떨어진다(보수적 실패). 복사가 없다.
struct Records
{
    Fields all;
    size_t width = 0;
    size_t count = 0;

    [[nodiscard]] size_t size() const noexcept { return count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    Fields operator[](size_t result) const noexcept { return all.subspan(result * width, width); }
};

Records split_records(Fields fields, int count, size_t min_fields) noexcept;

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

// 숫자 필드 변환. 예외 없이 실패를 bool로 돌려주고, 실패면 out을 건드리지 않는다.
// 문자열 전체가 숫자여야 한다("215000abc"·"1,000"은 실패) — 전문의 한 칸이 밀리면 앞자리만
//  숫자로 읽혀 값이 조용히 어긋나는 것을 막는다(D-039). 앞의 '+'는 허용한다(KIS 부호 표기).
// std::from_chars는 로케일·예외·할당이 없다. 부동소수 지원이 없는 표준 라이브러리(GCC 10 이하)는
//  strtod로 대신하되 같은 "전부 소비" 규칙을 지킨다.
inline const char* number_begin(std::string_view text) noexcept
{
    return (!text.empty() && text[0] == '+') ? text.data() + 1 : text.data();
}

bool to_double(std::string_view text, double& out) noexcept;

template <typename Int> inline bool to_integer(std::string_view text, Int& out) noexcept
{
    const char* begin = number_begin(text);
    const char* end = text.data() + text.size();

    if (begin == end)
    {
        return false;
    }

    Int  value = 0;
    auto parse_result = std::from_chars(begin, end, value);

    if (parse_result.ec != std::errc() || parse_result.ptr != end)
    {
        return false;
    }

    out = value;
    return true;
}

inline bool to_i64(std::string_view text, int64_t& out) noexcept
{
    return to_integer<int64_t>(text, out);
}

inline bool to_int(std::string_view text, int& out) noexcept
{
    return to_integer<int>(text, out);
}

// 5단계 호가 블록. 현물·선물이 시작 위치만 다르고 배열 규칙은 같다.
bool fill_levels(Fields fields, size_t ask_price, size_t ask_quantity, size_t bid_price,
                        size_t bid_quantity, OrderBook& order_book);

} // namespace detail

// ─── 국내 현물 호가 (H0STASP0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]시각 [2]시간구분 [3-12]매도호가1-10 [13-22]매수호가1-10 [23-32]매도잔량1-10 [33-42]매수잔량1-10.
//        전문은 10단계, 여기서는 앞 5단계만 쓴다(asks[i]=f[3+i]/f[23+i], bids[i]=f[13+i]/f[33+i]).
Decode decode_orderbook(Fields fields, OrderBook& order_book);

// ─── 국내 현물 체결 (H0STCNT0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시간 [2]현재가 [12]체결량 [13]누적거래량 [18]체결강도(CTTR)
//        [21]체결구분(1=매수,5=매도). 13·18은 보조 필드라 숫자가 아니어도 실패로 치지 않는다(0).
Decode decode_kr_trade(Fields fields, TradeData& trade);

// ─── 미국 체결 (HDFSCNT0) ────────────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시간(KST) [2]현재가 [8]체결량 [20]방향(미검증 — 20필드 이하면 0)
Decode decode_us_trade(Fields fields, TradeData& trade);

// ─── 국내 선물 체결 (H0IFCNT0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]체결시각 [5]현재가 [9]단위체결량 [10]누적거래량 [18]미결제약정. 방향 코드가 없어 direction=0.
Decode decode_future_trade(Fields fields, TradeData& trade);

// ─── 국내 선물 호가 (H0IFASP0) ───────────────────────────────────────────
// [wire] [0]종목코드 [1]시각 [2-6]매도호가 [7-11]매수호가 [12-21]호가건수(건너뜀) [22-26]매도잔량 [27-31]매수잔량
Decode decode_future_orderbook(Fields fields, OrderBook& order_book);

// ─── 체결통보 (H0STCNI0 실거래 / H0STCNI9 모의) ──────────────────────────
// [wire] [2]ODER_NO [4]SELN_BYOV_CLS(01=매도,02=매수) [8]STCK_SHRN_ISCD [9]CNTG_QTY [10]CNTG_UNPR
//        [11]STCK_CNTG_HOUR [13]CNTG_YN(1=접수/정정/취소/거부 통보, 2=체결 — 모의 실측 확인)
//        [11]체결시각(HHMMSS) [13]CNTG_YN(1=접수/정정/취소/거부 통보, 2=체결통보)
// 체결통보(2)만 kOk. 원장에 들어가는 값이라 매매구분·수량·단가 어느 하나라도 못 읽으면 채우지 않는다.
Decode decode_fill(Fields fields, FillNotification& fill_notification);

} // namespace kis_websocket
