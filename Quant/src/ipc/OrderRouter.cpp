#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

// ─── 오늘 날짜 YYYYMMDD (로컬) ───────────────────────────────────────────
//  날짜별 파일 이름에 쓴다. 원장 CSV가 쓰는 것과 같은 기준(로컬 시각)이다.
static std::string today_ymd()
{
    std::time_t tt = std::time(nullptr);
    std::tm     lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &lt);
    return std::string(buf);
}


// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    // 주문마다 부르는 곳이라 스트림 대신 고정 버퍼로 만든다(D-042). 6자리를 넘으면 자릿수만 늘어난다.
    const unsigned long long n = ++seq_;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ORD-%06llu", n);
    return std::string(buf);
}

// ─── 거부 사유에 KIS 오류코드 꼬리표 부착 ───────────────────────────────
//  order_thread가 EGW00201(초당 거래건수 초과)을 문자열로 판별해 적응적 재시도를 걸 수 있게,
//  응답의 err_code를 " [코드]" 형태로 reject_reason 끝에 붙인다. 코드 없으면 빈 문자열.
std::string OrderRouter::kis_err_suffix(const OrderAck& ack)
{
    return ack.err_code.empty() ? std::string() : (" [" + ack.err_code + "]");
}

// ─── 주문 제출 — action에 따라 라우팅 (MM-1) ─────────────────────────────
//  전 경로가 단일 order_thread에서만 실행된다(Engine::order_thread_fn) — OrderGate C6의
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다.
// 취소가 "취소 대상 없음"으로 되돌아온 뒤 그 종목의 신규 매수를 막아 두는 시간(초).
//  전략의 재구성 주기(min_action_ms 3초 + 재조회)보다 길고, 존 이탈 청산을 늦출 만큼
//  길지는 않은 값. 이 창 안에 들어온 매수는 원주문 체결분과 겹칠 수 있다.
static constexpr int kCancelMissGuardSec = 10;

ManagedOrder OrderRouter::submit(const OrderSignal& sig)
{
    switch (sig.action)
    {
    case OrderAction::CANCEL:  return cancel_route(sig);
    case OrderAction::REPLACE: return replace_route(sig);
    case OrderAction::NEW:
    default:                   return new_route(sig);
    }
}

// ─── 신규 주문 (기존 경로) ─────────────────────────────────────────────────
ManagedOrder OrderRouter::new_route(const OrderSignal& in_sig)
{
    auto now = std::chrono::system_clock::now();

    // 0. 한도 클램프 — 한도를 넘치면 거부 대신 한도 안으로 줄여 낸다.
    //    분할 매수 전략은 매 틱 같은 rung을 다시 내므로, 넘친다고 버리면 그 종목은 하루 종일
    //    한 주도 못 나가면서 초당 주문 예산만 태운다(09-08 오전 126640·293490 반복 거부).
    //    여유가 0이면 손대지 않는다 — 아래 check()가 어느 한도에 걸렸는지 그대로 남기게 둔다.
    OrderSignal sig = in_sig;
    bool sell_no_qty = false;
    const int allowed = gate_.clamp_buy_qty(sig);

    if (allowed == 0 && sig.side == OrderSide::SELL && sig.action == OrderAction::NEW &&
        sig.quantity > 0)
    {
        // 매도가능수량이 0이면 여기서 끊는다. BUY와 달리 아래 check()는 매도가능수량을 모르므로
        //  그대로 통과시키고, KIS가 주문을 통째로 40240000(모의투자 잔고내역이 없습니다)으로
        //  거부한다 — 한 주도 못 빠져나오면서 초당 주문 예산만 태운다(09-08 001450 105주·
        //  086450 486주·047050 254주/381주가 모두 이 경로로 전량 거부됐다).
        sell_no_qty = true;
    }
    else if (allowed > 0 && allowed < sig.quantity)
    {
        LOG_INFO("[OrderRouter] 한도 클램프 " + sig.ticker + " " +
                 std::to_string(sig.quantity) + "주 → " + std::to_string(allowed) + "주");
        sig.quantity = allowed;
    }

    ManagedOrder mo;
    mo.order_id    = next_id();
    mo.signal      = sig;
    mo.submitted_at = now;
    mo.updated_at   = now;
    mo.status       = OrderStatus::PENDING;

    ++total_count_;

    // 방금 이 종목의 취소가 "취소 대상 없음"으로 되돌아왔다면, 원주문이 이미 체결됐을 수
    //  있다. 전략은 그 결과를 보지 못한 채 대체 주문을 이어 내므로 그대로 두면 중복 매수가
    //  된다. 다음 재구성 주기에 전략이 실제 보유수량을 다시 읽을 때까지만 막는다.
    //  매도는 막지 않는다 — 노출을 줄이는 쪽이고, 늦추면 손실이 커진다.
    if (sig.side == OrderSide::BUY)
    {
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lk(hist_mtx_);
            auto it = cancel_miss_.find(sig.ticker);

            if (it != cancel_miss_.end())
            {
                const auto age = std::chrono::steady_clock::now() - it->second;

                if (age < std::chrono::seconds(kCancelMissGuardSec))
                {
                    blocked = true;
                }
                else
                {
                    cancel_miss_.erase(it);
                }
            }
        }

        if (blocked)
        {
            mo.status        = OrderStatus::REJECTED;
            mo.reject_reason = "직전 취소가 대상 없음 — 보유수량 재확인까지 보류";
            ++rejected_count_;
            LOG_WARN("[OrderRouter] 대체 주문 보류 [" + mo.order_id + "] " + sig.ticker +
                     " " + mo.reject_reason);
            record(mo);
            return mo;
        }
    }

    // 1. OrderGate 검증
    std::string reject_reason;

    if (sell_no_qty)
    {
        reject_reason = "매도가능수량 0 (미체결 매도·미결제분) — 발주 생략";
    }

    if (!reject_reason.empty() || !gate_.check(sig, reject_reason))
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = reject_reason;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 거부 [" + mo.order_id + "] " +
                 sig.ticker + " → " + reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(sig, false);
        }
#endif
        record(mo);
        return mo;
    }

    // 2. KIS 주문 전송 (submit_order_ack로 ODNO + KRX 조직번호 캡처 — 정정/취소 준비)
    //    접수 왕복지연(RTT)을 재서 접수 로그에 남긴다 → log_report.py가 중앙값(p50)·상위 1%(p99) 집계.
    mo.status = OrderStatus::SUBMITTED;
    std::string odno;
    OrderAck ack;
    const auto t_send = std::chrono::steady_clock::now();
    long rtt_ms = 0;

    try
    {
        ++kis_calls_;
        ack  = kis_.submit_order_ack(sig);
        odno = ack.odno;
        rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t_send)
                     .count();
    }
    catch (const std::exception& e)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = std::string("KIS 예외: ") + e.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 예외 [" + mo.order_id + "] " + sig.ticker + " — " + e.what());
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(sig, false);
        }
#endif
        record(mo);
        return mo;
    }

    mo.updated_at = std::chrono::system_clock::now();

    // 청산차단 자가정리 — SELL이 "주문가능분 없음"(40240000)으로 막히면, 그 종목의
    //  미체결 예약매도(이전 세션/수동 예약이 보유수량을 묶은 것)를 조회·취소하고 시장가로 1회
    //  재시도한다. 성공하면 아래 접수 블록이 그대로 처리(odno/ack가 재시도 결과로 갱신됨).
    if (!ack.ok() && sig.side == OrderSide::SELL && ack.err_code == kis_err::kNoSellableQty)
    {
        OrderAck rack = reconcile_blocked_sell(sig);

        if (rack.ok())
        {
            ack  = rack;
            odno = rack.odno;
        }
    }

    if (!odno.empty())
    {
        mo.status      = OrderStatus::ACCEPTED;
        mo.kis_order_no = odno;
        mo.krx_orgno    = ack.krx_orgno; // 정정/취소 시 원주문 조직번호로 재입력
        ++accepted_count_;
        // KIS 접수 시점에 포지션 선점 (보수적 추적 — 실제 체결 확인 전까지 재주문 차단)
        //  선점가는 지정가=price, 시장가(0)=ref_price로 근사 stamp → §3d 총노출이 시장가 선점을
        //  과소평가하지 않게(check()의 eval_px와 대칭, 보수측).
        gate_.on_accept(sig.account_id, sig.ticker, sig.side, sig.quantity,
                        sig.price > 0.0 ? sig.price : sig.ref_price);

        // client_oid → order_id 인덱스 (취소/정정 대상 조회용). hist_mtx_는 record()에서 잡으므로
        //   여기선 별도로 짧게 보호한다(단일 order_thread라 경합은 on_fill/recent와만 발생).
        if (!sig.client_oid.empty())
        {
            std::lock_guard<std::mutex> lk(hist_mtx_);
            oid_index_[sig.client_oid] = mo.order_id;
        }

        LOG_INFO("[OrderRouter] 접수 [" + mo.order_id + "] ODNO=" + odno +
                 " " + sig.ticker +
                 (sig.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(sig.quantity) + "주 RTT=" +
                 std::to_string(rtt_ms) + "ms");
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(sig, true);
        }
#endif
    }
    else
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS API 거부 (빈 ODNO)" + kis_err_suffix(ack);
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 거부 [" + mo.order_id + "] " + sig.ticker + mo.reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_order(sig, false);
        }
#endif
    }

    record(mo);
    return mo;
}

// ─── 청산차단 자가정리 — 예약매도 취소 후 시장가 재매도 (장중) ─────────────
//  전제: SELL이 40240000(주문가능분 없음)으로 막힌 직후 호출. 그 종목의 미체결 예약매도가
//  보유수량을 묶어 ord_psbl_qty=0이 된 상황을 KIS 미체결 조회로 규명하고, 예약을 취소해
//  수량을 풀어준 뒤 시장가 매도를 1회 재시도한다. 취소한 예약이 이번 세션 주문(history_에
//  ODNO가 있음)이면 CANCELLED로 닫고 게이트 선점(reserved_)을 풀어 원장 행을 남긴다 — 그러지
//  않으면 선점이 스윕 때까지 남아 한도 계산을 조인다(C-2). 이전 세션·수동 예약은 history_에
//  없으므로 gate_를 건드리지 않는다(포지션 정합은 체결통보로).
OrderAck OrderRouter::reconcile_blocked_sell(const OrderSignal& sig)
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
        std::lock_guard<std::mutex> lk(hist_mtx_);

        for (const auto& mo : history_)
        {
            if (mo.status != OrderStatus::ACCEPTED || mo.signal.side != OrderSide::SELL ||
                mo.signal.ticker != sig.ticker || mo.kis_order_no.empty())
            {
                continue;
            }

            const int outstanding = mo.signal.quantity - mo.confirmed_qty;

            if (outstanding <= 0)
            {
                continue;
            }

            OpenOrder o;
            o.ticker    = mo.signal.ticker;
            o.odno      = mo.kis_order_no;
            o.krx_orgno = mo.krx_orgno;
            o.psbl_qty  = outstanding;
            o.ord_unpr  = mo.signal.price;
            o.side      = OrderSide::SELL;
            opens.push_back(o);
        }
    }
    else
    {
        try
        {
            opens = kis_.get_open_orders();
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[OrderRouter] 미체결 조회 예외 — " + std::string(e.what()));
            return OrderAck::fail(kis_err::kTransport);
        }
    }

    int cancelled = 0;

    for (const auto& o : opens)
    {
        if (o.ticker != sig.ticker || o.side != OrderSide::SELL)
        {
            continue; // 해당 종목의 예약'매도'만 대상
        }

        LOG_WARN("[OrderRouter] 청산차단 해소 " + sig.ticker + " 예약매도 " +
                 std::to_string(o.psbl_qty) + "주 ODNO=" + o.odno + " @" +
                 std::to_string(static_cast<int>(o.ord_unpr)) + " → 취소 시도");
        OrderAck cxl;

        try
        {
            ++kis_calls_;
            cxl = kis_.cancel_order(o.ticker, o.odno, o.krx_orgno, o.psbl_qty, /*all_remaining=*/true);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[OrderRouter] 예약취소 예외 " + sig.ticker + " — " + std::string(e.what()));
            continue;
        }

        if (!cxl.ok())
        {
            continue;
        }

        ++cancelled;

        // 이번 세션 주문이면 이력·선점을 같이 정리한다. 잠금 순서 hist_→gate는 cancel_route와 같다.
        ManagedOrder closed;
        bool         found   = false;
        int          release = 0;
        {
            std::lock_guard<std::mutex> lk(hist_mtx_);

            for (auto& mo : history_)
            {
                if (mo.status != OrderStatus::ACCEPTED || mo.kis_order_no != o.odno)
                {
                    continue;
                }

                release = mo.signal.quantity - mo.confirmed_qty;

                if (release < 0)
                {
                    release = 0;
                }

                mo.status        = OrderStatus::CANCELLED;
                mo.reject_reason = "청산차단 해소 취소";
                mo.updated_at    = std::chrono::system_clock::now();
                closed           = mo;
                found            = true;
                oid_index_.erase(mo.signal.client_oid);
                break;
            }

            if (release > 0)
            {
                gate_.on_cancel(closed.signal.account_id, closed.signal.ticker, OrderSide::SELL, release);
            }
        }

        if (found)
        {
            write_trade_row("", closed, 0, 0.0);
        }
    }

    if (cancelled == 0)
    {
        LOG_WARN("[OrderRouter] 청산차단 미해소 " + sig.ticker +
                 " — 취소할 예약매도 없음/취소 실패 (수동 확인 필요)");
        return OrderAck::fail(kis_err::kNoSellableQty); // 원인은 그대로 — 호출부가 거부 사유로 남긴다
    }

    LOG_INFO("[OrderRouter] 예약매도 " + std::to_string(cancelled) + "건 취소 완료 → " +
             sig.ticker + " 시장가 매도 재시도");

    try
    {
        ++kis_calls_;
        return kis_.submit_order_ack(sig);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[OrderRouter] 청산 재매도 예외 " + sig.ticker + " — " + std::string(e.what()));
        return OrderAck::fail(kis_err::kTransport);
    }
}

// ─── 유령 선점 정리 ───────────────────────────────────────────────────────
//  게이트의 선점(reserved_)은 접수 때만 생기고 체결·취소 통보로만 풀린다. 통보를 한 번
//  놓치면 그 선점이 슬롯을 물고 남아, 실제 보유가 한도에 못 미치는데 신규 진입이 막힌다
//  (09-09: 보유 20인데 "25 >= 25" 거부). 라우터 이력에 살아있는 주문이 없으면 푼다.
int OrderRouter::sweep_stale_reservations()
{
    std::vector<std::string> live;
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);

        // [inv] 이력이 비면 아무 것도 풀지 않는다. 선점은 접수 때만 생기고 접수는 이력에도
        //  남으므로 정상적으로는 둘이 같이 비어 있다. 이력만 비는 경우(재기동 직후, 상한 초과로
        //  잘려 나간 뒤)에 정본으로 믿으면 살아 있는 선점을 통째로 푼다.
        if (history_.empty())
        {
            return 0;
        }

        for (const auto& mo : history_)
        {
            if (mo.status == OrderStatus::ACCEPTED && mo.confirmed_qty < mo.signal.quantity)
            {
                live.push_back(mo.signal.ticker);
            }
        }
    }

    const auto gone = gate_.prune_reservations(live);

    if (!gone.empty())
    {
        std::string list;

        for (const auto& t : gone)
        {
            list += (list.empty() ? "" : ",") + t;
        }

        LOG_WARN("[OrderRouter] 살아있는 주문 없는 선점 " + std::to_string(gone.size()) +
                 "종목 해제 (" + list + ")");
    }

    return static_cast<int>(gone.size());
}

// ─── 이력 저장 (max_history 초과 시 체결 완료/거부된 것만 삭제) ───────────
void OrderRouter::record(const ManagedOrder& mo)
{
    std::string open_orders;
    uint64_t    seq = 0;
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        history_.push_back(mo);

        while (static_cast<int>(history_.size()) > cfg_.max_history)
        {
            // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
            if (history_.front().status == OrderStatus::ACCEPTED)
            {
                break;
            }

            history_.pop_front();
        }

        open_orders = snapshot_open_orders_locked();
        seq         = ++open_orders_seq_;
    }

    // 파일 I/O는 hist_mtx_ 밖에서 — 디스크가 느린 순간 체결(on_fill)·발주(submit)가 같이 밀리지 않게(W-8).
    // 거래 원장 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → mo.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    write_trade_row("", mo, 0, 0.0);
    write_open_orders_file(open_orders, seq);
    append_order_reason(mo);
}

// ─── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
//  형식: odno|ticker|side|수량|가격|기준가|전략|사유   (한 줄 한 주문, 헤더 없음)
//  접수된 신규·정정 주문만 남긴다. 취소는 체결되지 않으므로 대상이 아니다.
void OrderRouter::append_order_reason(const ManagedOrder& mo)
{
    if (mo.status != OrderStatus::ACCEPTED || mo.kis_order_no.empty() ||
        mo.signal.action == OrderAction::CANCEL || mo.signal.quantity <= 0)
    {
        return;
    }

    // 구분자와 줄바꿈은 공백으로 바꾼다 — 사유 문구에 무엇이 들어와도 한 줄을 유지한다.
    auto safe = [](std::string s)
    {
        for (char& c : s)
        {
            if (c == '|' || c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }

        return s;
    };

    std::ostringstream line;
    line << mo.kis_order_no << '|' << safe(mo.signal.ticker) << '|'
         << (mo.signal.side == OrderSide::BUY ? "BUY" : "SELL") << '|'
         << mo.signal.quantity << '|'
         << static_cast<long long>(mo.signal.price) << '|'
         << static_cast<long long>(mo.signal.ref_price) << '|'
         << safe(mo.signal.strategy_id) << '|' << safe(mo.signal.reason) << '\n';

    std::lock_guard<std::mutex> lk(io_mtx_);
    std::ofstream out(Logger::instance().path_for("order_reasons_" + today_ymd() + ".txt"),
                      std::ios::app);

    if (out)
    {
        out << line.str();
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
    int n = 0;

    while (std::getline(in, line))
    {
        std::vector<std::string> f;
        std::string tok;
        std::istringstream ss(line);

        while (std::getline(ss, tok, '|'))
        {
            f.push_back(tok);
        }

        if (f.size() < 8 || f[0].empty())
        {
            continue;
        }

        OrderReason r;
        r.ticker      = f[1];
        r.side        = (f[2] == "SELL") ? OrderSide::SELL : OrderSide::BUY;
        r.strategy_id = f[6];
        r.reason      = f[7];

        try
        {
            r.quantity  = std::stoi(f[3]);
            r.price     = std::stod(f[4]);
            r.ref_price = std::stod(f[5]);
        }
        catch (const std::exception&)
        {
            continue;   // 숫자가 깨진 줄은 버린다
        }

        if (r.quantity <= 0)
        {
            continue;
        }

        order_reasons_[f[0]] = r;   // 같은 ODNO가 여러 줄이면 마지막 것이 맞다
        ++n;
    }

    if (n > 0)
    {
        LOG_INFO("[OrderRouter] 주문 사유 기록 " + std::to_string(n) + "건 복원");
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
    std::ostringstream buf;

    for (const auto& o : history_)
    {
        if (o.status != OrderStatus::ACCEPTED)
        {
            continue;
        }

        const int remain = o.signal.quantity - o.confirmed_qty;

        if (remain <= 0 || o.kis_order_no.empty())
        {
            continue;
        }

        buf << o.kis_order_no << '|' << o.krx_orgno << '|' << o.signal.ticker << '|'
            << (o.signal.side == OrderSide::BUY ? "BUY" : "SELL") << '|' << remain << '\n';
    }

    // 이전 세션 줄은 아직 취소가 안 끝난 것만 남아 있다 — 이번 세션 줄과 합쳐 쓴다.
    {
        std::lock_guard<std::mutex> lk(carry_mtx_);

        for (const auto& f : carry_rows_)
        {
            buf << f[0] << '|' << f[1] << '|' << f[2] << '|' << f[3] << '|' << f[4] << '\n';
        }
    }

    return buf.str();
}

void OrderRouter::rewrite_open_orders()
{
    std::string body;
    uint64_t    seq = 0;
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        body = snapshot_open_orders_locked();
        seq  = ++open_orders_seq_;
    }

    write_open_orders_file(body, seq);
}

void OrderRouter::write_open_orders_file(const std::string& body, uint64_t seq)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::lock_guard<std::mutex> lk(io_mtx_);

    if (seq <= open_orders_written_seq_)
    {
        return; // 더 새 스냅샷이 먼저 쓰였다 — 옛 것으로 덮으면 살아있는 주문이 사라진다
    }

    open_orders_written_seq_ = seq;

    fs::path path = Logger::instance().path_for("open_orders.txt");
    fs::path tmp  = Logger::instance().path_for("open_orders.tmp");

    // 임시파일에 쓰고 원자적으로 갈아끼운다 — 기동 중 크래시로 반쪽 파일을 읽지 않도록.
    {
        std::ofstream out(tmp, std::ios::trunc);

        if (!out)
        {
            return;
        }

        out << body;
    }

    fs::rename(tmp, path, ec);

    if (ec)
    {
        fs::remove(tmp, ec);
    }
}

// ─── 이전 세션이 남긴 미체결 주문 취소 (기동 시 1회) ──────────────────────
OrderRouter::~OrderRouter()
{
    stale_stop_.store(true, std::memory_order_relaxed);

    if (stale_thr_.joinable())
    {
        stale_thr_.join();
    }
}

void OrderRouter::cancel_stale_orders_async()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path path = Logger::instance().path_for("open_orders.txt");

    if (!fs::exists(path, ec))
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

            std::array<std::string, 5> f;
            size_t pos = 0, idx = 0;
            bool ok = true;

            while (idx < 5)
            {
                size_t bar = line.find('|', pos);

                if (idx < 4 && bar == std::string::npos) { ok = false; break; }
                f[idx++] = line.substr(pos, idx < 5 && bar != std::string::npos
                                                ? bar - pos : std::string::npos);

                if (bar == std::string::npos)
                {
                    break;
                }

                pos = bar + 1;
            }

            if (ok && idx == 5 && !f[0].empty())
            {
                rows.push_back(f);
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
        std::lock_guard<std::mutex> lk(carry_mtx_);
        carry_rows_.clear();

        for (const auto& f : rows)
        {
            int q = 0;

            try { q = std::stoi(f[4]); } catch (...) {}

            if (q > 0)
            {
                carry_rows_.push_back(f);   // 수량이 없는 줄은 취소할 것도 없다 — 넘기지 않는다
            }
        }
    }

    LOG_WARN("[OrderRouter] 이전 세션 미체결 " + std::to_string(rows.size()) +
             "건 발견 — 백그라운드 취소 시작(유령 주문이 현금을 묶고 청산 직후 재진입을 만든다)");

    if (stale_thr_.joinable())
    {
        stale_thr_.join();
    }

    // 취소는 건당 왕복 3~5초다. 기동 경로에서 돌리면 장중 재기동이 5분씩 멈춘다.
    //  잔고 시드는 이 스레드를 기다리지 않아도 된다 — 미체결 취소는 보유수량을 바꾸지
    //  않고 주문가능현금·매도가능수량만 푸는데, 둘 다 주기 잔고 대조가 다시 읽는다.
    stale_thr_ = std::thread([this, rows]()
    {
        int cancelled = 0;

        for (const auto& f : rows)
        {
            if (stale_stop_.load(std::memory_order_relaxed))
            {
                LOG_WARN("[OrderRouter] 유령주문 취소 중단(종료 요청) — 남은 " +
                         std::to_string(rows.size() - static_cast<size_t>(cancelled)) + "건");
                return;
            }

            int qty = 0;

            try { qty = std::stoi(f[4]); } catch (...) { continue; }

            if (qty <= 0)
            {
                continue;
            }

            OrderAck res;
            bool     rate_limited = false;

            // 한도 거부(EGW00201)는 "이미 종료"가 아니다. 같은 분기로 흘리면 유령 예약이 KIS에
            //  남은 채 전략이 같은 종목을 새로 깔아 체결 시 이중 포지션이 된다(09-11 09:17~09:18
            //  180640·005935·007660 6건). 한도는 1초 창이라 잠깐 쉬고 다시 보낸다.
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                try
                {
                    ++kis_calls_;
                    res = kis_.cancel_order(f[2], f[0], f[1], qty, /*all_remaining=*/true);
                }
                catch (const std::exception& e)
                {
                    LOG_WARN("[OrderRouter] 유령주문 취소 예외 " + f[2] + " ODNO=" + f[0] + " — " + e.what());
                    break;
                }

                rate_limited = !res.ok() && res.err_code == kis_err::kRateLimit;

                if (!rate_limited || stale_stop_.load(std::memory_order_relaxed))
                {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            }

            if (rate_limited)
            {
                // 줄은 carry_rows_에 남긴다 — 다음 재기동이 다시 시도한다.
                LOG_WARN("[OrderRouter] 유령주문 취소 실패(한도 거부 반복) — KIS에 잔존 " + f[2] +
                         " ODNO=" + f[0] + " " + std::to_string(qty) + "주 (부속 파일에 유지)");
                continue;
            }

            if (res.ok())
            {
                ++cancelled;
                LOG_INFO("[OrderRouter] 유령주문 취소 " + f[2] + " " + f[3] + " " +
                         std::to_string(qty) + "주 ODNO=" + f[0]);

                // 취소로 브로커에서는 수량이 풀렸지만 게이트의 sellable_은 잔고 시드값
                //  (ord_psbl_qty, 취소 전 스냅샷) 그대로다. 되돌리지 않으면 미체결이 없는데도
                //  자기 청산이 막힌다 — 09-09 000215은 13:45 취소 뒤 16분간 "매도가능수량 0"으로
                //  교체 진입이 네 번 무산됐다. 매도 취소만 해당한다(매수는 현금을 풀 뿐이다).
                if (f[3] == "SELL")
                {
                    gate_.restore_sellable(std::string(), f[2], qty);
                }
            }
            else
            {
                // 이미 체결·취소됐으면 KIS가 거부한다 — 정상이다.
                LOG_INFO("[OrderRouter] 유령주문 취소 불가(이미 종료 추정) " + f[2] + " ODNO=" + f[0]);
            }

            // 취소 접수든 이미 종료든 이 줄은 끝났다 — 부속 파일에서 뺀다.
            {
                std::lock_guard<std::mutex> lk(carry_mtx_);
                carry_rows_.erase(std::remove_if(carry_rows_.begin(), carry_rows_.end(),
                                                 [&f](const std::array<std::string, 5>& r) { return r[0] == f[0]; }),
                                  carry_rows_.end());
            }

            rewrite_open_orders();

            // 초당 거래건수 상한(EGW00201)에 걸리지 않게 간격을 둔다. 종료 요청에 몇 분씩
            //  붙들리지 않도록 잘게 끊어 자면서 플래그를 본다.
            for (int i = 0; i < 4 && !stale_stop_.load(std::memory_order_relaxed); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        LOG_INFO("[OrderRouter] 이전 세션 미체결 정리 완료: " + std::to_string(cancelled) + "건 취소 접수");
    });
}

// ─── 거래 원장 CSV 적재 ───────────────────────────────────────────────────
//  실행 로그(quant_trader.log)와 별개로 매수·매도·거부·체결·잔고 대조를 구조적으로 남긴다.
//  logs/trades_YYYYMMDD.csv 에 한 줄씩 append(날짜별 파일). record()·on_fill()·record_reconcile()
//  이 줄을 만들고 append_trade_line이 io_mtx_로 직렬화해 쓴다(hist_mtx_ 밖 — 동시쓰기 없음).
//  원장 쓰기 실패는 매매를 막지 않는다(best-effort — 조용히 반환).
//  열 정본은 kTradeHeader 하나다. 열을 더할 때는 끝에 붙인다 — 스키마 승격이 옛 파일 행 끝에 빈 칸을
//  덧붙이는 방식이라 중간 삽입은 기존 행의 값을 엉뚱한 열로 밀어낸다. Python 판독기는 열 이름으로 읽는다.
static const std::string kTradeHeader =
    "ts_kst,event,order_id,odno,strategy,ticker,side,type,"
    "order_qty,order_price,fill_qty,fill_price,status,reason,entry_reason,realized_pnl,seq";

// CSV 깨짐 방지: 콤마/개행 공백 치환
static std::string csv_safe(std::string s)
{
    for (char& c : s)
    {
        if (c == ',' || c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }

    return s;
}

void OrderRouter::trade_row_timestamp(char (&dbuf)[9], char (&tbuf)[20])
{
    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    std::strftime(dbuf, sizeof(dbuf), "%Y%m%d", &lt);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
}

void OrderRouter::append_trade_line(const std::string& line)
{
    std::lock_guard<std::mutex> io_lk(io_mtx_);

    char dbuf[9], tbuf[20];
    trade_row_timestamp(dbuf, tbuf);

    namespace fs = std::filesystem;
    std::error_code ec;
    // 실행 위치(cwd)와 무관하게 로그 폴더(main에서 고정)에 매매원장 append.
    fs::path path = Logger::instance().path_for(std::string("trades_") + dbuf + ".csv");

    const bool need_header = !fs::exists(path, ec);

    // 스키마 승격 — 같은 날 파일이 옛 헤더(열이 적음)면 새 열을 붙여 한 번 재작성한다.
    //  한 파일에 15열 헤더와 16열 데이터가 섞이면 판독기가 값을 어긋난 키로 읽는다.
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

                for (std::string ln; std::getline(in, ln); )
                {
                    if (!ln.empty() && ln.back() == '\r')
                    {
                        ln.pop_back();
                    }

                    if (!ln.empty())
                    {
                        rows.push_back(ln + std::string(static_cast<size_t>(add), ','));
                    }
                }

                in.close();
                std::ofstream out(path, std::ios::trunc);

                if (out)
                {
                    out << kTradeHeader << '\n';

                    for (const auto& r : rows)
                    {
                        out << r << '\n';
                    }
                }
            }
        }
    }

    std::ofstream f(path, std::ios::app);

    if (!f.is_open())
    {
        return; // best-effort
    }

    if (need_header)
    {
        f << kTradeHeader << '\n';
    }

    f << tbuf << ',' << line << '\n';
}

void OrderRouter::write_trade_row(const std::string& event, const ManagedOrder& mo,
                                  int fill_qty, double fill_price, double realized_pnl)
{
    const OrderSignal& sig = mo.signal;

    auto side_str = [](OrderSide s) {
        return s == OrderSide::BUY ? "BUY" : (s == OrderSide::SELL ? "SELL" : "NONE");
    };
    auto type_str = [](OrderType t) { return t == OrderType::LIMIT ? "LIMIT" : "MARKET"; };
    auto status_str = [](OrderStatus st) -> const char* {
        switch (st)
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
    std::string ev = event.empty() ? status_str(mo.status) : event;
    // reason = 거부/봉쇄 사유(OrderGate·KIS), entry_reason = 진입 판단 근거(전략, G4) — 분리 컬럼.
    std::string reason       = csv_safe(mo.reject_reason);
    std::string entry_reason = csv_safe(sig.reason);

    std::ostringstream f;
    f << ev << ','
      << mo.order_id << ','
      << mo.kis_order_no << ','
      << sig.strategy_id << ','
      << sig.ticker << ','
      << side_str(sig.side) << ','
      << type_str(sig.type) << ','
      << sig.quantity << ','
      << std::fixed << std::setprecision(2) << sig.price << ','
      << fill_qty << ','
      << std::fixed << std::setprecision(2) << fill_price << ','
      << status_str(mo.status) << ','
      << reason << ','
      << entry_reason << ',';

    // 실현손익은 매도 체결에서만 의미가 있다. 매수·접수·거부 행은 빈 칸으로 둬서
    //  0원 실현으로 오독되지 않게 한다.
    if (event == "FILL" && sig.side == OrderSide::SELL)
    {
        f << std::fixed << std::setprecision(2) << realized_pnl;
    }

    // seq는 전략 스레드가 stamp한 신호 순번(C-2). 미부여(0)는 빈 칸 — 재기동 전 주문의 체결 등.
    f << ',';

    if (sig.seq != 0)
    {
        f << sig.seq;
    }

    append_trade_line(f.str());
}

// ─── 잔고 대조 기록 (C-2) ─────────────────────────────────────────────────
//  live_orders는 history_에서 센다(hist_mtx_). 파일 쓰기는 락 밖.
void OrderRouter::record_reconcile(const ReconcileNote& n)
{
    int live_orders = 0;
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);

        for (const auto& mo : history_)
        {
            if (mo.status == OrderStatus::ACCEPTED && mo.signal.ticker == n.ticker &&
                mo.signal.quantity - mo.confirmed_qty > 0)
            {
                ++live_orders;
            }
        }
    }

    std::ostringstream reason;
    reason << "live_orders=" << live_orders << " diff_qty=" << (n.broker_qty - n.ledger_qty);

    if (!n.note.empty())
    {
        reason << ' ' << csv_safe(n.note);
    }

    if (n.action != "KEEP")
    {
        LOG_WARN("[OrderRouter] 잔고 대조 " + n.ticker + " 원장 " + std::to_string(n.ledger_qty) +
                 "주@" + std::to_string(static_cast<long long>(n.ledger_avg)) + " vs 브로커 " +
                 std::to_string(n.broker_qty) + "주@" +
                 std::to_string(static_cast<long long>(n.broker_avg)) + " → " + n.action + " (" +
                 reason.str() + ")");
    }

    std::ostringstream f;
    f << "RECONCILE" << ",,,,"            // order_id, odno, strategy 빈 칸
      << n.ticker << ",NONE,,"            // side, type 빈 칸
      << n.ledger_qty << ','
      << std::fixed << std::setprecision(2) << n.ledger_avg << ','
      << n.broker_qty << ','
      << std::fixed << std::setprecision(2) << n.broker_avg << ','
      << csv_safe(n.action) << ','
      << reason.str() << ",,,";           // entry_reason, realized_pnl, seq 빈 칸

    append_trade_line(f.str());
}

// ─── client_oid로 살아있는 주문 조회 (호출자가 hist_mtx_ 보유) ────────────
//  live = ACCEPTED 이면서 미체결 잔량이 남은 주문(부분체결도 status는 ACCEPTED 유지).
//  FILLED/CANCELLED/REJECTED는 취소·정정 대상 아님.
ManagedOrder* OrderRouter::find_live_by_oid(const std::string& client_oid)
{
    if (client_oid.empty())
    {
        return nullptr;
    }

    for (auto& mo : history_)
    {
        if (mo.signal.client_oid != client_oid)
        {
            continue;
        }

        if (mo.status == OrderStatus::ACCEPTED && mo.confirmed_qty < mo.signal.quantity)
        {
            return &mo;
        }
    }

    return nullptr;
}

// ─── 취소 라우팅 (action=CANCEL) ──────────────────────────────────────────
//  1) orig_client_oid로 live 주문 조회 → 원 ODNO/조직번호/미체결 잔량 스냅샷
//  2) lock 밖에서 KIS 취소 호출(네트워크)
//  3) 성공 시에만 lock 재획득 → 미체결 잔량을 '그 시점 confirmed_qty로 재계산'해 reserved 해제
//     (2)와 (3) 사이 WS 스레드의 on_fill이 confirmed_qty를 올릴 수 있으므로 재계산이 이중해제를 막는다.
ManagedOrder OrderRouter::cancel_route(const OrderSignal& sig)
{
    auto now = std::chrono::system_clock::now();
    ManagedOrder mo;
    mo.order_id     = next_id();
    mo.signal       = sig;
    mo.submitted_at = now;
    mo.updated_at   = now;
    mo.status       = OrderStatus::PENDING;
    ++total_count_;

    // 1) 원주문 스냅샷 (record()는 hist_mtx_를 재획득하므로 lock 스코프 밖에서만 호출)
    std::string ticker, kis_order_no, krx_orgno, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    // 취소가 빗나갔을 때 "원주문이 이미 체결됐을 수 있나"를 같은 락 안에서 답해 둔다.
    //  find_live_by_oid는 살아있는 주문만 보므로 !found는 세 경우를 뭉뚱그린다 —
    //  체결됨 / 이미 취소됨 / 애초에 접수된 적 없음(REJECTED·이력 없음).
    //  중복 매수 위험은 첫째에만 있다. [why D-033]
    bool orig_may_have_filled = false;
    const char* gone_why = "이력 없음(재기동·이력초과)";
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        ManagedOrder* orig = find_live_by_oid(sig.orig_client_oid);

        if (!orig && !sig.orig_client_oid.empty())
        {
            for (const auto& h : history_)
            {
                if (h.signal.client_oid != sig.orig_client_oid)
                {
                    continue;
                }

                if (h.status == OrderStatus::FILLED ||
                    (h.status == OrderStatus::ACCEPTED && h.confirmed_qty > 0))
                {
                    orig_may_have_filled = true;
                    gone_why             = "이미 체결";
                }
                else if (h.status == OrderStatus::CANCELLED)
                {
                    gone_why = "이미 취소";
                }
                else if (h.status == OrderStatus::REJECTED)
                {
                    gone_why = "접수된 적 없음(거부)";
                }

                break;
            }
        }

        if (orig)
        {
            found        = true;
            ticker       = orig->signal.ticker;
            kis_order_no = orig->kis_order_no;
            krx_orgno    = orig->krx_orgno;
            account      = orig->signal.account_id;
            side         = orig->signal.side;
            outstanding  = orig->signal.quantity - orig->confirmed_qty;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        // 취소할 것이 없는 것은 거부가 아니라 끝난 상태다 — 전략은 취소 결과를 안 보고 계획을
        //  다시 짜므로, 거부된 rung·이미 취소된 rung을 다시 취소하는 요청이 재구성마다 온다
        //  (09-10~11 이틀 323건, 그중 체결 흔적은 21건). REJECTED로 세면 거부 통계와 경고가
        //  실제 문제(게이트·KIS 거부)를 덮는다. CANCELLED로 닫고, 체결 가능성이 있는 경우만
        //  경고와 매수 보류를 남긴다. [why D-035]
        mo.status        = OrderStatus::CANCELLED;
        mo.reject_reason = std::string("취소 대상 없음 (") + gone_why + ") oid=" + sig.orig_client_oid;

        // 체결 흔적이 있을 때만 매수를 잠근다. 접수된 적 없는 oid(전략이 거부된 주문을
        //  live로 들고 있는 경우)에도 잠그면 매 재구성 주기마다 취소 빗나감 → 매수 거부 →
        //  거부된 oid가 다시 live로 → 다음 주기에 또 취소 빗나감으로 되돌아, 창이 계속
        //  갱신되며 그 종목 매수가 영구히 막힌다. 2026-09-10 006910이 이 모양으로
        //  보유 0인 채 57분간 한 주도 못 샀다(대체 주문 보류 176건). [why D-033]
        //  이미 무장돼 있으면 시각을 갱신하지 않는다 — 창은 연장되지 않는다.
        if (orig_may_have_filled)
        {
            std::lock_guard<std::mutex> lk(hist_mtx_);
            cancel_miss_.emplace(sig.ticker, std::chrono::steady_clock::now());
        }

        if (orig_may_have_filled)
        {
            LOG_WARN("[OrderRouter] 취소 무시 [" + mo.order_id + "] " + mo.reject_reason +
                     " — 체결 가능성 있어 신규매수 보류");
        }
        else
        {
            LOG_INFO("[OrderRouter] 취소 불요 [" + mo.order_id + "] " + mo.reject_reason);
        }

        record(mo);
        return mo;
    }

    // 2) KIS 취소 (lock 밖)
    OrderAck cxl;

    try
    {
        ++kis_calls_;
        cxl = kis_.cancel_order(ticker, kis_order_no, krx_orgno, outstanding, /*all_remaining=*/true);
    }
    catch (const std::exception& e)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = std::string("KIS 취소 예외: ") + e.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] 취소 예외 [" + mo.order_id + "] " + ticker + " — " + e.what());
        record(mo);
        return mo;
    }

    if (!cxl.ok())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_err_suffix(cxl);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 취소 거부 [" + mo.order_id + "] " + ticker +
                 " 원oid=" + sig.orig_client_oid);
        record(mo);
        return mo;
    }

    // 3) 성공 — reserved 해제(잔량 재계산) + 원주문 CANCELLED 표기 + 인덱스 정리
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        ManagedOrder* orig = find_live_by_oid(sig.orig_client_oid);
        int release = 0;

        if (orig)
        {
            release = orig->signal.quantity - orig->confirmed_qty; // 취소 성공 시점 실제 미체결

            if (release < 0)
            {
                release = 0;
            }

            orig->status     = OrderStatus::CANCELLED;
            orig->updated_at = std::chrono::system_clock::now();
        }

        oid_index_.erase(sig.orig_client_oid);

        // gate 뮤텍스는 hist_mtx_와 독립. 잠금 순서 hist_→positions_는 on_fill과 동일(데드락 없음).
        if (release > 0)
        {
            gate_.on_cancel(account, ticker, side, release);
        }
    }

    mo.status       = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    mo.kis_order_no = cxl.odno;
    mo.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + mo.order_id + "] " + ticker +
             " 원oid=" + sig.orig_client_oid + " 취소ODNO=" + cxl.odno);
    record(mo);
    return mo;
}

// ─── 정정 라우팅 (action=REPLACE) ─────────────────────────────────────────
//  KIS 정정 1콜 = cancel-replace. 성공 시 새 ODNO 발급.
//  reserved 조정: 원 미체결 잔량 해제 후 new_qty 재선점(같은 side). 원주문은 CANCELLED,
//  정정 결과를 새 ManagedOrder(ACCEPTED)로 추적(새 ODNO/새 client_oid).
//  ⚠ 첫 컷 한계: 부분체결 상태 정정은 수량 정합이 복잡 → MM은 REPLACE 미사용(CANCEL+NEW 사용).
//     본 경로는 미체결 전량 대상 정정만 안전. 부분체결분 정정은 Phase 2에서 정밀화.
ManagedOrder OrderRouter::replace_route(const OrderSignal& sig)
{
    auto now = std::chrono::system_clock::now();
    ManagedOrder mo;
    mo.order_id     = next_id();
    mo.signal       = sig;
    mo.submitted_at = now;
    mo.updated_at   = now;
    mo.status       = OrderStatus::PENDING;
    ++total_count_;

    std::string ticker, kis_order_no, krx_orgno, account;
    OrderSide side = OrderSide::NONE;
    int outstanding = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        ManagedOrder* orig = find_live_by_oid(sig.orig_client_oid);

        if (orig)
        {
            found        = true;
            ticker       = orig->signal.ticker;
            kis_order_no = orig->kis_order_no;
            krx_orgno    = orig->krx_orgno;
            account      = orig->signal.account_id;
            side         = orig->signal.side;
            outstanding  = orig->signal.quantity - orig->confirmed_qty;

            if (outstanding < 0)
            {
                outstanding = 0;
            }
        }
    }

    if (!found)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "정정 대상 없음 oid=" + sig.orig_client_oid;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 정정 무시 [" + mo.order_id + "] " + mo.reject_reason);
        record(mo);
        return mo;
    }

    int new_qty = (sig.quantity > 0) ? sig.quantity : outstanding;

    OrderAck rev;

    try
    {
        ++kis_calls_;
        rev = kis_.revise_order(ticker, kis_order_no, krx_orgno, new_qty, sig.price);
    }
    catch (const std::exception& e)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = std::string("KIS 정정 예외: ") + e.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] 정정 예외 [" + mo.order_id + "] " + ticker + " — " + e.what());
        record(mo);
        return mo;
    }

    if (!rev.ok())
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_err_suffix(rev);
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 정정 거부 [" + mo.order_id + "] " + ticker +
                 " 원oid=" + sig.orig_client_oid);
        record(mo);
        return mo;
    }

    // 성공 — 원 미체결 잔량 해제 후 new_qty 재선점, 원주문 CANCELLED, 정정본 ACCEPTED 추적
    {
        std::lock_guard<std::mutex> lk(hist_mtx_);
        ManagedOrder* orig = find_live_by_oid(sig.orig_client_oid);
        int release = outstanding;

        if (orig)
        {
            release = orig->signal.quantity - orig->confirmed_qty;

            if (release < 0)
            {
                release = 0;
            }

            orig->status     = OrderStatus::CANCELLED;
            orig->updated_at = std::chrono::system_clock::now();
        }

        oid_index_.erase(sig.orig_client_oid);

        if (release > 0)
        {
            gate_.on_cancel(account, ticker, side, release);
        }

        // 정정본 재선점 — 새 side는 원주문과 동일 (선점가는 지정가=price, 시장가=ref_price 근사)
        gate_.on_accept(account, ticker, side, new_qty, sig.price > 0.0 ? sig.price : sig.ref_price);

        if (!sig.client_oid.empty())
        {
            oid_index_[sig.client_oid] = mo.order_id;
        }
    }

    mo.status       = OrderStatus::ACCEPTED;
    mo.kis_order_no = rev.odno;
    mo.krx_orgno    = krx_orgno; // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    mo.signal.side  = side;      // NONE 방지: 원주문 side 승계
    mo.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + mo.order_id + "] " + ticker +
             " 원oid=" + sig.orig_client_oid + " 새ODNO=" + rev.odno +
             " qty=" + std::to_string(new_qty) + " @" + std::to_string((int)sig.price));
    record(mo);
    return mo;
}

// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
void OrderRouter::on_fill(const FillNotification& fn)
{
    // unique_lock: 원장 갱신까지만 잡고, 파일 쓰기·publish 전에 푼다(W-8).
    std::unique_lock<std::mutex> lk(hist_mtx_);
    // 중복 제거 — KIS 체결통보는 at-least-once(재전송/WS 재구독 시 중복 가능).
    // H0STCNI0 전문에 체결고유번호가 없어 odno+체결시각+수량+단가를 조합 키로 사용.
    // ODNO는 영업일 단위 재사용되고 fill_time은 HHMMSS(날짜 없음)라, 거래일(수신일)을
    // prefix로 붙여, 서로 다른 날의 동일키 충돌로 실체결을 오인해 drop하는 일을 막는다 (V-4).
    std::time_t tt = std::chrono::system_clock::to_time_t(fn.timestamp);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char dbuf[9];
    std::strftime(dbuf, sizeof(dbuf), "%Y%m%d", &lt);
    std::string fill_key = std::string(dbuf) + ":" + fn.odno + ":" + fn.fill_time + ":" +
                           std::to_string(fn.filled_qty) + ":" +
                           std::to_string(static_cast<long long>(fn.filled_price * 100));
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
        LOG_INFO("[OrderRouter] 동일키 분할체결 " + std::to_string(seen) + "회차 ODNO=" + fn.odno +
                 " time=" + fn.fill_time + " " + std::to_string(fn.filled_qty) + "주");
    }

    // 재기동 복원 — 이전 세션이 낸 주문이면 접수 때 남긴 사유 기록에서 되살린다.
    //  history_는 메모리라 재기동으로 비지만 기록 파일에는 ODNO·종목·수량·전략·사유가
    //  그대로 있다. 되살려 history_에 넣으면 아래 매칭 루프가 잔량 클램프까지 평소대로
    //  처리하므로, 전략 귀속을 잃는 미매핑 경로로 빠지지 않는다.
    if (!fn.odno.empty())
    {
        load_order_reasons_locked();   // 첫 체결통보 때 1회만 파일을 읽는다
        bool known = false;

        for (const auto& mo : history_)
        {
            if (mo.kis_order_no == fn.odno)
            {
                known = true;
                break;
            }
        }

        auto jit = known ? order_reasons_.end() : order_reasons_.find(fn.odno);

        if (jit != order_reasons_.end())
        {
            ManagedOrder rec;
            rec.order_id           = next_id();
            rec.kis_order_no       = fn.odno;
            rec.status             = OrderStatus::ACCEPTED;
            rec.confirmed_qty      = 0;
            rec.signal.ticker      = jit->second.ticker;
            rec.signal.side        = jit->second.side;
            rec.signal.type        = OrderType::LIMIT;
            rec.signal.quantity    = jit->second.quantity;
            rec.signal.price       = jit->second.price;
            rec.signal.ref_price   = jit->second.ref_price;
            rec.signal.strategy_id = jit->second.strategy_id;
            rec.signal.reason      = jit->second.reason;
            rec.submitted_at       = fn.timestamp;
            rec.updated_at         = fn.timestamp;
            history_.push_back(rec);
            // 선점(reserved_)은 이전 세션과 함께 사라졌다. 아래 체결 처리가
            //  on_fill_confirmed로 선점을 깎으므로, 주문수량만큼 먼저 되살려 순변화를 맞춘다.
            //  일부만 체결되고 나머지가 취소되면 그만큼 선점이 남는데, 주기 잔고 대조의
            //  reset_reserved()가 실제 잔고로 되맞춘다.
            gate_.on_accept(rec.signal.account_id, rec.signal.ticker, rec.signal.side,
                            rec.signal.quantity,
                            rec.signal.price > 0.0 ? rec.signal.price : rec.signal.ref_price);
            LOG_INFO("[OrderRouter] 재기동 복원 [" + rec.order_id + "] ODNO=" + fn.odno + " " +
                     rec.signal.ticker +
                     (rec.signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
                     std::to_string(rec.signal.quantity) + "주 전략=" + rec.signal.strategy_id +
                     " (주문 사유 기록에서 복구)");
            order_reasons_.erase(jit);   // 같은 ODNO를 두 번 되살리지 않는다
        }
    }


    // 이미 주문수량을 다 채운 주문의 추가 통보인지 구분한다. 이걸 아래 미매핑 경로로
    //  흘려보내면 같은 체결이 포지션에 두 번 쌓인다(ODNO는 아는데 잔량만 없는 상태).
    bool exhausted = false;

    for (auto& mo : history_)
    {
        if (mo.kis_order_no != fn.odno)
        {
            continue;
        }

        // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용
        if (mo.status != OrderStatus::ACCEPTED && mo.status != OrderStatus::FILLED)
        {
            continue;
        }

        // 이미 전량 체결 완료된 주문은 재처리 방지
        if (mo.confirmed_qty >= mo.signal.quantity)
        {
            exhausted = true;
            continue;
        }

        // 주문 잔량 상한 — 누적 체결이 주문수량을 넘지 못하게 클램프한다.
        //  통보 재전송으로 같은 체결이 두 번 와도 과체결로 원장이 부풀지 않는다.
        const int outstanding = mo.signal.quantity - mo.confirmed_qty;
        const int apply_qty   = (fn.filled_qty > outstanding) ? outstanding : fn.filled_qty;

        if (apply_qty < fn.filled_qty)
        {
            LOG_WARN("[OrderRouter] 주문잔량 초과 체결통보 — 잔량으로 클램프 [" + mo.order_id +
                     "] ODNO=" + fn.odno + " 통보=" + std::to_string(fn.filled_qty) +
                     "주 잔량=" + std::to_string(outstanding) + "주");
        }

        mo.confirmed_qty += apply_qty;
        mo.updated_at     = fn.timestamp;

        if (mo.confirmed_qty >= mo.signal.quantity)
        {
            mo.status = OrderStatus::FILLED;
        }

        LOG_INFO("[OrderRouter] 체결 확인 [" + mo.order_id + "] ODNO=" + fn.odno +
                 " " + fn.ticker +
                 (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(apply_qty) + "주 @" +
                 std::to_string(static_cast<int>(fn.filled_price)) +
                 " (누적 " + std::to_string(mo.confirmed_qty) +
                 "/" + std::to_string(mo.signal.quantity) + "주)");


        // 포지션 원장 갱신 (avg_price 재계산 + 실현손익) — 원주문의 계좌로 파티션.
        // 현재는 단일 CANO 전제라 ODNO가 유일 → mo.signal.account_id 매핑이 정확하다.
        // TODO(다계좌): 진짜 다중 CANO 라우팅 시 ODNO가 계좌별로 재사용되므로 체결 매칭 키를
        //   (odno + account) 또는 CANO별 H0STCNI 피드 분리로 확장해야 오적립을 막는다.
        auto result = gate_.on_fill_confirmed(mo.signal.account_id, fn.ticker, fn.side,
                                              apply_qty, fn.filled_price);

        // 락 밖에서 쓰려고 복사한다 — mo는 history_ 원소라 record()의 축출로 참조가 죽을 수 있다.
        const ManagedOrder snap        = mo;
        const std::string  open_orders = snapshot_open_orders_locked(); // 잔량이 줄었으니 부속 파일을 다시 쓴다
        const uint64_t     seq         = ++open_orders_seq_;
        lk.unlock();

        if (result.basis_unknown)
        {
            LOG_WARN("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [" + snap.order_id + "] " +
                     fn.ticker + " " + std::to_string(apply_qty) + "주 @" +
                     std::to_string(static_cast<int>(fn.filled_price)) + " (원장 재시드 필요)");
        }

        // 거래 원장 CSV — 실제 체결(부분/전량)을 한 줄로 영속화. 실현손익을 같이 남기려고
        //   gate_.on_fill_confirmed() 뒤에 쓴다(mo.status는 위에서 이미 갱신됨).
        write_trade_row("FILL", snap, apply_qty, fn.filled_price, result.realized_pnl);
        write_open_orders_file(open_orders, seq);
#ifdef HAS_ZMQ
        if (zmq_)
        {
            zmq_->publish_fill(fn, result.commission, result.tax,
                               result.avg_price, result.net_qty,
                               result.realized_pnl);
        }
#endif
        return;
    }

    if (exhausted)
    {
        LOG_WARN("[OrderRouter] 주문수량 충족 후 추가 체결통보 무시 ODNO=" + fn.odno +
                 " " + fn.ticker + " " + std::to_string(fn.filled_qty) + "주 (통보 재전송 추정)");
        return;
    }

    // ── ODNO 미매핑 체결 — 이 프로세스가 낸 주문이 아니다 ────────────────────
    //  history_는 메모리에만 있어서 장중 재시작하면 이전 세션의 미체결 주문이 사라진다.
    //  거래소 호가창에는 그 주문이 그대로 살아있으므로, 나중에 체결되면 여기로 떨어진다.
    //  2026-09-07 ODNO 0000014893이 이 경우다 — 09:58 접수, 10:38 재시작, 11:07 91주 전량
    //  체결이 통째로 버려져 원장·포지션이 91주(약 498만원) 어긋났다.
    //  체결 자체는 실재하므로 버리지 않고 원장·포지션에 반영한다. 전략 귀속만 알 수 없어
    //  strategy_id를 "ORPHAN"으로 남긴다(사후 분석에서 구분 가능).
    //  선점(reserved_)은 이전 세션과 함께 사라졌다. on_fill_confirmed는 선점 해제를 전제로
    //  reserved_를 깎으므로, 그대로 부르면 음수 선점이 생겨 이후 한도 계산이 왜곡된다.
    //  같은 수량을 on_accept로 먼저 되살린 뒤 해제시켜 순변화를 0으로 맞춘다.
    //  미연결은 history_에 넣지 않으므로(주문수량을 몰라 잔량 클램프가 없다) 같은 통보가 재전송되면
    //  또 여기로 떨어진다. 키(거래일:odno:시각:수량:단가)로 2회차부터 막는다 — 같은 초·같은
    //  수량·단가로 갈라진 미연결 분할체결은 잃지만, 두 번 쌓는 쪽이 더 큰 사고다(W-6).
    if (!orphan_fill_keys_.insert(fill_key).second)
    {
        LOG_WARN("[OrderRouter] 미매핑 체결 재통보 무시 ODNO=" + fn.odno + " " + fn.ticker + " " +
                 std::to_string(fn.filled_qty) + "주 time=" + fn.fill_time + " (같은 키 재수신)");
        return;
    }

    ManagedOrder orphan;
    orphan.order_id           = next_id();
    orphan.kis_order_no       = fn.odno;
    orphan.status             = OrderStatus::FILLED;
    orphan.confirmed_qty      = fn.filled_qty;
    orphan.signal.strategy_id = "ORPHAN";
    orphan.signal.ticker      = fn.ticker;
    orphan.signal.side        = fn.side;
    orphan.signal.type        = OrderType::LIMIT;
    orphan.signal.quantity    = fn.filled_qty;
    orphan.signal.price       = fn.filled_price;
    orphan.signal.reason      = "이전 세션 주문 체결(ODNO 미매핑)";
    orphan.submitted_at       = fn.timestamp;
    orphan.updated_at         = fn.timestamp;

    LOG_WARN("[OrderRouter] 미매핑 체결 원장 반영 [" + orphan.order_id + "] ODNO=" + fn.odno +
             " " + fn.ticker + (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(fn.filled_qty) + "주 @" +
             std::to_string(static_cast<int>(fn.filled_price)) +
             " — 이전 세션 주문으로 추정(재시작 전 접수분)");

    gate_.on_accept(orphan.signal.account_id, fn.ticker, fn.side,
                    fn.filled_qty, fn.filled_price);
    auto result = gate_.on_fill_confirmed(orphan.signal.account_id, fn.ticker, fn.side,
                                          fn.filled_qty, fn.filled_price);
    lk.unlock(); // 원장 갱신 끝 — 파일 쓰기는 락 밖에서

    if (result.basis_unknown)
    {
        LOG_WARN("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [" + orphan.order_id + "] " +
                 fn.ticker + " " + std::to_string(fn.filled_qty) + "주 @" +
                 std::to_string(static_cast<int>(fn.filled_price)) + " (원장 재시드 필요)");
    }

    write_trade_row("FILL", orphan, fn.filled_qty, fn.filled_price, result.realized_pnl);
#ifdef HAS_ZMQ
    if (zmq_)
    {
        zmq_->publish_fill(fn, result.commission, result.tax,
                           result.avg_price, result.net_qty,
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
    std::lock_guard<std::mutex> lk(hist_mtx_);
    seen_fills_.clear();
    orphan_fill_keys_.clear();
    oid_index_.clear(); // MM-1: client_oid 인덱스도 장 마감 정리 (당일 주문 장 마감 소멸과 정합)
    // 사유 기록도 거래일이 바뀌면 다시 읽는다(파일이 날짜별이라 어제 것을 들고 있으면 안 된다).
    order_reasons_.clear();
    order_reasons_loaded_ = false;
}

// ─── 통계 ─────────────────────────────────────────────────────────────────
OrderRouter::Stats OrderRouter::stats() const
{
    return {total_count_.load(), accepted_count_.load(), rejected_count_.load()};
}

// ─── 최근 N건 이력 ────────────────────────────────────────────────────────
std::vector<ManagedOrder> OrderRouter::recent(int n) const
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    int start = std::max(0, static_cast<int>(history_.size()) - n);
    return {history_.begin() + start, history_.end()};
}
