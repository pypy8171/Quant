#pragma once
#include "core/Types.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

// 스레드에 멈춤을 요청하고 끝날 때까지 기다린다. 이미 끝났거나 띄운 적 없으면 기다리지 않는다.
//  라우터·기록기 소멸자가 스레드를 정해진 순서로 세울 때 쓴다.
void stop_join(std::jthread& thread);

// ─────────────────────────────────────────────────────────────────────────────
// OrderJournal  —  주문 라우터의 부속 파일 쓰기 (D-123·D-124)
//
//  세 파일을 주문 스레드 대신 전담 스레드 둘이 쓴다.
//   - logs/open_orders.txt        : 살아 있는 주문 스냅샷. 한 칸짜리 대기함, 최신이 이긴다(쓰기 스레드 1).
//   - logs/trades_YYYYMMDD.csv    : 거래 원장 CSV. 줄을 세우는 큐(쓰기 스레드 2).
//   - logs/order_reasons_YYYYMMDD.txt : 접수된 주문의 사유. 원장 CSV와 같은 큐.
//  스냅샷 본문과 번호는 라우터가 history_mutex_ 아래에서 떠서 넘긴다 — 이 클래스는 이력을 모른다.
//
//  [lock-order] 라우터 history_mutex_ → io_mutex_ → append_outbox_mutex_. 라우터 on_fill은 세션 첫 체결 때
//   history_mutex_를 쥔 채 flush_append_outbox를 부른다. 이 클래스 안에서 io_mutex_를 쥔 채 라우터 락을 잡는 곳은 없다.
// ─────────────────────────────────────────────────────────────────────────────
class OrderJournal
{
public:
    // 두 쓰기 스레드를 바로 띄운다.
    OrderJournal();
    // stop()과 같다 — 라우터가 먼저 불렀으면 할 일이 없다.
    ~OrderJournal();
    // 스레드·뮤텍스·파일 핸들을 소유한다 — 복사는 같은 자원을 두 번 닫는 길이라 막는다.
    OrderJournal(const OrderJournal&)            = delete;
    OrderJournal& operator=(const OrderJournal&) = delete;

    // 미결주문 쓰기 스레드를 세우고 남은 스냅샷을 쓴 뒤, 원장·사유 쓰기 스레드를 세우고 남은 줄을 쓴다.
    //  라우터 소멸자가 자기 스레드(되묻기·기동 취소·체결 복구)를 먼저 세운 다음 부른다. 두 번 불러도 된다.
    void stop();

    // 부속 파일 쓰기를 전담 스레드에 넘기고 곧바로 돌아온다. 대기함은 한 칸이고 최신이 이긴다 —
    //  중간 스냅샷을 읽는 쪽이 없어서다(다음 기동이 보는 것은 마지막 하나뿐). 주문 스레드가
    //  여기서 디스크를 기다리면 시퀀서 전체가 같이 선다. sequence가 이미 줄 선 것보다 오래됐으면 버린다. [why D-123]
    void queue_open_orders_file(std::string body, uint64_t sequence);

    // 접수된 주문 한 건의 사유 줄을 덧붙이기 큐에 넣는다. 파일은 쓰기 스레드가 상주 핸들 order_reason_file_로
    //  쓰고, 날짜가 바뀌면 다시 연다. 접수된 신규·정정만 적고 나머지는 그냥 돌아온다. [why D-094] [why D-124]
    //  형식: odno|ticker|side|수량|가격|기준가|전략|사유   (한 줄 한 주문, 헤더 없음)
    void append_order_reason(const ManagedOrder& managed_order);

    // 거래 원장 CSV 적재 — 주문/체결을 logs/trades_YYYYMMDD.csv 에 한 줄씩 영속화.
    //   event가 빈 문자열이면 managed_order.status를 event로 사용(접수/거부/취소). 체결은 "FILL".
    //   줄을 만들어 덧붙이기 큐에 넣을 뿐 파일은 쓰기 스레드가 쓴다(history_mutex_ 밖에서 호출). [why D-124]
    //   realized_pnl은 매도 체결의 실현손익(수수료·세금 차감 후). 그 외 행은 빈 칸으로 남긴다.
    //   strategy_realized_pnl은 같은 매도 체결의 strategy_id 기준 실현손익(D-089, 열 맨 끝 추가분).
    void write_trade_row(const std::string& event, const ManagedOrder& managed_order, int fill_quantity, double fill_price,
                         double realized_pnl = 0.0, double strategy_realized_pnl = 0.0);
    // 원장 CSV에 덧붙일 한 줄을 줄 세우는 큐에 놓고 곧바로 돌아온다(디스크는 전담 스레드가 기다린다).
    //  시각 열은 여기서 박는다 — 쓰기 스레드가 언제 쓰든 행의 시각은 주문 스레드가 지나간 그 순간이다.
    //  write_trade_row·라우터 record_reconcile이 줄을 만들어 여기로 보낸다. [why D-094] [why D-124]
    void append_trade_line(const std::string& line);

    // 큐를 끝까지 비운다. io_mutex_를 먼저 잡고 그 안에서 꺼낸다 — 두 스레드가 같이 비워도
    //  꺼낸 순서와 쓴 순서가 어긋나지 않는다. 라우터 on_fill은 history_mutex_를 쥔 채 부른다([lock-order] 위).
    void flush_append_outbox();

    // CSV 깨짐 방지 — 콤마·개행을 공백으로.
    static std::string csv_safe(std::string text);

private:
    // ── 미결주문 파일 ───────────────────────────────────────────────────────
    // 부속 파일 덮어쓰기(io_mutex_). sequence가 이미 쓴 것보다 오래됐으면 건너뛴다 —
    //  락 밖에서 쓰므로 스냅샷 순서와 쓰기 순서가 뒤집힐 수 있다. 실패는 매매를 막지 않는다.
    void write_open_orders_file(const std::string& body, uint64_t sequence);
    // 대기 중인 스냅샷을 부르는 스레드에서 끝까지 쓴다. 쓸 것이 없으면 아무것도 안 한다.
    void flush_open_orders_file();
    // 쓰기 스레드 본문 — 대기함에 뭔가 들어올 때까지 자고, 깨면 비운다. 멈춤 요청 뒤 한 번 더 비운다.
    void open_orders_writer_loop(std::stop_token stop_token);

    // ── 덧붙이기 큐(원장 CSV·사유) ────────────────────────────────────────
    //  미결주문 파일과 달리 이 둘은 중간 것도 다 남아야 한다. 그래서 한 칸짜리 대기함이 아니라
    //  줄을 세우는 큐이고, 꺼낸 순서와 쓴 순서가 같아야 한다. [why D-124]
    struct PendingLine
    {
        enum class Sink
        {
            TRADE,   // logs/trades_YYYYMMDD.csv
            REASON   // logs/order_reasons_YYYYMMDD.txt
        };

        Sink        sink = Sink::TRADE;
        std::string date;   // 어느 날짜 파일로 갈 줄인가 — 자정을 넘겨도 줄이 제 파일로 간다
        std::string text;   // TRADE는 시각 열까지 붙은 완성된 행(줄바꿈 없음), REASON은 줄바꿈까지 포함한 한 줄
    };

    // 줄 하나를 큐에 놓고 쓰기 스레드를 깨운다. 큐가 한도(kAppendOutboxLimit)를 넘으면 부른 쪽이
    //  직접 비운다 — 디스크가 못 따라가는 동안 큐만 자라는 것을 막는 대신 그때는 예전처럼 기다린다.
    void queue_append_line(PendingLine line);
    // 꺼낸 묶음을 파일에 쓴다. 호출자는 io_mutex_를 보유해야 한다. flush는 묶음당 한 번이다.
    void write_pending_lines_locked(const std::deque<PendingLine>& batch);
    // order_reason_file_을 그 날짜 파일로 (재)연다. 호출자는 io_mutex_를 보유해야 한다.
    void open_order_reason_file_locked(const std::string& date);
    // 쓰기 스레드 본문 — 큐에 줄이 들어올 때까지 자고, 깨면 비운다. 멈춤 요청 뒤 한 번 더 비운다.
    void append_writer_loop(std::stop_token stop_token);
    // trade_file_을 그 날짜 파일로 (재)연다 — 없으면 헤더를 쓰고, 옛 헤더면 열을 맞춰 한 번
    //  재작성한다. 호출자는 io_mutex_를 보유해야 한다.
    void open_trade_file_locked(const std::string& date);
    // 원장 CSV 시각 열 — 날짜 파일명(YYYYMMDD)과 행 시각("YYYY-MM-DD HH:MM:SS")을 같이 만든다. KST 고정.
    static void trade_row_timestamp(std::string& date, std::string& stamp);

    std::mutex io_mutex_;                          // 원장 CSV·부속 파일 쓰기 직렬화
    uint64_t   open_orders_written_sequence_ = 0; // io_mutex_ 보호
    // 부속 파일 쓰기 대기함 — 한 칸짜리, 최신이 이긴다. sequence 0은 "대기 중인 것 없음"(스냅샷 번호는 1부터).
    //  [lock-order] 라우터 history_mutex_·open_orders_outbox_mutex_·io_mutex_ 셋은 겹쳐 잡지 않는다 — 스냅샷은
    //   history_mutex_ 안에서 뜨고 대기함에는 그 락을 푼 뒤 넣으며, 쓰기 스레드는 대기함 락을 푼 뒤 io_mutex_를 잡는다.
    std::mutex                  open_orders_outbox_mutex_;
    std::condition_variable_any open_orders_outbox_signal_;
    std::string                 open_orders_pending_body_;
    uint64_t                    open_orders_pending_sequence_ = 0;
    // 상주 파일 핸들(io_mutex_ 보호) — 주문마다 열고 닫는 대신 날짜가 바뀔 때만 다시 연다.
    //  LatencyTrace.h의 opened_ 패턴과 같다. [why D-094]
    std::ofstream trade_file_;
    std::string   trade_file_date_;
    std::ofstream order_reason_file_;
    std::string   order_reason_file_date_;

    // 원장 CSV·사유 덧붙이기 큐 — 줄을 세운다(미결주문 파일과 달리 중간 것도 다 남아야 한다).
    //  [lock-order] io_mutex_ → append_outbox_mutex_. 넣는 쪽은 append_outbox_mutex_만 잡는다.
    //   라우터 on_fill은 세션 첫 체결 때 history_mutex_를 쥔 채 큐를 비운다(history_mutex_ → io_mutex_ → append_outbox_mutex_).
    std::mutex                  append_outbox_mutex_;
    std::condition_variable_any append_outbox_signal_;
    std::deque<PendingLine>     append_outbox_;
    // 큐가 이만큼 밀리면 넣은 쪽이 직접 비운다 — 디스크가 못 따라갈 때 메모리만 늘지 않게.
    //  2,700종목 부하가 초당 300건대를 접수하므로 한도를 넘는 것은 디스크가 몇 분 멈춘 상황뿐이다.
    static constexpr size_t kAppendOutboxLimit = 50'000;

    // 부속 파일 쓰기 스레드. 생성자에서 바로 뜬다.
    //  [inv] 이 줄은 대기함 멤버(open_orders_outbox_*)·io_mutex_보다 반드시 뒤에 있어야 한다 —
    //   멤버는 선언 순서대로 지어지고, 스레드는 지어지는 즉시 그 셋을 만진다.
    std::jthread open_orders_writer_;

    // 원장 CSV·사유 쓰기 스레드. 생성자에서 바로 뜬다.
    //  [inv] 이 줄은 큐 멤버(append_outbox_*)·파일 핸들·io_mutex_보다 반드시 뒤에 있어야 한다 —
    //   멤버는 선언 순서대로 지어지고, 스레드는 지어지는 즉시 그것들을 만진다.
    std::jthread append_writer_;
};
