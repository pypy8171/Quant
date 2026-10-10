// 장부 사본 — 전략 쪽이 OrderGate를 직접 보지 않고 이것만 본다. 주문 쪽이 한 바퀴 끝에 채우고, 전략 쪽은 읽기만 한다.
//  포인터도 std::string도 담지 않는다. 단계 4에서 이 구조체가 공유메모리에 그대로 얹히기 때문이다 — 주소는 프로세스마다
//  다르므로 포인터를 담으면 건너간 쪽에서 엉뚱한 곳을 가리킨다. 종목은 전부 정수 id 배열 인덱스다.
//  글자를 담아야 하는 것은 계좌 이름 하나뿐이고, 그것만 고정 길이 char 배열로 둔다(배열은 주소가 아니라 값이라
//  그대로 건너간다). [why D-114]
//
//  [inv] 한 번에 한 스레드만 판을 뒤집는다 — 장부를 바꾸는 스레드가 둘(접수·체결)이라
//  OrderGate가 발행 전용 잠금으로 줄을 세운다. 읽는 쪽은 여럿이어도 되고 잠금을 안 잡는다.
//  판 번호(version_)가 홀수면 쓰는 중이라 읽은 값이 반쪽일 수 있다 — read_stable이 다시 읽는다.
//  사본에 "낡음 허용치"는 두지 않는다. 낡은 보유수량으로 매수하면 이중 발주이기 때문이다.
//
//  [inv] 한 판에 싣는 줄 수는 "보유 + 미체결"이라 슬롯 상한(config max_concurrent_positions, 지금 20~30)
//  언저리의 수십 줄이다. 이 폭을 넘겨 쓰면 읽는 쪽이 밀린다 — 쓰는 창이 넓으면 읽는 쪽이 판 번호가
//  짝수인 순간을 못 잡고 계속 되읽는다. 300ms 동안 쉼 없이 실어 재면 64줄은 읽기 16만~27만 번,
//  2,000줄은 44~90번이다. 종목 전체(8,192줄)를 한 판에 싣고 싶어지면 그때는 이 방식이 아니라
//  판을 여러 장 두고 돌려 쓰는 쪽으로 바꿔야 한다.
//
//  한 계좌만 담는다. 다계좌는 계좌당 프로세스로 가기로 했으므로(엔진 안 다계좌 금지) 계좌 축이 필요 없다.
//  다른 계좌의 값이 섞여 들어오면 publish 쪽이 세어서 판정 행에 올린다.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/SymbolTable.h"

namespace ipc
{

// 종목 하나의 장부 값. 전략이 한 종목을 볼 때 이 넷을 함께 본다 — 따로 읽으면 그 사이에 판이 바뀌어
//  "보유 0인데 매도가능 3"처럼 서로 안 맞는 조합을 본다.
struct LedgerRow
{
    // 이 줄을 채운 판 번호. 지금 판과 다르면 이번 판에 값이 없는 것이다(보유도 선점도 0).
    //  이 표시가 있어서 판마다 8,192줄을 0으로 미는 일이 없다 — 채운 줄만 최신이고 나머지는 저절로 빈 줄이 된다.
    uint64_t stamp         = 0;
    double   average_price = 0.0; // 평단(원)
    int32_t  position      = 0;   // 확정 보유 수량
    int32_t  reserved      = 0;   // 미체결 선점 순값(매수 - 매도). 선점이 있는지 볼 때 쓴다
    int32_t  sellable      = 0;   // 지금 팔 수 있는 수량(상한 - 미체결 매도, 음수면 0)
    int32_t  reserved_sell = 0;   // 미체결 매도 선점. 순값에서 뒤집어 쓰면 매수가 같이 걸린 종목의 매도가 가려진다
    uint8_t  slot_exempt   = 0;   // 슬롯 계산 밖 종목(바스켓 슬리브 소유) [why D-109]
    uint8_t  padding_[7]   = {};  // 40바이트로 맞춘다 — 공유메모리에서 양쪽 컴파일러가 같은 칸을 보게
};

// 종목과 무관한 값들. 기동 시 고정되는 넷도 여기 같이 둔다 — 전략이 config를 따로 들 일이 없게.
struct LedgerGlobals
{
    double  entry_scale              = 1.0; // 매수 명목 비율 0~1
    double  max_notional_per_ticker  = 0.0; // 종목당 명목 한도(원). 기동 시 고정
    double  displace_unscored_z      = 0.0; // 점수 없는 종목을 교체 후보로 볼 때 쓰는 z. 기동 시 고정
    int32_t open_slot_count          = 0;   // 열린 슬롯 수
    int32_t max_concurrent_positions = 0;   // 동시 보유 슬롯 한도. 기동 시 고정
    uint8_t entry_halted             = 0;   // 진입 정지 — 네 원천의 OR 결과만 싣는다(원천 구분은 주문 쪽 일이다)
    uint8_t manual_sell_halted       = 0;   // 전략 매도 정지
    uint8_t capacity_full            = 0;   // 신규 종목을 열 여력이 없다(자리 또는 예산)
    uint8_t displace_enabled         = 0;   // 교체 진입 사용. 기동 시 고정

    // 이 판이 담은 계좌 이름. 한 판은 한 계좌만 담으므로 하나면 된다. 전략 쪽이 강제청산·한도 정리 주문에
    //  계좌를 적으려면 이것이 있어야 한다 — 없으면 주문 쪽 계좌 표를 다시 들여다보게 된다.
    //  [inv] 채우는 쪽이 끝에 0을 넣어 끊는다. 32칸이면 계좌번호 문자열의 세 배다.
    char    account[32]              = {};
    uint8_t padding_[4]              = {};  // 72바이트(8의 배수)로 맞춘다 — 공개 칸을 8바이트 단어로 옮기기 때문이다
};

// 진입 판단에 필요한 셋을 한 판에서 함께 본 것. OrderGate::EntrySnapshot과 같은 뜻이고,
//  같은 이유로 따로 읽지 않는다. [why D-086]
struct EntryView
{
    int32_t position      = 0;
    int32_t reserved      = 0;
    bool    capacity_full = false;
};

// 공개 칸은 8바이트 단어 배열이다. 쓰는 쪽과 읽는 쪽이 같은 칸을 겹쳐 만지므로 단어마다 원자 연산으로 옮긴다 —
//  보통 메모리로 두면 판 번호로 버리더라도 C++ 규칙상 경합이고, 컴파일러가 읽기를 합치거나 쪼갤 수 있다.
static_assert(sizeof(LedgerRow) % sizeof(uint64_t) == 0, "LedgerRow는 8바이트 단어로 나뉘어야 한다");
static_assert(sizeof(LedgerGlobals) % sizeof(uint64_t) == 0, "LedgerGlobals는 8바이트 단어로 나뉘어야 한다");
static_assert(std::is_trivially_copyable_v<LedgerRow>, "LedgerRow는 memcpy로 옮긴다");
static_assert(std::is_trivially_copyable_v<LedgerGlobals>, "LedgerGlobals는 memcpy로 옮긴다");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "공유메모리 단어는 잠금 없는 원자여야 한다");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "공유메모리 단어는 잠금 없는 원자여야 한다");

class LedgerSnapshot
{
public:
    // SymbolTable 기본 capacity와 같은 값. 배열을 고정으로 두는 이유는 공유메모리에 얹으려면 크기가
    //  기동 전에 정해져 있어야 해서다 — vector는 힙 주소를 들고 있어 건너가지 못한다. [why D-114]
    static constexpr size_t kMaxSymbols = 8192;

    static constexpr size_t kRowWords     = sizeof(LedgerRow) / sizeof(uint64_t);
    static constexpr size_t kGlobalsWords = sizeof(LedgerGlobals) / sizeof(uint64_t);

    // 판이 안정되기를 기다리는 한도. 쓰는 쪽이 판을 연 채 멈추면(예외·정지) 읽는 쪽이 끝없이 돌던 것을 막는다.
    //  처음 kStableReadSpinAttempts번은 바로 다시 읽고, 그 뒤로는 양보하며 kStableReadDeadline까지 기다린다.
    //  한 판을 쓰는 데 걸리는 시간은 수십 줄 기준 수 마이크로초라 50ms는 정상 판에서는 닿지 않는 값이다.
    static constexpr int                       kStableReadSpinAttempts = 256;
    static constexpr std::chrono::milliseconds kStableReadDeadline{50};

    // ── 주문 쪽(쓰기) ────────────────────────────────────────────────────────
    // 한 바퀴 끝에 이 순서로 부른다: begin_publish → 값 채우기 → end_publish.
    //  값은 쓰는 쪽 전용 칸(staging)에 채우고, end_publish가 판 번호가 홀수인 동안 공개 칸으로 옮긴다.
    // [lock-order] 잠금이 없다. 판 번호만 홀짝으로 뒤집는다.
    void begin_publish() noexcept;

    void end_publish() noexcept;

    // 쓰는 쪽 전용 참조. 이번 판에 처음 손대는 줄이면 0으로 되돌려 놓고 준다.
    //  id가 상한을 넘으면 버리는 칸을 준다 — 호출부가 매번 범위를 확인하지 않게.
    //  [inv] 돌려준 참조는 end_publish 전까지만 쓴다 — 그 뒤에 쓴 값은 다음 판에야 실린다.
    [[nodiscard]] LedgerRow& row_for_write(symbol::SymbolId id) noexcept;

    [[nodiscard]] LedgerGlobals& globals_for_write() noexcept
    {
        return staging_globals_;
    }

    // ── 전략 쪽(읽기) ────────────────────────────────────────────────────────
    // 셋 다 판이 바뀌면 스스로 다시 읽는다. 돌려주는 값은 한 판에서 본 것이라 서로 맞는다.
    //  판이 kStableReadDeadline 안에 안정되지 않으면 보수적인 값을 돌려주고 stale_read_count()를 올린다:
    //  row는 빈 줄, entry는 capacity_full=true, globals는 진입·매도 정지와 capacity_full을 켠 값, collect_rows는 0.
    [[nodiscard]] LedgerRow row(symbol::SymbolId id) const noexcept;

    [[nodiscard]] LedgerGlobals globals() const noexcept;

    [[nodiscard]] EntryView entry(symbol::SymbolId id) const noexcept;

    // 이번 판에 값이 실린 종목을 한 판에서 모아 온다 — 강제청산·한도 정리·보호 주문이 보유 전체를 훑는 자리다
    //  (OrderGate::snapshot_positions()가 하던 일). 돌려주는 줄은 전부 같은 판의 것이라 서로 맞는다.
    //  capacity보다 실린 종목이 많으면 capacity개까지만 채우고 실제 수를 돌려준다 — 호출부가 잘렸는지 안다.
    [[nodiscard]] size_t collect_rows(symbol::SymbolId* out_ids, LedgerRow* out_rows, size_t capacity) const noexcept;

    // 지금까지 몇 판 나왔는가. 사본이 한 바퀴 안에 안 바뀌는 것을 밖에서 잡는다.
    [[nodiscard]] uint64_t generation() const noexcept;

    // 판이 안정되지 않아 보수값을 돌려준 횟수(기동 후 누적). 0이 아니면 쓰는 쪽이 판을 연 채 멈춘 적이 있다.
    [[nodiscard]] uint64_t stale_read_count() const noexcept
    {
        return stale_read_count_.load(std::memory_order_relaxed);
    }

private:
    // 판이 안정될 때까지 다시 읽는다. 쓰는 쪽이 한 바퀴에 한 번만 판을 뒤집으므로 되읽기는 드물다.
    //  kStableReadDeadline이 지나도 안정되지 않으면 nullopt — 호출부가 보수값으로 바꾼다.
    //  [inv] reader는 이 사본만 읽고 부수효과가 없어야 한다 — 버려지는 판을 읽을 수 있다.
    template <typename Reader>
    auto read_stable(Reader&& reader) const noexcept -> std::optional<decltype(reader())>
    {
        // 시계는 첫 몇 번이 실패한 뒤에야 본다 — 정상 판에서는 한 번에 끝나므로 시계 값을 읽지 않는다.
        std::chrono::steady_clock::time_point deadline{};
        int                                   attempts = 0;

        while (true)
        {
            const uint64_t before = version_.load(std::memory_order_acquire);

            if ((before & 1U) == 0U)
            {
                auto value = reader();

                // 값을 다 읽은 뒤에 판 번호를 다시 본다. acquire 로드는 "뒤에 오는 읽기"만 묶으므로
                //  이 울타리가 없으면 위의 값 읽기가 아래로 내려가 다시 본 판 번호보다 늦게 일어날 수 있다.
                //  울타리 없이 2,000줄 판을 돌렸을 때 읽기 12,800번당 반쪽 판이 4~6번 나왔다.
                std::atomic_thread_fence(std::memory_order_acquire);

                if (version_.load(std::memory_order_relaxed) == before)
                {
                    return value;
                }
            }

            ++attempts;

            if (attempts < kStableReadSpinAttempts)
            {
                continue;
            }

            const auto now = std::chrono::steady_clock::now();

            if (attempts == kStableReadSpinAttempts)
            {
                deadline = now + kStableReadDeadline;
            }
            else if (now >= deadline)
            {
                stale_read_count_.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }

            std::this_thread::yield();
        }
    }

    // 공개 칸 한 줄을 단어 단위로 읽어 구조체로 옮긴다. read_stable의 reader 안에서만 부른다.
    [[nodiscard]] LedgerRow load_published_row(size_t index) const noexcept;

    std::atomic<uint64_t> version_{0};    // 짝수면 읽어도 되는 판, 홀수면 쓰는 중
    std::atomic<uint64_t> generation_{0}; // 몇 번째 판인가. 줄의 stamp와 맞춰 본다

    // 공개 칸 — 읽는 쪽이 보는 것은 이것뿐이다. end_publish만 쓴다.
    std::atomic<uint64_t> published_globals_[kGlobalsWords]{};
    std::atomic<uint64_t> published_rows_[kMaxSymbols][kRowWords]{};

    // 쓰는 쪽 전용 칸 — 읽는 쪽은 만지지 않는다. 판 사이에 값이 남는다(globals는 다음 판도 그대로 쓴다).
    LedgerGlobals         staging_globals_{};
    LedgerRow             staging_rows_[kMaxSymbols]{};
    LedgerRow             discard_{}; // 상한을 넘은 id가 쓰고 버리는 칸

    // 이번 판에 값을 실은 종목 번호만 모아 둔 촘촘한 목록. 전체를 훑을 때 8,192줄을 다 뒤지지 않으려고 둔다.
    //  쓰는 쪽은 판 번호가 홀수인 동안만 채우고, 읽는 쪽은 판 번호 안에서 written_count_까지만 본다.
    std::atomic<symbol::SymbolId> written_ids_[kMaxSymbols]{};
    std::atomic<uint32_t>         written_count_{0};

    // 판이 안정되지 않아 보수값을 돌려준 횟수. 읽는 쪽 여럿이 올리므로 원자다.
    mutable std::atomic<uint64_t> stale_read_count_{0};
};

// ── 읽는 쪽 편의 ────────────────────────────────────────────────────────────
//  이번 판에 실린 줄을 하나도 빠짐없이 담는다. 처음엔 작게 잡고, 모자라면 collect_rows가 알려 준 실제 수만큼
//  키워 한 번 더 읽는다. 버퍼를 처음부터 kMaxSymbols(295KB)로 잡아 두지 않으려고 이렇게 한다.
//  vector를 쓰는 것은 읽는 쪽이라 괜찮다 — 공유메모리로 건너가는 것은 LedgerSnapshot 자체뿐이다. [why D-114]
//  [inv] ids와 rows는 같은 길이로 맞춰서 돌려준다. 같은 자리끼리 짝이다.
void collect_all_rows(const LedgerSnapshot& snapshot, std::vector<symbol::SymbolId>& ids, std::vector<LedgerRow>& rows);

} // namespace ipc
