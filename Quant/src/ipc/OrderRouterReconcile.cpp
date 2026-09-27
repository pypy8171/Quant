// 주문 라우터 — 재기동 미결 대조, 유령 선점 정리, 이전 세션 미체결 취소, 전송 타임아웃 되묻기, 잔고 대조 기록.
#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include "core/WakeGate.h"
#include "utils/ThreadName.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// 기동 직후 유령 지정가를 하나씩 취소할 때 취소 사이에 두는 간격(ms). 초당 거래건수 상한(EGW00201)을 피할 만큼만.
static constexpr int kStaleCancelGapMs = 400;

namespace
{
// 주문번호 없이 남은 INTENT를 KIS 미체결 한 건과 짝짓는다. KIS 주문 요청에는 우리 주문 id를 실을 칸이 없어
//  (order-cash 요청 필드, MCP 공식 예제 2026-09-25 확인) 종목·방향·수량·가격으로 맞춘다. 잔량은 죽은 사이
//  일부 체결됐을 수 있어 INTENT 수량 이하면 받는다. 시장가는 KIS 단가가 주문가와 달라 가격을 보지 않는다.
//  후보가 여럿이면 번호가 가장 작은 것 — INTENT를 주문 순서로 도니 먼저 낸 주문끼리 짝이 된다.
//  못 찾으면 nullptr. [why D-113]
const OpenOrder* match_unnumbered_intent(const OrderGate::OpenIntent& intent, const std::vector<OpenOrder>& open_orders,
                                         const std::unordered_set<uint64_t>& claimed_numbers)
{
    const OpenOrder* best        = nullptr;
    uint64_t         best_number = 0;

    for (const auto& open : open_orders)
    {
        const uint64_t number = digits_to_number(open.kis_order_no);

        if (number == 0 || claimed_numbers.count(number) > 0 || open.ticker != intent.ticker || open.side != intent.side ||
            open.psbl_qty <= 0 || open.psbl_qty > intent.remaining)
        {
            continue;
        }

        if (intent.type == OrderType::LIMIT && std::abs(open.ord_unpr - intent.price) >= 0.5)
        {
            continue;
        }

        if (best == nullptr || number < best_number)
        {
            best        = &open;
            best_number = number;
        }
    }

    return best;
}
} // namespace

// ─── 재기동 미결 주문 대조 ─────────────────────────────────────────────────
//  원장 저널의 미결 INTENT를 KIS 미체결과 맞춰 되살리거나 선점을 푼다. 세 갈래는 헤더 선언 주석에 있다. [why D-113]
OrderRouter::AdoptResult OrderRouter::adopt_open_intents(const std::vector<OrderGate::OpenIntent>& intents)
{
    auto& ledger = gate_.ledger();

    AdoptResult result;

    if (intents.empty())
    {
        return result;
    }

    // 주문번호 없는 INTENT = 전송 뒤 접수 응답 전에 죽은 주문일 수 있다. 모의는 그런 주문이 있을 때만 묻는다 —
    //  모의 미체결조회(VTTC0081R)로 ACCEPT를 본 주문까지 가리면 종전 판단이 바뀌므로 그 몫은 그대로 둔다.
    const bool has_unnumbered = std::any_of(intents.begin(), intents.end(),
                                            [](const OrderGate::OpenIntent& intent)
                                            {
                                                return intent.kis_order_number == 0;
                                            });
    std::vector<OpenOrder> open_orders;
    // 조회 실패는 "브로커에 못 물어봤다"로 둔다 — 빈 목록으로 읽으면 살아 있는 주문의 선점을 다 푼다. [why 전수조사 B1-2]
    const bool asked_broker = (!kis_.is_paper() || has_unnumbered) &&
                              fetch_open_orders(open_orders,
                                                "[OrderRouter] 재기동 미체결 조회 실패 — 접수된 주문은 살아 있는 것으로 둔다: ",
                                                "[OrderRouter] 재기동 미체결 조회 예외 — ");

    // 주문번호 → 미체결 한 건. 번호를 아는 INTENT가 먼저 제 몫을 차지해, 번호 없는 INTENT가 남의 주문과 짝지어지지 않게 한다.
    std::unordered_map<uint64_t, const OpenOrder*> open_by_number;
    std::unordered_set<uint64_t>                   claimed_numbers;

    for (const auto& open : open_orders)
    {
        open_by_number.emplace(digits_to_number(open.kis_order_no), &open);
    }

    for (const auto& intent : intents)
    {
        if (intent.kis_order_number != 0)
        {
            claimed_numbers.insert(intent.kis_order_number);
        }
    }

    for (const auto& intent : intents)
    {
        uint64_t         kis_order_number = intent.kis_order_number;
        const OpenOrder* open             = nullptr;
        bool             live             = false;

        if (kis_order_number == 0)
        {
            open = asked_broker ? match_unnumbered_intent(intent, open_orders, claimed_numbers) : nullptr;
            live = open != nullptr;

            if (live)
            {
                kis_order_number = digits_to_number(open->kis_order_no);
                claimed_numbers.insert(kis_order_number);
                // 다음 재기동이 같은 짝짓기를 되풀이하지 않게 ACCEPT를 적어 번호를 남긴다.
                ledger.on_accepted(intent.account, intent.ticker, intent.side, intent.remaining,
                                   OrderGate::OrderRef{intent.order_id, kis_order_number, intent.type});
                LOG_WARN(std::format("[OrderRouter] 재기동 미결 주문 짝 [{}] 접수 응답 전에 끊긴 주문 — KIS 미체결 ODNO={} {} {} {}주로 되살림",
                                     intent.order_id, kis_order_number, intent.ticker,
                                     intent.side == OrderSide::BUY ? "BUY" : "SELL", open->psbl_qty));
            }
        }
        else if (asked_broker && !kis_.is_paper())
        {
            const auto found = open_by_number.find(kis_order_number);
            open             = found != open_by_number.end() ? found->second : nullptr;
            live             = open != nullptr;
        }
        else
        {
            // 브로커에 못 물어본 경우(모의·조회 예외)는 ACCEPT를 본 주문을 살아 있는 것으로 본다 — 접수된 주문을
            //  지레 풀어 같은 수량을 또 내는 쪽이 더 큰 사고다.
            live = intent.accepted;
        }

        if (!live)
        {
            LOG_WARN(std::format("[OrderRouter] 재기동 미결 주문 선점 해제 [{}] ODNO={} {} {} {}주 — {}",
                                 intent.order_id, intent.kis_order_number, intent.ticker,
                                 intent.side == OrderSide::BUY ? "BUY" : "SELL", intent.remaining,
                                 asked_broker ? "KIS 미체결에 없다" : "미체결 조회를 못 했고 접수 기록도 없다"));
            ledger.on_cancel(intent.account, intent.ticker, intent.side, intent.remaining,
                            OrderGate::OrderRef{intent.order_id, intent.kis_order_number, intent.type});
            ++result.released;
            continue;
        }

        restore_intent(intent, kis_order_number, open);
        ++result.restored;
    }

    LOG_INFO(std::format("[OrderRouter] 재기동 미결 주문 대조: 되살림 {}건 · 선점 해제 {}건 (저널 {}건)", result.restored,
                         result.released, intents.size()));
    return result;
}

void OrderRouter::restore_intent(const OrderGate::OpenIntent& intent, uint64_t kis_order_number, const OpenOrder* open)
{
    OrderSignal signal;
    signal.ticker      = intent.ticker;
    signal.account_id  = intent.account;
    signal.side        = intent.side;
    signal.type        = intent.type;
    signal.quantity    = intent.remaining;
    signal.price       = intent.price;
    signal.strategy_id = intent.strategy_name.empty() ? std::string("UNLINKED") : intent.strategy_name;
    signal.reason      = "재기동 복원(원장 저널 미결 주문)";
    ManagedOrder managed_order = make_restored_order(std::format("ORD-{:06}", intent.order_id),
                                                     std::format("{:010}", kis_order_number), std::move(signal),
                                                     std::chrono::system_clock::now());

    // 정정·취소에 필요한 원주문 조직번호는 저널에 없다 — 미체결조회에서 찾았으면 그 값을, 아니면 빈 값으로 두고
    //  라우터가 취소를 미체결조회 결과로 낸다.
    if (open != nullptr)
    {
        managed_order.krx_forwarding_org_no = open->krx_forwarding_org_no;
    }

    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        history_.push(std::move(managed_order));
    }

    // 되살린 번호 위에서 이어 센다 — 같은 ORD-NNNNNN이 두 번 생기면 체결통보가 엉뚱한 주문에 붙는다.
    uint64_t seen = sequence_.load(std::memory_order_relaxed);

    while (seen < intent.order_id && !sequence_.compare_exchange_weak(seen, intent.order_id))
    {
    }
}

ManagedOrder OrderRouter::make_restored_order(std::string order_id, std::string kis_order_no, OrderSignal signal,
                                              std::chrono::system_clock::time_point at)
{
    auto&        ledger = gate_.ledger();
    ManagedOrder managed_order;
    managed_order.order_id              = std::move(order_id);
    managed_order.kis_order_no          = std::move(kis_order_no);
    managed_order.kis_order_number      = digits_to_number(managed_order.kis_order_no);
    managed_order.status                = OrderStatus::ACCEPTED;
    managed_order.signal                = std::move(signal);
    managed_order.signal.symbol_id      = ledger.intern_symbol(managed_order.signal.ticker); // 파일의 문자열 — 복원 때 한 번
    managed_order.signal.strategy_index = ledger.strategy_index_of(managed_order.signal.strategy_id); // 서브원장 귀속은 번호로
    managed_order.submitted_at          = at;
    managed_order.updated_at            = at;
    return managed_order;
}

// ─── 유령 선점 정리 ───────────────────────────────────────────────────────
//  원장(PositionLedger)의 선점(reserved_)은 전송 직전 INTENT 때 생기고 체결·취소·거부로만 풀린다. 통보를 한 번
//  놓치면 그 선점이 슬롯을 물고 남아, 실제 보유가 한도에 못 미치는데 신규 진입이 막힌다
//  (09-09: 보유 20인데 "25 >= 25" 거부). 살아 있는 주문이 없는 종목의 선점을 푼다.
int OrderRouter::sweep_stale_reservations()
{
    auto& ledger = gate_.ledger();

    std::vector<bool> live(ledger.symbols().capacity(), false);
    // [inv] 표시 목록을 읽고 원장 선점을 풀 때까지 in_flight_mutex_를 쥔다 — 그 사이에 주문 스레드가 표시를
    //  걸고 INTENT를 적으면, 읽을 때 없던 새 선점을 풀게 된다.
    std::lock_guard<std::mutex> in_flight_lock(in_flight_mutex_);

    for (const symbol::SymbolId symbol_id : in_flight_symbols_)
    {
        if (symbol_id < live.size())
        {
            live[symbol_id] = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        // [inv] 이력이 비면 아무 것도 풀지 않는다. 이력만 비는 경우(재기동 직후, 상한 초과로 잘려 나간 뒤)에
        //  정본으로 믿으면 살아 있는 선점을 통째로 푼다.
        if (history_.empty())
        {
            return 0;
        }

        for (const auto& managed_order : history_)
        {
            if (is_live(managed_order) && managed_order.signal.symbol_id < live.size())
            {
                live[managed_order.signal.symbol_id] = true;
            }
        }
    }

    const auto gone = ledger.prune_reservations(live);

    if (!gone.empty())
    {
        std::string list;

        for (const auto& gone_ticker : gone)
        {
            if (!list.empty())
            {
                list += ',';
            }

            list += gone_ticker;
        }

        LOG_WARN(std::format("[OrderRouter] 살아있는 주문 없는 선점 {}종목 해제 ({})", gone.size(), list));
    }

    return static_cast<int>(gone.size());
}

// ─── 전송 타임아웃 뒤 되묻기 ────────────────────────────────────────────────
//  응답을 못 받은 주문이 KIS에 접수돼 있으면 엔진 장부 밖에서 보유분을 묶는다. 부속 파일이
//  아는 번호와 견주어, 우리 것이 아닌 미체결만 지운다. 주문 스레드를 막지 않으려고 따로 돈다.
void OrderRouter::reconcile_unknown_order_async(std::string ticker)
{
    if (reconcile_busy_.exchange(true))
    {
        return;   // 앞 건이 돌고 있다 — 다음 타임아웃이나 다음 기동이 다시 잡는다
    }

    // 주문 스레드에서 번호를 받아 둔다 — 종목 표에 새로 넣는 일은 이 스레드 몫이다.
    const symbol::SymbolId symbol_id = gate_.ledger().intern_symbol(ticker);

    transport_reconcile_ = std::jthread([this, ticker = std::move(ticker), symbol_id](std::stop_token stop_token)
    {
        thread_name::set_current("Reconcile");

        // KIS가 접수를 조회에 반영할 틈을 준다. 곧바로 물으면 방금 낸 주문이 안 보인다.
        //  근거 없음(2026-09-27): 공식 샘플에 조회 반영 지연 설명이 없고, 곧바로 물어 안 보였던 실측 기록도 찾지 못했다.
        //  3초는 커밋 198005a에서 정한 값이다.
        //  3초를 한 번에 자지 않고 잘게 나눠 멈춤 요청을 본다 — 소멸자가 이 스레드를 기다린다.
        constexpr int  kSettleSlices = 30;
        constexpr auto kSettleSlice  = std::chrono::milliseconds(100);

        for (int slice = 0; slice < kSettleSlices && !stop_token.stop_requested(); ++slice)
        {
            std::this_thread::sleep_for(kSettleSlice);
        }

        if (stop_token.stop_requested())
        {
            reconcile_busy_ = false;
            return;
        }

        try
        {
            std::vector<std::string> known;
            {
                std::ifstream in(Logger::instance().path_for("open_orders.txt"));
                std::string line;

                while (std::getline(in, line))
                {
                    const size_t bar = line.find('|');

                    if (bar != std::string::npos)
                    {
                        known.push_back(line.substr(0, bar));
                    }
                }
            }

            ++kis_calls_;
            const auto open_orders = kis_.get_open_orders();

            if (!open_orders)
            {
                LOG_WARN("[OrderRouter] 전송 타임아웃 되묻기 — 미체결 조회 실패, 다음 기동이 다시 잡는다: " + error_text(open_orders));
                reconcile_busy_ = false;
                return;
            }

            // 파일은 쓰기 스레드가 늦게 쓰므로 조회가 끝난 뒤의 이력도 본다 — 그 사이 접수된 우리 주문이 파일에
            //  아직 없을 수 있다. 같은 종목이 KIS 답을 기다리는 중이면 번호를 모르는 우리 주문일 수 있어 건너뛴다.
            {
                std::lock_guard<std::mutex> in_flight_lock(in_flight_mutex_);

                if (std::find(in_flight_symbols_.begin(), in_flight_symbols_.end(), symbol_id) !=
                    in_flight_symbols_.end())
                {
                    LOG_INFO("[OrderRouter] 전송 타임아웃 되묻기 건너뜀 — 같은 종목 주문이 전송 중 " + ticker);
                    reconcile_busy_ = false;
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> history_lock(history_mutex_);

                for (const auto& managed_order : history_)
                {
                    if (!managed_order.kis_order_no.empty())
                    {
                        known.push_back(managed_order.kis_order_no);
                    }
                }
            }

            for (const auto& open : *open_orders)
            {
                if (open.ticker != ticker || open.kis_order_no.empty() || open.psbl_qty <= 0)
                {
                    continue;
                }

                if (std::find(known.begin(), known.end(), open.kis_order_no) != known.end())
                {
                    continue;   // 우리가 아는 주문이다
                }

                LOG_WARN("[OrderRouter] 전송 타임아웃 뒤 장부 밖 주문 발견 — 취소 " + open.ticker +
                         " ODNO=" + open.kis_order_no + " " + std::to_string(open.psbl_qty) + "주");
                const OrderAck cancelled = send_cancel(open.ticker, open.kis_order_no, open.krx_forwarding_org_no,
                                                       open.psbl_qty);

                if (cancelled.ok())
                {
                    if (open.side == OrderSide::SELL)
                    {
                        gate_.ledger().restore_sellable(std::string(), open.ticker, open.psbl_qty);
                    }
                }
                else
                {
                    LOG_WARN("[OrderRouter] 장부 밖 주문 취소 실패 — 다음 기동이 다시 지운다 " + open.ticker +
                             " ODNO=" + open.kis_order_no);
                }
            }
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 전송 타임아웃 되묻기 실패 — " + std::string(exception.what()));
        }

        reconcile_busy_ = false;
    });
}

// ─── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ──────────────────────
void OrderRouter::cancel_stale_orders_async()
{
    std::error_code             error_code;
    const std::filesystem::path path = Logger::instance().path_for("open_orders.txt");

    if (!std::filesystem::exists(path, error_code))
    {
        return;
    }

    std::vector<CarryRow> rows = read_open_orders_file(path);
    add_broker_open_orders(rows);

    if (rows.empty())
    {
        return;
    }

    // 파일은 비우지 않는다. 읽은 줄을 carry_rows_에 들고 있으면 이번 세션의 스냅샷마다 같이
    //  실리므로, 취소를 마치기 전에 죽거나 한도 거부로 남긴 주문도 다음 재기동에 그대로 넘어간다.
    {
        std::lock_guard<std::mutex> lock(carry_mutex_);
        carry_rows_.clear();

        for (const auto& row : rows)
        {
            if (parse_quantity(row[4]).value_or(0) > 0)
            {
                carry_rows_.push_back(row);   // 수량이 없는 줄은 취소할 것도 없다 — 넘기지 않는다
            }
        }
    }

    LOG_WARN("[OrderRouter] 이전 세션 미체결 " + std::to_string(rows.size()) +
             "건 발견 — 백그라운드 취소 시작(유령 주문이 현금을 묶고 청산 직후 재진입을 만든다)");

    if (stale_threshold_.joinable())
    {
        stale_threshold_.join();
    }

    // 취소는 건당 왕복 3~5초다. 기동 경로에서 돌리면 장중 재기동이 5분씩 멈춘다.
    //  잔고 시드는 이 스레드를 기다리지 않아도 된다 — 미체결 취소는 보유수량을 바꾸지
    //  않고 주문가능현금·매도가능수량만 푸는데, 둘 다 주기 잔고 대조가 다시 읽는다.
    //  rows는 스레드가 이 함수보다 오래 살아 옮겨 넣는다(참조로 잡으면 반환 뒤 사라진다).
    stale_threshold_ = std::jthread([this, rows = std::move(rows)](std::stop_token stop_token)
    {
        cancel_stale_rows(stop_token, rows);
    });
}

std::vector<OrderRouter::CarryRow> OrderRouter::read_open_orders_file(const std::filesystem::path& path)
{
    std::vector<CarryRow> rows;
    std::ifstream         in(path);
    std::string           line;

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line.empty())
        {
            continue;
        }

        CarryRow fields;
        size_t   position = 0;
        size_t   index    = 0;
        bool     parsed   = true;

        while (index < fields.size())
        {
            const size_t separator = line.find('|', position);

            if (index < fields.size() - 1 && separator == std::string::npos)
            {
                parsed = false;
                break;
            }

            // 마지막 칸은 구분자가 없어도 줄 끝까지 가져온다. 칸 번호를 늘리는 것과 읽는 것을
            //  한 식에 두면 어느 값으로 판단할지 정해지지 않아 마지막 칸이 잘릴 수도,
            //  끝까지 갈 수도 있었다(-Wsequence-point). 둘을 갈랐다.
            const size_t length = (index < fields.size() - 1 && separator != std::string::npos)
                                      ? separator - position : std::string::npos;
            fields[index] = line.substr(position, length);
            ++index;

            if (separator == std::string::npos)
            {
                break;
            }

            position = separator + 1;
        }

        if (parsed && index == fields.size() && !fields[0].empty())
        {
            rows.push_back(std::move(fields));
        }
    }

    return rows;
}

// 부속 파일은 우리가 ODNO를 받은 주문만 안다. 전송이 타임아웃 나면 KIS에는 접수됐는데 우리는 ODNO를
//  못 받아 파일에 못 적는다 — 그렇게 남은 주문은 아무도 취소해 주지 않는다(2026-09-23 09:26 021240,
//  ODNO=0000007886 매도 18주가 살아남아 보유분이 묶이고 손절 불능이 됐다). 브로커에 직접 물어 빠진
//  주문을 채운다. 조회가 실패하면 부속 파일 줄만 취소한다. [why D-101]
void OrderRouter::add_broker_open_orders(std::vector<CarryRow>& rows)
{
    std::vector<OpenOrder> open_orders;

    if (!fetch_open_orders(open_orders, "[OrderRouter] 미체결 보충 조회 실패 — 부속 파일 줄만 취소한다: ",
                           "[OrderRouter] 미체결 보충 조회 실패 — "))
    {
        return;
    }

    for (const auto& open : open_orders)
    {
        if (open.kis_order_no.empty() || open.psbl_qty <= 0)
        {
            continue;
        }

        const bool known = std::any_of(rows.begin(), rows.end(),
                                       [&open](const CarryRow& parts)
                                       {
                                           return parts[0] == open.kis_order_no;
                                       });

        if (known)
        {
            continue;
        }

        LOG_WARN("[OrderRouter] 부속 파일에 없는 미체결 — 브로커 조회로 보충 " + open.ticker + " ODNO=" +
                 open.kis_order_no + " " + std::to_string(open.psbl_qty) + "주");
        rows.push_back({open.kis_order_no, open.krx_forwarding_org_no, open.ticker,
                        open.side == OrderSide::SELL ? "SELL" : "BUY", std::to_string(open.psbl_qty)});
    }
}

void OrderRouter::cancel_stale_rows(std::stop_token stop_token, const std::vector<CarryRow>& rows)
{
    // 한도 거부·전송 실패 때 같은 취소를 다시 보내기까지 쉬는 시간과 최대 시도 수. 한도는 1초 창이다.
    constexpr int  kCancelAttempts   = 3;
    constexpr auto kRateLimitBackoff = std::chrono::milliseconds(1200);

    thread_name::set_current("StaleCancel");

    int cancelled = 0;

    for (const auto& row : rows)
    {
        if (stop_token.stop_requested())
        {
            LOG_WARN("[OrderRouter] 유령주문 취소 중단(종료 요청) — 남은 " +
                     std::to_string(rows.size() - static_cast<size_t>(cancelled)) + "건");
            return;
        }

        const std::optional<int> parsed_quantity = parse_quantity(row[4]);

        if (!parsed_quantity || *parsed_quantity <= 0)
        {
            continue;
        }

        const int quantity = *parsed_quantity;
        OrderAck  result;
        bool      rate_limited      = false;
        bool      transport_unknown = false;

        // 한도 거부(EGW00201)는 "이미 종료"가 아니다. 같은 분기로 흘리면 유령 예약이 KIS에
        //  남은 채 전략이 같은 종목을 새로 깔아 체결 시 이중 포지션이 된다(09-11 09:17~09:18
        //  180640·005935·007660 6건). 한도는 1초 창이라 잠깐 쉬고 다시 보낸다.
        for (int attempt = 0; attempt < kCancelAttempts; ++attempt)
        {
            try
            {
                result = send_cancel(row[2], row[0], row[1], quantity);
            }
            catch (const std::exception& exception)
            {
                LOG_WARN("[OrderRouter] 유령주문 취소 예외 " + row[2] + " ODNO=" + row[0] + " — " + exception.what());
                break;
            }

            rate_limited = !result.ok() && result.error_code == kis_error::kRateLimit;
            // 전송 실패는 '취소됨'이 아니라 '모름'이다 — 응답만 못 받았을 뿐 취소가 안 갔을 수 있다.
            //  아래에서 한도 거부와 같이 잔존으로 다룬다.
            transport_unknown = !result.ok() && result.error_code == kis_error::kTransport;

            if ((!rate_limited && !transport_unknown) || stop_token.stop_requested())
            {
                break;
            }

            std::this_thread::sleep_for(kRateLimitBackoff);
        }

        if (rate_limited || transport_unknown)
        {
            // 줄은 carry_rows_에 남긴다 — 다음 재기동이 다시 시도한다.
            //  전송 실패를 종료로 단정해 지우면 그 주문은 아무도 다시 지우지 않는다. 2026-09-23 09:43
            //  기동 취소에서 316140(ODNO=0000009703)·012750(ODNO=0000009712) 두 건이 그렇게 사라졌고,
            //  보유 37주·16주가 주문가능 0으로 묶여 손절이 닿아도 못 파는 상태가 됐다. [why D-101]
            LOG_WARN(std::string("[OrderRouter] 유령주문 취소 실패(") +
                     (rate_limited ? "한도 거부 반복" : "전송 실패 — 취소됐는지 모름") +
                     ") — KIS에 잔존 " + row[2] + " ODNO=" + row[0] + " " +
                     std::to_string(quantity) + "주 (부속 파일에 유지)");
            continue;
        }

        if (result.ok())
        {
            ++cancelled;
            LOG_INFO("[OrderRouter] 유령주문 취소 " + row[2] + " " + row[3] + " " +
                     std::to_string(quantity) + "주 ODNO=" + row[0]);

            // 취소로 브로커에서는 수량이 풀렸지만 원장(PositionLedger)의 sellable_은 잔고 시드값
            //  (ord_psbl_qty, 취소 전 스냅샷) 그대로다. 되돌리지 않으면 미체결이 없는데도
            //  자기 청산이 막힌다 — 09-09 000215은 13:45 취소 뒤 16분간 "매도가능수량 0"으로
            //  교체 진입이 네 번 무산됐다. 매도 취소만 해당한다(매수는 현금을 풀 뿐이다).
            if (row[3] == "SELL")
            {
                gate_.ledger().restore_sellable(std::string(), row[2], quantity);
            }
        }
        else
        {
            // 이미 체결·취소됐으면 KIS가 거부한다 — 정상이다.
            LOG_INFO("[OrderRouter] 유령주문 취소 불가(이미 종료 추정) " + row[2] + " ODNO=" + row[0]);
        }

        // 취소 접수든 이미 종료든 이 줄은 끝났다 — 부속 파일에서 뺀다.
        (void)erase_carry_row(row[0]);
        rewrite_open_orders();

        // 초당 거래건수 상한(EGW00201)에 걸리지 않게 간격을 둔다. 정지 요청이 오면 바로 깬다.
        wake::sleep_unless_stopped(stop_token, std::chrono::milliseconds(kStaleCancelGapMs));
    }

    LOG_INFO("[OrderRouter] 이전 세션 미체결 정리 완료: " + std::to_string(cancelled) + "건 취소 접수");
}

// ─── 잔고 대조 기록 (C-2) ─────────────────────────────────────────────────
//  live_orders는 history_에서 센다(history_mutex_). 파일 쓰기는 락 밖.
void OrderRouter::record_reconcile(const ReconcileNote& reconcile_note)
{
    int live_orders = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        const symbol::SymbolId reconcile_symbol = gate_.ledger().symbol_id_of(reconcile_note.ticker); // 대조 메모는 문자열 — 한 번만 바꾼다

        for (const auto& managed_order : history_)
        {
            if (is_live(managed_order) && managed_order.signal.symbol_id == reconcile_symbol)
            {
                ++live_orders;
            }
        }
    }

    std::string reason = std::format("live_orders={} diff_qty={}", live_orders, reconcile_note.broker_quantity - reconcile_note.ledger_quantity);

    if (!reconcile_note.note.empty())
    {
        reason += ' ';
        reason += OrderJournal::csv_safe(reconcile_note.note);
    }

    if (reconcile_note.action != "KEEP")
    {
        LOG_WARN(std::format("[OrderRouter] 잔고 대조 {} 원장 {}주@{} vs 브로커 {}주@{} → {} ({})", reconcile_note.ticker,
                             reconcile_note.ledger_quantity, static_cast<long long>(reconcile_note.ledger_average), reconcile_note.broker_quantity,
                             static_cast<long long>(reconcile_note.broker_average), reconcile_note.action, reason));
    }

    // 빈 칸: order_id·kis_order_no·strategy, side·type, 끝의 entry_reason·realized_pnl·sequence·strategy_realized_pnl.
    journal_.append_trade_line(std::format("RECONCILE,,,,{},NONE,,{},{:.2f},{},{:.2f},{},{},,,,", reconcile_note.ticker, reconcile_note.ledger_quantity,
                                  reconcile_note.ledger_average, reconcile_note.broker_quantity, reconcile_note.broker_average, OrderJournal::csv_safe(reconcile_note.action), reason));
}
