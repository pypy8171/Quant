// 전략 ↔ 주문 요청·응답 통로 — 레코드 형식과 순번·중복 거름 규칙만. 큐도 소켓도 공유메모리도 여기 없다.
//  전략이 신호 한 건에 순번을 붙여 보내고, 주문 쪽이 그 순번으로 받았다는 답을 돌려준다. 답이 없으면 전략이
//  다시 보내고, 주문 쪽은 같은 순번을 한 번만 받는다. 이 통로의 처리량은 초당 몇 건이라 문제가 아니다. [why D-114]
//  지금은 한 프로세스 안 두 스레드(전략 스레드 ↔ 주문 스레드)가 양끝이고, 단계 4에서 프로세스가 갈릴 때
//  레코드는 그대로 두고 운반 수단만 공유메모리로 바꾼다 — 그래서 레코드에 std::string도 포인터도 안 쓴다.
#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Types.h" // OrderSignal, symbol::SymbolId, strategy_table::StrategyId

namespace ipc
{

// 사유 문자열 칸. 고정 길이라 레코드가 통째로 바이트 복사된다(ledger_journal::Record와 같은 이유).
constexpr size_t kOrderReasonMax = 48;

// 주문 쪽이 요청 한 건을 어떻게 했는지. 값은 로그·시험이 물고 가므로 끝에만 더한다.
enum class OrderResult : uint8_t
{
    kAccepted  = 1, // 증권사가 접수했다(주문번호 있음)
    kRejected  = 2, // 게이트나 증권사가 거부했다 — 사유가 reason에 있다
    kFailed    = 3, // 보내다 실패했다(예외·전송 오류) — 재시도 대상
    kDuplicate = 4, // 이미 받은 순번이라 아무것도 하지 않았다
};

// 전략 → 주문. OrderSignal 전체가 아니라 주문을 내는 데 필요한 것만 담는다 — 문자열(종목코드·전략이름·사유)은
//  종목 id·전략 번호로 대신하고, 받는 쪽이 표에서 되찾는다(원칙 6). [inv] 순번은 0이 아니어야 한다.
struct OrderRequest
{
    uint64_t                   sequence       = 0;
    int64_t                    sent_at_ns     = 0; // steady_clock, 전략이 보낸 시각
    symbol::SymbolId           symbol_id      = symbol::kNone;
    strategy_table::StrategyId strategy_index = strategy_table::kNone;
    int32_t                    quantity       = 0;
    double                     price          = 0.0; // 0 = 시장가
    uint8_t                    side           = 0;   // OrderSide::Value
    uint8_t                    order_type     = 0;   // OrderType
    uint8_t                    action         = 0;   // OrderAction
    uint8_t                    reserved0      = 0;
};

// 주문 → 전략. 전략은 이걸 받아야 그 순번을 기다리는 것에서 지운다.
struct OrderResponse
{
    uint64_t sequence         = 0;
    uint64_t kis_order_number = 0; // kAccepted일 때만 의미 있다. 0 = 없음
    int64_t  handled_at_ns    = 0; // steady_clock, 주문 쪽이 처리를 마친 시각
    uint8_t  result           = 0; // OrderResult
    uint8_t  reserved0        = 0;
    uint16_t reserved1        = 0;
    uint32_t reserved2        = 0;
    char     reason[kOrderReasonMax] = {};
};

// 신호 하나를 요청 레코드로 옮긴다. 순번·시각은 신호가 이미 갖고 있는 것을 그대로 쓴다(SignalDispatcher::emit이 찍는다).
OrderRequest to_request(const OrderSignal& signal) noexcept;

// 결과·사유로 응답 레코드를 만든다. 사유는 칸을 넘으면 잘린다.
// 증권사 주문번호 문자열("0000123456")을 레코드의 정수 손잡이로. 숫자가 아니면 0이다.
// [inv] 앞의 0은 사라진다 — 이 값은 전략이 답과 요청을 맞추고 로그에 남기는 손잡이지, 다시 증권사를 부르는 데
//  쓰지 않는다(취소·정정은 주문 쪽이 한다).
uint64_t to_order_number(std::string_view kis_order_no) noexcept;

OrderResponse make_response(uint64_t sequence, OrderResult result, uint64_t kis_order_number,
                            std::string_view reason, int64_t handled_at_ns) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// 전략 쪽 — 보낸 것과 답이 온 것을 맞춘다
// ─────────────────────────────────────────────────────────────────────────────

// 답을 기다리는 요청을 순번으로 들고 있다가, 답이 오면 지운다. 오래 답이 없는 것은 다시 보낼 후보로 꺼내 준다.
//  [inv] 전략 스레드 하나만 부른다 — 동기화는 없다. SignalDispatcher가 그렇듯 소유 스레드가 하나다.
class PendingRequests
{
public:
    // 답을 기다리는 상한. 넘으면 가장 오래된 것부터 버린다 — 여기서 자라게 두면 주문 쪽이 멈췄을 때
    //  전략 스레드의 메모리가 대신 자란다.
    explicit PendingRequests(size_t capacity);

    // 보냈다고 적는다. 같은 순번을 두 번 적으면 시각만 갱신한다(재전송).
    void note_sent(uint64_t sequence, int64_t now_ns);

    // 답이 왔다고 적는다. 기다리던 순번이었으면 참.
    bool note_response(uint64_t sequence);

    // 보낸 지 timeout_ns를 넘긴 것 중 가장 오래된 순번. 없으면 빈 값 — 재전송은 호출자가 정한다.
    [[nodiscard]] std::optional<uint64_t> oldest_overdue(int64_t now_ns, int64_t timeout_ns) const;

    [[nodiscard]] size_t size() const noexcept
    {
        return entries_.size();
    }

    // 상한을 넘겨 버린 요청 수. 0이 아니면 주문 쪽이 답을 못 주고 있다는 뜻이다.
    [[nodiscard]] uint64_t evicted() const noexcept
    {
        return evicted_;
    }

private:
    struct Entry
    {
        uint64_t sequence   = 0;
        int64_t  sent_at_ns = 0;
    };

    // 순번 순으로 늘어서는 작은 목록. 답은 보낸 순서대로 오는 것이 보통이라 앞에서 지워진다.
    std::vector<Entry> entries_;
    size_t             capacity_ = 0;
    uint64_t           evicted_  = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 주문 쪽 — 같은 순번은 한 번만
// ─────────────────────────────────────────────────────────────────────────────

// 최근 본 순번을 창 하나만큼 기억해 두 번째부터는 거른다.
//  [inv] 주문 스레드 하나만 부른다 — 동기화는 없다. 주문·원장은 단일 시퀀서다(원칙 4).
class DuplicateFilter
{
public:
    // 기억할 순번 수. 주문 큐 용량보다 넉넉해야 한다 — 창보다 오래 밀린 순번은 판정할 근거가 없어 거른다.
    explicit DuplicateFilter(size_t window);

    // 처음 보는 순번이면 참(받아서 처리한다). 이미 본 것이거나 창보다 오래된 것이면 거짓.
    //  순번 0은 거짓이다 — 순번을 안 찍은 경로는 통로를 쓸 수 없다.
    bool accept(uint64_t sequence);

    [[nodiscard]] uint64_t duplicates() const noexcept
    {
        return duplicates_;
    }

    [[nodiscard]] uint64_t highest_seen() const noexcept
    {
        return highest_seen_;
    }

private:
    // 받은 순번을 덮어쓰며 도는 고리. 0은 빈 칸이다.
    std::vector<uint64_t> seen_;
    size_t                write_index_  = 0;
    uint64_t              highest_seen_ = 0;
    uint64_t              duplicates_   = 0;
};

} // namespace ipc
