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

// 답의 사유 칸. 고정 길이라 레코드가 통째로 바이트 복사된다(ledger_journal::Record와 같은 이유).
constexpr size_t kOrderReasonMax = 48;

// 신호가 싣고 온 판단 근거 칸. 답의 사유보다 넓다 — 이 글은 원장 CSV의 entry_reason 열로 그대로 남아
//  나중에 "왜 샀나"를 읽는 유일한 근거다(G4). DeviationScale 진입 근거가 한글 섞어 130바이트 남짓이라
//  여유를 뒀다. 넘치면 바이트가 아니라 글자 경계에서 자른다. [why D-114]
constexpr size_t kSignalReasonMax = 192;

// 주문 이름 칸. "<전략이름>:B:<순번>" 꼴이라 전략 이름(최대 31자)에 꼬리가 붙는다.
constexpr size_t kClientOrderIdMax = 48;

// 계좌 칸. 계좌는 표를 두지 않는다 — 한 프로세스가 보는 계좌가 몇 개뿐이라 글자 그대로 나른다.
constexpr size_t kAccountIdMax = 16;

// 해외 거래소 칸("NAS"·"NYS"). 국내는 빈 칸이다.
constexpr size_t kExchangeMax = 8;

// 주문 쪽이 요청 한 건을 어떻게 했는지. 값은 로그·시험이 물고 가므로 끝에만 더한다.
enum class OrderResult : uint8_t
{
    kAccepted  = 1, // 증권사가 접수했다(주문번호 있음)
    kRejected  = 2, // 게이트나 증권사가 거부했다 — 사유가 reason에 있다
    kFailed    = 3, // 보내다 실패했다(예외·전송 오류) — 재시도 대상
    kDuplicate = 4, // 이미 받은 순번이라 아무것도 하지 않았다
    kInvalid   = 5, // 값이 말이 안 되는 요청이라 버렸다(is_plausible). 재전송해도 같은 값이면 또 버린다
};

// 전략 → 주문. OrderSignal에서 std::string을 걷어낸 모양이다 — 포인터가 들어가면 공유 쪽지를 건널 수 없다.
//  전략 이름은 번호로 대신하고 받는 쪽이 전략 표에서 되찾는다(원칙 6). 종목 코드·계좌·주문 이름·판단 근거는
//  고정 칸에 글자 그대로 싣는다. [why D-114]
//  - 종목 코드: 표에 아직 없는 종목은 번호가 kNone 으로 오고, 받는 쪽이 이 글자로 표에 올린다. 번호만 실으면
//    그 종목은 주문이 되지 않는다.
//  - 계좌: 표가 없다. 전략 이름과 달리 기동 때 다 모이지 않는다(보유분을 따라온다).
//  - 주문 이름·판단 근거: 로그와 원장 CSV에 그대로 실리는 글이라 번호로 바꿀 수 없다.
//  [inv] 순번은 0이 아니어야 한다. [inv] 글자 칸은 모두 0으로 끝난다 — is_plausible 이 그것까지 본다.
struct OrderRequest
{
    uint64_t                   sequence                     = 0;
    int64_t                    sent_at_ns                   = 0;   // steady_clock, 전략이 보낸 시각
    int64_t                    tick_at_ns                   = 0;   // steady_clock, 근거가 된 체결을 받은 시각. 0=안 찍음
    int64_t                    timestamp_ns                 = 0;   // system_clock epoch ns, 신호를 만든 시각
    uint64_t                   client_order_number          = 0;   // 취소·정정이 원주문을 찾는 키
    uint64_t                   original_client_order_number = 0;   // CANCEL/REPLACE 대상 원주문 번호
    double                     price                        = 0.0; // 0 = 시장가
    double                     reference_price              = 0.0; // 시장가 명목 한도 평가용 참조가
    symbol::SymbolId           symbol_id                    = symbol::kNone;
    strategy_table::StrategyId strategy_index               = strategy_table::kNone;
    int32_t                    quantity                     = 0;
    uint8_t                    side                         = 0;   // OrderSide::Value
    uint8_t                    order_type                   = 0;   // OrderType
    uint8_t                    action                       = 0;   // OrderAction
    uint8_t                    market                       = 0;   // Market
    symbol::Ticker             ticker;                             // 고정 배열이라 레코드가 통째로 복사된다
    char                       exchange[kExchangeMax]                    = {};
    char                       account_id[kAccountIdMax]                 = {};
    char                       client_order_id[kClientOrderIdMax]        = {};
    char original_client_order_id[kClientOrderIdMax]                     = {};
    char                       reason[kSignalReasonMax]                  = {};
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

// ─────────────────────────────────────────────────────────────────────────────
// 꺼낸 칸은 믿지 않는다 — 값이 말이 되는지 보고 아니면 버린다
// ─────────────────────────────────────────────────────────────────────────────

// 요청 한 건이 말이 되는지 보는 기준. 종목·전략 수는 기동 때 표에서 받아 채운다.
//  [inv] 한 번 정하면 장중에 안 바뀐다 — 표가 커지는 자리(SymbolTable::intern)는 기동 구간뿐이다.
struct RequestLimits
{
    uint32_t symbol_count   = 0;             // 종목 표 크기. id는 1부터 이 수까지다(0은 없음)
    uint32_t strategy_count = 0;             // 전략 표 크기. 번호도 1부터다
    int32_t  quantity_max   = 1'000'000;     // 한 건 최대 수량
    double   price_max      = 100'000'000.0; // 한 주 최대 가격(원)
};

// 큐에서 꺼낸 요청이 주문이 되어도 되는가. 프로세스를 갈라도 남는 공유 면이 큐 하나뿐이라, 건너편이
//  망가졌거나 칸이 덮였을 때 그 값으로 주문을 내지 않으려고 여기를 지나게 한다. 거짓이면 버리고 센다.
//  순번 단조·중복은 DuplicateFilter가 본다 — 여기서는 값의 범위와 글자 칸이 0으로 끝나는지만 본다.
//  보는 기준이 액션마다 다르다 — 취소는 수량이 0이고 방향이 NONE 이어도 맞는 주문이다(대상은 원주문 번호로
//  찾는다). 여기서 NEW 기준 하나로 보면 전략이 낸 취소가 통째로 사라진다. [why D-114]
[[nodiscard]] bool is_plausible(const OrderRequest& request, const RequestLimits& limits) noexcept;

// 큐에서 꺼낸 응답이 말이 되는가. 사유 칸이 칸 안에서 끝나는지(끝나지 않으면 읽다가 칸을 넘는다)까지 본다.
[[nodiscard]] bool is_plausible(const OrderResponse& response) noexcept;

// 신호 하나를 요청 레코드로 옮긴다. 순번·시각은 신호가 이미 갖고 있는 것을 그대로 쓴다(SignalDispatcher::emit이 찍는다).
//  칸을 넘는 글자는 글자 경계에서 잘린다 — 잘렸는지는 truncated 가 참으로 알린다(널이면 안 알린다).
OrderRequest to_request(const OrderSignal& signal, bool* truncated = nullptr) noexcept;

// 요청 레코드를 신호로 되돌린다. 전략 이름은 레코드에 없어 받는 쪽이 전략 표에서 찾아 넘긴다.
//  [inv] 받는 쪽(주문 프로세스)에서만 부른다 — 표를 가진 쪽이 그쪽이다(원칙 4). [why D-114]
[[nodiscard]] OrderSignal to_signal(const OrderRequest& request, std::string_view strategy_id);

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
