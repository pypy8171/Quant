#include "ipc/OrderRouter.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    uint64_t n = ++seq_;
    std::ostringstream ss;
    ss << "ORD-" << std::setfill('0') << std::setw(6) << n;
    return ss.str();
}

// ─── 거부 사유에 KIS 오류코드 꼬리표 부착 ───────────────────────────────
//  order_thread가 EGW00201(초당 거래건수 초과)을 문자열로 판별해 적응적 재시도를 걸 수 있게,
//  KIS가 준 msg_cd를 " [코드]" 형태로 reject_reason 끝에 붙인다. 코드 없으면 빈 문자열.
std::string OrderRouter::kis_err_suffix() const
{
    std::string ec = kis_.last_order_error_code();
    return ec.empty() ? std::string() : (" [" + ec + "]");
}

// ─── 주문 제출 — action에 따라 라우팅 (MM-1) ─────────────────────────────
//  전 경로가 단일 order_thread에서만 실행된다(Engine::order_thread_fn) — OrderGate C6의
//  단일생산자·단일소비자(SPSC) 불변 보존. 전략 스레드는 여기 진입하지 않는다.
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
ManagedOrder OrderRouter::new_route(const OrderSignal& sig)
{
    auto now = std::chrono::system_clock::now();

    ManagedOrder mo;
    mo.order_id    = next_id();
    mo.signal      = sig;
    mo.submitted_at = now;
    mo.updated_at   = now;
    mo.status       = OrderStatus::PENDING;

    ++total_count_;

    // 1. OrderGate 검증
    std::string reject_reason;
    if (!gate_.check(sig, reject_reason))
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = reject_reason;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 거부 [" + mo.order_id + "] " +
                 sig.ticker + " → " + reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, false);
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
            zmq_->publish_order(sig, false);
#endif
        record(mo);
        return mo;
    }

    mo.updated_at = std::chrono::system_clock::now();

    // 청산차단 자가정리 — SELL이 "주문가능분 없음"(40240000)으로 막히면, 그 종목의
    //  미체결 예약매도(이전 세션/수동 예약이 보유수량을 묶은 것)를 조회·취소하고 시장가로 1회
    //  재시도한다. 성공하면 아래 접수 블록이 그대로 처리(odno/ack가 재시도 결과로 갱신됨).
    if (odno.empty() && sig.side == OrderSide::SELL &&
        kis_.last_order_error_code() == kis_err::kNoSellableQty)
    {
        OrderAck rack = reconcile_blocked_sell(sig);
        if (!rack.odno.empty())
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
            zmq_->publish_order(sig, true);
#endif
    }
    else
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS API 거부 (빈 ODNO)" + kis_err_suffix();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 거부 [" + mo.order_id + "] " + sig.ticker + mo.reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, false);
#endif
    }

    record(mo);
    return mo;
}

// ─── 청산차단 자가정리 — 예약매도 취소 후 시장가 재매도 (장중) ─────────────
//  전제: SELL이 40240000(주문가능분 없음)으로 막힌 직후 호출. 그 종목의 미체결 예약매도가
//  보유수량을 묶어 ord_psbl_qty=0이 된 상황을 KIS 미체결 조회로 규명하고, 예약을 취소해
//  수량을 풀어준 뒤 시장가 매도를 1회 재시도한다. 취소 대상은 이전 세션/수동 예약일 수 있어
//  내부 reserved_(이번 세션 것)엔 없으므로 gate_는 건드리지 않는다(포지션 정합은 체결통보로).
OrderAck OrderRouter::reconcile_blocked_sell(const OrderSignal& sig)
{
    // 모의투자는 정정취소가능조회(inquire-psbl-rvsecncl) TR을 미지원("없는 서비스 코드") →
    //  예약매도를 조회·취소할 방법이 없어 이 자가정리는 구조적으로 불가. 헛도는 실패 조회와
    //  오해 소지 로그("수동 확인 필요")를 피하려 정직하게 단락한다. 실계좌에선 정상 동작.
    //  (애초에 익절·청산 매도를 매도가능분으로 클램프하므로 40240000 자체가 거의 안 난다.)
    if (kis_.is_paper())
    {
        LOG_WARN("[OrderRouter] 청산차단 자가정리 스킵 " + sig.ticker +
                 " — 모의투자는 미체결조회 미지원(실계좌 전용 경로)");
        return OrderAck{};
    }

    std::vector<OpenOrder> opens;
    try
    {
        opens = kis_.get_open_orders();
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[OrderRouter] 미체결 조회 예외 — " + std::string(e.what()));
        return OrderAck{};
    }

    int cancelled = 0;
    for (const auto& o : opens)
    {
        if (o.ticker != sig.ticker || o.side != OrderSide::SELL)
            continue; // 해당 종목의 예약'매도'만 대상
        LOG_WARN("[OrderRouter] 청산차단 해소 " + sig.ticker + " 예약매도 " +
                 std::to_string(o.psbl_qty) + "주 ODNO=" + o.odno + " @" +
                 std::to_string(static_cast<int>(o.ord_unpr)) + " → 취소 시도");
        std::string cxl;
        try
        {
            cxl = kis_.cancel_order(o.ticker, o.odno, o.krx_orgno, o.psbl_qty, /*all_remaining=*/true);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[OrderRouter] 예약취소 예외 " + sig.ticker + " — " + std::string(e.what()));
            continue;
        }
        if (!cxl.empty())
            ++cancelled;
    }

    if (cancelled == 0)
    {
        LOG_WARN("[OrderRouter] 청산차단 미해소 " + sig.ticker +
                 " — 취소할 예약매도 없음/취소 실패 (수동 확인 필요)");
        return OrderAck{};
    }

    LOG_INFO("[OrderRouter] 예약매도 " + std::to_string(cancelled) + "건 취소 완료 → " +
             sig.ticker + " 시장가 매도 재시도");
    try
    {
        return kis_.submit_order_ack(sig);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[OrderRouter] 청산 재매도 예외 " + sig.ticker + " — " + std::string(e.what()));
        return OrderAck{};
    }
}

// ─── 이력 저장 (max_history 초과 시 체결 완료/거부된 것만 삭제) ───────────
void OrderRouter::record(const ManagedOrder& mo)
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    history_.push_back(mo);
    // 거래 원장 CSV — 주문 종착 상태(접수/거부/취소)를 한 줄로 영속화.
    //   event="" → mo.status 문자열(ACCEPTED/REJECTED/CANCELLED)이 event가 된다.
    write_trade_row("", mo, 0, 0.0);
    while (static_cast<int>(history_.size()) > cfg_.max_history)
    {
        // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
        if (history_.front().status == OrderStatus::ACCEPTED)
            break;
        history_.pop_front();
    }
}

// ─── 거래 원장 CSV 적재 ───────────────────────────────────────────────────
//  실행 로그(quant_trader.log)와 별개로 매수·매도·거부·체결을 구조적으로 남긴다.
//  logs/trades_YYYYMMDD.csv 에 한 줄씩 append(날짜별 파일). record()·on_fill()에서만
//  호출되며 두 경로 모두 hist_mtx_ 보유 상태라 파일 쓰기가 직렬화된다(동시쓰기 없음).
//  원장 쓰기 실패는 매매를 막지 않는다(best-effort — 조용히 반환).
void OrderRouter::write_trade_row(const std::string& event, const ManagedOrder& mo,
                                  int fill_qty, double fill_price)
{
    const OrderSignal& sig = mo.signal;

    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char dbuf[9], tbuf[20];
    std::strftime(dbuf, sizeof(dbuf), "%Y%m%d", &lt);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);

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

    namespace fs = std::filesystem;
    std::error_code ec;
    // 실행 위치(cwd)와 무관하게 로그 폴더(main에서 고정)에 매매원장 append.
    fs::path path = Logger::instance().path_for(std::string("trades_") + dbuf + ".csv");

    bool need_header = !fs::exists(path, ec);
    std::ofstream f(path, std::ios::app);
    if (!f.is_open())
        return; // best-effort

    if (need_header)
        f << "ts_kst,event,order_id,odno,strategy,ticker,side,type,"
             "order_qty,order_price,fill_qty,fill_price,status,reason,entry_reason\n";

    // event 빈 문자열이면 상태 문자열을 사용
    std::string ev = event.empty() ? status_str(mo.status) : event;
    // CSV 깨짐 방지: 콤마/개행 공백 치환
    auto csv_safe = [](std::string s)
    {
        for (char& c : s)
            if (c == ',' || c == '\n' || c == '\r')
                c = ' ';
        return s;
    };
    // reason = 거부/봉쇄 사유(OrderGate·KIS), entry_reason = 진입 판단 근거(전략, G4) — 분리 컬럼.
    std::string reason       = csv_safe(mo.reject_reason);
    std::string entry_reason = csv_safe(sig.reason);

    f << tbuf << ','
      << ev << ','
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
      << entry_reason << '\n';
}

// ─── client_oid로 살아있는 주문 조회 (호출자가 hist_mtx_ 보유) ────────────
//  live = ACCEPTED 이면서 미체결 잔량이 남은 주문(부분체결도 status는 ACCEPTED 유지).
//  FILLED/CANCELLED/REJECTED는 취소·정정 대상 아님.
ManagedOrder* OrderRouter::find_live_by_oid(const std::string& client_oid)
{
    if (client_oid.empty())
        return nullptr;
    for (auto& mo : history_)
    {
        if (mo.signal.client_oid != client_oid)
            continue;
        if (mo.status == OrderStatus::ACCEPTED && mo.confirmed_qty < mo.signal.quantity)
            return &mo;
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
            if (outstanding < 0) outstanding = 0;
        }
    }
    if (!found)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "취소 대상 없음 (이미 체결/취소/이력초과) oid=" + sig.orig_client_oid;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 취소 무시 [" + mo.order_id + "] " + mo.reject_reason);
        record(mo);
        return mo;
    }

    // 2) KIS 취소 (lock 밖)
    std::string cancel_odno;
    try
    {
        cancel_odno = kis_.cancel_order(ticker, kis_order_no, krx_orgno, outstanding, /*all_remaining=*/true);
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

    if (cancel_odno.empty())
    {
        // KIS 거부(이미 체결/취소 등) → reserved 미변경. 체결이 먼저면 체결 경로가 이미 해제함.
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS 취소 거부(원주문 이미 체결/소멸 가능)" + kis_err_suffix();
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
            if (release < 0) release = 0;
            orig->status     = OrderStatus::CANCELLED;
            orig->updated_at = std::chrono::system_clock::now();
        }
        oid_index_.erase(sig.orig_client_oid);
        // gate 뮤텍스는 hist_mtx_와 독립. 잠금 순서 hist_→positions_는 on_fill과 동일(데드락 없음).
        if (release > 0)
            gate_.on_cancel(account, ticker, side, release);
    }

    mo.status       = OrderStatus::CANCELLED; // 취소 요청 자체는 성공 접수
    mo.kis_order_no = cancel_odno;
    mo.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 취소 접수 [" + mo.order_id + "] " + ticker +
             " 원oid=" + sig.orig_client_oid + " 취소ODNO=" + cancel_odno);
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
            if (outstanding < 0) outstanding = 0;
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

    std::string new_odno;
    try
    {
        new_odno = kis_.revise_order(ticker, kis_order_no, krx_orgno, new_qty, sig.price);
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

    if (new_odno.empty())
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS 정정 거부(원주문 이미 체결/소멸 가능)" + kis_err_suffix();
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
            if (release < 0) release = 0;
            orig->status     = OrderStatus::CANCELLED;
            orig->updated_at = std::chrono::system_clock::now();
        }
        oid_index_.erase(sig.orig_client_oid);
        if (release > 0)
            gate_.on_cancel(account, ticker, side, release);
        // 정정본 재선점 — 새 side는 원주문과 동일 (선점가는 지정가=price, 시장가=ref_price 근사)
        gate_.on_accept(account, ticker, side, new_qty, sig.price > 0.0 ? sig.price : sig.ref_price);
        if (!sig.client_oid.empty())
            oid_index_[sig.client_oid] = mo.order_id;
    }

    mo.status       = OrderStatus::ACCEPTED;
    mo.kis_order_no = new_odno;
    mo.krx_orgno    = krx_orgno; // 정정 응답의 조직번호를 미파싱해 원 조직번호를 승계(통상 동일). TODO: 응답서 재캡처
    mo.signal.side  = side;      // NONE 방지: 원주문 side 승계
    mo.updated_at   = std::chrono::system_clock::now();
    ++accepted_count_;
    LOG_INFO("[OrderRouter] 정정 접수 [" + mo.order_id + "] " + ticker +
             " 원oid=" + sig.orig_client_oid + " 새ODNO=" + new_odno +
             " qty=" + std::to_string(new_qty) + " @" + std::to_string((int)sig.price));
    record(mo);
    return mo;
}

// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
void OrderRouter::on_fill(const FillNotification& fn)
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
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
    if (!seen_fills_.insert(fill_key).second)
    {
        LOG_WARN("[OrderRouter] 중복 체결통보 무시 ODNO=" + fn.odno + " time=" + fn.fill_time);
        return;
    }
    for (auto& mo : history_)
    {
        if (mo.kis_order_no != fn.odno)
            continue;
        // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용
        if (mo.status != OrderStatus::ACCEPTED && mo.status != OrderStatus::FILLED)
            continue;
        // 이미 전량 체결 완료된 주문은 재처리 방지
        if (mo.confirmed_qty >= mo.signal.quantity)
            continue;

        mo.confirmed_qty += fn.filled_qty;
        mo.updated_at     = fn.timestamp;
        if (mo.confirmed_qty >= mo.signal.quantity)
            mo.status = OrderStatus::FILLED;

        LOG_INFO("[OrderRouter] 체결 확인 [" + mo.order_id + "] ODNO=" + fn.odno +
                 " " + fn.ticker +
                 (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(fn.filled_qty) + "주 @" +
                 std::to_string(static_cast<int>(fn.filled_price)) +
                 " (누적 " + std::to_string(mo.confirmed_qty) +
                 "/" + std::to_string(mo.signal.quantity) + "주)");

        // 거래 원장 CSV — 실제 체결(부분/전량)을 한 줄로 영속화. mo.status는 여기서
        //   이미 갱신됨(전량이면 FILLED). 이 체결 건의 수량/단가를 fill_qty/price로 기록.
        write_trade_row("FILL", mo, fn.filled_qty, fn.filled_price);

        // 포지션 원장 갱신 (avg_price 재계산 + 실현손익) — 원주문의 계좌로 파티션.
        // 현재는 단일 CANO 전제라 ODNO가 유일 → mo.signal.account_id 매핑이 정확하다.
        // TODO(다계좌): 진짜 다중 CANO 라우팅 시 ODNO가 계좌별로 재사용되므로 체결 매칭 키를
        //   (odno + account) 또는 CANO별 H0STCNI 피드 분리로 확장해야 오적립을 막는다.
        auto result = gate_.on_fill_confirmed(mo.signal.account_id, fn.ticker, fn.side,
                                              fn.filled_qty, fn.filled_price);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_fill(fn, result.commission, result.tax,
                               result.avg_price, result.net_qty,
                               result.realized_pnl);
#endif
        return;
    }
    LOG_WARN("[OrderRouter] 체결통보 매핑 실패 ODNO=" + fn.odno +
             " (이미 처리됐거나 이력 범위 초과)");
}

// ─── 일별 리셋 (장 시작 시 Engine이 호출) ─────────────────────────────────
// 중복방지 키(seen_fills_)의 무한 증가를 해소. 거래일 prefix로 cross-day 충돌은 이미
// 차단되므로, 전일 키는 더 이상 필요 없다.
void OrderRouter::reset_daily()
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    seen_fills_.clear();
    oid_index_.clear(); // MM-1: client_oid 인덱스도 EOD 정리 (당일 주문 EOD 소멸과 정합)
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
