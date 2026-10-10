// 주문 라우터 — 생성·소멸, 미결주문 스냅샷, 이력 색인, 통계. 파일 쓰기는 OrderJournal.cpp가 맡는다.
//  발주 경로는 OrderRouterSubmit.cpp, 기동·재확인 대조는 OrderRouterReconcile.cpp, 체결은 OrderRouterFill.cpp.
#include "ipc/OrderRouter.h"

#include <format>
#include <iterator>
#include <vector>

// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    // 주문마다 부르는 곳이라 스트림을 쓰지 않는다(D-042). 6자리를 넘으면 자릿수만 늘어난다.
    return std::format("ORD-{:06}", ++sequence_);
}

void OrderRouter::continue_order_numbers(uint64_t highest) noexcept
{
    uint64_t current = sequence_.load();

    while (current < highest && !sequence_.compare_exchange_weak(current, highest))
    {
    }
}

uint64_t OrderRouter::order_number_of(std::string_view order_id) noexcept
{
    // next_id가 붙이는 머리글을 떼고 숫자만 읽는다 — 통째로 읽으면 'O'에서 멈춰 늘 0이 되었다(09-28 확인,
    //  D-113 저널 도입부터 order_id가 전부 0이라 재기동 미결 주문 대조가 한 번도 짝을 못 찾았다).
    constexpr std::string_view prefix = "ORD-";

    if (!order_id.starts_with(prefix))
    {
        return 0;
    }

    return digits_to_number(order_id.substr(prefix.size()));
}

OrderRouter::InFlightMark::InFlightMark(OrderRouter& router, symbol::SymbolId symbol_id)
    : router_(router), symbol_id_(symbol_id)
{
    std::lock_guard<std::mutex> lock(router_.in_flight_mutex_);
    router_.in_flight_symbols_.push_back(symbol_id_);
}

OrderRouter::InFlightMark::~InFlightMark()
{
    std::lock_guard<std::mutex> lock(router_.in_flight_mutex_);
    auto& symbols = router_.in_flight_symbols_;
    const auto found = std::find(symbols.begin(), symbols.end(), symbol_id_);

    if (found != symbols.end())
    {
        symbols.erase(found);
    }
}

// [inv] 부르는 쪽이 history_mutex_를 쥐고 있다. 정리(sweep_stale_reservations)가 같은 락 아래에서 acquire로 읽으므로
//  여기는 relaxed로 충분하다 — 락이 순서를 맞춘다.
OrderRouter::LedgerHandoff::LedgerHandoff(OrderRouter& router)
    : router_(router)
{
    router_.ledger_handoffs_.fetch_add(1, std::memory_order_relaxed);
}

// 장부 기록을 마친 뒤 내린다. release는 정리가 이 값을 0으로 읽을 때 장부 기록이 먼저 보이게 한다.
OrderRouter::LedgerHandoff::~LedgerHandoff()
{
    router_.ledger_handoffs_.fetch_sub(1, std::memory_order_release);
}

// ─── 미체결 주문 부속 파일 ─────────────────────────────────────────────────
//  형식: odno|orgno|ticker|side|remaining  (한 줄 한 주문, 헤더 없음)
//  history_는 프로세스 메모리라 재기동으로 사라진다. 그래서 살아있는 주문을 파일에
//  남겨 두고 다음 기동이 그것을 취소한다 — 기동 취소는 이 파일을 먼저 읽고, 빠진 주문은
//  브로커 미체결 조회로 채운다(모의투자도 VTTC0081R로 답한다). 매 상태변화마다 통째로 덮어쓰되, 쓰기는 전담 스레드가 한다.
//  [wire] 출처: VTTC0081R은 KIS 공식 샘플 inquire_daily_ccld의 모의 TR(2026-09-27 MCP 확인), 모의에서 쓰는 이유는 D-101.
//  주문 스레드에서 바로 쓰던 때는 건당 1,589us로 record_us의 절반을 먹었다 — 살아있는 주문이 수십 건이라
//  비용이 무시할 만하다고 본 것은 라이브 기준이었고, 951줄이 쌓이면 그렇지 않았다(2026-09-23 회차 E). [why D-123]
std::string OrderRouter::snapshot_open_orders_locked() const
{
    std::string buffer;

    for (const auto& history_entry : history_)
    {
        if (!is_live(history_entry) || history_entry.kis_order_no.empty())
        {
            continue;
        }

        std::format_to(std::back_inserter(buffer), "{}|{}|{}|{}|{}\n", history_entry.kis_order_no, history_entry.krx_forwarding_org_no, history_entry.signal.ticker,
                       history_entry.signal.side == OrderSide::BUY ? "BUY" : "SELL", outstanding_of(history_entry));
    }

    // 이전 세션 줄은 아직 취소가 안 끝난 것만 남아 있다 — 이번 세션 줄과 합쳐 쓴다.
    {
        std::lock_guard<std::mutex> lock(carry_mutex_);

        for (const auto& carry_row : carry_rows_)
        {
            std::format_to(std::back_inserter(buffer), "{}|{}|{}|{}|{}\n", carry_row[0], carry_row[1], carry_row[2], carry_row[3], carry_row[4]);
        }
    }

    return buffer;
}

void OrderRouter::rewrite_open_orders()
{
    std::string body;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        body = snapshot_open_orders_locked();
        sequence  = ++open_orders_sequence_;
    }

    journal_.queue_open_orders_file(std::move(body), sequence);
}

void OrderRouter::flush_file_writes()
{
    journal_.flush_append_outbox();
}

// ─── 생성 ─────────────────────────────────────────────────────────────────
//  복구 스레드는 멤버 중 맨 끝에 선언돼 있어 초기화도 맨 끝이다 — 스레드가 쓰는 멤버가 모두 준비된 뒤 뜬다.
#ifdef HAS_ZMQ
OrderRouter::OrderRouter(OrderGate& gate, IOrderExecutor& kis, ZmqBridge* zmq, OrderRouterConfig config)
    : gate_(gate), kis_(kis), config_(config), zmq_(zmq),
      unlinked_strategy_index_(gate.ledger().strategy_index_of("UNLINKED")),
      fill_recovery_([this](std::stop_token stop_token)
      {
          fill_recovery_loop(stop_token);
      })
{
}
#else
OrderRouter::OrderRouter(OrderGate& gate, IOrderExecutor& kis, OrderRouterConfig config)
    : gate_(gate), kis_(kis), config_(config),
      unlinked_strategy_index_(gate.ledger().strategy_index_of("UNLINKED")),
      fill_recovery_([this](std::stop_token stop_token)
      {
          fill_recovery_loop(stop_token);
      })
{
}
#endif

// ─── 소멸 — 스레드 회수 ───────────────────────────────────────────────────
OrderRouter::~OrderRouter()
{
    // jthread 소멸자가 같은 일을 하지만 그건 멤버 소멸 순서 안에서다 — 스레드가 쓰는 멤버가 먼저 죽지 않게 여기서 회수한다.
    //  재확인 스레드가 맨 먼저다 — 이 스레드가 쓰는 reconcile_mutex_·reconcile_pending_·kis_calls_는 선언이 뒤라 먼저 소멸한다.
    stop_join(fill_recovery_); // 조회 중이면 그 조회가 끝나야 멈춘다
    stop_join(transport_reconcile_);
    stop_join(stale_threshold_);
    // 라우터 스레드가 다 선 뒤에 기록기를 세운다 — 위 스레드들이 스냅샷·장부 줄을 넘기므로 먼저 세우면 그 줄이 남는다.
    //  미결주문 쓰기 스레드 → 남은 스냅샷 쓰기 → 장부·사유 쓰기 스레드 → 남은 줄 쓰기 순서다.
    journal_.stop();
}

symbol::SymbolId OrderRouter::symbol_of(const OrderSignal& signal)
{
    return signal.symbol_id != symbol::kNone ? signal.symbol_id : gate_.ledger().intern_symbol(signal.ticker);
}

// ─── 통계 ─────────────────────────────────────────────────────────────────
OrderRouter::Stats OrderRouter::statistics() const
{
    return {total_count_.load(), accepted_count_.load(), rejected_count_.load()};
}

// ─── 최근 N건 이력 ────────────────────────────────────────────────────────
std::vector<ManagedOrder> OrderRouter::recent(int count) const
{
    // 락 안에서 뜬 사본을 돌려준다 — 호출자는 락 밖에서 읽고, history_ 원소는 축출로 사라질 수 있다.
    std::lock_guard<std::mutex> lock(history_mutex_);
    return history_.recent(count);
}
