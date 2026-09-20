#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include "core/WakeGate.h"
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

// ─── 오늘 날짜 YYYYMMDD (KST) ────────────────────────────────────────────
//  날짜별 파일 이름에 쓴다. 원장 CSV 파일명·행 시각과 같은 기준(KST 고정, 머신 TZ 무관)이다. [why D-070]
static std::string today_ymd()
{
    return kst::date_yyyymmdd(std::time(nullptr));
}

// 거래일 YYYYMMDD 정수 — 체결통보 키의 날짜 칸. 문자열을 만들지 않는다.
static uint32_t trade_date_number(std::time_t now_utc)
{
    const std::chrono::year_month_day date = kst::date(now_utc);
    return static_cast<uint32_t>(static_cast<int>(date.year())) * 10000u + static_cast<unsigned>(date.month()) * 100u +
           static_cast<unsigned>(date.day());
}


// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    // 주문마다 부르는 곳이라 스트림을 쓰지 않는다(D-042). 6자리를 넘으면 자릿수만 늘어난다.
    return std::format("ORD-{:06}", ++sequence_);
}

// ─── 거부 사유에 KIS 오류코드 꼬리표 부착 ───────────────────────────────
//  order_thread가 EGW00201(초당 거래건수 초과)을 문자열로 판별해 적응적 재시도를 걸 수 있게,
//  응답의 err_code를 " [코드]" 형태로 reject_reason 끝에 붙인다. 코드 없으면 빈 문자열.
std::string OrderRouter::kis_error_suffix(const OrderAck& acknowledgement)
{
    return acknowledgement.error_code.empty() ? std::string() : (" [" + acknowledgement.error_code + "]");
}

// ─── 주문 제출 — action에 따라 라우팅 (MM-1) ─────────────────────────────
//  전 경로가 단일 order_thread에서만 실행된다(Engine::order_thread_fn) — OrderGate C6의
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다.
// 취소가 "취소 대상 없음"으로 되돌아온 뒤 그 종목의 신규 매수를 막아 두는 시간(초).
//  전략의 재구성 주기(min_action_ms 3초 + 재조회)보다 길고, 존 이탈 청산을 늦출 만큼
//  길지는 않은 값. 이 창 안에 들어온 매수는 원주문 체결분과 겹칠 수 있다.
static constexpr int kCancelMissGuardSec = 10;
// 같은 종목·같은 전략의 시장가 매도가 접수된 뒤 체결 통보가 아직 없을 때, 같은 매도를 다시 KIS로 보내지 않는 시간(초).
//  발주 스레드가 밀리면 통보까지 몇 분이 걸릴 수 있어 전략 백오프(30초)보다 길게 잡는다. 지나면 통보를 잃은
//  것으로 보고 놓아준다 — 그 뒤는 게이트 선점 클램프·자가정리가 막는다.
static constexpr int kDupMarketSellGuardSec = 120;

ManagedOrder OrderRouter::submit(const OrderSignal& signal)
{
    switch (signal.action)
    {
    case OrderAction::CANCEL:  return cancel_route(signal);
    case OrderAction::REPLACE: return replace_route(signal);
    case OrderAction::NEW:
    default:                   return new_route(signal);
    }
}

// ─── 신규 주문 (기존 경로) ─────────────────────────────────────────────────
ManagedOrder OrderRouter::new_route(const OrderSignal& in_signal)
{
    auto now = std::chrono::system_clock::now();

    // 0. 한도 클램프 — 한도를 넘치면 거부 대신 한도 안으로 줄여 낸다.
    //    분할 매수 전략은 매 틱 같은 분할 단계를 다시 내므로, 넘친다고 버리면 그 종목은 하루 종일
    //    한 주도 못 나가면서 초당 주문 예산만 태운다(09-08 오전 126640·293490 반복 거부).
    //    여유가 0이면 손대지 않는다 — 아래 check()가 어느 한도에 걸렸는지 그대로 남기게 둔다.
    OrderSignal signal = in_signal; // 사본 — 아래에서 수량을 잘라 고친다
    signal.symbol_id   = symbol_of(signal); // 이력 항목이 종목 id를 들게 — 아래 비교·색인이 문자열을 안 본다
    bool sell_no_quantity = false;
    const int allowed = gate_.clamp_buy_quantity(signal);

    if (allowed == 0 && signal.side == OrderSide::SELL && signal.action == OrderAction::NEW &&
        signal.quantity > 0)
    {
        // 매도가능수량이 0이면 여기서 끊는다. BUY와 달리 아래 check()는 매도가능수량을 모르므로
        //  그대로 통과시키고, KIS가 주문을 통째로 40240000(모의투자 잔고내역이 없습니다)으로
        //  거부한다 — 한 주도 못 빠져나오면서 초당 주문 예산만 태운다(09-08 001450 105주·
        //  086450 486주·047050 254주/381주가 모두 이 경로로 전량 거부됐다).
        sell_no_quantity = true;
    }
    else if (allowed > 0 && allowed < signal.quantity && signal.side == OrderSide::SELL &&
             gate_.sellable_view(signal.account_id, signal.ticker).pending > 0)
    {
        // 매도가능이 모자란 이유가 이 세션의 예약매도(익절 지정가)라면 잘라 내지 않고, 그 예약을 취소해
        //  수량을 풀고 전량을 낸다 — 아래 sell_no_quantity 와 같은 길. 잘라 내면 나머지는 전략이 취소를 낸 뒤
        //  다음 백오프(30초 뒤)에야 나간다(09-14 15:15 012210 115주 중 100주만, 나머지는 15:16 뒤). [why D-082]
        sell_no_quantity = true;
    }
    else if (allowed > 0 && allowed < signal.quantity)
    {
        LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주", signal.ticker, signal.quantity, allowed));
        signal.quantity = allowed;
    }

    ManagedOrder managed_order;
    managed_order.order_id    = next_id();
    managed_order.signal      = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;

    ++total_count_;

    // 방금 이 종목의 취소가 "취소 대상 없음"으로 되돌아왔다면, 원주문이 이미 체결됐을 수
    //  있다. 전략은 그 결과를 보지 못한 채 대체 주문을 이어 내므로 그대로 두면 중복 매수가
    //  된다. 다음 재구성 주기에 전략이 실제 보유수량을 다시 읽을 때까지만 막는다.
    //  매도는 막지 않는다 — 노출을 줄이는 쪽이고, 늦추면 손실이 커진다.
    if (signal.side == OrderSide::BUY)
    {
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);

            if (signal.symbol_id < cancel_miss_.size() && cancel_miss_[signal.symbol_id] != std::chrono::steady_clock::time_point{})
            {
                const auto age = std::chrono::steady_clock::now() - cancel_miss_[signal.symbol_id];

                if (age < std::chrono::seconds(kCancelMissGuardSec))
                {
                    blocked = true;
                }
                else
                {
                    cancel_miss_[signal.symbol_id] = {};
                }
            }
        }

        if (blocked)
        {
            managed_order.status        = OrderStatus::REJECTED;
            managed_order.reject_reason = "직전 취소가 대상 없음 — 보유수량 재확인까지 보류";
            ++rejected_count_;
            LOG_WARN("[OrderRouter] 대체 주문 보류 [" + managed_order.order_id + "] " + signal.ticker +
                     " " + managed_order.reject_reason);
            record(managed_order);
            return managed_order;
        }
    }

    // 같은 청산의 중복 발주 차단 — 시장가 매도가 접수돼 아직 체결 통보가 없는데(발주 스레드가 밀리면 몇 분)
    //  전략이 백오프마다 같은 매도를 다시 낸다(09-14 15:15 036930 SELL 9 가 30초·60초 뒤 두 번 더 큐에 쌓임).
    //  라우터 이력에 같은 종목·같은 전략의 시장가 매도가 미체결 잔량을 들고 살아 있으면 KIS 로 보내지 않는다.
    //  KIS 호출이 없으니 발주 스레드 예산을 안 쓴다. [why D-082]
    if (signal.side == OrderSide::SELL && signal.action == OrderAction::NEW && signal.type == OrderType::MARKET)
    {
        std::string duplicate;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            const auto now_sc = std::chrono::system_clock::now();

            for (const auto& history_entry : history_)
            {
                if (history_entry.signal.symbol_id != signal.symbol_id || history_entry.signal.strategy_index != signal.strategy_index ||
                    history_entry.signal.side != OrderSide::SELL || history_entry.signal.type != OrderType::MARKET ||
                    history_entry.signal.action != OrderAction::NEW)
                {
                    continue;
                }

                if ((history_entry.status != OrderStatus::ACCEPTED && history_entry.status != OrderStatus::SUBMITTED) ||
                    history_entry.confirmed_quantity >= history_entry.signal.quantity ||
                    now_sc - history_entry.submitted_at > std::chrono::seconds(kDupMarketSellGuardSec))
                {
                    continue;
                }

                duplicate = history_entry.order_id + " 미체결 " + std::to_string(history_entry.signal.quantity - history_entry.confirmed_quantity) + "주";
                break;
            }
        }

        if (!duplicate.empty())
        {
            managed_order.status        = OrderStatus::REJECTED;
            managed_order.reject_reason = "같은 시장가 매도 진행 중 [" + duplicate + "] — 중복 발주 생략";
            ++rejected_count_;
            LOG_INFO("[OrderRouter] 중복 생략 [" + managed_order.order_id + "] " + signal.ticker + " " +
                     std::to_string(signal.quantity) + "주 → " + managed_order.reject_reason);
            record(managed_order);
            return managed_order;
        }
    }

    // 1. OrderGate 검증
    std::string reject_reason;
    OrderAck    acknowledgement;
    bool        freed = false;

    if (sell_no_quantity)
    {
        // 왜 0인지 남기고, 이 프로세스가 아는 예약매도(이번 세션 이력·이전 세션 부속 파일)를 취소해
        //  수량을 풀어 본다. 거부만 하고 끝내면 예약매도가 브로커에 남은 채 청산이 하루 종일 막힌다
        //  (09-11 014530: 09:17 익절 지정가 118주가 취소 한도거부로 잔존, 이후 재기동 8회 내내 거부).
        //  풀리면 그 자리에서 재발주한 접수로 이어간다. [why D-055]
        const auto sellable_view = gate_.sellable_view(signal.account_id, signal.ticker);
        LOG_WARN(std::format("[OrderRouter] 매도가능 {}/{}주 {} — 원장 보유 {}주, 잔고 주문가능 {}주, 이 세션 미체결 매도 {}주 → 예약매도 취소 시도",
                             allowed, signal.quantity, signal.ticker, sellable_view.held, sellable_view.possible_quantity_cap, sellable_view.pending));
        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal);

        if (reconcile_acknowledgement.ok())
        {
            acknowledgement = std::move(reconcile_acknowledgement);
            freed = true;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty && allowed > 0)
        {
            // 취소할 예약매도를 못 찾았지만 일부는 나갈 수 있다 — 잘라서라도 낸다(예전 클램프 경로).
            LOG_INFO(std::format("[OrderRouter] 한도 클램프 {} {}주 → {}주 (예약매도 취소 불발)", signal.ticker, signal.quantity, allowed));
            signal.quantity = allowed;
            managed_order.signal    = signal;
        }
        else if (reconcile_acknowledgement.error_code == kis_error::kNoSellableQty)
        {
            reject_reason = "매도가능수량 0 (미체결 매도·미결제분) — 취소할 예약매도 없음, 발주 생략";
        }
        else
        {
            // 예약매도는 취소됐는데 재매도가 거부(유량한도·전송 실패)된 것 — 수량은 풀렸으니 재시도가 낸다.
            //  09-14 15:00 096770: 취소 뒤 재매도가 EGW00201 에 걸렸는데 "취소할 예약매도 없음"으로 남아 원인을 잘못 짚었다.
            reject_reason = "예약매도 취소 뒤 재매도 실패 [" + reconcile_acknowledgement.error_code + "] — 재시도 대상";
        }
    }

    if (!reject_reason.empty() || (!freed && !gate_.check(signal, reject_reason)))
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = reject_reason;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 거부 [" + managed_order.order_id + "] " +
                 signal.ticker + " → " + reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        record(managed_order);
        return managed_order;
    }

    // 2. KIS 주문 전송 (submit_order_ack로 ODNO + KRX 조직번호 캡처 — 정정/취소 준비)
    //    접수 왕복지연(RTT)을 재서 접수 로그에 남긴다 → log_report.py가 중앙값(p50)·상위 1%(p99) 집계.
    //    RTT 안에는 초당 한도 버킷 대기(rate_limit_acquire)가 섞여 있어 그 몫을 따로 적는다 — 09-14~18 RTT p50 2초가
    //    망 지연인지 버킷 줄서기인지 이 숫자 없이는 못 가른다. 전송 스레드 분리(T-13-2)는 이 값을 보고 정한다. [why D-071]
    managed_order.status = OrderStatus::SUBMITTED;
    const auto send_thread = std::chrono::steady_clock::now();
    const std::uint64_t bucket_wait_before_ns = kis_.rate_limit_wait_ns_this_thread();
    std::chrono::milliseconds::rep rtt_ms = 0; // count()의 타입 그대로 — MSVC는 long long이라 long이면 잘린다(C4244)
    std::chrono::milliseconds::rep bucket_wait_ms = 0;

    try
    {
        if (!freed) // 예약매도 취소 뒤 재발주가 이미 접수됐으면 그 결과를 쓴다
        {
            ++kis_calls_;
            acknowledgement = kis_.submit_order_acknowledgement(signal);
        }

        rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - send_thread)
                     .count();
        bucket_wait_ms = static_cast<std::chrono::milliseconds::rep>(
            (kis_.rate_limit_wait_ns_this_thread() - bucket_wait_before_ns) / 1000000ULL);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 예외: ") + exception.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 예외 [" + managed_order.order_id + "] " + signal.ticker + " — " + exception.what());
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
        record(managed_order);
        return managed_order;
    }

    managed_order.updated_at = std::chrono::system_clock::now();

    // 청산차단 자가정리 — SELL이 "주문가능분 없음"(40240000)으로 막히면, 그 종목의
    //  미체결 예약매도(이전 세션/수동 예약이 보유수량을 묶은 것)를 조회·취소하고 시장가로 1회
    //  재시도한다. 성공하면 아래 접수 블록이 그대로 처리(kis_order_no/ack가 재시도 결과로 갱신됨).
    if (!acknowledgement.ok() && signal.side == OrderSide::SELL && acknowledgement.error_code == kis_error::kNoSellableQty)
    {
        const auto sellable_view = gate_.sellable_view(signal.account_id, signal.ticker);

        if (sellable_view.held <= 0)
        {
            // 게이트는 원장이 모르는 종목을 자르지 않고 KIS에 넘긴다 — 그 거부가 여기로 온다.
            LOG_WARN("[OrderRouter] 보유수량 0(원장 기준) " + signal.ticker + " — 매도 불가, KIS도 주문가능분 없음으로 거부");
        }

        OrderAck reconcile_acknowledgement = reconcile_blocked_sell(signal);

        if (reconcile_acknowledgement.ok())
        {
            acknowledgement = std::move(reconcile_acknowledgement);
        }
    }

    if (!acknowledgement.kis_order_no.empty())
    {
        managed_order.status      = OrderStatus::ACCEPTED;
        managed_order.kis_order_no = std::move(acknowledgement.kis_order_no);
        managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no); // 전문 문자열이 정수가 되는 자리
        managed_order.krx_forwarding_org_no    = std::move(acknowledgement.krx_forwarding_org_no); // 정정/취소 시 원주문 조직번호로 재입력
        ++accepted_count_;
        // KIS 접수 시점에 포지션 선점 (보수적 추적 — 실제 체결 확인 전까지 재주문 차단)
        //  선점가는 지정가=price, 시장가(0)=ref_price로 근사 stamp → §3d 총노출이 시장가 선점을
        //  과소평가하지 않게(check()의 eval_px와 대칭, 보수측).
        gate_.on_accept(signal.account_id, signal.ticker, signal.side, signal.quantity,
                        signal.price > 0.0 ? signal.price : signal.reference_price);

        LOG_INFO(std::format("[OrderRouter] 접수 [{}] ODNO={} {} {} {}주 RTT={}ms 버킷대기={}ms", managed_order.order_id, managed_order.kis_order_no,
                             signal.ticker, signal.side == OrderSide::BUY ? "BUY" : "SELL", signal.quantity, rtt_ms, bucket_wait_ms));
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, true);
        }
#endif
    }
    else
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS API 거부 (빈 ODNO)" + kis_error_suffix(acknowledgement);
        ++rejected_count_;
        // 거부도 같은 왕복을 치르므로 함께 남긴다(09-14 KIS 호출 797건 중 거부 279건).
        LOG_ERROR(std::format("[OrderRouter] KIS 거부 [{}] {}{} RTT={}ms 버킷대기={}ms", managed_order.order_id, signal.ticker,
                              managed_order.reject_reason, rtt_ms, bucket_wait_ms));
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(signal, false);
        }
#endif
    }

    record(managed_order);
    return managed_order;
}

// ─── 청산차단 자가정리 — 예약매도 취소 후 시장가 재매도 (장중) ─────────────
//  전제: SELL이 40240000(주문가능분 없음)으로 막힌 직후 호출. 그 종목의 미체결 예약매도가
//  보유수량을 묶어 ord_psbl_qty=0이 된 상황을 KIS 미체결 조회로 규명하고, 예약을 취소해
//  수량을 풀어준 뒤 시장가 매도를 1회 재시도한다. 취소한 예약이 이번 세션 주문(history_에
//  ODNO가 있음)이면 CANCELLED로 닫고 게이트 선점(reserved_)을 풀어 원장 행을 남긴다 — 그러지
//  않으면 선점이 스윕 때까지 남아 한도 계산을 조인다(C-2). 이전 세션·수동 예약은 history_에
//  없으므로 gate_를 건드리지 않는다(포지션 정합은 체결통보로).
OrderAck OrderRouter::reconcile_blocked_sell(const OrderSignal& signal)
{
    std::vector<OpenOrder> opens;

    // 모의투자는 정정취소가능조회(inquire-psbl-rvsecncl) TR을 미지원한다("없는 서비스 코드").
    //  대안으로 일별주문체결조회(VTTC8001R)도 붙여봤으나 기간을 어떻게 주든 output1이 0행이라
    //  미체결을 열거할 수 없었다(2026-09-07). 그래서 브로커 대신 라우터 이력을 정본으로 쓴다 —
    //  접수됐는데 아직 다 안 채워진 이 종목의 매도가 곧 수량을 묶고 있는 예약매도다.
    //  한계는 분명하다: 이번 세션이 낸 주문만 보인다. 이전 세션·수동 예약은 여전히 안 보이므로
    //  그때는 아래 "취소할 예약매도 없음"으로 떨어진다. 그래도 통째로 단락하는 것보다 낫다.
    if (kis_.is_paper())
    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        for (const auto& managed_order : history_)
        {
            if (managed_order.status != OrderStatus::ACCEPTED || managed_order.signal.side != OrderSide::SELL ||
                managed_order.signal.symbol_id != signal.symbol_id || managed_order.kis_order_number == 0)
            {
                continue;
            }

            const int outstanding = managed_order.signal.quantity - managed_order.confirmed_quantity;

            if (outstanding <= 0)
            {
                continue;
            }

            OpenOrder open_order;
            open_order.ticker    = managed_order.signal.ticker;
            open_order.kis_order_no      = managed_order.kis_order_no;
            open_order.krx_forwarding_org_no = managed_order.krx_forwarding_org_no;
            open_order.psbl_qty  = outstanding;
            open_order.ord_unpr  = managed_order.signal.price;
            open_order.side      = OrderSide::SELL;
            opens.push_back(std::move(open_order));
        }

        // 이전 세션이 남긴 미체결(부속 파일)도 후보다 — 기동 스윕이 아직 못 지웠거나 한도 거부로
        //  남긴 줄이 이 종목의 수량을 묶고 있을 수 있다. 취소되면 아래에서 줄을 지운다.
        std::lock_guard<std::mutex> ck(carry_mutex_);

        for (const auto& carry_row : carry_rows_)
        {
            if (carry_row[2] != signal.ticker || carry_row[3] != "SELL")
            {
                continue;
            }

            OpenOrder open_order;
            open_order.ticker    = carry_row[2];
            open_order.kis_order_no      = carry_row[0];
            open_order.krx_forwarding_org_no = carry_row[1];

            try { open_order.psbl_qty = std::stoi(carry_row[4]); } catch (...) { continue; }
            open_order.side      = OrderSide::SELL;
            opens.push_back(std::move(open_order));
        }
    }
    else
    {
        try
        {
            opens = kis_.get_open_orders();
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 미체결 조회 예외 — " + std::string(exception.what()));
            return OrderAck::fail(kis_error::kTransport);
        }
    }

    int cancelled = 0;

    for (const auto& open : opens)
    {
        if (open.ticker != signal.ticker || open.side != OrderSide::SELL)
        {
            continue; // 해당 종목의 예약'매도'만 대상
        }

        LOG_WARN(std::format("[OrderRouter] 청산차단 해소 {} 예약매도 {}주 ODNO={} @{} → 취소 시도", signal.ticker,
                             open.psbl_qty, open.kis_order_no, static_cast<int>(open.ord_unpr)));
        OrderAck cancel;

        try
        {
            ++kis_calls_;
            cancel = kis_.cancel_order(open.ticker, open.kis_order_no, open.krx_forwarding_org_no, open.psbl_qty, /*all_remaining=*/true);
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 예약취소 예외 " + signal.ticker + " — " + std::string(exception.what()));
            continue;
        }

        if (!cancel.ok())
        {
            continue;
        }

        ++cancelled;

        // 이번 세션 주문이면 이력·선점을 같이 정리한다. 잠금 순서 history_→gate는 cancel_route와 같다.
        //  closed는 락 안에서 뜬 사본 — 락 밖의 원장 기록에 쓰고, history_ 원소는 축출로 참조가 죽을 수 있다.
        ManagedOrder closed;
        bool         found   = false;
        int          release = 0;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            ManagedOrder* managed_order = find_by_order_number_locked(digits_to_number(open.kis_order_no));

            if (managed_order && managed_order->status == OrderStatus::ACCEPTED)
            {
                release = managed_order->signal.quantity - managed_order->confirmed_quantity;

                if (release < 0)
                {
                    release = 0;
                }

                managed_order->status        = OrderStatus::CANCELLED;
                managed_order->reject_reason = "청산차단 해소 취소";
                managed_order->updated_at    = std::chrono::system_clock::now();
                closed                       = *managed_order;
                found                        = true;
            }

            if (release > 0)
            {
                gate_.on_cancel(closed.signal.account_id, closed.signal.ticker, OrderSide::SELL, release);
            }
        }

        if (found)
        {
            // 선점(reserved_)만 풀면 잔고 시드값 sellable_(취소 전 스냅샷, 주문가능 0)이 그대로라 다음 매도도 0으로
            //  깎인다 — 09-14 15:00 096770 은 익절 취소 뒤 재매도가 유량한도에 막히자 재시도 3회가 전부 "매도가능 0".
            //  취소로 브로커에서 풀린 수량만큼 되돌린다(이전 세션 줄과 같은 처리).
            gate_.restore_sellable(closed.signal.account_id, closed.signal.ticker, release);
            write_trade_row("", closed, 0, 0.0);
        }
        else
        {
            // 이전 세션 줄 — 부속 파일에서 빼고, 취소로 풀린 수량을 게이트에 되돌린다(스윕과 같은 처리).
            bool carried = false;
            {
                std::lock_guard<std::mutex> ck(carry_mutex_);
                const auto before = carry_rows_.size();
                carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                                 [&open](const std::array<std::string, 5>& parts) { return parts[0] == open.kis_order_no; }),
                                  carry_rows_.end());
                carried = carry_rows_.size() != before;
            }

            if (carried)
            {
                rewrite_open_orders();
            }

            gate_.restore_sellable(signal.account_id, signal.ticker, open.psbl_qty);
        }
    }

    if (cancelled == 0)
    {
        LOG_WARN("[OrderRouter] 청산차단 미해소 " + signal.ticker +
                 " — 취소할 예약매도 없음/취소 실패 (수동 확인 필요)");
        return OrderAck::fail(kis_error::kNoSellableQty); // 원인은 그대로 — 호출부가 거부 사유로 남긴다
    }

    LOG_INFO(std::format("[OrderRouter] 예약매도 {}건 취소 완료 → {} 시장가 매도 재시도", cancelled, signal.ticker));

    try
    {
        ++kis_calls_;
        return kis_.submit_order_acknowledgement(signal);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR("[OrderRouter] 청산 재매도 예외 " + signal.ticker + " — " + std::string(exception.what()));
        return OrderAck::fail(kis_error::kTransport);
    }
}

// ─── 유령 선점 정리 ───────────────────────────────────────────────────────
//  게이트의 선점(reserved_)은 접수 때만 생기고 체결·취소 통보로만 풀린다. 통보를 한 번
//  놓치면 그 선점이 슬롯을 물고 남아, 실제 보유가 한도에 못 미치는데 신규 진입이 막힌다
//  (09-09: 보유 20인데 "25 >= 25" 거부). 라우터 이력에 살아있는 주문이 없으면 푼다.
int OrderRouter::sweep_stale_reservations()
{
    std::vector<bool> live(gate_.symbols().capacity(), false);
    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        // [inv] 이력이 비면 아무 것도 풀지 않는다. 선점은 접수 때만 생기고 접수는 이력에도
        //  남으므로 정상적으로는 둘이 같이 비어 있다. 이력만 비는 경우(재기동 직후, 상한 초과로
        //  잘려 나간 뒤)에 정본으로 믿으면 살아 있는 선점을 통째로 푼다.
        if (history_.empty())
        {
            return 0;
        }

        for (const auto& managed_order : history_)
        {
            if (managed_order.status == OrderStatus::ACCEPTED && managed_order.confirmed_quantity < managed_order.signal.quantity &&
                managed_order.signal.symbol_id < live.size())
            {
                live[managed_order.signal.symbol_id] = true;
            }
        }
    }

    const auto gone = gate_.prune_reservations(live);

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

// ─── 이력 저장 (max_history 초과 시 체결 완료/거부된 것만 삭제) ───────────
void OrderRouter::record(const ManagedOrder& managed_order)
{
    std::string open_orders;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        push_history_locked(managed_order);

        while (static_cast<int>(history_.size()) > config_.max_history)
        {
            // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
            if (history_.front().status == OrderStatus::ACCEPTED)
            {
                break;
            }

            pop_history_front_locked();
        }

        open_orders = snapshot_open_orders_locked();
        sequence         = ++open_orders_sequence_;
    }

    // 파일 I/O는 history_mutex_ 밖에서 — 디스크가 느린 순간 체결(on_fill)·발주(submit)가 같이 밀리지 않게(W-8).
    // 거래 원장 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → managed_order.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    write_trade_row("", managed_order, 0, 0.0);
    write_open_orders_file(open_orders, sequence);
    append_order_reason(managed_order);
}

// ─── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
//  형식: odno|ticker|side|수량|가격|기준가|전략|사유   (한 줄 한 주문, 헤더 없음)
//  접수된 신규·정정 주문만 남긴다. 취소는 체결되지 않으므로 대상이 아니다.
void OrderRouter::append_order_reason(const ManagedOrder& managed_order)
{
    if (managed_order.status != OrderStatus::ACCEPTED || managed_order.kis_order_no.empty() ||
        managed_order.signal.action == OrderAction::CANCEL || managed_order.signal.quantity <= 0)
    {
        return;
    }

    // 구분자와 줄바꿈은 공백으로 바꾼다 — 사유 문구에 무엇이 들어와도 한 줄을 유지한다.
    auto safe = [](std::string text)
    {
        for (char& character : text)
        {
            if (character == '|' || character == '\n' || character == '\r')
            {
                character = ' ';
            }
        }

        return text;
    };

    const std::string line = std::format("{}|{}|{}|{}|{}|{}|{}|{}\n", managed_order.kis_order_no, safe(managed_order.signal.ticker),
                                         managed_order.signal.side == OrderSide::BUY ? "BUY" : "SELL", managed_order.signal.quantity,
                                         static_cast<long long>(managed_order.signal.price),
                                         static_cast<long long>(managed_order.signal.reference_price), safe(managed_order.signal.strategy_id),
                                         safe(managed_order.signal.reason));

    std::lock_guard<std::mutex> lock(io_mutex_);
    const std::string date = today_ymd();

    if (date != order_reason_file_date_ || !order_reason_file_.is_open())
    {
        order_reason_file_.close();
        order_reason_file_.clear();
        order_reason_file_.open(Logger::instance().path_for("order_reasons_" + date + ".txt"), std::ios::app);
        order_reason_file_date_ = date;
    }

    if (order_reason_file_)
    {
        order_reason_file_ << line;
        order_reason_file_.flush();
    }
}

void OrderRouter::load_order_reasons_locked()
{
    if (order_reasons_loaded_)
    {
        return;
    }

    order_reasons_loaded_ = true;
    std::ifstream in(Logger::instance().path_for("order_reasons_" + today_ymd() + ".txt"));

    if (!in)
    {
        return;
    }

    std::string line;
    int count = 0;

    while (std::getline(in, line))
    {
        std::vector<std::string> fields;
        std::string token;
        std::istringstream stream(line);

        while (std::getline(stream, token, '|'))
        {
            fields.push_back(std::move(token));
        }

        if (fields.size() < 8 || fields[0].empty())
        {
            continue;
        }

        OrderReason order_reason;
        order_reason.ticker      = std::move(fields[1]);
        order_reason.side        = OrderSide::from_string(fields[2]);
        order_reason.strategy_id = std::move(fields[6]);
        order_reason.reason      = std::move(fields[7]);

        try
        {
            order_reason.quantity  = std::stoi(fields[3]);
            order_reason.price     = std::stod(fields[4]);
            order_reason.reference_price = std::stod(fields[5]);
        }
        catch (const std::exception&)
        {
            continue;   // 숫자가 깨진 줄은 버린다
        }

        if (order_reason.quantity <= 0)
        {
            continue;
        }

        const uint64_t order_number = digits_to_number(fields[0]);

        if (order_number == 0)
        {
            continue;
        }

        order_reasons_[order_number] = std::move(order_reason);   // 같은 ODNO가 여러 줄이면 마지막 것이 맞다
        ++count;
    }

    if (count > 0)
    {
        LOG_INFO("[OrderRouter] 주문 사유 기록 " + std::to_string(count) + "건 복원");
    }
}


// ─── 미체결 주문 부속 파일 ─────────────────────────────────────────────────
//  형식: odno|orgno|ticker|side|remaining  (한 줄 한 주문, 헤더 없음)
//  history_는 프로세스 메모리라 재기동으로 사라진다. 모의투자는 정정취소가능조회
//  TR을 지원하지 않아 브로커에도 물어볼 수 없다. 그래서 살아있는 주문을 파일에
//  남겨 두고 다음 기동이 그것을 취소한다. 매 상태변화마다 통째로 덮어쓴다
//  (동시에 살아있는 주문은 많아야 수십 건이라 비용이 무시할 만하다).
std::string OrderRouter::snapshot_open_orders_locked() const
{
    std::string buffer;

    for (const auto& history_entry : history_)
    {
        if (history_entry.status != OrderStatus::ACCEPTED)
        {
            continue;
        }

        const int remain = history_entry.signal.quantity - history_entry.confirmed_quantity;

        if (remain <= 0 || history_entry.kis_order_no.empty())
        {
            continue;
        }

        std::format_to(std::back_inserter(buffer), "{}|{}|{}|{}|{}\n", history_entry.kis_order_no, history_entry.krx_forwarding_org_no, history_entry.signal.ticker,
                       history_entry.signal.side == OrderSide::BUY ? "BUY" : "SELL", remain);
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

    write_open_orders_file(body, sequence);
}

void OrderRouter::write_open_orders_file(const std::string& body, uint64_t sequence)
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (sequence <= open_orders_written_sequence_)
    {
        return; // 더 새 스냅샷이 먼저 쓰였다 — 옛 것으로 덮으면 살아있는 주문이 사라진다
    }

    open_orders_written_sequence_ = sequence;

    fs::path path = Logger::instance().path_for("open_orders.txt");
    fs::path temporary  = Logger::instance().path_for("open_orders.tmp");

    // 임시파일에 쓰고 원자적으로 갈아끼운다 — 기동 중 크래시로 반쪽 파일을 읽지 않도록.
    {
        std::ofstream out(temporary, std::ios::trunc);

        if (!out)
        {
            return;
        }

        out << body;
    }

    fs::rename(temporary, path, error_code);

    if (error_code)
    {
        fs::remove(temporary, error_code);
    }
}

// ─── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ──────────────────────
OrderRouter::~OrderRouter()
{
    // jthread 소멸자가 같은 일을 하지만 그건 멤버 소멸 순서 안에서다 — 스레드가 쓰는 멤버가 먼저 죽지 않게 여기서 회수한다.
    stale_threshold_.request_stop();

    if (stale_threshold_.joinable())
    {
        stale_threshold_.join();
    }
}

void OrderRouter::cancel_stale_orders_async()
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    fs::path path = Logger::instance().path_for("open_orders.txt");

    if (!fs::exists(path, error_code))
    {
        return;
    }

    std::vector<std::array<std::string, 5>> rows;
    {
        std::ifstream in(path);
        std::string line;

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

            std::array<std::string, 5> fields;
            size_t position = 0, index = 0;
            bool ok = true;

            while (index < 5)
            {
                size_t bar = line.find('|', position);

                if (index < 4 && bar == std::string::npos) { ok = false; break; }
                fields[index++] = line.substr(position, index < 5 && bar != std::string::npos
                                                ? bar - position : std::string::npos);

                if (bar == std::string::npos)
                {
                    break;
                }

                position = bar + 1;
            }

            if (ok && index == 5 && !fields[0].empty())
            {
                rows.push_back(std::move(fields));
            }
        }
    }

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
            int quantity = 0;

            try { quantity = std::stoi(row[4]); } catch (...) {}

            if (quantity > 0)
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
        int cancelled = 0;

        for (const auto& row : rows)
        {
            if (stop_token.stop_requested())
            {
                LOG_WARN("[OrderRouter] 유령주문 취소 중단(종료 요청) — 남은 " +
                         std::to_string(rows.size() - static_cast<size_t>(cancelled)) + "건");
                return;
            }

            int quantity = 0;

            try { quantity = std::stoi(row[4]); } catch (...) { continue; }

            if (quantity <= 0)
            {
                continue;
            }

            OrderAck result;
            bool     rate_limited = false;

            // 한도 거부(EGW00201)는 "이미 종료"가 아니다. 같은 분기로 흘리면 유령 예약이 KIS에
            //  남은 채 전략이 같은 종목을 새로 깔아 체결 시 이중 포지션이 된다(09-11 09:17~09:18
            //  180640·005935·007660 6건). 한도는 1초 창이라 잠깐 쉬고 다시 보낸다.
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                try
                {
                    ++kis_calls_;
                    result = kis_.cancel_order(row[2], row[0], row[1], quantity, /*all_remaining=*/true);
                }
                catch (const std::exception& exception)
                {
                    LOG_WARN("[OrderRouter] 유령주문 취소 예외 " + row[2] + " ODNO=" + row[0] + " — " + exception.what());
                    break;
                }

                rate_limited = !result.ok() && result.error_code == kis_error::kRateLimit;

                if (!rate_limited || stop_token.stop_requested())
                {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            }

            if (rate_limited)
            {
                // 줄은 carry_rows_에 남긴다 — 다음 재기동이 다시 시도한다.
                LOG_WARN("[OrderRouter] 유령주문 취소 실패(한도 거부 반복) — KIS에 잔존 " + row[2] +
                         " ODNO=" + row[0] + " " + std::to_string(quantity) + "주 (부속 파일에 유지)");
                continue;
            }

            if (result.ok())
            {
                ++cancelled;
                LOG_INFO("[OrderRouter] 유령주문 취소 " + row[2] + " " + row[3] + " " +
                         std::to_string(quantity) + "주 ODNO=" + row[0]);

                // 취소로 브로커에서는 수량이 풀렸지만 게이트의 sellable_은 잔고 시드값
                //  (ord_psbl_qty, 취소 전 스냅샷) 그대로다. 되돌리지 않으면 미체결이 없는데도
                //  자기 청산이 막힌다 — 09-09 000215은 13:45 취소 뒤 16분간 "매도가능수량 0"으로
                //  교체 진입이 네 번 무산됐다. 매도 취소만 해당한다(매수는 현금을 풀 뿐이다).
                if (row[3] == "SELL")
                {
                    gate_.restore_sellable(std::string(), row[2], quantity);
                }
            }
            else
            {
                // 이미 체결·취소됐으면 KIS가 거부한다 — 정상이다.
                LOG_INFO("[OrderRouter] 유령주문 취소 불가(이미 종료 추정) " + row[2] + " ODNO=" + row[0]);
            }

            // 취소 접수든 이미 종료든 이 줄은 끝났다 — 부속 파일에서 뺀다.
            {
                std::lock_guard<std::mutex> lock(carry_mutex_);
                carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                                 [&row](const std::array<std::string, 5>& parts) { return parts[0] == row[0]; }),
                                  carry_rows_.end());
            }

            rewrite_open_orders();

            // 초당 거래건수 상한(EGW00201)에 걸리지 않게 간격을 둔다. 정지 요청이 오면 바로 깬다.
            sync::sleep_unless_stopped(stop_token, std::chrono::milliseconds(400));
        }

        LOG_INFO("[OrderRouter] 이전 세션 미체결 정리 완료: " + std::to_string(cancelled) + "건 취소 접수");
    });
}

// ─── 거래 원장 CSV 적재 ───────────────────────────────────────────────────
//  실행 로그(quant_trader.log)와 별개로 매수·매도·거부·체결·잔고 대조를 구조적으로 남긴다.
//  logs/trades_YYYYMMDD.csv 에 한 줄씩 append(날짜별 파일). record()·on_fill()·record_reconcile()
//  이 줄을 만들고 append_trade_line이 io_mtx_로 직렬화해 쓴다(history_mutex_ 밖 — 동시쓰기 없음).
//  원장 쓰기 실패는 매매를 막지 않는다(best-effort — 조용히 반환).
//  열 정본은 kTradeHeader 하나다. 열을 더할 때는 끝에 붙인다 — 스키마 승격이 옛 파일 행 끝에 빈 칸을
//  덧붙이는 방식이라 중간 삽입은 기존 행의 값을 엉뚱한 열로 밀어낸다. Python 판독기는 열 이름으로 읽는다.
static const std::string kTradeHeader =
    "ts_kst,event,order_id,odno,strategy,ticker,side,type,"
    "order_qty,order_price,fill_qty,fill_price,status,reason,entry_reason,realized_pnl,seq,"
    "strategy_realized_pnl";

// CSV 깨짐 방지: 콤마/개행 공백 치환
static std::string csv_safe(std::string text)
{
    for (char& character : text)
    {
        if (character == ',' || character == '\n' || character == '\r')
        {
            character = ' ';
        }
    }

    return text;
}

void OrderRouter::trade_row_timestamp(std::string& date, std::string& stamp)
{
    const std::time_t now_time = std::time(nullptr);
    date  = kst::date_yyyymmdd(now_time);
    stamp = kst::datetime(now_time);
}

void OrderRouter::open_trade_file_locked(const std::string& date)
{
    namespace fs = std::filesystem;
    std::error_code error_code;
    // 실행 위치(cwd)와 무관하게 로그 폴더(main에서 고정)에 매매원장 append.
    fs::path path = Logger::instance().path_for(std::string("trades_") + date + ".csv");

    const bool need_header = !fs::exists(path, error_code);

    // 스키마 승격 — 같은 날 파일이 옛 헤더(열이 적음)면 새 열을 붙여 한 번 재작성한다.
    //  한 파일에 15열 헤더와 16열 데이터가 섞이면 판독기가 값을 어긋난 키로 읽는다.
    //  날짜가 바뀌어 파일을 새로 여는 순간에만 확인한다 — 이미 이번 세션에서 연 파일의
    //  헤더는 우리 자신이 썼으므로 매 줄마다 다시 볼 필요가 없다. [why D-094]
    if (!need_header)
    {
        std::ifstream in(path);
        std::string first;

        if (in && std::getline(in, first))
        {
            if (!first.empty() && first.back() == '\r')
            {
                first.pop_back();
            }

            if (first != kTradeHeader)
            {
                long add = static_cast<long>(std::count(kTradeHeader.begin(), kTradeHeader.end(), ',')) -
                           static_cast<long>(std::count(first.begin(), first.end(), ','));

                if (add < 0)
                {
                    add = 0;
                }

                std::vector<std::string> rows;

                for (std::string line; std::getline(in, line); )
                {
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    if (!line.empty())
                    {
                        line.append(static_cast<size_t>(add), ',');
                        rows.push_back(std::move(line));
                    }
                }

                in.close();
                std::ofstream out(path, std::ios::trunc);

                if (out)
                {
                    out << kTradeHeader << '\n';

                    for (const auto& row : rows)
                    {
                        out << row << '\n';
                    }
                }
            }
        }
    }

    trade_file_.close();
    trade_file_.clear();
    trade_file_.open(path, std::ios::app);

    if (trade_file_.is_open() && need_header)
    {
        trade_file_ << kTradeHeader << '\n';
        trade_file_.flush();
    }

    trade_file_date_ = date;
}

void OrderRouter::append_trade_line(const std::string& line)
{
    std::lock_guard<std::mutex> io_lk(io_mutex_);

    std::string dbuf, time_buffer;
    trade_row_timestamp(dbuf, time_buffer);

    if (dbuf != trade_file_date_ || !trade_file_.is_open())
    {
        open_trade_file_locked(dbuf);
    }

    if (!trade_file_.is_open())
    {
        return; // best-effort
    }

    trade_file_ << time_buffer << ',' << line << '\n';
    trade_file_.flush();
}

void OrderRouter::write_trade_row(const std::string& event, const ManagedOrder& managed_order,
                                  int fill_quantity, double fill_price, double realized_pnl,
                                  double strategy_realized_pnl)
{
    const OrderSignal& signal = managed_order.signal;

    auto side_string = [](OrderSide order_side) {
        return order_side == OrderSide::BUY ? "BUY" : (order_side == OrderSide::SELL ? "SELL" : "NONE");
    };
    auto type_string = [](OrderType order_type) { return order_type == OrderType::LIMIT ? "LIMIT" : "MARKET"; };
    auto status_string = [](OrderStatus status) -> const char* {
        switch (status)
        {
        case OrderStatus::PENDING:   return "PENDING";
        case OrderStatus::SUBMITTED: return "SUBMITTED";
        case OrderStatus::ACCEPTED:  return "ACCEPTED";
        case OrderStatus::REJECTED:  return "REJECTED";
        case OrderStatus::FILLED:    return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        default:                     return "?";
        }
    };

    // event 빈 문자열이면 상태 문자열을 사용
    const std::string_view event_text = event.empty() ? std::string_view(status_string(managed_order.status)) : std::string_view(event);
    // reason = 거부/봉쇄 사유(OrderGate·KIS), entry_reason = 진입 판단 근거(전략, G4) — 분리 컬럼.
    std::string reason       = csv_safe(managed_order.reject_reason);
    std::string entry_reason = csv_safe(signal.reason);

    std::string field = std::format("{},{},{},{},{},{},{},{},{:.2f},{},{:.2f},{},{},{},", event_text, managed_order.order_id,
                                managed_order.kis_order_no, signal.strategy_id, signal.ticker, side_string(signal.side), type_string(signal.type),
                                signal.quantity, signal.price, fill_quantity, fill_price, status_string(managed_order.status), reason,
                                entry_reason);

    // 실현손익은 매도 체결에서만 의미가 있다. 매수·접수·거부 행은 빈 칸으로 둬서
    //  0원 실현으로 오독되지 않게 한다.
    if (event == "FILL" && signal.side == OrderSide::SELL)
    {
        std::format_to(std::back_inserter(field), "{:.2f}", realized_pnl);
    }

    // sequence는 전략 스레드가 stamp한 신호 순번(C-2). 미부여(0)는 빈 칸 — 재기동 전 주문의 체결 등.
    field += ',';

    if (signal.sequence != 0)
    {
        field += std::to_string(signal.sequence);
    }

    // strategy_realized_pnl은 realized_pnl과 같은 조건(SELL 체결)에서만 채운다 — 열 끝 추가분(D-089).
    field += ',';

    if (event == "FILL" && signal.side == OrderSide::SELL)
    {
        std::format_to(std::back_inserter(field), "{:.2f}", strategy_realized_pnl);
    }

    append_trade_line(field);
}

// ─── 잔고 대조 기록 (C-2) ─────────────────────────────────────────────────
//  live_orders는 history_에서 센다(history_mutex_). 파일 쓰기는 락 밖.
void OrderRouter::record_reconcile(const ReconcileNote& reconcile_note)
{
    int live_orders = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        const symbol::SymbolId reconcile_symbol = gate_.symbol_id_of(reconcile_note.ticker); // 대조 메모는 문자열 — 한 번만 바꾼다

        for (const auto& managed_order : history_)
        {
            if (managed_order.status == OrderStatus::ACCEPTED && managed_order.signal.symbol_id == reconcile_symbol &&
                managed_order.signal.quantity - managed_order.confirmed_quantity > 0)
            {
                ++live_orders;
            }
        }
    }

    std::string reason = std::format("live_orders={} diff_qty={}", live_orders, reconcile_note.broker_quantity - reconcile_note.ledger_quantity);

    if (!reconcile_note.note.empty())
    {
        reason += ' ';
        reason += csv_safe(reconcile_note.note);
    }

    if (reconcile_note.action != "KEEP")
    {
        LOG_WARN(std::format("[OrderRouter] 잔고 대조 {} 원장 {}주@{} vs 브로커 {}주@{} → {} ({})", reconcile_note.ticker,
                             reconcile_note.ledger_quantity, static_cast<long long>(reconcile_note.ledger_average), reconcile_note.broker_quantity,
                             static_cast<long long>(reconcile_note.broker_average), reconcile_note.action, reason));
    }

    // 빈 칸: order_id·kis_order_no·strategy, side·type, 끝의 entry_reason·realized_pnl·sequence.
    append_trade_line(std::format("RECONCILE,,,,{},NONE,,{},{:.2f},{},{:.2f},{},{},,,", reconcile_note.ticker, reconcile_note.ledger_quantity,
                                  reconcile_note.ledger_average, reconcile_note.broker_quantity, reconcile_note.broker_average, csv_safe(reconcile_note.action), reason));
}

// ─── 주문 번호로 살아있는 주문 조회 (호출자가 history_mutex_ 보유) ────────────
//  live = ACCEPTED 이면서 미체결 잔량이 남은 주문(부분체결도 status는 ACCEPTED 유지).
//  FILLED/CANCELLED/REJECTED는 취소·정정 대상 아님.
ManagedOrder* OrderRouter::find_live_by_client_number(uint64_t client_order_number)
{
    ManagedOrder* managed_order = find_by_client_number_locked(client_order_number);

    if (managed_order && managed_order->status == OrderStatus::ACCEPTED &&
        managed_order->confirmed_quantity < managed_order->signal.quantity)
    {
        return managed_order;
    }

    return nullptr;
}

// ─── 이력 색인 (호출자가 history_mutex_ 보유) ─────────────────────────────────
void OrderRouter::push_history_locked(ManagedOrder managed_order)
{
    const uint64_t history_sequence = history_base_ + history_.size();

    if (managed_order.kis_order_number != 0)
    {
        slot_by_order_number_[managed_order.kis_order_number] = history_sequence;
    }

    if (managed_order.signal.client_order_number != 0)
    {
        slot_by_client_number_[managed_order.signal.client_order_number] = history_sequence;
    }

    history_.push_back(std::move(managed_order));
}

void OrderRouter::pop_history_front_locked()
{
    const ManagedOrder& front = history_.front();
    // 같은 키를 더 새 항목이 차지했으면 그 색인은 남긴다.
    const auto erase_if_mine = [this](std::unordered_map<uint64_t, uint64_t>& index, uint64_t key)
    {
        const auto iterator = index.find(key);

        if (iterator != index.end() && iterator->second == history_base_)
        {
            index.erase(iterator);
        }
    };

    if (front.kis_order_number != 0)
    {
        erase_if_mine(slot_by_order_number_, front.kis_order_number);
    }

    if (front.signal.client_order_number != 0)
    {
        erase_if_mine(slot_by_client_number_, front.signal.client_order_number);
    }

    history_.pop_front();
    ++history_base_;
}

ManagedOrder* OrderRouter::history_at_locked(uint64_t history_sequence)
{
    if (history_sequence < history_base_ || history_sequence - history_base_ >= history_.size())
    {
        return nullptr;
    }

    return &history_[history_sequence - history_base_];
}

ManagedOrder* OrderRouter::find_by_order_number_locked(uint64_t kis_order_number)
{
    if (kis_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_order_number_.find(kis_order_number);
    return iterator == slot_by_order_number_.end() ? nullptr : history_at_locked(iterator->second);
}

ManagedOrder* OrderRouter::find_by_client_number_locked(uint64_t client_order_number)
{
    if (client_order_number == 0)
    {
        return nullptr;
    }

    const auto iterator = slot_by_client_number_.find(client_order_number);
    return iterator == slot_by_client_number_.end() ? nullptr : history_at_locked(iterator->second);
}

symbol::SymbolId OrderRouter::symbol_of(const OrderSignal& signal)
{
    return signal.symbol_id != symbol::kNone ? signal.symbol_id : gate_.intern_symbol(signal.ticker);
}

// ─── 취소 라우팅 (action=CANCEL) ──────────────────────────────────────────
//  1) orig_client_oid로 live 주문 조회 → 원 ODNO/조직번호/미체결 잔량 스냅샷
//  2) lock 밖에서 KIS 취소 호출(네트워크)
//  3) 성공 시에만 lock 재획득 → 미체결 잔량을 '그 시점 confirmed_quantity로 재계산'해 reserved 해제
//     (2)와 (3) 사이 WS 스레드의 on_fill이 confirmed_quantity를 올릴 수 있으므로 재계산이 이중해제를 막는다.
ManagedOrder OrderRouter::cancel_route(const OrderSignal& signal)
{
    auto now = std::chrono::system_clock::now();
    ManagedOrder managed_order;
    managed_order.order_id     = next_id();
    managed_order.signal       = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;
    ++total_count_;

    // 1) 원주문 스냅샷 (record()는 hist_mtx_를 재획득하므로 lock 스코프 밖에서만 호출)
    std::string ticker, kis_order_no, krx_forwarding_org_no, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    // 취소가 빗나갔을 때 "원주문이 이미 체결됐을 수 있나"를 같은 락 안에서 답해 둔다.
    //  find_live_by_oid는 살아있는 주문만 보므로 !found는 세 경우를 뭉뚱그린다 —
    //  체결됨 / 이미 취소됨 / 애초에 접수된 적 없음(REJECTED·이력 없음).
    //  중복 매수 위험은 첫째에만 있다. [why D-033]
    bool original_may_have_filled = false;
    const char* gone_why = "이력 없음(재기동·이력초과)";
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);

        if (!original)
        {
            if (const ManagedOrder* history_entry = find_by_client_number_locked(signal.original_client_order_number))
            {
                if (history_entry->status == OrderStatus::FILLED ||
                    (history_entry->status == OrderStatus::ACCEPTED && history_entry->confirmed_quantity > 0))
                {
                    original_may_have_filled = true;
                    gone_why                 = "이미 체결";
                }
                else if (history_entry->status == OrderStatus::CANCELLED)
                {
                    gone_why = "이미 취소";
                }
                else if (history_entry->status == OrderStatus::REJECTED)
                {
                    gone_why = "접수된 적 없음(거부)";
                }
            }
        }

        if (original)
        {
            found        = true;
            ticker       = original->signal.ticker;
            kis_order_no = original->kis_order_no;
            krx_forwarding_org_no    = original->krx_forwarding_org_no;
            account      = original->signal.account_id;
            side         = original->signal.side;
            outstanding  = original->signal.quantity - original->confirmed_quantity;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        // 취소할 것이 없는 것은 거부가 아니라 끝난 상태다 — 전략은 취소 결과를 안 보고 계획을
        //  다시 짜므로, 거부된 분할 단계·이미 취소된 분할 단계를 다시 취소하는 요청이 재구성마다 온다
        //  (09-10~11 이틀 323건, 그중 체결 흔적은 21건). REJECTED로 세면 거부 통계와 경고가
        //  실제 문제(게이트·KIS 거부)를 덮는다. CANCELLED로 닫고, 체결 가능성이 있는 경우만
        //  경고와 매수 보류를 남긴다. [why D-035]
        managed_order.status        = OrderStatus::CANCELLED;
        managed_order.reject_reason = std::string("취소 대상 없음 (") + gone_why + ") oid=" + signal.original_client_order_id;

        // 체결 흔적이 있을 때만 매수를 잠근다. 접수된 적 없는 order_id(전략이 거부된 주문을
        //  live로 들고 있는 경우)에도 잠그면 매 재구성 주기마다 취소 빗나감 → 매수 거부 →
        //  거부된 oid가 다시 live로 → 다음 주기에 또 취소 빗나감으로 되돌아, 창이 계속
        //  갱신되며 그 종목 매수가 영구히 막힌다. 2026-09-10 006910이 이 모양으로
        //  보유 0인 채 57분간 한 주도 못 샀다(대체 주문 보류 176건). [why D-033]
        //  이미 무장돼 있으면 시각을 갱신하지 않는다 — 창은 연장되지 않는다.
        if (original_may_have_filled)
        {
            const symbol::SymbolId      symbol = symbol_of(signal);
            std::lock_guard<std::mutex> lock(history_mutex_);

            if (symbol >= cancel_miss_.size())
            {
                cancel_miss_.resize(std::max(gate_.symbols().capacity(), static_cast<size_t>(symbol) + 1));
            }

            if (cancel_miss_[symbol] == std::chrono::steady_clock::time_point{})
            {
                cancel_miss_[symbol] = std::chrono::steady_clock::now();
            }
        }

        if (original_may_have_filled)
        {
            LOG_WARN("[OrderRouter] 취소 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason +
                     " — 체결 가능성 있어 신규매수 보류");
        }
        else
        {
            LOG_INFO("[OrderRouter] 취소 불요 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        }

        record(managed_order);
        return managed_order;
    }

    // 2) KIS 취소 (lock 밖)
    OrderAck cancel;

    try
    {
        ++kis_calls_;
        cancel = kis_.cancel_order(ticker, kis_order_no, krx_forwarding_org_no, outstanding, /*all_remaining=*/true);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 취소 예외: ") + exception.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] 취소 예외 [" + managed_order.order_id + "] " + ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!cancel.ok())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(cancel);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 취소 거부 [" + managed_order.order_id + "] " + ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 3) 성공 — reserved 해제(잔량 재계산) + 원주문 CANCELLED 표기 + 인덱스 정리
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);
        int release = 0;

        if (original)
        {
            release = original->signal.quantity - original->confirmed_quantity; // 취소 성공 시점 실제 미체결

            if (release < 0)
            {
                release = 0;
            }

            original->status     = OrderStatus::CANCELLED;
            original->updated_at = std::chrono::system_clock::now();
        }

        // gate 뮤텍스는 hist_mtx_와 독립. 잠금 순서 history_→positions_는 on_fill과 동일(데드락 없음).
        if (release > 0)
        {
            gate_.on_cancel(account, ticker, side, release);
        }
    }

    managed_order.status       = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    managed_order.kis_order_no = std::move(cancel.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + managed_order.order_id + "] " + ticker +
             " 원oid=" + signal.original_client_order_id + " 취소ODNO=" + managed_order.kis_order_no);
    record(managed_order);
    return managed_order;
}

// ─── 정정 라우팅 (action=REPLACE) ─────────────────────────────────────────
//  KIS 정정 1콜 = cancel-replace. 성공 시 새 ODNO 발급.
//  reserved 조정: 원 미체결 잔량 해제 후 new_quantity 재선점(같은 side). 원주문은 CANCELLED,
//  정정 결과를 새 ManagedOrder(ACCEPTED)로 추적(새 ODNO/새 client_order_id).
//  ⚠ 첫 컷 한계: 부분체결 상태 정정은 수량 정합이 복잡 → MM은 REPLACE 미사용(CANCEL+NEW 사용).
//     본 경로는 미체결 전량 대상 정정만 안전. 부분체결분 정정은 Phase 2에서 정밀화.
ManagedOrder OrderRouter::replace_route(const OrderSignal& signal)
{
    auto now = std::chrono::system_clock::now();
    ManagedOrder managed_order;
    managed_order.order_id     = next_id();
    managed_order.signal       = signal;
    managed_order.submitted_at = now;
    managed_order.updated_at   = now;
    managed_order.status       = OrderStatus::PENDING;
    ++total_count_;

    // 원주문 스냅샷 — 락 밖에서 KIS를 부르는 동안 history_ 원소가 축출될 수 있어 값으로 뜬다.
    std::string ticker, kis_order_no, krx_forwarding_org_no, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);

        if (original)
        {
            found        = true;
            ticker       = original->signal.ticker;
            kis_order_no = original->kis_order_no;
            krx_forwarding_org_no    = original->krx_forwarding_org_no;
            account      = original->signal.account_id;
            side         = original->signal.side;
            outstanding  = original->signal.quantity - original->confirmed_quantity;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "정정 대상 없음 oid=" + signal.original_client_order_id;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 정정 무시 [" + managed_order.order_id + "] " + managed_order.reject_reason);
        record(managed_order);
        return managed_order;
    }

    int new_quantity = (signal.quantity > 0) ? signal.quantity : outstanding;

    OrderAck revise_acknowledgement;

    try
    {
        ++kis_calls_;
        revise_acknowledgement = kis_.revise_order(ticker, kis_order_no, krx_forwarding_org_no, new_quantity, signal.price);
    }
    catch (const std::exception& exception)
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = std::string("KIS 정정 예외: ") + exception.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] 정정 예외 [" + managed_order.order_id + "] " + ticker + " — " + exception.what());
        record(managed_order);
        return managed_order;
    }

    if (!revise_acknowledgement.ok())
    {
        managed_order.status        = OrderStatus::REJECTED;
        managed_order.reject_reason = "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_error_suffix(revise_acknowledgement);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 정정 거부 [" + managed_order.order_id + "] " + ticker +
                 " 원oid=" + signal.original_client_order_id);
        record(managed_order);
        return managed_order;
    }

    // 성공 — 원 미체결 잔량 해제 후 new_quantity 재선점, 원주문 CANCELLED, 정정본 ACCEPTED 추적
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        ManagedOrder* original = find_live_by_client_number(signal.original_client_order_number);
        int release = outstanding;

        if (original)
        {
            release = original->signal.quantity - original->confirmed_quantity;

            if (release < 0)
            {
                release = 0;
            }

            original->status     = OrderStatus::CANCELLED;
            original->updated_at = std::chrono::system_clock::now();
        }

        if (release > 0)
        {
            gate_.on_cancel(account, ticker, side, release);
        }

        // 정정본 재선점 — 새 side는 원주문과 동일 (선점가는 지정가=price, 시장가=reference_price 근사)
        gate_.on_accept(account, ticker, side, new_quantity, signal.price > 0.0 ? signal.price : signal.reference_price);
    }

    managed_order.status       = OrderStatus::ACCEPTED;
    managed_order.kis_order_no = std::move(revise_acknowledgement.kis_order_no);
    managed_order.kis_order_number = digits_to_number(managed_order.kis_order_no);
    managed_order.krx_forwarding_org_no    = std::move(krx_forwarding_org_no); // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    managed_order.signal.side  = side;      // NONE 방지: 원주문 side 승계
    managed_order.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + managed_order.order_id + "] " + ticker +
             " 원oid=" + signal.original_client_order_id + " 새ODNO=" + managed_order.kis_order_no +
             std::format(" qty={} @{}", new_quantity, static_cast<int>(signal.price)));
    record(managed_order);
    return managed_order;
}

// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
void OrderRouter::on_fill(const FillNotification& fill_notification)
{
    // unique_lock: 원장 갱신까지만 잡고, 파일 쓰기·publish 전에 푼다(W-8).
    std::unique_lock<std::mutex> lock(history_mutex_);
    // 중복 제거 — KIS 체결통보는 at-least-once(재전송/WS 재구독 시 중복 가능).
    // H0STCNI0 전문에 체결고유번호가 없어 kis_order_no+체결시각+수량+단가를 조합 키로 사용.
    // ODNO는 영업일 단위 재사용되고 fill_time은 HHMMSS(날짜 없음)라, 거래일(수신일)을
    // prefix로 붙여, 서로 다른 날의 동일키 충돌로 실체결을 오인해 drop하는 일을 막는다 (V-4).
    const uint64_t order_number = digits_to_number(fill_notification.kis_order_no); // 전문 문자열이 정수가 되는 자리
    const FillKey  fill_key{trade_date_number(std::chrono::system_clock::to_time_t(fill_notification.timestamp)), order_number,
                           static_cast<uint32_t>(digits_to_number(fill_notification.fill_time)), fill_notification.filled_quantity,
                           static_cast<int64_t>(fill_notification.filled_price * 100)};
    // 이 키는 유일하지 않다. 같은 초에 같은 수량·단가로 나뉘어 체결되면 서로 다른 실체결이
    //  같은 키를 갖는다. 2026-09-07 ODNO 0000014893(047050 BUY 91주)이 8건으로 분할체결되며
    //  6/53/3/2/6/15/4/2주가 같은 초에 들어왔고, 마지막 2주가 앞선 2주와 같은 키라는 이유로
    //  버려졌다(원장·포지션 2주 누락). 그래서 키를 집합 원소가 아니라 발생 횟수로 세고,
    //  n번째 발생을 각각 별개 체결로 처리한다.
    //  과체결 방어는 중복 키가 아니라 아래 "주문 잔량 상한"이 담당한다 — 통보가 재전송돼도
    //  누적 체결은 주문수량을 넘을 수 없다.
    const int seen = ++seen_fills_[fill_key];

    if (seen > 1)
    {
        LOG_INFO(std::format("[OrderRouter] 동일키 분할체결 {}회차 ODNO={} time={} {}주", seen, fill_notification.kis_order_no, fill_notification.fill_time,
                             fill_notification.filled_quantity));
    }

    // 재기동 복원 — 이전 세션이 낸 주문이면 접수 때 남긴 사유 기록에서 되살린다.
    //  history_는 메모리라 재기동으로 비지만 기록 파일에는 ODNO·종목·수량·전략·사유가
    //  그대로 있다. 되살려 history_에 넣으면 아래 매칭 루프가 잔량 클램프까지 평소대로
    //  처리하므로, 전략 귀속을 잃는 미매핑 경로로 빠지지 않는다.
    if (order_number != 0)
    {
        load_order_reasons_locked();   // 첫 체결통보 때 1회만 파일을 읽는다
        const bool known  = find_by_order_number_locked(order_number) != nullptr;
        auto       jitter = known ? order_reasons_.end() : order_reasons_.find(order_number);

        if (jitter != order_reasons_.end())
        {
            ManagedOrder record;
            record.order_id           = next_id();
            record.kis_order_no       = fill_notification.kis_order_no;
            record.kis_order_number   = order_number;
            record.status             = OrderStatus::ACCEPTED;
            record.confirmed_quantity      = 0;
            record.signal.ticker      = std::move(jitter->second.ticker); // 사유 기록은 아래에서 지우므로 옮겨 온다
            record.signal.symbol_id   = gate_.intern_symbol(record.signal.ticker); // 파일의 문자열 — 복원 때 한 번
            record.signal.side        = jitter->second.side;
            record.signal.type        = OrderType::LIMIT;
            record.signal.quantity    = jitter->second.quantity;
            record.signal.price       = jitter->second.price;
            record.signal.reference_price   = jitter->second.reference_price;
            record.signal.strategy_id = std::move(jitter->second.strategy_id);
            record.signal.strategy_index = gate_.strategy_index_of(record.signal.strategy_id); // 서브원장 귀속은 번호로
            record.signal.reason      = std::move(jitter->second.reason);
            record.submitted_at       = fill_notification.timestamp;
            record.updated_at         = fill_notification.timestamp;
            // 선점(reserved_)은 이전 세션과 함께 사라졌다. 아래 체결 처리가
            //  on_fill_confirmed로 선점을 깎으므로, 주문수량만큼 먼저 되살려 순변화를 맞춘다.
            //  일부만 체결되고 나머지가 취소되면 그만큼 선점이 남는데, 주기 잔고 대조의
            //  reset_reserved()가 실제 잔고로 되맞춘다.
            gate_.on_accept(record.signal.account_id, record.signal.ticker, record.signal.side,
                            record.signal.quantity,
                            record.signal.price > 0.0 ? record.signal.price : record.signal.reference_price);
            LOG_INFO("[OrderRouter] 재기동 복원 [" + record.order_id + "] ODNO=" + fill_notification.kis_order_no + " " +
                     record.signal.ticker +
                     (record.signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                     std::to_string(record.signal.quantity) + "주 전략=" + record.signal.strategy_id +
                     " (주문 사유 기록에서 복구)");
            push_history_locked(std::move(record));
            order_reasons_.erase(jitter);   // 같은 ODNO를 두 번 되살리지 않는다
        }
    }


    // 이미 주문수량을 다 채운 주문의 추가 통보인지 구분한다. 이걸 아래 미매핑 경로로
    //  흘려보내면 같은 체결이 포지션에 두 번 쌓인다(ODNO는 아는데 잔량만 없는 상태).
    bool          exhausted = false;
    ManagedOrder* matched   = find_by_order_number_locked(order_number); // ODNO 색인 한 번 — 이력을 훑지 않는다

    // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용. 그 밖의 상태(취소·거부)는 미매핑 경로로.
    if (matched && (matched->status == OrderStatus::ACCEPTED || matched->status == OrderStatus::FILLED))
    {
        ManagedOrder& managed_order = *matched;

        // 이미 전량 체결 완료된 주문은 재처리 방지
        if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
        {
            exhausted = true;
        }
        else
        {

        // 주문 잔량 상한 — 누적 체결이 주문수량을 넘지 못하게 클램프한다.
        //  통보 재전송으로 같은 체결이 두 번 와도 과체결로 원장이 부풀지 않는다.
        const int outstanding = managed_order.signal.quantity - managed_order.confirmed_quantity;
        const int apply_quantity   = (fill_notification.filled_quantity > outstanding) ? outstanding : fill_notification.filled_quantity;

        if (apply_quantity < fill_notification.filled_quantity)
        {
            LOG_WARN(std::format("[OrderRouter] 주문잔량 초과 체결통보 — 잔량으로 클램프 [{}] ODNO={} 통보={}주 잔량={}주",
                                 managed_order.order_id, fill_notification.kis_order_no, fill_notification.filled_quantity, outstanding));
        }

        managed_order.confirmed_quantity += apply_quantity;
        managed_order.updated_at     = fill_notification.timestamp;

        if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
        {
            managed_order.status = OrderStatus::FILLED;
        }

        LOG_INFO("[OrderRouter] 체결 확인 [" + managed_order.order_id + "] ODNO=" + fill_notification.kis_order_no +
                 " " + fill_notification.ticker +
                 (fill_notification.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(apply_quantity) + "주 @" +
                 std::to_string(static_cast<int>(fill_notification.filled_price)) +
                 " (누적 " + std::to_string(managed_order.confirmed_quantity) +
                 "/" + std::to_string(managed_order.signal.quantity) + "주)");


        // 포지션 원장 갱신 (average_price 재계산 + 실현손익) — 원주문의 계좌로 파티션.
        // 현재는 단일 CANO 전제라 ODNO가 유일 → managed_order.signal.account_id 매핑이 정확하다.
        // TODO(다계좌): 진짜 다중 CANO 라우팅 시 ODNO가 계좌별로 재사용되므로 체결 매칭 키를
        //   (kis_order_no + account) 또는 CANO별 H0STCNI 피드 분리로 확장해야 오적립을 막는다.
        auto result = gate_.on_fill_confirmed(managed_order.signal.account_id, fill_notification.ticker, fill_notification.side,
                                              apply_quantity, fill_notification.filled_price, managed_order.signal.strategy_index);

        // 락 밖에서 쓰려고 복사한다 — managed_order는 history_ 원소라 record()의 축출로 참조가 죽을 수 있다.
        const ManagedOrder snapshot        = managed_order;
        const std::string  open_orders = snapshot_open_orders_locked(); // 잔량이 줄었으니 부속 파일을 다시 쓴다
        const uint64_t     sequence         = ++open_orders_sequence_;
        lock.unlock();

        if (result.basis_unknown)
        {
            LOG_WARN(std::format("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [{}] {} {}주 @{} (원장 재시드 필요)",
                                 snapshot.order_id, fill_notification.ticker, apply_quantity, static_cast<int>(fill_notification.filled_price)));
        }

        // 거래 원장 CSV — 실제 체결(부분/전량)을 한 줄로 영속화. 실현손익을 같이 남기려고
        //   gate_.on_fill_confirmed() 뒤에 쓴다(managed_order.status는 위에서 이미 갱신됨).
        write_trade_row("FILL", snapshot, apply_quantity, fill_notification.filled_price, result.realized_pnl,
                        result.strategy_realized_pnl);
        write_open_orders_file(open_orders, sequence);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_fill(fill_notification, snapshot.signal.strategy_id, result.commission, result.tax,
                               result.average_price, result.net_quantity,
                               result.realized_pnl);
        }
#endif
        return;
        }
    }

    if (exhausted)
    {
        LOG_WARN(std::format("[OrderRouter] 주문수량 충족 후 추가 체결통보 무시 ODNO={} {} {}주 (통보 재전송 추정)", fill_notification.kis_order_no,
                             fill_notification.ticker, fill_notification.filled_quantity));
        return;
    }

    // ── ODNO 미매핑 체결 — 이 프로세스가 낸 주문이 아니다 ────────────────────
    //  history_는 메모리에만 있어서 장중 재시작하면 이전 세션의 미체결 주문이 사라진다.
    //  거래소 호가창에는 그 주문이 그대로 살아있으므로, 나중에 체결되면 여기로 떨어진다.
    //  2026-09-07 ODNO 0000014893이 이 경우다 — 09:58 접수, 10:38 재시작, 11:07 91주 전량
    //  체결이 통째로 버려져 원장·포지션이 91주(약 498만원) 어긋났다.
    //  체결 자체는 실재하므로 버리지 않고 원장·포지션에 반영한다. 전략 귀속만 알 수 없어
    //  strategy_id를 "UNLINKED"로 남긴다(사후 분석에서 구분 가능).
    //  선점(reserved_)은 이전 세션과 함께 사라졌다. on_fill_confirmed는 선점 해제를 전제로
    //  reserved_를 깎으므로, 그대로 부르면 음수 선점이 생겨 이후 한도 계산이 왜곡된다.
    //  같은 수량을 on_accept로 먼저 되살린 뒤 해제시켜 순변화를 0으로 맞춘다.
    //  미연결은 history_에 넣지 않으므로(주문수량을 몰라 잔량 클램프가 없다) 같은 통보가 재전송되면
    //  또 여기로 떨어진다. 키(거래일:kis_order_no:시각:수량:단가)로 2회차부터 막는다 — 같은 초·같은
    //  수량·단가로 갈라진 미연결 분할체결은 잃지만, 두 번 쌓는 쪽이 더 큰 사고다(W-6).
    if (!unlinked_fill_keys_.insert(fill_key).second)
    {
        LOG_WARN(std::format("[OrderRouter] 미매핑 체결 재통보 무시 ODNO={} {} {}주 time={} (같은 키 재수신)", fill_notification.kis_order_no,
                             fill_notification.ticker, fill_notification.filled_quantity, fill_notification.fill_time));
        return;
    }

    ManagedOrder unlinked_fill;
    unlinked_fill.order_id           = next_id();
    unlinked_fill.kis_order_no       = fill_notification.kis_order_no;
    unlinked_fill.status             = OrderStatus::FILLED;
    unlinked_fill.confirmed_quantity      = fill_notification.filled_quantity;
    unlinked_fill.signal.strategy_id    = "UNLINKED";
    unlinked_fill.signal.strategy_index = unlinked_strategy_index_;
    unlinked_fill.signal.ticker      = fill_notification.ticker;
    unlinked_fill.signal.side        = fill_notification.side;
    unlinked_fill.signal.type        = OrderType::LIMIT;
    unlinked_fill.signal.quantity    = fill_notification.filled_quantity;
    unlinked_fill.signal.price       = fill_notification.filled_price;
    unlinked_fill.signal.reason      = "이전 세션 주문 체결(ODNO 미매핑)";
    unlinked_fill.submitted_at       = fill_notification.timestamp;
    unlinked_fill.updated_at         = fill_notification.timestamp;

    LOG_WARN(std::format("[OrderRouter] 미매핑 체결 원장 반영 [{}] ODNO={} {} {} {}주 @{} — 이전 세션 주문으로 추정(재시작 전 접수분)",
                         unlinked_fill.order_id, fill_notification.kis_order_no, fill_notification.ticker, fill_notification.side == OrderSide::BUY ? "BUY" : "SELL",
                         fill_notification.filled_quantity, static_cast<int>(fill_notification.filled_price)));

    gate_.on_accept(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                    fill_notification.filled_quantity, fill_notification.filled_price);
    auto result = gate_.on_fill_confirmed(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                                          fill_notification.filled_quantity, fill_notification.filled_price, unlinked_fill.signal.strategy_index);
    lock.unlock(); // 원장 갱신 끝 — 파일 쓰기는 락 밖에서

    if (result.basis_unknown)
    {
        LOG_WARN(std::format("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [{}] {} {}주 @{} (원장 재시드 필요)",
                             unlinked_fill.order_id, fill_notification.ticker, fill_notification.filled_quantity, static_cast<int>(fill_notification.filled_price)));
    }

    write_trade_row("FILL", unlinked_fill, fill_notification.filled_quantity, fill_notification.filled_price, result.realized_pnl,
                    result.strategy_realized_pnl);
#ifdef HAS_ZMQ
    if (zmq_)
    {
        zmq_->publish_fill(fill_notification, unlinked_fill.signal.strategy_id, result.commission, result.tax,
                           result.average_price, result.net_quantity,
                           result.realized_pnl);
    }
#else
    (void)result;
#endif
}

// ─── 일별 리셋 (장 시작 시 Engine이 호출) ─────────────────────────────────
// 중복방지 키(seen_fills_)의 무한 증가를 해소. 거래일 prefix로 cross-day 충돌은 이미
// 차단되므로, 전일 키는 더 이상 필요 없다.
void OrderRouter::reset_daily()
{
    std::lock_guard<std::mutex> lock(history_mutex_);
    seen_fills_.clear();
    unlinked_fill_keys_.clear();
    // 사유 기록도 거래일이 바뀌면 다시 읽는다(파일이 날짜별이라 어제 것을 들고 있으면 안 된다).
    order_reasons_.clear();
    order_reasons_loaded_ = false;
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
    int start = std::max(0, static_cast<int>(history_.size()) - count);
    return {history_.begin() + start, history_.end()};
}
