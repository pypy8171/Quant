// 부하시험 주문 수신단 — 바깥(파이썬 인젝터)에서 온 가상 주문을 ZMQ로 받아 오더북에 넣고, 거기서 난 체결을
//  엔진의 시세 창구(feed::IFeedSource)로 올린다. 엔진이 보기에는 그냥 또 하나의 피드라 샤드·전략·OrderGate·원장이
//  평소대로 돈다. KIS에는 아무것도 나가지 않는다.
//
//        인젝터 ─ZMQ PULL─▶ 수신 스레드 ─▶ MatchingEngine ─체결─▶ TradeData ─▶ 엔진 기존 파이프라인
//                                 ▲                                                    │
//                                 └────── submit() ◀── 전략이 낸 주문(IOrderExecutor 어댑터) ─┘
//
//  지금은 테스트만 submit()을 부른다. IOrderExecutor 어댑터는 아직 없다.
//
// 스레드: 수신 스레드 하나가 소켓 하나를 맡는다(원칙 1). 종목은 순번으로 갈라 한 종목의 오더북은 한 스레드만
//  만진다(원칙 2) — 그래서 오더북에 락이 없다. 다른 스레드가 낸 주문은 MpscQueue로 건너와 그 스레드가 처리한다.
// [why D-128] (원칙 1·2·6은 D-071)
#pragma once
#include "core/IFeedSource.h"
#include "core/MpscQueue.h"
#include "core/SymbolTable.h"
#include "exchange/MatchingEngine.h"
#include "exchange/OrderWire.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace exchange
{

class ZmqOrderFeed : public feed::IFeedSource
{
public:
    struct Options
    {
        uint32_t    lane_count   = 1;                 // 수신 스레드 = 소켓 수. 종목을 이 수로 나눠 맡는다
        int         base_port    = 5600;              // 수신 스레드 i 는 base_port + i 를 연다
        std::string bind_address = "tcp://127.0.0.1"; // 같은 기계 안에서만 쓴다 — 바깥에 열지 않는다
        size_t      submit_queue_capacity = 1 << 16;  // 전략 주문이 수신 스레드로 건너오는 큐 크기
        bool        publish_order_book    = true;     // 체결마다 최우선호가를 같이 올릴지
        // 소켓을 열고 수신 스레드를 띄울지. 시험은 false로 두고 ingest()로 바이트를 직접 먹인다.
        bool        start_receive_threads = true;
        // 체결에 찍을 장중 시각의 시작점(HHMMSS). 부하시험은 아무 때나 돌리는데 전략·게이트는 09:00~15:30
        //  창을 보므로, 이 값이 있으면 connect() 시점을 여기로 놓고 흐른 초만큼 더해 찍는다. 0이면 실제 시계.
        int32_t     session_start_hhmmss  = 0;

        // connect()가 종목 순번표를 적을 파일. 인젝터가 이 파일을 읽어 순번을 맞춘다 — 양쪽이 config를 따로
        //  읽어 순서가 조용히 어긋나는 것을 막는다. 비면 안 적는다.
        std::string universe_out_path;

        // 종목 번호를 남이 찍어 줄 때까지 connect()가 기다릴지. 갈라 띄운 판의 시세 프로세스는 번호를 찍지
        //  못하고 찾기만 하므로(D-114 단계 5) 전략 프로세스가 등록을 마칠 때까지 기다려야 한다. [why D-114 단계 5]
        bool        await_shared_symbols = false;
    };

    struct Statistics
    {
        uint64_t batches        = 0; // 받은 전문 통 수
        uint64_t records        = 0; // 읽은 주문 건수
        uint64_t executions     = 0; // 맞은 건수
        uint64_t rejected       = 0; // 모르는 종목과 ACCUMULATE 모드의 격자 밖 주문. MATCH 모드에서 버린 격자 밖 잔량은 세지 않는다.
        uint64_t submitted      = 0; // 전략이 submit()으로 넣은 건수
        uint64_t submit_dropped = 0; // 큐가 차서 못 받은 건수
    };

    ZmqOrderFeed(symbol::SymbolTable& symbols, Options options);
    ~ZmqOrderFeed() override;

    ZmqOrderFeed(const ZmqOrderFeed&)            = delete;
    ZmqOrderFeed& operator=(const ZmqOrderFeed&) = delete;

    // ── feed::IFeedSource ───────────────────────────────────────────────────
    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override;
    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override;

    [[nodiscard]] uint32_t lanes() const override
    {
        return static_cast<uint32_t>(lanes_.size());
    }

    // 종목 목록을 순번으로 굳히고 수신 스레드를 띄운다. 인젝터는 같은 목록을 같은 순서로 읽어 순번을 맞춘다.
    bool connect(const std::vector<WatchSpec>& specifications) override;
    void disconnect() override;

    bool                   subscribe_incremental(const WatchSpec& specification) override;
    [[nodiscard]] bool     has_specification(const WatchSpec& specification) const override;
    std::vector<WatchSpec> take_overflow_specifications() override;

    [[nodiscard]] bool is_connected() const override;

    // 부하시험 소스는 늙지 않는다 — 인젝터가 멈추면 시험이 끝난 것이지 재연결할 일이 아니다.
    [[nodiscard]] bool is_stale(int threshold_sec) const override;

    // ── 부하시험 전용 ───────────────────────────────────────────────────────
    // 엔진 전략이 낸 주문을 같은 오더북에 넣는다. 어느 스레드에서 불러도 된다 — 종목을 맡은 수신 스레드로 건네준다.
    //  큐가 차 있으면 false. 지금은 테스트만 submit()을 부른다. IOrderExecutor 어댑터는 아직 없다.
    bool submit(const IncomingOrder& order);

    // 소켓을 거치지 않고 바이트를 바로 먹인다. 단위 시험과 같은 프로세스 벤치가 쓴다.
    //  [inv] 그 수신 스레드가 안 돌고 있을 때만 부른다 — 오더북에 락이 없다.
    void ingest(uint32_t lane, const void* data, size_t size);

    // 종목 순번을 맡은 수신 스레드 번호. 인젝터가 어느 포트로 보낼지 정하는 규칙과 같아야 한다.
    [[nodiscard]] uint32_t lane_of_symbol_index(uint32_t symbol_index) const;

    // 종목 순번 → 엔진 종목 id. 모르는 순번이면 symbol::kNone.
    [[nodiscard]] symbol::SymbolId symbol_id_of_index(uint32_t symbol_index) const;

    [[nodiscard]] Statistics statistics() const;

private:
    // 수신 스레드 하나가 쥐는 것 전부. 스레드와 원자 변수를 담아 옮길 수 없어 포인터로 들고 있는다.
    struct Lane
    {
        explicit Lane(size_t submit_queue_capacity);

        MatchingEngine                engine;
        MpscQueue<IncomingOrder>      submit_queue;
        std::thread                   thread;
        std::atomic<uint64_t>         batches{0};
        std::atomic<uint64_t>         records{0};
        std::atomic<uint64_t>         executions{0};
        std::atomic<uint64_t>         rejected{0};
        std::atomic<uint64_t>         submitted{0};
        std::atomic<uint64_t>         submit_dropped{0};
    };

    // 전문 한 통을 처리한다. 그 수신 스레드에서만 부른다.
    void process_batch(uint32_t lane, const OrderWireBatch& batch);

    // 전문 한 건을 처리한다.
    void process_record(uint32_t lane, const OrderWireRecord& record, int32_t hhmmss_now);

    // 다른 스레드가 넣어 둔 전략 주문을 비운다.
    void drain_submit_queue(uint32_t lane, int32_t hhmmss_now);

    // 체결 하나를 TradeData로 만들어 엔진에 올린다.
    void publish_execution(uint32_t lane, const Execution& execution, int direction, int32_t hhmmss_now);

    // 최우선호가를 OrderBook으로 올린다.
    void publish_order_book(uint32_t lane, symbol::SymbolId symbol_id, int32_t hhmmss_now);

    // 수신 스레드 본체. 소켓을 열고 메시지를 받아 process_batch로 넘긴다.
    void receive_loop(uint32_t lane);

    // 초당 받은 주문·맞은 체결을 로그로 남긴다. 그 수신 스레드가 직전 값을 들고 있다가 넘겨준다.
    void report_throughput(uint32_t lane, std::chrono::steady_clock::time_point& last_report,
                           uint64_t& last_records, uint64_t& last_executions);

    // 체결에 찍을 시각. options_.session_start_hhmmss 가 0이면 실제 시계, 아니면 그 시각 + connect() 이후 흐른 초.
    [[nodiscard]] int32_t market_hhmmss() const;

    // 구독 목록 순서대로 종목 번호를 채운다. options_.await_shared_symbols 면 남이 다 찍어 줄 때까지 기다린다.
    //  다 못 채우고 한도를 넘기면 false.
    [[nodiscard]] bool resolve_symbol_indices();

    // 종목 순번표를 파일로 적는다. connect()가 순번을 굳힌 뒤 한 번.
    void write_universe_file() const;

    symbol::SymbolTable& symbols_;
    Options              options_;

    std::vector<std::unique_ptr<Lane>> lanes_;
    std::vector<WatchSpec>             specifications_;
    std::vector<symbol::SymbolId>      index_to_symbol_id_; // 종목 순번 → 엔진 종목 id
    std::vector<uint32_t>              symbol_id_to_index_; // 엔진 종목 id → 종목 순번

    LaneOrderBookCb on_lane_order_book_;
    LaneTradeCb     on_lane_trade_;

    std::chrono::steady_clock::time_point session_start_{}; // 장중 시각을 셈하는 기준점. connect()에서 찍는다

    std::atomic<bool> running_{false};
};

} // namespace exchange
