// 주문 라우터의 부속 파일 쓰기 — 미결주문 스냅샷, 거래 원장 CSV, 주문 사유. 선언부 주석은 Quant/include/ipc/OrderJournal.h.
#include "ipc/OrderJournal.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <iterator>
#include <string_view>
#include <vector>

void stop_join(std::jthread& thread)
{
    thread.request_stop();

    if (thread.joinable())
    {
        thread.join();
    }
}

OrderJournal::OrderJournal()
    : open_orders_writer_([this](std::stop_token stop_token)
                          {
                              open_orders_writer_loop(stop_token);
                          }),
      append_writer_([this](std::stop_token stop_token)
                     {
                         append_writer_loop(stop_token);
                     })
{
}

OrderJournal::~OrderJournal()
{
    stop();
}

void OrderJournal::stop()
{
    stop_join(open_orders_writer_);
    flush_open_orders_file(); // 스레드가 멈춘 뒤 남은 것이 있으면 여기서 쓴다
    stop_join(append_writer_);
    flush_append_outbox(); // 줄 서 있던 원장 행·사유 줄을 마저 쓴다
}

// ─── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
//  형식: odno|ticker|side|수량|가격|기준가|전략|사유   (한 줄 한 주문, 헤더 없음)
//  접수된 신규·정정 주문만 남긴다. 취소는 체결되지 않으므로 대상이 아니다.
void OrderJournal::append_order_reason(const ManagedOrder& managed_order)
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

    PendingLine pending;
    pending.sink = PendingLine::Sink::REASON;
    pending.date = kst::date_yyyymmdd(std::time(nullptr));
    pending.text = line;   // 줄바꿈이 이미 붙어 있다
    queue_append_line(std::move(pending));
}

void OrderJournal::write_open_orders_file(const std::string& body, uint64_t sequence)
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

// ─── 부속 파일 쓰기 넘기기 ────────────────────────────────────────────────
void OrderJournal::queue_open_orders_file(std::string body, uint64_t sequence)
{
    {
        std::lock_guard<std::mutex> lock(open_orders_outbox_mutex_);

        if (sequence <= open_orders_pending_sequence_)
        {
            return; // 더 새 스냅샷이 이미 줄 서 있다 — 옛 것으로 덮으면 살아있는 주문이 사라진다
        }

        open_orders_pending_body_     = std::move(body);
        open_orders_pending_sequence_ = sequence;
    }

    open_orders_outbox_signal_.notify_one();
}

void OrderJournal::flush_open_orders_file()
{
    std::string body;
    uint64_t    sequence = 0;
    {
        std::lock_guard<std::mutex> lock(open_orders_outbox_mutex_);

        if (open_orders_pending_sequence_ == 0)
        {
            return;
        }

        body     = std::move(open_orders_pending_body_);
        sequence = open_orders_pending_sequence_;
        open_orders_pending_body_.clear();
        open_orders_pending_sequence_ = 0;
    }

    write_open_orders_file(body, sequence);
}

void OrderJournal::open_orders_writer_loop(std::stop_token stop_token)
{
    while (!stop_token.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(open_orders_outbox_mutex_);
            open_orders_outbox_signal_.wait(lock, stop_token,
                                            [this]
                                            {
                                                return open_orders_pending_sequence_ != 0;
                                            });
        }

        flush_open_orders_file();
    }

    flush_open_orders_file(); // 멈추라는 말을 듣고도 마지막 스냅샷은 디스크에 남긴다
}

// ─── 원장 CSV·사유 덧붙이기 넘기기 ────────────────────────────────────────
void OrderJournal::queue_append_line(PendingLine line)
{
    bool backed_up = false;
    {
        std::lock_guard<std::mutex> lock(append_outbox_mutex_);
        append_outbox_.push_back(std::move(line));
        backed_up = append_outbox_.size() >= kAppendOutboxLimit;
    }

    append_outbox_signal_.notify_one();

    if (backed_up)
    {
        // 디스크가 못 따라간다. 여기서 기다리는 것은 고치기 전과 같은 상태지만, 큐가 메모리를
        //  끝없이 먹는 것보다 낫다. 순서는 flush_append_outbox가 io_mutex_ 안에서 지킨다.
        flush_append_outbox();
    }
}

void OrderJournal::flush_append_outbox()
{
    // io_mutex_를 먼저 잡고 그 안에서 꺼낸다 — 쓰기 스레드와 부른 쪽이 같이 비우더라도
    //  꺼낸 순서와 쓴 순서가 어긋나지 않는다. [lock-order] io_mutex_ → append_outbox_mutex_
    std::lock_guard<std::mutex> io_lock(io_mutex_);

    while (true)
    {
        std::deque<PendingLine> batch;
        {
            std::lock_guard<std::mutex> lock(append_outbox_mutex_);

            if (append_outbox_.empty())
            {
                return;
            }

            batch.swap(append_outbox_);
        }

        write_pending_lines_locked(batch);   // 쓰는 동안 들어온 줄은 다음 바퀴가 가져간다
    }
}

void OrderJournal::write_pending_lines_locked(const std::deque<PendingLine>& batch)
{
    bool wrote_trade  = false;
    bool wrote_reason = false;

    for (const PendingLine& pending : batch)
    {
        if (pending.sink == PendingLine::Sink::TRADE)
        {
            if (pending.date != trade_file_date_ || !trade_file_.is_open())
            {
                open_trade_file_locked(pending.date);
            }

            if (!trade_file_.is_open())
            {
                continue; // best-effort — 원장 정본은 OrderGate 저널이다(D-113)
            }

            trade_file_ << pending.text << '\n';
            wrote_trade = true;
        }
        else
        {
            if (pending.date != order_reason_file_date_ || !order_reason_file_.is_open())
            {
                open_order_reason_file_locked(pending.date);
            }

            if (!order_reason_file_)
            {
                continue;
            }

            order_reason_file_ << pending.text;
            wrote_reason = true;
        }
    }

    // flush는 묶음당 한 번이다 — 줄마다 하던 것을 줄여 쓰기 스레드가 큐에 밀리지 않게 한다.
    if (wrote_trade)
    {
        trade_file_.flush();
    }

    if (wrote_reason)
    {
        order_reason_file_.flush();
    }
}

void OrderJournal::open_order_reason_file_locked(const std::string& date)
{
    order_reason_file_.close();
    order_reason_file_.clear();
    order_reason_file_.open(Logger::instance().path_for("order_reasons_" + date + ".txt"), std::ios::app);
    order_reason_file_date_ = date;
}

void OrderJournal::append_writer_loop(std::stop_token stop_token)
{
    while (!stop_token.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(append_outbox_mutex_);
            append_outbox_signal_.wait(lock, stop_token, [this]
            {
                return !append_outbox_.empty();
            });
        }

        flush_append_outbox();
    }

    flush_append_outbox(); // 멈추라는 말을 듣고도 줄 서 있던 것은 디스크에 남긴다
}

// ─── 거래 원장 CSV 적재 ───────────────────────────────────────────────────
//  실행 로그(quant_trader.log)와 별개로 매수·매도·거부·체결·잔고 대조를 구조적으로 남긴다.
//  logs/trades_YYYYMMDD.csv 에 한 줄씩 append(날짜별 파일). record()·on_fill()·record_reconcile()
//  이 줄을 만들어 줄 대기열에 넣고, 쓰기 스레드 하나가 파일에 쓴다(동시쓰기 없음). [why D-124]
//  원장 쓰기 실패는 매매를 막지 않는다(best-effort — 조용히 반환).
//  열 정본은 kTradeHeader 하나다. 열을 더할 때는 끝에 붙인다 — 스키마 승격이 옛 파일 행 끝에 빈 칸을
//  덧붙이는 방식이라 중간 삽입은 기존 행의 값을 엉뚱한 열로 밀어낸다. Python 판독기는 열 이름으로 읽는다.
constexpr std::string_view kTradeHeader =
    "ts_kst,event,order_id,odno,strategy,ticker,side,type,"
    "order_qty,order_price,fill_qty,fill_price,status,reason,entry_reason,realized_pnl,seq,"
    "strategy_realized_pnl";

// CSV 깨짐 방지: 콤마/개행 공백 치환
std::string OrderJournal::csv_safe(std::string text)
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

void OrderJournal::trade_row_timestamp(std::string& date, std::string& stamp)
{
    const std::time_t now_time = std::time(nullptr);
    date  = kst::date_yyyymmdd(now_time);
    stamp = kst::datetime(now_time);
}

void OrderJournal::open_trade_file_locked(const std::string& date)
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

void OrderJournal::append_trade_line(const std::string& line)
{
    // 시각은 지금 박는다 — 쓰기 스레드가 언제 쓰든 행의 시각은 주문 스레드가 지나간 그 순간이어야 한다.
    std::string date, time_buffer;
    trade_row_timestamp(date, time_buffer);

    PendingLine pending;
    pending.sink = PendingLine::Sink::TRADE;
    pending.date = std::move(date);
    pending.text = time_buffer + ',' + line;
    queue_append_line(std::move(pending));
}

void OrderJournal::write_trade_row(const std::string& event, const ManagedOrder& managed_order,
                                  int fill_quantity, double fill_price, double realized_pnl,
                                  double strategy_realized_pnl)
{
    const OrderSignal& signal = managed_order.signal;

    auto side_string = [](OrderSide order_side) {
        return order_side == OrderSide::BUY ? "BUY" : (order_side == OrderSide::SELL ? "SELL" : "NONE");
    };
    auto type_string = [](OrderType order_type)
    {
        return order_type == OrderType::LIMIT ? "LIMIT" : "MARKET";
    };
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

