// 전략 → 주문 제어 요청 — 사고 파는 주문이 아니라 주문 쪽 표를 고치는 요청이다.
//  전략 쪽이 OrderGate·보호 주문 표를 직접 고치던 자리(슬롯 면제 집합·진입 우선순위 표·보호 주문 등록·
//  종목 표 등록·주문 쪽 스위치 다섯)를 이 레코드 한 줄로 바꾼다. 표를 고치는 일은 단일 시퀀서인 주문 스레드가 한다(원칙 4). [why D-114]
//  ipc/OrderChannel.h 와 같은 규칙이다 — 포인터도, 길이가 정해지지 않은 문자열도 안 싣는다
//  (계좌·티커는 고정 칸이다). 단계 4에서 프로세스가 갈리면
//  레코드는 그대로 두고 운반 수단만 공유메모리로 바꾼다. 처리량은 분당 몇 건이라 레코드 크기보다
//  어느 칸이 무슨 뜻인지가 먼저다 — 그래서 칸을 겹쳐 쓰지 않고 이름을 따로 둔다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/Types.h" // symbol::SymbolId, symbol::Ticker, strategy_table::StrategyId

namespace ipc
{

// 계좌 이름 칸. LedgerGlobals::account 와 같은 크기다 — 같은 값이 오간다.
constexpr size_t kControlAccountMax = 32;

// 거래소 칸. kWatchSubscribe 가 미국 거래소 코드("NAS"·"NYS")를 나른다 — 국내는 비어 있다.
constexpr size_t kControlExchangeMax = 8;

// 표 한 장이 담을 수 있는 줄 수. 슬롯 면제는 바스켓 소유 종목 수, 우선순위는 전 슬리브를 합친 수다.
//  넘으면 그 표는 버린다 — 반만 거는 것보다 낫다(아래 ControlTableBuilder 주석).
constexpr size_t kControlTableMax = 2048;

// 무엇을 고치라는 요청인가. 값은 로그·시험이 물고 가므로 끝에만 더한다.
enum class ControlKind : uint8_t
{
    kNone                = 0,
    kSlotExemptBegin     = 1, // 슬롯 면제 집합을 새로 연다
    kSlotExemptEntry     = 2, // 그 집합에 종목 하나
    kSlotExemptCommit    = 3, // 모은 집합을 통째로 건다
    kEntryPriorityBegin  = 4, // 진입 우선순위 표를 새로 연다
    kEntryPriorityEntry  = 5, // 그 표에 종목 하나(랭크·z)
    kEntryPriorityCommit = 6, // 모은 표를 통째로 건다(rank 칸에 전체 종목 수)
    kArmProtective       = 7, // 보호 주문 한 건 등록
    kDisarmProtective    = 8, // 보호 주문 한 건 해제
    kRegisterSymbol      = 9, // 종목 표에 티커 하나를 넣어 달라 — 넣는 쪽은 주문 프로세스 하나다
    kResetDaily          = 10, // 장이 열렸다 — 하루치 세기·중복방지 키·총평가금 기준선을 새로 연다
    kEntryHalt           = 11, // 신규 진입 정지 스위치(청산·취소는 그대로 통과한다)
    kEntryScale          = 12, // 신규 진입 매수 비율 0~1
    kKillSwitch          = 13, // 전방향 주문 차단
    kManualHalt          = 14, // 운영단말이 손으로 거는 한 방향 정지
    kWatchSubscribe      = 15, // 이 종목 시세를 구독해 달라 — 소켓을 쥔 쪽은 시세 프로세스 하나다
    kRegisterStrategy    = 16, // 전략 이름표에 이름 하나를 넣어 달라 — 넣는 쪽은 주문 프로세스 하나다
    kWatchUnsubscribe    = 17, // 이 종목 시세를 그만 받아 달라 — 구독 자리가 한정되어 있어 놓는 낱말이 있어야 한다
    kWatchPriority       = 18, // 이 종목의 구독 칸 우선순위(rank 칸, websocket_slot::priority_of) — 칸을 누구에게 줄지 시세 쪽이 고른다
};

// 이 낱말이 시세 프로세스로 가는가. 제어 줄은 낱말로 가른다 — 구독·해지만 전략 → 시세 줄로 가고 나머지는
//  전과 같이 전략 → 주문 줄로 간다. 줄 둘 다 보내는 쪽은 전략 하나라 SPSC 가 그대로다(받는 쪽만 다르다).
//  시세가 주문에게 무엇을 청하는 길은 내지 않는다 — 그 줄에 시세가 끼면 보내는 쪽이 둘이 된다. [why D-114]
//  [inv] 보내는 자리(Engine::send_control_switch)는 이 함수 하나로 줄을 고른다. 낱말을 더하면 여기도 본다.
[[nodiscard]] bool routes_to_feed(ControlKind kind) noexcept;

// 제어 요청 한 줄. 칸은 kind 마다 쓰는 것만 채우고 나머지는 기본값 그대로 둔다.
struct ControlRequest
{
    uint64_t                   sequence          = 0; // 보낸 순서. 0은 통로 밖에서 들어온 것이라 받지 않는다
    uint64_t                   batch             = 0; // 표를 여는 줄의 sequence. 그 표에 딸린 줄이 같은 값을 든다
    int64_t                    sent_at_ns        = 0; // steady_clock, 전략 쪽이 보낸 시각
    symbol::SymbolId           symbol_id         = symbol::kNone;
    strategy_table::StrategyId owner_index       = strategy_table::kNone; // kArmProtective 손익 귀속 전략 번호
    int32_t                    rank              = 0;   // kEntryPriorityEntry 랭크 / kEntryPriorityCommit 표의 전체 종목 수 / kWatchPriority 칸 우선순위
    uint32_t                   row_count         = 0;   // *Commit: 이 표로 보낸 줄 수. 받는 쪽이 셈이 맞는지 본다
    ControlKind                kind              = ControlKind::kNone;
    uint8_t                    toggle_on         = 0;   // kEntryHalt·kKillSwitch·kManualHalt — 켜면 1
    uint8_t                    halt_side         = 0;   // kManualHalt — OrderSide::Value 를 담는다
    uint8_t                    market            = 0;   // kWatchSubscribe — Market 을 담는다
    uint8_t                    trade_only        = 0;   // kWatchSubscribe — 호가 빼고 체결만 구독
    uint8_t                    is_future         = 0;   // kWatchSubscribe — 국내 선물 채널로 구독
    double                     score_z           = 0.0; // kEntryPriorityEntry
    double                     entry_scale       = 0.0; // kEntryScale
    double                     stop_loss_percent = 0.0; // kArmProtective
    double                     trail_arm_percent = 0.0; // kArmProtective
    double                     trail_percent     = 0.0; // kArmProtective
    char                       account[kControlAccountMax]   = {}; // kArmProtective·kDisarmProtective
    char                       exchange[kControlExchangeMax] = {}; // kWatchSubscribe — 미국 거래소 코드
    symbol::Ticker             ticker;                             // kRegisterSymbol·kWatchSubscribe
    // kRegisterStrategy. 티커 칸(15자)을 겹쳐 쓰지 않는다 — 전략 이름은 설정이 정해 31자까지 온다.
    strategy_table::StrategyName strategy_name;
};

// 계좌 이름을 칸에 담는다. 칸을 넘으면 자르고 끝에 0을 넣는다.
void set_account(ControlRequest& request, std::string_view account) noexcept;

// 칸에 담긴 계좌 이름. [inv] 돌려주는 조각은 request 가 사는 동안만 유효하다.
[[nodiscard]] std::string_view account_of(const ControlRequest& request) noexcept;

// 거래소 코드를 칸에 담는다. 칸을 넘으면 자르고 끝에 0을 넣는다.
void set_exchange(ControlRequest& request, std::string_view exchange) noexcept;

// 칸에 담긴 거래소 코드. [inv] 돌려주는 조각은 request 가 사는 동안만 유효하다.
[[nodiscard]] std::string_view exchange_of(const ControlRequest& request) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// 주문 쪽 — 여러 줄로 나뉘어 오는 표 하나를 모은다
// ─────────────────────────────────────────────────────────────────────────────

// begin 으로 열고 add 로 쌓고 commit 으로 닫는다. 열지 않고 온 줄, 다른 표의 줄, 상한을 넘은 줄은
//  버리고 센다 — 표를 반만 거는 것이 제일 나쁘다(슬롯 면제가 반만 걸리면 멀쩡한 바스켓 보유분이
//  남의 슬롯을 먹고, 우선순위가 반만 걸리면 랭크를 잃은 종목이 우선순위 바를 건너뛴다). [why D-114]
//  표를 만드는 쪽이 둘 이상이어도 batch 가 갈라 준다 — 남의 표 줄은 섞이지 않는다.
//  [inv] 주문 스레드 하나만 부른다 — 동기화는 없다.
class ControlTableBuilder
{
public:
    explicit ControlTableBuilder(size_t capacity);

    // 새 표를 연다. 열려 있던 표가 있으면 그 줄을 전부 버리고 센다(commit 을 못 받은 표다).
    void begin(uint64_t batch);

    // 열린 표에 줄 하나.
    void add(const ControlRequest& request);

    // 표를 닫는다. 참이면 rows() 가 걸 표다. expected_rows 는 여는 쪽이 센 줄 수 —
    //  큐가 가득 차 중간이 통째로 사라지면 받는 쪽은 그 줄을 본 적이 없어 모른다. 그래서 수를 같이 보낸다.
    [[nodiscard]] bool commit(uint64_t batch, size_t expected_rows);

    [[nodiscard]] const std::vector<ControlRequest>& rows() const noexcept
    {
        return rows_;
    }

    [[nodiscard]] bool is_open() const noexcept
    {
        return open_;
    }

    // 버린 줄 수. 0이 아니면 큐가 가득 찼거나 표를 만드는 쪽이 둘 이상이라는 뜻이다.
    [[nodiscard]] uint64_t discarded() const noexcept
    {
        return discarded_;
    }

private:
    std::vector<ControlRequest> rows_;
    size_t                      capacity_   = 0;
    uint64_t                    open_batch_ = 0;
    uint64_t                    discarded_  = 0;
    bool                        open_       = false;
    bool                        spoiled_    = false; // 열린 표의 줄을 하나라도 잃었다 — commit 이 거짓을 돌려준다
};

} // namespace ipc
