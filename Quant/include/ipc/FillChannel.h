// 시세 → 주문 체결통보 통로 — 증권사 체결통보 한 건을 경계 너머로 나르는 큐 하나.
//  앱키가 하나라 실시간 세션도 하나이고, 체결통보(H0STCNI)는 시세와 같은 세션에 실린다. 그 소켓을 쥔 쪽이
//  시세 프로세스로 옮겨 가면서 체결통보가 프로세스 경계를 넘게 됐다 — 이 통로가 그 자리다. 체결이 안
//  들어오면 주문 쪽 OrderRouter 의 예약 수량이 안 풀려 총노출을 이중계상하므로, 이 통로가 막히면 버린
//  건수를 세어 건강 판정에 싣는다. [why D-114]
//
//  [inv] 줄이 하나다 — 세션이 하나라 보내는 스레드도 하나다(원칙 5: 생산자가 하나면 한줄 큐). 체결통보가
//   소켓 둘 이상에 실리는 날이 오면 MarketFeedChannel 처럼 줄을 갈라야 한다.
//  [inv] 보내는 쪽은 기다리지 않는다 — 큐가 차면 버리고 센다(원칙 3).
#pragma once

#include "core/Types.h" // FillNotification, OrderSide
#include "ipc/SharedSpscRing.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ipc
{

// 증권사 주문번호 칸. KIS 주문번호는 열 자리 안팎이라 여유를 뒀다. 정수로 바꾸지 않는 것은 앞의 0이
//  사라지면 취소·정정 때 증권사에 되돌려 줄 수 없기 때문이다(주문 쪽이 그 글자를 그대로 쓴다).
constexpr size_t kFillOrderNumberMax = 24;

// 단축종목코드 칸. 국내 6자리·해외 심볼까지 담긴다.
constexpr size_t kFillTickerMax = 16;

// 체결시각 칸("HHMMSS").
constexpr size_t kFillTimeMax = 8;

// 주문거래소 칸("KRX"·"NXT"). 짧은 전문에서는 빈 칸이다.
constexpr size_t kFillExchangeMax = 8;

// 체결통보 큐 칸 수. 한 프로세스로 돌던 때의 체결 큐(Engine::ShardPipeline 의 체결 큐)와 같은 수다 —
//  하루 체결이 수백 건이라 시세 체결 큐(kFeedTradeCapacity = 16384)만큼 잡을 까닭이 없고, 두 자리에 서로
//  다른 수를 두면 "어느 쪽이 정본인가"가 생긴다. 주문 쪽이 한 박자 쉬는 동안 오는 것을 받아 둘 만큼은 된다.
constexpr size_t kFillCapacity = 1024;

// 체결통보 한 건을 경계 너머로 나르는 레코드. FillNotification 에서 std::string 과 시각 객체를 걷어낸
//  모양이다 — 포인터가 들어가면 공유 쪽지를 건널 수 없다. 종목 코드를 번호로 바꾸지 않은 것은 번호를 다는
//  쪽이 주문 하나뿐이고(원칙 4), 시세는 표에 없는 종목의 체결도 그대로 넘겨야 하기 때문이다. 받는 쪽이
//  이 글자로 제 표를 짚는다.
//  [inv] 순번은 0이 아니어야 한다. [inv] 글자 칸은 모두 0으로 끝난다 — is_plausible 이 그것까지 본다.
struct FillNotice
{
    uint64_t sequence        = 0;
    int64_t  sent_at_ns      = 0;   // steady_clock, 시세 쪽이 보낸 시각
    int64_t  timestamp_ns    = 0;   // system_clock epoch ns, 전문을 받은 시각
    double   filled_price    = 0.0; // 체결단가 (CNTG_UNPR)
    int32_t  filled_quantity = 0;   // 체결수량 (CNTG_QTY)
    int32_t  order_quantity  = 0;   // 주문수량 (ODER_QTY). 짧은 전문에서는 0 = "모른다"
    uint8_t  side            = 0;   // OrderSide::Value
    uint8_t  reserved0       = 0;
    uint16_t reserved1       = 0;
    uint32_t reserved2       = 0;

    char kis_order_no[kFillOrderNumberMax]      = {};
    char original_order_no[kFillOrderNumberMax] = {};
    char ticker[kFillTickerMax]                 = {};
    char fill_time[kFillTimeMax]                = {};
    char exchange[kFillExchangeMax]             = {};
};

static_assert(std::is_trivially_copyable_v<FillNotice>, "체결 레코드는 바이트째 복사된다");
static_assert(sizeof(FillNotice) == 128, "체결 레코드는 캐시라인 두 줄이어야 한다 — 칸을 더하면 reserved를 쓴다");

// 꺼낸 체결이 말이 되는지 보는 기준.
struct FillLimits
{
    int32_t quantity_max = 1'000'000;     // 한 건 최대 체결 수량
    double  price_max    = 100'000'000.0; // 한 주 최대 가격(원)
};

// 큐에서 꺼낸 체결이 원장에 들어가도 되는가. 건너편이 망가졌거나 칸이 덮였을 때 그 값으로 예약 수량을
//  풀지 않으려고 여기를 지나게 한다. 거짓이면 버리고 센다. 글자 칸이 칸 안에서 끝나는지까지 본다.
[[nodiscard]] bool is_plausible(const FillNotice& notice, const FillLimits& limits) noexcept;

// 체결통보 한 건을 레코드로 옮긴다. 순번은 보내는 쪽이 1부터 매긴다. 칸을 넘는 글자는 글자 경계에서
//  잘리고, 잘렸는지는 truncated 가 참으로 알린다(널이면 안 알린다).
[[nodiscard]] FillNotice to_notice(const FillNotification& fill, uint64_t sequence, int64_t sent_at_ns,
                                   bool* truncated = nullptr) noexcept;

// 레코드를 체결통보로 되돌린다. [inv] 0으로 끝나는 것을 확인한 뒤에 부른다(is_plausible).
[[nodiscard]] FillNotification to_fill(const FillNotice& notice);

// 체결통보 큐 한쪽 끝. 한 인스턴스가 한쪽 끝만 맡는다 — create 는 쪽지를 만드는 쪽(주문 프로세스)이
//  자리를 놓느라 한 번 부르고, attach 는 보내는 쪽(시세)이 RingEndpoint::kProducer 로 부른다.
//  주문 프로세스는 만든 쪽이 그대로 받는 끝을 겸한다(create 가 양끝을 맡는다).
class FillChannel
{
public:
    FillChannel()                              = default;
    FillChannel(const FillChannel&)            = delete;
    FillChannel& operator=(const FillChannel&) = delete;

    // 칸 capacity개가 차지하는 바이트.
    [[nodiscard]] static size_t bytes_for(size_t capacity = kFillCapacity);

    // 통로를 새로 놓는다. base는 캐시라인 경계여야 하고 capacity는 2의 거듭제곱이다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, size_t capacity = kFillCapacity);

    // 이미 놓인 통로에 붙는다. 큐 머리가 다르면 붙지 않는다.
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, RingEndpoint endpoint, size_t capacity = kFillCapacity);

    void unbind() noexcept;

    [[nodiscard]] bool is_bound() const noexcept
    {
        return ring_.is_bound();
    }

    // 보내는 쪽. 거짓은 "큐가 참" — 그 건은 버려지고 overflow()가 하나 는다. 수신 스레드는 기다리지 않는다.
    [[nodiscard]] bool push(const FillNotice& notice) noexcept;

    // 받는 쪽. 참이면 out에 한 건을 채운다. 말이 안 되는 칸은 안에서 버리고 세므로(discarded()) 부르는 쪽이
    //  검사를 잊을 수 없다 — 이 통로를 꺼내는 자리는 여기 하나뿐이다.
    [[nodiscard]] bool pop(const FillLimits& limits, FillNotice& out) noexcept;

    // 큐가 차서 버린 수. 0이 아니면 주문 쪽이 체결통보를 못 따라오고 있다는 뜻이다 — 예약 수량이 안 풀린다.
    [[nodiscard]] uint64_t overflow() const noexcept
    {
        return overflow_.load(std::memory_order_relaxed);
    }

    // 값이 말이 안 돼 버린 수. 0이 아니면 건너편 프로세스를 의심한다.
    [[nodiscard]] uint64_t discarded() const noexcept
    {
        return discarded_.load(std::memory_order_relaxed);
    }

    // 통로로 밀어 넣은 건수·통로에서 꺼낸 건수. 늘리는 계수기를 따로 두지 않는다 — 링이 순번으로 이미
    //  세고 있어 그 순번을 그대로 읽는다(MarketFeedChannel 과 같은 방식). 순번은 공유 칸에 있어 어느
    //  프로세스에서 물어도 같은 답이 온다 — 한쪽만 보고도 통로가 도는지 알 수 있다. [why D-114 단계 5]
    [[nodiscard]] uint64_t sent() const;
    [[nodiscard]] uint64_t received() const;

    // 도장이 제 차례보다 앞서 있던 횟수 — 칸이 덮였다는 뜻이다. 건강 판정이 이 수를 본다.
    [[nodiscard]] uint64_t stamp_out_of_turn() const;

    // 보내는 쪽이 보는 대기 칸 수(어림값). [inv] 보내는 스레드만 부른다.
    [[nodiscard]] size_t pending() const;

    // 받는 쪽이 보는 읽을 칸 수(어림값). [inv] 받는 스레드만 부른다.
    [[nodiscard]] size_t readable() const;

    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

private:
    SharedSpscRing<FillNotice> ring_;

    // 보내는 쪽과 받는 쪽이 각각 제 수를 올리지만, 감시 스레드가 같은 값을 읽어 로그에 싣는 자리라 원자로
    //  센다. 실패했을 때만 오르므로 hot path 비용은 없다.
    std::atomic<uint64_t> overflow_  = 0;
    std::atomic<uint64_t> discarded_ = 0;
    std::string           last_error_;
};

} // namespace ipc
